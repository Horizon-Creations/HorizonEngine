param(
    [string]$Name,
    [string]$Nfif = "",          # "" = no override (fix state / pre-fix build as built)
    [string]$Vsync = "1",
    [string]$Scribble = "",
    [int]$Frames = 3000,
    [int]$TimeoutSec = 240,
    [string]$Exe = "C:\hw97\deploy\Editor\HorizonEditor.exe"
)
# Thema 97 Schritt 5: one capture run of the instrumented editor on D3D12.
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:APPDATA = "C:\hw97\appdata"
$cfg = "C:\hw97\appdata\HorizonEngine\config.json"
$j = Get-Content $cfg -Raw | ConvertFrom-Json
$j.RHI = 3
$j.LastProjectPath = "C:/hw97/proj/Testie/Testie.heproj"
$j | ConvertTo-Json -Depth 20 | Out-File -Encoding utf8 $cfg
$capDir = "C:\hw97\shots\s5_$Name"
if (Test-Path $capDir) { Remove-Item -Recurse -Force $capDir }
New-Item -ItemType Directory -Force $capDir | Out-Null
$env:HE_T97_SWEEP = "1"
$env:HE_T97_CAPDIR = $capDir
$env:HE_T97_VSYNC = $Vsync
if ($Nfif) { $env:HE_T97_NFIF = $Nfif }
if ($Scribble) { $env:HE_T97_SCRIBBLE = $Scribble }
$exeDir = Split-Path $Exe
$log = Join-Path $exeDir "HorizonEngine.log"
if (Test-Path $log) { Remove-Item -Force $log }
$p = Start-Process -FilePath $Exe -WorkingDirectory $exeDir -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
$done = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 1000
    if (Test-Path $log) {
        $last = Select-String -Path $log -Pattern "T97CAP compared=(\d+)" | Select-Object -Last 1
        if ($last -and [int]$last.Matches[0].Groups[1].Value -ge $Frames) { $done = $true; break }
    }
}
Start-Sleep -Milliseconds 500
if (-not $p.HasExited) {
    $pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($pp -and $pp.Path -eq $Exe) { Stop-Process -Id $p.Id -Force }
}
Start-Sleep -Milliseconds 800
Copy-Item $log "$capDir\HorizonEngine.log"
"run=$Name nfif=$Nfif vsync=$Vsync scribble=$Scribble reached=$done secs=$([int]$sw.Elapsed.TotalSeconds) exited=$($p.HasExited)"
Select-String -Path "$capDir\HorizonEngine.log" -Pattern "T97 ImGui|T97CAP init|Initialized ImGui|RHI|D3D12 debug layer" | Select-Object -First 6 | ForEach-Object { $_.Line }
Select-String -Path "$capDir\HorizonEngine.log" -Pattern "T97 nfif" | Select-Object -Last 1 | ForEach-Object { $_.Line }
Select-String -Path "$capDir\HorizonEngine.log" -Pattern "T97CAP compared" | Select-Object -Last 1 | ForEach-Object { $_.Line }
$mm = Select-String -Path "$capDir\HorizonEngine.log" -Pattern "T97CAP (MISMATCH|SCRIBBLE-MISSED)"
"mismatchLines=$($mm.Count)"
$mm | Select-Object -First 8 | ForEach-Object { $_.Line }
