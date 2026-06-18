#pragma once
#include <Math/Math.h>
#include <cstdint>

enum class UIAnchor : uint8_t {
    TopLeft=0, TopCenter, TopRight,
    MiddleLeft, MiddleCenter, MiddleRight,
    BottomLeft, BottomCenter, BottomRight
};

struct UIElementComponent {
    glm::vec2 position = {0.0f, 0.0f};   // offset from anchor, in canvas units
    glm::vec2 size     = {100.0f, 30.0f};
    glm::vec2 pivot    = {0.5f, 0.5f};   // 0-1 within the element
    float     rotation = 0.0f;
    UIAnchor  anchor   = UIAnchor::TopLeft;
    int       layer    = 0;
    bool      active   = true;
};
