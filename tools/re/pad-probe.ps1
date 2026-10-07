<#
.SYNOPSIS
  Lists the game pads Windows sees: XInput users 0 to 3 (through the system xinput1_3.dll and
  xinput1_4.dll) and the HID joysticks and gamepads (raw input), with their vendor and product ids.

.DESCRIPTION
  Read only. Answers "is a pad connected, and which input path can the game read it through":
  an XInput pad reaches the game through XInputGetState (and so through the mod's proxy); a pad
  that only appears as a HID game controller (DualShock 4, DualSense, many generic pads without
  Steam Input) is read by the game's own DirectInput/HID code instead.

.PARAMETER Seconds
  Poll the XInput users for this many seconds and print every button change (0 = one sample).
#>
param([double]$Seconds = 0)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class PadProbe {
    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_GAMEPAD { public ushort wButtons; public byte bLeftTrigger, bRightTrigger; public short sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_STATE { public uint dwPacketNumber; public XINPUT_GAMEPAD Gamepad; }

    [DllImport("xinput1_3.dll", EntryPoint = "XInputGetState")] static extern uint GetState13(uint user, out XINPUT_STATE s);
    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")] static extern uint GetState14(uint user, out XINPUT_STATE s);

    public static string XInput(int dll, uint user, out ushort buttons) {
        XINPUT_STATE s;
        uint rc = dll == 13 ? GetState13(user, out s) : GetState14(user, out s);
        buttons = s.Gamepad.wButtons;
        if (rc == 1167) return "not connected";
        if (rc != 0) return "error " + rc;
        return String.Format("connected, packet {0}, buttons 0x{1:x4}, triggers {2}/{3}, sticks L({4},{5}) R({6},{7})", s.dwPacketNumber, s.Gamepad.wButtons,
            s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger, s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
    }

    [StructLayout(LayoutKind.Sequential)]
    struct RAWINPUTDEVICELIST { public IntPtr hDevice; public uint dwType; }
    [StructLayout(LayoutKind.Sequential)]
    struct RID_DEVICE_INFO_HID { public uint dwVendorId, dwProductId, dwVersionNumber; public ushort usUsagePage, usUsage; }
    [StructLayout(LayoutKind.Explicit)]
    struct RID_DEVICE_INFO { [FieldOffset(0)] public uint cbSize; [FieldOffset(4)] public uint dwType; [FieldOffset(8)] public RID_DEVICE_INFO_HID hid; [FieldOffset(8)] public ulong pad0; [FieldOffset(16)] public ulong pad1; }

    [DllImport("user32.dll")] static extern uint GetRawInputDeviceList([Out] RAWINPUTDEVICELIST[] list, ref uint count, uint size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetRawInputDeviceInfoW")] static extern uint GetInfoName(IntPtr dev, uint cmd, StringBuilder data, ref uint size);
    [DllImport("user32.dll", EntryPoint = "GetRawInputDeviceInfoW")] static extern uint GetInfo(IntPtr dev, uint cmd, ref RID_DEVICE_INFO data, ref uint size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("hid.dll", CharSet = CharSet.Unicode)] static extern bool HidD_GetProductString(SafeFileHandle h, StringBuilder buf, uint len);
    [DllImport("hid.dll", CharSet = CharSet.Unicode)] static extern bool HidD_GetManufacturerString(SafeFileHandle h, StringBuilder buf, uint len);

    public static List<string> HidControllers(bool all) {
        var outp = new List<string>();
        uint n = 0, sz = (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICELIST));
        GetRawInputDeviceList(null, ref n, sz);
        var list = new RAWINPUTDEVICELIST[n];
        if (GetRawInputDeviceList(list, ref n, sz) == uint.MaxValue) { outp.Add("GetRawInputDeviceList failed"); return outp; }
        foreach (var d in list) {
            if (d.dwType != 2) continue;  // RIM_TYPEHID
            var info = new RID_DEVICE_INFO(); info.cbSize = (uint)Marshal.SizeOf(typeof(RID_DEVICE_INFO));
            uint isz = info.cbSize;
            if (GetInfo(d.hDevice, 0x2000000b, ref info, ref isz) == uint.MaxValue) continue;  // RIDI_DEVICEINFO
            var h = info.hid;
            bool pad = h.usUsagePage == 1 && (h.usUsage == 4 || h.usUsage == 5 || h.usUsage == 8);
            if (!pad && !all) continue;
            uint nsz = 512; var name = new StringBuilder(512);
            GetInfoName(d.hDevice, 0x20000007, name, ref nsz);  // RIDI_DEVICENAME
            string product = "", maker = "";
            using (var f = CreateFileW(name.ToString(), 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
                if (!f.IsInvalid) {
                    var b = new StringBuilder(256);
                    if (HidD_GetProductString(f, b, 512)) product = b.ToString();
                    b.Clear();
                    if (HidD_GetManufacturerString(f, b, 512)) maker = b.ToString();
                }
            }
            string kind = h.usUsage == 4 ? "joystick" : h.usUsage == 5 ? "gamepad" : h.usUsage == 8 ? "multi-axis" : String.Format("usage {0:x}:{1:x}", h.usUsagePage, h.usUsage);
            bool xinputHid = name.ToString().IndexOf("IG_", StringComparison.OrdinalIgnoreCase) >= 0;
            outp.Add(String.Format("{0}: VID {1:x4} PID {2:x4} '{3}' '{4}'{5}  {6}", kind, h.dwVendorId, h.dwProductId, maker, product,
                xinputHid ? " (XInput device, IG_ in the path)" : "", name));
        }
        return outp;
    }
}
'@

Write-Output "XInput (system DLLs; 1167 = not connected):"
foreach ($dll in 13, 14) {
    foreach ($u in 0..3) {
        $b = [uint16]0
        try { $r = [PadProbe]::XInput($dll, [uint32]$u, [ref]$b) } catch { $r = "call failed: $($_.Exception.Message)" }
        Write-Output ("  xinput1_{0} user {1}: {2}" -f ($(if ($dll -eq 13) { '3' } else { '4' })), $u, $r)
    }
}
Write-Output "HID joysticks and gamepads (raw input):"
$hid = [PadProbe]::HidControllers($false)
if ($hid.Count -eq 0) { Write-Output "  none" } else { $hid | ForEach-Object { Write-Output "  $_" } }

if ($Seconds -gt 0) {
    Write-Output "Watching XInput buttons for $Seconds s:"
    $last = @{}
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        foreach ($u in 0..3) {
            $b = [uint16]0
            $r = [PadProbe]::XInput(14, [uint32]$u, [ref]$b)
            $key = "$u"
            $v = if ($r -like 'connected*') { '{0:x4}' -f $b } else { $r }
            if ($last[$key] -ne $v) { Write-Output ("  {0,8:n0} ms user {1}: {2}" -f $sw.Elapsed.TotalMilliseconds, $u, $v); $last[$key] = $v }
        }
        Start-Sleep -Milliseconds 4
    }
}
