/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_2D_point_uniform_size_uniform_color_outline_aa_info.hh"

VERTEX_SHADER_CREATE_INFO(gpu_shader_2D_point_uniform_size_uniform_color_outline_aa)

void main()
{
  gl_Position = ModelViewProjectionMatrix * vec4(pos, 0.0, 1.0);
  gl_PointSize = size;

  /* calculate concentric radii in pixels */
  float radius = 0.5 * size;

  /* start at the outside and progress toward the center */
  radii[0] = radius;
  radii[1] = radius - 1.0;
  radii[2] = radius - outlineWidth;
  radii[3] = radius - outlineWidth - 1.0;

  /* convert to PointCoord units */
  radii /= size;
}
