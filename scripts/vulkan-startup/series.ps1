# The interleaved Vulkan load-time series behind docs/audit-vulkan-backend-2026-10-18.md
# (Thema 215): pipeline cache off/empty/loaded against a cold/warm NVIDIA driver cache,
# then the upload path with and without HE_VK_UPLOAD_SYNC. Calls measure.ps1 per run,
# writes <OutDir>\series.csv and prints the table.
#
#   .\series.ps1 -Exe F:\hw215\deploy\Editor\HorizonEditor.exe -OutDir F:\hw215\runs_proj `
#                -Project F:\hw215\proj\HEProject\HEProject.heproj -NoValidation
#
# "Driver cold" = a copy of the exe under a new name: NVIDIA keys its Vulkan cache
# (%LOCALAPPDATA%\NVIDIA\DXCache\*.nvph) by the exe NAME. The copies are deleted at the
# end, and so is every DXCache file that appeared while the series ran. On a shared
# machine that can include another process's new files from the same minutes (a D3D
# run, a game), so say when you ran it.
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$OutDir,
    [string]$Project = "",
    [int]$N = 3,
    [switch]$NoValidation
)
$ErrorActionPreference = "Stop"
$m   = Join-Path $PSScriptRoot "measure.ps1"
$dir = Split-Path $Exe
$dx  = "$env:LOCALAPPDATA\NVIDIA\DXCache"
$before = @{}
if (Test-Path $dx) { Get-ChildItem $dx -File | ForEach-Object { $before[$_.Name] = 1 } }

function Run($exe, $label, $extra) {
    $pa = @{ Exe = $exe; OutDir = $OutDir; Label = $label; Project = $Project; NoValidation = [bool]$NoValidation } + $extra
    $t = (& $m @pa) -join "`n"
    # The log lines carry an em dash; match it with '.' so the file stays ASCII
    # (Windows PowerShell 5.1 reads a BOM-less script as ANSI).
    $r = [ordered]@{ label = $label }
    if ($t -match 'Vulkan Initialize: ms=([\d.]+) pipelines=(\d+) pipelineMs=([\d.]+)') { $r.initMs = $Matches[1]; $r.initPipeMs = $Matches[3] }
    if ($t -match 'first frame presented: frame=\d+ ms=([\d.]+)') { $r.firstFrameMs = $Matches[1] }
    if ($t -match 'pipelines . (\d+) created, ([\d.]+) ms') { $r.pipes = $Matches[1]; $r.pipeMs = $Matches[2] }
    if ($t -match 'cache (.+?) \(in (\d+) KB, out (\d+) KB, ([^)]+)\)') { $r.cache = $Matches[1]; $r.inKB = $Matches[2]; $r.outKB = $Matches[3]; $r.save = $Matches[4] }
    if ($t -match 'uploads . (\d+) meshes, (\d+) textures, ([\d.]+) MB in (\d+) batches \((\d+) oversize.*?; ([\d.]+) ms CPU in upload calls, (\d+) waits / ([\d.]+) ms') {
        $r.meshes = $Matches[1]; $r.textures = $Matches[2]; $r.uploadMB = $Matches[3]; $r.batches = $Matches[4]
        $r.oversize = $Matches[5]; $r.uploadCpuMs = $Matches[6]; $r.waits = $Matches[7]; $r.waitMs = $Matches[8]
    }
    if ($t -match 'exit=(-?\d+) wallTotal=(\d+)') { $r.exit = $Matches[1]; $r.wallMs = $Matches[2] }
    [pscustomobject]$r
}

$rows = @()
for ($i = 1; $i -le $N; $i++) {
    $off = Join-Path $dir "HEvkOff$i.exe"; Copy-Item $Exe $off -Force
    $rows += Run $off "off_drvcold_$i" @{ NoCache = $true }
    $rows += Run $off "off_warm_$i"    @{ NoCache = $true }
    $on = Join-Path $dir "HEvkOn$i.exe"; Copy-Item $Exe $on -Force
    $rows += Run $on "on_bothcold_$i"  @{ CacheDir = "$OutDir\vkpc_$i"; Cold = $true }
    $rows += Run $on "on_warm_$i"      @{ CacheDir = "$OutDir\vkpc_$i" }
    $ond = Join-Path $dir "HEvkOnD$i.exe"; Copy-Item $Exe $ond -Force
    $rows += Run $ond "on_appwarm_drvcold_$i" @{ CacheDir = "$OutDir\vkpc_$i" }
}
for ($i = 1; $i -le $N; $i++) {
    $rows += Run $Exe "upload_sync_$i"  @{ CacheDir = "$OutDir\vkpc_1"; UploadSync = $true }
    $rows += Run $Exe "upload_batch_$i" @{ CacheDir = "$OutDir\vkpc_1" }
}
$rows | Export-Csv -NoTypeInformation (Join-Path $OutDir "series.csv")
$rows | Format-Table -AutoSize | Out-String -Width 400

Get-ChildItem $dir -Filter "HEvk*.exe" | Remove-Item -Force
if (Test-Path $dx) {
    $new = @(Get-ChildItem $dx -File | Where-Object { -not $before.ContainsKey($_.Name) })
    "new DXCache files: $($new.Count), $([math]::Round(($new | Measure-Object Length -Sum).Sum / 1MB, 1)) MB - removed"
    $new | Remove-Item -Force
}
