#include "ShaderCompiler.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <spirv_msl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_glsl.hpp>

#include <mutex>

namespace he::shaderc
{
namespace
{
// glslang's process init/teardown is global, not re-entrant — serialise it.
std::mutex g_glslangMutex;

EShLanguage toEsh(Stage s)
{
    switch (s)
    {
        case Stage::Vertex:   return EShLangVertex;
        case Stage::Fragment: return EShLangFragment;
        case Stage::Compute:  return EShLangCompute;
    }
    return EShLangFragment;
}

// GLSL → SPIR-V. Caller holds g_glslangMutex.
bool glslToSpirv(const std::string& glsl, Stage stage,
                 std::vector<uint32_t>& spirvOut, std::string& log)
{
    const EShLanguage esh = toEsh(stage);
    glslang::TShader shader(esh);
    const char* src = glsl.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, esh, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const TBuiltInResource* res = GetDefaultResources();
    const EShMessages msgs = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(res, 100, false, msgs))
    {
        log += "glslang parse: ";
        log += shader.getInfoLog();
        log += '\n';
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs))
    {
        log += "glslang link: ";
        log += program.getInfoLog();
        log += '\n';
        return false;
    }

    spv::SpvBuildLogger spvLog;
    glslang::SpvOptions spvOpts;
    glslang::GlslangToSpv(*program.getIntermediate(esh), spirvOut, &spvLog, &spvOpts);
    const std::string m = spvLog.getAllMessages();
    if (!m.empty()) log += m;
    return !spirvOut.empty();
}

// SPIR-V → textual target via SPIRV-Cross.
bool spirvToSource(const std::vector<uint32_t>& spirv, Target target,
                   std::string& out, std::string& log)
{
    try
    {
        switch (target)
        {
            case Target::Msl:
            {
                spirv_cross::CompilerMSL c(spirv);
                spirv_cross::CompilerMSL::Options o;
                o.platform = spirv_cross::CompilerMSL::Options::macOS;
                o.set_msl_version(2, 3);
                c.set_msl_options(o);
                out = c.compile();
                return true;
            }
            case Target::HlslSm50:
            {
                spirv_cross::CompilerHLSL c(spirv);
                spirv_cross::CompilerHLSL::Options o;
                o.shader_model = 50;
                c.set_hlsl_options(o);
                out = c.compile();
                return true;
            }
            case Target::Glsl410:
            case Target::GlslEs300:
            {
                spirv_cross::CompilerGLSL c(spirv);
                spirv_cross::CompilerGLSL::Options o;
                o.version = (target == Target::Glsl410) ? 410u : 300u;
                o.es      = (target == Target::GlslEs300);
                c.set_common_options(o);
                out = c.compile();
                return true;
            }
            case Target::SpirvBinary:
                return true; // no textual source; spirv already carried in Result
        }
    }
    catch (const std::exception& e)
    {
        log += "SPIRV-Cross: ";
        log += e.what();
        log += '\n';
        return false;
    }
    return false;
}
} // namespace

Result compile(const std::string& glsl, Stage stage, Target target)
{
    Result r;
    std::lock_guard<std::mutex> lock(g_glslangMutex);
    glslang::InitializeProcess();
    if (glslToSpirv(glsl, stage, r.spirv, r.log))
        r.ok = spirvToSource(r.spirv, target, r.source, r.log);
    glslang::FinalizeProcess();
    return r;
}

MultiResult compileMany(const std::string& glsl, Stage stage,
                        const std::vector<Target>& targets)
{
    MultiResult mr;
    std::lock_guard<std::mutex> lock(g_glslangMutex);
    glslang::InitializeProcess();
    if (glslToSpirv(glsl, stage, mr.spirv, mr.log))
    {
        mr.ok = true;
        mr.perTarget.reserve(targets.size());
        for (Target t : targets)
        {
            Result r;
            r.spirv = mr.spirv;
            r.ok = spirvToSource(mr.spirv, t, r.source, r.log);
            mr.ok = mr.ok && r.ok;
            mr.log += r.log;
            mr.perTarget.push_back(std::move(r));
        }
    }
    glslang::FinalizeProcess();
    return mr;
}
} // namespace he::shaderc
