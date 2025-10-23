#!/usr/bin/env python3
"""
Скрипт для проверки компиляции и запуска Blender с новым Asset Catalog Image Browser Template
Для Blender 5.0

Этот скрипт:
1. Проверяет успешность компиляции
2. Запускает Blender с тестовым скриптом
3. Проверяет доступность нового template
"""

import subprocess
import sys
import os
import time

# Пути к файлам Blender
BLENDER_ROOT = r"N:\Blender_CODE\blender"
BLENDER_EXECUTABLE = r"N:\Blender_CODE\build_windows_Lite_x64_vc17_Release\bin\Release\blender.exe"
BUILD_LOG = r"N:\Blender_CODE\build_windows_Lite_x64_vc17_Release\Build.log"
TEST_SCRIPT = os.path.join(BLENDER_ROOT, "test_asset_catalog_image_browser.py")

def check_build_log():
    """Проверить лог сборки на наличие ошибок"""
    print("Проверка лога сборки...")
    
    if not os.path.exists(BUILD_LOG):
        print(f"❌ Лог сборки не найден: {BUILD_LOG}")
        return False
    
    try:
        with open(BUILD_LOG, 'r', encoding='utf-8', errors='ignore') as f:
            log_content = f.read()
        
        # Ищем ошибки в логе
        error_indicators = [
            "error C",
            "fatal error",
            "Build FAILED",
            "compilation terminated",
            ": error:",
        ]
        
        errors_found = []
        lines = log_content.split('\n')
        
        for i, line in enumerate(lines):
            for indicator in error_indicators:
                if indicator.lower() in line.lower():
                    errors_found.append(f"Строка {i+1}: {line.strip()}")
        
        if errors_found:
            print(f"❌ Найдены ошибки компиляции ({len(errors_found)}):")
            for error in errors_found[-10:]:  # Показываем последние 10 ошибок
                print(f"  {error}")
            return False
        else:
            print("✅ Ошибки компиляции не найдены")
            return True
            
    except Exception as e:
        print(f"❌ Ошибка при чтении лога: {e}")
        return False

def check_blender_executable():
    """Проверить существование исполняемого файла Blender"""
    print("Проверка исполняемого файла Blender...")
    
    if not os.path.exists(BLENDER_EXECUTABLE):
        print(f"❌ Исполняемый файл Blender не найден: {BLENDER_EXECUTABLE}")
        return False
    
    print(f"✅ Исполняемый файл найден: {BLENDER_EXECUTABLE}")
    return True

def run_blender_with_test():
    """Запустить Blender с тестовым скриптом"""
    print("Запуск Blender с тестовым скриптом...")
    
    if not os.path.exists(TEST_SCRIPT):
        print(f"❌ Тестовый скрипт не найден: {TEST_SCRIPT}")
        return False
    
    try:
        # Команда для запуска Blender с Python скриптом
        cmd = [
            BLENDER_EXECUTABLE,
            "--background",  # Запуск в фоновом режиме
            "--python", TEST_SCRIPT,
            "--python-exit-code", "1"  # Выход с кодом ошибки при Python ошибках
        ]
        
        print(f"Выполнение команды: {' '.join(cmd)}")
        
        # Запускаем процесс
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=BLENDER_ROOT
        )
        
        # Ждем завершения с таймаутом
        try:
            stdout, stderr = process.communicate(timeout=60)  # 60 секунд таймаут
        except subprocess.TimeoutExpired:
            process.kill()
            print("❌ Таймаут при запуске Blender")
            return False
        
        # Проверяем результат
        if process.returncode == 0:
            print("✅ Blender успешно запущен и выполнил тестовый скрипт")
            print("\nВывод Blender:")
            print(stdout)
            return True
        else:
            print(f"❌ Blender завершился с ошибкой (код: {process.returncode})")
            print("\nВывод ошибок:")
            print(stderr)
            print("\nСтандартный вывод:")
            print(stdout)
            return False
            
    except Exception as e:
        print(f"❌ Ошибка при запуске Blender: {e}")
        return False

def run_interactive_test():
    """Запустить Blender в интерактивном режиме для ручного тестирования"""
    print("Запуск Blender в интерактивном режиме...")
    
    try:
        cmd = [
            BLENDER_EXECUTABLE,
            "--python", TEST_SCRIPT
        ]
        
        print(f"Выполнение команды: {' '.join(cmd)}")
        print("Blender откроется в интерактивном режиме.")
        print("Для тестирования:")
        print("1. Откройте Properties > Scene")
        print("2. Найдите панель 'Asset Image Browser Test'")
        print("3. Нажмите кнопки для тестирования")
        
        # Запускаем интерактивный процесс
        subprocess.run(cmd, cwd=BLENDER_ROOT)
        
        return True
        
    except Exception as e:
        print(f"❌ Ошибка при запуске интерактивного Blender: {e}")
        return False

def main():
    """Главная функция"""
    print("="*60)
    print("ПРОВЕРКА КОМПИЛЯЦИИ И ТЕСТИРОВАНИЕ BLENDER")
    print("Asset Catalog Image Browser Template")
    print("="*60)
    
    # Шаг 1: Проверка лога сборки
    if not check_build_log():
        print("\n❌ Сборка содержит ошибки. Исправьте их перед тестированием.")
        return False
    
    # Шаг 2: Проверка исполняемого файла
    if not check_blender_executable():
        print("\n❌ Исполняемый файл Blender не найден. Выполните сборку.")
        return False
    
    # Шаг 3: Автоматический тест
    print("\n" + "-"*40)
    print("АВТОМАТИЧЕСКОЕ ТЕСТИРОВАНИЕ")
    print("-"*40)
    
    if run_blender_with_test():
        print("\n✅ Автоматические тесты прошли успешно!")
    else:
        print("\n❌ Автоматические тесты не прошли.")
        print("Проверьте ошибки выше.")
    
    # Шаг 4: Предложение интерактивного тестирования
    print("\n" + "-"*40)
    print("ИНТЕРАКТИВНОЕ ТЕСТИРОВАНИЕ")
    print("-"*40)
    
    response = input("Запустить Blender для интерактивного тестирования? (y/n): ")
    if response.lower() in ['y', 'yes', 'да', 'д']:
        run_interactive_test()
    
    print("\n✅ Проверка завершена!")
    return True

if __name__ == "__main__":
    main()