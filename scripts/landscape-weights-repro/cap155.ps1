param(
    [Parameter(Mandatory)] [string]$Rhi,   # OpenGL | D3D11 | D3D12 | Vulkan
    [Parameter(Mandatory)] [string]$Out,   # .bmp path
    [Parameter(Mandatory)] [string]$Exe,   # a PRIVATE deploy's HorizonEditor.exe, never the user's
    [string]$AppRoot = (Join-Path $env:TEMP "he155")
)
# Thema 155: one HE_DUMP_LANDSCAPELAYERS capture, top-down over the witness
# terrain (100 m at y=300: red field, green disc in the middle, blue disc at
# -34/20). Count with count155.py. D3D12 runs with the debug layer
# (HE_GPU_DEBUG=1); grep "<Out>.log" for "D3D12 debug layer".
#
# Fresh APPDATA per run (a clean exit rewrites config.json), every HE_* cleared
# first (env leaks between runs in one PowerShell call). The log lives NEXT TO
# the exe, so only one capture per deploy at a time. The witness paints in
# OnInit, before the first frame — it does NOT exercise a repaint.
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$app = Join-Path $AppRoot "appdata_$Rhi"
if (Test-Path $app) { Remove-Item -Recurse -Force $app }
New-Item -ItemType Directory -Force $app | Out-Null
$env:APPDATA = $app
$env:HE_COLLAB_OFFLINE = "1"
$env:HE_DUMP_PATH = $Out
$env:HE_DUMP_QUIT = "1"
$env:HE_DUMP_RHI = $Rhi
$env:HE_DUMP_FRAMES = "16"
$env:HE_DUMP_LANDSCAPELAYERS = "1"
$env:HE_DUMP_SKYTEST = "1"
$env:HE_DUMP_CAMX = "0"; $env:HE_DUMP_CAMY = "392"; $env:HE_DUMP_CAMZ = "0"; $env:HE_DUMP_PITCH = "-89"
$env:HE_DUMP_TOD = "0.5"
if ($Rhi -eq "D3D12") { $env:HE_GPU_DEBUG = "1" }
$dir = Split-Path $Exe
$log = Join-Path $dir "HorizonEngine.log"
if (Test-Path $log) { Remove-Item -Force $log }
if (Test-Path $Out) { Remove-Item -Force $Out }
$p = Start-Process -FilePath $Exe -WorkingDirectory $dir -PassThru
$t0 = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 150) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern "frame dumped" -Quiet)) { Start-Sleep -Seconds 8; break }
    Start-Sleep -Seconds 2
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Copy-Item $log ($Out + ".log") -ErrorAction SilentlyContinue
"$Rhi -> $Out exists=$(Test-Path $Out)"
