# Switches the Cam Link 4K's audio function (interface 2, MI_02) between Windows' audio driver and
# WinUSB, so camlink.py can send the card vendor requests without Zadig.
#
#   camlink_driver.ps1            shows which driver each Cam Link interface has
#   camlink_driver.ps1 -Mode List lists the drivers winusb.inf offers for MI_02 (dry run, no admin)
#   camlink_driver.ps1 -Mode WinUSB
#   camlink_driver.ps1 -Mode Audio
#
# WinUSB comes from Windows' own signed winusb.inf (the "WinUsb Device" entry Device Manager offers
# under "Let me pick"), so no certificate or driver package gets installed. The card's sound is gone
# from Windows while WinUSB is on. Audio removes the MI_02 device node and rescans, and Windows binds
# its audio driver again. WinUSB and Audio ask for admin through UAC.
param(
    [ValidateSet('Status', 'List', 'WinUSB', 'Audio')][string]$Mode = 'Status',
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'

# libusb finds a WinUSB device through its device interface. Windows' generic WinUSB entry doesn't
# register one, so the script writes this GUID into the device's hardware key before installing.
$InterfaceGuid = '{6E4C5A2B-1F0B-4C38-9D8E-2A7B3C4D5E61}'
$Match = 'USB\\VID_1209&PID_0001&MI_02\\'

function Get-CamLinkNodes {
    Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'VID_1209&PID_0001' | ForEach-Object {
        $svc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service).Data
        [pscustomobject]@{ Status = $_.Status; Class = $_.Class; Service = $svc; InstanceId = $_.InstanceId }
    }
}

function Get-Mi02 {
    $n = Get-PnpDevice -PresentOnly | Where-Object InstanceId -match $Match
    if (-not $n) { throw 'The Cam Link 4K interface 2 (MI_02) is not present.' }
    $n.InstanceId
}

if ($Mode -eq 'Status') { Get-CamLinkNodes | Format-Table -AutoSize | Out-String -Width 200; return }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (($Mode -in @('WinUSB', 'Audio')) -and -not $isAdmin) {
    $logFile = Join-Path $env:TEMP "camlink_driver_$Mode.log"
    Remove-Item $logFile -ErrorAction SilentlyContinue
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Mode', $Mode, '-Log', "`"$logFile`"")
    Start-Process powershell.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList $argList
    if (Test-Path $logFile) { Get-Content $logFile } else { 'The elevated run left no log. Was the UAC prompt declined?' }
    return
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class CamLinkDrv {
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SP_DEVINSTALL_PARAMS {
        public uint cbSize; public uint Flags; public uint FlagsEx;
        public IntPtr hwndParent; public IntPtr InstallMsgHandler; public IntPtr InstallMsgHandlerContext;
        public IntPtr FileQueue; public UIntPtr ClassInstallReserved; public uint Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string DriverPath;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SP_DRVINFO_DATA {
        public uint cbSize; public uint DriverType; public UIntPtr Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Description;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string MfgName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string ProviderName;
        public long DriverDate; public ulong DriverVersion;
    }

    const uint DI_ENUMSINGLEINF = 0x00010000;
    const uint DI_FLAGSEX_ALLOWEXCLUDEDDRVS = 0x00000800;
    const uint SPDIT_CLASSDRIVER = 1;
    const uint DIIDFLAG_NOFINISHINSTALLUI = 2;

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr ClassGuid, IntPtr hwndParent);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string id, IntPtr hwnd, uint flags, ref SP_DEVINFO_DATA d);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInstallParamsW(IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetDeviceInstallParamsW(IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiBuildDriverInfoList(IntPtr set, ref SP_DEVINFO_DATA d, uint type);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiEnumDriverInfoW(IntPtr set, ref SP_DEVINFO_DATA d, uint type, uint index, ref SP_DRVINFO_DATA drv);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetSelectedDriverW(IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DRVINFO_DATA drv);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", SetLastError = true)]
    static extern bool DiInstallDevice(IntPtr hwnd, IntPtr set, ref SP_DEVINFO_DATA d, ref SP_DRVINFO_DATA drv, uint flags, out int reboot);

    [DllImport("setupapi.dll", SetLastError = true, EntryPoint = "SetupDiGetDeviceInstallParamsW", CharSet = CharSet.Unicode)]
    static extern bool GetSetInstallParams(IntPtr set, IntPtr d, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", SetLastError = true, EntryPoint = "SetupDiSetDeviceInstallParamsW", CharSet = CharSet.Unicode)]
    static extern bool SetSetInstallParams(IntPtr set, IntPtr d, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", SetLastError = true, EntryPoint = "SetupDiBuildDriverInfoList")]
    static extern bool BuildSetDriverList(IntPtr set, IntPtr d, uint type);
    [DllImport("setupapi.dll", SetLastError = true, EntryPoint = "SetupDiEnumDriverInfoW", CharSet = CharSet.Unicode)]
    static extern bool EnumSetDriver(IntPtr set, IntPtr d, uint type, uint index, ref SP_DRVINFO_DATA drv);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA d, uint property, byte[] buf, uint size);
    const uint SPDRP_CLASSGUID = 8;

    static Exception Fail(string what) { return new Win32Exception(Marshal.GetLastWin32Error(), what + " failed"); }

    static SP_DEVINSTALL_PARAMS SingleInf(SP_DEVINSTALL_PARAMS p, string infPath) {
        p.DriverPath = infPath;
        p.Flags |= DI_ENUMSINGLEINF;
        p.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        return p;
    }

    // Dry run with no device and no admin: the class drivers infPath offers for class cls.
    public static string List(Guid cls, string infPath) {
        var log = new StringBuilder();
        IntPtr set = SetupDiCreateDeviceInfoList(ref cls, IntPtr.Zero);
        if (set == new IntPtr(-1)) throw Fail("SetupDiCreateDeviceInfoList");
        try {
            var p = new SP_DEVINSTALL_PARAMS();
            p.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINSTALL_PARAMS));
            if (!GetSetInstallParams(set, IntPtr.Zero, ref p)) throw Fail("SetupDiGetDeviceInstallParams");
            p = SingleInf(p, infPath);
            if (!SetSetInstallParams(set, IntPtr.Zero, ref p)) throw Fail("SetupDiSetDeviceInstallParams");
            if (!BuildSetDriverList(set, IntPtr.Zero, SPDIT_CLASSDRIVER)) throw Fail("SetupDiBuildDriverInfoList");
            for (uint i = 0; ; i++) {
                var drv = new SP_DRVINFO_DATA();
                drv.cbSize = (uint)Marshal.SizeOf(typeof(SP_DRVINFO_DATA));
                if (!EnumSetDriver(set, IntPtr.Zero, SPDIT_CLASSDRIVER, i, ref drv)) break;
                log.AppendLine("offered: " + drv.Description + " (" + drv.ProviderName + ")");
            }
            return log.ToString();
        } finally {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    // Moves the device into class cls, the class of infPath, then installs the driver from infPath
    // whose description is wanted. SetupAPI only offers a device the class drivers of its own class.
    public static string Install(string instanceId, Guid cls, string infPath, string wanted) {
        var log = new StringBuilder();
        IntPtr set = SetupDiCreateDeviceInfoList(IntPtr.Zero, IntPtr.Zero);
        if (set == new IntPtr(-1)) throw Fail("SetupDiCreateDeviceInfoList");
        try {
            var d = new SP_DEVINFO_DATA();
            d.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            if (!SetupDiOpenDeviceInfoW(set, instanceId, IntPtr.Zero, 0, ref d)) throw Fail("SetupDiOpenDeviceInfo");
            if (d.ClassGuid != cls) {
                byte[] g = Encoding.Unicode.GetBytes(cls.ToString("B") + "\0");
                if (!SetupDiSetDeviceRegistryPropertyW(set, ref d, SPDRP_CLASSGUID, g, (uint)g.Length)) throw Fail("Setting the class GUID");
                log.AppendLine("class set to " + d.ClassGuid.ToString("B"));
            }
            var p = new SP_DEVINSTALL_PARAMS();
            p.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINSTALL_PARAMS));
            if (!SetupDiGetDeviceInstallParamsW(set, ref d, ref p)) throw Fail("SetupDiGetDeviceInstallParams");
            p = SingleInf(p, infPath);
            if (!SetupDiSetDeviceInstallParamsW(set, ref d, ref p)) throw Fail("SetupDiSetDeviceInstallParams");
            if (!SetupDiBuildDriverInfoList(set, ref d, SPDIT_CLASSDRIVER)) throw Fail("SetupDiBuildDriverInfoList");
            var pick = new SP_DRVINFO_DATA();
            bool found = false;
            for (uint i = 0; ; i++) {
                var drv = new SP_DRVINFO_DATA();
                drv.cbSize = (uint)Marshal.SizeOf(typeof(SP_DRVINFO_DATA));
                if (!SetupDiEnumDriverInfoW(set, ref d, SPDIT_CLASSDRIVER, i, ref drv)) break;
                log.AppendLine("offered: " + drv.Description + " (" + drv.ProviderName + ")");
                if (!found && drv.Description == wanted) { pick = drv; found = true; }
            }
            if (!found) throw new Exception(infPath + " offers no '" + wanted + "' driver for " + instanceId);
            if (!SetupDiSetSelectedDriverW(set, ref d, ref pick)) throw Fail("SetupDiSetSelectedDriver");
            int reboot;
            if (!DiInstallDevice(IntPtr.Zero, set, ref d, ref pick, DIIDFLAG_NOFINISHINSTALLUI, out reboot)) throw Fail("DiInstallDevice");
            log.AppendLine("installed " + wanted + (reboot != 0 ? ", Windows wants a reboot" : ""));
            return log.ToString();
        } finally {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
}
'@

$inf = Join-Path $env:windir 'INF\winusb.inf'
$usbDeviceClass = [guid]'{88BAE032-5A81-49F0-BC3D-A4FF138216D6}'
& {
    try {
        $id = Get-Mi02
        "MI_02 is $id"
        switch ($Mode) {
            'List' { [CamLinkDrv]::List($usbDeviceClass, $inf) }
            'WinUSB' {
                $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
                if (-not (Test-Path $key)) { New-Item $key -Force | Out-Null }
                New-ItemProperty $key -Name DeviceInterfaceGUIDs -PropertyType MultiString -Value @($InterfaceGuid) -Force | Out-Null
                $r = [CamLinkDrv]::Install($id, $usbDeviceClass, $inf, 'WinUsb Device')
                $r
                # An app holding the card's audio keeps the old driver loaded, and Windows asks for
                # a reboot. Restarting the interface loads WinUSB once the app lets go.
                if ($r -match 'reboot') { pnputil /restart-device $id }
            }
            'Audio' {
                pnputil /remove-device $id
                pnputil /scan-devices
                Start-Sleep -Seconds 2
            }
        }
        Get-CamLinkNodes | Format-Table -AutoSize | Out-String -Width 200
        'done'
    } catch {
        "error: $($_.Exception.Message)"
    }
} *>&1 | ForEach-Object { if ($Log) { $_ | Out-File $Log -Append -Encoding utf8 } else { $_ } }
