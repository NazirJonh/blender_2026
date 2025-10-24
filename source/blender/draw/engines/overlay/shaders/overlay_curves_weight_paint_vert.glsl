/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_curves_weight_paint_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_curves_weight_paint)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);
  
  /* Set point size for GPU_PRIM_POINTS primitive */
  gl_PointSize = 4.0;

  /* Separate actual weight and alerts for independent interpolation */
  weight_interp = max(float2(weight, -weight), 0.0f);

  /* Use pre-computed tangent from CPU for fake shading */
#ifdef FAKE_SHADING
  float3 view_tangent = normalize(drw_normal_object_to_view(tangent));
  color_fac = abs(dot(view_tangent, light_dir));
  color_fac = color_fac * 0.9f + 0.1f;
#else
  color_fac = 1.0f;
#endif

  view_clipping_distances(world_pos);
}