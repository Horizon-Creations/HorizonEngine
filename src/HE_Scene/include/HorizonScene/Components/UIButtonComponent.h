#pragma once
#include <Math/Math.h>
#include <string>
#include <cstdint>

enum class UIButtonState : uint8_t { Normal = 0, Hovered, Pressed };

struct UIButtonComponent {
    glm::vec4     normalColor   = {0.20f, 0.20f, 0.20f, 1.0f};
    glm::vec4     hoveredColor  = {0.30f, 0.30f, 0.30f, 1.0f};
    glm::vec4     pressedColor  = {0.10f, 0.10f, 0.10f, 1.0f};
    std::string   onClickFunction;
    UIButtonState state         = UIButtonState::Normal;
};
