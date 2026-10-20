# Thema 187, Schritt 6: one editor start for the Windows acceptance of the
# EngineContent manifest fix (docs/engine-content-manifest-windows-lock-2026-10-13.md,
# "Windows-Abnahme (Schritt 6)"). Starts a hidden editor against a scratch APPDATA,
# waits until both startup refreshes (or the failure) and the project preflight are in
# HorizonEngine.log, closes it with WM_CLOSE and copies the log to <Root>\evidence\<Name>.log.
#
# Setup once: a deploy built with -DDEPLOY_DIR=<Root>/deploy and an EngineContent
# account compiled in, a project copy, and <Root>\appdata\HorizonEngine\config.json with
#   { "LastProjectPath": "C:/<Root>/proj/X.heproj", "RHI": 3 }   (forward slashes)
#
#   -Name           evidence file stem
#   -KillOnPartial  TerminateProcess the instant .manifest.json.tmp.partial exists in the
#                   cache (a kill mid-download; taskkill.exe is too slow for a 16 KB file)
#   -CloseWait      seconds to keep running after the refreshes settled
param(
    [Parameter(Mandatory)] [string]$Name,
    [string]$Root = "C:\hw187",
    [switch]$KillOnPartial,
    [int]$CloseWait = 2,
    [int]$Timeout = 60
)
$ErrorActionPreference = "Stop"
$deploy = Join-Path $Root "deploy\Editor"
$ev     = Join-Path $Root "evidence"
$cache  = Join-Path $Root "appdata\HorizonEngine\EngineContentCache"
$log    = Join-Path $deploy "HorizonEngine.log"
New-Item -ItemType Directory -Force $ev | Out-Null

function Read-Log {
    if (-not (Test-Path $log)) { return @() }
    # FileShare ReadWrite|Delete: never hold anything the editor might rename
    $fs = [IO.File]::Open($log, 'Open', 'Read', [IO.FileShare]'ReadWrite, Delete')
    try { $t = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    return $t -split "`r?`n"
}

$before = if (Test-Path $cache) { Get-ChildItem $cache -Force | Select-Object Name, Length } else { @() }

$env:APPDATA           = Join-Path $Root "appdata"
$env:HE_COLLAB_OFFLINE = "1"   # a new exe path would otherwise raise the firewall prompt
$env:HE_HIDDEN_WINDOW  = "1"
$env:HE_BACKGROUND_FPS = "0"   # hidden but unthrottled: the first frame (preflight) comes as early as on screen
# The previous run's log stays until the new editor truncates it; a poll in that
# window reads the OLD lines and closes the new editor during its init.
Remove-Item $log -Force -ErrorAction SilentlyContinue
$start = Get-Date
$p = Start-Process -FilePath (Join-Path $deploy "HorizonEditor.exe") -WorkingDirectory $deploy -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
$killed = $false; $note = ""
while ($sw.Elapsed.TotalSeconds -lt $Timeout) {
    if ($p.HasExited) { break }
    if ($KillOnPartial) {
        if (Test-Path (Join-Path $cache ".manifest.json.tmp.partial")) {
            $p.Kill(); $killed = $true
            $note = "killed on .partial at $([int]$sw.Elapsed.TotalMilliseconds) ms"; break
        }
        [Threading.Thread]::Sleep(1); continue
    }
    $l    = Read-Log
    $ref  = @($l | Select-String 'EngineContent manifest refreshed').Count
    $fail = @($l | Select-String 'Could not fetch EngineContent manifest|cannot reach').Count
    $pf   = @($l | Select-String 'Project preflight: \d+ engine asset').Count
    if ($pf -ge 1 -and ($ref + $fail) -ge 2) { Start-Sleep -Seconds $CloseWait; break }
    Start-Sleep -Milliseconds 100
}
$timedOut = $sw.Elapsed.TotalSeconds -ge $Timeout
if (-not $killed -and -not $p.HasExited) {
    taskkill /PID $p.Id | Out-Null          # WM_CLOSE: clean shutdown
    if (-not $p.WaitForExit(20000)) { taskkill /F /T /PID $p.Id | Out-Null; $note = "clean close timed out, forced" }
} else { $null = $p.WaitForExit(10000) }

Copy-Item $log (Join-Path $ev "$Name.log") -Force -ErrorAction SilentlyContinue
$after = if (Test-Path $cache) { Get-ChildItem $cache -Force | Select-Object Name, Length } else { @() }

"=== $Name  start $($start.ToString('HH:mm:ss.fff'))  pid $($p.Id)  exit $($p.ExitCode)  timedOut=$timedOut  $note"
Read-Log | Select-String 'Project preflight|EngineContent|Could not|Renaming' | ForEach-Object { $_.Line }
"--- cache before: " + (($before | ForEach-Object { "$($_.Name)($($_.Length))" }) -join ', ')
"--- cache after:  " + (($after  | ForEach-Object { "$($_.Name)($($_.Length))" }) -join ', ')
