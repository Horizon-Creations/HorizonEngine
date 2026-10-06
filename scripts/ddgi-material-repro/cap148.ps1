param(
    [string]$Name, [string]$Rhi, [int]$Gi = 1, [string]$Scene = 'bleed', [string]$Bleed = '3',
    [int]$Frames = 60, [hashtable]$Extra = @{},
    [string]$Deploy = 'C:\hw148\deploy',   # private deploy: $Deploy\Editor\HorizonEditor.exe
    [string]$Out = 'C:\hw148\cap'
)
# One headless capture for the DDGI graph-material hardware acceptance (Thema 148,
# follow-up of Thema 120 / PR #79). Scenes:
#   bleed   - HE_DUMP_MATERIALTEST graph sphere + HE_DUMP_GIBLEED floor slab.
#             -Bleed 3/4: red/grey floor under the GRAPH sphere (heLitP reads the probes),
#             -Bleed 1/2: red/grey floor under the BUILT-IN control sphere (x+6).
#   terrain - HE_DUMP_LANDSCAPELAYERS painted witness terrain (a graph material), top-down.
# Everything temporal / screen-space that could differ per backend for reasons other
# than the probe field is off (SSR, GI reflections, SSAO, AA, bloom, DOF, motion blur,
# clouds). HE_SKY_TIME=2*pi pins the graph's Time node, so its sin(time) -> Metallic is ~0
# (a dielectric sphere: the diffuse probe term is what we measure).
# Fresh %APPDATA% per capture; D3D does not always exit, the process is killed ~8 s after
# "frame dumped", and only if its path lies under $Deploy.
$ErrorActionPreference = 'Continue'
$exeDir = Join-Path $Deploy 'Editor'
$exe    = Join-Path $exeDir 'HorizonEditor.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($k in @(Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' })) { Remove-Item "env:$($k.Name)" }
$appdata = Join-Path $Out "appdata\$Name"
if (Test-Path $appdata) { cmd /c rd /s /q "$appdata" }
New-Item -ItemType Directory -Force $appdata | Out-Null
$oldAppdata = $env:APPDATA
$env:APPDATA = $appdata
$env:HE_COLLAB_OFFLINE = '1'; $env:HE_NET_LOOPBACK_ONLY = '1'
$env:HE_DUMP_PATH = "$Out\$Name.bmp"; $env:HE_DUMP_QUIT = '1'
$env:HE_DUMP_RHI = $Rhi; $env:HE_DUMP_FRAMES = "$Frames"; $env:HE_DUMP_GI = "$Gi"
$env:HE_SKY_TIME = '6.2832'
$env:HE_DUMP_SKYTEST = '1'; $env:HE_DUMP_CLOUDMODE = '0'; $env:HE_DUMP_CLOUDSHADOWS = '0'; $env:HE_DUMP_COVERAGE = '0'
$env:HE_DUMP_RENDERPATH = '0'; $env:HE_DUMP_SSR = '0'; $env:HE_DUMP_GIREFL = '0'; $env:HE_DUMP_SSAO = '0'
$env:HE_DUMP_AA = '0'; $env:HE_DUMP_BLOOM = '0'; $env:HE_DUMP_DOF = '0'; $env:HE_DUMP_MOTIONBLUR = '0'
if ($Scene -eq 'bleed') {
    $env:HE_DUMP_MATERIALTEST = '1'; $env:HE_DUMP_GIBLEED = $Bleed
    $env:HE_DUMP_TOD = '0.5'; $env:HE_DUMP_CAMX = '0'; $env:HE_DUMP_CAMY = '2'; $env:HE_DUMP_CAMZ = '0'
    $env:HE_DUMP_PITCH = '-8'; $env:HE_DUMP_YAW = '0'
} elseif ($Scene -eq 'terrain') {
    $env:HE_DUMP_LANDSCAPELAYERS = '1'
    $env:HE_DUMP_TOD = '0.4'; $env:HE_DUMP_CAMX = '0'; $env:HE_DUMP_CAMY = '392'; $env:HE_DUMP_CAMZ = '0'
    $env:HE_DUMP_PITCH = '-89'
} else { throw "unknown scene $Scene" }
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
    if ($pp -and $pp.Path -like "$Deploy\*") { Stop-Process -Id $p.Id -Force }
}
Start-Sleep -Milliseconds 500
if (Test-Path $log) { Copy-Item $log "$Out\$Name.log" -Force }
foreach ($k in @(Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' })) { Remove-Item "env:$($k.Name)" }
$env:APPDATA = $oldAppdata
"$Name rhi=$Rhi gi=$Gi scene=$Scene bleed=$Bleed dumped=$dumped bmp=$(Test-Path "$Out\$Name.bmp") secs=$([int]((Get-Date)-$t0).TotalSeconds)"
