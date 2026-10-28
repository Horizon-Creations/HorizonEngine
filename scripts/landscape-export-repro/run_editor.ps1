param([string]$Name, [int]$SettleSec = 40)
# Thema 189 step 1: private editor (deploy copy without Textures/Landscape), private
# APPDATA whose EngineContentCache holds the three landscape arrays (= the human's
# setup), screenshot the editor window, then export through the MCP bridge.
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:APPDATA = "C:\hw189\appdata_$Name"
$env:HE_MCP = "1"; $env:HE_COLLAB_OFFLINE = "1"; $env:HE_NET_LOOPBACK_ONLY = "1"
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"
$exe = "C:\hw189\editor\HorizonEditor.exe"
$log = "C:\hw189\editor\HorizonEngine.log"
if (Test-Path $log) { Remove-Item $log -Force }
$ep = "$env:APPDATA\HorizonEngine\mcp-endpoint.json"
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
$p = Start-Process -FilePath $exe -WorkingDirectory "C:\hw189\editor" -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 120; $i++) { Start-Sleep -Milliseconds 500; $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle; break } }
if ($h -eq [IntPtr]::Zero) { "editor window never appeared"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Start-Sleep -Seconds $SettleSec
$p.Refresh(); $h = $p.MainWindowHandle   # the first handle is the splash; the main window comes later
New-Item -ItemType Directory -Force "C:\hw189\shots\editor_$Name" | Out-Null
$bmp = [HeWin3]::Capture($h); $bmp.Save("C:\hw189\shots\editor_$Name\editor.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
if (Test-Path $ep) {
    $out = "C:\hw189\out_$Name"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    python C:\hw189\pkg.py $ep $out
} else { "no MCP endpoint at $ep" }
$pp = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($pp -and $pp.Path -eq $exe) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 1
Copy-Item $log "C:\hw189\shots\editor_$Name\HorizonEngine.log" -ErrorAction SilentlyContinue
"done $Name"
