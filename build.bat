@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /O2 /GL /EHsc /std:c++17 /W3 /MT /Zi /Fdobj\ /DUNICODE /D_UNICODE /Foobj\ src\main.cpp ^
  /Fe:bin\CapView.exe ^
  /link /LTCG /DEBUG /OPT:REF /OPT:ICF /MANIFEST:EMBED /PDB:obj\CapView.pdb /SUBSYSTEM:WINDOWS ^
  d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mf.lib mfuuid.lib ole32.lib avrt.lib cfgmgr32.lib user32.lib shell32.lib propsys.lib comctl32.lib d3dcompiler.lib advapi32.lib gdi32.lib d2d1.lib dwrite.lib
exit /b %errorlevel%
