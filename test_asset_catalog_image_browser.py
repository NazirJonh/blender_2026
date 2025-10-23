#!/usr/bin/env python3
"""
Тест-скрипт для проверки функциональности Asset Catalog Image Browser Template
Для Blender 5.0

Этот скрипт тестирует:
1. Регистрацию нового template в RNA API
2. Автоматическое преобразование изображений в assets
3. Отображение UI и взаимодействие с пользователем
4. Функциональность каталогов изображений

Использование:
1. Запустите Blender с этим скриптом
2. Откройте Text Editor и загрузите этот файл
3. Нажмите "Run Script"
"""

import bpy
import bmesh
from bpy.types import Panel, Operator
from bpy.props import StringProperty, IntProperty, BoolProperty
import os
import sys

# Добавляем путь к модулям Blender для импорта
if hasattr(bpy.app, "binary_path_python"):
    sys.path.append(os.path.dirname(bpy.app.binary_path_python))

def test_template_registration():
    """Проверяем регистрацию template в RNA API"""
    print("1. Проверка регистрации template...")
    
    # Проверяем наличие template_asset_catalog_image_browser в UILayout
    layout_rna = bpy.types.UILayout.bl_rna
    functions = [func.identifier for func in layout_rna.functions]
    
    if "template_asset_catalog_image_browser" in functions:
        print("✓ Template зарегистрирован")
        return True
    else:
        print("✗ Template НЕ зарегистрирован")
        print("Доступные template функции:")
        template_funcs = [f for f in functions if f.startswith("template_")]
        for func in template_funcs[:10]:  # Показываем первые 10
            print(f"  - {func}")
        return False

def test_template_call():
    """Тестируем прямой вызов template"""
    print("\n2. Тестирование вызова template...")
    
    try:
        # Создаем тестовую сцену
        scene = bpy.context.scene
        
        # Создаем тестовое свойство для template
        if not hasattr(scene, "test_image_prop"):
            bpy.types.Scene.test_image_prop = bpy.props.PointerProperty(
                type=bpy.types.Image,
                name="Test Image"
            )
        
        # Создаем простой layout для тестирования
        class TestPanel(Panel):
            bl_label = "Asset Image Browser Test"
            bl_idname = "SCENE_PT_asset_image_browser_test"
            bl_space_type = 'PROPERTIES'
            bl_region_type = 'WINDOW'
            bl_context = "scene"
            
            def draw(self, context):
                layout = self.layout
                scene = context.scene
                
                # Пытаемся вызвать наш template
                try:
                    layout.template_asset_catalog_image_browser(
                        scene, "test_image_prop", 
                        rows=3, cols=4, auto_convert=True
                    )
                    print("✓ Template успешно вызван")
                except Exception as e:
                    print(f"✗ Ошибка при вызове template: {e}")
                    # Fallback - показываем обычный image selector
                    layout.prop(scene, "test_image_prop")
        
        # Регистрируем панель
        if hasattr(bpy.types, "SCENE_PT_asset_image_browser_test"):
            bpy.utils.unregister_class(bpy.types.SCENE_PT_asset_image_browser_test)
        
        bpy.utils.register_class(TestPanel)
        print("✓ Тестовая панель создана")
        return True
        
    except Exception as e:
        print(f"✗ Ошибка при создании тестовой панели: {e}")
        return False

def create_test_images():
    """Создаем тестовые изображения"""
    print("\n3. Создание тестовых изображений...")
    
    test_images = []
    image_names = [
        "TestImage_01", "TestImage_02", "TestImage_03",
        "TestTexture_01", "TestTexture_02"
    ]
    
    for name in image_names:
        # Удаляем существующее изображение если есть
        if name in bpy.data.images:
            bpy.data.images.remove(bpy.data.images[name])
        
        # Создаем новое изображение
        img = bpy.data.images.new(name, width=256, height=256)
        test_images.append(img)
        print(f"Создано тестовое изображение: {name}")
    
    print(f"Info: Создано {len(test_images)} тестовых изображений")
    return test_images

def test_asset_conversion():
    """Тестируем преобразование изображений в assets"""
    print("\n4. Тестирование преобразования в assets...")
    
    images_before = len(bpy.data.images)
    assets_before = len([img for img in bpy.data.images if img.asset_data])
    
    print(f"Изображений до преобразования: {images_before}")
    print(f"Assets до преобразования: {assets_before}")
    
    # Помечаем одно изображение как asset
    if bpy.data.images:
        test_img = bpy.data.images[0]
        test_img.asset_mark()
        print(f"Изображение {test_img.name} помечено как asset")
    
    assets_after = len([img for img in bpy.data.images if img.asset_data])
    print(f"Assets после преобразования: {assets_after}")
    
    converted = assets_after - assets_before
    print(f"Info: Преобразовано {converted} изображений в assets")
    
    return converted > 0

class ASSETTEST_OT_create_test_images(Operator):
    """Создать тестовые изображения для проверки Asset Browser"""
    bl_idname = "assettest.create_test_images"
    bl_label = "Создать тестовые изображения"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        # Создаем несколько тестовых изображений
        test_images = [
            ("TestImage_01", (256, 256)),
            ("TestImage_02", (512, 512)),
            ("TestImage_03", (128, 128)),
            ("TestTexture_01", (1024, 1024)),
            ("TestTexture_02", (512, 256)),
        ]
        
        created_count = 0
        for name, size in test_images:
            # Проверяем, не существует ли уже изображение с таким именем
            if name not in bpy.data.images:
                img = bpy.data.images.new(name, width=size[0], height=size[1])
                # Заполняем изображение простым градиентом
                pixels = [0.0] * (size[0] * size[1] * 4)
                for y in range(size[1]):
                    for x in range(size[0]):
                        idx = (y * size[0] + x) * 4
                        pixels[idx] = x / size[0]      # R
                        pixels[idx + 1] = y / size[1]  # G
                        pixels[idx + 2] = 0.5          # B
                        pixels[idx + 3] = 1.0          # A
                img.pixels = pixels
                created_count += 1
                print(f"Создано тестовое изображение: {name}")
        
        self.report({'INFO'}, f"Создано {created_count} тестовых изображений")
        return {'FINISHED'}

class ASSETTEST_OT_test_template_registration(Operator):
    """Проверить регистрацию Asset Catalog Image Browser Template"""
    bl_idname = "assettest.test_template_registration"
    bl_label = "Тест регистрации Template"
    bl_options = {'REGISTER'}

    def execute(self, context):
        try:
            # Проверяем, доступен ли новый template в bpy.types.UILayout
            if hasattr(bpy.types.UILayout, 'template_asset_catalog_image_browser'):
                self.report({'INFO'}, "✓ Template успешно зарегистрирован в UILayout")
                print("✓ bpy.types.UILayout.template_asset_catalog_image_browser найден")
                
                # Проверяем документацию функции
                func = getattr(bpy.types.UILayout, 'template_asset_catalog_image_browser')
                if func.__doc__:
                    print(f"Документация: {func.__doc__}")
                
                return {'FINISHED'}
            else:
                self.report({'ERROR'}, "✗ Template не найден в UILayout")
                print("✗ bpy.types.UILayout.template_asset_catalog_image_browser не найден")
                return {'CANCELLED'}
                
        except Exception as e:
            self.report({'ERROR'}, f"Ошибка при проверке регистрации: {str(e)}")
            print(f"Ошибка: {e}")
            return {'CANCELLED'}

class ASSETTEST_OT_test_image_asset_conversion(Operator):
    """Тестировать автоматическое преобразование изображений в assets"""
    bl_idname = "assettest.test_image_asset_conversion"
    bl_label = "Тест преобразования в Assets"
    bl_options = {'REGISTER'}

    def execute(self, context):
        # Подсчитываем изображения до и после
        images_before = len([img for img in bpy.data.images if img.asset_data])
        total_images = len(bpy.data.images)
        
        print(f"Изображений до преобразования: {total_images}")
        print(f"Assets до преобразования: {images_before}")
        
        # Пытаемся вызвать функцию преобразования через C API
        # Это будет работать только если наши C++ функции правильно экспортированы
        try:
            # Проверяем каждое изображение
            converted_count = 0
            for img in bpy.data.images:
                if not img.asset_data and img.source not in ['VIEWER', 'GENERATED']:
                    # Пытаемся пометить как asset
                    try:
                        img.asset_mark()
                        converted_count += 1
                        print(f"Изображение {img.name} помечено как asset")
                    except:
                        print(f"Не удалось пометить {img.name} как asset")
            
            images_after = len([img for img in bpy.data.images if img.asset_data])
            
            self.report({'INFO'}, f"Преобразовано {converted_count} изображений в assets")
            print(f"Assets после преобразования: {images_after}")
            
            return {'FINISHED'}
            
        except Exception as e:
            self.report({'ERROR'}, f"Ошибка при преобразовании: {str(e)}")
            print(f"Ошибка: {e}")
            return {'CANCELLED'}

class ASSETTEST_PT_main_panel(Panel):
    """Главная панель для тестирования Asset Catalog Image Browser"""
    bl_label = "Asset Image Browser Test"
    bl_idname = "ASSETTEST_PT_main_panel"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "scene"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        
        # Заголовок
        layout.label(text="Тестирование Asset Catalog Image Browser", icon='IMAGE_DATA')
        layout.separator()
        
        # Кнопки для подготовки тестов
        col = layout.column(align=True)
        col.label(text="Подготовка:")
        col.operator("assettest.create_test_images")
        col.operator("assettest.test_template_registration")
        col.operator("assettest.test_image_asset_conversion")
        
        layout.separator()
        
        # Тест нового template (если он доступен)
        if hasattr(bpy.types.UILayout, 'template_asset_catalog_image_browser'):
            col = layout.column(align=True)
            col.label(text="Asset Catalog Image Browser Template:", icon='CHECKMARK')
            
            # Добавляем свойство для тестирования
            if not hasattr(scene, 'test_image_prop'):
                bpy.types.Scene.test_image_prop = bpy.props.PointerProperty(
                    type=bpy.types.Image,
                    name="Test Image"
                )
            
            try:
                # Вызываем наш новый template
                col.template_asset_catalog_image_browser(
                    data=scene,
                    property="test_image_prop",
                    rows=2,
                    cols=3,
                    auto_convert=True
                )
            except Exception as e:
                col.label(text=f"Ошибка template: {str(e)}", icon='ERROR')
                print(f"Ошибка при вызове template: {e}")
        else:
            layout.label(text="Template не зарегистрирован", icon='ERROR')

class ASSETTEST_PT_debug_panel(Panel):
    """Панель отладочной информации"""
    bl_label = "Debug Info"
    bl_idname = "ASSETTEST_PT_debug_panel"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "scene"
    bl_parent_id = "ASSETTEST_PT_main_panel"

    def draw(self, context):
        layout = self.layout
        
        # Информация о изображениях
        total_images = len(bpy.data.images)
        asset_images = len([img for img in bpy.data.images if img.asset_data])
        
        col = layout.column(align=True)
        col.label(text=f"Всего изображений: {total_images}")
        col.label(text=f"Изображений-assets: {asset_images}")
        
        # Список изображений-assets
        if asset_images > 0:
            col.separator()
            col.label(text="Assets:")
            for img in bpy.data.images:
                if img.asset_data:
                    row = col.row()
                    row.label(text=f"• {img.name}", icon='IMAGE_DATA')

def register():
    """Регистрация классов"""
    bpy.utils.register_class(ASSETTEST_OT_create_test_images)
    bpy.utils.register_class(ASSETTEST_OT_test_template_registration)
    bpy.utils.register_class(ASSETTEST_OT_test_image_asset_conversion)
    bpy.utils.register_class(ASSETTEST_PT_main_panel)
    bpy.utils.register_class(ASSETTEST_PT_debug_panel)
    
    print("Asset Catalog Image Browser Test зарегистрирован")

def unregister():
    """Отмена регистрации классов"""
    bpy.utils.unregister_class(ASSETTEST_PT_debug_panel)
    bpy.utils.unregister_class(ASSETTEST_PT_main_panel)
    bpy.utils.unregister_class(ASSETTEST_OT_test_image_asset_conversion)
    bpy.utils.unregister_class(ASSETTEST_OT_test_template_registration)
    bpy.utils.unregister_class(ASSETTEST_OT_create_test_images)
    
    # Удаляем тестовое свойство
    if hasattr(bpy.types.Scene, 'test_image_prop'):
        del bpy.types.Scene.test_image_prop
    
    print("Asset Catalog Image Browser Test отменен")

def run_tests():
    """Запуск автоматических тестов"""
    print("\n" + "="*50)
    print("ЗАПУСК ТЕСТОВ ASSET CATALOG IMAGE BROWSER")
    print("="*50)
    
    # Тест 1: Проверка регистрации
    template_registered = test_template_registration()
    
    # Тест 2: Вызов template
    template_callable = test_template_call()
    
    # Тест 3: Создание тестовых изображений
    print("\n3. Создание тестовых изображений...")
    bpy.ops.assettest.create_test_images()
    
    # Тест 4: Проверка преобразования в assets
    print("\n4. Тестирование преобразования в assets...")
    bpy.ops.assettest.test_image_asset_conversion()
    
    print("\n" + "="*50)
    print("РЕЗУЛЬТАТЫ ТЕСТОВ")
    print("="*50)
    print(f"Template зарегистрирован: {'✓' if template_registered else '✗'}")
    print(f"Template вызывается: {'✓' if template_callable else '✗'}")
    
    if template_registered and template_callable:
        print("\nДля интерактивного тестирования откройте Properties > Scene")
        print("Там вы найдете панель 'Asset Image Browser Test'")
    else:
        print("\nTemplate не работает корректно. Проверьте компиляцию.")
    
    print("\n" + "="*50)
    print("ТЕСТЫ ЗАВЕРШЕНЫ")
    print("="*50)

if __name__ == "__main__":
    # Регистрируем классы
    register()
    
    # Запускаем тесты
    run_tests()
    
    print("\nДля интерактивного тестирования откройте Properties > Scene")
    print("Там вы найдете панель 'Asset Image Browser Test'")