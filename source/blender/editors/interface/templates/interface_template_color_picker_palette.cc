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
#include "DNA_ID.h"

#include "BKE_context.hh"
#include "BKE_paint.hh"
#include "BKE_brush.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_interface_c.hh"
#include "interface_intern.hh"

#include "IMB_colormanagement.hh"

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

  /* Get context for layout panel */
  bContext *C = static_cast<bContext *>(block->evil_C);
  if (C == nullptr) {
    return;
  }

  /* Determine context-specific panel ID based on the owner type.
   * This allows separate state for Paint modes vs Material/other contexts.
   * 
   * Strategy:
   * 1. Check RNA struct identifier name for Paint-related types
   * 2. Check if ptr->data is a Material ID (ID_MA)
   * 3. Check owner ID code for Material (ID_MA)
   * 4. Default to generic ID if context cannot be determined
   */
  std::string panel_id = "color_picker_palette";
  const char *struct_id = RNA_struct_identifier(ptr->type);
  
  printf("[DEBUG] template_colorpicker_palette: struct_id='%s', propname='%s', ptr->data=%p, ptr->owner_id=%p\n", 
         struct_id ? struct_id : "NULL", propname.c_str(), ptr->data, ptr->owner_id);
  
  /* Check if this is a Paint context by struct name */
  if (struct_id) {
    if (strstr(struct_id, "Paint") || strstr(struct_id, "Sculpt")) {
      panel_id = "color_picker_palette_paint";
    }
    else if (strstr(struct_id, "Material")) {
      panel_id = "color_picker_palette_material";
    }
  }
  
  /* Check if ptr->data is a Material ID (for Shader Editor) */
  if (panel_id == "color_picker_palette" && ptr->data) {
    ID *id = static_cast<ID *>(ptr->data);
    if (id && GS(id->name) == ID_MA) {
      panel_id = "color_picker_palette_material";
    }
  }
  
  /* Fallback: check owner ID code for Material */
  if (panel_id == "color_picker_palette" && ptr->owner_id) {
    const short idcode = GS(ptr->owner_id->name);
    if (idcode == ID_MA) {
      panel_id = "color_picker_palette_material";
    }
  }

  /* Get the region where state should be stored.
   * For popups (temporary regions), use a persistent region from the area instead.
   * This ensures state persists between popup sessions. */
  ARegion *popup_region = CTX_wm_region_popup(C);
  bool is_in_popup = (popup_region != nullptr) || (block->handle != nullptr);
  
  Panel *state_panel = nullptr;
  
  /* Check if we're inside a popup */
  if (is_in_popup) {
    /* Get the actual popup region if available */
    ARegion *actual_popup_region = popup_region;
    if (!actual_popup_region && block->handle) {
      PopupBlockHandle *popup_handle = static_cast<PopupBlockHandle *>(block->handle);
      if (popup_handle) {
        actual_popup_region = popup_handle->region;
      }
    }
    
    /* Try to get persistent region from area */
    ScrArea *area = CTX_wm_area(C);
    if (area) {
      /* Find first non-temporary region in the area */
      LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
        if (region->regiontype != RGN_TYPE_TEMPORARY) {
          /* Find or create root panel in persistent region */
          LISTBASE_FOREACH (Panel *, panel, &region->panels) {
            if (panel->type == nullptr) { /* Root panel has no type */
              state_panel = panel;
              break;
            }
          }
          
          if (state_panel) {
            break;
          }
        }
      }
    }
    
    /* If no root panel found in persistent region, try to create one */
    if (!state_panel && area) {
      /* Find first persistent region to create root panel in */
      LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
        if (region->regiontype != RGN_TYPE_TEMPORARY) {
          /* Create root panel if it doesn't exist */
          Panel *root_panel = nullptr;
          LISTBASE_FOREACH (Panel *, panel, &region->panels) {
            if (panel->type == nullptr) {
              root_panel = panel;
              break;
            }
          }
          
          if (!root_panel) {
            /* Create a root panel for this region */
            root_panel = BKE_panel_new(nullptr);
            BLI_addtail(&region->panels, root_panel);
          }
          
          state_panel = root_panel;
          break;
        }
      }
    }
    
    if (!state_panel) {
      state_panel = layout->root_panel();
    }
  }
  else {
    /* Use current layout's root panel for persistent regions */
    state_panel = layout->root_panel();
  }
  
  PanelLayout panel;
  
  if (state_panel && state_panel != layout->root_panel()) {
    /* Use persistent panel for state storage (for popups) */
    LayoutPanelState *state = BKE_panel_layout_panel_state_ensure(
        state_panel, panel_id.c_str(), true);

    PointerRNA state_ptr = RNA_pointer_create_discrete(nullptr, &RNA_LayoutPanelState, state);
    panel = layout->panel_prop(C, &state_ptr, "is_open");
  }
  else {
    /* Use default panel() for persistent regions or fallback */
    panel = layout->panel(C, panel_id.c_str(), true);
  }
  
  panel.header->label(IFACE_("Color Palette"), ICON_COLOR);
  
  /* Header is always created, even when panel is closed.
   * Only show content if panel is open */
  if (!panel.body) {
    /* Don't return early - header needs to be in layout for clicks to work */
    /* The header will be registered in layout_panels.headers during resolve */
    return;
  }

  const PointerRNA cptr = RNA_property_pointer_get(ptr, prop);
  
  /* If no palette, show template_id selector in panel body */
  if (!cptr.data || !RNA_struct_is_a(cptr.type, &RNA_Palette)) {
    template_id_simple(panel.body, C, ptr, propname, "PALETTE_OT_new", nullptr, 0, std::nullopt);
    return;
  }

  /* Show palette selector with name, duplicate and delete options */
  /* Use custom unlink operator to refresh popup after unlink */
  template_id_simple(panel.body, C, ptr, propname, "PALETTE_OT_new", "PALETTE_OT_unlink", 0, std::nullopt);

  Palette *palette = static_cast<Palette *>(cptr.data);

  /* Color controls row */
  Layout *col = &panel.body->column(true);
  Layout *button_row = &col->row(false);
  
  /* Left side: Add/Delete buttons */
  button_row->row(true);
  
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
  /* Disable BUT_ICON_LEFT flag to center icon instead of left-aligning */
  button_drawflag_disable(add_but, BUT_ICON_LEFT);
                
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
  /* Disable BUT_ICON_LEFT flag to center icon instead of left-aligning */
  button_drawflag_disable(del_but, BUT_ICON_LEFT);
  
  /* Right side: Size toggle button */
  Layout *right_buttons = &button_row->row(true);
  right_buttons->alignment_set(LayoutAlign::Right);
  
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
  /* Disable BUT_ICON_LEFT flag to center icon instead of left-aligning */
  button_drawflag_disable(size_but, BUT_ICON_LEFT);

  /* Color grid */
  const float base_button_size = palette_large_buttons ? (UI_UNIT_X * 1.8f) : UI_UNIT_X;
  /* Calculate number of columns that fit exactly in the available width */
  const int cols_per_row = std::max(int(panel.body->width() / base_button_size), 1);
  /* Adjust button size to fill the width exactly when possible */
  const float button_size = (cols_per_row > 1) ? (panel.body->width() / cols_per_row) : base_button_size;

  /* Check if palette has any colors */
  int color_count = BLI_listbase_count(&palette->colors);

  if (color_count == 0) {
    /* Show empty palette message in gray/disabled style */
    Layout *empty_layout = &panel.body->column(true);
    empty_layout->alignment_set(LayoutAlign::Center);
    Button *empty_label = uiItemL_ex(empty_layout, IFACE_("No colors in palette"), ICON_NONE, false, false);
    button_flag_enable(empty_label, BUT_DISABLED);
  }
  else {
    Layout *col_grid = &panel.body->column(true);
    col_grid->row(true);

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

    LISTBASE_FOREACH (PaletteColor *, color, &palette->colors) {
      if (row_cols >= cols_per_row) {
        col_grid->row(true);
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
    }
  }
}

/* Callback function for Add button - calls operator and triggers popup refresh */
static void ui_colorpicker_palette_add_cb(bContext *C, void *but1, void *arg)
{
  Button *but = static_cast<Button *>(but1);
  Block *block = static_cast<Block *>(arg);
  
  if (!block || !but || !C) {
    return;
  }
  
  PopupBlockHandle *popup = block->handle;
  if (!popup) {
    ARegion *region = CTX_wm_region_popup(C);
    if (!region) {
      region = CTX_wm_region(C);
    }
    if (region && region->regiondata) {
      popup = static_cast<PopupBlockHandle *>(region->regiondata);
    }
  }

  float color[3] = {0.0f, 0.0f, 0.0f};
  bool color_found = false;

  const bool is_color_picker_popup = (block->color_pickers.list.first != nullptr);

  if (popup && is_color_picker_popup) {
    /* Only use retvec if it's not zero or if we are sure it's from a real picker.
     * In context menus, retvec is often zero. */
    if (popup->retvec[0] != 0.0f || popup->retvec[1] != 0.0f || popup->retvec[2] != 0.0f) {
      copy_v3_v3(color, popup->retvec);
      color_found = true;
      printf("DEBUG: Palette Add: color from retvec (%.3f, %.3f, %.3f)\n", color[0], color[1], color[2]);
    }
  }

  if (!color_found && popup && popup->popup_create_vars.but) {
    Button *from_but = popup->popup_create_vars.but;
    if (from_but->rnaprop &&
        ELEM(RNA_property_subtype(from_but->rnaprop), PROP_COLOR, PROP_COLOR_GAMMA))
    {
      float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      button_v4_get(from_but, rgba);
      copy_v3_v3(color, rgba);
      
      /* Palette colors are stored in scene linear space. */
      if (RNA_property_subtype(from_but->rnaprop) == PROP_COLOR_GAMMA) {
        IMB_colormanagement_srgb_to_scene_linear_v3(color, color);
      }
      
      color_found = true;
    }
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
    return;
  }
  
  /* Check if color already exists in palette */
  if (color_found) {
    LISTBASE_FOREACH (PaletteColor *, existing_color, &palette->colors) {
      if (compare_v3v3(existing_color->color, color, 0.01f)) {
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
    }
  }
  
  if (!popup || !popup->can_refresh) {
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
  popup->menuretval |= RETURN_UPDATE;
  
  /* Tag region for refresh UI - this triggers popup_block_refresh */
  if (popup->region) {
    ED_region_tag_refresh_ui(popup->region);
  }
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
}

}  // namespace blender::ui
