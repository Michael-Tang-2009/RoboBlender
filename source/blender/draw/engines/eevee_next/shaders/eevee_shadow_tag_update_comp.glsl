/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Virtual shadow-mapping: Update tagging
 *
 * Any updated shadow caster needs to tag the shadow map tiles it was in and is now into.
 * This is done in 2 pass of this same shader. One for past object bounds and one for new object
 * bounds. The bounding boxes are roughly software rasterized (just a plain rectangle) in order to
 * tag the appropriate tiles.
 */

#include "infos/eevee_shadow_info.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_shadow_tag_update)

#include "common_aabb_lib.glsl"
#include "common_intersect_lib.glsl"

#include "draw_view_lib.glsl"
#include "eevee_shadow_tilemap_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

vec3 safe_project(mat4 winmat, mat4 viewmat, inout int clipped, vec3 v)
{
  vec4 tmp = winmat * (viewmat * vec4(v, 1.0));
  /* Detect case when point is behind the camera. */
  clipped += int(tmp.w < 0.0);
  return tmp.xyz / tmp.w;
}

void main()
{
  ShadowTileMapData tilemap = tilemaps_buf[gl_GlobalInvocationID.z];

  IsectPyramid frustum;
  if (tilemap.projection_type == SHADOW_PROJECTION_CUBEFACE) {
    Pyramid pyramid = shadow_tilemap_cubeface_bounds(tilemap, ivec2(0), ivec2(SHADOW_TILEMAP_RES));
    frustum = isect_pyramid_setup(pyramid);
  }

  uint resource_id = resource_ids_buf[gl_GlobalInvocationID.x];
  resource_id = (resource_id & 0x7FFFFFFFu);

  ObjectBounds bounds = bounds_buf[resource_id];
  if (!drw_bounds_are_valid(bounds)) {
    return;
  }
  IsectBox box = isect_box_setup(bounds.bounding_corners[0].xyz,
                                 bounds.bounding_corners[1].xyz,
                                 bounds.bounding_corners[2].xyz,
                                 bounds.bounding_corners[3].xyz);

  int clipped = 0;
  /* NDC space post projection [-1..1] (unclamped). */
  AABB aabb_ndc = aabb_init_min_max();
  for (int v = 0; v < 8; v++) {
    aabb_merge(aabb_ndc, safe_project(tilemap.winmat, tilemap.viewmat, clipped, box.corners[v]));
  }

  if (tilemap.projection_type == SHADOW_PROJECTION_CUBEFACE) {
    if (clipped == 8) {
      /* All verts are behind the camera. */
      return;
    }
    else if (clipped > 0) {
      /* Not all verts are behind the near clip plane. */
      if (intersect(frustum, box)) {
        /* We cannot correctly handle this case so we fallback by covering the whole view. */
        aabb_ndc.max = vec3(1.0);
        aabb_ndc.min = vec3(-1.0);
      }
      else {
        /* Still out of the frustum. Ignore. */
        return;
      }
    }
    else {
      /* None of the verts are behind the camera. The projected AABB is correct. */
    }
  }

  AABB aabb_tag;
  AABB aabb_map = shape_aabb(vec3(-0.99999), vec3(0.99999));

  /* Directional `winmat` have no correct near/far in the Z dimension at this point.
   * Do not clip in this dimension. */
  if (tilemap.projection_type != SHADOW_PROJECTION_CUBEFACE) {
    aabb_map.min.z = -FLT_MAX;
    aabb_map.max.z = FLT_MAX;
  }

  if (!aabb_clip(aabb_map, aabb_ndc, aabb_tag)) {
    return;
  }

  /* Raster the bounding rectangle of the Box projection. */
  const float tilemap_half_res = float(SHADOW_TILEMAP_RES / 2);
  ivec2 box_min = ivec2(aabb_tag.min.xy * tilemap_half_res + tilemap_half_res);
  ivec2 box_max = ivec2(aabb_tag.max.xy * tilemap_half_res + tilemap_half_res);

  for (int lod = 0; lod <= SHADOW_TILEMAP_LOD; lod++, box_min >>= 1, box_max >>= 1) {
    for (int y = box_min.y; y <= box_max.y; y++) {
      for (int x = box_min.x; x <= box_max.x; x++) {
        int tile_index = shadow_tile_offset(uvec2(x, y), tilemap.tiles_index, lod);
        atomicOr(tiles_buf[tile_index], uint(SHADOW_DO_UPDATE));
      }
    }
  }
}
