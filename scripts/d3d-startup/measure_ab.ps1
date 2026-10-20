# Interleaved prefetch A/B for the D3D first start (Thema 200 step 2,
# docs/d3d-erststart-vorkompilierung-2026-10-18.md).
#
# One round = cold with prefetch, cold without (-NoPrefetch), then a warm start on
# each of the two cache dirs the cold runs just filled. The order of pf/nopf flips
# every round (ABBA), so a load change on the device does not land on one side only.
# Before every run the CPU load is sampled (Win32_Processor LoadPercentage and the
# CPU seconds of the busiest foreign process over 2 s), because on NN-WS03 a game
# of the human can triple the sky FXC time.
#
#   .\measure_ab.ps1 -Exe C:\hw200\deploy\Editor\HorizonEditor.exe -OutDir C:\hw200\runs11 -Rhi 2 -Tag bare -Rounds 3
#   .\measure_ab.ps1 ... -Rhi 3 -OutDir C:\hw200\runs12 -Tag proj -Project C:\hw200\proj\HEProject\HEProject.heproj
#
# Reuse an OutDir whose dxcache_mine.txt already exists (see measure.ps1): a fresh
# one records nothing on its first run and -Cold then leaves the driver cache warm.
# Rows are appended to <OutDir>\ab.csv.
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$OutDir,
    [Parameter(Mandatory)] [int]$Rhi,
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Project = "",
    [int]$Rounds = 3,
    [int]$FirstRound = 1
)
$ErrorActionPreference = "Stop"
$measure = Join-Path $PSScriptRoot "measure.ps1"
$csv = Join-Path $OutDir "ab.csv"
if (-not (Test-Path $csv)) {
    "label,rhi,load,fgProc,fgCpuPct,firstFrameMs,slowFrameMs,slowFrameSinceRunMs,skyPhaseMs,compiles,cached,prefetched,uncached,sumCompileMs,unused,cacheFiles,exit" |
        Set-Content -Encoding ascii $csv
}

function Sample-Load {
    $mine = @($PID)
    $a = @{}; Get-Process | ForEach-Object { $a[$_.Id] = $_.CPU }
    $load = (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average
    Start-Sleep -Seconds 2
    $top = Get-Process | Where-Object { $a.ContainsKey($_.Id) -and $_.Id -notin $mine -and $_.CPU } |
        ForEach-Object { [pscustomobject]@{ Name = $_.Name; D = $_.CPU - $a[$_.Id] } } |
        Sort-Object D -Descending | Select-Object -First 1
    # CPU seconds over 2 s -> percent of ONE core.
    [pscustomobject]@{ Load = $load; Proc = $top.Name; Pct = [math]::Round($top.D / 2 * 100) }
}

function Parse-Run([string]$log, [string]$cacheDir) {
    $l = Select-String -Path $log -Pattern '\[startup\]' | ForEach-Object { $_.Line }
    $r = [ordered]@{}
    $ff = $l | Where-Object { $_ -match 'app first frame presented: frame=\d+ ms=([\d.]+)' } | Select-Object -First 1
    $r.firstFrameMs = if ($ff -match 'ms=([\d.]+)') { [math]::Round([double]$Matches[1]) } else { "" }
    # The project opens from frame 1 on; its material compiles land in the slowest later frame.
    $slow = $l | ForEach-Object { if ($_ -match 'slow frame=\d+ busyMs=([\d.]+) \(([\d.]+) since Run\)') {
        [pscustomobject]@{ B = [double]$Matches[1]; S = [double]$Matches[2] } } } | Sort-Object B -Descending | Select-Object -First 1
    $r.slowFrameMs = if ($slow) { [math]::Round($slow.B) } else { "" }
    $r.slowFrameSinceRunMs = if ($slow) { [math]::Round($slow.S) } else { "" }
    $sky = $l | Where-Object { $_ -match 'phase=createSkyPipeline ms=([\d.]+)' } | Select-Object -First 1
    $r.skyPhaseMs = if ($sky -match 'ms=([\d.]+)') { [math]::Round([double]$Matches[1]) } else { "" }
    $c = @($l | Where-Object { $_ -match 'D3DCompile ' })
    $r.compiles = $c.Count
    $r.cached = @($c | Where-Object { $_ -match ' cached' }).Count
    $r.prefetched = @($c | Where-Object { $_ -match ' prefetched' }).Count
    $r.uncached = $r.compiles - $r.cached
    $sum = 0.0; foreach ($x in $c) { if ($x -match ' ms=([\d.]+)') { $sum += [double]$Matches[1] } }
    $r.sumCompileMs = [math]::Round($sum)
    $pf = $l | Where-Object { $_ -match 'shader prefetch: .*unused=(\d+)' } | Select-Object -First 1
    $r.unused = if ($pf -match 'unused=(\d+)') { $Matches[1] } else { "" }
    $r.cacheFiles = if (Test-Path $cacheDir) { (Get-ChildItem $cacheDir -File).Count } else { 0 }
    $r
}

function Run-One([string]$label, [string]$cacheDir, [bool]$cold, [bool]$noPf) {
    $ld = Sample-Load
    $a = @{ Exe = $Exe; OutDir = $OutDir; Rhi = $Rhi; Label = $label; CacheDir = $cacheDir }
    if ($Project) { $a.Project = $Project }
    if ($cold) { $a.Cold = $true }
    if ($noPf) { $a.NoPrefetch = $true }
    $o = & $measure @a
    $exit = if (($o | Select-Object -First 3) -join ' ' -match 'exit=(-?\d+)') { $Matches[1] } else { "?" }
    $o | Where-Object { $_ -match 'LOCKED' } | ForEach-Object { "  $label $_" }
    $r = Parse-Run (Join-Path $OutDir "$label.log") $cacheDir
    $row = @($label, $Rhi, $ld.Load, $ld.Proc, $ld.Pct) + @($r.Values) + @($exit)
    ($row -join ',') | Add-Content -Encoding ascii $csv
    "{0,-26} load={1,3}% {2}={3}% first={4} slow={5}@{6} sky={7} compiles={8} cached={9} pf={10} unused={11} files={12} exit={13}" -f `
        $label, $ld.Load, $ld.Proc, $ld.Pct, $r.firstFrameMs, $r.slowFrameMs, $r.slowFrameSinceRunMs, $r.skyPhaseMs,
        $r.compiles, $r.cached, $r.prefetched, $r.unused, $r.cacheFiles, $exit
}

$base = Split-Path $OutDir
for ($i = $FirstRound; $i -lt $FirstRound + $Rounds; $i++) {
    $order = if ($i % 2) { @($false, $true) } else { @($true, $false) }
    foreach ($phase in @("cold", "warm")) {
        foreach ($noPf in $order) {
            $v = if ($noPf) { "nopf" } else { "pf" }
            $cd = Join-Path $base "c$Rhi`_$Tag`_$v$i"
            Run-One "d3d$($Rhi + 9)_$Tag`_$phase`_$v$i" $cd ($phase -eq "cold") $noPf
        }
    }
}
