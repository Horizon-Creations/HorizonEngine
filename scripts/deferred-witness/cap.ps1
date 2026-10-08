param(
    [Parameter(Mandatory)] [string]$Name,
    [Parameter(Mandatory)] [string]$Rhi,
    [hashtable]$Env = @{}
)
# One headless capture from the PRIVATE deploy (C:\hw150\deploy), fresh APPDATA,
# every HE_* variable cleared first so nothing leaks from an earlier run.
$exe = "C:\hw150\deploy\Editor\HorizonEditor.exe"
$log = "C:\hw150\deploy\Editor\HorizonEngine.log"
$out = "C:\hw150\cap"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$app = "$out\appdata_$Name"
if (Test-Path $app) { cmd /c rd /s /q "$app" }
New-Item -ItemType Directory -Force $app | Out-Null
$env:APPDATA = $app
$env:HE_COLLAB_OFFLINE = "1"
$env:HE_DUMP_PATH = "$out\$Name.bmp"
$env:HE_DUMP_QUIT = "1"
$env:HE_DUMP_RHI = $Rhi
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
if (Test-Path $env:HE_DUMP_PATH) { Remove-Item $env:HE_DUMP_PATH }
if (Test-Path $log) { Remove-Item $log -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
$t0 = Get-Date
$dumped = $false
while (((Get-Date) - $t0).TotalSeconds -lt 180) {
    if ($p.HasExited) { break }
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'frame dumped' -Quiet)) { $dumped = $true; break }
    Start-Sleep -Milliseconds 500
}
if (-not $p.HasExited) {
    Start-Sleep -Seconds 6
    if (-not $p.HasExited) {
        $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
        if ($proc -and $proc.Path -eq $exe) { Stop-Process -Id $p.Id -Force }
    }
}
Start-Sleep -Milliseconds 500
if (Test-Path $log) { Copy-Item $log "$out\$Name.log" -Force }
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$ok = Test-Path "$out\$Name.bmp"
"{0,-28} {1,-7} bmp={2} secs={3:N0}" -f $Name, $Rhi, $ok, ((Get-Date) - $t0).TotalSeconds
