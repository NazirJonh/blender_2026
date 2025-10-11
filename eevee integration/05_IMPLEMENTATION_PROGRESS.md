# EEVEE Integration - Implementation Progress

**Date:** 2025-01-11  
**Current Phase:** 4.3 - Mesh Drawing  
**Status:** 🔄 IN PROGRESS

---

## ✅ Completed Phases

### Phase 4.1: Skeleton (DONE) ✅

**Files Created:**
- ✅ `eevee_uv_checker_shared.hh` - Data structures
- ✅ `eevee_uv_checker.hh` - Class definition  
- ✅ `eevee_uv_checker.cc` - Implementation skeleton

**Integration:**
- ✅ Added include to `eevee_instance.hh`
- ✅ Added `UVChecker uv_checker` member
- ✅ Added constructor initialization
- ✅ Added to `CMakeLists.txt` (3 files)

**Result:** Module compiles without errors!

### Phase 4.2: Basic Rendering (DONE) ✅

**Implemented:**
- ✅ `UVChecker::init()` - Resource initialization
- ✅ `UVChecker::sync()` - Settings synchronization from v3d->overlay
- ✅ `UVChecker::postfx_enabled()` - Enable check
- ✅ Hook into `eevee_view.cc::ShadingView::render()` - After postfx, before film

**Validation:**
```cpp
// In sync():
enabled_ = (v3d->shading.type == OB_MATERIAL) && 
           overlay.uv_checker_enabled &&
           !inst_.is_baking();
```

**Debug Output:**
```
[UV Checker EEVEE] sync: enabled=1, scale=8.00, opacity=0.75, source=0, lighting=0
[UV Checker EEVEE] render() called: scale=8.00, opacity=0.75
```

**Result:** Hook works, sync works, ready for mesh rendering!

---

## 🔄 Current Phase: 4.3 - Mesh Drawing

**Goal:** Render mesh batches with UV checker pattern overlay.

**Status:** Design stage - choosing shader approach

### Challenge: Shader Access

**Problem:** EEVEE and Overlay are separate engines. Need to access overlay's UV checker shader.

**Options:**

#### Option A: Create EEVEE Shader (Complex)
```
1. Add shader to eShaderType enum
2. Create shader create info in shaders/infos/
3. Register in ShaderModule
4. Use inst_.shaders.static_shader_get()
```

**Pros:**
- ✅ Native EEVEE integration
- ✅ Full control

**Cons:**
- ❌ Duplicate code (shader exists in overlay)
- ❌ Complex setup
- ❌ More files to maintain

#### Option B: Call Overlay Engine (Moderate)
```
1. Get overlay instance through DRW
2. Call overlay UVChecker methods
3. Let overlay handle rendering
```

**Pros:**
- ✅ Reuse existing overlay code
- ✅ Less code

**Cons:**
- ❌ Cross-engine dependencies
- ❌ Potential state conflicts

#### Option C: GPU Direct Rendering (Simple - RECOMMENDED)
```
1. Get compiled shader: GPU_shader_get_builtin_shader_with_config()
2. Use low-level GPU API
3. Manual batch rendering
```

**Pros:**
- ✅ Simple and direct
- ✅ Full control over rendering
- ✅ No engine dependencies

**Cons:**
- ❌ Need to load shader manually
- ❌ More manual setup

### Recommended Implementation (Option C)

**Step 1: Get UV Checker Shader**

```cpp
// In sync() or init():
GPUShader *shader = GPU_shader_get_builtin_shader_with_config(
    GPU_SHADER_CFG_DEFAULT,
    GPU_SHADER_3D_UV_CHECKER  // Need to add this to GPU_shader.h
);
```

**Alternative:** Load shader from overlay shader cache:
```cpp
#include "overlay_shader.h"  // Access to overlay shaders
shader = overlay_shader_get(OVERLAY_SHADER_UV_CHECKER);
```

**Step 2: Iterate Scene Objects**

```cpp
void UVChecker::render(View &view, gpu::Texture *color_tx, gpu::Texture **depth_tx)
{
  // Iterate all objects in scene
  DEG_OBJECT_ITER_BEGIN(inst_.depsgraph,
                        ob,
                        DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                        DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET |
                        DEG_ITER_OBJECT_FLAG_VISIBLE |
                        DEG_ITER_OBJECT_FLAG_DUPLI)
  {
    if (ob->type == OB_MESH) {
      render_mesh_object(ob, view, depth_tx);
    }
  }
  DEG_OBJECT_ITER_END;
}
```

**Step 3: Render Single Mesh**

```cpp
void UVChecker::render_mesh_object(Object *ob, View &view, gpu::Texture **depth_tx)
{
  Mesh *mesh = BKE_object_get_evaluated_mesh(ob);
  if (!mesh) return;
  
  // Check for UV data
  if (CustomData_get_active_layer(&mesh->corner_data, CD_PROP_FLOAT2) == -1) {
    return;  // No UVs
  }
  
  // Get mesh batch
  gpu::Batch *geom = DRW_cache_mesh_surface_get(ob);
  if (!geom) return;
  
  // Setup GPU state
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_face_cull(GPU_CULL_BACK);
  
  // Bind shader and uniforms
  GPU_shader_bind(shader_);
  GPU_shader_uniform_1f(shader_, "checker_scale", checker_scale_);
  GPU_shader_uniform_1f(shader_, "checker_opacity", checker_opacity_);
  
  // Draw batch
  GPU_batch_set_shader(geom, shader_);
  GPU_batch_draw(geom);
}
```

---

## Implementation Decision Needed

**QUESTION:** Какой подход использовать для shader access?

### Recommended: Hybrid Approach

**Use overlay engine's infrastructure через DRW Manager:**

```cpp
// In render():
Manager &manager = *inst_.manager;

// Initialize pass with overlay shader
overlay_ps_.init();

// Get overlay shader handle (через global shader registry)
gpu::Shader *uv_checker_shader = /* ... получить shader ... */;

overlay_ps_.shader_set(uv_checker_shader);
overlay_ps_.state_set(DRW_STATE_WRITE_COLOR | 
                      DRW_STATE_BLEND_ALPHA |
                      DRW_STATE_DEPTH_LESS_EQUAL);

// Push constants
overlay_ps_.push_constant("checker_scale", checker_scale_);
overlay_ps_.push_constant("checker_opacity", checker_opacity_);
overlay_ps_.push_constant("use_image", checker_source_);
overlay_ps_.push_constant("use_lighting", checker_lighting_);

// Iterate objects and draw
DEG_OBJECT_ITER_BEGIN(...) {
  gpu::Batch *geom = DRW_cache_mesh_surface_get(ob);
  ResourceHandle handle = manager.unique_handle(ob);
  overlay_ps_.draw(geom, handle);
}
DEG_OBJECT_ITER_END;

// Submit
manager.submit(overlay_ps_, view);
```

---

## Next Steps for Phase 4.3

### Task 1: Shader Access Solution

**Need to resolve:** How to get overlay_uv_checker shader from EEVEE context?

**Options to investigate:**
1. Direct shader registry access
2. Create shader info in EEVEE (duplicate from overlay)
3. Use GPU_shader_create_from_info() directly

**Recommended:** Create simplified EEVEE shader info (copy from overlay)

### Task 2: Mesh Iteration

**Implement:**
```cpp
DEG_OBJECT_ITER_BEGIN(inst_.depsgraph, ob, FLAGS) {
  if (ob->type == OB_MESH) {
    render_mesh(ob, view, depth_tx);
  }
}
DEG_OBJECT_ITER_END;
```

### Task 3: Batch Rendering

**Implement:**
```cpp
void render_mesh(Object *ob, View &view, gpu::Texture **depth_tx) {
  // Get mesh and check UVs
  // Get batch
  // Draw with overlay_ps_
}
```

### Task 4: Depth Test

**Setup:**
```cpp
overlay_ps_.bind_texture("depth_tx", *depth_tx);
// Depth test in shader: gl_FragCoord.z vs texture(depth_tx, uv).r
```

---

## Current Status Summary

**Working:**
- ✅ Module skeleton compiles
- ✅ Integration into EEVEE instance
- ✅ Settings sync from overlay
- ✅ Hook in render pipeline
- ✅ Debug output confirms calls

**Not Working Yet:**
- ❌ Actual mesh rendering (needs shader)
- ❌ Shader access from EEVEE
- ❌ Object iteration
- ❌ Depth testing

**Blocker:** Shader access method needs to be chosen and implemented.

---

## Estimated Remaining Time

**Phase 4.3 Remaining:** 3-4 hours
- Shader solution: 1-2h
- Mesh iteration: 1h
- Testing: 1h

**Total Remaining:** 8-10 hours (including 4.4, 4.5)

---

## Files Modified So Far

```
✅ Created:
source/blender/draw/engines/eevee/
├─ eevee_uv_checker_shared.hh    (34 lines)
├─ eevee_uv_checker.hh            (88 lines)
└─ eevee_uv_checker.cc            (119 lines)

✅ Modified:
├─ eevee_instance.hh              (+2 lines - include, member)
├─ eevee_instance.cc              (+2 lines - init, sync)
├─ eevee_view.cc                  (+5 lines - render hook)
└─ CMakeLists.txt                 (+3 lines - file registration)

Total: ~250 lines new code, ~12 lines modifications
```

---

## Next Session Plan

**When resuming:**

1. **Choose shader approach** (Option A, B, or C)
2. **Implement chosen solution**
3. **Add object iteration**
4. **Test basic rendering**
5. **Continue to Phase 4.4**

**Recommendation:** Go with simplified approach first:
- Use GPU direct rendering
- Simple procedural shader only
- Test it works
- Then add full features

---

## Questions for Next Session

1. Should we create EEVEE-specific shader or reuse overlay shader?
2. Use Manager submission or direct GPU calls?
3. Priority: get basic working version first or full-featured?

**Recommended:** Basic working version first (procedural only, no lighting), then iterate.

---

**Progress:** 60% complete  
**Time invested:** ~6 hours  
**Time remaining:** ~8 hours  
**ETA:** 2-3 more sessions

---

**Status:** Ready to continue Phase 4.3! 🚀

