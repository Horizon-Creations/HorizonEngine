# Thema 158, Schritt 2: the landscape layer witness on every Windows backend.
#
#   powershell -File cap158.ps1 -Deploy C:\hw158\deploy\Editor -Out C:\hw158\shots -Layers 8
#
# Renders HE_DUMP_LANDSCAPELAYERS (=1: three layers, =8: eight layers, five of
# them on the second weight page) top-down, in the Unlit view so the frame IS
# the layer blend — no lighting, shadow or GI difference between backends can
# hide or fake a weightmap difference. One fresh APPDATA per capture (a clean
# exit rewrites config.json), HE_COLLAB_OFFLINE so a new deploy path raises no
# firewall prompt. D3D11/D3D12 crash or hang on exit after the dump (known,
# pre-existing): the BMP is written first, so the process is killed a few
# seconds after "frame dumped" appears in HorizonEngine.log next to the exe.
param(
    [Parameter(Mandatory = $true)][string]$Deploy,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Layers = "8",
    [string]$ViewMode = "unlit",
    [string[]]$Backends = @("OpenGL", "D3D11", "D3D12", "Vulkan"),
    [string]$Tag = ""
)
$ErrorActionPreference = "Stop"
# "-Backends OpenGL,D3D11" through powershell -File arrives as ONE string.
$Backends = @($Backends | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$exe = Join-Path $Deploy "HorizonEditor.exe"
if (-not (Test-Path $exe)) { throw "no editor at $exe" }
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Deploy "HorizonEngine.log"

foreach ($rhi in $Backends) {
    # Clear everything a previous capture in this process may have set.
    Get-ChildItem env: | Where-Object { $_.Name -like "HE_DUMP_*" } | ForEach-Object { Remove-Item "env:$($_.Name)" }
    $name = "L$Layers-$ViewMode-$rhi$Tag"
    $bmp = Join-Path $Out "$name.bmp"
    $app = Join-Path $Out "appdata-$name"
    if (Test-Path $bmp) { Remove-Item $bmp }
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    New-Item -ItemType Directory -Force $app | Out-Null

    $env:APPDATA = $app
    $env:HE_COLLAB_OFFLINE = "1"
    $env:HE_DUMP_PATH = $bmp
    $env:HE_DUMP_QUIT = "1"
    $env:HE_DUMP_RHI = $rhi
    $env:HE_DUMP_FRAMES = "16"
    $env:HE_DUMP_SKYTEST = "1"
    $env:HE_DUMP_TOD = "0.4"
    $env:HE_DUMP_COVERAGE = "0"
    $env:HE_DUMP_CLOUDMODE = "0"
    $env:HE_DUMP_CAMX = "0"; $env:HE_DUMP_CAMY = "392"; $env:HE_DUMP_CAMZ = "0"
    $env:HE_DUMP_PITCH = "-89"
    $env:HE_DUMP_GI = "0"; $env:HE_DUMP_SSAO = "0"; $env:HE_DUMP_SSR = "0"
    $env:HE_DUMP_AA = "0"; $env:HE_DUMP_BLOOM = "0"; $env:HE_DUMP_DOF = "0"; $env:HE_DUMP_MOTIONBLUR = "0"
    $env:HE_DUMP_VIEWMODE = $ViewMode
    $env:HE_DUMP_LANDSCAPELAYERS = $Layers

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
        $w = Select-String -Path $log -Pattern "LANDSCAPELAYERS witness" | Select-Object -First 1
        $e = (Select-String -Path $log -Pattern "\[ERROR\]|validation|failed" | Measure-Object).Count
        "{0,-28} bmp={1} errors/validation lines={2} {3}" -f $name, (Test-Path $bmp), $e, ($(if ($w) { $w.Line.Trim() } else { "(no witness line)" }))
    } else {
        "{0,-28} bmp={1} (no log)" -f $name, (Test-Path $bmp)
    }
}
