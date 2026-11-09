# Thema 233, step 5: Windows port of cap233.sh — group witnesses (HE_DUMP_AUTOLAND +
# HE_DUMP_AUTOLANDGROUP) per backend (D3D11/D3D12/Vulkan/OpenGL; Metal/GL already run on the Mac).
#
#   cap233_win.ps1 -Out <dir> -Src <arraydir> -Group Mountains|Forest|Grassland -Rhi D3D11|D3D12|Vulkan|OpenGL [-Tag t] [-Ed <editor dir>] [-Extra "HE_DUMP_X=.."]
#
# Src holds the group's three arrays (read only; Mountains: %APPDATA%\HorizonEngine\EngineContentCache\Textures\Landscape,
# Forest/Grassland: `landscape_tex_gen <dir> --arrays-only --group <G>` placeholders). Every run gets a private
# APPDATA + HE_CONFIG_DIR, so the real cache/config is never touched. GI/SSAO/AA/bloom off, 16 frames, quit after
# dump — same pinning as cap233.sh. Prints md5, missing-asset count, the dump-counters line.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [Parameter(Mandatory=$true)][string]$Src,
    [Parameter(Mandatory=$true)][string]$Group,
    [Parameter(Mandatory=$true)][string]$Rhi,
    [string]$Tag = "",
    [string]$Ed = "C:\hb\l7deploy\Editor",
    [string[]]$Extra = @()
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path "$Ed\HorizonEditor.exe")) { throw "no editor at $Ed" }
New-Item -ItemType Directory -Force $Out | Out-Null
$pfx = if ($Group -ne "Mountains") { "${Group}_" } else { "" }
$name = "$Group-$Rhi" + $(if ($Tag) { "-$Tag" } else { "" })
$bmp = "$Out\$name.bmp"; $outLog = "$Out\$name.log"
$cfg = "$Out\cfg-$name"; $homeDir = "$Out\home-$name"
Remove-Item $bmp, $cfg, $homeDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cfg | Out-Null
$cache = "$homeDir\HorizonEngine\EngineContentCache\Textures\Landscape"
New-Item -ItemType Directory -Force $cache | Out-Null
foreach ($f in "Albedo", "Normal", "Mask") {
    Copy-Item "$Src\T_Landscape_${pfx}${f}_Array.hasset" "$cache\T_Landscape_${pfx}${f}_Array.hasset" -Force
}
$realAppData = $env:APPDATA
Get-ChildItem env: | Where-Object { $_.Name -like "HE_DUMP_*" } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:APPDATA = $homeDir
$env:HE_CONFIG_DIR = $cfg
$env:HE_COLLAB_OFFLINE = "1"; $env:HE_NET_LOOPBACK_ONLY = "1"; $env:HE_SKY_TIME = "30"
$env:HE_DUMP_PATH = $bmp; $env:HE_DUMP_QUIT = "1"; $env:HE_DUMP_RHI = $Rhi; $env:HE_DUMP_FRAMES = "16"
$env:HE_DUMP_SKYTEST = "1"; $env:HE_DUMP_TOD = "0.4"; $env:HE_DUMP_COVERAGE = "0"; $env:HE_DUMP_CLOUDMODE = "0"
$env:HE_DUMP_CLOUDSHADOWS = "0"; $env:HE_DUMP_CAMX = "0"; $env:HE_DUMP_CAMY = "400"; $env:HE_DUMP_CAMZ = "0"; $env:HE_DUMP_PITCH = "-89"
$env:HE_DUMP_GI = "0"; $env:HE_DUMP_SSAO = "0"; $env:HE_DUMP_SSR = "0"; $env:HE_DUMP_AA = "0"; $env:HE_DUMP_BLOOM = "0"
$env:HE_DUMP_DOF = "0"; $env:HE_DUMP_MOTIONBLUR = "0"; $env:HE_DUMP_RENDERPATH = "0"; $env:HE_DUMP_AUTOLAND = "1"
if ($Group -ne "Mountains") { $env:HE_DUMP_AUTOLANDGROUP = $Group }
foreach ($kv in $Extra) { if ($kv) { $k, $v = $kv -split "=", 2; Set-Item "env:$k" $v } }

$engineLog = "$Ed\HorizonEngine.log"
Remove-Item $engineLog -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$Ed\HorizonEditor.exe" -WorkingDirectory $Ed -PassThru
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 400
    if ((Test-Path $engineLog) -and (Select-String -Path $engineLog -Pattern "frame dumped" -Quiet)) { Start-Sleep 1; break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 300
$env:APPDATA = $realAppData
if (Test-Path $engineLog) { Copy-Item $engineLog $outLog -Force }
$missing = 0
$counters = ""
if (Test-Path $outLog) {
    $missing = (Select-String -Path $outLog -Pattern "Cannot load asset" -AllMatches -ErrorAction SilentlyContinue | Measure-Object).Count
    $m = Select-String -Path $outLog -Pattern "dump counters.*" | Select-Object -Last 1
    if ($m) { $counters = $m.Matches[0].Value }
}
$md5 = if (Test-Path $bmp) { (Get-FileHash $bmp -Algorithm MD5).Hash } else { "NO-BMP" }
if (Test-Path $bmp) {
    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Image]::FromFile($bmp)
    $img.Save("$Out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
}
Remove-Item $homeDir -Recurse -Force -ErrorAction SilentlyContinue
"{0,-28} md5={1} missing-logs={2} {3}" -f $name, $md5, $missing, $counters
