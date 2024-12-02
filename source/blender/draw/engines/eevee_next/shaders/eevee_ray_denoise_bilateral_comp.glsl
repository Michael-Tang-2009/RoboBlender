/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bilateral filtering of denoised ray-traced radiance.
 *
 * Dispatched at full-resolution using a tile list.
 *
 * Input: Temporally Stabilized Radiance, Stabilized Variance
 * Output: Denoised radiance
 *
 * Following "Stochastic All The Things: Raytracing in Hybrid Real-Time Rendering"
 * by Tomasz Stachowiak
 * https://www.ea.com/seed/news/seed-dd18-presentation-slides-raytracing
 */

#include "infos/eevee_tracing_info.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_ray_denoise_bilateral)

#include "draw_view_lib.glsl"
#include "eevee_closure_lib.glsl"
#include "eevee_filter_lib.glsl"
#include "eevee_gbuffer_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/* In order to remove some more fireflies, "tone-map" the color samples during the accumulation. */
vec3 to_accumulation_space(vec3 color)
{
  return color / (1.0 + reduce_add(color));
}
vec3 from_accumulation_space(vec3 color)
{
  return color / (1.0 - reduce_add(color));
}

void main()
{
  const uint tile_size = RAYTRACE_GROUP_SIZE;
  uvec2 tile_coord = unpackUvec2x16(tiles_coord_buf[gl_WorkGroupID.x]);
  ivec2 texel_fullres = ivec2(gl_LocalInvocationID.xy + tile_coord * tile_size);
  vec2 center_uv = (vec2(texel_fullres) + 0.5) * uniform_buf.raytrace.full_resolution_inv;

  float center_depth = texelFetch(depth_tx, texel_fullres, 0).r;
  vec3 center_P = drw_point_screen_to_world(vec3(center_uv, center_depth));

  ClosureUndetermined center_closure = gbuffer_read_bin(
      gbuf_header_tx, gbuf_closure_tx, gbuf_normal_tx, texel_fullres, closure_index);

  if (center_closure.type == CLOSURE_NONE_ID) {
    /* Output nothing. This shouldn't even be loaded. */
    return;
  }

  float roughness = closure_apparent_roughness_get(center_closure);
  float variance = imageLoadFast(in_variance_img, texel_fullres).r;
  vec3 in_radiance = imageLoadFast(in_radiance_img, texel_fullres).rgb;

  bool is_background = (center_depth == 0.0);
  bool is_smooth = (roughness < 0.05);
  bool is_low_variance = (variance < 0.05);
  bool is_high_variance = (variance > 0.5);

  /* Width of the box filter in pixels. */
  float filter_size_factor = saturate(roughness * 8.0);
  float filter_size = mix(0.0, 9.0, filter_size_factor);
  uint sample_count = uint(mix(1.0, 10.0, filter_size_factor) * (is_high_variance ? 1.5 : 1.0));

  if (is_smooth || is_background || is_low_variance) {
    /* Early out cases. */
    imageStoreFast(out_radiance_img, texel_fullres, vec4(in_radiance, 0.0));
    return;
  }

  vec2 noise = interlieved_gradient_noise(vec2(texel_fullres) + 0.5, vec2(3, 5), vec2(0.0));
  noise += sampling_rng_2D_get(SAMPLING_RAYTRACE_W);

  vec3 accum_radiance = to_accumulation_space(in_radiance);
  float accum_weight = 1.0;
  /* We want to resize the blur depending on the roughness and keep the amount of sample low.
   * So we do a random sampling around the center point. */
  for (uint i = 0u; i < sample_count; i++) {
    /* Essentially a box radius overtime. */
    vec2 offset_f = (fract(hammersley_2d(i, sample_count) + noise) - 0.5) * filter_size;
    ivec2 offset = ivec2(floor(offset_f + 0.5));

    ivec2 sample_texel = texel_fullres + offset;
    ivec3 sample_tile = ivec3(sample_texel / RAYTRACE_GROUP_SIZE, closure_index);
    /* Make sure the sample has been processed and do not contain garbage data. */
    if (imageLoad(tile_mask_img, sample_tile).r == 0u) {
      continue;
    }

    float sample_depth = texelFetch(depth_tx, sample_texel, 0).r;
    vec2 sample_uv = (vec2(sample_texel) + 0.5) * uniform_buf.raytrace.full_resolution_inv;
    vec3 sample_P = drw_point_screen_to_world(vec3(sample_uv, sample_depth));

    /* Background case. */
    if (sample_depth == 0.0) {
      continue;
    }

    vec3 radiance = imageLoadFast(in_radiance_img, sample_texel).rgb;

    /* Do not gather unprocessed pixels. */
    if (all(equal(radiance, FLT_11_11_10_MAX))) {
      continue;
    }

    ClosureUndetermined sample_closure = gbuffer_read_bin(
        gbuf_header_tx, gbuf_closure_tx, gbuf_normal_tx, sample_texel, closure_index);

    if (sample_closure.type == CLOSURE_NONE_ID) {
      continue;
    }

    float gauss = filter_gaussian_factor(filter_size, 1.5);

    /* TODO(fclem): Scene parameter. 10000.0 is dependent on scene scale. */
    float depth_weight = filter_planar_weight(center_closure.N, center_P, sample_P, 10000.0);
    float spatial_weight = filter_gaussian_weight(gauss, length_squared(vec2(offset)));
    float normal_weight = filter_angle_weight(center_closure.N, sample_closure.N);
    float weight = depth_weight * spatial_weight * normal_weight;

    accum_radiance += to_accumulation_space(radiance) * weight;
    accum_weight += weight;
  }

  vec3 out_radiance = accum_radiance * safe_rcp(accum_weight);
  out_radiance = from_accumulation_space(out_radiance);

  imageStoreFast(out_radiance_img, texel_fullres, vec4(out_radiance, 0.0));
}
