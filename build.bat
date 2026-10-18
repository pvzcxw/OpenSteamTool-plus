@echo off
setlocal EnableExtensions

REM Build the requested DLLs with the CMake generator available on this machine.
cd /d "%~dp0"

REM Optional environment variables: GENERATOR, ARCH, CONFIG, BUILD_DIR.
if "%CONFIG%"=="" set "CONFIG=Release"
if "%ARCH%"=="" set "ARCH=x64"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%~dp0src\out\build\build-bat"

echo [INFO] Build directory: %BUILD_DIR%
echo [INFO] Configuration: %CONFIG%

if defined GENERATOR (
    echo [INFO] CMake generator: %GENERATOR%
    echo "%GENERATOR%" | findstr /I /C:"Visual Studio" >nul
    if not errorlevel 1 (
        cmake -S src -B "%BUILD_DIR%" -G "%GENERATOR%" -A "%ARCH%" -DCMAKE_BUILD_TYPE=%CONFIG%
    ) else (
        cmake -S src -B "%BUILD_DIR%" -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=%CONFIG%
    )
) else (
    echo [INFO] Using CMake's default generator.
    cmake -S src -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG%
)
if errorlevel 1 goto :fail

echo [INFO] Building dwmapi and OpenSteamTool...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target dwmapi OpenSteamTool --parallel
if errorlevel 1 goto :fail

if not exist "src\out\dwmapi-fixed\dwmapi.dll" goto :missing_output
if not exist "src\out\dwmapi-fixed\OpenSteamTool.dll" goto :missing_output

echo [OK] Built artifacts:
echo      %~dp0src\out\build\dwmapi.dll
echo      %~dp0src\out\build\OpenSteamTool.dll
exit /b 0

:missing_output
echo [ERROR] Build finished, but the expected DLL output was not found in src\out\dwmapi-fixed.
exit /b 1

:fail
echo [ERROR] Build failed.
exit /b 1
