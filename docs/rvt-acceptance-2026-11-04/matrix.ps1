param([int]$Reps = 3, [int]$W = 1920, [int]$H = 1080, [string]$Prefix = "m")
$s=$PSScriptRoot   # scratch files (logs, dumps, mcp_drv.py) live beside the scripts
$out = "$s\matrix_$Prefix.txt"
"" | Set-Content $out
foreach ($rep in 1..$Reps) {
  foreach ($be in 'D3D11','D3D12','Vulkan') {
    foreach ($rvt in '0','1') {
      $tag = "${Prefix}_${be}_rvt${rvt}_r$rep"
      $r = & "$s\run_game.ps1" -Backend $be -Rvt $rvt -Tag $tag -Warmup 900 -Capture 600 -W $W -H $H 2>&1 | Select-Object -Last 1
      Add-Content $out $r
    }
  }
}
"done" | Add-Content $out
