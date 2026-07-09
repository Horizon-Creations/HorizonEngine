#pragma once
#include <Application/Application.h>
#include <Hpak/ProjectConfig.h>
#include <Scripting/ScriptEngine.h>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/UIInputSystem.h>
#include <HorizonScene/GameInstanceHost.h>
#include <HorizonScene/AudioEngine.h>

class ScriptContext;

// Game-specific Application: handles the packaged-shipping bootstrap
// (project.hcfg, encrypted hpak, key derivation, GameLogic.dll loading).
class GameApplication : public HE::Application
{
public:
    // Both defined in the .cpp, where HorizonWorld + ScriptContext are complete
    // (the unique_ptr members need the full types for construction/destruction).
    explicit GameApplication(std::string startupPath);
    ~GameApplication() override;

protected:
    HE::ApplicationConfig GetConfig() const override;
    void            OnInit()               override;
    bool            OnEvent(const SDL_Event& event) override;
    void            OnRender(float dt)     override;
    void            OnShutdown()           override;

    std::unique_ptr<IRenderer> CreateRenderer()      override;

private:
    // Grab/release the mouse for FPS-style look: relative mode + hidden cursor.
    // The packaged game starts captured; Esc toggles it so the cursor is
    // reachable (e.g. to quit) without trapping the player.
    void setMouseCaptured(bool captured);

    // Built-in free-fly camera so a shipped game is navigable out of the box:
    // mouse look + WASD/QE (Shift = sprint) drive the scene's main camera while
    // the mouse is captured. A no-op if the scene has no camera; game logic can
    // ignore it by not marking a camera isMain / releasing the mouse.
    void updateCameraController(float dt);

    // Start every enabled ScriptComponent in the startup scene (Lua/Python), and
    // tick their onUpdate each frame — the packaged game's ECS gameplay-script
    // driver, mirroring the editor's play mode. Native C++ GameLogic (above) is
    // independent; a game may use either or both.
    void startScripts();
    void updateScripts(float dt);

    // In-game UI pointer input: hit-test the (uncaptured) mouse against UI
    // elements, drive button states and dispatch onClick/onHover* to scripts.
    void updateUIInput();

    ProjectConfig                 m_config;
    // App-wide HorizonCode host: owns the runtime the world runs on and the
    // GameInstance (OnInit fires before the scene loads; OnShutdown at exit).
    GameInstanceHost m_gameInstance;
    std::unique_ptr<HorizonWorld> m_world; // startup scene, ticked + rendered each frame
    bool m_mouseCaptured = false;          // set true in OnInit once the window exists
    bool m_vsyncOn       = true;           // mirrors GetConfig().windowprops.vsync; V toggles it

    std::unique_ptr<ScriptContext> m_scriptContext; // ECS Lua/Python scripts (null until OnInit)
    std::unordered_map<uint32_t, ScriptEngine::InstanceId> m_scriptInstances; // entity → instance
    UIInputSystem::InputState m_uiInput;   // frame-to-frame UI pointer tracking
    AudioEngine m_audioEngine;             // game-runtime audio (playOnStart + audio.* API)
};

