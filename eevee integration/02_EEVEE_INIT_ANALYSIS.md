# EEVEE Initialization Order Analysis

**Date:** 2025-01-11  
**Goal:** Понять порядок инициализации EEVEE и точку crash

## EEVEE Instance Structure

```cpp
class Instance : public DrawEngine {
  // Core Components (initialized in constructor)
  ShaderModule &shaders;
  SyncModule sync;
  MaterialModule materials;
  // ... many more modules
  
  Film film;  // ← CRASH ЗДЕСЬ в Film::init
  RenderBuffers render_buffers;
  Camera camera;
  // ... etc
```

## Initialization Flow

### 1. Instance Construction
```cpp
Instance::Instance()
  : shaders(*ShaderModule::module_get()),
    sync(*this),
    film(*this, uniform_data.data.film),  // Film создается рано
    // ... all modules initialized
```

### 2. Instance::init() - Viewport Mode

**File:** `eevee_instance.cc:60-126`

```cpp
void Instance::init()
{
  this->draw_ctx = DRW_context_get();
  
  Depsgraph *depsgraph = draw_ctx->depsgraph;
  Scene *scene = draw_ctx->scene;
  View3D *v3d = draw_ctx->v3d;  // ← Может быть nullptr!
  // ...
  
  // Вызывается полная init
  init(size, &rect, &visible_rect, nullptr, depsgraph, camera, nullptr, &default_view, v3d, rv3d);
}
```

### 3. Instance::init(full signature)

**File:** `eevee_instance.cc:128-200`

```cpp
void Instance::init(const int2 &output_res,
                    const rcti *output_rect,
                    const rcti *visible_rect,
                    RenderEngine *render_,
                    Depsgraph *depsgraph_,
                    Object *camera_object_,
                    const RenderLayer *render_layer_,
                    View *drw_view_,
                    const View3D *v3d_,
                    const RegionView3D *rv3d_)
{
  // Сохраняет параметры
  render = render_;
  depsgraph = depsgraph_;
  v3d = v3d_;
  rv3d = rv3d_;
  // ...
  
  sampling.init(scene);
  camera.init();
  film.init(output_res, output_rect);  // ← Line 196: ВЫЗОВ INIT
  render_buffers.init();
  // ...
}
```

### 4. Film::init() - CRASH SITE

**File:** `eevee_film.cc:266-400` (approx)

```cpp
void Film::init(const int2 &extent, const rcti *output_rect)
{
  Sampling &sampling = inst_.sampling;
  Scene &scene = *inst_.scene;
  
  if (inst_.is_viewport()) {
    // ⚠️ Обращение к inst_.v3d->shading
    const View3DShading &shading = inst_.v3d->shading;  // ← Возможный nullptr dereference?
    
    int update = 0;
    update += assign_if_different(ui_render_pass_, 
                                   eViewLayerEEVEEPassType(shading.render_pass));
    // ...
  }
  
  // Line 305: Проверка overlays
  if (inst_.overlays_enabled() || inst_.gpencil_engine_enabled()) {
    // Overlays нужны depth pass
    enabled_passes |= EEVEE_RENDER_PASS_DEPTH;
  }
  
  // ... много других инициализаций
}
```

**Ключевые проверки в Film::init:**
- `inst_.v3d->shading` - может быть проблема если v3d не валидна
- `inst_.overlays_enabled()` - зависит от v3d state
- Множество texture allocations
- Pass configuration

## Crash Analysis

**Stack Trace:**
```
blender::eevee::Film::init
  ← blender::eevee::Instance::init
    ← DRWContext::engines_init_and_sync
```

**Exception:** `EXCEPTION_ACCESS_VIOLATION at offset 0x0000000000000474`

**Offset 0x474 = 1140 bytes**

Это примерно размер структуры или указатель в структуре. Возможные кандидаты:
- `inst_.v3d` - может быть nullptr
- Какой-то member в `FilmData` или `View3DShading`
- Texture pointer до allocation

## Timing Conflict

```
Timeline of Initialization:
┌────────────────────────────────────────┐
│ 1. DRW_context_get()                   │
│ 2. EEVEE::Instance::init() START       │
│    ├─ sampling.init()                  │
│    ├─ camera.init()                    │
│    ├─ film.init()  ← POINT A           │
│    │   └─ v3d->shading access          │
│    └─ render_buffers.init()            │
├────────────────────────────────────────┤
│ 3. Overlay::Instance::begin_sync()     │
│    └─ UVChecker::begin_sync()          │
├────────────────────────────────────────┤
│ 4. Overlay::Instance::object_sync()    │
│    └─ UVChecker::object_sync()         │
│        └─ DRW_mesh_batch_cache_get_    │ ← POINT B
│           surface_texpaint_single()     │ (ИЗМЕНЯЕТ CACHE)
├────────────────────────────────────────┤
│ 5. EEVEE материалы читают cache        │ ← POINT C
│    ⚠️ Cache в неожиданном состоянии!   │
│    💥 CRASH: nullptr dereference        │
└────────────────────────────────────────┘
```

## Key Findings

### 1. V3D Access Pattern
```cpp
// Instance
const View3D *v3d;  // Может быть nullptr!

// overlays_enabled() check
bool overlays_enabled() const {
  return overlays_enabled_;
}

// Установка в init():
overlays_enabled_ = v3d && !(v3d->flag2 & V3D_HIDE_OVERLAYS);
```

### 2. Mesh Cache State

**Problem:** `DRW_mesh_batch_cache_get_surface_texpaint_single()` вызывается в `object_sync()`, который происходит МЕЖДУ EEVEE init фазами.

**Effect:** EEVEE ожидает определенное состояние mesh cache для material shader compilation, но UV Checker изменяет это состояние.

### 3. Depth Pass Dependencies

```cpp
// Film::init line 305
if (inst_.overlays_enabled() || inst_.gpencil_engine_enabled()) {
  // Overlays нужен depth для правильного compositing
  enabled_passes |= EEVEE_RENDER_PASS_DEPTH;
}
```

**Observation:** EEVEE ЗНАЕТ об overlays и настраивает depth pass соответственно!

## Critical Insights

### Insight 1: EEVEE ожидает stable mesh cache

EEVEE полагается на то, что mesh cache НЕ ИЗМЕНИТСЯ между `film.init()` и material shader compilation.

**Наше нарушение:**
```cpp
// UV Checker в object_sync():
gpu::Batch *geom = DRW_mesh_batch_cache_get_surface_texpaint_single(*ob, mesh);
// ☝️ Это ИЗМЕНЯЕТ cache->cd_needed, cache state
```

### Insight 2: Film::init зависит от v3d->shading

```cpp
if (inst_.is_viewport()) {
  const View3DShading &shading = inst_.v3d->shading;
  // ☝️ Если v3d nullptr или не инициализирован → crash
}
```

### Insight 3: Overlays integration point

```cpp
// Line 305 в Film::init
if (inst_.overlays_enabled() || inst_.gpencil_engine_enabled()) {
  enabled_passes |= EEVEE_RENDER_PASS_DEPTH;
}
```

**Это показывает:**
- EEVEE ЗНАЕТ о overlays
- Есть механизм для overlay integration
- Depth pass управляется через enabled_passes

## Safe Integration Points

Based on this analysis, есть несколько безопасных точек для UV Checker integration:

### Option 1: Post-Film Init
```cpp
// В Instance::init после film.init():
film.init(output_res, output_rect);
render_buffers.init();
// ← ЗДЕСЬ безопасно работать с mesh cache
```

### Option 2: After End_Sync
```cpp
// В Instance::end_sync():
void Instance::end_sync() {
  // ... EEVEE sync завершен
  // ← ЗДЕСЬ mesh cache стабилен
}
```

### Option 3: Separate Draw Pass
```cpp
// В Instance::draw_viewport():
void Instance::draw_viewport() {
  render_sample();  // EEVEE rendering
  // ← ЗДЕСЬ добавить UV Checker post-process
}
```

## Recommended Approach

**Best: Post-Process After EEVEE Render**

```cpp
// Pseudo-code
Instance::render_sample() {
  // 1. EEVEE renders scene → render_buffers
  // 2. Film accumulates → film textures
  // 3. [NEW] UV Checker post-process:
  //    - Read EEVEE depth buffer
  //    - Render UV checker with depth test
  //    - Blend with EEVEE output
}
```

**Advantages:**
- ✅ Mesh cache уже стабилен
- ✅ EEVEE depth доступен
- ✅ No initialization conflicts
- ✅ Clean separation

## Next Steps

1. ✅ **Completed:** Understand EEVEE init order
2. 🔄 **In Progress:** Find post-process hook point
3. ⏳ **TODO:** Study Film::accumulate() and display() methods
4. ⏳ **TODO:** Find examples of post-process overlays in EEVEE
5. ⏳ **TODO:** Design UV Checker post-process pass

## References

**Key Files:**
- `eevee_instance.cc:60-200` - Initialization
- `eevee_film.cc:266-400` - Film::init() (crash site)
- `eevee_film.cc:588-630` - Film::init_pass() (pass setup)

**Key Concepts:**
- **enabled_passes:** Bitmask controlling which render passes are active
- **overlays_enabled():** EEVEE knows about overlays and adjusts rendering
- **mesh cache state:** Must be stable during EEVEE init

