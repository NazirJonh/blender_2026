/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_mesh_face_sets)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  float3 view_pos = drw_point_world_to_view(world_pos);
  gl_Position = drw_point_view_to_homogenous(view_pos);

  /* Use proper Z-offset like retopology shader */
  gl_Position.z += get_homogenous_z_offset(
      drw_view().winmat, view_pos.z, gl_Position.w, retopology_offset);

  /* Extract face set color from fset_color */
  /* FIXED: fset_color is float4 from VBO (UNORM_8_8_8_8), already normalized */
  face_set_color = fset_color;
  /* Apply theme face_retopology alpha for Edit Mode Face Sets opacity */
  /* Use face_retopology color for proper transparency control */
  float base_alpha = theme.colors.face_retopology[3];
  
  /* Apply theme face_retopology alpha for Edit Mode Face Sets opacity */
  /* Retopology is only used for depth testing, not for Face Sets transparency */
  face_set_color.a = face_set_color.a * base_alpha;
  /* Apply premultiplication in vertex shader for better performance */
  face_set_color.rgb *= face_set_color.a;
  
  /* Apply fake shading for lit mode */
#ifdef FAKE_SHADING
  float3 view_normal = normalize(drw_normal_object_to_view(nor));
  color_fac = abs(dot(view_normal, light_dir));
  color_fac = color_fac * 0.9f + 0.1f;
#else
  color_fac = 1.0f;
#endif
  
  /* DEBUG: Force alpha to 0 for testing - remove this after testing */
  /* face_set_color.a = 0.0; */
  /* face_set_color.rgb = float3(0.0); */
  

  view_clipping_distances(world_pos);
}
