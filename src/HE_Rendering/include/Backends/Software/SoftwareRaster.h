#pragma once
#include <Renderer/UIRenderObject.h>

namespace HE { class MatCpuProgram; }

#include <cstdint>
#include <vector>

// ── Drawing the UI without a GPU ──────────────────────────────────────────────
// A CPU rasterizer for the engine's UI vocabulary, and for nothing else. This is
// the half of Block G (docs/he-apps-plan.md) that has no window, no SDL and no
// IRenderer in it: quads in, pixels out. SoftwareRenderer is the thin shell that
// gets the quads from the extractor and puts the pixels in a window.
//
// Split that way ON PURPOSE. "Schicht 0" — rounded corners, borders, gradients,
// shadows — could until now only be checked by looking at a screenshot from a
// real GPU, which needs a machine with a display and a person with eyes. A
// rasterizer that is a plain function is a rasterizer a test can call, so the
// same style vocabulary becomes assertable in ctest.
//
// What it is NOT: a pixel-for-pixel match of Metal or OpenGL. Blending is
// straight source-over in 8-bit with no gamma correction, the same honest
// divergence tests/ImGuiSoftwareRaster.h documents. What it IS: the same
// fragment MODEL, ported literally from the shaders — one function per pixel
// with the same branches in the same order, so a change to the look happens in
// three places or in none.
//
// Materials: a quad with a MATERIAL is shaded by the graph run on the CPU
// (HE::MatCpuProgram) when every node in it is one the CPU evaluates — the
// widget node set an application with Advanced Shader Effects off is offered.
// A material that is not (a node that needs a mesh or a world) still falls
// back to Schicht 0 plus its tint: the D5 contract for what cannot be drawn.
namespace HE::sw
{

// One rendered frame, straight RGBA8, row-major from the top-left.
struct Image
{
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> rgba;   // width * height * 4

    bool valid() const
    {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
    void resize(int w, int h);
    void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
    // The same, limited to one rectangle — what a partial redraw starts with.
    void clearRect(const glm::vec4& rect,
                   std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
    // Straight pixel read for the assertions that ask about colour rather than
    // about layout ("is the corner empty", "did the border land on the rim").
    void pixel(int x, int y, std::uint8_t& r, std::uint8_t& g,
               std::uint8_t& b, std::uint8_t& a) const;
};

// The signed distance to a rounded box with four independent radii, in the same
// CSS order the rest of the engine uses (x = top-left, y = top-right,
// z = bottom-right, w = bottom-left). `p` is measured from the box's centre with
// y pointing down.
//
// Kept in the header and tested directly: it is the one piece of arithmetic that
// exists three times over — here, in MSL and in GLSL — and the only way to keep
// three copies honest is to be able to ask this one what it thinks.
float roundedBoxSDF(float px, float py, float halfW, float halfH,
                    const glm::vec4& radii);

// A picture a quad wants drawn on it, already in memory. Textures live on the
// CPU anyway (TextureAsset keeps its bytes), so the rasterizer needs no upload
// path — only somebody who knows how to turn an asset id into these four
// numbers, which is the shell's job and not this file's.
struct TextureLevel
{
    const std::uint8_t* rgba = nullptr;   // width * height * 4, row-major
    int width = 0, height = 0;
};
struct TextureView
{
    const std::uint8_t* rgba = nullptr;   // width * height * 4, row-major
    int width = 0, height = 0;
    // Smaller copies, each half the one before, down to 1x1 (level 1 first).
    // Optional: without them a picture drawn much smaller than it is samples
    // only four of the many texels under a pixel, and shimmers. With them the
    // rasterizer picks the two levels that match the on-screen size and blends
    // them, which is what a GPU's trilinear filter does.
    const TextureLevel* mips = nullptr;
    int mipCount = 0;
    // Colour data stored sRGB-encoded. An Image quad samples the bytes as they
    // are (the UI pass does); a MATERIAL decodes them to linear first, as the
    // GPU's sRGB texture formats do — so only material sampling reads this.
    bool srgb = false;
};

// A material on a quad, ready to run (HE::MatCpuProgram, the graph compiled
// for the CPU) with its textures in the program's slot order.
struct MaterialView
{
    const HE::MatCpuProgram* program = nullptr;
    const TextureView* textures = nullptr;
    int textureCount = 0;
};
using MaterialResolver = MaterialView (*)(const HE::UUID& id, void* user);
using TextureResolver = TextureView (*)(const HE::UUID& id, void* user);

// Everything draw() can be handed besides the quads.
struct DrawOptions
{
    glm::vec4 clip{ 0.0f };                 // {0,0,0,0} = the whole image
    TextureResolver texture = nullptr;  void* textureUser = nullptr;
    MaterialResolver material = nullptr; void* materialUser = nullptr;
    float time = 0.0f;                      // what a material's Time node reads
    bool threads = true;
};
// Box-filters `base` (w x h RGBA8) down to 1x1, appending every level after the
// base to `out` (out[0] is level 1). The caller keeps the bytes and points
// TextureView::mips into them.
void buildMipChain(const std::uint8_t* base, int w, int h,
                   std::vector<std::vector<std::uint8_t>>& out);

// Draw `objects` over whatever `target` already holds. The caller owns the
// clear: a full redraw clears first, a dirty-rectangle redraw does not.
//
// `clip` limits every write to that rectangle in pixels ({0,0,0,0} = the whole
// image). It is how a partial redraw is expressed — the object list stays the
// same, only less of it reaches the image.
//
// `tex` resolves a quad's texture; null means no quad has one, which is what a
// test that only cares about Schicht 0 passes. It is only ever called on the
// calling thread, before any pixel is written.
//
// A large region is split into horizontal stripes drawn on the job pool. Every
// pixel still sees the quads in list order, so the picture is the same byte for
// byte as a single-threaded one; `threads = false` forces that single thread.
void draw(Image& target, const std::vector<UIRenderObject>& objects,
          const glm::vec4& clip = glm::vec4(0.0f),
          TextureResolver tex = nullptr, void* texUser = nullptr,
          bool threads = true);

// The same with materials: a quad whose material resolves to a program is
// shaded by it (what the GPU's material pipeline would put there), one that
// does not falls back to Schicht 0 plus its tint. A material reading Backdrop
// sees everything drawn before it, blurred the way the GPU blurs its snapshot
// (a mip level plus nine taps) — the list is drawn in phases around each such
// quad, because the blur reads rows other stripes would still be writing.
void draw(Image& target, const std::vector<UIRenderObject>& objects, const DrawOptions& options);

// Every pixel a quad can possibly touch, as {x, y, w, h}. Wider than the quad's
// own rectangle by design: a rotated quad's corners swing out past it, and a
// drop shadow's falloff reaches beyond the shape. Both the rasterizer's inner
// loop and the dirty-rectangle diff below have to agree on this number, so they
// ask the same function.
glm::vec4 quadBounds(const UIRenderObject& o);

// ── What actually changed since last frame ───────────────────────────────────
// A CPU rasterizer that repaints a whole 4K window when one hover state flips
// is unusable; a full frame is 8.3 million pixels. Event-driven drawing (A2)
// covers the idle case, but not dragging a slider or resizing a window, which is
// where this comes in.
//
// The comparison is per POSITION in the list, not per element identity: UI quads
// arrive in painter order, so quad 7 being different from last frame's quad 7 is
// exactly the question. Anything that shifts the list (a widget appearing) makes
// everything from there on differ, and that IS the honest answer — everything
// after it moved.
//
// Returns false when tracking is not worth it: the first frame, a size change,
// or a diff so large that the bookkeeping costs more than the repaint. The
// caller then redraws in full. `out` is only meaningful when it returns true.
bool dirtyRects(const std::vector<UIRenderObject>& prev,
                const std::vector<UIRenderObject>& cur,
                int width, int height,
                std::vector<glm::vec4>& out);

} // namespace HE::sw
