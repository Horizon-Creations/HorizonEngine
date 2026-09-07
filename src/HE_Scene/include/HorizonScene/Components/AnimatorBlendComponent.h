#pragma once
#include <Types/UUID.h>

struct AnimatorBlendComponent
{
    HE::UUID clipAId;
    HE::UUID clipBId;
    float    blendAlpha    = 0.0f;  // 0=pure clipA, 1=pure clipB
    float    playbackTime  = 0.0f;
    float    playbackSpeed = 1.0f;
    bool     looping       = true;
    bool     playing       = true;

    // One per clip, not one for the component: the two clips run on their own
    // wrapped playheads (each against its own duration), so each has its own
    // first frame to close. Runtime only — see AnimatorComponent for why.
    bool     notifiesPrimedA = false;
    bool     notifiesPrimedB = false;
};
