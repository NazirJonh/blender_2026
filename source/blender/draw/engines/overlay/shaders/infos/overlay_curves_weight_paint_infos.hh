/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"
#endif

#include "overlay_common_infos.hh"

/* -------------------------------------------------------------------- */
/** \name Curves Weight Paint
 * \{ */

GPU_SHADER_INTERFACE_INFO(overlay_curves_weight_paint_iface)
SMOOTH(float2, weight_interp)
SMOOTH(float, color_fac)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_curves_weight_paint)
DO_STATIC_COMPILATION()
VERTEX_IN(0, float, weight)
VERTEX_IN(1, float3, pos)
VERTEX_IN(2, float3, tangent)
VERTEX_OUT(overlay_curves_weight_paint_iface)
SAMPLER(0, sampler1D, colorramp)
PUSH_CONSTANT(float, opacity)      /* `1.0f` by default. */
PUSH_CONSTANT(bool, draw_contours) /* `false` by default. */
FRAGMENT_OUT(0, float4, frag_color)
FRAGMENT_OUT(1, float4, line_output)
VERTEX_SOURCE("overlay_curves_weight_paint_vert.glsl")
FRAGMENT_SOURCE("overlay_curves_weight_paint_frag.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_curves_weight_paint_clipped, overlay_curves_weight_paint, drw_clipped)

GPU_SHADER_CREATE_INFO(overlay_curves_weight_paint_fake_shading)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(overlay_curves_weight_paint)
DEFINE("FAKE_SHADING")
PUSH_CONSTANT(float3, light_dir)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_curves_weight_paint_fake_shading_clipped,
                    overlay_curves_weight_paint_fake_shading,
                    drw_clipped)

/** \} */