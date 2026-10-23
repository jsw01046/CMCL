@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [错误] 找不到 cmake，请先安装 CMake 并加入 PATH。
    pause
    exit /b 1
)

set "NEED_CONFIGURE=0"
set "VCPKG_LIB=build\vs2026-x64\vcpkg_installed\x64-windows-static-md\lib\WebView2LoaderStatic.lib"

if not exist "build\vs2026-x64\CMakeCache.txt" set "NEED_CONFIGURE=1"

if not exist "%VCPKG_LIB%" (
    echo [修复] vcpkg WebView2 依赖不完整，正在重新安装...
    if exist "build\vs2026-x64\vcpkg_installed" (
        move "build\vs2026-x64\vcpkg_installed" "build\vs2026-x64\vcpkg_installed.broken.%RANDOM%" >nul
    )
    set "NEED_CONFIGURE=1"
)

if not exist "web\app.css" (
    echo [警告] web 资源不完整，请从安装包或备份恢复 web 目录。
)

if "%NEED_CONFIGURE%"=="1" (
    echo [1/2] 配置 CMake...
    cmake --preset vs2026-x64
    if errorlevel 1 (
        echo [错误] CMake 配置失败。
        pause
        exit /b 1
    )
)

echo [2/2] 编译 Debug...
cmake --build "build\vs2026-x64" --config Debug --parallel
if errorlevel 1 (
    echo [错误] 编译失败。如果启动器正在运行，请先关闭它。
    pause
    exit /b 1
)

start "" "build\vs2026-x64\Debug\cmcl-ui.exe"
