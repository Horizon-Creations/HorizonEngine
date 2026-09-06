#pragma once
#include <Math/Math.h>

struct CameraComponent {
    float fovDegrees   = 60.0f;
    float nearPlane    = 0.1f;
    float farPlane     = 1000.0f;
    bool  isMain       = false;   // only one camera per world may have isMain = true
    bool  orthographic = false;

    // Runtime, NOT serialised: the additive part of the FOV, written by the
    // camera rig's FOV kick and by a blend between two cameras of different FOV.
    // The projection uses fovDegrees + fovOffset; everything that means "the
    // camera's FOV" — the inspector, the serialiser, camera.getFov/setFov —
    // still means fovDegrees alone.
    //
    // Additive rather than folded in, for the same reason shake is not written
    // into the transform: a kick added to fovDegrees and read back next frame
    // accumulates, and it would accumulate into the saved scene as well.
    float fovOffset    = 0.0f;
};
