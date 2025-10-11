/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * UV Checker overlay implementation.
 */

#include "eevee_uv_checker.hh"
#include "eevee_instance.hh"

#include "BLI_utildefines.h"

#include "DRW_render.hh"
#include "draw_context_private.hh"

#include "BKE_customdata.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"
#include "DEG_depsgraph_query.hh"
#include "DEG_depsgraph.hh"
#include "GPU_batch.hh"
#include "GPU_debug.hh"
#include "GPU_framebuffer.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "draw_cache.hh"
#include "draw_cache_impl.hh"
#include "draw_manager.hh"

namespace blender::eevee {

using namespace blender::draw;

void UVChecker::init()
{
  /* Resources are acquired on-demand during sync/render.
   * Nothing to pre-allocate here. */
}

bool UVChecker::validate_object_has_uv(Object *ob)
{
  /* Quick checks first. */
  if (!ob || ob->type != OB_MESH) {
    return false;
  }
  
  /* Check if mesh has UV data. */
  Mesh *mesh = BKE_object_get_evaluated_mesh(ob);
  if (!mesh) {
    return false;
  }
  
  int uv_layer = CustomData_get_active_layer(&mesh->corner_data, CD_PROP_FLOAT2);
  if (uv_layer == -1) {
    return false;
  }
  
  return true;
}

bool UVChecker::should_rebuild_cache()
{
  /* Check if depsgraph has been updated since last cache build. */
  if (!inst_.depsgraph) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] should_rebuild_cache: NO - no depsgraph\n");
    return false;
  }
  
  uint64_t current_update = DEG_get_update_count(inst_.depsgraph);
  
  /* Rebuild cache if this is the first build or depsgraph changed. */
  bool needs_rebuild = (depsgraph_last_update_ == 0) || (current_update != depsgraph_last_update_);
  
  if (needs_rebuild) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ⚠️ Cache rebuild needed: old=%llu, new=%llu (DIFFERENT!)\n",
                          depsgraph_last_update_, current_update);
    depsgraph_last_update_ = current_update;
  }
  else {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ✓ Cache is valid: update_count=%llu (SAME)\n", current_update);
  }
  
  return needs_rebuild;
}

void UVChecker::rebuild_object_cache()
{
  cache_rebuild_count_++;
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========================================\n");
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] 🔄 Rebuilding object cache #%llu (EXPENSIVE OPERATION!)\n", 
                        cache_rebuild_count_);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ⚠️ WARNING: Cache is being rebuilt! This should be rare!\n");
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========================================\n");
  
  /* Clear existing cache. */
  int old_cache_size = cached_objects_.size();
  cached_objects_.clear();
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] Cleared old cache (%d objects)\n", old_cache_size);
  
  /* Get scene for iteration. */
  Scene *scene = inst_.scene;
  if (!scene) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ❌ No scene available for cache rebuild\n");
    return;
  }
  
  /* Iterate through objects and cache those with UV data. */
  DEGObjectIterSettings deg_iter_settings = {nullptr};
  deg_iter_settings.depsgraph = inst_.depsgraph;
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY | DEG_ITER_OBJECT_FLAG_VISIBLE;
  
  int total_objects_checked = 0;
  int skipped_not_mesh = 0;
  int skipped_no_mesh = 0;
  int skipped_no_uv = 0;
  int skipped_no_batch = 0;
  int cached_count = 0;
  
  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, ob) {
    total_objects_checked++;
    
    if (ob->type != OB_MESH) {
      skipped_not_mesh++;
      continue;
    }
    
    /* Validate object has mesh and UV data. */
    Mesh *mesh = BKE_object_get_evaluated_mesh(ob);
    if (!mesh) {
      skipped_no_mesh++;
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ❌ Object '%s': no evaluated mesh\n", ob->id.name + 2);
      continue;
    }
    
    int uv_layer = CustomData_get_active_layer(&mesh->corner_data, CD_PROP_FLOAT2);
    if (uv_layer == -1) {
      skipped_no_uv++;
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ❌ Object '%s': no UV layer\n", ob->id.name + 2);
      continue;
    }
    
    /* Get mesh batch with UV attributes. */
    gpu::Batch *geom = DRW_cache_mesh_surface_texpaint_single_get(ob);
    if (!geom) {
      skipped_no_batch++;
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ❌ Object '%s': no texpaint batch\n", ob->id.name + 2);
      continue;
    }
    
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ✓ Caching object '%s' (UV layer %d, batch %p)\n", 
                          ob->id.name + 2, uv_layer, (void*)geom);
    
    /* Add to cache. */
    cached_objects_.append(ObjectDrawData(ob, geom));
    cached_count++;
  }
  DEG_OBJECT_ITER_END;
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========================================\n");
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ✅ Cache rebuild complete:\n");
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   Total objects checked: %d\n", total_objects_checked);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ✓ Cached: %d\n", cached_count);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ❌ Skipped (not mesh): %d\n", skipped_not_mesh);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ❌ Skipped (no mesh): %d\n", skipped_no_mesh);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ❌ Skipped (no UV): %d\n", skipped_no_uv);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ❌ Skipped (no batch): %d\n", skipped_no_batch);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========================================\n");
}

void UVChecker::sync()
{
  /* OPTIMIZED FAST PATH: Check cached state first to avoid any memory access.
   * This eliminates overhead when UV checker is disabled in UI.
   * Absolute minimum: only check cached bool and return immediately.
   * Zero overhead: no operations, no memory reads/writes, just a single bool check and return.
   * 
   * Using likely/unlikely hints to help CPU branch prediction.
   * When UV checker is disabled (common case), this branch is highly predictable. */
  if (UNLIKELY(!cached_uv_checker_enabled_)) {
    /* Fast exit - zero overhead: no operations, no memory access, no function calls. */
    return;
  } 
  
  /* From here, we know cached_uv_checker_enabled_ is true, so we need to do full sync.
   * Update counters and debug info only when actually processing. */
  sync_call_count_++;
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========== sync() CALLED #%llu ==========\n", sync_call_count_);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: enabled_ before = %d, cached_objects_.size() = %zu\n", 
                        enabled_, cached_objects_.size());
  
  /* Validate pointers - needed for full sync path. */
  if (!inst_.v3d || !inst_.view_layer || !inst_.scene) {
    enabled_ = false;
    cached_uv_checker_enabled_ = false;
    /* Only clear cache if it was previously enabled (avoid unnecessary work). */
    if (prev_uv_checker_enabled_ && !cached_objects_.is_empty()) {
      cached_objects_.clear();
      cached_texture_ = nullptr;
    }
    prev_uv_checker_enabled_ = false;
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: DISABLED - missing v3d/view_layer/scene\n");
    return;
  }
  
  const View3D *v3d = inst_.v3d;
  const View3DOverlay &overlay = v3d->overlay;
  
  /* Update cache if overlay.uv_checker_enabled changed.
   * This handles state changes and initializes cache on first access.
   * Also check if state changed from disabled to enabled (very rare check using counter). */
  bool current_uv_checker_enabled = overlay.uv_checker_enabled;
  
  /* Very rare check (every 256 sync calls) to detect if user enabled UV checker in UI
   * when we're in disabled state. This is only needed when cached_uv_checker_enabled_ is false
   * but we somehow entered full sync path (shouldn't happen, but safety check). */
  if (!cached_uv_checker_enabled_ && current_uv_checker_enabled && 
      (sync_call_count_ & 0xFF) == 0) {
    /* State changed from disabled to enabled - update cache. */
    cached_uv_checker_enabled_ = true;
    prev_uv_checker_enabled_ = false;
  }
  
  if (current_uv_checker_enabled != cached_uv_checker_enabled_) {
    bool was_enabled = cached_uv_checker_enabled_;
    cached_uv_checker_enabled_ = current_uv_checker_enabled;
    
    /* If disabled now, handle state change. */
    if (!cached_uv_checker_enabled_) {
      enabled_ = false;
      /* Only clear cache if state changed from enabled to disabled. */
      if (was_enabled && prev_uv_checker_enabled_ && !cached_objects_.is_empty()) {
        cached_objects_.clear();
        cached_texture_ = nullptr;
      }
      prev_uv_checker_enabled_ = false;
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: DISABLED - overlay.uv_checker_enabled changed to false\n");
      return;
    }
  }
  
  /* Update previous state tracker. */
  prev_uv_checker_enabled_ = cached_uv_checker_enabled_;
  
  /* Разрешаем UV Checker EEVEE только в Material Preview.
   * Solid/Wireframe покрывает overlay-движок, чтобы избежать конфликтов за batched UV. */
  enabled_ = inst_.is_viewport() && (v3d->shading.type == OB_MATERIAL) && !inst_.is_baking();
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: v3d=%p, shading.type=%d, uv_checker_enabled=%d\n",
         (void*)v3d, (int)v3d->shading.type, (int)overlay.uv_checker_enabled);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: is_viewport=%d, shading_ok=%d, baking=%d => enabled=%d\n",
         inst_.is_viewport(),
         (v3d->shading.type <= OB_SOLID || v3d->shading.type == OB_MATERIAL),
         inst_.is_baking(),
         enabled_);
  
  if (!enabled_) {
    /* Clear cache when disabled to free memory.
     * Only clear if state changed from enabled to disabled (avoid unnecessary work). */
    bool was_enabled_before = (prev_checker_scale_ >= 0.0f || !cached_objects_.is_empty());
    if (was_enabled_before && !cached_objects_.is_empty()) {
      cached_objects_.clear();
      cached_texture_ = nullptr;
    }
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: DISABLED - not in Material Preview mode\n");
    return;
  }
  
  /* Sync settings from overlay. */
  checker_scale_ = overlay.uv_checker_scale;
  checker_opacity_ = overlay.uv_checker_opacity;
  checker_source_ = overlay.uv_checker_source;
  checker_image_ = overlay.uv_checker_image;
  checker_lighting_ = overlay.uv_checker_lighting;
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: scale=%.2f, opacity=%.2f, source=%d, lighting=%d\n",
         checker_scale_, checker_opacity_, checker_source_, checker_lighting_);
  
  /* Validate settings. */
  if (checker_scale_ < 0.1f) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] WARNING: scale too low (%.2f), clamping to 8.0\n", checker_scale_);
    checker_scale_ = 8.0f;  // Use default scale instead of 0.1f
  }
  
  /* Disable if opacity is zero (invisible anyway). */
  if (checker_opacity_ <= 0.0f) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: DISABLED - opacity is zero (%.2f)\n", checker_opacity_);
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] HINT: Enable UV Checker in Overlays panel and set opacity > 0!\n");
    enabled_ = false;
    /* Only clear cache if it was previously enabled (avoid unnecessary work). */
    bool was_enabled_before = (prev_checker_scale_ >= 0.0f || !cached_objects_.is_empty());
    if (was_enabled_before && !cached_objects_.is_empty()) {
      cached_objects_.clear();
      cached_texture_ = nullptr;
    }
    return;
  }
  
  /* Rebuild object cache if depsgraph changed (objects added/removed/modified). */
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: Checking if cache rebuild needed...\n");
  if (should_rebuild_cache()) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: ⚠️ Cache rebuild triggered! This is expensive!\n");
    rebuild_object_cache();
  }
  else {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: ✓ Cache is valid, no rebuild needed\n");
  }
  
  /* Check if settings changed - if so, we need to reinitialize pass system. */
  bool settings_changed = (prev_checker_scale_ != checker_scale_) ||
                          (prev_checker_opacity_ != checker_opacity_) ||
                          (prev_checker_source_ != checker_source_) ||
                          (prev_checker_image_ != checker_image_) ||
                          (prev_checker_lighting_ != checker_lighting_);
  
  /* If image or source changed, invalidate texture cache. */
  if (prev_checker_image_ != checker_image_ || prev_checker_source_ != checker_source_) {
    cached_texture_ = nullptr;
  }
  
  /* Initialize pass system every frame (like overlay_mesh does).
   * This clears previous draw calls and sets up shader/state/uniforms. */
  overlay_ps_.init();
  
  /* Always rebind resources after init() clears everything.
   * This is necessary because pass.init() clears all commands including shader/state/bindings.
   * However, we can optimize by only rebinding what actually changed. */
  if (settings_changed) {
    /* Settings changed - need to update pass configuration. */
    init_pass_system();
    prev_checker_scale_ = checker_scale_;
    prev_checker_opacity_ = checker_opacity_;
    prev_checker_source_ = checker_source_;
    prev_checker_image_ = checker_image_;
    prev_checker_lighting_ = checker_lighting_;
  }
  else {
    /* Settings unchanged - just rebind resources (shader/uniforms/textures).
     * This is still needed because pass.init() cleared all commands.
     * But we can optimize by checking if texture actually changed. */
    reinit_pass_resources();
  }
  
  /* Add draw calls for all cached objects (moved from render()).
   * This follows the pattern from overlay_mesh where draw calls are added in sync(). */
  int draw_count = 0;
  for (ObjectDrawData &data : cached_objects_) {
    /* Generate or reuse cached resource handle for this object.
     * Resource handles can change between frames, so we always fetch fresh. */
    ResourceHandleRange res_handle = inst_.manager->unique_handle(data.ob_ref);
    data.cached_res_handle_ = res_handle;
    data.res_handle_valid_ = true;
    
    /* Add draw call to pass. */
    overlay_ps_.draw(data.geom, res_handle);
    draw_count++;
    
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE]   ✓ Added draw call %d for '%s' (batch %p)\n", 
                          draw_count, data.object->id.name + 2, (void*)data.geom);
  }
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: Added %d draw calls to pass (from %zu cached objects)\n", 
                        draw_count, cached_objects_.size());
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: ENABLED successfully with %zu cached objects\n", 
                        cached_objects_.size());
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: Performance stats: sync_calls=%llu, cache_rebuilds=%llu, render_calls=%llu\n",
                        sync_call_count_, cache_rebuild_count_, render_call_count_);
  /* Warn only if cache rebuilds happen too frequently (more than 5% of sync calls).
   * Note: First sync always rebuilds cache, so ratio starts high and decreases - this is normal. */
  if (sync_call_count_ > 10 && cache_rebuild_count_ > 0) {
    float rebuild_ratio = (float(cache_rebuild_count_) / float(sync_call_count_)) * 100.0f;
    if (rebuild_ratio > 5.0f) {
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ⚠️ PERFORMANCE WARNING: Cache rebuild ratio = %.1f%% (should be < 5%%!)\n", rebuild_ratio);
      if (rebuild_ratio > 50.0f) {
        UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ❌ CRITICAL: Cache rebuilds happen too often! This kills performance!\n");
      }
    }
  }
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========== sync() END ==========\n");
}

void UVChecker::init_pass_system()
{
  /* Note: overlay_ps_.init() is called in sync() before this function.
   * This function only sets up shader, uniforms, and resources. */
  
  /* Get UV Checker shader from EEVEE shader module. */
  gpu::Shader *shader = inst_.shaders.static_shader_get(UV_CHECKER_OVERLAY);
  if (!shader) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ERROR: UV Checker shader not loaded yet\n");
    enabled_ = false;
    return;
  }
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] Shader loaded: %p\n", (void*)shader);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] Shader loaded successfully\n");
  
  overlay_ps_.shader_set(shader);
  
  /* Cache shader pointer for faster access in render(). */
  cached_shader_ = shader;
  
  /* Set uniforms using EEVEE pass system. */
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] Setting uniform data: scale=%.2f, opacity=%.2f\n", checker_scale_, checker_opacity_);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: checker_scale_ = %.2f\n", checker_scale_);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: checker_opacity_ = %.2f\n", checker_opacity_);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: checker_source_ = %d\n", checker_source_);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: checker_lighting_ = %d\n", checker_lighting_);
  
  int use_image_flag = checker_source_;
  
  /* Debug: Check if shader is valid */
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: Shader pointer = %p\n", (void*)shader);
  if (shader) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: Shader is valid\n");
  } else {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ERROR: Shader is NULL!\n");
  }
  
  /* Debug: Check if texture is bound */
  if (checker_source_ && checker_image_) {
    /* Always fetch fresh texture pointer - BKE_image_get_gpu_texture may return different
     * pointer if image was reloaded or GPU texture was recreated. */
    gpu::Texture *tex = BKE_image_get_gpu_texture(checker_image_, nullptr);
    cached_texture_ = tex;  /* Cache for potential reuse, but always fetch fresh. */
    if (tex) {
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: Texture bound successfully: %p\n", (void*)tex);
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: Texture is valid, binding to shader\n");
      GPUSamplerState sampler = GPUSamplerState::default_sampler();
      sampler.extend_x = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      sampler.extend_yz = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      sampler.filtering = GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP;
      overlay_ps_.bind_texture("checker_image", tex, sampler);
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: Texture bound to shader successfully\n");
      use_image_flag = 1;
    } else {
      UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ERROR: Failed to get GPU texture!\n");
      cached_texture_ = nullptr;
      use_image_flag = 0;
    }
  } else {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] DEBUG: Using procedural checker (no texture)\n");
    cached_texture_ = nullptr;
    use_image_flag = 0;
  }

  overlay_ps_.push_constant("checker_scale", checker_scale_);
  overlay_ps_.push_constant("checker_opacity", checker_opacity_);
  overlay_ps_.push_constant("use_image", use_image_flag);
  overlay_ps_.push_constant("use_lighting", checker_lighting_);
  
  /* Bind EEVEE uniform data for proper integration. */
  overlay_ps_.bind_resources(inst_.uniform_data);
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] Pass system initialized successfully\n");
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] sync: enabled_ at end of sync = %d\n", enabled_);
}

void UVChecker::reinit_pass_resources()
{
  /* Rebind resources without full pass reinitialization.
   * Called when settings haven't changed but we still need to rebind resources
   * after pass.init() clears everything. */
  
  /* Use cached shader if available, otherwise fetch it. */
  gpu::Shader *shader = cached_shader_;
  if (!shader) {
    shader = inst_.shaders.static_shader_get(UV_CHECKER_OVERLAY);
    cached_shader_ = shader;
  }
  
  if (!shader) {
    return;
  }
  
  overlay_ps_.shader_set(shader);
  
  /* Re-bind texture if using image checker.
   * Note: We must always rebind after pass.init() clears commands, but we can
   * optimize by checking if texture pointer changed before calling expensive BKE function. */
  int use_image_flag = checker_source_;
  if (checker_source_ && checker_image_) {
    /* Always fetch fresh texture pointer - BKE_image_get_gpu_texture may return different
     * pointer if image was reloaded or GPU texture was recreated. However, in most cases
     * the pointer stays the same, so we can optimize by checking cached value first. */
    gpu::Texture *tex = BKE_image_get_gpu_texture(checker_image_, nullptr);
    
    /* Only rebind texture if pointer actually changed. */
    if (tex != cached_texture_) {
      cached_texture_ = tex;
      if (tex) {
        GPUSamplerState sampler = GPUSamplerState::default_sampler();
        sampler.extend_x = GPU_SAMPLER_EXTEND_MODE_REPEAT;
        sampler.extend_yz = GPU_SAMPLER_EXTEND_MODE_REPEAT;
        sampler.filtering = GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP;
        overlay_ps_.bind_texture("checker_image", tex, sampler);
        use_image_flag = 1;
      }
      else {
        cached_texture_ = nullptr;
        use_image_flag = 0;
      }
    }
    else if (cached_texture_) {
      /* Texture pointer unchanged - still need to rebind after pass.init() cleared commands,
       * but we can reuse cached pointer. */
      GPUSamplerState sampler = GPUSamplerState::default_sampler();
      sampler.extend_x = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      sampler.extend_yz = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      sampler.filtering = GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP;
      overlay_ps_.bind_texture("checker_image", cached_texture_, sampler);
      use_image_flag = 1;
    }
    else {
      use_image_flag = 0;
    }
  }
  else {
    if (cached_texture_) {
      /* Switching from image to procedural - clear cached texture. */
      cached_texture_ = nullptr;
    }
  }
  
  /* Re-bind push constants (always needed after pass.init() clears commands). */
  overlay_ps_.push_constant("checker_scale", checker_scale_);
  overlay_ps_.push_constant("checker_opacity", checker_opacity_);
  overlay_ps_.push_constant("use_image", use_image_flag);
  overlay_ps_.push_constant("use_lighting", checker_lighting_);
  
  /* Re-bind EEVEE uniform data (always needed after pass.init() clears commands). */
  overlay_ps_.bind_resources(inst_.uniform_data);
}

void UVChecker::render(View &view, Framebuffer &combined_fb, gpu::Texture ** /*depth_tx*/)
{
  render_call_count_++;
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========== render() CALLED #%llu ==========\n", render_call_count_);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] render: enabled=%d, cached_objects_.size()=%zu\n", 
                        enabled_, cached_objects_.size());
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] render: Stats: sync_calls=%llu, cache_rebuilds=%llu\n",
                        sync_call_count_, cache_rebuild_count_);
  
  if (!enabled_) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] render() SKIPPED - not enabled\n");
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========== render() END (early exit) ==========\n");
    return;
  }
  
  /* Early exit if no objects to render. */
  if (cached_objects_.is_empty()) {
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ⚠️ render() SKIPPED - no cached objects (cache is empty!)\n");
    UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========== render() END (no objects) ==========\n");
    return;
  }
  
  GPU_debug_group_begin("UV Checker EEVEE Overlay");
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] render: Submitting pass (draw calls already added in sync())\n");
  
  /* Set render state and framebuffer (these can change per-frame). */
  /* Blend over scene; test depth but don't draw backfaces. */
  /* Use correct depth test state from Film (GREATER_EQUAL for reversed Z, LESS_EQUAL otherwise). */
  overlay_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA_PREMUL |
                        inst_.film.depth.test_state | DRW_STATE_CULL_BACK);
  
  /* Bind the combined framebuffer for rendering. */
  overlay_ps_.framebuffer_set(&combined_fb);
  
  /* Submit the pass (draw calls were already added in sync()). */
  inst_.manager->submit(overlay_ps_, view);
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ✓ Pass submitted successfully\n");
  
  UV_CHECKER_DEBUG_PRINT("[UV Checker EEVEE] ========== render() END ==========\n");
  
  GPU_debug_group_end();
}

}  // namespace blender::eevee

