# Installs RTX Encore next to CapView.exe as dxgi.dll, which gives CapView NVIDIA Smooth Motion on an
# RTX 30 card. Run it in PowerShell in the folder that holds CapView.exe:
#   irm https://raw.githubusercontent.com/shin2344234/capview/main/install-rtx-encore.ps1 | iex
# It downloads the latest release from https://github.com/SilyNoMeta/rtx-encore, checks the zip against
# the SHA-256 GitHub lists for it and copies out rtx-encore.dll. Run it again to update RTX Encore.
& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $dir = (Get-Location).ProviderPath
    $dxgi = Join-Path $dir 'dxgi.dll'
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'CapView.exe'))) {
        Write-Host "There's no CapView.exe in $dir. Open PowerShell in CapView's folder and run this again." -ForegroundColor Red
        return
    }
    if (Get-Process CapView -ErrorAction SilentlyContinue | Where-Object { $_.Path -and (Split-Path $_.Path) -eq $dir }) {
        Write-Host 'Close CapView, then run this again.' -ForegroundColor Red
        return
    }
    # RTX Encore keeps rtx-encore.jsonc and rtx-encore-logs beside itself. A dxgi.dll without them belongs
    # to something else, such as ReShade or Special K, so it stays.
    $encoreHere = (Test-Path -LiteralPath (Join-Path $dir 'rtx-encore.jsonc')) -or (Test-Path -LiteralPath (Join-Path $dir 'rtx-encore-logs'))
    if ((Test-Path -LiteralPath $dxgi) -and -not $encoreHere) {
        Write-Host "$dxgi is from another program, so this stopped. Move it out of the folder and run this again." -ForegroundColor Red
        return
    }

    $rel = Invoke-RestMethod 'https://api.github.com/repos/SilyNoMeta/rtx-encore/releases/latest' -UseBasicParsing
    $asset = $rel.assets | Where-Object { $_.name -like '*.zip' } | Select-Object -First 1
    if (-not $asset) { throw "RTX Encore $($rel.tag_name) has no zip to download." }
    $zip = Join-Path ([IO.Path]::GetTempPath()) $asset.name
    try {
        Write-Host ("Downloading RTX Encore {0}, {1:N0} MB" -f $rel.tag_name, ($asset.size / 1MB))
        Invoke-WebRequest $asset.browser_download_url -OutFile $zip -UseBasicParsing
        $want = "$($asset.digest)" -replace '^sha256:', ''
        if ($want) {
            if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $want) { throw 'The download is damaged. Run this again.' }
            Write-Host 'SHA-256 matches the release.'
        } else {
            Write-Host 'GitHub lists no SHA-256 for this zip, so it went unchecked.' -ForegroundColor Yellow
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $z = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $dll = $z.Entries | Where-Object { $_.Name -eq 'rtx-encore.dll' -and $_.FullName -notmatch 'alternative-proxies' } | Select-Object -First 1
            if (-not $dll) { throw "The zip has no rtx-encore.dll. Install it by hand as RTX Encore's README says." }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($dll, "$dxgi.new", $true)
        } finally {
            $z.Dispose()
        }
        Move-Item -LiteralPath "$dxgi.new" -Destination $dxgi -Force
    } finally {
        Remove-Item -LiteralPath $zip, "$dxgi.new" -ErrorAction SilentlyContinue
    }
    Write-Host "RTX Encore $($rel.tag_name) is in place as $dxgi." -ForegroundColor Green
    Write-Host 'Start CapView, press S for settings and pick NVIDIA Smooth Motion. CapView turns it on in RTX Encore and offers to restart.'
}
