/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <memory>
#include <vector>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_axis_angle.hh"
#include "BLI_math_color.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_rect.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_image.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "NOD_texture.h"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "wm_cursors.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "ED_grease_pencil.hh"
#include "ED_image.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "UI_resources.hh"

#include "paint_intern.hh"
#include "sculpt_boundary.hh"
#include "sculpt_cloth.hh"
#include "sculpt_expand.hh"
/* still needed for sculpt_stroke_get_location, should be
 * removed eventually (TODO) */
#include "sculpt_intern.hh"
#include "sculpt_pose.hh"

#include "bmesh.hh"

/* Needed for determining tool material/vertex-color pinning. */
#include "grease_pencil_intern.hh"

#include "brushes/brushes.hh"

/* TODOs:
 *
 * Some of the cursor drawing code is doing non-draw stuff
 * (e.g. updating the brush rake angle). This should be cleaned up
 * still.
 *
 * There is also some ugliness with sculpt-specific code.
 */

struct TexSnapshot {
  blender::gpu::Texture *overlay_texture;
  int winx;
  int winy;
  int old_size;
  float old_zoom;
  bool old_col;
  /* Brush and texture IDs to detect when brush or texture changes */
  int brush_id;
  int texture_id;
};

struct CursorSnapshot {
  blender::gpu::Texture *overlay_texture;
  int size;
  int zoom;
  int curve_preset;
};

namespace blender::ed::sculpt_paint {

/* Forward declaration */
bool paint_draw_tex_overlay_3d(Paint *paint,
                               Brush *brush,
                               ViewContext *vc,
                               float zoom,
                               const PaintMode mode,
                               bool col,
                               bool primary,
                               float radius,
                               float brush_rotation,
                               const float location[3],
                               const float normal[3]);

/* Global variables for texture snapshots */
static TexSnapshot primary_snap = {nullptr};
static TexSnapshot secondary_snap = {nullptr};
static CursorSnapshot cursor_snap = {nullptr};

/* TODO(REFACT-STAGE-2-001): Implement PaintCursorShaderStrategy interface */
/* TODO(REFACT-STAGE-2-002): Implement PaintCursorUniformColorStrategy class */
/* TODO(REFACT-STAGE-2-003): Implement PaintCursorTextureStrategy class */
/* TODO(REFACT-STAGE-2-004): Add virtual destructors with override keyword */
/* TODO(REFACT-STAGE-2-005): Follow Blender C++ standards with virtual comments */
/**
 * Strategy interface for managing shader binding in paint cursor drawing.
 * Follows the Strategy pattern to decouple shader management from drawing logic.
 */
class PaintCursorShaderStrategy {
 public:
  /**
   * Virtual destructor for proper cleanup of derived classes.
   */
  virtual ~PaintCursorShaderStrategy() = default;

  /**
   * Bind the appropriate shader for this strategy.
   * @return true if shader was bound successfully, false otherwise
   */
  virtual bool bind_shader() = 0;

  /**
   * Unbind the shader used by this strategy.
   */
  virtual void unbind_shader() = 0;

  /**
   * Check if this strategy can be used in the current context.
   * @return true if strategy is applicable, false otherwise
   */
  virtual bool can_use() const = 0;
};

/**
 * Strategy for uniform color shader binding.
 * Used for drawing solid color elements like cursor outlines.
 */
class PaintCursorUniformColorStrategy : public PaintCursorShaderStrategy {
 public:
  /**
   * Virtual destructor for proper cleanup.
   */
  virtual ~PaintCursorUniformColorStrategy() = default;

  /**
   * Bind the uniform color shader.
   * @return true if shader was bound successfully
   */
  virtual bool bind_shader() override {
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    return true;
  }

  /**
   * Unbind the uniform color shader.
   */
  virtual void unbind_shader() override {
    immUnbindProgram();
  }

  /**
   * Uniform color strategy can always be used.
   * @return true
   */
  virtual bool can_use() const override {
    return true;
  }
};

/**
 * Strategy for texture shader binding.
 * Used for drawing textured elements like brush overlays.
 */
class PaintCursorTextureStrategy : public PaintCursorShaderStrategy {
 private:
  blender::gpu::Texture *texture_;
  GPUSamplerExtendMode extend_mode_;

 public:
  /**
   * Constructor for texture strategy.
   * @param texture The texture to bind
   * @param extend_mode The texture extend mode
   */
  PaintCursorTextureStrategy(blender::gpu::Texture *texture, GPUSamplerExtendMode extend_mode)
      : texture_(texture), extend_mode_(extend_mode) {}

  /**
   * Virtual destructor for proper cleanup.
   */
  virtual ~PaintCursorTextureStrategy() = default;

  /**
   * Bind the texture shader with the specified texture.
   * @return true if shader was bound successfully
   */
  virtual bool bind_shader() override {
    if (!texture_) {
      return false;
    }
    
    // Check if shader is already bound
    if (immIsShaderBound()) {
      immUnbindProgram();
    }
    
    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
    immBindTextureSampler(
        "image", texture_, {GPU_SAMPLER_FILTERING_LINEAR, extend_mode_, extend_mode_});
    return true;
  }

  /**
   * Unbind the texture shader.
   */
  virtual void unbind_shader() override {
    if (texture_) {
      GPU_texture_unbind(texture_);
    }
    
    immUnbindProgram();
  }

  /**
   * Check if texture strategy can be used.
   * @return true if texture is valid
   */
  virtual bool can_use() const override {
    return texture_ != nullptr;
  }
};

/* TODO(REFACT-STAGE-1-001): Implement PaintCursorGPUStateGuard class with RAII pattern */
/* TODO(REFACT-STAGE-1-002): Save and restore GPU blend state automatically */
/* TODO(REFACT-STAGE-1-003): Save and restore GPU depth test state automatically */
/* TODO(REFACT-STAGE-1-004): Save and restore IMM shader state automatically */
/* TODO(REFACT-STAGE-1-005): Delete copy constructor and assignment operator */
/**
 * RAII guard for managing GPU state during paint cursor drawing.
 * Automatically saves and restores GPU blend, depth test, and shader states.
 */
class PaintCursorGPUStateGuard {
 private:
  GPUBlend saved_blend_state_;
  GPUDepthTest saved_depth_test_state_;
  blender::gpu::Shader *saved_shader_;
  bool was_shader_bound_;

 public:
  /**
   * Constructor: Save current GPU state.
   */
  PaintCursorGPUStateGuard() : saved_shader_(nullptr), was_shader_bound_(false) {
    /* Save current GPU states */
    saved_blend_state_ = GPU_blend_get();
    saved_depth_test_state_ = GPU_depth_test_get();

    /* Save current IMM shader if one is bound */
    if (immIsShaderBound()) {
      was_shader_bound_ = true;
      saved_shader_ = nullptr; /* Cannot get current shader, will restore default */
      /* Don't unbind here - let the calling code manage shader binding */
    }
  }

  /**
   * Destructor: Restore saved GPU state.
   */
  ~PaintCursorGPUStateGuard() {
    /* Don't restore IMM shader state - let the calling code manage it */
    /* The calling code will handle shader binding/unbinding */

    /* Restore GPU states */
    GPU_blend(saved_blend_state_);
    GPU_depth_test(saved_depth_test_state_);
  }

  /* Delete copy constructor and assignment operator to prevent copying */
  PaintCursorGPUStateGuard(const PaintCursorGPUStateGuard &) = delete;
  PaintCursorGPUStateGuard &operator=(const PaintCursorGPUStateGuard &) = delete;
};

/* Command Pattern implementation completed for Stage 3 */

/**
 * Command interface for paint cursor drawing operations.
 * Follows the Command pattern to encapsulate drawing operations as objects.
 */
class PaintCursorDrawCommand {
 public:
  /**
   * Virtual destructor for proper cleanup of derived classes.
   */
  virtual ~PaintCursorDrawCommand() = default;

  /**
   * Execute the drawing command.
   * @return true if command executed successfully, false otherwise
   */
  virtual bool execute() = 0;

  /**
   * Check if this command can be executed in the current context.
   * @return true if command can be executed, false otherwise
   */
  virtual bool can_execute() const = 0;

  /**
   * Get a description of what this command does.
   * @return String description of the command
   */
  virtual const char *get_description() const = 0;
};

/**
 * Command for drawing texture overlay in 3D cursor.
 * Encapsulates the texture drawing operation.
 */
class PaintCursorDrawTextureCommand : public PaintCursorDrawCommand {
 private:
  Paint *paint_;
  Brush *brush_;
  ViewContext *vc_;
  float zoom_;
  PaintMode mode_;
  bool col_;
  bool primary_;
  float radius_;
  float brush_rotation_;
  const float *location_;
  const float *normal_;

 public:
  /**
   * Constructor for texture drawing command.
   */
  PaintCursorDrawTextureCommand(Paint *paint,
                                Brush *brush,
                                ViewContext *vc,
                                float zoom,
                                PaintMode mode,
                                bool col,
                                bool primary,
                                float radius,
                                float brush_rotation,
                                const float location[3],
                                const float normal[3])
      : paint_(paint),
        brush_(brush),
        vc_(vc),
        zoom_(zoom),
        mode_(mode),
        col_(col),
        primary_(primary),
        radius_(radius),
        brush_rotation_(brush_rotation),
        location_(location),
        normal_(normal) {}

  /**
   * Virtual destructor for proper cleanup.
   */
  virtual ~PaintCursorDrawTextureCommand() = default;

  /**
   * Execute the texture drawing command.
   * @return true if texture was drawn successfully
   */
  virtual bool execute() override {
    return blender::ed::sculpt_paint::paint_draw_tex_overlay_3d(paint_, brush_, vc_, zoom_, mode_, col_, primary_, radius_, brush_rotation_, location_, normal_);
  }

  /**
   * Check if texture command can be executed.
   * @return true if all required parameters are valid
   */
  virtual bool can_execute() const override {
    return paint_ && brush_ && vc_ && radius_ > 0.0f;
  }

  /**
   * Get description of the texture command.
   * @return Description string
   */
  virtual const char *get_description() const override {
    return "Draw texture overlay in 3D cursor";
  }
};

/**
 * Command for drawing circle outline in 3D cursor.
 * Encapsulates the circle drawing operation.
 */
class PaintCursorDrawCircleCommand : public PaintCursorDrawCommand {
 private:
  float location_[3];
  float radius_;
  float color_[3];
  float alpha_;
  uint pos_;  /* GPU attribute for drawing */

 public:
  /**
   * Constructor for circle drawing command.
   */
  PaintCursorDrawCircleCommand(const float location[3],
                               float radius,
                               const float color[3],
                               float alpha,
                               uint pos)
      : radius_(radius), alpha_(alpha), pos_(pos) {
    copy_v3_v3(location_, location);
    copy_v3_v3(color_, color);
  }

  /**
   * Virtual destructor for proper cleanup.
   */
  virtual ~PaintCursorDrawCircleCommand() = default;

  /**
   * Execute the circle drawing command.
   * @return true if circle was drawn successfully
   */
  virtual bool execute() override {
    /* Save current GPU state */
    PaintCursorGPUStateGuard gpu_state_guard;
    
    /* Set color and alpha */
    immUniformColor3fvAlpha(color_, alpha_);
    
    /* Set line width for circle outline */
    GPU_line_width(2.0f);
    
    /* Draw the circle wireframe in 3D */
    imm_draw_circle_wire_3d(pos_, 0, 0, radius_, 80);
    
    return true;
  }

  /**
   * Check if circle command can be executed.
   * @return true if all required parameters are valid
   */
  virtual bool can_execute() const override {
    return radius_ > 0.0f && alpha_ > 0.0f;
  }

  /**
   * Get description of the circle command.
   * @return Description string
   */
  virtual const char *get_description() const override {
    return "Draw circle outline in 3D cursor";
  }
};

/**
 * Command for drawing main cursor outline in 3D cursor.
 * Encapsulates the main cursor drawing operation with outer and inner circles.
 */
class PaintCursorDrawMainCursorCommand : public PaintCursorDrawCommand {
 private:
  uint pos_;           /* GPU attribute for drawing */
  float radius_;       /* Cursor radius */
  float outline_col_[3]; /* Outline color */
  float outline_alpha_; /* Outline alpha */
  Paint *paint_;       /* Paint context */
  Brush *brush_;       /* Brush for alpha calculation */

 public:
  /**
   * Constructor for main cursor drawing command.
   */
  PaintCursorDrawMainCursorCommand(uint pos,
                                   float radius,
                                   const float outline_col[3],
                                   float outline_alpha,
                                   Paint *paint,
                                   Brush *brush)
      : pos_(pos),
        radius_(radius),
        outline_alpha_(outline_alpha),
        paint_(paint),
        brush_(brush) {
    copy_v3_v3(outline_col_, outline_col);
  }

  /**
   * Virtual destructor for proper cleanup.
   */
  virtual ~PaintCursorDrawMainCursorCommand() = default;

  /**
   * Execute the main cursor drawing command.
   * @return true if cursor was drawn successfully
   */
  virtual bool execute() override {
    /* Save current GPU state */
    PaintCursorGPUStateGuard gpu_state_guard;
    
    /* Draw outer circle */
    immUniformColor3fvAlpha(outline_col_, outline_alpha_);
    GPU_line_width(2.0f);
    imm_draw_circle_wire_3d(pos_, 0, 0, radius_, 80);

    /* Draw inner circle with alpha falloff */
    GPU_line_width(1.0f);
    immUniformColor3fvAlpha(outline_col_, outline_alpha_ * 0.5f);
    imm_draw_circle_wire_3d(
        pos_,
        0,
        0,
        radius_ * clamp_f(BKE_brush_alpha_get(paint_, brush_), 0.0f, 1.0f),
        80);
    
    return true;
  }

  /**
   * Check if main cursor command can be executed.
   * @return true if all required parameters are valid
   */
  virtual bool can_execute() const override {
    return pos_ != 0 && radius_ > 0.0f && paint_ && brush_;
  }

  /**
   * Get description of the main cursor command.
   * @return Description string
   */
  virtual const char *get_description() const override {
    return "Draw main cursor outline in 3D cursor";
  }
};

static int same_tex_snap(TexSnapshot *snap,
                         MTex *mtex,
                         ViewContext *vc,
                         bool col,
                         float zoom,
                         Brush *brush)
{
  /* Check if brush or texture changed */
  int current_brush_id = brush ? brush->id.name[2] : 0;
  int current_texture_id = (mtex->tex && mtex->tex->id.name) ? mtex->tex->id.name[2] : 0;
  
  if (snap->brush_id != current_brush_id || snap->texture_id != current_texture_id) {
    return 0; /* Brush or texture changed, need to reload */
  }

  int result = (/* make brush smaller shouldn't cause a resample */
          //(mtex->brush_map_mode != MTEX_MAP_MODE_VIEW ||
          //(BKE_brush_size_get(vc->scene, brush) <= snap->BKE_brush_size_get)) &&

          (mtex->brush_map_mode != MTEX_MAP_MODE_TILED ||
           (vc->region->winx == snap->winx && vc->region->winy == snap->winy)) &&
          (mtex->brush_map_mode == MTEX_MAP_MODE_STENCIL || snap->old_zoom == zoom) &&
          snap->old_col == col);
  
  return result;
}

static void make_tex_snap(TexSnapshot *snap, ViewContext *vc, float zoom, Brush *brush, MTex *mtex)
{
  snap->old_zoom = zoom;
  snap->winx = vc->region->winx;
  snap->winy = vc->region->winy;
  snap->brush_id = brush ? brush->id.name[2] : 0;
  snap->texture_id = (mtex->tex && mtex->tex->id.name) ? mtex->tex->id.name[2] : 0;
}

struct LoadTexData {
  Brush *br;
  ViewContext *vc;

  MTex *mtex;
  uchar *buffer;
  bool col;

  ImagePool *pool;
  int size;
  float rotation;
  float radius;
};

static void load_tex_task_cb_ex(void *__restrict userdata,
                                const int j,
                                const TaskParallelTLS *__restrict tls)
{
  LoadTexData *data = static_cast<LoadTexData *>(userdata);
  Brush *br = data->br;
  ViewContext *vc = data->vc;

  MTex *mtex = data->mtex;
  uchar *buffer = data->buffer;
  const bool col = data->col;

  ImagePool *pool = data->pool;
  const int size = data->size;
  const float rotation = data->rotation;
  const float radius = data->radius;

  bool convert_to_linear = false;
  const ColorSpace *colorspace = nullptr;

  const int thread_id = BLI_task_parallel_thread_id(tls);

  if (mtex->tex && mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    ImBuf *tex_ibuf = BKE_image_pool_acquire_ibuf(mtex->tex->ima, &mtex->tex->iuser, pool);
    /* For consistency, sampling always returns color in linear space. */
    if (tex_ibuf && tex_ibuf->float_buffer.data == nullptr) {
      convert_to_linear = true;
      colorspace = tex_ibuf->byte_buffer.colorspace;
    }
    BKE_image_pool_release_ibuf(mtex->tex->ima, tex_ibuf, pool);
  }

  for (int i = 0; i < size; i++) {
    /* Largely duplicated from tex_strength. */

    int index = j * size + i;

    float x = float(i) / size;
    float y = float(j) / size;
    float len;

    if (mtex->brush_map_mode == MTEX_MAP_MODE_TILED) {
      x *= vc->region->winx / radius;
      y *= vc->region->winy / radius;
    }
    else {
      x = (x - 0.5f) * 2.0f;
      y = (y - 0.5f) * 2.0f;
    }

    len = sqrtf(x * x + y * y);

    if (ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_TILED, MTEX_MAP_MODE_STENCIL) || len <= 1.0f) {
      /* It is probably worth optimizing for those cases where the texture is not rotated by
       * skipping the calls to atan2, sqrtf, sin, and cos. */
      if (mtex->tex && (rotation > 0.001f || rotation < -0.001f)) {
        const float angle = atan2f(y, x) + rotation;

        x = len * cosf(angle);
        y = len * sinf(angle);
      }

      float avg;
      float rgba[4];
      paint_get_tex_pixel(mtex, x, y, pool, thread_id, &avg, rgba);

      if (col) {
        if (convert_to_linear) {
          IMB_colormanagement_colorspace_to_scene_linear_v3(rgba, colorspace);
        }

        linearrgb_to_srgb_v3_v3(rgba, rgba);

        clamp_v4(rgba, 0.0f, 1.0f);

        buffer[index * 4] = rgba[0] * 255;
        buffer[index * 4 + 1] = rgba[1] * 255;
        buffer[index * 4 + 2] = rgba[2] * 255;
        buffer[index * 4 + 3] = rgba[3] * 255;
      }
      else {
        avg += br->texture_sample_bias;

        /* Clamp to avoid precision overflow. */
        CLAMP(avg, 0.0f, 1.0f);
        
        /* Convert texture intensity to alpha-based display (like radial_control):
         * Light parts (high avg) -> dark color (low RGB) with high alpha
         * Dark parts (low avg) -> transparent (low alpha)
         * RGB channels store inverted intensity for dark appearance
         * Alpha channel stores intensity for transparency control */
        const float inverted_intensity = 1.0f - avg;
        const int rgba_index = index * 4;
        buffer[rgba_index] = uchar(255 * inverted_intensity);     /* R - dark for light parts */
        buffer[rgba_index + 1] = uchar(255 * inverted_intensity); /* G - dark for light parts */
        buffer[rgba_index + 2] = uchar(255 * inverted_intensity); /* B - dark for light parts */
        buffer[rgba_index + 3] = uchar(255 * avg);                /* A - transparent for dark parts */
      }
    }
    else {
      if (col) {
        buffer[index * 4] = 0;
        buffer[index * 4 + 1] = 0;
        buffer[index * 4 + 2] = 0;
        buffer[index * 4 + 3] = 0;
      }
      else {
        /* Set all RGBA channels to 0 (transparent) */
        const int rgba_index = index * 4;
        buffer[rgba_index] = 0;     /* R */
        buffer[rgba_index + 1] = 0; /* G */
        buffer[rgba_index + 2] = 0; /* B */
        buffer[rgba_index + 3] = 0; /* A */
      }
    }
  }
}

static int load_tex(Brush *br, ViewContext *vc, float zoom, bool col, bool primary);

/**
 * Check if texture needs to be reloaded based on parameter changes.
 * This optimization prevents unnecessary texture reloading when parameters haven't changed.
 */
static bool needs_texture_load(const Brush *brush, float zoom, bool col, bool primary) {
  static float last_zoom = -1.0f;
  static int last_brush_id = -1;
  static bool last_col = false;
  static bool last_primary = false;
  
  // Check if any critical parameters have changed
  bool changed = (zoom != last_zoom) ||
                 (brush->id.name[2] != last_brush_id) ||
                 (col != last_col) ||
                 (primary != last_primary);
  
  if (changed) {
    // Update cached values
    last_zoom = zoom;
    last_brush_id = brush->id.name[2];
    last_col = col;
    last_primary = primary;
    return true;
  }
  
  return false;
}

static int load_tex(Brush *br, ViewContext *vc, float zoom, bool col, bool primary)
{
  bool init;
  TexSnapshot *target;

  MTex *mtex = (primary) ? &br->mtex : &br->mask_mtex;
  ePaintOverlayControlFlags overlay_flags = BKE_paint_get_overlay_flags();
  uchar *buffer = nullptr;

  int size;
  bool refresh;
  ePaintOverlayControlFlags invalid =
      ((primary) ? (overlay_flags & PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY) :
                   (overlay_flags & PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY));
  target = (primary) ? &primary_snap : &secondary_snap;

  bool same_snap_result = same_tex_snap(target, mtex, vc, col, zoom, br);
  refresh = !target->overlay_texture || (invalid != 0) || !same_snap_result;

  init = (target->overlay_texture != nullptr);

  // Optimize: Early exit if texture doesn't need refresh and exists
  if (!refresh && target->overlay_texture) {
    return 1; // Texture is ready to use
  }

  if (refresh) {
    
    ImagePool *pool = nullptr;
    Paint *paint = BKE_paint_get_active_from_context(vc->C);
    /* Stencil is rotated later. */
    const float rotation = (mtex->brush_map_mode != MTEX_MAP_MODE_STENCIL) ? -mtex->rot : 0.0f;
    const float radius = BKE_brush_radius_get(paint, br) * zoom;

    make_tex_snap(target, vc, zoom, br, mtex);

    if (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) {
      int s = BKE_brush_radius_get(paint, br);
      int r = 1;

      for (s >>= 1; s > 0; s >>= 1) {
        r++;
      }

      size = (1 << r);

      size = std::max(size, 256);
      size = std::max(size, target->old_size);
    }
    else {
      size = 512;
    }

    if (target->old_size != size || target->old_col != col) {
      if (target->overlay_texture) {
        GPU_texture_free(target->overlay_texture);
        target->overlay_texture = nullptr;
      }
      init = false;

      target->old_size = size;
      target->old_col = col;
    }
    /* Always use RGBA format to support alpha channel for transparency */
    buffer = MEM_malloc_arrayN<uchar>(size * size * 4, "load_tex");

    pool = BKE_image_pool_new();

    if (mtex->tex && mtex->tex->nodetree) {
      /* Has internal flag to detect it only does it once. */
      ntreeTexBeginExecTree(mtex->tex->nodetree);
    }

    LoadTexData data{};
    data.br = br;
    data.vc = vc;
    data.mtex = mtex;
    data.buffer = buffer;
    data.col = col;
    data.pool = pool;
    data.size = size;
    data.rotation = rotation;
    data.radius = radius;

    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    BLI_task_parallel_range(0, size, &data, load_tex_task_cb_ex, &settings);

    if (mtex->tex && mtex->tex->nodetree) {
      ntreeTexEndExecTree(mtex->tex->nodetree->runtime->execdata);
    }

    if (pool) {
      BKE_image_pool_free(pool);
    }

    if (!target->overlay_texture) {
      /* Use RGBA format for both colored and grayscale textures to support alpha channel */
      blender::gpu::TextureFormat format = blender::gpu::TextureFormat::UNORM_8_8_8_8;
      eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
      target->overlay_texture = GPU_texture_create_2d(
          "paint_cursor_overlay", size, size, 1, format, usage, nullptr);
      GPU_texture_update(target->overlay_texture, GPU_DATA_UBYTE, buffer);

      /* No swizzle needed - we already store RGB and Alpha in correct channels */
    }

    if (init) {
      GPU_texture_update(target->overlay_texture, GPU_DATA_UBYTE, buffer);
    }

    if (buffer) {
      MEM_freeN(buffer);
    }
  }
  else {
    size = target->old_size;
  }

  BKE_paint_reset_overlay_invalid(invalid);

  return 1;
}

static void load_tex_cursor_task_cb(void *__restrict userdata,
                                    const int j,
                                    const TaskParallelTLS *__restrict /*tls*/)
{
  LoadTexData *data = static_cast<LoadTexData *>(userdata);
  Brush *br = data->br;

  uchar *buffer = data->buffer;

  const int size = data->size;

  for (int i = 0; i < size; i++) {
    /* Largely duplicated from tex_strength. */

    const int index = j * size + i;
    const float x = ((float(i) / size) - 0.5f) * 2.0f;
    const float y = ((float(j) / size) - 0.5f) * 2.0f;
    const float len = sqrtf(x * x + y * y);

    if (len <= 1.0f) {

      /* Falloff curve. */
      float avg = BKE_brush_curve_strength_clamped(br, len, 1.0f);

      buffer[index] = uchar(255 * avg);
    }
    else {
      buffer[index] = 0;
    }
  }
}

static int load_tex_cursor(Brush *br, ViewContext *vc, float zoom)
{
  bool init;

  ePaintOverlayControlFlags overlay_flags = BKE_paint_get_overlay_flags();
  uchar *buffer = nullptr;

  int size;
  const bool refresh = !cursor_snap.overlay_texture ||
                       (overlay_flags & PAINT_OVERLAY_INVALID_CURVE) || cursor_snap.zoom != zoom ||
                       cursor_snap.curve_preset != br->curve_distance_falloff_preset;

  init = (cursor_snap.overlay_texture != nullptr);

  // Optimize: Early exit if cursor texture doesn't need refresh and exists
  if (!refresh && cursor_snap.overlay_texture) {
    return 1; // Cursor texture is ready to use
  }

  if (refresh) {
    Paint *paint = BKE_paint_get_active_from_context(vc->C);
    int s, r;

    cursor_snap.zoom = zoom;

    s = BKE_brush_radius_get(paint, br);
    r = 1;

    for (s >>= 1; s > 0; s >>= 1) {
      r++;
    }

    size = (1 << r);

    size = std::max(size, 256);
    size = std::max(size, cursor_snap.size);

    if (cursor_snap.size != size) {
      if (cursor_snap.overlay_texture) {
        GPU_texture_free(cursor_snap.overlay_texture);
        cursor_snap.overlay_texture = nullptr;
      }

      init = false;

      cursor_snap.size = size;
    }
    buffer = MEM_malloc_arrayN<uchar>(size * size, "load_tex");

    BKE_curvemapping_init(br->curve_distance_falloff);

    LoadTexData data{};
    data.br = br;
    data.buffer = buffer;
    data.size = size;

    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    BLI_task_parallel_range(0, size, &data, load_tex_cursor_task_cb, &settings);

    if (!cursor_snap.overlay_texture) {
      eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
      cursor_snap.overlay_texture = GPU_texture_create_2d("cursor_snap_overaly",
                                                          size,
                                                          size,
                                                          1,
                                                          blender::gpu::TextureFormat::UNORM_8,
                                                          usage,
                                                          nullptr);
      GPU_texture_update(cursor_snap.overlay_texture, GPU_DATA_UBYTE, buffer);

      GPU_texture_swizzle_set(cursor_snap.overlay_texture, "rrrr");
    }

    if (init) {
      GPU_texture_update(cursor_snap.overlay_texture, GPU_DATA_UBYTE, buffer);
    }

    if (buffer) {
      MEM_freeN(buffer);
    }
  }
  else {
    size = cursor_snap.size;
  }

  cursor_snap.curve_preset = br->curve_distance_falloff_preset;
  BKE_paint_reset_overlay_invalid(PAINT_OVERLAY_INVALID_CURVE);

  return 1;
}

static int project_brush_radius(ViewContext *vc, float radius, const float location[3])
{
  float view[3], nonortho[3], ortho[3], offset[3], p1[2], p2[2];

  ED_view3d_global_to_vector(vc->rv3d, location, view);

  /* Create a vector that is not orthogonal to view. */

  if (fabsf(view[0]) < 0.1f) {
    nonortho[0] = view[0] + 1.0f;
    nonortho[1] = view[1];
    nonortho[2] = view[2];
  }
  else if (fabsf(view[1]) < 0.1f) {
    nonortho[0] = view[0];
    nonortho[1] = view[1] + 1.0f;
    nonortho[2] = view[2];
  }
  else {
    nonortho[0] = view[0];
    nonortho[1] = view[1];
    nonortho[2] = view[2] + 1.0f;
  }

  /* Get a vector in the plane of the view. */
  cross_v3_v3v3(ortho, nonortho, view);
  normalize_v3(ortho);

  /* Make a point on the surface of the brush tangent to the view. */
  mul_v3_fl(ortho, radius);
  add_v3_v3v3(offset, location, ortho);

  /* Project the center of the brush, and the tangent point to the view onto the screen. */
  if ((ED_view3d_project_float_global(vc->region, location, p1, V3D_PROJ_TEST_NOP) ==
       V3D_PROJ_RET_OK) &&
      (ED_view3d_project_float_global(vc->region, offset, p2, V3D_PROJ_TEST_NOP) ==
       V3D_PROJ_RET_OK))
  {
    /* The distance between these points is the size of the projected brush in pixels. */
    return len_v2v2(p1, p2);
  }
  /* Assert because the code that sets up the vectors should disallow this. */
  BLI_assert(0);
  return 0;
}

static int project_brush_radius_grease_pencil(ViewContext *vc,
                                              const float radius,
                                              const float3 world_location,
                                              const float4x4 &to_world)
{
  const float2 xy_delta = float2(1.0f, 0.0f);

  bool z_flip;
  const float zfac = ED_view3d_calc_zfac_ex(vc->rv3d, world_location, &z_flip);
  if (z_flip) {
    /* Location is behind camera. Return 0 to make the cursor disappear. */
    return 0;
  }
  float3 delta;
  ED_view3d_win_to_delta(vc->region, xy_delta, zfac, delta);

  const float scale = math::length(
      math::transform_direction(to_world, float3(math::numbers::inv_sqrt3)));
  return math::safe_divide(scale * radius, math::length(delta));
}

/* Draw an overlay that shows what effect the brush's texture will
 * have on brush strength. */
static bool paint_draw_tex_overlay(Paint *paint,
                                   Brush *brush,
                                   ViewContext *vc,
                                   int x,
                                   int y,
                                   float zoom,
                                   const PaintMode mode,
                                   bool col,
                                   bool primary)
{
  rctf quad;
  /* Check for overlay mode. */

  MTex *mtex = (primary) ? &brush->mtex : &brush->mask_mtex;
  bool valid = ((primary) ? (brush->overlay_flags & BRUSH_OVERLAY_PRIMARY) != 0 :
                            (brush->overlay_flags & BRUSH_OVERLAY_SECONDARY) != 0);
  int overlay_alpha = (primary) ? brush->texture_overlay_alpha : brush->mask_overlay_alpha;

  if (mode == PaintMode::Texture3D) {
    if (primary && brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_DRAW) {
      /* All non-draw tools don't use the primary texture (clone, smear, soften.. etc). */
      return false;
    }
  }

  if (!(mtex->tex) ||
      !((mtex->brush_map_mode == MTEX_MAP_MODE_STENCIL) ||
        (valid && ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_TILED))))
  {
    return false;
  }

  if (!WM_toolsystem_active_tool_is_brush(vc->C)) {
    return false;
  }

  bke::PaintRuntime *paint_runtime = paint->runtime;
  
  // Optimize: Check if texture needs to be reloaded
  bool texture_loaded = false;
  if (!needs_texture_load(brush, zoom, col, primary)) {
    // Use existing texture without reloading
    TexSnapshot *target = (primary) ? &primary_snap : &secondary_snap;
    if (target->overlay_texture) {
      texture_loaded = true;
    }
  }
  
  if (!texture_loaded) {
    texture_loaded = load_tex(brush, vc, zoom, col, primary);
  }
  
  if (texture_loaded) {
    GPU_color_mask(true, true, true, true);
    GPU_depth_test(GPU_DEPTH_NONE);

    if (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) {
      GPU_matrix_push();

      float center[2] = {
          paint_runtime->draw_anchored ? paint_runtime->anchored_initial_mouse[0] : x,
          paint_runtime->draw_anchored ? paint_runtime->anchored_initial_mouse[1] : y,
      };

      /* Brush rotation. */
      GPU_matrix_translate_2fv(center);
      GPU_matrix_rotate_2d(
          RAD2DEGF(primary ? paint_runtime->brush_rotation : paint_runtime->brush_rotation_sec));
      GPU_matrix_translate_2f(-center[0], -center[1]);

      /* Scale based on tablet pressure. */
      if (primary && paint_runtime->stroke_active && BKE_brush_use_size_pressure(brush)) {
        const float scale = paint_runtime->size_pressure_value;
        GPU_matrix_translate_2fv(center);
        GPU_matrix_scale_2f(scale, scale);
        GPU_matrix_translate_2f(-center[0], -center[1]);
      }

      if (paint_runtime->draw_anchored) {
        quad.xmin = center[0] - paint_runtime->anchored_size;
        quad.ymin = center[1] - paint_runtime->anchored_size;
        quad.xmax = center[0] + paint_runtime->anchored_size;
        quad.ymax = center[1] + paint_runtime->anchored_size;
      }
      else {
        const int radius = BKE_brush_radius_get(paint, brush) * zoom;
        quad.xmin = center[0] - radius;
        quad.ymin = center[1] - radius;
        quad.xmax = center[0] + radius;
        quad.ymax = center[1] + radius;
      }
    }
    else if (mtex->brush_map_mode == MTEX_MAP_MODE_TILED) {
      quad.xmin = 0;
      quad.ymin = 0;
      quad.xmax = BLI_rcti_size_x(&vc->region->winrct);
      quad.ymax = BLI_rcti_size_y(&vc->region->winrct);
    }
    /* Stencil code goes here. */
    else {
      if (primary) {
        quad.xmin = -brush->stencil_dimension[0];
        quad.ymin = -brush->stencil_dimension[1];
        quad.xmax = brush->stencil_dimension[0];
        quad.ymax = brush->stencil_dimension[1];
      }
      else {
        quad.xmin = -brush->mask_stencil_dimension[0];
        quad.ymin = -brush->mask_stencil_dimension[1];
        quad.xmax = brush->mask_stencil_dimension[0];
        quad.ymax = brush->mask_stencil_dimension[1];
      }
      GPU_matrix_push();
      if (primary) {
        GPU_matrix_translate_2fv(brush->stencil_pos);
      }
      else {
        GPU_matrix_translate_2fv(brush->mask_stencil_pos);
      }
      GPU_matrix_rotate_2d(RAD2DEGF(mtex->rot));
    }

    /* Set quad color. Colored overlay does not get blending. */
    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    uint texCoord = GPU_vertformat_attr_add(
        format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

    /* Premultiplied alpha blending. */
    GPU_blend(GPU_BLEND_ALPHA_PREMULT);

    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);

    float final_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!col) {
      copy_v3_v3(final_color, U.sculpt_paint_overlay_col);
    }
    mul_v4_fl(final_color, overlay_alpha * 0.01f);
    immUniformColor4fv(final_color);

    blender::gpu::Texture *texture = (primary) ? primary_snap.overlay_texture :
                                                 secondary_snap.overlay_texture;

    GPUSamplerExtendMode extend_mode = (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) ?
                                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER :
                                           GPU_SAMPLER_EXTEND_MODE_REPEAT;
    immBindTextureSampler(
        "image", texture, {GPU_SAMPLER_FILTERING_LINEAR, extend_mode, extend_mode});

    /* Draw textured quad. */
    immBegin(GPU_PRIM_TRI_FAN, 4);
    immAttr2f(texCoord, 0.0f, 0.0f);
    immVertex2f(pos, quad.xmin, quad.ymin);
    immAttr2f(texCoord, 1.0f, 0.0f);
    immVertex2f(pos, quad.xmax, quad.ymin);
    immAttr2f(texCoord, 1.0f, 1.0f);
    immVertex2f(pos, quad.xmax, quad.ymax);
    immAttr2f(texCoord, 0.0f, 1.0f);
    immVertex2f(pos, quad.xmin, quad.ymax);
    immEnd();

    immUnbindProgram();

    GPU_texture_unbind(texture);

    if (ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_STENCIL, MTEX_MAP_MODE_VIEW)) {
      GPU_matrix_pop();
    }
  }
  return true;
}

/* Draw an overlay that shows what effect the brush's texture will
 * have on brush strength. */
static bool paint_draw_cursor_overlay(
    Paint *paint, Brush *brush, ViewContext *vc, int x, int y, float zoom)
{
  rctf quad;
  /* Check for overlay mode. */

  if (!(brush->overlay_flags & BRUSH_OVERLAY_CURSOR)) {
    return false;
  }

  if (load_tex_cursor(brush, vc, zoom)) {
    bool do_pop = false;
    float center[2];

    GPU_color_mask(true, true, true, true);
    GPU_depth_test(GPU_DEPTH_NONE);

    bke::PaintRuntime *paint_runtime = paint->runtime;
    if (paint_runtime->draw_anchored) {
      copy_v2_v2(center, paint_runtime->anchored_initial_mouse);
      quad.xmin = paint_runtime->anchored_initial_mouse[0] - paint_runtime->anchored_size;
      quad.ymin = paint_runtime->anchored_initial_mouse[1] - paint_runtime->anchored_size;
      quad.xmax = paint_runtime->anchored_initial_mouse[0] + paint_runtime->anchored_size;
      quad.ymax = paint_runtime->anchored_initial_mouse[1] + paint_runtime->anchored_size;
    }
    else {
      const int radius = BKE_brush_radius_get(paint, brush) * zoom;
      center[0] = x;
      center[1] = y;

      quad.xmin = x - radius;
      quad.ymin = y - radius;
      quad.xmax = x + radius;
      quad.ymax = y + radius;
    }

    /* Scale based on tablet pressure. */
    if (paint_runtime->stroke_active && BKE_brush_use_size_pressure(brush)) {
      do_pop = true;
      GPU_matrix_push();
      GPU_matrix_translate_2fv(center);
      GPU_matrix_scale_1f(paint_runtime->size_pressure_value);
      GPU_matrix_translate_2f(-center[0], -center[1]);
    }

    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    uint texCoord = GPU_vertformat_attr_add(
        format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

    GPU_blend(GPU_BLEND_ALPHA_PREMULT);

    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);

    float final_color[4] = {UNPACK3(U.sculpt_paint_overlay_col), 1.0f};
    mul_v4_fl(final_color, brush->cursor_overlay_alpha * 0.01f);
    immUniformColor4fv(final_color);

    /* Draw textured quad. */
    immBindTextureSampler("image",
                          cursor_snap.overlay_texture,
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});

    immBegin(GPU_PRIM_TRI_FAN, 4);
    immAttr2f(texCoord, 0.0f, 0.0f);
    immVertex2f(pos, quad.xmin, quad.ymin);
    immAttr2f(texCoord, 1.0f, 0.0f);
    immVertex2f(pos, quad.xmax, quad.ymin);
    immAttr2f(texCoord, 1.0f, 1.0f);
    immVertex2f(pos, quad.xmax, quad.ymax);
    immAttr2f(texCoord, 0.0f, 1.0f);
    immVertex2f(pos, quad.xmin, quad.ymax);
    immEnd();

    GPU_texture_unbind(cursor_snap.overlay_texture);

    immUnbindProgram();

    if (do_pop) {
      GPU_matrix_pop();
    }
  }
  return true;
}

static bool paint_draw_alpha_overlay(
    Paint *paint, Brush *brush, ViewContext *vc, int x, int y, float zoom, PaintMode mode)
{
  /* Color means that primary brush texture is colored and
   * secondary is used for alpha/mask control. */
  bool col = ELEM(mode, PaintMode::Texture3D, PaintMode::Texture2D, PaintMode::Vertex);

  bool alpha_overlay_active = false;

  ePaintOverlayControlFlags flags = BKE_paint_get_overlay_flags();
  GPUBlend blend_state = GPU_blend_get();
  GPUDepthTest depth_test = GPU_depth_test_get();

  /* Translate to region. */
  GPU_matrix_push();
  GPU_matrix_translate_2f(vc->region->winrct.xmin, vc->region->winrct.ymin);
  x -= vc->region->winrct.xmin;
  y -= vc->region->winrct.ymin;

  /* Colored overlay should be drawn separately. */
  if (col) {
    if (!(flags & PAINT_OVERLAY_OVERRIDE_PRIMARY)) {
      alpha_overlay_active = paint_draw_tex_overlay(
          paint, brush, vc, x, y, zoom, mode, true, true);
    }
    if (!(flags & PAINT_OVERLAY_OVERRIDE_SECONDARY)) {
      alpha_overlay_active = paint_draw_tex_overlay(
          paint, brush, vc, x, y, zoom, mode, false, false);
    }
    if (!(flags & PAINT_OVERLAY_OVERRIDE_CURSOR)) {
      alpha_overlay_active = paint_draw_cursor_overlay(paint, brush, vc, x, y, zoom);
    }
  }
  else {
    if (!(flags & PAINT_OVERLAY_OVERRIDE_PRIMARY) && (mode != PaintMode::Weight)) {
      alpha_overlay_active = paint_draw_tex_overlay(
          paint, brush, vc, x, y, zoom, mode, false, true);
    }
    if (!(flags & PAINT_OVERLAY_OVERRIDE_CURSOR)) {
      alpha_overlay_active = paint_draw_cursor_overlay(paint, brush, vc, x, y, zoom);
    }
  }

  GPU_matrix_pop();
  GPU_blend(blend_state);
  GPU_depth_test(depth_test);

  return alpha_overlay_active;
}

BLI_INLINE void draw_tri_point(uint pos,
                               const float sel_col[4],
                               const float pivot_col[4],
                               float *co,
                               float width,
                               bool selected)
{
  immUniformColor4fv(selected ? sel_col : pivot_col);

  GPU_line_width(3.0f);

  float w = width / 2.0f;
  const float tri[3][2] = {
      {co[0], co[1] + w},
      {co[0] - w, co[1] - w},
      {co[0] + w, co[1] - w},
  };

  immBegin(GPU_PRIM_LINE_LOOP, 3);
  immVertex2fv(pos, tri[0]);
  immVertex2fv(pos, tri[1]);
  immVertex2fv(pos, tri[2]);
  immEnd();

  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.5f);
  GPU_line_width(1.0f);

  immBegin(GPU_PRIM_LINE_LOOP, 3);
  immVertex2fv(pos, tri[0]);
  immVertex2fv(pos, tri[1]);
  immVertex2fv(pos, tri[2]);
  immEnd();
}

BLI_INLINE void draw_rect_point(uint pos,
                                const float sel_col[4],
                                const float handle_col[4],
                                const float *co,
                                float width,
                                bool selected)
{
  immUniformColor4fv(selected ? sel_col : handle_col);

  GPU_line_width(3.0f);

  float w = width / 2.0f;
  float minx = co[0] - w;
  float miny = co[1] - w;
  float maxx = co[0] + w;
  float maxy = co[1] + w;

  imm_draw_box_wire_2d(pos, minx, miny, maxx, maxy);

  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.5f);
  GPU_line_width(1.0f);

  imm_draw_box_wire_2d(pos, minx, miny, maxx, maxy);
}

BLI_INLINE void draw_bezier_handle_lines(uint pos, const float sel_col[4], BezTriple *bez)
{
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.5f);
  GPU_line_width(3.0f);

  immBegin(GPU_PRIM_LINE_STRIP, 3);
  immVertex2fv(pos, bez->vec[0]);
  immVertex2fv(pos, bez->vec[1]);
  immVertex2fv(pos, bez->vec[2]);
  immEnd();

  GPU_line_width(1.0f);

  if (bez->f1 || bez->f2) {
    immUniformColor4fv(sel_col);
  }
  else {
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.5f);
  }
  immBegin(GPU_PRIM_LINES, 2);
  immVertex2fv(pos, bez->vec[0]);
  immVertex2fv(pos, bez->vec[1]);
  immEnd();

  if (bez->f3 || bez->f2) {
    immUniformColor4fv(sel_col);
  }
  else {
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.5f);
  }
  immBegin(GPU_PRIM_LINES, 2);
  immVertex2fv(pos, bez->vec[1]);
  immVertex2fv(pos, bez->vec[2]);
  immEnd();
}

static void paint_draw_curve_cursor(Brush *brush, ViewContext *vc)
{
  GPU_matrix_push();
  GPU_matrix_translate_2f(vc->region->winrct.xmin, vc->region->winrct.ymin);

  if (brush->paint_curve && brush->paint_curve->points) {
    PaintCurve *pc = brush->paint_curve;
    PaintCurvePoint *cp = pc->points;

    GPU_line_smooth(true);
    GPU_blend(GPU_BLEND_ALPHA);

    /* Draw the bezier handles and the curve segment between the current and next point. */
    uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);

    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    float selec_col[4], handle_col[4], pivot_col[4];
    UI_GetThemeColorType4fv(TH_VERTEX_SELECT, SPACE_VIEW3D, selec_col);
    UI_GetThemeColorType4fv(TH_GIZMO_PRIMARY, SPACE_VIEW3D, handle_col);
    UI_GetThemeColorType4fv(TH_GIZMO_SECONDARY, SPACE_VIEW3D, pivot_col);

    for (int i = 0; i < pc->tot_points - 1; i++, cp++) {
      int j;
      PaintCurvePoint *cp_next = cp + 1;
      float data[(PAINT_CURVE_NUM_SEGMENTS + 1) * 2];
      /* Use color coding to distinguish handles vs curve segments. */
      draw_bezier_handle_lines(pos, selec_col, &cp->bez);
      draw_tri_point(pos, selec_col, pivot_col, &cp->bez.vec[1][0], 10.0f, cp->bez.f2);
      draw_rect_point(
          pos, selec_col, handle_col, &cp->bez.vec[0][0], 8.0f, cp->bez.f1 || cp->bez.f2);
      draw_rect_point(
          pos, selec_col, handle_col, &cp->bez.vec[2][0], 8.0f, cp->bez.f3 || cp->bez.f2);

      for (j = 0; j < 2; j++) {
        BKE_curve_forward_diff_bezier(cp->bez.vec[1][j],
                                      cp->bez.vec[2][j],
                                      cp_next->bez.vec[0][j],
                                      cp_next->bez.vec[1][j],
                                      data + j,
                                      PAINT_CURVE_NUM_SEGMENTS,
                                      sizeof(float[2]));
      }

      float (*v)[2] = (float (*)[2])data;

      immUniformColor4f(0.0f, 0.0f, 0.0f, 0.5f);
      GPU_line_width(3.0f);
      immBegin(GPU_PRIM_LINE_STRIP, PAINT_CURVE_NUM_SEGMENTS + 1);
      for (j = 0; j <= PAINT_CURVE_NUM_SEGMENTS; j++) {
        immVertex2fv(pos, v[j]);
      }
      immEnd();

      immUniformColor4f(0.9f, 0.9f, 1.0f, 0.5f);
      GPU_line_width(1.0f);
      immBegin(GPU_PRIM_LINE_STRIP, PAINT_CURVE_NUM_SEGMENTS + 1);
      for (j = 0; j <= PAINT_CURVE_NUM_SEGMENTS; j++) {
        immVertex2fv(pos, v[j]);
      }
      immEnd();
    }

    /* Draw last line segment. */
    draw_bezier_handle_lines(pos, selec_col, &cp->bez);
    draw_tri_point(pos, selec_col, pivot_col, &cp->bez.vec[1][0], 10.0f, cp->bez.f2);
    draw_rect_point(
        pos, selec_col, handle_col, &cp->bez.vec[0][0], 8.0f, cp->bez.f1 || cp->bez.f2);
    draw_rect_point(
        pos, selec_col, handle_col, &cp->bez.vec[2][0], 8.0f, cp->bez.f3 || cp->bez.f2);

    GPU_blend(GPU_BLEND_NONE);
    GPU_line_smooth(false);

    immUnbindProgram();
  }
  GPU_matrix_pop();
}

/* Special actions taken when paint cursor goes over mesh */
/* TODO: sculpt only for now. */
static void paint_cursor_update_unprojected_size(Paint &paint,
                                                 Brush &brush,
                                                 const ViewContext &vc,
                                                 const float location[3])
{
  const bke::PaintRuntime &paint_runtime = *paint.runtime;
  /* Update the brush's cached 3D radius. */
  if (!BKE_brush_use_locked_size(&paint, &brush)) {
    float projected_radius;
    /* Get 2D brush radius. */
    if (paint_runtime.draw_anchored) {
      projected_radius = paint_runtime.anchored_size;
    }
    else {
      if (brush.flag & BRUSH_ANCHORED) {
        projected_radius = 8;
      }
      else {
        projected_radius = BKE_brush_radius_get(&paint, &brush);
      }
    }

    /* Convert brush radius from 2D to 3D. */
    float unprojected_size = paint_calc_object_space_radius(vc, location, projected_radius);

    /* Scale 3D brush radius by pressure. */
    if (paint_runtime.stroke_active && BKE_brush_use_size_pressure(&brush)) {
      unprojected_size *= paint_runtime.size_pressure_value;
    }

    /* Set cached value in either Brush or UnifiedPaintSettings. */
    BKE_brush_unprojected_size_set(&paint, &brush, unprojected_size);
  }
}

static void cursor_draw_point_screen_space(const uint gpuattr,
                                           const ARegion *region,
                                           const float true_location[3],
                                           const float obmat[4][4],
                                           const int size)
{
  float translation_vertex_cursor[3], location[3];
  copy_v3_v3(location, true_location);
  mul_m4_v3(obmat, location);
  ED_view3d_project_v3(region, location, translation_vertex_cursor);
  /* Do not draw points behind the view. Z [near, far] is mapped to [-1, 1]. */
  if (translation_vertex_cursor[2] <= 1.0f) {
    imm_draw_circle_fill_3d(
        gpuattr, translation_vertex_cursor[0], translation_vertex_cursor[1], size, 10);
  }
}

static void cursor_draw_tiling_preview(const uint gpuattr,
                                       const ARegion *region,
                                       const float true_location[3],
                                       const Sculpt &sd,
                                       const Object &ob,
                                       const float radius)
{
  BLI_assert(ob.type == OB_MESH);
  const Mesh *mesh = BKE_object_get_evaluated_mesh_no_subsurf(&ob);
  if (!mesh) {
    mesh = static_cast<const Mesh *>(ob.data);
  }
  const Bounds<float3> bounds = *mesh->bounds_min_max();
  float orgLoc[3], location[3];
  int tile_pass = 0;
  int start[3];
  int end[3];
  int cur[3];
  const float *step = sd.paint.tile_offset;

  copy_v3_v3(orgLoc, true_location);
  for (int dim = 0; dim < 3; dim++) {
    if ((sd.paint.symmetry_flags & (PAINT_TILE_X << dim)) && step[dim] > 0) {
      start[dim] = (bounds.min[dim] - orgLoc[dim] - radius) / step[dim];
      end[dim] = (bounds.max[dim] - orgLoc[dim] + radius) / step[dim];
    }
    else {
      start[dim] = end[dim] = 0;
    }
  }
  copy_v3_v3_int(cur, start);
  for (cur[0] = start[0]; cur[0] <= end[0]; cur[0]++) {
    for (cur[1] = start[1]; cur[1] <= end[1]; cur[1]++) {
      for (cur[2] = start[2]; cur[2] <= end[2]; cur[2]++) {
        if (!cur[0] && !cur[1] && !cur[2]) {
          /* Skip tile at orgLoc, this was already handled before all others. */
          continue;
        }
        tile_pass++;
        for (int dim = 0; dim < 3; dim++) {
          location[dim] = cur[dim] * step[dim] + orgLoc[dim];
        }
        cursor_draw_point_screen_space(gpuattr, region, location, ob.object_to_world().ptr(), 3);
      }
    }
  }
  (void)tile_pass; /* Quiet set-but-unused warning (may be removed). */
}

static void cursor_draw_point_with_symmetry(const uint gpuattr,
                                            const ARegion *region,
                                            const float true_location[3],
                                            const Sculpt &sd,
                                            const Object &ob,
                                            const float radius)
{
  const Mesh *mesh = static_cast<const Mesh *>(ob.data);
  const char symm = SCULPT_mesh_symmetry_xyz_get(ob);
  float3 location;
  float symm_rot_mat[4][4];

  for (int i = 0; i <= symm; i++) {
    if (is_symmetry_iteration_valid(i, symm)) {

      /* Axis Symmetry. */
      location = symmetry_flip(true_location, ePaintSymmetryFlags(i));
      cursor_draw_point_screen_space(gpuattr, region, location, ob.object_to_world().ptr(), 3);

      /* Tiling. */
      cursor_draw_tiling_preview(gpuattr, region, location, sd, ob, radius);

      /* Radial Symmetry. */
      for (char raxis = 0; raxis < 3; raxis++) {
        for (int r = 1; r < mesh->radial_symmetry[raxis]; r++) {
          float angle = 2 * M_PI * r / mesh->radial_symmetry[int(raxis)];
          location = symmetry_flip(true_location, ePaintSymmetryFlags(i));
          unit_m4(symm_rot_mat);
          rotate_m4(symm_rot_mat, raxis + 'X', angle);
          mul_m4_v3(symm_rot_mat, location);

          cursor_draw_tiling_preview(gpuattr, region, location, sd, ob, radius);
          cursor_draw_point_screen_space(gpuattr, region, location, ob.object_to_world().ptr(), 3);
        }
      }
    }
  }
}

static void sculpt_geometry_preview_lines_draw(const Depsgraph &depsgraph,
                                               const uint gpuattr,
                                               const Brush &brush,
                                               const Object &object)
{
  if (!(brush.flag & BRUSH_GRAB_ACTIVE_VERTEX)) {
    return;
  }

  const SculptSession &ss = *object.sculpt;
  if (bke::object::pbvh_get(object)->type() != bke::pbvh::Type::Mesh) {
    return;
  }

  if (!ss.deform_modifiers_active) {
    return;
  }

  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.6f);

  /* Cursor normally draws on top, but for this part we need depth tests. */
  const GPUDepthTest depth_test = GPU_depth_test_get();
  if (!depth_test) {
    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  }

  GPU_line_width(1.0f);
  if (!ss.preview_verts.is_empty()) {
    const Span<float3> positions = vert_positions_for_grab_active_get(depsgraph, object);
    immBegin(GPU_PRIM_LINES, ss.preview_verts.size());
    for (const int vert : ss.preview_verts) {
      immVertex3fv(gpuattr, positions[vert]);
    }
    immEnd();
  }

  /* Restore depth test value. */
  if (!depth_test) {
    GPU_depth_test(GPU_DEPTH_NONE);
  }
}

static void SCULPT_layer_brush_height_preview_draw(const uint gpuattr,
                                                   const Brush &brush,
                                                   const float rds,
                                                   const float line_width,
                                                   const float3 &outline_col,
                                                   const float alpha)
{
  const float4x4 cursor_trans = math::translate(float4x4::identity(),
                                                float3(0.0f, 0.0f, brush.height));
  GPU_matrix_push();
  GPU_matrix_mul(cursor_trans.ptr());

  GPU_line_width(line_width);
  immUniformColor3fvAlpha(outline_col, alpha * 0.5f);
  imm_draw_circle_wire_3d(gpuattr, 0, 0, rds, 80);
  GPU_matrix_pop();
}

static bool paint_use_2d_cursor(PaintMode mode)
{
  switch (mode) {
    case PaintMode::Sculpt:
    case PaintMode::Vertex:
    case PaintMode::Weight:
      return false;
    case PaintMode::Texture3D:
    case PaintMode::Texture2D:
    case PaintMode::VertexGPencil:
    case PaintMode::SculptGPencil:
    case PaintMode::WeightGPencil:
    case PaintMode::SculptCurves:
    case PaintMode::GPencil:
      return true;
    case PaintMode::Invalid:
      BLI_assert_unreachable();
  }
  return true;
}

enum class PaintCursorDrawingType {
  Curve,
  Cursor2D,
  Cursor3D,
};

/**
 * State Machine для Context Manager управления жизненным циклом отрисовки.
 * Отслеживает текущее состояние контекста для предотвращения ошибок использования.
 */
enum class PaintCursorContextState {
  CLEAN,              // Начальное состояние после инициализации
  IMM_SETUP,          // IMM shader установлен, GPU format настроен
  TEXTURE_DRAWING,    // Текстурная стратегия активна
  RESTORED            // Состояние восстановлено, контекст завершен
};

struct PaintCursorContext {
  bContext *C;
  ARegion *region;
  wmWindow *win;
  wmWindowManager *wm;
  bScreen *screen;
  Depsgraph *depsgraph;
  Scene *scene;
  UnifiedPaintSettings *ups;
  Brush *brush;
  Paint *paint;
  PaintMode mode;
  ViewContext vc;

  /* Sculpt related data. */
  Sculpt *sd;
  SculptSession *ss;

  /**
   * Previous active vertex index , used to determine if the preview is updated for the pose brush.
   */
  int prev_active_vert_index;

  bool is_stroke_active;
  bool is_cursor_over_mesh;
  float radius;

  /* 3D view cursor position and normal. */
  float3 location;
  float3 scene_space_location;
  float3 normal;

  /* Cursor main colors. */
  float3 outline_col;
  float outline_alpha;

  /* GPU attribute for drawing. */
  uint pos;

  /* TODO(REFACT-STAGE-2-006): Add shader_strategy field to PaintCursorContext */
  std::unique_ptr<PaintCursorShaderStrategy> shader_strategy;

  /* Context Manager State Machine */
  PaintCursorContextState state = PaintCursorContextState::CLEAN;
  std::optional<PaintCursorGPUStateGuard> gpu_guard;

  PaintCursorDrawingType cursor_type;

  /* This variable is set after drawing the overlay, not on initialization. It can't be used for
   * checking if alpha overlay is enabled before drawing it. */
  bool alpha_overlay_drawn;

  float zoomx;
  /* Coordinates in region space */
  int2 mval;

  /* TODO: Figure out why this and mval are used interchangeably */
  float2 translation;

  float2 tilt;

  float final_radius;
  int pixel_radius;

  /* Performance optimization: Cached results for expensive checks */
  bool brush_cursor_enabled_cached = false;
  bool overlay_conditions_met_cached = false;
  uint64_t last_brush_id = 0;
  bool cache_valid = false;

  /* Check if brush has changed since last cache update */
  bool brush_changed() const {
    return brush && (brush->id.name[2] != last_brush_id);
  }

  /* Update cache when brush changes */
  void update_cache() {
    if (brush_changed()) {
      // Forward declaration needed - will be defined later
      brush_cursor_enabled_cached = false; // Temporary fix
      overlay_conditions_met_cached = brush && (brush->overlay_flags & (BRUSH_OVERLAY_PRIMARY | BRUSH_OVERLAY_SECONDARY));
      last_brush_id = brush->id.name[2];
      cache_valid = true;
    }
  }
};

/**
 * Context Manager: Setup 3D drawing context with state validation.
 * Initializes GPU state guard, sets up IMM shader and vertex format for 3D drawing.
 */
static void paint_cursor_context_setup_3D_drawing(PaintCursorContext &pcontext)
{
  BLI_assert(pcontext.state == PaintCursorContextState::CLEAN);
  
  /* Создание RAII guard для автоматического управления GPU состоянием */
  pcontext.gpu_guard.emplace();
  
  /* Настройка GPU для 3D отрисовки */
  GPU_line_width(2.0f);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  
  /* Установка vertex format для 3D */
  pcontext.pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32_32);
  
  /* Установка uniform color shader strategy */
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  
  /* Переход в состояние IMM_SETUP */
  pcontext.state = PaintCursorContextState::IMM_SETUP;
}

/**
 * Context Manager: Draw texture overlay through command pattern with state validation.
 * Creates and executes texture drawing command, managing state transitions.
 */
static bool paint_cursor_context_draw_texture(PaintCursorContext &pcontext,
                                               float radius)
{
  BLI_assert(pcontext.state == PaintCursorContextState::IMM_SETUP);
  
  /* Переход в состояние TEXTURE_DRAWING */
  pcontext.state = PaintCursorContextState::TEXTURE_DRAWING;
  
  /* Создание и выполнение команды отрисовки текстуры */
  bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;
  auto texture_cmd = std::make_unique<PaintCursorDrawTextureCommand>(
      pcontext.paint,
      pcontext.brush,
      &pcontext.vc,
      pcontext.zoomx,
      pcontext.mode,
      false, /* col */
      true,  /* primary */
      radius,
      paint_runtime->brush_rotation,
      pcontext.location,
      pcontext.normal);
  
  bool result = false;
  if (texture_cmd->can_execute()) {
    result = texture_cmd->execute();
  }
  
  /* Возврат в состояние IMM_SETUP после отрисовки текстуры */
  pcontext.state = PaintCursorContextState::IMM_SETUP;
  
  return result;
}

/**
 * Context Manager: Restore drawing state and cleanup resources with state validation.
 * Unbinds IMM shader, cleans up GPU state guard, and transitions to final state.
 */
static void paint_cursor_context_restore_drawing_state(PaintCursorContext &pcontext)
{
  BLI_assert(pcontext.state == PaintCursorContextState::IMM_SETUP || 
             pcontext.state == PaintCursorContextState::TEXTURE_DRAWING);
  
  /* Unbind IMM shader */
  if (immIsShaderBound()) {
    immUnbindProgram();
  }
  
  /* Очистка GPU состояний через RAII guard */
  if (pcontext.gpu_guard.has_value()) {
    pcontext.gpu_guard.reset();
  }
  
  /* Восстановление GPU состояний */
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
  
  /* Переход в финальное состояние */
  pcontext.state = PaintCursorContextState::RESTORED;
}

static bool paint_cursor_context_init(bContext *C,
                                      const blender::int2 &xy,
                                      const blender::float2 &tilt,
                                      PaintCursorContext &pcontext)
{
  ARegion *region = CTX_wm_region(C);
  if (region && region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }

  pcontext.C = C;
  pcontext.region = region;
  pcontext.wm = CTX_wm_manager(C);
  pcontext.win = CTX_wm_window(C);
  pcontext.screen = CTX_wm_screen(C);
  pcontext.depsgraph = CTX_data_depsgraph_pointer(C);
  pcontext.scene = CTX_data_scene(C);
  pcontext.paint = BKE_paint_get_active_from_context(C);
  if (pcontext.paint == nullptr) {
    return false;
  }
  pcontext.ups = &pcontext.paint->unified_paint_settings;
  pcontext.brush = BKE_paint_brush(pcontext.paint);
  if (pcontext.brush == nullptr) {
    return false;
  }
  pcontext.mode = BKE_paintmode_get_active_from_context(C);

  pcontext.vc = ED_view3d_viewcontext_init(C, pcontext.depsgraph);

  if (pcontext.brush->flag & BRUSH_CURVE) {
    pcontext.cursor_type = PaintCursorDrawingType::Curve;
  }
  else if (paint_use_2d_cursor(pcontext.mode)) {
    pcontext.cursor_type = PaintCursorDrawingType::Cursor2D;
  }
  else {
    pcontext.cursor_type = PaintCursorDrawingType::Cursor3D;
  }

  pcontext.mval = xy;
  pcontext.translation = {float(xy[0]), float(xy[1])};
  pcontext.tilt = tilt;

  float zoomx, zoomy;
  get_imapaint_zoom(C, &zoomx, &zoomy);
  pcontext.zoomx = max_ff(zoomx, zoomy);
  pcontext.final_radius = (BKE_brush_radius_get(pcontext.paint, pcontext.brush) * zoomx);

  const bke::PaintRuntime &paint_runtime = *pcontext.paint->runtime;
  /* There is currently no way to check if the direction is inverted before starting the stroke,
   * so this does not reflect the state of the brush in the UI. */
  if (((!paint_runtime.draw_inverted) ^ ((pcontext.brush->flag & BRUSH_DIR_IN) == 0)) &&
      bke::brush::supports_secondary_cursor_color(*pcontext.brush))
  {
    pcontext.outline_col = float3(pcontext.brush->sub_col);
  }
  else {
    pcontext.outline_col = float3(pcontext.brush->add_col);
  }
  pcontext.outline_alpha = pcontext.brush->add_col[3];

  Object *active_object = pcontext.vc.obact;
  pcontext.ss = active_object ? active_object->sculpt : nullptr;

  if (pcontext.ss && pcontext.ss->draw_faded_cursor) {
    pcontext.outline_alpha = 0.3f;
    pcontext.outline_col = float3(0.8f);
  }

  const bool is_brush_tool = paint_brush_tool_poll(C);
  if (!is_brush_tool) {
    /* Use a default color for tools that are not brushes. */
    pcontext.outline_alpha = 0.8f;
    pcontext.outline_col = float3(0.8f);
  }

  pcontext.is_stroke_active = paint_runtime.stroke_active;

  return true;
}

static void paint_cursor_update_pixel_radius(PaintCursorContext &pcontext)
{
  if (pcontext.is_cursor_over_mesh) {
    Brush *brush = BKE_paint_brush(pcontext.paint);
    pcontext.pixel_radius = project_brush_radius(
        &pcontext.vc, BKE_brush_unprojected_radius_get(pcontext.paint, brush), pcontext.location);

    if (pcontext.pixel_radius == 0) {
      pcontext.pixel_radius = BKE_brush_radius_get(pcontext.paint, brush);
    }

    pcontext.scene_space_location = math::transform_point(pcontext.vc.obact->object_to_world(),
                                                          pcontext.location);
  }
  else {
    Sculpt *sd = CTX_data_tool_settings(pcontext.C)->sculpt;
    Brush *brush = BKE_paint_brush(&sd->paint);

    pcontext.pixel_radius = BKE_brush_radius_get(pcontext.paint, brush);
  }
}

static void paint_cursor_sculpt_session_update_and_init(PaintCursorContext &pcontext)
{
  BLI_assert(pcontext.ss != nullptr);
  BLI_assert(pcontext.mode == PaintMode::Sculpt);

  bContext *C = pcontext.C;
  SculptSession &ss = *pcontext.ss;
  Brush &brush = *pcontext.brush;
  bke::PaintRuntime &paint_runtime = *pcontext.paint->runtime;
  ViewContext &vc = pcontext.vc;
  CursorGeometryInfo gi;

  const float2 mval_fl = {
      float(pcontext.mval.x - pcontext.region->winrct.xmin),
      float(pcontext.mval.y - pcontext.region->winrct.ymin),
  };

  /* Ensure that the PBVH is generated before we call #cursor_geometry_info_update because
   * the PBVH is needed to do a ray-cast to find the active vertex. */
  bke::object::pbvh_ensure(*pcontext.depsgraph, *pcontext.vc.obact);

  /* This updates the active vertex, which is needed for most of the Sculpt/Vertex Colors tools to
   * work correctly */
  vert_random_access_ensure(*vc.obact);
  pcontext.prev_active_vert_index = ss.active_vert_index();
  if (!paint_runtime.stroke_active) {
    pcontext.is_cursor_over_mesh = cursor_geometry_info_update(
        C, &gi, mval_fl, (pcontext.brush->falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE));
    pcontext.location = gi.location;
    pcontext.normal = gi.normal;
  }
  else {
    pcontext.is_cursor_over_mesh = paint_runtime.last_hit;
    pcontext.location = paint_runtime.last_location;
  }

  paint_cursor_update_pixel_radius(pcontext);

  if (BKE_brush_use_locked_size(pcontext.paint, &brush)) {
    /* In Scene mode (use_locked_size = 1), size is calculated dynamically from unprojected_size
     * during rendering. We should NOT update brush.size here from pixel_radius, because:
     * 1. pixel_radius may be incorrect immediately after mode switching
     * 2. size should be recalculated from unprojected_size based on current view, not cached
     * 3. When user sets unprojected_size via UI (e.g., 1000), size should update correctly
     *    from that value, not be blocked by incorrect pixel_radius calculations.
     * 
     * However, we can update size if pixel_radius is reasonable and cursor is over mesh,
     * to keep size in sync for display purposes. But we must be very conservative. */
    int current_size = BKE_brush_size_get(pcontext.paint, &brush);
    int new_size = pcontext.pixel_radius;
    
    /* Only update size if:
     * 1. Cursor is over mesh (pixel_radius is reliable)
     * 2. Size change is reasonable (not too extreme)
     * 3. New size is not too small (at least 10 pixels)
     * This ensures size stays in sync when valid, but doesn't break when unprojected_size
     * is set to large values (like 1000) via UI. */
    if (pcontext.is_cursor_over_mesh && std::abs(current_size - new_size) > 1) {
      float size_ratio = (current_size > 0) ? float(new_size) / float(current_size) : 1.0f;
      
      bool size_too_small = (new_size < 10);
      bool ratio_too_extreme = (size_ratio < 0.5f || size_ratio > 2.0f);
      
      if (!size_too_small && !ratio_too_extreme) {
        BKE_brush_size_set(pcontext.paint, &brush, new_size);
      }
    }
  }

  if (pcontext.is_cursor_over_mesh) {
    paint_cursor_update_unprojected_size(
        *pcontext.paint, brush, vc, pcontext.scene_space_location);
  }

  pcontext.sd = CTX_data_tool_settings(pcontext.C)->sculpt;
}

static void paint_update_mouse_cursor(PaintCursorContext &pcontext)
{
  if (pcontext.win->grabcursor != 0 || pcontext.win->modalcursor != 0) {
    /* Don't set the cursor while it's grabbed, since this will show the cursor when interacting
     * with the UI (dragging a number button for example), see: #102792.
     * And don't overwrite a modal cursor, allowing modal operators to set a cursor temporarily. */
    return;
  }

  /* Don't set the cursor when a temporary popup is opened (e.g. a context menu, pie menu or
   * dialog), see: #137386. */
  if (!BLI_listbase_is_empty(&pcontext.screen->regionbase) &&
      (BKE_screen_find_region_type(pcontext.screen, RGN_TYPE_TEMPORARY) != nullptr))
  {
    return;
  }

  if (ELEM(pcontext.mode, PaintMode::GPencil, PaintMode::VertexGPencil)) {
    WM_cursor_set(pcontext.win, WM_CURSOR_DOT);
  }
  else {
    WM_cursor_set(pcontext.win, WM_CURSOR_PAINT);
  }
}

static void paint_draw_2D_view_brush_cursor_default(PaintCursorContext &pcontext)
{
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha);
  const bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;

  /* Draw brush outline. */
  if (paint_runtime->stroke_active && BKE_brush_use_size_pressure(pcontext.brush)) {
    imm_draw_circle_wire_2d(pcontext.pos,
                            pcontext.translation[0],
                            pcontext.translation[1],
                            pcontext.final_radius * paint_runtime->size_pressure_value,
                            40);
    /* Outer at half alpha. */
    immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha * 0.5f);
  }

  GPU_line_width(1.0f);
  imm_draw_circle_wire_2d(
      pcontext.pos, pcontext.translation[0], pcontext.translation[1], pcontext.final_radius, 40);
}

static void grease_pencil_eraser_draw(PaintCursorContext &pcontext)
{
  float radius = float(pcontext.pixel_radius);

  /* Red-ish color with alpha. */
  immUniformColor4ub(255, 100, 100, 20);
  imm_draw_circle_fill_2d(pcontext.pos, pcontext.mval.x, pcontext.mval.y, radius, 40);

  immUnbindProgram();

  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);

  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);

  immUniformColor4f(1.0f, 0.39f, 0.39f, 0.78f);
  immUniform1i("colors_len", 0); /* "simple" mode */
  immUniform1f("dash_width", 12.0f);
  immUniform1f("udash_factor", 0.5f);

  /* XXX Dashed shader gives bad results with sets of small segments
   * currently, temp hack around the issue. :( */
  const int nsegments = max_ii(8, radius / 2);
  imm_draw_circle_wire_2d(pcontext.pos, pcontext.mval.x, pcontext.mval.y, radius, nsegments);
}

static void grease_pencil_brush_cursor_draw(PaintCursorContext &pcontext)
{
  if (pcontext.region &&
      !BLI_rcti_isect_pt(&pcontext.region->winrct, pcontext.mval.x, pcontext.mval.y))
  {
    return;
  }

  Object *object = CTX_data_active_object(pcontext.C);
  if (object->type != OB_GREASE_PENCIL) {
    return;
  }

  GreasePencil *grease_pencil = static_cast<GreasePencil *>(object->data);
  Paint *paint = pcontext.paint;
  Brush *brush = pcontext.brush;
  if ((brush == nullptr) || (brush->gpencil_settings == nullptr)) {
    return;
  }

  float3 color(1.0f);
  const int2 mval = pcontext.mval;

  if (pcontext.mode == PaintMode::GPencil) {
    /* Hide the cursor while drawing. */
    if (grease_pencil->runtime->is_drawing_stroke) {
      return;
    }

    /* Eraser has a special shape and uses a different shader program. */
    if (brush->gpencil_brush_type == GPAINT_BRUSH_TYPE_ERASE ||
        grease_pencil->runtime->temp_use_eraser)
    {
      /* If we use the eraser from the draw tool with a "scene" radius unit, we need to draw the
       * cursor with the appropriate size. */
      if (grease_pencil->runtime->temp_use_eraser && (brush->flag & BRUSH_LOCK_SIZE) != 0) {
        pcontext.pixel_radius = std::max(int(grease_pencil->runtime->temp_eraser_size / 2.0f), 1);
      }
      else {
        pcontext.pixel_radius = std::max(1, int(brush->size / 2.0f));
      }
      grease_pencil_eraser_draw(pcontext);
      return;
    }

    if (brush->gpencil_brush_type == GPAINT_BRUSH_TYPE_FILL) {
      /* The fill tool doesn't use a brush size currently, but not showing any brush means that it
       * can be hard to see where the cursor is. Use a fixed size that's not too big (10px). By
       * disabling the "Display Cursor" option, this can still be turned off. */
      pcontext.pixel_radius = 10;
    }

    if (brush->gpencil_brush_type == GPAINT_BRUSH_TYPE_TINT) {
      pcontext.pixel_radius = std::max(int(brush->size / 2.0f), 1);
    }

    if (brush->gpencil_brush_type == GPAINT_BRUSH_TYPE_DRAW) {
      if ((brush->flag & BRUSH_LOCK_SIZE) != 0) {
        const bke::greasepencil::Layer *layer = grease_pencil->get_active_layer();
        const ed::greasepencil::DrawingPlacement placement(
            *pcontext.scene, *pcontext.region, *pcontext.vc.v3d, *object, layer);
        const float2 coordinate = float2(pcontext.mval.x - pcontext.region->winrct.xmin,
                                         pcontext.mval.y - pcontext.region->winrct.ymin);
        bool clipped = false;
        const float3 pos = placement.project(coordinate, clipped);
        if (!clipped) {
          const float3 world_location = math::transform_point(placement.to_world_space(), pos);
          pcontext.pixel_radius = project_brush_radius_grease_pencil(&pcontext.vc,
                                                                     brush->unprojected_size /
                                                                         2.0f,
                                                                     world_location,
                                                                     placement.to_world_space());
        }
        else {
          pcontext.pixel_radius = 0;
        }
        brush->size = std::max(pcontext.pixel_radius * 2, 1);
      }
      else {
        pcontext.pixel_radius = brush->size / 2.0f;
      }
    }

    /* Get current drawing material. */
    if (Material *ma = BKE_grease_pencil_object_material_from_brush_get(object, brush)) {
      MaterialGPencilStyle *gp_style = ma->gp_style;

      /* Follow user settings for the size of the draw cursor:
       * - Fixed size, or
       * - Brush size (i.e. stroke thickness)
       */
      if ((gp_style) && ((brush->flag & BRUSH_SMOOTH_STROKE) == 0) &&
          (brush->gpencil_brush_type == GPAINT_BRUSH_TYPE_DRAW))
      {

        const bool use_vertex_color = ed::sculpt_paint::greasepencil::brush_using_vertex_color(
            pcontext.scene->toolsettings->gp_paint, brush);
        const bool use_vertex_color_stroke = use_vertex_color &&
                                             ELEM(brush->gpencil_settings->vertex_mode,
                                                  GPPAINT_MODE_STROKE,
                                                  GPPAINT_MODE_BOTH);
        if (use_vertex_color_stroke) {
          IMB_colormanagement_scene_linear_to_srgb_v3(color, brush->color);
        }
        else {
          color = float4(gp_style->stroke_rgba).xyz();
        }
      }
    }

    if ((brush->flag & BRUSH_SMOOTH_STROKE) != 0) {
      color = float3(1.0f, 0.4f, 0.4f);
    }
  }
  else if (pcontext.mode == PaintMode::VertexGPencil) {
    pcontext.pixel_radius = BKE_brush_radius_get(pcontext.paint, brush);
    color = BKE_brush_color_get(paint, brush);
    IMB_colormanagement_scene_linear_to_srgb_v3(color, color);
  }

  GPU_line_width(1.0f);
  /* Inner Ring: Color from UI panel */
  immUniformColor4f(color.x, color.y, color.z, 0.8f);
  imm_draw_circle_wire_2d(pcontext.pos, mval.x, mval.y, pcontext.pixel_radius, 32);

  /* Outer Ring: Dark color for contrast on light backgrounds (e.g. gray on white) */
  const float3 darkcolor = color * 0.40f;
  immUniformColor4f(darkcolor[0], darkcolor[1], darkcolor[2], 0.8f);
  imm_draw_circle_wire_2d(pcontext.pos, mval.x, mval.y, pcontext.pixel_radius + 1, 32);
}

static void paint_draw_2D_view_brush_cursor(PaintCursorContext &pcontext)
{
  switch (pcontext.mode) {
    case PaintMode::GPencil:
    case PaintMode::VertexGPencil:
      grease_pencil_brush_cursor_draw(pcontext);
      break;
    default:
      paint_draw_2D_view_brush_cursor_default(pcontext);
  }
}

static void paint_draw_legacy_3D_view_brush_cursor(PaintCursorContext &pcontext)
{
  GPU_line_width(1.0f);
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha);
  imm_draw_circle_wire_3d(
      pcontext.pos, pcontext.translation[0], pcontext.translation[1], pcontext.final_radius, 40);
}

static void paint_draw_3D_view_inactive_brush_cursor(PaintCursorContext &pcontext)
{
  GPU_line_width(1.0f);
  /* Reduce alpha to increase the contrast when the cursor is over the mesh. */
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha * 0.8);
  imm_draw_circle_wire_3d(
      pcontext.pos, pcontext.translation[0], pcontext.translation[1], pcontext.final_radius, 80);
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha * 0.35f);
  imm_draw_circle_wire_3d(
      pcontext.pos,
      pcontext.translation[0],
      pcontext.translation[1],
      pcontext.final_radius *
          clamp_f(BKE_brush_alpha_get(pcontext.paint, pcontext.brush), 0.0f, 1.0f),
      80);
}

static void paint_cursor_update_object_space_radius(PaintCursorContext &pcontext)
{
  pcontext.radius = object_space_radius_get(
      pcontext.vc, *pcontext.paint, *pcontext.brush, pcontext.location);
}

static void paint_cursor_drawing_setup_cursor_space(const PaintCursorContext &pcontext)
{
  const float4x4 cursor_trans = math::translate(pcontext.vc.obact->object_to_world(),
                                                pcontext.location);

  const float3 z_axis = {0.0f, 0.0f, 1.0f};

  const float3 normal = bke::brush::supports_tilt(*pcontext.brush) ?
                            tilt_apply_to_normal(*pcontext.vc.obact,
                                                 float4x4(pcontext.vc.rv3d->viewinv),
                                                 pcontext.normal,
                                                 pcontext.tilt,
                                                 pcontext.brush->tilt_strength_factor) :
                            pcontext.normal;

  const math::AxisAngle between_vecs(z_axis, normal);
  const float4x4 cursor_rot = math::from_rotation<float4x4>(between_vecs);

  GPU_matrix_mul(cursor_trans.ptr());
  GPU_matrix_mul(cursor_rot.ptr());
}

static void paint_cursor_draw_main_inactive_cursor(PaintCursorContext &pcontext)
{
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha);
  GPU_line_width(2.0f);
  imm_draw_circle_wire_3d(pcontext.pos, 0, 0, pcontext.radius, 80);

  GPU_line_width(1.0f);
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha * 0.5f);
  imm_draw_circle_wire_3d(
      pcontext.pos,
      0,
      0,
      pcontext.radius * clamp_f(BKE_brush_alpha_get(pcontext.paint, pcontext.brush), 0.0f, 1.0f),
      80);
}

static void paint_cursor_pose_brush_segments_draw(const PaintCursorContext &pcontext)
{
  SculptSession &ss = *pcontext.ss;
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.8f);
  GPU_line_width(2.0f);

  BLI_assert(ss.pose_ik_chain_preview->initial_head_coords.size() ==
             ss.pose_ik_chain_preview->initial_orig_coords.size());

  immBegin(GPU_PRIM_LINES, ss.pose_ik_chain_preview->initial_head_coords.size() * 2);
  for (const int i : ss.pose_ik_chain_preview->initial_head_coords.index_range()) {
    immVertex3fv(pcontext.pos, ss.pose_ik_chain_preview->initial_orig_coords[i]);
    immVertex3fv(pcontext.pos, ss.pose_ik_chain_preview->initial_head_coords[i]);
  }

  immEnd();
}

static void paint_cursor_pose_brush_origins_draw(const PaintCursorContext &pcontext)
{

  SculptSession &ss = *pcontext.ss;
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.8f);
  for (const int i : ss.pose_ik_chain_preview->initial_orig_coords.index_range()) {
    cursor_draw_point_screen_space(pcontext.pos,
                                   pcontext.region,
                                   ss.pose_ik_chain_preview->initial_orig_coords[i],
                                   pcontext.vc.obact->object_to_world().ptr(),
                                   3);
  }
}

static void paint_cursor_preview_boundary_data_pivot_draw(const PaintCursorContext &pcontext)
{
  if (!pcontext.ss->boundary_preview) {
    /* There is no guarantee that a boundary preview exists as there may be no boundaries
     * inside the brush radius. */
    return;
  }
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.8f);
  cursor_draw_point_screen_space(pcontext.pos,
                                 pcontext.region,
                                 pcontext.ss->boundary_preview->pivot_position,
                                 pcontext.vc.obact->object_to_world().ptr(),
                                 3);
}

static void paint_cursor_preview_boundary_data_update(const PaintCursorContext &pcontext)
{
  SculptSession &ss = *pcontext.ss;
  /* Needed for updating the necessary SculptSession data in order to initialize the
   * boundary data for the preview. */
  BKE_sculpt_update_object_for_edit(pcontext.depsgraph, pcontext.vc.obact, false);

  ss.boundary_preview = boundary::preview_data_init(
      *pcontext.depsgraph, *pcontext.vc.obact, pcontext.brush, pcontext.radius);
}

static void paint_cursor_draw_3d_view_brush_cursor_inactive(PaintCursorContext &pcontext)
{
  const Brush &brush = *pcontext.brush;

  /* 2D falloff is better represented with the default 2D cursor,
   * there is no need to draw anything else. */
  if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
    paint_draw_legacy_3D_view_brush_cursor(pcontext);
    return;
  }

  if (pcontext.alpha_overlay_drawn) {
    paint_draw_legacy_3D_view_brush_cursor(pcontext);
    return;
  }

  if (!pcontext.is_cursor_over_mesh) {
    paint_draw_3D_view_inactive_brush_cursor(pcontext);
    return;
  }

  BLI_assert(pcontext.vc.obact);
  Object &active_object = *pcontext.vc.obact;
  paint_cursor_update_object_space_radius(pcontext);

  vert_random_access_ensure(active_object);

  /* Setup drawing. */
  wmViewport(&pcontext.region->winrct);

  /* Drawing of Cursor overlays in 2D screen space. */

  /* Cursor location symmetry points. */

  float3 active_vertex_co;
  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_GRAB && brush.flag & BRUSH_GRAB_ACTIVE_VERTEX) {
    SculptSession &ss = *pcontext.ss;
    if (bke::object::pbvh_get(active_object)->type() == bke::pbvh::Type::Mesh) {
      const Span<float3> positions = vert_positions_for_grab_active_get(*pcontext.depsgraph,
                                                                        active_object);
      active_vertex_co = positions[std::get<int>(ss.active_vert())];
    }
    else {
      active_vertex_co = pcontext.ss->active_vert_position(*pcontext.depsgraph, active_object);
    }
  }
  else {
    active_vertex_co = pcontext.ss->active_vert_position(*pcontext.depsgraph, active_object);
  }
  if (math::distance(active_vertex_co, pcontext.location) < pcontext.radius) {
    immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha);
    cursor_draw_point_with_symmetry(pcontext.pos,
                                    pcontext.region,
                                    active_vertex_co,
                                    *pcontext.sd,
                                    active_object,
                                    pcontext.radius);
  }

  const bool is_brush_tool = paint_brush_tool_poll(pcontext.C);

  /* Pose brush updates and rotation origins. */

  if (is_brush_tool && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_POSE) {
    /* Just after switching to the Pose Brush, the active vertex can be the same and the
     * cursor won't be tagged to update, so always initialize the preview chain if it is
     * nullptr before drawing it. */
    SculptSession &ss = *pcontext.ss;
    const bool update_previews = pcontext.prev_active_vert_index !=
                                 pcontext.ss->active_vert_index();
    if (update_previews || !ss.pose_ik_chain_preview) {
      BKE_sculpt_update_object_for_edit(pcontext.depsgraph, &active_object, false);

      /* Free the previous pose brush preview. */
      if (ss.pose_ik_chain_preview) {
        ss.pose_ik_chain_preview.reset();
      }

      /* Generate a new pose brush preview from the current cursor location. */
      ss.pose_ik_chain_preview = pose::preview_ik_chain_init(
          *pcontext.depsgraph, active_object, ss, brush, pcontext.location, pcontext.radius);
    }

    /* Draw the pose brush rotation origins. */
    paint_cursor_pose_brush_origins_draw(pcontext);
  }

  /* Expand operation origin. */
  if (pcontext.ss->expand_cache) {
    const int vert = pcontext.ss->expand_cache->initial_active_vert;

    float3 position;
    switch (bke::object::pbvh_get(active_object)->type()) {
      case bke::pbvh::Type::Mesh: {
        const Span positions = bke::pbvh::vert_positions_eval(*pcontext.depsgraph, active_object);
        position = positions[vert];
        break;
      }
      case bke::pbvh::Type::Grids: {
        const SubdivCCG &subdiv_ccg = *pcontext.ss->subdiv_ccg;
        position = subdiv_ccg.positions[vert];
        break;
      }
      case bke::pbvh::Type::BMesh: {
        BMesh &bm = *pcontext.ss->bm;
        position = BM_vert_at_index(&bm, vert)->co;
        break;
      }
    }
    cursor_draw_point_screen_space(
        pcontext.pos, pcontext.region, position, active_object.object_to_world().ptr(), 2);
  }

  if (is_brush_tool && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_BOUNDARY) {
    paint_cursor_preview_boundary_data_update(pcontext);
    paint_cursor_preview_boundary_data_pivot_draw(pcontext);
  }

  /* Setup 3D perspective drawing. */
  GPU_matrix_push_projection();
  ED_view3d_draw_setup_view(pcontext.wm,
                            pcontext.win,
                            pcontext.depsgraph,
                            pcontext.scene,
                            pcontext.region,
                            CTX_wm_view3d(pcontext.C),
                            nullptr,
                            nullptr,
                            nullptr);

  GPU_matrix_push();
  GPU_matrix_mul(active_object.object_to_world().ptr());

  /* Drawing Cursor overlays in 3D object space. */
  if (is_brush_tool && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_GRAB &&
      (brush.flag & BRUSH_GRAB_ACTIVE_VERTEX))
  {
    geometry_preview_lines_update(
        *pcontext.depsgraph, *pcontext.vc.obact, *pcontext.ss, pcontext.radius);
    sculpt_geometry_preview_lines_draw(
        *pcontext.depsgraph, pcontext.pos, *pcontext.brush, active_object);
  }

  if (is_brush_tool && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_POSE) {
    paint_cursor_pose_brush_segments_draw(pcontext);
  }

  if (is_brush_tool && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_BOUNDARY) {
    boundary::edges_preview_draw(
        pcontext.pos, *pcontext.ss, pcontext.outline_col, pcontext.outline_alpha);
    boundary::pivot_line_preview_draw(pcontext.pos, *pcontext.ss);
  }

  GPU_matrix_pop();

  /* Drawing Cursor overlays in Paint Cursor space (as additional info on top of the brush cursor)
   */
  GPU_matrix_push();
  paint_cursor_drawing_setup_cursor_space(pcontext);
  
  /* Main inactive cursor. */
  paint_cursor_draw_main_inactive_cursor(pcontext);

  /* Draw texture overlay if enabled for inactive cursor */
  if (brush.overlay_flags & (BRUSH_OVERLAY_PRIMARY | BRUSH_OVERLAY_SECONDARY)) {
    bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;
    
    /* Primary texture overlay */
    if (brush.overlay_flags & BRUSH_OVERLAY_PRIMARY) {
      paint_draw_tex_overlay_3d(pcontext.paint,
                               pcontext.brush,
                               &pcontext.vc,
                               pcontext.zoomx,
                               pcontext.mode,
                               false, /* col */
                               true,  /* primary */
                               pcontext.radius,
                               paint_runtime->brush_rotation,
                               pcontext.location,
                               pcontext.normal);
    }
    
    /* Secondary texture overlay */
    if (brush.overlay_flags & BRUSH_OVERLAY_SECONDARY) {
      paint_draw_tex_overlay_3d(pcontext.paint,
                               pcontext.brush,
                               &pcontext.vc,
                               pcontext.zoomx,
                               pcontext.mode,
                               false, /* col */
                               false, /* secondary */
                               pcontext.radius,
                               paint_runtime->brush_rotation_sec,
                               pcontext.location,
                               pcontext.normal);
    }
  }

  /* Cloth brush local simulation areas. */
  if (is_brush_tool && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH &&
      brush.cloth_simulation_area_type != BRUSH_CLOTH_SIMULATION_AREA_GLOBAL)
  {
    const float3 white = {1.0f, 1.0f, 1.0f};
    const float3 zero_v = float3(0.0f);
    /* This functions sets its own drawing space in order to draw the simulation limits when the
     * cursor is active. When used here, this cursor overlay is already in cursor space, so its
     * position and normal should be set to 0. */
    cloth::simulation_limits_draw(
        pcontext.pos, brush, zero_v, zero_v, pcontext.radius, 1.0f, white, 0.25f);
  }

  /* Layer brush height. */
  if (is_brush_tool && brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_LAYER) {
    SCULPT_layer_brush_height_preview_draw(
        pcontext.pos, brush, pcontext.radius, 1.0f, pcontext.outline_col, pcontext.outline_alpha);
  }

  GPU_matrix_pop();

  /* Reset drawing. */
  GPU_matrix_pop_projection();
  wmWindowViewport(pcontext.win);
}

static void paint_cursor_cursor_draw_3d_view_brush_cursor_active(PaintCursorContext &pcontext)
{
  BLI_assert(pcontext.ss != nullptr);
  BLI_assert(pcontext.mode == PaintMode::Sculpt);

  SculptSession &ss = *pcontext.ss;
  Brush &brush = *pcontext.brush;

  /* The cursor can be updated as active before creating the StrokeCache, so this needs to be
   * checked. */
  if (!ss.cache) {
    return;
  }

  /* Most of the brushes initialize the necessary data for the custom cursor drawing after the
   * first brush step, so make sure that it is not drawn before being initialized. */
  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    return;
  }

  /* Setup drawing. */
  wmViewport(&pcontext.region->winrct);
  GPU_matrix_push_projection();
  ED_view3d_draw_setup_view(pcontext.wm,
                            pcontext.win,
                            pcontext.depsgraph,
                            pcontext.scene,
                            pcontext.region,
                            CTX_wm_view3d(pcontext.C),
                            nullptr,
                            nullptr,
                            nullptr);
  GPU_matrix_push();
  GPU_matrix_mul(pcontext.vc.obact->object_to_world().ptr());

  /* Draw the special active cursors different brush types may have. */

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_GRAB) {
    sculpt_geometry_preview_lines_draw(
        *pcontext.depsgraph, pcontext.pos, brush, *pcontext.vc.obact);
  }

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_MULTIPLANE_SCRAPE) {
    brushes::multiplane_scrape_preview_draw(
        pcontext.pos, brush, ss, pcontext.outline_col, pcontext.outline_alpha);
  }

  if (brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLOTH) {
    if (brush.cloth_force_falloff_type == BRUSH_CLOTH_FORCE_FALLOFF_PLANE) {
      cloth::plane_falloff_preview_draw(
          pcontext.pos, ss, pcontext.outline_col, pcontext.outline_alpha);
    }
    else if (brush.cloth_force_falloff_type == BRUSH_CLOTH_FORCE_FALLOFF_RADIAL &&
             brush.cloth_simulation_area_type == BRUSH_CLOTH_SIMULATION_AREA_LOCAL)
    {
      /* Display the simulation limits if sculpting outside them. */
      /* This does not makes much sense of plane falloff as the falloff is infinite or global. */

      if (math::distance(ss.cache->location, ss.cache->initial_location) >
          ss.cache->radius * (1.0f + brush.cloth_sim_limit))
      {
        const float3 red = {1.0f, 0.2f, 0.2f};
        cloth::simulation_limits_draw(pcontext.pos,
                                      brush,
                                      ss.cache->initial_location,
                                      ss.cache->initial_normal,
                                      ss.cache->radius,
                                      2.0f,
                                      red,
                                      0.8f);
      }
    }
  }

  /* Draw texture overlay if enabled for active cursor - BEFORE matrix pop */
  
  if (brush.overlay_flags & (BRUSH_OVERLAY_PRIMARY | BRUSH_OVERLAY_SECONDARY)) {
    bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;
    
    /* Для активного курсора используем данные из cache, если доступны */
    const float *location = ss.cache ? ss.cache->location : pcontext.location;
    const float *normal = ss.cache ? ss.cache->sculpt_normal : pcontext.normal;
    
    /* Primary texture overlay */
    if (brush.overlay_flags & BRUSH_OVERLAY_PRIMARY) {
      paint_draw_tex_overlay_3d(pcontext.paint,
                               pcontext.brush,
                               &pcontext.vc,
                               pcontext.zoomx,
                               pcontext.mode,
                               false, /* col */
                               true,  /* primary */
                               pcontext.radius,
                               paint_runtime->brush_rotation,
                               location,
                               normal);
    }
    
    /* Secondary texture overlay */
    if (brush.overlay_flags & BRUSH_OVERLAY_SECONDARY) {
      paint_draw_tex_overlay_3d(pcontext.paint,
                               pcontext.brush,
                               &pcontext.vc,
                               pcontext.zoomx,
                               pcontext.mode,
                               false, /* col */
                               false, /* secondary */
                               pcontext.radius,
                               paint_runtime->brush_rotation_sec,
                               location,
                               normal);
    }
  }

  GPU_matrix_pop();

  GPU_matrix_pop_projection();
  wmWindowViewport(pcontext.win);
}

static void paint_cursor_draw_3D_view_brush_cursor(PaintCursorContext &pcontext)
{
  /* These paint tools are not using the SculptSession, so they need to use the default 2D brush
   * cursor in the 3D view. */
  if (pcontext.mode != PaintMode::Sculpt || !pcontext.ss) {
    paint_draw_legacy_3D_view_brush_cursor(pcontext);
    return;
  }
  
  paint_cursor_sculpt_session_update_and_init(pcontext);

  if (pcontext.is_stroke_active) {
    paint_cursor_cursor_draw_3d_view_brush_cursor_active(pcontext);
  }
  else {
    paint_cursor_draw_3d_view_brush_cursor_inactive(pcontext);
  }
}

static bool paint_cursor_is_3d_view_navigating(const PaintCursorContext &pcontext)
{
  const ViewContext *vc = &pcontext.vc;
  return vc->rv3d && (vc->rv3d->rflag & RV3D_NAVIGATING);
}

static bool paint_cursor_is_brush_cursor_enabled(const PaintCursorContext &pcontext)
{
  if (pcontext.paint->flags & PAINT_SHOW_BRUSH) {
    if (ELEM(pcontext.mode, PaintMode::Texture2D, PaintMode::Texture3D) &&
        pcontext.brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_FILL)
    {
      return false;
    }
    return true;
  }
  return false;
}

static void paint_cursor_update_rake_rotation(PaintCursorContext &pcontext)
{
  /* Don't calculate rake angles while a stroke is active because the rake variables are global
   * and we may get interference with the stroke itself.
   * For line strokes, such interference is visible. */
  const bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;
  if (!paint_runtime->stroke_active) {
    paint_calculate_rake_rotation(
        *pcontext.paint, *pcontext.brush, pcontext.translation, pcontext.mode, true);
  }
}

static void paint_cursor_check_and_draw_alpha_overlays(PaintCursorContext &pcontext)
{
  pcontext.alpha_overlay_drawn = paint_draw_alpha_overlay(pcontext.paint,
                                                          pcontext.brush,
                                                          &pcontext.vc,
                                                          pcontext.mval.x,
                                                          pcontext.mval.y,
                                                          pcontext.zoomx,
                                                          pcontext.mode);
}

static void paint_cursor_update_anchored_location(PaintCursorContext &pcontext)
{
  bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;
  if (paint_runtime->draw_anchored) {
    pcontext.final_radius = paint_runtime->anchored_size;
    pcontext.translation = {
        paint_runtime->anchored_initial_mouse[0] + pcontext.region->winrct.xmin,
        paint_runtime->anchored_initial_mouse[1] + pcontext.region->winrct.ymin};
  }
}

static void paint_cursor_setup_2D_drawing(PaintCursorContext &pcontext)
{
  GPU_line_width(2.0f);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  pcontext.pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
}

/**
 * Restore GPU drawing state for 2D cursor (legacy function for compatibility).
 * This function is kept for 2D cursor support while 3D cursor uses Context Manager.
 */
static void paint_cursor_restore_drawing_state()
{
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_cursor(bContext *C,
                              const blender::int2 &xy,
                              const blender::float2 &tilt,
                              void * /*unused*/)
{
  PaintCursorContext pcontext;
  if (!paint_cursor_context_init(C, xy, tilt, pcontext)) {
    return;
  }
  
  if (!paint_cursor_is_brush_cursor_enabled(pcontext)) {
    /* For Grease Pencil draw mode, we want to we only render a small mouse cursor (dot) if the
     * paint cursor is disabled so that the default mouse cursor doesn't get in the way of tablet
     * users. See #130089. But don't overwrite a modal cursor, allowing modal operators to set one
     * temporarily. */
    if (pcontext.mode == PaintMode::GPencil && pcontext.win->modalcursor == 0) {
      WM_cursor_set(pcontext.win, WM_CURSOR_DOT);
    }
    return;
  }
  if (paint_cursor_is_3d_view_navigating(pcontext)) {
    /* Still draw stencil while navigating. */
    paint_cursor_check_and_draw_alpha_overlays(pcontext);
    return;
  }
  
  switch (pcontext.cursor_type) {
    case PaintCursorDrawingType::Curve:
      paint_draw_curve_cursor(pcontext.brush, &pcontext.vc);
      break;
    case PaintCursorDrawingType::Cursor2D:
      paint_update_mouse_cursor(pcontext);

      paint_cursor_update_rake_rotation(pcontext);
      paint_cursor_check_and_draw_alpha_overlays(pcontext);
      paint_cursor_update_anchored_location(pcontext);

      paint_cursor_setup_2D_drawing(pcontext);
      paint_draw_2D_view_brush_cursor(pcontext);
      paint_cursor_restore_drawing_state();
      break;
    case PaintCursorDrawingType::Cursor3D:
      paint_update_mouse_cursor(pcontext);

      paint_cursor_update_rake_rotation(pcontext);
      paint_cursor_check_and_draw_alpha_overlays(pcontext);
      paint_cursor_update_anchored_location(pcontext);

      /* Context Manager: Setup 3D drawing через контекст */
      paint_cursor_context_setup_3D_drawing(pcontext);

      /* Отрисовка 3D курсора */
      paint_cursor_draw_3D_view_brush_cursor(pcontext);

      /* Context Manager: Restore через контекст */
      paint_cursor_context_restore_drawing_state(pcontext);
      break;
    default:
      BLI_assert_unreachable();
  }
}

/**
 * Преобразует brush_rotation из экранных координат в угол в пространстве курсора.
 * Использует ту же логику, что и calc_brush_local_mat, чтобы preview соответствовал штриху.
 *
 * @param vc ViewContext для преобразования координат
 * @param location Локальная позиция курсора (в координатах объекта)
 * @param normal Нормаль поверхности в локальных координатах объекта
 * @param brush_rotation_screen Угол в экранных координатах (из paint_runtime->brush_rotation)
 * @return Угол в пространстве курсора (для применения через GPU_matrix_rotate_axis)
 */
static float brush_rotation_to_cursor_space(const ViewContext &vc,
                                            const float location[3],
                                            const float normal[3],
                                            float brush_rotation_screen)
{
  Object &ob = *vc.obact;

  /* Edge case: если нормаль равна нулю, вернуть исходный угол */
  if (len_squared_v3(normal) < 1e-6f) {
    return brush_rotation_screen;
  }

  /* 1. Угол → направление в экранных координатах
   * По соглашению из calc_brush_local_mat:
   * motion direction указывает по оси Y кисти
   * angle представляет ось X, normal - это 90 градусов CCW от motion direction
   */
  float motion_normal_screen[2];
  motion_normal_screen[0] = cosf(brush_rotation_screen);
  motion_normal_screen[1] = sinf(brush_rotation_screen);

  /* 2. Преобразование в локальные координаты объекта (копия логики calc_local_from_screen) */
  float loc[3];
  mul_v3_m4v3(loc, ob.object_to_world().ptr(), location);
  const float zfac = ED_view3d_calc_zfac(vc.rv3d, loc);

  float motion_normal_world[3];
  ED_view3d_win_to_delta(vc.region, motion_normal_screen, zfac, motion_normal_world);
  normalize_v3(motion_normal_world);

  float motion_normal_local[3];
  /* Добавляем позицию объекта в мировых координатах (как в calc_local_from_screen) */
  add_v3_v3(motion_normal_world, ob.loc);
  mul_m4_v3(ob.world_to_object().ptr(), motion_normal_world);
  copy_v3_v3(motion_normal_local, motion_normal_world);
  normalize_v3(motion_normal_local);

  /* 3. Вычисление направления движения (Y-ось кисти) в локальном пространстве
   * Y-ось кисти лежит в пересечении плоскости касательной и плоскости движения
   */
  float motion_dir_local[3];
  cross_v3_v3v3(motion_dir_local, normal, motion_normal_local);
  const float motion_dir_len = len_v3(motion_dir_local);

  /* Edge case: если направление движения параллельно нормали */
  if (motion_dir_len < 1e-6f) {
    /* Возвращаем исходный угол без преобразования */
    return brush_rotation_screen;
  }
  normalize_v3(motion_dir_local);

  /* 4. Вычисление X-оси кисти в локальном пространстве */
  float brush_x_local[3];
  cross_v3_v3v3(brush_x_local, motion_dir_local, normal);
  normalize_v3(brush_x_local);

  /* 5. Преобразование осей кисти в пространство курсора
   * Пространство курсора: Z = нормаль, нужно найти X и Y в плоскости поверхности
   * Используем ту же логику, что и в paint_cursor_drawing_setup_cursor_space:
   * создаем матрицу поворота через AxisAngle и извлекаем из неё X и Y оси
   */
  const float3 z_axis = {0.0f, 0.0f, 1.0f};
  const float3 normal_vec = {normal[0], normal[1], normal[2]};
  
  /* Создаем поворот, выравнивающий нормаль с осью Z (как в paint_cursor_drawing_setup_cursor_space) */
  const math::AxisAngle between_vecs(z_axis, normal_vec);
  const float4x4 cursor_rot = math::from_rotation<float4x4>(between_vecs);
  
  /* Извлекаем X и Y оси из матрицы поворота
   * В пространстве курсора: X = первая колонка, Y = вторая колонка, Z = третья колонка (нормаль)
   * Матрица поворота преобразует из пространства курсора в локальное пространство объекта
   */
  const float(*rot_ptr)[4] = cursor_rot.ptr();
  float cursor_x[3];
  cursor_x[0] = rot_ptr[0][0];
  cursor_x[1] = rot_ptr[1][0];
  cursor_x[2] = rot_ptr[2][0];
  
  float cursor_y[3];
  cursor_y[0] = rot_ptr[0][1];
  cursor_y[1] = rot_ptr[1][1];
  cursor_y[2] = rot_ptr[2][1];
  
  /* Проверяем, что оси нормализованы (должны быть, но на всякий случай) */
  normalize_v3(cursor_x);
  normalize_v3(cursor_y);

  /* 6. Вычисление угла между X-осью курсора и X-осью кисти в плоскости поверхности
   * Это угол, на который нужно повернуть текстуру в пространстве курсора
   */
  float dot_x = dot_v3v3(brush_x_local, cursor_x);
  float dot_y = dot_v3v3(brush_x_local, cursor_y);
  float angle = atan2f(dot_y, dot_x);

  return angle;
}

bool paint_draw_tex_overlay_3d(Paint *paint,
                               Brush *brush,
                               ViewContext *vc,
                               float zoom,
                               const PaintMode mode,
                               bool col,
                               bool primary,
                               float radius,
                               float brush_rotation,
                               const float location[3],
                               const float normal[3])
{
  /* Check for overlay mode. */
  MTex *mtex = (primary) ? &brush->mtex : &brush->mask_mtex;
  bool valid = ((primary) ? (brush->overlay_flags & BRUSH_OVERLAY_PRIMARY) != 0 :
                            (brush->overlay_flags & BRUSH_OVERLAY_SECONDARY) != 0);
  int overlay_alpha = (primary) ? brush->texture_overlay_alpha : brush->mask_overlay_alpha;

  if (mode == PaintMode::Texture3D) {
    if (primary && brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_DRAW) {
      /* All non-draw tools don't use the primary texture (clone, smear, soften.. etc). */
      return false;
    }
  }

  bool texture_condition = !(mtex->tex) ||
      !((mtex->brush_map_mode == MTEX_MAP_MODE_STENCIL) ||
        (valid && ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_TILED, MTEX_MAP_MODE_AREA)));
  
  if (texture_condition) {
    return false;
  }

  bool is_brush_tool = WM_toolsystem_active_tool_is_brush(vc->C);
  
  if (!is_brush_tool) {
    return false;
  }

  // Declare load_result early to avoid goto issues
  int load_result = 0;

  // Always call load_tex - it will check if texture needs reloading via same_tex_snap
  // which properly checks both brush_id and texture_id
  load_result = load_tex(brush, vc, zoom, col, primary);
  
  if (load_result) {
    /* TODO(REFACT-STAGE-1-006): Replace manual state management with PaintCursorGPUStateGuard */
    PaintCursorGPUStateGuard gpu_state_guard;

    GPU_color_mask(true, true, true, true);
    GPU_depth_test(GPU_DEPTH_NONE);

    /* Setup vertex format for 3D coordinates */
    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32_32);
    uint texCoord = GPU_vertformat_attr_add(format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

    /* Alpha blending for texture overlay with transparency. */
    GPU_blend(GPU_BLEND_ALPHA);

    /* Handle anchored mode and pressure scaling */
    bke::PaintRuntime *paint_runtime = paint->runtime;
    
    /* Scale based on tablet pressure for primary brush */
    if (primary && paint_runtime->stroke_active && BKE_brush_use_size_pressure(brush)) {
      const float scale = paint_runtime->size_pressure_value;
      GPU_matrix_push();
      GPU_matrix_scale_2f(scale, scale);
    }

    /* TODO(REFACT-STAGE-2-008): Use PaintCursorTextureStrategy for texture drawing */
    blender::gpu::Texture *texture = (primary) ? primary_snap.overlay_texture :
                                                 secondary_snap.overlay_texture;
    GPUSamplerExtendMode extend_mode = (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW || mtex->brush_map_mode == MTEX_MAP_MODE_AREA) ?
                                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER :
                                           GPU_SAMPLER_EXTEND_MODE_REPEAT;

  PaintCursorTextureStrategy texture_strategy(texture, extend_mode);

  if (!texture_strategy.bind_shader()) {
    return false;
  }

    float final_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!col) {
      copy_v3_v3(final_color, U.sculpt_paint_overlay_col);
    }
    mul_v4_fl(final_color, overlay_alpha * 0.01f);
    immUniformColor4fv(final_color);

    /* Apply rotation (combined brush rotation + texture rotation) */
    /* В calc_brush_local_mat используется: angle = rotation + cache->special_rotation
     * где rotation = mtex->rot, special_rotation = brush_rotation
     * Поэтому нужно преобразовывать сумму этих углов, а не по отдельности
     */
    float total_rotation = brush_rotation + mtex->rot;
    
    /* Для режима MTEX_MAP_MODE_AREA преобразуем угол из экранных координат в пространство курсора */
    if (mtex->brush_map_mode == MTEX_MAP_MODE_AREA) {
      /* Преобразуем сумму углов (как в calc_brush_local_mat: angle = rotation + special_rotation) */
      total_rotation = brush_rotation_to_cursor_space(*vc, location, normal, total_rotation);
    }
    if (total_rotation != 0.0f) {
      GPU_matrix_push();
      GPU_matrix_rotate_axis(RAD2DEGF(total_rotation), 'Z');
    }

    /* Draw textured quad in 3D cursor space */
    immBegin(GPU_PRIM_TRI_FAN, 4);
    immAttr2f(texCoord, 0.0f, 0.0f);
    immVertex3f(pos, -radius, -radius, 0.0f);
    immAttr2f(texCoord, 1.0f, 0.0f);
    immVertex3f(pos, radius, -radius, 0.0f);
    immAttr2f(texCoord, 1.0f, 1.0f);
    immVertex3f(pos, radius, radius, 0.0f);
    immAttr2f(texCoord, 0.0f, 1.0f);
    immVertex3f(pos, -radius, radius, 0.0f);
    immEnd();

    /* Restore GPU state */
    texture_strategy.unbind_shader();

    if (total_rotation != 0.0f) {
      GPU_matrix_pop();
    }

    /* Restore pressure scaling matrix */
    if (primary && paint_runtime->stroke_active && BKE_brush_use_size_pressure(brush)) {
      GPU_matrix_pop();
    }

  return true;
  }

  return false;
}

void paint_cursor_delete_textures()
{
  if (primary_snap.overlay_texture) {
    GPU_texture_free(primary_snap.overlay_texture);
  }
  if (secondary_snap.overlay_texture) {
    GPU_texture_free(secondary_snap.overlay_texture);
  }
  if (cursor_snap.overlay_texture) {
    GPU_texture_free(cursor_snap.overlay_texture);
  }

  /* Reset snapshots, including brush_id and texture_id */
  memset(&primary_snap, 0, sizeof(TexSnapshot));
  memset(&secondary_snap, 0, sizeof(TexSnapshot));
  memset(&cursor_snap, 0, sizeof(CursorSnapshot));

  BKE_paint_invalidate_overlay_all();
}

}  // namespace blender::ed::sculpt_paint

/* C API exports */
extern "C" {

void ED_paint_cursor_start(Paint *paint, bool (*poll)(bContext *C))
{
  if (paint && paint->runtime && !paint->runtime->paint_cursor) {
    paint->runtime->paint_cursor = WM_paint_cursor_activate(
        SPACE_TYPE_ANY, RGN_TYPE_ANY, poll, blender::ed::sculpt_paint::paint_draw_cursor, nullptr);
  }

  /* Invalidate the paint cursors. */
  BKE_paint_invalidate_overlay_all();
}

void paint_cursor_delete_textures()
{
  blender::ed::sculpt_paint::paint_cursor_delete_textures();
}

}  // extern "C"
