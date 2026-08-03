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

:: ── Step 1: Compile ViGEmClient.cpp ───────────────────────────────────────
echo [1/2] Compiling ViGEmClient...

cl.exe /nologo /std:c++17 /EHsc /O2 /W1 /MT /DNOMINMAX /c ^
    /I"third_party\ViGEmClient\include" ^
    /I"third_party\ViGEmClient\include\ViGEm" ^
    /I"third_party\ViGEmClient\src" ^
    third_party\ViGEmClient\src\ViGEmClient.cpp ^
    /Fo"ViGEmClient.obj"

if errorlevel 1 goto BUILD_FAIL

:: ── Step 2: Compile and link main.cpp ─────────────────────────────────────
:: Sources live in src\.  The repo root (".") must be on the include path
:: because vigem_loader.h includes the vendored header via a repo-root-relative
:: path ("third_party\ViGEmClient\include\ViGEm\Client.h").
echo [2/2] Compiling main.cpp...

cl.exe /nologo /std:c++17 /EHsc /O2 /W3 /MT /DNOMINMAX ^
    /I"." ^
    /I"src" ^
    /I"third_party\ViGEmClient\include" ^
    /I"third_party\ViGEmClient\include\ViGEm" ^
    /I"third_party\ViGEmClient\src" ^
    src\main.cpp ^
    ViGEmClient.obj ^
    /Fe:ControllerPassthrough.exe ^
    /link ^
    xinput.lib ^
    winmm.lib ^
    user32.lib ^
    shell32.lib ^
    advapi32.lib ^
    setupapi.lib ^
    hid.lib ^
    cfgmgr32.lib ^
    /SUBSYSTEM:CONSOLE

if errorlevel 1 goto BUILD_FAIL

echo.
echo ======================================================================
echo [SUCCESS] ControllerPassthrough.exe built successfully.
echo ======================================================================
echo.
pause
exit /b 0

:BUILD_FAIL
echo.
echo [FAILED] Build failed — see errors above.
pause
exit /b 1
