# Thema 158, Schritt 5: the auto landscape material witness on every Windows backend.
#
#   powershell -File cap158auto.ps1 -Deploy C:\hw158\deploy\Editor -Out C:\hw158\shots5 -Mode 1
#
# HE_DUMP_AUTOLAND (EditorApplication.cpp): an analytic 128 m landscape at y=300
# (flat plain, ramp up to ~62 degrees, 40 m plateau) with the auto landscape
# material, top-down. Modes: 1 (the shipped asset through a Material Instance),
# nobomb (bombing switch off), masks / ground / normal / surface (unlit debug
# views of the material's masks and lighting inputs), builtin (the default
# terrain material: the control for lighting/shadow differences between
# backends). Same camera and settings as cap158auto.sh (macOS), so all five
# backends compare with ana158auto.py diff. One fresh APPDATA per capture,
# HE_COLLAB_OFFLINE, sky time pinned. D3D11/D3D12 crash or hang on exit after
# the dump (known): the process is killed a few seconds after "frame dumped".
#
# Shadows: the ramp foot lies in the ramp's cast shadow (TOD 0.4, docs §11.3).
# OpenGL FORWARD gives graph materials no sun shadow at all (csmSplits.w = 0),
# every other path draws it -- so with shadows GL forward is no reference. For
# the MATERIAL comparison against GL pass -Extra @{HE_DUMP_SHADOW='0.1'}
# (shadow distance 0.1 m) with -Tag _s01, and check the shadow per backend with
# ana158auto.py shadow AL<mode>-<rhi>.bmp AL<mode>-<rhi>_s01.bmp.
# -Extra is a hashtable, so call the script IN-PROCESS (powershell -File turns
# it into a string):
#   Set-ExecutionPolicy -Scope Process Bypass -Force
#   & .\cap158auto.ps1 -Deploy ... -Out ... -Mode 1 -Extra @{HE_DUMP_SHADOW='0.1'} -Tag _s01
# D3D11/D3D12/Vulkan give graph materials no sky IBL (fog.z = 0), so their
# shadow ratios come out lower than Metal's (0.30 / 0.48 vs 0.37 / 0.56, §12).
param(
    [Parameter(Mandatory = $true)][string]$Deploy,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Mode = "1",
    [string[]]$Backends = @("OpenGL", "D3D11", "D3D12", "Vulkan"),
    [string]$Tag = "",
    [hashtable]$Extra = @{}
)
$ErrorActionPreference = "Stop"
$Backends = @($Backends | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$exe = Join-Path $Deploy "HorizonEditor.exe"
if (-not (Test-Path $exe)) { throw "no editor at $exe" }
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Deploy "HorizonEngine.log"

foreach ($rhi in $Backends) {
    Get-ChildItem env: | Where-Object { $_.Name -like "HE_DUMP_*" } | ForEach-Object { Remove-Item "env:$($_.Name)" }
    $name = "AL$Mode-$rhi$Tag"
    $bmp = Join-Path $Out "$name.bmp"
    $app = Join-Path $Out "appdata-$name"
    if (Test-Path $bmp) { Remove-Item $bmp }
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    New-Item -ItemType Directory -Force $app | Out-Null

    $env:APPDATA = $app
    $env:HE_COLLAB_OFFLINE = "1"
    $env:HE_SKY_TIME = "30"
    $env:HE_DUMP_PATH = $bmp
    $env:HE_DUMP_QUIT = "1"
    $env:HE_DUMP_RHI = $rhi
    $env:HE_DUMP_FRAMES = "16"
    $env:HE_DUMP_SKYTEST = "1"
    $env:HE_DUMP_TOD = "0.4"
    $env:HE_DUMP_COVERAGE = "0"
    $env:HE_DUMP_CLOUDMODE = "0"
    $env:HE_DUMP_CLOUDSHADOWS = "0"
    $env:HE_DUMP_CAMX = "0"; $env:HE_DUMP_CAMY = "400"; $env:HE_DUMP_CAMZ = "0"
    $env:HE_DUMP_PITCH = "-89"
    $env:HE_DUMP_GI = "0"; $env:HE_DUMP_SSAO = "0"; $env:HE_DUMP_SSR = "0"
    $env:HE_DUMP_AA = "0"; $env:HE_DUMP_BLOOM = "0"; $env:HE_DUMP_DOF = "0"; $env:HE_DUMP_MOTIONBLUR = "0"
    $env:HE_DUMP_RENDERPATH = "0"
    $env:HE_DUMP_AUTOLAND = $Mode
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
    if (Test-Path $log) {
        Copy-Item $log (Join-Path $Out "$name.log") -Force
        $w = Select-String -Path $log -Pattern "AUTOLAND witness" | Select-Object -First 1
        $e = (Select-String -Path $log -Pattern "\[ERROR\]|validation|failed" | Measure-Object).Count
        "{0,-24} bmp={1} errors/validation lines={2} {3}" -f $name, (Test-Path $bmp), $e, ($(if ($w) { $w.Line.Trim() } else { "(no witness line)" }))
    } else {
        "{0,-24} bmp={1} (no log)" -f $name, (Test-Path $bmp)
    }
}
