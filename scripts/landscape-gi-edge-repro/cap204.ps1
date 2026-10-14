# Thema 204: wrong GI shadow lines on steep landscape slopes.
#
#   Set-ExecutionPolicy -Scope Process Bypass -Force
#   & .\cap204.ps1 -Deploy C:\hw204\deploy -Out C:\hw204\shots -Name gi1 -Gi 1
#   & .\cap204.ps1 ... -Name gi1_noind -Gi 1 -Cfg @{GIIndirectIntensity=0.0}
#
# The HE_DUMP_AUTOLAND witness (EditorApplication.cpp): a 128 m terrain at
# y=300, 2x2 chunks (chunk seams at x=0 and z=0), a smoothstep ramp from
# x=-24 to x=8 (0 -> ~62 degrees at x=-8 -> 0) up to a 40 m plateau, constant
# along Z. -Mode builtin = the default terrain material (no textures, no
# normal map), so every line in the picture is lighting.
#
# -Cfg writes a config.json with just those CustomConfig keys into the fresh
# APPDATA (missing keys keep their defaults): GIIndirectIntensity,
# GILightRadius, GIShadowQuality, ... -Extra sets further env vars
# (HE_DUMP_GISHADOWRAYS, HE_GI_REFERENCE, HE_DUMP_VIEWMODE, ...).
# D3D11/D3D12 may hang on exit after the dump (known): killed 4 s after
# "frame dumped".
param(
    [Parameter(Mandatory = $true)][string]$Deploy,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Mode = "builtin",
    [string]$Gi = "1",
    [string[]]$Backends = @("D3D12"),
    [string]$Tod = "0.4",
    [string]$RenderPath = "0",   # 0 forward, 1 deferred (the editor default on NN-WS03)
    [string]$CamX = "0", [string]$CamY = "400", [string]$CamZ = "0",
    [string]$Pitch = "-89", [string]$Yaw = "",
    [hashtable]$Cfg = @{},
    [hashtable]$Extra = @{},
    # A .heproj to open before the dump (its startup scene is rendered). Pass a
    # COPY: the editor may write into the project. -Mode '' then, so the
    # AUTOLAND witness is not added on top.
    [string]$Project = ""
)
$ErrorActionPreference = "Stop"
$Backends = @($Backends | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$exe = Join-Path $Deploy "HorizonEditor.exe"
if (-not (Test-Path $exe)) { throw "no editor at $exe" }
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Deploy "HorizonEngine.log"
$savedAppData = $env:APPDATA

foreach ($rhi in $Backends) {
    Get-ChildItem env: | Where-Object { $_.Name -like "HE_DUMP_*" -or $_.Name -like "HE_GI*" } |
        ForEach-Object { Remove-Item "env:$($_.Name)" }
    $tag = "$Name-$rhi"
    $bmp = Join-Path $Out "$tag.bmp"
    $app = Join-Path $Out "appdata-$tag"
    if (Test-Path $bmp) { Remove-Item $bmp }
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    New-Item -ItemType Directory -Force "$app\HorizonEngine" | Out-Null
    if ($Cfg.Count -gt 0 -or $Project) {
        $entries = @($Cfg.Keys | ForEach-Object { @{ Key = $_; Value = $Cfg[$_] } })
        $doc = @{ CustomConfig = $entries }
        # Forward slashes: the config reader wants them (memory: MCP bridge).
        if ($Project) { $doc.LastProjectPath = ($Project -replace '\\', '/') }
        $json = $doc | ConvertTo-Json -Depth 4
        [System.IO.File]::WriteAllText("$app\HorizonEngine\config.json", $json)
    }

    $env:APPDATA = $app
    $env:HE_COLLAB_OFFLINE = "1"
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
    if ($Mode) { $env:HE_DUMP_AUTOLAND = $Mode }
    foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

    if (Test-Path $log) { Remove-Item $log -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath $exe -WorkingDirectory $Deploy -PassThru
    $deadline = (Get-Date).AddSeconds(150)
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
    "{0,-28} bmp={1}" -f $tag, (Test-Path $bmp)
}
$env:APPDATA = $savedAppData
