using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// Helpers for pacecheck.ps1: reads and sets a process's GPU scheduling priority
// class, and logs every foreground window change with its QPC time so the
// script can say which app had focus during each phase.
public static class PaceCheck
{
    [DllImport("gdi32.dll")] static extern int D3DKMTSetProcessSchedulingPriorityClass(IntPtr h, int p);
    [DllImport("gdi32.dll")] static extern int D3DKMTGetProcessSchedulingPriorityClass(IntPtr h, out int p);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(int a, int b, bool attach);
    [DllImport("kernel32.dll")] static extern int GetCurrentThreadId();

    public static IntPtr Foreground() { return GetForegroundWindow(); }

    // SetForegroundWindow from a process that isn't in front only works while
    // its input is attached to the thread that owns the current foreground window.
    public static void Focus(IntPtr h)
    {
        int fgThread = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        int me = GetCurrentThreadId();
        AttachThreadInput(me, fgThread, true);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(me, fgThread, false);
    }

    const uint PROCESS_SET_INFORMATION = 0x0200, PROCESS_QUERY_INFORMATION = 0x0400;

    // 0 idle, 1 below normal, 2 normal, 3 above normal, 4 high, 5 realtime.
    // Returns the class, or -1 with the NTSTATUS in status. The read also needs
    // PROCESS_SET_INFORMATION; with query rights alone it returns access denied.
    public static int GetGpu(int pid, out int status)
    {
        IntPtr h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, false, pid);
        status = 0;
        if (h == IntPtr.Zero) { status = Marshal.GetLastWin32Error(); return -1; }
        int p;
        status = D3DKMTGetProcessSchedulingPriorityClass(h, out p);
        CloseHandle(h);
        return status == 0 ? p : -1;
    }

    // Returns the NTSTATUS, 0 on success.
    public static int SetGpu(int pid, int p)
    {
        IntPtr h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, false, pid);
        if (h == IntPtr.Zero) return Marshal.GetLastWin32Error();
        int s = D3DKMTSetProcessSchedulingPriorityClass(h, p);
        CloseHandle(h);
        return s;
    }

    static Thread sampler;
    static volatile bool stop;

    public static void StartSampler(string path)
    {
        stop = false;
        sampler = new Thread(() =>
        {
            using (var w = new StreamWriter(path, false, Encoding.UTF8))
            {
                IntPtr last = IntPtr.Zero;
                while (!stop)
                {
                    IntPtr fg = GetForegroundWindow();
                    if (fg != last)
                    {
                        last = fg;
                        int pid; GetWindowThreadProcessId(fg, out pid);
                        string name = "?";
                        try { name = Process.GetProcessById(pid).ProcessName; } catch { }
                        w.WriteLine("q" + Stopwatch.GetTimestamp() + " " + name);
                        w.Flush();
                    }
                    Thread.Sleep(50);
                }
            }
        });
        sampler.IsBackground = true;
        sampler.Start();
    }

    public static void StopSampler()
    {
        stop = true;
        if (sampler != null) sampler.Join(2000);
    }
}
