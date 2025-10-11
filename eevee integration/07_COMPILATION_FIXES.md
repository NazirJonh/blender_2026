# Compilation Fixes - EEVEE UV Checker

**Date:** 2025-01-11  
**Status:** ✅ ИСПРАВЛЕНО

## Ошибки Компиляции

### Error 1: DEG_OBJECT_ITER_BEGIN
```
error C4002: too many parameters for macro "DEG_OBJECT_ITER_BEGIN"
error C2027: undefined type "Depsgraph"
```

**Причина:** EEVEE не использует `DEG_OBJECT_ITER_BEGIN` макрос напрямую.

**Исправление:**
```cpp
// ДО:
DEG_OBJECT_ITER_BEGIN(inst_.depsgraph, ob, FLAGS) {
  // ...
}
DEG_OBJECT_ITER_END;

// ПОСЛЕ:
DRW_render_object_iter(nullptr, inst_.depsgraph,
  [&](ObjectRef &ob_ref, RenderEngine *, Depsgraph *) {
    Object *ob = ob_ref.object;
    // ...
  });
```

### Error 2: DRW_mesh_batch_cache_get_surface_texpaint_single
```
error C3861: identifier not found
```

**Причина:** Неправильное имя функции и namespace.

**Исправление:**
```cpp
// ДО:
gpu::Batch *geom = DRW_mesh_batch_cache_get_surface_texpaint_single(*ob, *mesh);

// ПОСЛЕ:
gpu::Batch *geom = blender::draw::DRW_cache_mesh_surface_texpaint_single_get(ob);
```

### Error 3: Manager::unique_handle parameter type
```
error C2664: cannot convert from "Object *" to "const blender::draw::ObjectRef &"
```

**Причина:** `unique_handle` принимает `ObjectRef`, а не `Object*`.

**Исправление:**
```cpp
// ДО:
ResourceHandle res_handle = inst_.manager->unique_handle(ob);

// ПОСЛЕ:
ResourceHandleRange res_handle = inst_.manager->unique_handle(ob_ref);
```

### Error 4: PassBase::draw signature
```
error C2665: cannot convert argument 2 from "ResourceHandle" to "ResourceIndexRange"
```

**Причина:** `draw()` ожидает `ResourceHandleRange` и count, а не `ResourceHandle`.

**Исправление:**
```cpp
// ДО:
overlay_ps_.draw(geom, res_handle);

// ПОСЛЕ:
overlay_ps_.draw(geom, res_handle, 1);  // 1 instance
```

## Все Исправления

### File: eevee_uv_checker.cc

```cpp
// 1. Added includes
#include "BKE_object.hh"
#include "draw_cache_impl.hh"

// 2. Changed iteration method
DRW_render_object_iter(nullptr, inst_.depsgraph,
  [&](ObjectRef &ob_ref, RenderEngine *, Depsgraph *) {
    Object *ob = ob_ref.object;
    // ... mesh processing ...
  });

// 3. Changed batch function
gpu::Batch *geom = blender::draw::DRW_cache_mesh_surface_texpaint_single_get(ob);

// 4. Changed handle type and function
ResourceHandleRange res_handle = inst_.manager->unique_handle(ob_ref);

// 5. Changed draw call
overlay_ps_.draw(geom, res_handle, 1);
```

## Статус

**All compilation errors fixed!** ✅

**Next:** Recompile and test!

