// he::shaderc without glslang and SPIRV-Cross — the build that ships an APPLICATION.
//
// docs/he-apps-plan.md A3b: the cross-compiler's ~5 MB is what the full renderer really
// costs in a packaged binary, not its half megabyte of feature code. A build configured
// with -DHE_ENABLE_SHADERC=OFF links THIS translation unit under the same target name
// (he_shadercompiler) and the same header, so every consumer — MaterialShaderLibrary and
// through it all five renderers — compiles and links unchanged. What changes is the
// answer: every compile fails, at runtime, with a log line saying why.
//
// That is deliberately NOT a compile-time hole. The material path stays in the binary and
// keeps working from the PRECOMPILED variants baked into the .hpak at export time
// (MaterialShaderVariant); only the fallback that would cross-compile at load is gone.
// The alternative — #ifdef-ing the material path out of the renderers — is what left a
// shaderc-free build without a UI material path at all.
#include "ShaderCompiler.h"

namespace he::shaderc
{
namespace
{
// One sentence, always the same, so a log grep finds every one of them. It names the
// switch rather than the symptom: "compile failed" in a build that cannot compile is
// not news, "this build has no shader compiler" is.
constexpr const char* kNoCompiler =
    "he::shaderc: built without the shader cross-compiler (HE_ENABLE_SHADERC=OFF) — "
    "shaders must come precompiled (MaterialShaderVariant); nothing was compiled";

Result failed()
{
    Result r;
    r.ok  = false;
    r.log = kNoCompiler;
    return r;
}
} // namespace

Result compile(const std::string&, Stage, Target) { return failed(); }

Result compileMslPinned(const std::string&, Stage, const std::vector<MslPin>&, const MslOptions&)
{
    return failed();
}

MultiResult compileMany(const std::string&, Stage, const std::vector<Target>& targets)
{
    MultiResult m;
    m.ok  = false;
    m.log = kNoCompiler;
    // Same shape as the real one: one entry per requested target, each not-ok, so a
    // caller that indexes perTarget[i] reads a failure instead of running off the end.
    m.perTarget.resize(targets.size());
    for (auto& r : m.perTarget) r.log = kNoCompiler;
    return m;
}
} // namespace he::shaderc
