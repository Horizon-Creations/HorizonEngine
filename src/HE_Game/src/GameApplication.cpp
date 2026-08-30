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

namespace
{
// What this platform ran on before the config could name a backend, and what an
// unusable choice falls back to.
constexpr HE::RendererBackend kDefaultBackend =
#ifdef __APPLE__
	HE::RendererBackend::Metal;
#else
	HE::RendererBackend::OpenGL;
#endif

// Backends by NAME (the editor's getRHIName spelling), because config.json is a
// file a player or a support ticket edits by hand: "Metal" survives a
// renumbering of the enum, a bare 4 does not.
bool backendFromName(const std::string& name, HE::RendererBackend& out)
{
	if (name == "OpenGL") { out = HE::RendererBackend::OpenGL; return true; }
	if (name == "Vulkan") { out = HE::RendererBackend::Vulkan; return true; }
	if (name == "D3D11")  { out = HE::RendererBackend::D3D11;  return true; }
	if (name == "D3D12")  { out = HE::RendererBackend::D3D12;  return true; }
	if (name == "Metal")  { out = HE::RendererBackend::Metal;  return true; }
	if (name == "Software") { out = HE::RendererBackend::Software; return true; }
	return false;
}

// Whether THIS runtime can create that backend at all. RendererFactory throws
// for one whose implementation was not compiled in, so the same #ifdef set has
// to be answered here — a config authored for another platform must fall back,
// not abort the game before its window opens.
bool backendAvailable(HE::RendererBackend backend)
{
	switch (backend)
	{
	case HE::RendererBackend::OpenGL: return true;
	// A CPU rasterizer needs nothing to be present, so it is available wherever
	// the runtime is — which is what makes it the safe fallback as well as the
	// deliberate choice.
	case HE::RendererBackend::Software: return true;
#ifdef HE_VULKAN_ENABLED
	case HE::RendererBackend::Vulkan: return true;
#endif
#ifdef _WIN32
	case HE::RendererBackend::D3D11:
	case HE::RendererBackend::D3D12:  return true;
#endif
#ifdef __APPLE__
	case HE::RendererBackend::Metal:  return true;
#endif
	default: return false;
	}
}

bool windowModeFromName(const std::string& name, HE::WindowMode& out)
{
	if (name == "Windowed")   { out = HE::WindowMode::Windowed;   return true; }
	if (name == "Fullscreen") { out = HE::WindowMode::Fullscreen; return true; }
	if (name == "Borderless") { out = HE::WindowMode::Borderless; return true; }
	return false;
}

// The settings the export wrote next to the game data, laid OVER whatever
// GlobalState resolved. Reading them here rather than leaving it to GlobalState
// is not redundancy: its search order is "the working directory, then the
// per-user data dir", and a shipped game matches neither — a macOS .app
// launched from Finder runs with "/" as its working directory, and the per-user
// file is shared with the editor and every other Horizon game on the machine.
// The overlay restores, on every platform, the precedence GlobalState documents
// for an executable-adjacent config. Returns how many entries were applied.
size_t overlayShippedConfig(const fs::path& exeDir)
{
	const fs::path path = exeDir / "config.json";
	std::ifstream in(path);
	if (!in) return 0;

	const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object())
	{
		HE_LOG_WARN(Core, "GameApplication: %s is corrupt — running on the built-in settings",
		            path.string().c_str());
		return 0;
	}
	const auto entries = j.find("CustomConfig");
	if (entries == j.end() || !entries->is_array()) return 0;

	size_t applied = 0;
	for (const auto& e : *entries)
	{
		if (!e.is_object()) continue;
		const auto key   = e.find("Key");
		const auto value = e.find("Value");
		if (key == e.end() || !key->is_string() || value == e.end()) continue;
		GlobalState::getInstance().setCustomConfigEntry(key->get<std::string>(), *value);
		++applied;
	}
	return applied;
}
} // namespace


GameApplication::GameApplication(std::string startupPath)
	: HE::Application(std::move(startupPath))
{
	applyShippedConfig();
}
GameApplication::~GameApplication() = default;

void GameApplication::applyShippedConfig()
{
	m_backend = kDefaultBackend;

	// Before a single shipped key is laid over the in-memory config: the game
	// must not persist any of it. configFilePath() resolves to the per-user file
	// the EDITOR uses on a developer machine, and ~Application writes it on the
	// way out — so without this, running an export once would stamp that game's
	// graphics settings onto the editor's preferences.
	GlobalState::getInstance().setConfigPersistent(false);

	// SDL_GetBasePath needs no SDL_Init, and inside a macOS .app it resolves to
	// Contents/Resources — the same directory OnInit reads project.hcfg and the
	// pak from, and where the exporter puts config.json.
	const char* baseRaw = SDL_GetBasePath();
	if (!baseRaw)
		HE_LOG_WARN(Core, "%s",
			"GameApplication: SDL_GetBasePath returned null — the shipped graphics "
			"settings cannot be located");
	else if (const size_t applied = overlayShippedConfig(fs::path(baseRaw)); applied > 0)
		HE_LOG_INFO(Core, "GameApplication: applied %zu shipped setting(s) from config.json",
		            applied);

	// ── An application opens in a WINDOW ─────────────────────────────────────
	// The member's default is Fullscreen, which is right for a game and wrong
	// for a tool: nobody ships a todo list that takes over the display, and
	// docs/he-apps-plan.md says so ("eine App startet standardmäßig als
	// windowed, man kann sie ja dann maximieren").
	//
	// Which it is lives in project.hcfg, and that is read in OnInit — far too
	// late, the window exists by then. So it is peeked at here, into a LOCAL
	// config: this is only the default, and an explicit GameWindowMode below
	// still wins.
	if (baseRaw)
	{
		ProjectConfig peek;
		if (ProjectConfigLoader::load(fs::path(baseRaw), peek) && peek.appMode)
			m_windowMode = HE::WindowMode::Windowed;
	}

	// An absent key keeps what the member already holds, which is what a game
	// shipped before any of this existed: 1280x720 fullscreen, VSync on, on the
	// platform's built-in backend. Existing exports therefore boot unchanged.
	GlobalState& gs = GlobalState::getInstance();
	const int w = gs.getCustomConfigInt("GameWindowWidth",  static_cast<int>(m_windowWidth));
	const int h = gs.getCustomConfigInt("GameWindowHeight", static_cast<int>(m_windowHeight));
	if (w > 0 && h > 0)
	{
		m_windowWidth  = static_cast<uint32_t>(w);
		m_windowHeight = static_cast<uint32_t>(h);
	}
	if (const std::string mode = gs.getCustomConfigString("GameWindowMode");
	    !mode.empty() && !windowModeFromName(mode, m_windowMode))
		HE_LOG_WARN(Core, "GameApplication: unknown window mode '%s' — keeping the default",
		            mode.c_str());
	m_vsyncOn = gs.getCustomConfigBool("GameVSync", m_vsyncOn);

	if (const std::string name = gs.getCustomConfigString("GameBackend"); !name.empty())
	{
		HE::RendererBackend wanted = kDefaultBackend;
		if (!backendFromName(name, wanted))
			HE_LOG_WARN(Core, "GameApplication: unknown graphics backend '%s' — using the default",
			            name.c_str());
		else if (!backendAvailable(wanted))
			HE_LOG_WARN(Core, "GameApplication: graphics backend '%s' is not in this build — using the default",
			            name.c_str());
		else
			m_backend = wanted;
	}
}

HE::ApplicationConfig GameApplication::GetConfig() const
{
	HE::ApplicationConfig cfg;
	cfg.windowprops.title  = m_config.projectName.empty() ? "HorizonGame" : m_config.projectName;
	cfg.windowprops.width  = m_windowWidth;
	cfg.windowprops.height = m_windowHeight;
	cfg.windowprops.vsync  = m_vsyncOn;
	cfg.windowprops.mode   = m_windowMode;
	cfg.backend            = m_backend;
	return cfg;
}

std::unique_ptr<IRenderer> GameApplication::CreateRenderer()
{
	HE_LOG_INFO(Core, "%s", "GameApplication: creating renderer");
	return RendererFactory::Create(m_backend);
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
	// Application build (docs/he-apps-plan.md A1): everything below that belongs
	// to a GAME is skipped. Latched into a member because half a dozen places
	// downstream ask, and reaching into m_config at each of them invites one of
	// them being forgotten.
	m_appMode = m_config.appMode;
	if (m_appMode)
	{
		HE_LOG_INFO(Core, "%s", "GameApplication: application mode — no world, no physics, "
		                        "no scene");
		// The FPS-style grab above happens before this config is readable, so it
		// is undone here rather than made conditional there — an app must not
		// swallow the cursor, and the early-return paths above (no hcfg, no pak)
		// keep behaving exactly as they always did.
		setMouseCaptured(false);
		// A2: draw on events, not on a clock.
		setEventDriven(true);
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

	// ── The theme, before anything is on screen ─────────────────────────────
	// Both halves BEFORE the GameInstance runs: OnInit is where the first widget
	// is created, and a widget created against the default theme and re-themed a
	// frame later is a flash of the wrong colours.
	//
	// The desktop's own light/dark is asked for here and again on
	// SDL_EVENT_SYSTEM_THEME_CHANGED, so an application set to "System" follows
	// it while it runs. Unknown (a platform SDL cannot ask) keeps the dark
	// default rather than guessing light.
	{
		// m_widgets, NOT m_world->widgets(): the world is built further down and
		// does not exist yet here. It BORROWS this manager when it is
		// (setWidgetManager), so the two are the same object from then on — but
		// only one of them can be reached this early.
		WidgetManager& wm = m_widgets;
		const SDL_SystemTheme sys = SDL_GetSystemTheme();
		if (sys == SDL_SYSTEM_THEME_LIGHT)     wm.setSystemThemeMode(HE::UIThemeMode::Light);
		else if (sys == SDL_SYSTEM_THEME_DARK) wm.setSystemThemeMode(HE::UIThemeMode::Dark);
		wm.setThemePreference(HE::uiThemePreferenceFromName(m_config.themeMode));
		if (!m_config.theme.empty())
		{
			if (const HE::UUID id = contentManager().loadAsset(m_config.theme); id != HE::UUID{})
				if (const ThemeAsset* a = contentManager().getTheme(id))
				{
					HE::UITheme t;
					if (HE::uiThemeFromJson(a->json, t)) wm.setTheme(t);
					else HE_LOG_WARN(Core, "GameApplication: theme '%s' is unreadable — "
					                       "using the built-in default",
					                 m_config.theme.c_str());
				}
			if (wm.theme().name == HE::uiDefaultTheme().name && !m_config.theme.empty())
				HE_LOG_INFO(Core, "GameApplication: theme '%s'", m_config.theme.c_str());
		}
		HE_LOG_INFO(Core, "GameApplication: theme mode %s (asked for %s)",
		            HE::uiThemeModeName(wm.themeMode()),
		            HE::uiThemePreferenceName(wm.themePreference()));
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
		svc.createObject  = [this](const std::string& p, const float* pos,
		                          const float* rot) -> uint32_t {
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
					{
						// Placement travels with the spawn (null = authored), so
						// Construct/BeginPlay already run at the destination.
						const HorizonCode::InstanceId inst =
							m_entityHost.spawn(ea->path, entt::null, pos, rot).instance;
						// The PlayerHost no longer creates characters, so this is
						// the only place it can learn that one exists — and it has
						// to, or a project without a controller loses its input.
						if (HorizonCode::engineClassIsA(rc.engineBase, "PlayerCharacter"))
							m_playerHost.addCharacter(inst);
						return inst;
					}
				}
			// Below here the object has no body at all, so a placement has nothing
			// to be written to: pos/rot are deliberately dropped, not defaulted.
			//
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
			// Unknown id — an old graph naming a row that was removed or renamed.
			// See the editor's twin for why this is said here and not in find().
			if (!fn) {
				HE_LOG_WARN(Script, "callApi: unknown engine api '%s' (removed or renamed?) - call skipped", id.c_str());
				return {};
			}
			// The caller travels along — see the editor's twin. So does what
			// "quit" means here: the shipped game IS the application, so app.quit
			// leaves the loop for real (the editor binds the same hook to
			// stopping play mode instead). A main menu's Exit button has nothing
			// else to call.
			HE::api::Ctx c{ m_world.get(), m_physicsWorld.get(), &contentManager(), &m_audioEngine,
			                &m_gameInstance.runtime(), self, &m_entityHost,
			                [this]{ Quit(); } };
			// The window rows (app.setTitle/setSize/size, app.requestRedraw). The
			// shipped build owns its window outright, so unlike the editor there
			// is nothing to protect here — a graph that resizes it means it.
			c.setWindowTitle = [this](const std::string& t) { setWindowTitle(t); };
			c.setWindowSize  = [this](uint32_t w, uint32_t h) { setWindowSize(w, h); };
			c.windowSize     = [this] {
				const HE::Window* w = window();
				return w ? glm::vec2(static_cast<float>(w->GetWidth()),
				                     static_cast<float>(w->GetHeight()))
				         : glm::vec2(0.0f);
			};
			c.requestRedraw  = [this] { requestRedraw(); };
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
	// An application has no scene to load. The world above still exists and is
	// still handed to the renderer below, because it is what routes the widget
	// API (ScriptApi::createWidget goes through HorizonWorld::widgets) — it just
	// stays empty.
	if (!m_appMode && m_config.hasPackedScene)
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
	if (!m_appMode && !sceneLoaded && !m_config.mainSceneName.empty())
	{
		// Fallback: loose .hescene (JSON) next to the executable.
		const std::filesystem::path scenePath = exeDir / m_config.mainSceneName;
		if (serializer.load(*m_world, scenePath, SerializeFormat::JSON))
			HE_LOG_INFO(Core, "%s", ("GameApplication: loaded scene " + m_config.mainSceneName).c_str());
		else
			HE_LOG_WARN(Core, "%s", ("GameApplication: failed to load scene " + scenePath.string()).c_str());
	}
	setWorld(m_world.get());

	// Player controller classes + input events: discover the project's input
	// assets, spawn the controllers on the shared runtime (Construct + BeginPlay)
	// and start pumping Tick/Input.* events (OnRender). After the scene load so
	// BeginPlay can reach scene entities through the engine-call API.
	// None of this exists in an application: no bodies to simulate, no player to
	// possess, no characters to animate. Left unstarted rather than started and
	// then fed an empty world, so the cost is zero rather than nearly zero — and
	// so their per-frame ticks below no-op on their own.
	if (!m_appMode)
	{
		startPhysics();
		// The entity host FIRST: a controller's BeginPlay is where the game spawns its
		// character with Create Object, and that spawn is only served with a body
		// while the entity host is running.
		if (m_world)
			m_entityHost.begin(m_gameInstance.runtime(), *m_world, contentManager());
		// The entity host is handed over so the player host can find the characters
		// the LEVEL already placed; it never spawns through it.
		m_playerHost.begin(m_gameInstance.runtime(), contentManager(), &m_entityHost);
		// Last of the hosts: a player character spawned just above may be the very
		// entity whose state machine needs a sync graph.
		m_animatorHost.begin(m_gameInstance.runtime(), *m_world, contentManager());
	}

	// AFTER the player spawns, not before: a PlayerCharacter class brings its own
	// camera along, and that camera only exists once the class has been
	// instantiated. Checking first would find an empty scene, add a fallback
	// camera flagged isMain, and that fallback — created earlier — is the one the
	// extractor picks. The player would then own a camera nothing renders through.
	// Kept in application mode too, deliberately: the extractor and every backend
	// build their frame around an active camera, and a UI-only frame still goes
	// through that path. It costs one entity and nothing per frame — what an app
	// skips is the CONTROLLER (updateCameraController below), so the camera sits
	// still and WASD belongs to the UI.
	if (ensureDefaultCamera(*m_world))
		HE_LOG_INFO(Core, "%s",
			m_appMode ? "GameApplication: added the still camera the UI frame renders through"
			          : "GameApplication: added a default free-fly camera (scene had none)");

	// Audio: init the engine and start playOnStart sources, mirroring the editor's
	// play mode — packaged games get sound too (HC/script audio.* routes here).
	if (m_audioEngine.init())
		AudioSystem::playOnStart(*m_world, m_audioEngine, &contentManager());
	else
		HE_LOG_WARN(Core, "%s",
			"GameApplication: audio device init failed — running silent");

	// Nothing to stream without a scene: an app's assets are reached through its
	// widgets, which load on demand.
	if (!m_appMode)
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
	// The same meaning of "quit" the HorizonCode services bind in OnInit: the
	// shipped game IS the application, so horizon.app.quit leaves the loop for
	// real. Bound BEFORE the scripts start — an onStart may already call it.
	m_scriptContext->setQuitHandler([this]{ Quit(); });

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
	//
	// Game-only routing joins that condition rather than skipping this function:
	// processPointer still has to RUN with an invalid pointer, because that is
	// what clears a hover the UI is already showing. Returning early would leave
	// the last hovered button lit for as long as the mode lasts.
	const bool uiTakesInput = HE::api::input::mode() != HE::api::input::Mode::GameOnly;
	const bool pointerValid = !m_mouseCaptured && w != nullptr && uiTakesInput;

	// Widget pointer input first — widgets draw on top of entity UI. The answer
	// ("the pointer is on something clickable") is kept, not dropped: it is what
	// masks the mouse buttons out of gameplay next frame, so pressing a menu
	// button does not also fire the weapon behind it.
	m_uiWantsPointer =
		m_world->widgets().processPointer(static_cast<float>(pw), static_cast<float>(ph),
		                                  mx * sx, my * sy,
		                                  (buttons & SDL_BUTTON_LMASK) != 0, pointerValid);

	// Tell the OS where the focused field is, so an input method opens its
	// candidate list beside it instead of in the corner of the screen. Pushed
	// every frame a field is focused: the field can move (a scrolled list, a
	// resized window), and SDL only remembers what it was last told.
	if (SDL_Window* win = window() ? window()->GetNativeWindow() : nullptr)
	{
		HE::UIWidgetRect fieldRect{};
		if (m_world->widgets().focusedFieldRect(static_cast<float>(pw),
		                                        static_cast<float>(ph), fieldRect))
		{
			// Back to window points: SDL wants the rect in the same space it
			// reports the mouse in, and the UI works in drawable pixels.
			const SDL_Rect area{
				static_cast<int>(fieldRect.x / (sx > 0.0f ? sx : 1.0f)),
				static_cast<int>(fieldRect.y / (sy > 0.0f ? sy : 1.0f)),
				static_cast<int>(fieldRect.w / (sx > 0.0f ? sx : 1.0f)),
				static_cast<int>(fieldRect.h / (sy > 0.0f ? sy : 1.0f)) };
			SDL_SetTextInputArea(win, &area, 0);
		}
	}

	// A double-click selects the word under it, a triple-click the whole line.
	// Consumed here rather than in OnEvent so it reuses the pointer arithmetic
	// above instead of a second, drifting copy of it. The press that came with
	// the same click already focused the field and put the caret there.
	if (pointerValid && m_uiClickCount >= 2)
	{
		if (m_uiClickCount == 2)
			m_world->widgets().selectWordAtPointer(static_cast<float>(pw),
			                                       static_cast<float>(ph), mx * sx);
		else
			m_world->widgets().selectAllFocused();
	}
	m_uiClickCount = 0;

	// The wheel goes to a scroll box under the cursor first; what it does not
	// consume stays available to whatever else reads the wheel this frame.
	if (pointerValid)
	{
		const float wheel = input().mouse().wheel;
		if (wheel != 0.0f)
			m_world->widgets().processWheel(static_cast<float>(pw), static_cast<float>(ph),
			                                mx * sx, my * sy, wheel);
	}

	// Show the cursor the hovered widget element requested (default = arrow).
	if (pointerValid) HE::applyUICursor(m_world->widgets().hoverCursor());

	// ── Keyboard / gamepad menu navigation ───────────────────────────────────
	// A menu has to be usable without a mouse. Arrow keys and the pad's D-Pad
	// move the focus, Enter/Space and the south button activate it. Not routed
	// while a text field has the keyboard: there the arrows belong to the text.
	if (uiTakesInput && !m_world->widgets().hasFocusedTextField())
	{
		Input& in = input();
		using Nav = WidgetManager::NavDir;
		// Held state only reaches here, so the edges are tracked in m_uiNavPrev:
		// holding Down must step ONE entry, not run through the whole menu.
		const struct { Nav dir; SDL_Scancode key; SDL_GamepadButton pad; } kNav[] = {
			{ Nav::Up,    SDL_SCANCODE_UP,    SDL_GAMEPAD_BUTTON_DPAD_UP    },
			{ Nav::Down,  SDL_SCANCODE_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_DOWN  },
			{ Nav::Left,  SDL_SCANCODE_LEFT,  SDL_GAMEPAD_BUTTON_DPAD_LEFT  },
			{ Nav::Right, SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
		};
		uint8_t now = 0;
		for (int i = 0; i < 4; ++i)
			if (in.IsKeyDown(kNav[i].key) || in.isGamepadButtonDown(kNav[i].pad))
				now |= static_cast<uint8_t>(1u << i);
		if (in.IsKeyDown(SDL_SCANCODE_RETURN) || in.IsKeyDown(SDL_SCANCODE_SPACE) ||
		    in.isGamepadButtonDown(SDL_GAMEPAD_BUTTON_SOUTH))
			now |= 1u << 4;

		const uint8_t edges = static_cast<uint8_t>(now & ~m_uiNavPrev);
		m_uiNavPrev = now;
		for (int i = 0; i < 4; ++i)
			if (edges & (1u << i))
			{
				m_world->widgets().navigate(kNav[i].dir, static_cast<float>(pw),
				                            static_cast<float>(ph));
				break;
			}
		if (edges & (1u << 4)) m_world->widgets().activateFocused();
	}

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
	// Ask POSSESSION, not spawn order. Characters come out of the game's own
	// Create Object now, so "the first character that exists" could be an NPC,
	// a corpse or a spawn the player never took — the camera has to follow the
	// one a controller is actually steering.
	for (const HorizonCode::InstanceId ctrl : m_playerHost.controllers())
	{
		const HorizonCode::InstanceId pawn = HE::api::player::possessed(ctrl);
		if (pawn == 0) continue;
		const Entity e = m_entityHost.entityOf(pawn);
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
	// A double- or triple-click on a text field selects a word or the line.
	// Only the COUNT is taken here; where the pointer was is worked out in
	// updateUIInput, which already does that arithmetic once.
	if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT &&
	    event.button.clicks >= 2)
		m_uiClickCount = event.button.clicks;

	// OS window focus → GameInstance OnWindowFocusChanged (while running).
	if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)      m_gameInstance.setWindowFocus(true);
	else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)   m_gameInstance.setWindowFocus(false);

	// A focused in-game text field owns the keyboard: route text + edit keys to
	// the widget and swallow them so they don't drive the camera/gameplay.
	// Not under game-only routing: there the UI receives nothing, and a field
	// that still held focus from before the switch would otherwise keep eating
	// the movement keys with no way for the player to take them back.
	if (m_world && HE::api::input::mode() != HE::api::input::Mode::GameOnly &&
	    m_world->widgets().hasFocusedTextField())
	{
		if (event.type == SDL_EVENT_TEXT_INPUT)
		{
			m_world->widgets().inputText(event.text.text);
			return true;
		}
		// What an input method is still building. Sent repeatedly while the user
		// types on a CJK keyboard, and finally as an empty string when the
		// composition is committed or cancelled — which is exactly what clears it.
		if (event.type == SDL_EVENT_TEXT_EDITING)
		{
			m_world->widgets().inputComposition(event.edit.text ? event.edit.text : "",
			                                    event.edit.start);
			return true;
		}
		// The desktop switched between light and dark while we were running. An
		// application set to "System" follows it now rather than at the next
		// start — that is the entire difference between the setting and a
		// once-at-boot reading.
		if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED)
		{
			const SDL_SystemTheme sys = SDL_GetSystemTheme();
			if (sys == SDL_SYSTEM_THEME_LIGHT)
				m_widgets.setSystemThemeMode(HE::UIThemeMode::Light);
			else if (sys == SDL_SYSTEM_THEME_DARK)
				m_widgets.setSystemThemeMode(HE::UIThemeMode::Dark);
			return true;
		}
		if (event.type == SDL_EVENT_KEY_DOWN)
		{
			// The full editing grammar, not just "append and backspace": caret
			// keys, shift to select, and the clipboard (which SDL owns, hence
			// the copy in and out here rather than inside the widget layer).
			WidgetManager& wm = m_world->widgets();
			using TE = WidgetManager::TextEdit;
			const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
			const bool ctrl  = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
			const bool alt   = (event.key.mod & SDL_KMOD_ALT) != 0;
			switch (event.key.key)
			{
			// Word-wise variants first: Ctrl (Cmd on a Mac) and Alt both mean
			// "by word" for the arrows and Backspace, which is what the two
			// platform conventions expect and neither is wrong here.
			case SDLK_BACKSPACE:
				if (ctrl || alt) { wm.editFocusedText(TE::DeleteWordLeft, false); return true; }
				wm.inputBackspace(); return true;
			case SDLK_DELETE:    wm.editFocusedText(TE::Delete, false); return true;
			case SDLK_LEFT:
				wm.editFocusedText((ctrl || alt) ? TE::WordLeft : TE::Left, shift);
				return true;
			case SDLK_RIGHT:
				wm.editFocusedText((ctrl || alt) ? TE::WordRight : TE::Right, shift);
				return true;
			case SDLK_HOME:      wm.editFocusedText(TE::Home,  shift);  return true;
			case SDLK_END:       wm.editFocusedText(TE::End,   shift);  return true;
			case SDLK_RETURN:
			case SDLK_KP_ENTER:  wm.inputSubmit(); return true;
			case SDLK_A: if (ctrl) { wm.editFocusedText(TE::SelectAll, false); return true; } break;
			case SDLK_C:
				if (ctrl)
				{
					const std::string sel = wm.focusedSelection();
					if (!sel.empty()) SDL_SetClipboardText(sel.c_str());
					return true;
				}
				break;
			case SDLK_X:
				if (ctrl)
				{
					const std::string sel = wm.focusedSelection();
					if (!sel.empty()) { SDL_SetClipboardText(sel.c_str()); wm.deleteFocusedSelection(); }
					return true;
				}
				break;
			case SDLK_V:
				if (ctrl)
				{
					if (char* clip = SDL_GetClipboardText())
					{ wm.inputText(clip); SDL_free(clip); }
					return true;
				}
				break;
			default: break;
			}
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

bool GameApplication::WantsPresent()
{
	// Outside application mode this is never consulted (the loop only asks in
	// event-driven mode), but answering true keeps the contract honest if that
	// ever changes: a game's world moves every frame whether the UI did or not.
	if (!m_appMode || !m_world) return true;
	// Consuming, once per frame — see WidgetManager::consumeVisualDirty for why
	// asking must also clear.
	return m_world->widgets().consumeVisualDirty();
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
	// A click that landed on a UI element belongs to the UI alone. Swallowing it
	// at the widget call site would not be enough — scripts poll the buttons
	// straight out of this snapshot — so they are masked out of the snapshot
	// itself, which is the one place every frontend reads them from. Movement is
	// deliberately left alone: only the buttons are the UI's.
	const auto inputMode = HE::api::input::mode();
	if (inputMode == HE::api::input::Mode::UIOnly)
	{
		// UI-only is the stronger form of the masking below: not "the click
		// landed on a button" but "gameplay is not being played right now".
		// Keys, buttons and the wheel go; the POSITION stays, because a widget
		// graph asking where the pointer is deserves the truth and a position on
		// its own moves nothing.
		HE::api::input::setKeysDown({});
		HE::api::input::setMouse(HE::api::input::mousePosition(), glm::vec2(0.0f), 0u, 0.0f);
	}
	else if (m_uiWantsPointer)
		HE::api::input::setMouse(HE::api::input::mousePosition(),
		                         HE::api::input::mouseDelta(), 0u,
		                         HE::api::input::scrollDelta());
	// Gamepad snapshot: PUSHED from Input's merged frame, deadzone-filtered —
	// the snapshot never polls SDL itself (one owner per device stream).
	// Reported as disconnected under UI-only: the pad is the menu's now, and a
	// gameplay graph polling it must see the same nothing the keyboard shows.
	if (inputMode == HE::api::input::Mode::UIOnly)
	{
		HE::api::input::setGamepad(false, nullptr, 0, nullptr, 0);
	}
	else
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
	// Not in an application: its camera is a still one that exists only so the
	// frame has a viewpoint, and a fly-camera controller there would answer WASD
	// while the user is typing into a text field.
	if (!m_appMode) updateCameraController(gameDt);

	// Keep the audio listener + spatial sources tracking their entities.
	if (m_world && m_audioEngine.isInitialized())
		AudioSystem::updateSpatial(*m_world, m_audioEngine);

	// Live widgets: per-frame logic tick (EventTick). RAW dt on purpose — the
	// pause menu is a widget, and a menu that stops ticking when the game pauses
	// cannot animate, count down, or hand the player a way back out.
	if (m_world) m_world->widgets().tick(deltaTime);

	// Player instances: Tick + Input.<Action>.* events (mapping ticked against
	// the app Input state, which ProcessEvent keeps current).
	// The frame's MOVEMENT goes straight through to the input mapping: capture
	// only decides whether the OS keeps the cursor in the window, and an
	// uncaptured game still gets motion while the pointer is over it. The
	// BUTTONS are the exception — a mouse-button action binding is gameplay, so
	// it is masked while the pointer sits on a UI element, the same way the
	// editor blanks this very frame outside capture.
	MouseFrame playerMouse = input().mouse();
	if (m_uiWantsPointer || inputMode == HE::api::input::Mode::UIOnly)
		playerMouse.buttons = 0;
	m_playerHost.tick(input(), gameDt, playerMouse);
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
	// An application has none of these in its (empty) world, and every one of them
	// would walk a view of zero entities per frame for the rest of its life.
	if (m_world && !m_appMode)
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

		// Post-process + lighting settings, all read from the same config.json
		// keys the editor's Preferences write, so a shipped game looks like the
		// editor preview it was authored in. Capability-gated where a backend can
		// be unable to do it at all.
		// ── An application draws quads, not a world ──────────────────────────
		// Everything below is a WORLD effect: bloom over emissive surfaces, ambient
		// occlusion between them, anti-aliasing of their edges, global illumination
		// bouncing off them, screen-space reflections in them. An app has none, and
		// leaving them on means paying for a full post-processing chain over an
		// empty scene every frame — which is exactly what made an exported app feel
		// slow next to a running editor, both of them on one GPU.
		//
		// Pushed as OFF rather than skipped: the renderer keeps whatever it was
		// last told, so saying nothing would leave its own defaults (bloom and SSAO
		// are on by default) running.
		if (r && m_appMode)
		{
			r->SetBloomSettings(IRenderer::BloomSettings{ false, 1.0f, 0.6f });
			r->SetSSAOSettings(IRenderer::SSAOSettings{ false, 0.5f, 1.0f, 0 });
			// GI and SSR default to disabled, so their default-constructed form IS
			// the "off" push.
			r->SetGISettings(IRenderer::GISettings{});
			r->SetSSRSettings(IRenderer::SSRSettings{});
			IRenderer::AntiAliasingSettings aaOff;
			aaOff.method = static_cast<int>(HE::AAMethod::Off);
			r->SetAntiAliasingSettings(aaOff);
			// Forward: the deferred path builds a G-buffer and runs a fullscreen
			// lighting resolve, for surfaces that do not exist here.
			r->SetRenderPath(HE::RenderPath::Forward);
			// …and no SKY. Same trap as the effects above, and it was left open:
			// the environment is pushed in the OTHER branch only, so an
			// application never said anything about it and the renderer kept its
			// own default — which draws one. An atmosphere behind a settings
			// dialog is not a subtle bug, and it cost a sky pass per frame.
			r->SetEnvironmentSettings(IRenderer::EnvironmentSettings{ .skyEnabled = false });
		}
		else if (r)
		{
			// Bloom + AO. The packaged game pushed neither for a long time, which
			// meant a shipped build ran on the renderer's built-in defaults no
			// matter what the project was set to.
			r->SetBloomSettings(IRenderer::BloomSettings{
				GlobalState::getInstance().getCustomConfigBool("BloomEnabled", true),
				static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("BloomThreshold", 1.0f)),
				static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("BloomIntensity", 0.6f))});
			r->SetSSAOSettings(IRenderer::SSAOSettings{
				GlobalState::getInstance().getCustomConfigBool("SSAOEnabled", true),
				static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("SSAORadius", 0.5f)),
				static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("SSAOIntensity", 1.0f)),
				GlobalState::getInstance().getCustomConfigInt("SSAOMethod", 0)});

			// Global Illumination — GlobalIlluminationEnabled/GIIndirectIntensity/
			// GILightRadius, capability-gated so non-Metal/non-raytracing builds no-op.
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
			gr.blur         = GlobalState::getInstance().getCustomConfigBool("GIReflBlur", true);
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
