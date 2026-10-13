# D3D11/D3D12 startup timing run (Thema 196, docs/d3d-startzeit-messung-2026-10-13.md).
#
# Starts the editor hidden on one backend, waits for HE_EXIT_AFTER_FRAMES, copies
# HorizonEngine.log to <OutDir>\<Label>.log and prints the [startup] timeline plus
# the summed D3DCompile time. Run it against a PRIVATE deploy (-DDEPLOY_DIR=...),
# never the user's editor.
#
#   .\measure.ps1 -Exe C:\hwNN\deploy\Editor\HorizonEditor.exe -OutDir C:\hwNN\runs -Rhi 2 -Label d3d11_cold -Cold
#   .\measure.ps1 ... -Rhi 3 -Label d3d12_proj -Project C:\hwNN\proj\X\X.heproj
#
# The backend comes from a scratch config.json ("RHI": 1 Vulkan, 2 D3D11, 3 D3D12):
# HE_DUMP_RHI outside dump mode crashes D3D at frame 1. -Cold deletes the FXC
# bytecode cache and this exe's NVIDIA DXCache files. The bytecode cache sits in the
# REAL roaming folder (SDL_GetPrefPath ignores %APPDATA%), so a scratch APPDATA does
# not make a run cold by itself.
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$OutDir,
    [Parameter(Mandatory)] [int]$Rhi,
    [Parameter(Mandatory)] [string]$Label,
    [switch]$Cold,
    [string]$Project = "",     # .heproj to open, or empty for the bare editor
    [int]$Frames = 300,
    [string]$Dump = ""         # HE_D3D_SHADER_DUMP: every compiled HLSL source as a file
)
$ErrorActionPreference = "Stop"
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } |
    ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, $null) }
New-Item -ItemType Directory -Force $OutDir | Out-Null

# NVIDIA keeps one DXCache file per exe path; the first run records which one is ours.
$dx = "$env:LOCALAPPDATA\NVIDIA\DXCache"
$mineList = Join-Path $OutDir "dxcache_mine.txt"
if ($Cold -and (Test-Path $mineList)) {
    $n = 0
    foreach ($name in Get-Content $mineList) {
        $p = Join-Path $dx $name
        if (Test-Path $p) { Remove-Item $p -Force; $n++ }
    }
    "cold: removed $n DXCache files of this exe"
}
$before = @{}
if (Test-Path $dx) { Get-ChildItem $dx -File | ForEach-Object { $before[$_.Name] = $_.LastWriteTimeUtc } }

$sc = "$env:USERPROFILE\AppData\Roaming\HorizonCreations\HorizonEngine\d3dshadercache"
if ($Cold -and (Test-Path $sc)) { Remove-Item $sc -Recurse -Force; "cold: removed d3dshadercache" }

$appdata = Join-Path $OutDir "appdata\rhi$Rhi"
New-Item -ItemType Directory -Force "$appdata\HorizonEngine" | Out-Null
$cfg = @{ RHI = $Rhi }
if ($Project) { $cfg.LastProjectPath = $Project.Replace('\', '/') }
$cfg | ConvertTo-Json | Set-Content -Encoding ascii "$appdata\HorizonEngine\config.json"

# Delete the old log first: it is only truncated once the new process gets to it.
$log = Join-Path (Split-Path $Exe) "HorizonEngine.log"
if (Test-Path $log) { Remove-Item $log -Force }

$env:APPDATA = $appdata
$env:HE_HIDDEN_WINDOW = "1"; $env:HE_BACKGROUND_FPS = "0"
$env:HE_EXIT_AFTER_FRAMES = "$Frames"; $env:HE_COLLAB_OFFLINE = "1"
if ($Dump) { $env:HE_D3D_SHADER_DUMP = $Dump }

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -PassThru
$firstFrameWall = $null
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt 300) {
    if (-not $firstFrameWall -and (Test-Path $log) -and
        (Select-String -Path $log -Pattern 'app first frame presented' -Quiet)) {
        $firstFrameWall = $sw.Elapsed.TotalMilliseconds
    }
    Start-Sleep -Milliseconds 50
}
if (-not $p.HasExited) { "TIMEOUT - killing"; $p.Kill() }
$total = $sw.Elapsed.TotalMilliseconds

$out = Join-Path $OutDir "$Label.log"
Copy-Item $log $out -Force

$new = @()
if (Test-Path $dx) {
    $new = Get-ChildItem $dx -File | Where-Object { -not $before.ContainsKey($_.Name) -or $before[$_.Name] -ne $_.LastWriteTimeUtc }
}
if (-not (Test-Path $mineList) -and $new) { $new.Name | Set-Content $mineList }
"exit=$($p.ExitCode) wallFirstFrame~=$([math]::Round($firstFrameWall)) ms (poll 50 ms) wallTotal=$([math]::Round($total)) ms; DXCache new/changed files: $(@($new).Count)"
$lines = Select-String -Path $out -Pattern '\[startup\]' | ForEach-Object { $_.Line }
$compiles = $lines | Where-Object { $_ -match 'D3DCompile ' }
$sum = 0.0; foreach ($l in $compiles) { if ($l -match ' ms=([\d.]+)') { $sum += [double]$Matches[1] } }
"D3DCompile lines=$(@($compiles).Count) cached=$(@($compiles | Where-Object { $_ -match ' cached' }).Count) sumMs=$([math]::Round($sum))"
if (Test-Path $sc) { "cache files: $((Get-ChildItem $sc -File).Count)" }
$lines | Where-Object { $_ -notmatch 'D3DCompile ' }
