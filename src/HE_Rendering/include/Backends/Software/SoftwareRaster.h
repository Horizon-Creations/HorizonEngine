#pragma once
#include <Renderer/UIRenderObject.h>

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
// The hard limit, stated rather than hidden (the D5 fallback contract): a quad
// with a MATERIAL cannot be drawn here, because a material is a translated
// shader graph. Such a quad draws Schicht 0 plus its tint — the rounding, the
// border, the gradient and the shadows, only without the graph. That is what
// the closed vocabulary is FOR.
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
struct TextureView
{
    const std::uint8_t* rgba = nullptr;   // width * height * 4, row-major
    int width = 0, height = 0;
};
using TextureResolver = TextureView (*)(const HE::UUID& id, void* user);

// Draw `objects` over whatever `target` already holds. The caller owns the
// clear: a full redraw clears first, a dirty-rectangle redraw does not.
//
// `clip` limits every write to that rectangle in pixels ({0,0,0,0} = the whole
// image). It is how a partial redraw is expressed — the object list stays the
// same, only less of it reaches the image.
//
// `tex` resolves a quad's texture; null means no quad has one, which is what a
// test that only cares about Schicht 0 passes.
void draw(Image& target, const std::vector<UIRenderObject>& objects,
          const glm::vec4& clip = glm::vec4(0.0f),
          TextureResolver tex = nullptr, void* texUser = nullptr);

} // namespace HE::sw
