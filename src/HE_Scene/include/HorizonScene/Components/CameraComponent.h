#pragma once
#include <Math/Math.h>

struct CameraComponent {
    float fovDegrees   = 60.0f;
    float nearPlane    = 0.1f;
    float farPlane     = 1000.0f;
    bool  isMain       = false;   // only one camera per world may have isMain = true
    bool  orthographic = false;
};
