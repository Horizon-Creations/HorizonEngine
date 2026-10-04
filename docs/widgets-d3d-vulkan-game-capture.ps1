param([string]$Out = 'C:\hw133\out_pre', [string]$Tag = 'gpre',
      [string[]]$Rhis = @('OpenGL','D3D11','D3D12','Vulkan'), [string]$Shots = 'C:\hw133\shots')
# Thema 133 step 1: run the exported UiWit game (docs/widgets-d3d-vulkan-witness-scene.ps1)
# windowed on each backend, capture the client area from outside with PrintWindow
# (no HE_CAPTURE_FRAME: that moves the game onto the editor-style viewport path) and
# sample the three button quads plus the vertically mirrored spots.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
New-Item -ItemType Directory -Force $Shots | Out-Null
$exe = Join-Path $Out 'HorizonGame.exe'; $log = Join-Path $Out 'HorizonEngine.log'
$cfgPath = Join-Path $Out 'config.json'
foreach ($rhi in $Rhis) {
    $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
    $set = @{ GameBackend = $rhi; GameWindowMode = 'Windowed'; GameWindowWidth = 1280; GameWindowHeight = 720; PauseOnFocusLoss = $false }
    foreach ($k in $set.Keys) {
        $e = $cfg.CustomConfig | Where-Object { $_.Key -eq $k }
        if ($e) { $e.Value = $set[$k] } else { $cfg.CustomConfig += [pscustomobject]@{ Key = $k; Value = $set[$k] } }
    }
    $cfg | ConvertTo-Json -Depth 20 | Out-File -Encoding ascii $cfgPath
    if (Test-Path $log) { Remove-Item $log }
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
    $env:HE_NET_LOOPBACK_ONLY = '1'; $env:HE_COLLAB_OFFLINE = '1'
    $p = Start-Process -FilePath $exe -WorkingDirectory $Out -PassThru
    $t0 = Get-Date; $ready = $false
    while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 60) {
        Start-Sleep -Milliseconds 500
        $txt = $null
        try { $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $txt = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close() } catch {}
        if ($txt -and $txt -match 'OnInit complete') { $ready = $true; break }
    }
    Start-Sleep -Seconds 5
    $p.Refresh(); $h = $p.MainWindowHandle
    $res = "$rhi : ready=$ready exited=$($p.HasExited)"
    if (-not $p.HasExited -and $h -ne [IntPtr]::Zero) {
        $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [W]::PrintWindow($h, $dc, 3) | Out-Null
        $g.ReleaseHdc($dc); $g.Dispose()
        $bmp.Save("$Shots\${Tag}_$rhi.png", [System.Drawing.Imaging.ImageFormat]::Png)
        function S($x, $y) { $c = $bmp.GetPixel([int]($x / 1280 * $w), [int]($y / 720 * $hh)); "($($c.R),$($c.G),$($c.B))" }
        $res += " client=${w}x$hh red@TL=$(S 200 110) red@mirror=$(S 200 610) green@BR=$(S 1080 610) green@mirror=$(S 1080 110) blue@C=$(S 500 360)"
        $bmp.Dispose()
    }
    $pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($pp -and $pp.Path -eq $exe) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 1
    Copy-Item $log "$Shots\${Tag}_$rhi.log" -ErrorAction SilentlyContinue
    $res
}
