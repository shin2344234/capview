param(
    [int]$Seconds = 300,
    [int]$IntervalMs = 50,
    [string]$Out = "$env:TEMP\capview-watch",
    [string]$PresentMon = "C:\oddras-build\presentmon\PresentMon-2.6.0-x64.exe",
    [switch]$AnalyzeOnly
)
# Records CapView's presents with PresentMon and, at the same time, every change
# to the windows, cursor, notification state and processes around CapView
# (Watch.cs). Then prints each stretch of one present mode, with how many of its
# presents never reached the screen, next to the watcher events from 1.5 s
# before it started to 0.5 s after. Both logs are matched on the QPC clock.
# -AnalyzeOnly prints the report again from the files already in -Out.

New-Item -ItemType Directory -Force $Out | Out-Null
$csv = Join-Path $Out "presents.csv"; $log = Join-Path $Out "watch.log"
if (-not $AnalyzeOnly) {
    Remove-Item $csv, $log -ErrorAction SilentlyContinue
    Add-Type -Path (Join-Path $PSScriptRoot "Watch.cs")
    $pm = Start-Process $PresentMon -ArgumentList '--process_name', 'CapView.exe', '--output_file', $csv, '--timed', $Seconds,
        '--terminate_after_timed', '--no_console_stats', '--qpc_time' -PassThru -WindowStyle Hidden
    [CapWatch]::Run($log, $Seconds, $IntervalMs)
    $pm.WaitForExit(20000) | Out-Null
}

$q = 0
$events = foreach ($line in Get-Content $log) {
    if ($line -match '^\S+ q(\d+) ') { $q = [double]$matches[1] / 1e7 }
    [pscustomobject]@{ Q = $q; Line = ($line -replace ' q\d+ ', ' ') }
}
$baseQ = $events[0].Q
$baseT = [datetime]::ParseExact($events[0].Line.Substring(0, 12), 'HH:mm:ss.fff', $null)
$rows = @(Import-Csv $csv)
if ($rows.Count -eq 0) { "PresentMon saw no presents from CapView"; return }
$switches = @(); $prev = $null
for ($i = 0; $i -lt $rows.Count; $i++) {
    if ($rows[$i].PresentMode -ne $prev) {
        $switches += [pscustomobject]@{ Index = $i; Q = [double]$rows[$i].TimeInQPC / 1e7; Mode = $rows[$i].PresentMode }
        $prev = $rows[$i].PresentMode
    }
}
"{0} presents, {1} present mode switches" -f $rows.Count, ($switches.Count - 1)
for ($i = 0; $i -lt $switches.Count; $i++) {
    $s = $switches[$i]
    $endIndex = if ($i + 1 -lt $switches.Count) { $switches[$i + 1].Index } else { $rows.Count }
    $endQ = [double]$rows[$endIndex - 1].TimeInQPC / 1e7
    $notShown = 0
    for ($j = $s.Index; $j -lt $endIndex; $j++) { if ($rows[$j].MsUntilDisplayed -eq 'NA') { $notShown++ } }
    $at = $baseT.AddSeconds($s.Q - $baseQ)
    ""
    "=== {0:HH:mm:ss.fff} {1}, {2:N1} s, {3} presents, {4} not shown" -f $at, $s.Mode, ($endQ - $s.Q), ($endIndex - $s.Index), $notShown
    if ($i -eq 0) { continue }
    $events | Where-Object { $_.Q -ge $s.Q - 1.5 -and $_.Q -le $s.Q + 0.5 } | ForEach-Object { "  " + $_.Line }
}
