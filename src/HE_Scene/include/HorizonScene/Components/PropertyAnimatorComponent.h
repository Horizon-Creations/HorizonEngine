#pragma once
#include <Types/UUID.h>

struct PropertyAnimatorComponent {
    HE::UUID clipId;
    float    playbackTime  = 0.0f;
    float    playbackSpeed = 1.0f;
    bool     looping       = true;
    bool     playing       = true;
};
