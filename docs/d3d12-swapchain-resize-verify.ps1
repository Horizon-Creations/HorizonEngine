param(
    [string]$Name,
    [string]$Exe = "C:\hw112\deploy\Editor\HorizonEditor.exe",
    [string]$Root = "C:\hw112",
    [string]$Project = "C:/hw112/proj/Testie/Testie.heproj",
    [int]$StartTimeoutSec = 180,
    [int]$SettleMs = 1500
)
# Thema 112: drive the INTERACTIVE editor on D3D12 through a series of window
# resizes from outside (SetWindowPos, no activation — the human may be at the
# console), capture the client area after each step (PrintWindow, flip-model
# safe) and measure where the first horizontal UI edges sit. With a swapchain
# stuck at its start size DXGI stretches the image, so those rows scale with
# clientH / startH; with ResizeBuffers they stay put. Afterwards the log is
# checked for the renderer's resize lines and for D3D12 debug-layer messages
# (HE_GPU_DEBUG=1 turns the layer on in Release).
#
# Usage: powershell -File docs\d3d12-swapchain-resize-verify.ps1 -Name post
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$appdata = Join-Path $Root "appdata_$Name"
if (Test-Path $appdata) { Remove-Item -Recurse -Force $appdata }
New-Item -ItemType Directory -Force (Join-Path $appdata "HorizonEngine") | Out-Null
# 3 = D3D12. Before Thema 124 (A6) HE_DUMP_RHI outside dump mode crashed at
# frame 1; fixed since, but the config pin keeps this script valid on older builds.
"{ `"RHI`": 3, `"LastProjectPath`": `"$Project`" }" |
    Out-File -Encoding ascii (Join-Path $appdata "HorizonEngine\config.json")
$env:APPDATA = $appdata
$env:HE_GPU_DEBUG = "1"
$env:HE_COLLAB_OFFLINE = "1"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class HeWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
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
}
"@

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

if ([HeWin]::IsZoomed($h)) { [HeWin]::ShowWindow($h, 4) | Out-Null; Start-Sleep -Milliseconds $SettleMs } # SW_SHOWNOACTIVATE
$results = @()
function Shot([string]$tag) {
    Start-Sleep -Milliseconds $SettleMs
    $c = [HeWin]::Client($h)
    $bmp = [HeWin]::Capture($h)
    $bmp.Save((Join-Path $shotDir "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $e = [HeWin]::Edges($bmp, [int]($bmp.Width * 0.5), 200, 60, 4)
    $bmp.Dispose()
    $script:results += [pscustomobject]@{ step = $tag; client = "$($c[0])x$($c[1])"; edgesY = $e }
}

$start = [HeWin]::Client($h)
Shot "00_start"
[HeWin]::SetClient($h, 1280, 720);  Shot "01_1280x720"
[HeWin]::SetClient($h, 1800, 1000); Shot "02_1800x1000"
[HeWin]::SetClient($h, 960, 600);   Shot "03_960x600"
# Burst: many sizes back to back while the editor keeps rendering.
$rnd = New-Object System.Random 112
for ($i = 0; $i -lt 25; ++$i) {
    [HeWin]::SetClient($h, $rnd.Next(700, 1900), $rnd.Next(450, 1050))
    Start-Sleep -Milliseconds 40
}
[HeWin]::SetClient($h, 1600, 900);  Shot "04_after_burst_1600x900"
# WARNING: restoring the EDITOR from minimised takes the foreground even with
# SW_SHOWNOACTIVATE (step 2 saw it twice against a human's fullscreen game).
# Only run this part when nobody is at the console.
[HeWin]::ShowWindow($h, 7) | Out-Null  # SW_SHOWMINNOACTIVE
Start-Sleep -Milliseconds 2000
[HeWin]::ShowWindow($h, 4) | Out-Null  # SW_SHOWNOACTIVATE
Shot "05_restored"
[HeWin]::SetClient($h, 1280, 720);  Shot "06_back_1280x720"

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
"resizeLines=$((Select-String -Path $L -Pattern 'swapchain resized').Count)"
Select-String -Path $L -Pattern 'swapchain resized' | Select-Object -Last 3 | ForEach-Object { $_.Line }
# The ClearRenderTargetView "clear values do not match" warning is pre-existing
# (every frame, viewport RT) and says nothing about the swapchain.
$dbg = Select-String -Path $L -Pattern 'D3D12 debug layer' |
       Where-Object { $_.Line -notmatch 'clear values do not match' }
$err = $dbg | Where-Object { $_.Line -match '\[ERROR\]' }
"debugLayerLines(other than clear-value)=$($dbg.Count) errorLines=$($err.Count)"
$dbg | Select-Object -First 8 | ForEach-Object { $_.Line }
Select-String -Path $L -Pattern 'GPU debug layer|Present failed|DeviceRemoved' | Select-Object -First 4 | ForEach-Object { $_.Line }
