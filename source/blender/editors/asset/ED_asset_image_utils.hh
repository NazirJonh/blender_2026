/* SPDX-FileCopyrightText: 2025 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include "BLI_vector.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"

struct bContext;
struct Image;
struct Main;

/**
 * Ensure the given image is marked as an asset.
 * If not already an asset, marks it and generates preview.
 * \return true if successful or already an asset.
 */
bool ensure_image_is_asset(const bContext *C, Image *image);

/**
 * Assign the image to a default catalog ("Images").
 * Creates the catalog if it doesn't exist.
 */
void assign_default_catalog(const bContext *C, Image *image);

/**
 * Convert all valid images in bmain to assets.
 * Skips generated and viewer images.
 */
void ensure_all_images_are_assets(const bContext *C, Main *bmain);

/**
 * Get all images that are marked as assets.
 * \return Vector of image assets.
 */
blender::Vector<Image *> get_all_image_assets(Main *bmain);

/**
 * Get all images assigned to a specific catalog.
 * \param catalog_id: The catalog UUID to filter by.
 * \return Vector of images in the catalog.
 */
blender::Vector<Image *> get_images_in_catalog(Main *bmain, const blender::asset_system::CatalogID &catalog_id);

/**
 * Get all images that are not yet marked as assets.
 * \return Vector of images that can be converted to assets.
 */
blender::Vector<Image *> get_non_asset_images(Main *bmain);

/**
 * Check if an image can be converted to an asset.
 * \param image: The image to check.
 * \return true if the image can be converted to an asset.
 */
bool can_convert_to_asset(Image *image);

/**
 * Automatically convert all valid images to assets.
 * \param C: The context.
 * \param bmain: The main database.
 * \return Number of images converted to assets.
 */
int auto_convert_images_to_assets(const bContext *C, Main *bmain);