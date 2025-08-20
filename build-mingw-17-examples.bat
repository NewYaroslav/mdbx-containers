@echo off
setlocal

REM === Settings ===
set BUILD_DIR=build-mingw-cpp17-examples
set GENERATOR=MinGW Makefiles
set BUILD_TYPE=Release

REM === Create build dir ===
if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

REM === Configure ===
cmake -S . -B "%BUILD_DIR%" ^
    -G "%GENERATOR%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_CXX_STANDARD=17 ^
    -DMDBXC_DEPS_MODE=AUTO ^
    -DMDBXC_BUILD_STATIC_LIB=OFF ^
    -DMDBXC_BUILD_TESTS=OFF ^
    -DMDBXC_BUILD_EXAMPLES=ON ^
	-DMDBXC_USE_ASAN=OFF

if errorlevel 1 (
    echo [ERROR] CMake generation failed.
    pause
    exit /b 1
)

REM === Build ===
cmake --build "%BUILD_DIR%" -- -j

if errorlevel 1 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo === Build and tests finished successfully ===
pause
exit /b 0