@echo off
setlocal

echo ======================================================================
echo   CONTROLLER PASSTHROUGH - BUILD SCRIPT
echo ======================================================================
echo.

:: Run from the repository root so all relative paths below resolve, no matter
:: where the script is launched from.
cd /d "%~dp0"

where cl.exe >nul 2>nul
if %errorlevel%==0 goto BUILD

set VCVARS=

if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"    set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
if exist "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"   set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if exist "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"   set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if "%VCVARS%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if "%VCVARS%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if "%VCVARS%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"   set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if "%VCVARS%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"   set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if "%VCVARS%"=="" goto NO_VCVARS

echo [INFO] Initializing MSVC from: "%VCVARS%"
call "%VCVARS%" >nul 2>&1
goto BUILD

:NO_VCVARS
echo [ERROR] Could not find Visual Studio. Install VS 2026/2022 with the C++ workload.
pause
exit /b 1

:BUILD
echo [OK] Compiler ready.
echo.

set COMMON_INC=/I"." /I"src" /I"third_party\ViGEmClient\include" /I"third_party\ViGEmClient\include\ViGEm" /I"third_party\ViGEmClient\src"
set IMGUI_INC=/I"third_party\imgui" /I"third_party\imgui\backends"
set SYSLIBS=xinput.lib winmm.lib user32.lib shell32.lib advapi32.lib setupapi.lib hid.lib cfgmgr32.lib ole32.lib

:: ── Step 1: ViGEmClient ────────────────────────────────────────────────────
echo [1/4] Compiling ViGEmClient...
cl.exe /nologo /std:c++17 /EHsc /O2 /W1 /MT /DNOMINMAX /c ^
    /I"third_party\ViGEmClient\include" /I"third_party\ViGEmClient\include\ViGEm" /I"third_party\ViGEmClient\src" ^
    third_party\ViGEmClient\src\ViGEmClient.cpp /Fo"ViGEmClient.obj"
if errorlevel 1 goto BUILD_FAIL

:: ── Step 2: Dear ImGui (core + Win32/DX11 backends) ────────────────────────
echo [2/4] Compiling Dear ImGui...
cl.exe /nologo /std:c++17 /EHsc /O2 /W1 /MT /DNOMINMAX /c %IMGUI_INC% ^
    third_party\imgui\imgui.cpp ^
    third_party\imgui\imgui_draw.cpp ^
    third_party\imgui\imgui_tables.cpp ^
    third_party\imgui\imgui_widgets.cpp ^
    third_party\imgui\backends\imgui_impl_win32.cpp ^
    third_party\imgui\backends\imgui_impl_dx11.cpp
if errorlevel 1 goto BUILD_FAIL

:: ── Step 3: GUI front-end (primary) ────────────────────────────────────────
echo [3/4] Compiling GUI (ControllerPassthroughGui.exe)...
cl.exe /nologo /std:c++17 /EHsc /O2 /W3 /MT /DNOMINMAX /DUNICODE /D_UNICODE ^
    %COMMON_INC% %IMGUI_INC% ^
    src\gui_main.cpp ^
    ViGEmClient.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_impl_win32.obj imgui_impl_dx11.obj ^
    /Fe:ControllerPassthroughGui.exe ^
    /link %SYSLIBS% d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib gdi32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 goto BUILD_FAIL

:: ── Step 4: CLI front-end ──────────────────────────────────────────────────
echo [4/4] Compiling CLI (ControllerPassthrough.exe)...
cl.exe /nologo /std:c++17 /EHsc /O2 /W3 /MT /DNOMINMAX ^
    %COMMON_INC% ^
    src\main.cpp ViGEmClient.obj ^
    /Fe:ControllerPassthrough.exe ^
    /link %SYSLIBS% /SUBSYSTEM:CONSOLE
if errorlevel 1 goto BUILD_FAIL

echo.
echo ======================================================================
echo [SUCCESS] Built ControllerPassthroughGui.exe (GUI) and ControllerPassthrough.exe (CLI).
echo ======================================================================
echo.
pause
exit /b 0

:BUILD_FAIL
echo.
echo [FAILED] Build failed — see errors above.
pause
exit /b 1
