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

// Extra MSL emission options. framebufferFetchSubpasses turns GLSL subpassInput
// declarations into Metal [[color(input_attachment_index)]] framebuffer-fetch
// reads (Apple-GPU tile memory; requires MSL 2.3 on macOS, which the compiler
// already targets) — the deferred renderer's single-pass lighting resolve reads
// the G-buffer from tile storage this way instead of sampling stored textures.
struct MslOptions
{
    bool framebufferFetchSubpasses = false;
    // Combined samplers (set, binding) to emit as INLINE constexpr samplers
    // (linear filter, clamp-to-edge) instead of a [[sampler(n)]] argument.
    // Frees a sampler slot when the fragment stage sits at Metal's 16-sampler
    // cap — the texture still needs (and gets) its [[texture(n)]] pin.
    std::vector<std::pair<uint32_t, uint32_t>> constexprLinearSamplers;
};

// Compile canonical GLSL to MSL with explicit buffer-slot assignments (Target::Msl).
Result compileMslPinned(const std::string& glsl, Stage stage,
                        const std::vector<MslPin>& pins,
                        const MslOptions& opts = {});

// The HLSL counterpart of MslPin. Without it SPIRV-Cross turns `layout(binding = N)`
// straight into `register(bN/tN/sN)`, and the canonical GLSL of this engine uses
// binding numbers up to 33 — past D3D11's hard API limits of 14 constant-buffer and
// 16 sampler slots per stage. A shader that a hand-written D3D11 pipeline has to bind
// therefore needs its high bindings pinned down into range (SRVs are fine either way,
// 128 slots, but they are pinned along with the sampler so the pair stays together).
// D3D12 has no such limit; it uses the same pins so both backends share one contract.
struct HlslPin
{
    Stage    stage;     // which shader stage the resource is used in
    uint32_t set;       // GLSL: layout(set = ...)
    uint32_t binding;   // GLSL: layout(binding = ...)
    uint32_t reg;       // target HLSL register index (b/t/s all get this index)
};

// Compile canonical GLSL to HLSL SM 5.0 with explicit register assignments
// (Target::HlslSm50).
Result compileHlslPinned(const std::string& glsl, Stage stage,
                         const std::vector<HlslPin>& pins);

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
