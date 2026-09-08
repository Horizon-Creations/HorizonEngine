#pragma once
#include <Types/UUID.h>

struct AnimatorComponent
{
    HE::UUID clipAssetId;
    float    playbackTime  = 0.0f;   // current position in the clip, seconds
    float    playbackSpeed = 1.0f;
    bool     looping       = true;
    bool     playing       = true;

    // Has this playhead already had a frame of notifies walked out of it?
    //
    // Runtime only, never saved: it says something about THIS session's first
    // frame, and a scene file that carried it would tell the next session that a
    // frame it never ran had already happened. False means the next span closes
    // at its origin, so a notify sitting exactly on the starting time — the
    // footstep on frame 0 — fires instead of falling just outside.
    bool     notifiesPrimed = false;
};
