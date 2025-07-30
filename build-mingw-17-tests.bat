@echo off
setlocal

REM === Настройки ===
set BUILD_DIR=build-cpp17-tests-deps

REM === Создание папки сборки ===
if not exist %BUILD_DIR% (
    mkdir %BUILD_DIR%
)

REM === Генерация CMake проекта ===
cmake -S . -B %BUILD_DIR% ^
    -G "MinGW Makefiles" ^
    -DCMAKE_CXX_STANDARD=17 ^
    -DBUILD_DEPS=ON ^
    -DBUILD_STATIC_LIB=OFF ^
    -DBUILD_TESTS=ON ^
    -DBUILD_EXAMPLES=OFF

if %errorlevel% neq 0 (
    echo [ERROR] CMake generation failed.
    pause
    exit /b 1
)

REM === Сборка ===
cmake --build %BUILD_DIR% --config Release

if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo === Build finished successfully ===
pause
endlocal