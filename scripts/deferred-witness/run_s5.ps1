# Thema 150 Schritt 5: the extra passes in the deferred frame on D3D11, D3D12 and
# Vulkan (RTX 4070, Release, private deploy, debug/validation layer on where it
# exists). Captures land in C:\hw150\cap (cap.ps1).
#   ssr  : SSRTEST floor + wall, chrome graph ball, SSR on/off, forward/deferred
#   dec  : DECALTEST at dusk under a point light (self-lit vs resolve-lit decal)
#   post : MATERIALTEST + bloom + SMAA, forward/deferred
# Usage: run_s5.ps1 [-Only d11_ssr_*,vk_*]
param([string[]]$Only = @())
$capScript = Join-Path $PSScriptRoot "cap.ps1"
$base = @{ HE_SKY_TIME = "6.2832"; HE_DUMP_AA = "0"; HE_DUMP_BLOOM = "0"; HE_DUMP_DOF = "0"; HE_DUMP_MOTIONBLUR = "0";
          HE_DUMP_SKYTEST = "1"; HE_DUMP_CLOUDMODE = "0"; HE_DUMP_COVERAGE = "0"; HE_DUMP_TOD = "0.45" }
$ssr  = @{ HE_DUMP_SSRTEST = "1"; HE_DUMP_MATERIALTEST = "chrome"; HE_DUMP_MATTESTPOS = "-3.5,2.5,-8";
           HE_DUMP_CAMY = "3.5"; HE_DUMP_CAMZ = "6"; HE_DUMP_PITCH = "-12"; HE_DUMP_GI = "0"; HE_DUMP_SSAO = "0" }
function Cap([string]$name, [string]$rhi, [hashtable]$extra) {
    if ($Only.Count -gt 0 -and -not ($Only | Where-Object { $name -like $_ })) { return }
    $e = $base.Clone()
    foreach ($k in $extra.Keys) { $e[$k] = $extra[$k] }
    & $capScript -Name $name -Rhi $rhi -Env $e
}
$backs = @(@("d11", "D3D11"), @("d12", "D3D12"), @("vk", "Vulkan"))
foreach ($b in $backs) {
    $tag = $b[0]; $rhi = $b[1]
    foreach ($rp in "0", "1") {
        $o = @{ HE_GPU_DEBUG = "1"; HE_DUMP_RENDERPATH = $rp }
        Cap "${tag}_ssr_$rp"    $rhi ($o + $ssr + @{ HE_DUMP_SSR = "1" })
        Cap "${tag}_ssroff_$rp" $rhi ($o + $ssr + @{ HE_DUMP_SSR = "0" })
        # Quality 2 reads the trace history: a moving camera so the history is used.
        Cap "${tag}_ssrq2_$rp"  $rhi ($o + $ssr + @{ HE_DUMP_SSR = "1"; HE_DUMP_SSRQUALITY = "2"; HE_DUMP_MBYAWSTEP = "0.3" })
    }
}
# OpenGL references (no SSR in GL deferred, plan 10.2).
foreach ($rp in "0", "1") {
    Cap "gl_ssr_$rp" "OpenGL" (@{ HE_DUMP_RENDERPATH = $rp } + $ssr + @{ HE_DUMP_SSR = "1" })
}
