# Windows port of scripts/perf/frame_pixel_ab.sh: one HE_DUMP frame per witness, md5 of the BMP, per build and backend.
#   frame_pixel_ab_win.ps1 -Build Lev_old -Backends D3D11,D3D12,Vulkan -Tag run1
param([Parameter(Mandatory=$true)][string]$Build, [string]$Work = "C:\hw237_perf", [string[]]$Backends = @("D3D11","D3D12","Vulkan"), [string]$Tag = "run1", [string]$Set = "all")
$ErrorActionPreference = "Stop"
$W = $Work
$deploy = "$W\$Build"; $exe = "$deploy\HorizonEditor.exe"; $log = "$deploy\HorizonEngine.log"
$out = "$W\pixelab\$Build-$Tag"; New-Item -ItemType Directory -Force $out | Out-Null
$LS = "TOD=0 COVERAGE=0 CAMY=206 CAMZ=2 PITCH=-40"
$DC = "TOD=0.5 COVERAGE=0.3 CAMY=5 CAMZ=2 PITCH=-30"
$FO = "TOD=0.5 COVERAGE=0.3 CAMX=0 CAMY=40 CAMZ=-120 PITCH=-20"
$SI = "TOD=0.35 COVERAGE=0.3 CAMY=5 PITCH=-18"
$SB = "COVERAGE=0.3 CAMY=5 PITCH=-18"
$SC = "SHADOWINSTTEST=1 RENDERPATH=0 TOD=0.35 $SB"
$shots = [ordered]@{}
if ($Set -in "frame","all") {
  $shots["ls_point_fwd"]="LOCALSHADOW=point RENDERPATH=0 $LS"; $shots["ls_point_def"]="LOCALSHADOW=point RENDERPATH=1 $LS"
  $shots["ls_spot_fwd"]="LOCALSHADOW=spot RENDERPATH=0 $LS"; $shots["ls_spot_def"]="LOCALSHADOW=spot RENDERPATH=1 $LS"
  $shots["decal_def"]="DECALTEST=1 RENDERPATH=1 $DC"; $shots["decal_fwd"]="DECALTEST=1 RENDERPATH=0 $DC"
  $shots["foliage_fwd"]="FOLIAGETEST=2000 RENDERPATH=0 $FO"; $shots["foliage_def"]="FOLIAGETEST=2000 RENDERPATH=1 $FO"
  $shots["shadinst_fwd"]="SHADOWINSTTEST=1 RENDERPATH=0 $SI"; $shots["shadinst_def"]="SHADOWINSTTEST=1 RENDERPATH=1 $SI"
  $shots["odd_shadinst_fwd"]="SHADOWINSTTEST=1 RENDERSCALE=0.51 RENDERPATH=0 SSAO=1 $SI"
  $shots["odd_shadinst_def"]="SHADOWINSTTEST=1 RENDERSCALE=0.51 RENDERPATH=1 SSAO=1 $SI"
  $shots["odd_ls_point_fwd"]="LOCALSHADOW=point RENDERSCALE=0.51 RENDERPATH=0 SSAO=1 $LS"
  $shots["odd_ls_spot_fwd"]="LOCALSHADOW=spot RENDERSCALE=0.51 RENDERPATH=0 SSAO=1 $LS"
  $shots["odd_shadinst_nossao"]="SHADOWINSTTEST=1 RENDERSCALE=0.51 RENDERPATH=0 SSAO=0 $SI"
}
if ($Set -in "shadow","all") {
  foreach ($t in 0,25,50,75) { $shots["dn_${t}_fwd"]="SHADOWINSTTEST=1 RENDERPATH=0 TOD=0.$('{0:D2}' -f $t) $SB" }
  $shots["dn_30_def"]="SHADOWINSTTEST=1 RENDERPATH=1 TOD=0.30 $SB"
  $shots["casc1_fwd"]="$SC SHADOW=60,1,2048,0.5"; $shots["casc3_fwd"]="$SC SHADOW=60,3,2048,0.5"
  $shots["casc3_def"]="SHADOWINSTTEST=1 RENDERPATH=1 TOD=0.35 $SB SHADOW=60,3,2048,0.5"
  $shots["casc3_near_fwd"]="$SC SHADOW=60,3,2048,0.5 CAMZ=-5"; $shots["casc3_far_fwd"]="$SC SHADOW=60,3,2048,0.5 CAMZ=15"
  $shots["casc3_split_fwd"]="$SC SHADOW=60,3,2048,0.5 YAW=40"
  $shots["ls_point_day_fwd"]="LOCALSHADOW=point RENDERPATH=0 TOD=0.35 COVERAGE=0 CAMY=206 CAMZ=2 PITCH=-40"
  $shots["odd_casc2_fwd"]="$SC SHADOW=60,2,2048,0.5 RENDERSCALE=0.51 SSAO=1"
}
$res = "$out\md5.txt"; if (Test-Path $res) { Remove-Item $res }
$realAppData = $env:APPDATA
foreach ($rhi in $Backends) {
  $app = "$W\appdata-pixelab-$Build-$rhi"; New-Item -ItemType Directory -Force $app | Out-Null
  foreach ($name in $shots.Keys) {
    Get-ChildItem env: | Where-Object { $_.Name -like "HE_DUMP_*" } | ForEach-Object { Remove-Item "env:$($_.Name)" }
    $bmp = "$out\$rhi-$name.bmp"; if (Test-Path $bmp) { Remove-Item $bmp }
    $env:APPDATA = $app; $env:HE_COLLAB_OFFLINE = "1"; $env:HE_NET_LOOPBACK_ONLY = "1"; $env:HE_SKY_TIME = "30"
    $env:HE_DUMP_PATH = $bmp; $env:HE_DUMP_QUIT = "1"; $env:HE_DUMP_RHI = $rhi; $env:HE_DUMP_AA = "0"; $env:HE_DUMP_SKYTEST = "1"
    foreach ($kv in (($shots[$name] + " COVERAGE=0 CLOUDMODE=0 CLOUDSHADOWS=0") -split " ")) { if ($kv) { $k,$v = $kv -split "=",2; Set-Item "env:HE_DUMP_$k" $v } }
    if (Test-Path $log) { Remove-Item $log -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath $exe -WorkingDirectory $deploy -PassThru
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
      Start-Sleep -Milliseconds 400
      if ((Test-Path $log) -and (Select-String -Path $log -Pattern "frame dumped" -Quiet)) { Start-Sleep 1; break }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
    $env:APPDATA = $realAppData
    $h = if (Test-Path $bmp) { (Get-FileHash $bmp -Algorithm MD5).Hash } else { "MISSING" }
    "$rhi $name $h" | Add-Content $res
  }
}
"done $Build $Tag"


