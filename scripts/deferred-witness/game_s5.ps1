# Thema 150 Schritt 5: the EXPORTED game (Depthy from Thema 130, swapchain post
# chain) on D3D11, D3D12 and Vulkan with the deferred path plus SSR, bloom and
# SMAA from config.json, and the SSR-off / forward controls.
# Game dir: a copy of C:\hw150\game12 (the export) with HorizonRendering.dll from
# the build tree (deploy\Game is stale after DLL-only builds).
param([string]$GameDir = "C:\hw150\game15", [string]$Root = "C:\hw150\g5", [string[]]$Only = @())
$runner = Join-Path $PSScriptRoot "..\..\docs\spielpfad-postfx-run-game.ps1"
New-Item -ItemType Directory -Force $Root | Out-Null
$env:S2_GPUDEBUG = "1"   # D3D12 debug layer / Vulkan validation
$common = "GameWindowMode=Windowed;PauseOnFocusLoss=false;BloomEnabled=true;AntiAliasing=2;DoFEnabled=false;MotionBlurEnabled=false"
foreach ($b in "D3D11", "D3D12", "Vulkan") {
    $runs = [ordered]@{
        "$($b)_def_ssr"   = "GameBackend=$b;RenderPath=1;SSREnabled=true;$common"
        "$($b)_def_nossr" = "GameBackend=$b;RenderPath=1;SSREnabled=false;$common"
        "$($b)_fwd_ssr"   = "GameBackend=$b;RenderPath=0;SSREnabled=true;$common"
    }
    foreach ($name in $runs.Keys) {
        if ($Only.Count -gt 0 -and -not ($Only | Where-Object { $name -like $_ })) { continue }
        & $runner -Name $name -GameDir $GameDir -Mode fresh -W 1280 -H 720 -SettleMs 4000 -Root $Root -Set $runs[$name]
    }
}
Remove-Item env:S2_GPUDEBUG -ErrorAction SilentlyContinue
