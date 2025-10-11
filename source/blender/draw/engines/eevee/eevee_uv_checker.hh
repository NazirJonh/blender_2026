/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * UV Checker overlay for Material Preview mode.
 * Renders a checker pattern (procedural or image-based) as a post-process overlay
 * on top of rendered materials to visualize UV layout and texel density.
 */

#pragma once

#include "eevee_uv_checker_shared.hh"
#include "draw_manager.hh"
#include "DNA_image_types.h"
#include "GPU_uniform_buffer.hh"
#include "draw_pass.hh"

/* Debug flag for UV Checker. Define UV_CHECKER_DEBUG to enable debug printf output.
 * Temporarily enabled by default for performance debugging. */
#ifndef UV_CHECKER_DEBUG_DISABLE
#  define UV_CHECKER_DEBUG 0
#endif
#ifdef UV_CHECKER_DEBUG
#  define UV_CHECKER_DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#  define UV_CHECKER_DEBUG_PRINT(...) ((void)0)
#endif

namespace blender::eevee {

using namespace blender::draw;
class Instance;

/**
 * \struct ObjectDrawData
 * \brief Cached data for rendering a single object with UV Checker overlay.
 * 
 * Stores pre-validated object references and batch data to avoid repeated
 * per-frame checks of mesh UV availability.
 */
struct ObjectDrawData {
  /** Pointer to the object being rendered. */
  Object *object;
  /** Pre-fetched GPU batch with UV attributes. */
  gpu::Batch *geom;
  /** Object reference for resource handle generation. */
  draw::ObjectRef ob_ref;
  /** Cached resource handle to avoid lookup every frame. */
  ResourceHandleRange cached_res_handle_;
  /** Whether resource handle is valid. */
  bool res_handle_valid_ = false;
  
  ObjectDrawData(Object *ob, gpu::Batch *batch) 
    : object(ob), geom(batch), ob_ref(ob), res_handle_valid_(false) {}
};

/**
 * \class UVChecker
 * \brief UV Checker post-process overlay for EEVEE.
 * 
 * Renders as a post-process pass after EEVEE rendering, displaying a checker
 * pattern mapped to UV coordinates. Supports both procedural and image-based
 * checkers with optional scene lighting.
 */
class UVChecker {
 private:
  /** Reference to parent EEVEE instance. */
  Instance &inst_;
  
  /** Settings synchronized from View3DOverlay. */
  bool enabled_ = false;
  float checker_scale_ = 8.0f;
  float checker_opacity_ = 0.75f;
  int checker_source_ = 0;  // 0=procedural, 1=image
  ::Image *checker_image_ = nullptr;
  int checker_lighting_ = 0;  // 0=unlit, 1=lit
  
  /** Cached settings to detect changes and avoid pass recreation. */
  float prev_checker_scale_ = -1.0f;
  float prev_checker_opacity_ = -1.0f;
  int prev_checker_source_ = -1;
  ::Image *prev_checker_image_ = nullptr;
  int prev_checker_lighting_ = -1;
  
  /** Uniform data buffer. */
  UniformBuffer<UVCheckerData> data_ = {"uv_checker_data"};
  
  /** Rendering pass for UV checker overlay. */
  PassSimple overlay_ps_ = {"UV Checker Overlay"};
  
  /** Cache for objects with UV data to avoid per-frame validation.
   * Rebuilt only when depsgraph changes (objects added/removed/modified). */
  Vector<ObjectDrawData> cached_objects_;
  
  /** Last depsgraph update ID to detect scene changes. */
  uint64_t depsgraph_last_update_ = 0;
  
  /** Cached shader pointer to avoid lookup every frame. */
  gpu::Shader *cached_shader_ = nullptr;
  
  /** Cached GPU texture pointer to avoid lookup every frame. */
  gpu::Texture *cached_texture_ = nullptr;
  
  /** Cached state of overlay.uv_checker_enabled for fast path optimization.
   * Updated only when state changes to avoid memory access overhead. */
  bool cached_uv_checker_enabled_ = false;
  /** Previous state to detect changes and trigger cleanup only when needed. */
  bool prev_uv_checker_enabled_ = false;
  
  /** Debug counters for performance analysis. */
  uint64_t sync_call_count_ = 0;
  uint64_t render_call_count_ = 0;
  uint64_t cache_rebuild_count_ = 0;
  
 public:
  UVChecker(Instance &inst) : inst_(inst) {}
  ~UVChecker() = default;
  
  /**
   * Initialize resources.
   * Called once per frame during Instance::init().
   */
  void init();
  
  /**
   * Synchronize settings from viewport overlay.
   * Called during Instance::begin_sync().
   * Reads settings from v3d->overlay and prepares rendering pass.
   */
  void sync();
  
  /**
   * Initialize EEVEE pass system for UV Checker.
   * Called from sync() to set up the rendering pass.
   */
  void init_pass_system();
  
  /**
   * Rebind pass resources without full reinitialization.
   * Called from sync() when settings haven't changed but pass was cleared by init().
   */
  void reinit_pass_resources();
  
  /**
   * Validate if object has UV data and can be rendered with UV Checker.
   * Performs all necessary checks for mesh and UV availability.
   * 
   * \param ob: Object to validate.
   * \return: True if object has valid mesh with UV data.
   */
  bool validate_object_has_uv(Object *ob);
  
  /**
   * Check if object cache needs rebuilding due to depsgraph changes.
   * \return: True if cache is stale and needs rebuild.
   */
  bool should_rebuild_cache();
  
  /**
   * Rebuild cache of drawable objects with UV data.
   * Called from sync() when depsgraph changes detected.
   */
  void rebuild_object_cache();
  
  /**
   * Check if UV checker overlay is enabled.
   * Used by rendering code to determine if render() should be called.
   */
  bool postfx_enabled() const
  {
    /* Optimized: removed debug print to reduce overhead (called twice per frame). */
    return enabled_;
  }
  
  /**
   * Render UV checker as post-process overlay.
   * Called from ShadingView::render() after EEVEE rendering.
   * 
   * \param view: Current view for rendering.
   * \param combined_fb: Framebuffer to render into.
   * \param depth_tx: EEVEE depth buffer pointer (for depth testing).
   */
  void render(View &view, Framebuffer &combined_fb, gpu::Texture **depth_tx);
};

}  // namespace blender::eevee

