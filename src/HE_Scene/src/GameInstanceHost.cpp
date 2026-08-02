#include "HorizonScene/GameInstanceHost.h"
#include <HorizonCode/HorizonCode.h>
#include <Diagnostics/Log.h>

void GameInstanceHost::setGraph(const std::string& graphJson)
{
    HorizonCode::Graph g;
    if (!graphJson.empty() && !HorizonCode::fromJson(graphJson, g))
        // An unparsable GameInstance silently becomes an empty one — and then
        // nothing the game does at OnInit happens, with no error anywhere.
        HE_LOG_ERROR(HorizonCode, "GameInstance graph could not be parsed (%zu bytes) — "
                                  "running with an empty GameInstance", graphJson.size());
    else
        HE_LOG_DEBUG(HorizonCode, "GameInstance graph set: %zu node(s)", g.nodes.size());
    m_runtime.setGameInstance(std::move(g)); // replaces any prior GameInstance
}

void GameInstanceHost::fireInit()
{
    if (m_running) return;
    m_running = true;
    // Fresh state each play session (the GameInstance persists, so it isn't
    // re-registered like scene scripts are — reset its variables here).
    m_runtime.reseedVariables(m_runtime.gameInstance());
    HE_LOG_INFO(HorizonCode, "%s", "GameInstance OnInit");
    m_runtime.fireEvent(m_runtime.gameInstance(), "OnInit", 0);
}

void GameInstanceHost::fireShutdown()
{
    if (!m_running) return;
    m_running = false;
    HE_LOG_INFO(HorizonCode, "%s", "GameInstance OnShutdown");
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
