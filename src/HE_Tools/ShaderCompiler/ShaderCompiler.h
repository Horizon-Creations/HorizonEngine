// HorizonEngine shader cross-compiler — canonical GLSL → SPIR-V → per-backend source.
//
// This is the M0/M1 backbone of the cross-backend material system
// (docs/material-system-design.md): author ONE canonical GLSL source, compile to
// SPIR-V via glslang, then emit MSL / HLSL / GLSL(-ES) via SPIRV-Cross, or hand the
// SPIR-V straight to Vulkan. The runtime renderers and the offline cook tool both
// link this library.
//
// The header intentionally leaks NO glslang / spirv_cross types, so consumers need
// only this one include.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace he::shaderc
{
enum class Stage
{
    Vertex,
    Fragment,
    Compute,
};

enum class Target
{
    SpirvBinary, // raw SPIR-V words (for Vulkan)
    Msl,         // Metal Shading Language (macOS)
    HlslSm50,    // HLSL, shader model 5.0 (D3D11/12)
    Glsl410,     // desktop GLSL 4.10 (macOS OpenGL core)
    GlslEs300,   // OpenGL ES / WebGL2 GLSL
};

struct Result
{
    bool                  ok = false;
    std::string           source;  // emitted textual source (empty for SpirvBinary)
    std::vector<uint32_t> spirv;   // populated for every target (the intermediate)
    std::string           log;     // diagnostics; non-empty on failure
};

// Compile canonical GLSL (Vulkan semantics, #version 450+) to the requested target.
// Thread-safe: serialises glslang process init/teardown internally.
Result compile(const std::string& glsl, Stage stage, Target target);

// Pin a GLSL resource (descriptor set + binding) to an explicit MSL buffer slot, so a
// cross-compiled shader can be a DROP-IN for a hand-written Metal pipeline whose bind
// points are fixed (e.g. vertex buffer at [[buffer(0)]], uniforms at [[buffer(1)]]).
// Without pinning, SPIRV-Cross auto-assigns MSL slots and the indices may not match
// what the renderer binds.
struct MslPin
{
    Stage    stage;       // which shader stage the resource is used in
    uint32_t set;         // GLSL: layout(set = ...)
    uint32_t binding;     // GLSL: layout(binding = ...)
    uint32_t mslBuffer;   // target MSL [[buffer(n)]]
};

// Compile canonical GLSL to MSL with explicit buffer-slot assignments (Target::Msl).
Result compileMslPinned(const std::string& glsl, Stage stage,
                        const std::vector<MslPin>& pins);

// Convenience: compile once to SPIR-V, then emit several targets from it (cheaper
// than re-parsing the GLSL per target). Returns SPIR-V + a source per requested target,
// in the same order as `targets`. `out[i].ok == false` on a per-target failure.
struct MultiResult
{
    bool                  ok = false;
    std::vector<uint32_t> spirv;
    std::vector<Result>   perTarget;
    std::string           log;
};
MultiResult compileMany(const std::string& glsl, Stage stage,
                        const std::vector<Target>& targets);
} // namespace he::shaderc
