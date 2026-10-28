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
    // Tessellation (Thema 207). glslang and the SPIR-V/GLSL/MSL targets take them;
    // SPIRV-Cross has no HLSL for them (it throws "Unsupported execution model"),
    // so an HLSL target fails with a log line instead — D3D hull/domain shaders are
    // hand-written shells around a kernel from compileHlslKernel.
    TessControl,
    TessEval,
};

enum class Target
{
    SpirvBinary, // raw SPIR-V words (for Vulkan)
    Msl,         // Metal Shading Language (macOS)
    HlslSm50,    // HLSL, shader model 5.0 (D3D11/12)
    Glsl410,     // desktop GLSL 4.10 (macOS OpenGL core)
    GlslEs300,   // OpenGL ES / WebGL2 GLSL
    Glsl430,     // desktop GLSL 4.30 (GL 4.3+: SSBOs). Uniform blocks and samplers
                 // carry NO binding (bound by name, like Glsl410); storage buffers
                 // keep theirs — see compileGlslPinned.
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
//
// The sampler may be split off from its SRV: a combined GLSL sampler2D is BOTH a
// register(tN) and a register(sN), and only the sampler half is scarce (16 slots
// against 128). The material fragment keeps every texture at its binding number,
// so the renderers' per-draw SRV binds stay readable against the GLSL, and moves
// only the six samplers that sit past s15.
inline constexpr uint32_t kHlslPinSamplerAsReg = ~0u;
struct HlslPin
{
    Stage    stage;     // which shader stage the resource is used in
    uint32_t set;       // GLSL: layout(set = ...)
    uint32_t binding;   // GLSL: layout(binding = ...)
    uint32_t reg;       // target HLSL register index (b/t/u, and s unless `sampler` says otherwise)
    uint32_t sampler = kHlslPinSamplerAsReg; // register(s…) when it differs from reg
};

// Compile canonical GLSL to HLSL SM 5.0 with explicit register assignments
// (Target::HlslSm50).
Result compileHlslPinned(const std::string& glsl, Stage stage,
                         const std::vector<HlslPin>& pins);

// A VERTEX-stage GLSL as a callable HLSL SM 5.0 kernel: the entry point is renamed to
// `entryName` (SPIRV-Cross use_entry_point_name), so the body comes out as
// `void <entryName>_inner()` working on static globals — one per GLSL in/out, named
// as in the GLSL, plus `gl_Position`. A hand-written shader of another stage (the
// D3D domain shell) sets the inputs, calls it and reads the outputs. The wrapper
// entry `<entryName>(SPIRV_Cross_Input)` stays in the text, unused.
Result compileHlslKernel(const std::string& glsl, const std::string& entryName,
                         const std::vector<HlslPin>& pins);

// The GLSL 4.30 counterpart (Target::Glsl430). The GL backend binds uniform blocks
// and samplers by name, so the emitted source drops their `layout(binding = N)`;
// a shader storage block cannot be rebound by name before it compiles, and a
// binding at or past GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS (only 8 guaranteed by
// GL 4.3) is a COMPILE error — so SSBOs keep a binding, the canonical one unless
// a pin moves it. A pinned resource of any kind gets `binding = glBinding`.
struct GlslPin
{
    Stage    stage;     // which shader stage the resource is used in
    uint32_t set;       // GLSL: layout(set = ...)
    uint32_t binding;   // GLSL: layout(binding = ...)
    uint32_t glBinding; // emitted layout(binding = ...)
};

// Compile canonical GLSL to desktop GLSL 4.30 with explicit binding assignments.
Result compileGlslPinned(const std::string& glsl, Stage stage,
                         const std::vector<GlslPin>& pins);

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
