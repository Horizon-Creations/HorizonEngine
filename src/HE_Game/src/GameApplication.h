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
#include <HorizonScene/PlayerHost.h>
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
    // Player controller/character HorizonCode instances + their input pump.
    // Runs on m_gameInstance's runtime; begun after the startup scene loads,
    // ended in OnShutdown before the GameInstance fires OnShutdown.
    PlayerHost m_playerHost;
    // App-level UI: the GameInstance's widgets live here (not in any world), so a
    // HUD created in OnInit exists before the first scene and survives scene
    // switches. Each world borrows it via setWidgetManager. Declared after
    // m_gameInstance so it is destroyed BEFORE the runtime it references.
    WidgetManager m_widgets;
    std::unique_ptr<HorizonWorld> m_world; // startup scene, ticked + rendered each frame
    bool m_mouseCaptured = false;          // set true in OnInit once the window exists
    bool m_vsyncOn       = true;           // mirrors GetConfig().windowprops.vsync; V toggles it

    std::unique_ptr<ScriptContext> m_scriptContext; // ECS Lua/Python scripts (null until OnInit)
    std::unordered_map<uint32_t, ScriptEngine::InstanceId> m_scriptInstances; // entity → instance
    UIInputSystem::InputState m_uiInput;   // frame-to-frame UI pointer tracking
    AudioEngine m_audioEngine;             // game-runtime audio (playOnStart + audio.* API)

    // ── Scene transitions (HE::api::scene requests, executed at frame start) ──
    void executeSceneRequests();
    bool performSceneSwitch(const std::string& scenePath);
    // Swap the running world for an already-loaded one (shared by switch + activate).
    void swapToWorld(std::unique_ptr<HorizonWorld> newWorld, const std::string& label);
    // Resolve a project-relative .hescene: packed pak entry (path-derived UUID)
    // → loose JSON in the project → loose JSON next to the executable.
    bool loadSceneInto(HorizonWorld& world, const std::string& scenePath,
                       bool additive, std::vector<entt::entity>* outCreated);
    int  startScriptsFor(const std::vector<entt::entity>& entities); // additive zones
    // Level preload (scene.load with hidden=true): built here, swapped in on
    // scene.activate(). Zone bookkeeping lives centrally in HE::api::scene.
    std::unique_ptr<HorizonWorld> m_pendingWorld;
    std::string                   m_pendingScenePath;
};

