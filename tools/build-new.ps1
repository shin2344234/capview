# Builds CapView to obj\CapView_new.exe, so a running bin\CapView.exe doesn't block the build.
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$libs = 'd3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mf.lib mfuuid.lib ole32.lib avrt.lib cfgmgr32.lib user32.lib shell32.lib propsys.lib comctl32.lib d3dcompiler.lib advapi32.lib gdi32.lib d2d1.lib dwrite.lib'
$cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && cl /nologo /O2 /GL /EHsc /std:c++17 /W3 /MT /Zi /Fdobj\ /DUNICODE /D_UNICODE /Foobj\ src\main.cpp /Fe:obj\CapView_new.exe /link /LTCG /DEBUG /OPT:REF /OPT:ICF /MANIFEST:EMBED /PDB:obj\CapView_new.pdb /SUBSYSTEM:WINDOWS $libs"
$out = cmd /c $cmd 2>&1
$out | Where-Object { $_ -notmatch '^main\.cpp$|Generating code|Finished generating' }
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
"built " + (Get-Item obj\CapView_new.exe).LastWriteTime
