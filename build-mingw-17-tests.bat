@echo off
setlocal

REM === Settings ===
set BUILD_DIR=build-mingw-cpp17-tests
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
    -DMDBXC_BUILD_TESTS=ON ^
    -DMDBXC_BUILD_EXAMPLES=OFF ^
    -DMDBXC_USE_ASAN=ON

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

REM === Run tests ===
REM Добавим bin в PATH на случай, если зависят от DLL.
set "PATH=%CD%\%BUILD_DIR%\bin;%PATH%"

ctest --test-dir "%BUILD_DIR%" --output-on-failure
REM ctest -R kv_container_all_types_test -V

if errorlevel 1 (
    echo [ERROR] Tests failed.
    pause
    exit /b 1
)

echo.
echo === Build and tests finished successfully ===
pause
exit /b 0
