param(
    [string]$Name, [string]$Rhi, [int]$Gi = 1, [int]$Frames = 60,
    [hashtable]$Extra = @{}, [string]$Config = '',
    [string]$Root = 'C:\hw131'   # private build tree: $Root\deploy\Editor, captures in $Root\cap
)
# One headless GI-shadow capture (Thema 131) from a PRIVATE deploy -- never the
# user's HorizonEngineBuild. Scene: HE_DUMP_SHADOWINSTTEST floor + cube row, sun
# at TOD 0.35, clouds off. Fresh %APPDATA% per capture (a clean exit rewrites
# config.json); -Config copies a config.json template in first (e.g. one with
# GILightRadius 6.0 -- at the 0.5 deg default the penumbra is ~1 half-res pixel
# and the noise hides). D3D does not always exit: the process is killed ~8 s
# after "frame dumped", and only if its path is under $Root.
# See docs/gi-shadow-edge-noise-analysis-2026-10-02.md.
$ErrorActionPreference = 'Continue'
$exeDir = Join-Path $Root 'deploy\Editor'
$exe    = Join-Path $exeDir 'HorizonEditor.exe'
$out    = Join-Path $Root 'cap'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($k in @(Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' })) { Remove-Item "env:$($k.Name)" }
$appdata = Join-Path $Root "appdata\$Name"
if (Test-Path $appdata) { cmd /c rd /s /q "$appdata" }
New-Item -ItemType Directory -Force $appdata | Out-Null
$env:APPDATA = $appdata
if ($Config) { New-Item -ItemType Directory -Force "$appdata\HorizonEngine" | Out-Null; Copy-Item $Config "$appdata\HorizonEngine\config.json" }
$env:HE_COLLAB_OFFLINE = '1'; $env:HE_NET_LOOPBACK_ONLY = '1'
$env:HE_DUMP_PATH = "$out\$Name.bmp"; $env:HE_DUMP_QUIT = '1'
$env:HE_DUMP_RHI = $Rhi; $env:HE_DUMP_FRAMES = "$Frames"; $env:HE_DUMP_GI = "$Gi"
$env:HE_DUMP_SHADOWINSTTEST = '1'; $env:HE_DUMP_SKYTEST = '1'
$env:HE_DUMP_TOD = '0.35'; $env:HE_DUMP_PITCH = '-18'; $env:HE_DUMP_CAMY = '5'
$env:HE_DUMP_CAMX = '0'; $env:HE_DUMP_CAMZ = '0'; $env:HE_DUMP_CLOUDMODE = '0'; $env:HE_DUMP_CLOUDSHADOWS = '0'
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }
$log = Join-Path $exeDir 'HorizonEngine.log'
if (Test-Path $log) { Remove-Item $log -Force }
if (Test-Path $env:HE_DUMP_PATH) { Remove-Item $env:HE_DUMP_PATH -Force }
$p = Start-Process -FilePath $exe -WorkingDirectory $exeDir -PassThru -WindowStyle Minimized
$t0 = Get-Date; $dumped = $false
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 240) {
    Start-Sleep -Milliseconds 500
    if (-not $dumped -and (Test-Path $log) -and (Select-String -Path $log -Pattern 'frame dumped' -Quiet)) { $dumped = $true; $td = Get-Date }
    if ($dumped -and ((Get-Date) - $td).TotalSeconds -gt 8) { break }
}
if (-not $p.HasExited) {
    $pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($pp -and $pp.Path -like "$Root\*") { Stop-Process -Id $p.Id -Force }
}
Start-Sleep -Milliseconds 500
if (Test-Path $log) { Copy-Item $log "$out\$Name.log" -Force }
"$Name rhi=$Rhi gi=$Gi frames=$Frames dumped=$dumped bmp=$(Test-Path $env:HE_DUMP_PATH) secs=$([int]((Get-Date)-$t0).TotalSeconds)"

