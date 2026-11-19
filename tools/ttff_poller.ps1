# SPDX-License-Identifier: Apache-2.0
# tools/ttff_poller.ps1 -ProcessId <blender pid> -OutPrefix <path> -StopFile <path> [-RegionX -RegionY -RegionW -RegionH]
#   [-CropW 960 -CropH 540]
# The window poller of the first-frame measurement (0007 d35 V7a). Capture method: Windows.Graphics.Capture of Blender's
# own window (a GraphicsCaptureItem from the process's main window handle, a free-threaded frame pool polled with
# TryGetNextFrame), not GDI: CopyFromScreen and PrintWindow return only the window background for Blender's swapchain on
# this machine. pwsh 7 has no WinRT projection, so the WinRT objects are called through their vtables (slot numbers read
# from Windows.Graphics.winmd). Frames are GPU textures; only the crop (a centred part of the viewport region) is copied
# to a staging texture and reduced to 32 x 18 block means, so nothing but those numbers leaves this process, and no other
# window or the desktop is ever read.
# A record is written for every new frame (its time is the frame's SystemRelativeTime, the QPC time at which the
# compositor produced it, converted to the wall clock of Python's time.time()) and a heartbeat record repeating the last
# blocks every 5 ms while the picture is still, so the file holds >= 100 records per second. json: captures (records),
# new_frames (distinct frames), rate_hz, new_frame_hz.
# The region is given in client pixels of the window, y from the top. It stops when -StopFile exists. Writes
# <OutPrefix>.bin (records: float64 time, 32*18*3 bytes B,G,R per block; the caller sorts by time) and <OutPrefix>.json.
# Detection is done by the caller (tools/ttff_probe.py).
# Ref: Microsoft Windows.Graphics.Capture (GraphicsCaptureItem, Direct3D11CaptureFramePool.CreateFreeThreaded,
# IGraphicsCaptureItemInterop::CreateForWindow); docs/decisions/0007-blender-integration.md d35;
# docs/spikes/B-blender-end-to-end/RESULTS.md section 3.
param(
  [Parameter(Mandatory)][int]$ProcessId,
  [Parameter(Mandatory)][string]$OutPrefix,
  [Parameter(Mandatory)][string]$StopFile,
  [int]$RegionX = 0, [int]$RegionY = 0, [int]$RegionW = 0, [int]$RegionH = 0,
  [int]$CropW = 960, [int]$CropH = 540,
  [double]$MaxSeconds = 120,
  [string]$DumpPng = ''   # debugging: the last crop is saved here
)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies 'System.Drawing', 'System.Drawing.Common', 'System.Drawing.Primitives', 'System.Diagnostics.Process', 'System.ComponentModel.Primitives', 'System.Runtime.InteropServices', 'System.Collections', 'System.Threading.Thread' -TypeDefinition @'
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
  [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT value, int size);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int value, int size);
  [DllImport("d3d11.dll")] static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software, uint flags,
    IntPtr levels, uint nLevels, uint sdk, out IntPtr device, out int level, out IntPtr context);
  [DllImport("d3d11.dll")] static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);
  [DllImport("combase.dll")] static extern int RoInitialize(int type);
  [DllImport("combase.dll", CharSet = CharSet.Unicode)] static extern int WindowsCreateString(string src, int len, out IntPtr hstring);
  [DllImport("combase.dll")] static extern int WindowsDeleteString(IntPtr hstring);
  [DllImport("combase.dll")] static extern int RoGetActivationFactory(IntPtr classId, ref Guid iid, out IntPtr factory);

  public static string WindowState = "";
  public static int Timeouts;

  // COM calls through the vtable; slot numbers are those of the Windows SDK headers (IUnknown 0..2, then the interface's
  // methods in declaration order) and, for WinRT interfaces, of Windows.Graphics.winmd (IInspectable occupies 3..5).
  delegate int FnRelease(IntPtr self);
  delegate int FnQI(IntPtr self, ref Guid iid, out IntPtr obj);
  delegate int FnCreateTex(IntPtr self, IntPtr desc, IntPtr init, out IntPtr tex);   // ID3D11Device::CreateTexture2D (5)
  delegate void FnCopyRegion(IntPtr self, IntPtr dst, uint dstSub, uint x, uint y, uint z, IntPtr src, uint srcSub, IntPtr box); // CopySubresourceRegion (46)
  delegate int FnMap(IntPtr self, IntPtr res, uint sub, uint type, uint flags, IntPtr mapped);   // ID3D11DeviceContext::Map (14)
  delegate void FnUnmap(IntPtr self, IntPtr res, uint sub);                                      // Unmap (15)
  delegate int FnForWindow(IntPtr self, IntPtr hwnd, ref Guid iid, out IntPtr item);   // IGraphicsCaptureItemInterop::CreateForWindow (3)
  delegate int FnOutLong(IntPtr self, out long v);   // GraphicsCaptureItem.get_Size (7); frame get_SystemRelativeTime (7), get_ContentSize (8)
  delegate int FnCreatePool(IntPtr self, IntPtr device, int format, int buffers, long size, out IntPtr pool);   // Statics2::CreateFreeThreaded (6)
  delegate int FnOutPtr(IntPtr self, out IntPtr p);   // pool TryGetNextFrame (7); frame get_Surface (6)
  delegate int FnCreateSession(IntPtr self, IntPtr item, out IntPtr session);   // pool CreateCaptureSession (10)
  delegate int FnVoid(IntPtr self);   // session StartCapture (6); IClosable::Close (6)
  delegate int FnGetInterface(IntPtr self, ref Guid iid, out IntPtr obj);   // IDirect3DDxgiInterfaceAccess::GetInterface (3)
  static IntPtr Slot(IntPtr com, int i) { return Marshal.ReadIntPtr(Marshal.ReadIntPtr(com), i * IntPtr.Size); }
  static T Fx<T>(IntPtr com, int i) where T : class { return (T)(object)Marshal.GetDelegateForFunctionPointer(Slot(com, i), typeof(T)); }
  static void Check(int hr, string what) { if (hr < 0) throw new Exception(what + " failed: 0x" + hr.ToString("X8")); }
  static void Release(IntPtr com) { if (com != IntPtr.Zero) Fx<FnRelease>(com, 2)(com); }
  static IntPtr Query(IntPtr com, Guid iid) { IntPtr o; Check(Fx<FnQI>(com, 0)(com, ref iid, out o), "QueryInterface"); return o; }
  static IntPtr Factory(string cls, Guid iid) {
    IntPtr hs; Check(WindowsCreateString(cls, cls.Length, out hs), "WindowsCreateString");
    IntPtr f; int hr = RoGetActivationFactory(hs, ref iid, out f); WindowsDeleteString(hs);
    Check(hr, "RoGetActivationFactory(" + cls + ")"); return f;
  }

  public const int GX = 32, GY = 18;

  public static int Run(int pid, string binPath, int rx, int ry, int rw, int rh, int cw, int ch,
                        string stopFile, double maxSeconds, string dumpPng, out double epoch0, out double elapsed,
                        out int csx, out int csy, out int cwin, out int chwin, out int cropx, out int cropy, out int pw, out int ph,
                        out int newFrames, out int frameW, out int frameH) {
    newFrames = 0; Timeouts = 0;
    SetProcessDPIAware();
    var proc = Process.GetProcessById(pid);
    proc.Refresh();
    IntPtr h = proc.MainWindowHandle;
    if (h == IntPtr.Zero) throw new Exception("process " + pid + " has no main window");
    ShowWindow(h, 9);
    SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040);   // topmost, no move, no size, show: Blender draws only a visible window
    SetForegroundWindow(h);
    System.Threading.Thread.Sleep(300);
    int cloaked; DwmGetWindowAttribute(h, 14, out cloaked, 4);
    WindowState = "visible=" + IsWindowVisible(h) + " iconic=" + IsIconic(h) + " cloaked=" + cloaked + " foreground=" + (GetForegroundWindow() == h);
    RECT r; POINT p = new POINT();
    GetClientRect(h, out r); ClientToScreen(h, ref p);
    csx = p.X; csy = p.Y; cwin = r.Right; chwin = r.Bottom;
    // The captured frame covers the window's extended frame bounds (no invisible resize border): where the client starts in it.
    RECT efb; Check(DwmGetWindowAttribute(h, 9, out efb, 16), "DwmGetWindowAttribute(EXTENDED_FRAME_BOUNDS)");
    int ox = csx - efb.Left, oy = csy - efb.Top;
    if (rw <= 0) { rx = 0; ry = 0; rw = cwin; rh = chwin; }
    pw = Math.Min(cw, rw); ph = Math.Min(ch, rh);
    cropx = ox + rx + (rw - pw) / 2; cropy = oy + ry + (rh - ph) / 2;
    pw -= pw % GX; ph -= ph % GY;   // whole blocks
    int bw = pw / GX, bh = ph / GY;

    IntPtr dev, ctx; int level;
    Check(D3D11CreateDevice(IntPtr.Zero, 1, IntPtr.Zero, 0x20, IntPtr.Zero, 0, 7, out dev, out level, out ctx), "D3D11CreateDevice");   // hardware, BGRA support
    IntPtr dxgi = Query(dev, new Guid("54ec77fa-1377-44e6-8c32-88fd5f44c84c"));   // IDXGIDevice
    IntPtr winrtDev; Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi, out winrtDev), "CreateDirect3D11DeviceFromDXGIDevice");

    RoInitialize(1);   // MTA; an already initialised thread answers with an error code that does not matter
    IntPtr itemFactory = Factory("Windows.Graphics.Capture.GraphicsCaptureItem", new Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356"));   // IGraphicsCaptureItemInterop
    Guid itemIid = new Guid("79C3F95B-31F7-4EC2-A464-632EF5D30760");   // IGraphicsCaptureItem
    IntPtr item; Check(Fx<FnForWindow>(itemFactory, 3)(itemFactory, h, ref itemIid, out item), "CreateForWindow");
    long size; Check(Fx<FnOutLong>(item, 7)(item, out size), "GraphicsCaptureItem.Size");
    frameW = (int)(size & 0xffffffff); frameH = (int)(size >> 32);
    IntPtr poolFactory = Factory("Windows.Graphics.Capture.Direct3D11CaptureFramePool", new Guid("589B103F-6BBC-5DF5-A991-02E28B3B66D5"));   // Statics2
    IntPtr pool; Check(Fx<FnCreatePool>(poolFactory, 6)(poolFactory, winrtDev, 87, 3, size, out pool), "CreateFreeThreaded");   // B8G8R8A8UIntNormalized
    IntPtr session; Check(Fx<FnCreateSession>(pool, 10)(pool, item, out session), "CreateCaptureSession");
    Check(Fx<FnVoid>(session, 6)(session), "StartCapture");
    var tryNext = Fx<FnOutPtr>(pool, 7);

    IntPtr td = Marshal.AllocHGlobal(44);   // D3D11_TEXTURE2D_DESC: B8G8R8A8_UNORM, usage STAGING, CPU read
    uint[] d = { (uint)pw, (uint)ph, 1, 1, 87, 1, 0, 3, 0, 0x20000, 0 };
    for (int i = 0; i < 11; i++) Marshal.WriteInt32(td, i * 4, (int)d[i]);
    IntPtr staging; Check(Fx<FnCreateTex>(dev, 5)(dev, td, IntPtr.Zero, out staging), "CreateTexture2D");
    IntPtr box = Marshal.AllocHGlobal(24);   // D3D11_BOX left, top, front, right, bottom, back
    int[] bx = { cropx, cropy, 0, cropx + pw, cropy + ph, 1 };
    for (int i = 0; i < 6; i++) Marshal.WriteInt32(box, i * 4, bx[i]);
    IntPtr mapped = Marshal.AllocHGlobal(16);
    Guid tex2d = new Guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c");   // ID3D11Texture2D
    var copy = Fx<FnCopyRegion>(ctx, 46); var map = Fx<FnMap>(ctx, 14); var unmap = Fx<FnUnmap>(ctx, 15);

    var buf = new byte[pw * ph * 4];
    var sums = new long[GX * GY * 3];
    var rec = new byte[8 + GX * GY * 3];
    double freq = Stopwatch.Frequency;
    var sw = Stopwatch.StartNew();
    long qpc0 = Stopwatch.GetTimestamp();
    epoch0 = (DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
    int n = 0; bool have = false; double lastRec = -1;
    using (var fs = new FileStream(binPath, FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 20)) {
      while (sw.Elapsed.TotalSeconds < maxSeconds) {
        IntPtr frame; Check(tryNext(pool, out frame), "TryGetNextFrame");
        double t = epoch0 + sw.Elapsed.TotalSeconds;
        if (frame != IntPtr.Zero) {
          // The frame's own time (QPC, 100 ns ticks) on the wall clock: epoch0 is the wall time at qpc0.
          long ticks; Check(Fx<FnOutLong>(frame, 7)(frame, out ticks), "SystemRelativeTime");
          double ft = epoch0 + (ticks / 1.0e7 - qpc0 / freq);
          if (ft <= t) t = ft;   // never later than now (a clock-base mismatch must not move records into the future)
          long cs; Check(Fx<FnOutLong>(frame, 8)(frame, out cs), "ContentSize");
          if ((int)(cs & 0xffffffff) < cropx + pw || (int)(cs >> 32) < cropy + ph)
            throw new Exception("the captured frame (" + (cs & 0xffffffff) + "x" + (cs >> 32) + ") is smaller than the crop");
          IntPtr surf; Check(Fx<FnOutPtr>(frame, 6)(frame, out surf), "Surface");
          IntPtr access = Query(surf, new Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"));   // IDirect3DDxgiInterfaceAccess
          IntPtr tex; Check(Fx<FnGetInterface>(access, 3)(access, ref tex2d, out tex), "GetInterface(ID3D11Texture2D)");
          copy(ctx, staging, 0, 0, 0, 0, tex, 0, box);
          Release(tex); Release(access); Release(surf);
          IntPtr closable = Query(frame, new Guid("30D5A829-7FA4-4026-83BB-D75BAE4EA99E"));   // IClosable: returns the buffer to the pool
          Fx<FnVoid>(closable, 6)(closable); Release(closable); Release(frame);
          Check(map(ctx, staging, 0, 1, 0, mapped), "Map");
          IntPtr data = Marshal.ReadIntPtr(mapped); int pitch = Marshal.ReadInt32(mapped, 8);
          for (int y = 0; y < ph; y++) Marshal.Copy(data + y * pitch, buf, y * pw * 4, pw * 4);
          unmap(ctx, staging, 0);
          Array.Clear(sums, 0, sums.Length);
          for (int y = 0; y < ph; y++) {
            int row = y * pw * 4, by = (y / bh) * GX;
            for (int x = 0; x < pw; x++) {
              int o = row + x * 4, b = (by + x / bw) * 3;
              sums[b] += buf[o]; sums[b + 1] += buf[o + 1]; sums[b + 2] += buf[o + 2];
            }
          }
          int cnt = bw * bh;
          for (int i = 0; i < sums.Length; i++) rec[8 + i] = (byte)(sums[i] / cnt);
          have = true; newFrames++;
        } else {
          Timeouts++;
          if (!have || t - lastRec < 0.005) { System.Threading.Thread.Yield(); if ((Timeouts & 255) == 0 && File.Exists(stopFile)) break; continue; }
        }
        BitConverter.GetBytes(t).CopyTo(rec, 0);
        fs.Write(rec, 0, rec.Length);
        lastRec = t; n++;
        if ((n & 15) == 0 && File.Exists(stopFile)) break;
      }
    }
    elapsed = sw.Elapsed.TotalSeconds;
    if (dumpPng != "" && have) {
      var bmp = new Bitmap(pw, ph, PixelFormat.Format32bppArgb);
      var bd = bmp.LockBits(new Rectangle(0, 0, pw, ph), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
      Marshal.Copy(buf, 0, bd.Scan0, buf.Length);
      bmp.UnlockBits(bd);
      bmp.Save(dumpPng, ImageFormat.Png);
    }
    return n;
  }
}
'@

$bin = "$OutPrefix.bin"
[double]$e0 = 0; [double]$el = 0
[int]$sx = 0; [int]$sy = 0; [int]$cw = 0; [int]$chh = 0; [int]$px = 0; [int]$py = 0; [int]$pw = 0; [int]$ph = 0; [int]$nf = 0; [int]$fw = 0; [int]$fh = 0
Set-Content -Path "$OutPrefix.ready" -Value $ProcessId
$n = [TtffCapture]::Run($ProcessId, $bin, $RegionX, $RegionY, $RegionW, $RegionH, $CropW, $CropH, $StopFile,
  $MaxSeconds, $DumpPng, [ref]$e0, [ref]$el, [ref]$sx, [ref]$sy, [ref]$cw, [ref]$chh, [ref]$px, [ref]$py, [ref]$pw, [ref]$ph, [ref]$nf, [ref]$fw, [ref]$fh)
$meta = [ordered]@{
  pid = $ProcessId; method = 'Windows.Graphics.Capture (window)'
  epoch0 = $e0; seconds = $el; captures = $n; new_frames = $nf; rate_hz = [math]::Round($n / $el, 1); new_frame_hz = [math]::Round($nf / $el, 1)
  grid = @(32, 18); client_origin = @($sx, $sy); client_size = @($cw, $chh); frame_size = @($fw, $fh)
  capture_rect = @($px, $py, $pw, $ph); foreign_checks = 0; record_bytes = 8 + 32 * 18 * 3
}
$meta | ConvertTo-Json -Compress | Set-Content -Path "$OutPrefix.json"
Write-Host "ttff_poller: $($meta.method); window $([TtffCapture]::WindowState)"
Write-Host "ttff_poller: $n records ($nf new frames) in $([math]::Round($el, 2)) s ($($meta.rate_hz) Hz, $($meta.new_frame_hz) new frames/s)"
