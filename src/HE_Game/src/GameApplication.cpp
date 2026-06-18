#include "GameApplication.h"
#include <Hpak/ProjectConfig.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <filesystem>

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

	const char* baseRaw = SDL_GetBasePath();
	if (!baseRaw)
	{
		Logger::Log(Logger::LogLevel::Warning, "GameApplication: SDL_GetBasePath returned null");
		return;
	}
	const std::filesystem::path exeDir(baseRaw);

	if (!ProjectConfigLoader::load(exeDir, m_config))
	{
		Logger::Log(Logger::LogLevel::Info, "GameApplication: no project.hcfg — running without pak");
		return;
	}

	// Override content root set by Application base (it uses argv[0] + "Content")
	contentManager().setContentRoot((exeDir / "Content").string());

	const std::string pakPath = (exeDir / m_config.hpakFilename).string();
	if (contentManager().loadPak(pakPath))
		Logger::Log(Logger::LogLevel::Info, ("GameApplication: loaded " + m_config.hpakFilename).c_str());
	else
		Logger::Log(Logger::LogLevel::Warning, ("GameApplication: pak not found: " + pakPath).c_str());
}

void GameApplication::OnRender(float deltaTime)
{
	(void)deltaTime;
}

void GameApplication::OnShutdown()
{
	Logger::Log(Logger::LogLevel::Info, "GameApplication::OnShutdown");
}
