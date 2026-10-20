# Thema 190 step 6: GI seam captures on Windows (D3D11 / D3D12 / Vulkan / OpenGL),
# the twin of cap_metal.sh's mountain-witness runs from steps 1 and 2.
#
#   Set-ExecutionPolicy -Scope Process Bypass -Force
#   & .\cap190.ps1 -Deploy C:\hw190\deploy\Editor -Out C:\hw190\shots -Name near -Variant stitched
#   & .\cap190.ps1 ... -Variant nostitch      # HE_DUMP_NOSTITCH=1, the negative control
#   & .\cap190.ps1 ... -Variant lod0          # LODSCALE 1: every chunk LOD0, the relief alone
#
# Scene: HE_DUMP_MOUNTAINTEST=after (240 m landscape at y=300, 4x4 chunks of 60 m,
# seams at x/z = -60, 0, 60; MTROUGH=1 for the rough relief), HE_DUMP_TERRAINTICK=1
# runs TerrainTess -> LOD -> TerrainSeams from the dump camera (the dump never ticks
# the world otherwise). The log line "HE_DUMP_TERRAINTICK chunks LOD0=.. stitched=.."
# is the witness that the seams in frame are mixed ones. Built-in terrain material.
# D3D11/D3D12 may hang or crash on exit after the dump (known): killed 4 s after
# "frame dumped". Every capture gets a fresh APPDATA (the editor rewrites config.json).
param(
    [Parameter(Mandatory = $true)][string]$Deploy,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Name,
    [ValidateSet("stitched", "nostitch", "lod0")][string]$Variant = "stitched",
    [string[]]$Backends = @("D3D11", "D3D12", "Vulkan"),
    [string]$Gi = "1",
    [string]$Tod = "0.4",
    [string]$RenderPath = "0",
    [string]$CamX = "15", [string]$CamY = "360", [string]$CamZ = "45",
    [string]$Pitch = "-40", [string]$Yaw = "",
    [string]$LodScale = "0.1",
    [string]$Rough = "1",
    [hashtable]$Extra = @{}
)
$ErrorActionPreference = "Stop"
$Backends = @($Backends | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$exe = Join-Path $Deploy "HorizonEditor.exe"
if (-not (Test-Path $exe)) { throw "no editor at $exe" }
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Deploy "HorizonEngine.log"
$savedAppData = $env:APPDATA

foreach ($rhi in $Backends) {
    Get-ChildItem env: | Where-Object { $_.Name -like "HE_DUMP_*" -or $_.Name -like "HE_GI*" -or
                                        $_.Name -eq "HE_WORLD_PREVIEW_DUMP" -or $_.Name -eq "HE_GPU_DEBUG" } |
        ForEach-Object { Remove-Item "env:$($_.Name)" }
    $tag = "$Name-$Variant-$rhi"
    $bmp = Join-Path $Out "$tag.bmp"
    $app = Join-Path $Out "appdata-$tag"
    if (Test-Path $bmp) { Remove-Item $bmp }
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    New-Item -ItemType Directory -Force "$app\HorizonEngine" | Out-Null

    $env:APPDATA = $app
    $env:HE_COLLAB_OFFLINE = "1"
    $env:HE_NET_LOOPBACK_ONLY = "1"
    $env:HE_SKY_TIME = "30"
    $env:HE_DUMP_PATH = $bmp
    $env:HE_DUMP_QUIT = "1"
    $env:HE_DUMP_RHI = $rhi
    $env:HE_DUMP_FRAMES = "24"
    $env:HE_DUMP_SKYTEST = "1"
    $env:HE_DUMP_TOD = $Tod
    $env:HE_DUMP_COVERAGE = "0"
    $env:HE_DUMP_CLOUDMODE = "0"
    $env:HE_DUMP_CLOUDSHADOWS = "0"
    $env:HE_DUMP_CAMX = $CamX; $env:HE_DUMP_CAMY = $CamY; $env:HE_DUMP_CAMZ = $CamZ
    $env:HE_DUMP_PITCH = $Pitch
    if ($Yaw) { $env:HE_DUMP_YAW = $Yaw }
    $env:HE_DUMP_GI = $Gi; $env:HE_DUMP_SSAO = "0"; $env:HE_DUMP_SSR = "0"
    $env:HE_DUMP_AA = "0"; $env:HE_DUMP_BLOOM = "0"; $env:HE_DUMP_DOF = "0"; $env:HE_DUMP_MOTIONBLUR = "0"
    $env:HE_DUMP_RENDERPATH = $RenderPath
    $env:HE_DUMP_MOUNTAINTEST = "after"
    $env:HE_DUMP_MTROUGH = $Rough
    $env:HE_DUMP_TERRAINTICK = "1"
    $env:HE_DUMP_LODSCALE = $(if ($Variant -eq "lod0") { "1" } else { $LodScale })
    if ($Variant -eq "nostitch") { $env:HE_DUMP_NOSTITCH = "1" }
    foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

    if (Test-Path $log) { Remove-Item $log -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath $exe -WorkingDirectory $Deploy -PassThru -WindowStyle Minimized
    $deadline = (Get-Date).AddSeconds(180)
    $dumped = $null
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern "frame dumped" -Quiet)) {
            if (-not $dumped) { $dumped = Get-Date }
            elseif (((Get-Date) - $dumped).TotalSeconds -gt 4) { break }
        }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if (Test-Path $log) { Copy-Item $log (Join-Path $Out "$tag.log") -Force }
    $tick = ""
    if (Test-Path (Join-Path $Out "$tag.log")) {
        $m = Select-String -Path (Join-Path $Out "$tag.log") -Pattern "HE_DUMP_TERRAINTICK chunks (.*)" | Select-Object -First 1
        if ($m) { $tick = $m.Matches[0].Groups[1].Value }
    }
    "{0,-34} bmp={1} {2}" -f $tag, (Test-Path $bmp), $tick
}
$env:APPDATA = $savedAppData
