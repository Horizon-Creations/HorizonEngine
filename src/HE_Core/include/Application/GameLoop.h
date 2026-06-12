#pragma once
#include "Types/Defines.h"
#include <cstdint>

class HorizonWorld;
class IGameLogic;

namespace HE {

struct GameLoopConfig {
    float    fixedTimestep = 1.0f / 60.0f;
    uint32_t maxFixedSteps = 5;
    bool     vsync         = true;
};

// Frame pacing & fixed-timestep tick.
// Used internally by HE::Application; not normally instantiated by user code.
class HE_API GameLoop {
public:
    explicit GameLoop(const GameLoopConfig& config = {});

    // One frame. Returns false if the loop should exit.
    bool tick(HorizonWorld& world, IGameLogic* logic, float deltaTime);

    void requestStop();
    bool running() const { return running_; }

    const GameLoopConfig& config() const { return config_; }

private:
    GameLoopConfig config_;
    bool           running_      = true;
    float          accumulator_  = 0.0f;
};

} // namespace HE
