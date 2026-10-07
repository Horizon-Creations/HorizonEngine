param([string]$Deploy = 'C:\hw157\deploy', [string]$Tag = 'base', [string[]]$Rhis = @('OpenGL','Vulkan','D3D11','D3D12'),
      [string]$Shots = 'C:\hw157\shots', [string]$Scratch = 'C:\hw157', [string]$UiTest = 'image',
      [hashtable]$Extra = @{})
# Thema 157: UI-Bild-Quad-Zeuge. HE_DUMP_UITEST=image legt zu den 12 Stil-Kacheln aus
# Thema 133 eine 13. Kachel (Image-Element, erzeugtes Vier-Quadranten-Bild, Tint weiss) an
# und schickt alles durch WidgetManager -> extractUI -> UI-Pass des Backends. Aufgenommen
# wird das Viewport-RT des Editors (der Weg, den auch PIE nimmt).
# Jeder Lauf bekommt ein frisches APPDATA unter $Scratch (nie die Konfiguration des Menschen).
# Auswertung: python scripts/ui-image-repro/ana157.py <Shots> <Tag>
# -Deploy und -Shots immer ausdruecklich setzen, sonst misst man still einen anderen Build.
$exe = Join-Path $Deploy 'Editor\HorizonEditor.exe'
$log = Join-Path $Deploy 'Editor\HorizonEngine.log'
if (-not (Test-Path $exe)) { throw "kein Editor unter $exe" }
New-Item -ItemType Directory -Force $Shots | Out-Null
foreach ($rhi in $Rhis) {
    Get-ChildItem env: | Where-Object { $_.Name -like 'HE_DUMP_*' -or $_.Name -in @('HE_WORLD_PREVIEW_DUMP','HE_DEPTH_INSTANCING','HE_GPU_DEBUG','HE_MCP') } |
        ForEach-Object { Remove-Item "env:$($_.Name)" }
    $app = Join-Path $Scratch "appdata_${Tag}_$rhi"
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    New-Item -ItemType Directory -Force $app | Out-Null
    $env:APPDATA = $app
    $env:HE_COLLAB_OFFLINE = '1'
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
    $bmp = Join-Path $Shots "${Tag}_$rhi.bmp"
    if (Test-Path $bmp) { Remove-Item $bmp }
    $env:HE_DUMP_PATH = $bmp
    $env:HE_DUMP_QUIT = '1'
    $env:HE_DUMP_RHI = $rhi
    $env:HE_DUMP_FRAMES = '16'
    $env:HE_DUMP_UITEST = $UiTest
    # Zusaetzliche Variablen NACH dem Aufraeumen oben, z. B. -Extra @{HE_GPU_DEBUG='1'}
    # fuer den D3D12-Debug-Layer (sonst loescht die Schleife sie wieder).
    foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }
    $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Minimized
    $t0 = Get-Date; $dumped = $null
    # D3D11/D3D12 stuerzen nach dem Dump beim Beenden ab (bekannt): BMP abwarten, dann beenden.
    while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 150) {
        Start-Sleep -Milliseconds 500
        if (-not $dumped -and (Test-Path $bmp)) { $dumped = Get-Date }
        if ($dumped -and ((Get-Date) - $dumped).TotalSeconds -gt 10) { break }
    }
    if (-not $p.HasExited) {
        $pp = (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)
        if ($pp -and $pp.Path -eq $exe) { Stop-Process -Id $p.Id -Force }
    }
    Copy-Item $log (Join-Path $Shots "${Tag}_$rhi.log") -ErrorAction SilentlyContinue
    "$rhi : exited=$($p.HasExited) bmp=$(Test-Path $bmp) secs=$([int]((Get-Date)-$t0).TotalSeconds)"
}
