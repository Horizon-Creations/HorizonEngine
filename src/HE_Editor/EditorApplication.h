#pragma once
#include <Application/Application.h>
#include <Renderer/RendererFactory.h>
#include <Diagnostics/GlobalState.h>
#include "Types/Enums.h"
#include "ProjectManager.h"
#include "EditorUndo.h"
#include "EditorCamera.h"
#include <HorizonScene/HorizonScene.h>
#include <functional>
#include <future>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
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

	// Quick Settings panel — collapsed state of each section header
	bool QsRendererOpen = true;
	bool QsEditorOpen   = true;

	// Scene viewport: draw the world-space ground grid
	bool ShowGrid = true;

	// Content browser tree-panel width (-1 = auto on first frame)
	float CbTreeWidth = -1.0f;

	// Preferences (Edit > Preferences)
	float UiFontScale       = 1.0f;   // global editor font scale (style.FontScaleMain)
	float EditorCameraSpeed = 6.0f;   // editor fly-camera speed, world units/second

	// Post-process: bloom (pushed to the renderer each frame via SetBloomSettings)
	bool  BloomEnabled   = true;
	float BloomThreshold = 1.0f;
	float BloomIntensity = 0.6f;

	// Environment: day-night cycle (pushed via SetEnvironmentSettings)
	bool  DayNightCycle       = false;
	float TimeOfDay           = 0.5f;    // 0..1 (0.5 = noon)
	bool  DayNightAutoAdvance = false;   // advance TimeOfDay automatically
	float DayNightCycleSeconds = 120.0f; // real seconds for one full day
	// Sun & moon directional lights — user-adjustable colour and brightness.
	glm::vec3 SunColor      = glm::vec3(1.0f, 0.97f, 0.90f);
	float     SunIntensity  = 2.2f;
	glm::vec3 MoonColor     = glm::vec3(0.55f, 0.65f, 0.95f);
	float     MoonIntensity = 0.66f;
	// Procedural cloud amount (0 = clear sky … 1 = full overcast).
	float     CloudCoverage = 0.5f;

	EditorMode mode = EditorMode::View;
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

// All data the UI layer needs — assembled by EditorApplication each frame.
// No raw application pointer; UI code only sees this context.
struct AppContext
{
	// ImGui readiness flag (read-only for UI)
	bool           imguiReady = false;

	// Quit callback — called when the user requests exit from the UI
	std::function<void()> quit;

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

	// Editor scene-view camera (orbit/fly/focus). Owned by EditorApplication;
	// the UI drives it from viewport input and pushes it to the renderer.
	EditorCamera*      editorCamera = nullptr;

	// Entity selected in the outliner/viewport — drives the Details panel
	Entity& selectedEntity;

	// Play-in-editor: snapshot on play, restore on stop
	bool isPlaying = false;
	std::function<void(bool)> setPlayMode;

	// ── Scene file management ──────────────────────────────────────────────
	// currentScenePath is empty for an unsaved/new scene. sceneDirty reflects
	// unsaved changes since the last save/load (tracked via the undo revision).
	std::string& currentScenePath;
	bool         sceneDirty = false;
	// Raised by EditorApplication when an OS close request was vetoed (unsaved
	// scene); the UI turns it into a guarded Quit and clears it.
	bool&        exitRequested;
	std::function<void(const std::string&)> saveSceneToPath; // write world → .hescene (JSON)
	std::function<void(const std::string&)> openScene;       // load .hescene, replacing the world
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

	// Scene world — created once, alive for the entire editor session
	std::unique_ptr<HorizonWorld> m_editorWorld;

	// Outliner/inspector selection
	Entity m_selectedEntity = entt::null;

	// Editor scene-view camera
	EditorCamera m_editorCamera;

	// Play-in-editor
	bool m_isPlaying = false;
	void setPlayMode(bool play);

	// Scene file management. m_currentScenePath is the .hescene the editor
	// world was last saved to / loaded from (empty = new/unsaved). m_savedRevision
	// is the undo revision at that point; the scene is dirty when it differs.
	std::string m_currentScenePath;
	uint64_t    m_savedRevision = 0;
	void saveSceneToPath(const std::string& path);
	void openScene(const std::string& path);
	void newScene();

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

	void* m_d3d12SrvHeap      = nullptr;
	void* m_d3d12SrvAllocator = nullptr;

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

