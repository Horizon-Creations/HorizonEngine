// M0 spike — prove the cross-backend shader chain end-to-end:
//   canonical GLSL → glslang → SPIR-V → SPIRV-Cross → MSL / HLSL / GLSL(desktop).
// This is a throwaway feasibility harness for docs/material-system-design.md M0.
// It does NOT touch the renderer; it only proves author→compile→cross-compile works
// and (via `xcrun metal`, run separately) that the emitted MSL is valid.

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <spirv_msl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_glsl.hpp>

#include <cstdio>
#include <string>
#include <vector>

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

bool compileToSpirv(const char* glsl, EShLanguage stage, std::vector<uint32_t>& spirvOut)
{
    glslang::TShader shader(stage);
    shader.setStrings(&glsl, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const TBuiltInResource* resources = GetDefaultResources();
    const EShMessages messages =
        static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(resources, 100, false, messages))
    {
        std::printf("[glslang] parse failed:\n%s\n%s\n",
                    shader.getInfoLog(), shader.getInfoDebugLog());
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
    {
        std::printf("[glslang] link failed:\n%s\n", program.getInfoLog());
        return false;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions spvOptions;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirvOut, &logger, &spvOptions);
    const std::string msgs = logger.getAllMessages();
    if (!msgs.empty())
        std::printf("[glslang→spv] %s\n", msgs.c_str());
    return !spirvOut.empty();
}

std::string crossToMsl(const std::vector<uint32_t>& spirv)
{
    spirv_cross::CompilerMSL c(spirv);
    spirv_cross::CompilerMSL::Options o;
    o.platform = spirv_cross::CompilerMSL::Options::macOS;
    o.set_msl_version(2, 3);
    c.set_msl_options(o);
    return c.compile();
}

std::string crossToHlsl(const std::vector<uint32_t>& spirv)
{
    spirv_cross::CompilerHLSL c(spirv);
    spirv_cross::CompilerHLSL::Options o;
    o.shader_model = 50; // SM 5.0 (D3D11)
    c.set_hlsl_options(o);
    return c.compile();
}

std::string crossToGlsl(const std::vector<uint32_t>& spirv, uint32_t version, bool es)
{
    spirv_cross::CompilerGLSL c(spirv);
    spirv_cross::CompilerGLSL::Options o;
    o.version = version;
    o.es = es;
    c.set_common_options(o);
    return c.compile();
}
} // namespace

int main()
{
    glslang::InitializeProcess();

    std::vector<uint32_t> spirv;
    if (!compileToSpirv(kFragGlsl, EShLangFragment, spirv))
    {
        std::printf("SPIKE FAILED: GLSL → SPIR-V\n");
        glslang::FinalizeProcess();
        return 1;
    }
    std::printf("== GLSL → SPIR-V OK: %zu words ==\n\n", spirv.size());

    try
    {
        std::printf("========== MSL (Metal) ==========\n%s\n", crossToMsl(spirv).c_str());
        std::printf("========== HLSL (D3D SM5.0) ==========\n%s\n", crossToHlsl(spirv).c_str());
        std::printf("========== GLSL 410 (desktop GL) ==========\n%s\n",
                    crossToGlsl(spirv, 410, false).c_str());
        std::printf("========== GLSL ES 300 ==========\n%s\n",
                    crossToGlsl(spirv, 300, true).c_str());
    }
    catch (const std::exception& e)
    {
        std::printf("SPIKE FAILED: SPIRV-Cross threw: %s\n", e.what());
        glslang::FinalizeProcess();
        return 2;
    }

    glslang::FinalizeProcess();
    std::printf("== SPIKE OK: one GLSL source → SPIR-V → MSL + HLSL + GLSL + GLSL-ES ==\n");
    return 0;
}
