#include "HorizonScene/GameInstanceHost.h"
#include <HorizonCode/HorizonCode.h>

void GameInstanceHost::setGraph(const std::string& graphJson)
{
    HorizonCode::Graph g;
    if (!graphJson.empty())
        HorizonCode::fromJson(graphJson, g); // broken/absent → empty GameInstance
    m_runtime.setGameInstance(std::move(g)); // replaces any prior GameInstance
}

void GameInstanceHost::fireInit()
{
    if (m_running) return;
    m_running = true;
    // Fresh state each play session (the GameInstance persists, so it isn't
    // re-registered like scene scripts are — reset its variables here).
    m_runtime.reseedVariables(m_runtime.gameInstance());
    m_runtime.fireEvent(m_runtime.gameInstance(), "OnInit", 0);
}

void GameInstanceHost::fireShutdown()
{
    if (!m_running) return;
    m_running = false;
    m_runtime.fireEvent(m_runtime.gameInstance(), "OnShutdown", 0);
    // Drop any play-session state (and its refs to created objects) so the scene
    // teardown sweep collects objects the GameInstance was holding.
    m_runtime.reseedVariables(m_runtime.gameInstance());
}

void GameInstanceHost::setWindowFocus(bool focused)
{
    if (!m_running || focused == m_focused) return;
    m_focused = focused;
    m_runtime.fireEvent(m_runtime.gameInstance(), "OnWindowFocusChanged", 0,
                        HorizonCode::Value::ofBool(focused));
}
