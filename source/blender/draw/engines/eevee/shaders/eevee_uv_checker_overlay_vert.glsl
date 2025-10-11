/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * UV Checker overlay vertex shader for EEVEE.
 * Simplified version of overlay UV checker for Material Preview mode.
 */

#include "infos/eevee_uv_checker_infos.hh"

VERTEX_SHADER_CREATE_INFO(eevee_uv_checker_overlay)

#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

void main()
{
  /* Transform vertex position to clip space. */
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = reverse_z::transform(drw_point_world_to_homogenous(world_pos));

  /* Depth offset убран: используем depth-тест по сцене. */

  /* Pass UV coordinates to fragment shader.
   * 'a' is the default/render UV layer attribute. */
  uv_interp = a;
  
  /* Transform and normalize normals to world space for lighting.
   * Normalization is done here in vertex shader to reduce per-fragment cost. */
  float3 world_normal = drw_normal_object_to_world(nor);
  normal_interp = normalize(world_normal);
}

