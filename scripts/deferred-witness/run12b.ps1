# Thema 150 Schritt 3, Nachtrag: TAA, SSAO und Decals im D3D12-Deferred-Frame (Debug-Layer an).
$base = @{ HE_SKY_TIME = "6.2832"; HE_DUMP_BLOOM = "0"; HE_DUMP_DOF = "0"; HE_DUMP_MOTIONBLUR = "0";
           HE_DUMP_SKYTEST = "1"; HE_DUMP_CLOUDMODE = "0"; HE_DUMP_COVERAGE = "0"; HE_GPU_DEBUG = "1" }
function Cap([string]$name, [hashtable]$extra) {
    $e = $base.Clone()
    foreach ($k in $extra.Keys) { $e[$k] = $extra[$k] }
    & C:\hw150\cap.ps1 -Name $name -Rhi "D3D12" -Env $e
}
foreach ($rp in "0", "1") {
    Cap "d12_taa_$rp" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_AA = "3"; HE_DUMP_TOD = "0.45"; HE_DUMP_MATERIALTEST = "1"; HE_DUMP_GIBLEED = "3"; HE_DUMP_GI = "1" }
    Cap "d12_ao_$rp"  @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_AA = "0"; HE_DUMP_TOD = "0.26"; HE_DUMP_MATERIALTEST = "1"; HE_DUMP_GIBLEED = "3"; HE_DUMP_GI = "0"; HE_DUMP_SSAO = "1" }
    Cap "d12_dec_$rp" @{ HE_DUMP_RENDERPATH = $rp; HE_DUMP_AA = "0"; HE_DUMP_TOD = "0.45"; HE_DUMP_DECALTEST = "1" }
}
# SSAO control: deferred without SSAO (the AO slot must matter).
Cap "d12_aooff_1" @{ HE_DUMP_RENDERPATH = "1"; HE_DUMP_AA = "0"; HE_DUMP_TOD = "0.26"; HE_DUMP_MATERIALTEST = "1"; HE_DUMP_GIBLEED = "3"; HE_DUMP_GI = "0"; HE_DUMP_SSAO = "0" }
