#pragma once
#include <Math/Math.h>
#include <Types/UUID.h>
#include <cstdint>

// One drawable item produced by UISystem::extract, consumed by renderer backends.
struct UIRenderObject {
    glm::vec2   position;          // top-left corner in screen pixels
    glm::vec2   size;              // width/height in screen pixels
    glm::vec4   color     = {1.0f, 1.0f, 1.0f, 1.0f};
    HE::UUID    materialAssetId;   // image quads (custom material; nil = solid color)
    // Texture asset drawn on the quad, tinted by `color` and sampled over
    // uvMin..uvMax. Nil = no texture (solid colour, or the material above).
    // A material wins when both are set: it owns the pixels.
    HE::UUID    textureAssetId;
    // 0 = rect/image, 2 = font-atlas glyph quad (uvMin/uvMax into
    // UISystem::sharedFont atlas). Text is emitted per glyph as type 2, so there
    // is no whole-string kind; 1 is unused and left as a hole to keep the values
    // that every backend already branches on stable.
    uint8_t     type      = 0;
    int         layer     = 0;
    glm::vec2   uvMin     = {0.0f, 0.0f}; // glyph quads: atlas UV rect
    glm::vec2   uvMax     = {0.0f, 0.0f};
    // Corner radii in pixels for solid quads (type 0), one per corner in CSS
    // order: x = top-left, y = top-right, z = bottom-right, w = bottom-left.
    // 0 = square. All four at min(w,h)/2 yields a circle — used for the slider
    // handle. Ignored by glyphs.
    //
    // Four rather than one because a tab, a chat bubble and the top half of a
    // card all round some corners and not others, and every one of those was
    // impossible to author with a single number.
    glm::vec4   cornerRadius{ 0.0f, 0.0f, 0.0f, 0.0f };
    // ── Border ("Schicht 0", docs/he-apps-plan.md D5) ────────────────────────
    // An outline drawn INSIDE the quad, following the corner radius, in pixels.
    // 0 = none, which is what every quad carried before borders existed.
    //
    // Inside rather than centred on the edge: an element's rect is what the
    // layout gave it, and a border that grew outwards would overlap its
    // neighbours and change with its own width.
    float       borderWidth = 0.0f;
    glm::vec4   borderColor{ 0.0f, 0.0f, 0.0f, 1.0f };
    // ── Linear gradient ("Schicht 0") ────────────────────────────────────────
    // Off = the quad is `color` throughout, which is what every quad was before.
    // On = `color` fades to `gradientColor` along `gradientAngleDeg`, measured
    // clockwise from "down": 0 = top to bottom, 90 = left to right.
    //
    // Down at zero rather than right, because a vertical fade is what almost
    // every button and header wants, and the common case should be the one that
    // needs no number typed into it.
    bool        gradient = false;
    glm::vec4   gradientColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float       gradientAngleDeg = 0.0f;
    // 0 = linear (the angle above), 1 = radial: `color` at the centre of the
    // quad fading to `gradientColor` at its FARTHEST CORNER, which is the rule
    // CSS's radial-gradient uses by default. Farthest corner rather than
    // nearest side because it is the only normalization under which the second
    // colour actually reaches every part of the box.
    int         gradientShape = 0;
    // Glyph quads (type 2): which baked font atlas to sample. 0 = shared default
    // font; other keys index UIFontCache (an imported Font asset).
    uint32_t    fontAtlasKey = 0;
    // Scissor rectangle in screen pixels {x, y, w, h}: pixels outside it are not
    // drawn. w <= 0 means unclipped, which is what every quad carried before
    // clipping existed. Set by the UI producer (WidgetManager intersects the
    // rects of every clipping ancestor), applied by the backends as a scissor —
    // one state change per RUN of equally-clipped quads, not per quad.
    glm::vec4   clipRect{ 0.0f, 0.0f, 0.0f, 0.0f };
    // Render rotation: every corner is turned by `rotation` radians about
    // `rotationPivot` (screen pixels) after the rect is built. 0 = upright,
    // which is what every quad carried before rotation existed. A chain of
    // rotations about different points is again one rotation about a point, so
    // an inherited rotation needs no more than this (the producer folds the
    // chain and shifts the rect; see uiElementRotation).
    float       rotation = 0.0f;
    glm::vec2   rotationPivot{ 0.0f, 0.0f };
};
