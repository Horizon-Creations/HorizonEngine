#include "GameApplication.h"
#include <cstdint>
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
#include <HorizonScene/CollisionSystem.h>
#include <DebugDraw/DebugDraw.h>     // DebugLine (HE::api::debug drain)
#include <Hpak/ProjectExporter.h>    // sceneUuidForPath (packed scene lookup)
#include <HorizonCode/HcCompiledLoader.h> // compiled HorizonCode classes (hybrid)
#include <HorizonCode/HcClassResolve.h>
#include "HorizonVersion.h"          // HE_VERSION_STRING (compiled-classes handshake)
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/ScriptApi.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/EnvironmentPush.h>      // makeEnvironmentSettings (shared with the editor)
#include <HorizonScene/FlyCameraController.h>  // free-fly camera (shared with the editor's PIE)
#include <HorizonScene/CameraRigController.h>  // first/third person rig (shared with the editor's PIE)
#include <Scripting/ScriptTypes.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/TypeRegistry.h>      // eager struct/enum registration (type index)
#include <nlohmann/json.hpp>
#include <Types/UUID.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <filesystem>

// File-local alias. It used to arrive transitively from the public
// HorizonRendering/ShaderManager.h, which declared it at global scope and so
// leaked `fs` into every consumer of that header.
namespace fs = std::filesystem;


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
	cfg.backend = HE::RendererBackend::Metal;
#else
	cfg.backend = HE::RendererBackend::OpenGL;
#endif
	return cfg;
}

std::unique_ptr<IRenderer> GameApplication::CreateRenderer()
{
	const auto backend = GetConfig().backend;
	HE_LOG_INFO(Core, "%s", "GameApplication: creating renderer");
	return RendererFactory::Create(backend);
}

void GameApplication::OnInit()
{
	HE_LOG_INFO(Core, "%s", "GameApplication::OnInit");

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
		HE_LOG_WARN(Core, "%s", "GameApplication: SDL_GetBasePath returned null");
		return;
	}
	const std::filesystem::path exeDir(baseRaw);

	if (!ProjectConfigLoader::load(exeDir, m_config))
	{
		HE_LOG_INFO(Core, "%s", "GameApplication: no project.hcfg — running without pak");
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
		HE_LOG_INFO(Core, "%s",
			("GameApplication: mounted " + m_config.hpakFilename).c_str());

		// Mod overlays: every .hpak in Mods/ next to the executable, mounted on
		// top of the base pak in alphabetical order. Same UUID = replacement,
		// new UUID = addition — this also lets a mod override the packed startup
		// scene, which is why mods mount BEFORE the scene is read below.
		if (m_config.enableModSupport)
		{
			const size_t mods = contentManager().mountPakOverlays(exeDir / "Mods");
			if (mods > 0)
				HE_LOG_INFO(Core, "%s",
					("GameApplication: mounted " + std::to_string(mods) + " mod pak(s)").c_str());
		}
	}
	else
		HE_LOG_WARN(Core, "%s", ("GameApplication: pak not found: " + pakPath).c_str());

	// User-defined types (Struct/Enum assets): register their definitions in the
	// process-global TypeRegistry BEFORE anything scripts — the Lua/Python
	// bootstrap generates horizon.enums/horizon.structs from it, and graph type
	// pins resolve against it. Packed builds read the exporter's type index
	// (pak entries are UUID-keyed, on-demand streaming would never "happen to"
	// touch a definition); loose dev runs walk the content dir.
	{
		size_t types = 0;
		const auto idxBytes = contentManager().readMountedEntry(sceneUuidForPath(kTypeIndexEntry));
		if (!idxBytes.empty())
		{
			const auto idx = nlohmann::json::parse(idxBytes.begin(), idxBytes.end(),
			                                       nullptr, /*allow_exceptions=*/false);
			if (idx.is_array())
				for (const auto& e : idx)
					if (e.is_string() &&
					    !(contentManager().loadAsset(e.get<std::string>()) == HE::UUID{}))
						++types;
		}
		else
			types = HE::TypeRegistry::refreshFromContent(contentManager());
		if (types)
			HE_LOG_INFO(Core, "%s",
				("GameApplication: registered " + std::to_string(types) +
				 " user type definitions").c_str());
	}

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
			HE_LOG_WARN(Core, "%s",
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
			HE_LOG_INFO(Core, "%s", "GameApplication: loaded packed GameInstance graph");
		}
		else if (std::ifstream gif(exeDir / "GameInstance.hcode"); gif)
		{
			giJson.assign(std::istreambuf_iterator<char>(gif), std::istreambuf_iterator<char>());
			HE_LOG_INFO(Core, "%s", "GameApplication: loaded loose GameInstance.hcode");
		}
		else
			HE_LOG_WARN(Core, "%s",
				"GameApplication: no GameInstance graph found — app lifecycle/UI scripts will not run");
		// Compiled GameInstance takes precedence; the graph still ships in the
		// pak, so a fallback (or an older runtime) interprets it unchanged.
		if (auto compiled = HorizonCode::compiledClasses().create(kGameInstanceEntry))
		{
			m_gameInstance.runtime().setGameInstanceCompiled(std::move(compiled));
			HE_LOG_INFO(Core, "%s", "GameApplication: GameInstance running compiled");
		}
		else
			m_gameInstance.setGraph(giJson);
	}

	// fs/save sandbox: the per-user pref dir (never the install dir, which may be
	// read-only). All script/graph file I/O is jailed under <pref>/Saved. This
	// MUST happen before fireInit and the player host's BeginPlay below — "load
	// the save on startup" is the most common save pattern, and with an empty
	// root every fs/save call silently no-ops.
	{
		const std::string org = "HorizonCreations";
		const std::string app = m_config.projectName.empty() ? "HorizonGame" : m_config.projectName;
		if (char* pref = SDL_GetPrefPath(org.c_str(), app.c_str()))
		{
			HE::api::fs::setSandboxRoot((std::filesystem::path(pref) / "Saved").string());
			SDL_free(pref);
		}
	}
	// Savegames: the shipped game IS play mode, and save.create() resolves the
	// project's default template from the hcfg.
	HE::api::save::setDefaultTemplate(m_config.defaultSaveTemplate);
	HE::api::save::setPlayMode(true);

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
			// An Entity class has a BODY, so it goes through the host that gives
			// it one — before the compiled shortcut below, because it needs that
			// body whichever backend ends up serving its logic (EntityHost::bind
			// consults the compiled table itself). Creating it here instead would
			// produce a half-object: answers a Cast to Entity, owns no entity,
			// never ticks.
			if (m_entityHost.running())
				if (const HorizonCodeClassAsset* ea =
				        contentManager().getHorizonCodeClass(contentManager().loadAsset(p)))
				{
					// The RESOLVED engine base: a class deriving from another class
					// that is an Entity is one too.
					const HorizonCode::ResolvedClass rc =
						HorizonCode::resolveClassAsset(contentManager(), ea->path);
					if (HorizonCode::engineClassIsA(rc.engineBase, "Entity"))
						return m_entityHost.spawn(ea->path).instance;
				}
			// Compiled class first (the whole per-asset hybrid is this lookup);
			// miss → the interpreted asset path, unchanged.
			if (auto compiled = HorizonCode::compiledClasses().create(p))
			{
				const HorizonCode::InstanceId inst =
					m_gameInstance.runtime().addCompiled(std::move(compiled));
				m_gameInstance.runtime().fireConstruct(inst);
				return inst;
			}
			const HE::UUID id = contentManager().loadAsset(p);
			const HorizonCodeClassAsset* a = contentManager().getHorizonCodeClass(id);
			if (!a) return 0u;
			// The asset's OWN path is the class key, matching what the compiled
			// branch above gets from classKey() — so one class stays one class
			// to a Cast no matter which backend served this instance. The graph
			// is the FLATTENED one: this class plus what it inherits.
			HorizonCode::ResolvedClass rc =
				HorizonCode::resolveClassAsset(contentManager(), a->path);
			const HorizonCode::InstanceId inst = m_gameInstance.runtime().addLevels(
				std::move(rc.levels), {}, { a->path, rc.engineBase, rc.chain });
			m_gameInstance.runtime().fireConstruct(inst);
			return inst;
		};
		svc.destroyObject = [this](uint32_t ref){
			auto& rt = m_gameInstance.runtime();
			if (ref == 0 || ref == rt.gameInstance()) return;
			// An Entity class owns a body; destroying the object takes it too, or
			// a mesh without logic stays standing in the scene. Read the entity
			// BEFORE the instance goes and destroy it AFTER, so Destruct can
			// still reach its own entity.
			const uint32_t owned = rt.ownedEntity(ref);
			rt.destroy(ref); // fires "Destruct"
			if (owned != 0 && m_world &&
			    m_world->registry().valid(static_cast<Entity>(owned)))
				m_world->destroyEntity(static_cast<Entity>(owned));
		};
		// EngineCall nodes dispatch through the HE::api registry against the CURRENT
		// world, physics and content — all resolved at CALL time, which is what
		// lets this be bound before OnInit has built any of them, and what makes a
		// scene switch (which replaces both the world and the physics world)
		// transparent to every graph. Both are still null while the GameInstance's
		// OnInit runs, and the null-Ctx tolerance covers that.
		svc.callApi = [this](HorizonCode::InstanceId self, const std::string& id,
		                     const std::vector<HorizonCode::Value>& args)
			-> std::vector<HorizonCode::Value> {
			const HE::api::ApiFn* fn = HE::api::find(id);
			if (!fn) return {};
			// The caller travels along — see the editor's twin.
			HE::api::Ctx c{ m_world.get(), m_physicsWorld.get(), &contentManager(), &m_audioEngine,
			                &m_gameInstance.runtime(), self, &m_entityHost };
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
			HE_LOG_INFO(Core, "%s", "GameApplication: loaded packed startup scene");
		}
		else
			HE_LOG_WARN(Core, "%s", "GameApplication: failed to load packed startup scene");
	}
	if (!sceneLoaded && !m_config.mainSceneName.empty())
	{
		// Fallback: loose .hescene (JSON) next to the executable.
		const std::filesystem::path scenePath = exeDir / m_config.mainSceneName;
		if (serializer.load(*m_world, scenePath, SerializeFormat::JSON))
			HE_LOG_INFO(Core, "%s", ("GameApplication: loaded scene " + m_config.mainSceneName).c_str());
		else
			HE_LOG_WARN(Core, "%s", ("GameApplication: failed to load scene " + scenePath.string()).c_str());
	}
	setWorld(m_world.get());

	// Player controller/character classes + input events: discover the project's
	// input assets, spawn the player instances on the shared runtime (Construct +
	// BeginPlay) and start pumping Tick/Input.* events (OnRender). After the scene
	// load so BeginPlay can reach scene entities through the engine-call API.
	startPhysics();
	// The entity host FIRST: the player host spawns its characters through it,
	// so they arrive with the components their class carries.
	if (m_world)
		m_entityHost.begin(m_gameInstance.runtime(), *m_world, contentManager());
	m_playerHost.begin(m_gameInstance.runtime(), contentManager(), &m_entityHost);
	// Last of the hosts: a player character spawned just above may be the very
	// entity whose state machine needs a sync graph.
	m_animatorHost.begin(m_gameInstance.runtime(), *m_world, contentManager());

	// AFTER the player spawns, not before: a PlayerCharacter class brings its own
	// camera along, and that camera only exists once the class has been
	// instantiated. Checking first would find an empty scene, add a fallback
	// camera flagged isMain, and that fallback — created earlier — is the one the
	// extractor picks. The player would then own a camera nothing renders through.
	if (ensureDefaultCamera(*m_world))
		HE_LOG_INFO(Core, "%s",
			"GameApplication: added a default free-fly camera (scene had none)");

	// Audio: init the engine and start playOnStart sources, mirroring the editor's
	// play mode — packaged games get sound too (HC/script audio.* routes here).
	if (m_audioEngine.init())
		AudioSystem::playOnStart(*m_world, m_audioEngine, &contentManager());
	else
		HE_LOG_WARN(Core, "%s",
			"GameApplication: audio device init failed — running silent");

	HE_LOG_INFO(Core, "%s",
		("GameApplication: streaming " + std::to_string(streamSceneAssets(*m_world)) +
		 " scene-referenced asset roots").c_str());

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
		// Engine services (savegames) go in BEFORE onStart, so "load the save
		// on startup" works from the first native line. The world resolves per
		// call — scene switches stay transparent to the library.
		m_saveServicesBinding.world   = [this]() { return m_world.get(); };
		m_saveServicesBinding.content = &contentManager();
		HE::api::fillSaveServices(m_saveServices, &m_saveServicesBinding);
		logicLoader().injectServices(&m_saveServices);
		logicLoader().logic()->onStart(*m_world);
		HE_LOG_INFO(Core, "%s", "GameApplication: native game logic started");
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

// ── Shared scene bring-up steps ──────────────────────────────────────────────

bool GameApplication::ensureDefaultCamera(HorizonWorld& world)
{
	auto& reg = world.registry();
	for (auto e : reg.view<CameraComponent>()) { (void)e; return false; } // authored camera wins
	auto camE = world.createEntity("GameCamera");
	TransformComponent tc;
	tc.position = glm::vec3(0.0f, 2.0f, 8.0f); // back + up, looking toward -Z
	reg.emplace<TransformComponent>(camE, tc);
	CameraComponent cc; cc.isMain = true;
	reg.emplace<CameraComponent>(camE, cc);
	return true;
}

size_t GameApplication::streamSceneAssets(HorizonWorld& world)
{
	const auto refs = SceneSystems::collectAssetRefs(world);
	for (HE::UUID r : refs) contentManager().loadAssetAsync(r);
	return refs.size();
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
		HE_LOG_WARN(Core, "%s",
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

	ensureDefaultCamera(*m_world);

	if (m_audioEngine.isInitialized())
		AudioSystem::playOnStart(*m_world, m_audioEngine, &contentManager());

	// Seamlessness comes from the async streaming pipeline: the swap itself is a
	// cheap main-thread deserialize; meshes/textures stream in the background.
	const size_t refCount = streamSceneAssets(*m_world);

	startPhysics();
	startScripts();
	// The entity classes belong to the world too — without this the new
	// scene's Entity classes never run, and the old world's instances would
	// linger against entities that no longer exist.
	m_entityHost.end();
	m_entityHost.begin(m_gameInstance.runtime(), *m_world, contentManager());
	// Same reason: the sync instances belong to the world that is going away.
	m_animatorHost.end();
	m_animatorHost.begin(m_gameInstance.runtime(), *m_world, contentManager());
	m_world->fireLevelLoaded();
	HE_LOG_INFO(Core, "%s",
		("GameApplication: switched to scene '" + label + "' ("
		 + std::to_string(refCount) + " asset roots streaming)").c_str());
}

void GameApplication::executeSceneRequests()
{
	using Kind = HE::api::scene::RequestKind;
	const auto requests = HE::api::scene::takeRequests();
	for (const auto& r : requests)
	{
		switch (r.kindOf())
		{
		case Kind::Switch:      // full switch — or, hidden, a background PRELOAD
		{
			if (!r.hidden) { performSceneSwitch(r.path); break; }
			auto pending = std::make_unique<HorizonWorld>();
			if (!loadSceneInto(*pending, r.path, /*additive=*/false, nullptr))
			{
				HE_LOG_WARN(Core, "%s",
					("GameApplication: scene.load (hidden) failed — '" + r.path + "' not found").c_str());
				break;
			}
			// Warm the pending scene's assets NOW so the later activate() swap
			// presents without a streaming pop.
			const size_t refCount = streamSceneAssets(*pending);
			if (m_pendingWorld)
				HE_LOG_WARN(Core, "%s",
					("GameApplication: replacing pending scene '" + m_pendingScenePath + "'").c_str());
			m_pendingWorld     = std::move(pending);
			m_pendingScenePath = r.path;
			HE::api::scene::notePendingLevel(true);
			HE_LOG_INFO(Core, "%s",
				("GameApplication: preloaded scene '" + r.path + "' ("
				 + std::to_string(refCount) + " asset roots streaming) — awaiting activate").c_str());
			break;
		}
		case Kind::Activate:    // activate the preloaded level
		{
			if (!m_pendingWorld)
			{
				HE_LOG_WARN(Core, "%s",
					"GameApplication: scene.activate with no pending scene (load hidden first)");
				break;
			}
			swapToWorld(std::move(m_pendingWorld), m_pendingScenePath);
			m_pendingScenePath.clear();
			HE::api::scene::notePendingLevel(false);
			break;
		}
		case Kind::Additive:    // additive zone
		{
			if (!m_world) break;
			std::vector<entt::entity> created;
			if (!loadSceneInto(*m_world, r.path, /*additive=*/true, &created))
			{
				HE_LOG_WARN(Core, "%s",
					("GameApplication: scene.loadAdditive failed — '" + r.path + "' not found").c_str());
				break;
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
			streamSceneAssets(*m_world);
			const int started = startScriptsFor(created);
			HE_LOG_INFO(Core, "%s",
				("GameApplication: zone " + std::to_string(r.zone) + " loaded ('" + r.path +
				 "', " + std::to_string(created.size()) + " entities, " +
				 std::to_string(started) + " scripts" + (r.hidden ? ", hidden" : "") + ")").c_str());
			break;
		}
		case Kind::ZoneVisible: // show/hide a zone (queued so it orders after a load)
		{
			if (!m_world) break;
			HE::api::Ctx c{ m_world.get(), nullptr, &contentManager(), &m_audioEngine };
			HE::api::scene::setZoneVisible(c, r.zone, r.flag);
			break;
		}
		case Kind::ZonePosition: // move a zone (queued so it orders after a load)
		{
			if (!m_world) break;
			HE::api::Ctx c{ m_world.get(), nullptr, &contentManager(), &m_audioEngine };
			HE::api::scene::setZonePosition(c, r.zone, r.pos);
			break;
		}
		case Kind::UnloadZone:  // unload additive zone
		{
			const HE::api::scene::ZoneInfo* z = HE::api::scene::zoneInfo(r.zone);
			if (!z || !m_world) break;
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
			HE_LOG_INFO(Core, "%s",
				("GameApplication: zone " + std::to_string(r.zone) + " unloaded ("
				 + std::to_string(gone) + " entities)").c_str());
			break;
		}
		}
	}
}

int GameApplication::startScriptsFor(const std::vector<entt::entity>& entities)
{
	if (!m_world) return 0;
	// A streamed-in zone carries both kinds of code, so both hosts get the
	// new entities — the Lua/Python one and the HorizonCode one. Missing the
	// second is how a zone's Entity classes would silently never run.
	int started = m_entityHost.bindFor(entities);
	if (m_scriptContext)
		started += m_scriptContext->startScriptsFor(entities, contentManager(), m_scriptInstances);
	return started;
}

void GameApplication::startPhysics()
{
	if (!m_world) return;
	// Rebuilt, never reused: the bodies belong to the world that is going
	// away, and a stale contact from it would report entity ids the new
	// world knows nothing about.
	m_physicsWorld = std::make_unique<PhysicsWorld>();
	m_physicsWorld->initialize(*m_world);
	m_physicsAccum = 0.0f;
}

void GameApplication::startScripts()
{
	if (!m_world) return;
	m_scriptContext = std::make_unique<ScriptContext>(*m_world);
	m_scriptContext->setPhysicsWorld(m_physicsWorld.get());
	m_scriptContext->setContentManager(&contentManager());

	const int started = m_scriptContext->startWorldScripts(contentManager(), m_scriptInstances);
	if (started > 0)
		HE_LOG_INFO(Core, "%s",
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

Entity GameApplication::possessedCharacterEntity() const
{
	for (HorizonCode::InstanceId inst : m_playerHost.characters())
	{
		const Entity e = m_entityHost.entityOf(inst);
		if (e != entt::null) return e;
	}
	return entt::null;
}

void GameApplication::updateCameraController(float dt)
{
	if (!m_mouseCaptured || !m_world || dt <= 0.0f) return;

	// The cursor is parked back at this window's centre every frame — but only
	// while WE have focus, so an alt-tabbed game never yanks the cursor away from
	// another app.
	SDL_Window* warpWin = window() ? window()->GetNativeWindow() : nullptr;
	if (warpWin && !(SDL_GetWindowFlags(warpWin) & SDL_WINDOW_INPUT_FOCUS)) warpWin = nullptr;

	// A camera rig wins when the scene has one it can actually drive. Only when
	// it cannot — no rig camera, or a target that does not resolve — does the
	// built-in free flight take over, so a scene without a rig behaves exactly
	// as it always did.
	HE::CameraLookInput look;
	look.mouse  = input().mouse();
	look.stickX = input().gamepadAxisFiltered(SDL_GAMEPAD_AXIS_RIGHTX);
	look.stickY = input().gamepadAxisFiltered(SDL_GAMEPAD_AXIS_RIGHTY);
	look.dt     = dt;
	if (HE::CameraRigController::update(*m_world, look,
	                                    possessedCharacterEntity(),
	                                    m_physicsWorld.get()).driven)
	{
		// The rig path has to park the cursor itself. With relative mode engaged
		// this is a pure internal position update, but when it is NOT engaged (a
		// focus transition, a platform quirk) the OS cursor drifts and the look
		// stalls at the screen edge. FlyCameraController does this for its own
		// path; without it here a scene with a rig would lose a safety net that a
		// scene without one has.
		if (warpWin)
		{
			int ww = 0, wh = 0;
			SDL_GetWindowSize(warpWin, &ww, &wh);
			SDL_WarpMouseInWindow(warpWin, ww * 0.5f, wh * 0.5f);
		}
		return;
	}

	HE::FlyCameraController::update(m_world->registry(), input(), dt, warpWin);
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
#ifdef HE_GAME_DEV_HOTKEYS
		// V: static VSync toggle — DEVELOPMENT BUILDS ONLY. A shipped game has no
		// settings menu yet, and a stray V must not silently flip the player's
		// present mode; the define is set only for non-Release configurations
		// (HE_Game/CMakeLists.txt), so shipping builds don't compile this in.
		if (event.key.key == SDLK_V)
		{
			m_vsyncOn = !m_vsyncOn;
			setVSync(m_vsyncOn);
			HE_LOG_INFO(Core, "%s",
				m_vsyncOn ? "GameApplication: VSync ON" : "GameApplication: VSync OFF");
			return true;
		}
#endif
		// Esc: release/re-grab the mouse.
		if (event.key.key == SDLK_ESCAPE)
		{
			setMouseCaptured(!m_mouseCaptured);
			return true;
		}
	}
	return false;
}

void GameApplication::OnRender(float deltaTime)
{
	// The base Application tolerates a null renderer ("running without graphics"),
	// so every renderer touch below goes through this one handle and its null check
	// — the checks used to be applied to some calls and forgotten on others.
	IRenderer* const r = renderer();

	// Feed the per-frame engine clock + input snapshot so time.*/input.* nodes and
	// scripts read fresh values this frame (before the ECS/script updates below).
	HE::api::time::advance(deltaTime);
	HE::api::input::pushSdlSnapshot(input().mouse().dx, input().mouse().dy);
	// Gamepad snapshot: PUSHED from Input's merged frame, deadzone-filtered —
	// the snapshot never polls SDL itself (one owner per device stream).
	{
		float axes[SDL_GAMEPAD_AXIS_COUNT];
		for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
			axes[a] = input().gamepadAxisFiltered(static_cast<SDL_GamepadAxis>(a));
		HE::api::input::setGamepad(input().gamepad().connected,
		                           axes, SDL_GAMEPAD_AXIS_COUNT,
		                           input().gamepad().buttons, SDL_GAMEPAD_BUTTON_COUNT);
	}

	// From here on there are TWO clocks, and which one a tick gets is a design
	// decision, not a detail:
	//   gameDt — the scaled one (time.setTimeScale), for everything that IS the
	//            game: scripts, physics, cameras, animation, the ECS systems.
	//   deltaTime — the raw frame time, for what has to keep running while the
	//            game is paused at scale 0: the UI (a pause menu that froze
	//            could never unpause itself) and timed debug primitives (which
	//            would otherwise never expire).
	const float gameDt = HE::api::time::deltaTime();

	// Deferred scene transitions requested by scripts/graphs last frame: executed
	// at the frame START so nothing downstream touches a half-swapped world.
	executeSceneRequests();

	// Timed debug primitives (debug.* nodes/scripts) → the renderer's line pass.
	{
		std::vector<DebugLine> dbg;
		HE::api::debug::collect(deltaTime, dbg);
		if (r) r->SetDebugLines(dbg);
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
	if (!justRegistered.empty() && r)
		r->WarmupMaterials(justRegistered);

	// Per-frame ECS script update (Lua/Python onUpdate), before the systems tick so
	// script-driven transforms/params are reflected the same frame.
	updateScripts(gameDt);

	// Physics at a FIXED rate, the same one the editor previews at
	// (PhysicsWorld::kFixedDt) — a game that simulates at a different rate
	// than it was authored against is not the same game.
	if (m_world && m_physicsWorld)
	{
		HE_PROFILE_SCOPE_N("PhysicsStep");
		m_physicsAccum += gameDt;
		// Bounded so a long stall (a streaming hitch, a breakpoint) cannot
		// spiral into ever more catch-up steps. The bound RIDES the time scale:
		// at scale 5 a single frame legitimately owes ~5× the steps, and a fixed
		// cap of 5 would saturate every frame and dump the remainder below —
		// fast-forward would silently decay into slow motion. The rate itself
		// stays fixed; only the number of steps per frame moves.
		const int maxSteps = 5 * std::max(1, (int)std::ceil(HE::api::time::timeScale()));
		int steps = 0;
		while (m_physicsAccum >= PhysicsWorld::kFixedDt && steps++ < maxSteps)
		{
			m_physicsWorld->step(*m_world, PhysicsWorld::kFixedDt);
			m_physicsAccum -= PhysicsWorld::kFixedDt;
		}
		if (m_physicsAccum > PhysicsWorld::kFixedDt) m_physicsAccum = 0.0f;

		// ONE dispatch for both frontends: polling drains the queues, so a
		// second call would find nothing left and it would look like the
		// events simply never fire for that language.
		HE_PROFILE_SCOPE_N("CollisionDispatch");
		CollisionSystem::dispatch(*m_physicsWorld, m_scriptContext.get(), m_scriptInstances,
		                          &m_gameInstance.runtime(), m_entityHost.instances());
	}

	// Cameras run AFTER physics, and after the scripts that move things: a camera
	// following a target it updated before the step would follow where that
	// target was LAST frame, and that lag is visible. Still before the systems
	// tick, so LOD and precipitation get this frame's camera position.
	updateCameraController(gameDt);

	// Keep the audio listener + spatial sources tracking their entities.
	if (m_world && m_audioEngine.isInitialized())
		AudioSystem::updateSpatial(*m_world, m_audioEngine);

	// Live widgets: per-frame logic tick (EventTick). RAW dt on purpose — the
	// pause menu is a widget, and a menu that stops ticking when the game pauses
	// cannot animate, count down, or hand the player a way back out.
	if (m_world) m_world->widgets().tick(deltaTime);

	// Player instances: Tick + Input.<Action>.* events (mapping ticked against
	// the app Input state, which ProcessEvent keeps current).
	// A running game always owns the mouse — there is no editor UI competing for
	// it — so the frame's movement goes straight through to the input mapping.
	// Capture only decides whether the OS keeps the cursor in the window; an
	// uncaptured game still gets motion while the pointer is over it.
	m_playerHost.tick(input(), gameDt, input().mouse());
	// Entity classes: Tick, plus reaping the ones whose entity is gone.
	m_entityHost.tick(gameDt);

	// Latent HorizonCode flow (Delay nodes): resume expired continuations on
	// the app-wide runtime (GameInstance + widgets + level + objects share it).
	// Both clocks: a Delay counts game seconds by default (so a pause stops it,
	// the rule Unreal/Unity timers follow), and real seconds when its Real Time
	// pin is set. Widgets share this runtime, so that pin is what lets a pause
	// menu time anything at all while the game behind it stands still.
	m_gameInstance.runtime().update(gameDt, deltaTime);

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
		                          r && r->GetCapabilities().supportsGpuParticles;
		// Unbraced on purpose: the scope reaches to the end of this `if (m_world)`
		// block, so the GI + environment pushes below are timed with it (as before).
		HE_PROFILE_SCOPE_N("SceneSystemsTick");
		SceneSystems::tickWorld(*m_world, contentManager(), r, camPos, gameDt,
		                        m_physicsWorld.get(), gpuParticles);
		// Animation last, after every system that could have moved something this
		// frame — a state machine reads what gameplay just produced. Still ahead
		// of extraction, which consumes the bone matrices.
		SceneSystems::tickAnimation(*m_world, contentManager(), gameDt, &m_animatorHost);

		// Global Illumination: same GlobalState config.json key the editor's
		// Preferences checkbox writes (GlobalIlluminationEnabled/GIIndirectIntensity/
		// GILightRadius), read directly here — mirrors the GpuParticles pattern above,
		// NOT the SSAO/Bloom gap (those settings are never pushed by the packaged
		// game at all). Capability-gated so non-Metal/non-raytracing builds no-op.
		if (r)
		{
			const bool giEnabled = GlobalState::getInstance().getCustomConfigBool("GlobalIlluminationEnabled", false) &&
			                       r->GetCapabilities().supportsGlobalIllumination;
			r->SetGISettings(IRenderer::GISettings{
				giEnabled,
				static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("GIIndirectIntensity", 1.0f)),
				static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("GILightRadius", 0.5f))});

			// SSR — same config.json keys the editor writes, capability-gated
			// (Metal deferred tile mode only in v1).
			const bool ssrEnabled =
				GlobalState::getInstance().getCustomConfigBool("SSREnabled", false) &&
				r->GetCapabilities().supportsScreenSpaceReflections;
			IRenderer::SSRSettings ssr;
			ssr.enabled      = ssrEnabled;
			ssr.intensity    = static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("SSRIntensity", 1.0f));
			ssr.maxRoughness = static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("SSRMaxRoughness", 0.6f));
			ssr.quality      = GlobalState::getInstance().getCustomConfigInt("SSRQuality", 1);
			r->SetSSRSettings(ssr);

			// Ray-traced GI reflections — same config.json keys the editor
			// writes, capability-gated (Metal tile deferred + HW RT in v1).
			const bool giReflEnabled =
				GlobalState::getInstance().getCustomConfigBool("GIReflectionsEnabled", false) &&
				r->GetCapabilities().supportsGIReflections;
			IRenderer::GIReflectionSettings gr;
			gr.enabled      = giReflEnabled;
			gr.intensity    = static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("GIReflIntensity", 1.0f));
			gr.maxRoughness = static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("GIReflMaxRoughness", 0.6f));
			gr.quality      = GlobalState::getInstance().getCustomConfigInt("GIReflQuality", 1);
			gr.bounces      = GlobalState::getInstance().getCustomConfigInt("GIReflBounces", 1);
			r->SetGIReflectionSettings(gr);

			// Anti-aliasing — same config.json keys the editor's Preferences write
			// ("AntiAliasing" = AAMethod int, plus sharpness/scale/specular-AA).
			// Not capability-gated here: the backend runs the shared
			// IRenderer::ResolveAAMethod fallback itself, so a config authored on a
			// Metal machine opens sanely in a D3D11 build of the same game.
			IRenderer::AntiAliasingSettings aa;
			aa.method             = GlobalState::getInstance().getCustomConfigInt("AntiAliasing", 1);
			aa.sharpness          = static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("AASharpness", 0.35f));
			aa.renderScale        = static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("RenderScale", 1.0f));
			aa.specularAA         = GlobalState::getInstance().getCustomConfigBool("SpecularAA", true);
			aa.specularAAStrength = static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("SpecularAAStrength", 1.0f));
			r->SetAntiAliasingSettings(aa);

			// Render path — same config.json key the editor's Preferences combo
			// writes ("RenderPath": 0 = Forward, 1 = Deferred), capability-gated.
			const bool deferredPath =
				GlobalState::getInstance().getCustomConfigInt("RenderPath", 0) == 1 &&
				r->GetCapabilities().supportsDeferredRendering;
			r->SetRenderPath(deferredPath ? HE::RenderPath::Deferred : HE::RenderPath::Forward);

			// Push the scene environment to the renderer. The base Application renders the
			// world but never pushes EnvironmentSettings (that lived only in the editor), so
			// without this the weather sky / clouds / fog / flash would not show in-game.
			// The Sky is a scene entity now — no Sky entity → skip the sky pass.
			// makeEnvironmentSettings also ADVANCES the day-night cycle by deltaTime, so
			// it must stay exactly this one call per frame.
			const Entity gEnvEntity = m_world->environmentEntity();
			auto* env = (gEnvEntity == entt::null)
				? nullptr : m_world->registry().try_get<EnvironmentComponent>(gEnvEntity);
			if (!env)
				r->SetEnvironmentSettings(IRenderer::EnvironmentSettings{ .skyEnabled = false });
			else
			{
				// Scaled: the day-night cycle is world state, so slow motion slows
				// the sun and a pause holds it in place.
				r->SetEnvironmentSettings(HE::makeEnvironmentSettings(*env, gameDt));
				// Mirror onto the built-in sun/moon LightComponents, so gameplay code
				// reading them sees the Sky's values and not a stale default.
				m_world->syncEnvironmentLights();
			}
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
	m_animatorHost.end();
	m_entityHost.end();
	m_playerHost.end();
	m_physicsWorld.reset();

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
	HE_LOG_INFO(Core, "%s", "GameApplication::OnShutdown");
}
