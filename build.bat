@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "!VSWHERE!" (
    echo Build Tools introuvables. Installe "Visual Studio Build Tools" avec la charge
    echo de travail "Developpement Desktop en C++".
    pause & exit /b 1
  )
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -property installationPath`) do (
    call "%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)

if not exist build mkdir build
rc /nologo /fo build\app.res app.rc || goto :err
cl /nologo /O2 /W3 /MT /DNDEBUG /Fobuild\ /Fe:build\AmpliVision.exe main.c build\app.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
   d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib || goto :err

echo.
echo OK : build\AmpliVision.exe
pause & exit /b 0

:err
echo.
echo Echec de compilation.
pause & exit /b 1