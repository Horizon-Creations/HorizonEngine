#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <string>

// ── GameInstanceHost ─────────────────────────────────────────────────────────
// Owns the app-wide HorizonCode runtime and the single, always-present
// GameInstance script. The GameInstance is the app's HorizonCode class: it
// receives central lifecycle events (OnInit before anything loads, OnShutdown
// at exit, OnWindowFocusChanged) and is referenceable from ANY graph via the
// Get Game Instance node.
//
// The application (editor or packaged game) owns one host, injects its runtime()
// into every HorizonWorld it creates (HorizonWorld::setScriptRuntime), and
// drives the lifecycle. Because the runtime lives here — not in a world — the
// GameInstance and its state persist across scene loads, and its OnInit can fire
// before the first scene is loaded.
class GameInstanceHost
{
public:
    // The shared runtime to inject into worlds (widgets + level scripts join it).
    HorizonCode::Runtime&       runtime()       { return m_runtime; }
    const HorizonCode::Runtime& runtime() const { return m_runtime; }
    HorizonCode::InstanceId     id() const { return m_runtime.gameInstance(); }

    // Register/replace the GameInstance graph (from JSON). Referenceable
    // immediately; does NOT fire OnInit. Empty/broken JSON → an empty (but still
    // referenceable) GameInstance. Call when the graph is loaded or edited.
    void setGraph(const std::string& graphJson);

    // Lifecycle. fireInit runs "OnInit" once (call it before any scene loads);
    // fireShutdown runs "OnShutdown" once. Idempotent w.r.t. the running flag.
    void fireInit();
    void fireShutdown();
    // Forward the OS window focus state; fires "OnWindowFocusChanged" (Bool arg)
    // on a real change while the GameInstance is running.
    void setWindowFocus(bool focused);

    bool isRunning() const { return m_running; }

private:
    HorizonCode::Runtime m_runtime;
    bool m_running = false;
    bool m_focused = true;
};
