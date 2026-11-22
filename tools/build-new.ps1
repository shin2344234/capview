# Builds CapView to obj\CapView_new.exe, so a running bin\CapView.exe doesn't block the build.
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$out = cmd /c .\build.bat obj\CapView_new.exe 2>&1
$out | Where-Object { $_ -notmatch '^\S+\.cpp$|Generating code|Finished generating' }
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
"built " + (Get-Item obj\CapView_new.exe).LastWriteTime
