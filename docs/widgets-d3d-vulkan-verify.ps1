param([string]$Shots = 'C:\hw133\shots', [string]$EditorTag = 'fix', [string]$GameTag = 'gfix',
      [string[]]$Rhis = @('OpenGL','D3D11','D3D12','Vulkan'))
# Thema 133 step 2: PASS/FAIL over the before/after captures, per backend and path.
#
#   Editor/PIE path: <Shots>\<EditorTag>_<RHI>.bmp from docs/widgets-d3d-vulkan-editor-capture.ps1
#     (HE_DUMP_UITEST, 1280x720, WidgetManager widget -> extractUI -> backend UI pass).
#     - tile 0 (x 70..290, y 90..180) has the tile colour (66,76,97) at its TRUE rows
#     - the vertically mirrored rows (y 540..630) do NOT carry it (catches the Vulkan flip)
#     - Vulkan differs from D3D11 on < 1 % of sampled pixels (same UI shader feature set;
#       catches a flip of everything incl. glyphs, which a pixel COUNT cannot)
#   Game path: <Shots>\<GameTag>_<RHI>.png from docs/widgets-d3d-vulkan-game-capture.ps1
#     (exported UiWit game, swapchain). Red top-left, green bottom-right, blue centre,
#     and red NOT at its mirrored spot.
#
# Not checked here, on purpose: the style features GL has and D3D11/D3D12/Vulkan do not
# (rounded corners, border, gradient, blur/drop shadow, inner shadow, textures, UI
# materials). See docs/widgets-d3d-vulkan-analysis-2026-10-02.md, Befund 3.
#
# Exit code = number of failed checks. Before the fix (tags base/gpre) D3D11/D3D12/Vulkan
# fail; after it (fix/gfix) everything passes.
Add-Type -AssemblyName System.Drawing
$fails = 0
function Near($c, $want, $tol) {
    return [math]::Abs($c[0] - $want[0]) -le $tol -and [math]::Abs($c[1] - $want[1]) -le $tol -and [math]::Abs($c[2] - $want[2]) -le $tol
}
function Mean($bmp, $x0, $x1, $y0, $y1) {
    $r = 0; $g = 0; $b = 0; $n = 0
    for ($y = $y0; $y -lt $y1; $y += 2) { for ($x = $x0; $x -lt $x1; $x += 2) {
        $c = $bmp.GetPixel($x, $y); $r += $c.R; $g += $c.G; $b += $c.B; $n++ } }
    return @([math]::Round($r / $n), [math]::Round($g / $n), [math]::Round($b / $n))
}
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:fails++ }
    '{0,-4} {1,-44} {2}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $detail
}

$tile = @(66, 76, 97)
$editor = @{}
foreach ($rhi in $Rhis) {
    $f = Join-Path $Shots "${EditorTag}_$rhi.bmp"
    if (-not (Test-Path $f)) { Check "editor $rhi" $false "missing $f"; continue }
    $bmp = [System.Drawing.Bitmap]::FromFile($f); $editor[$rhi] = $bmp
    $t = Mean $bmp 70 290 90 180; $m = Mean $bmp 70 290 540 630
    Check "editor $rhi tile0 at true rows" (Near $t $tile 6) "mean=($($t -join ','))"
    Check "editor $rhi tile0 not at mirrored rows" (-not (Near $m $tile 6)) "mean=($($m -join ','))"
}
if ($editor.ContainsKey('Vulkan') -and $editor.ContainsKey('D3D11')) {
    $a = $editor['D3D11']; $v = $editor['Vulkan']; $n = 0; $bad = 0
    for ($y = 0; $y -lt $a.Height; $y += 4) { for ($x = 0; $x -lt $a.Width; $x += 4) {
        $p = $a.GetPixel($x, $y); $q = $v.GetPixel($x, $y); $n++
        if ([math]::Max([math]::Max([math]::Abs($p.R - $q.R), [math]::Abs($p.G - $q.G)), [math]::Abs($p.B - $q.B)) -gt 8) { $bad++ } } }
    $pct = 100.0 * $bad / $n
    Check "editor Vulkan == D3D11 (whole image)" ($pct -lt 1.0) ('{0:N2} % of sampled pixels differ' -f $pct)
}
foreach ($bmp in $editor.Values) { $bmp.Dispose() }

$red = @(229, 25, 25); $green = @(25, 204, 51); $blue = @(25, 76, 229)
foreach ($rhi in $Rhis) {
    $f = Join-Path $Shots "${GameTag}_$rhi.png"
    if (-not (Test-Path $f)) { Check "game $rhi" $false "missing $f"; continue }
    $bmp = [System.Drawing.Bitmap]::FromFile($f); $w = $bmp.Width; $h = $bmp.Height
    function S($x, $y) { $c = $bmp.GetPixel([int]($x / 1280 * $w), [int]($y / 720 * $h)); return @($c.R, $c.G, $c.B) }
    $tl = S 200 110; $tlm = S 200 610; $br = S 1080 610; $c = S 500 360
    Check "game $rhi red top-left" (Near $tl $red 12) "($($tl -join ','))"
    Check "game $rhi red not at mirrored spot" (-not (Near $tlm $red 12)) "($($tlm -join ','))"
    Check "game $rhi green bottom-right" (Near $br $green 12) "($($br -join ','))"
    Check "game $rhi blue centre" (Near $c $blue 12) "($($c -join ','))"
    $bmp.Dispose()
}
"failed checks: $fails"
exit $fails
