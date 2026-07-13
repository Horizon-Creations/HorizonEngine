#include "GameApplication.h"
#include "EmbeddedPakKey.h"
#include <fstream>
#include <Hpak/ProjectConfig.h>
#include <Diagnostics/Logger.h>
#include <Diagnostics/Profiler.h>
#include <Diagnostics/GlobalState.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/UICursorSDL.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/AudioSystem.h>
#include <DebugDraw/DebugDraw.h>     // DebugLine (HE::api::debug drain)
#include <Hpak/ProjectExporter.h>    // sceneUuidForPath (packed scene lookup)
#include <HorizonCode/HcCompiledLoader.h> // compiled HorizonCode classes (hybrid)
#include "HorizonVersion.h"          // HE_VERSION_STRING (compiled-classes handshake)
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/ScriptApi.h>
#include <HorizonScene/EngineApi.h>
#include <Scripting/ScriptTypes.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/UUID.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <filesystem>

GameApplication::GameApplication(std::string startupPath)
	: HE::Application(std::move(startupPath)) {}
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

	// Grab the mouse on startup (FPS-style look). Done first so it holds even on
	// the early-return paths below (no hcfg / no pak); Esc toggles it back so the
	// cursor is always reachable. The window is already open by the time OnInit runs.
	setMouseCaptured(true);

	// Enable SDL text-input so focused in-game TextInput widgets receive
	// SDL_EVENT_TEXT_INPUT. Harmless when no field is focused (OnEvent only
	// routes text while a widget field has focus).
	if (SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr)
		SDL_StartTextInput(w);

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
	// Index loose content (UUID → path) so a WIP build without a pak (or with
	// assets missing from the pak) still resolves scene references from disk.
	// No-op in a fully packaged build where Content/ doesn't exist.
	contentManager().scanContentDirectory();

	const std::string pakPath = (exeDir / m_config.hpakFilename).string();
	// AES key for an encrypted pak; nullptr for an unencrypted one. Preferred
	// source is the key block the exporter patched into THIS executable
	// (EmbeddedPakKey.h — no key file next to the game); the key in project.hcfg
	// is the legacy fallback for exports made against a runtime without the block.
	const uint8_t* pakKey = nullptr;
	if (g_hePakKeyBlock.hasKey)      pakKey = g_hePakKeyBlock.key;
	else if (m_config.encrypted)     pakKey = m_config.encKey;
	// Mount (not eager-load): open + index the archive. Assets are then streamed on
	// background workers, seeded from what the scene actually references (below), so
	// only the reference-graph closure loads — unused pak assets are never touched.
	// pollAsyncResults() in OnRender registers arrivals on the main thread; the
	// renderer skips not-yet-resident assets, so the scene pops in over the first
	// frames. Mounting also enables overlay paks (patch/DLC/mods) later.
	// Reconstruct the packed-scene UUID (if any) to read the scene entry directly.
	HE::UUID sceneUuid{};
	if (m_config.hasPackedScene)
	{
		std::memcpy(&sceneUuid.hi, m_config.startupSceneUuid,     8);
		std::memcpy(&sceneUuid.lo, m_config.startupSceneUuid + 8, 8);
	}

	if (contentManager().mountPak(pakPath, pakKey))
	{
		Logger::Log(Logger::LogLevel::Info,
			("GameApplication: mounted " + m_config.hpakFilename).c_str());

		// Mod overlays: every .hpak in Mods/ next to the executable, mounted on
		// top of the base pak in alphabetical order. Same UUID = replacement,
		// new UUID = addition — this also lets a mod override the packed startup
		// scene, which is why mods mount BEFORE the scene is read below.
		if (m_config.enableModSupport)
		{
			const size_t mods = contentManager().mountPakOverlays(exeDir / "Mods");
			if (mods > 0)
				Logger::Log(Logger::LogLevel::Info,
					("GameApplication: mounted " + std::to_string(mods) + " mod pak(s)").c_str());
		}
	}
	else
		Logger::Log(Logger::LogLevel::Warning, ("GameApplication: pak not found: " + pakPath).c_str());

	// Compiled HorizonCode classes: the export may have translated the project's
	// graphs to native C++ (HorizonCodeGen library beside the executable). Loaded
	// once for process lifetime; every host below (GameInstance, createObject,
	// widgets, level scripts) consults the table and falls back to the
	// interpreter on a miss. A misconfiguration (hcfg says compiled, library
	// missing/rejected) is loud, not a silent slowdown.
	{
		HorizonCode::compiledClasses().load(exeDir / HorizonCode::compiledLibraryName(),
		                                    HE_VERSION_STRING);
		if (m_config.horizonCodeCompiled && !HorizonCode::compiledClasses().loaded())
			Logger::Log(Logger::LogLevel::Warning,
				"GameApplication: project.hcfg says HorizonCode is compiled, but the "
				"HorizonCodeGen library is missing or was rejected — running interpreted");
	}

	// App-wide GameInstance: load its graph. Preferred source: packed into the
	// .hpak (ships with the same codec/encryption/bundle layout); fallback: a loose
	// GameInstance.hcode next to the exe (dev runs on loose content). Empty → an
	// empty but referenceable GameInstance.
	{
		std::string giJson;
		const auto giBytes = contentManager().readMountedEntry(sceneUuidForPath(kGameInstanceEntry));
		if (!giBytes.empty())
		{
			giJson.assign(giBytes.begin(), giBytes.end());
			Logger::Log(Logger::LogLevel::Info, "GameApplication: loaded packed GameInstance graph");
		}
		else if (std::ifstream gif(exeDir / "GameInstance.hcode"); gif)
		{
			giJson.assign(std::istreambuf_iterator<char>(gif), std::istreambuf_iterator<char>());
			Logger::Log(Logger::LogLevel::Info, "GameApplication: loaded loose GameInstance.hcode");
		}
		else
			Logger::Log(Logger::LogLevel::Warning,
				"GameApplication: no GameInstance graph found — app lifecycle/UI scripts will not run");
		// Compiled GameInstance takes precedence; the graph still ships in the
		// pak, so a fallback (or an older runtime) interprets it unchanged.
		if (auto compiled = HorizonCode::compiledClasses().create(kGameInstanceEntry))
		{
			m_gameInstance.runtime().setGameInstanceCompiled(std::move(compiled));
			Logger::Log(Logger::LogLevel::Info, "GameApplication: GameInstance running compiled");
		}
		else
			m_gameInstance.setGraph(giJson);
	}

	// The GameInstance's UI is APP-LEVEL: widgets live in m_widgets (owned here,
	// not by any world), so they exist before the first world and PERSIST across
	// scene switches — the world only holds the 3D scene. Wire it onto the app
	// runtime, then wire the runtime services (widget/object/engine calls) so the
	// GameInstance's OnInit can create UI. Services target m_widgets + the app
	// runtime directly (not m_world), which is what lets OnInit fire FIRST — before
	// any world exists.
	m_widgets.setRuntime(&m_gameInstance.runtime());
	{
		HorizonCode::Runtime::Services svc;
		svc.createWidget  = [this](const std::string& p){ return m_widgets.createWidget(contentManager(), p); };
		svc.showWidget    = [this](int id){ m_widgets.showWidget(id); };
		svc.hideWidget    = [this](int id){ m_widgets.hideWidget(id); };
		svc.destroyWidget = [this](int id){ m_widgets.destroyWidget(id); };
		svc.createObject  = [this](const std::string& p) -> uint32_t {
			// Compiled class first (the whole per-asset hybrid is this lookup);
			// miss → the interpreted asset path, unchanged.
			if (auto compiled = HorizonCode::compiledClasses().create(p))
			{
				const HorizonCode::InstanceId inst =
					m_gameInstance.runtime().addCompiled(std::move(compiled));
				m_gameInstance.runtime().fireEvent(inst, "Construct", 0);
				return inst;
			}
			const HE::UUID id = contentManager().loadAsset(p);
			const HorizonCodeClassAsset* a = contentManager().getHorizonCodeClass(id);
			if (!a) return 0u;
			HorizonCode::Graph g;
			if (!a->graphJson.empty()) HorizonCode::fromJson(a->graphJson, g);
			const HorizonCode::InstanceId inst = m_gameInstance.runtime().add(std::move(g));
			m_gameInstance.runtime().fireEvent(inst, "Construct", 0);
			return inst;
		};
		svc.destroyObject = [this](uint32_t ref){
			if (ref != 0 && ref != m_gameInstance.runtime().gameInstance())
				m_gameInstance.runtime().destroy(ref); // fires "Destruct"
		};
		// EngineCall nodes dispatch through the HE::api registry against the CURRENT
		// world (+ content) — resolved at call time, so it's null-tolerant while
		// OnInit runs before the world exists. No PhysicsWorld in the shipping
		// runtime yet → physics nodes no-op (null-Ctx tolerance).
		svc.callApi = [this](const std::string& id, const std::vector<HorizonCode::Value>& args)
			-> std::vector<HorizonCode::Value> {
			const HE::api::ApiFn* fn = HE::api::find(id);
			if (!fn) return {};
			HE::api::Ctx c{ m_world.get(), nullptr, &contentManager(), &m_audioEngine };
			return fn->invoke(c, args);
		};
		m_gameInstance.runtime().setServices(std::move(svc));
	}

	// GameInstance OnInit fires FIRST — before any world/scene. Its UI (m_widgets)
	// and objects are app-level, so they're up from frame one and survive scene
	// loads.
	m_gameInstance.fireInit();

	// Load the startup scene into a world and hand it to the renderer. The base
	// Application renders m_world each frame; OnRender ticks its gameplay systems.
	// The world borrows the app-level WidgetManager (setWidgetManager) so the
	// renderer + input see the GameInstance's UI, and a scene switch never clears it.
	m_world = std::make_unique<HorizonWorld>();
	// Widgets + the level script join the app-wide runtime (shared with the
	// GameInstance), so any scene script can Get Game Instance / bind its events.
	m_world->setScriptRuntime(&m_gameInstance.runtime());
	m_world->setWidgetManager(&m_widgets);   // borrow the app-level UI

	SceneSerializer serializer;
	bool sceneLoaded = false;
	if (m_config.hasPackedScene)
	{
		// Preferred: binary (CBOR) scene packed into the .hpak.
		const auto sceneBytes = contentManager().readMountedEntry(sceneUuid);
		if (!sceneBytes.empty() && serializer.loadFromMemory(*m_world, sceneBytes))
		{
			sceneLoaded = true;
			// Key the level script by the packed scene's UUID so a compiled one
			// (if the export shipped it) is picked up at fireLevelLoaded.
			m_world->setLevelScriptKey(levelScriptKeyForUuid(sceneUuid));
			Logger::Log(Logger::LogLevel::Info, "GameApplication: loaded packed startup scene");
		}
		else
			Logger::Log(Logger::LogLevel::Warning, "GameApplication: failed to load packed startup scene");
	}
	if (!sceneLoaded && !m_config.mainSceneName.empty())
	{
		// Fallback: loose .hescene (JSON) next to the executable.
		const std::filesystem::path scenePath = exeDir / m_config.mainSceneName;
		if (serializer.load(*m_world, scenePath, SerializeFormat::JSON))
			Logger::Log(Logger::LogLevel::Info, ("GameApplication: loaded scene " + m_config.mainSceneName).c_str());
		else
			Logger::Log(Logger::LogLevel::Warning, ("GameApplication: failed to load scene " + scenePath.string()).c_str());
	}
	setWorld(m_world.get());

	// Ensure a camera the free-fly controller can drive. A scene authored without
	// one otherwise renders through the extractor's fixed fallback camera, which
	// can't move — so the game would look frozen. Only added when the scene has
	// no camera at all; an authored camera is never overridden.
	{
		auto& reg = m_world->registry();
		bool hasCamera = false;
		for (auto e : reg.view<CameraComponent>()) { (void)e; hasCamera = true; break; }
		if (!hasCamera)
		{
			auto camE = m_world->createEntity("GameCamera");
			TransformComponent tc;
			tc.position = glm::vec3(0.0f, 2.0f, 8.0f); // back + up, looking toward -Z
			reg.emplace<TransformComponent>(camE, tc);
			CameraComponent cc; cc.isMain = true;
			reg.emplace<CameraComponent>(camE, cc);
			Logger::Log(Logger::LogLevel::Info,
				"GameApplication: added a default free-fly camera (scene had none)");
		}
	}

	// Player controller/character classes + input events: discover the project's
	// input assets, spawn the player instances on the shared runtime (Construct +
	// BeginPlay) and start pumping Tick/Input.* events (OnRender). After the scene
	// load so BeginPlay can reach scene entities through the engine-call API.
	m_playerHost.begin(m_gameInstance.runtime(), contentManager());

	// Audio: init the engine and start playOnStart sources, mirroring the editor's
	// play mode — packaged games get sound too (HC/script audio.* routes here).
	if (m_audioEngine.init())
		AudioSystem::playOnStart(*m_world, m_audioEngine, &contentManager());
	else
		Logger::Log(Logger::LogLevel::Warning,
			"GameApplication: audio device init failed — running silent");

	// fs/save sandbox: the per-user pref dir (never the install dir, which may be
	// read-only). All script/graph file I/O is jailed under <pref>/Saved.
	{
		const std::string org = "HorizonCreations";
		const std::string app = m_config.projectName.empty() ? "HorizonGame" : m_config.projectName;
		if (char* pref = SDL_GetPrefPath(org.c_str(), app.c_str()))
		{
			HE::api::fs::setSandboxRoot((std::filesystem::path(pref) / "Saved").string());
			SDL_free(pref);
		}
	}

	// Reference-graph streaming seed: kick off async loads for the assets this scene
	// actually references. Their baked transitive dependencies (materials → textures)
	// follow automatically via the frontier in pollAsyncResults, so the loader pulls
	// only the closure the scene needs — unused pak assets are never loaded. The
	// async UUID loader resolves from mounted paks first and falls back to the disk
	// registry, so this also works for a WIP build running on loose content.
	{
		const auto refs = SceneSystems::collectAssetRefs(*m_world);
		for (HE::UUID r : refs) contentManager().loadAssetAsync(r);
		Logger::Log(Logger::LogLevel::Info,
			("GameApplication: streaming " + std::to_string(refs.size()) +
			 " scene-referenced asset roots").c_str());
	}

	// Native C++ game logic: an optional GameLogic library next to the executable
	// (built from the game's C++ project). Once loaded, the base Application loop
	// ticks logic->onUpdate at the fixed timestep automatically. Absent library =
	// pure script game, no warning needed.
#ifdef _WIN32
	const auto logicPath = exeDir / "GameLogic.dll";
#elif defined(__APPLE__)
	const auto logicPath = exeDir / "GameLogic.dylib";
#else
	const auto logicPath = exeDir / "GameLogic.so";
#endif
	if (std::filesystem::exists(logicPath) && logicLoader().load(logicPath))
	{
		logicLoader().logic()->onStart(*m_world);
		Logger::Log(Logger::LogLevel::Info, "GameApplication: native game logic started");
	}

	// horizon.showCursor()/hideCursor(): scripts release/re-grab the mouse.
	ScriptApi::setCursorHook([this](bool show){ setMouseCaptured(!show); });

	// ECS gameplay scripts (Lua/Python): the packaged game drives them exactly like
	// the editor's play mode, so a shipped game behaves like PIE.
	startScripts();

	// Level script "OnLevelLoaded" fires once the world + scripts are up; the
	// matching "OnLevelUnloaded" fires at shutdown.
	if (m_world) m_world->fireLevelLoaded();
}

// ── Scene transitions ────────────────────────────────────────────────────────

bool GameApplication::loadSceneInto(HorizonWorld& world, const std::string& scenePath,
                                    bool additive, std::vector<entt::entity>* outCreated)
{
	SceneSerializer ser;
	// 1) Packed pak entry under the path-derived UUID (shipped builds).
	const HE::UUID pathUuid = sceneUuidForPath(scenePath);
	const auto bytes = contentManager().readMountedEntry(pathUuid);
	if (!bytes.empty())
	{
		const bool ok = additive ? ser.loadAdditiveFromMemory(world, bytes, outCreated)
		                         : ser.loadFromMemory(world, bytes);
		// A full load owns the world's level script — key it so fireLevelLoaded
		// can pick this scene's COMPILED level script when the export shipped one.
		if (ok && !additive) world.setLevelScriptKey(levelScriptKeyForUuid(pathUuid));
		return ok;
	}
	// 2) Loose JSON in the project (dev / WIP builds running on loose content),
	//    then 3) next to the executable. Scene paths are project-relative.
	std::filesystem::path candidates[2];
	int n = 0;
	if (!contentManager().contentRoot().empty())
		candidates[n++] = std::filesystem::path(contentManager().contentRoot()).parent_path() / scenePath;
	if (const char* base = SDL_GetBasePath())
		candidates[n++] = std::filesystem::path(base) / scenePath;
	std::error_code ec;
	for (int i = 0; i < n; ++i)
		if (std::filesystem::exists(candidates[i], ec))
			return additive ? ser.loadAdditive(world, candidates[i], SerializeFormat::JSON, outCreated)
			                : ser.load(world, candidates[i], SerializeFormat::JSON);
	return false;
}

bool GameApplication::performSceneSwitch(const std::string& scenePath)
{
	// Build the NEW world first: a failed load must leave the running scene
	// untouched (no half-torn-down state).
	auto newWorld = std::make_unique<HorizonWorld>();
	if (!loadSceneInto(*newWorld, scenePath, /*additive=*/false, nullptr))
	{
		Logger::Log(Logger::LogLevel::Warning,
			("GameApplication: scene.load failed — '" + scenePath + "' not found "
			 "(packed entry, project file, exe dir)").c_str());
		return false;
	}
	swapToWorld(std::move(newWorld), scenePath);
	return true;
}

void GameApplication::swapToWorld(std::unique_ptr<HorizonWorld> newWorld, const std::string& label)
{
	// Tear down the old scene: unload event first (handlers still see the world),
	// then scripts (finalizers may touch entities), sounds, zones. The app-level
	// UI (m_widgets, the GameInstance's widgets) is deliberately NOT cleared — it
	// persists across scene switches (a HUD created in OnInit stays up).
	if (m_world)
		m_world->fireLevelUnloaded();
	m_scriptContext.reset();
	m_scriptInstances.clear();
	m_audioEngine.stopAll();
	HE::api::scene::clearZones();

	// Swap + bring the new scene up exactly like OnInit does for the startup scene.
	m_world = std::move(newWorld);
	m_world->setWidgetManager(&m_widgets);   // keep the app-level UI on the new world
	m_world->setScriptRuntime(&m_gameInstance.runtime());
	setWorld(m_world.get());

	auto& reg = m_world->registry();
	bool hasCamera = false;
	for (auto e : reg.view<CameraComponent>()) { (void)e; hasCamera = true; break; }
	if (!hasCamera)
	{
		auto camE = m_world->createEntity("GameCamera");
		TransformComponent tc; tc.position = glm::vec3(0.0f, 2.0f, 8.0f);
		reg.emplace<TransformComponent>(camE, tc);
		CameraComponent cc; cc.isMain = true;
		reg.emplace<CameraComponent>(camE, cc);
	}

	if (m_audioEngine.isInitialized())
		AudioSystem::playOnStart(*m_world, m_audioEngine, &contentManager());

	// Seamlessness comes from the async streaming pipeline: the swap itself is a
	// cheap main-thread deserialize; meshes/textures stream in the background.
	const auto refs = SceneSystems::collectAssetRefs(*m_world);
	for (HE::UUID r : refs) contentManager().loadAssetAsync(r);

	startScripts();
	m_world->fireLevelLoaded();
	Logger::Log(Logger::LogLevel::Info,
		("GameApplication: switched to scene '" + label + "' ("
		 + std::to_string(refs.size()) + " asset roots streaming)").c_str());
}

void GameApplication::executeSceneRequests()
{
	const auto requests = HE::api::scene::takeRequests();
	for (const auto& r : requests)
	{
		if (r.kind == 0)      // full switch — or, hidden, a background PRELOAD
		{
			if (!r.hidden) { performSceneSwitch(r.path); continue; }
			auto pending = std::make_unique<HorizonWorld>();
			if (!loadSceneInto(*pending, r.path, /*additive=*/false, nullptr))
			{
				Logger::Log(Logger::LogLevel::Warning,
					("GameApplication: scene.load (hidden) failed — '" + r.path + "' not found").c_str());
				continue;
			}
			// Warm the pending scene's assets NOW so the later activate() swap
			// presents without a streaming pop.
			const auto refs = SceneSystems::collectAssetRefs(*pending);
			for (HE::UUID ar : refs) contentManager().loadAssetAsync(ar);
			if (m_pendingWorld)
				Logger::Log(Logger::LogLevel::Warning,
					("GameApplication: replacing pending scene '" + m_pendingScenePath + "'").c_str());
			m_pendingWorld     = std::move(pending);
			m_pendingScenePath = r.path;
			HE::api::scene::notePendingLevel(true);
			Logger::Log(Logger::LogLevel::Info,
				("GameApplication: preloaded scene '" + r.path + "' ("
				 + std::to_string(refs.size()) + " asset roots streaming) — awaiting activate").c_str());
		}
		else if (r.kind == 3) // activate the preloaded level
		{
			if (!m_pendingWorld)
			{
				Logger::Log(Logger::LogLevel::Warning,
					"GameApplication: scene.activate with no pending scene (load hidden first)");
				continue;
			}
			swapToWorld(std::move(m_pendingWorld), m_pendingScenePath);
			m_pendingScenePath.clear();
			HE::api::scene::notePendingLevel(false);
		}
		else if (r.kind == 1) // additive zone
		{
			if (!m_world) continue;
			std::vector<entt::entity> created;
			if (!loadSceneInto(*m_world, r.path, /*additive=*/true, &created))
			{
				Logger::Log(Logger::LogLevel::Warning,
					("GameApplication: scene.loadAdditive failed — '" + r.path + "' not found").c_str());
				continue;
			}
			// Register the zone centrally (queries/show/hide/position work off it).
			// Root = the merged scene's fresh sub-root: the created entity parented
			// directly under the world root.
			HE::api::scene::ZoneInfo info;
			info.path = r.path;
			info.entities.reserve(created.size());
			for (entt::entity e : created) info.entities.push_back((uint32_t)e);
			for (entt::entity e : created)
			{
				const auto* h = m_world->registry().try_get<HierarchyComponent>(e);
				if (h && h->parent == m_world->rootEntity()) { info.root = (uint32_t)e; break; }
			}
			if (info.root == 0 && !created.empty()) info.root = (uint32_t)created.front();
			HE::api::scene::noteZoneLoaded(r.zone, std::move(info));

			HE::api::Ctx c{ m_world.get(), nullptr, &contentManager(), &m_audioEngine };
			// Placement: move the zone's root to the requested position (zero =
			// as authored; the merge root is a fresh identity entity).
			if (r.pos != glm::vec3(0.0f))
				HE::api::scene::setZonePosition(c, r.zone, r.pos);
			// Hidden zones load with their renderables invisible until Show Zone.
			if (r.hidden)
				HE::api::scene::setZoneVisible(c, r.zone, false);

			// Stream the merged zone's assets + start its ECS scripts. playOnStart
			// audio is deliberately NOT re-fired (it would restart existing
			// sources); zone audio starts from its scripts/graphs.
			const auto refs = SceneSystems::collectAssetRefs(*m_world);
			for (HE::UUID ar : refs) contentManager().loadAssetAsync(ar);
			const int started = startScriptsFor(created);
			Logger::Log(Logger::LogLevel::Info,
				("GameApplication: zone " + std::to_string(r.zone) + " loaded ('" + r.path +
				 "', " + std::to_string(created.size()) + " entities, " +
				 std::to_string(started) + " scripts" + (r.hidden ? ", hidden" : "") + ")").c_str());
		}
		else if (r.kind == 4) // show/hide a zone (queued so it orders after a load)
		{
			if (!m_world) continue;
			HE::api::Ctx c{ m_world.get(), nullptr, &contentManager(), &m_audioEngine };
			HE::api::scene::setZoneVisible(c, r.zone, r.flag);
		}
		else if (r.kind == 5) // move a zone (queued so it orders after a load)
		{
			if (!m_world) continue;
			HE::api::Ctx c{ m_world.get(), nullptr, &contentManager(), &m_audioEngine };
			HE::api::scene::setZonePosition(c, r.zone, r.pos);
		}
		else if (r.kind == 2) // unload additive zone
		{
			const HE::api::scene::ZoneInfo* z = HE::api::scene::zoneInfo(r.zone);
			if (!z || !m_world) continue;
			auto& reg = m_world->registry();
			int gone = 0;
			for (uint32_t id : z->entities)
			{
				const auto e = (entt::entity)id;
				if (!reg.valid(e)) continue;
				// Drop the per-entity script instance before the entity dies.
				m_scriptInstances.erase(id);
				ScriptApi::destroy(*m_world, id);
				++gone;
			}
			HE::api::scene::noteZoneUnloaded(r.zone);
			Logger::Log(Logger::LogLevel::Info,
				("GameApplication: zone " + std::to_string(r.zone) + " unloaded ("
				 + std::to_string(gone) + " entities)").c_str());
		}
	}
}

int GameApplication::startScriptsFor(const std::vector<entt::entity>& entities)
{
	if (!m_world || !m_scriptContext) return 0;
	auto& reg = m_world->registry();
	int started = 0;
	for (entt::entity entity : entities)
	{
		if (!reg.valid(entity)) continue;
		auto* sc = reg.try_get<ScriptComponent>(entity);
		if (!sc || !sc->enabled) continue;
		const ScriptAsset* asset = contentManager().getScript(sc->scriptAssetId);
		if (!asset || asset->sourceCode.empty()) continue;
		if (!m_scriptContext->isScriptLoaded(sc->moduleName, asset->language))
			m_scriptContext->loadScript(sc->moduleName, asset->sourceCode, asset->language);
		auto instId = m_scriptContext->createInstance(sc->moduleName, entity, asset->language);
		if (instId == ScriptEngine::kInvalidInstance) continue;
		m_scriptContext->injectProperties(instId, sc->properties);
		m_scriptContext->callOnStart(instId);
		m_scriptInstances[static_cast<uint32_t>(entity)] = instId;
		++started;
	}
	return started;
}

void GameApplication::startScripts()
{
	if (!m_world) return;
	m_scriptContext = std::make_unique<ScriptContext>(*m_world);
	// No PhysicsWorld in the shipping runtime yet → raycast/velocity/isGrounded
	// no-op; onStart/onUpdate + horizon.setMaterialParam work.
	m_scriptContext->setContentManager(&contentManager());

	int started = 0;
	auto& reg = m_world->registry();
	for (auto [entity, sc] : reg.view<ScriptComponent>().each())
	{
		if (!sc.enabled) continue;
		const ScriptAsset* asset = contentManager().getScript(sc.scriptAssetId);
		if (!asset || asset->sourceCode.empty()) continue;
		if (!m_scriptContext->isScriptLoaded(sc.moduleName, asset->language))
			m_scriptContext->loadScript(sc.moduleName, asset->sourceCode, asset->language);
		auto instId = m_scriptContext->createInstance(sc.moduleName, entity, asset->language);
		if (instId == ScriptEngine::kInvalidInstance) continue;
		m_scriptContext->injectProperties(instId, sc.properties);
		m_scriptContext->callOnStart(instId);
		m_scriptInstances[static_cast<uint32_t>(entity)] = instId;
		++started;
	}
	if (started > 0)
		Logger::Log(Logger::LogLevel::Info,
			("GameApplication: started " + std::to_string(started) + " ECS script(s)").c_str());
}

void GameApplication::updateScripts(float dt)
{
	if (!m_scriptContext || dt <= 0.0f) return;
	HE_PROFILE_SCOPE_N("ScriptUpdate");
	for (auto& [entityId, instId] : m_scriptInstances)
		m_scriptContext->callOnUpdate(instId, dt);
}

void GameApplication::updateUIInput()
{
	if (!m_world) return;

	SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr;
	float mx = 0.0f, my = 0.0f;
	const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);

	// The UI pass renders at drawable resolution; SDL reports the mouse in
	// window points — rescale (HiDPI).
	int ww = 1, wh = 1, pw = 1, ph = 1;
	if (w)
	{
		SDL_GetWindowSize(w, &ww, &wh);
		SDL_GetWindowSizeInPixels(w, &pw, &ph);
	}
	const float sx = ww > 0 ? static_cast<float>(pw) / ww : 1.0f;
	const float sy = wh > 0 ? static_cast<float>(ph) / wh : 1.0f;

	// While the fly-look holds the mouse captive there is no visible cursor —
	// hover states clear and nothing is clickable (Esc releases the mouse).
	const bool pointerValid = !m_mouseCaptured && w != nullptr;

	// Widget pointer input first — widgets draw on top of entity UI.
	m_world->widgets().processPointer(static_cast<float>(pw), static_cast<float>(ph),
	                                  mx * sx, my * sy,
	                                  (buttons & SDL_BUTTON_LMASK) != 0, pointerValid);

	// Show the cursor the hovered widget element requested (default = arrow).
	if (pointerValid) HE::applyUICursor(m_world->widgets().hoverCursor());

	std::vector<UIInputSystem::PointerEvent> events;
	UIInputSystem::update(*m_world, m_uiInput,
	                      static_cast<float>(pw), static_cast<float>(ph),
	                      mx * sx, my * sy,
	                      (buttons & SDL_BUTTON_LMASK) != 0, pointerValid,
	                      events);

	if (!m_scriptContext) return;
	for (const auto& ev : events)
	{
		auto it = m_scriptInstances.find(ev.entity);
		if (it == m_scriptInstances.end()) continue;
		const UIScriptEvent se =
			ev.type == UIInputSystem::PointerEvent::Type::Click ? UIScriptEvent::Click :
			ev.type == UIInputSystem::PointerEvent::Type::HoverEnter ? UIScriptEvent::HoverEnter
			                                                         : UIScriptEvent::HoverExit;
		m_scriptContext->callOnUIEvent(it->second, se);
	}
}

void GameApplication::setMouseCaptured(bool captured)
{
	m_mouseCaptured = captured;
	SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr;
	if (!w) return;
	// Relative mode alone doesn't reliably hide the OS cursor on SDL3/macOS,
	// so drive the cursor visibility explicitly (mirrors the editor's fly-look).
	SDL_SetWindowRelativeMouseMode(w, captured);
	if (captured)
	{
		SDL_HideCursor();
		// Drop any relative motion accumulated while released so the first
		// look-frame after (re)capture doesn't jump by a stale delta.
		SDL_GetRelativeMouseState(nullptr, nullptr);
	}
	else SDL_ShowCursor();
}

void GameApplication::updateCameraController(float dt)
{
	if (!m_mouseCaptured || !m_world || dt <= 0.0f) return;
	auto& reg = m_world->registry();

	// The scene's main camera (prefer isMain; else the first camera found).
	entt::entity cam = entt::null;
	for (auto [e, t, c] : reg.view<TransformComponent, CameraComponent>().each())
	{
		if (cam == entt::null) cam = e;
		if (c.isMain) { cam = e; break; }
	}
	if (cam == entt::null) return; // nothing to drive (fallback camera is fixed)

	auto& t = reg.get<TransformComponent>(cam);

	// Mouse look from the relative motion accumulated since last frame.
	float dx = 0.0f, dy = 0.0f;
	SDL_GetRelativeMouseState(&dx, &dy);

	// Park the cursor back at the window centre after each frame's relative motion.
	// With relative mode engaged this is a pure internal position update (no motion
	// events); when it is NOT engaged (focus transition, platform quirk) the OS cursor
	// physically drifts and would stall the look at the screen edge — the warp keeps it
	// centred either way. SDL pre-sets last_x/last_y to the warp target, so the warp
	// never pollutes the relative accumulator. Skipped while unfocused (alt-tabbed) so
	// we never yank the cursor away from another app.
	if (SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr)
	{
		if (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)
		{
			int ww = 0, wh = 0;
			SDL_GetWindowSize(w, &ww, &wh);
			SDL_WarpMouseInWindow(w, ww * 0.5f, wh * 0.5f);
		}
	}

	constexpr float kSensitivity = 0.12f; // degrees per pixel
	t.rotation.y -= dx * kSensitivity;    // yaw
	t.rotation.x -= dy * kSensitivity;    // pitch
	t.rotation.x = std::clamp(t.rotation.x, -89.0f, 89.0f);

	// Movement along the camera's own axes (rotation matches SceneGraph: the
	// worldMatrix is built from glm::quat(radians(rotation))).
	const glm::quat q = glm::quat(glm::radians(t.rotation));
	const glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, -1.0f);
	const glm::vec3 right   = q * glm::vec3(1.0f, 0.0f, 0.0f);
	const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);

	Input& in = input();
	glm::vec3 move(0.0f);
	if (in.IsKeyDown(SDL_SCANCODE_W)) move += forward;
	if (in.IsKeyDown(SDL_SCANCODE_S)) move -= forward;
	if (in.IsKeyDown(SDL_SCANCODE_D)) move += right;
	if (in.IsKeyDown(SDL_SCANCODE_A)) move -= right;
	if (in.IsKeyDown(SDL_SCANCODE_E) || in.IsKeyDown(SDL_SCANCODE_SPACE)) move += worldUp;
	if (in.IsKeyDown(SDL_SCANCODE_Q) || in.IsKeyDown(SDL_SCANCODE_LCTRL)) move -= worldUp;

	if (glm::dot(move, move) > 0.0f)
	{
		float speed = 6.0f; // units/sec
		if (in.IsKeyDown(SDL_SCANCODE_LSHIFT) || in.IsKeyDown(SDL_SCANCODE_RSHIFT)) speed *= 3.0f;
		t.position += glm::normalize(move) * speed * dt;
	}
	t.dirty = true;
}

bool GameApplication::OnEvent(const SDL_Event& event)
{
	// OS window focus → GameInstance OnWindowFocusChanged (while running).
	if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)      m_gameInstance.setWindowFocus(true);
	else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)   m_gameInstance.setWindowFocus(false);

	// A focused in-game text field owns the keyboard: route text + edit keys to
	// the widget and swallow them so they don't drive the camera/gameplay.
	if (m_world && m_world->widgets().hasFocusedTextField())
	{
		if (event.type == SDL_EVENT_TEXT_INPUT)
		{
			m_world->widgets().inputText(event.text.text);
			return true;
		}
		if (event.type == SDL_EVENT_KEY_DOWN)
		{
			if (event.key.key == SDLK_BACKSPACE) { m_world->widgets().inputBackspace(); return true; }
			if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
				{ m_world->widgets().inputSubmit(); return true; }
			if (event.key.key != SDLK_ESCAPE) return true; // swallow other keys while typing
		}
	}

	if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
	{
		// V: static VSync toggle for packaged builds (no settings menu yet).
		if (event.key.key == SDLK_V)
		{
			m_vsyncOn = !m_vsyncOn;
			setVSync(m_vsyncOn);
			Logger::Log(Logger::LogLevel::Info,
				m_vsyncOn ? "GameApplication: VSync ON" : "GameApplication: VSync OFF");
			return true;
		}
		// Esc: release/re-grab the mouse.
		if (event.key.key == SDLK_ESCAPE)
		{
			setMouseCaptured(!m_mouseCaptured);
			return true;
		}
	}
	return false;
}

// Push the current SDL keyboard/mouse state into HE::api::input so the input.*
// registry nodes and scripts can poll it. Mouse delta + scroll are left at 0 here
// to avoid consuming SDL's relative-motion accumulator the camera controller uses;
// position + buttons + keys (by SDL scancode name, e.g. "W"/"Space") are polled.
static void pushEngineInputSnapshot()
{
	int n = 0;
	const bool* ks = SDL_GetKeyboardState(&n);
	std::vector<std::string> down;
	if (ks)
		for (int sc = 0; sc < n; ++sc)
			if (ks[sc]) { const char* name = SDL_GetScancodeName((SDL_Scancode)sc); if (name && name[0]) down.emplace_back(name); }
	float mx = 0.0f, my = 0.0f;
	const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
	uint32_t buttons = 0;
	if (mb & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))   buttons |= 1u << 0;
	if (mb & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))  buttons |= 1u << 1;
	if (mb & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) buttons |= 1u << 2;
	HE::api::input::setMouse({ mx, my }, { 0.0f, 0.0f }, buttons, 0.0f);
	HE::api::input::setKeysDown(down);
}

void GameApplication::OnRender(float deltaTime)
{
	// Feed the per-frame engine clock + input snapshot so time.*/input.* nodes and
	// scripts read fresh values this frame (before the ECS/script updates below).
	HE::api::time::advance(deltaTime);
	pushEngineInputSnapshot();

	// Deferred scene transitions requested by scripts/graphs last frame: executed
	// at the frame START so nothing downstream touches a half-swapped world.
	executeSceneRequests();

	// Timed debug primitives (debug.* nodes/scripts) → the renderer's line pass.
	{
		std::vector<DebugLine> dbg;
		HE::api::debug::collect(deltaTime, dbg);
		if (renderer()) renderer()->SetDebugLines(dbg);
	}

	// Register assets that finished streaming since last frame (main-thread insert —
	// safe point for the SlotMaps, never during draw). Budgeted so a burst of
	// simultaneously-finished loads is spread across frames instead of freezing one;
	// the rest stay queued for the next frame. Cheap no-op once fully streamed in.
	constexpr size_t kStreamRegistrationsPerFrame = 16;
	const std::vector<HE::UUID> justRegistered =
		contentManager().pollAsyncResults(kStreamRegistrationsPerFrame);
	// Warm up node-graph material pipelines the moment their material becomes
	// resident — building the pipeline here (before the material is first drawn)
	// keeps the first frame that shows it from stalling on a synchronous
	// cross-compile inside the encoder loop. Non-material ids are skipped.
	if (!justRegistered.empty() && renderer())
		renderer()->WarmupMaterials(justRegistered);

	// Built-in free-fly camera (mouse look + WASD) so the game is navigable —
	// runs before the systems tick so LOD/particles follow the new camera pos.
	updateCameraController(deltaTime);

	// Per-frame ECS script update (Lua/Python onUpdate), before the systems tick so
	// script-driven transforms/params are reflected the same frame.
	updateScripts(deltaTime);

	// Keep the audio listener + spatial sources tracking their entities.
	if (m_world && m_audioEngine.isInitialized())
		AudioSystem::updateSpatial(*m_world, m_audioEngine);

	// Live widgets: per-frame logic tick (EventTick).
	if (m_world) m_world->widgets().tick(deltaTime);

	// Player instances: Tick + Input.<Action>.* events (mapping ticked against
	// the app Input state, which ProcessEvent keeps current).
	m_playerHost.tick(input(), deltaTime);

	// Latent HorizonCode flow (Delay nodes): resume expired continuations on
	// the app-wide runtime (GameInstance + widgets + level + objects share it).
	m_gameInstance.runtime().update(deltaTime);

	// In-game UI pointer input (hover/click on buttons + scripted elements).
	updateUIInput();

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
		const bool gpuParticles = GlobalState::getInstance().getCustomConfigBool("GpuParticles", true) &&
		                          renderer()->GetCapabilities().supportsGpuParticles;
		HE_PROFILE_SCOPE_N("SceneSystemsTick");
		SceneSystems::tick(*m_world, contentManager(), renderer(), camPos, deltaTime,
		                   nullptr, gpuParticles);

		// Push the scene environment to the renderer. The base Application renders the
		// world but never pushes EnvironmentSettings (that lived only in the editor), so
		// without this the weather sky / clouds / fog / flash would not show in-game.
		// The Sky is a scene entity now — no Sky entity → skip the sky pass.
		const Entity gEnvEntity = m_world->environmentEntity();
		auto* env = (gEnvEntity == entt::null)
			? nullptr : m_world->registry().try_get<EnvironmentComponent>(gEnvEntity);
		if (!env)
			renderer()->SetEnvironmentSettings(IRenderer::EnvironmentSettings{ .skyEnabled = false });
		if (env)
		{
			if (env->dayNightCycle && env->autoAdvance && deltaTime > 0.0f)
			{
				const float dayFrac = deltaTime / std::max(env->cycleSeconds, 1.0f);
					if (env->moonPhaseAuto) { env->moonPhase += dayFrac / std::max(env->moonCycleDays, 0.1f); env->moonPhase -= std::floor(env->moonPhase); }
				env->timeOfDay += dayFrac;
				env->timeOfDay -= std::floor(env->timeOfDay);
			}
			renderer()->SetEnvironmentSettings(IRenderer::EnvironmentSettings{
				.dayNightCycle = env->dayNightCycle, .timeOfDay = env->timeOfDay,
				.sunColor = env->sunColor, .sunIntensity = env->sunIntensity,
				.moonColor = env->moonColor, .moonIntensity = env->moonIntensity, .moonPhase = env->moonPhase,
				.cloudCoverage = env->cloudCoverage,
				.fogDensity = env->fogDensity, .fogHeightFalloff = env->fogHeightFalloff,
				.auroraIntensity = env->auroraIntensity,
				.milkyWayIntensity = env->milkyWayIntensity, .nebulaIntensity = env->nebulaIntensity,
				.nebulaColor = env->nebulaColor, .nebulaColor2 = env->nebulaColor2,
				.nebulaColor3 = env->nebulaColor3, .nebulaSeed = env->nebulaSeed,
				.nebulaCoverage = env->nebulaCoverage,
				.nebulaQuality = env->nebulaQuality,
				.auroraColor = env->auroraColor,
				.auroraColorTop = env->auroraColorTop,
				.auroraHeight = env->auroraHeight, .auroraFragmentation = env->auroraFragmentation,
				.windDirection = env->windDirection, .windSpeed = env->windSpeed, .flash = env->flash,
				.wetness = env->wetness, .snowAmount = env->snowAmount, .rainAmount = env->rainAmount,
				.cloudMode = env->cloudMode, .cloudHeight = env->cloudHeight,
				.cloudQuality = env->cloudQuality, .lowResClouds = env->lowResClouds,
				.cloudDensity = env->cloudDensity, .cloudFluffiness = env->cloudFluffiness,
				.cloudTint = env->cloudTint,
				.contrailAmount = env->contrailAmount,
				.cirrusAmount = env->cirrusAmount, .cirrusSeed = env->cirrusSeed,
				.godRays = env->godRays, .shootingStars = env->shootingStars, .lensFlare = env->lensFlare,
				.starBrightness = env->starBrightness, .starColor = env->starColor,
				.starSize = env->starSize, .starSizeVariation = env->starSizeVariation,
				.starGlow = env->starGlow, .starTwinkle = env->starTwinkle,
				.starDensity = env->starDensity});
		}
	}
}

void GameApplication::OnShutdown()
{
	// Stop audio first: sounds reference asset PCM the ContentManager owns.
	m_audioEngine.shutdown();

	// Level script "OnLevelUnloaded" runs while the world is still alive (the
	// world's destructor is default and never calls clear(), so fire it here).
	if (m_world) m_world->fireLevelUnloaded();

	// Player instances go down before the GameInstance (their Destruct may still
	// reference it), symmetric to being spawned after its OnInit.
	m_playerHost.end();

	// GameInstance OnShutdown fires last (symmetric to OnInit firing first).
	m_gameInstance.fireShutdown();

	// Clear the app-level UI while the runtime it registered on is still alive
	// (fires each widget's Destruct). It's a member destroyed before m_gameInstance
	// anyway, but the destructor doesn't fire Destruct — do it explicitly.
	m_widgets.clear();

	// Tear down ECS scripts before the world (their finalizers may touch entities).
	m_scriptContext.reset();
	m_scriptInstances.clear();

	// Stop + unload native game logic before the world is torn down.
	if (m_world && logicLoader().isLoaded())
		logicLoader().unload(*m_world);
	Logger::Log(Logger::LogLevel::Info, "GameApplication::OnShutdown");
}
