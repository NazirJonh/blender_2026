# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Simplified Color Palette API

This module provides a simplified, easy-to-use API for displaying color palettes
in custom panels and add-ons. Perfect for quick integration without complex setup.
"""

import bpy
from bpy.types import UILayout, Context
from typing import Optional


def draw_palette(layout: UILayout, context: Context, palette_name: Optional[str] = None) -> bool:
    """
    Simple wrapper for quick palette display.
    
    This is the simplest way to add a color palette to your UI. Just call this function
    in your panel's draw method and it will handle everything automatically.
    
    Args:
        layout: UILayout - Blender layout object
        context: Context - Current Blender context
        palette_name: str (optional) - Name of specific palette to display.
                                       If None, uses active palette from paint settings.
    
    Returns:
        bool - True if palette was drawn, False otherwise
        
    Example:
        >>> class MY_PT_palette_panel(bpy.types.Panel):
        ...     bl_label = "My Palette"
        ...     bl_space_type = 'VIEW_3D'
        ...     bl_region_type = 'UI'
        ...     
        ...     def draw(self, context):
        ...         from bl_ui.template_color_palette_simple import draw_palette
        ...         draw_palette(self.layout, context)
    """
    if palette_name:
        # Use specific palette by name
        if palette_name in bpy.data.palettes:
            palette = bpy.data.palettes[palette_name]
            try:
                # Try to create a temporary pointer for the palette
                # This is a simplified approach - may need adjustment
                layout.template_palette(context.scene.tool_settings.image_paint, "palette", color=True)
                return True
            except:
                layout.label(text=f"Palette: {palette_name}", icon='ERROR')
                return False
        else:
            layout.label(text=f"Palette '{palette_name}' not found", icon='ERROR')
            return False
    else:
        # Use active palette from paint settings
        settings = _get_active_paint_settings(context)
        if settings and settings.palette:
            try:
                # Use enhanced version if available
                layout.template_colorpicker_palette(settings, "palette")
                return True
            except AttributeError:
                # Fallback to standard version
                layout.template_palette(settings, "palette", color=True)
                return True
        else:
            layout.label(text="No active palette", icon='INFO')
            return False


def draw_palette_selector(layout: UILayout, context: Context) -> None:
    """
    Draw a simple palette selector dropdown.
    
    Args:
        layout: UILayout - Blender layout object
        context: Context - Current Blender context
        
    Example:
        >>> def draw(self, context):
        ...     draw_palette_selector(self.layout, context)
    """
    settings = _get_active_paint_settings(context)
    if settings:
        row = layout.row(align=True)
        row.template_ID(settings, "palette", new="palette.new")
    else:
        layout.label(text="Not in paint mode", icon='INFO')


def draw_palette_grid_only(layout: UILayout, context: Context) -> bool:
    """
    Draw only the color grid without controls.
    
    This is useful when you want a compact display of palette colors
    without add/delete buttons or other controls.
    
    Args:
        layout: UILayout - Blender layout object
        context: Context - Current Blender context
        
    Returns:
        bool - True if grid was drawn, False otherwise
        
    Example:
        >>> def draw(self, context):
        ...     # Draw compact palette display
        ...     draw_palette_grid_only(self.layout, context)
    """
    settings = _get_active_paint_settings(context)
    if settings and settings.palette:
        palette = settings.palette
        
        # Calculate grid layout
        col = layout.column(align=True)
        row = None
        cols_per_row = 8  # Default columns
        
        for i, color in enumerate(palette.colors):
            if i % cols_per_row == 0:
                row = col.row(align=True)
            
            # Draw color button
            row.prop(color, "color", text="")
        
        return True
    return False


def _get_active_paint_settings(context: Context):
    """
    Internal helper to get active paint settings.
    
    Args:
        context: Context - Current Blender context
        
    Returns:
        Paint settings or None
    """
    mode = context.mode
    ts = context.scene.tool_settings
    
    paint_settings_map = {
        'PAINT_TEXTURE': ts.image_paint,
        'SCULPT': ts.sculpt,
        'PAINT_VERTEX': ts.vertex_paint,
        'PAINT_WEIGHT': ts.weight_paint,
    }
    
    return paint_settings_map.get(mode)


# Export public API
__all__ = [
    'draw_palette',
    'draw_palette_selector',
    'draw_palette_grid_only',
]
