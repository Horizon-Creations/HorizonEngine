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
    m_runtime.fireEvent(m_runtime.gameInstance(), "OnInit", 0);
}

void GameInstanceHost::fireShutdown()
{
    if (!m_running) return;
    m_running = false;
    m_runtime.fireEvent(m_runtime.gameInstance(), "OnShutdown", 0);
}

void GameInstanceHost::setWindowFocus(bool focused)
{
    if (!m_running || focused == m_focused) return;
    m_focused = focused;
    m_runtime.fireEvent(m_runtime.gameInstance(), "OnWindowFocusChanged", 0,
                        HorizonCode::Value::ofBool(focused));
}
