#include "ShaderCompiler.h"
#include <cstdint>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <spirv_msl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_glsl.hpp>

#include <mutex>
#include <Diagnostics/Log.h>

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
                // macOS GL 4.1 core does not expose GL_ARB_shading_language_420pack, so
                // don't emit `layout(binding = N)` on UBOs/samplers — the host binds blocks
                // via glUniformBlockBinding by name instead. (Vertex attribute locations are
                // core GLSL 330+ and stay.)
                o.enable_420pack_extension = false;
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

// A shader that fails to compile is a hard failure with a very specific cause,
// and the caller only ever gets a bool + a log string it may or may not print.
// Report it here so the reason always lands in the engine log.
void reportShaderResult(const char* what, bool ok, const std::string& log, size_t glslBytes)
{
    if (ok)
    {
        if (!log.empty())
            HE_LOG_DEBUG(Shader, "%s succeeded with diagnostics: %s", what, log.c_str());
        return;
    }
    HE_LOG_ERROR(Shader, "%s failed (%zu bytes of GLSL): %s", what, glslBytes,
                 log.empty() ? "no diagnostics from the compiler" : log.c_str());
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
    reportShaderResult("Shader compile", r.ok, r.log, glsl.size());
    return r;
}

Result compileMslPinned(const std::string& glsl, Stage stage,
                        const std::vector<MslPin>& pins,
                        const MslOptions& opts)
{
    Result r;
    std::lock_guard<std::mutex> lock(g_glslangMutex);
    glslang::InitializeProcess();
    if (glslToSpirv(glsl, stage, r.spirv, r.log))
    {
        try
        {
            spirv_cross::CompilerMSL c(r.spirv);
            spirv_cross::CompilerMSL::Options o;
            o.platform = spirv_cross::CompilerMSL::Options::macOS;
            o.set_msl_version(2, 3);
            // subpassInput → [[color(n)]] framebuffer fetch (tile memory on
            // Apple GPUs) instead of an input-attachment texture bind.
            o.use_framebuffer_fetch_subpasses = opts.framebufferFetchSubpasses;
            c.set_msl_options(o);
            for (const auto& [set, binding] : opts.constexprLinearSamplers)
            {
                spirv_cross::MSLConstexprSampler s;
                s.min_filter = spirv_cross::MSL_SAMPLER_FILTER_LINEAR;
                s.mag_filter = spirv_cross::MSL_SAMPLER_FILTER_LINEAR;
                c.remap_constexpr_sampler_by_binding(set, binding, s);
            }
            for (const MslPin& p : pins)
            {
                spirv_cross::MSLResourceBinding b{};
                b.stage = (p.stage == Stage::Vertex)   ? spv::ExecutionModelVertex
                        : (p.stage == Stage::Fragment) ? spv::ExecutionModelFragment
                                                       : spv::ExecutionModelGLCompute;
                b.desc_set    = p.set;
                b.binding     = p.binding;
                b.msl_buffer  = p.mslBuffer;
                b.msl_texture = p.mslBuffer;
                b.msl_sampler = p.mslBuffer;
                c.add_msl_resource_binding(b);
            }
            r.source = c.compile();
            r.ok = true;
        }
        catch (const std::exception& e)
        {
            r.log += "SPIRV-Cross(MSL pinned): ";
            r.log += e.what();
            r.log += '\n';
        }
    }
    glslang::FinalizeProcess();
    reportShaderResult("Pinned-MSL shader compile", r.ok, r.log, glsl.size());
    return r;
}

Result compileHlslPinned(const std::string& glsl, Stage stage,
                         const std::vector<HlslPin>& pins)
{
    Result r;
    std::lock_guard<std::mutex> lock(g_glslangMutex);
    glslang::InitializeProcess();
    if (glslToSpirv(glsl, stage, r.spirv, r.log))
    {
        try
        {
            spirv_cross::CompilerHLSL c(r.spirv);
            spirv_cross::CompilerHLSL::Options o;
            o.shader_model = 50;
            c.set_hlsl_options(o);
            for (const HlslPin& p : pins)
            {
                spirv_cross::HLSLResourceBinding b{};
                b.stage = (p.stage == Stage::Vertex)   ? spv::ExecutionModelVertex
                        : (p.stage == Stage::Fragment) ? spv::ExecutionModelFragment
                                                       : spv::ExecutionModelGLCompute;
                b.desc_set = p.set;
                b.binding  = p.binding;
                // One binding can be several HLSL resources at once (a GLSL
                // sampler2D is an SRV *and* a sampler), so every kind gets the
                // index; only the ones the resource actually has are consumed.
                b.cbv.register_binding     = p.reg;
                b.srv.register_binding     = p.reg;
                b.sampler.register_binding = p.reg;
                b.uav.register_binding     = p.reg;
                c.add_hlsl_resource_binding(b);
            }
            r.source = c.compile();
            r.ok = true;
        }
        catch (const std::exception& e)
        {
            r.log += "SPIRV-Cross(HLSL pinned): ";
            r.log += e.what();
            r.log += '\n';
        }
    }
    glslang::FinalizeProcess();
    reportShaderResult("Pinned-HLSL shader compile", r.ok, r.log, glsl.size());
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
    reportShaderResult("Multi-target shader compile", mr.ok, mr.log, glsl.size());
    return mr;
}
} // namespace he::shaderc
