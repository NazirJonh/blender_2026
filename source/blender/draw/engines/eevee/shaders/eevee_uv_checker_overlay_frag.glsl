/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * UV Checker overlay fragment shader for EEVEE.
 * Renders checker pattern (procedural or image-based) with optional lighting.
 */

#include "infos/eevee_uv_checker_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_uv_checker_overlay)

/* Procedural checker pattern generation. */
float3 procedural_checker(float2 uv, float scale)
{
  float2 checker_coord = uv * scale;
  float2 checker = floor(checker_coord);
  float checker_value = mod(checker.x + checker.y, 2.0);
  
  float3 dark_color = float3(0.1, 0.1, 0.1);
  float3 light_color = float3(0.9, 0.9, 0.9);
  
  return mix(dark_color, light_color, checker_value);
}

void main()
{
  /* Backface culling is enabled in DRW_STATE_CULL_BACK, no need to check gl_FrontFacing. */
  
  float3 color;
  
  /* Choose between procedural or image-based checker. */
  if (use_image > 0) {
    float4 tex_color = texture(checker_image, uv_interp * checker_scale);
    color = tex_color.rgb;
  }
  else {
    color = procedural_checker(uv_interp, checker_scale);
  }
  
  /* Apply simple directional lighting if enabled. */
  if (use_lighting > 0) {
    /* normal_interp is already normalized in vertex shader. */
    float3 light_dir = normalize(float3(0.5, 0.5, 1.0));
    float ndotl = max(0.0, dot(normal_interp, light_dir));
    float light_factor = mix(0.5, 1.0, ndotl);
    color *= light_factor;
  }
  
  float alpha = checker_opacity;
  /* Premultiplied output for correct alpha blend with Film. */
  frag_color = float4(color * alpha, alpha);
}

