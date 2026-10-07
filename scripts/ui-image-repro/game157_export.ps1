param([string]$Deploy = 'C:\hw157\deploy', [string]$Root = 'C:\hw157\game', [string]$Out = 'C:\hw157\game\out')
# Thema 157 Schritt 4: privater Editor (eigenes APPDATA, HE_MCP=1, GL) oeffnet
# ImgWit.heproj, game157_export.py legt das Widget an und exportiert nach $Out.
# Vorher: python game157_setup.py. In einer Kind-PowerShell starten (biegt APPDATA um).
$exe = Join-Path $Deploy 'Editor\HorizonEditor.exe'
$app = Join-Path $Root 'appdata_edit'
if (Test-Path $app) { Remove-Item -Recurse -Force $app }
New-Item -ItemType Directory -Force (Join-Path $app 'HorizonEngine') | Out-Null
$proj = ((Join-Path $Root 'proj\ImgWit\ImgWit.heproj') -replace '\\', '/')
"{ `"LastProjectPath`": `"$proj`", `"RHI`": `"OpenGL`" }" | Out-File -Encoding ascii (Join-Path $app 'HorizonEngine\config.json')
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
Get-ChildItem env: | Where-Object { $_.Name -like 'HE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:APPDATA = $app
$env:HE_MCP = '1'
$env:HE_COLLAB_OFFLINE = '1'
$env:HE_NET_LOOPBACK_ONLY = '1'
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
$ep = Join-Path $app 'HorizonEngine\mcp-endpoint.json'
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Minimized
$t0 = Get-Date
while (-not (Test-Path $ep) -and ((Get-Date) - $t0).TotalSeconds -lt 90 -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path $ep)) { "kein MCP-Endpunkt nach 90 s (exited=$($p.HasExited))"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Start-Sleep -Seconds 3
python (Join-Path $PSScriptRoot 'game157_export.py') $ep $Out (Join-Path $Deploy 'Editor')
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Copy-Item (Join-Path $Deploy 'Editor\HorizonEngine.log') (Join-Path $Root 'export_editor.log') -ErrorAction SilentlyContinue
