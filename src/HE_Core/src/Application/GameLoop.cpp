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

    // Nothing to step means nothing to fall behind on. Without this the fixed
    // loop is pure bookkeeping — it counts steps it does not take — and an
    // application (which ships no game-logic module at all) is told once every
    // two seconds that a simulation it does not have is running late.
    if (!logic) { m_accumulator = 0.0f; return m_running; }

    m_accumulator += deltaTime;
    uint32_t steps = 0;
    while (m_accumulator >= m_config.fixedTimestep && steps < m_config.maxFixedSteps) {
        if (logic) logic->onUpdate(world, m_config.fixedTimestep);
        m_accumulator -= m_config.fixedTimestep;
        ++steps;
    }

    // Hitting the step cap means fixed-update cannot keep up with real time. The
    // outstanding time is DROPPED rather than carried, because carrying it is
    // the spiral of death: the backlog grows every frame, each frame does more
    // work than the last, and the simulation slides ever further into slow
    // motion with no way back.
    //
    // It is also what an event-driven application hits by construction and not
    // by being slow (docs/he-apps-plan.md A2): drawing when something changes
    // means a frame can be a tenth of a second long, which is six fixed steps
    // for a cap of five — every single frame, at idle, forever. That is what the
    // Wave-1 acceptance found, and clamping is the answer for both cases.
    //
    // The warning stays: falling behind is worth knowing about even once the
    // consequence is bounded.
    if (steps >= m_config.maxFixedSteps && m_accumulator >= m_config.fixedTimestep)
    {
        HE_LOG_THROTTLE(Core, Warning, 2.0,
                        "Fixed update is falling behind: %u step(s) ran (the cap) and "
                        "%.1f ms of simulation time was dropped to keep real time",
                        steps, m_accumulator * 1000.0f);
        m_accumulator = 0.0f;
    }

    return m_running;
}

void GameLoop::requestStop() { m_running = false; }

} // namespace HE
