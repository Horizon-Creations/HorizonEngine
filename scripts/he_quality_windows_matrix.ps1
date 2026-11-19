# he_quality_windows_matrix.ps1 - the Windows quality witness harness (Thema 324/336, steps 7-10).
#
# Dot-source it, then call Invoke-Dump once per headless editor dump:
#   . scripts\he_quality_windows_matrix.ps1
#   Invoke-Dump -Rhi D3D12 -Tag demo -Env @{ HE_DUMP_SKYTEST="1"; HE_DUMP_TERRAINTICK="1"; HE_DUMP_TOD="0.5";
#       HE_DUMP_CAMX="-10"; HE_DUMP_CAMY="335"; HE_DUMP_CAMZ="75"; HE_DUMP_PITCH="-22";
#       HE_DUMP_QUALITYPRESET="High" } -GpuFrames 150
# It runs the editor hidden with HE_DUMP_AUTOLAND (the 128 m landscape witness), writes <Tag>.bmp and
# <Tag>.log to $script:Out / $script:Logs and returns the whole-frame GPU time (median/mean/p90),
# the warning/error counts and the wall time. Set $env:HE_EDITOR_EXE / HE_WITNESS_DIR to move it.
# HE_WITNESS_APPDATA = the private APPDATA of the runs (default <witness dir>ppdata), so nobody's preferences leak in.
# The first dump of a session is a warm-up (terrain streams in): throw it away.
$script:Editor = if ($env:HE_EDITOR_EXE) { $env:HE_EDITOR_EXE } else { "C:\hq\deploy\Editor\HorizonEditor.exe" }
$script:Out    = if ($env:HE_WITNESS_DIR) { $env:HE_WITNESS_DIR } else { "C:\hq\witness" }
$script:Logs   = Join-Path $script:Out "logs"
New-Item -ItemType Directory -Force $script:Out, $script:Logs | Out-Null

function Invoke-Dump {
    param(
        [Parameter(Mandatory)] [string] $Rhi,
        [Parameter(Mandatory)] [string] $Tag,
        [hashtable] $Env = @{},
        [int] $GpuFrames = 150,
        [string] $Scene = "1"
    )
    $saved = @{}
    $all = @{
        APPDATA = if ($env:HE_WITNESS_APPDATA) { $env:HE_WITNESS_APPDATA } else { Join-Path $script:Out "appdata" }
        HE_HIDDEN_WINDOW = "1"
        HE_DUMP_RHI = $Rhi
        HE_DUMP_AUTOLAND = $Scene
        HE_SKY_TIME = "12"
        HE_DUMP_PATH = "$($script:Out)\$Tag.bmp"
        HE_DUMP_QUIT = "1"
        HE_DUMP_GPUTIME = "$GpuFrames"
    }
    foreach ($k in $Env.Keys) { $all[$k] = [string]$Env[$k] }
    foreach ($k in $all.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $all[$k]) }
    $log = "$($script:Logs)\$Tag.log"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $script:Editor -Wait -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $code = $proc.ExitCode
    $sw.Stop()
    foreach ($k in $all.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    $text = (Get-Content $log -Raw) + "`n" + (Get-Content "$log.err" -Raw)
    if ($null -eq $text) { $text = '' }
    $gpu = if ($text -match 'gpu ms median=([\d\.]+) mean=([\d\.]+) min=([\d\.]+) p90=([\d\.]+) max=([\d\.]+)') { [pscustomobject]@{ median=[double]$Matches[1]; mean=[double]$Matches[2]; p90=[double]$Matches[4] } } else { $null }
    $warns = ([regex]::Matches($text, '\[ *WARN')).Count
    $errs  = ([regex]::Matches($text, '\[ *(ERROR|FATAL)')).Count
    [pscustomobject]@{ Tag=$Tag; Rhi=$Rhi; Exit=$code; GpuMedian=$gpu.median; GpuMean=$gpu.mean; GpuP90=$gpu.p90; Warn=$warns; Err=$errs; Secs=[math]::Round($sw.Elapsed.TotalSeconds,1); Bmp="$($script:Out)\$Tag.bmp" }
}
