/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * UV Checker overlay shared data structures.
 * Shared between C++ and GLSL code.
 */

#pragma once

namespace blender::eevee {

/**
 * UV Checker overlay uniform data.
 * Pushed to GPU as uniform buffer.
 */
struct UVCheckerData {
  /** Scale of the checker pattern (higher = more repetitions). */
  float checker_scale;
  /** Opacity/alpha of the overlay (0.0 to 1.0). */
  float checker_opacity;
  /** Use custom image texture (0=procedural, 1=image). */
  int use_image;
  /** Apply scene lighting to checker (0=unlit, 1=lit). */
  int use_lighting;
  /** Padding for alignment. */
  float _pad[4];
};

}  // namespace blender::eevee

