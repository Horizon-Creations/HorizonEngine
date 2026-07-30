#include "Application/GameLoop.h"
#include <cstdint>
#include "IGameLogic.h"

namespace HE {

GameLoop::GameLoop(const GameLoopConfig& config)
    : m_config(config)
{
}

bool GameLoop::tick(HorizonWorld& world, IGameLogic* logic, float deltaTime)
{
    if (!m_running) return false;

    m_accumulator += deltaTime;
    uint32_t steps = 0;
    while (m_accumulator >= m_config.fixedTimestep && steps < m_config.maxFixedSteps) {
        if (logic) logic->onUpdate(world, m_config.fixedTimestep);
        m_accumulator -= m_config.fixedTimestep;
        ++steps;
    }
    return m_running;
}

void GameLoop::requestStop() { m_running = false; }

} // namespace HE
