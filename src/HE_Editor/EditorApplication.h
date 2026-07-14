#pragma once
#include <Application/Application.h>
#include <Renderer/RendererFactory.h>
#include <Diagnostics/GlobalState.h>
#include "Types/Enums.h"
#include "ProjectManager.h"
#include "EditorUndo.h"
#include "EditorCamera.h"
#include <HorizonScene/HorizonScene.h>
#include <Scripting/ScriptEngine.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/AudioSystem.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/CollisionSystem.h>
#include <HorizonScene/UIInputSystem.h>
#include <HorizonScene/GameInstanceHost.h>
#include <HorizonScene/PlayerHost.h>
#include <HorizonScene/HcCodegen.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <cstdint>
#endif

// Stable userdata struct for SDL async dialog callbacks.
// Lives as a member of EditorApplication — never on the stack.
struct SDLDialogBridge
{
	std::string* pendingDirResult  = nullptr;
	bool*        pendingDirReady   = nullptr;
	std::string* pendingFileResult = nullptr;
	bool*        pendingFileReady  = nullptr;
};

enum class EditorMode
{
	View,
	Landscape,
};

struct EditorConfig
{
	bool KeepCPUAssets = false;   
	bool KeepCPUAssetsInfoAcknoleged = false;
	int  ContentBrowserRefreshRate = 60;

	// Content browser tree-panel width (-1 = auto on first frame)
	float CbTreeWidth = -1.0f;

	// Preferences (Edit > Preferences)
	float UiFontScale       = 1.0f;   // global editor font scale (style.FontScaleMain)
	float EditorCameraSpeed = 6.0f;   // editor fly-camera speed, world units/second
	float MaxFps            = 0.0f;   // VSync-off frame cap (0 = unlimited); paces mouse-look

	// Post-process: bloom (pushed to the renderer each frame via SetBloomSettings)
	bool  BloomEnabled   = true;
	float BloomThreshold = 1.0f;
	float BloomIntensity = 0.6f;

	// Post-process: SSAO (pushed to the renderer each frame via SetSSAOSettings)
	bool  SSAOEnabled   = true;
	float SSAORadius    = 0.5f;   // hemisphere sampling radius, view-space units
	float SSAOIntensity = 1.0f;   // 0 = off … 1 = full ambient occlusion
	int   SSAOMethod    = 0;      // AO method: 0 = SSAO, 1 = HBAO, 2 = GTAO (planned)

	// GPU weather particles: simulate rain/snow on the GPU (transform feedback / compute)
	// instead of the CPU pool. Default on; the backend's supportsGpuParticles gates it
	// (GL + Metal = yes, so it's the path used unless the user turns it off).
	bool  GpuParticles  = true;

	// NOTE: environment / sky settings (day-night, sun, moon, clouds, fog, night
	// sky, wind) are scene data now — they live on the World root entity as an
	// EnvironmentComponent, are edited in its Details panel and persist with the
	// scene. They are no longer editor preferences.

	// Quick Settings = the engine settings the user pinned in Preferences. Stored
	// as a comma-separated list of stable setting keys (see DrawEngineSettings).
	std::string QuickSettingsFavorites = "backend,vsync,grid,bloom,ssao";

	EditorMode mode = EditorMode::View;

	// New-landscape creation-form parameters. Transient (not serialised) — shared
	// here so the renderer can draw a 3D grid preview of the terrain-to-be while
	// the Landscape creation form is open. Mirrors TerrainComponent's noise fields.
	struct NewTerrainParams
	{
		float sizeX       = 100.0f;
		float sizeZ       = 100.0f;
		int   resolution  = 128;
		float heightScale = 20.0f;
		int   seed        = 0;     // 0 = flat
		int   octaves     = 4;
		float frequency   = 1.0f;
		float lacunarity  = 2.0f;
		float gain        = 0.5f;
	};
	NewTerrainParams newTerrain;

	std::string modeString() const
	{
		switch (mode)
		{
		case EditorMode::View:      return "View";
		case EditorMode::Landscape: return "Landscape";
		default:                   return "Unknown";
		}
	}
};

// One captured warning/error from a play session (post-PIE report).
struct PlayLogEntry
{
	HE::LogLevel level;      // Warning or Error/Critical
	std::string  message;
	float        time = 0.0f; // play-clock seconds it was FIRST logged
	int          count = 1;   // consecutive identical repeats collapsed into this entry
};

// All data the UI layer needs — assembled by EditorApplication each frame.
// No raw application pointer; UI code only sees this context.
struct AppContext
{
	// ImGui readiness flag (read-only for UI)
	bool           imguiReady = false;

	// Quit callback — called when the user requests exit from the UI
	std::function<void()> quit;

	// Toggle a profiler benchmark capture (same path as the F9 hotkey: disables
	// vsync on start so frame times reflect true cost, dumps + restores on stop).
	std::function<void()> toggleProfilerCapture;

	// Apply a vsync change THROUGH Application::setVSync so the app's vsync state
	// (used by the profiler capture to save/restore) stays in sync with the editor's
	// preference — otherwise a capture restores a stale vsync after F9.
	std::function<void(bool)> setVSync;
	// Apply a VSync-off frame cap through Application::setMaxFps (0 = unlimited).
	std::function<void(float)> setMaxFps;

	// Config (mutable — UI writes changes directly)
	EditorConfig&  editorConfig;
	bool&          vsync;
	std::string&   backendName;

	// Renderer backend identity (read-only for UI)
	HE::RendererBackend backend;

	// Global state + project manager (UI calls addKnownProject, writeConfig, etc.)
	GlobalState*       globalState  = nullptr;
	ProjectManager*    projectManager = nullptr;
	IRenderer*         renderer     = nullptr;
	HE::Window*        window       = nullptr;
	HorizonWorld*      world        = nullptr;
	ContentManager*    contentManager = nullptr;

	// The project's app-wide GameInstance graph (edited in the Game Instance
	// window). commitGameInstance re-registers it with the app runtime + saves it.
	HorizonCode::Graph*   gameInstanceGraph = nullptr;
	std::function<void()> commitGameInstance;
	ScriptEngine*      propScriptEngine = nullptr; // read-only, for inspector property reading

	// Editor scene-view camera (orbit/fly/focus). Owned by EditorApplication;
	// the UI drives it from viewport input and pushes it to the renderer.
	EditorCamera*      editorCamera = nullptr;

	// Entity selected in the outliner/viewport — drives the Details panel
	Entity& selectedEntity;

	// Play-in-editor: snapshot on play, restore on stop
	bool isPlaying = false;
	// Post-PIE report: the warnings/errors captured during the last play session
	// (guarded by playLogMutex — workers may still append while the UI reads),
	// and the open-flag for the report window (set when play stops with entries).
	std::vector<PlayLogEntry>* playLog = nullptr;
	std::mutex*                playLogMutex = nullptr;
	bool*                      playReportOpen = nullptr;
	std::function<void(bool)> setPlayMode;
	// PIE UI pointer feed: viewport-relative mouse in render-target pixels +
	// viewport size + LMB state; valid=false while outside/captured.
	std::function<void(float mx, float my, float vpW, float vpH,
	                   bool down, bool valid)> reportPlayUIPointer;

	// ── Scene file management ──────────────────────────────────────────────
	// currentScenePath is empty for an unsaved/new scene. sceneDirty reflects
	// unsaved changes since the last save/load (tracked via the undo revision).
	std::string& currentScenePath;
	bool         sceneDirty = false;
	// Raised by EditorApplication when an OS close request was vetoed (unsaved
	// scene); the UI turns it into a guarded Quit and clears it.
	bool&        exitRequested;
	std::function<void(const std::string&)> saveSceneToPath; // write world → .hescene (JSON)
	std::function<void(const std::string&)> openScene;          // load .hescene, replacing the world
	std::function<void(const std::string&)> openSceneAdditive; // merge .hescene into the existing world
	std::function<void()>                    newScene;        // clear to an empty scene

	// Undo/redo. UI calls undoSys capture/stash/commit around mutations;
	// undo()/redo() also reset the selection (entity handles are remapped).
	EditorUndo* undoSys = nullptr;
	std::function<void()> undo;
	std::function<void()> redo;

	// Editor/hub flags (mutable)
	bool& projectLoaded;
	bool& contentRefreshPending;
	bool& contentRefreshDone;

	// Startup toolchain probe (cmake + C++ compiler), run once on a background
	// thread so a slow/missing toolchain never blocks editor init. Null until
	// the probe finishes. The "Toolchain Missing" dialog (EditorUI) opens when
	// it's non-null and something's missing, unless suppressed in preferences.
	const HE::hccg::ToolchainProbe* toolchainProbe = nullptr;
	std::function<void()> recheckToolchain; // re-runs the probe (after the user installs something)

	// Auto-install of the missing toolchain (see HcCodegen::installToolchain). The
	// "Toolchain Missing" dialog calls startToolchainInstall(needCmake, needCompiler);
	// the install runs on a background thread and streams output, which the dialog
	// shows live by polling toolchainInstallLog() each frame.
	std::function<void(bool needCmake, bool needCompiler)> startToolchainInstall;
	std::function<std::string()> toolchainInstallLog; // thread-safe snapshot of the streamed output
	bool toolchainInstalling  = false; // an install is currently running
	bool toolchainInstallDone = false; // the last install finished (success or failure)
	bool toolchainInstallOk   = false; // finished, launched an installer, and it exited 0

	// Performance counters (mutable, updated each frame by UI)
	float* frametimeHistory = nullptr;
	int    fpsHistorySize   = 0;
	int&   fpsHistoryOffset;
	float& fpsAccum;
	int&   fpsAccumCount;
	float& smoothFps;

#ifdef HE_IMGUI_ENABLED
	// ── Editor tabs ──────────────────────────────────────────────────────
	struct EditorTab
	{
		std::string label;
		std::string assetPath; // empty for built-in tabs like Viewport
		bool        closable = true;
		bool        open     = true;
	};
	std::vector<EditorTab>& tabs;
	int&                    activeTab;

	// Fonts
	ImFont* fontBody       = nullptr;
	ImFont* fontSubheading = nullptr;
	ImFont* fontHeading    = nullptr;
	ImFont* codeFont       = nullptr;  // monospace, for the script code editor

	// Logo
	ImTextureID logoTexture = 0;
	int         logoW       = 0;
	int         logoH       = 0;

	// Content browser icon textures (white images, tinted at render time)
	struct CbIcons
	{
		ImTextureID folder   = 0;
		ImTextureID material = 0;
		ImTextureID model2d  = 0;
		ImTextureID model3d  = 0;
		ImTextureID script   = 0;
		ImTextureID sound    = 0;
		ImTextureID texture  = 0;
		ImTextureID scene    = 0;
	} cbIcons;

	// Toolbar icon textures
	struct ToolbarIcons
	{
		ImTextureID play = 0;
		ImTextureID stop = 0;
		ImTextureID undo = 0;
		ImTextureID redo = 0;
	} toolbarIcons;

	// Content browser splitter
	float& cbTreeWidth;

	// Project hub transient state
	int&   hubSelectedPreset;
	int&   hubSelectedLang;   // scripting-language pick (ProjectScriptLanguage order)
	char*  hubProjectName = nullptr;  // points into EditorApplication's char array
	int    hubProjectNameSize = 0;
	char*  hubProjectDir  = nullptr;
	int    hubProjectDirSize  = 0;
	std::string& hubCreateError;
	std::string& hubOpenError;
	int&   hubRemoveIndex;
	bool&  hubRemoveRequested;
	std::string& pendingDirResult;
	bool&  pendingDirReady;
	std::string& pendingFileResult;
	bool&  pendingFileReady;
	SDLDialogBridge* dialogBridge = nullptr;
#endif
};

class EditorApplication : public HE::Application
{
public:
	explicit EditorApplication(std::string startupPath)
		: HE::Application(std::move(startupPath)) {}
	~EditorApplication() override; // defined in .cpp where ScriptEngine is complete

public:
	// Logger sink target: appends a play-session warning/error (any thread).
	void appendPlayLog(HE::LogLevel level, const char* message);

protected:
	HE::ApplicationConfig GetConfig()          const override;
	void OnInit()                                    override;
	void OnRender(float dt)                          override;
	void OnShutdown()                                override;
	bool OnEvent(const SDL_Event& event)             override;

	std::unique_ptr<IRenderer> CreateRenderer()      override;

private:
	bool m_imguiReady        = false;
	bool m_projectLoaded     = false;
	bool m_contentRefreshPending = false;
	bool m_contentRefreshDone    = false;
	HE::RendererBackend m_backend;
	std::string m_backend_name;
	ProjectManager m_projectManager;
	EditorConfig m_editorConfig;

	// App-wide HorizonCode host: owns the runtime the editor world runs on and
	// the GameInstance script. m_gameInstanceGraph is the authored source (edited
	// in the Game Instance window, saved to the project); the host holds the live
	// running copy. Loaded per project; OnInit/OnShutdown fire around play mode.
	GameInstanceHost   m_gameInstance;
	// Player controller/character instances + input pump — PIE only (begun on
	// entering play mode, ended on leaving). Shares m_gameInstance's runtime.
	PlayerHost         m_playerHost;
	HorizonCode::Graph m_gameInstanceGraph;
	void loadGameInstanceGraph();  // read the project's GameInstance.hcode → host
	void saveGameInstanceGraph();  // write m_gameInstanceGraph → project file
	std::string gameInstancePath(); // <projectDir>/GameInstance.hcode

	// Per-project open-tab persistence (stored in the global config keyed by
	// project path). restoreOpenTabs runs on project load; saveOpenTabs runs when
	// the tab set changes (via the signature check each frame) + on shutdown.
	void        saveOpenTabs();
	void        restoreOpenTabs();
	std::string m_lastTabSig; // change detection for the auto-save

	// Scene world — created once, alive for the entire editor session
	std::unique_ptr<HorizonWorld> m_editorWorld;

	// Lightweight ScriptEngine used only for reading M.properties in the inspector.
	// Never creates instances; only loadScript + getScriptProperties.
	std::unique_ptr<ScriptEngine> m_propScriptEngine;

	// Physics simulation — active only while in play mode.
	std::unique_ptr<PhysicsWorld> m_physicsWorld;
	float m_physicsAccum = 0.0f;
	static constexpr float kPhysicsFixedDt = 1.0f / 60.0f;

	// Audio engine — initialised at startup, active always (spatial update only in play mode).
	AudioEngine m_audioEngine;

	// Script execution context (play mode only; null outside play mode).
	std::unique_ptr<ScriptContext> m_scriptContext;
	// Maps raw entity handle → Lua instance id (parallel lifecycle to m_scriptContext).
	std::unordered_map<uint32_t, ScriptEngine::InstanceId> m_scriptInstances;

	// Outliner/inspector selection
	Entity m_selectedEntity = entt::null;

	// Editor scene-view camera
	EditorCamera m_editorCamera;

	// In-game UI pointer input during PIE. The viewport panel reports the
	// mouse in render-target pixels each frame (reportPlayUIPointer); the
	// update loop hit-tests + dispatches onClick/onHover* to scripts.
	UIInputSystem::InputState m_uiInputState;
	float m_uiPointerX = 0.0f, m_uiPointerY = 0.0f;
	float m_uiViewportW = 0.0f, m_uiViewportH = 0.0f;
	bool  m_uiPointerDown = false, m_uiPointerValid = false;
	bool  m_widgetTextInputActive = false; // SDL text input toggled for a focused widget field

	// Play-in-editor
	bool m_isPlaying = false;
	// Play-session log capture (see PlayLogEntry / the post-PIE report window).
	std::vector<PlayLogEntry> m_playLog;
	std::mutex                m_playLogMutex;
	bool                      m_playReportOpen = false;
	void setPlayMode(bool play);

	// Mouse-captured free-fly camera while playing in the editor — mirrors the
	// packaged game so PIE is navigable. Captured on play-enter, released on exit,
	// toggled with Esc. Drives the scene's main camera from the mouse + WASD.
	bool m_playMouseCaptured = false;
	void setPlayMouseCaptured(bool captured);
	void updatePlayCameraController(float dt);

	// Scene file management. m_currentScenePath is the .hescene the editor
	// world was last saved to / loaded from (empty = new/unsaved). m_savedRevision
	// is the undo revision at that point; the scene is dirty when it differs.
	std::string m_currentScenePath;
	uint64_t    m_savedRevision = 0;
	void saveSceneToPath(const std::string& path);
	void openScene(const std::string& path);
	void openSceneAdditive(const std::string& path);
	void newScene();
	// Build the node-graph material pipelines referenced by the current world ahead
	// of the first draw (no first-frame cross-compile hitch). Materials are resident
	// by call time (preloadAssetRefs ran); a no-op for backends that build eagerly.
	void warmupWorldMaterials();
	// Auto-advances (dt > 0) and pushes the World root's EnvironmentComponent to
	// the renderer via SetEnvironmentSettings. The environment is scene data now,
	// not an editor preference.
	void pushEnvironment(float dt);

	// Set by OnEvent when an OS-level close (X / Cmd+Q) is vetoed because the
	// scene has unsaved changes; the UI reads it to raise the save-prompt with a
	// quit intent, then clears it.
	bool m_exitRequested = false;

	// Undo/redo
	EditorUndo m_undo;

	bool  m_vsync     = true;

	// Asynchroner Content-Refresh-Timer
	float m_contentRefreshTimer = 0.0f;
	std::future<void> m_contentRefreshFuture;

	// Startup toolchain probe (cmake + real C++ compiler check, see HcCodegen).
	// Runs once on a detached-but-joined-at-shutdown worker; the UI polls
	// m_toolchainChecked and reads m_toolchainProbe once it flips true.
	std::thread              m_toolchainThread;
	std::atomic<bool>        m_toolchainChecked{false};
	HE::hccg::ToolchainProbe m_toolchainProbe;
	void startToolchainProbe(); // (re)launches m_toolchainThread

	// Auto-install worker (see startToolchainInstall / HcCodegen::installToolchain).
	// Streams installer output into m_installLog under m_installLogMutex; the UI polls.
	std::thread       m_installThread;
	std::atomic<bool> m_installRunning{false};
	std::atomic<bool> m_installFinished{false};
	std::atomic<int>  m_installExit{0};
	std::atomic<bool> m_installAttempted{false};
	std::mutex        m_installLogMutex;
	std::string       m_installLog;
	void startToolchainInstall(bool needCmake, bool needCompiler);

	// Hot-reload: disk-asset change detection
	float m_hotReloadTimer = 0.0f;

	static constexpr int k_fpsHistorySize = 128;
	float m_frametimeHistory[k_fpsHistorySize] = {};
	int   m_fpsHistoryOffset = 0;
	float m_fpsAccum         = 0.0f;
	int   m_fpsAccumCount    = 0;
	float m_smoothFps        = 0.0f;

	bool  m_isDraggingWindow = false;
	int   m_dragOffsetX      = 0;
	int   m_dragOffsetY      = 0;

	// ── Headless frame dump (HE_DUMP_PATH / HE_DUMP_QUIT) ──────────────────
	// Debug/validation hook: when HE_DUMP_PATH is set, render the scene to an
	// offscreen target and write it as a .bmp during OnInit, then (unless
	// HE_DUMP_QUIT=0) quit. Lets the renderer be validated without OS
	// screen-recording permission. No effect when the env var is unset.
	std::string   m_dumpPath;
	bool          m_dumpQuit = true;
	bool          m_dumpDone = false;
	void dumpFrameHeadless();

#ifdef HE_IMGUI_ENABLED
	ImFont* m_fontBody       = nullptr;
	ImFont* m_fontSubheading = nullptr;
	ImFont* m_fontHeading    = nullptr;
	ImFont* m_fontMono       = nullptr;  // monospace (ProggyClean) for the code editor

	void*    m_d3d12SrvHeap      = nullptr;
	void*    m_d3d12SrvAllocator = nullptr;
	// Viewport offscreen RT SRV slot in the ImGui heap (D3D12 only).
	uint64_t m_d3d12ViewportSrvCpuPtr   = 0;
	uint64_t m_d3d12ViewportSrvGpuPtr   = 0;
	bool     m_d3d12ViewportSrvAllocated = false;
	// Viewport ImGui descriptor set (Vulkan only); void* to avoid Vulkan headers here.
	void*    m_vkViewportDescSet         = nullptr;

	ImTextureID m_logoTexture = 0;
	int         m_logoW       = 0;
	int         m_logoH       = 0;

	// Content browser icons
	ImTextureID m_iconFolder   = 0;
	ImTextureID m_iconMaterial = 0;
	ImTextureID m_iconModel2d  = 0;
	ImTextureID m_iconModel3d  = 0;
	ImTextureID m_iconScript   = 0;
	ImTextureID m_iconSound    = 0;
	ImTextureID m_iconTexture  = 0;
	ImTextureID m_iconScene    = 0;

	// Toolbar icons
	ImTextureID m_iconPlay     = 0;
	ImTextureID m_iconStop     = 0;
	ImTextureID m_iconUndo     = 0;
	ImTextureID m_iconRedo     = 0;

	// Project Hub transient state
	int         m_hubSelectedPreset  = 0;
	int         m_hubSelectedLang    = 0;  // index into the wizard's ProjectScriptLanguage order
	char        m_hubProjectName[256]= {};
	char        m_hubProjectDir[512] = {};
	std::string m_hubCreateError;
	std::string m_hubOpenError;
	int         m_hubRemoveIndex     = -1;
	bool        m_hubRemoveRequested = false;
	std::string m_pendingDirResult;
	bool        m_pendingDirReady    = false;
	std::string m_pendingFileResult;
	bool        m_pendingFileReady   = false;

	SDLDialogBridge m_sdlDialogBridge;

	std::vector<AppContext::EditorTab> m_tabs;
	int                                m_activeTab = 0;
#endif

	AppContext makeContext();
};

