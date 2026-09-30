param([string]$Name = "editor", [string]$Exe = "C:\hw112\deploy\Editor\HorizonEditor.exe", [int]$SettleMs = 1500)
# Thema 112 step 2: the EDITOR viewport under D3D12 through the same kind of
# resize series, the Depthy cube scene in the viewport. The editor draws the
# scene into its own viewport RT (own depth), sized by the dock layout, so the
# check is a revisit test: every size visited twice (before and after the fast
# burst / slow staircase / minimise) must give the same pixels. The diff box
# says WHERE pixels differ (status-bar FPS text vs. the viewport).
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$root = "C:\hw112\s2"
$appdata = Join-Path $root "appdata_$Name"
if (Test-Path $appdata) { Remove-Item -Recurse -Force $appdata }
New-Item -ItemType Directory -Force (Join-Path $appdata "HorizonEngine") | Out-Null
'{ "RHI": 3, "LastProjectPath": "C:/hw112/proj/Depthy/Depthy.heproj" }' | Out-File -Encoding ascii (Join-Path $appdata "HorizonEngine\config.json")
$env:APPDATA = $appdata; $env:HE_GPU_DEBUG = "1"; $env:HE_COLLAB_OFFLINE = "1"; $env:HE_NET_LOOPBACK_ONLY = "1"; $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class EdWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    public static int FgPid() { int p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
    public static int[] Client(IntPtr h) { RECT r; GetClientRect(h, out r); return new int[] { r.R - r.L, r.B - r.T }; }
    public static void SetClient(IntPtr h, int w, int hh) {
        SetWindowPos(h, IntPtr.Zero, 0, 0, w, hh, 0x2 | 0x4 | 0x10);
        int[] c = Client(h);
        SetWindowPos(h, IntPtr.Zero, 0, 0, w + (w - c[0]), hh + (hh - c[1]), 0x2 | 0x4 | 0x10);
    }
    public static Bitmap Capture(IntPtr h) {
        int[] c = Client(h);
        Bitmap bmp = new Bitmap(Math.Max(1, c[0]), Math.Max(1, c[1]), PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); }
        return bmp;
    }
    static byte[] Px(Bitmap b, out int stride) {
        var d = b.LockBits(new Rectangle(0,0,b.Width,b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        stride = d.Stride; var a = new byte[stride*b.Height]; Marshal.Copy(d.Scan0, a, 0, a.Length); b.UnlockBits(d); return a; }
    // Differing pixels and their bounding box, split at yCut (above = sky band of the viewport).
    public static string Diff(string pa, string pb, int yCut) {
        using (var A = new Bitmap(pa)) using (var B = new Bitmap(pb)) {
            if (A.Width != B.Width || A.Height != B.Height) return "size mismatch";
            int sa, sb; var a = Px(A, out sa); var b = Px(B, out sb);
            long n = 0; int x0 = int.MaxValue, y0 = int.MaxValue, x1 = -1, y1 = -1; long sky = 0;
            for (int y = 0; y < A.Height; ++y) for (int x = 0; x < A.Width; ++x) {
                int i = y*sa + x*4;
                if (a[i] != b[i] || a[i+1] != b[i+1] || a[i+2] != b[i+2]) {
                    if (y < yCut) { ++sky; continue; }
                    ++n; if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; } }
            return n == 0 ? string.Format("belowCut=0 (sky band {0})", sky)
                          : string.Format("belowCut={0} box=({1},{2})-({3},{4}) (sky band {5})", n, x0, y0, x1, y1, sky);
        } }
}
"@

$shotDir = Join-Path $root "shots\$Name"
if (Test-Path $shotDir) { Remove-Item -Recurse -Force $shotDir }
New-Item -ItemType Directory -Force $shotDir | Out-Null
$exeDir = Split-Path $Exe
$log = Join-Path $exeDir "HorizonEngine.log"
if (Test-Path $log) { Remove-Item -Force $log }
$p = Start-Process -FilePath $Exe -WorkingDirectory $exeDir -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
function Guard { if ([EdWin]::FgPid() -eq $p.Id) { Stop-Process -Id $p.Id -Force; "ABORT: editor became the foreground window"; exit 2 } }
function Wait([int]$ms) { for ($k = 0; $k -lt [Math]::Max(1, [int]($ms / 200)); ++$k) { Start-Sleep -Milliseconds 200; Guard } }
$h = [IntPtr]::Zero
while ($sw.Elapsed.TotalSeconds -lt 180 -and -not $p.HasExited) {
    Wait 500; $p.Refresh()
    if ($p.MainWindowTitle -like "*Depthy*") { $h = $p.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) { "editor window never appeared"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Wait 6000
if ([EdWin]::IsZoomed($h)) { Stop-Process -Id $p.Id -Force; "editor started maximised - not un-maximising (ShowWindow can take the foreground)"; exit 1 }
function Shot([string]$tag) { Wait $SettleMs; $b = [EdWin]::Capture($h); $b.Save((Join-Path $shotDir "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose() }

[EdWin]::SetClient($h, 1600, 900);  Shot "a_1600x900_1"
[EdWin]::SetClient($h, 1800, 1000); Shot "b_1800x1000_1"
[EdWin]::SetClient($h, 960, 600);   Shot "c_960x600_1"
$rnd = New-Object System.Random 112
for ($i = 0; $i -lt 30; ++$i) { [EdWin]::SetClient($h, $rnd.Next(700, 1820), $rnd.Next(450, 1020)); Start-Sleep -Milliseconds 30; Guard }
[EdWin]::SetClient($h, 1800, 1000); Shot "b_1800x1000_2_after_fast"
foreach ($s in @(@(1700,950),@(1500,850),@(1300,730),@(1100,620),@(1300,730),@(1500,850))) { [EdWin]::SetClient($h, $s[0], $s[1]); Wait 400 }
[EdWin]::SetClient($h, 1600, 900);  Shot "a_1600x900_2_after_slow"
# No minimise/restore here: restoring the editor took the foreground from the
# human's fullscreen game (2026-09-30, twice); step 1 covered it for the editor.
[EdWin]::SetClient($h, 960, 600);   Shot "c_960x600_2"

$pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($pp -and $pp.Path -eq $Exe) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 1500
Copy-Item $log (Join-Path $shotDir "HorizonEngine.log") -ErrorAction SilentlyContinue
"run=$Name secs=$([int]$sw.Elapsed.TotalSeconds) fgNow=$([EdWin]::FgPid())"
foreach ($pair in @(@("a_1600x900_1","a_1600x900_2_after_slow",900),@("b_1800x1000_1","b_1800x1000_2_after_fast",1000),@("c_960x600_1","c_960x600_2",600))) {
    "{0,-26} vs {1,-26} {2}" -f $pair[0], $pair[1], [EdWin]::Diff((Join-Path $shotDir "$($pair[0]).png"), (Join-Path $shotDir "$($pair[1]).png"), [int]($pair[2] * 0.25))
}
$L = Join-Path $shotDir "HorizonEngine.log"
"resizeLines=$((Select-String -Path $L -Pattern 'swapchain resized').Count)"
$bad = Select-String -Path $L -Pattern 'swapchain resized to (\d+)x(\d+) \(client (\d+)x(\d+)\)' | Where-Object { $_.Matches[0].Groups[1].Value -ne $_.Matches[0].Groups[3].Value -or $_.Matches[0].Groups[2].Value -ne $_.Matches[0].Groups[4].Value }
"swapchain!=client: $(@($bad).Count)"
$dbg = Select-String -Path $L -Pattern 'D3D12 debug layer|\[ERROR\]' | Where-Object { $_.Line -notmatch 'clear values do not match' }
"debug/error lines (excluding clear-value warning)=$(@($dbg).Count)"
$dbg | Select-Object -First 5 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }

