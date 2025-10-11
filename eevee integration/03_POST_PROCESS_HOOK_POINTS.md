# EEVEE Post-Process Hook Points

**Date:** 2025-01-11  
**Status:** ✅ CRITICAL FINDINGS

## 🎯 KEY DISCOVERY: render_postfx()

**File:** `eevee_view.cc:183-206`

```cpp
gpu::Texture *ShadingView::render_postfx(gpu::Texture *input_tx)
{
  if (!inst_.depth_of_field.postfx_enabled() && !inst_.motion_blur.postfx_enabled()) {
    return input_tx;
  }
  postfx_tx_.acquire(extent_, gpu::TextureFormat::SFLOAT_16_16_16_16);
  
  gpu::Texture *output_tx = postfx_tx_;
  
  /* Swapping is done internally. Actual output is set to the next input. */
  inst_.motion_blur.render(render_view_, &input_tx, &output_tx);
  inst_.depth_of_field.render(render_view_, &input_tx, &output_tx, dof_buffer_);
  
  return input_tx;  // ← Final texture
}
```

## Rendering Pipeline Flow

```
ShadingView::render() {
  ┌─────────────────────────────────────────┐
  │ 1. Acquire RenderBuffers                │
  │    rbufs.acquire(extent_)               │
  ├─────────────────────────────────────────┤
  │ 2. Setup Framebuffers                   │
  │    combined_fb_, prepass_fb_, gbuffer_fb_ │
  ├─────────────────────────────────────────┤
  │ 3. Clear & Prepass                      │
  │    GPU_framebuffer_clear()              │
  │    pipelines.background.clear()         │
  ├─────────────────────────────────────────┤
  │ 4. Deferred Rendering                   │
  │    pipelines.deferred.render()          │
  ├─────────────────────────────────────────┤
  │ 5. Background                           │
  │    pipelines.background.render()        │
  ├─────────────────────────────────────────┤
  │ 6. Volume & AO                          │
  │    volume.draw_compute()                │
  │    ambient_occlusion.render_pass()      │
  ├─────────────────────────────────────────┤
  │ 7. Forward Rendering                    │
  │    pipelines.forward.render()           │
  ├─────────────────────────────────────────┤
  │ 8. Transparent Pass                     │
  │    render_transparent_pass()            │
  ├─────────────────────────────────────────┤
  │ 9. Debug Draws                          │
  │    lights.debug_draw()                  │
  │    shadows.debug_draw()                 │
  │    probes.viewport_draw()               │
  ├─────────────────────────────────────────┤
  │ 10. ⭐ POST-PROCESS ⭐                   │  ← UV CHECKER ЗДЕСЬ!
  │    gpu::Texture *combined_final_tx =    │
  │        render_postfx(rbufs.combined_tx) │
  │                                         │
  │    → motion_blur.render()               │
  │    → depth_of_field.render()            │
  │    → [UV_CHECKER HERE!]                 │
  ├─────────────────────────────────────────┤
  │ 11. Film Accumulation                   │
  │    film.accumulate(view, combined_final) │
  ├─────────────────────────────────────────┤
  │ 12. Release Resources                   │
  │    rbufs.release()                      │
  │    postfx_tx_.release()                 │
  └─────────────────────────────────────────┘
}
```

## Perfect Integration Point: Option A

### Location: Inside render_postfx()

**Add UV Checker between DoF and film accumulation:**

```cpp
gpu::Texture *ShadingView::render_postfx(gpu::Texture *input_tx)
{
  postfx_tx_.acquire(extent_, gpu::TextureFormat::SFLOAT_16_16_16_16);
  gpu::Texture *output_tx = postfx_tx_;
  
  // Existing post-process effects
  inst_.motion_blur.render(render_view_, &input_tx, &output_tx);
  inst_.depth_of_field.render(render_view_, &input_tx, &output_tx, dof_buffer_);
  
  // 🆕 UV CHECKER POST-PROCESS
  if (inst_.uv_checker.postfx_enabled()) {
    inst_.uv_checker.render(render_view_, &input_tx, &output_tx, rbufs.depth_tx);
  }
  
  return input_tx;
}
```

**Advantages:**
- ✅ EEVEE rendering fully complete
- ✅ Mesh cache stable
- ✅ Depth buffer available (rbufs.depth_tx)
- ✅ Combined texture available (color output)
- ✅ No initialization conflicts
- ✅ Follows existing pattern (DoF, Motion Blur)

### Location: Alternative - After render_postfx()

**Add UV Checker right before film.accumulate():**

```cpp
// Line 160 in ShadingView::render()
gpu::Texture *combined_final_tx = render_postfx(rbufs.combined_tx);

// 🆕 UV CHECKER OVERLAY
if (inst_.uv_checker.overlay_enabled()) {
  inst_.uv_checker.draw_overlay(render_view_, combined_fb_, rbufs.depth_tx);
}

inst_.film.accumulate(jitter_view_, combined_final_tx);
```

**Advantages:**
- ✅ Even simpler - no need to modify render_postfx()
- ✅ Direct rendering to combined_fb
- ✅ All resources available

## Implementation Pattern

### Pattern from Depth of Field

**File:** `eevee_depth_of_field.hh:50-150`

```cpp
class DepthOfField {
 private:
  Instance &inst_;
  
  // Textures for processing
  gpu::Texture *input_color_tx_ = nullptr;
  gpu::Texture *output_color_tx_ = nullptr;
  
  // Passes
  PassSimple setup_ps_ = {"Setup"};
  PassSimple resolve_ps_ = {"Resolve"};
  
 public:
  DepthOfField(Instance &inst) : inst_(inst) {};
  
  void init();  // Called in Instance::init()
  void sync();  // Called in Instance::begin_sync()
  
  bool postfx_enabled() const {
    return enabled_;
  }
  
  void render(View &view, 
              gpu::Texture **input_tx,
              gpu::Texture **output_tx,
              DepthOfFieldBuffer &dof_buffer);
};
```

### UV Checker Implementation (Similar Pattern)

```cpp
// In eevee_instance.hh
class Instance {
  // ... existing modules
  DepthOfField depth_of_field;
  MotionBlurModule motion_blur;
  UVCheckerModule uv_checker;  // 🆕 Add here
  // ...
};
```

```cpp
// New file: eevee_uv_checker.hh
class UVChecker {
 private:
  Instance &inst_;
  
  bool enabled_ = false;
  float checker_scale_ = 8.0f;
  float checker_opacity_ = 0.75f;
  
  PassSimple uv_checker_ps_ = {"UV Checker Overlay"};
  
 public:
  UVChecker(Instance &inst) : inst_(inst) {}
  
  void init();
  void sync();
  
  bool postfx_enabled() const {
    return enabled_ && inst_.v3d && inst_.v3d->overlay.uv_checker_enabled;
  }
  
  void render(View &view,
              gpu::Texture **input_tx,
              gpu::Texture **output_tx,
              gpu::Texture *depth_tx);
};
```

## Available Resources at Hook Point

At `render_postfx()` / before `film.accumulate()`:

### Textures
- ✅ `rbufs.combined_tx` - Color output from EEVEE
- ✅ `rbufs.depth_tx` - Depth buffer (for depth testing)
- ✅ `rbufs.vector_tx` - Motion vectors
- ✅ All render passes available

### State
- ✅ `inst_.v3d->overlay.uv_checker_enabled` - User setting
- ✅ `inst_.v3d->overlay.uv_checker_scale` - Scale
- ✅ `inst_.v3d->overlay.uv_checker_opacity` - Opacity
- ✅ `render_view_` - Current view for rendering
- ✅ `extent_` - Render extent

### Scene Data
- ✅ All meshes already rendered by EEVEE
- ✅ Mesh cache STABLE (no more modifications)
- ✅ Materials applied
- ✅ Lighting calculated

## Implementation Steps

### Step 1: Create UVChecker Module

**Files to create:**
- `eevee_uv_checker.hh` - Header with class definition
- `eevee_uv_checker.cc` - Implementation
- `eevee_uv_checker_shared.hh` - Shared data structures

**Pattern:** Copy structure from `eevee_depth_of_field.*`

### Step 2: Integrate into Instance

**File:** `eevee_instance.hh`
```cpp
#include "eevee_uv_checker.hh"

class Instance {
  // Add after depth_of_field:
  UVChecker uv_checker;
  
  // Constructor:
  Instance() : /* ... */ uv_checker(*this) {}
};
```

### Step 3: Add init/sync calls

**File:** `eevee_instance.cc`
```cpp
void Instance::init(...)
{
  // ... existing init calls
  depth_of_field.init();
  uv_checker.init();  // 🆕 Add here
  // ...
}

void Instance::begin_sync()
{
  // ... existing sync calls
  depth_of_field.sync();
  uv_checker.sync();  // 🆕 Add here
  // ...
}
```

### Step 4: Hook into render_postfx()

**File:** `eevee_view.cc`

**Option A: Inside render_postfx()**
```cpp
gpu::Texture *ShadingView::render_postfx(gpu::Texture *input_tx)
{
  // ... existing code
  inst_.motion_blur.render(render_view_, &input_tx, &output_tx);
  inst_.depth_of_field.render(render_view_, &input_tx, &output_tx, dof_buffer_);
  
  // 🆕 UV Checker
  inst_.uv_checker.render(render_view_, &input_tx, &output_tx);
  
  return input_tx;
}
```

**Option B: After render_postfx()**
```cpp
void ShadingView::render()
{
  // ... existing rendering
  gpu::Texture *combined_final_tx = render_postfx(rbufs.combined_tx);
  
  // 🆕 UV Checker Overlay
  inst_.uv_checker.draw_overlay(render_view_, combined_fb_, rbufs.depth_tx);
  
  inst_.film.accumulate(jitter_view_, combined_final_tx);
  // ...
}
```

### Step 5: Create Shaders

**Files:**
- `shaders/eevee_uv_checker_overlay.glsl` - Vertex/Fragment shaders
- `shaders/infos/eevee_uv_checker_infos.hh` - Shader create info

**Pattern:** Similar to overlay shaders but simpler

## Mesh Data Access at Hook Point

**Q:** How to access mesh UVs at this stage?

**A:** Two approaches:

### Approach 1: Re-render Meshes (Recommended)
```cpp
void UVChecker::render(View &view, ...)
{
  // Request mesh batches AFTER EEVEE finished
  for (Object *ob : scene_objects) {
    gpu::Batch *geom = DRW_cache_mesh_surface_get(ob);
    // ✅ Safe now - EEVEE done with cache
    // Draw with UV checker shader
  }
}
```

### Approach 2: Fullscreen Pass
```cpp
// Use G-buffer or depth buffer to reconstruct UVs
// More complex but no mesh cache access needed
```

## Depth Buffer Handling

```cpp
void UVChecker::render(...)
{
  // Bind EEVEE depth for testing
  pass.bind_texture("depth_tx", depth_tx);
  
  // Depth test: only draw on rendered geometry
  pass.state_set(DRW_STATE_WRITE_COLOR | 
                 DRW_STATE_BLEND_ALPHA |
                 DRW_STATE_DEPTH_LESS_EQUAL);  // Test against EEVEE depth
}
```

## Next Steps

1. ✅ **Completed:** Found post-process hook point
2. ✅ **Completed:** Studied integration pattern
3. ⏳ **TODO:** Create eevee_uv_checker module skeleton
4. ⏳ **TODO:** Implement basic rendering
5. ⏳ **TODO:** Test with simple procedural checker
6. ⏳ **TODO:** Add full features (image, lighting, etc.)

## Key Insights

### Insight 1: Perfect Timing
Post-process passes execute AFTER all EEVEE rendering, when mesh cache is completely stable. No conflicts possible!

### Insight 2: Existing Pattern
DoF and Motion Blur provide perfect template. Copy their structure = safe implementation.

### Insight 3: Resource Availability
At this point we have:
- ✅ Rendered color
- ✅ Depth buffer
- ✅ Stable mesh cache
- ✅ View/projection matrices

### Insight 4: Minimal Changes
Can implement with ZERO changes to existing EEVEE code. Just add new module following existing pattern.

## Conclusion

**EEVEE integration is FEASIBLE and SAFE!**

The `render_postfx()` hook point provides:
- ✅ Perfect timing (after EEVEE, before film)
- ✅ All required resources
- ✅ No mesh cache conflicts
- ✅ Existing pattern to follow
- ✅ Clean separation of concerns

**Estimated implementation:** 8-12 hours for full integration

**Risk level:** LOW - Following proven pattern

