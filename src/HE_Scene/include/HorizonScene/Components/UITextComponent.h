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
};
