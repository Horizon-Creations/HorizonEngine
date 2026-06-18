#pragma once
#include <Math/Math.h>
#include <string>

struct UITextComponent {
    std::string text     = "Text";
    float       fontSize = 14.0f;
    glm::vec4   color    = {1.0f, 1.0f, 1.0f, 1.0f};
};
