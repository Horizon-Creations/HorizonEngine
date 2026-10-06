param(
    [Parameter(Mandatory)][string]$Root,      # private build tree with deploy\Editor (see cap.ps1)
    [string[]]$Rhis    = @('D3D11', 'OpenGL'),
    [string[]]$Casters = @('0', '64', '256', '1024', '4096', '256f', '1024f'),
    [string[]]$Rays    = @('1', '2', '4'),
    [hashtable]$Extra  = @{},
    [string]$Tag       = 'cost'
)
# Thema 142 step 3: GPU cost of the GI sun rays on the SOFTWARE path against the
# number of shadow casters. One 240-frame dump per point with HE_GI_SHADOW_BENCH=1
# (exclusive timer around the shadow-ray dispatch, p10/p50/p90 over 160 frames,
# GIShadowBench.h) and HE_DUMP_GICASTERS (N extra cubes: "N" below the floor, "Nf"
# as a field on it, EditorApplication.cpp). D3D11 and GL always trace in software;
# their kernels are the HLSL/GLSL ones D3D12/Vulkan fall back to without RT cores.
# Output: one line per point, and the raw logs in $Root\cap\<Tag>_<rhi>_c<N>_r<rays>.log.
#   cost142.ps1 -Root C:\hw142
# The GPU clock is sampled alongside (nvidia-smi, median over samples with >5 %
# load): a light D3D11 dump does NOT lift the RTX 4070 out of its 210 MHz idle
# clock, GL does — only points at boost clock compare (§8.4).
# See docs/gi-shadow-restflackern-1spp-2026-10-03.md §8.4.
$here = $PSScriptRoot
foreach ($rhi in $Rhis) { foreach ($c in $Casters) { foreach ($r in $Rays) {
    $e = @{ HE_GI_SHADOW_BENCH = '1'; HE_DUMP_GISHADOWRAYS = $r }
    if ($c -ne '0') { $e.HE_DUMP_GICASTERS = $c }
    foreach ($k in $Extra.Keys) { $e[$k] = $Extra[$k] }
    $n = "${Tag}_${rhi}_c${c}_r$r"
    $job = Start-Job { while ($true) { & nvidia-smi --query-gpu=clocks.gr,utilization.gpu --format=csv,noheader,nounits; Start-Sleep -Milliseconds 150 } }
    & "$here\cap.ps1" -Root $Root -Name $n -Rhi $rhi -Frames 240 -Config "$here\config_r05.json" -Extra $e | Out-Null
    Stop-Job $job; $smi = Receive-Job $job; Remove-Job $job
    $busy = @($smi | ForEach-Object { $f = $_ -split ','; if ($f.Count -eq 2 -and [int]$f[1].Trim() -gt 5) { [int]$f[0].Trim() } } | Sort-Object)
    $clk = if ($busy.Count) { $busy[[int][math]::Floor($busy.Count / 2)] } else { 0 }
    $l = Select-String -Path "$Root\cap\$n.log" -Pattern 'GI shadow bench' | Select-Object -First 1
    "$n " + $(if ($l) { $l.Line -replace '.*dispatch GPU ms ', '' } else { 'NO BENCH LINE' }) + " clockMHz $clk"
} } }
