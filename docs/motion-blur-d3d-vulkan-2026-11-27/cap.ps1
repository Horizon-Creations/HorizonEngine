param([string]$Rhi, [string]$Name, [string]$Extra = "", [string]$Editor = "C:\hwmbb\deploy\Editor\HorizonEditor.exe")
# One headless capture of the motion-blur witness (SHADOWINSTTEST floor + cubes, pinned sky clock), private config/APPDATA.
$out = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($k in @(Get-ChildItem env: | Where-Object { $_.Name -like 'HE_DUMP_*' } | ForEach-Object Name)) { Remove-Item "env:$k" }
$cfg = Join-Path $out "cfg-$Rhi"; New-Item -ItemType Directory -Force $cfg, "$out\appdata" | Out-Null
$env:APPDATA = "$out\appdata"; $env:HE_CONFIG_DIR = $cfg
$env:HE_HIDDEN_WINDOW = "1"; $env:HE_BACKGROUND_FPS = "0"; $env:HE_COLLAB_OFFLINE = "1"; $env:HE_NET_LOOPBACK_ONLY = "1"
$env:HE_SKY_TIME = "100"
$env:HE_DUMP_PATH = "$out\$Name.bmp"; $env:HE_DUMP_QUIT = "1"; $env:HE_DUMP_RHI = $Rhi; $env:HE_DUMP_FRAMES = "16"
$env:HE_DUMP_SKYTEST = "1"; $env:HE_DUMP_SHADOWINSTTEST = "1"; $env:HE_DUMP_TOD = "0.35"; $env:HE_DUMP_PITCH = "-20"
$env:HE_DUMP_CAMX = "0"; $env:HE_DUMP_CAMY = "4"; $env:HE_DUMP_CAMZ = "8"
$env:HE_DUMP_AA = "0"; $env:HE_DUMP_BLOOM = "0"; $env:HE_DUMP_DOF = "0"; $env:HE_DUMP_MOTIONBLUR = "0"
foreach ($kv in ($Extra -split ' ' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item "env:$k" $v }
Remove-Item "$out\$Name.bmp" -ErrorAction SilentlyContinue
$log = Join-Path (Split-Path $Editor) "HorizonEngine.log"; Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Editor -WorkingDirectory (Split-Path $Editor) -PassThru -WindowStyle Hidden
$t0 = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 120) {
    if ((Test-Path "$out\$Name.bmp") -and ((Get-Date) - $t0).TotalSeconds -gt 3) { Start-Sleep -Milliseconds 300; if (Select-String -Path $log -Pattern 'frame dumped|Dumped' -Quiet) { break } }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 2
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Copy-Item $log "$out\$Name.log" -ErrorAction SilentlyContinue
"$Name : " + $(if (Test-Path "$out\$Name.bmp") { (Get-FileHash "$out\$Name.bmp" -Algorithm MD5).Hash } else { "NO BMP" })
