param(
    [int]$Seconds = 10,
    [string]$PresentMon = "C:\oddras-build\presentmon\PresentMon-2.6.0-x64.exe",
    [string]$Csv = "$env:TEMP\capview-latency.csv"
)
# Records CapView's presents with PresentMon and prints how long frames wait
# between Present and the screen. CapView must already be running.
# With Smooth Motion or AFMF, each real frame goes out right after a generated
# one (under 5 ms apart), so the two are reported separately.

if (-not (Get-Process CapView -ErrorAction SilentlyContinue)) { throw "CapView is not running" }
Remove-Item $Csv -ErrorAction SilentlyContinue
& $PresentMon --process_name CapView.exe --output_file $Csv --timed $Seconds `
    --terminate_after_timed --no_console_stats 2>&1 | Out-Null
if (-not (Test-Path $Csv)) { throw "PresentMon wrote no file" }
$r = @(Import-Csv $Csv)
if ($r.Count -eq 0) { throw "PresentMon saw no presents from CapView" }

function Show-Group($name, $rows) {
    if ($rows.Count -eq 0) { return }
    $shown = @($rows | Where-Object { $_.MsUntilDisplayed -ne 'NA' })
    $m = $shown | ForEach-Object { [double]$_.MsUntilDisplayed } | Measure-Object -Average -Minimum -Maximum
    "{0,-28} {1,5} presents, {2,4} not shown, Present to screen avg {3,5:N1} ms (min {4:N1}, max {5:N1})" -f `
        $name, $rows.Count, ($rows.Count - $shown.Count), $m.Average, $m.Minimum, $m.Maximum
}

"{0} presents in {1} s" -f $r.Count, $Seconds
$r | Group-Object PresentMode | ForEach-Object { "  {0}: {1}" -f $_.Name, $_.Count }
$paired = @($r | Where-Object { [double]$_.MsBetweenPresents -lt 5 })
if ($paired.Count -gt $r.Count / 3) {
    Show-Group "Generated (first of pair)" @($r | Where-Object { [double]$_.MsBetweenPresents -ge 5 })
    Show-Group "Real (second of pair)" $paired
} else {
    Show-Group "All presents" $r
}
$dc = $r | Where-Object { $_.MsBetweenDisplayChange -ne 'NA' } | ForEach-Object { [double]$_.MsBetweenDisplayChange } |
    Measure-Object -Average -Maximum
"Screen updates every {0:N1} ms on average, longest gap {1:N1} ms" -f $dc.Average, $dc.Maximum
