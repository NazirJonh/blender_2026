import bpy

class ASSETCATALOG_PT_test_panel(bpy.types.Panel):
    bl_label = "Asset Catalog Test"
    bl_idname = "ASSETCATALOG_PT_test_panel"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Test"

    def draw(self, context):
        layout = self.layout
        
        # Используем context.scene как data и "name" как property
        layout.template_asset_catalog_image_browser(
            data=context.scene,
            property="name",
            rows=3,
            cols=4,
            auto_convert=True
        )
        
        layout.label(text="Test Info:")
        layout.label(text=f"Scene: {context.scene.name}")

def register():
    bpy.utils.register_class(ASSETCATALOG_PT_test_panel)
    print("Popover template test panel registered successfully!")

def unregister():
    bpy.utils.unregister_class(ASSETCATALOG_PT_test_panel)

if __name__ == "__main__":
    register()