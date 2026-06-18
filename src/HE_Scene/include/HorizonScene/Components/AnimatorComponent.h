#pragma once
#include <Types/UUID.h>

struct AnimatorComponent
{
    HE::UUID clipAssetId;
    float    playbackTime  = 0.0f;   // current position in the clip, seconds
    float    playbackSpeed = 1.0f;
    bool     looping       = true;
    bool     playing       = true;
};
