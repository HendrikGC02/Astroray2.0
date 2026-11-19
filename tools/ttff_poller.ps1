# SPDX-License-Identifier: Apache-2.0
# tools/ttff_poller.ps1 -ProcessId <blender pid> -OutPrefix <path> -StopFile <path> [-RegionX -RegionY -RegionW -RegionH]
#   [-CropW 960 -CropH 540]
# The screen poller of the first-frame measurement (0007 d35 V7a): copies the screen rectangle of one
# region of Blender's own window (found from the PID, never any other window) as fast as it can
# (CopyFromScreen of a centred crop, spike B's method) and keeps, per capture, the wall-clock time
# (UTC epoch seconds, the clock of Python's time.time()) and a 32 x 18 grid of block means.
# The region is given in client pixels of the window, y from the top. It stops when -StopFile exists.
# Every 16th capture it checks that the windows at five points of the crop are Blender's own (foreign_checks
# counts the failures; a run with any is not valid). Writes <OutPrefix>.bin (records: float64 time, 32*18*3 bytes B,G,R per block) and <OutPrefix>.json
# (epoch of the start, grid, rectangle, capture count, achieved rate). Detection is done by the caller.
# Ref: docs/decisions/0007-blender-integration.md d35; docs/spikes/B-blender-end-to-end/RESULTS.md section 3.
param(
  [Parameter(Mandatory)][int]$ProcessId,
  [Parameter(Mandatory)][string]$OutPrefix,
  [Parameter(Mandatory)][string]$StopFile,
  [int]$RegionX = 0, [int]$RegionY = 0, [int]$RegionW = 0, [int]$RegionH = 0,
  [int]$CropW = 960, [int]$CropH = 540,
  [double]$MaxSeconds = 120,
  [switch]$PrintWindow,   # capture with PrintWindow(PW_RENDERFULLCONTENT) of the whole window instead of CopyFromScreen
  [string]$DumpPng = ''   # debugging: the last crop is saved here, only when every check found Blender's own window
)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies 'System.Drawing','System.Drawing.Common','System.Drawing.Primitives','System.Diagnostics.Process','System.ComponentModel.Primitives','System.Runtime.InteropServices','System.Collections','System.Threading.Thread' -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class TtffCapture {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int value, int size);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  public static string WindowState = "";
  public static string State(IntPtr h) {
    int cloaked; DwmGetWindowAttribute(h, 14, out cloaked, 4);
    return "visible=" + IsWindowVisible(h) + " iconic=" + IsIconic(h) + " cloaked=" + cloaked + " foreground=" + (GetForegroundWindow() == h);
  }
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  // True when the window under (x, y) belongs to the process: the crop is Blender's own pixels.
  static bool Owned(int x, int y, int pid) {
    POINT p = new POINT(); p.X = x; p.Y = y;
    IntPtr top = GetAncestor(WindowFromPoint(p), 2);
    uint owner; GetWindowThreadProcessId(top, out owner);
    return owner == (uint)pid;
  }

  public const int GX = 32, GY = 18;
  public static int Client(IntPtr h, out int sx, out int sy, out int w, out int hgt) {
    RECT r; POINT p = new POINT();
    GetClientRect(h, out r); ClientToScreen(h, ref p);
    sx = p.X; sy = p.Y; w = r.Right; hgt = r.Bottom; return 0;
  }
  // Returns the capture count; fills elapsed seconds.
  public static int Run(int pid, string binPath, int rx, int ry, int rw, int rh, int cw, int ch,
                        string stopFile, double maxSeconds, string dumpPng, bool printWindow, out double epoch0, out double elapsed,
                        out int sx, out int sy, out int cwin, out int chwin, out int px, out int py, out int pw, out int ph, out int foreign) {
    foreign = 0;
    SetProcessDPIAware();
    var proc = Process.GetProcessById(pid);
    proc.Refresh();
    IntPtr h = proc.MainWindowHandle;
    if (h == IntPtr.Zero) throw new Exception("process " + pid + " has no main window");
    ShowWindow(h, 9);
    SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040);   // topmost, no move, no size, show
    SetForegroundWindow(h);
    System.Threading.Thread.Sleep(300);
    WindowState = State(h);
    Client(h, out sx, out sy, out cwin, out chwin);
    if (rw <= 0) { rx = 0; ry = 0; rw = cwin; rh = chwin; }
    pw = Math.Min(cw, rw); ph = Math.Min(ch, rh);
    px = sx + rx + (rw - pw) / 2; py = sy + ry + (rh - ph) / 2;
    pw -= pw % GX; ph -= ph % GY;   // whole blocks
    int bw = pw / GX, bh = ph / GY;
    var bmp = new Bitmap(pw, ph, PixelFormat.Format32bppArgb);
    var g = Graphics.FromImage(bmp);
    var size = new Size(pw, ph);
    var full = new Bitmap(cwin, chwin, PixelFormat.Format32bppArgb);
    var gw = Graphics.FromImage(full);
    var buf = new byte[pw * ph * 4];
    var sums = new long[GX * GY * 3];
    var rec = new byte[8 + GX * GY * 3];
    var sw = Stopwatch.StartNew();
    epoch0 = (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
    int n = 0;
    using (var fs = new FileStream(binPath, FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 20)) {
      while (sw.Elapsed.TotalSeconds < maxSeconds) {
        if (printWindow) {
          IntPtr hdc = gw.GetHdc();
          PrintWindow(h, hdc, 2);
          gw.ReleaseHdc(hdc);
          g.DrawImage(full, new Rectangle(0, 0, pw, ph), new Rectangle(px - sx, py - sy, pw, ph), GraphicsUnit.Pixel);
        } else {
          g.CopyFromScreen(px, py, 0, 0, size, CopyPixelOperation.SourceCopy);
        }
        double t = epoch0 + sw.Elapsed.TotalSeconds;
        var bd = bmp.LockBits(new Rectangle(0, 0, pw, ph), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(bd.Scan0, buf, 0, buf.Length);
        bmp.UnlockBits(bd);
        Array.Clear(sums, 0, sums.Length);
        for (int y = 0; y < ph; y++) {
          int row = y * pw * 4, by = (y / bh) * GX;
          for (int x = 0; x < pw; x++) {
            int o = row + x * 4, b = (by + x / bw) * 3;
            sums[b] += buf[o]; sums[b + 1] += buf[o + 1]; sums[b + 2] += buf[o + 2];
          }
        }
        int cnt = bw * bh;
        BitConverter.GetBytes(t).CopyTo(rec, 0);
        for (int i = 0; i < sums.Length; i++) rec[8 + i] = (byte)(sums[i] / cnt);
        fs.Write(rec, 0, rec.Length);
        n++;
        if ((n & 15) == 0) {
          if (!(Owned(px + 2, py + 2, pid) && Owned(px + pw - 3, py + 2, pid) && Owned(px + 2, py + ph - 3, pid)
                && Owned(px + pw - 3, py + ph - 3, pid) && Owned(px + pw / 2, py + ph / 2, pid))) foreign++;
          if (File.Exists(stopFile)) break;
        }
      }
    }
    elapsed = sw.Elapsed.TotalSeconds;
    if (dumpPng != "" && foreign == 0) bmp.Save(dumpPng, ImageFormat.Png);
    return n;
  }
}
'@

$bin = "$OutPrefix.bin"
[double]$e0 = 0; [double]$el = 0
[int]$sx = 0; [int]$sy = 0; [int]$cw = 0; [int]$chh = 0; [int]$px = 0; [int]$py = 0; [int]$pw = 0; [int]$ph = 0; [int]$foreign = 0
$ready = "$OutPrefix.ready"
Set-Content -Path $ready -Value $ProcessId
$n = [TtffCapture]::Run($ProcessId, $bin, $RegionX, $RegionY, $RegionW, $RegionH, $CropW, $CropH, $StopFile,
  $MaxSeconds, $DumpPng, [bool]$PrintWindow, [ref]$e0, [ref]$el, [ref]$sx, [ref]$sy, [ref]$cw, [ref]$chh, [ref]$px, [ref]$py, [ref]$pw, [ref]$ph, [ref]$foreign)
$meta = [ordered]@{
  pid = $ProcessId; epoch0 = $e0; seconds = $el; captures = $n; rate_hz = [math]::Round($n / $el, 1)
  grid = @(32, 18); client_origin = @($sx, $sy); client_size = @($cw, $chh)
  capture_rect = @($px, $py, $pw, $ph); foreign_checks = $foreign; record_bytes = 8 + 32 * 18 * 3
}
$meta | ConvertTo-Json -Compress | Set-Content -Path "$OutPrefix.json"
Write-Host "ttff_poller: window $([TtffCapture]::WindowState)"
Write-Host "ttff_poller: $n captures in $([math]::Round($el, 2)) s ($($meta.rate_hz) Hz)"
