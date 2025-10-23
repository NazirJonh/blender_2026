/* SPDX-FileCopyrightText: 2025 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 * Utilities for managing Image data-blocks as assets.
 */

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_preview_image.hh"

#include "DNA_image_types.h"

#include "ED_asset_image_utils.hh"
#include "ED_asset_mark_clear.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"

using namespace blender::asset_system;

static const char *DEFAULT_IMAGE_CATALOG_PATH = "Images";

bool ensure_image_is_asset(const bContext *C, Image *image)
{
  if (!image) {
    return false;
  }

  /* Check if already an asset */
  if (image->id.asset_data) {
    return true;
  }

  /* Mark as asset */
  if (!blender::ed::asset::mark_id(&image->id)) {
    return false;
  }

  /* Ensure preview exists */
  BKE_previewimg_id_ensure(&image->id);

  /* Assign to default catalog if no catalog assigned */
  if (BLI_uuid_is_nil(image->id.asset_data->catalog_id)) {
    assign_default_catalog(C, image);
  }

  return true;
}

void assign_default_catalog([[maybe_unused]] const bContext *C, Image *image)
{
  if (!image || !image->id.asset_data) {
    return;
  }

  Main *bmain = CTX_data_main(C);
  
  /* Get current file asset library */
  AssetLibrary *library = AS_asset_library_load(
      bmain, current_file_library_reference());
  if (!library) {
    return;
  }

  AssetCatalogService &catalog_service = library->catalog_service();
  AssetCatalog *catalog = catalog_service.find_catalog_by_path(
      AssetCatalogPath(DEFAULT_IMAGE_CATALOG_PATH));

  if (!catalog) {
    /* Create default catalog */
    catalog = catalog_service.create_catalog(AssetCatalogPath(DEFAULT_IMAGE_CATALOG_PATH));
  }

  if (catalog) {
    image->id.asset_data->catalog_id = catalog->catalog_id;
  }
}

void ensure_all_images_are_assets([[maybe_unused]] const bContext *C, Main *bmain)
{
  for (Image *image = static_cast<Image *>(bmain->images.first); image; image = static_cast<Image *>(image->id.next)) {
    /* Skip built-in and temporary images */
    if (image->source == IMA_SRC_VIEWER || image->source == IMA_SRC_GENERATED) {
      continue;
    }
    
    ensure_image_is_asset(C, image);
  }
}

blender::Vector<Image *> get_images_in_catalog(Main *bmain, const CatalogID &catalog_id)
{
  blender::Vector<Image *> images;
  
  for (Image *image = static_cast<Image *>(bmain->images.first); image; image = static_cast<Image *>(image->id.next)) {
    if (!image->id.asset_data) {
      continue;
    }
    
    if (BLI_uuid_equal(image->id.asset_data->catalog_id, catalog_id)) {
      images.append(image);
    }
  }
  
  return images;
}

blender::Vector<Image *> get_all_image_assets(Main *bmain)
{
  blender::Vector<Image *> images;
  
  for (Image *image = static_cast<Image *>(bmain->images.first); image; image = static_cast<Image *>(image->id.next)) {
    if (image->id.asset_data) {
      images.append(image);
    }
  }
  
  return images;
}

blender::Vector<Image *> get_non_asset_images(Main *bmain)
{
  blender::Vector<Image *> non_assets;
  
  for (Image *image = static_cast<Image *>(bmain->images.first); 
       image; 
       image = static_cast<Image *>(image->id.next)) {
    
    /* Skip if already an asset */
    if (image->id.asset_data) {
      continue;
    }
    
    /* Check if can be converted */
    if (can_convert_to_asset(image)) {
      non_assets.append(image);
    }
  }
  
  return non_assets;
}

bool can_convert_to_asset(Image *image)
{
  if (!image) {
    return false;
  }
  
  /* Skip built-in and temporary images */
  if (image->source == IMA_SRC_VIEWER || image->source == IMA_SRC_GENERATED) {
    return false;
  }
  
  /* Must have valid source (file or packed data) */
  if (image->source == IMA_SRC_FILE && !image->filepath[0]) {
    return false;
  }
  
  /* Skip if already an asset */
  if (image->id.asset_data) {
    return false;
  }
  
  return true;
}

int auto_convert_images_to_assets(const bContext *C, Main *bmain)
{
  if (!C || !bmain) {
    return 0;
  }
  
  blender::Vector<Image *> non_assets = get_non_asset_images(bmain);
  int converted_count = 0;
  
  for (Image *image : non_assets) {
    if (ensure_image_is_asset(C, image)) {
      converted_count++;
    }
  }
  
  return converted_count;
}