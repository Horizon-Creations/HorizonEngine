#pragma once
#include <cstdint>

enum class UIRenderMode : uint8_t { ScreenSpace = 0, WorldSpace = 1 };

struct UICanvasComponent {
    float        width      = 1920.0f;
    float        height     = 1080.0f;
    UIRenderMode renderMode = UIRenderMode::ScreenSpace;
    bool         active     = true;
};
