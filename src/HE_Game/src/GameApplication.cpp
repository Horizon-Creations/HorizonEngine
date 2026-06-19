#include "GameApplication.h"
#include <Hpak/ProjectConfig.h>
#include <Diagnostics/Logger.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <cmath>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <filesystem>

GameApplication::~GameApplication() = default;

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

	// Load the startup scene into a world and hand it to the renderer. The base
	// Application renders m_world each frame; OnRender ticks its gameplay systems.
	m_world = std::make_unique<HorizonWorld>();
	if (!m_config.mainSceneName.empty())
	{
		const std::filesystem::path scenePath = exeDir / m_config.mainSceneName;
		SceneSerializer serializer;
		if (serializer.load(*m_world, scenePath, SerializeFormat::JSON))
			Logger::Log(Logger::LogLevel::Info, ("GameApplication: loaded scene " + m_config.mainSceneName).c_str());
		else
			Logger::Log(Logger::LogLevel::Warning, ("GameApplication: failed to load scene " + scenePath.string()).c_str());
	}
	setWorld(m_world.get());
}

void GameApplication::OnRender(float deltaTime)
{
	// Tick the shared gameplay/visual systems (weather, animation, particles, …) so a
	// shipped game animates exactly like the editor preview. Feed the active scene
	// camera's world position so LOD + precipitation follow the player.
	if (m_world)
	{
		glm::vec3 camPos(0.0f);
		for (auto [e, tc, cam] : m_world->registry().view<TransformComponent, CameraComponent>().each())
		{
			camPos = glm::vec3(tc.worldMatrix[3]);
			break;
		}
		SceneSystems::tick(*m_world, contentManager(), renderer(), camPos, deltaTime);

		// Push the scene environment to the renderer. The base Application renders the
		// world but never pushes EnvironmentSettings (that lived only in the editor), so
		// without this the weather sky / clouds / fog / flash would not show in-game.
		if (auto* env = m_world->registry().try_get<EnvironmentComponent>(m_world->rootEntity()))
		{
			if (env->dayNightCycle && env->autoAdvance && deltaTime > 0.0f)
			{
				env->timeOfDay += deltaTime / std::max(env->cycleSeconds, 1.0f);
				env->timeOfDay -= std::floor(env->timeOfDay);
			}
			renderer()->SetEnvironmentSettings(IRenderer::EnvironmentSettings{
				env->dayNightCycle, env->timeOfDay,
				env->sunColor, env->sunIntensity,
				env->moonColor, env->moonIntensity,
				env->cloudCoverage,
				env->fogDensity, env->fogHeightFalloff,
				env->auroraIntensity,
				env->milkyWayIntensity, env->nebulaIntensity,
				env->nebulaColor, env->auroraColor,
				env->windDirection, env->windSpeed, env->flash});
		}
	}
}

void GameApplication::OnShutdown()
{
	Logger::Log(Logger::LogLevel::Info, "GameApplication::OnShutdown");
}
