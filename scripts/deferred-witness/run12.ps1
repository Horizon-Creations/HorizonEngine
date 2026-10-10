# Thema 150 Schritt 3: D3D12 deferred witnesses on the RTX 4070 (Release, private deploy).
# Every run: frozen sky clock, AA/bloom/DOF/motion blur off, D3D12 debug layer on.
param([string[]]$Only = @())
$base = @{ HE_SKY_TIME = "6.2832"; HE_DUMP_AA = "0"; HE_DUMP_BLOOM = "0"; HE_DUMP_DOF = "0"; HE_DUMP_MOTIONBLUR = "0";
          HE_DUMP_SKYTEST = "1"; HE_DUMP_CLOUDMODE = "0"; HE_DUMP_COVERAGE = "0"; HE_DUMP_TOD = "0.45" }
$ml = @{ HE_DUMP_MANYLIGHTS = "16"; HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" }
function Cap([string]$name, [string]$rhi, [hashtable]$extra) {
    if ($Only.Count -gt 0 -and -not ($Only | Where-Object { $name -like $_ })) { return }
    $e = $base.Clone()
    foreach ($k in $extra.Keys) { $e[$k] = $extra[$k] }
    & C:\hw150\cap.ps1 -Name $name -Rhi $rhi -Env $e
}
foreach ($rp in "0", "1") {
    $dbg = @{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = $rp }
    Cap "d12_mt_$rp"  "D3D12" ($dbg + @{ HE_DUMP_MATERIALTEST = "matte" })
    Cap "d12_ml_$rp"  "D3D12" ($dbg + $ml)
    Cap "d12_lsp_$rp" "D3D12" ($dbg + @{ HE_DUMP_LOCALSHADOW = "point"; HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" })
    Cap "d12_lss_$rp" "D3D12" ($dbg + @{ HE_DUMP_LOCALSHADOW = "spot";  HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" })
    Cap "d12_gi_$rp"  "D3D12" ($dbg + @{ HE_DUMP_MATERIALTEST = "1"; HE_DUMP_GIBLEED = "3"; HE_DUMP_GI = "1" })
    Cap "d12_si_$rp"  "D3D12" ($dbg + @{ HE_DUMP_SHADOWINSTTEST = "1" })
    Cap "d12_land_$rp" "D3D12" ($dbg + @{ HE_DUMP_LANDSCAPELAYERS = "1"; HE_DUMP_CAMY = "340"; HE_DUMP_CAMZ = "70"; HE_DUMP_PITCH = "-35" })
    # References on the other backends, same build, same knobs (no debug layer).
    Cap "gl_mt_$rp"   "OpenGL" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_MATERIALTEST = "matte" }
    Cap "d11_mt_$rp"  "D3D11"  @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_MATERIALTEST = "matte" }
    Cap "gl_lsp_$rp"  "OpenGL" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_LOCALSHADOW = "point"; HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" }
    Cap "gl_lss_$rp"  "OpenGL" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_LOCALSHADOW = "spot"; HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" }
    Cap "d11_lsp_$rp" "D3D11"  @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_LOCALSHADOW = "point"; HE_DUMP_TOD = "0"; HE_DUMP_CAMY = "207"; HE_DUMP_CAMZ = "2"; HE_DUMP_PITCH = "-38" }
    Cap "gl_land_$rp" "OpenGL" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_LANDSCAPELAYERS = "1"; HE_DUMP_CAMY = "340"; HE_DUMP_CAMZ = "70"; HE_DUMP_PITCH = "-35" }
    Cap "d11_land_$rp" "D3D11" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_LANDSCAPELAYERS = "1"; HE_DUMP_CAMY = "340"; HE_DUMP_CAMZ = "70"; HE_DUMP_PITCH = "-35" }
    Cap "gl_si_$rp"   "OpenGL" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_SHADOWINSTTEST = "1" }
    Cap "gl_ml_$rp"   "OpenGL" (@{ HE_DUMP_RENDERPATH = $rp } + $ml)
    Cap "d11_ml_$rp"  "D3D11"  (@{ HE_DUMP_RENDERPATH = $rp } + $ml)
}
# Negative control: 8-light window instead of the clustered lists (forward and resolve).
Cap "d12_ml_nc_1" "D3D12" (@{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "1"; HE_FORWARD_CLUSTER = "0" } + $ml)
Cap "d12_ml_nc_0" "D3D12" (@{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "0"; HE_FORWARD_CLUSTER = "0" } + $ml)
# GI off control for the GI scene (deferred).
Cap "d12_gioff_1" "D3D12" @{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "1"; HE_DUMP_MATERIALTEST = "1"; HE_DUMP_GIBLEED = "3"; HE_DUMP_GI = "0" }
# G-buffer views (built-in materials), D3D12 vs GL.
foreach ($v in 1..4) {
    Cap "d12_sigb$v" "D3D12" @{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = "1"; HE_DUMP_SHADOWINSTTEST = "1"; HE_DUMP_GBUFFER = "$v" }
    Cap "gl_sigb$v"  "OpenGL" @{ HE_DUMP_RENDERPATH = "1"; HE_DUMP_SHADOWINSTTEST = "1"; HE_DUMP_GBUFFER = "$v" }
}
