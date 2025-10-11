/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_uv_checker)

/* Procedural checker pattern generation.
 * Creates a checkerboard pattern with specified scale.
 * Returns color value between 0.0 and 1.0. */
float3 procedural_checker(float2 uv, float scale)
{
  /* Scale UV coordinates. */
  float2 checker_coord = uv * scale;
  
  /* Calculate checker pattern using floor and modulo.
   * This creates alternating squares of 0 and 1. */
  float2 checker = floor(checker_coord);
  float checker_value = mod(checker.x + checker.y, 2.0);
  
  /* Define dark and light colors for checker pattern.
   * Dark squares: ~10% gray, Light squares: ~90% gray */
  float3 dark_color = float3(0.1, 0.1, 0.1);
  float3 light_color = float3(0.9, 0.9, 0.9);
  
  /* Mix between dark and light based on checker value. */
  return mix(dark_color, light_color, checker_value);
}

void main()
{
  float3 color;
  
  /* Choose between procedural checker or custom image texture. */
  if (use_image > 0) {
    /* Sample the custom checker image texture.
     * UV coordinates are scaled by checker_scale. */
    float4 tex_color = texture(checker_image, uv_interp * checker_scale);
    color = tex_color.rgb;
  }
  else {
    /* Generate procedural checker pattern. */
    color = procedural_checker(uv_interp, checker_scale);
  }
  
  /* Apply simple lighting if enabled. */
  if (use_lighting > 0) {
    /* Simple directional lighting (Lambert diffuse model).
     * Light direction: slightly from front-top-right (mimics studio lighting). */
    float3 light_dir = normalize(float3(0.5, 0.5, 1.0));
    float3 normal = normalize(normal_interp);
    
    /* Calculate diffuse term (dot product of normal and light direction).
     * Clamp to [0,1] range to avoid negative lighting on backfaces. */
    float ndotl = max(0.0, dot(normal, light_dir));
    
    /* Mix between ambient (0.5) and full brightness (1.0) based on lighting angle.
     * This ensures shadowed areas aren't completely black. */
    float light_factor = mix(0.5, 1.0, ndotl);
    color *= light_factor;
  }
  
  /* Output final color with specified opacity. */
  frag_color = float4(color, checker_opacity);
}

