#pragma once
#include <Application/Application.h>
#include <Hpak/ProjectConfig.h>
#include <memory>
#include <HorizonScene/HorizonWorld.h>

// Game-specific Application: handles the packaged-shipping bootstrap
// (project.hcfg, encrypted hpak, key derivation, GameLogic.dll loading).
class GameApplication : public HE::Application
{
public:
    explicit GameApplication(std::string startupPath)
        : HE::Application(std::move(startupPath)) {}
    ~GameApplication() override; // defined in the .cpp where HorizonWorld is complete

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

    ProjectConfig                 m_config;
    std::unique_ptr<HorizonWorld> m_world; // startup scene, ticked + rendered each frame
    bool m_mouseCaptured = false;          // set true in OnInit once the window exists
    bool m_vsyncOn       = true;           // mirrors GetConfig().windowprops.vsync; V toggles it
};

