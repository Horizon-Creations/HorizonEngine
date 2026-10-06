param(
    [Parameter(Mandatory)][string]$Root,     # private build tree with deploy\Editor (see cap.ps1)
    [Parameter(Mandatory)][string]$Rhi,      # D3D11 | D3D12 | Vulkan | OpenGL
    [Parameter(Mandatory)][string[]]$Cases,
    [Parameter(Mandatory)][string[]]$Variants,
    [switch]$Only60                           # references / ghost inputs need f60 only
)
# Thema 134 measurement matrix on Windows (Thema 142, step 2): the cap.ps1 twin of
# run134.sh. Same cases, same camera/motion env, same config templates; one capture
# at a time per deploy (the log next to the exe is shared). Capture names carry the
# backend: <Rhi>_<case>__<variant>_f60.bmp in $Root\cap.
#   run142.ps1 -Root C:\hw142  -Rhi Vulkan -Cases s6,s05 -Variants gtR -Only60
#   run142.ps1 -Root C:\hw142  -Rhi Vulkan -Cases s6,pan6 -Variants ABC,ABCr1,ABCr4
#   run142.ps1 -Root C:\hw142s -Rhi Vulkan -Cases s6,pan6 -Variants stock   # build before PR #86
# then: python ana134.py <cap>\Vulkan_s6__gtR_f60.bmp <cap>\Vulkan_pan6__ABC_f60.bmp <cap>\Vulkan_pan6__ABC_f61.bmp
# (motion cases against the reference of their static twin: pan6/move6 -> s6, cpan6/cmove6 -> c6).
$here = $PSScriptRoot
$CC = @{ HE_DUMP_CAMX = '-3'; HE_DUMP_CAMY = '6'; HE_DUMP_CAMZ = '-6'; HE_DUMP_PITCH = '-50' }   # over the standing row
function Case($cfg, $envs) { @{ cfg = "$here\$cfg"; env = $envs } }
$CASE = @{
    s6     = Case config_r6.json  @{}
    s05    = Case config_r05.json @{}
    c6     = Case config_r6.json  ($CC + @{ HE_DUMP_SHADOWINSTTEST = 'contact' })
    c05    = Case config_r05.json ($CC + @{ HE_DUMP_SHADOWINSTTEST = 'contact' })
    cnear  = Case config_r05.json @{ HE_DUMP_SHADOWINSTTEST = 'contact'; HE_DUMP_CAMX = '-7'; HE_DUMP_CAMY = '2.2'; HE_DUMP_CAMZ = '-9'; HE_DUMP_PITCH = '-35' }
    pan6   = Case config_r6.json  @{ HE_DUMP_PANYAW = '0.25' }
    move6  = Case config_r6.json  @{ HE_DUMP_PANMOVE = '0.03' }
    cpan6  = Case config_r6.json  ($CC + @{ HE_DUMP_SHADOWINSTTEST = 'contact'; HE_DUMP_PANYAW = '0.25' })
    cmove6 = Case config_r6.json  ($CC + @{ HE_DUMP_SHADOWINSTTEST = 'contact'; HE_DUMP_PANMOVE = '0.03' })
    s6old  = Case config_r6.json  @{ HE_DUMP_TOD = '0.345' }
    s6mov  = Case config_r6.json  @{ HE_DUMP_TODSTEP = '0.005' }
    s05old = Case config_r05.json @{ HE_DUMP_TOD = '0.345' }
    s05mov = Case config_r05.json @{ HE_DUMP_TODSTEP = '0.005' }
}
$VAR = @{
    stock = @{}                                # run against the pre-#86 deploy (-Root ...s)
    ABC   = @{}                                # the shipped chain at its defaults (Medium, 2 rays)
    ctl   = @{}                                # second ABC/stock run: the noise floor between runs
    gtR   = @{ HE_GI_REFERENCE = '1' }         # 256 rays, history 0.98, no filter
    ABCr1 = @{ HE_DUMP_GISHADOWRAYS = '1' }    # GI Shadow Quality Low
    ABCr4 = @{ HE_DUMP_GISHADOWRAYS = '4' }    # ... and High
    ABCsw = @{ HE_GI_FORCE_SW = '1' }          # D3D12/Vulkan: software rays on RT hardware
}
foreach ($c in $Cases) {
    if (-not $CASE.ContainsKey($c)) { "unknown case $c"; continue }
    foreach ($v in $Variants) {
        if (-not $VAR.ContainsKey($v)) { "unknown variant $v"; continue }
        foreach ($f in $(if ($Only60) { @(60) } else { @(60, 61) })) {
            $extra = @{}; foreach ($k in $CASE[$c].env.Keys) { $extra[$k] = $CASE[$c].env[$k] }
            foreach ($k in $VAR[$v].Keys) { $extra[$k] = $VAR[$v][$k] }
            & "$here\cap.ps1" -Root $Root -Name "${Rhi}_${c}__${v}_f$f" -Rhi $Rhi -Frames $f -Config $CASE[$c].cfg -Extra $extra
        }
    }
}
