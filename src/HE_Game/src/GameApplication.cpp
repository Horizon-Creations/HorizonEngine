#include "GameApplication.h"
#include <cstdint>
#include "EmbeddedPakKey.h"
#include <fstream>
#include <Hpak/ProjectConfig.h>
#include <Application/AppIcon.h>       // the window icon the export generated
#include <Application/Autostart.h>     // …and the login entry app.setAutostart writes
#ifdef __APPLE__
#include "AppMacMenu.h"                // the menu bar in the system bar (macOS)
#include "AppNotify.h"                 // …and the notification centre
#endif
#ifdef __linux__
#include <Platform/Process.h>          // notifyShow hands the text to notify-send
#endif
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
#include <HorizonScene/TransformHierarchy.h>   // worldPositionOf — the camera position fed to tickWorld
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
#include <functional>                // the host services a Ctx carries (see g_host)
#include <unordered_set>
#include <SDL3/SDL.h>
#include <SDL3/SDL_tray.h>   // the tray icon (plan A7)
#include <list>              // tray ids: addresses SDL's userdata may keep
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <filesystem>

// File-local alias. It used to arrive transitively from the public
// HorizonRendering/ShaderManager.h, which declared it at global scope and so
// leaked `fs` into every consumer of that header.
namespace fs = std::filesystem;

namespace
{
// ── The frame of a borderless window (docs/he-apps-plan.md F3) ──────────────
// The grab band along the edges, in window POINTS, so it is the same physical
// thickness on a Retina screen as on a plain one. Six is what the desktops
// settle around; below four it is a band nobody can hit, above eight it starts
// eating the clicks of what is drawn at the edge of the window.
constexpr float kFrameBorderPoints = 6.0f;
// …and the smallest window an edge drag may leave behind. Handed to SDL as well
// as used by the manual resizer, so the platform that resizes for us and the
// platform we resize ourselves stop at the same place.
constexpr struct { int w, h; } kFrameMinPoints{ 320, 200 };

// ── The host half of every HE::api::Ctx this file builds ─────────────────────
// Everything a Ctx carries that belongs to the APPLICATION rather than to the
// current scene: it is filled once in OnInit and outlives every world, physics
// world and script context. The scene-scoped handles (world, physics, content)
// stay per-call, because a scene switch replaces them.
//
// It exists so that exactly ONE place builds a Ctx here. Assembling one per call
// site is how the half-filled Ctx got everywhere: leave out `audio` and eleven
// audio rows silently return their neutral default, with nothing to see in the
// log. Fill this, and every row works from every call site by construction.
struct HostCtxParts
{
	AudioEngine*          audio    = nullptr;
	HorizonCode::Runtime* runtime  = nullptr;
	EntityHost*           entities = nullptr;
	// "Create/destroy an object of this class" as the host means it — the very
	// lambdas the HorizonCode services get, so a spawn from Lua, from Python and
	// from a Create Object node are the same operation and not three.
	std::function<uint32_t(const std::string&, const float*, const float*)> createObject;
	std::function<void(uint32_t)> destroyObject;
	std::function<void()>         quit;
	// The window rows (app.setTitle/setSize/size) and app.requestRedraw. They
	// belong HERE and not at a call site, which is the lesson this struct exists
	// to teach: they were bound at the one Ctx the HorizonCode path builds, so a
	// Lua or Python script asking for the window size got a silent zero.
	std::function<void(const std::string&)> setWindowTitle;
	std::function<void(uint32_t, uint32_t)> setWindowSize;
	std::function<glm::vec2()>              windowSize;
	std::function<void()>                   requestRedraw;
	// The other two title-bar buttons (plan F3), bound here for the reason the
	// three rows above are: a Lua script and a graph must reach the same window.
	std::function<void()>     minimizeWindow;
	std::function<void(bool)> setWindowMaximized;
	std::function<bool()>     windowMaximized;
	// The tray rows, bound for the same reason and in the same place.
	std::function<void(const std::string&)>                     showTray;
	std::function<void()>                                       hideTray;
	std::function<void(const std::string&, const std::string&)> addTrayItem;
	std::function<void()>                                       clearTrayMenu;
	std::function<bool(bool)>                                   setAutostart;
	std::function<bool()>                                       autostart;
	std::function<void(const std::string&, const std::string&)> addMenu;
	std::function<void(const std::string&, const std::string&, const std::string&,
	                   const std::string&)> addMenuItem;
	std::function<void(const std::string&)>                     addMenuSeparator;
	std::function<void()>                                       clearMenuBar;
	std::function<void(const std::string&, bool)>               setMenuItemEnabled;
	std::function<void(const std::string&, bool)>               setMenuItemChecked;
	std::function<bool(const std::string&)>                     menuItemEnabled;
	std::function<bool(const std::string&)>                     menuItemChecked;
};
// One application per process; cleared in OnShutdown so nothing here outlives
// the object its lambdas capture.
HostCtxParts g_host;

// ── The tray (plan A7) ───────────────────────────────────────────────────────
// Beside g_host and for its reason: one application per process. SDL hands a
// tray entry's callback a bare void*, so the thing it points at must outlive the
// click and never move — a std::list of ids does both, a vector would not.
struct TrayItem { std::string id; };
SDL_Tray*            g_tray     = nullptr;
SDL_TrayMenu*        g_trayMenu = nullptr;
std::list<TrayItem>  g_trayIds;
// A click arrives from INSIDE SDL's event pump. Firing a graph from there would
// re-enter the interpreter in the middle of a frame, so the id waits here and
// the frame loop delivers it — the same shape the drop path uses.
std::vector<std::string> g_trayClicks;

// The menu bar changed and the SYSTEM bar has not been told yet (macOS). Set by
// the four app.*Menu* callbacks, acted on once per frame — a graph that builds a
// menu of six entries touches the bar seven times, and rebuilding NSMenus seven
// times to arrive at the same bar is six rebuilds nobody asked for.
bool g_menuDirty = false;

void SDLCALL trayEntryClicked(void* userdata, SDL_TrayEntry*)
{
    if (const TrayItem* item = static_cast<const TrayItem*>(userdata))
        g_trayClicks.push_back(item->id);
}

// ── Notifications (plan C) ───────────────────────────────────────────────────
// One function, three answers, and only one of them written where it can be
// seen working. macOS goes through UserNotifications (AppNotify.mm). Linux hands
// the text to `notify-send`, the tool every desktop that has a notification
// daemon ships with — the D-Bus call underneath it would be the same message
// with a protocol implementation around it, and this one is readable.
//
// Windows has neither and gets a warning instead of a wrong guess: its toasts
// need a WinRT activation identity and a Start-menu shortcut to post from, which
// is a piece of work rather than a fallback, and nothing here could be tested.
bool notifyAvailable()
{
#if defined(__APPLE__)
    return HE::AppNotify::available();
#elif defined(__linux__)
    return HE::Proc::which("notify-send").has_value();
#else
    return false;
#endif
}

bool notifyShow(const std::string& title, const std::string& body)
{
#if defined(__APPLE__)
    return HE::AppNotify::show(title, body);
#elif defined(__linux__)
    const auto tool = HE::Proc::which("notify-send");
    if (!tool)
    {
        HE_LOG_WARN(Core, "%s", "notify: notify-send is not installed — nothing was shown");
        return false;
    }
    // Title first, then the text, which is notify-send's own argument order.
    // Two arguments and never a shell line: a title with a quote in it would
    // otherwise be somebody else's command.
    HE::Proc::Options opt;
    opt.exe = *tool;
    opt.args.push_back(title.empty() ? body : title);
    if (!title.empty() && !body.empty()) opt.args.push_back(body);
    opt.timeoutMs = 5000;
    return HE::Proc::run(opt).ok();
#else
    (void)title; (void)body;
    HE_LOG_WARN(Core, "%s", "notify: not implemented on this platform yet");
    return false;
#endif
}

// What a login entry has to point at. SDL_GetBasePath gives the directory the
// executable lives in — inside a .app that is Contents/Resources, and launching
// THAT does nothing, so the bundle itself is what macOS is told to open.
std::filesystem::path executablePathForAutostart()
{
    const char* base = SDL_GetBasePath();
    if (!base) return {};
    std::filesystem::path p(base);
#ifdef __APPLE__
    // …/Foo.app/Contents/Resources/ → …/Foo.app
    for (std::filesystem::path up = p; !up.empty() && up != up.root_path(); up = up.parent_path())
        if (up.extension() == ".app") return up;
#endif
    return p / "HorizonGame";
}

void destroyTray()
{
    if (g_tray) SDL_DestroyTray(g_tray);   // takes the menu and its entries with it
    g_tray     = nullptr;
    g_trayMenu = nullptr;
    g_trayIds.clear();
}

void trayShow(const std::string& tooltip)
{
    if (g_tray)
    {
        // Already up: showing it again is how a tooltip is changed.
        SDL_SetTrayTooltip(g_tray, tooltip.empty() ? nullptr : tooltip.c_str());
        return;
    }
    // The icon the export generated — the same picture as the window's, because
    // an application with two different icons is two applications to whoever is
    // looking at the screen.
    SDL_Surface* icon = nullptr;
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    if (const char* base = SDL_GetBasePath())
        if (HE::heLoadPngRGBA(std::filesystem::path(base) / "AppIcon.png", rgba, w, h))
            icon = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);

    g_tray = SDL_CreateTray(icon, tooltip.empty() ? nullptr : tooltip.c_str());
    if (icon) SDL_DestroySurface(icon);
    if (!g_tray)
    {
        HE_LOG_WARN(Core, "app.showTray: the system refused a tray icon (%s)", SDL_GetError());
        return;
    }
    g_trayMenu = SDL_CreateTrayMenu(g_tray);
}

void trayAddItem(const std::string& id, const std::string& label)
{
    if (!g_trayMenu)
    {
        HE_LOG_WARN(Core, "%s", "app.addTrayItem: there is no tray yet — call Show Tray Icon first");
        return;
    }
    g_trayIds.push_back({ id });
    SDL_TrayEntry* entry =
        SDL_InsertTrayEntryAt(g_trayMenu, -1, label.c_str(), SDL_TRAYENTRY_BUTTON);
    if (!entry)
    {
        g_trayIds.pop_back();
        HE_LOG_WARN(Core, "app.addTrayItem: %s", SDL_GetError());
        return;
    }
    SDL_SetTrayEntryCallback(entry, trayEntryClicked, &g_trayIds.back());
}

void trayClearMenu()
{
    if (!g_trayMenu) return;
    int count = 0;
    // Copied first: removing an entry invalidates the list SDL handed back.
    const SDL_TrayEntry** entries = SDL_GetTrayEntries(g_trayMenu, &count);
    std::vector<SDL_TrayEntry*> doomed;
    for (int i = 0; i < count; ++i)
        doomed.push_back(const_cast<SDL_TrayEntry*>(entries[i]));
    for (SDL_TrayEntry* e : doomed) SDL_RemoveTrayEntry(e);
    // The ids go with them; nothing points at them any more.
    g_trayIds.clear();
}

// The only Ctx factory in this file. `self` is the calling HorizonCode instance
// (0 for everything that is not a graph — the scene-request pump, Lua, Python).
HE::api::Ctx apiCtx(HorizonWorld* world, PhysicsWorld* physics, ContentManager* content,
                    uint32_t self = 0)
{
	HE::api::Ctx c;
	c.world         = world;
	c.physics       = physics;
	c.content       = content;
	c.audio         = g_host.audio;
	c.runtime       = g_host.runtime;
	c.self          = self;
	c.entities      = g_host.entities;
	c.createObject  = g_host.createObject;
	c.destroyObject = g_host.destroyObject;
	c.requestQuit   = g_host.quit;
	c.setWindowTitle = g_host.setWindowTitle;
	c.setWindowSize  = g_host.setWindowSize;
	c.windowSize     = g_host.windowSize;
	c.requestRedraw  = g_host.requestRedraw;
	c.minimizeWindow     = g_host.minimizeWindow;
	c.setWindowMaximized = g_host.setWindowMaximized;
	c.windowMaximized    = g_host.windowMaximized;
	c.showTray       = g_host.showTray;
	c.hideTray       = g_host.hideTray;
	c.addTrayItem    = g_host.addTrayItem;
	c.clearTrayMenu  = g_host.clearTrayMenu;
	c.setAutostart   = g_host.setAutostart;
	c.autostart      = g_host.autostart;
	c.addMenu          = g_host.addMenu;
	c.addMenuItem      = g_host.addMenuItem;
	c.addMenuSeparator = g_host.addMenuSeparator;
	c.clearMenuBar     = g_host.clearMenuBar;
	c.setMenuItemEnabled = g_host.setMenuItemEnabled;
	c.setMenuItemChecked = g_host.setMenuItemChecked;
	c.menuItemEnabled    = g_host.menuItemEnabled;
	c.menuItemChecked    = g_host.menuItemChecked;
	// Not through g_host: notifications need nothing from the application object,
	// so they are the platform function itself and there is no state to capture.
	c.notify          = [](const std::string& t, const std::string& b) { return notifyShow(t, b); };
	c.notifyAvailable = [] { return notifyAvailable(); };
	return c;
}

// What this runtime runs on before the config could name a backend, and what an
// unusable choice falls back to. It used to be a constexpr picked by __APPLE__,
// which stopped being the right question with the runtime flavours (A3b): an
// app-basic runtime links the software rasterizer ALONE, and defaulting it to
// Metal would throw in RendererFactory before the window opens. The library that
// links the backends is the only one that knows, so it answers.
static HE::RendererBackend defaultBackend()
{
	return RendererFactory::Default();
}

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
// for one whose implementation was not compiled in, so the question has to be
// asked before the answer is used — a config authored for another platform, or
// for another runtime flavour, must fall back and not abort the game before its
// window opens. The duplicate #ifdef ladder that used to stand here is gone: it
// had to be kept in step with the factory by hand, and the flavours (A3b) are
// exactly the change that would have broken that.
bool backendAvailable(HE::RendererBackend backend)
{
	return RendererFactory::Available(backend);
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
	m_backend = defaultBackend();

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
	//
	// The answer is LATCHED into m_appMode, because the window mode is not the
	// only thing that has to know before OnInit reads the real config: the
	// FPS-style mouse grab at the top of OnInit is the other one (plan E6).
	// OnInit overwrites the member from the same file a moment later, so this is
	// the same answer arriving earlier, not a second source of truth.
	if (baseRaw)
	{
		ProjectConfig peek;
		if (ProjectConfigLoader::load(fs::path(baseRaw), peek) && peek.appMode)
		{
			m_appMode    = true;
			m_windowMode = HE::WindowMode::Windowed;
		}
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
	// Alt-tabbing out of a single-player game and coming back to a corpse is a
	// complaint, not a feature, so this is on by default. It is still a switch
	// and not an automatism: a game with its own pause menu wants to open THAT
	// on focus loss, and a multiplayer client must not freeze at all. Read once
	// here rather than per event — the file does not change mid-session.
	m_pauseOnFocusLoss = gs.getCustomConfigBool("PauseOnFocusLoss", m_pauseOnFocusLoss);

	if (const std::string name = gs.getCustomConfigString("GameBackend"); !name.empty())
	{
		HE::RendererBackend wanted = defaultBackend();
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
	// The flavour goes in the log next to the backend because the two are the
	// whole difference between the three shipped runtimes, and a report that
	// says "the app is slow" is worth nothing without knowing which one ran.
	HE_LOG_INFO(Core, "GameApplication: creating renderer (runtime flavour '%s')",
	            RendererFactory::RuntimeFlavor());
	return RendererFactory::Create(m_backend);
}

void GameApplication::OnInit()
{
	HE_LOG_INFO(Core, "%s", "GameApplication::OnInit");

	// Grab the mouse on startup (FPS-style look). Done first so it holds even on
	// the early-return paths below (no hcfg / no pak); Esc toggles it back so the
	// cursor is always reachable. The window is already open by the time OnInit runs.
	//
	// …and that last sentence is exactly why an APPLICATION must not come
	// through here (plan E6). The window is open, so this hides the system
	// cursor and puts the window into relative mode; the release further down,
	// once project.hcfg is read, undoes it — but a tool that flashes the pointer
	// away on launch is a tool that looks like a game engine's leftovers. The
	// constructor already peeked at the same file, so the answer is available
	// before the grab rather than after it. A game, and every path where there
	// is no hcfg to peek at, behaves exactly as before.
	if (!m_appMode)
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

	// ── The window icon (plan A7) ────────────────────────────────────────────
	// AppIcon.png sits beside the data because the exporter generated it there.
	// On macOS the Dock icon comes from the bundle's .icns and setting one here
	// would replace it with the bare bitmap, so this is the other platforms'
	// path — where the window icon IS the taskbar entry. Same reasoning the
	// editor's own icon follows.
#ifndef __APPLE__
	if (SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr)
		HE::heSetWindowIcon(w, exeDir / "AppIcon.png");
#endif

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
		// Normally already false: the grab at the top of OnInit is skipped for an
		// application, off the constructor's peek at this same file. This is the
		// case the peek cannot answer — a config that only becomes readable HERE
		// (SDL_GetBasePath disagreeing with itself, a hcfg written between the
		// two reads). Cheap, and an app that swallowed the cursor is not a bug
		// the user can talk their way out of.
		setMouseCaptured(false);
		// A2: draw on events, not on a clock.
		setEventDriven(true);

		// ── F3: the frame this window asked the OS to leave off ──────────────
		// Borderless plus an application means "I draw my own title bar", and
		// from that moment the OS has to be told what its picture means: which
		// part carries the window, which parts are its edges. The widget tree
		// answers both, through one callback, and the whole rest of the feature
		// is that answer being right.
		//
		// Only for an application, and only for Borderless: a game that goes
		// borderless means "fullscreen without the mode switch", and a fullscreen
		// window with draggable regions in it would be a window the user could
		// pull off their screen.
		if (m_windowMode == HE::WindowMode::Borderless && window())
		{
			// The same floor the manual resizer clamps to, given to SDL as well:
			// on Windows and Linux the window manager does the resizing and would
			// otherwise let the window shrink to nothing.
			if (SDL_Window* fw = window()->GetNativeWindow())
				SDL_SetWindowMinimumSize(fw, kFrameMinPoints.w, kFrameMinPoints.h);
			window()->SetHitTest([this](int px, int py) { return frameHitAt(px, py); });
			m_customFrame = true;
			HE_LOG_INFO(Core, "%s", "GameApplication: borderless window — the widget tree "
			                        "owns the title bar and the resize edges (plan F3)");
		}
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
	// What this build may reach outside itself, straight off the hcfg. Set even
	// when everything is false: perm::set replaces the whole struct, and a
	// process that ran an application with permissions and then one without has
	// to end up with the second one's answer.
	{
		HE::api::perm::Grants g;
		g.files     = m_config.allowFiles;
		g.processes = m_config.allowProcesses;
		g.network   = m_config.allowNetwork;
		HE::api::perm::set(g);
	}
	// Which scripts the text in this build is written in, straight off the hcfg
	// and before anything draws: the atlas is baked once, on first use, so this
	// is the only moment it can be decided. A shipped app never sees the editor,
	// which is why the answer travels in the config rather than being asked.
	HE::uiSetFontScripts(m_config.fontScripts);
	HE::uiSetFontWeightBold(m_config.fontWeightBold);

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
		// The app-level half of every Ctx, published once, here: this is where the
		// object services are written, and a second copy of them somewhere else is
		// how the two paths drift apart.
		g_host.audio    = &m_audioEngine;
		g_host.runtime  = &m_gameInstance.runtime();
		g_host.entities = &m_entityHost;
		// The shipped game IS the application, so app.quit leaves the loop for
		// real (the editor binds the same hook to stopping play mode instead).
		g_host.quit     = [this]{ Quit(); };
		// The shipped build owns its window outright, so unlike the editor there
		// is nothing to protect here — a graph that resizes it means it. And
		// "quit" means quit: the shipped game IS the application, so app.quit
		// leaves the loop for real (the editor binds the same hook to stopping
		// play mode instead). A main menu's Exit button has nothing else to call.
		g_host.setWindowTitle = [this](const std::string& t) { setWindowTitle(t); };
		g_host.setWindowSize  = [this](uint32_t w, uint32_t h) { setWindowSize(w, h); };
		g_host.windowSize     = [this] {
			const HE::Window* w = window();
			return w ? glm::vec2(static_cast<float>(w->GetWidth()),
			                     static_cast<float>(w->GetHeight()))
			         : glm::vec2(0.0f);
		};
		g_host.requestRedraw  = [this] { requestRedraw(); };
		// The two title-bar buttons that had nothing to call (plan F3). Bound
		// wherever setWindowSize is bound and for the same reason: the shipped
		// game owns its window, so a graph that minimises it means it.
		g_host.minimizeWindow     = [this] { setWindowMinimized(); };
		g_host.setWindowMaximized = [this](bool on) { setWindowMaximized(on); };
		g_host.windowMaximized    = [this] { return windowMaximized(); };
		g_host.showTray       = [](const std::string& tip) { trayShow(tip); };
		g_host.hideTray       = [] { destroyTray(); };
		g_host.addTrayItem    = [](const std::string& id, const std::string& label)
		                        { trayAddItem(id, label); };
		g_host.clearTrayMenu  = [] { trayClearMenu(); };
		// Autostart needs three things the host is the only one to know: who this
		// application is, what it is called, and where its executable actually
		// sits right now — a login entry pointing at where it USED to be is worse
		// than none.
		g_host.setAutostart   = [this](bool on) {
			return HE::heSetAutostart(m_config.bundleId, m_config.projectName,
			                          executablePathForAutostart(), on);
		};
		g_host.autostart      = [this] { return HE::heAutostart(m_config.bundleId); };
		// The menu bar lives in the widget manager, which is where it is drawn.
		// Built row by row here rather than handed over whole: a graph adds a
		// menu, then its entries, and each call has to land somewhere.
		//
		// Every one of them only marks the bar dirty for the SYSTEM menu (macOS);
		// rebuilding NSMenus once per call would rebuild them four times while a
		// graph's OnInit lays out one menu, and the frame loop is where that kind
		// of work belongs anyway.
		g_host.addMenu = [this](const std::string& id, const std::string& label) {
			std::vector<HE::AppMenu> menus = m_widgets.menuBar();
			for (const HE::AppMenu& m : menus)
				if (m.id == id) return;   // adding the same menu twice is one menu
			menus.push_back({ id, label, {} });
			m_widgets.setMenuBar(std::move(menus));
			g_menuDirty = true;
		};
		g_host.addMenuItem = [this](const std::string& menuId, const std::string& id,
		                            const std::string& label, const std::string& shortcut) {
			std::vector<HE::AppMenu> menus = m_widgets.menuBar();
			for (HE::AppMenu& m : menus)
				if (m.id == menuId)
				{
					m.items.push_back({ id, label, false, shortcut });
					m_widgets.setMenuBar(std::move(menus));
					g_menuDirty = true;
					return;
				}
			HE_LOG_WARN(Core, "app.addMenuItem: no menu called '%s'", menuId.c_str());
		};
		g_host.addMenuSeparator = [this](const std::string& menuId) {
			std::vector<HE::AppMenu> menus = m_widgets.menuBar();
			for (HE::AppMenu& m : menus)
				if (m.id == menuId)
				{
					m.items.push_back({ "", "", true, "" });
					m_widgets.setMenuBar(std::move(menus));
					g_menuDirty = true;
					return;
				}
		};
		g_host.clearMenuBar = [this] { m_widgets.setMenuBar({}); g_menuDirty = true; };
		// The two rows that change while the program runs. Straight through to
		// the manager rather than round the copy-modify-setMenuBar dance the
		// four above do: setMenuBar closes an open menu, and this is the call a
		// graph makes precisely while one is open.
		g_host.setMenuItemEnabled = [this](const std::string& id, bool on) {
			if (!m_widgets.setMenuItemEnabled(id, on))
				HE_LOG_WARN(Core, "app.setMenuItemEnabled: no menu entry called '%s'",
				            id.c_str());
			else g_menuDirty = true;
		};
		g_host.setMenuItemChecked = [this](const std::string& id, bool on) {
			if (!m_widgets.setMenuItemChecked(id, on))
				HE_LOG_WARN(Core, "app.setMenuItemChecked: no menu entry called '%s'",
				            id.c_str());
			else g_menuDirty = true;
		};
		g_host.menuItemEnabled = [this](const std::string& id) {
			return m_widgets.menuItemEnabled(id);
		};
		g_host.menuItemChecked = [this](const std::string& id) {
			return m_widgets.menuItemChecked(id);
		};
		g_host.createObject = [this](const std::string& p, const float* pos,
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
					// Copied NOW. resolveClassAsset below loads every ancestor of
					// this class, and asset pointers live in a dense vector — so
					// `ea` is dead from the next line, and the string it owns with
					// it. That matters twice here: it is the argument being passed
					// in, and it is read again afterwards.
					const std::string assetPath = ea->path;
					// The RESOLVED engine base: a class deriving from another class
					// that is an Entity is one too.
					const HorizonCode::ResolvedClass rc =
						HorizonCode::resolveClassAsset(contentManager(), assetPath);
					if (HorizonCode::engineClassIsA(rc.engineBase, "Entity"))
					{
						// Placement travels with the spawn (null = authored), so
						// Construct/BeginPlay already run at the destination.
						const HorizonCode::InstanceId inst =
							m_entityHost.spawn(assetPath, entt::null, pos, rot).instance;
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
			// Same reason as the Entity branch above: the resolve loads, and the
			// asset pool moves when it does.
			const std::string assetPath = a->path;
			// The asset's OWN path is the class key, matching what the compiled
			// branch above gets from classKey() — so one class stays one class
			// to a Cast no matter which backend served this instance. The graph
			// is the FLATTENED one: this class plus what it inherits.
			HorizonCode::ResolvedClass rc =
				HorizonCode::resolveClassAsset(contentManager(), assetPath);
			const HorizonCode::InstanceId inst = m_gameInstance.runtime().addLevels(
				std::move(rc.levels), {}, { assetPath, rc.engineBase, rc.chain });
			m_gameInstance.runtime().fireConstruct(inst);
			return inst;
		};
		g_host.destroyObject = [this](uint32_t ref){
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
			{
				// Hand the bodies back BEFORE the entities go: afterwards the
				// hierarchy is gone and the subtree cannot be walked. After
				// Destruct rather than before, so a dying object's last frame of
				// logic still sees the physics it lived in.
				if (m_physicsWorld)
					m_physicsWorld->removeEntityTree(*m_world, owned);
				m_world->destroyEntity(static_cast<Entity>(owned));
			}
		};
		// HorizonCode reaches the two through its services; the registry rows that
		// Lua and Python call reach the SAME lambdas through the Ctx apiCtx()
		// builds. Copies of one std::function, not a second implementation.
		svc.createObject  = g_host.createObject;
		svc.destroyObject = g_host.destroyObject;
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
			// The caller travels along — see the editor's twin — and so does the
			// whole app-level half of the Ctx (audio, runtime, entity host, the
			// object services, the window rows, quit): apiCtx() is the one place
			// that fills it.
			HE::api::Ctx c = apiCtx(m_world.get(), m_physicsWorld.get(),
			                        &contentManager(), self);
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
		// Physics first of the three, because startPhysics() is what hands the
		// entity host the world it builds spawned bodies in.
		startPhysics();
		// Then the entity host, still before the player host: a controller's
		// BeginPlay is where the game spawns its character with Create Object,
		// and that spawn is only served — with an entity, and now with a body on
		// it — while the entity host is running. The body half of that sentence
		// used to be a claim rather than a fact; EntityHost::spawn makes it true.
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

	// A scene must not inherit the last one's time controls. Whoever loaded a
	// level in slow motion, or from behind a pause menu, meant to leave it —
	// nothing in the new scene knows to lift a scale it never set. elapsed() and
	// frameCount() deliberately keep running: they are session clocks, and a
	// session clock that restarts at every door is not one.
	HE::api::time::resetControls();

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

			HE::api::Ctx c = apiCtx(m_world.get(), m_physicsWorld.get(), &contentManager());
			// Placement: move the zone's root to the requested position (zero =
			// as authored; the merge root is a fresh identity entity).
			if (r.pos != glm::vec3(0.0f))
				HE::api::scene::setZonePosition(c, r.zone, r.pos);
			// Hidden zones load with their renderables invisible until Show Zone.
			if (r.hidden)
				HE::api::scene::setZoneVisible(c, r.zone, false);

			// A streamed-in zone needs COLLISION, or the player walks through its
			// floor: loadSceneInto only deserialises components, and nothing
			// builds bodies for entities that arrive that way — initialize() ran
			// once at scene start, and step() only reaps what has gone away.
			// `created` already holds every entity of the zone, strays included,
			// so it is one addEntity each rather than a tree walk per root.
			//
			// Placed between the two on purpose: after setZonePosition, so the
			// root's body is built where the zone ends up, and before
			// startScriptsFor, so a zone graph's BeginPlay sees a world it can
			// stand on.
			if (m_physicsWorld)
				for (entt::entity e : created)
					m_physicsWorld->addEntity(*m_world, (uint32_t)e);

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
			HE::api::Ctx c = apiCtx(m_world.get(), m_physicsWorld.get(), &contentManager());
			HE::api::scene::setZoneVisible(c, r.zone, r.flag);
			break;
		}
		case Kind::ZonePosition: // move a zone (queued so it orders after a load)
		{
			if (!m_world) break;
			// The physics world travels in the Ctx because setZonePosition needs
			// it: moving the root moves what is DRAWN, while a body is baked once
			// from the pose its entity had when it was built. So the call moves
			// the root AND rebuilds every zone entity that already has physics,
			// at the place it now stands. With a null here that rebuild is
			// skipped and the zone would keep colliding where it was authored.
			HE::api::Ctx c = apiCtx(m_world.get(), m_physicsWorld.get(), &contentManager());
			HE::api::scene::setZonePosition(c, r.zone, r.pos);
			break;
		}
		case Kind::UnloadZone:  // unload additive zone
		{
			const HE::api::scene::ZoneInfo* z = HE::api::scene::zoneInfo(r.zone);
			if (!z || !m_world) break;
			auto& reg = m_world->registry();
			// Bodies go back in a pass of their OWN, before anything is
			// destroyed. Destroying a zone entity takes its whole subtree with
			// it, so by the time the loop below reaches a child's id that child
			// is already invalid and skipped — most of the zone's colliders
			// would then survive until step()'s sweep noticed them. removeEntity
			// needs no valid handle and is a silent no-op on an unknown id,
			// which is what makes an unconditional first pass the simple answer.
			if (m_physicsWorld)
				for (uint32_t id : z->entities) m_physicsWorld->removeEntity(id);
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
	// BEFORE initialize(): mesh and convex-hull colliders are cut from the
	// entity's mesh asset, and without a content manager to hand it over they
	// fall back to a box — a house that collides as a crate. PhysicsWorld says
	// so at ERROR, which in the editor reaches the notification centre; a
	// packaged game has nothing but its log file, so this line is the only
	// thing standing between a shipped level and crate-shaped houses.
	m_physicsWorld->setContentManager(&contentManager());
	m_physicsWorld->initialize(*m_world);
	// Every runtime spawn goes through the entity host, so it is the host that
	// has to know where bodies are built. Set HERE rather than at the two call
	// sites because this function is the only place the physics world is
	// replaced — including on a scene switch, where a host still pointing at the
	// previous one would spawn into freed memory.
	m_entityHost.setPhysicsWorld(m_physicsWorld.get());
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
	// What a text script cannot reach on its own. Lua and Python build their own
	// HE::api::Ctx from world/physics/content alone, so without this the rows
	// behind audio.*, the runtime and the entity host answer with their neutral
	// default and say nothing — and entity.spawnClass would be dead in exactly
	// the two languages it was added for. Bound BEFORE the scripts start, like
	// the quit hook: an onStart may already spawn.
	{
		ScriptContext::HostServices hs;
		hs.audio         = &m_audioEngine;
		hs.entities      = &m_entityHost;
		hs.runtime       = &m_gameInstance.runtime();
		hs.createObject  = g_host.createObject;   // the same lambdas HorizonCode uses
		hs.destroyObject = g_host.destroyObject;
		m_scriptContext->setHostServices(std::move(hs));
	}

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

HE::UIWindowHit GameApplication::frameHitAt(int pointX, int pointY)
{
	if (!m_customFrame) return HE::UIWindowHit::Normal;
	SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr;
	if (!w) return HE::UIWindowHit::Normal;

	// SDL asks in window points; the widget tree lays out and hit-tests in
	// drawable pixels. On a Retina display those differ by two, and skipping
	// this line would put the title bar at half its height — the same class of
	// mistake F2 found twice.
	int ww = 1, wh = 1, pw = 1, ph = 1;
	SDL_GetWindowSize(w, &ww, &wh);
	SDL_GetWindowSizeInPixels(w, &pw, &ph);
	const float sx = ww > 0 ? static_cast<float>(pw) / ww : 1.0f;
	const float sy = wh > 0 ? static_cast<float>(ph) / wh : 1.0f;

	return m_widgets.windowHitAt(static_cast<float>(pw), static_cast<float>(ph),
	                             pointX * sx, pointY * sy,
	                             kFrameBorderPoints * sx);
}

bool GameApplication::menuShortcutFromKey(const SDL_KeyboardEvent& key)
{
	if (!m_world) return false;

	// The name comes from the SCANCODE with no modifiers, not from the keycode
	// in the event. The keycode is what the layout says the key produces right
	// now, so Shift+2 arrives as "quotedbl" on a German keyboard and Ctrl+Alt+Q
	// as something else again on the layouts that put a character there — a
	// chord written "Ctrl+Shift+2" would then match on one keyboard and not on
	// the next. The scancode is the physical key, and the unmodified keycode is
	// the name a person would write for it.
	const SDL_Keycode plain = SDL_GetKeyFromScancode(key.scancode, SDL_KMOD_NONE, false);
	const char* name = SDL_GetKeyName(plain);
	if (!name || !*name) return false;

	// Cmd and Ctrl are one flag here, exactly as they are in the text-editing
	// keys above: an application says Ctrl+S once and gets Cmd+S on a Mac.
	return m_world->widgets().fireMenuShortcut(
		name,
		(key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0,
		(key.mod & SDL_KMOD_SHIFT) != 0,
		(key.mod & SDL_KMOD_ALT) != 0);
}

void GameApplication::updateUIInput()
{
	if (!m_world) return;

	SDL_Window* const primary = window() ? window()->GetNativeWindow() : nullptr;
	float mx = 0.0f, my = 0.0f;
	const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);

	// ── Which window are those coordinates in? ───────────────────────────────
	// SDL_GetMouseState reports relative to the window the mouse is OVER, so
	// the size and the scale have to come from that window too — on a second
	// monitor with a different setting, taking the main window's numbers puts
	// every click somewhere else entirely. Off every window of ours (or no
	// second window at all) means the main one, which is what it always was.
	SDL_Window* w = SDL_GetMouseFocus();
	uint32_t uiWindow = 0;
	if (w && w != primary)
	{
		const uint32_t sid = SDL_GetWindowID(w);
		if (getWindow(HE::WindowHandle{ sid })) uiWindow = sid;
		else                                    w = primary;   // not one of ours
	}
	else
		w = primary;

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

	// The system scaling, asked fresh every frame rather than watched for
	// SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: it is one cheap call, and it is
	// right the frame the window is dragged onto a second monitor with a
	// different setting. This is what a ConstantPixel canvas lays out in, and
	// it is NOT the sx/sy above — that pair converts window points to pixels
	// (macOS Retina), this one also carries what Windows and X11 call the
	// content scale, where the points ARE pixels and only the scale says 200%.
	// …for THIS window. Two windows on two monitors have two of these, and the
	// one that matters is the one the pointer is in.
	m_widgets.setDisplayScale(uiWindow, w ? SDL_GetWindowDisplayScale(w) : 1.0f);

	// While the fly-look holds the mouse captive there is no visible cursor —
	// hover states clear and nothing is clickable (Esc releases the mouse).
	//
	// Game-only routing joins that condition rather than skipping this function:
	// processPointer still has to RUN with an invalid pointer, because that is
	// what clears a hover the UI is already showing. Returning early would leave
	// the last hovered button lit for as long as the mode lasts.
	const bool uiTakesInput = HE::api::input::mode() != HE::api::input::Mode::GameOnly;
	// …and an edge drag owns the pointer while it lasts, the same way the
	// fly-look does: the mouse is sizing the window, not pointing at anything in
	// it, and a slider under the edge that starts following it would be the UI
	// answering a gesture that was never aimed at it.
	const bool pointerValid = !m_mouseCaptured && w != nullptr && uiTakesInput &&
	                          !m_frameResize.active();

	// Widget pointer input first — widgets draw on top of entity UI. The answer
	// ("the pointer is on something clickable") is kept, not dropped: it is what
	// masks the mouse buttons out of gameplay next frame, so pressing a menu
	// button does not also fire the weapon behind it.
	m_uiWantsPointer =
		m_world->widgets().processPointer(uiWindow,
		                                  static_cast<float>(pw), static_cast<float>(ph),
		                                  mx * sx, my * sy,
		                                  (buttons & SDL_BUTTON_LMASK) != 0, pointerValid,
		                                  (buttons & SDL_BUTTON_RMASK) != 0);

	// Tell the OS where the focused field is, so an input method opens its
	// candidate list beside it instead of in the corner of the screen. Pushed
	// every frame a field is focused: the field can move (a scrolled list, a
	// resized window), and SDL only remembers what it was last told.
	// The window the field is IN, which is the one the pointer just wrote to.
	if (SDL_Window* win = w)
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
		{
			// Text first, then the things for which a double-click means "open
			// this": a list row. One gesture, one meaning per thing it lands on.
			if (!m_world->widgets().selectWordAtPointer(static_cast<float>(pw),
			                                            static_cast<float>(ph),
			                                            mx * sx, my * sy))
				m_world->widgets().activateAtPointer(static_cast<float>(pw),
				                                     static_cast<float>(ph), mx * sx, my * sy);
		}
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

	// ── …and the edge cursors of a borderless window, on the one platform ────
	// Windows, X11 and Wayland set the resize cursor themselves the moment the
	// hit test names an edge — doing it a second time from here would fight
	// them. macOS reads only SDL_HITTEST_DRAGGABLE and never touches the cursor,
	// so an edge there would look like ordinary content right up until it moved.
	// AFTER applyUICursor, because on an edge the frame outranks whatever the
	// element under it wanted.
#if defined(__APPLE__)
	// Main window only: the borderless frame is the application's own chrome,
	// and a tool window has the system's title bar like everything else.
	if (m_customFrame && uiWindow == 0 && w && !m_mouseCaptured)
	{
		const HE::UIWindowHit fh = m_frameResize.active()
			? m_frameResize.edge()
			: frameHitAt(static_cast<int>(mx), static_cast<int>(my));
		if (HE::uiWindowHitIsResize(fh))
			HE::applyUICursor(HE::uiWindowHitCursor(fh));
	}
#endif

	// ── Keyboard / gamepad menu navigation ───────────────────────────────────
	// A menu has to be usable without a mouse. Arrow keys and the pad's D-Pad
	// move the focus, Enter/Space and the south button activate it. Not routed
	// while a text field has the keyboard: there the arrows belong to the text.
	// ── Back, and NOT behind the text-field gate ─────────────────────────────
	// isEditingText() is "something has the keyboard", and showModal gives
	// the keyboard to the dialog by construction — so putting this inside the
	// block below would switch it off at exactly the moment a dialog is up. It
	// also has to work WHILE typing: cancelling a dialog from inside its own
	// input field is what Escape means everywhere.
	if (uiTakesInput)
	{
		const bool back = input().isGamepadButtonDown(SDL_GAMEPAD_BUTTON_EAST);
		if (back && !m_uiBackPrev) m_world->widgets().closeTopLayer();
		m_uiBackPrev = back;

		// Tab is the other way through a form, and it belongs OUTSIDE the gate
		// below for the same reason Escape does: it has to work while a text
		// field has the keyboard, because leaving that field is exactly what it
		// is for. Shift+Tab goes back.
		const bool tab = input().IsKeyDown(SDL_SCANCODE_TAB);
		if (tab && !m_uiTabPrev)
		{
			const bool back2 = input().IsKeyDown(SDL_SCANCODE_LSHIFT) ||
			                   input().IsKeyDown(SDL_SCANCODE_RSHIFT);
			m_world->widgets().focusNext(back2, static_cast<float>(pw),
			                             static_cast<float>(ph));
		}
		m_uiTabPrev = tab;
	}
	// The arrows reach the widgets when no text field has the keyboard — or when
	// a list hangs open, because then they belong to the list whatever else has
	// the focus. That second half is what makes a dropdown opened from beside a
	// text field navigable at all.
	if (uiTakesInput && (!m_world->widgets().isEditingText() ||
	                     m_world->widgets().hasOpenDropdown()))
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
	// ── Resizing a borderless window at its edges (plan F3) ──────────────────
	// First in this function, and consuming: a press on an edge is a press on
	// the window's frame, and nothing behind the frame may see it.
	//
	// This runs on macOS and nowhere else, without a single #ifdef. Windows,
	// X11 and Wayland take the RESIZE_* answer out of the hit test, size the
	// window themselves and swallow the press while doing it — so on those three
	// the button-down below simply never arrives, and the same code is dead by
	// construction rather than by a platform switch that could go stale.
	if (m_customFrame)
	{
		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
		{
			const HE::UIWindowHit hit = frameHitAt(static_cast<int>(event.button.x),
			                                       static_cast<int>(event.button.y));
			if (HE::uiWindowHitIsResize(hit))
			{
				SDL_Window* fw = window() ? window()->GetNativeWindow() : nullptr;
				if (fw)
				{
					HE::UIWindowRect r{};
					SDL_GetWindowPosition(fw, &r.x, &r.y);
					SDL_GetWindowSize(fw, &r.w, &r.h);
					// The DESKTOP position, not the one in the event: the whole
					// point of dragging the left edge is that the pointer leaves
					// the window, and a window-relative position stops moving
					// exactly when the window starts to.
					float gx = 0.0f, gy = 0.0f;
					SDL_GetGlobalMouseState(&gx, &gy);
					m_frameResize.begin(hit, r, static_cast<int>(gx), static_cast<int>(gy));
					// A double-click on the edge is a resize that started twice,
					// not a word being selected somewhere behind it.
					m_uiClickCount = 0;
					return true;
				}
			}
		}
		else if (m_frameResize.active())
		{
			if (event.type == SDL_EVENT_MOUSE_MOTION)
			{
				float gx = 0.0f, gy = 0.0f;
				SDL_GetGlobalMouseState(&gx, &gy);
				const HE::UIWindowRect r = m_frameResize.update(
					static_cast<int>(gx), static_cast<int>(gy),
					kFrameMinPoints.w, kFrameMinPoints.h);
				if (SDL_Window* fw = window() ? window()->GetNativeWindow() : nullptr)
				{
					// Position first, then size: growing to the left is the
					// window moving AND growing, and doing it the other way
					// round makes the far edge jitter by one frame's delta.
					SDL_SetWindowPosition(fw, r.x, r.y);
					SDL_SetWindowSize(fw, r.w, r.h);
				}
				return true;
			}
			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT)
			{
				m_frameResize.end();
				return true;
			}
		}
	}

	// A double- or triple-click on a text field selects a word or the line.
	// Only the COUNT is taken here; where the pointer was is worked out in
	// updateUIInput, which already does that arithmetic once.
	if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT &&
	    event.button.clicks >= 2)
		m_uiClickCount = event.button.clicks;

	// OS window focus → GameInstance OnWindowFocusChanged (while running), and —
	// when the project asks for it — the FocusLost pause reason.
	//
	// The event stays regardless of the switch: a project that already built a
	// graph on OnWindowFocusChanged keeps it, and gets to do more than freeze
	// (open its own menu, mute the mixer). The reason is its own channel, so a
	// script that resumes while the window is still in the background does not
	// accidentally lift this one, and this one does not lift the script's.
	if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
	{
		m_gameInstance.setWindowFocus(true);
		// Resumed unconditionally, not only when m_pauseOnFocusLoss is set:
		// resuming a reason nobody set is a no-op, and the alternative is a game
		// stuck paused forever if the switch is ever flipped off mid-flight.
		HE::api::time::resume(HE::api::time::PauseReason::FocusLost);
	}
	else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
	{
		m_gameInstance.setWindowFocus(false);
		if (m_pauseOnFocusLoss) HE::api::time::pause(HE::api::time::PauseReason::FocusLost);
	}

	// The desktop switched between light and dark while we were running. An
	// application set to "System" follows it now rather than at the next start —
	// that is the entire difference between the setting and a once-at-boot
	// reading. Outside every gate below: the desktop's theme has nothing to do
	// with whether a text field happens to be open, and it sat inside that gate
	// long enough to mean the setting only worked while someone was typing.
	if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED)
	{
		const SDL_SystemTheme sys = SDL_GetSystemTheme();
		if (sys == SDL_SYSTEM_THEME_LIGHT)
			m_widgets.setSystemThemeMode(HE::UIThemeMode::Light);
		else if (sys == SDL_SYSTEM_THEME_DARK)
			m_widgets.setSystemThemeMode(HE::UIThemeMode::Dark);
		return true;
	}

	// ── Files dragged in from the desktop (docs/he-apps-plan.md B7) ──────────
	// SDL reports one drag as four kinds of event: it entered the window, it
	// moved, here is a file, it is over. The files arrive one at a time BEFORE
	// the end, so they are collected and delivered when the gesture finishes —
	// dropping three files is one drop, not three.
	if (event.type == SDL_EVENT_DROP_BEGIN || event.type == SDL_EVENT_DROP_POSITION ||
	    event.type == SDL_EVENT_DROP_FILE  || event.type == SDL_EVENT_DROP_COMPLETE)
	{
		// The drop position comes in window points, like the mouse; the UI works
		// in drawable pixels. Same conversion updateUIInput does, and for the
		// same reason: on a Retina display the two differ by a factor of two.
		SDL_Window* win = window() ? window()->GetNativeWindow() : nullptr;
		int ww = 1, wh = 1, pw = 1, ph = 1;
		if (win) { SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowSizeInPixels(win, &pw, &ph); }
		const float dsx = ww > 0 ? static_cast<float>(pw) / ww : 1.0f;
		const float dsy = wh > 0 ? static_cast<float>(ph) / wh : 1.0f;
		if (event.type != SDL_EVENT_DROP_COMPLETE)
		{
			m_dropX = event.drop.x * dsx;
			m_dropY = event.drop.y * dsy;
		}
		switch (event.type)
		{
		case SDL_EVENT_DROP_BEGIN:
			m_dropPaths.clear();
			break;
		case SDL_EVENT_DROP_POSITION:
			m_widgets.dropHover(static_cast<float>(pw), static_cast<float>(ph),
			                    m_dropX, m_dropY, true);
			break;
		case SDL_EVENT_DROP_FILE:
			if (event.drop.data) m_dropPaths.emplace_back(event.drop.data);
			break;
		default:   // SDL_EVENT_DROP_COMPLETE
			// Dragging a file onto the window IS choosing it, exactly as picking
			// it in an open dialog is — same person, same intent, and the app was
			// handed the path either way. So the drop grants it, or the event
			// would arrive carrying a path the script is not allowed to read,
			// which is a file drop that cannot open a file. The grant belongs
			// HERE and not in the widget layer: turning a human gesture into a
			// permission is the host's job, and the dialogs do it from the same
			// side (HE::api::fs::grantPath).
			for (const std::string& p : m_dropPaths) HE::api::fs::grantPath(p);
			m_widgets.processDrop(static_cast<float>(pw), static_cast<float>(ph),
			                      m_dropX, m_dropY, m_dropPaths);
			m_dropPaths.clear();
			break;
		}
		return true;
	}

	// A focused in-game text field owns the keyboard: route text + edit keys to
	// the widget and swallow them so they don't drive the camera/gameplay.
	// Not under game-only routing: there the UI receives nothing, and a field
	// that still held focus from before the switch would otherwise keep eating
	// the movement keys with no way for the player to take them back.
	// A focused selectable LABEL owns a much smaller grammar than a field: it
	// can be selected in and copied out of, and nothing else. Its own block
	// rather than a widening of the one below, so a paragraph cannot swallow
	// Return, Backspace or a letter the application bound to something.
	if (m_world && HE::api::input::mode() != HE::api::input::Mode::GameOnly &&
	    m_world->widgets().isSelectingText() && event.type == SDL_EVENT_KEY_DOWN)
	{
		WidgetManager& wm = m_world->widgets();
		using TE = WidgetManager::TextEdit;
		const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
		const bool ctrl  = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
		const bool alt   = (event.key.mod & SDL_KMOD_ALT) != 0;
		switch (event.key.key)
		{
		case SDLK_LEFT:
			wm.editFocusedText((ctrl || alt) ? TE::WordLeft : TE::Left, shift);  return true;
		case SDLK_RIGHT:
			wm.editFocusedText((ctrl || alt) ? TE::WordRight : TE::Right, shift); return true;
		case SDLK_HOME: wm.editFocusedText(TE::Home, shift); return true;
		case SDLK_END:  wm.editFocusedText(TE::End,  shift); return true;
		case SDLK_UP:   wm.editFocusedText(TE::Up,   shift); return true;
		case SDLK_DOWN: wm.editFocusedText(TE::Down, shift); return true;
		case SDLK_A: if (ctrl) { wm.editFocusedText(TE::SelectAll, false); return true; } break;
		case SDLK_C:
			if (ctrl)
			{
				const std::string sel = wm.focusedSelection();
				if (!sel.empty()) SDL_SetClipboardText(sel.c_str());
				return true;
			}
			break;
		// Everything else belongs to whatever the application does with it. A
		// label is not a text box and must not act like one.
		default: break;
		}
	}
	if (m_world && HE::api::input::mode() != HE::api::input::Mode::GameOnly &&
	    m_world->widgets().isEditingText())
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
			// Only a multiline field answers these; in a single-line one they do
			// nothing and the key is still swallowed, because a focused text
			// field owns the arrow keys either way.
			case SDLK_UP:        wm.editFocusedText(TE::Up,    shift);  return true;
			case SDLK_DOWN:      wm.editFocusedText(TE::Down,  shift);  return true;
			case SDLK_RETURN:
			case SDLK_KP_ENTER:  wm.inputSubmit(); return true;
			case SDLK_A: if (ctrl) { wm.editFocusedText(TE::SelectAll, false); return true; } break;
			case SDLK_Z:
				// Ctrl+Z takes back, Ctrl+Shift+Z puts back — the chord every
				// platform agrees on. Ctrl+Y is the Windows spelling of redo and
				// is bound below for the people who reach for it.
				if (ctrl)
				{
					if (shift) wm.redoFocusedText(); else wm.undoFocusedText();
					return true;
				}
				break;
			case SDLK_Y: if (ctrl) { wm.redoFocusedText(); return true; } break;
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
			// ── A menu's shortcut, from inside a field somebody is typing in ──
			// After the switch above and not before it, and that is the whole
			// decision: an application that binds Ctrl+C to Edit → Copy must not
			// take the field's own copy away, because there is nothing left to
			// copy WITH — the clipboard lives out here and no graph can reach a
			// field's selection. So the field's chords win where they overlap,
			// and every other chord reaches the menu.
			//
			// Only WITH a modifier. A bare F5 while a form is being filled in is
			// a person typing, not a person reloading, and a menu that fired on
			// it would make text fields unusable in any application whose menus
			// carry plain-key shortcuts.
			if (!event.key.repeat && (ctrl || alt))
				if (menuShortcutFromKey(event.key)) return true;
			if (event.key.key != SDLK_ESCAPE) return true; // swallow other keys while typing
		}
	}

	if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
	{
		// ── A menu's shortcut, nothing being typed ───────────────────────────
		// FIRST in this block and ahead of everything the widgets are polled
		// for: a shortcut is the menu answered without opening it, and a bar
		// that only answers once its own strip has been clicked open is not a
		// shortcut at all. Bare keys are allowed here — F5 means F5 when nobody
		// is in a text field.
		if (menuShortcutFromKey(event.key)) return true;
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
		// Esc closes the topmost dialog, popup or menu FIRST, and only when
		// there is none does it mean what it always meant here. The order is the
		// whole point: a pause dialog that Escape cannot dismiss because Escape
		// is already spoken for is a dialog nobody can leave.
		if (event.key.key == SDLK_ESCAPE)
		{
			// Leaving a field comes first, and before closing anything: Escape
			// out of a search box means "stop typing", not "throw away the
			// dialog I was typing in". The focus stays where it is, so the very
			// next Tab carries on through the form.
			if (m_world && m_world->widgets().stopEditingText()) return true;
			if (m_world && m_world->widgets().closeTopLayer()) return true;
			// …and in an APPLICATION it means nothing else. There is no
			// FPS-style grab to give back — an app never took the cursor — so
			// toggling one here only hid the pointer of a program that has no
			// use for a hidden pointer. Not swallowed either: Escape is a key an
			// application's own logic is entitled to see.
			if (m_appMode) return false;
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

	// ── "Open with", delivered once ──────────────────────────────────────────
	// Through the SAME door a drop uses, and deliberately so: an application that
	// opens what it is handed wants ONE place to say so, and whether the document
	// arrived by double-click or by being dragged onto the window is the system's
	// business, not the author's. Position (-1, -1) is honest — nothing was
	// dropped anywhere, so no element takes it and it reaches the GameInstance.
	// (On macOS this list is empty: the system sends an open event, which SDL
	// turns into the drop path above.)
	if (m_launchFilesPending)
	{
		m_launchFilesPending = false;
		const std::vector<std::string>& files = launchArguments();
		if (!files.empty())
		{
			for (const std::string& p : files) HE::api::fs::grantPath(p);
			int pw = 1, ph = 1;
			if (SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr)
				SDL_GetWindowSizeInPixels(w, &pw, &ph);
			m_widgets.processDrop(static_cast<float>(pw), static_cast<float>(ph),
			                      -1.0f, -1.0f, files);
		}
	}

	// Tray clicks, collected inside SDL's pump and delivered here — outside it,
	// where firing a graph is what the frame is for. Same reason the launch
	// files above wait, and the ids go to the GameInstance because the tray
	// belongs to the application and not to any element.
	if (!g_trayClicks.empty())
	{
		std::vector<std::string> clicks;
		clicks.swap(g_trayClicks);
		if (const HorizonCode::InstanceId gi = m_gameInstance.runtime().gameInstance())
			for (const std::string& id : clicks)
				m_gameInstance.runtime().fireOnTrayItem(gi, 0, id);
	}

	// Answered HTTP requests, collected on the worker thread and delivered here
	// for the same reason the tray's clicks are: firing a graph belongs in the
	// frame. The graph gets the TICKET and asks the readers what came back.
	{
		int ticket = 0;
		while (HE::api::http::takeFinished(ticket))
			if (const HorizonCode::InstanceId gi = m_gameInstance.runtime().gameInstance())
				m_gameInstance.runtime().fireOnHttpResponse(gi, 0, ticket);
	}

	// Watched files. The scan itself happens HERE and not on a thread: the
	// sandbox root, the granted paths and the permission bits are process-wide
	// statics that no lock guards, so a watcher thread resolving a path would
	// race every file dialog. It costs a stat per watch per second, and an idle
	// application still reaches this line because the event-driven loop keeps a
	// 100 ms heartbeat.
	{
		HE::api::fs::pollWatches(deltaTime);
		std::string changed;
		while (HE::api::fs::takeChange(changed))
		{
			if (const HorizonCode::InstanceId gi = m_gameInstance.runtime().gameInstance())
				m_gameInstance.runtime().fireOnFileChanged(gi, 0, changed);
			// Nothing about a file arriving is an OS event for this window, so
			// the frame that would draw the reaction has to be asked for.
			requestRedraw();
		}
	}

	// Timers. Same shape as the two above, and one thing more: the loop is
	// event-driven and wakes on a 100 ms heartbeat, so a timer left to that
	// would be up to a tenth of a second late on EVERY tick. The next due time
	// is handed to the loop as a shorter wait — asked again every frame,
	// because askWakeWithinMs is a one-shot.
	{
		HE::api::timer::poll(deltaTime);
		int fired = 0;
		while (HE::api::timer::takeFired(fired))
		{
			if (const HorizonCode::InstanceId gi = m_gameInstance.runtime().gameInstance())
				m_gameInstance.runtime().fireOnTimer(gi, 0, fired);
			// A timer coming due is not an OS event for this window, so the
			// frame that draws the reaction has to be asked for.
			requestRedraw();
		}
		const double due = HE::api::timer::nextDueSeconds();
		if (due >= 0.0)
		{
			// Rounded DOWN and never below one: waking a millisecond early costs
			// one idle turn of the loop, waking late is the thing this exists to
			// prevent.
			const double ms = due * 1000.0;
			askWakeWithinMs(ms < 1.0 ? 1 : static_cast<int>(ms));
		}
	}

#ifdef __APPLE__
	// ── The same menu bar, in the system bar (plan A6) ───────────────────────
	// Whether there IS one is asked here and not at startup: it is SDL's answer,
	// and it only becomes true once SDL has registered the application. Asking
	// once too early would draw the strip for the rest of the run.
	if (g_menuDirty)
	{
		g_menuDirty = false;
		m_widgets.setMenuBarNative(HE::AppMacMenu::available());
		HE::AppMacMenu::set(m_widgets.menuBar());
	}
	// A menu click comes out of AppKit's own run loop, so it waits exactly like a
	// tray click and arrives at the same door as the drawn bar's: OnMenuItem at
	// the GameInstance, carrying the id.
	{
		std::string menuItemId;
		while (HE::AppMacMenu::take(menuItemId))
			if (const HorizonCode::InstanceId gi = m_gameInstance.runtime().gameInstance())
				m_gameInstance.runtime().fireOnMenuItem(gi, 0, menuItemId);
	}
#endif

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
		// Bounded so a long stall (a streaming hitch, a breakpoint) cannot spiral
		// into ever more catch-up steps, with the bound riding the time scale —
		// the rule lives in HE::advanceFixedSteps so the editor's preview cannot
		// pace differently from the game it is previewing.
		HE::advanceFixedSteps(m_physicsAccum, gameDt, PhysicsWorld::kFixedDt,
		                      HE::api::time::timeScale(),
		                      [&](float step){ m_physicsWorld->step(*m_world, step); });

		// ONE dispatch for both frontends: polling drains the queues, so a
		// second call would find nothing left and it would look like the
		// events simply never fire for that language.
		//
		// The world goes with it because a contact can name an entity that was
		// destroyed earlier in this very frame: entity.destroy from a script
		// touches no physics at all, so the body outlives the entity until the
		// next step reaps it. dispatch checks both ids against the registry and
		// drops the whole event — the guard is only as good as this argument.
		HE_PROFILE_SCOPE_N("CollisionDispatch");
		CollisionSystem::dispatch(*m_physicsWorld, *m_world,
		                          m_scriptContext.get(), m_scriptInstances,
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

	// An animation is the one thing that changes the picture without anyone
	// touching the machine, and an event-driven application sleeps until
	// something happens (see Application::setEventDriven). Asking for the next
	// frame while one runs is what makes it run at the display's speed instead
	// of the idle heartbeat's ten frames a second. The last frame of an
	// animation still asks, and the one after it does not — the value has
	// arrived by then, so the loop goes back to sleep on its own.
	if (m_world && m_world->widgets().isAnimating()) requestRedraw();

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
	// Entity classes: Tick, plus reaping the ones whose entity is gone — and
	// handing their bodies back as it notices them, rather than leaving them to
	// step()'s own sweep a frame later.
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
		for (auto e : m_world->registry().view<TransformComponent, CameraComponent>())
		{
			// Composed from the parent chain, not read out of tc.worldMatrix: that
			// matrix is only as fresh as the last propagateTransforms. The rig path
			// above runs one, the free-flight path does not, and a camera spawned
			// this frame still carries the identity — which would put LOD and the
			// precipitation volume at the world origin instead of at the player.
			camPos = HE::worldPositionOf(*m_world, e);
			break;
		}
		const bool gpuParticles = GlobalState::getInstance().getCustomConfigBool("GpuParticles", true) &&
		                          r && r->GetCapabilities().supportsGpuParticles;
		// Unbraced on purpose: the scope reaches to the end of this block, which is
		// now the world-systems tick alone — the renderer pushes moved out from
		// under it when the app-mode branch turned out to be unreachable there.
		HE_PROFILE_SCOPE_N("SceneSystemsTick");
		SceneSystems::tickWorld(*m_world, contentManager(), r, camPos, gameDt,
		                        m_physicsWorld.get(), gpuParticles);
		// Animation last, after every system that could have moved something this
		// frame — a state machine reads what gameplay just produced. Still ahead
		// of extraction, which consumes the bone matrices.
		SceneSystems::tickAnimation(*m_world, contentManager(), gameDt, &m_animatorHost);
	}

	// ── Renderer settings, in BOTH modes ─────────────────────────────────────
	// This used to sit INSIDE the `if (m_world && !m_appMode)` block above, which
	// made the `if (r && m_appMode)` branch below unreachable: a condition nested
	// inside its own negation. Nothing warned, nothing failed, and the effect was
	// that an application never turned any of this off — it kept the renderer's
	// defaults and drew a sky, clouds and a ground behind its interface, paying
	// for the whole chain over an empty world. Found by capturing a frame and
	// asking why the fix that was supposedly already there had changed nothing.
	{
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
		else if (r && m_world)
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

void GameApplication::OnWindowClosing(HE::WindowHandle handle)
{
	// Everything that hung in it goes with it. Not hidden — destroyed: the
	// window is about to stop existing, and a widget that still names it would
	// be drawn by nothing and clicked by nobody. destroyWidget also lets go of
	// any grab it held, which is what keeps the OTHER window clickable.
	const int gone = m_widgets.destroyWidgetsOfWindow(handle.id);
	if (gone > 0)
		HE_LOG_INFO(Widget, "Window %u closed — %d widget(s) destroyed with it",
		            handle.id, gone);
	// The pointer cannot still be in a window that is gone.
	if (m_widgets.pointerWindow() == handle.id)
		m_widgets.processPointer(0u, 1.0f, 1.0f, 0.0f, 0.0f, false, false);
}

void GameApplication::OnShutdown()
{
	// The tray outlives the window unless it is taken down deliberately, and an
	// icon left in the menu bar of a program that has exited is the worst thing
	// a tray can do.
	destroyTray();
	g_trayClicks.clear();
	// The HTTP worker before anything it could still answer into. A request in
	// flight is waited out (up to its own timeout), which is why that timeout is
	// five seconds and not ten.
	HE::api::http::shutdown();
	// The watches are this file's statics too, and one left standing would keep
	// stat-ing a path for an application that has stopped listening.
	HE::api::fs::clearWatches();
	// …and so are the timers, for exactly the same reason.
	HE::api::timer::cancelAll();
	// The database connections last: closing one flushes it, and a file left
	// half-written is the one of these three that costs somebody their data.
	HE::api::db::closeAll();
#ifdef __APPLE__
	// Ours out of the system bar again, SDL's own left standing. One application
	// per process, and this file's statics outlive the object that filled them.
	HE::AppMacMenu::set({});
	g_menuDirty = false;
#endif

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
	// The host borrows the physics world and end() deliberately does not clear
	// that (the apps set it before begin()), so it is dropped here — before the
	// world it points at goes away.
	m_entityHost.setPhysicsWorld(nullptr);
	m_physicsWorld.reset();

	// GameInstance OnShutdown fires last (symmetric to OnInit firing first).
	m_gameInstance.fireShutdown();

	// Clear the app-level UI while the runtime it registered on is still alive
	// (fires each widget's Destruct). It's a member destroyed before m_gameInstance
	// anyway, but the destructor doesn't fire Destruct — do it explicitly.
	m_widgets.clear();

	// Tear down ECS scripts before the world (their finalizers may touch entities).
	// The host services go FIRST, and while the context that published them still
	// exists: the copy ScriptContext hands to the Python backend is process-wide
	// and outlives this object, and every lambda in it captures `this`. AFTER
	// fireShutdown above, though — a GameInstance's OnShutdown graph still runs,
	// and it would be the one place where Create Object worked and
	// entity.spawnClass did not.
	if (m_scriptContext) m_scriptContext->setHostServices({});
	g_host = {};
	m_scriptContext.reset();
	m_scriptInstances.clear();

	// Stop + unload native game logic before the world is torn down.
	if (m_world && logicLoader().isLoaded())
		logicLoader().unload(*m_world);
	HE_LOG_INFO(Core, "%s", "GameApplication::OnShutdown");
}
