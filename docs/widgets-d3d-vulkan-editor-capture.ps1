param([string]$Deploy = 'C:\hw133\deploy', [string]$Tag = 'base', [string[]]$Rhis = @('OpenGL','D3D11','D3D12','Vulkan'),
      [string]$Shots = 'C:\hw133\shots', [string]$Scratch = 'C:\hw133')
# HE_DUMP_UITEST witness: one widget asset through WidgetManager -> extractUI -> backend UI pass,
# captured from the editor viewport RT (the path PIE renders through). Thema 133.
# Each run gets a fresh private APPDATA under $Scratch (never the human's config).
# Check the result with docs/widgets-d3d-vulkan-verify.ps1.
$exe = Join-Path $Deploy 'Editor\HorizonEditor.exe'
$log = Join-Path $Deploy 'Editor\HorizonEngine.log'
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
    $env:HE_DUMP_UITEST = '1'
    if ($Tag -like '*gi*') { $env:HE_DUMP_GI = '1'; $env:HE_DUMP_SSAO = '1'; $env:HE_DUMP_SHADOWINSTTEST = '1'; $env:HE_DUMP_SKYTEST = '1' }
    $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Minimized
    $t0 = Get-Date; $dumped = $null
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
