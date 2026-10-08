param(
    [string[]]$Roots = @('C:\hw146\pre', 'C:\hw146\post'),  # each holds deploy\Editor
    [string[]]$Rhis  = @('Vulkan'),
    [string]$Step    = '0.02',   # day fraction the sun jumps for the captured frame
    [int]$Frames     = 30
)
# CSM sun-lag witness (Thema 146). GI off, so the sun-edge shadows are the CSM
# cascades; every temporal pass off, so nothing from the settle frames bleeds
# into the capture except what the renderer itself carries over. Per root/RHI:
#   S    static at TOD 0.35         S2   the same again (noise floor)
#   S0   static at TOD 0.35 - Step  P1   settled at 0.35 - Step, ONE frame at 0.35
#                                        (HE_DUMP_TODSTEPFRAMES=1)
# A pass that extracts with the previous frame's sun puts P1's shadow edges
# where S0 has them; without the lag P1 matches S within S/S2. ana146.py does
# the numbers. cap.ps1 runs each capture with a fresh %APPDATA%.
$cap = Join-Path $PSScriptRoot 'cap.ps1'
$common = @{ HE_DUMP_SSAO = '0'; HE_DUMP_AA = '0'; HE_DUMP_MOTIONBLUR = '0'; HE_DUMP_DOF = '0';
             HE_DUMP_BLOOM = '0'; HE_DUMP_SSR = '0'; HE_DUMP_RENDERPATH = '0' }
$tod0 = ([double]0.35 - [double]$Step).ToString([Globalization.CultureInfo]::InvariantCulture)
foreach ($root in $Roots) {
    foreach ($rhi in $Rhis) {
        $r = $rhi.ToLower()
        $cases = [ordered]@{
            "${r}_S"  = @{}
            "${r}_S2" = @{}
            "${r}_S0" = @{ HE_DUMP_TOD = $tod0 }
            "${r}_P1" = @{ HE_DUMP_TODSTEP = $Step; HE_DUMP_TODSTEPFRAMES = '1' }
        }
        foreach ($name in $cases.Keys) {
            $extra = $common.Clone()
            foreach ($k in $cases[$name].Keys) { $extra[$k] = $cases[$name][$k] }
            & $cap -Name $name -Rhi $rhi -Gi 0 -Frames $Frames -Extra $extra -Root $root
        }
    }
}
