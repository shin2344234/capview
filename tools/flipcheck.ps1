param(
    [int]$HoldSeconds = 5,
    [string]$PresentMon = "C:\oddras-build\presentmon\PresentMon-2.6.0-x64.exe",
    [string]$Csv = "$env:TEMP\capview-flipcheck.csv"
)
# Brings CapView to the front, then puts a test window in front: small and
# then maximized on the other monitor, small on CapView's monitor, and moved
# back and maximized. Then CapView again. Prints the present mode and dropped
# frames PresentMon saw in each step, and whether CapView was topmost. It ends
# by putting back the window that covered CapView, if one did, and giving focus
# back to the window that had it. Takes about 6 x HoldSeconds.

# Add-Type in PowerShell 7 can't resolve the WinForms references, so run in 5.1.
if ($PSVersionTable.PSEdition -eq 'Core') {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -HoldSeconds $HoldSeconds -PresentMon $PresentMon -Csv $Csv
    return
}
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -Path (Join-Path $PSScriptRoot "FlipCheck.cs")
Remove-Item $Csv -ErrorAction SilentlyContinue
$total = 6 * $HoldSeconds + 10
$pm = Start-Process $PresentMon -ArgumentList '--process_name', 'CapView.exe', '--output_file', $Csv, '--timed', $total,
    '--terminate_after_timed', '--no_console_stats', '--qpc_time' -PassThru -WindowStyle Hidden
# PresentMon's trace session takes a few seconds before it records anything.
Start-Sleep 3
$marks = [FlipCheck]::Run($HoldSeconds * 1000)
$pm.WaitForExit(($total + 20) * 1000) | Out-Null

$steps = foreach ($m in $marks) { if ($m -match '^q(\d+) (.*)$') { [pscustomobject]@{ Q = [double]$matches[1] / 1e7; Name = $matches[2] } } }
$rows = @(Import-Csv $Csv)
for ($i = 0; $i -lt $steps.Count - 1; $i++) {
    $a = $steps[$i].Q; $b = $steps[$i + 1].Q - 0.3
    $seg = @($rows | Where-Object { $q = [double]$_.TimeInQPC / 1e7; $q -ge $a -and $q -lt $b })
    $modes = ($seg | Group-Object PresentMode | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', '
    $lost = @($seg | Where-Object { $_.MsUntilDisplayed -eq 'NA' }).Count
    $name, $state = $steps[$i].Name -split ' \| ', 2
    "{0,-48} {1,4} presents, {2,4} not shown | {3} | {4}" -f $name, $seg.Count, $lost, $modes, $state
}
