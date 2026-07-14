#include "Application/GameLoop.h"
#include <cstdint>
#include "IGameLogic.h"

namespace HE {

GameLoop::GameLoop(const GameLoopConfig& config)
    : config_(config)
{
}

bool GameLoop::tick(HorizonWorld& world, IGameLogic* logic, float deltaTime)
{
    if (!running_) return false;

    accumulator_ += deltaTime;
    uint32_t steps = 0;
    while (accumulator_ >= config_.fixedTimestep && steps < config_.maxFixedSteps) {
        if (logic) logic->onUpdate(world, config_.fixedTimestep);
        accumulator_ -= config_.fixedTimestep;
        ++steps;
    }
    return running_;
}

void GameLoop::requestStop() { running_ = false; }

} // namespace HE
