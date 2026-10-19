param([string[]]$Rhis = @('D3D11','D3D12','Vulkan','OpenGL'), [string]$Out = 'C:\hw207\shots', [string[]]$Only = @())
# Thema 207 step 4: Displacement-pin witness on each backend.
#   ref  = pin unconnected (HE_DUMP_DISPPIN=0), ref2 = the same again (noise floor)
#   disp = height 0.4 tessellated, off = height 0.4 with HE_TESSELLATION=0 (fallback contract)
$exe = 'C:\hw207\deploy\Editor\HorizonEditor.exe'
$log = 'C:\hw207\deploy\Editor\HorizonEngine.log'
New-Item -ItemType Directory -Force $Out | Out-Null
$variants = [ordered]@{ ref = @{ HE_DUMP_DISPPIN = '0' }; ref2 = @{ HE_DUMP_DISPPIN = '0' };
                        disp = @{}; off = @{ HE_TESSELLATION = '0' };
                        f1 = @{ HE_DUMP_DISPFACTOR = '1' }; f3 = @{ HE_DUMP_DISPFACTOR = '3' };
                        f1ref = @{ HE_DUMP_DISPFACTOR = '1'; HE_DUMP_DISPHEIGHT = '0' };
                        f1fwd = @{ HE_DUMP_DISPFACTOR = '1'; HE_DUMP_RENDERPATH = '0' };
                        f1def = @{ HE_DUMP_DISPFACTOR = '1'; HE_DUMP_RENDERPATH = '1' };
                        c1 = @{ HE_DUMP_DISPFACTOR = '1'; HE_DUMP_DISPCONST = '0.3' };
                        c16 = @{ HE_DUMP_DISPCONST = '0.3' } }
foreach ($rhi in $Rhis) {
  foreach ($v in $variants.Keys) {
    if ($Only.Count -gt 0 -and $Only -notcontains $v) { continue }
    if ($Only.Count -eq 0 -and ($v -like 'f*' -or $v -like 'c*')) { continue }
    Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
    $app = "C:\hw207\appdata\$rhi-$v"; New-Item -ItemType Directory -Force $app | Out-Null
    $env:APPDATA = $app
    $env:HE_HIDDEN_WINDOW = '1'; $env:HE_COLLAB_OFFLINE = '1'; $env:HE_D3D_SHADER_CACHE = '0'
    $env:HE_DUMP_PATH = "$Out\$rhi-$v.bmp"; $env:HE_DUMP_QUIT = '1'; $env:HE_DUMP_RHI = $rhi
    $env:HE_DUMP_FRAMES = '16'; $env:HE_DUMP_MATERIALTEST = 'displace'
    $env:HE_DUMP_SKYTEST = '1'; $env:HE_DUMP_TOD = '0.35'; $env:HE_DUMP_PITCH = '-6'
    $env:HE_DUMP_CAMX = '0'; $env:HE_DUMP_CAMY = '3.5'; $env:HE_DUMP_CAMZ = '3'
    $env:HE_DUMP_AA = '0'; $env:HE_DUMP_DOF = '0'; $env:HE_DUMP_MOTIONBLUR = '0'; $env:HE_DUMP_BLOOM = '0'
    $env:HE_DUMP_CLOUDMODE = '0'; $env:HE_DUMP_COVERAGE = '0'; $env:HE_SKY_TIME = '6.2832'
    if ($rhi -eq 'D3D12' -and $v -eq 'disp') { $env:HE_GPU_DEBUG = '1' }
    foreach ($k in $variants[$v].Keys) { Set-Item "env:$k" $variants[$v][$k] }
    Remove-Item $log -ErrorAction SilentlyContinue
    Remove-Item "$Out\$rhi-$v.bmp" -ErrorAction SilentlyContinue
    $p = Start-Process $exe -PassThru -WindowStyle Hidden
    $t0 = Get-Date
    while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 240) {
      Start-Sleep -Milliseconds 500
      if ((Test-Path "$Out\$rhi-$v.bmp") -and (Test-Path $log) -and (Select-String -Path $log -Pattern 'frame dumped' -Quiet)) {
        if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        break
      }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    if (Test-Path $log) { Copy-Item $log "$Out\$rhi-$v.log" -Force }
    "{0,-7} {1,-5} bmp={2} exit={3}" -f $rhi, $v, (Test-Path "$Out\$rhi-$v.bmp"), $p.ExitCode
  }
}
