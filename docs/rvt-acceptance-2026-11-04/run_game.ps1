param([string]$Backend = "D3D11", [string]$Rvt = "", [string]$Tag = "g", [int]$Warmup = 900, [int]$Capture = 600, [int]$W = 1920, [int]$H = 1080,
       [int]$Detailed = 1, [int]$ExitFrames = 0, [string]$Settings = "")
$s=$PSScriptRoot   # scratch files (logs, dumps, mcp_drv.py) live beside the scripts
$dir = if ($env:HW_DIR) { $env:HW_DIR } else { 'C:\hw333\export' }   # the exported game to run
Get-Process Rvt -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
$cfgPath = "$dir\data\config.json"
if (-not (Test-Path "$cfgPath.orig")) { Copy-Item $cfgPath "$cfgPath.orig" }
$j = Get-Content "$cfgPath.orig" -Raw | ConvertFrom-Json
function SetKey($name, $val) {
    $e = $j.CustomConfig | Where-Object { $_.Key -eq $name }
    if ($e) { $e.Value = $val } else { $j.CustomConfig += [pscustomobject]@{ Key = $name; Value = $val } }
}
SetKey 'GameBackend' $Backend
SetKey 'GameWindowMode' 'Windowed'
SetKey 'GameWindowWidth' $W
SetKey 'GameWindowHeight' $H
SetKey 'GameVSync' $false
SetKey 'PauseOnFocusLoss' $false
($j | ConvertTo-Json -Depth 20) | Set-Content $cfgPath -Encoding utf8
if ($Settings) { Copy-Item $Settings "$dir\data\Config\ProjectSettings.json" -Force }
$env:HE_HIDDEN_WINDOW="1"; $env:HE_BACKGROUND_FPS="0"
foreach ($v in "HE_RVT","HE_PROFILE_CAPTURE","HE_PROFILE_WARMUP","HE_PROFILE_QUIT","HE_PROFILE_DETAILED","HE_EXIT_AFTER_FRAMES","HE_PROFILE_NOTE") { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
if ($Rvt -ne "") { $env:HE_RVT = $Rvt }
if ($Capture -gt 0) { $env:HE_PROFILE_CAPTURE="$Capture"; $env:HE_PROFILE_WARMUP="$Warmup"; $env:HE_PROFILE_QUIT="1"; $env:HE_PROFILE_DETAILED="$Detailed"; $env:HE_PROFILE_NOTE=$Tag }
if ($ExitFrames -gt 0) { $env:HE_EXIT_AFTER_FRAMES="$ExitFrames" }
$logFile = "$dir\HorizonEngine.log"
if (Test-Path $logFile) { Remove-Item $logFile -Force }
$memBefore = (nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits).Trim()
$p = Start-Process -FilePath "$dir\Rvt.exe" -WorkingDirectory $dir -PassThru -RedirectStandardOutput "$s\game_${Tag}_out.txt" -RedirectStandardError "$s\game_${Tag}_err.txt"
$mem = @()
$deadline = (Get-Date).AddSeconds(420)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2; $mem += [int](nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits).Trim() }
if (-not $p.HasExited) { "TIMEOUT"; $p.Kill() }
$memPeak = if ($mem.Count) { ($mem | Measure-Object -Maximum).Maximum } else { 0 }
$dumps = "$dir\dumps"
$new = if (Test-Path $dumps) { Get-ChildItem $dumps -Filter 'profile_*.json' | Sort-Object LastWriteTime | Select-Object -Last 1 } else { $null }
if ($new -and $Capture -gt 0) { Copy-Item $new.FullName "$s\game_${Tag}.json" -Force }
if (Test-Path $logFile) { Copy-Item $logFile "$s\game_${Tag}.log" -Force }
"tag=$Tag backend=$Backend rvt='$Rvt' exit=$($p.ExitCode) gpuMemBefore=$memBefore MiB peakDuringRun=$memPeak MiB dump=$($new.Name)"
