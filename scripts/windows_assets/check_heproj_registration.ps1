# Proves that FileTypes\register_heproj.cmd leaves the registry the way Explorer needs
# it, and that unregister_heproj.cmd takes it all away again. Run by CI on the Windows
# runner against the packaged editor; also what to run by hand on a Windows machine:
#
#   powershell -ExecutionPolicy Bypass -File scripts\windows_assets\check_heproj_registration.ps1 -EditorDir <unpacked Editor folder>
#
# It does everything the real scripts do, to the real HKCU\Software\Classes of whoever
# runs it, and removes it at the end. It does NOT double-click a file: that needs a
# desktop session and the editor window, which is the hardware check this does not claim.
#
# Two rounds. The first runs against the package as it is, and also asks the shell
# itself what it resolves .heproj to. The second copies the FileTypes folder next to a
# placeholder HorizonEditor.exe in a path with spaces and parentheses, because the
# quoting of "<exe>" "%1" is exactly what a path like "C:\Program Files (x86)\Horizon
# Editor" breaks, and a runner's own path never has one.
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

    # Called directly, not through "cmd /c": PowerShell quotes the path for a batch file
    # itself, which is what Explorer does too; cmd /c would trip over the parentheses.
    & (Join-Path $types 'register_heproj.cmd') /quiet
    Check ($LASTEXITCODE -eq 0) 'register_heproj.cmd exits 0'

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
        # ASSOCSTR_EXECUTABLE = 2, ASSOCSTR_FRIENDLYDOCNAME = 3
        $shellExe = [HeAssoc]::Query(2, '.heproj')
        $shellName = [HeAssoc]::Query(3, '.heproj')
        Check ($shellExe -eq $resolvedExe) "the shell resolves .heproj to $resolvedExe (it says: $shellExe)"
        Check ($shellName -eq 'Horizon Engine Project') "the shell names .heproj 'Horizon Engine Project' (it says: $shellName)"
    }

    & (Join-Path $types 'unregister_heproj.cmd') /quiet
    Check ($LASTEXITCODE -eq 0) 'unregister_heproj.cmd exits 0'
    Check (-not (Test-RegKey 'Software\Classes\.heproj')) '.heproj is gone again'
    Check (-not (Test-RegKey "Software\Classes\$progId")) 'the ProgID is gone again'
}

$editor = (Resolve-Path -LiteralPath $EditorDir).ProviderPath
Test-Round 'the packaged editor' $editor $true

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('He Editor (x86) ' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    New-Item -ItemType Directory -Path $scratch | Out-Null
    Set-Content -LiteralPath (Join-Path $scratch 'HorizonEditor.exe') -Value 'placeholder' -Encoding ASCII
    Copy-Item -LiteralPath (Join-Path $editor 'FileTypes') -Destination (Join-Path $scratch 'FileTypes') -Recurse
    Test-Round 'a path with spaces and parentheses' $scratch $false
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# A registration that is somebody else's must survive our unregister.
$other = $hkcu.CreateSubKey('Software\Classes\.heproj')
$other.SetValue('', 'SomeoneElse.Project')
$other.Dispose()
& (Join-Path $editor 'FileTypes\unregister_heproj.cmd') /quiet
Check ((Get-Value 'Software\Classes\.heproj' '') -eq 'SomeoneElse.Project') "unregister leaves another application's .heproj association alone"
$hkcu.DeleteSubKeyTree('Software\Classes\.heproj', $false)

if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host "$($failures.Count) check(s) failed:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host ''
Write-Host 'All .heproj registration checks passed.'
