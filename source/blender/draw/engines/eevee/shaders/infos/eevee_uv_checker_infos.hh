/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

/* UV Checker overlay for EEVEE Material Preview mode. */

GPU_SHADER_INTERFACE_INFO(eevee_uv_checker_iface)
SMOOTH(float2, uv_interp)
SMOOTH(float3, normal_interp)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(eevee_uv_checker_overlay)
DO_STATIC_COMPILATION()
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float3, nor)
VERTEX_IN(2, float2, a)  /* Default UV layer name (same as overlay) */
VERTEX_OUT(eevee_uv_checker_iface)
FRAGMENT_OUT(0, float4, frag_color)
PUSH_CONSTANT(float, checker_scale)
PUSH_CONSTANT(float, checker_opacity)
PUSH_CONSTANT(int, use_image)
PUSH_CONSTANT(int, use_lighting)
SAMPLER(0, sampler2D, checker_image)
VERTEX_SOURCE("eevee_uv_checker_overlay_vert.glsl")
FRAGMENT_SOURCE("eevee_uv_checker_overlay_frag.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_resource_id_varying)
GPU_SHADER_CREATE_END()

