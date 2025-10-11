/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_uv_checker)

#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  /* Transform vertex position to clip space. */
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);

  /* Apply small depth offset to prevent z-fighting with the mesh surface.
   * This ensures the UV checker overlay draws slightly in front of the geometry. */
  gl_Position.z -= 1e-5 * abs(gl_Position.w);

  /* Pass UV coordinates to fragment shader.
   * 'a' is the default/render UV layer attribute name used by mesh extractors.
   * This is more universally available than 'au' (active UV). */
  uv_interp = a;
  
  /* Transform normals to world space for lighting calculations.
   * Use normal matrix (transpose of inverse model matrix) to handle non-uniform scaling. */
  float3 world_normal = drw_normal_object_to_world(nor);
  normal_interp = normalize(world_normal);
}

