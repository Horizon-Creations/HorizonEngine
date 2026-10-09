# Thema 150 Schritt 4: Vulkan deferred witnesses on the RTX 4070 (Release, private deploy),
# Vulkan validation layer on (HE_GPU_DEBUG=1). Same scenes and knobs as run12.ps1 (D3D12),
# plus the references on OpenGL and D3D12 from the SAME build, and the tail witnesses
# (TAA, SSAO on a floor, decals, trails, water, instancing).
# Usage: run_vk.ps1 [-Only vk_mt_*,gl_*]   — captures land in C:\hw150\cap (cap.ps1).
param([string[]]$Only = @())
$capScript = Join-Path $PSScriptRoot "cap.ps1"
$base = @{ HE_SKY_TIME = "6.2832"; HE_DUMP_AA = "0"; HE_DUMP_BLOOM = "0"; HE_DUMP_DOF = "0"; HE_DUMP_MOTIONBLUR = "0";
          HE_DUMP_SKYTEST = "1"; HE_DUMP_CLOUDMODE = "0"; HE_DUMP_COVERAGE = "0"; HE_DUMP_TOD = "0.45" }
$ml   = @{ HE_DUMP_MANYLIGHTS = "16"; HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" }
$lsp  = @{ HE_DUMP_LOCALSHADOW = "point"; HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" }
$lss  = @{ HE_DUMP_LOCALSHADOW = "spot";  HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" }
$land = @{ HE_DUMP_LANDSCAPELAYERS = "1"; HE_DUMP_CAMY = "340"; HE_DUMP_CAMZ = "70"; HE_DUMP_PITCH = "-35" }
$gi   = @{ HE_DUMP_MATERIALTEST = "1"; HE_DUMP_GIBLEED = "3"; HE_DUMP_GI = "1" }
# AO needs a floor under the sphere (a sphere alone is AO-blind, lesson Thema 150).
$ao   = @{ HE_DUMP_SSRTEST = "1"; HE_DUMP_SSR = "0"; HE_DUMP_MATERIALTEST = "matte"; HE_DUMP_MATTESTPOS = "-3.5,2.5,-8";
           HE_DUMP_CAMY = "3.5"; HE_DUMP_CAMZ = "6"; HE_DUMP_PITCH = "-12"; HE_DUMP_TOD = "0.26"; HE_DUMP_GI = "0" }
function Cap([string]$name, [string]$rhi, [hashtable]$extra) {
    if ($Only.Count -gt 0 -and -not ($Only | Where-Object { $name -like $_ })) { return }
    $e = $base.Clone()
    foreach ($k in $extra.Keys) { $e[$k] = $extra[$k] }
    & $capScript -Name $name -Rhi $rhi -Env $e
}
foreach ($rp in "0", "1") {
    $vk = @{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = $rp }
    Cap "vk_mt_$rp"   "Vulkan" ($vk + @{ HE_DUMP_MATERIALTEST = "matte" })
    Cap "vk_ml_$rp"   "Vulkan" ($vk + $ml)
    Cap "vk_lsp_$rp"  "Vulkan" ($vk + $lsp)
    Cap "vk_lss_$rp"  "Vulkan" ($vk + $lss)
    Cap "vk_gi_$rp"   "Vulkan" ($vk + $gi)
    Cap "vk_si_$rp"   "Vulkan" ($vk + @{ HE_DUMP_SHADOWINSTTEST = "1" })
    Cap "vk_land_$rp" "Vulkan" ($vk + $land)
    Cap "vk_inst_$rp" "Vulkan" ($vk + @{ HE_DUMP_INSTANCETEST = "1" })
    Cap "vk_taa_$rp"  "Vulkan" ($vk + $gi + @{ HE_DUMP_AA = "3" })
    Cap "vk_ao_$rp"   "Vulkan" ($vk + $ao + @{ HE_DUMP_SSAO = "1" })
    Cap "vk_dec_$rp"  "Vulkan" ($vk + @{ HE_DUMP_DECALTEST = "1" })
    Cap "vk_rope_$rp" "Vulkan" ($vk + @{ HE_DUMP_ROPETEST = "1" })
    Cap "vk_wat_$rp"  "Vulkan" ($vk + @{ HE_DUMP_WATERTEST = "1" })
    # References from the same build (GL: no debug layer; D3D12: debug layer).
    $o = @{ HE_DUMP_RENDERPATH = $rp }
    Cap "gl_mt_$rp"   "OpenGL" ($o + @{ HE_DUMP_MATERIALTEST = "matte" })
    Cap "gl_ml_$rp"   "OpenGL" ($o + $ml)
    Cap "gl_lsp_$rp"  "OpenGL" ($o + $lsp)
    Cap "gl_lss_$rp"  "OpenGL" ($o + $lss)
    Cap "gl_land_$rp" "OpenGL" ($o + $land)
    Cap "gl_si_$rp"   "OpenGL" ($o + @{ HE_DUMP_SHADOWINSTTEST = "1" })
    $d = @{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = $rp }
    Cap "d12_mt_$rp"  "D3D12"  ($d + @{ HE_DUMP_MATERIALTEST = "matte" })
    Cap "d12_ml_$rp"  "D3D12"  ($d + $ml)
    Cap "d12_lsp_$rp" "D3D12"  ($d + $lsp)
    Cap "d12_si_$rp"  "D3D12"  ($d + @{ HE_DUMP_SHADOWINSTTEST = "1" })
}
# Negative controls on Vulkan: the 8-light window instead of the clustered lists,
# GI off under the GI scene, SSAO off on the AO floor.
Cap "vk_ml_nc_1"  "Vulkan" (@{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "1"; HE_FORWARD_CLUSTER = "0" } + $ml)
Cap "vk_ml_nc_0"  "Vulkan" (@{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "0"; HE_FORWARD_CLUSTER = "0" } + $ml)
Cap "vk_gioff_1"  "Vulkan" (@{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "1"; HE_DUMP_MATERIALTEST = "1"; HE_DUMP_GIBLEED = "3"; HE_DUMP_GI = "0" })
Cap "vk_aooff_1"  "Vulkan" (@{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "1"; HE_DUMP_SSAO = "0" } + $ao)
Cap "vk_aooff_0"  "Vulkan" (@{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "0"; HE_DUMP_SSAO = "0" } + $ao)
# G-buffer views (built-in materials), Vulkan vs GL.
foreach ($v in 1..4) {
    Cap "vk_sigb$v" "Vulkan" @{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "1"; HE_DUMP_SHADOWINSTTEST = "1"; HE_DUMP_GBUFFER = "$v" }
    Cap "gl_sigb$v" "OpenGL" @{ HE_DUMP_RENDERPATH = "1"; HE_DUMP_SHADOWINSTTEST = "1"; HE_DUMP_GBUFFER = "$v" }
}
