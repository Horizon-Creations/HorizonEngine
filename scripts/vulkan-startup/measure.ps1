# Vulkan startup / load timing run (Thema 215, docs/audit-vulkan-backend-2026-10-18.md).
#
# Sibling of scripts/d3d-startup/measure.ps1. Starts the editor hidden on Vulkan,
# waits for HE_EXIT_AFTER_FRAMES, copies HorizonEngine.log to <OutDir>\<Label>.log
# and prints the [startup] timeline, the pipeline line (REN-808) and the upload
# line (REN-802/803). Run it against a PRIVATE deploy (-DDEPLOY_DIR=...), never
# the user's editor.
#
#   .\measure.ps1 -Exe F:\hw215\deploy\Editor\HorizonEditor.exe -OutDir F:\hw215\runs -Label warm -CacheDir F:\hw215\vkpc
#   .\measure.ps1 ... -Label off -NoCache -Project F:\hw215\proj\HEProject\HEProject.heproj
#
# -CacheDir sets HE_VK_PIPELINE_CACHE_DIR (SDL_GetPrefPath ignores %APPDATA%, so
# without it the run reads and writes the real roaming cache); -Cold empties that
# dir first. -NoCache sets HE_VK_PIPELINE_CACHE=0: no VkPipelineCache, the pre-
# REN-808 path, same binary. -UploadSync sets HE_VK_UPLOAD_SYNC=1: a wait after
# every upload (the waits of the pre-REN-802 path; the rest of the step-1 code stays).
# -NoValidation sets VK_LOADER_LAYERS_DISABLE=*validation*: until REN-804 (Thema 212)
# is merged, createInstance forces the validation layer on, and timings include it.
# NVIDIA keeps its own Vulkan cache in %LOCALAPPDATA%\NVIDIA\DXCache, keyed by the
# exe NAME; __GL_SHADER_DISK_CACHE=0 does NOT switch it off on Windows (checked
# 2026-10-18). For a driver-cold run start a copy of the exe under a new name, as
# series.ps1 does.
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$OutDir,
    [Parameter(Mandatory)] [string]$Label,
    [string]$Project = "",     # .heproj to open, or empty for the bare editor
    [int]$Frames = 300,
    [string]$CacheDir = "",
    [switch]$Cold,
    [switch]$NoCache,
    [switch]$UploadSync,
    [switch]$NoValidation
)
$ErrorActionPreference = "Stop"
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' -or $_.Name -like 'VK_LOADER_*' } |
    ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, $null) }
New-Item -ItemType Directory -Force $OutDir | Out-Null

if ($CacheDir) {
    if ($Cold -and (Test-Path $CacheDir)) { Remove-Item $CacheDir -Recurse -Force; "cold: removed $CacheDir" }
    New-Item -ItemType Directory -Force $CacheDir | Out-Null
    $env:HE_VK_PIPELINE_CACHE_DIR = $CacheDir
}

$appdata = Join-Path $OutDir "appdata\vk"
New-Item -ItemType Directory -Force "$appdata\HorizonEngine" | Out-Null
$cfg = @{ RHI = 1 }
if ($Project) { $cfg.LastProjectPath = $Project.Replace('\', '/') }
$cfg | ConvertTo-Json | Set-Content -Encoding ascii "$appdata\HorizonEngine\config.json"

# Delete the old log first: it is only truncated once the new process gets to it.
$log = Join-Path (Split-Path $Exe) "HorizonEngine.log"
if (Test-Path $log) { Remove-Item $log -Force }

$env:APPDATA = $appdata
$env:HE_HIDDEN_WINDOW = "1"; $env:HE_BACKGROUND_FPS = "0"
$env:HE_EXIT_AFTER_FRAMES = "$Frames"; $env:HE_COLLAB_OFFLINE = "1"
if ($NoCache) { $env:HE_VK_PIPELINE_CACHE = "0" }
if ($UploadSync) { $env:HE_VK_UPLOAD_SYNC = "1" }
if ($NoValidation) { $env:VK_LOADER_LAYERS_DISABLE = "*validation*" }

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -PassThru
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt 300) { Start-Sleep -Milliseconds 100 }
if (-not $p.HasExited) { "TIMEOUT - killing"; $p.Kill() }
$total = $sw.Elapsed.TotalMilliseconds

$out = Join-Path $OutDir "$Label.log"
Copy-Item $log $out -Force
"exit=$($p.ExitCode) wallTotal=$([math]::Round($total)) ms"
Select-String -Path $out -Pattern '\[startup\]|VulkanRenderer: pipelines|VulkanRenderer: uploads' |
    ForEach-Object { $_.Line -replace '^.*?(\[startup\]|VulkanRenderer:)', '$1' }
if ($CacheDir) { Get-ChildItem $CacheDir -File | ForEach-Object { "cache file: $($_.Name) $([math]::Round($_.Length / 1024)) KB" } }
