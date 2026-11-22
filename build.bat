@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj\camera mkdir obj\camera
set "OUT=%~1"
if not defined OUT set "OUT=bin\CapView.exe"
for %%F in ("%OUT%") do set "PDB=obj\%%~nF.pdb"

rem CapView Camera, the DirectShow filter that CapView.exe carries as resource 101.
cl /nologo /O2 /EHsc /std:c++17 /W3 /MT /LD /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
  /Isrc\camera /Isrc\camera\baseclasses /Foobj\camera\ /Fdobj\camera\ ^
  src\camera\camera.cpp src\camera\softcamcore\DShowSoftcam.cpp src\camera\softcamcore\FrameBuffer.cpp ^
  src\camera\softcamcore\Misc.cpp src\camera\softcamcore\Watchdog.cpp src\camera\baseclasses\*.cpp ^
  /Fe:obj\capview-camera.dll ^
  /link /DEF:src\camera\camera.def /OPT:REF /OPT:ICF /IMPLIB:obj\camera\capview-camera.lib ^
  strmiids.lib winmm.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib user32.lib gdi32.lib >obj\camera\build.log || (
  type obj\camera\build.log
  exit /b 1
)
rc /nologo /fo obj\capview.res src\capview.rc || exit /b 1

cl /nologo /O2 /GL /EHsc /std:c++17 /W3 /MT /Zi /Fdobj\ /DUNICODE /D_UNICODE /Isrc\camera /Foobj\ src\main.cpp ^
  src\camera\softcamcore\FrameBuffer.cpp src\camera\softcamcore\Misc.cpp src\camera\softcamcore\Watchdog.cpp ^
  src\camera\softcamcore\SenderAPI.cpp ^
  /Fe:%OUT% ^
  /link /LTCG /DEBUG /OPT:REF /OPT:ICF /MANIFEST:EMBED /PDB:%PDB% /SUBSYSTEM:WINDOWS obj\capview.res ^
  d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mf.lib mfuuid.lib ole32.lib avrt.lib cfgmgr32.lib user32.lib shell32.lib propsys.lib comctl32.lib d3dcompiler.lib advapi32.lib gdi32.lib d2d1.lib dwrite.lib winmm.lib
exit /b %errorlevel%
