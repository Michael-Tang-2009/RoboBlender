/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bmesh
 */

#include "BLI_compiler_attrs.h"

#include "bmesh_class.hh"

struct LinkNode *BM_mesh_calc_path_region_vert(BMesh *bm,
                                               BMElem *ele_src,
                                               BMElem *ele_dst,
                                               bool (*filter_fn)(BMVert *, void *user_data),
                                               void *user_data) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL(1, 2, 3);

struct LinkNode *BM_mesh_calc_path_region_edge(BMesh *bm,
                                               BMElem *ele_src,
                                               BMElem *ele_dst,
                                               bool (*filter_fn)(BMEdge *, void *user_data),
                                               void *user_data) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL(1, 2, 3);

struct LinkNode *BM_mesh_calc_path_region_face(BMesh *bm,
                                               BMElem *ele_src,
                                               BMElem *ele_dst,
                                               bool (*filter_fn)(BMFace *, void *user_data),
                                               void *user_data) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL(1, 2, 3);
