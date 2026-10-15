param([string]$Name, [int]$Rhi = 3, [string]$Out, [switch]$Incremental, [int]$SettleSec = 45,
      [string]$Editor = "C:\hw189b\deploy\Editor")
# Thema 189 step 3: the FIXED editor (a deploy without EngineContent/Textures) on a chosen
# backend (RHI 1 = Vulkan, 2 = D3D11, 3 = D3D12), with a private APPDATA copied from
# appdata_C (its EngineContentCache holds only the three landscape arrays = the human's
# setup). Screenshot the editor, then export through the MCP bridge into $Out; with
# -Incremental the export goes over whatever pak is already there (the dialog's default).
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$appdata = "C:\hw189\appdata_$Name"
if (-not (Test-Path $appdata)) { Copy-Item -Recurse C:\hw189\appdata_C $appdata }
$cfgPath = "$appdata\HorizonEngine\config.json"
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$cfg.RHI = $Rhi
$cfg | ConvertTo-Json -Depth 6 | Out-File -Encoding ascii $cfgPath
$env:APPDATA = $appdata
$env:HE_MCP = "1"; $env:HE_COLLAB_OFFLINE = "1"; $env:HE_NET_LOOPBACK_ONLY = "1"
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"
$exe = "$Editor\HorizonEditor.exe"
$log = "$Editor\HorizonEngine.log"
if (Test-Path $log) { Remove-Item $log -Force }
$ep = "$appdata\HorizonEngine\mcp-endpoint.json"
if (Test-Path $ep) { Remove-Item $ep -Force }

if (-not ("HeWin3" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class HeWin3 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public static Bitmap Capture(IntPtr h) {
        RECT r; GetClientRect(h, out r);
        Bitmap bmp = new Bitmap(Math.Max(1, r.R - r.L), Math.Max(1, r.B - r.T), PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); }
        return bmp;
    }
}
"@
}
[HeWin3]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$p = Start-Process -FilePath $exe -WorkingDirectory $Editor -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 120; $i++) { Start-Sleep -Milliseconds 500; $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle; break } }
if ($h -eq [IntPtr]::Zero) { "editor window never appeared"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Start-Sleep -Seconds $SettleSec
$p.Refresh(); $h = $p.MainWindowHandle   # the first handle is the splash; the main window comes later
New-Item -ItemType Directory -Force "C:\hw189\shots\editor_$Name" | Out-Null
$bmp = [HeWin3]::Capture($h); $bmp.Save("C:\hw189\shots\editor_$Name\editor.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
if (Test-Path $ep) {
    if (-not $Incremental -and (Test-Path $Out)) { Remove-Item -Recurse -Force $Out }
    python C:\hw189\pkg.py $ep $Out $(if ($Incremental) { "incr" } else { "full" })
} else { "no MCP endpoint at $ep" }
$pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($pp -and $pp.Path -eq $exe) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 1
Copy-Item $log "C:\hw189\shots\editor_$Name\HorizonEngine.log" -ErrorAction SilentlyContinue
"done $Name rhi=$Rhi out=$Out incremental=$Incremental"
