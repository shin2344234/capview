param(
    [int]$Phases = 6,
    [int]$PhaseSeconds = 30,
    [string]$PresentMon = "C:\oddras-build\presentmon\PresentMon-2.6.0-x64.exe",
    [string]$Out = "$env:TEMP\capview-pacecheck",
    [string]$Focus = "",
    [switch]$AnalyzeOnly
)
# -Focus <process name> puts that process's main window in front for the test
# and gives focus back to the window that had it at the end.
# A/B test for Smooth Motion pacing. Records CapView's presents with PresentMon
# in alternating phases: CapView's own CPU and GPU priority, then high CPU and
# high GPU priority set from outside on the running process, and so on. At the
# end it puts both priorities back. For each phase it prints how many real
# frames Smooth Motion showed right after the generated frame instead of
# holding them half a frame (each one is a visible hitch), and which app had
# focus. Run it with CapView on screen and focus where you normally keep it.

New-Item -ItemType Directory -Force $Out | Out-Null
$csv = Join-Path $Out "presents.csv"; $fgLog = Join-Path $Out "foreground.log"; $marks = Join-Path $Out "phases.txt"
Add-Type -Path (Join-Path $PSScriptRoot "PaceCheck.cs")

if (-not $AnalyzeOnly) {
    $cv = Get-Process CapView -ErrorAction Stop | Select-Object -First 1
    $origCpu = $cv.PriorityClass
    $st = 0; $origGpu = [PaceCheck]::GetGpu($cv.Id, [ref]$st)
    if ($origGpu -lt 0) { throw "Can't read CapView's GPU priority (NTSTATUS 0x{0:X8})" -f $st }
    Remove-Item $csv, $fgLog, $marks -ErrorAction SilentlyContinue
    $total = $Phases * $PhaseSeconds + 5
    $pm = Start-Process $PresentMon -ArgumentList '--process_name', 'CapView.exe', '--output_file', $csv, '--timed', $total,
        '--terminate_after_timed', '--no_console_stats', '--qpc_time' -PassThru -WindowStyle Hidden
    $previous = [PaceCheck]::Foreground()
    if ($Focus) {
        $w = Get-Process $Focus -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if (-not $w) { throw "$Focus has no main window" }
        [PaceCheck]::Focus($w.MainWindowHandle)
    }
    [PaceCheck]::StartSampler($fgLog)
    # PresentMon's trace session takes a few seconds before it records anything.
    Start-Sleep 3
    try {
        for ($i = 0; $i -lt $Phases; $i++) {
            $high = ($i % 2) -eq 1
            $cv.PriorityClass = if ($high) { 'High' } else { $origCpu }
            $s = [PaceCheck]::SetGpu($cv.Id, $(if ($high) { 4 } else { $origGpu }))
            $name = if ($high) { "high CPU and GPU priority" } else { "CapView's own priority ($origCpu, GPU class $origGpu)" }
            if ($s -ne 0) { $name += " (GPU set failed 0x{0:X8})" -f $s }
            "q{0} {1}" -f [Diagnostics.Stopwatch]::GetTimestamp(), $name | Add-Content $marks
            Start-Sleep $PhaseSeconds
        }
        "q{0} end" -f [Diagnostics.Stopwatch]::GetTimestamp() | Add-Content $marks
    } finally {
        $cv.PriorityClass = $origCpu
        [PaceCheck]::SetGpu($cv.Id, $origGpu) | Out-Null
        [PaceCheck]::StopSampler()
        if ($Focus -and $previous -ne [IntPtr]::Zero) { [PaceCheck]::Focus($previous) }
    }
    $pm.WaitForExit(30000) | Out-Null
}

$fg = foreach ($l in Get-Content $fgLog) { if ($l -match '^q(\d+) (.*)$') { [pscustomobject]@{ Q = [double]$matches[1] / 1e7; Name = $matches[2] } } }
$steps = foreach ($l in Get-Content $marks) { if ($l -match '^q(\d+) (.*)$') { [pscustomobject]@{ Q = [double]$matches[1] / 1e7; Name = $matches[2] } } }
$rows = @(Import-Csv $csv)
function Num($v) { if ($v -eq 'NA') { $null } else { [double]$v } }
$totals = @{}
for ($i = 0; $i -lt $steps.Count - 1; $i++) {
    $a = $steps[$i].Q + 0.5; $b = $steps[$i + 1].Q
    $miss = 0; $notShown = 0; $n = 0; $modes = @{}
    for ($j = 1; $j -lt $rows.Count; $j++) {
        $q = [double]$rows[$j].TimeInQPC / 1e7
        if ($q -lt $a -or $q -ge $b) { continue }
        $n++
        $modes[$rows[$j].PresentMode] = 1 + [int]$modes[$rows[$j].PresentMode]
        if ($rows[$j].MsUntilDisplayed -eq 'NA') { $notShown++; continue }
        $bp = Num $rows[$j].MsBetweenPresents; $bd = Num $rows[$j].MsBetweenDisplayChange; $gp = Num $rows[$j - 1].MsBetweenPresents
        # A real frame (0.2 ms after its generated frame) shown under 3 ms after it.
        # Right after a present gap over 45 ms the pacer restarts, so those don't count.
        if ($bp -ne $null -and $bp -lt 3 -and $bd -ne $null -and $bd -lt 3 -and $gp -ne $null -and $gp -le 45) { $miss++ }
    }
    # Not $focus: PowerShell names ignore case, so that would overwrite the -Focus parameter.
    $focusSecs = @{}
    $cur = ($fg | Where-Object { $_.Q -le $a } | Select-Object -Last 1).Name
    $t = $a
    foreach ($e in ($fg | Where-Object { $_.Q -gt $a -and $_.Q -lt $b })) { $focusSecs[$cur] = [double]$focusSecs[$cur] + ($e.Q - $t); $t = $e.Q; $cur = $e.Name }
    $focusSecs[$cur] = [double]$focusSecs[$cur] + ($b - $t)
    $focusText = ($focusSecs.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { "{0} {1:N0} s" -f $_.Key, $_.Value }) -join ', '
    $modeText = ($modes.GetEnumerator() | ForEach-Object { "$($_.Key -replace 'Hardware: ', '') $($_.Value)" }) -join ', '
    $secs = $b - $a
    "{0,-44} {1,5:N0} s  {2,3} hitches ({3,4:N1}/min)  {4} not shown | {5} | focus: {6}" -f $steps[$i].Name, $secs, $miss, ($miss / $secs * 60), $notShown, $modeText, $focusText
    $key = if ($steps[$i].Name -like 'high*') { 'high' } else { 'own' }
    if (-not $totals[$key]) { $totals[$key] = [pscustomobject]@{ Secs = 0.0; Miss = 0 } }
    $totals[$key].Secs += $secs; $totals[$key].Miss += $miss
}
""
foreach ($k in 'own', 'high') { if ($totals[$k]) { "{0,-5} priority: {1} hitches in {2:N0} s, {3:N1} per minute" -f $k, $totals[$k].Miss, $totals[$k].Secs, ($totals[$k].Miss / $totals[$k].Secs * 60) } }
