/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include <numeric>

#include "AS_asset_representation.hh"

#include "MEM_guardedalloc.h"

#include "DNA_collection_types.h"
#include "DNA_node_types.h"
#include "DNA_texture_types.h"

#include "BLI_easing.h"
#include "BLI_listbase.h"
#include "BLI_math_geom.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_texture.h"

#include "DEG_depsgraph_build.hh"

#include "ED_asset.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_node.hh" /* own include */
#include "ED_render.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_view2d.hh"

#include "io_utils.hh"
#include <fmt/format.h>

#include "node_intern.hh" /* own include */

namespace blender::ed::space_node {

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

static void position_node_based_on_mouse(bNode &node, const float2 &location)
{
  node.location[0] = location.x - NODE_DY * 1.5f / UI_SCALE_FAC;
  node.location[1] = location.y + NODE_DY * 0.5f / UI_SCALE_FAC;
}

bNode *add_node(const bContext &C, const StringRef idname, const float2 &location)
{
  SpaceNode &snode = *CTX_wm_space_node(&C);
  Main &bmain = *CTX_data_main(&C);
  bNodeTree &node_tree = *snode.edittree;

  node_deselect_all(node_tree);

  const std::string idname_str = idname;

  bNode *node = bke::node_add_node(&C, &node_tree, idname_str.c_str());
  BLI_assert(node && node->typeinfo);

  position_node_based_on_mouse(*node, location);

  bke::node_set_selected(node, true);
  ED_node_set_active(&bmain, &snode, &node_tree, node, nullptr);

  ED_node_tree_propagate_change(bmain, &node_tree);
  return node;
}

bNode *add_static_node(const bContext &C, int type, const float2 &location)
{
  SpaceNode &snode = *CTX_wm_space_node(&C);
  Main &bmain = *CTX_data_main(&C);
  bNodeTree &node_tree = *snode.edittree;

  node_deselect_all(node_tree);

  bNode *node = bke::node_add_static_node(&C, &node_tree, type);
  BLI_assert(node && node->typeinfo);

  position_node_based_on_mouse(*node, location);

  bke::node_set_selected(node, true);
  ED_node_set_active(&bmain, &snode, &node_tree, node, nullptr);

  ED_node_tree_propagate_change(bmain, &node_tree);
  return node;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Reroute Operator
 * \{ */

std::optional<float2> link_path_intersection(const bNodeLink &link, const Span<float2> path)
{
  std::array<float2, NODE_LINK_RESOL + 1> coords;
  node_link_bezier_points_evaluated(link, coords);

  for (const int i : path.index_range().drop_back(1)) {
    for (const int j : IndexRange(NODE_LINK_RESOL)) {
      float2 result;
      if (isect_seg_seg_v2_point(path[i], path[i + 1], coords[j], coords[j + 1], result) > 0) {
        return result;
      }
    }
  }

  return std::nullopt;
}

struct RerouteCutsForSocket {
  /* The output socket's owner node. */
  bNode *from_node;
  /* Intersected links connected to the socket and their path intersection locations. */
  Map<bNodeLink *, float2> links;
};

static int add_reroute_exec(bContext *C, wmOperator *op)
{
  const ARegion &region = *CTX_wm_region(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;

  Vector<float2> path;
  RNA_BEGIN (op->ptr, itemptr, "path") {
    float2 loc_region;
    RNA_float_get_array(&itemptr, "loc", loc_region);
    float2 loc_view;
    UI_view2d_region_to_view(&region.v2d, loc_region.x, loc_region.y, &loc_view.x, &loc_view.y);
    path.append(loc_view);
    if (path.size() >= 256) {
      break;
    }
  }
  RNA_END;

  if (path.is_empty()) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  node_deselect_all(ntree);

  ntree.ensure_topology_cache();
  const Vector<bNode *> frame_nodes = ntree.nodes_by_type("NodeFrame");

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  /* All link "cuts" that start at a particular output socket. Deduplicating new reroutes per
   * output socket is useful because it allows reusing reroutes for connected intersections.
   * Further deduplication using the second map means we only have one cut per link. */
  Map<bNodeSocket *, RerouteCutsForSocket> cuts_per_socket;

  LISTBASE_FOREACH (bNodeLink *, link, &ntree.links) {
    if (node_link_is_hidden_or_dimmed(region.v2d, *link)) {
      continue;
    }
    const std::optional<float2> cut = link_path_intersection(*link, path);
    if (!cut) {
      continue;
    }
    RerouteCutsForSocket &from_cuts = cuts_per_socket.lookup_or_add_default(link->fromsock);
    from_cuts.from_node = link->fromnode;
    from_cuts.links.add(link, *cut);
  }

  for (const auto item : cuts_per_socket.items()) {
    const Map<bNodeLink *, float2> &cuts = item.value.links;

    bNode *reroute = bke::node_add_static_node(C, &ntree, NODE_REROUTE);

    bke::node_add_link(&ntree,
                       item.value.from_node,
                       item.key,
                       reroute,
                       static_cast<bNodeSocket *>(reroute->inputs.first));

    /* Reconnect links from the original output socket to the new reroute. */
    for (bNodeLink *link : cuts.keys()) {
      link->fromnode = reroute;
      link->fromsock = static_cast<bNodeSocket *>(reroute->outputs.first);
      BKE_ntree_update_tag_link_changed(&ntree);
    }

    /* Place the new reroute at the average location of all connected cuts. */
    const float2 insert_point = std::accumulate(
                                    cuts.values().begin(), cuts.values().end(), float2(0)) /
                                cuts.size();
    reroute->location[0] = insert_point.x / UI_SCALE_FAC;
    reroute->location[1] = insert_point.y / UI_SCALE_FAC;

    /* Attach the reroute node to frame nodes behind it. */
    for (const int i : frame_nodes.index_range()) {
      bNode *frame_node = frame_nodes.last(i);
      if (BLI_rctf_isect_pt_v(&frame_node->runtime->draw_bounds, insert_point)) {
        bke::node_attach_node(&ntree, reroute, frame_node);
        break;
      }
    }
  }

  ED_node_tree_propagate_change(*CTX_data_main(C), &ntree);
  return OPERATOR_FINISHED;
}

void NODE_OT_add_reroute(wmOperatorType *ot)
{
  ot->name = "Add Reroute";
  ot->idname = "NODE_OT_add_reroute";
  ot->description = "Add a reroute node";

  ot->invoke = WM_gesture_lines_invoke;
  ot->modal = WM_gesture_lines_modal;
  ot->exec = add_reroute_exec;
  ot->cancel = WM_gesture_lines_cancel;

  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  /* properties */
  PropertyRNA *prop;
  prop = RNA_def_collection_runtime(ot->srna, "path", &RNA_OperatorMousePath, "Path", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  /* internal */
  RNA_def_int(ot->srna, "cursor", WM_CURSOR_CROSS, 0, INT_MAX, "Cursor", "", 0, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Node Group Operator
 * \{ */

static bool node_group_add_poll(const bNodeTree &node_tree,
                                const bNodeTree &node_group,
                                ReportList &reports)
{
  if (node_group.type != node_tree.type) {
    return false;
  }

  const char *disabled_hint = nullptr;
  if (!bke::node_group_poll(&node_tree, &node_group, &disabled_hint)) {
    if (disabled_hint) {
      BKE_reportf(&reports,
                  RPT_ERROR,
                  "Cannot add node group '%s' to '%s':\n  %s",
                  node_group.id.name + 2,
                  node_tree.id.name + 2,
                  disabled_hint);
    }
    else {
      BKE_reportf(&reports,
                  RPT_ERROR,
                  "Cannot add node group '%s' to '%s'",
                  node_group.id.name + 2,
                  node_tree.id.name + 2);
    }

    return false;
  }

  return true;
}

static int node_add_group_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree *ntree = snode->edittree;

  bNodeTree *node_group = reinterpret_cast<bNodeTree *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_NT));
  if (!node_group) {
    return OPERATOR_CANCELLED;
  }
  if (!node_group_add_poll(*ntree, *node_group, *op->reports)) {
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  const char *node_idname = node_group_idname(C);
  if (node_idname[0] == '\0') {
    BKE_report(op->reports, RPT_WARNING, "Could not determine type of group node");
    return OPERATOR_CANCELLED;
  }

  bNode *group_node = add_node(*C, node_idname, snode->runtime->cursor);
  if (!group_node) {
    BKE_report(op->reports, RPT_WARNING, "Could not add node group");
    return OPERATOR_CANCELLED;
  }
  if (!RNA_boolean_get(op->ptr, "show_datablock_in_node")) {
    /* By default, don't show the data-block selector since it's not usually necessary for assets.
     */
    group_node->flag &= ~NODE_OPTIONS;
  }
  group_node->width = node_group->default_group_node_width;

  group_node->id = &node_group->id;
  id_us_plus(group_node->id);
  BKE_ntree_update_tag_node_property(snode->edittree, group_node);

  bke::node_set_active(ntree, group_node);
  ED_node_tree_propagate_change(*bmain, nullptr);
  WM_event_add_notifier(C, NC_NODE | NA_ADDED, nullptr);
  DEG_relations_tag_update(bmain);
  return OPERATOR_FINISHED;
}

static bool node_add_group_poll(bContext *C)
{
  if (!ED_operator_node_editable(C)) {
    return false;
  }
  const SpaceNode *snode = CTX_wm_space_node(C);
  if (snode->edittree->type == NTREE_CUSTOM) {
    CTX_wm_operator_poll_msg_set(
        C, "Adding node groups isn't supported for custom (Python defined) node trees");
    return false;
  }
  return true;
}

static int node_add_group_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceNode *snode = CTX_wm_space_node(C);

  /* Convert mouse coordinates to v2d space. */
  UI_view2d_region_to_view(&region->v2d,
                           event->mval[0],
                           event->mval[1],
                           &snode->runtime->cursor[0],
                           &snode->runtime->cursor[1]);

  snode->runtime->cursor[0] /= UI_SCALE_FAC;
  snode->runtime->cursor[1] /= UI_SCALE_FAC;

  return node_add_group_exec(C, op);
}

void NODE_OT_add_group(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Node Group";
  ot->description = "Add an existing node group to the current node editor";
  ot->idname = "NODE_OT_add_group";

  /* callbacks */
  ot->exec = node_add_group_exec;
  ot->invoke = node_add_group_invoke;
  ot->poll = node_add_group_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  WM_operator_properties_id_lookup(ot, true);

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "show_datablock_in_node", true, "Show the datablock selector in the node", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Node Group Asset Operator
 * \{ */

static bool add_node_group_asset(const bContext &C,
                                 const asset_system::AssetRepresentation &asset,
                                 ReportList &reports)
{
  Main &bmain = *CTX_data_main(&C);
  SpaceNode &snode = *CTX_wm_space_node(&C);
  bNodeTree &edit_tree = *snode.edittree;

  bNodeTree *node_group = reinterpret_cast<bNodeTree *>(
      asset::asset_local_id_ensure_imported(bmain, asset));
  if (!node_group) {
    return false;
  }
  if (!node_group_add_poll(edit_tree, *node_group, reports)) {
    /* Remove the node group if it was newly appended but can't be added to the tree. */
    id_us_plus(&node_group->id);
    BKE_id_free_us(&bmain, node_group);
    return false;
  }

  ED_preview_kill_jobs(CTX_wm_manager(&C), CTX_data_main(&C));

  bNode *group_node = add_node(
      C, bke::node_tree_type_find(node_group->idname)->group_idname, snode.runtime->cursor);
  if (!group_node) {
    BKE_report(&reports, RPT_WARNING, "Could not add node group");
    return false;
  }
  /* By default, don't show the data-block selector since it's not usually necessary for assets. */
  group_node->flag &= ~NODE_OPTIONS;
  group_node->width = node_group->default_group_node_width;

  group_node->id = &node_group->id;
  id_us_plus(group_node->id);
  BKE_ntree_update_tag_node_property(&edit_tree, group_node);

  bke::node_set_active(&edit_tree, group_node);
  ED_node_tree_propagate_change(bmain, nullptr);
  WM_event_add_notifier(&C, NC_NODE | NA_ADDED, nullptr);
  DEG_relations_tag_update(&bmain);

  return true;
}

static int node_add_group_asset_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion &region = *CTX_wm_region(C);
  SpaceNode &snode = *CTX_wm_space_node(C);

  const asset_system::AssetRepresentation *asset =
      asset::operator_asset_reference_props_get_asset_from_all_library(*C, *op->ptr, op->reports);
  if (!asset) {
    return OPERATOR_CANCELLED;
  }

  /* Convert mouse coordinates to v2d space. */
  UI_view2d_region_to_view(&region.v2d,
                           event->mval[0],
                           event->mval[1],
                           &snode.runtime->cursor[0],
                           &snode.runtime->cursor[1]);

  snode.runtime->cursor /= UI_SCALE_FAC;

  if (!add_node_group_asset(*C, *asset, *op->reports)) {
    return OPERATOR_CANCELLED;
  }

  wmOperatorType *ot = WM_operatortype_find("NODE_OT_translate_attach_remove_on_cancel", true);
  BLI_assert(ot);
  PointerRNA ptr;
  WM_operator_properties_create_ptr(&ptr, ot);
  WM_operator_name_call_ptr(C, ot, WM_OP_INVOKE_DEFAULT, &ptr, nullptr);
  WM_operator_properties_free(&ptr);

  return OPERATOR_FINISHED;
}

static std::string node_add_group_asset_get_description(bContext *C,
                                                        wmOperatorType * /*ot*/,
                                                        PointerRNA *ptr)
{
  const asset_system::AssetRepresentation *asset =
      asset::operator_asset_reference_props_get_asset_from_all_library(*C, *ptr, nullptr);
  if (!asset) {
    return "";
  }
  const AssetMetaData &asset_data = asset->get_metadata();
  if (!asset_data.description) {
    return "";
  }
  return TIP_(asset_data.description);
}

void NODE_OT_add_group_asset(wmOperatorType *ot)
{
  ot->name = "Add Node Group Asset";
  ot->description = "Add a node group asset to the active node tree";
  ot->idname = "NODE_OT_add_group_asset";

  ot->invoke = node_add_group_asset_invoke;
  ot->poll = node_add_group_poll;
  ot->get_description = node_add_group_asset_get_description;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  asset::operator_asset_reference_props_register(*ot->srna);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Node Object Operator
 * \{ */

static int node_add_object_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree *ntree = snode->edittree;

  Object *object = reinterpret_cast<Object *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_OB));

  if (!object) {
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  bNode *object_node = add_static_node(*C, GEO_NODE_OBJECT_INFO, snode->runtime->cursor);
  if (!object_node) {
    BKE_report(op->reports, RPT_WARNING, "Could not add node object");
    return OPERATOR_CANCELLED;
  }

  bNodeSocket *sock = bke::node_find_socket(object_node, SOCK_IN, "Object");
  if (!sock) {
    BLI_assert_unreachable();
    return OPERATOR_CANCELLED;
  }

  bNodeSocketValueObject *socket_data = (bNodeSocketValueObject *)sock->default_value;
  socket_data->value = object;
  id_us_plus(&object->id);
  BKE_ntree_update_tag_socket_property(ntree, sock);

  bke::node_set_active(ntree, object_node);
  ED_node_tree_propagate_change(*bmain, ntree);
  DEG_relations_tag_update(bmain);

  return OPERATOR_FINISHED;
}

static int node_add_object_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceNode *snode = CTX_wm_space_node(C);

  /* Convert mouse coordinates to v2d space. */
  UI_view2d_region_to_view(&region->v2d,
                           event->mval[0],
                           event->mval[1],
                           &snode->runtime->cursor[0],
                           &snode->runtime->cursor[1]);

  snode->runtime->cursor[0] /= UI_SCALE_FAC;
  snode->runtime->cursor[1] /= UI_SCALE_FAC;

  return node_add_object_exec(C, op);
}

static bool node_add_object_poll(bContext *C)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  return ED_operator_node_editable(C) && ELEM(snode->nodetree->type, NTREE_GEOMETRY);
}

void NODE_OT_add_object(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Node Object";
  ot->description = "Add an object info node to the current node editor";
  ot->idname = "NODE_OT_add_object";

  /* callbacks */
  ot->exec = node_add_object_exec;
  ot->invoke = node_add_object_invoke;
  ot->poll = node_add_object_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Node Collection Operator
 * \{ */

static int node_add_collection_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;

  Collection *collection = reinterpret_cast<Collection *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_GR));

  if (!collection) {
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  bNode *collection_node = add_static_node(*C, GEO_NODE_COLLECTION_INFO, snode.runtime->cursor);
  if (!collection_node) {
    BKE_report(op->reports, RPT_WARNING, "Could not add node collection");
    return OPERATOR_CANCELLED;
  }

  bNodeSocket *sock = bke::node_find_socket(collection_node, SOCK_IN, "Collection");
  if (!sock) {
    BKE_report(op->reports, RPT_WARNING, "Could not find node collection socket");
    return OPERATOR_CANCELLED;
  }

  bNodeSocketValueCollection *socket_data = (bNodeSocketValueCollection *)sock->default_value;
  socket_data->value = collection;
  id_us_plus(&collection->id);
  BKE_ntree_update_tag_socket_property(&ntree, sock);

  bke::node_set_active(&ntree, collection_node);
  ED_node_tree_propagate_change(*bmain, &ntree);
  DEG_relations_tag_update(bmain);

  return OPERATOR_FINISHED;
}

static int node_add_collection_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceNode *snode = CTX_wm_space_node(C);

  /* Convert mouse coordinates to v2d space. */
  UI_view2d_region_to_view(&region->v2d,
                           event->mval[0],
                           event->mval[1],
                           &snode->runtime->cursor[0],
                           &snode->runtime->cursor[1]);

  snode->runtime->cursor[0] /= UI_SCALE_FAC;
  snode->runtime->cursor[1] /= UI_SCALE_FAC;

  return node_add_collection_exec(C, op);
}

static bool node_add_collection_poll(bContext *C)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  return ED_operator_node_editable(C) && ELEM(snode->nodetree->type, NTREE_GEOMETRY);
}

void NODE_OT_add_collection(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Node Collection";
  ot->description = "Add a collection info node to the current node editor";
  ot->idname = "NODE_OT_add_collection";

  /* callbacks */
  ot->exec = node_add_collection_exec;
  ot->invoke = node_add_collection_invoke;
  ot->poll = node_add_collection_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add File Node Operator
 * \{ */

static bool node_add_file_poll(bContext *C)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  return ED_operator_node_editable(C) &&
         ELEM(snode->nodetree->type, NTREE_SHADER, NTREE_TEXTURE, NTREE_COMPOSIT, NTREE_GEOMETRY);
}

/** Node stack animation data, sorts nodes so each node is placed on top of each other. */
struct NodeStackAnimationData {
  Vector<bNode *> nodes;
  wmTimer *anim_timer;
};

static int node_add_file_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  NodeStackAnimationData *data = static_cast<NodeStackAnimationData *>(op->customdata);

  if (event->type != TIMER || data == nullptr || data->anim_timer != event->customdata) {
    return OPERATOR_PASS_THROUGH;
  }

  const float node_stack_anim_duration = 0.25f;
  const float duration = float(data->anim_timer->time_duration);
  const float prev_duration = duration - float(data->anim_timer->time_delta);
  const float clamped_duration = math::min(duration, node_stack_anim_duration);
  const float delta_factor =
      BLI_easing_cubic_ease_in_out(clamped_duration, 0.0f, 1.0f, node_stack_anim_duration) -
      BLI_easing_cubic_ease_in_out(prev_duration, 0.0f, 1.0f, node_stack_anim_duration);

  bool redraw = false;
  /* Each node is pushed by all previous nodes in the stack. */
  float stack_offset = 0.0f;

  for (bNode *node : data->nodes) {
    node->location[1] -= stack_offset;
    stack_offset += (node->runtime->draw_bounds.ymax - node->runtime->draw_bounds.ymin) *
                    delta_factor;
    redraw = true;
  }

  if (redraw) {
    ED_region_tag_redraw(CTX_wm_region(C));
  }

  /* End stack animation. */
  if (duration > node_stack_anim_duration) {
    WM_event_timer_remove(CTX_wm_manager(C), nullptr, data->anim_timer);
    MEM_delete(data);
    op->customdata = nullptr;
    return (OPERATOR_FINISHED | OPERATOR_PASS_THROUGH);
  }

  return OPERATOR_RUNNING_MODAL;
}

static int node_add_file_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  int type = 0;
  switch (snode.nodetree->type) {
    case NTREE_SHADER:
      type = SH_NODE_TEX_IMAGE;
      break;
    case NTREE_TEXTURE:
      type = TEX_NODE_IMAGE;
      break;
    case NTREE_COMPOSIT:
      type = CMP_NODE_IMAGE;
      break;
    case NTREE_GEOMETRY:
      type = GEO_NODE_IMAGE_TEXTURE;
      break;
    default:
      return OPERATOR_CANCELLED;
  }
  Vector<Image *> images;
  /* Load all paths as ID Images. */
  const Vector<std::string> paths = ed::io::paths_from_operator_properties(op->ptr);
  for (const std::string &path : paths) {
    RNA_string_set(op->ptr, "filepath", path.c_str());
    Image *image = (Image *)WM_operator_drop_load_path(C, op, ID_IM);
    if (!image) {
      BKE_report(op->reports, RPT_WARNING, fmt::format("Could not load {}", path).c_str());
      continue;
    }
    images.append(image);
    /* When adding new image file via drag-drop we need to load #ImBuf in order
     * to get proper image source. */
    BKE_image_signal(bmain, image, nullptr, IMA_SIGNAL_RELOAD);
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
  }

  /* If not path is provided, try to get a ID Image from operator. */
  if (paths.is_empty()) {
    Image *image = (Image *)WM_operator_drop_load_path(C, op, ID_IM);
    if (image) {
      images.append(image);
    }
  }

  bNodeTree &node_tree = *snode.edittree;
  float2 position = snode.runtime->cursor;
  Vector<bNode *> nodes;
  /* Add a node for each image. */
  for (Image *image : images) {
    bNode *node = add_static_node(*C, type, position);
    if (!node) {
      BKE_report(op->reports, RPT_WARNING, "Could not add an image node");
      continue;
    }
    if (type == GEO_NODE_IMAGE_TEXTURE) {
      bNodeSocket *image_socket = (bNodeSocket *)node->inputs.first;
      bNodeSocketValueImage *socket_value = (bNodeSocketValueImage *)image_socket->default_value;
      socket_value->value = image;
      BKE_ntree_update_tag_socket_property(&node_tree, image_socket);
    }
    else {
      node->id = (ID *)image;
      blender::bke::node_tag_update_id(node);
    }
    BKE_ntree_update_tag_node_property(&node_tree, node);
    nodes.append(node);
    /* Initial offset between nodes. */
    position[1] -= 20.0f;
  }

  if (nodes.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* Set new nodes as selected. */
  node_deselect_all(node_tree);
  for (bNode *node : nodes) {
    bke::node_set_selected(node, true);
  }
  ED_node_set_active(bmain, &snode, &node_tree, nodes[0], nullptr);

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  ED_node_tree_propagate_change(*bmain, snode.edittree);
  DEG_relations_tag_update(bmain);

  if (nodes.size() == 1) {
    return OPERATOR_FINISHED;
  }

  /* Start the stack animation, so each node is placed on top of each other. */
  NodeStackAnimationData *data = MEM_new<NodeStackAnimationData>(__func__);
  data->nodes = std::move(nodes);
  data->anim_timer = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.02);
  op->customdata = data;
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static int node_add_file_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceNode *snode = CTX_wm_space_node(C);

  /* Convert mouse coordinates to `v2d` space. */
  UI_view2d_region_to_view(&region->v2d,
                           event->mval[0],
                           event->mval[1],
                           &snode->runtime->cursor[0],
                           &snode->runtime->cursor[1]);

  snode->runtime->cursor[0] /= UI_SCALE_FAC;
  snode->runtime->cursor[1] /= UI_SCALE_FAC;

  if (WM_operator_properties_id_lookup_is_set(op->ptr) ||
      RNA_struct_property_is_set(op->ptr, "filepath"))
  {
    return node_add_file_exec(C, op);
  }
  return WM_operator_filesel(C, op, event);
}

void NODE_OT_add_file(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add File Node";
  ot->description = "Add a file node to the current node editor";
  ot->idname = "NODE_OT_add_file";

  /* callbacks */
  ot->exec = node_add_file_exec;
  ot->modal = node_add_file_modal;
  ot->invoke = node_add_file_invoke;
  ot->poll = node_add_file_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_IMAGE | FILE_TYPE_MOVIE,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_RELPATH | WM_FILESEL_DIRECTORY |
                                     WM_FILESEL_FILES,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Mask Node Operator
 * \{ */

static bool node_add_mask_poll(bContext *C)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  return ED_operator_node_editable(C) && snode->nodetree->type == NTREE_COMPOSIT;
}

static int node_add_mask_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);

  ID *mask = WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_MSK);
  if (!mask) {
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  bNode *node = add_static_node(*C, CMP_NODE_MASK, snode.runtime->cursor);

  if (!node) {
    BKE_report(op->reports, RPT_WARNING, "Could not add a mask node");
    return OPERATOR_CANCELLED;
  }

  node->id = mask;
  id_us_plus(mask);

  ED_node_tree_propagate_change(*bmain, snode.edittree);
  DEG_relations_tag_update(bmain);

  return OPERATOR_FINISHED;
}

void NODE_OT_add_mask(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Mask Node";
  ot->description = "Add a mask node to the current node editor";
  ot->idname = "NODE_OT_add_mask";

  /* callbacks */
  ot->exec = node_add_mask_exec;
  ot->poll = node_add_mask_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Material Operator
 * \{ */

static int node_add_material_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree *ntree = snode->edittree;

  Material *material = reinterpret_cast<Material *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_MA));

  if (!material) {
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  bNode *material_node = add_static_node(*C, GEO_NODE_INPUT_MATERIAL, snode->runtime->cursor);
  if (!material_node) {
    BKE_report(op->reports, RPT_WARNING, "Could not add material");
    return OPERATOR_CANCELLED;
  }

  material_node->id = &material->id;
  id_us_plus(&material->id);

  ED_node_tree_propagate_change(*bmain, ntree);
  DEG_relations_tag_update(bmain);

  return OPERATOR_FINISHED;
}

static int node_add_material_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceNode *snode = CTX_wm_space_node(C);

  /* Convert mouse coordinates to v2d space. */
  UI_view2d_region_to_view(&region->v2d,
                           event->mval[0],
                           event->mval[1],
                           &snode->runtime->cursor[0],
                           &snode->runtime->cursor[1]);

  snode->runtime->cursor[0] /= UI_SCALE_FAC;
  snode->runtime->cursor[1] /= UI_SCALE_FAC;

  return node_add_material_exec(C, op);
}

static bool node_add_material_poll(bContext *C)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  return ED_operator_node_editable(C) && ELEM(snode->nodetree->type, NTREE_GEOMETRY);
}

void NODE_OT_add_material(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Material";
  ot->description = "Add a material node to the current node editor";
  ot->idname = "NODE_OT_add_material";

  /* callbacks */
  ot->exec = node_add_material_exec;
  ot->invoke = node_add_material_invoke;
  ot->poll = node_add_material_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Node Tree Operator
 * \{ */

static int new_node_tree_exec(bContext *C, wmOperator *op)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  Main *bmain = CTX_data_main(C);
  bNodeTree *ntree;
  PointerRNA ptr;
  PropertyRNA *prop;
  const char *idname;
  char treename_buf[MAX_ID_NAME - 2];
  const char *treename;

  if (RNA_struct_property_is_set(op->ptr, "type")) {
    prop = RNA_struct_find_property(op->ptr, "type");
    RNA_property_enum_identifier(C, op->ptr, prop, RNA_property_enum_get(op->ptr, prop), &idname);
  }
  else if (snode) {
    idname = snode->tree_idname;
  }
  else {
    return OPERATOR_CANCELLED;
  }

  if (!bke::node_tree_type_find(idname)) {
    BKE_reportf(op->reports, RPT_ERROR, "Node tree type %s undefined", idname);
    return OPERATOR_CANCELLED;
  }

  if (RNA_struct_property_is_set(op->ptr, "name")) {
    RNA_string_get(op->ptr, "name", treename_buf);
    treename = treename_buf;
  }
  else {
    const bke::bNodeTreeType *type = bke::node_tree_type_find(idname);
    treename = type->ui_name.c_str();
  }

  ntree = bke::node_tree_add_tree(bmain, treename, idname);

  /* Hook into UI. */
  UI_context_active_but_prop_get_templateID(C, &ptr, &prop);

  if (prop) {
    /* #RNA_property_pointer_set increases the user count, fixed here as the editor is the initial
     * user. */
    id_us_min(&ntree->id);

    if (ptr.owner_id) {
      BKE_id_move_to_same_lib(*bmain, ntree->id, *ptr.owner_id);
    }

    PointerRNA idptr = RNA_id_pointer_create(&ntree->id);
    RNA_property_pointer_set(&ptr, prop, idptr, nullptr);
    RNA_property_update(C, &ptr, prop);
  }
  else if (snode) {
    snode->nodetree = ntree;

    tree_update(C);
  }

  WM_event_add_notifier(C, NC_NODE | NA_ADDED, nullptr);

  return OPERATOR_FINISHED;
}

static const EnumPropertyItem *new_node_tree_type_itemf(bContext * /*C*/,
                                                        PointerRNA * /*ptr*/,
                                                        PropertyRNA * /*prop*/,
                                                        bool *r_free)
{
  return rna_node_tree_type_itemf(nullptr, nullptr, r_free);
}

void NODE_OT_new_node_tree(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "New Node Tree";
  ot->idname = "NODE_OT_new_node_tree";
  ot->description = "Create a new node tree";

  /* api callbacks */
  ot->exec = new_node_tree_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_enum(ot->srna, "type", rna_enum_dummy_NULL_items, 0, "Tree Type", "");
  RNA_def_enum_funcs(prop, new_node_tree_type_itemf);
  RNA_def_string(ot->srna, "name", "NodeTree", MAX_ID_NAME - 2, "Name", "");
}

/** \} */

}  // namespace blender::ed::space_node
