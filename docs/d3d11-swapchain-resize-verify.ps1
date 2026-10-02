param(
    [string]$Name,
    [string]$Exe = "C:\hw128\deploy\Editor\HorizonEditor.exe",
    [string]$Root = "C:\hw128",
    [string]$Project = "C:/hw128/proj/Testie/Testie.heproj",
    [int]$StartTimeoutSec = 180,
    [int]$SettleMs = 1500,
    # Restoring the EDITOR from minimised takes the foreground even with
    # SW_SHOWNOACTIVATE — only pass this when nobody is at the console.
    [switch]$WithMinimize,
    # Optional own sequence of client sizes, e.g. "1280x720,2000x1125,1280x720",
    # instead of the default series (stops at the first exit).
    [string]$Seq = ""
)
# Thema 128: the D3D11 twin of docs/d3d12-swapchain-resize-verify.ps1. Drives
# the INTERACTIVE editor on D3D11 through a series of window resizes from
# outside (SetWindowPos, no activation), captures the client area after each
# step (PrintWindow) and measures where the first horizontal UI edges sit.
# With a swapchain stuck at its start size DXGI stretches the image, so those
# rows scale with clientH / startH; with ResizeBuffers they stay put.
# Afterwards the log is checked for the renderer's resize lines (swapchain
# size read back via GetDesc must equal the client size) and for failures.
# D3D11 has no HE_GPU_DEBUG switch (the device is created without
# D3D11_CREATE_DEVICE_DEBUG), so there is no debug-layer count here.
#
# Usage: powershell -File docs\d3d11-swapchain-resize-verify.ps1 -Name post
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$appdata = Join-Path $Root "appdata_$Name"
if (Test-Path $appdata) { Remove-Item -Recurse -Force $appdata }
New-Item -ItemType Directory -Force (Join-Path $appdata "HorizonEngine") | Out-Null
# 2 = D3D11. HE_DUMP_RHI must NOT be used outside dump mode (crashes at frame 1).
"{ `"RHI`": 2, `"LastProjectPath`": `"$Project`" }" |
    Out-File -Encoding ascii (Join-Path $appdata "HorizonEngine\config.json")
$env:APPDATA = $appdata
$env:HE_COLLAB_OFFLINE = "1"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class HeWin11 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    public static int[] Client(IntPtr h) { RECT r; GetClientRect(h, out r); return new int[] { r.R - r.L, r.B - r.T }; }
    // Outer size so that the CLIENT area becomes w x h (borders differ per style).
    public static void SetClient(IntPtr h, int w, int hh) {
        SetWindowPos(h, IntPtr.Zero, 0, 0, w, hh, 0x2 | 0x4 | 0x10); // NOMOVE|NOZORDER|NOACTIVATE
        int[] c = Client(h);
        SetWindowPos(h, IntPtr.Zero, 0, 0, w + (w - c[0]), hh + (hh - c[1]), 0x2 | 0x4 | 0x10);
    }
    public static Bitmap Capture(IntPtr h) {
        int[] c = Client(h);
        Bitmap bmp = new Bitmap(Math.Max(1, c[0]), Math.Max(1, c[1]), PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) {
            IntPtr dc = g.GetHdc();
            PrintWindow(h, dc, 3); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
            g.ReleaseHdc(dc);
        }
        return bmp;
    }
    // Rows (from the top, in one column) where the colour jumps by more than
    // `thr` summed over RGB — the edges of menu bar / toolbar / panel headers.
    public static string Edges(Bitmap b, int x, int maxRows, int thr, int count) {
        var sb = new System.Text.StringBuilder();
        int found = 0;
        Color prev = b.GetPixel(x, 0);
        for (int y = 1; y < Math.Min(maxRows, b.Height) && found < count; ++y) {
            Color c = b.GetPixel(x, y);
            int d = Math.Abs(c.R - prev.R) + Math.Abs(c.G - prev.G) + Math.Abs(c.B - prev.B);
            if (d > thr) { if (sb.Length > 0) sb.Append(','); sb.Append(y); ++found; }
            prev = c;
        }
        return sb.ToString();
    }
    // Lowest row (in one column) that is not near-black: with a stretched or
    // cut-off image the drawn area ends short of the client height.
    public static int LastLitRow(Bitmap b, int x) {
        for (int y = b.Height - 1; y >= 0; --y) {
            Color c = b.GetPixel(x, y);
            if (c.R + c.G + c.B > 24) return y;
        }
        return -1;
    }
}
"@
# Physical pixels: at a display scale != 100 % a DPI-unaware caller sees the
# (per-monitor-aware) editor's client rect virtualised — 1600x900 for a real
# 2000x1125 at 125 % — and could not compare it with the renderer's log line.
[HeWin11]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null  # PER_MONITOR_AWARE_V2

$shotDir = Join-Path $Root "shots\$Name"
if (Test-Path $shotDir) { Remove-Item -Recurse -Force $shotDir }
New-Item -ItemType Directory -Force $shotDir | Out-Null
$exeDir = Split-Path $Exe
$log = Join-Path $exeDir "HorizonEngine.log"
if (Test-Path $log) { Remove-Item -Force $log }

$p = Start-Process -FilePath $Exe -WorkingDirectory $exeDir -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
$h = [IntPtr]::Zero
while ($sw.Elapsed.TotalSeconds -lt $StartTimeoutSec -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 1000
    $p.Refresh()
    # The first MainWindowHandle is the splash; the editor's title names the project.
    if ($p.MainWindowTitle -like "*Testie*") { $h = $p.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) { "run=${Name}: editor window never appeared (exited=$($p.HasExited))"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Start-Sleep -Seconds 4   # let the first frames and the project load settle

if ([HeWin11]::IsZoomed($h)) { [HeWin11]::ShowWindow($h, 4) | Out-Null; Start-Sleep -Milliseconds $SettleMs } # SW_SHOWNOACTIVATE
$results = @()
function Shot([string]$tag) {
    Start-Sleep -Milliseconds $SettleMs
    if ($p.HasExited) {
        $script:results += [pscustomobject]@{ step = $tag; client = "exited 0x{0:X8}" -f $p.ExitCode; edgesY = ""; lastLitRow = "" }
        return
    }
    $c = [HeWin11]::Client($h)
    $bmp = [HeWin11]::Capture($h)
    $bmp.Save((Join-Path $shotDir "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $e = [HeWin11]::Edges($bmp, [int]($bmp.Width * 0.5), 200, 60, 4)
    # Right panel column (Inspector etc. reach the bottom edge in the default layout).
    $lit = [HeWin11]::LastLitRow($bmp, [int]($bmp.Width * 0.9))
    $bmp.Dispose()
    $script:results += [pscustomobject]@{ step = $tag; client = "$($c[0])x$($c[1])"; edgesY = $e; lastLitRow = $lit }
}

$start = [HeWin11]::Client($h)
Shot "00_start"
if ($Seq) {
    $i = 1
    foreach ($s in $Seq.Split(',')) {
        $wh = $s.Split('x'); [HeWin11]::SetClient($h, [int]$wh[0], [int]$wh[1]); Shot ("{0:D2}_{1}" -f $i, $s); ++$i
        if ($p.HasExited) { break }
    }
} else {
[HeWin11]::SetClient($h, 1280, 720);  Shot "01_1280x720"
# Larger than the start size in both axes (start is 2000x1125 at 125 %).
[HeWin11]::SetClient($h, 2300, 1250); Shot "02_2300x1250"
[HeWin11]::SetClient($h, 960, 600);   Shot "03_960x600"
# Burst: many sizes back to back while the editor keeps rendering.
$rnd = New-Object System.Random 128
for ($i = 0; $i -lt 25; ++$i) {
    [HeWin11]::SetClient($h, $rnd.Next(700, 2300), $rnd.Next(450, 1250))
    Start-Sleep -Milliseconds 40
}
[HeWin11]::SetClient($h, 1600, 900);  Shot "04_after_burst_1600x900"
if ($WithMinimize) {
    [HeWin11]::ShowWindow($h, 7) | Out-Null  # SW_SHOWMINNOACTIVE
    Start-Sleep -Milliseconds 2000
    [HeWin11]::ShowWindow($h, 4) | Out-Null  # SW_SHOWNOACTIVATE
    Shot "05_restored"
}
[HeWin11]::SetClient($h, 1280, 720);  Shot "06_back_1280x720"
}

$alive = -not $p.HasExited
if ($alive) {
    $pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($pp -and $pp.Path -eq $Exe) { Stop-Process -Id $p.Id -Force }
}
Start-Sleep -Milliseconds 800
Copy-Item $log (Join-Path $shotDir "HorizonEngine.log")
$L = Join-Path $shotDir "HorizonEngine.log"
"run=$Name startClient=$($start[0])x$($start[1]) aliveAtEnd=$alive secs=$([int]$sw.Elapsed.TotalSeconds)"
$results | Format-Table -AutoSize | Out-String -Width 200
$lines = Select-String -Path $L -Pattern 'D3D11Renderer: swapchain resized'
"resizeLines=$($lines.Count)"
# "swapchain resized to WxH (client WxH)": the two sizes must agree on every line.
$mismatch = $lines | Where-Object { $_.Line -notmatch 'resized to (\d+)x(\d+) \(client \1x\2\)' }
"resizeLinesSwapchainNotClient=$(@($mismatch).Count)"
$lines | Select-Object -Last 3 | ForEach-Object { $_.Line }
Select-String -Path $L -Pattern 'ResizeBuffers|Present failed|DeviceRemoved|\[ERROR\]' | Select-Object -First 8 | ForEach-Object { $_.Line }
