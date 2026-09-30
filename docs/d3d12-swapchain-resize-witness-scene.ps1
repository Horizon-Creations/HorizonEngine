param([string]$Scene = "C:/hw112/proj/Depthy/Content/StartupScene.hescene")
# Thema 112 step 2: the depth witness scene for the resize checks. A floor, a
# back wall and eight default cubes (mesh [1,1]) placed so that near cubes
# overlap far ones all over the frame, including the lower right, which at
# 1800x1000 lies outside a 1280x720 start extent: a scene depth left at the
# start size shows there first. Camera (isMain) at (0, 3.2, 10), pitch -12.
# The .heproj beside Content/ is any project file with "name" changed.
$root = @(7700112, 1); $ents = @(); $kids = @(); $script:i = 0
function U { $script:i++; return @(7700112, (1000 + $script:i)) }
function Cube($name, $pos, $rot, $scale) {
    $u = U; $script:kids += , $u
    $script:ents += [ordered]@{ children = @(); components = [ordered]@{
        mesh = [ordered]@{ asset = @(1, 1); castsShadow = $true; receivesShadow = $true; visible = $true; lodBias = 0 }
        transform = [ordered]@{ position = $pos; rotation = $rot; scale = $scale } }; name = $name; parent = $root; uuid = $u }
}
Cube "Floor"        @(0, -0.1, 0)     @(0, 0, 0)   @(40, 0.2, 40)
Cube "BackWall"     @(0, 3, -9)       @(0, 0, 0)   @(40, 6, 0.5)
Cube "FarRight"     @(4.5, 2, -3)     @(0, 20, 0)  @(2.5, 4, 2.5)
Cube "NearRight"    @(3.2, 0.9, 3.5)  @(0, 35, 0)  @(1.6, 1.8, 1.6)
Cube "FarLeft"      @(-4.5, 2, -3)    @(0, -15, 0) @(2.5, 4, 2.5)
Cube "NearLeft"     @(-3.2, 0.9, 3.5) @(0, -30, 0) @(1.6, 1.8, 1.6)
Cube "MidBack"      @(0.8, 1.5, -1)   @(0, 10, 0)  @(2, 3, 2)
Cube "MidFront"     @(-0.4, 0.8, 2)   @(0, 45, 0)  @(1.4, 1.6, 1.4)
Cube "LowNearRight" @(1.9, 0.5, 6.0)  @(0, 15, 0)  @(1.0, 1.0, 1.0)
$cu = U; $kids += , $cu
$ents += [ordered]@{ children = @(); components = [ordered]@{
    camera = [ordered]@{ fovDegrees = 60.0; nearPlane = 0.1; farPlane = 200.0; isMain = $true; orthographic = $false }
    transform = [ordered]@{ position = @(0, 3.2, 10); rotation = @(-12, 0, 0); scale = @(1, 1, 1) } }; name = "MainCamera"; parent = $root; uuid = $cu }
$su = U; $kids += , $su
$ents += [ordered]@{ children = @(); components = [ordered]@{
    environment = [ordered]@{ timeOfDay = 0.38; sunIntensity = 2.2; dayNightCycle = $false; autoAdvance = $false; cloudCoverage = 0.3 } }; name = "Sky"; parent = $root; uuid = $su }
$ents += [ordered]@{ children = $kids; name = "World"; parent = $null; uuid = $root }
[ordered]@{ entities = $ents; version = "1.1" } | ConvertTo-Json -Depth 10 | Out-File -Encoding ascii $Scene
