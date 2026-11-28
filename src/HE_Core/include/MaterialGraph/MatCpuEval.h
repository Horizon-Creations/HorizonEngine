#pragma once
// ── A material graph, evaluated on the CPU ───────────────────────────────────
// The software renderer has no shader compiler and no GPU, so a material on a
// widget used to fall back to its plain surface (D5 "Schicht 0"). This is the
// other half: the graph turned into a flat list of operations once, then run
// per pixel. Not a translation of the generated GLSL — the same graph walked by
// the same rules generateFragment walks it with (pin coercion, unconnected
// defaults, function inlining, static switches, the Divide guard), so the two
// agree on what a node means without sharing a line of text.
//
// Only the nodes a WIDGET can use are offered (matCpuSupports): constants,
// parameters, the element inputs (size, UV, state, SDFs, Backdrop), texture
// samples, arithmetic, logic, Panner, Checker and function calls built from
// those. What needs a mesh, a world or the scene's light (normals, world
// position, Fresnel, weather, wind, landscape, the noises whose GPU hash a CPU
// float cannot reproduce bit for bit) is refused at compile time, by name —
// an application project with Advanced Shader Effects off only offers this set.
#include <MaterialGraph/MaterialGraph.h>
#include <Math/Math.h>
#include <Types/Defines.h>

#include <map>
#include <string>
#include <vector>

namespace HE
{

// Can the CPU evaluate this node type? The editor's node palette for an
// application on the software renderer is exactly this set.
HE_API bool matCpuSupports(MatNodeType type);

// The element under the pixel — the HeUI block of the GPU path.
struct MatCpuElement
{
    glm::vec2 size{ 1.0f, 1.0f };    // the quad in pixels
    glm::vec2 pos{ 0.0f, 0.0f };     // its top-left on the target, pixels
    glm::vec4 radius{ 0.0f };        // corner radii, CSS order
    glm::vec4 state{ 0.0f };         // hovered, pressed, focused, disabled
    glm::vec2 screen{ 1.0f, 1.0f };  // the target in pixels
};

// What a program reads from outside the graph, per pixel.
struct MatCpuHooks
{
    // Texture slot `slot` (MatCpuProgram::textures()[slot]) at `uv`, as the GPU
    // samples it: repeat addressing, an sRGB asset decoded to linear. Null or
    // an unknown slot reads white, like the GPU's unbound heTex0.
    glm::vec4 (*sample)(void* user, int slot, glm::vec2 uv) = nullptr;
    // What was drawn behind the element, blurred by `radius` pixels, at `uv`
    // across the whole target (0..1, top-left origin). Null reads black.
    glm::vec3 (*backdrop)(void* user, glm::vec2 uv, float radius) = nullptr;
    void* user = nullptr;
};

class HE_API MatCpuProgram
{
public:
    // Compile `graph` (an Output node and what feeds it). False when a node on
    // the way is not CPU-evaluable; `error` then names the first one. Static
    // switches resolve like generateFragment's (override map, then default).
    bool compile(const MaterialGraph& graph, const MatFunctionLoader& loader = {},
                 const std::map<std::string, bool>* switchOverrides = nullptr,
                 std::string* error = nullptr);

    // Parameter values by NAME — the material asset's graphParamNames with its
    // shaderParamData (4 floats per name). A name not listed keeps the node's
    // authored default, as an over-budget parameter does on the GPU.
    void bindParams(const std::vector<std::string>& names, const std::vector<float>& data);

    // One pixel. `uv` is 0..1 across the element (top-left origin), `color` the
    // quad's colour (the Vertex Color node). Returns the GPU's oColor: rgb =
    // BaseColor + Emissive, a = Opacity (Translucent) or 1. `discard` is set
    // when a Masked material's mask falls under its cutoff.
    glm::vec4 eval(const MatCpuElement& e, glm::vec2 uv, const glm::vec3& color, float time,
                   const MatCpuHooks& hooks, bool& discard) const;

    bool valid() const { return m_valid; }
    // Texture paths in slot order, as the nodes name them.
    const std::vector<std::string>& textures() const { return m_textures; }
    bool usesTime()     const { return m_usesTime; }
    bool usesBackdrop() const { return m_usesBackdrop; }

    // ── Implementation (public so the compiler in the .cpp can fill it) ─────
    enum class Op : uint8_t
    {
        Const, VColor, UV, Time, Param, ElemSize, ElemUV, BorderDist, ElemState,
        Extract, Coerce,
        Add, Sub, Mul, Div, Lerp, OneMinus, Power, Saturate, Dot, Sine, Combine3,
        Abs, Fract, Smoothstep, Step, Normalize3, CombineRGBA, Select,
        Greater, Less, GreaterEq, LessEq, Equal, NotEqual, And, Or, Not,
        Panner, Checker, TexSample, RRectSDF, Mask, Backdrop, Copy,
    };
    struct Instr
    {
        Op op;
        int dst = 0, a = -1, b = -1, c = -1, d = -1;
        glm::vec4 k{ 0.0f };   // constants, UV tiling, param defaults
        int aux = 0;           // component / slot / param index / coerce types
    };
    struct ParamRef { std::string name; int kind = 0; glm::vec4 value{ 0.0f }; };

    std::vector<Instr>    m_code;
    std::vector<ParamRef> m_params;
    std::vector<std::string> m_textures;
    int  m_regs = 0;
    int  m_base = -1, m_emis = -1, m_opacity = -1, m_mask = -1;
    float m_cutoff = 0.5f;
    bool m_valid = false, m_usesTime = false, m_usesBackdrop = false;
};

} // namespace HE
