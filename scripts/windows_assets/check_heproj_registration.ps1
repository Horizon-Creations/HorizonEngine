# Proves that the .heproj file type registration leaves the registry the way Explorer
# needs it: FileTypes\register_heproj.cmd (and unregister_heproj.cmd, which takes it all
# away again), and the editor registering itself, HorizonEditor.exe --register-file-types,
# which is also what it runs at every start. Run by CI on the Windows runner against the
# packaged editor; also what to run by hand on a Windows machine:
#
#   powershell -ExecutionPolicy Bypass -File scripts\windows_assets\check_heproj_registration.ps1 -EditorDir <unpacked Editor folder>
#
# It does everything the real scripts and the editor do, to the real HKCU\Software\Classes
# of whoever runs it. What was there before (a .heproj association, the ProgID) is
# exported first and put back at the end, also when a check fails half way, so it is
# safe on a machine somebody uses. It does NOT double-click a file: that needs a desktop
# session and the editor window, which is the hardware check this does not claim.
#
# Three rounds. The first runs the scripts against the package as it is, and also asks
# the shell itself what it resolves .heproj to. The second copies the FileTypes folder
# next to a placeholder HorizonEditor.exe in a path with spaces and parentheses, because
# the quoting of "<exe>" "%1" is exactly what a path like "C:\Program Files (x86)\Horizon
# Editor" breaks, and a runner's own path never has one. A check on the side: unregister
# leaves another application's .heproj association alone.
#
# The third round runs the packaged HorizonEditor.exe itself, with --register-file-types:
# from nothing, again (a registration that is current must not be rewritten, not even a
# cosmetic value), after the folder moved, after the icon went missing, and with .heproj
# owned by another application (exit code 3 and nothing written). The exe is a GUI
# program and the line it prints may not survive a redirect, so the verdict is its exit
# code and the registry; the line is checked only when it did arrive. On an account where
# Explorer or the machine already maps .heproj to another application the round says so
# and stops, because the editor leaves that alone on purpose.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$EditorDir)

$ErrorActionPreference = 'Stop'
$progId = 'dev.horizoncreations.heproj'
$hkcu = [Microsoft.Win32.Registry]::CurrentUser

$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what"; $failures.Add($what) }
}

# Open, read, close: a handle left open here would keep a deleted key alive and make
# "is it gone" answer wrongly.
function Test-RegKey([string]$subKey) {
    $key = $hkcu.OpenSubKey($subKey)
    if ($key) { $key.Dispose(); return $true }
    return $false
}
function Get-Value([string]$subKey, [string]$name) {
    $key = $hkcu.OpenSubKey($subKey)
    if (-not $key) { return $null }
    try { return $key.GetValue($name) } finally { $key.Dispose() }
}
function Get-ValueNames([string]$subKey) {
    $key = $hkcu.OpenSubKey($subKey)
    if (-not $key) { return @() }
    try { return $key.GetValueNames() } finally { $key.Dispose() }
}

# Runs a launcher the way a command prompt would, and returns its exit code. "cmd /S /C"
# strips exactly the outer pair of quotes and runs  "<path>" /quiet  as written, which is
# the one form that survives parentheses in the path; the command line is built by hand
# and handed over as one string so PowerShell adds no quoting of its own.
function Invoke-Launcher([string]$cmdPath) {
    $line = '/S /C ""{0}" /quiet"' -f $cmdPath
    $p = Start-Process -FilePath $env:ComSpec -ArgumentList $line -Wait -PassThru -NoNewWindow
    return $p.ExitCode
}

# What the shell itself resolves for ".heproj", as opposed to what we wrote: the
# executable and the document name Explorer would show.
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class HeAssoc {
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
    static extern uint AssocQueryString(uint flags, uint str, string assoc, string extra, StringBuilder buffer, ref uint length);
    public static string Query(uint str, string assoc) {
        uint length = 2048;
        StringBuilder buffer = new StringBuilder((int)length);
        uint hr = AssocQueryString(0, str, assoc, null, buffer, ref length);
        if (hr != 0) return "HRESULT 0x" + hr.ToString("X8");
        return buffer.ToString();
    }
}
'@

function Test-Round([string]$label, [string]$dir, [bool]$askShell) {
    Write-Host "== $label ($dir)"
    $exe = Join-Path $dir 'HorizonEditor.exe'
    $types = Join-Path $dir 'FileTypes'
    Check (Test-Path -LiteralPath $exe -PathType Leaf) 'HorizonEditor.exe is there'
    foreach ($f in 'register_heproj.cmd', 'unregister_heproj.cmd', 'register_heproj.ps1', 'heproj.ico') {
        Check (Test-Path -LiteralPath (Join-Path $types $f) -PathType Leaf) "FileTypes\$f is there"
    }

    Check ((Invoke-Launcher (Join-Path $types 'register_heproj.cmd')) -eq 0) 'register_heproj.cmd exits 0'

    $resolvedExe = (Resolve-Path -LiteralPath $exe).ProviderPath
    Check ((Get-Value 'Software\Classes\.heproj' '') -eq $progId) ".heproj points at $progId"
    Check ((Get-Value 'Software\Classes\.heproj' 'Content Type') -eq 'application/x-heproj') 'Content Type is application/x-heproj'
    Check ((Get-ValueNames 'Software\Classes\.heproj\OpenWithProgids') -contains $progId) 'the ProgID is listed under OpenWithProgids'
    Check ((Get-Value "Software\Classes\$progId" '') -eq 'Horizon Engine Project') 'the ProgID names the type'
    $icon = Get-Value "Software\Classes\$progId\DefaultIcon" ''
    Check (($null -ne $icon) -and (Test-Path -LiteralPath $icon -PathType Leaf)) "DefaultIcon is a file that exists ($icon)"
    $command = Get-Value "Software\Classes\$progId\shell\open\command" ''
    $expected = '"{0}" "%1"' -f $resolvedExe
    Check ($command -eq $expected) "open command is exactly [$expected] (found [$command])"

    if ($askShell) {
        # A choice the user made in Explorer (UserChoice) outranks the registration, so on
        # a machine where somebody has already picked an app for .heproj the shell answers
        # with THAT, and the registration is not what is being asked about.
        if (Test-RegKey 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.heproj\UserChoice') {
            Write-Host '  skip the shell resolver: this account has a UserChoice for .heproj, which outranks the registration'
        } else {
            # ASSOCSTR_EXECUTABLE = 2, ASSOCSTR_FRIENDLYDOCNAME = 3
            $shellExe = [HeAssoc]::Query(2, '.heproj')
            $shellName = [HeAssoc]::Query(3, '.heproj')
            Check ($shellExe -eq $resolvedExe) "the shell resolves .heproj to $resolvedExe (it says: $shellExe)"
            Check ($shellName -eq 'Horizon Engine Project') "the shell names .heproj 'Horizon Engine Project' (it says: $shellName)"
        }
    }

    Check ((Invoke-Launcher (Join-Path $types 'unregister_heproj.cmd')) -eq 0) 'unregister_heproj.cmd exits 0'
    Check (-not (Test-RegKey 'Software\Classes\.heproj')) '.heproj is gone again'
    Check (-not (Test-RegKey "Software\Classes\$progId")) 'the ProgID is gone again'
}

# ---- Round 3: the editor registers .heproj for itself ---------------------------------
# HorizonEditor.exe --register-file-types is the registration the editor runs at every
# start, synchronously and without a window (src/HE_Editor/HeprojRegistration.*). Exit
# codes: 0 registered, updated or already registered; 1 failed; 3 left alone (another
# application owns .heproj, and then nothing at all is written); 4 no FileTypes folder
# next to the exe; 5 unsupported platform. It also prints one line "<keyword>: <sentence>",
# but a GUI program does not always get a redirect through, so the verdict is the exit
# code and the registry, and the line is only checked when it did arrive.

# A string value, its key created if need be; the handle is closed again at once (see
# Test-RegKey).
function Set-Value([string]$subKey, [string]$name, [string]$value) {
    $key = $hkcu.CreateSubKey($subKey)
    try { $key.SetValue($name, $value, [Microsoft.Win32.RegistryValueKind]::String) } finally { $key.Dispose() }
}

# Both class keys out, so that the next run of the editor starts from "nothing registered".
function Remove-OurKeys {
    foreach ($name in '.heproj', $progId) { $hkcu.DeleteSubKeyTree("Software\Classes\$name", $false) }
}

# What a program left in a redirect file, trimmed and capped, or '' (no file, an empty one,
# one that is still locked). NULs and a byte order mark are dropped, so UTF-16 output
# reads as text.
function Read-Text([string]$path) {
    $text = ''
    try { $text = [IO.File]::ReadAllText($path) } catch { return '' }
    $text = ($text -replace '[\x00\uFEFF]', '').Trim()
    if ($text.Length -gt 600) { $text = $text.Substring(0, 600) }
    return $text
}

function Format-ExitCode($code) {
    if ($null -eq $code) { return 'none' }
    return ('{0} (0x{1:X8})' -f $code, $code)
}

# One run of the editor in registration mode, with stdout and stderr redirected into files
# of the throwaway folder $work, which is also its working directory: whatever the exe
# drops there does not end up in the package. Never throws; what happened comes back as an
# object: Started, ExitCode, Output and Error (the redirected text) and Problem (why it did
# not start). -Wait -PassThru is the form Invoke-Launcher relies on for a reliable ExitCode.
function Invoke-SelfRegistration([string]$exe, [string]$work) {
    $stamp = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $outFile = Join-Path $work ('stdout-' + $stamp + '.txt')
    $errFile = Join-Path $work ('stderr-' + $stamp + '.txt')
    $run = [pscustomobject]@{ Started = $false; ExitCode = $null; Output = ''; Error = ''; Problem = '' }
    try {
        $p = Start-Process -FilePath $exe -ArgumentList '--register-file-types' -WorkingDirectory $work -Wait -PassThru -NoNewWindow -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        $run.Started = $true
        $run.ExitCode = $p.ExitCode
    } catch {
        $run.Problem = $_.Exception.Message
    }
    $run.Output = Read-Text $outFile
    $run.Error = Read-Text $errFile
    return $run
}

# Says what a run did and returns false (after recording a FAIL) when it never got as far
# as running our code, so that the steps which depend on it are not taken.
function Confirm-EditorRan($run, [string]$when) {
    Write-Host ('  [{0}] exit code {1}; stdout [{2}]; stderr [{3}]' -f $when, (Format-ExitCode $run.ExitCode), $run.Output, $run.Error)
    if (-not $run.Started) {
        Check $false "[$when] HorizonEditor.exe could not be started ($($run.Problem))"
        return $false
    }
    if ($null -eq $run.ExitCode) {
        Check $false "[$when] HorizonEditor.exe ran, but PowerShell could not read its exit code ($($run.Problem))"
        return $false
    }
    # STATUS_DLL_NOT_FOUND as the signed Int32 that ExitCode is (0xC0000135 as a literal
    # would be a positive number and never match).
    if ($run.ExitCode -eq -1073741515) {
        Check $false "[$when] HorizonEditor.exe could not load a DLL (exit code -1073741515 = 0xC0000135): the package is incomplete, or a runtime is missing on this machine"
        return $false
    }
    return $true
}

function Check-Exit($run, [int]$want, [string]$when) {
    Check ($run.ExitCode -eq $want) "[$when] exit code is $want (it was $(Format-ExitCode $run.ExitCode))"
}

# The line the exe printed ("<keyword>: <sentence>"), when one came through the redirect.
# Looked for at the start of any line, in case something else wrote to stdout before it.
function Check-Printed($run, [string]$pattern, [string]$when) {
    if ($run.Output.Length -eq 0) {
        Write-Host "  note [$when] nothing came through the redirected stdout of this GUI program; the exit code and the registry decide"
        return
    }
    Check ($run.Output -match ('(?m)' + $pattern)) "[$when] the editor printed the expected line (it said: $($run.Output))"
}

# What the editor leaves in HKCU (the layout register_heproj.ps1 writes), judged against
# the exe that was run and the icon it ships ($icon = <exe folder>\FileTypes\heproj.ico).
function Check-Registration([string]$exe, [string]$icon, [string]$when) {
    Check ((Get-Value 'Software\Classes\.heproj' '') -eq $progId) "[$when] .heproj points at $progId"
    Check ((Get-Value 'Software\Classes\.heproj' 'Content Type') -eq 'application/x-heproj') "[$when] Content Type is application/x-heproj"
    Check ((Get-ValueNames 'Software\Classes\.heproj\OpenWithProgids') -contains $progId) "[$when] the ProgID is listed under OpenWithProgids"
    Check ((Get-Value "Software\Classes\$progId" '') -eq 'Horizon Engine Project') "[$when] the ProgID names the type"
    $have = Get-Value "Software\Classes\$progId\DefaultIcon" ''
    Check ((-not [string]::IsNullOrEmpty($have)) -and (Test-Path -LiteralPath $have -PathType Leaf)) "[$when] DefaultIcon is a file that exists ($have)"
    Check ($have -eq $icon) "[$when] DefaultIcon is [$icon] (found [$have])"
    $command = Get-Value "Software\Classes\$progId\shell\open\command" ''
    $expected = '"{0}" "%1"' -f $exe
    Check ($command -eq $expected) "[$when] open command is exactly [$expected] (found [$command])"
}

function Test-SelfRegistration([string]$dir) {
    Write-Host "== the editor registers .heproj for itself ($dir)"
    $exeFile = Join-Path $dir 'HorizonEditor.exe'
    if (-not (Test-Path -LiteralPath $exeFile -PathType Leaf)) {
        Check $false "HorizonEditor.exe is there ($exeFile)"
        return
    }
    $exe = (Resolve-Path -LiteralPath $exeFile).ProviderPath
    $icon = Join-Path ([IO.Path]::GetDirectoryName($exe)) 'FileTypes\heproj.ico'
    $userChoiceKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.heproj\UserChoice'

    # Before it writes anything the editor looks at a choice made in Explorer (UserChoice),
    # then at the merged HKEY_CLASSES_ROOT view, and only then at its own three values. Where
    # the first or the second belongs to another application it rightly writes nothing:
    # there is nothing to register on such an account, and failing would blame the editor
    # for being polite.
    $chosen = Get-Value $userChoiceKey 'ProgId'
    if ($chosen -and ($chosen -ne $progId) -and ($chosen -ne 'Applications\HorizonEditor.exe')) {
        Write-Host "  skip: this account picked '$chosen' for .heproj in Explorer (UserChoice), so the editor rightly leaves .heproj alone"
        return
    }

    $before = @(Get-ChildItem -LiteralPath $dir -Force | ForEach-Object { $_.Name })
    $work = Join-Path ([IO.Path]::GetTempPath()) ('heproj-selfreg-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    $configDir = Join-Path $work 'editor config'
    New-Item -ItemType Directory -Path $configDir | Out-Null
    # Whatever the mode does, it must not land in a real editor config.
    $oldConfigDir = [Environment]::GetEnvironmentVariable('HE_CONFIG_DIR', 'Process')
    [Environment]::SetEnvironmentVariable('HE_CONFIG_DIR', $configDir, 'Process')
    try {
        Remove-OurKeys
        $machineWide = [Microsoft.Win32.Registry]::GetValue('HKEY_CLASSES_ROOT\.heproj', '', $null)
        if ($machineWide -and ($machineWide -ne $progId)) {
            Write-Host "  skip: HKEY_CLASSES_ROOT maps .heproj to '$machineWide' with nothing of ours in HKCU, so the editor rightly leaves it alone"
            return
        }

        # 1. From nothing: it registers, and the shell agrees.
        $run = Invoke-SelfRegistration $exe $work
        if (-not (Confirm-EditorRan $run 'clean start')) { return }
        Check-Exit $run 0 'clean start'
        Check-Printed $run '^registered:' 'clean start'
        Check-Registration $exe $icon 'clean start'
        Check (-not (Test-Path -LiteralPath (Join-Path $configDir 'config.json'))) '[clean start] the editor config was not touched (no config.json)'
        if (Test-RegKey $userChoiceKey) {
            Write-Host '  skip the shell resolver: this account has a UserChoice for .heproj, which outranks the registration'
        } else {
            # ASSOCSTR_EXECUTABLE = 2, ASSOCSTR_FRIENDLYDOCNAME = 3
            $shellExe = [HeAssoc]::Query(2, '.heproj')
            $shellName = [HeAssoc]::Query(3, '.heproj')
            Check ($shellExe -eq $exe) "[clean start] the shell resolves .heproj to $exe (it says: $shellExe)"
            Check ($shellName -eq 'Horizon Engine Project') "[clean start] the shell names .heproj 'Horizon Engine Project' (it says: $shellName)"
        }

        # 2. Again: a registration that is current is left exactly as it is. Only the
        # program and the icon decide that, so a changed Content Type, which nobody
        # double-clicks on, must survive.
        Set-Value 'Software\Classes\.heproj' 'Content Type' 'text/tampered'
        $run = Invoke-SelfRegistration $exe $work
        if (-not (Confirm-EditorRan $run 'second run')) { return }
        Check-Exit $run 0 'second run'
        Check ((Get-Value 'Software\Classes\.heproj' 'Content Type') -eq 'text/tampered') '[second run] a registration that is current is not rewritten (the tampered Content Type is still there)'
        Check-Printed $run '^already-registered:' 'second run'

        # 3. The folder moved: the registry still names the old place. The repair writes
        # everything again, so the Content Type tampered with in step 2 is back as well.
        Set-Value "Software\Classes\$progId\shell\open\command" '' '"C:\Somewhere Else (x86)\HorizonEditor.exe" "%1"'
        $run = Invoke-SelfRegistration $exe $work
        if (-not (Confirm-EditorRan $run 'moved')) { return }
        Check-Exit $run 0 'moved'
        Check-Printed $run '^updated:' 'moved'
        Check-Registration $exe $icon 'moved'

        # 4. The icon went missing (the FileTypes folder was tidied away, then restored).
        Set-Value "Software\Classes\$progId\DefaultIcon" '' (Join-Path $work 'no such folder\gone.ico')
        $run = Invoke-SelfRegistration $exe $work
        if (-not (Confirm-EditorRan $run 'icon')) { return }
        Check-Exit $run 0 'icon'
        Check-Printed $run '^updated:' 'icon'
        Check-Registration $exe $icon 'icon'

        # 5. .heproj belongs to another application: the editor leaves it alone and writes
        # nothing, not even the ProgID. (Where a UserChoice exists the editor would refuse
        # because of that, and this step would no longer be about the association.)
        if (Test-RegKey $userChoiceKey) {
            Write-Host '  skip the foreign-association step: this account has a UserChoice for .heproj'
        } else {
            Remove-OurKeys
            Set-Value 'Software\Classes\.heproj' '' 'SomeoneElse.Project'
            $run = Invoke-SelfRegistration $exe $work
            if (Confirm-EditorRan $run 'foreign') {
                Check-Exit $run 3 'foreign'
                Check-Printed $run '^left-alone:' 'foreign'
                Check ((Get-Value 'Software\Classes\.heproj' '') -eq 'SomeoneElse.Project') "[foreign] another application's .heproj association is still there"
                Check (-not (Test-RegKey "Software\Classes\$progId")) '[foreign] the ProgID was not created'
                Check (-not (Test-RegKey 'Software\Classes\.heproj\OpenWithProgids')) '[foreign] no Open with entry was added'
                Check ($null -eq (Get-Value 'Software\Classes\.heproj' 'Content Type')) '[foreign] no Content Type was added'
            }
        }
    } finally {
        [Environment]::SetEnvironmentVariable('HE_CONFIG_DIR', $oldConfigDir, 'Process')
        # 6. Ours out again (the outer finally also puts back what the user had before).
        Remove-OurKeys
        # This check runs before the package is zipped, so a run must leave nothing in it (a
        # log next to the exe, a dumps folder). Not a failure: a log next to the exe is how
        # every editor run starts. A log that was already there is rotated, not created, so
        # a developer's own logs are left alone.
        $hadLog = @($before | Where-Object { $_ -like 'HorizonEngine.log*' }).Count -gt 0
        foreach ($item in @(Get-ChildItem -LiteralPath $dir -Force)) {
            if ($before -contains $item.Name) { continue }
            if ($hadLog -and ($item.Name -like 'HorizonEngine.log*')) { continue }
            Remove-Item -LiteralPath $item.FullName -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "  note: --register-file-types left $($item.Name) in the package folder; removed it"
        }
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# What was registered before we started, exported so it can be put back.
$classKeys = @('.heproj', $progId)
$backups = @{}
$backupDir = Join-Path ([IO.Path]::GetTempPath()) ('heproj-registry-backup-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $backupDir | Out-Null
foreach ($name in $classKeys) {
    if (Test-RegKey "Software\Classes\$name") {
        $file = Join-Path $backupDir ($name + '.reg')
        & reg.exe export "HKCU\Software\Classes\$name" $file /y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "could not back up HKCU\Software\Classes\$name before the check" }
        $backups[$name] = $file
        Write-Host "backed up the existing HKCU\Software\Classes\$name"
    }
}

$scratch = $null
try {
    $editor = (Resolve-Path -LiteralPath $EditorDir).ProviderPath
    Test-Round 'the packaged editor' $editor $true

    $scratch = Join-Path ([IO.Path]::GetTempPath()) ('He Editor (x86) ' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    Set-Content -LiteralPath (Join-Path $scratch 'HorizonEditor.exe') -Value 'placeholder' -Encoding ASCII
    Copy-Item -LiteralPath (Join-Path $editor 'FileTypes') -Destination (Join-Path $scratch 'FileTypes') -Recurse
    Test-Round 'a path with spaces and parentheses' $scratch $false

    # A registration that is somebody else's must survive our unregister.
    $other = $hkcu.CreateSubKey('Software\Classes\.heproj')
    $other.SetValue('', 'SomeoneElse.Project')
    $other.Dispose()
    Invoke-Launcher (Join-Path $editor 'FileTypes\unregister_heproj.cmd') | Out-Null
    Check ((Get-Value 'Software\Classes\.heproj' '') -eq 'SomeoneElse.Project') "unregister leaves another application's .heproj association alone"

    # The editor registering itself, for real (HorizonEditor.exe --register-file-types).
    # It starts by clearing both keys, so the association left behind just above is gone.
    Test-SelfRegistration $editor
} finally {
    # Ours out, whatever was there before back in; also when a check threw.
    foreach ($name in $classKeys) {
        $hkcu.DeleteSubKeyTree("Software\Classes\$name", $false)
        if ($backups.ContainsKey($name)) {
            try {
                & reg.exe import $backups[$name] 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) { Write-Host "WARNING: could not restore HKCU\Software\Classes\$name from $($backups[$name])" }
            } catch {
                Write-Host "WARNING: could not restore HKCU\Software\Classes\$name from $($backups[$name]): $($_.Exception.Message)"
            }
        }
    }
    if ($scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host "$($failures.Count) check(s) failed:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host ''
Write-Host 'All .heproj registration checks passed.'
