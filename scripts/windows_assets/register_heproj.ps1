# Associates .heproj project files with the HorizonEditor.exe in the folder above this
# one, for the CURRENT USER only: everything goes under HKCU\Software\Classes, so no
# administrator is needed and nothing outside the user's own profile changes.
#
#   register_heproj.cmd     register (double-click it, or run it from a prompt)
#   unregister_heproj.cmd   remove it again
#
# Why a script and not a .reg file or an installer: the Windows editor ships as a ZIP
# you unpack anywhere, so the one thing a ProgID has to say -- WHERE HorizonEditor.exe
# is -- is only known here. Run it once after unpacking, and again if you move the
# folder (the registry holds the absolute path).
#
# What gets written, per the shell's file association contract:
#   .heproj                     (default) = the ProgID, Content Type = application/x-heproj
#   .heproj\OpenWithProgids     the ProgID, so "Open with" lists it too
#   <ProgID>                    (default) = the name Explorer shows in the Type column
#   <ProgID>\DefaultIcon        FileTypes\heproj.ico
#   <ProgID>\shell\open\command "<this folder>\..\HorizonEditor.exe" "%1"
# Explorer is told afterwards (SHChangeNotify), so icons refresh without a sign-out.
#
# Keep the ProgID and MIME type in step with the macOS UTI (scripts/package_macos.sh)
# and the Linux MIME XML (scripts/linux_assets/horizon-editor.xml).
[CmdletBinding()]
param([switch]$Unregister)

$ErrorActionPreference = 'Stop'

$progId   = 'dev.horizoncreations.heproj'
$typeName = 'Horizon Engine Project'
$mimeType = 'application/x-heproj'
$classes  = 'Software\Classes'

$hkcu = [Microsoft.Win32.Registry]::CurrentUser
$string = [Microsoft.Win32.RegistryValueKind]::String

function Set-Value([string]$subKey, [string]$name, [string]$value) {
    $key = $hkcu.CreateSubKey($subKey)
    try { $key.SetValue($name, $value, $string) } finally { $key.Dispose() }
}

# Tell Explorer the associations changed; without it a new icon shows up after the
# next sign-in. Best effort: the registration is complete either way.
function Send-AssociationChanged {
    try {
        Add-Type -Namespace HorizonEngine -Name Shell -MemberDefinition @'
[DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
'@
        # SHCNE_ASSOCCHANGED, SHCNF_IDLIST
        [HorizonEngine.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    } catch {
        Write-Warning "Explorer could not be notified ($($_.Exception.Message)); icons refresh after the next sign-in."
    }
}

if ($Unregister) {
    $ext = $hkcu.OpenSubKey("$classes\.heproj")
    $owner = $null
    if ($ext) { $owner = $ext.GetValue(''); $ext.Dispose() }
    if ($owner -eq $progId) {
        # Ours alone: the extension key only exists because register_heproj made it.
        $hkcu.DeleteSubKeyTree("$classes\.heproj", $false)
    } else {
        # Somebody else's association: leave it, take only our "Open with" entry away.
        $with = $hkcu.OpenSubKey("$classes\.heproj\OpenWithProgids", $true)
        if ($with) { try { $with.DeleteValue($progId, $false) } finally { $with.Dispose() } }
    }
    $hkcu.DeleteSubKeyTree("$classes\$progId", $false)
    Send-AssociationChanged
    Write-Host 'Removed the .heproj file type registration.'
    exit 0
}

$editorDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
$exe  = Join-Path $editorDir 'HorizonEditor.exe'
$icon = Join-Path $PSScriptRoot 'heproj.ico'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Write-Error "HorizonEditor.exe not found at '$exe'. Run this from the unpacked editor folder (FileTypes\register_heproj.cmd)."
}
if (-not (Test-Path -LiteralPath $icon -PathType Leaf)) {
    Write-Error "heproj.ico not found at '$icon'."
}

Set-Value "$classes\.heproj" '' $progId
Set-Value "$classes\.heproj" 'Content Type' $mimeType
# REG_NONE with no data is how a value in OpenWithProgids says "this ProgID may open me".
$with = $hkcu.CreateSubKey("$classes\.heproj\OpenWithProgids")
try { $with.SetValue($progId, [byte[]]@(), [Microsoft.Win32.RegistryValueKind]::None) } finally { $with.Dispose() }

Set-Value "$classes\$progId" '' $typeName
Set-Value "$classes\$progId\DefaultIcon" '' $icon
Set-Value "$classes\$progId\shell\open\command" '' ('"{0}" "%1"' -f $exe)

Send-AssociationChanged
Write-Host "Registered .heproj project files with $exe"
Write-Host 'Double-clicking a .heproj in Explorer now opens it in the Horizon Editor.'
Write-Host 'If Windows already had a choice stored for .heproj, pick the editor under Open with > Choose another app.'
