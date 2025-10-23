/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_screen.hh"
#include "BKE_asset.hh"
#include "BKE_texture.h"
#include "BKE_main.hh"
#include "BKE_lib_id.hh"
#include "BKE_brush.hh"
#include "BKE_paint.hh"
#include "BKE_context.hh"

#include "ED_asset_image_utils.hh"
#include "ED_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"

#include "MEM_guardedalloc.h"

#include "ED_asset_image_utils.hh"
#include "ED_asset_shelf.hh"
#include "ED_asset_list.hh"
#include "ED_asset_filter.hh"
#include "ED_asset_library.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_path.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_interface_c.hh"
#include "UI_tree_view.hh"
#include "UI_grid_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "AS_asset_library.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_representation.hh"

#include "../../asset/intern/asset_shelf.hh"

#include "interface_templates_intern.hh"
#include "interface_intern.hh"

using blender::StringRef;
using blender::StringRefNull;
using blender::Vector;

namespace blender::ui {

constexpr int LEFT_COL_WIDTH_UNITS = 8;
constexpr int RIGHT_COL_WIDTH_UNITS_DEFAULT = 40;

/**
 * Template data structure for Asset Catalog Image Browser
 */
struct TemplateAssetCatalogImageBrowser {
  PointerRNA ptr{};
  PropertyRNA *prop{nullptr};
  
  const bContext *context{nullptr};
  
  int preview_rows{0};
  int preview_cols{0};
  
  bool show_local_images{false};
  bool auto_convert_to_assets{false};
  
  /* Catalog filtering */
  bUUID active_catalog_id{};
  bool show_all_catalogs{false};
  bool show_catalog_selector{true};
  
  /* Search filtering */
  char search_filter[64]{};
  
  /* Asset shelf integration */
  AssetShelf *shelf{nullptr};
  
  ~TemplateAssetCatalogImageBrowser()
  {
    if (shelf) {
      MEM_delete(shelf);
    }
  }
};

/**
 * Static storage for popover template data
 */
class StaticPopupImageBrowsers {
 public:
  Vector<TemplateAssetCatalogImageBrowser *> popup_browsers;

  ~StaticPopupImageBrowsers()
  {
    for (TemplateAssetCatalogImageBrowser *browser : popup_browsers) {
      MEM_delete(browser);
    }
  }

  static Vector<TemplateAssetCatalogImageBrowser *> &browsers()
  {
    static StaticPopupImageBrowsers storage;
    return storage.popup_browsers;
  }
};

/**
 * Debug function to convert all textures and their images to assets
 */
static void debug_convert_all_textures_to_assets(const bContext &C, Main *bmain)
{
  static bool debug_done = false;
  if (debug_done) {
    return; /* Only run debug conversion once per session */
  }
  debug_done = true;
  
  printf("DEBUG: Starting debug conversion of all textures to assets\n");
  
  int texture_count = 0;
  int texture_image_count = 0;
  int all_image_count = 0;
  
  /* Iterate through all textures */
  for (Tex *tex = static_cast<Tex *>(bmain->textures.first); tex; tex = static_cast<Tex *>(tex->id.next)) {
    texture_count++;
    
    /* Check if texture has an image */
    if (tex->type == TEX_IMAGE && tex->ima) {
      /* Convert the image to asset if it's not already an asset */
      if (!tex->ima->id.asset_data) {
        if (ensure_image_is_asset(&C, tex->ima)) {
          texture_image_count++;
        }
      }
    }
  }
  
  /* Also convert all standalone images */
  for (Image *image = static_cast<Image *>(bmain->images.first); image; image = static_cast<Image *>(image->id.next)) {
    if (!image->id.asset_data) {
      if (ensure_image_is_asset(&C, image)) {
        all_image_count++;
      }
    }
  }
  
  printf("DEBUG: Conversion summary: %d textures, %d texture images, %d standalone images converted\n", 
         texture_count, texture_image_count, all_image_count);
}

/**
 * Ensure images are initialized for the browser
 */
static void ensure_images_initialized(const bContext &C, 
                                     TemplateAssetCatalogImageBrowser &browser_data)
{
  Main *bmain = CTX_data_main(&C);
  if (!bmain) {
    return;
  }
  
  /* Debug: Convert all textures and their images to assets */
  debug_convert_all_textures_to_assets(C, bmain);
  
  /* Автоматическая конвертация, если включена */
  if (browser_data.auto_convert_to_assets) {
    int converted_count = auto_convert_images_to_assets(&C, bmain);
    if (converted_count > 0) {
      printf("DEBUG: Auto-converted %d images to assets\n", converted_count);
    }
  }
}

/**
 * Get filtered list of image assets based on browser settings
 */
static Vector<Image *> get_filtered_image_assets(const bContext &C,
                                                 TemplateAssetCatalogImageBrowser &browser_data)
{
  Vector<Image *> filtered_images;
  Main *bmain = CTX_data_main(&C);
  
  if (!bmain) {
    return filtered_images;
  }
  
  /* Get all image assets using the asset system */
  blender::Vector<Image *> image_assets = get_all_image_assets(bmain);
  
  /* Filter by catalog if needed - use shelf settings for catalog filtering */
  if (browser_data.shelf) {
    if (blender::ed::asset::shelf::settings_is_all_catalog_active(browser_data.shelf->settings)) {
      filtered_images = image_assets;
    } else {
      const char *active_catalog_path = browser_data.shelf->settings.active_catalog_path;
      if (active_catalog_path && active_catalog_path[0] != '\0') {
        /* Get the asset library to find catalog by path */
        const asset_system::AssetLibrary *library = blender::ed::asset::list::library_get_once_available(
            browser_data.shelf->settings.asset_library_reference);
        
        if (library) {
          const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog_by_path(
              asset_system::AssetCatalogPath(active_catalog_path));
          
          if (catalog) {
            for (Image *image : image_assets) {
              if (image->id.asset_data && 
                  BLI_uuid_equal(image->id.asset_data->catalog_id, catalog->catalog_id)) {
                filtered_images.append(image);
              }
            }
          } else {
            /* If catalog not found, show all images */
            filtered_images = image_assets;
          }
        } else {
          filtered_images = image_assets;
        }
      } else {
        filtered_images = image_assets;
      }
    }
  } else {
    filtered_images = image_assets;
  }
  
  /* Filter by search query if provided */
  if (browser_data.search_filter[0] != '\0') {
    Vector<Image *> search_filtered_images;
    const char *search_query = browser_data.search_filter;
    
    for (Image *image : filtered_images) {
      const char *image_name = image->id.name + 2; /* Skip "IM" prefix */
      
      /* Case-insensitive search in image name */
      if (BLI_strcasestr(image_name, search_query)) {
        search_filtered_images.append(image);
      }
    }
    
    filtered_images = search_filtered_images;
  }
  
  return filtered_images;
}

/**
 * Callback for search filter updates
 */
static void search_filter_callback(bContext *C, void *arg_template, void * /*arg_unused*/)
{
  TemplateAssetCatalogImageBrowser *template_data = static_cast<TemplateAssetCatalogImageBrowser *>(arg_template);
  
  if (!template_data) {
    return;
  }
  
  /* Update UI to refresh the grid with new search filter */
  WM_event_add_notifier(C, NC_WM | ND_DATACHANGED, nullptr);
}

/**
 * Callback for toggling catalog selector visibility
 */
static void template_catalog_selector_toggle_cb(bContext *C, void *arg_template, void * /*arg_unused*/)
{
  TemplateAssetCatalogImageBrowser *template_data = static_cast<TemplateAssetCatalogImageBrowser *>(arg_template);
  
  printf("DEBUG: Toggle callback called\n");
  
  if (!template_data) {
    printf("DEBUG: No template data in callback\n");
    return;
  }
  
  /* Toggle catalog selector visibility */
  template_data->show_catalog_selector = !template_data->show_catalog_selector;
  
  printf("DEBUG: Toggled show_catalog_selector to %s\n", 
         template_data->show_catalog_selector ? "true" : "false");
  
  /* Update UI */
  WM_event_add_notifier(C, NC_WM | ND_DATACHANGED, nullptr);
}

/**
 * Callback for image selection - creates texture and applies to brush
 */
static void template_asset_catalog_image_set_cb(bContext *C, void *arg_template, void *arg_image)
{
  TemplateAssetCatalogImageBrowser *template_data = static_cast<TemplateAssetCatalogImageBrowser *>(arg_template);
  Image *image = static_cast<Image *>(arg_image);
  
  if (!template_data || !template_data->prop || !image) {
    return;
  }
  
  Main *bmain = CTX_data_main(C);
  if (!bmain) {
    return;
  }
  
  /* Get the current brush */
  Scene *scene = CTX_data_scene(C);
  if (!scene) {
    return;
  }
  
  wmWindow *window = CTX_wm_window(C);
  ViewLayer *view_layer = WM_window_get_active_view_layer(window);
  Brush *brush = BKE_paint_brush(BKE_paint_get_active(scene, view_layer));
  
  if (!brush) {
    return;
  }
  
  /* Create a texture from the image */
  char texture_name[64];
  BLI_snprintf(texture_name, sizeof(texture_name), "%s_tex", image->id.name + 2);
  
  Tex *texture = BKE_texture_add(bmain, texture_name);
  if (!texture) {
    return;
  }
  
  /* Set texture type to image */
  BKE_texture_type_set(texture, TEX_IMAGE);
  texture->ima = image;
  id_us_plus(&image->id);
  
  /* Set the texture in the brush using the proper function */
  /* Set the texture in the brush */
  brush->mtex.tex = texture;
  id_us_plus(&texture->id);
  
  /* Also apply the texture to the property if it's a texture property */
  PointerRNA texture_ptr = RNA_id_pointer_create(&texture->id);
  RNA_property_pointer_set(&template_data->ptr, template_data->prop, texture_ptr, nullptr);
  RNA_property_update(C, &template_data->ptr, template_data->prop);
  
  /* Tag brush for update */
  BKE_brush_tag_unsaved_changes(brush);
  
  /* Update UI to refresh preview */
  WM_event_add_notifier(C, NC_WM | ND_DATACHANGED, nullptr);
  
  printf("DEBUG: Applied image '%s' as texture to brush '%s'\n", 
         image->id.name + 2, brush->id.name + 2);
}

/**
 * Calculate minimum popover width based on catalog + 6 grid elements
 */
static int calculate_minimum_popover_width()
{
  /* Catalog width + toggle button + 6 grid elements */
  return LEFT_COL_WIDTH_UNITS + 2 + (6 * 6); /* 6 units per grid element */
}

/**
 * Calculate minimum popover height for 3 rows
 */
static int calculate_minimum_popover_height()
{
  /* 3 rows of grid elements (6 for preview + 1 for name) */
  return 3 * 7 * UI_UNIT_Y; /* 7 units per row */
}

/**
 * Ensure the popover width fits into the window
 */
static int layout_width_units_clamped(const wmWindow *win)
{
  const int max_units_x = (WM_window_native_pixel_x(win) / UI_UNIT_X) - 2;
  const int min_width = calculate_minimum_popover_width();
  return std::max(min_width, std::min(LEFT_COL_WIDTH_UNITS + RIGHT_COL_WIDTH_UNITS_DEFAULT, max_units_x));
}

/**
 * Lookup template data from context
 */
static TemplateAssetCatalogImageBrowser *lookup_template_from_context(const bContext *C)
{
  /* Try to get template data from context first */
  PointerRNA context_ptr = CTX_data_pointer_get(C, "template_asset_catalog_image_browser");
  if (context_ptr.data) {
    return static_cast<TemplateAssetCatalogImageBrowser *>(context_ptr.data);
  }
  
  /* Fallback to the first available template data */
  Vector<TemplateAssetCatalogImageBrowser *> &browsers = StaticPopupImageBrowsers::browsers();
  if (!browsers.is_empty()) {
    return browsers[0];
  }
  return nullptr;
}

/**
 * Operator for activating image assets and applying them to brush texture settings
 */
static wmOperatorStatus image_asset_activate_exec(bContext *C, wmOperator *op)
{
  /* Get the asset from context */
  const asset_system::AssetRepresentation *asset = CTX_wm_asset(C);
  if (!asset) {
    BKE_report(op->reports, RPT_ERROR, "No asset selected");
    return OPERATOR_CANCELLED;
  }
  
  /* Verify it's an image asset */
  if (asset->get_id_type() != ID_IM) {
    BKE_report(op->reports, RPT_ERROR, "Selected asset is not an image");
    return OPERATOR_CANCELLED;
  }
  
  /* Get the image ID */
  ID *image_id = asset->local_id();
  if (!image_id) {
    BKE_report(op->reports, RPT_ERROR, "Image asset not found in current file");
    return OPERATOR_CANCELLED;
  }
  
  Image *image = reinterpret_cast<Image *>(image_id);
  
  /* Get the template data from context */
  TemplateAssetCatalogImageBrowser *browser_data = lookup_template_from_context(C);
  if (!browser_data || !browser_data->prop) {
    BKE_report(op->reports, RPT_ERROR, "No template context found");
    return OPERATOR_CANCELLED;
  }
  
  Main *bmain = CTX_data_main(C);
  
  /* Get the current brush */
  Scene *scene = CTX_data_scene(C);
  if (!scene) {
    BKE_report(op->reports, RPT_ERROR, "No active scene");
    return OPERATOR_CANCELLED;
  }
  
  wmWindow *window = CTX_wm_window(C);
  ViewLayer *view_layer = WM_window_get_active_view_layer(window);
  Brush *brush = BKE_paint_brush(BKE_paint_get_active(scene, view_layer));
  
  if (!brush) {
    BKE_report(op->reports, RPT_ERROR, "No active brush");
    return OPERATOR_CANCELLED;
  }
  
  /* Create a texture from the image */
  char texture_name[64];
  BLI_snprintf(texture_name, sizeof(texture_name), "%s_tex", image->id.name + 2);
  
  Tex *texture = BKE_texture_add(bmain, texture_name);
  if (!texture) {
    BKE_report(op->reports, RPT_ERROR, "Failed to create texture");
    return OPERATOR_CANCELLED;
  }
  
  /* Set texture type to image */
  BKE_texture_type_set(texture, TEX_IMAGE);
  texture->ima = image;
  id_us_plus(&image->id);
  
  /* Set the texture in the brush using the proper function */
  /* Set the texture in the brush */
  brush->mtex.tex = texture;
  id_us_plus(&texture->id);
  
  /* Also apply the texture to the property if it's a texture property */
  PointerRNA texture_ptr = RNA_id_pointer_create(&texture->id);
  RNA_property_pointer_set(&browser_data->ptr, browser_data->prop, texture_ptr, nullptr);
  RNA_property_update(C, &browser_data->ptr, browser_data->prop);
  
  /* Tag brush for update */
  BKE_brush_tag_unsaved_changes(brush);
  
  /* Update UI to refresh preview */
  WM_event_add_notifier(C, NC_WM | ND_DATACHANGED, nullptr);
  
  BKE_reportf(op->reports, RPT_INFO, "Applied image '%s' as texture to brush '%s'", 
              image->id.name + 2, brush->id.name + 2);
  
  return OPERATOR_FINISHED;
}

static void IMAGE_ASSET_OT_activate(wmOperatorType *ot)
{
  ot->name = "Activate Image Asset";
  ot->description = "Apply selected image asset to brush texture settings";
  ot->idname = "IMAGE_ASSET_OT_activate";
  
  ot->exec = image_asset_activate_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/**
 * Create a custom AssetShelfType for image assets
 */
static AssetShelfType *create_image_asset_shelf_type()
{
  static AssetShelfType *shelf_type = nullptr;
  
  if (!shelf_type) {
    shelf_type = MEM_new<AssetShelfType>(__func__);
    
    /* Initialize the shelf type */
    STRNCPY_UTF8(shelf_type->idname, "IMAGE_ASSET_SHELF");
    shelf_type->space_type = SPACE_EMPTY; /* Allow in any space */
    
    /* Set up asset polling to only show image assets */
    shelf_type->asset_poll = [](const AssetShelfType * /*shelf_type*/, 
                               const asset_system::AssetRepresentation *asset) -> bool {
      if (!asset) {
        return false;
      }
      
      /* Only show image assets */
      return asset->get_id_type() == ID_IM;
    };
    
    /* Set up activation operator for setting image as texture */
    shelf_type->activate_operator = "IMAGE_ASSET_OT_activate";
    
    /* Set default preview size */
    shelf_type->default_preview_size = ASSET_SHELF_PREVIEW_SIZE_DEFAULT;
  }
  
  return shelf_type;
}

/**
 * Draw catalog selector (left column) with asset system integration
 */
static void draw_catalog_selector(uiLayout &layout,
                                  const bContext &C,
                                  TemplateAssetCatalogImageBrowser &browser_data)
{
  /* Ensure we have an AssetShelf for asset system integration */
  if (!browser_data.shelf) {
    /* Create AssetShelfType for images */
    AssetShelfType *shelf_type = create_image_asset_shelf_type();
    
    /* Create AssetShelf from the type */
    browser_data.shelf = blender::ed::asset::shelf::create_shelf_from_type(*shelf_type);
    
    /* Initialize with current file library by default */
    browser_data.shelf->settings.asset_library_reference = blender::ed::asset::library_reference_from_enum_value(ASSET_LIBRARY_LOCAL);
  }

  /* Use the asset system's library selector */
  /* Create a namespace alias to avoid conflicts */
  namespace asset_shelf = blender::ed::asset::shelf;
  
  /* Draw library selector - this provides the asset library dropdown and refresh button */
  asset_shelf::library_selector_draw(&C, &layout, *browser_data.shelf);
  
  /* Draw catalog tree - this provides the catalog hierarchy */
  
  const asset_system::AssetLibrary *library = blender::ed::asset::list::library_get_once_available(
      browser_data.shelf->settings.asset_library_reference);
  
  if (library) {
    /* Create catalog tree view */
    uiBlock *block = layout.block();
    
    /* Create AssetCatalogTreeView similar to asset_shelf_popover.cc */
    class AssetCatalogTreeView : public ui::AbstractTreeView {
      AssetShelf &shelf_;
      asset_system::AssetCatalogTree catalog_tree_;

     public:
      AssetCatalogTreeView(const asset_system::AssetLibrary &library, AssetShelf &shelf)
          : shelf_(shelf)
      {
        /* 
         * Use build_filtered_catalog_tree to get only catalogs that contain image assets.
         * 
         * IMPORTANT: This shows ONLY catalogs that contain at least one image asset.
         * Empty catalogs or catalogs with only other asset types (materials, objects, etc.)
         * will NOT be displayed in the tree view.
         * 
         * How it works:
         * 1. Iterates through all assets in the library
         * 2. Filters assets using the predicate (only ID_IM type passes)
         * 3. Collects catalog paths that contain these filtered assets
         * 4. Builds a tree containing only catalogs with at least one image asset
         */
        catalog_tree_ = blender::ed::asset::build_filtered_catalog_tree(
            library,
            shelf_.settings.asset_library_reference,
            [this](const asset_system::AssetRepresentation &asset) {
              /* Only show image assets - this predicate determines which assets are considered */
              return asset.get_id_type() == ID_IM;
            });
      }

      void build_tree() override
      {
        /* 
         * If catalog_tree_ is empty, it means there are no catalogs containing image assets
         * in the selected asset library. This can happen when:
         * - No image assets exist in the library
         * - All image assets are not marked as assets
         * - All image assets are in catalogs that don't exist in the library
         */
        if (catalog_tree_.is_empty()) {
          auto &item = this->add_tree_item<ui::BasicTreeViewItem>(RPT_("No image assets found"),
                                                                  ICON_INFO);
          item.disable_interaction();
          return;
        }

        /* 
         * Add "All" item - shows all image assets from all catalogs in the selected library.
         * This bypasses catalog filtering and displays every image asset regardless of catalog.
         */
        auto &all_item = this->add_tree_item<ui::BasicTreeViewItem>(IFACE_("All"));
        all_item.set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem &) {
          blender::ed::asset::shelf::settings_set_all_catalog_active(shelf_.settings);
          blender::ed::asset::shelf::send_redraw_notifier(C);
        });
        all_item.set_is_active_fn(
            [this]() { return blender::ed::asset::shelf::settings_is_all_catalog_active(shelf_.settings); });
        all_item.uncollapse_by_default();

        catalog_tree_.foreach_root_item([&, this](
                                            const asset_system::AssetCatalogTreeItem &catalog_item) {
          ui::BasicTreeViewItem &item = this->build_catalog_items_recursive(all_item, catalog_item);
          item.uncollapse_by_default();
        });
      }

     private:
      /*
       * Recursively build catalog tree items. Each item represents a catalog that contains
       * at least one image asset (as determined by build_filtered_catalog_tree).
       * 
       * When a catalog item is activated, it filters the asset view to show only
       * image assets from that specific catalog.
       */
      ui::BasicTreeViewItem &build_catalog_items_recursive(
          ui::TreeViewOrItem &parent_view_item,
          const asset_system::AssetCatalogTreeItem &catalog_item) const
      {
        ui::BasicTreeViewItem &view_item = parent_view_item.add_tree_item<ui::BasicTreeViewItem>(
            catalog_item.get_name());

        asset_system::AssetCatalogPath catalog_path = catalog_item.catalog_path();
        /* When catalog is activated, filter assets to show only those from this catalog */
        view_item.set_on_activate_fn([this, catalog_path](bContext &C, ui::BasicTreeViewItem &) {
          blender::ed::asset::shelf::settings_set_active_catalog(shelf_.settings, catalog_path);
          blender::ed::asset::shelf::send_redraw_notifier(C);
        });
        /* Check if this catalog is currently active/selected */
        view_item.set_is_active_fn([this, catalog_path]() {
          return blender::ed::asset::shelf::settings_is_active_catalog(shelf_.settings, catalog_path);
        });

        const int parent_count = view_item.count_parents() + 1;

        catalog_item.foreach_child([&, this](const asset_system::AssetCatalogTreeItem &child) {
          ui::BasicTreeViewItem &child_item = build_catalog_items_recursive(view_item, child);

          /* Uncollapse to some level (gives quick access, but don't let the tree get too big). */
          if (parent_count < 3) {
            child_item.uncollapse_by_default();
          }
        });
        
        return view_item;
      }
    };
    
    ui::AbstractTreeView *tree_view = UI_block_add_view(
        *block,
        "asset catalog tree view",
        std::make_unique<AssetCatalogTreeView>(*library, *browser_data.shelf));
    
    ui::TreeViewBuilder::build_tree_view(C, *tree_view, layout);
  } else {
    /* Fallback to simple catalog selector if library is not available */
    printf("DEBUG: Library not available, using fallback catalog selector\n");
    
    uiLayout *col = &layout.column(false);
    
    /* Category label */
    col->label(IFACE_("Category:"), ICON_NONE);
    
    /* All Images button */
    uiLayout *row = &col->row(true);
    uiBut *but = uiDefBut(row->block(),
             ButType::Toggle,
             0,
             IFACE_("All Images"),
             0, 0,
             UI_UNIT_X * 8, UI_UNIT_Y,
             nullptr,
             0.0f, 0.0f,
             std::optional<blender::StringRef>());
    
    if (but) {
      UI_but_flag_enable(but, UI_BUT_ACTIVE_DEFAULT);
    }
    
    if (!browser_data.show_all_catalogs) {
      col->label(IFACE_("Default Images"), ICON_NONE);
    }
  }
}

/**
 * Get the currently active image from the brush texture
 */
static Image *get_active_brush_image(const bContext &C)
{
  Scene *scene = CTX_data_scene(&C);
  if (!scene) {
    return nullptr;
  }
  
  wmWindow *window = CTX_wm_window(&C);
  ViewLayer *view_layer = WM_window_get_active_view_layer(window);
  Brush *brush = BKE_paint_brush(BKE_paint_get_active(scene, view_layer));
  
  if (!brush || !brush->mtex.tex) {
    return nullptr;
  }
  
  return brush->mtex.tex->ima;
}

/**
 * Custom grid view item for image assets
 */
class ImageAssetGridItem : public ui::PreviewGridItem {
  Image *image_;
  TemplateAssetCatalogImageBrowser &browser_data_;

 public:
  ImageAssetGridItem(Image *image, TemplateAssetCatalogImageBrowser &browser_data)
      : PreviewGridItem(image->id.name + 2, image->id.name + 2, ICON_IMAGE_DATA),
        image_(image),
        browser_data_(browser_data)
  {
    /* Set up active state checking */
    set_is_active_fn([this]() -> bool {
      if (!browser_data_.context) {
        return false;
      }
      
      Image *active_image = get_active_brush_image(*browser_data_.context);
      return active_image == image_;
    });
  }

  void build_grid_tile(const bContext &C, uiLayout &layout) const override
  {
    /* Use the standard PreviewGridItem approach - this will handle tooltip automatically */
    printf("DEBUG: Building grid tile for image '%s'\n", image_->id.name + 2);
    
    /* Set tooltip BEFORE creating the button, like PreviewGridItem does */
    UI_but_func_quick_tooltip_set(this->view_item_button(),
                                  [this](const uiBut * /*but*/) { 
                                    printf("DEBUG: Grid tooltip callback called for image '%s'\n", image_->id.name + 2);
                                    return image_->id.name + 2; 
                                  });
    
    /* Use the standard build_grid_tile_button with custom preview icon */
    this->build_grid_tile_button(layout, [&C, this]() -> BIFIconID {
      /* Get preview icon if available */
      int icon_id = ui_id_icon_get(&C, &image_->id, true);
      if (icon_id != ICON_NONE) {
        return icon_id;
      } else {
        /* Fallback to default image icon */
        return ICON_IMAGE_DATA;
      }
    }());
    
    printf("DEBUG: Grid tile built for image '%s'\n", image_->id.name + 2);
  }

  void on_activate(bContext &C) override
  {
    /* Apply image as texture to brush */
    template_asset_catalog_image_set_cb(&C, &browser_data_, image_);
    
    /* Close popover after selection */
    uiBut *but = reinterpret_cast<uiBut *>(this->view_item_button());
    if (but && but->block && but->block->handle) {
      but->block->handle->menuretval = UI_RETURN_OK;
    }
  }
};

/**
 * Custom grid view for image assets
 */
class ImageAssetGridView : public ui::AbstractGridView {
  TemplateAssetCatalogImageBrowser &browser_data_;
  Vector<Image *> filtered_images_;

 public:
  ImageAssetGridView(TemplateAssetCatalogImageBrowser &browser_data,
                     Vector<Image *> filtered_images)
      : browser_data_(browser_data), filtered_images_(filtered_images)
  {
    /* Get preview size from shelf settings */
    int preview_size = ASSET_SHELF_PREVIEW_SIZE_DEFAULT;
    if (browser_data_.shelf) {
      preview_size = browser_data_.shelf->settings.preview_size;
    }
    
    /* Set tile size based on preview size (preview + 1 unit for name) */
    this->set_tile_size(preview_size, preview_size + UI_UNIT_Y);
  }

  void build_items() override
  {
    for (Image *image : filtered_images_) {
      this->add_item<ImageAssetGridItem>(image, browser_data_);
    }
  }
};

/**
 * Draw image assets grid (right column) with fixed 6x3 layout
 */
static void draw_image_grid(uiLayout &layout,
                            const bContext &C,
                            TemplateAssetCatalogImageBrowser &browser_data)
{
  if (!browser_data.shelf) {
    return;
  }

  /* Use the asset shelf's asset view system instead of custom grid */
  blender::ed::asset::shelf::build_asset_view(layout, 
                                              browser_data.shelf->settings.asset_library_reference, 
                                              *browser_data.shelf, 
                                              C);
}

/**
 * Main popover panel draw function
 */
static void popover_panel_draw(const bContext *C, Panel *panel)
{
  const wmWindow *win = CTX_wm_window(C);
  const int layout_width_units = layout_width_units_clamped(win);
  
  TemplateAssetCatalogImageBrowser *browser_data = lookup_template_from_context(C);
  if (!browser_data) {
    return;
  }

  browser_data->context = C;

  /* Ensure images are initialized and converted to assets if needed */
  ensure_images_initialized(*C, *browser_data);

  uiLayout *layout = panel->layout;
  layout->ui_units_x_set(layout_width_units);
  
  /* Set minimum height for 3 rows */
  const int min_height = calculate_minimum_popover_height();
  layout->ui_units_y_set(min_height);
  
  /* Set template data in context for the operator */
  PointerRNA template_ptr;
  template_ptr.data = browser_data;
  template_ptr.type = nullptr;  /* No specific RNA type for our custom struct */
  template_ptr.owner_id = nullptr;
  layout->context_ptr_set("template_asset_catalog_image_browser", &template_ptr);

  /* Main layout with toggle button */
  uiLayout *main_row = &layout->row(false);
  
  /* Left column - catalog selector (only when visible) */
  if (browser_data->show_catalog_selector) {
    uiLayout *catalogs_col = &main_row->column(false);
    catalogs_col->ui_units_x_set(LEFT_COL_WIDTH_UNITS);
    catalogs_col->fixed_size_set(true);
    draw_catalog_selector(*catalogs_col, *C, *browser_data);
  }
  
  /* Toggle button - spans full height */
  uiLayout *toggle_col = &main_row->column(false);
  toggle_col->ui_units_x_set(2); /* Wider column for toggle button */
  toggle_col->fixed_size_set(true);
  
  /* Set appropriate icon based on state */
  int icon_id = browser_data->show_catalog_selector ? ICON_TRIA_LEFT : ICON_TRIA_RIGHT ;
  
  printf("DEBUG: Creating toggle button with icon_id=%d, show_catalog_selector=%s\n", 
         icon_id, browser_data->show_catalog_selector ? "true" : "false");
  
  /* Create button using layout API with proper sizing */
  uiLayout *toggle_row = &toggle_col->row(false);
  toggle_row->ui_units_y_set(layout_width_units); /* Full height */
  
  /* Create button using layout API */
  uiBut *toggle_but = toggle_row->button(
    "", /* No text, only icon */
    icon_id,
    [browser_data](bContext &C) {
      printf("DEBUG: Button pressed via lambda\n");
      
      /* Toggle catalog selector visibility */
      browser_data->show_catalog_selector = !browser_data->show_catalog_selector;
      
      printf("DEBUG: Toggled show_catalog_selector to %s\n", 
             browser_data->show_catalog_selector ? "true" : "false");
      
      /* Update UI */
      WM_event_add_notifier(&C, NC_WM | ND_DATACHANGED, nullptr);
    });
  
  if (toggle_but) {
    printf("DEBUG: Toggle button created successfully\n");
    
    /* Style the button */
    UI_but_flag_enable(toggle_but, UI_BUT_ACTIVE_DEFAULT);
  } else {
    printf("DEBUG: Failed to create toggle button\n");
  }

  /* Right column - image grid */
  uiLayout *right_col = &main_row->column(false);
  
  /* Calculate width based on catalog visibility */
  int left_col_width = browser_data->show_catalog_selector ? LEFT_COL_WIDTH_UNITS : 0;
  int right_col_width = layout_width_units - left_col_width - 2; /* -2 for toggle button */
  
  /* Search and preview size row - same as asset shelf */
  uiLayout *search_size_row = &right_col->row(false);
  
  /* Search field - takes most of the width */
  uiLayout *search_col = &search_size_row->column(false);
  search_col->ui_units_x_set(right_col_width - 8); /* Reserve 8 units for size controls */
  
  /* Create a simple text input for search */
  uiBut *search_but = uiDefBut(search_col->block(),
                               ButType::Text,
                               0,
                               "",
                               0, 0,
                               UI_UNIT_X * 20, UI_UNIT_Y,
                               browser_data->search_filter,
                               0, 64,
                               "");
  
  if (search_but) {
    ui_def_but_icon(search_but, ICON_VIEWZOOM, UI_HAS_ICON);
    UI_but_flag_enable(search_but, UI_BUT_ACTIVE_DEFAULT);
    /* Search will be handled by the text input directly */
  }
  
  /* Size controls - fixed width on the right */
  uiLayout *size_col = &search_size_row->column(false);
  size_col->ui_units_x_set(8); /* Fixed width for size controls */
  size_col->fixed_size_set(true);
  
  /* Add preview size controls */
  uiLayout *size_row = &size_col->row(false);
  size_row->label(IFACE_("Size:"), ICON_NONE);
  
  /* Create RNA pointer for shelf settings */
  bScreen *screen = CTX_wm_screen(C);
  PointerRNA shelf_ptr = RNA_pointer_create_discrete(&screen->id, &RNA_AssetShelf, browser_data->shelf);
  
  /* Add preview size slider with update callback */
  size_row->prop(&shelf_ptr, "preview_size", UI_ITEM_NONE, "", ICON_NONE);
  
  /* Add update callback for preview size changes - force popover rebuild */
  uiBut *size_but = size_row->block()->buttons.last().get();
  if (size_but) {
    UI_but_func_set(size_but, [](bContext *C, void *arg_template, void * /*arg_unused*/) {
      (void)arg_template; /* Suppress unused parameter warning */
      
      /* Force popover panel to update with size recalculation */
      ARegion *region = CTX_wm_region(C);
      if (region) {
        uiBlock *block = static_cast<uiBlock *>(region->runtime->uiblocks.first);
        if (block && block->handle) {
          uiPopupBlockHandle *popup = block->handle;
          
          /* 1. Reset previous block rect for size recalculation */
          memset(&popup->prev_block_rect, 0, sizeof(popup->prev_block_rect));
          
          /* 2. Set bounds type for correct positioning */
          block->bounds_type = UI_BLOCK_BOUNDS_POPUP_MOUSE;
          
          /* 3. Request popup update */
          popup->menuretval = UI_RETURN_UPDATE;
          
          /* 4. Force immediate redraw of popup region */
          if (popup->region) {
            ED_region_tag_redraw(popup->region);
          }
        }
        
        /* 5. Also redraw main region */
        ED_region_tag_redraw(region);
      }
    }, browser_data, nullptr);
  }
  
  uiLayout *asset_view_col = &right_col->column(false);
  
  asset_view_col->ui_units_x_set(right_col_width);
  asset_view_col->fixed_size_set(true);
  
  draw_image_grid(*asset_view_col, *C, *browser_data);
}

/**
 * Popover panel poll function
 */
static bool popover_panel_poll(const bContext *C, PanelType * /*panel_type*/)
{
  return lookup_template_from_context(C) != nullptr;
}

/**
 * Register the popover panel type and operator
 */
static void popover_panel_register()
{
  /* Check if already registered */
  if (WM_paneltype_find("ASSETCATALOG_PT_image_browser_popover", true)) {
    return;
  }

  /* No need for complex RNA registration for simple search */

  /* Register the image asset activation operator */
  WM_operatortype_append(IMAGE_ASSET_OT_activate);

  PanelType *pt = MEM_new<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "ASSETCATALOG_PT_image_browser_popover");
  STRNCPY_UTF8(pt->label, N_("Asset Catalog Image Browser"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Display asset catalog image browser in a popover panel");
  pt->draw = popover_panel_draw;
  pt->poll = popover_panel_poll;
  /* Offset positioning */
  pt->offset_units_xy.x = -(LEFT_COL_WIDTH_UNITS + 1.5f);
  pt->offset_units_xy.y = 8.0f;
  
  WM_paneltype_add(pt);
}

/**
 * Main template function - creates popover button with preview
 */
void template_asset_catalog_image_browser(uiLayout &layout,
                                         const bContext &C,
                                         PointerRNA *ptr,
                                         const StringRefNull propname,
                                         int rows,
                                         int cols,
                                         bool auto_convert)
{
  printf("DEBUG: template_asset_catalog_image_browser called with propname='%s'\n", propname.c_str());
  
  if (!ptr || propname.is_empty()) {
    printf("DEBUG: Invalid parameters - ptr=%p, propname='%s'\n", ptr, propname.c_str());
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (!prop) {
    printf("DEBUG: Property not found: %s\n", propname.c_str());
    RNA_warning("Property not found: %s", propname.c_str());
    return;
  }

  printf("DEBUG: Property found, registering popover panel\n");
  
  /* Ensure popover panel is registered */
  popover_panel_register();

  /* Create or get template data */
  Vector<TemplateAssetCatalogImageBrowser *> &browsers = StaticPopupImageBrowsers::browsers();
  
  TemplateAssetCatalogImageBrowser *browser_data = nullptr;
  if (browsers.is_empty()) {
    printf("DEBUG: Creating new browser data\n");
    browser_data = MEM_new<TemplateAssetCatalogImageBrowser>(__func__);
    browsers.append(browser_data);
  } else {
    printf("DEBUG: Using existing browser data\n");
    browser_data = browsers[0];
  }
  
  browser_data->ptr = *ptr;
  browser_data->prop = prop;
  browser_data->preview_rows = rows > 0 ? rows : 4;
  browser_data->preview_cols = cols > 0 ? cols : 6;
  browser_data->auto_convert_to_assets = auto_convert;
  browser_data->show_local_images = true;
  browser_data->show_all_catalogs = true;

  printf("DEBUG: Creating popover button with preview\n");
  
  /* Get current property value to show preview */
  PointerRNA current_ptr = RNA_property_pointer_get(ptr, prop);
  ID *current_id = static_cast<ID *>(current_ptr.data);
  
  /* Create popover button with preview */
  const ARegion *region = CTX_wm_region(&C);
  uiLayout *row = &layout.row(true);
  const bool use_big_size = !RGN_TYPE_IS_HEADER_ANY(region->regiontype);
  
  if (use_big_size) {
    /* Use same size as uiTemplateIDPreview: 6x6 UI units */
    row->scale_x_set(6.0f);
    row->scale_y_set(6.0f);
  }
  else {
    row->ui_units_x_set(7.0f);
  }

  printf("DEBUG: Creating popover with panel 'ASSETCATALOG_PT_image_browser_popover'\n");
  
  /* Create the popover */
  row->popover(&C, "ASSETCATALOG_PT_image_browser_popover", "", ICON_IMAGE_DATA);
  
  /* Get the button and configure it with preview */
  uiBlock *block = layout.block();
  uiBut *but = block->buttons.last().get();
  if (but) {
    if (use_big_size) {
      /* Use preview icon if we have a current image/texture */
      if (current_id) {
        int icon_id = ui_id_icon_get(&C, current_id, true);
        ui_def_but_icon(but, icon_id, UI_HAS_ICON | UI_BUT_ICON_PREVIEW);
      } else {
        ui_def_but_icon(but, ICON_IMAGE_DATA, UI_HAS_ICON | UI_BUT_ICON_PREVIEW);
      }
    } else {
      ui_def_but_icon(but, ICON_IMAGE_DATA, UI_HAS_ICON);
    }
    UI_but_menu_disable_hover_open(but);
    
    /* Add tooltip with information about current image or general functionality */
    printf("DEBUG: Setting tooltip for popover button\n");
    UI_but_func_quick_tooltip_set(but, [current_id](const uiBut * /*but*/) {
      printf("DEBUG: Popover tooltip callback called\n");
      if (current_id && current_id->name) {
        printf("DEBUG: Popover tooltip - current image: '%s'\n", current_id->name + 2);
        return std::string("Current: ") + (current_id->name + 2);
      } else {
        printf("DEBUG: Popover tooltip - no current image\n");
        return std::string("Browse Image Assets");
      }
    });
    printf("DEBUG: Popover tooltip set\n");
  }
  
  printf("DEBUG: template_asset_catalog_image_browser completed successfully\n");
}

}  // namespace blender::ui

using namespace blender;

void uiTemplateAssetCatalogImageBrowser(uiLayout *layout,
                                       bContext *C,
                                       PointerRNA *ptr,
                                       const char *propname,
                                       int rows,
                                       int cols,
                                       bool auto_convert)
{
  ui::template_asset_catalog_image_browser(*layout, *C, ptr, propname, rows, cols, auto_convert);
}