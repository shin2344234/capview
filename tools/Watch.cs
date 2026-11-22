using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// Logs every change to what can sit over CapView's window: the foreground
// window, visible windows above CapView in z-order that overlap it, the
// cursor, the shell notification state and processes that start or stop.
// Loaded by watch.ps1.
public static class CapWatch
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string c, string t);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr h, int i);
    [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CURSORINFO ci);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int s);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT v, int s);
    [DllImport("shell32.dll")] static extern int SHQueryUserNotificationState(out int state);

    static readonly Dictionary<int, string> names = new Dictionary<int, string>();

    static string ProcName(int pid)
    {
        string n;
        if (names.TryGetValue(pid, out n)) return n;
        try { n = Process.GetProcessById(pid).ProcessName; } catch { n = "pid" + pid; }
        names[pid] = n;
        return n;
    }

    static string Describe(IntPtr h, bool withRect)
    {
        if (h == IntPtr.Zero) return "(none)";
        int pid; GetWindowThreadProcessId(h, out pid);
        var t = new StringBuilder(200); GetWindowTextW(h, t, 200);
        var c = new StringBuilder(200); GetClassNameW(h, c, 200);
        string s = string.Format("{0} [{1}] '{2}'", ProcName(pid), c, t);
        if (withRect)
        {
            RECT r;
            if (DwmGetWindowAttribute(h, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out r);
            s += string.Format(" {0},{1} to {2},{3} ex 0x{4:X}", r.L, r.T, r.R, r.B, GetWindowLongW(h, -20));
        }
        return s;
    }

    public static void Run(string logPath, int seconds, int intervalMs)
    {
        using (var log = new StreamWriter(logPath, false, new UTF8Encoding(false)))
        {
            log.AutoFlush = true;
            // Each line carries the QPC value, the clock PresentMon's --qpc_time uses.
            Action<string> W = msg => log.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + " q" + Stopwatch.GetTimestamp() + " " + msg);
            string lastFg = null, lastAbove = null, lastCursor = null, lastCv = null;
            int lastNotif = -1;
            var procs = new Dictionary<int, string>();
            foreach (var p in Process.GetProcesses()) procs[p.Id] = p.ProcessName;
            var sw = Stopwatch.StartNew();
            long nextProcScan = 1000;
            W("watch start");
            while (sw.ElapsedMilliseconds < seconds * 1000L)
            {
                IntPtr cv = FindWindowW("CapViewWindow", null);
                RECT r = new RECT();
                string cvs = "CapView window missing";
                if (cv != IntPtr.Zero)
                {
                    GetWindowRect(cv, out r);
                    cvs = string.Format("CapView window {0},{1} to {2},{3} style 0x{4:X}", r.L, r.T, r.R, r.B, GetWindowLongW(cv, -16));
                }
                if (cvs != lastCv) { W(cvs); lastCv = cvs; }

                string fg = Describe(GetForegroundWindow(), true);
                if (fg != lastFg) { W("foreground " + fg); lastFg = fg; }

                var above = new StringBuilder();
                if (cv != IntPtr.Zero)
                {
                    for (IntPtr h = GetWindow(cv, 3); h != IntPtr.Zero; h = GetWindow(h, 3))
                    {
                        if (!IsWindowVisible(h)) continue;
                        int cloaked; DwmGetWindowAttribute(h, 14, out cloaked, 4);
                        if (cloaked != 0) continue;
                        // Overlap uses the full window rect, invisible resize border
                        // included. Describe prints the visible frame, so a window
                        // that only reaches CapView with its border shows up as such.
                        RECT w; GetWindowRect(h, out w);
                        if (w.R <= w.L || w.B <= w.T) continue;
                        if (w.R <= r.L || w.L >= r.R || w.B <= r.T || w.T >= r.B) continue;
                        above.Append("\n    ").Append(Describe(h, true));
                    }
                }
                string ab = above.Length == 0 ? " none" : above.ToString();
                if (ab != lastAbove) { W("above CapView:" + ab); lastAbove = ab; }

                var ci = new CURSORINFO(); ci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
                GetCursorInfo(ref ci);
                bool over = cv != IntPtr.Zero && ci.pt.X >= r.L && ci.pt.X < r.R && ci.pt.Y >= r.T && ci.pt.Y < r.B;
                string cur = string.Format("cursor flags {0} handle 0x{1:X} {2}", ci.flags, ci.hCursor.ToInt64(), over ? "over CapView" : "elsewhere");
                if (cur != lastCursor) { W(cur + string.Format(" at {0},{1}", ci.pt.X, ci.pt.Y)); lastCursor = cur; }

                int ns; SHQueryUserNotificationState(out ns);
                if (ns != lastNotif) { W("notification state " + ns); lastNotif = ns; }

                if (sw.ElapsedMilliseconds >= nextProcScan)
                {
                    nextProcScan += 1000;
                    var now = new Dictionary<int, string>();
                    foreach (var p in Process.GetProcesses()) now[p.Id] = p.ProcessName;
                    foreach (var kv in now) if (!procs.ContainsKey(kv.Key)) W("process start " + kv.Value + " " + kv.Key);
                    foreach (var kv in procs) if (!now.ContainsKey(kv.Key)) W("process exit " + kv.Value + " " + kv.Key);
                    procs = now;
                }
                Thread.Sleep(intervalMs);
            }
            W("watch end");
        }
    }
}
