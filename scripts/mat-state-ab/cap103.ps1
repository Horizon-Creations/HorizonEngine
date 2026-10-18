# Material-draw state / UI batching A/B (REN-103/601/603,
# docs/audit-ren103-601-603-d3d11-gl-draw-state-2026-10-18.md).
#
# One HE_DUMP capture of the editor from a PRIVATE deploy on -Rhi (D3D11 or
# OpenGL), with a scratch APPDATA and a private FXC cache. -Env passes the
# witness scene and the same-binary "before" switches:
#   HE_D3D11_MAT_STATE_EAGER=1  D3D11 binds/restores the material state per draw again
#   HE_GL_UI_BATCH=0            GL draws every UI quad on its own again
#
#   .\cap103.ps1 -Exe C:\hw217\deploy\Editor\HorizonEditor.exe -OutDir C:\hw217\runs -Label land_new `
#       -Rhi D3D11 -Env @{ HE_DUMP_LANDSCAPELAYERS = "1"; HE_DUMP_CAMY = "392"; HE_DUMP_CAMZ = "0"; HE_DUMP_PITCH = "-89" }
#
# Writes <OutDir>\<Label>.bmp and <Label>.log and prints the witness lines (graph-
# material runs, UI batches), the dump counters and the HE_DUMP_GPUTIME line. D3D
# may sit in Shutdown for a while: the process is killed 15 s after the last
# expected line, never the user's editor (only the PID started here).
# Diff two captures with ..\d3d12-static-heap\diff202.py.
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$OutDir,
    [Parameter(Mandatory)] [string]$Label,
    [ValidateSet("D3D11", "OpenGL", "D3D12", "Vulkan")] [string]$Rhi = "D3D11",
    [hashtable]$Env = @{},
    [int]$TimeoutSec = 300
)
$ErrorActionPreference = "Stop"
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } |
    ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, $null) }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$appdata = Join-Path $OutDir "appdata\$Label"
if (Test-Path $appdata) { Remove-Item $appdata -Recurse -Force }
New-Item -ItemType Directory -Force "$appdata\HorizonEngine" | Out-Null

$log = Join-Path (Split-Path $Exe) "HorizonEngine.log"
if (Test-Path $log) { Remove-Item $log -Force }
$bmp = Join-Path $OutDir "$Label.bmp"
if (Test-Path $bmp) { Remove-Item $bmp -Force }

$env:APPDATA = $appdata
$env:HE_HIDDEN_WINDOW = "1"; $env:HE_COLLAB_OFFLINE = "1"; $env:HE_NET_LOOPBACK_ONLY = "1"
$env:HE_D3D_SHADER_CACHE_DIR = Join-Path $OutDir "d3dshadercache"
$env:HE_DUMP_PATH = $bmp; $env:HE_DUMP_QUIT = "1"; $env:HE_DUMP_RHI = $Rhi
$env:HE_DUMP_FRAMES = "16"; $env:HE_SKY_TIME = "0"
$env:HE_DUMP_SKYTEST = "1"; $env:HE_DUMP_TOD = "0.35"; $env:HE_DUMP_PITCH = "-6"
$env:HE_DUMP_CAMX = "0"; $env:HE_DUMP_CAMY = "3.5"; $env:HE_DUMP_CAMZ = "3"
foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, [string]$Env[$k]) }

$wantGpuTime = [bool]$env:HE_DUMP_GPUTIME
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -PassThru
$doneAt = $null
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    if (-not $doneAt -and (Test-Path $log)) {
        $dumped = Select-String -Path $log -Pattern 'frame dumped|frame dump failed' -Quiet
        $timed  = (-not $wantGpuTime) -or (Select-String -Path $log -Pattern 'HE_DUMP_GPUTIME' -Quiet)
        if ($dumped -and $timed) { $doneAt = $sw.Elapsed.TotalSeconds }
    }
    if ($doneAt -and $sw.Elapsed.TotalSeconds - $doneAt -gt 15) { "shutdown slow - killing PID $($p.Id)"; $p.Kill(); break }
    Start-Sleep -Milliseconds 200
}
if (-not $p.HasExited) { "TIMEOUT - killing PID $($p.Id)"; $p.Kill() }
Start-Sleep -Milliseconds 500
Copy-Item $log (Join-Path $OutDir "$Label.log") -Force
"[$Label] $Rhi exit=$($p.ExitCode) wall=$([math]::Round($sw.Elapsed.TotalSeconds,1)) s bmp=$(Test-Path $bmp)"
Select-String -Path $log -Pattern 'graph-material draws|UI pass \d+ quads|dump counters|HE_DUMP_GPUTIME|frame dump' |
    ForEach-Object { "  " + $_.Line.Trim() }
$err = @(Select-String -Path $log -Pattern '\[ERROR\]|link failed|compile failed|GL error' -CaseSensitive:$false)
"  error lines: $($err.Count)"
$err | Select-Object -First 5 | ForEach-Object { "    " + $_.Line.Trim() }
