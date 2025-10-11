# UV Checker EEVEE Integration - Implementation Design

**Date:** 2025-01-11  
**Phase:** 3 - Design  
**Status:** Ready for implementation

## Executive Summary

UV Checker будет интегрирован как **post-process module** в EEVEE, следуя паттерну Depth of Field и Motion Blur.

**Hook Point:** `ShadingView::render_postfx()` - после EEVEE rendering, до film accumulation

**Integration Method:** New EEVEE module (`UVChecker`) with minimal changes to existing code

**Risk:** LOW - Following proven pattern, no mesh cache conflicts

## Architecture

```
EEVEE::Instance
├─ Film
├─ RenderBuffers
├─ DepthOfField
├─ MotionBlur
└─ UVChecker  🆕 NEW MODULE
   ├─ init()   - Initialize textures/passes
   ├─ sync()   - Sync settings from v3d->overlay
   └─ render() - Post-process overlay rendering
```

## File Structure

### New Files to Create

```
source/blender/draw/engines/eevee/
├─ eevee_uv_checker.hh             🆕 Class definition
├─ eevee_uv_checker.cc             🆕 Implementation
├─ eevee_uv_checker_shared.hh      🆕 Shared data structures
└─ shaders/
   ├─ infos/eevee_uv_checker_infos.hh  🆕 Shader create info
   └─ eevee_uv_checker_overlay.glsl    🆕 Shader code
```

### Files to Modify

```
source/blender/draw/engines/eevee/
├─ eevee_instance.hh       ✏️ Add UVChecker member
├─ eevee_instance.cc       ✏️ Call init/sync
├─ eevee_view.hh           ✏️ (Maybe) add helper
└─ eevee_view.cc           ✏️ Hook into render_postfx()
```

## Detailed Component Design

### Component 1: eevee_uv_checker.hh

```cpp
/* SPDX-FileCopyrightText: 2025 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_pass.hh"
#include "eevee_uv_checker_shared.hh"

namespace blender::eevee {

using namespace draw;
class Instance;

class UVChecker {
 private:
  Instance &inst_;
  
  /** Settings synchronized from View3DOverlay. */
  bool enabled_ = false;
  float checker_scale_ = 8.0f;
  float checker_opacity_ = 0.75f;
  int checker_source_ = 0;  // 0=procedural, 1=image
  ::Image *checker_image_ = nullptr;
  int checker_lighting_ = 0;  // 0=unlit, 1=lit
  
  /** Rendering pass for UV checker overlay. */
  PassSimple overlay_ps_ = {"UV Checker Overlay"};
  
  /** Uniform data buffer. */
  UniformBuffer<UVCheckerData> data_ = {"uv_checker_data"};
  
 public:
  UVChecker(Instance &inst) : inst_(inst) {}
  
  /** Initialize resources. Called once per frame. */
  void init();
  
  /** Synchronize settings from viewport overlay. */
  void sync();
  
  /** Check if UV checker overlay is enabled. */
  bool postfx_enabled() const {
    return enabled_;
  }
  
  /** Render UV checker as post-process overlay. */
  void render(View &view,
              gpu::Texture *color_tx,
              gpu::Texture *depth_tx);
};

}  // namespace blender::eevee
```

### Component 2: eevee_uv_checker_shared.hh

```cpp
/* SPDX-FileCopyrightText: 2025 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender::eevee {

struct UVCheckerData {
  float checker_scale;
  float checker_opacity;
  int use_image;
  int use_lighting;
  float4 _pad;  // Alignment
};

}  // namespace blender::eevee
```

### Component 3: eevee_uv_checker.cc (Skeleton)

```cpp
/* SPDX-FileCopyrightText: 2025 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_uv_checker.hh"
#include "eevee_instance.hh"

#include "BKE_image.hh"
#include "DNA_view3d_types.h"

namespace blender::eevee {

void UVChecker::init()
{
  // Nothing to allocate yet
  // Resources acquired on demand
}

void UVChecker::sync()
{
  const View3D *v3d = inst_.v3d;
  if (!v3d) {
    enabled_ = false;
    return;
  }
  
  const View3DOverlay &overlay = v3d->overlay;
  
  // Only enable in viewport with EEVEE shading
  enabled_ = (v3d->shading.type == OB_MATERIAL) &&
             overlay.uv_checker_enabled &&
             !inst_.is_baking();
  
  if (!enabled_) {
    return;
  }
  
  // Sync settings
  checker_scale_ = overlay.uv_checker_scale;
  checker_opacity_ = overlay.uv_checker_opacity;
  checker_source_ = overlay.uv_checker_source;
  checker_image_ = overlay.uv_checker_image;
  checker_lighting_ = overlay.uv_checker_lighting;
  
  // Validate
  if (checker_scale_ < 0.1f) {
    checker_scale_ = 0.1f;
  }
  if (checker_opacity_ <= 0.0f) {
    enabled_ = false;
    return;
  }
  
  // Setup pass
  overlay_ps_.init();
  overlay_ps_.shader_set(inst_.shaders.static_shader_get(UV_CHECKER_OVERLAY));
  overlay_ps_.state_set(DRW_STATE_WRITE_COLOR | 
                        DRW_STATE_BLEND_ALPHA |
                        DRW_STATE_DEPTH_LESS_EQUAL);
  
  // Bind uniforms
  data_.checker_scale = checker_scale_;
  data_.checker_opacity = checker_opacity_;
  data_.use_image = checker_source_;
  data_.use_lighting = checker_lighting_;
  data_.push_update();
  
  overlay_ps_.bind_ubo("uv_checker", &data_);
  
  // Bind checker image if available
  if (checker_source_ && checker_image_) {
    gpu::Texture *tex = BKE_image_get_gpu_texture(checker_image_, nullptr);
    if (tex) {
      overlay_ps_.bind_texture("checker_image", tex);
    }
    else {
      // Fallback to procedural
      data_.use_image = 0;
    }
  }
}

void UVChecker::render(View &view,
                       gpu::Texture *color_tx,
                       gpu::Texture *depth_tx)
{
  if (!enabled_) {
    return;
  }
  
  GPU_debug_group_begin("UV Checker");
  
  // Bind depth for testing
  overlay_ps_.bind_texture("depth_tx", depth_tx);
  
  // TODO: Render mesh batches with UV checker shader
  // For each object in scene:
  //   - Get mesh batch (safe now - EEVEE done)
  //   - Draw with UV checker shader
  //   - Depth test against EEVEE depth
  
  // Submit pass
  inst_.manager->submit(overlay_ps_, view);
  
  GPU_debug_group_end();
}

}  // namespace blender::eevee
```

### Component 4: Integration into Instance

**eevee_instance.hh:**
```cpp
#include "eevee_uv_checker.hh"  // Add include

class Instance : public DrawEngine {
 public:
  // Existing modules...
  DepthOfField depth_of_field;
  MotionBlurModule motion_blur;
  UVChecker uv_checker;  // 🆕 Add member
  
  Instance()
    : /* ... */
      depth_of_field(*this),
      motion_blur(*this),
      uv_checker(*this) {};  // 🆕 Initialize
};
```

**eevee_instance.cc:**
```cpp
void Instance::init(...)
{
  // ... existing init calls ...
  depth_of_field.init();
  motion_blur.init();
  uv_checker.init();  // 🆕 Call init
}

void Instance::begin_sync()
{
  // ... existing sync calls ...
  depth_of_field.sync();
  motion_blur.sync();
  uv_checker.sync();  // 🆕 Call sync
}
```

### Component 5: Hook into Rendering

**eevee_view.cc:**

**Option A: Inside render_postfx()**
```cpp
gpu::Texture *ShadingView::render_postfx(gpu::Texture *input_tx)
{
  if (!inst_.depth_of_field.postfx_enabled() && 
      !inst_.motion_blur.postfx_enabled() &&
      !inst_.uv_checker.postfx_enabled()) {  // 🆕 Check UV checker
    return input_tx;
  }
  
  postfx_tx_.acquire(extent_, gpu::TextureFormat::SFLOAT_16_16_16_16);
  gpu::Texture *output_tx = postfx_tx_;
  
  inst_.motion_blur.render(render_view_, &input_tx, &output_tx);
  inst_.depth_of_field.render(render_view_, &input_tx, &output_tx, dof_buffer_);
  
  // 🆕 UV Checker Post-Process
  // Note: UV checker renders directly to combined_fb_, not via texture swap
  if (inst_.uv_checker.postfx_enabled()) {
    inst_.uv_checker.render(render_view_, input_tx, inst_.render_buffers.depth_tx);
  }
  
  return input_tx;
}
```

**Option B: Separate call after postfx (Simpler)**
```cpp
void ShadingView::render()
{
  // ... existing rendering ...
  
  gpu::Texture *combined_final_tx = render_postfx(rbufs.combined_tx);
  
  // 🆕 UV Checker Overlay (renders directly to combined_fb_)
  if (inst_.uv_checker.postfx_enabled()) {
    GPU_framebuffer_bind(combined_fb_);  // Bind framebuffer
    inst_.uv_checker.render(render_view_, rbufs.combined_tx, rbufs.depth_tx);
  }
  
  inst_.film.accumulate(jitter_view_, combined_final_tx);
  // ...
}
```

## Shader Implementation

### Shader Create Info

**File:** `shaders/infos/eevee_uv_checker_infos.hh`

```cpp
GPU_SHADER_CREATE_INFO(eevee_uv_checker_overlay)
DO_STATIC_COMPILATION()
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float2, uv)      // UV coords
VERTEX_IN(2, float3, nor)      // Normals (for lighting)
FRAGMENT_OUT(0, float4, frag_color)
UNIFORM_BUF(7, UVCheckerData, uv_checker)
SAMPLER(0, sampler2D, checker_image)
SAMPLER(1, sampler2D, depth_tx)
VERTEX_SOURCE("eevee_uv_checker_overlay.glsl")
FRAGMENT_SOURCE("eevee_uv_checker_overlay.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
GPU_SHADER_CREATE_END()
```

### Shader Code

**File:** `shaders/eevee_uv_checker_overlay.glsl`

```glsl
/* Vertex Shader */
#ifdef VERTEX_SHADER
void main()
{
  vec3 world_pos = transform_point(ModelMatrix, pos);
  gl_Position = ViewProjectionMatrix * vec4(world_pos, 1.0);
  
  // Pass UVs and normals to fragment
  uv_interp = uv;
  normal_interp = normalize(normal_object_to_world(nor));
}
#endif

/* Fragment Shader */
#ifdef FRAGMENT_SHADER
float3 procedural_checker(float2 uv, float scale)
{
  float2 checker_coord = uv * scale;
  float2 checker = floor(checker_coord);
  float checker_value = mod(checker.x + checker.y, 2.0);
  
  float3 dark = float3(0.1);
  float3 light = float3(0.9);
  return mix(dark, light, checker_value);
}

void main()
{
  // Sample depth buffer to check if fragment should be visible
  vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(depth_tx, 0));
  float scene_depth = texture(depth_tx, screen_uv).r;
  
  // Discard if behind scene geometry
  if (gl_FragCoord.z > scene_depth) {
    discard;
  }
  
  // Get checker color
  float3 color;
  if (uv_checker.use_image > 0) {
    color = texture(checker_image, uv_interp * uv_checker.checker_scale).rgb;
  }
  else {
    color = procedural_checker(uv_interp, uv_checker.checker_scale);
  }
  
  // Apply lighting if enabled
  if (uv_checker.use_lighting > 0) {
    vec3 light_dir = normalize(vec3(0.5, 0.5, 1.0));
    float ndotl = max(0.0, dot(normalize(normal_interp), light_dir));
    color *= mix(0.5, 1.0, ndotl);
  }
  
  // Output with opacity
  frag_color = vec4(color, uv_checker.checker_opacity);
}
#endif
```

## Implementation Roadmap

### Phase 3.1: Skeleton (2 hours)
- [x] Create file structure
- [ ] Add empty class definitions
- [ ] Integrate into Instance
- [ ] Add init/sync calls
- [ ] Test compilation

### Phase 3.2: Basic Rendering (3 hours)
- [ ] Implement sync() - read overlay settings
- [ ] Setup rendering pass
- [ ] Hook into render_postfx()
- [ ] Test with empty pass (no drawing yet)

### Phase 3.3: Mesh Drawing (4 hours)
- [ ] Access mesh batches in render()
- [ ] Create UV checker shader (procedural only)
- [ ] Render meshes with checker
- [ ] Depth test against EEVEE depth
- [ ] Test in viewport

### Phase 3.4: Full Features (3 hours)
- [ ] Add image texture support
- [ ] Implement lighting mode
- [ ] Add error handling
- [ ] Performance optimization
- [ ] Test edge cases

## Testing Strategy

### Test 1: Basic Visibility
- Enable UV Checker in Material Preview
- ✅ Should see checker pattern on mesh
- ✅ No crash

### Test 2: Depth Occlusion
- Place object behind another
- ✅ Checker should be occluded correctly
- ✅ Matches EEVEE depth

### Test 3: Opacity
- Adjust checker opacity slider
- ✅ Blends with material underneath
- ✅ Alpha blending works

### Test 4: Scale
- Change checker scale
- ✅ Pattern scales correctly
- ✅ No artifacts

### Test 5: Image Texture
- Set checker source to Image
- Load custom texture
- ✅ Custom texture displays
- ✅ Fallback to procedural if invalid

### Test 6: Lighting
- Toggle Unlit/Lit
- ✅ Lighting applies in Lit mode
- ✅ Flat shading in Unlit mode

### Test 7: Performance
- Large scene with many objects
- ✅ Maintains acceptable FPS
- ✅ No memory leaks

### Test 8: Mode Switching
- Switch between Solid and Material Preview
- ✅ Checker appears only in Material Preview
- ✅ No crash when switching

## Potential Issues & Solutions

### Issue 1: Mesh Batch Access
**Problem:** Accessing mesh batches might still conflict

**Solution:** Use `DRW_cache_mesh_surface_get()` which is read-only and safe after EEVEE render

### Issue 2: Depth Buffer Format
**Problem:** Depth comparison might not work correctly

**Solution:** Use same depth format/comparison as EEVEE (DEPTH_GREATER_EQUAL with reverse-Z)

### Issue 3: Performance with Many Objects
**Problem:** Redrawing all meshes might be slow

**Solution:** 
- Only draw visible objects
- Use LOD if available
- Consider spatial culling

### Issue 4: UV Attribute Missing
**Problem:** Some meshes might not have UVs

**Solution:** Check for UV presence, skip if missing (similar to current overlay implementation)

## Success Criteria

- ✅ UV Checker works in Material Preview mode
- ✅ No crashes or conflicts with EEVEE
- ✅ Correct depth occlusion
- ✅ All features work (scale, opacity, image, lighting)
- ✅ Performance acceptable (< 5ms overhead)
- ✅ Clean code following EEVEE patterns

## Estimated Time

**Total:** 12-14 hours

- Phase 3.1 (Skeleton): 2h
- Phase 3.2 (Basic): 3h
- Phase 3.3 (Meshes): 4h
- Phase 3.4 (Features): 3h
- Testing & Polish: 2h

## Conclusion

This design provides a **complete, safe, and maintainable** solution for UV Checker integration in EEVEE.

**Key Advantages:**
- ✅ No mesh cache conflicts
- ✅ Follows proven patterns
- ✅ Minimal code changes
- ✅ Easy to test and debug
- ✅ Extensible for future features

**Ready for implementation!**

