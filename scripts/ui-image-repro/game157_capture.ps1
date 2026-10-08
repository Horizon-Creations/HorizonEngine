param([string]$Out = 'C:\hw157\game\out', [string]$Tag = 'g4',
      [string[]]$Rhis = @('OpenGL','Vulkan','D3D11','D3D12'), [string]$Shots = 'C:\hw157\game\shots',
      [string]$GpuDebug = '1')
# Thema 157 Schritt 4: das exportierte ImgWit-Spiel (game157_setup.py + game157_export.ps1)
# pro Backend im Fenster starten, nach 'OnInit complete' + 5 s die Client-Flaeche von aussen
# per PrintWindow aufnehmen. KEIN HE_CAPTURE_FRAME: das schiebt das Spiel auf den
# Viewport-Pfad des Editors, gemessen werden soll aber der Swapchain-Pfad.
# HE_GPU_DEBUG=1 schaltet Vulkan-Validation / D3D-Debug-Layer an.
# Auswertung: python game157_ana.py <Shots> <Tag>
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
New-Item -ItemType Directory -Force $Shots | Out-Null
$exe = Join-Path $Out 'HorizonGame.exe'; $log = Join-Path $Out 'HorizonEngine.log'
$cfgPath = Join-Path $Out 'config.json'
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
$env:HE_NET_LOOPBACK_ONLY = '1'; $env:HE_COLLAB_OFFLINE = '1'
if ($GpuDebug -eq '1') { $env:HE_GPU_DEBUG = '1' }
foreach ($rhi in $Rhis) {
    $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
    $set = @{ GameBackend = $rhi; GameWindowMode = 'Windowed'; GameWindowWidth = 1280; GameWindowHeight = 720; PauseOnFocusLoss = $false }
    foreach ($k in $set.Keys) {
        $e = $cfg.CustomConfig | Where-Object { $_.Key -eq $k }
        if ($e) { $e.Value = $set[$k] } else { $cfg.CustomConfig += [pscustomobject]@{ Key = $k; Value = $set[$k] } }
    }
    $cfg | ConvertTo-Json -Depth 20 | Out-File -Encoding ascii $cfgPath
    if (Test-Path $log) { Remove-Item $log }
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
    $fg = ([W]::GetForegroundWindow() -eq $h)
    $res = "$rhi : ready=$ready exited=$($p.HasExited) foreground=$fg"
    if (-not $p.HasExited -and $h -ne [IntPtr]::Zero) {
        $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [W]::PrintWindow($h, $dc, 3) | Out-Null
        $g.ReleaseHdc($dc); $g.Dispose()
        $bmp.Save("$Shots\${Tag}_$rhi.png", [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $res += " client=${w}x$hh"
    }
    $pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($pp -and $pp.Path -eq $exe) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 1
    Copy-Item $log "$Shots\${Tag}_$rhi.log" -ErrorAction SilentlyContinue
    $res
}
