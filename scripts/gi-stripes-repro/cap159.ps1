param(
    [string]$Name, [string]$Rhi, [int]$Gi = 1, [int]$Frames = 60,
    [ValidateSet('mountain', 'layers')] [string]$Scene = 'mountain',
    [int]$Sphere = 1,
    [hashtable]$Extra = @{},
    [string]$Root = 'C:\hw159'   # private build tree: $Root\deploy\Editor, captures in $Root\cap
)
# One headless capture for Thema 159 (black stripes with GI on, Vulkan/D3D).
# Scene, both at y=300, clear of any loaded scene:
#   mountain  HE_DUMP_MOUNTAINTEST=before: 240 m rolling landscape, BUILT-IN terrain material
#   layers    HE_DUMP_LANDSCAPELAYERS=1: 100 m flat landscape, GRAPH material (layer blend)
# -Sphere 1 adds HE_DUMP_MATERIALTEST=1 (graph-material sphere 8 m in front of the camera).
# Sun at TOD 0.35, clouds off, every temporal/post pass off, HE_SKY_TIME pinned, fresh
# %APPDATA% per capture. Never the user's HorizonEngineBuild. Derived from
# scripts/gi-shadow-repro/cap.ps1 (same kill-after-dump logic for D3D).
$ErrorActionPreference = 'Continue'
$exeDir = Join-Path $Root 'deploy\Editor'
$exe    = Join-Path $exeDir 'HorizonEditor.exe'
$out    = Join-Path $Root 'cap'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($k in @(Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' })) { Remove-Item "env:$($k.Name)" }
$appdata = Join-Path $Root "appdata\$Name"
if (Test-Path $appdata) { cmd /c rd /s /q "$appdata" }
New-Item -ItemType Directory -Force $appdata | Out-Null
$oldAppdata = $env:APPDATA
$env:APPDATA = $appdata
$env:HE_COLLAB_OFFLINE = '1'; $env:HE_NET_LOOPBACK_ONLY = '1'
$env:HE_SKY_TIME = '10'
$env:HE_DUMP_PATH = "$out\$Name.bmp"; $env:HE_DUMP_QUIT = '1'
$env:HE_DUMP_RHI = $Rhi; $env:HE_DUMP_FRAMES = "$Frames"; $env:HE_DUMP_GI = "$Gi"
$env:HE_DUMP_SKYTEST = '1'
if ($Scene -eq 'mountain') { $env:HE_DUMP_MOUNTAINTEST = 'before' } else { $env:HE_DUMP_LANDSCAPELAYERS = '1' }
if ($Sphere) { $env:HE_DUMP_MATERIALTEST = '1' }
$env:HE_DUMP_TOD = '0.35'; $env:HE_DUMP_PITCH = '-20'; $env:HE_DUMP_CAMY = '306'
$env:HE_DUMP_CAMX = '0'; $env:HE_DUMP_CAMZ = '20'; $env:HE_DUMP_CLOUDMODE = '0'; $env:HE_DUMP_CLOUDSHADOWS = '0'
$env:HE_DUMP_COVERAGE = '0'
$env:HE_DUMP_SSAO = '0'; $env:HE_DUMP_AA = '0'; $env:HE_DUMP_MOTIONBLUR = '0'; $env:HE_DUMP_DOF = '0'
$env:HE_DUMP_BLOOM = '0'; $env:HE_DUMP_SSR = '0'; $env:HE_DUMP_RENDERPATH = '0'
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
foreach ($k in @(Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' })) { Remove-Item "env:$($k.Name)" }
$env:APPDATA = $oldAppdata
"$Name rhi=$Rhi gi=$Gi scene=$Scene frames=$Frames dumped=$dumped bmp=$(Test-Path "$out\$Name.bmp") secs=$([int]((Get-Date)-$t0).TotalSeconds)"
