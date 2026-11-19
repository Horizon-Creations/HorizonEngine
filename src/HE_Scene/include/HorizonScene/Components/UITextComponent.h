#pragma once
#include <Math/Math.h>
#include <cstdint>
#include <string>

struct UITextComponent {
    std::string text     = "Text";
    float       fontSize = 14.0f;
    glm::vec4   color    = {1.0f, 1.0f, 1.0f, 1.0f};

    // Optional Font asset (content-relative path) for this text; empty = the
    // shared default UI font. Resolved to fontAtlasKey during UI extraction,
    // mirroring UIElement.h:698-701.
    std::string font;
    uint32_t    fontAtlasKey = 0; // transient: baked-atlas key (not serialized)

    // Transient: the `font` value fontAtlasKey was last resolved against (not
    // serialized). extract() only re-resolves when this differs from `font`, so
    // a stale path or a TTF that fails to bake costs one disk probe / one failed
    // bake, not one per frame — see UISystem.cpp's extract().
    std::string resolvedFont;
};
