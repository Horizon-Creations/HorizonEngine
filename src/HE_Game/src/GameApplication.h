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
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/EngineApi.h>   // SaveServicesBinding (C++ GameLogic services)
#include <HorizonGameServices.h>      // HeSaveServices (the injected C-ABI table)

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
    // Read the graphics + window settings the export shipped next to the game
    // data and lay them over whatever GlobalState resolved, then settle the
    // startup window and backend from them. Called from the constructor: Run()
    // asks for GetConfig() before OnInit, so this is the last moment at which
    // the window and the RHI can still be chosen.
    void applyShippedConfig();

    // Grab/release the mouse for FPS-style look: relative mode + hidden cursor.
    // The packaged game starts captured; Esc toggles it so the cursor is
    // reachable (e.g. to quit) without trapping the player.
    void setMouseCaptured(bool captured);

    // Whatever camera the scene asks for, while the mouse is captured: a
    // CameraRigComponent camera (first/third person, following a target) when
    // there is one, otherwise the built-in free flight — mouse look + WASD/QE
    // (Shift = sprint) — so a shipped game is navigable out of the box. A no-op
    // if the scene has no camera; game logic can ignore it by not marking a
    // camera isMain / releasing the mouse.
    void updateCameraController(float dt);

    // The scene entity of the first player character the PlayerHost spawned, so
    // a rig with no explicit target follows the player. entt::null when there is
    // none (no player class, or one that brings no entity).
    Entity possessedCharacterEntity() const;

    // Build the physics world for the CURRENT scene. Called BEFORE startScripts,
    // which hands the world to the script context; run again after every scene
    // switch, since the bodies belong to the world that is going away.
    void startPhysics();

    // Start every enabled ScriptComponent in the startup scene (Lua/Python), and
    // tick their onUpdate each frame — the packaged game's ECS gameplay-script
    // driver, mirroring the editor's play mode. Native C++ GameLogic (above) is
    // independent; a game may use either or both.
    void startScripts();
    void updateScripts(float dt);

    // In-game UI pointer input: hit-test the (uncaptured) mouse against UI
    // elements, drive button states and dispatch onClick/onHover* to scripts.
    void updateUIInput();

    // Ensure a camera the free-fly controller can drive. A scene authored without
    // one otherwise renders through the extractor's fixed fallback camera, which
    // can't move — so the game would look frozen. Only added when the scene has no
    // camera at all; an authored camera is never overridden. Returns true if one
    // was added (the startup path logs that, a scene switch does not).
    static bool ensureDefaultCamera(HorizonWorld& world);

    // Reference-graph streaming seed: kick off async loads for the assets this scene
    // actually references. Their baked transitive dependencies (materials → textures)
    // follow automatically via the frontier in pollAsyncResults, so the loader pulls
    // only the closure the scene needs — unused pak assets are never loaded. The
    // async UUID loader resolves from mounted paks first and falls back to the disk
    // registry, so this also works for a WIP build running on loose content.
    // Returns the number of seeded asset roots (for the log lines).
    size_t streamSceneAssets(HorizonWorld& world);

    ProjectConfig                 m_config;
    // App-wide HorizonCode host: owns the runtime the world runs on and the
    // GameInstance (OnInit fires before the scene loads; OnShutdown at exit).
    GameInstanceHost m_gameInstance;
    // Player controller/character HorizonCode instances + their input pump.
    // Runs on m_gameInstance's runtime; begun after the startup scene loads,
    // ended in OnShutdown before the GameInstance fires OnShutdown.
    PlayerHost m_playerHost;
    // HorizonCode classes attached to scene entities (ScriptComponent pointing
    // at a class asset). Same runtime and same lifetime as m_playerHost.
    EntityHost m_entityHost;
    // The state machines' sync graphs. Not ticked from here — it is handed to
    // the animation phase, which fires each graph right before the transitions
    // it feeds (see AnimationStateMachineSystem::update).
    AnimatorHost m_animatorHost;
    // Physics. The shipping runtime used to have none at all, which made every
    // physics.* call a silent no-op and left the collision/overlap events dead
    // in an exported game while they worked in PIE. Rebuilt on every scene
    // switch, because the bodies belong to the world that is going away.
    std::unique_ptr<PhysicsWorld> m_physicsWorld;
    float m_physicsAccum = 0.0f;
    // App-level UI: the GameInstance's widgets live here (not in any world), so a
    // HUD created in OnInit exists before the first scene and survives scene
    // switches. Each world borrows it via setWidgetManager. Declared after
    // m_gameInstance so it is destroyed BEFORE the runtime it references.
    WidgetManager m_widgets;

    // C++ GameLogic services (HorizonGameServices.h): the table + its binding
    // must outlive the loaded library, so they live here. Filled + injected
    // right after the library loads.
    HE::api::SaveServicesBinding m_saveServicesBinding;
    HeSaveServices               m_saveServices{};
    std::unique_ptr<HorizonWorld> m_world; // startup scene, ticked + rendered each frame
    bool m_mouseCaptured = false;          // set true in OnInit once the window exists
    // Last frame's UI-navigation buttons (bits: up/down/left/right/activate).
    // The input layer reports held state, and a held Down must step one entry
    // rather than run through the whole menu — so the edges are kept here.
    uint8_t m_uiNavPrev = 0;
    // True while the pointer sits on an interactive UI element (the return value
    // of WidgetManager::processPointer, kept from the last updateUIInput). The
    // mouse BUTTONS are masked out of everything gameplay reads while it holds,
    // so a click on a menu button is not also a shot — the mirror image of the
    // keyboard swallow in OnEvent while a text field has focus.
    bool m_uiWantsPointer = false;
    // Startup window + backend, settled once by applyShippedConfig. The values
    // here are the ones a game shipped with before the config could carry them,
    // so an export without those keys behaves exactly as it always did.
    uint32_t            m_windowWidth  = 1280;
    uint32_t            m_windowHeight = 720;
    HE::WindowMode      m_windowMode   = HE::WindowMode::Fullscreen;
    HE::RendererBackend m_backend      = HE::RendererBackend::OpenGL;
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

