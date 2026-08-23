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

// ─── HLSL register pinning — the D3D twin of MslPin ──────────────────────────
// SPIRV-Cross maps GLSL `binding = N` straight onto `register(tN)`, `register(sN)`
// and `register(bN)`. Shader model 5.0 offers only s0..s15 and b0..b13, so every
// binding above those limits is simply unreachable — the emitted HLSL looks fine
// and FXC then rejects it with X4509 (sampler) or X4567 (cbuffer).
//
// That is not hypothetical: it is why node-graph materials never rendered as
// authored on D3D11/D3D12 (the shared lighting preamble reaches binding 33), and
// why three of the four SSR shaders would not build there either. Metal hit the
// same wall and was given MslPin; the HLSL side was given nothing, and because the
// failure happens one step AFTER the cross-compile, `Result::ok` stayed true and
// hid it.
//
// A pin left at kHlslPinUnused keeps SPIRV-Cross's own choice for that register
// class, so a table only has to name what actually has to move.
inline constexpr uint32_t kHlslPinUnused = 0xFFFFFFFFu;

struct HlslPin
{
    Stage    stage;                    // which stage the resource is used in
    uint32_t set     = 0;              // GLSL: layout(set = ...)
    uint32_t binding = 0;              // GLSL: layout(binding = ...)
    uint32_t srv     = kHlslPinUnused; // → register(t…)  textures / SRVs
    uint32_t sampler = kHlslPinUnused; // → register(s…)  0..15 on SM 5.0
    uint32_t cbv     = kHlslPinUnused; // → register(b…)  0..13 on SM 5.0
};

// Compile canonical GLSL to HLSL SM 5.0 with explicit register assignments.
// `unusedPins` (optional) receives the pins SPIRV-Cross never consumed, which is
// how a caller notices its table has drifted from the shader — a pin for a binding
// the shader dropped is silently ignored otherwise.
Result compileHlslPinned(const std::string& glsl, Stage stage,
                         const std::vector<HlslPin>& pins,
                         std::vector<HlslPin>* unusedPins = nullptr);

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
