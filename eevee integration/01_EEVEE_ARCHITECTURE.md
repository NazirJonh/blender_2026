# EEVEE Architecture Research

**Goal:** Понять архитектуру EEVEE для безопасной интеграции UV Checker overlay.

**Date:** 2025-01-11

## EEVEE Pipeline Overview

EEVEE (Extra Easy Virtual Environment Engine) - это real-time render engine в Blender, основанный на OpenGL/Vulkan rasterization.

### Key Components

```
EEVEE::Instance
├─ Film (framebuffer management, post-processing)
├─ Sync (scene data synchronization)
├─ RenderBuffers (G-buffer, depth, etc.)
├─ Materials (shader compilation)
├─ Lights (lighting setup)
└─ Pipeline (render passes)
```

## Investigation Tasks

### 1. EEVEE Initialization Order

**Question:** В каком порядке инициализируются компоненты EEVEE?

**Files to investigate:**
- `source/blender/draw/engines/eevee_next/eevee_instance.cc`
- `source/blender/draw/engines/eevee_next/eevee_sync.cc`
- `source/blender/draw/engines/eevee_next/eevee_film.cc`

### 2. Mesh Cache Management

**Question:** Как EEVEE управляет mesh cache и когда безопасно запрашивать batches?

**Files to investigate:**
- `source/blender/draw/intern/draw_cache_impl_mesh.cc`
- `source/blender/draw/engines/eevee_next/eevee_shader.cc`

### 3. Post-Process Hooks

**Question:** Где в pipeline можно вставить post-process pass?

**Files to investigate:**
- `source/blender/draw/engines/eevee_next/eevee_pipeline.cc`
- Existing post-process effects (bloom, DOF, etc.)

### 4. Depth Buffer Access

**Question:** Как получить доступ к EEVEE depth buffer из overlay?

**Files to investigate:**
- `source/blender/draw/engines/overlay/overlay_private.hh` (`state.is_render_depth_available`)
- EEVEE depth output

## Status

- [ ] EEVEE initialization order mapped
- [ ] Mesh cache timing understood
- [ ] Post-process hook points identified
- [ ] Depth buffer access method found
- [ ] Example implementation created

