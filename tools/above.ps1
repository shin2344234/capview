# Lists the visible windows above CapView in z-order that overlap its window.
# Any of them can push CapView out of Independent Flip into Composed Flip.
Add-Type -Namespace AB -Name W -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string c, string t);
[DllImport("user32.dll")] public static extern System.IntPtr GetWindow(System.IntPtr h, uint cmd);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(System.IntPtr h, out int pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern int GetWindowLongW(System.IntPtr h, int i);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(System.IntPtr h, int a, out int v, int s);
public struct RECT { public int L, T, R, B; }
'@
$cv = [AB.W]::FindWindowW("CapViewWindow", [NullString]::Value)
if ($cv -eq [IntPtr]::Zero) { throw "CapView window not found" }
$r = New-Object AB.W+RECT; [AB.W]::GetWindowRect($cv, [ref]$r) | Out-Null
"CapView at {0},{1} to {2},{3}" -f $r.L, $r.T, $r.R, $r.B
$h = [AB.W]::GetWindow($cv, 3)   # GW_HWNDPREV, the window above
while ($h -ne [IntPtr]::Zero) {
    $cloaked = 0; [AB.W]::DwmGetWindowAttribute($h, 14, [ref]$cloaked, 4) | Out-Null
    if ([AB.W]::IsWindowVisible($h) -and $cloaked -eq 0) {
        $w = New-Object AB.W+RECT; [AB.W]::GetWindowRect($h, [ref]$w) | Out-Null
        if ($w.R -gt $r.L -and $w.L -lt $r.R -and $w.B -gt $r.T -and $w.T -lt $r.B -and $w.R -gt $w.L -and $w.B -gt $w.T) {
            $pid2 = 0; [AB.W]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
            $t = New-Object Text.StringBuilder 256; [AB.W]::GetWindowTextW($h, $t, 256) | Out-Null
            $c = New-Object Text.StringBuilder 256; [AB.W]::GetClassNameW($h, $c, 256) | Out-Null
            $ex = [AB.W]::GetWindowLongW($h, -20)
            "{0} [{1}] '{2}' {3},{4} to {5},{6} exstyle 0x{7:X}" -f (Get-Process -Id $pid2 -ErrorAction SilentlyContinue).ProcessName, $c, $t, $w.L, $w.T, $w.R, $w.B, $ex
        }
    }
    $h = [AB.W]::GetWindow($h, 3)
}
