param(
    [string[]]$Rhis = @('OpenGL', 'D3D11', 'D3D12', 'Vulkan'),
    [string[]]$Builds = @('post', 'ctl'),   # C:\hw148\<build>\Editor; ctl = gate forced to 0
    [string]$Root = 'C:\hw148',
    [string]$Tag = ''                        # suffix for a repeat run (noise floor)
)
# Thema 148 matrix: per backend and build, the GIBLEED pair under the graph sphere (3/4),
# the pair under the built-in control sphere (1/2), each GI on/off, plus the painted
# terrain GI on/off. Names: <build>_<rhi>_<scene>_gi<0|1><tag>.bmp in $Root\cap.
$cap = Join-Path $PSScriptRoot 'cap148.ps1'
foreach ($b in $Builds) {
    foreach ($r in $Rhis) {
        $rs = $r.ToLower()
        foreach ($gi in 1, 0) {
            foreach ($bl in '3', '4', '1', '2') {
                & $cap -Name "${b}_${rs}_b${bl}_gi${gi}$Tag" -Rhi $r -Gi $gi -Scene bleed -Bleed $bl -Deploy "$Root\$b"
            }
            & $cap -Name "${b}_${rs}_terrain_gi${gi}$Tag" -Rhi $r -Gi $gi -Scene terrain -Deploy "$Root\$b"
        }
    }
}
