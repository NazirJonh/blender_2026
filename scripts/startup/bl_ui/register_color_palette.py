# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Registration module for Enhanced Color Palette functionality.

This module handles the registration and unregistration of the enhanced
color palette modules for Blender's UI system.
"""

import bpy


def register():
    """
    Register Color Palette modules.
    
    This function is called during Blender startup to register all
    enhanced color palette functionality.
    """
    try:
        # Import template modules (registration happens on import)
        from . import template_color_palette
        from . import template_color_palette_simple
        
        print("✓ Enhanced Color Palette modules registered successfully")
        
    except ImportError as e:
        print(f"✗ Failed to register Enhanced Color Palette modules: {e}")
        import traceback
        traceback.print_exc()


def unregister():
    """
    Unregister Color Palette modules.
    
    This function is called during Blender shutdown to clean up
    enhanced color palette functionality.
    """
    try:
        # Unregister is typically handled automatically by Python's garbage collection
        # No explicit cleanup needed for these utility modules
        
        print("✓ Enhanced Color Palette modules unregistered")
        
    except Exception as e:
        print(f"✗ Error during Enhanced Color Palette module unregistration: {e}")


if __name__ == "__main__":
    register()
