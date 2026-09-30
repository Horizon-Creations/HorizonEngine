param(
    [string]$Name,
    [string]$Nfif = "",          # "" = no override (fix state)
    [string]$Vsync = "1",
    [string]$Scribble = "",
    [int]$Secs = 60,             # length of the SendInput sweep
    [double]$MinIdle = 0,        # refuse to move the cursor if the human touched anything more recently
    [switch]$Dry,                # start the editor, compute targets, move nothing
    [switch]$Manual,             # no SendInput: a human sweeps with the real mouse, then closes the editor
    [string]$Exe = "C:\hw97\deploy\Editor\HorizonEditor.exe",
    [string]$Mouse = "$env:TEMP\t97mouse\t97mouse.exe"
)
# Thema 97 Schritt 8: like d3d12-imgui-flicker-bildvergleich.ps1, but the stimulus is the REAL OS
# cursor (SendInput -> WM_MOUSEMOVE -> SDL3 -> ImGui), not HE_T97_SWEEP. HE_T97_SWEEP stays unset,
# otherwise the synthetic AddMousePosEvent would override the real mouse every frame.
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:APPDATA = "C:\hw97\appdata"
$cfg = "C:\hw97\appdata\HorizonEngine\config.json"
$j = Get-Content $cfg -Raw | ConvertFrom-Json
$j.RHI = 3
$j.LastProjectPath = "C:/hw97/proj/Testie/Testie.heproj"
$j | ConvertTo-Json -Depth 20 | Out-File -Encoding utf8 $cfg
$capDir = "C:\hw97\shots\s8_$Name"
if (Test-Path $capDir) { Remove-Item -Recurse -Force $capDir }
New-Item -ItemType Directory -Force $capDir | Out-Null
$env:HE_T97_CAPDIR = $capDir
$env:HE_T97_MOUSELOG = "1"
$env:HE_T97_VSYNC = $Vsync
$env:HE_COLLAB_OFFLINE = "1"
if ($Nfif) { $env:HE_T97_NFIF = $Nfif }
if ($Scribble) { $env:HE_T97_SCRIBBLE = $Scribble }
$exeDir = Split-Path $Exe
$log = Join-Path $exeDir "HorizonEngine.log"
if (Test-Path $log) { Remove-Item -Force $log }
$p = Start-Process -FilePath $Exe -WorkingDirectory $exeDir -PassThru
if ($Manual) {
    # A human moves the mouse and closes the editor when done; the frame diff counts meanwhile.
    "Editor laeuft (D3D12, Vsync=$Vsync). Maus 1-2 min schnell ueber Toolbar, Quick Settings, Outliner, Content Browser fahren, dann Editor schliessen."
    $p.WaitForExit(15 * 60 * 1000) | Out-Null
    $mout = @("manual"); $mexit = 0
} else {
    $margs = @("$($p.Id)", $log, "$Secs", "--minidle=$MinIdle")
    if ($Dry) { $margs += "--dry" }
    $mout = & $Mouse @margs
    $mexit = $LASTEXITCODE
}
Start-Sleep -Milliseconds 1500   # let the readback ring drain the last frames
if (-not $p.HasExited) {
    $pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($pp -and $pp.Path -eq $Exe) { Stop-Process -Id $p.Id -Force }
}
Start-Sleep -Milliseconds 800
Copy-Item $log "$capDir\HorizonEngine.log"
$mout | Out-File -Encoding utf8 "$capDir\t97mouse.txt"
"run=$Name nfif=$Nfif vsync=$Vsync scribble=$Scribble mouseExit=$mexit"
$mout
$L = "$capDir\HorizonEngine.log"
Select-String -Path $L -Pattern "T97 ImGui|T97CAP init" | Select-Object -First 3 | ForEach-Object { $_.Line }
Select-String -Path $L -Pattern "T97MOUSE" | Select-Object -Last 3 | ForEach-Object { $_.Line }
Select-String -Path $L -Pattern "T97 nfif" | Select-Object -Last 1 | ForEach-Object { $_.Line }
Select-String -Path $L -Pattern "T97CAP compared" | Select-Object -Last 1 | ForEach-Object { $_.Line }
$mm = Select-String -Path $L -Pattern "T97CAP (MISMATCH|SCRIBBLE-MISSED)"
"mismatchLines=$($mm.Count)"
$mm | Select-Object -First 8 | ForEach-Object { $_.Line }
