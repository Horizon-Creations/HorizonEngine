# D3D12 static geometry A/B (REN-202, docs/audit/ren202-d3d12-default-heap-2026-10-18.md).
#
# One HE_DUMP capture of the editor on D3D12 from a PRIVATE deploy, with a scratch
# APPDATA and a private FXC cache. -UploadHeap sets HE_D3D12_STATIC_UPLOAD_HEAP=1:
# the same binary with meshes + GI BVH back in the UPLOAD heap (the "before").
# -Env passes the witness scene / feature knobs, e.g.
#
#   .\cap202.ps1 -Exe C:\hw217\deploy\Editor\HorizonEditor.exe -OutDir C:\hw217\runs -Label sec_def `
#       -Env @{ HE_DUMP_SECTIONTEST = "1" }
#   .\cap202.ps1 ... -Label fol_up -UploadHeap -Env @{ HE_DUMP_FOLIAGETEST = "200000"; HE_DUMP_GPUTIME = "300" }
#
# Writes <OutDir>\<Label>.bmp and <Label>.log and prints the REN-202 witness lines,
# the dump counters and the HE_DUMP_GPUTIME line. D3D12 may sit in Shutdown for a
# while: the process is killed 15 s after the last expected line, never the user's
# editor (only the PID started here).
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$OutDir,
    [Parameter(Mandatory)] [string]$Label,
    [switch]$UploadHeap,
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
$env:HE_HIDDEN_WINDOW = "1"; $env:HE_COLLAB_OFFLINE = "1"
$env:HE_D3D_SHADER_CACHE_DIR = Join-Path $OutDir "d3dshadercache"
$env:HE_DUMP_PATH = $bmp; $env:HE_DUMP_QUIT = "1"; $env:HE_DUMP_RHI = "D3D12"
$env:HE_DUMP_FRAMES = "16"; $env:HE_SKY_TIME = "0"
$env:HE_DUMP_SKYTEST = "1"; $env:HE_DUMP_TOD = "0.35"; $env:HE_DUMP_PITCH = "-6"
$env:HE_DUMP_CAMX = "0"; $env:HE_DUMP_CAMY = "3.5"; $env:HE_DUMP_CAMZ = "3"
if ($UploadHeap) { $env:HE_D3D12_STATIC_UPLOAD_HEAP = "1" }
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
"[$Label] exit=$($p.ExitCode) wall=$([math]::Round($sw.Elapsed.TotalSeconds,1)) s bmp=$(Test-Path $bmp)"
Select-String -Path $log -Pattern 'static geometry|STATIC_UPLOAD_HEAP|dump counters|HE_DUMP_GPUTIME|frame dump' |
    ForEach-Object { "  " + $_.Line.Trim() }
$err = @(Select-String -Path $log -Pattern 'D3D12 debug layer|D3D12 ERROR|CORRUPTION|removed' -CaseSensitive:$false)
"  debug-layer/error lines: $($err.Count)"
$err | Select-Object -First 5 | ForEach-Object { "    " + $_.Line.Trim() }
