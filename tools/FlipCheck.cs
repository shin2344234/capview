using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

// Steps through the cases that can push CapView out of Independent Flip and
// records the QPC time of each, so flipcheck.ps1 can match them to PresentMon.
// Each mark also says whether CapView is topmost and whether the test window
// sits above it. Loaded by flipcheck.ps1.
public static class FlipCheck
{
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string c, string t);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(int a, int b, bool attach);
    [DllImport("kernel32.dll")] static extern int GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr h, int i);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT v, int s);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int s);

    // SetForegroundWindow from a process that isn't in front only works while
    // its input is attached to the thread that owns the current foreground window.
    static void Front(IntPtr h)
    {
        int fgThread = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        int me = GetCurrentThreadId();
        AttachThreadInput(me, fgThread, true);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(me, fgThread, false);
    }

    static IntPtr cv, form;
    static List<string> marks;

    static bool Above(IntPtr w)
    {
        for (IntPtr h = GetWindow(cv, 3); h != IntPtr.Zero; h = GetWindow(h, 3)) if (h == w) return true;
        return false;
    }

    static void Mark(string what)
    {
        bool top = (GetWindowLongW(cv, -20) & 8) != 0;
        marks.Add("q" + Stopwatch.GetTimestamp() + " " + what + " | topmost " + (top ? "yes" : "no") +
                  ", test window " + (Above(form) ? "above" : "below") + " CapView");
    }

    static void Hold(int ms)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms) { Application.DoEvents(); Thread.Sleep(10); }
    }

    // The highest ordinary window above CapView whose visible frame overlaps it,
    // so the test can put it back in front at the end.
    static IntPtr Coverer()
    {
        RECT c; GetWindowRect(cv, out c);
        IntPtr found = IntPtr.Zero;
        for (IntPtr h = GetWindow(cv, 3); h != IntPtr.Zero; h = GetWindow(h, 3))
        {
            if (!IsWindowVisible(h) || IsIconic(h) || (GetWindowLongW(h, -20) & 8) != 0) continue;
            int cloaked; DwmGetWindowAttribute(h, 14, out cloaked, 4);
            if (cloaked != 0) continue;
            RECT r;
            if (DwmGetWindowAttribute(h, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out r);
            if (r.R > c.L && r.L < c.R && r.B > c.T && r.T < c.B) found = h;
        }
        return found;
    }

    public static List<string> Run(int holdMs)
    {
        marks = new List<string>();
        cv = FindWindowW("CapViewWindow", null);
        if (cv == IntPtr.Zero) throw new Exception("CapView window not found");
        IntPtr previous = GetForegroundWindow();
        IntPtr coverer = Coverer();

        Screen mine = Screen.FromHandle(cv), other = null;
        foreach (var s in Screen.AllScreens)
            if (!s.Bounds.Contains(mine.Bounds.Location)) { other = s; break; }
        if (other == null) throw new Exception("needs a second monitor");
        var onOther = new System.Drawing.Rectangle(other.WorkingArea.X + 200, other.WorkingArea.Y + 200, 600, 400);
        var onMine = new System.Drawing.Rectangle(mine.WorkingArea.X + 200, mine.WorkingArea.Y + 200, 600, 400);

        var f = new Form();
        f.Text = "CapView flip check";
        f.StartPosition = FormStartPosition.Manual;
        f.Bounds = onOther;
        f.ShowInTaskbar = true;

        Front(cv); Hold(300);
        form = f.Handle;
        Mark("CapView in front");
        Hold(holdMs);

        f.Show(); Front(form); Hold(300);
        Mark("small window on the other monitor in front");
        Hold(holdMs);

        f.WindowState = FormWindowState.Maximized; Front(form); Hold(300);
        Mark("maximized window on the other monitor in front");
        Hold(holdMs);

        f.WindowState = FormWindowState.Normal; f.Bounds = onMine; Front(form); Hold(300);
        Mark("small window on CapView's monitor in front");
        Hold(holdMs);

        f.Bounds = onOther; Hold(100); f.WindowState = FormWindowState.Maximized; Hold(300);
        Mark("same window moved back and maximized");
        Hold(holdMs);

        Front(cv); Hold(300);
        Mark("CapView in front again");
        Hold(holdMs);

        Mark("closing the test window and restoring");
        f.Close();
        if (coverer != IntPtr.Zero) { Front(coverer); Hold(300); }
        if (previous != IntPtr.Zero && previous != cv) Front(previous);
        Hold(300);
        Mark("previous windows restored");
        return marks;
    }
}
