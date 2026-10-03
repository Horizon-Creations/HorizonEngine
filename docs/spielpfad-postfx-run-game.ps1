param(
    [string]$Name,                       # label for shots/<Name>
    [string]$GameDir,                    # exported game folder (HorizonGame.exe + pak + config.json)
    [string]$Mode = "resize",            # resize | fresh
    [int]$W = 1280, [int]$H = 720,       # start size (fresh: the size to capture)
    [int]$SettleMs = 1500,
    [string]$Root = "C:\hw130\s2",
    [string]$Set = ""                    # "Key=Value;Key=Value" config.json overrides
)
# Thema 130 step 2 (from Thema 112's game_resize.ps1): drive an EXPORTED D3D11 game (swapchain scene path,
# !useViewport) through window resizes from outside and capture the client
# area after each step. Mode "fresh" starts the game directly at W x H and
# captures one reference frame: a resized frame must match the fresh frame of
# the same size (stretching or a stale depth buffer would not).
#
# The human may be in a fullscreen game at the console: the window is shown
# without activation (SDL hint), and if the game ever becomes the foreground
# process it is killed at once and the run aborts.
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
if ($env:S2_BGFPS) { $env:HE_BACKGROUND_FPS = $env:S2_BGFPS }
$env:HE_COLLAB_OFFLINE = "1"
if ($env:S2_CAPTURE) { $env:HE_CAPTURE_FRAME = $env:S2_CAPTURE; $env:HE_CAPTURE_PATH = Join-Path $Root "capture_$Name.ppm" }
$env:HE_NET_LOOPBACK_ONLY = "1"
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"
$appdata = Join-Path $Root "appdata_$Name"
if (Test-Path $appdata) { Remove-Item -Recurse -Force $appdata }
New-Item -ItemType Directory -Force $appdata | Out-Null
$env:APPDATA = $appdata

# Start size lives in the shipped config.json.
$cfgPath = Join-Path $GameDir "config.json"
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
($cfg.CustomConfig | Where-Object Key -eq "GameWindowWidth").Value  = $W
($cfg.CustomConfig | Where-Object Key -eq "GameWindowHeight").Value = $H
# Thema 130: backend, window mode and per-run post settings.
$kv = [ordered]@{ GameBackend = "D3D11"; GameWindowMode = "Windowed"; PauseOnFocusLoss = $false }
foreach ($pair in ($Set -split ';' | Where-Object { $_ })) {
    $k, $v = $pair -split '=', 2
    if ($v -eq 'true') { $v = $true } elseif ($v -eq 'false') { $v = $false } elseif ($v -match '^-?\d+$') { $v = [int]$v } elseif ($v -match '^-?\d*\.\d+$') { $v = [double]$v }
    $kv[$k] = $v
}
foreach ($k in $kv.Keys) {
    $e = $cfg.CustomConfig | Where-Object Key -eq $k
    if ($e) { $e.Value = $kv[$k] } else { $cfg.CustomConfig += [pscustomobject]@{ Key = $k; Value = $kv[$k] } }
}
$cfg | ConvertTo-Json -Depth 5 | Out-File -Encoding ascii $cfgPath

if (-not ("HeWin2" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class HeWin2 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    public static int FgPid() { int p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
    public static int[] Client(IntPtr h) { RECT r; GetClientRect(h, out r); return new int[] { r.R - r.L, r.B - r.T }; }
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
}
"@
}

# NN-WS03 runs at 125 %: without this, client sizes and PrintWindow are DPI-virtualised (scaled).
[HeWin2]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$shotDir = Join-Path $Root "shots\$Name"
if (Test-Path $shotDir) { Remove-Item -Recurse -Force $shotDir }
New-Item -ItemType Directory -Force $shotDir | Out-Null
$exe = Join-Path $GameDir "HorizonGame.exe"
Get-ChildItem $GameDir -Filter "*.log" | Remove-Item -Force

$p = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
$script:aborted = $false
function Guard {
    if ([HeWin2]::FgPid() -eq $p.Id) {
        Stop-Process -Id $p.Id -Force
        $script:aborted = $true
        "ABORT run=${Name}: the game became the foreground window - killed at t=$([int]$sw.Elapsed.TotalSeconds)s"
        return $true
    }
    return $false
}
$h = [IntPtr]::Zero
while ($sw.Elapsed.TotalSeconds -lt 90 -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 250
    if (Guard) { exit 2 }
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero -and $p.MainWindowTitle -like "HorizonGame*") { $h = $p.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) { "run=${Name}: game window never appeared (exited=$($p.HasExited) code=$(if($p.HasExited){$p.ExitCode}))"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
# The window exists long before the first frame: D3D12 spends ~7 s building
# pipelines first. Capturing earlier gives an all-black client area, so wait
# for the main loop to start (the log is written live) and then settle.
$gameLog = Join-Path $GameDir "HorizonEngine.log"
function LogText { try { $fs = [IO.File]::Open($gameLog, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); return $t } catch { return "" } }
while ($sw.Elapsed.TotalSeconds -lt 120 -and -not $p.HasExited -and -not ((LogText) -match "OnInit complete")) {
    Start-Sleep -Milliseconds 250
    if (Guard) { exit 2 }
}
for ($k = 0; $k -lt 12; ++$k) { Start-Sleep -Milliseconds 250; if (Guard) { exit 2 } }   # first frames settle

$results = @()
function Shot([string]$tag) {
    for ($k = 0; $k -lt [int]($SettleMs / 250); ++$k) { Start-Sleep -Milliseconds 250; if (Guard) { exit 2 } }
    $c = [HeWin2]::Client($h)
    $bmp = [HeWin2]::Capture($h)
    $bmp.Save((Join-Path $shotDir "$tag.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $script:results += [pscustomobject]@{ step = $tag; client = "$($c[0])x$($c[1])" }
}

$start = [HeWin2]::Client($h)
if ($Mode -eq "fresh") {
    Shot ("fresh_{0}x{1}" -f $start[0], $start[1])
} else {
    Shot "00_start"
    [HeWin2]::SetClient($h, 1800, 1000); Shot "01_1800x1000"      # beyond the start size in both axes
    [HeWin2]::SetClient($h, 960, 540);   Shot "02_960x540"
    [HeWin2]::SetClient($h, 1600, 900);  Shot "03_1600x900"
    # Fast: many sizes back to back while the game keeps rendering.
    $rnd = New-Object System.Random 112
    for ($i = 0; $i -lt 30; ++$i) {
        [HeWin2]::SetClient($h, $rnd.Next(640, 1820), $rnd.Next(360, 1020))
        Start-Sleep -Milliseconds 30
        if (Guard) { exit 2 }
    }
    [HeWin2]::SetClient($h, 1800, 1000); Shot "04_after_fast_1800x1000"
    # Slow: step by step, one size every 400 ms, shrinking then growing.
    foreach ($s in @(@(1700,950),@(1500,850),@(1300,730),@(1100,620),@(1300,730),@(1500,850),@(1700,950))) {
        [HeWin2]::SetClient($h, $s[0], $s[1])
        for ($k = 0; $k -lt 2; ++$k) { Start-Sleep -Milliseconds 200; if (Guard) { exit 2 } }
    }
    [HeWin2]::SetClient($h, 1600, 900);  Shot "05_after_slow_1600x900"
    [HeWin2]::ShowWindow($h, 7) | Out-Null   # SW_SHOWMINNOACTIVE
    for ($k = 0; $k -lt 8; ++$k) { Start-Sleep -Milliseconds 250; if (Guard) { exit 2 } }
    [HeWin2]::ShowWindow($h, 4) | Out-Null   # SW_SHOWNOACTIVATE
    Shot "06_restored_1600x900"
    [HeWin2]::SetClient($h, 1280, 720);  Shot "07_back_1280x720"
}

$alive = -not $p.HasExited
$pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($pp -and $pp.Path -eq (Resolve-Path $exe).Path) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 800
"run=$Name mode=$Mode startClient=$($start[0])x$($start[1]) aliveAtEnd=$alive secs=$([int]$sw.Elapsed.TotalSeconds) fgNow=$([HeWin2]::FgPid())"
$results | Format-Table -AutoSize | Out-String -Width 200
# The game's log: beside the exe or in its pref dir under APPDATA.
$logs = @(Get-ChildItem $GameDir -Filter "*.log") + @(Get-ChildItem $appdata -Recurse -Filter "*.log" -ErrorAction SilentlyContinue)
foreach ($l in $logs) { Copy-Item $l.FullName (Join-Path $shotDir $l.Name) }
"logs: " + (($logs | ForEach-Object FullName) -join ", ")


