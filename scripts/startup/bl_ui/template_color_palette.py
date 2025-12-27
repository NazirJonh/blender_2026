# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Enhanced Color Palette Template Module

This module provides helper functions and utilities for the enhanced color palette
template (template_colorpicker_palette). It includes functions for palette management,
color operations, and integration with paint modes.
"""

import bpy
from bpy.types import UILayout, Context
from typing import Optional


def template_color_palette_enhanced(layout: UILayout, context: Context) -> None:
    """
    Enhanced color palette with context menu support and visual indicators.
    
    This function wraps the C++ template_colorpicker_palette function and provides
    proper context handling for paint modes.
    
    Args:
        layout: UILayout - Blender layout object where palette will be drawn
        context: Context - Current Blender context (used to get active paint settings)
    
    Returns:
        None - Directly modifies layout
        
    Example:
        >>> def draw(self, context):
        ...     layout = self.layout
        ...     template_color_palette_enhanced(layout, context)
    """
    settings = _get_paint_settings(context)
    
    if settings and settings.palette:
        try:
            # Call the C++ enhanced template function
            layout.template_colorpicker_palette(settings, "palette")
        except AttributeError:
            # Fallback to standard template if enhanced version not available
            layout.label(text="Enhanced palette not available", icon='ERROR')
            layout.template_palette(settings, "palette", color=True)
    else:
        # Show message when no palette is active
        col = layout.column(align=True)
        col.label(text="No active palette", icon='INFO')
        col.operator("palette.new", text="New Palette", icon='ADD')


def get_active_palette(context: Context) -> Optional[bpy.types.Palette]:
    """
    Get currently active palette from context.
    
    Args:
        context: Context - Current Blender context
        
    Returns:
        Palette or None - Active palette if found, None otherwise
        
    Example:
        >>> palette = get_active_palette(context)
        >>> if palette:
        ...     print(f"Active palette: {palette.name}")
    """
    settings = _get_paint_settings(context)
    if settings:
        return settings.palette
    return None


def set_palette_color(palette: bpy.types.Palette, color_index: int, color_value: tuple) -> bool:
    """
    Set color in palette by index.
    
    Args:
        palette: Palette - Target palette
        color_index: int - Index of color to modify (0-based)
        color_value: tuple - RGB color tuple (r, g, b) with values 0.0-1.0
        
    Returns:
        bool - True if successful, False otherwise
        
    Example:
        >>> palette = bpy.data.palettes["MyPalette"]
        >>> set_palette_color(palette, 0, (1.0, 0.0, 0.0))  # Set first color to red
    """
    if not palette or color_index < 0:
        return False
    
    if color_index >= len(palette.colors):
        return False
    
    try:
        color = palette.colors[color_index]
        color.color = color_value[:3]  # Only RGB, no alpha
        return True
    except (IndexError, AttributeError):
        return False


def add_palette_color(palette: bpy.types.Palette, color_value: tuple) -> Optional[bpy.types.PaletteColor]:
    """
    Add new color to palette.
    
    Args:
        palette: Palette - Target palette
        color_value: tuple - RGB color tuple (r, g, b) with values 0.0-1.0
        
    Returns:
        PaletteColor or None - Newly created color if successful, None otherwise
        
    Example:
        >>> palette = bpy.data.palettes["MyPalette"]
        >>> new_color = add_palette_color(palette, (0.5, 0.5, 1.0))
        >>> if new_color:
        ...     print(f"Added color: {new_color.color}")
    """
    if not palette:
        return None
    
    try:
        new_color = palette.colors.new()
        new_color.color = color_value[:3]  # Only RGB, no alpha
        return new_color
    except AttributeError:
        return None


def get_brush_color(context: Context) -> Optional[tuple]:
    """
    Get current brush color from active paint mode.
    
    Args:
        context: Context - Current Blender context
        
    Returns:
        tuple or None - RGB color tuple (r, g, b) if found, None otherwise
        
    Example:
        >>> color = get_brush_color(context)
        >>> if color:
        ...     print(f"Brush color: R={color[0]:.3f} G={color[1]:.3f} B={color[2]:.3f}")
    """
    settings = _get_paint_settings(context)
    if settings and settings.brush:
        return tuple(settings.brush.color[:3])
    return None


def set_brush_color(context: Context, color_value: tuple) -> bool:
    """
    Set brush color in active paint mode.
    
    Args:
        context: Context - Current Blender context
        color_value: tuple - RGB color tuple (r, g, b) with values 0.0-1.0
        
    Returns:
        bool - True if successful, False otherwise
        
    Example:
        >>> set_brush_color(context, (1.0, 0.0, 0.0))  # Set brush to red
    """
    settings = _get_paint_settings(context)
    if settings and settings.brush:
        try:
            settings.brush.color = color_value[:3]
            return True
        except (AttributeError, TypeError):
            return False
    return False


def _get_paint_settings(context: Context):
    """
    Internal helper to get paint settings from context.
    
    Args:
        context: Context - Current Blender context
        
    Returns:
        Paint settings or None
    """
    # Try to get paint mode from context
    mode = context.mode
    tool_settings = context.scene.tool_settings
    
    if mode == 'PAINT_TEXTURE':
        return tool_settings.image_paint
    elif mode == 'SCULPT':
        return tool_settings.sculpt
    elif mode == 'PAINT_VERTEX':
        return tool_settings.vertex_paint
    elif mode == 'PAINT_WEIGHT':
        return tool_settings.weight_paint
    
    return None


# Export public API
__all__ = [
    'template_color_palette_enhanced',
    'get_active_palette',
    'set_palette_color',
    'add_palette_color',
    'get_brush_color',
    'set_brush_color',
]
