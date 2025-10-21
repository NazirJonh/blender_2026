/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_edit_mesh_face_sets)

#include "select_lib.glsl"

void main()
{
#ifdef SELECT_ENABLE
  /* DEBUG: Disable backface culling for testing */
  /* if (uniform_buf.backface_culling && !gl_FrontFacing) {
    return;
  } */
#endif

  /* Output face set color with alpha blending */
  /* Color is already premultiplied in vertex shader */
  frag_color = face_set_color;
  
  /* Apply fake shading for lit mode */
  frag_color.rgb = max(float3(0.005f), frag_color.rgb) * color_fac;

  /* This is optimized to NOP in the non select case. */
  select_id_output(select_id);
}
