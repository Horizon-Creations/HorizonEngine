// M0 spike / demo — exercise the he::shaderc cross-compiler end-to-end:
//   canonical GLSL → SPIR-V (glslang) → MSL / HLSL / GLSL(-ES) (SPIRV-Cross).
// Proves author→compile→cross-compile works; the emitted MSL is validated
// separately via `xcrun metal` (see docs/material-system-design.md M0).

#include "ShaderCompiler.h"

#include <cstdio>

namespace
{
// A trivial "unlit emissive" fragment shader in canonical GLSL — the M0 target
// material. Vulkan/SPIR-V rules require explicit locations on IO.
constexpr const char* kFragGlsl = R"(#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uBaseColor;
layout(set = 0, binding = 1) uniform MatParams {
    vec4 tint;      // rgb tint, a = emissive strength
} mat;

void main()
{
    vec3 base = texture(uBaseColor, vUV).rgb * mat.tint.rgb;
    oColor = vec4(base * mat.tint.a, 1.0);
}
)";
} // namespace

int main()
{
    using namespace he::shaderc;

    const auto mr = compileMany(kFragGlsl, Stage::Fragment,
        { Target::Msl, Target::HlslSm50, Target::Glsl410, Target::GlslEs300 });

    if (!mr.ok)
    {
        std::printf("SPIKE FAILED:\n%s\n", mr.log.c_str());
        return 1;
    }
    std::printf("== GLSL -> SPIR-V OK: %zu words ==\n\n", mr.spirv.size());

    const char* names[] = { "MSL (Metal)", "HLSL (D3D SM5.0)",
                            "GLSL 410 (desktop GL)", "GLSL ES 300" };
    for (size_t i = 0; i < mr.perTarget.size(); ++i)
        std::printf("========== %s ==========\n%s\n", names[i], mr.perTarget[i].source.c_str());

    std::printf("== SPIKE OK: one GLSL source -> SPIR-V -> MSL + HLSL + GLSL + GLSL-ES ==\n");
    return 0;
}
