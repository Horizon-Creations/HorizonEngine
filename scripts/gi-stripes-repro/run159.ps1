param(
    [Parameter(Mandatory)] [string]$Root,     # frozen deploy root: $Root\deploy\Editor
    [Parameter(Mandatory)] [string]$Prefix,   # file prefix, e.g. post / pre / fixA
    [string[]]$Rhis = @('OpenGL', 'Vulkan', 'D3D11', 'D3D12'),
    [string[]]$Cases = @('y300_gi0', 'y300_gi1', 'y0_gi0', 'y0_gi1', 'layers_y300_gi0', 'layers_y300_gi1'),
    [string]$Suffix = ''                      # e.g. _r2 for a run-to-run repeat
)
# Thema 159 S3: the cap159.ps1 matrix for one frozen deploy. Case names match the
# S2 captures (pre2\cap\pre_*): mountain + sphere at y=300 (default) or y=0
# (HE_DUMP_LANDY=0, camera at 6), and the flat graph-material layers terrain.
# Captures land in $Root\cap\<Prefix>_<rhi>_<case><Suffix>.bmp; ana159.py compares them.
$cap = Join-Path $PSScriptRoot 'cap159.ps1'
foreach ($rhi in $Rhis) {
    foreach ($c in $Cases) {
        $gi = [int]($c -match 'gi1')
        $scene = if ($c -like 'layers*') { 'layers' } else { 'mountain' }
        $extra = @{}
        if ($c -match 'y0_') { $extra = @{ HE_DUMP_LANDY = '0'; HE_DUMP_CAMY = '6' } }
        $name = "${Prefix}_$($rhi.ToLower())_$c$Suffix"
        & $cap -Name $name -Rhi $rhi -Gi $gi -Scene $scene -Extra $extra -Root $Root
    }
}
