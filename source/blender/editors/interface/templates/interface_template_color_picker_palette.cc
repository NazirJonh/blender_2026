/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * 
 * Enhanced color palette template - uses Layout Panel for collapsible interface.
 * Provides collapsible palette with size toggle and visual indicators.
 */

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_math_vector.h"

#include "MEM_guardedalloc.h"

#include "BLT_translation.hh"

#include "DNA_brush_types.h"
#include "DNA_screen_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_paint.hh"
#include "BKE_brush.hh"
#include "BKE_report.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_interface_c.hh"
#include "interface_intern.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

namespace blender::ui {

/* Palette state management */
static bool palette_large_buttons = false;

/* Forward declarations */
static void ui_colorpicker_palette_add_cb(bContext *C, void *but1, void *arg);
static void ui_colorpicker_palette_delete_cb(bContext *C, void *but1, void *arg);
static void ui_colorpicker_palette_size_toggle_cb(bContext *C, void *but1, void *arg);

void template_colorpicker_palette(Layout *layout, PointerRNA *ptr, const StringRefNull propname)
{
  Block *block = layout->block();
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  
  if (!prop) {
    RNA_warning("property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  const PointerRNA cptr = RNA_property_pointer_get(ptr, prop);
  
  if (!cptr.data || !RNA_struct_is_a(cptr.type, &RNA_Palette)) {
    return;
  }

  Palette *palette = static_cast<Palette *>(cptr.data);
  
  /* Get context for layout panel */
  bContext *C = static_cast<bContext *>(block->evil_C);
  if (C == nullptr) {
    return;
  }

  PanelLayout panel = layout->panel(C, "color_picker_palette", false);
  printf("[DEBUG] template_colorpicker_palette: Created panel, header=%p, body=%p\n",
         panel.header, panel.body);
  panel.header->label(IFACE_("Palette"), ICON_NONE);
  
  /* Only show content if panel is open */
  if (!panel.body) {
    printf("[DEBUG] template_colorpicker_palette: Panel body is null, returning\n");
    return;
  }
  printf("[DEBUG] template_colorpicker_palette: Panel body is not null, continuing\n");

  /* Color controls row */
  Layout *col = &panel.body->column(true);
  col->row(true);
  
  /* Add/Delete buttons with callbacks for popup update */
  /* Use regular buttons with callbacks instead of operator buttons to ensure
   * callback is called after operator execution */
  Button *add_but = uiDefIconBut(block,
                                  ButtonType::But,
                                  ICON_ADD,
                                  0,
                                  0,
                                  UI_UNIT_X,
                                  UI_UNIT_Y,
                                  nullptr,
                                  0.0,
                                  0.0,
                                  TIP_("Add new color to palette"));
  /* Store palette pointer in button's custom data for callback access */
  add_but->custom_data = palette;
  button_func_set(add_but, ui_colorpicker_palette_add_cb, add_but, block);
                
  Button *del_but = uiDefIconBut(block,
                                 ButtonType::But,
                                 ICON_REMOVE,
                                 0,
                                 0,
                                 UI_UNIT_X,
                                 UI_UNIT_Y,
                                 nullptr,
                                 0.0,
                                 0.0,
                                 TIP_("Delete active color from palette"));
  button_func_set(del_but, ui_colorpicker_palette_delete_cb, del_but, block);
  
  /* Size toggle button */
  const int size_icon = palette_large_buttons ? ICON_FULLSCREEN_EXIT : ICON_FULLSCREEN_ENTER;
  const char *size_tooltip = palette_large_buttons ? "Use smaller color swatches" :
                                                      "Use larger color swatches";
                             
  Button *size_but = uiDefIconBut(block,
                                  ButtonType::But,
                                  size_icon,
                                  0,
                                  0,
                                  UI_UNIT_X,
                                  UI_UNIT_Y,
                                  nullptr,
                                  0.0,
                                  0.0,
                                  size_tooltip);
                                   
  button_func_set(size_but, ui_colorpicker_palette_size_toggle_cb, size_but, block);

  /* Color grid */
  const float button_size = palette_large_buttons ? (UI_UNIT_X * 1.8f) : UI_UNIT_X;
  const int cols_per_row = std::max(int(panel.body->width() / button_size), 1);
  
  col = &panel.body->column(true);
  col->row(true);

  int row_cols = 0;
  int col_id = 0;
  
  /* Get current brush color for visual indicator */
  Paint *paint = BKE_paint_get_active_from_context(C);
  float current_brush_color[3] = {0.0f, 0.0f, 0.0f};
  
  if (paint && paint->brush) {
    const float *brush_color = BKE_brush_color_get(paint, paint->brush);
    if (brush_color) {
      copy_v3_v3(current_brush_color, brush_color);
    }
  }
  
  int color_count = 0;
  LISTBASE_FOREACH (PaletteColor *, color, &palette->colors) {
    if (row_cols >= cols_per_row) {
      col->row(true);
      row_cols = 0;
    }

    PointerRNA color_ptr = RNA_pointer_create_discrete(&palette->id, &RNA_PaletteColor, color);
    ButtonColor *color_but = (ButtonColor *)uiDefButR(block,
                                                       ButtonType::Color,
                                                       "",
                                                       0,
                                                       0,
                                                       button_size,
                                                       button_size,
                                                       &color_ptr,
                                                       "color",
                                                       -1,
                                                       0.0,
                                                       1.0,
                                                       "");
    color_but->is_pallete_color = true;
    color_but->palette_color_index = col_id;

    /* Check if this is the active color (visual indicator) */
    if (paint && paint->brush) {
      if (fabsf(color->rgb[0] - current_brush_color[0]) < 0.01f &&
          fabsf(color->rgb[1] - current_brush_color[1]) < 0.01f &&
          fabsf(color->rgb[2] - current_brush_color[2]) < 0.01f)
      {
        /* Mark as selected to show visual indicator */
        color_but->flag |= UI_SELECT;
      }
    }

    row_cols++;
    col_id++;
    color_count++;
  }
  (void)color_count;
}

/* Callback function for Add button - calls operator and triggers popup refresh */
static void ui_colorpicker_palette_add_cb(bContext *C, void *but1, void *arg)
{
  Button *but = static_cast<Button *>(but1);
  Block *block = static_cast<Block *>(arg);
  
  if (!block || !but || !C) {
    return;
  }
  
  /* Get popup handle to access color picker data */
  PopupBlockHandle *popup = block->handle;
  if (!popup) {
    ARegion *region = CTX_wm_region(C);
    if (region && region->regiondata) {
      popup = static_cast<PopupBlockHandle *>(region->regiondata);
    }
  }
  
  /* Get color from color picker - try from original button first, then retvec */
  float color[3] = {0.0f, 0.0f, 0.0f};
  bool color_found = false;
  
  if (popup && popup->popup_create_vars.but) {
    /* Get color from original button that opened color picker */
    Button *from_but = popup->popup_create_vars.but;
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    
    /* Use button_v4_get to get color with alpha - it handles all cases (RNA, editvec, etc.)
     * and returns color in scene linear space (which is what palette expects) */
    button_v4_get(from_but, rgba);
    /* Extract RGB (alpha is not stored in PaletteColor, only RGB) */
    copy_v3_v3(color, rgba);
    color_found = true;
    printf("[DEBUG] ui_colorpicker_palette_add_cb: got color from button: %.3f, %.3f, %.3f (alpha: %.3f)\n",
           color[0], color[1], color[2], rgba[3]);
  }
  else if (popup) {
    /* Fallback: try retvec if button not available */
    /* Use color from popup retvec (current color picker value) - retvec is 4 components */
    copy_v3_v3(color, popup->retvec);
    color_found = true;
    printf("[DEBUG] ui_colorpicker_palette_add_cb: got color from popup->retvec: %.3f, %.3f, %.3f (alpha: %.3f)\n",
           color[0], color[1], color[2], popup->retvec[3]);
  }
  
  /* Fallback: try to get color from paint context (for paint modes) */
  if (!color_found) {
    Paint *paint = BKE_paint_get_active_from_context(C);
    if (paint) {
      const Brush *brush = BKE_paint_brush_for_read(paint);
      if (brush) {
        const float *brush_color = BKE_brush_color_get(paint, brush);
        if (brush_color) {
          copy_v3_v3(color, brush_color);
          color_found = true;
          printf("[DEBUG] ui_colorpicker_palette_add_cb: got color from brush: %.3f, %.3f, %.3f\n",
                 color[0], color[1], color[2]);
        }
      }
    }
  }
  
  /* Get palette - try from button custom_data first, then from paint context */
  Palette *palette = nullptr;
  if (but->custom_data) {
    palette = static_cast<Palette *>(but->custom_data);
  }
  else {
    Paint *paint = BKE_paint_get_active_from_context(C);
    if (paint && paint->palette) {
      palette = paint->palette;
    }
  }
  
  if (!palette) {
    printf("[DEBUG] ui_colorpicker_palette_add_cb: no palette found\n");
    return;
  }
  
  /* If color not found yet, try to get it from paint context (for paint modes) */
  if (!color_found) {
    Paint *paint = BKE_paint_get_active_from_context(C);
    if (paint) {
      const Brush *brush = BKE_paint_brush_for_read(paint);
      if (brush) {
        const float *brush_color = BKE_brush_color_get(paint, brush);
        if (brush_color) {
          copy_v3_v3(color, brush_color);
          color_found = true;
          printf("[DEBUG] ui_colorpicker_palette_add_cb: got color from brush: %.3f, %.3f, %.3f\n",
                 color[0], color[1], color[2]);
        }
      }
    }
  }
  
  /* Check if color already exists in palette */
  if (color_found) {
    LISTBASE_FOREACH (PaletteColor *, existing_color, &palette->colors) {
      if (compare_v3v3(existing_color->color, color, 0.001f)) {
        printf("[DEBUG] Color already exists in palette - not adding\n");
        
        /* Show user feedback */
        wmWindowManager *wm = CTX_wm_manager(C);
        if (wm) {
          BKE_reportf(&wm->runtime->reports,
                      RPT_WARNING,
                      "Color (%.3f, %.3f, %.3f) already exists in palette",
                      color[0],
                      color[1],
                      color[2]);
          WM_report_banner_show(wm, CTX_wm_window(C));
        }
        return;
      }
    }
  }
  
  /* Add color to palette directly (bypassing operator for non-paint contexts) */
  PaletteColor *palcol = BKE_palette_color_add(palette);
  if (palcol) {
    palette->active_color = BLI_listbase_count(&palette->colors) - 1;
    
    /* Set color if we found one */
    if (color_found) {
      copy_v3_v3(palcol->color, color);
      palcol->value = 0.0f;
      printf("[DEBUG] ui_colorpicker_palette_add_cb: added color to palette: %.3f, %.3f, %.3f\n",
             palcol->color[0], palcol->color[1], palcol->color[2]);
    }
  }
  
  if (!popup || !popup->can_refresh) {
    printf("[DEBUG] ui_colorpicker_palette_add_cb: popup handle not found or can_refresh=false\n");
    return;
  }
  
  /* Reset prev_block_rect to force popup resize */
  popup->prev_block_rect.xmin = 0;
  popup->prev_block_rect.ymin = 0;
  popup->prev_block_rect.xmax = 0;
  popup->prev_block_rect.ymax = 0;
  
  /* Set bounds type on current block for proper popup size recalculation */
  block->bounds_type = BLOCK_BOUNDS_POPUP_MOUSE;
  
  /* Set RETURN_UPDATE to trigger popup refresh */
  popup->menuretval = RETURN_UPDATE;
  
  /* Tag region for refresh UI - this triggers popup_block_refresh */
  if (popup->region) {
    ED_region_tag_refresh_ui(popup->region);
  }
  
  printf("[DEBUG] ui_colorpicker_palette_add_cb: called operator and triggered popup refresh\n");
}

/* Callback function for Delete button - calls operator and triggers popup refresh */
static void ui_colorpicker_palette_delete_cb(bContext *C, void *but1, void *arg)
{
  Button *but = static_cast<Button *>(but1);
  Block *block = static_cast<Block *>(arg);
  
  if (!block || !but || !C) {
    return;
  }
  
  /* Call the operator directly */
  wmOperatorType *ot = WM_operatortype_find("PALETTE_OT_color_delete", false);
  if (ot) {
    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, nullptr, nullptr);
  }
  
  /* Get popup handle - try block->handle first, then region from context */
  PopupBlockHandle *popup = block->handle;
  if (!popup) {
    ARegion *region = CTX_wm_region(C);
    if (region && region->regiondata) {
      popup = static_cast<PopupBlockHandle *>(region->regiondata);
    }
  }
  if (!popup || !popup->can_refresh) {
    printf("[DEBUG] ui_colorpicker_palette_delete_cb: popup handle not found or can_refresh=false\n");
    return;
  }
  
  /* Reset prev_block_rect to force popup resize */
  popup->prev_block_rect.xmin = 0;
  popup->prev_block_rect.ymin = 0;
  popup->prev_block_rect.xmax = 0;
  popup->prev_block_rect.ymax = 0;
  
  /* Set bounds type on current block for proper popup size recalculation */
  block->bounds_type = BLOCK_BOUNDS_POPUP_MOUSE;
  
  /* Set RETURN_UPDATE to trigger popup refresh */
  popup->menuretval = RETURN_UPDATE;
  
  /* Tag region for refresh UI - this triggers popup_block_refresh */
  if (popup->region) {
    ED_region_tag_refresh_ui(popup->region);
  }
  
  printf("[DEBUG] ui_colorpicker_palette_delete_cb: called operator and triggered popup refresh\n");
}

/* Callback function for Size toggle button - toggles button size and triggers popup refresh */
static void ui_colorpicker_palette_size_toggle_cb(bContext *C, void *but1, void *arg)
{
  Button *but = static_cast<Button *>(but1);
  Block *block = static_cast<Block *>(arg);
  
  if (!block || !but || !C) {
    return;
  }
  
  /* Toggle palette large buttons state */
  palette_large_buttons = !palette_large_buttons;
  
  /* Get popup handle - try block->handle first, then region from context */
  PopupBlockHandle *popup = block->handle;
  if (!popup) {
    ARegion *region = CTX_wm_region(C);
    if (region && region->regiondata) {
      popup = static_cast<PopupBlockHandle *>(region->regiondata);
    }
  }
  if (!popup || !popup->can_refresh) {
    printf("[DEBUG] ui_colorpicker_palette_size_toggle_cb: popup handle not found or can_refresh=false\n");
    return;
  }
  
  /* Reset prev_block_rect to force popup resize */
  popup->prev_block_rect.xmin = 0;
  popup->prev_block_rect.ymin = 0;
  popup->prev_block_rect.xmax = 0;
  popup->prev_block_rect.ymax = 0;
  
  /* Set bounds type on current block for proper popup size recalculation */
  block->bounds_type = BLOCK_BOUNDS_POPUP_MOUSE;
  
  /* Set RETURN_UPDATE to trigger popup refresh */
  popup->menuretval = RETURN_UPDATE;
  
  /* Tag region for refresh UI - this triggers popup_block_refresh */
  if (popup->region) {
    ED_region_tag_refresh_ui(popup->region);
  }
  
  printf("[DEBUG] ui_colorpicker_palette_size_toggle_cb: toggled button size and triggered popup refresh\n");
}

}  // namespace blender::ui
