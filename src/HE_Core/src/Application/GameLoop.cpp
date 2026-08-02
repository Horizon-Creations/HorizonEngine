#include "Application/GameLoop.h"
#include <cstdint>
#include "IGameLogic.h"
#include "Diagnostics/Log.h"

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

    // Hitting the step cap means fixed-update cannot keep up with real time: the
    // simulation is now running slower than the wall clock and the accumulator
    // keeps growing (the classic "spiral of death"). Silent until now — the only
    // symptom was gameplay drifting into slow motion.
    if (steps >= m_config.maxFixedSteps && m_accumulator >= m_config.fixedTimestep)
        HE_LOG_THROTTLE(Core, Warning, 2.0,
                        "Fixed update is falling behind: %u step(s) ran (the cap) and "
                        "%.1f ms of simulation time is still outstanding",
                        steps, m_accumulator * 1000.0f);

    return m_running;
}

void GameLoop::requestStop() { m_running = false; }

} // namespace HE
