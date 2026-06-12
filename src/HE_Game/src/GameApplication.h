#pragma once
#include <Application/Application.h>
#include "ProjectConfig.h"

// Game-specific Application: handles the packaged-shipping bootstrap
// (project.hcfg, encrypted hpak, key derivation, GameLogic.dll loading).
class GameApplication : public HE::Application
{
public:
    explicit GameApplication(std::string startupPath)
        : HE::Application(std::move(startupPath)) {}

protected:
    HE::ApplicationConfig GetConfig() const override;
    void            OnInit()               override;
    void            OnRender(float dt)     override;
    void            OnShutdown()           override;

    std::unique_ptr<IRenderer> CreateRenderer()      override;

private:
    ProjectConfig m_config;
};

