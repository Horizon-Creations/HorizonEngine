param([string]$Out = "C:\hw333\export", [string]$Profile = "Release", [string]$Scene = "Content/Scenes/Main.hescene")
$s=$PSScriptRoot   # scratch files (logs, dumps, mcp_drv.py) live beside the scripts
$r=(Resolve-Path "$PSScriptRoot\..\..").Path   # the repository root
Get-Process HorizonEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*deploy-rvt*' } | Stop-Process -Force
Start-Sleep -Seconds 1
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$ad="C:\hw333\ad"
New-Item -ItemType Directory -Force "$ad\HorizonEngine" | Out-Null
Copy-Item "$s\appdata_live_probe\HorizonEngine\config.json" "$ad\HorizonEngine\config.json" -Force
$env:APPDATA=$ad; $env:HE_HIDDEN_WINDOW="1"; $env:HE_BACKGROUND_FPS="0"; $env:HE_MCP="1"; $env:HE_SKIP_PROJECT_PREFLIGHT="1"; $env:HE_EXIT_AFTER_FRAMES="400000"
foreach ($v in "HE_RVT","HE_PROFILE_CAPTURE","HE_PROFILE_WARMUP","HE_PROFILE_QUIT","HE_PROFILE_DETAILED") { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
$env:LIVE_EP="$ad\HorizonEngine\mcp-endpoint.json"; Remove-Item $env:LIVE_EP -ErrorAction SilentlyContinue
$exe="$r\out\deploy-rvt-rel\Editor\HorizonEditor.exe"
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -RedirectStandardOutput "$s\export_out.txt" -RedirectStandardError "$s\export_err.txt"
for ($i=0; $i -lt 120; $i++) { Start-Sleep -Milliseconds 500; if (Test-Path "$ad\HorizonEngine\mcp-endpoint.json") { break } }
Start-Sleep -Seconds 4
$py = @"
import sys, json, time
sys.path.insert(0, r'$s')
import mcp_drv
PROFILE = '$Profile'
def call(n, a):
    r = mcp_drv.call(n, a)
    return r
print('open', str(call('scene_open', {'path': 'Scenes/Big.hescene'}))[:160])
time.sleep(3)
print('package', str(call('project_package', {'outputDir': r'$Out', 'targetPlatform': 'Host', 'incremental': False, 'profile': PROFILE, 'startupScene': r'$Scene'}))[:400])
for i in range(120):
    time.sleep(2)
    st = call('project_build_status', {'maxLines': 6, 'minSeverity': 1})
    sc = st.get('result', {}).get('structuredContent', {})
    if not sc.get('running', True):
        print('status', json.dumps(sc)[:900]); break
"@
$py | python -
if (-not $p.HasExited) { $p.Kill() }
Get-ChildItem $Out -ErrorAction SilentlyContinue | Select-Object Name | Format-Table -HideTableHeaders | Out-String
