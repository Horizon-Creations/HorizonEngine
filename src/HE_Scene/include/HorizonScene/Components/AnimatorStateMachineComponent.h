#pragma once
#include <Types/UUID.h>
#include <string>
#include <vector>
#include <unordered_map>

enum class TransitionOp : uint8_t { Greater = 0, Less = 1, Equal = 2 };

struct AnimationState {
    std::string name;
    HE::UUID    clipId;
    bool        looping = true;
};

struct AnimationTransition {
    std::string  fromState;
    std::string  toState;
    std::string  paramName;
    TransitionOp op        = TransitionOp::Greater;
    float        threshold = 0.5f;
    float        duration  = 0.2f;  // crossfade length, seconds
};

struct AnimatorStateMachineComponent
{
    std::vector<AnimationState>            states;
    std::vector<AnimationTransition>       transitions;
    std::unordered_map<std::string, float> params;

    std::string currentStateName;
    float       clipTime      = 0.0f;
    float       playbackSpeed = 1.0f;

    // Active crossfade (only meaningful when inTransition==true)
    bool        inTransition       = false;
    std::string transitionTarget;
    float       transitionElapsed  = 0.0f;
    float       transitionDuration = 0.2f;
};
