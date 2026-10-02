param([string]$Scene = "C:/hw133/proj/UiWit/Content/StartupScene.hescene")
# Thema 133 step 1: the game-path witness for "widgets invisible on D3D/Vulkan".
# A floor, a main camera and a 1280x720 UI canvas with three solid button
# quads (entity UI -> UISystem::extract -> RenderExtractor::extractUI, the same
# feed the WidgetManager widgets go through). Red top-left, green bottom-right,
# blue centre with a text label — a vertical flip shows as red/green swapping
# rows, a missing feed as no quad at all. No game logic needed.
# The .heproj beside Content/ is any project file with "name" changed.
$root = @(7700133, 1); $ents = @(); $kids = @(); $script:i = 0
function U { $script:i++; return @(7700133, (1000 + $script:i)) }
$fu = U; $kids += , $fu
$ents += [ordered]@{ children = @(); components = [ordered]@{
    mesh = [ordered]@{ asset = @(1, 1); castsShadow = $true; receivesShadow = $true; visible = $true; lodBias = 0 }
    transform = [ordered]@{ position = @(0, -0.1, 0); rotation = @(0, 0, 0); scale = @(40, 0.2, 40) } }; name = "Floor"; parent = $root; uuid = $fu }
$cu = U; $kids += , $cu
$ents += [ordered]@{ children = @(); components = [ordered]@{
    camera = [ordered]@{ fovDegrees = 60.0; nearPlane = 0.1; farPlane = 200.0; isMain = $true; orthographic = $false }
    transform = [ordered]@{ position = @(0, 3.2, 10); rotation = @(-12, 0, 0); scale = @(1, 1, 1) } }; name = "MainCamera"; parent = $root; uuid = $cu }
$su = U; $kids += , $su
$ents += [ordered]@{ children = @(); components = [ordered]@{
    environment = [ordered]@{ timeOfDay = 0.38; sunIntensity = 2.2; dayNightCycle = $false; autoAdvance = $false; cloudCoverage = 0.3 } }; name = "Sky"; parent = $root; uuid = $su }

$canvas = U; $kids += , $canvas; $ckids = @()
function Btn($name, $anchor, $pos, $pivot, $size, $col, $text) {
    $u = U; $script:ckids += , $u
    $c = [ordered]@{
        uielement = [ordered]@{ position = $pos; size = $size; pivot = $pivot; rotation = 0.0; anchor = $anchor; layer = 0; active = $true }
        uibutton  = [ordered]@{ normalColor = $col; hoveredColor = $col; pressedColor = $col; onClickFunction = "" } }
    if ($text) { $c.uitext = [ordered]@{ text = $text; fontSize = 40.0; color = @(1, 1, 1, 1) } }
    $script:ents += [ordered]@{ children = @(); components = $c; name = $name; parent = $canvas; uuid = $u }
}
Btn "RedTopLeft"      0 @(40, 40)   @(0, 0)     @(320, 140) @(0.9, 0.1, 0.1, 1) $null
Btn "GreenBotRight"   8 @(-40, -40) @(1, 1)     @(320, 140) @(0.1, 0.8, 0.2, 1) $null
Btn "BlueCentre"      4 @(0, 0)     @(0.5, 0.5) @(360, 120) @(0.1, 0.3, 0.9, 1) "WIDGET"
$ents += [ordered]@{ children = $ckids; components = [ordered]@{
    uicanvas = [ordered]@{ width = 1280.0; height = 720.0; renderMode = 0; active = $true } }; name = "Canvas"; parent = $root; uuid = $canvas }
$ents += [ordered]@{ children = $kids; name = "World"; parent = $null; uuid = $root }
[ordered]@{ entities = $ents; version = "1.1" } | ConvertTo-Json -Depth 10 | Out-File -Encoding ascii $Scene
