#pragma once
#include <Application/Application.h>
#include <Renderer/RendererFactory.h>
#include <Diagnostics/GlobalState.h>
#include "Types/Enums.h"
#include "ProjectManager.h"
#include "EditorUndo.h"
#include "EditorCamera.h"
#include "CollabController.h"
#include "CollabDocSync.h"   // DocMirror for the two documents the editor owns
#include "CollabUndo.h"
#include "NotificationStore.h"
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
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/HcCodegen.h>
#include <SourceControl/GitProbe.h>
#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/SftpProbe.h>
#endif
#include <Net/RouterProbe.h>
#include "GitController.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
	bool KeepCPUAssetsInfoAcknowledged = false;
	int  ContentBrowserRefreshRate = 60;

	// Content browser tree-panel width (-1 = auto on first frame)
	float CbTreeWidth = -1.0f;

	// Collaboration: announce a hosted session on the local network, and listen
	// for other people's. On by default — it is the only route that works when
	// the router will not forward a port, and it carries no secret (the join
	// code is never announced). Off is for networks you would rather not be
	// visible on at all.
	bool CollabLanDiscovery = true;

	// Collaboration: also send the BIG assets — meshes, textures, audio, fonts —
	// over the session instead of leaving them to source control. Off by default,
	// and deliberately so: those are the files that are measured in hundreds of
	// megabytes, and somebody on a metered or slow connection has to be able to
	// say no.
	//
	// It is the HOST's decision for the session, because it is the host that
	// decides what the session carries; a guest whose own setting disagrees is
	// refused at the join and asked first (see JoinRejectReason). It cannot be
	// changed while a session is running — half a session with one rule and half
	// with another is a set of peers that quietly hold different files.
	bool CollabSyncLargeAssets = false;

	// The biggest single asset this editor will send or accept over a session,
	// in megabytes. It was a hard 64 shared with the session snapshot, which is
	// the wrong number to be fixed: with large-asset sync on, a 90 MB cooked
	// mesh is an ordinary file and simply never arrived — refused, not
	// truncated, and only a log line said so.
	//
	// Raising it is not free, which is why the setting explains itself rather
	// than just offering a number: a transfer is held WHOLE in memory on both
	// ends and queued a second time on the sender, and while it is going out it
	// is ahead of everything else the session wants to say. The cap is also the
	// only thing bounding what one peer can make another allocate.
	int CollabMaxAssetMB = 64;

	// Preferences (Edit > Preferences)
	float UiFontScale       = 1.0f;   // global editor font scale (style.FontScaleMain)
	float EditorCameraSpeed = 6.0f;   // editor fly-camera speed, world units/second
	float MaxFps            = 0.0f;   // VSync-off frame cap (0 = unlimited); paces mouse-look
	// Pointer-device grammar for the preview panes (see EditorInput.h):
	// 0 = Auto (detect trackpad), 1 = Mouse, 2 = Trackpad.
	int   PointerInput      = 0;
	// Gamepad deadzones, mirrored into Input each frame (Input owns the live
	// values; these are just what survives a restart). Sticks radial, trigger
	// scalar — see Input.h for why the shapes differ.
	float GamepadStickDeadzone   = 0.15f;
	float GamepadTriggerDeadzone = 0.05f;

	// Post-process: bloom (pushed to the renderer each frame via SetBloomSettings)
	bool  BloomEnabled   = true;
	float BloomThreshold = 1.0f;
	float BloomIntensity = 0.6f;

	// Post-process: SSAO (pushed to the renderer each frame via SetSSAOSettings)
	bool  SSAOEnabled   = true;
	float SSAORadius    = 0.5f;   // hemisphere sampling radius, view-space units
	float SSAOIntensity = 1.0f;   // 0 = off … 1 = full ambient occlusion
	int   SSAOMethod    = 0;      // AO method: 0 = SSAO, 1 = HBAO, 2 = GTAO (planned)

	// Anti-aliasing (pushed each frame via SetAntiAliasingSettings, see
	// docs/anti-aliasing-plan.md). `AntiAliasing` holds an HE AAMethod int —
	// 0 Off, 1 FXAA, 2 SMAA, 3 TAA, 4 MetalFX — and defaults to FXAA because
	// that is what the engine did before the setting existed. Modes the backend
	// cannot do are greyed out in Preferences and fall back at push time.
	int   AntiAliasing        = 1;
	float AASharpness         = 0.35f; // temporal modes only
	float RenderScale         = 1.0f;  // < 1 upscales, > 1 supersamples
	bool  SpecularAA          = true;  // roughness regularization (shading aliasing)
	float SpecularAAStrength  = 1.0f;

	// GPU weather particles: simulate rain/snow on the GPU (transform feedback / compute)
	// instead of the CPU pool. Default on; the backend's supportsGpuParticles gates it
	// (GL + Metal = yes, so it's the path used unless the user turns it off).
	bool  GpuParticles  = true;

	// Render path (pushed to the renderer each frame via SetRenderPath): 0 =
	// Forward (default), 1 = Deferred (G-buffer + fullscreen lighting resolve,
	// Metal + OpenGL). The backend's supportsDeferredRendering gates it.
	int   RenderPath = 0;

	// Screen-space reflections (pushed each frame via SetSSRSettings). v1 only
	// effective on Metal in the deferred render path; supportsScreenSpaceReflections
	// gates the toggle. Off by default (like GI).
	bool  SSREnabled      = false;
	float SSRIntensity    = 1.0f;
	float SSRMaxRoughness = 0.6f;
	int   SSRQuality      = 1;   // 0 Low (16 steps, raw) / 1 Med (32+blur) / 2 High (64+glossy)

	// Global Illumination: ray-traced DDGI (pushed to the renderer each frame via
	// SetGISettings). Metal-only; the backend's supportsGlobalIllumination gates
	// it (needs a ray-tracing-capable GPU + macOS 12+). Off by default — strictly
	// opt-in. When on and supported, COMPLETELY REPLACES CSM shadows and AO/ambient.
	bool  GlobalIlluminationEnabled = false;
	float GIIndirectIntensity       = 1.0f;
	float GILightRadius             = 0.5f;   // degrees — sun angular radius (shadow penumbra softness)

	// Ray-traced GI reflections (pushed each frame via SetGIReflectionSettings).
	// Real scene rays against the GI acceleration structure instead of the sky
	// cubemap; supportsGIReflections gates the toggle (Metal tile deferred +
	// HW RT, or an OpenGL 4.3 context on Windows/Linux).
	bool  GIReflectionsEnabled = false;
	float GIReflIntensity      = 1.0f;
	float GIReflMaxRoughness   = 0.6f;
	int   GIReflQuality        = 1;   // 0 raw / 1 blur / 2 glossy+temporal (SSR-style tiers)
	int   GIReflBounces        = 1;   // 1-4 mirror bounces (Metal only — the GL kernel traces one segment)
	// Post-trace blur. TEST DEFAULT OFF: the blur is meant to shrink as the tier
	// rises, and the fastest way to tell whether it is what a soft reflection is
	// actually made of is to look with it gone. Flip the checkbox in Preferences
	// (or this default) to bring it back.
	bool  GIReflBlur           = false;

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
	// The app's device-input state. Settings uses it for the gamepad section:
	// connected-pad readout, live axis values, and the deadzone knobs (which
	// live on Input so the filter and its setting cannot drift apart).
	Input*             appInput     = nullptr;
	HE::Window*        window       = nullptr;
	HorizonWorld*      world        = nullptr;
	ContentManager*    contentManager = nullptr;

	// The editor's own audio device — alive for the whole session, not just play
	// mode, so a panel can audition a clip without entering PIE. Panels must stop
	// the handles they start (a tab that closes while looping has no UI left to
	// kill it); everything an editor preview needs beyond play/stop lives on the
	// transport block of AudioEngine.
	AudioEngine*       audioEngine  = nullptr;

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
	// Transport pause. Meaningful only while playing, and always false outside it.
	bool isPaused  = false;
	// Post-PIE report: the warnings/errors captured during the last play session
	// (guarded by playLogMutex — workers may still append while the UI reads),
	// and the open-flag for the report window (set when play stops with entries).
	std::vector<PlayLogEntry>* playLog = nullptr;
	std::mutex*                playLogMutex = nullptr;
	bool*                      playReportOpen = nullptr;
	std::function<void(bool)> setPlayMode;
	// Freeze / thaw the world tick, and let exactly one frame through. stepFrame
	// pauses first when the scene is still running, so "step" is one gesture from
	// any transport state.
	std::function<void(bool)> setPaused;
	std::function<void()>     stepFrame;
	// PIE UI pointer feed: viewport-relative mouse in render-target pixels +
	// viewport size + LMB state + this frame's wheel; valid=false while
	// outside/captured. The wheel rides along because a scroll box under the
	// cursor has to get it before the editor camera's dolly does.
	std::function<void(float mx, float my, float vpW, float vpH,
	                   bool down, bool valid, float wheel)> reportPlayUIPointer;

	// ── Scene file management ──────────────────────────────────────────────
	// currentScenePath is empty for an unsaved/new scene. sceneDirty reflects
	// unsaved changes since the last save/load (tracked via the undo revision).
	std::string& currentScenePath;
	bool         sceneDirty = false;
	// Raised by EditorApplication when an OS close request was vetoed — either the
	// scene or an asset panel has unsaved edits (including a panel whose tab was
	// already closed); the UI turns it into a guarded Quit and clears it.
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

	// ── Entity editing gestures ──────────────────────────────────────────────
	// Duplicate / cut / copy / paste / delete of the SELECTED entity, one
	// implementation behind the Edit menu, the Outliner's context menu and the
	// keyboard. Each one snapshots for undo itself, so callers just call.
	//
	// The clipboard behind them is a prefab BLOB, not an entity handle: the
	// entity it came from is gone after a cut, and the whole world is replaced
	// by an undo, so a handle would name something else by the time it is
	// pasted. It is also why the components are not copied by hand — the
	// serializer already knows all of them, and the next component added to the
	// engine would otherwise be the one nobody remembers to copy.
	std::function<void()> duplicateEntity;
	std::function<void()> copyEntity;
	std::function<void()> cutEntity;
	std::function<void()> pasteEntity;
	std::function<void()> deleteEntity;
	// Is there anything to paste? Drives the Paste item's enabled state.
	bool entityClipboardFull = false;

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

	// Startup source-control probe (git, git-lfs, identity, credential helper),
	// same shape as the toolchain one above: run once on a background thread,
	// null until it finishes. That null is load-bearing — without it the "Source
	// Control Not Ready" dialog would flash on every startup before the answer
	// is known.
	const HE::Sc::GitProbe* gitProbe = nullptr;
	std::function<void()> recheckGit;
	// Sets user.name / user.email globally and re-probes. Offered in the dialog
	// because an unset identity is the most common reason a first commit fails
	// and the fix is one git config call, not an install.
	std::function<void(std::string name, std::string email)> setGitIdentity;
	bool gitIdentityApplying = false;

	// Startup collaboration-reachability probe (router / port forwarding / IPv6),
	// same null-means-still-running convention as the two above. Read-only: it
	// discovers the router but never creates a mapping. Shown in
	// Preferences ▸ Tools ▸ Status and in the Collaboration window.
	const HE::Net::RouterProbe* routerProbe = nullptr;
	std::function<void()> recheckRouter;

	// Source-control state for the panel and the Content Browser badges. Null in
	// builds without the module.
	GitController* git = nullptr;

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

	// Content browser icon textures (white images, tinted at render time).
	// There is one per HE::AssetType the browser can show: the grid used to pick
	// icons by file EXTENSION, which meant every engine asset — all of them
	// ".hasset" — matched nothing and rendered as an empty button. The extension
	// map now only serves loose source files (.png/.obj/.lua/…).
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
		ImTextureID materialFunction    = 0;
		ImTextureID shader              = 0;
		ImTextureID prefab              = 0;
		ImTextureID animationClip       = 0;
		ImTextureID propertyAnimClip    = 0;
		ImTextureID widget              = 0;
		ImTextureID horizonCodeClass    = 0;
		ImTextureID inputAction         = 0;
		ImTextureID inputMappingContext = 0;
		ImTextureID particleSystem      = 0;
		ImTextureID animatorStateMachine= 0;
		ImTextureID font                = 0;
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

	// Live collaboration session (see CollabController). Null until the editor
	// has constructed it; the panel treats that as "not available".
	CollabController* collab = nullptr;

	// Things that happened without the user asking — a peer that could not apply
	// a delete, a scan that could not read a file, an asset nobody answered
	// about. Posted from ANY thread (see NotificationStore), drawn by the footer
	// bell and its flyout. Null in a headless/test context, so every caller has
	// to check — which is also why posting goes through the helper below rather
	// than being written out at forty call sites.
	HE::Ed::NotificationStore* notifications = nullptr;

	// Hand an ON-DISK reference rewrite to the editor's single retarget queue
	// (EditorApplication::enqueueRetargetOnDisk). Panels must not run one
	// themselves: the walk rewrites every referencing file, and two of them over
	// the same file — a local move and an approved remote rename, say — lose one
	// rewrite entirely. It is also far too slow for a frame on a real project.
	// The IN-MEMORY half stays with the caller and stays on the main thread.
	// Null in tests/headless: callers fall back to doing it inline.
	std::function<void(const std::string& oldRel, const std::string& newRel, bool folder)>
		enqueueRetarget;

	// Per-user undo/redo used INSTEAD of the snapshot stack while a session is
	// running — see CollabUndo.h for why snapshots cannot work there.
	CollabUndo* collabUndo = nullptr;

	// An absolute path → the key a session addresses it by, or empty when it is
	// nothing a session carries. EditorApplication::collabSyncKey behind a
	// function, because panels need the same answer and cannot reach it: a
	// content-relative path cannot name a C++ class (Source is a SIBLING of
	// Content, so the content root does not contain it), and using the
	// content-relative form there silently made every create and delete of a
	// C++ class a local-only one — in a session that says the file travels.
	std::function<std::string(const std::string&, bool /*isFolder*/)> collabKeyForPath;
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
	// HorizonCode classes attached to scene entities (ScriptComponent pointing at
	// a class asset) — PIE only, same lifetime as m_playerHost, same runtime.
	EntityHost         m_entityHost;
	// The state machines' sync graphs — PIE only, same lifetime and runtime.
	// Handed to the animation phase rather than ticked here, so each graph fires
	// right before the transitions it feeds.
	AnimatorHost       m_animatorHost;
	HorizonCode::Graph m_gameInstanceGraph;
	void loadGameInstanceGraph();  // read the project's GameInstance.hcode → host
	void saveGameInstanceGraph();  // write m_gameInstanceGraph → project file
	std::string gameInstancePath(); // <projectDir>/GameInstance.hcode

	// Per-project open-tab persistence (stored in the global config keyed by
	// project path). restoreOpenTabs runs on project load; saveOpenTabs runs when
	// the tab set changes (via the signature check each frame) + on shutdown.
	// Write an asset file verbatim and reload it. Shared by remote asset updates
	// and by collaborative undo — writing bytes rather than deserializing by type
	// is what makes both uniform across every asset kind.
	void        applyAssetBytes(const std::string& relativePath,
	                            const std::vector<std::uint8_t>& bytes);
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
	// One definition for both apps (PhysicsWorld.h): the packaged game has to
	// simulate at the rate the editor previewed it at.
	static constexpr float kPhysicsFixedDt = PhysicsWorld::kFixedDt;

	// Audio engine — initialised at startup, active always (spatial update only in play mode).
	AudioEngine m_audioEngine;

	// Script execution context (play mode only; null outside play mode).
	std::unique_ptr<ScriptContext> m_scriptContext;
	// Maps raw entity handle → Lua instance id (parallel lifecycle to m_scriptContext).
	std::unordered_map<uint32_t, ScriptEngine::InstanceId> m_scriptInstances;

	// Outliner/inspector selection
	Entity m_selectedEntity = entt::null;

	// ── Duplicate / cut / copy / paste / delete ──────────────────────────────
	// Exposed through AppContext (see the block there for what the clipboard
	// holds and why). A copy taken from a scene that was since closed stays
	// valid — it is self-contained data, not a reference into a world.
	std::vector<std::uint8_t> m_entityClipboard;
	void duplicateSelectedEntity();
	void copySelectedEntity(bool cut);
	void pasteEntityClipboard();
	void deleteSelectedEntity();
	// Where a copy of `source` belongs: beside it, under the same parent. Shared
	// by duplicate and paste so the two land in the same place.
	Entity siblingParentFor(Entity source) const;

	// Editor scene-view camera
	EditorCamera m_editorCamera;

	// Live collaboration. Poll-driven: pumped once per frame from OnRender.
	CollabController m_collab;
	// Editor-wide "something happened" channel — see NotificationStore.h. Owned
	// here and handed to the UI through AppContext, the same shape m_collab and
	// m_git use, except that this one is written from worker threads too and
	// therefore carries its own mutex.
	HE::Ed::NotificationStore m_notifications;
	GitController    m_git;
	CollabUndo       m_collabUndo;
	// Entities the session already knows about. Diffed each frame so every
	// creation and deletion path is covered without hooking any of them.
	std::unordered_set<Entity> m_structureKnown;
	// New subtrees whose create the session refused, and for how many frames.
	// Separate from m_structureKnown because "we tried and it did not go" is not
	// "the peers have it" — recording the two as one is what let an oversize or
	// mid-hiccup subtree exist on this machine alone, with nothing retrying and
	// nothing said. Entries leave on the first success or when the retries run
	// out; see syncStructuralChanges for why there is a limit at all.
	std::unordered_map<Entity, int> m_structureUnsendable;
	void syncStructuralChanges();
	// Asset-level collaboration. Two layers, doing different jobs: item-level
	// DOCUMENT DELTAS make an open graph / UI editor live (the peer patches the
	// document it is already showing), and the debounced whole-file autosave
	// underneath is the baseline that persists edits into each peer's project,
	// serves a peer who opens the tab later, and covers the asset types with no
	// item structure. Locks stay lazy — but "may I edit this" is answered by the
	// HOST when the tab opens, not guessed from the replicated table.
	void updateAssetCollabSync(std::uint64_t nowMs);
	void publishDocDeltas(const std::string& absPath, const std::string& key);
	void applyRemoteDocDeltas(const std::string& key,
	                          const std::vector<HE::Net::CollabSession::DocDelta>& batch);

	// ── Sync keys ────────────────────────────────────────────────────────────
	// The string a syncable thing is addressed by across peers. Three shapes,
	// because not everything a session shares is a file under Content:
	//
	//   "Materials/Rock.hasset"   a content asset, relative to the Content root
	//   "::Source::Player.h"      a C++ source file, relative to <project>/Source
	//   "::LevelScript::"         the scene's HorizonCode graph — no file at all
	//   "::GameInstance::"        the project's GameInstance graph (<project>/…)
	//
	// The reserved prefixes are the existing virtual TAB paths, so a tab is its
	// own key and nothing has to be mapped. A bare content-relative path stays the
	// wire format it already was.
	// `isFolder` is passed, not sniffed with is_directory: the path exists at
	// every call site today, but a caller naming a destination that does not
	// exist yet would be misclassified in silence — and every other folder/file
	// distinction in this code travels as an explicit flag for the same reason.
	std::string collabSyncKey(const std::string& tabPath, bool isFolder = false);
	// Where a key lives locally; empty for a document that has no file of its own.
	std::string collabLocalPath(const std::string& key);
	// The project directory (currentProject().path may name the .heproj itself).
	std::string projectRoot();
	// Bindings for a tab, including the two documents the editor owns rather than
	// a panel (the level script lives in the world, the GameInstance graph here).
	CollabDocSync::DocBindings collabDocsForTab(const std::string& tabPath);
	// Reserved key prefix for the C++ tree.
	static constexpr const char* kSourceKeyPrefix = "::Source::";
	// Mirrors for the two editor-owned documents (panels keep their own).
	CollabDocSync::DocMirror m_levelScriptMirror;
	CollabDocSync::DocMirror m_gameInstanceMirror;
	std::unordered_map<std::string, std::uint64_t> m_assetLastAutosaveMs;
	// Syncable asset tabs that were open last pass, so the ones that closed can
	// drop what the host told them about their lock.
	std::unordered_set<std::string> m_docMirrorPaths;
	// Last transform we recorded for the held subject, so an undo entry spans a
	// whole edit rather than one entry per frame of a drag.
	std::uint64_t    m_undoBaselineSubject = 0;
	float            m_undoBaseline[9] {};

	// In-game UI pointer input during PIE. The viewport panel reports the
	// mouse in render-target pixels each frame (reportPlayUIPointer); the
	// update loop hit-tests + dispatches onClick/onHover* to scripts.
	UIInputSystem::InputState m_uiInputState;
	float m_uiPointerX = 0.0f, m_uiPointerY = 0.0f;
	float m_uiViewportW = 0.0f, m_uiViewportH = 0.0f;
	bool  m_uiPointerDown = false, m_uiPointerValid = false;
	float m_uiWheel = 0.0f;   // this frame's notches, consumed by the widget pass
	// Last frame's UI-navigation buttons (bits: up/down/left/right/activate) —
	// the input layer reports held state, and holding Down must step one entry.
	std::uint8_t m_uiNavPrev = 0;
	bool  m_widgetTextInputActive = false; // SDL text input toggled for a focused widget field

	// Play-in-editor
	bool m_isPlaying = false;
	// Transport pause + single step. The pause GATES THE WORLD TICK; it must never
	// write time::setTimeScale, because that knob belongs to the game — a title
	// with its own pause menu sets it itself, and two owners of one variable fight
	// each other. m_stepFrame lets exactly one tick through and is consumed by the
	// frame that used it, so the pause re-arms on its own.
	bool m_isPaused  = false;
	bool m_stepFrame = false;
	// Play-session log capture (see PlayLogEntry / the post-PIE report window).
	std::vector<PlayLogEntry> m_playLog;
	std::mutex                m_playLogMutex;
	bool                      m_playReportOpen = false;
	void setPlayMode(bool play);
	// app.quit() from a script asks to leave PLAY mode, not to close the editor —
	// a game testing its own main menu must not take the editor down with it. The
	// request is parked rather than acted on where it arrives: it comes out of a
	// running graph, and setPlayMode tears the runtime that graph is executing in
	// down. Consumed at the top of OnRender.
	bool m_playStopRequested = false;

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
	// Hand the current viewport image to the thumbnail cache as this scene's
	// tile. A scene has nothing to render a preview FROM, so its picture is
	// taken at save time instead of generated on demand.
	void captureSceneThumbnail(const std::string& scenePath);
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

	// Source-control capability probe. Same lifecycle as the toolchain probe:
	// worker thread, atomic flag, joined at shutdown.
	std::thread        m_gitThread;
	std::atomic<bool>  m_gitChecked{false};
	HE::Sc::GitProbe   m_gitProbe;
	void startGitProbe();
	// Applying an identity runs git config off the frame thread — brief, but a
	// misbehaving credential helper or a locked config file must not freeze the
	// editor mid-frame.
	std::thread       m_gitIdentityThread;
	std::atomic<bool> m_gitIdentityApplying{false};
	void applyGitIdentity(std::string name, std::string email);

#ifdef HE_HAVE_LIBSSH2
	// EngineContent SFTP reachability probe. Same lifecycle as the other
	// startup probes above. Absent entirely when libssh2 could not be
	// resolved at configure time (see the libssh2 block in the root
	// CMakeLists) — every call site is guarded the same way.
	std::thread          m_sftpThread;
	std::atomic<bool>    m_sftpChecked{false};
	HE::Cs::SftpProbeResult m_sftpProbe;
	void startSftpProbe();
	// Re-fetch manifest.json + re-merge the Engine tree, without re-probing.
	// Reuses m_sftpThread (joined first), so only one of the two can be in
	// flight — which is what we want: both write the same manifest state.
	void refreshEngineContentManifest();
	// Set by startSftpProbe()'s worker once a fresh manifest is available;
	// consumed (exchanged back to false) once, on the main thread in OnRender,
	// which is the only thread allowed to mutate ContentManager — see
	// registerRemoteAsset()'s contract.
	std::atomic<bool> m_sftpManifestReady{false};
#endif

	// Collaboration reachability probe (router / port forwarding / IPv6). Same
	// lifecycle again, with one addition: it is the only probe that waits on the
	// NETWORK, so it carries a cancel flag the shutdown join raises first —
	// otherwise quitting during a router timeout would stall for seconds.
	std::thread        m_routerThread;
	std::atomic<bool>  m_routerChecked{false};
	std::atomic<bool>  m_routerCancel{false};
	HE::Net::RouterProbe m_routerProbe;
	void startRouterProbe();

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
	// One per remaining HE::AssetType, so no engine asset falls back to a blank
	// tile (see AppContext::CbIcons).
	ImTextureID m_iconMaterialFunction    = 0;
	ImTextureID m_iconShader              = 0;
	ImTextureID m_iconPrefab              = 0;
	ImTextureID m_iconAnimationClip       = 0;
	ImTextureID m_iconPropertyAnimClip    = 0;
	ImTextureID m_iconWidget              = 0;
	ImTextureID m_iconHorizonCodeClass    = 0;
	ImTextureID m_iconInputAction         = 0;
	ImTextureID m_iconInputMappingContext = 0;
	ImTextureID m_iconParticleSystem      = 0;
	ImTextureID m_iconAnimatorStateMachine= 0;
	ImTextureID m_iconFont                = 0;

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

	// Reference retargeting for a rename that arrived from the session, kept off
	// the frame. Futures rather than raw threads on purpose: a std::async future
	// BLOCKS in its destructor until the work finishes, so quitting cannot leave
	// a half-rewritten content tree behind, and nothing has to remember to join
	// them. Pruned each frame so the vector does not grow with the session.
	std::vector<std::future<void>> m_retargetJobs;
	// Serialised, not parallel: the walk is a read-modify-write over every
	// referencing file, so two of them racing lose one rename's rewrites and
	// leave those references broken. One worker drains this queue.
	struct RetargetJob { std::string oldRel, newRel; bool folder = false; };
	std::vector<RetargetJob> m_retargetQueue;
	std::mutex               m_retargetMutex;
	bool                     m_retargetRunning = false;
	// THE one place an on-disk retarget may be started from. It began as the
	// collaboration path's private queue, and stayed private while the Content
	// Browser did its own retarget inline on the frame thread — which is a data
	// race, not just a stall: the walk reads each referencing file whole and
	// writes it back, so a local drag-move and an approved remote rename
	// rewriting the same file at the same time lose one of the two rewrites
	// entirely. One queue is what makes "one at a time" true for everybody.
	//
	// The in-memory half is NOT here: it mutates ContentManager's unguarded maps
	// and stays on the main thread, before the call.
	void enqueueRetargetOnDisk(const std::string& oldRel, const std::string& newRel,
	                           bool folder);

	AppContext makeContext();
};

