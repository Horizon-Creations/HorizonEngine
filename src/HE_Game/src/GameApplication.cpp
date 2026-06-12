#include "GameApplication.h"
#include <Diagnostics/Logger.h>

HE::ApplicationConfig GameApplication::GetConfig() const
{
	HE::ApplicationConfig cfg;
	cfg.windowprops.title  = m_config.projectName.empty() ? "HorizonGame" : m_config.projectName;
	cfg.windowprops.width  = 1280;
	cfg.windowprops.height = 720;
	cfg.windowprops.vsync  = true;
	cfg.windowprops.mode   = HE::WindowMode::Fullscreen;
#ifdef __APPLE__
	cfg.backend = RendererFactory::Backend::Metal;
#else
	cfg.backend = RendererFactory::Backend::OpenGL;
#endif
	return cfg;
}

std::unique_ptr<IRenderer> GameApplication::CreateRenderer()
{
	auto m_backend = GetConfig().backend;
	Logger::Log(Logger::LogLevel::Info, "GameApplication: creating renderer");
	return RendererFactory::Create(m_backend);
}

void GameApplication::OnInit()
{
	Logger::Log(Logger::LogLevel::Info, "GameApplication::OnInit");
}

void GameApplication::OnRender(float deltaTime)
{
	(void)deltaTime;
}

void GameApplication::OnShutdown()
{
	Logger::Log(Logger::LogLevel::Info, "GameApplication::OnShutdown");
}
