#include "EditorUI.h"
#include "EditorTheme.h"   // brand palette (the editor tab strip)
#include <algorithm>
#include <cstdint>
#include "EditorApplication.h"
#include "ScriptEditorPanel.h"
#include "CppClassEditorPanel.h"
#include "MaterialEditorPanel.h"
#include "UIEditorPanel.h"
#include "LevelScriptPanel.h"
#include "GameInstancePanel.h"
#include "HorizonCodeClassPanel.h"
#include "InputAssetPanel.h"
#include "TypeAssetPanel.h"
#include "ThemeAssetPanel.h"
#include "SkeletalMeshEditorPanel.h"
#include "StaticMeshEditorPanel.h"
#include "ParticleGraphEditorPanel.h"
#include "AudioEditorPanel.h"
#include "EditorInput.h"    // pointer-device grammar frame cache
#include "AnimatorStateMachineEditorPanel.h"
#include "ExportDialogPanel.h"           // Build > Export Project modal + packing worker
#include "ContentBrowserPanel.h"         // bottom dock: folder tree + asset grid
#include "InspectorPanel.h"              // right dock: per-entity Details panel
#include "TerrainTools.h"                // Landscape brush state, viewport sculpt + tool panel
#include "ViewportPanel.h"               // centre dock: Scene viewport, camera, gizmo, picking
#include "OutlinerPanel.h"               // right dock: World Outliner hierarchy tree
#include "ProjectHubPanel.h"             // start screen while no project is open
#include "TutorialPanel.h"               // first-start welcome + Help ▸ Interactive Tutorial
#include "ProfilerPanel.h"               // View > Performance Profiler window
#include "ConsolePanel.h"                // View > Console — every HE_LOG record, all levels
#include "EnvironmentPanel.h"
#include "CollabPanel.h"            // View > Collaboration (host / join a live session)
#include "CollabActivityBar.h"      // what the session did to the project — footer line
#include "CollabPresenceBar.h"      // who else is in the session — footer cluster + menu
#include "NotificationBar.h"        // "something happened" bell — footer cluster + flyout
#include "SourceControlPanel.h"     // View > Source Control (repository status)
#include "EngineContentSyncBar.h"   // EngineContent SFTP download queue — footer status
#include "EngineContentPublishDialog.h" // Assets > Publish Engine Content to Server...
#include "HcRenameDialog.h"            // "that rename reaches other files" — from both graph editors
#include "EditorSettingsPanel.h"         // engine-settings catalog + Preferences tab
#include "ToolchainDialog.h"
#include "GitMissingDialog.h"             // startup cmake/compiler check
#include "ReportIssueDialog.h"           // Help > Report Issue (pre-filled GitHub issue)
#include "DocsPanel.h"                   // Help > Documentation (the in-editor manual)
#include "EditorHelp.h"                  // one scope per menu; the rows look themselves up
#include "EditorDockState.h"             // "is this panel docked into the layout?"
#include "PlayReportPanel.h"             // post-PIE warning/error report
#include "EditorAssetTypeCache.h"        // shared path → AssetType sniff (invalidated below)
#include "EditorWidgets.h"               // dialog placement + detached-modal raise
#include "HorizonVersion.h"              // HE_VERSION_FULL — Help ▸ About
#ifdef __APPLE__
#include "MacMenuBar.h"   // native system menu bar (replaces the ImGui menu row)
#endif
#include <HorizonScene/HorizonScene.h>
#include <ContentManager/ContentManager.h>
#include <Types/Enums.h>
#include "ImporterCommon.h"   // Importer::importSource — extension → importer routing

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#endif

#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <array>

// Forward declaration — defined in EditorApplication.cpp
std::string getRHIName(HE::RendererBackend backend);

namespace
{
	// The async SDL file slot (pendingFileReady/Result) is shared across project
	// and scene operations; this records which one is currently in flight so the
	// single result handler can dispatch correctly.
	enum class PendingFileOp { OpenProject, OpenScene, SaveScene, ImportAsset, AddSceneAdditive };

	// A destructive action that would discard the current scene. When requested
	// while the scene is dirty it is stashed and a "Save changes?" modal is shown;
	// the action runs once the user resolves the modal.
	enum class GuardedAction {
		None, NewScene, OpenSceneDialog, OpenScenePath,
		OpenProjectDialog, CloseProject, Quit,
	};
}

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder* for the default dock layout
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#ifdef _WIN32
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <d3d11.h>
#include <d3d12.h>
#include <shobjidl.h>
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
#include <vulkan/vulkan.h>
#include <imgui_impl_vulkan.h>
#endif
#ifdef HE_IMGUI_METAL_ENABLED
#include "ImGuiMetalBridge.h"
#include <Backends/Metal/MetalRenderer.h>
#endif
#include <ImGuizmo.h>

// File-local alias. It used to arrive transitively from the public
// HorizonRendering/ShaderManager.h, which declared it at global scope and so
// leaked `fs` into every consumer of that header.
namespace fs = std::filesystem;


// Builds the editor's default dock layout into the given dockspace node. Only
// called when the imgui.ini did not already provide a layout (DockBuilderGetNode
// == nullptr), so a saved arrangement always wins. Windows the ini doesn't place
// fall into this default instead of floating loose. Mirrors the panel layout in
// the reference screenshots: thin toolbar floats on top; Quick Settings left,
// World Outliner + Details stacked right, Content Browser bottom, Scene centre.
// Set by View > Reset Layout; consumed by the dockspace block in renderEditor()
// to force a rebuild of the default layout even when a layout is already loaded.
static bool s_resetLayoutRequested = false;

// Latch for HideSceneTabBarOnce() below — cleared when the layout is rebuilt,
// because a fresh layout comes back with the tab bar showing.
static bool s_sceneTabBarHandled = false;


// Times Assets > Import Asset opened the file dialog this run (guided tour signal).
static int s_importDialogOpens = 0;

// (Preferences opens as an editor tab — see EditorSettingsPanel::kTabPath.)

// (The View-menu panel toggle is `togglePanelWindow` inside renderEditor — it
// needs the tab list to reveal a docked panel.)

// ── Revealing a tool window ──────────────────────────────────────────────────
// Show it and bring it to the front, whatever state it was in: closed, floating
// behind something else, or docked as a tab that is not the active one. Focusing
// a docked window is what activates its tab, so all three collapse into one
// call.
//
// Deliberately NOT toggleFloatingWindow: this is what the footer's status
// widgets do, and a status line that CLOSES the panel it reports on when you
// click it a second time is a trap rather than a shortcut.
//
// The two-frame latch covers the case the obvious version gets wrong. A window
// that was closed does not exist yet at the moment of the click — it is created
// later in the same frame — and SetWindowFocus on a name ImGui has never seen is
// a silent no-op. So the request is repeated once the window is certain to be
// there.
static const char* s_revealTitle       = nullptr;
static int         s_revealFocusFrames = 0;

static void revealFloatingWindow(bool& open, const char* title)
{
    open = true;
    ImGui::SetWindowFocus(title);
    s_revealTitle       = title;
    s_revealFocusFrames = 2;
}

// Toggled by View > Performance Profiler; drives the profiler panel.
static bool s_showProfiler = false;

// Toggled by View > Environment; drives the Sky/Weather add-remove window.
static bool s_showEnvironment = false;

// Toggled by View > Collaboration; drives the live-session panel.
static bool s_showCollab = false;
// Toggled by View > Source Control; drives the repository status panel.
static bool s_showSourceControl = false;
// Toggled by View > Console (and Ctrl/Cmd+`); drives the log window.
static bool s_showConsole = false;

// Help ▸ Documentation Online. The published manual on the website; the OFFLINE
// copy the reader panel shows ships next to the editor (EditorDeps/Docs), which
// is what Help ▸ Documentation opens.
static constexpr const char* kDocsUrl = "https://horizoncreations.dev/HorizonEngineDocs/";

// ── The docs reader's "Show me" ──────────────────────────────────────────────
// An article about a panel offers to point at it. Putting a panel on screen
// means flipping one of the file statics below, which is why the reader is
// handed this function instead of reaching for them: it takes an ImGui window
// name and returns whether it knew what to do with it.
//
// Registered per frame and only while a project is loaded (see render()) — on
// the Project Hub none of these windows exist, and a "Show me" that silently
// does nothing is worse than one that is not offered.
static bool docsPanelOpener(const char* window)
{
	if (!window || !window[0]) return false;

	// The View-menu panels: closed until asked for, so this both opens and
	// focuses (revealFloatingWindow's two-frame latch covers a window that does
	// not exist yet at the moment of the click).
	struct Toggle { const char* name; bool* flag; };
	const Toggle toggles[] = {
		{ "Performance Profiler", &s_showProfiler      },
		{ "Environment",          &s_showEnvironment   },
		{ "Collaboration",        &s_showCollab        },
		{ "Source Control",       &s_showSourceControl },
		{ "Console",              &s_showConsole       },
	};
	for (const Toggle& t : toggles)
		if (std::strcmp(window, t.name) == 0) { revealFloatingWindow(*t.flag, window); return true; }

	// The panels that are always part of the docked layout. They are submitted
	// every frame, so there is nothing to open — focusing one selects its tab,
	// which is what "show me" means for a panel that is already there.
	static const char* const kAlwaysSubmitted[] = {
		"Scene", "World Outliner", "Details", "Content Browser", "Quick Settings",
	};
	for (const char* name : kAlwaysSubmitted)
		if (std::strcmp(window, name) == 0) { ImGui::SetWindowFocus(window); return true; }

	return false;
}

// ── Remembering which panels were open ───────────────────────────────────────
// A panel the user docked into the layout is part of how their editor looks;
// having to re-tick it in the View menu after every restart makes the dock
// pointless. A panel left FLOATING is the opposite — a thing pulled up to look
// at once — so that one is not restored, and the editor comes back uncluttered.
//
// The dock test itself lives in EditorDockState — the obvious spelling of it
// (ImGui::IsWindowDocked) is wrong in a way that fails silently, so it is
// written down once and asserted in a test.
static bool panelIsDockedInLayout(const char* title)
{
	return EditorDockState::isDockedInLayout(title);
}

struct PanelVisibilityPref
{
	const char* title;
	const char* configKey;
	bool*       open;
	bool        written  = false;   // what config.json currently holds
	bool        pending  = false;   // candidate, once it stops changing
	int         stable   = 0;       // frames `pending` has held still
};

static PanelVisibilityPref s_panelPrefs[] = {
	{ "Performance Profiler", "PanelOpenProfiler",      &s_showProfiler      },
	{ "Environment",          "PanelOpenEnvironment",   &s_showEnvironment   },
	{ "Collaboration",        "PanelOpenCollaboration", &s_showCollab        },
	{ "Source Control",       "PanelOpenSourceControl", &s_showSourceControl },
	{ "Console",              "PanelOpenConsole",       &s_showConsole       },
};
static bool s_panelPrefsLoaded = false;

// A dock node is not resolved on the frame a window first appears, and a drag
// passes through undocked states on its way to a new slot. Persisting either
// would write the wrong answer, so a value has to hold still first. Half a
// second at 60 Hz: long enough to outlast both, short enough that quitting
// right after a toggle still catches it (and OnShutdown flushes what has not).
static constexpr int kPanelPrefStableFrames = 30;

static void loadPanelVisibility(AppContext& ctx)
{
	if (s_panelPrefsLoaded || !ctx.globalState) return;
	s_panelPrefsLoaded = true;
	for (PanelVisibilityPref& p : s_panelPrefs)
	{
		const bool open = ctx.globalState->getCustomConfigBool(p.configKey, false);
		*p.open   = open;
		p.written = open;
		p.pending = open;
		p.stable  = kPanelPrefStableFrames;   // nothing to write until it changes
	}
}

// Per frame, after the panels have been drawn.
//
// Only with a project open, and that guard is load-bearing rather than tidy:
// the Project Hub draws no panels at all, so every window would be missing and
// every preference would be read as "closed, not docked" and overwritten. The
// same reason savePanelVisibility checks it.
static void updatePanelVisibility(AppContext& ctx)
{
	if (!ctx.globalState || !ctx.projectLoaded) return;
	bool dirty = false;
	for (PanelVisibilityPref& p : s_panelPrefs)
	{
		const bool want = *p.open && panelIsDockedInLayout(p.title);
		if (want != p.pending) { p.pending = want; p.stable = 0; continue; }
		if (p.stable < kPanelPrefStableFrames) { ++p.stable; continue; }
		if (p.pending != p.written)
		{
			ctx.globalState->setCustomConfigEntry(p.configKey, p.pending);
			p.written = p.pending;
			dirty     = true;
		}
	}
	// One write for however many changed together (Reset Layout moves all five).
	if (dirty) ctx.globalState->writeConfig();
}

// (Level Script + Game Instance open as editor tabs, not toggled windows.)

// Build > Export Project — dialog state, the packing worker thread and the
// cook-time shader precompilers all live in ExportDialogPanel.cpp.
void EditorUI::joinPendingExport()
{
	ExportDialogPanel::joinPendingExport();
	// Same rule, different worker: Help ▸ Report Issue may still be uploading a
	// log or filing an issue, and a joinable std::thread destroyed at teardown
	// terminates the process.
	ReportIssueDialog::joinPendingWork();
}

void EditorUI::savePanelVisibility(AppContext& ctx)
{
	// Shutdown flush for a change that has not sat still long enough to have
	// been written yet — closing a panel and immediately quitting is an ordinary
	// thing to do, and it should still be remembered.
	// Quitting from the Project Hub must not be read as "the user closed
	// everything" — the hub simply never draws these panels (see
	// updatePanelVisibility). Leave what the last editor session wrote.
	if (!ctx.globalState || !s_panelPrefsLoaded || !ctx.projectLoaded) return;
	bool dirty = false;
	for (PanelVisibilityPref& p : s_panelPrefs)
	{
		const bool want = *p.open && panelIsDockedInLayout(p.title);
		if (want == p.written) continue;
		ctx.globalState->setCustomConfigEntry(p.configKey, want);
		p.written = want;
		dirty     = true;
	}
	if (dirty) ctx.globalState->writeConfig();
}

static void BuildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size)
{
	ImGui::DockBuilderRemoveNode(dockspaceId);
	ImGui::DockBuilderAddNode(dockspaceId,
		ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::DockBuilderSetNodeSize(dockspaceId, size);

	// Fractions are relative to the node being split (they compound). Tuned to the
	// reference layout: Quick Settings ~18% left, World Outliner/Details ~21%
	// right (split 50/50), Content Browser ~33% of the centre column's height.
	ImGuiID dockMain  = dockspaceId;
	ImGuiID dockLeft  = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left,  0.18f, nullptr, &dockMain);
	ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.26f, nullptr, &dockMain);
	ImGuiID dockDown  = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down,  0.33f, nullptr, &dockMain);
	ImGuiID dockRightBottom =
		ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.50f, nullptr, &dockRight);

	ImGui::DockBuilderDockWindow("Quick Settings", dockLeft);
	ImGui::DockBuilderDockWindow("World Outliner", dockRight);
	ImGui::DockBuilderDockWindow("Details",        dockRightBottom);
	ImGui::DockBuilderDockWindow("Content Browser", dockDown);
	// The Console shares the bottom node with the Content Browser: output belongs
	// next to the assets it is about, and a tab there costs no space until it is
	// opened.
	ImGui::DockBuilderDockWindow("Console",        dockDown);
	ImGui::DockBuilderDockWindow("Scene",          dockMain);
	ImGui::DockBuilderFinish(dockspaceId);
}

// The Scene viewport starts without its tab bar — the strip holding a single tab
// labelled with the window it is already inside, which costs a row of pixels off
// the rendered image. This is the state behind the little arrow in a docked
// window's corner ▸ "Hide tab bar", applied for you.
//
// It runs once per editor run rather than once ever: the state belongs to the
// dock node and is saved into imgui.ini, so an existing layout (or one the user
// re-showed the tab bar in) would otherwise keep it. Re-showing it during a
// session still works — ImGui leaves a small triangle in the node's corner for
// exactly that — the next start just goes back to hidden.
//
// Only ever applied to a node holding the viewport ALONE. Hiding the tab bar of
// a node the user docked a second window into would make that window
// unreachable, which is why ImGui itself only offers the option on a single tab.
static void HideSceneTabBarOnce()
{
	if (s_sceneTabBarHandled) return;
	ImGuiWindow* scene = ImGui::FindWindowByName("Scene");
	if (!scene || !scene->DockIsActive || !scene->DockNode)
		return;   // not submitted or not docked yet — try again next frame
	ImGuiDockNode* node = scene->DockNode;
	if (node->Windows.Size <= 1 && !node->IsHiddenTabBar() && !node->IsNoTabBar())
		node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
	s_sceneTabBarHandled = true;
}


#endif // HE_IMGUI_ENABLED

// ─── render ───────────────────────────────────────────────────────────────────

void EditorUI::render(AppContext& ctx, float dt)
{
#ifdef HE_IMGUI_ENABLED
    if (!ctx.imguiReady) return;

    // Refresh the frame-cached pointer-grammar answer for ctx-less call sites
    // (the shared GraphEditor canvas asks via EditorInput::trackpadActive()).
    EditorInput::trackpadPointer(ctx);

    ImGuiIO& io = ImGui::GetIO();

    // Begin new ImGui frame — platform backend is backend-specific
    switch (ctx.backend)
    {
    case HE::RendererBackend::OpenGL:
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
#ifdef _WIN32
    case HE::RendererBackend::D3D11:
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
    case HE::RendererBackend::D3D12:
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        {
            io.DisplaySize = ImVec2(static_cast<float>(ctx.window->GetWidth()),
                static_cast<float>(ctx.window->GetHeight()));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
        break;
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
    case HE::RendererBackend::Vulkan:
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
#endif
#ifdef HE_IMGUI_METAL_ENABLED
    case HE::RendererBackend::Metal:
        if (auto* mtl = static_cast<MetalRenderer*>(ctx.renderer))
            ImGuiMetalBridge::NewFrame(mtl->GetFramePassDescriptor());
        ImGui_ImplSDL3_NewFrame();
        break;
#endif
    default:
        break;
    }

    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    // Apply the user's UI font scale preference (clamped to a sane range).
    ImGui::GetStyle().FontScaleMain = std::clamp(ctx.editorConfig.UiFontScale, 0.5f, 3.0f);

    // ── Content-Refresh Popup ─────────────────────────────────────────────────
    if (ctx.contentRefreshPending || ctx.contentRefreshDone)
    {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(360, 100), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("##ContentRefresh", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            // One literal for both the measurement and the draw. They were two
            // copies of the same string, so editing the wording in one place left
            // the label centred against the length of the OTHER wording.
            constexpr const char* k_refreshLabel = "Updating project data...";

            float textY = (100.0f - ImGui::GetTextLineHeightWithSpacing() * 2.0f) * 0.5f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textY);
            ImGui::SetCursorPosX((360.0f - ImGui::CalcTextSize(k_refreshLabel).x) * 0.5f);
            ImGui::TextUnformatted(k_refreshLabel);

            if (ctx.contentRefreshPending)
            {
                ctx.globalState->refreshContentFolder();
                ctx.globalState->refreshSourceFolder();
                if (ctx.contentManager)
                    ctx.globalState->refreshEngineFolder(ctx.contentManager->engineContentRoot(),
                                                          ctx.contentManager->contentRoot());
                ctx.contentRefreshPending = false;
                ctx.contentRefreshDone    = true;
            }
            else
            {
                ImGui::CloseCurrentPopup();
                ctx.contentRefreshDone = false;
                // Deliberately does NOT touch projectLoaded. This used to set it
                // true, which reads as "the refresh finished, so a project must be
                // open" — and that is false exactly once: closing a project ends
                // its session, endProjectSession asks for a content refresh, and
                // this branch then reopened the editor on the project that was just
                // closed. The Project Hub was visible for the single frame in
                // between. Every path that opens a project sets the flag itself.
            }
            ImGui::EndPopup();
        }
        else
        {
            ImGui::OpenPopup("##ContentRefresh");
        }
    }

    // ── Silent content refresh (create/rename) ───────────────────────────────
    // Runs here, before renderEditor acquires the content-folder shared lock, so
    // refreshContentFolder()'s unique_lock can't deadlock against it.
    if (ContentBrowserPanel::quietRefreshRequested() && ctx.globalState)
    {
        ctx.globalState->refreshContentFolder();
        ctx.globalState->refreshSourceFolder();
        if (ctx.contentManager)
            ctx.globalState->refreshEngineFolder(ctx.contentManager->engineContentRoot(),
                                                  ctx.contentManager->contentRoot());
        ContentBrowserPanel::clearQuietRefreshRequest();
    }

    // ── Drop the path → asset-type cache whenever the content tree changed ────
    // The version counters are bumped by EVERY refresh — the two above, the modal
    // one, and EditorApplication's periodic async poll — which is exactly when the
    // asset at a path may have been replaced (deleted + recreated as another type,
    // or overwritten outside the editor). Without this the panels' cached header
    // sniff outlived the file and double-clicking opened the wrong editor. Runs
    // before the routing below, so this frame's tab dispatch already sees the truth.
    if (ctx.globalState)
    {
        static uint64_t s_typeCacheContentVersion = ~0ull;
        static uint64_t s_typeCacheEngineVersion  = ~0ull;
        const uint64_t contentV =
            ctx.globalState->contentFolderVersion.load(std::memory_order_acquire);
        const uint64_t engineV =
            ctx.globalState->engineFolderVersion.load(std::memory_order_acquire);
        if (contentV != s_typeCacheContentVersion || engineV != s_typeCacheEngineVersion)
        {
            s_typeCacheContentVersion = contentV;
            s_typeCacheEngineVersion  = engineV;
            EditorAssetTypeCache::invalidateAll();
        }
    }

    // ── Re-index UUID → path whenever the content tree changed ────────────────
    // ContentManager::scanContentDirectory() builds the disk registry a scene uses
    // to resolve its component references (mesh/material UUIDs). It used to run
    // exactly once, in the project-open path — so every asset that arrived AFTER
    // that (a git pull, a branch switch, a file copied in via Finder, a teammate's
    // push) appeared in the Content Browser, because the folder refresh saw it, but
    // its UUID never entered the registry: a scene referencing it fell back to the
    // default cube until the project was reopened, with nothing logged to explain
    // why. Riding the same version counters as the block above makes the registry
    // follow every refresh there is — the modal, the quiet create/rename refresh,
    // EditorApplication's periodic async poll, and the SFTP manifest re-merge.
    //
    // Placement matters twice over. This is the main thread: scanContentDirectory
    // mutates ContentManager's maps and must never be called from the periodic
    // poll's std::async worker, which is why the worker only bumps the counter and
    // the rescan happens here. And it is not a per-frame cost — the counters change
    // only when a refresh actually rebuilt a tree. That distinction is the whole
    // design: the scan is a full three-root walk with a header sniff per .hasset,
    // measured at ~9 ms warm for a ~3000-entry tree, which is nothing once a minute
    // and more than half a frame budget every frame.
    if (ctx.globalState && ctx.contentManager)
    {
        // Source/ is deliberately not watched: it holds .h/.cpp, never a .hasset,
        // so a refresh there cannot change the registry.
        static uint64_t s_registryContentVersion = ~0ull;
        static uint64_t s_registryEngineVersion  = ~0ull;
        const uint64_t contentV =
            ctx.globalState->contentFolderVersion.load(std::memory_order_acquire);
        const uint64_t engineV =
            ctx.globalState->engineFolderVersion.load(std::memory_order_acquire);
        if (contentV != s_registryContentVersion || engineV != s_registryEngineVersion)
        {
            s_registryContentVersion = contentV;
            s_registryEngineVersion  = engineV;
            ctx.contentManager->scanContentDirectory();
        }
    }

    // Which docked panels were open last time. Once, as soon as there is a
    // config to read — before anything can draw a panel and report it closed.
    loadPanelVisibility(ctx);

    // ── Startup toolchain check — overlays either screen below ───────────────
    ToolchainDialog::DrawToolchainDialog(ctx);

    // ── Startup source-control check ─────────────────────────────────────────
    // Same placement, and for the same reason: it must overlay the Project Hub
    // as well as the editor, since a user can clone a project before opening one.
    GitMissingDialog::DrawGitMissingDialog(ctx);

    // ── Assets ▸ Publish Engine Content to Server… ───────────────────────────
    EngineContentPublishDialog::Draw(ctx);

    // ── "That rename reaches other files" ────────────────────────────────────
    // Raised by the graph editors after a HorizonCode member was renamed. Drawn
    // here rather than in either of them because both raise it, and because it
    // outlives the panel that asked: it writes assets, so it must not vanish
    // because a tab was switched while it stood open.
    HcRenameDialog::Draw(ctx);

    // ── Help ▸ Report Issue… ─────────────────────────────────────────────────
    // Drawn here too: something worth reporting can just as easily happen while
    // opening a project as after, and the macOS Help menu is reachable in both.
    ReportIssueDialog::DrawReportIssueDialog(ctx);

    // ── Help ▸ Documentation ─────────────────────────────────────────────────
    // Before the branch, so the manual is readable on the Project Hub as well as
    // in the editor. That is not symmetry for its own sake: "how do I start a
    // project" is the question the user has BEFORE a project exists, and the Hub
    // is exactly where they are while they have it.
    //
    // "Show me" is only offered with a project open — see docsPanelOpener.
    DocsPanel::setPanelOpener(ctx.projectLoaded ? &docsPanelOpener : nullptr);
    DocsPanel::draw(ctx);

    // ── Route to either the Project Hub or the full Editor UI ─────────────────
    if (ctx.projectLoaded)
    {
        renderEditor(ctx, dt);
        // After renderEditor, not inside it: it has an early return for asset tabs,
        // and these have to stay on screen (and keep watching) across a tab switch.
        renderOverlays(ctx, dt);
    }
    else
    {
        ProjectHubPanel::render(ctx);
        // Drawn after the hub so the welcome card sits on top of it. Draws nothing
        // once the user has answered it once (persisted in the editor config).
        TutorialPanel::renderWelcome(ctx);
    }

    // ── The tooltip of whatever the mouse is on ──────────────────────────────
    // Last of everything, and that placement is the point: a tooltip drawn where
    // the control is would overwrite ImGui's "last item" with the tooltip
    // window's own, and the Details panel commits its undo steps off exactly
    // that (IsItemDeactivatedAfterEdit, immediately after the control). So the
    // controls only QUEUE their help and it is drawn here, once, when every
    // panel has been submitted. See EditorWidgets.h.
    //
    // F1 belongs to whatever is under the mouse: over a control with an entry it
    // opens the manual at the section that explains it, and anywhere else it
    // opens the manual.
    if (const char* topic = EditorWidgets::drawQueuedHelp())
    {
        DocsPanel::openTopic(topic);
    }
    // The `openedThisFrame` guard is what keeps this from undoing an F1 that was
    // already answered: a graph node's hover tooltip consumes F1 inline, while
    // the canvas is being drawn, and opens the manual at that node's entry. This
    // block runs afterwards, sees the same still-pressed key, and would toggle
    // the reader it just opened straight back shut.
    else if (!io.WantTextInput && !DocsPanel::openedThisFrame() &&
             ImGui::IsKeyPressed(ImGuiKey_F1, false))
    {
        // Ctrl/Cmd+F1 goes straight to the search box — the label the Help menu
        // carries, and it has to be honoured HERE or it would fall through to
        // the toggle below and do something else entirely.
        if (io.KeyCtrl || io.KeySuper)     DocsPanel::openSearch("");
        else if (DocsPanel::isOpen())      DocsPanel::close();
        else                               DocsPanel::open();
    }

    ImGui::Render();

    // ── Multi-viewport / platform windows ─────────────────────────────────────
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        SDL_Window*   backupWin = SDL_GL_GetCurrentWindow();
        SDL_GLContext backupCtx = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        if (backupWin && backupCtx)
            SDL_GL_MakeCurrent(backupWin, backupCtx);

        // A modal that did end up in its own OS window must not be left behind the
        // editor: invisible there, but still swallowing every click.
        if (ctx.window)
            EditorWidgets::raiseDetachedModals(ctx.window->GetNativeWindow());
    }
#endif // HE_IMGUI_ENABLED
}



// True if the editor tab for `assetPath` holds edits that were never written to disk.
// EVERY panel that can modify its asset has to be listed here — this one predicate
// gates the tab's dirty mark, forgetTabState (a panel missing from the list has its
// unsaved graph dropped when the tab closes) and the Quit/Close-Project guard. That
// is exactly how the Particle and Animator-State-Machine graphs used to be lost.
// View-only panels (Static/Skeletal Mesh) have nothing to lose and stay out.
// The virtual tabs (Level Script / Game Instance) edit the world, so their dirty
// state is the scene's (ctx.sceneDirty) and is guarded separately.
// Public (declared in EditorUI.h) because the OS-level quit veto in
// EditorApplication::OnEvent has to ask the same question: an asset panel's edits
// do NOT bump the world undo revision, so a clean scene is no proof that there is
// nothing to lose.
bool EditorUI::tabHasUnsavedEdits(const std::string& assetPath)
{
	if (assetPath.empty()) return false;
	return ScriptEditorPanel::isDirty(assetPath)        ||
	       CppClassEditorPanel::isDirty(assetPath)      ||
	       MaterialEditorPanel::isDirty(assetPath)      ||
	       UIEditorPanel::isDirty(assetPath)            ||
	       HorizonCodeClassPanel::isDirty(assetPath)    ||
	       InputAssetPanel::isDirty(assetPath)          ||
	       TypeAssetPanel::isDirty(assetPath)           ||
	       ThemeAssetPanel::isDirty(assetPath)          ||
	       ParticleGraphEditorPanel::isDirty(assetPath) ||
	       AnimatorStateMachineEditorPanel::isDirty(assetPath);
}

// Every unsaved asset, INCLUDING ones whose tab the user already closed.
// Closing a dirty tab deliberately keeps the panel state (so reopening restores
// the edits) but removes the tab from ctx.tabs — so a guard that walks ctx.tabs
// sees nothing to lose and lets the editor quit. Asking the panels directly is
// the only view that covers both.
std::vector<std::string> EditorUI::unsavedAssetPaths()
{
	std::vector<std::string> out;
	ScriptEditorPanel::appendDirtyPaths(out);
	CppClassEditorPanel::appendDirtyPaths(out);
	MaterialEditorPanel::appendDirtyPaths(out);
	UIEditorPanel::appendDirtyPaths(out);
	HorizonCodeClassPanel::appendDirtyPaths(out);
	InputAssetPanel::appendDirtyPaths(out);
	TypeAssetPanel::appendDirtyPaths(out);
	ThemeAssetPanel::appendDirtyPaths(out);
	ParticleGraphEditorPanel::appendDirtyPaths(out);
	AnimatorStateMachineEditorPanel::appendDirtyPaths(out);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

// Save one asset through whichever panel owns its edits — the write half of
// tabHasUnsavedEdits, and it must list exactly the same panels (a panel missing
// here silently keeps its edits after the prompt claimed to have saved them).
// Every panel's save() answers true for a path it isn't holding, so asking all of
// them is the whole dispatch: there is no path→panel map to keep in sync.
bool EditorUI::saveAsset(AppContext& ctx, const std::string& assetPath)
{
	if (assetPath.empty()) return false;
	bool ok = true;
	ok = ScriptEditorPanel::save(assetPath)                          && ok;
	ok = CppClassEditorPanel::save(assetPath)                        && ok;
	ok = MaterialEditorPanel::save(ctx, assetPath)                   && ok;
	ok = UIEditorPanel::save(ctx, assetPath)                         && ok;
	ok = HorizonCodeClassPanel::save(ctx, assetPath)                 && ok;
	ok = InputAssetPanel::save(ctx, assetPath)                       && ok;
	ok = TypeAssetPanel::save(ctx, assetPath)                        && ok;
	ok = ThemeAssetPanel::save(ctx, assetPath)                       && ok;
	ok = ParticleGraphEditorPanel::save(ctx, assetPath)              && ok;
	ok = AnimatorStateMachineEditorPanel::save(ctx, assetPath)       && ok;
	// The panels are the authority on their own dirty flag; re-asking also catches
	// a save that reported success but left the state dirty.
	return ok && !tabHasUnsavedEdits(assetPath);
}

// The live documents behind an open tab, for collaboration's item-level sync.
// Same dispatch as saveAsset/reloadAssetTabFromDisk: ask every panel, the one
// holding the path answers, so there is still no path→panel map to keep in sync.
// Empty for tabs that have no syncable document (meshes, scripts, the viewport).
CollabDocSync::DocBindings EditorUI::collabDocsFor(const std::string& assetPath)
{
	if (assetPath.empty()) return {};
	if (auto d = MaterialEditorPanel::collabDocs(assetPath);            !d.empty()) return d;
	if (auto d = UIEditorPanel::collabDocs(assetPath);                  !d.empty()) return d;
	if (auto d = HorizonCodeClassPanel::collabDocs(assetPath);          !d.empty()) return d;
	if (auto d = ParticleGraphEditorPanel::collabDocs(assetPath);       !d.empty()) return d;
	if (auto d = AnimatorStateMachineEditorPanel::collabDocs(assetPath); !d.empty()) return d;
	return {};
}

// Every panel's cached state for one asset, dropped without asking whether it
// is dirty. The tab-close path (see forgetTabState in renderEditor) keeps a
// dirty panel deliberately, so reopening a tab restores what was typed into it.
// This is the other case: the asset does not exist any more. Its "unsaved
// edits" are edits to nothing, and leaving them would let Save All write the
// file back and quietly undo a deletion — which in a session is a deletion
// everybody already agreed to.
void EditorUI::discardPanelState(AppContext& ctx, const std::string& assetPath)
{
	if (assetPath.empty()) return;
	MaterialEditorPanel::releasePreviewAssets(ctx, assetPath);
	ScriptEditorPanel::forget(assetPath);
	CppClassEditorPanel::forget(assetPath);
	MaterialEditorPanel::forget(assetPath);
	UIEditorPanel::forget(assetPath);
	HorizonCodeClassPanel::forget(assetPath);
	InputAssetPanel::forget(assetPath);
	TypeAssetPanel::forget(assetPath);
	ThemeAssetPanel::forget(assetPath);
	ParticleGraphEditorPanel::forget(assetPath);
	AnimatorStateMachineEditorPanel::forget(assetPath);
	StaticMeshEditorPanel::forget(assetPath);
	SkeletalMeshEditorPanel::forget(assetPath);
	AudioEditorPanel::forget(assetPath);
}

// ── Leaving a project ────────────────────────────────────────────────────────
// Opening a project while one is already open, or closing one, ends a SESSION.
// What survived it did not just look stale, it was wrong: every tab still
// belonged to the old project's assets, the world still held the old scene, and
// the panels behind those tabs still cached its graphs — all of it addressed by
// paths and ids that mean something else, or nothing, in the project now open.
//
// Deliberately unconditional, and deliberately AFTER the unsaved-changes guard
// (both entry points below are reached through requestGuarded, which has already
// offered to save). Anything still dirty at this point is something the user
// chose to discard, and keeping it alive would let a later Save All write the
// old project's edits into a session that no longer has anything to do with it.
void EditorUI::endProjectSession(AppContext& ctx)
{
	// The collaboration session goes FIRST, and not merely because a session is
	// scoped to one project (its keys are that project's paths, its locks are on
	// that project's assets). The teardown below empties documents the session
	// is actively mirroring — the level script and the GameInstance graph are
	// diffed whenever we hold their lock, with no dirty flag to hide behind — so
	// leaving the session running through it would publish the whole teardown to
	// the peer as OUR deletion, and they would lose their level script.
	if (ctx.collab && ctx.collab->inSession()) ctx.collab->leave();

	// Both the open tabs and the panels whose tab was already closed while
	// dirty — the second set has no tab to walk to.
	std::vector<std::string> paths = unsavedAssetPaths();
	for (const auto& t : ctx.tabs)
		if (!t.assetPath.empty()) paths.push_back(t.assetPath);
	std::sort(paths.begin(), paths.end());
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	for (const std::string& p : paths) discardPanelState(ctx, p);

	// The two virtual HorizonCode tabs own no asset, so nothing above reached
	// their view contexts.
	LevelScriptPanel::forgetAllGraphContexts();

	// Every tab that belongs to a document goes — asset tabs AND the virtual
	// ones (Level Script, Game Instance, Settings), which are just as much the
	// old project's. The Viewport is the session's own and stays; it is also the
	// tab EditorApplication::restoreOpenTabs only re-creates when it has a saved
	// list to restore, so clearing outright would leave a project with no
	// remembered tabs showing no tabs at all.
	ctx.tabs.erase(std::remove_if(ctx.tabs.begin(), ctx.tabs.end(),
		[](const auto& t){ return !t.assetPath.empty(); }), ctx.tabs.end());
	if (ctx.tabs.empty()) ctx.tabs.push_back({ "Viewport", "", false, true });
	ctx.activeTab = 0;
	// The world last: it drops the scene, the selection and the undo history,
	// and it is what a panel above might still have been reading.
	if (ctx.newScene) ctx.newScene();
	ctx.contentRefreshPending = true;
}

void EditorUI::discardPanelStateUnder(AppContext& ctx, const std::string& folderPath)
{
	if (folderPath.empty()) return;
	// The separator matters: a bare prefix test also matches a SIBLING whose
	// name merely starts the same way, so deleting "Mat" would take the panel
	// state of everything under "Materials" with it.
	const std::string prefix = folderPath + "/";
	// unsavedAssetPaths, not ctx.tabs: an asset whose tab was closed while dirty
	// still has panel state and no tab, and that is precisely the one Save All
	// would write back into the folder that just went away.
	for (const std::string& p : unsavedAssetPaths())
	{
		if (p.rfind(prefix, 0) == 0) discardPanelState(ctx, p);
	}
}

// The read half of saveAsset's dispatch: ask every panel; whichever holds the
// path refreshes. Same "no path→panel map" argument as over there.
bool EditorUI::reloadAssetTabFromDisk(const std::string& assetPath)
{
	if (assetPath.empty()) return false;
	bool any = false;
	any = ScriptEditorPanel::reloadFromDisk(assetPath)                    || any;
	any = CppClassEditorPanel::reloadFromDisk(assetPath)                  || any;
	any = MaterialEditorPanel::reloadFromDisk(assetPath)                  || any;
	any = UIEditorPanel::reloadFromDisk(assetPath)                        || any;
	any = HorizonCodeClassPanel::reloadFromDisk(assetPath)                || any;
	any = InputAssetPanel::reloadFromDisk(assetPath)                      || any;
	any = TypeAssetPanel::reloadFromDisk(assetPath)                       || any;
	any = ThemeAssetPanel::reloadFromDisk(assetPath)                      || any;
	any = ParticleGraphEditorPanel::reloadFromDisk(assetPath)             || any;
	any = AnimatorStateMachineEditorPanel::reloadFromDisk(assetPath)      || any;
	return any;
}

// ─── Full Editor UI ───────────────────────────────────────────────────────────
void EditorUI::renderEditor(AppContext& ctx, float dt)
{
#ifdef HE_IMGUI_ENABLED
	// Runs every frame regardless of which tab/panel is active (before any early-out):
	// guarantees the RMB fly-look capture can never stay stuck once the button is released.
	ViewportPanel::enforceViewportLookCaptureInvariant(ctx.window ? ctx.window->GetNativeWindow() : nullptr);

	// ── Scene-file dialog helpers ──────────────────────────────────────────
	static PendingFileOp s_pendingFileOp = PendingFileOp::OpenProject;

	// ── Unsaved-changes guard state ─────────────────────────────────────────
	// A destructive action requested while the scene is dirty is stashed here and
	// the "Unsaved Changes" modal is raised; the action runs when the user picks
	// Save (after the save completes) or Don't Save. s_guardSaveThenAct bridges the
	// async Save-As dialog: it tells the file-result handler to run s_guardAction
	// once the freshly chosen path has been written.
	static GuardedAction s_guardAction      = GuardedAction::None;
	static std::string   s_guardArg;            // path payload for OpenScenePath
	static bool          s_openUnsavedModal = false;
	static bool          s_guardSaveThenAct = false;
	// The same stash for the step BEFORE that one: the confirmation a host gets
	// when a session-ending action would drop the peers connected to this editor.
	static GuardedAction s_collabAction     = GuardedAction::None;
	static std::string   s_collabArg;
	static bool          s_openCollabModal  = false;
	// Why the prompt's last save attempt failed ("" = none). A failed write must
	// never let the guarded action run — the modal stays open with the reason and
	// the entries that are still dirty.
	static std::string   s_guardSaveError;

	// One-shot request to programmatically select a top-level tab (set by a
	// Content-Browser double-click). -1 = none. The tab bar applies SetSelected for
	// exactly one frame and then clears it, so it never fights the user's own tab
	// clicks — applying SetSelected every frame would (and did) do both.
	static int           s_tabSelectRequest = -1;

	auto sceneDialogDir = [&]() -> std::string
	{
		if (!ctx.projectManager) return {};
		std::filesystem::path p = ctx.projectManager->currentProject().path;
		if (std::filesystem::is_regular_file(p)) p = p.parent_path();
		const std::filesystem::path content = p / "Content";
		return std::filesystem::exists(content) ? content.string() : p.string();
	};
	auto fileDialogCb = [](void* userdata, const char* const* filelist, int)
	{
		auto* b = static_cast<SDLDialogBridge*>(userdata);
		if (filelist && filelist[0]) { *b->pendingFileResult = filelist[0]; *b->pendingFileReady = true; }
	};
	// Every path the IMPORT dialog returned — that one is multi-select, and the
	// bridge's single-string slot cannot carry 140 textures. A function-local
	// static rather than a bridge field: a lambda may name a static without
	// capturing it, so the callback stays convertible to the plain function
	// pointer SDL takes. Written from SDL's dialog callback with no lock, exactly
	// like pendingFileResult next door.
	static std::vector<std::string> s_pendingImportPaths;
	auto importDialogCb = [](void* userdata, const char* const* filelist, int)
	{
		auto* b = static_cast<SDLDialogBridge*>(userdata);
		if (!filelist || !filelist[0]) return;   // null = error, empty = cancelled
		for (const char* const* p = filelist; *p; ++p)
			s_pendingImportPaths.emplace_back(*p);
		// The shared handler's "ready ⇒ a path was chosen" invariant still holds
		// for anything that only looks at pendingFileResult.
		*b->pendingFileResult = filelist[0];
		*b->pendingFileReady  = true;
	};
	auto triggerOpenScene = [&]()
	{
		s_pendingFileOp = PendingFileOp::OpenScene;
		SDL_DialogFileFilter filters[] = { { "Horizon Scene", "hescene" } };
		const std::string dir = sceneDialogDir();
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
	};
	auto triggerAddSceneAdditive = [&]()
	{
		s_pendingFileOp = PendingFileOp::AddSceneAdditive;
		SDL_DialogFileFilter filters[] = { { "Horizon Scene", "hescene" } };
		const std::string dir = sceneDialogDir();
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
	};
	auto triggerSaveSceneAs = [&]()
	{
		s_guardSaveThenAct = false; // a manual Save-As is not part of a guard flow
		s_pendingFileOp = PendingFileOp::SaveScene;
		SDL_DialogFileFilter filters[] = { { "Horizon Scene", "hescene" } };
		const std::string dir = sceneDialogDir();
		SDL_ShowSaveFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, dir.empty() ? nullptr : dir.c_str());
	};
	auto doSaveScene = [&]()
	{
		if (ctx.currentScenePath.empty()) triggerSaveSceneAs();
		else if (ctx.saveSceneToPath)     ctx.saveSceneToPath(ctx.currentScenePath);
	};
	// ── Save (Ctrl/Cmd+S): the tab you are LOOKING AT ──────────────────────
	// Saving the scene from inside a material graph is the wrong document: the
	// user's edits are in the tab in front of them. So Save writes the active
	// tab's asset, and only tabs with no asset of their own fall back to the
	// scene:
	//   • the Scene tab itself (empty assetPath),
	//   • Level Script — its graph is stored INSIDE the scene, so the scene save
	//     is its save (the guided tour says exactly that),
	//   • Game Instance / Preferences — already persisted on every edit
	//     (commitGameInstance / settings apply immediately); falling back keeps
	//     the key from being dead there.
	// View-only tabs (mesh, audio) reach saveAsset, which is a no-op for a path
	// no panel holds edits for.
	auto doSaveActiveTab = [&]()
	{
		const std::string path =
			(ctx.activeTab >= 0 && ctx.activeTab < static_cast<int>(ctx.tabs.size()))
				? ctx.tabs[ctx.activeTab].assetPath : std::string{};
		if (path.empty()
		    || path == LevelScriptPanel::kTabPath
		    || path == GameInstancePanel::kTabPath
		    || path == EditorSettingsPanel::kTabPath)
		{
			doSaveScene();
			return;
		}
		if (!saveAsset(ctx, path))
			HE_LOG_ERROR(Editor, "%s", ("Editor: save failed for " + path).c_str());
	};
	// ── Save All (Ctrl/Cmd+Shift+S): every unsaved asset, then the scene ────
	// unsavedAssetPaths() is panel-driven, so this also catches assets whose tab
	// the user already closed (the edits survive the close). The scene goes LAST
	// on purpose: an unnamed scene opens the async Save-As dialog, and that is
	// far less confusing at the end of the run than in the middle of it.
	auto doSaveAll = [&]()
	{
		for (const std::string& path : unsavedAssetPaths())
			if (!saveAsset(ctx, path))
				HE_LOG_ERROR(Editor, "%s", ("Editor: save failed for " + path).c_str());
		if (ctx.sceneDirty) doSaveScene();
	};
	auto triggerOpenProject = [&]()
	{
		ctx.hubOpenError.clear();
		s_pendingFileOp = PendingFileOp::OpenProject;
		SDL_DialogFileFilter filters[] = { { "HorizonEngine Project", "heproj" } };
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, nullptr, false);
	};
	// Where a menu-driven import should land: the folder the Content Browser is
	// SHOWING, as a path relative to the content root ("" = the root itself).
	// File ▸ Import Asset used to pass nothing here, so every import written from
	// the menu appeared at the content root and the user dragged it into place —
	// on every import, while the Content Browser's own right-click Import had
	// always got this right.
	//
	// Only the Content root is a destination. The Engine root is read-only ground
	// (shared engine defaults; writing there would shadow them project-wide from a
	// gesture that never said so) and Source holds the C++ tree, which has no
	// business receiving a .hasset — both fall back to the content root.
	auto importTargetDir = [&]() -> std::filesystem::path
	{
		if (!ctx.contentManager) return {};
		if (ContentBrowserPanel::browsedRootKind() != 0) return {};
		const std::string browsed = ContentBrowserPanel::browsedFolderPath();
		if (browsed.empty()) return {};
		std::error_code ec;
		std::filesystem::path rel =
			std::filesystem::relative(browsed, ctx.contentManager->contentRoot(), ec);
		if (ec || rel == ".") return {};
		// A browsed path that is not under the content root at all yields
		// "../…" — following it would write the import outside the project.
		if (!rel.empty() && *rel.begin() == "..") return {};
		return rel;
	};
	auto triggerImportAsset = [&]()
	{
		if (!ctx.projectLoaded || !ctx.contentManager) return;
		// Counted for the guided tour's "open Assets > Import Asset" step: the file
		// dialog is the OS's, so opening it is the only part the editor can observe
		// (and cancelling it must still count — the step teaches where it lives).
		++s_importDialogOpens;
		s_pendingFileOp = PendingFileOp::ImportAsset;
		// Cancelling never fires the callback, so last run's selection would still
		// be sitting here when the NEXT dialog returns.
		s_pendingImportPaths.clear();
		SDL_DialogFileFilter filters[] = {
			{ "All Supported Assets", "gltf;glb;png;jpg;jpeg;tga;bmp;hdr;wav;hmat;ttf;otf" },
			{ "3D Models",            "gltf;glb" },
			{ "Textures",             "png;jpg;jpeg;tga;bmp;hdr" },
			{ "Audio",                "wav" },
			{ "Materials",            "hmat" },
			{ "Fonts",                "ttf;otf" },
		};
		// Open where the browser is standing, so the dialog's own "recent folder"
		// is not the only thing that decides what the user is looking at.
		const std::filesystem::path root(ctx.contentManager->contentRoot());
		const std::string dir = root.empty() ? std::string{}
		                                     : (root / importTargetDir()).string();
		SDL_ShowOpenFileDialog(importDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 6,
			dir.empty() ? nullptr : dir.c_str(),
			/*allow_many=*/true);
	};
	auto doCloseProject = [&]()
	{
		// Same teardown as switching projects — closing one left its tabs, its
		// scene and its panel caches standing, so the next project opened in the
		// session inherited them.
		EditorUI::endProjectSession(ctx);
		ctx.projectManager->closeProject();
		ctx.globalState->setLastProjectPath("");
		ctx.globalState->writeConfig();
		ctx.projectLoaded = false;
		// endProjectSession asks for a content refresh, which is right when it runs
		// for a project SWITCH and pointless here: there is no project left to scan,
		// and the modal would sit over the Hub announcing that it is updating one.
		ctx.contentRefreshPending = false;
		ctx.contentRefreshDone    = false;
	};

	// ── Unsaved-changes guard ───────────────────────────────────────────────
	// runGuardedAction performs a stashed destructive action; requestGuarded gates
	// it behind the save-prompt when the scene is dirty (else runs it immediately).
	auto runGuardedAction = [&](GuardedAction a, const std::string& arg)
	{
		switch (a)
		{
		case GuardedAction::NewScene:          if (ctx.newScene) ctx.newScene();   break;
		case GuardedAction::OpenSceneDialog:   triggerOpenScene();                 break;
		case GuardedAction::OpenScenePath:     if (ctx.openScene) ctx.openScene(arg); break;
		case GuardedAction::OpenProjectDialog: triggerOpenProject();               break;
		case GuardedAction::CloseProject:      doCloseProject();                   break;
		case GuardedAction::Quit:              if (ctx.quit) ctx.quit();           break;
		case GuardedAction::None:                                                  break;
		}
	};
	// Unsaved edits in an ASSET TAB (material / UI widget / particle / state-machine
	// graph …) live only in the panel's per-path state, which dies with the process.
	// An action that ends the session therefore has to ask about them just like a
	// dirty scene does — without this, quitting with a freshly edited particle graph
	// dropped it silently. Scene actions (New/Open Scene) keep the tabs AND their
	// panel state alive, so they deliberately stay unguarded.
	auto endsSession = [](GuardedAction a)
	{
		return a == GuardedAction::Quit || a == GuardedAction::CloseProject ||
		       a == GuardedAction::OpenProjectDialog;
	};
	// (asset path, label to show) per unsaved asset. The path is what the prompt's
	// per-asset Save button writes through EditorUI::saveAsset.
	auto unsavedTabEntries = [&]() -> std::vector<std::pair<std::string, std::string>>
	{
		// Driven by the PANELS, not by ctx.tabs: closing a dirty tab keeps its
		// state but drops the tab, so walking ctx.tabs would silently omit exactly
		// the edits the user is most likely to have forgotten about. Open tabs
		// still get their friendly label; a closed one falls back to its path,
		// content-relative so a deep absolute path can't stretch the prompt.
		std::vector<std::pair<std::string, std::string>> out;
		for (const std::string& path : unsavedAssetPaths())
		{
			const auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
				[&](const AppContext::EditorTab& t) { return t.assetPath == path; });
			if (it != ctx.tabs.end() && !it->label.empty()) { out.emplace_back(path, it->label); continue; }
			const std::string rel = ctx.contentManager
				? ctx.contentManager->toContentRelativePath(path) : std::string{};
			out.emplace_back(path, rel.empty() ? path : rel);
		}
		return out;
	};
	// The unsaved-changes half, once anything upstream has agreed to proceed.
	auto requestGuardedInner = [&](GuardedAction a, const std::string& arg)
	{
		const bool tabsDirty = endsSession(a) && !unsavedTabEntries().empty();
		if (!ctx.sceneDirty && !tabsDirty) { runGuardedAction(a, arg); return; }
		s_guardAction      = a;
		s_guardArg         = arg;
		s_guardSaveThenAct = false;
		s_guardSaveError.clear();
		s_openUnsavedModal = true;
	};
	// Ending the session while HOSTING ends it for everyone: the peers are
	// connected to THIS editor, and a project switch takes the session down with
	// it (endProjectSession leaves before it touches anything, or the teardown
	// would replicate as our deletion). That is a consequence for other people,
	// so it gets its own confirmation — asked BEFORE the unsaved-changes prompt,
	// because a "no" here means none of the rest matters. A guest disconnects
	// only itself and is not asked.
	auto hostsCollab = [&](GuardedAction a)
	{
		return endsSession(a) && ctx.collab && ctx.collab->inSession() && ctx.collab->isHost();
	};
	auto requestGuarded = [&](GuardedAction a, const std::string& arg = std::string{})
	{
		if (hostsCollab(a))
		{
			s_collabAction     = a;
			s_collabArg        = arg;
			s_openCollabModal  = true;
			return;
		}
		requestGuardedInner(a, arg);
	};

	// An OS-level close (window X / Cmd+Q) that EditorApplication vetoed because of
	// unsaved changes surfaces here as a guarded Quit request.
	if (ctx.exitRequested)
	{
		ctx.exitRequested = false;
		requestGuarded(GuardedAction::Quit);
	}

	// ── Menu actions shared by the ImGui menu bar and the macOS native menu ────
	// Open (or focus) the Level Script / Game Instance as editor tabs.
	auto openVirtualTab = [&](const char* label, const char* path)
	{
		auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
			[&](const AppContext::EditorTab& t){ return t.assetPath == path; });
		if (it == ctx.tabs.end())
		{ ctx.tabs.push_back({ label, path, true, true }); ctx.activeTab = (int)ctx.tabs.size() - 1; }
		else ctx.activeTab = (int)std::distance(ctx.tabs.begin(), it);
		s_tabSelectRequest = ctx.activeTab;
	};
	// Back to the scene tab (the one with no asset behind it, normally index 0).
	auto openViewportTab = [&]()
	{
		const auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
			[](const AppContext::EditorTab& t){ return t.assetPath.empty(); });
		if (it == ctx.tabs.end()) return;
		ctx.activeTab      = static_cast<int>(std::distance(ctx.tabs.begin(), it));
		s_tabSelectRequest = ctx.activeTab;
	};
	// View-menu panel toggle. Ticking one on has to make it VISIBLE, and where
	// that is depends on how the user keeps it: a FLOATING panel draws over
	// whichever tab is open, so it is only pulled to the front (it may already
	// exist from an earlier session, buried under another floating window). A
	// DOCKED one lives in the scene layout and is hidden on every other tab, so
	// opening it from inside a material graph would otherwise tick the menu item
	// and show nothing at all — switch to the tab it lives on instead.
	auto togglePanelWindow = [&](bool& open, const char* title)
	{
		const bool opening = !open;
		open = opening;
		if (!opening) return;
		if (panelIsDockedInLayout(title)) openViewportTab();
		else                              ImGui::SetWindowFocus(title);
	};
	// Open request raised outside this function (e.g. the Source Control window's
	// "set up the remote in Preferences" pointer) — consumed here where the tab
	// list lives.
	if (EditorSettingsPanel::takeOpenRequest())
		openVirtualTab("Preferences", EditorSettingsPanel::kTabPath);
	auto openExportDialog = [&]() { ExportDialogPanel::open(ctx); };
	// ── Entity editing ───────────────────────────────────────────────────────
	// One predicate behind the Edit menu AND the keyboard, so a greyed-out menu
	// item and a shortcut that quietly does nothing can never disagree. The
	// gestures themselves re-check all of this (EditorApplication) — this is only
	// what decides whether the UI offers them.
	//
	// Not while PLAYING: the play session runs on a throwaway copy of the world
	// that is restored on stop, so an edit made there is work the user watches
	// disappear.
	auto canEditEntity = [&]() -> bool
	{
		return ctx.projectLoaded && !ctx.isPlaying && ctx.world
		    && ctx.selectedEntity != entt::null
		    && ctx.world->registry().valid(ctx.selectedEntity)
		    && !ctx.world->isBuiltin(ctx.selectedEntity);
	};
	// Paste needs no selection — it lands beside whatever is selected, or under
	// the world root when nothing is.
	auto canPasteEntity = [&]() -> bool
	{
		return ctx.projectLoaded && !ctx.isPlaying && ctx.world && ctx.entityClipboardFull;
	};
	// Window::SetFullscreen is write-only, so the current state is read back off
	// the SDL window rather than mirrored in a static that drifts the first time
	// the user goes fullscreen through the window manager instead of this menu.
	auto toggleFullscreen = [&]()
	{
		if (!ctx.window) return;
		SDL_Window* win = ctx.window->GetNativeWindow();
		if (!win) return;
		ctx.window->SetFullscreen((SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) == 0);
	};
	auto beginNewProject = [&]()
	{
		ctx.hubProjectName[0] = '\0';
		ctx.hubProjectDir[0]  = '\0';
		ctx.hubSelectedPreset = 0;
		ctx.hubSelectedLang   = 0; // HorizonCode
		ctx.hubCreateError.clear();
	};

	// On macOS the menu lives in the system menu bar (next to the Apple symbol)
	// like any Mac app, and the in-window ImGui menu row is dropped entirely.
	bool nativeMenu = false;
	bool openNewProjectPopup = false;
	bool openAboutPopup = false;
#ifdef __APPLE__
	MacMenuBar::install();   // idempotent; needs NSApp, which SDL created long ago
	nativeMenu = MacMenuBar::available();
	if (nativeMenu)
	{
		MacMenuBar::setProjectLoaded(ctx.projectLoaded);
		// …and hide the rows that only mean something in a game. Both calls are
		// per-frame and both no-op unless the answer changed.
		MacMenuBar::setAppProject(ctx.projectManager &&
		                          ctx.projectManager->currentProject().appProject);
		using MC = MacMenuBar::Cmd;
		// The ImGui menu row shows these as ticked MenuItems; the native bar has
		// to be told. Without it the menu most users see cannot say whether a
		// panel is already open — which matters more now that a docked one comes
		// back by itself and the menu is no longer how it got there.
		MacMenuBar::setToggleState(MC::ToggleProfiler,      s_showProfiler);
		MacMenuBar::setToggleState(MC::ToggleEnvironment,   s_showEnvironment);
		MacMenuBar::setToggleState(MC::ToggleCollab,        s_showCollab);
		MacMenuBar::setToggleState(MC::ToggleSourceControl, s_showSourceControl);
		MacMenuBar::setToggleState(MC::ToggleConsole,       s_showConsole);
		MacMenuBar::setToggleState(MC::ToggleGroundGrid,    ViewportPanel::groundGridEnabled());
		MacMenuBar::setToggleState(MC::OpenTutorial,        TutorialPanel::isOpen());
		for (MC c; (c = MacMenuBar::take()) != MC::None; )
		{
			switch (c)
			{
			case MC::NewProject:      beginNewProject(); openNewProjectPopup = true;         break;
			case MC::OpenProject:     requestGuarded(GuardedAction::OpenProjectDialog);      break;
			case MC::CloseProject:    requestGuarded(GuardedAction::CloseProject);           break;
			case MC::NewScene:        requestGuarded(GuardedAction::NewScene);               break;
			case MC::OpenScene:       requestGuarded(GuardedAction::OpenSceneDialog);        break;
			case MC::AddSceneAdditive:triggerAddSceneAdditive();                             break;
			case MC::Save:            doSaveActiveTab();                                     break;
			case MC::SaveAll:         doSaveAll();                                           break;
			case MC::SaveSceneAs:     triggerSaveSceneAs();                                  break;
			case MC::Quit:            requestGuarded(GuardedAction::Quit);                   break;
			case MC::Preferences:     openVirtualTab("Preferences", EditorSettingsPanel::kTabPath); break;
			// Same guards the footer buttons use — the native items carry no
			// enabled-state of their own, so an empty stack has to be checked here.
			case MC::Undo:
				if (ctx.undoSys && ctx.undoSys->canUndo() && ctx.undo) ctx.undo();
				break;
			case MC::Redo:
				if (ctx.undoSys && ctx.undoSys->canRedo() && ctx.redo) ctx.redo();
				break;
			case MC::ResetLayout:     s_resetLayoutRequested = true;                         break;
			case MC::ToggleProfiler:  togglePanelWindow(s_showProfiler, "Performance Profiler"); break;
			case MC::ToggleEnvironment: togglePanelWindow(s_showEnvironment, "Environment"); break;
			case MC::ToggleCollab:      togglePanelWindow(s_showCollab, "Collaboration");    break;
			case MC::ToggleSourceControl:
				togglePanelWindow(s_showSourceControl, "Source Control"); break;
			case MC::ToggleConsole:   togglePanelWindow(s_showConsole, "Console");            break;
			case MC::ToggleGroundGrid:
				ViewportPanel::setGroundGridEnabled(!ViewportPanel::groundGridEnabled());     break;
			case MC::OpenLevelScript:
				if (ctx.projectLoaded) openVirtualTab("Level Script", LevelScriptPanel::kTabPath);
				break;
			case MC::OpenGameInstance:
				if (ctx.projectLoaded) openVirtualTab("Game Instance", GameInstancePanel::kTabPath);
				break;
			case MC::ImportAsset:     triggerImportAsset();                                  break;
			case MC::RefreshAssets:   if (ctx.projectLoaded) ctx.contentRefreshPending = true; break;
			case MC::ExportProject:   if (ctx.projectLoaded) openExportDialog();             break;
			case MC::OpenTutorial:    TutorialPanel::open();                                 break;
			case MC::ReportIssue:     ReportIssueDialog::open();                             break;
			case MC::Documentation:       DocsPanel::open();                                 break;
			case MC::SearchDocumentation: DocsPanel::openSearch("");                         break;
			case MC::DocumentationOnline: SDL_OpenURL(kDocsUrl);                             break;
#ifdef HE_HAVE_LIBSSH2
			case MC::PublishEngineContent:       EngineContentPublishDialog::open(ctx);                break;
			case MC::RebuildManifestFromServer:  EngineContentPublishDialog::openRebuildFromServer(ctx); break;
#endif
			default: break;
			}
		}
	}
#endif

	if (!nativeMenu)
	{
	ImGui::PushFont(ctx.fontSubheading);
	ImGui::BeginMainMenuBar();
	if (ImGui::BeginMenu("File"))
	{
		// Every row below is looked up as "File/<its label>" — one scope, and
		// the menu explains itself (see EditorWidgets::menuItem).
		HE::Ed::Help::Scope helpScope("File");
		if (EditorWidgets::menuItem("New Project", "Ctrl+N"))
		{
			beginNewProject();
			openNewProjectPopup = true;
		}
        if (EditorWidgets::menuItem("Open Project", "Ctrl+O"))
            requestGuarded(GuardedAction::OpenProjectDialog);
		if (EditorWidgets::menuItem("Close Project", "Ctrl+W"))
			requestGuarded(GuardedAction::CloseProject);
        ImGui::Separator();
        // Scenes are a game's unit of content. An application has none — its
        // interface comes up from the GameInstance — so the four scene rows are
        // hidden rather than offered and then refused (docs/he-apps-plan.md E2).
        const bool appProj = ctx.projectManager &&
                             ctx.projectManager->currentProject().appProject;
        if (!appProj)
        {
            if (EditorWidgets::menuItem("New Scene"))            requestGuarded(GuardedAction::NewScene);
            if (EditorWidgets::menuItem("Open Scene..."))        requestGuarded(GuardedAction::OpenSceneDialog);
            if (EditorWidgets::menuItem("Add Scene Additive...")) triggerAddSceneAdditive();
        }
        // Keep these three in step with MacMenuBar.mm's File block — a Mac user
        // never sees this row (see MacMenuBar.h).
        if (EditorWidgets::menuItem("Save", "Ctrl+S"))                    doSaveActiveTab();
        if (EditorWidgets::menuItem("Save All", "Ctrl+Shift+S"))          doSaveAll();
        if (!appProj)
            if (EditorWidgets::menuItem("Save Scene As...", "Ctrl+Alt+S")) triggerSaveSceneAs();
        ImGui::Separator();
        if (EditorWidgets::menuItem("Exit", "Alt+F4"))
            requestGuarded(GuardedAction::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        // Every row below is looked up as "Edit/<its label>" — one scope, and
        // the menu explains itself (see EditorWidgets::menuItem).
        HE::Ed::Help::Scope helpScope("Edit");
        // In a session, undo/redo operate on YOUR OWN changes as inverse
        // operations that get republished — the snapshot stack would restore a
        // whole world and revert everyone else's work with it (CollabUndo.h).
        const bool collabUndoActive = ctx.collab && ctx.collab->inSession() && ctx.collabUndo;
        if (collabUndoActive)
        {
            const std::string uLabel = ctx.collabUndo->canUndo()
                ? ctx.collabUndo->undoLabel() : std::string("Undo");
            const std::string rLabel = ctx.collabUndo->canRedo()
                ? ctx.collabUndo->redoLabel() : std::string("Redo");

            if (EditorWidgets::menuItem(uLabel.c_str(), "Ctrl+Z", false,
                                ctx.collabUndo->canUndo()))
                ctx.collabUndo->undo();
            if (EditorWidgets::menuItem(rLabel.c_str(), "Ctrl+Y", false,
                                ctx.collabUndo->canRedo()))
                ctx.collabUndo->redo();

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("While collaborating, undo applies only to your own changes.");
        }
        else
        {
            // The same stack the footer's two buttons and Ctrl+Z drive — there is
            // one undo history in the editor, and this is a second door onto it.
            const bool canUndo = ctx.undoSys && ctx.undoSys->canUndo();
            const bool canRedo = ctx.undoSys && ctx.undoSys->canRedo();
            if (EditorWidgets::menuItem("Undo", "Ctrl+Z", false, canUndo) && ctx.undo) ctx.undo();
            if (EditorWidgets::menuItem("Redo", "Ctrl+Y", false, canRedo) && ctx.redo) ctx.redo();
        }
        ImGui::Separator();
        // Cut/Copy/Paste act on the SELECTED ENTITY, not on text: an editor's Edit
        // menu is the scene's, and the text fields inside panels handle their own
        // clipboard through ImGui.
        {
            const bool canEdit  = canEditEntity();
            const bool canPaste = canPasteEntity();
            if (EditorWidgets::menuItem("Cut",   "Ctrl+X", false, canEdit)  && ctx.cutEntity)  ctx.cutEntity();
            if (EditorWidgets::menuItem("Copy",  "Ctrl+C", false, canEdit)  && ctx.copyEntity) ctx.copyEntity();
            if (EditorWidgets::menuItem("Paste", "Ctrl+V", false, canPaste) && ctx.pasteEntity) ctx.pasteEntity();
            if (EditorWidgets::menuItem("Duplicate", "Ctrl+D", false, canEdit) && ctx.duplicateEntity)
                ctx.duplicateEntity();
            if (EditorWidgets::menuItem("Delete", "Del", false, canEdit) && ctx.deleteEntity)
                ctx.deleteEntity();
        }
        ImGui::Separator();
		if (EditorWidgets::menuItem("Preferences", "Ctrl+,"))
			openVirtualTab("Preferences", EditorSettingsPanel::kTabPath);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        // Every row below is looked up as "View/<its label>" — one scope, and
        // the menu explains itself (see EditorWidgets::menuItem).
        HE::Ed::Help::Scope helpScope("View");
        if (EditorWidgets::menuItem("Toggle Fullscreen", "F11")) toggleFullscreen();
        if (EditorWidgets::menuItem("Reset Layout")) { s_resetLayoutRequested = true; }
        if (EditorWidgets::menuItem("Performance Profiler", nullptr, s_showProfiler))
            togglePanelWindow(s_showProfiler, "Performance Profiler");
        if (EditorWidgets::menuItem("Environment", nullptr, s_showEnvironment))
            togglePanelWindow(s_showEnvironment, "Environment");
        if (EditorWidgets::menuItem("Collaboration", nullptr, s_showCollab))
            togglePanelWindow(s_showCollab, "Collaboration");
        if (EditorWidgets::menuItem("Source Control", nullptr, s_showSourceControl))
            togglePanelWindow(s_showSourceControl, "Source Control");
        if (EditorWidgets::menuItem("Console", "Ctrl+`", s_showConsole))
            togglePanelWindow(s_showConsole, "Console");
        // Also in the viewport toolbar's options popup. It belongs in both: the
        // toolbar is where you reach for it while working, this menu is where you
        // look for it the first time. Both are gone in an application: there is
        // no ground to grid and no level to script — the Game Instance below is
        // the one an app really does own, and stays.
        const bool appProjView = ctx.projectManager &&
                                 ctx.projectManager->currentProject().appProject;
        if (!appProjView)
        {
            if (EditorWidgets::menuItem("Ground Grid", nullptr, ViewportPanel::groundGridEnabled(),
                                ctx.projectLoaded))
                ViewportPanel::setGroundGridEnabled(!ViewportPanel::groundGridEnabled());
            if (EditorWidgets::menuItem("Level Script", nullptr, false, ctx.projectLoaded))
                openVirtualTab("Level Script", LevelScriptPanel::kTabPath);
        }
        if (EditorWidgets::menuItem("Game Instance", nullptr, false, ctx.projectLoaded))
            openVirtualTab("Game Instance", GameInstancePanel::kTabPath);
        ImGui::EndMenu();
    }
	if (ImGui::BeginMenu("Assets"))
	{
		// Every row below is looked up as "Assets/<its label>" — one scope, and
		// the menu explains itself (see EditorWidgets::menuItem).
		HE::Ed::Help::Scope helpScope("Assets");
		const bool doImport = EditorWidgets::menuItem("Import Asset...", nullptr, false,
		                                      ctx.projectLoaded);
		EditorWidgets::helpForKey("content.import");
		if (doImport) triggerImportAsset();
		// The same rescan an import raises — the Content Browser picks the flag up
		// and re-walks the content tree, which is what makes a file dropped in
		// from the Finder appear.
		if (EditorWidgets::menuItem("Refresh Assets", nullptr, false, ctx.projectLoaded))
			ctx.contentRefreshPending = true;
#ifdef HE_HAVE_LIBSSH2
		// Dev-only: publishes the local EngineContent library to the SFTP
		// server other Editors download it from on demand (see HE_ContentSync).
		// Same gate ContentManager uses to decide whether writes to "Engine/..."
		// target the shared default instead of a per-project override.
		if (ContentManager::isEngineContentDevMode())
		{
			ImGui::Separator();
			if (EditorWidgets::menuItem("Publish Engine Content to Server..."))
				EngineContentPublishDialog::open(ctx);
			// For a server that already has content this tool never uploaded
			// (e.g. a pre-existing archive of raw source audio) — lists the
			// server directly instead of assuming local EngineContent is the
			// source of truth. See EngineContentPublish.h.
			if (EditorWidgets::menuItem("Rebuild Manifest from Server..."))
				EngineContentPublishDialog::openRebuildFromServer(ctx);
		}
#endif
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Build", ctx.projectLoaded))
	{
		HE::Ed::Help::Scope helpScope("Build");
		if (EditorWidgets::menuItem("Export Project..."))
			openExportDialog();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help"))
	{
		// Every row below is looked up as "Help/<its label>" — one scope, and
		// the menu explains itself (see EditorWidgets::menuItem).
		HE::Ed::Help::Scope helpScope("Help");
		// The manual first, and in the editor: it is the answer to most of what
		// brings anyone into this menu, and the version that opens a browser is
		// the one that loses whatever the user was in the middle of.
		if (EditorWidgets::menuItem("Documentation", "F1", DocsPanel::isOpen()))
			DocsPanel::open();
		if (EditorWidgets::menuItem("Search the Documentation...", "Ctrl+F1"))
			DocsPanel::openSearch("");
		if (EditorWidgets::menuItem("Documentation (Website)")) SDL_OpenURL(kDocsUrl);
		ImGui::Separator();
		if (EditorWidgets::menuItem("Interactive Tutorial", nullptr, TutorialPanel::isOpen()))
			TutorialPanel::open();
		ImGui::Separator();
		if (EditorWidgets::menuItem("Report Issue...", nullptr, ReportIssueDialog::isOpen()))
			ReportIssueDialog::open();
		ImGui::Separator();
		if (EditorWidgets::menuItem("About")) openAboutPopup = true;
		ImGui::EndMenu();
	}
    ImGui::EndMainMenuBar();
    ImGui::PopFont();
	}

    if (openNewProjectPopup)
        ImGui::OpenPopup("##NewProjectPopup");

    // ── About ───────────────────────────────────────────────────────────────
    // Windows and Linux only in practice: macOS drops the ImGui menu row and
    // answers Help ▸ About from the App menu's standard panel (MacMenuBar.mm),
    // which carries the same version string from the bundle's Info.plist.
    if (openAboutPopup)
        ImGui::OpenPopup("About Horizon Engine##about");
    // Only while the popup is actually up: pinDialogToEditorWindow sets ImGui's
    // NEXT-window position, and an unconditional call would hand it to whatever
    // window opens after this one instead.
    if (ImGui::IsPopupOpen("About Horizon Engine##about"))
        EditorWidgets::pinDialogToEditorWindow();
    if (ImGui::BeginPopupModal("About Horizon Engine##about", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
        ImGui::TextUnformatted("Horizon Engine");
        if (ctx.fontSubheading) ImGui::PopFont();
        ImGui::TextDisabled("Version %s", HE_VERSION_FULL);
        ImGui::Spacing();
        ImGui::TextUnformatted("Editor and runtime, built by Horizon Creations.");
        ImGui::Spacing();
        if (ImGui::SmallButton("Documentation")) { DocsPanel::open(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Website"))       SDL_OpenURL("https://horizoncreations.dev");
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── Export Project modal ────────────────────────────────────────────────
    ExportDialogPanel::render(ctx);

    // ── Hosting-a-session confirmation ──────────────────────────────────────
    // Ahead of the unsaved-changes prompt, because it is the question with the
    // wider consequence: closing or switching the project while hosting takes
    // the session down for every peer connected to this editor. Confirm hands
    // the action on to the ordinary guard, which may still ask about unsaved
    // work; Cancel drops it entirely and the session keeps running.
    if (s_openCollabModal)
    {
        ImGui::OpenPopup("Collaboration Session##host");
        s_openCollabModal = false;
    }
    {
        EditorWidgets::pinDialogToEditorWindow(ImVec2(420.0f, 0.0f));
        if (ImGui::BeginPopupModal("Collaboration Session##host", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            const GuardedAction action = s_collabAction;
            const std::string   arg    = s_collabArg;
            {
                EditorWidgets::WrapText wrap;
                // The participant list includes this editor, so the count of
                // OTHERS is one less — and never negative, because a host that
                // nobody has joined yet still appears in it.
                const int peers = ctx.collab
                    ? std::max(0, static_cast<int>(ctx.collab->participants().size()) - 1) : 0;
                ImGui::TextUnformatted("You are hosting a collaboration session.");
                ImGui::Spacing();
                if (peers > 0)
                    ImGui::Text("%d other %s connected to this editor.",
                                peers, peers == 1 ? "person is" : "people are");
                ImGui::TextUnformatted(
                    action == GuardedAction::Quit
                        ? "Quitting ends the session for everyone."
                        : "Leaving this project ends the session for everyone. "
                          "A session belongs to one project, so it cannot follow "
                          "you into the next.");
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const char* runLabel = action == GuardedAction::Quit      ? "Quit Anyway"
                                 : action == GuardedAction::CloseProject ? "Close Anyway"
                                                                        : "Switch Anyway";
            if (EditorWidgets::dangerButton(runLabel, ImVec2(130, 0)))
            {
                s_collabAction = GuardedAction::None;
                ImGui::CloseCurrentPopup();
                requestGuardedInner(action, arg);
            }
            ImGui::SameLine();
            if (EditorWidgets::cancelButton("Cancel", ImVec2(110, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                s_collabAction = GuardedAction::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ── Unsaved-changes modal ───────────────────────────────────────────────
    // Raised by requestGuarded() when a scene-discarding action is attempted with a
    // dirty scene, or when a session-ending one (Quit / Close Project / Open Project)
    // would take unsaved editor tabs with it. Save → write the scene (Save-As if
    // untitled) then run the action; Don't Save → run it straight away; Cancel →
    // abandon it.
    if (s_openUnsavedModal)
    {
        ImGui::OpenPopup("Unsaved Changes##scene");
        s_openUnsavedModal = false;
    }
    {
        // Auto-height, but a stable minimum width — the asset rows below would
        // otherwise make the popup jump around as entries are saved away. The
        // pin also caps the height: a long asset list must not grow the popup
        // past the editor window (see EditorWidgets.h).
        EditorWidgets::pinDialogToEditorWindow(ImVec2(420.0f, 0.0f));
        if (ImGui::BeginPopupModal("Unsaved Changes##scene", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            // Snapshot the stashed action — the buttons may clear it.
            const GuardedAction action = s_guardAction;
            const std::string   arg    = s_guardArg;

            // Editor tabs are only at risk when the action ends the session; a scene
            // switch keeps them and their unsaved edits (see requestGuarded).
            // Recomputed every frame, so anything saved from here drops off the list.
            const std::vector<std::pair<std::string, std::string>> dirtyTabs =
                endsSession(action) ? unsavedTabEntries()
                                    : std::vector<std::pair<std::string, std::string>>{};

            // The per-asset Save buttons below can empty the list while the prompt is
            // open — then there is nothing left to warn about, only the action to run.
            const bool anythingDirty = ctx.sceneDirty || !dirtyTabs.empty();

            const std::string sceneName = ctx.currentScenePath.empty()
                ? std::string("Untitled")
                : std::filesystem::path(ctx.currentScenePath).stem().string();
            {
                // The scene name is whatever the author called their file, so this
                // is the one line in the dialog with no length bound. Wrapped at an
                // absolute column rather than the window edge, and in its own block
                // so the pop lands while this popup is still current — EndPopup()
                // sits at the foot of this same body.
                EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 30.0f);
                if (ctx.sceneDirty)
                    ImGui::Text("Save changes to \"%s\" before continuing?", sceneName.c_str());
                else if (!dirtyTabs.empty())
                    ImGui::Text("%d editor tab(s) have unsaved changes.",
                                static_cast<int>(dirtyTabs.size()));
                else
                    ImGui::TextUnformatted("Everything is saved.");
            }
            if (!dirtyTabs.empty())
            {
                ImGui::Spacing();
                if (ctx.sceneDirty) ImGui::TextDisabled("These assets are unsaved too:");
                // Each asset gets its own Save button right here: the prompt writes it
                // through its panel (EditorUI::saveAsset), so the user never has to go
                // back into the tab — which for a tab they already CLOSED would have
                // meant reopening it first just to find the Save button.
                const int   rows   = static_cast<int>(dirtyTabs.size());
                const bool  scroll = rows > 8;
                if (scroll)
                    ImGui::BeginChild("##dirtyAssets",
                        ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() * 8.0f),
                        ImGuiChildFlags_Borders);
                for (const auto& [path, label] : dirtyTabs)
                {
                    ImGui::PushID(path.c_str());
                    if (ImGui::SmallButton("Save"))
                    {
                        if (EditorUI::saveAsset(ctx, path)) s_guardSaveError.clear();
                        else s_guardSaveError = "Could not save \"" + label + "\".";
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(label.c_str());
                    if (ImGui::IsItemHovered())
                    {
                        // The row shows the asset's short label; the tooltip is
                        // where the user finds out WHICH of two same-named assets
                        // this is — so it holds the absolute path, the one string
                        // here that is reliably wider than the screen. Spelled out
                        // as BeginTooltip/EndTooltip because SetTooltip has nowhere
                        // to put the wrap guard. The column is absolute: a tooltip
                        // is an auto-resizing window, so wrapping at its own edge
                        // would make it narrower every frame until only the last
                        // path component is left. The extra brace pair is the
                        // guard's scope: EndTooltip() hands ImGui's current window
                        // back to the modal underneath, and popping the wrap
                        // position after that would pop the modal's stack.
                        if (ImGui::BeginTooltip())   // false = nothing to end
                        {
                            {
                                EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
                                ImGui::TextUnformatted(path.c_str());
                            }
                            ImGui::EndTooltip();
                        }
                    }
                    ImGui::PopID();
                }
                if (scroll) ImGui::EndChild();
            }
            ImGui::Spacing();
            if (anythingDirty)
                ImGui::TextDisabled("Your unsaved changes will be lost otherwise.");
            if (!s_guardSaveError.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", s_guardSaveError.c_str());
                ImGui::TextDisabled("Nothing was closed or discarded.");
            }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            // Primary button: write EVERYTHING that's dirty (scene + every asset tab)
            // and then run the action. Offered whenever there is anything to save —
            // an unsaved asset tab counts even with a clean scene.
            if (anythingDirty)
            {
                const bool both = ctx.sceneDirty && !dirtyTabs.empty();
                if (ImGui::Button(both ? "Save All" : "Save", ImVec2(110, 0)))
                {
                    // Assets first: those writes are synchronous, so a failure can
                    // still abort the flow before the scene's async Save-As dialog is
                    // in flight (and before anything gets discarded).
                    int         failed = 0;
                    std::string firstFailed;
                    for (const auto& [path, label] : dirtyTabs)
                        if (!EditorUI::saveAsset(ctx, path))
                        {
                            if (failed++ == 0) firstFailed = label;
                        }
                    if (failed > 0)
                    {
                        s_guardSaveError = failed == 1
                            ? "Could not save \"" + firstFailed + "\"."
                            : std::to_string(failed) + " assets could not be saved.";
                    }
                    else
                    {
                        s_guardSaveError.clear();
                        const bool hadPath = !ctx.currentScenePath.empty();
                        if (ctx.sceneDirty)
                            doSaveScene(); // synchronous if a path exists, else async Save-As
                        if (!ctx.sceneDirty || hadPath)
                        {
                            runGuardedAction(action, arg);
                            s_guardAction = GuardedAction::None;
                        }
                        else
                        {
                            // Save-As dialog is in flight; the file-result handler runs
                            // the action once a path has been chosen and written.
                            s_guardSaveThenAct = true;
                        }
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
            }
            // Same button either way: run the action. It only DISCARDS anything while
            // something is still dirty, so the label follows that.
            // "Continue" commits nothing and stays neutral; the two labels that
            // DISCARD work are red, because that is the entire difference
            // between them and the Save button next door.
            const char* runLabel = !anythingDirty ? "Continue"
                                 : (ctx.sceneDirty ? "Don't Save" : "Discard");
            const bool runIt = anythingDirty
                ? EditorWidgets::dangerButton(runLabel, ImVec2(110, 0))
                : ImGui::Button(runLabel, ImVec2(110, 0));
            if (runIt)
            {
                runGuardedAction(action, arg);
                s_guardAction = GuardedAction::None;
                s_guardSaveError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (EditorWidgets::cancelButton("Cancel", ImVec2(110, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                s_guardAction      = GuardedAction::None;
                s_guardSaveThenAct = false;
                s_guardSaveError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if (ctx.pendingFileReady)
    {
        ctx.pendingFileReady = false;
        std::string chosen = ctx.pendingFileResult;
        ctx.pendingFileResult.clear();

        if (s_pendingFileOp == PendingFileOp::OpenScene)
        {
            if (!chosen.empty() && ctx.openScene) ctx.openScene(chosen);
        }
        else if (s_pendingFileOp == PendingFileOp::AddSceneAdditive)
        {
            if (!chosen.empty() && ctx.openSceneAdditive) ctx.openSceneAdditive(chosen);
        }
        else if (s_pendingFileOp == PendingFileOp::SaveScene)
        {
            if (!chosen.empty() && ctx.saveSceneToPath)
            {
                std::filesystem::path p(chosen);
                if (p.extension() != ".hescene") p += ".hescene";
                ctx.saveSceneToPath(p.string());
                // If this Save-As was the guard's "Save" choice, run the deferred
                // action now that the scene is on disk.
                if (s_guardSaveThenAct)
                {
                    runGuardedAction(s_guardAction, s_guardArg);
                    s_guardAction = GuardedAction::None;
                }
            }
            s_guardSaveThenAct = false;
        }
        else if (s_pendingFileOp == PendingFileOp::ImportAsset)
        {
            if (!s_pendingImportPaths.empty() && ctx.contentManager)
            {
                // Extension → importer routing lives in Importer::importSource, not
                // here: this handler and the Content Browser's right-click Import
                // each kept their own copy of the list, and they had already drifted
                // (fonts imported from one and not the other).
                const std::filesystem::path root(ctx.contentManager->contentRoot());
                const std::filesystem::path relDir = importTargetDir();

                size_t imported = 0;
                for (const std::string& src : s_pendingImportPaths)
                {
                    if (Importer::importSource(src, root, relDir)) ++imported;
                    else HE_LOG_ERROR(Editor, "%s",
                        ("Editor: import failed for " + src).c_str());
                }
                // One line for the whole batch, one refresh at the end: a hundred
                // textures must not mean a hundred progress modals or a hundred
                // rescans of the content tree.
                HE_LOG_INFO(Editor, "%s",
                    ("Editor: imported " + std::to_string(imported) + " of "
                     + std::to_string(s_pendingImportPaths.size()) + " file(s) into "
                     + (relDir.empty() ? std::string("the content root")
                                       : relDir.generic_string())).c_str());
                ctx.contentRefreshPending = true;
            }
            s_pendingImportPaths.clear();
        }
        else // OpenProject
        {
            // End the old session BEFORE loading the new one. The other order
            // looks safer — keep everything until the new project is known to
            // load — but it means tearing the old project's tabs and panels down
            // against a ContentManager that already points somewhere else, so
            // every path they hold resolves to nothing or to the wrong asset.
            // A failed load then simply leaves no project open, which is a state
            // the editor already has (the hub) and says what happened.
            const bool switching = ctx.projectLoaded;
            if (switching) EditorUI::endProjectSession(ctx);
            if (ctx.projectManager->loadProject(chosen))
            {
                ctx.globalState->addKnownProject(chosen);
                ctx.globalState->writeConfig();
                ctx.contentRefreshPending = true;
                ctx.projectLoaded = true;
            }
            else
            {
                if (switching) ctx.projectLoaded = false;   // the old one is gone
                ctx.hubOpenError = "Failed to load project file.";
                ImGui::OpenPopup("##EditorOpenError");
            }
        }
        s_pendingFileOp = PendingFileOp::OpenProject; // reset to default
    }

    // ── Save shortcuts: Cmd/Ctrl+S = active tab, Shift+Cmd/Ctrl+S = save all ──
    // (macOS never gets here for these two: the native menu's key equivalents
    // swallow the keystroke before SDL sees it — the MacMenuBar dispatch above
    // runs the SAME two lambdas.)
    {
        const ImGuiIO& kio = ImGui::GetIO();
        const bool mod = kio.KeyCtrl || kio.KeySuper;
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            if (kio.KeyAlt)        triggerSaveSceneAs();   // Save Scene As…
            else if (kio.KeyShift) doSaveAll();
            else                   doSaveActiveTab();
        }
        // Ctrl/Cmd+, opens the Preferences tab (matches the Edit menu shortcut label).
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Comma, false))
            openVirtualTab("Preferences", EditorSettingsPanel::kTabPath);
        // Ctrl/Cmd+` toggles the Console — the console key every engine uses, but
        // NOT the bare one. This block runs before the asset tabs are dispatched,
        // and the code editor is ImGuiColorTextEdit: it captures the keyboard by
        // writing io.WantTextInput itself, which ImGui recomputes at the next
        // NewFrame, so the guard above cannot see it. A modifier-free ` would
        // therefore open the console every time somebody typed a backtick into a
        // Lua string.
        //
        // It also carries more weight than the other two shortcuts here: on macOS
        // the ImGui menu row above does not exist (the native bar replaces it, and
        // an item there needs MacMenuBar), so until the Console has an entry in
        // that bar this is the only way a Mac user reaches the panel.
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
            togglePanelWindow(s_showConsole, "Console");
        // The two shortcuts the menu has always advertised and never had. F11
        // carries no modifier, so WantTextInput is the whole guard — a function
        // key cannot be typed into a field, but a code editor holding the
        // keyboard should still not have the window change shape under it.
        // (⌘O never gets here on macOS — the native bar's key equivalent takes
        // it first and dispatches the same action. F11 stays wired everywhere,
        // though a Mac usually claims that key for the system before we see it;
        // the native View menu's ⌃⌘F is the reliable route there.)
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_O, false))
            requestGuarded(GuardedAction::OpenProjectDialog);
        if (!kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F11, false))
            toggleFullscreen();
    }

    if (!ctx.hubOpenError.empty())
    {
        EditorWidgets::pinDialogToEditorWindow();
        if (ImGui::BeginPopupModal("##EditorOpenError", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            // hubOpenError is whatever went wrong opening a project, and it
            // normally names the file it failed on — an absolute path. Two things
            // follow. It has to wrap, or the dialog is one line as wide as the path
            // and the sentence ends off-screen. And the column has to be absolute:
            // this popup sizes itself to its content, so wrapping at its own right
            // edge is a feedback loop — every frame it fits the wrapped text and
            // every frame the text wraps tighter, down to ImGui's minimum.
            //
            // The guard lives in its own scope because EndPopup() below hands
            // ImGui's current window back to whatever is under this popup — here
            // the implicit debug window — and popping the wrap position after that
            // pops a stack that was never pushed.
            {
                EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 30.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextUnformatted(ctx.hubOpenError.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
                if (EditorWidgets::primaryButton("OK", ImVec2(120, 0)))
                {
                    ctx.hubOpenError.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    // ── New Project Popup ─────────────────────────────────────────────────────
    {
        // Templates: ProjectHubPanel::kPresetNames/kPresetDescs — shared with the
        // Hub's own create form so the two lists cannot drift apart.
        // Index order MUST match ProjectScriptLanguage (HorizonCode, Lua, Python, Cpp).
        static const std::array<const char*, 4> kLangNames = {
            "HorizonCode (Visual Scripting)", "Lua", "Python", "C++",
        };
        static const std::array<const char*, 4> kLangDesc = {
            "Node graphs; compiles to native C++ on export.",
            "Lightweight text scripting (default script backend).",
            "CPython scripting (needs a Python install on dev machines).",
            "Native GameLogic library, built with your own toolchain.",
        };

        ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Always);
        EditorWidgets::pinDialogToEditorWindow();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8.0f, 8.0f));
        if (ImGui::BeginPopupModal("##NewProjectPopup", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::PopStyleVar(2);
            // This form is the Project Hub's create form in a modal — the same
            // controls, so the same help scope. Without it the rows here are
            // filed under whatever scope happens to precede them in this file,
            // which is the Help MENU.
            HE::Ed::Help::Scope helpScope("Project Hub");

            if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
            ImGui::TextUnformatted("New Project");
            if (ctx.fontSubheading) ImGui::PopFont();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Project Name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##npName", ctx.hubProjectName, ctx.hubProjectNameSize);

            ImGui::Spacing();
            ImGui::Text("Project Directory");
            ImGui::SetNextItemWidth(-70.0f);
            ImGui::InputText("##npDir", ctx.hubProjectDir, ctx.hubProjectDirSize);
            ImGui::SameLine();
            if (ImGui::Button("Browse##npBrowse", ImVec2(62.0f, 0)))
            {
#ifdef _WIN32
                IFileOpenDialog* pDlg = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg))))
                {
                    DWORD dwOpts = 0;
                    pDlg->GetOptions(&dwOpts);
                    pDlg->SetOptions(dwOpts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                    HWND hwnd = nullptr;
                    if (ctx.window)
                        hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                            SDL_GetWindowProperties(ctx.window->GetNativeWindow()),
                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
                    if (SUCCEEDED(pDlg->Show(hwnd)))
                    {
                        IShellItem* pItem = nullptr;
                        if (SUCCEEDED(pDlg->GetResult(&pItem)))
                        {
                            PWSTR pPath = nullptr;
                            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)))
                            {
                                int len = WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
                                    nullptr, 0, nullptr, nullptr);
                                if (len > 0 && len <= ctx.hubProjectDirSize)
                                    WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
                                        ctx.hubProjectDir, ctx.hubProjectDirSize, nullptr, nullptr);
                                CoTaskMemFree(pPath);
                            }
                            pItem->Release();
                        }
                    }
                    pDlg->Release();
                }
#else
                SDL_ShowOpenFolderDialog(
                    [](void* userdata, const char* const* filelist, int)
                    {
                        auto* b = static_cast<SDLDialogBridge*>(userdata);
                        if (filelist && filelist[0])
                        {
                            *b->pendingDirResult = filelist[0];
                            *b->pendingDirReady  = true;
                        }
                    },
                    ctx.dialogBridge,
                    ctx.window ? ctx.window->GetNativeWindow() : nullptr,
                    nullptr, false);
#endif
            }

            if (ctx.pendingDirReady)
            {
                strncpy(ctx.hubProjectDir, ctx.pendingDirResult.c_str(), ctx.hubProjectDirSize - 1);
                ctx.hubProjectDir[ctx.hubProjectDirSize - 1] = '\0';
                ctx.pendingDirReady  = false;
                ctx.pendingDirResult.clear();
            }

            ImGui::Spacing();
            ImGui::Text("Template");
            ImGui::SetNextItemWidth(-1);
            // All of them, not the first five — see the Project Hub's twin.
            ImGui::ListBox("##npPresets", &ctx.hubSelectedPreset,
                ProjectHubPanel::kPresetNames, ProjectHubPanel::kPresetCount,
                ProjectHubPanel::kPresetCount);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", ProjectHubPanel::kPresetDescs[ctx.hubSelectedPreset]);
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Text("Scripting Language");
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##npLang", &ctx.hubSelectedLang,
                kLangNames.data(), static_cast<int>(kLangNames.size()));
            ImGui::TextDisabled("%s", kLangDesc[ctx.hubSelectedLang]);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.45f, 1.0f));
            ImGui::TextWrapped("Applies to the whole project and can't be changed after it's created.");
            ImGui::PopStyleColor();

            // Advanced Shader Effects: whether this project may author materials
            // (docs/he-apps-plan.md A0). Only for APPLICATIONS — a game without
            // materials is not a thing anyone wants, and offering the switch
            // everywhere is how an Empty project ended up with materials
            // disabled by a checkbox its author read as harmless.
            //
            // Forced back on for every other template, so leaving it unticked and
            // then picking Game cannot carry the setting across.
            if (isAppPreset(static_cast<ProjectPreset>(ctx.hubSelectedPreset)))
            {
                ImGui::Spacing();
                ImGui::Checkbox("Advanced Shader Effects", &ctx.hubAdvancedShaderFx);
                ImGui::TextDisabled("%s", ctx.hubAdvancedShaderFx
                    ? "Materials and material graphs are available. The packaged build ships a GPU renderer."
                    : "No materials: widgets are styled with corner radius, borders, gradients and shadows. "
                      "Smaller build, no GPU required.");
            }
            else
                ctx.hubAdvancedShaderFx = true;

            ImGui::Spacing();
            if (!ctx.hubCreateError.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", ctx.hubCreateError.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            ImGui::Separator();
            ImGui::Spacing();
            const float btnW = (480.0f - 16.0f * 2 - 8.0f) * 0.5f;
            if (EditorWidgets::primaryButton("Create", ImVec2(btnW, 0)))
            {
                ctx.hubCreateError.clear();
                std::string name = ctx.hubProjectName;
                std::string dir  = ctx.hubProjectDir;
                if (name.empty())
                    ctx.hubCreateError = "Please enter a project name.";
                else if (dir.empty())
                    ctx.hubCreateError = "Please select a project directory.";
                else
                {
                    std::filesystem::path projRoot = std::filesystem::path(dir) / name;
                    bool ok = ctx.projectManager->createNewProject(
                        projRoot.string(), name,
                        static_cast<ProjectPreset>(ctx.hubSelectedPreset),
                        static_cast<ProjectScriptLanguage>(ctx.hubSelectedLang),
                        /*appProject*/ false, ctx.hubAdvancedShaderFx);
                    if (ok)
                    {
                        const std::string& heprojPath = ctx.projectManager->currentProject().path;
                        ctx.globalState->addKnownProject(heprojPath);
                        ctx.globalState->writeConfig();
                        ctx.contentRefreshPending = true;
                        // Said here rather than left to the content-refresh modal to
                        // infer. This was the one project-opening path that did not
                        // set it, so the modal had to set it for everybody — and a
                        // modal that turns "a project is open" on unconditionally
                        // turns it on after a CLOSE too.
                        ctx.projectLoaded = true;
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        ctx.hubCreateError = "Failed to create project. Check path/permissions.";
                    }
                }
            }
            ImGui::SameLine();
            if (EditorWidgets::cancelButton("Cancel", ImVec2(btnW, 0)))
            {
                ctx.hubCreateError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else
        {
            ImGui::PopStyleVar(2);
        }
    }

    static constexpr float kFooterH  = 24.0f;
    static constexpr float kTabBarH  = 28.0f;
// Collab read-only banner above a locked asset tab. Tall enough for a real
// button ("Ask to edit") rather than text alone, AND for the sentence next to it
// to take two lines: that sentence is ~100 characters and only gets the tab
// width minus the button's 140px, so at any normal tab width it wraps. At one
// line's worth of height (the old 38) the second line fell outside a banner that
// has no scrollbar, and the clause it took with it — "Their changes appear here
// live" — is the one that stops the user closing a tab they think is frozen.
// MEASURED, not a constant. Two text lines plus the frame-padding baseline
// offset plus the window's padding above and below — the arithmetic that gives
// 52 at the default font, which is exactly why writing 52 down would be wrong.
// The editor's font scale is a user setting (Preferences, 0.5×–3×) and it scales
// the line height while WindowPadding and FramePadding are not rescaled with it,
// so a height tuned at 1× puts the second line back outside the banner — and the
// banner has no scrollbar — for anyone who enlarged the UI. Which is to say: for
// exactly the people who enlarged it because they were having trouble reading.
    const float kAssetLockBannerH =
        ImGui::GetTextLineHeight() * 2.0f +
        ImGui::GetStyle().FramePadding.y +
        ImGui::GetStyle().WindowPadding.y * 2.0f + 6.0f;

    // ── Footer bar ────────────────────────────────────────────────────────────
    // Must be rendered BEFORE the DockSpace window so ImGui processes it first
    // and docked windows cannot overlap it.
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kFooterH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kFooterH), ImGuiCond_Always);
        ImGui::SetNextWindowViewport(vp->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(8.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));

        // NoBringToFrontOnFocus: this is chrome pinned to the window edge. Clicking
        // Undo down here must not lift the bar over a floating panel that overlaps it.
        ImGui::Begin("##EditorFooter", nullptr,
            ImGuiWindowFlags_NoTitleBar            |
            ImGuiWindowFlags_NoResize              |
            ImGuiWindowFlags_NoMove                |
            ImGuiWindowFlags_NoScrollbar           |
            ImGuiWindowFlags_NoSavedSettings       |
            ImGuiWindowFlags_NoDocking             |
            ImGuiWindowFlags_NoFocusOnAppearing    |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNav                 |
            ImGuiWindowFlags_NoDecoration);

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();

		if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

		// Left — Undo / Redo buttons
		{
			constexpr float btnSize = 16.0f;
			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.10f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.20f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));

			const bool canUndo = ctx.undoSys && ctx.undoSys->canUndo();
			const bool canRedo = ctx.undoSys && ctx.undoSys->canRedo();

			ImGui::BeginDisabled(!canUndo);
			bool doUndo;
			if (ctx.toolbarIcons.undo)
				doUndo = ImGui::ImageButton("##footerUndo", ctx.toolbarIcons.undo, ImVec2(btnSize, btnSize));
			else
				doUndo = ImGui::Button("Undo");
			ImGui::EndDisabled();

			ImGui::SameLine(0.0f, 4.0f);

			ImGui::BeginDisabled(!canRedo);
			bool doRedo;
			if (ctx.toolbarIcons.redo)
				doRedo = ImGui::ImageButton("##footerRedo", ctx.toolbarIcons.redo, ImVec2(btnSize, btnSize));
			else
				doRedo = ImGui::Button("Redo");
			ImGui::EndDisabled();

			// Keyboard shortcuts: Cmd/Ctrl+Z, Shift+Cmd/Ctrl+Z (or Ctrl+Y)
			const ImGuiIO& kio = ImGui::GetIO();
			const bool mod = kio.KeyCtrl || kio.KeySuper;
			if (!kio.WantTextInput && mod)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
					(kio.KeyShift ? doRedo : doUndo) = true;
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
					doRedo = true;
			}

			if (doUndo && canUndo && ctx.undo) ctx.undo();
			if (doRedo && canRedo && ctx.redo) ctx.redo();

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
		}

		// Source control, immediately right of Undo/Redo — the bottom-left corner
		// every editor keeps it in. Ambient by design: "eleven files I have not
		// committed" is something to notice in passing, not to go looking for.
		ImGui::SameLine(0.0f, 14.0f);
		if (SourceControlPanel::DrawFooterStatus(ctx))
			revealFloatingWindow(s_showSourceControl, "Source Control");

		// Right — render resolution + FPS (drawn before SameLine so GetWindowWidth() is stable).
		// The resolution is the actual viewport framebuffer size the scene renders at.
		std::string fpsText = "FPS: " + std::to_string(static_cast<int>(ctx.smoothFps));
		int viewportPxW = 0, viewportPxH = 0;
		ViewportPanel::renderSizePx(viewportPxW, viewportPxH);
		if (viewportPxW > 0 && viewportPxH > 0)
			fpsText = std::to_string(viewportPxW) + "x" + std::to_string(viewportPxH)
			        + "   " + fpsText;
		const float fpsW = ImGui::CalcTextSize(fpsText.c_str()).x;
		ImGui::SameLine(ImGui::GetWindowWidth() - fpsW - ImGui::GetStyle().WindowPadding.x);
		ImGui::Text("%s", fpsText.c_str());

		// ── The right-anchored group ──────────────────────────────────────────
		// Everything below is placed by subtracting the width of everything to
		// its right, plus a 16px gap for each neighbour that is actually there.
		// The chain is hand-maintained: inserting a widget here means editing
		// EVERY block to its left as well, or the two overlap silently. Right to
		// left the order is: FPS, notification bell, presence, download queue,
		// session activity.

		// The notification bell, immediately left of the counters. It sits at the
		// right-hand end of the group rather than the left because it is the only
		// item here that is not transient — presence, the download queue and the
		// activity line all come and go, so a bell placed left of them would move
		// across half the footer depending on what else happened to be running,
		// and the one control the user is meant to reach for when something went
		// wrong must be in the same corner every time.
		const float bellW = NotificationBar::FooterWidth(ctx);
		if (bellW > 0.0f)
		{
			ImGui::SameLine(ImGui::GetWindowWidth() - fpsW - bellW
			                - ImGui::GetStyle().WindowPadding.x - 16.0f);
			NotificationBar::DrawFooter(ctx);
		}

		// Collaboration presence, immediately left of the counters — ambient by
		// design: who else is in the scene, without a window open.
		const float presenceW = CollabPresenceBar::FooterWidth(ctx);
		if (presenceW > 0.0f)
		{
			ImGui::SameLine(ImGui::GetWindowWidth() - fpsW - bellW - presenceW
			                - ImGui::GetStyle().WindowPadding.x - 16.0f
			                - (bellW > 0.0f ? 16.0f : 0.0f));
			CollabPresenceBar::DrawFooter(ctx);
		}

		// EngineContent SFTP download queue — right-aligned, left of the presence
		// cluster. Deliberately part of the RIGHT group rather than growing
		// rightwards from source control on the left: the label is an asset
		// filename of unbounded length, and a left-anchored item that grows runs
		// straight through the centred "Ready" text below. Right-aligning bounds
		// it against a fixed edge instead. Draws nothing when idle.
		const float syncW = EngineContentSyncBar::FooterWidth(ctx);
		if (syncW > 0.0f)
		{
			ImGui::SameLine(ImGui::GetWindowWidth() - fpsW - bellW - presenceW - syncW
			                - ImGui::GetStyle().WindowPadding.x - 16.0f
			                - (bellW > 0.0f ? 16.0f : 0.0f)
			                - (presenceW > 0.0f ? 16.0f : 0.0f));
			EngineContentSyncBar::DrawFooter(ctx);
		}

		// What the session just did to the project: assets other participants
		// created, and the host's answer to one of ours. Left of the download
		// queue, in the same right-anchored group and for the same reason — the
		// text is a sentence of unbounded length, and only a fixed edge keeps it
		// out of the centred status label.
		if (const float actW = CollabActivityBar::FooterWidth(ctx); actW > 0.0f)
		{
			ImGui::SameLine(ImGui::GetWindowWidth() - fpsW - bellW - presenceW - syncW - actW
			                - ImGui::GetStyle().WindowPadding.x - 16.0f
			                - (bellW > 0.0f ? 16.0f : 0.0f)
			                - (presenceW > 0.0f ? 16.0f : 0.0f)
			                - (syncW > 0.0f ? 16.0f : 0.0f));
			if (CollabActivityBar::DrawFooter(ctx))
				revealFloatingWindow(s_showCollab, "Collaboration");
		}

		// Middle — status
		const std::string statusText = "Ready";
		const float       statusW    = ImGui::CalcTextSize(statusText.c_str()).x;
		ImGui::SameLine((ImGui::GetWindowWidth() - statusW) * 0.5f);
		ImGui::TextDisabled("%s", statusText.c_str());

        if (ctx.fontBody) ImGui::PopFont();

        ImGui::End();
    }

    // ── Editor TabBar (below menu bar, above DockSpace) ────────────────────────
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kTabBarH), ImGuiCond_Always);
        ImGui::SetNextWindowViewport(vp->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(4.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding,      2.0f);
        {
            using namespace HE::Ed::Theme;
            ImGui::PushStyleColor(ImGuiCol_WindowBg,   warm(0.115f));
            ImGui::PushStyleColor(ImGuiCol_Tab,        warm(0.165f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered, mix(warm(0.165f), AccentHi, 0.26f));
            ImGui::PushStyleColor(ImGuiCol_TabActive,  mix(warm(0.225f), AccentHi, 0.14f));
        }

        // NoBringToFrontOnFocus for the same reason as the footer: switching tabs
        // must not lift this strip over a floating panel.
        ImGui::Begin("##EditorTabBar", nullptr,
            ImGuiWindowFlags_NoTitleBar            |
            ImGuiWindowFlags_NoResize              |
            ImGuiWindowFlags_NoMove                |
            ImGuiWindowFlags_NoScrollbar           |
            ImGuiWindowFlags_NoSavedSettings       |
            ImGuiWindowFlags_NoDocking             |
            ImGuiWindowFlags_NoFocusOnAppearing    |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNav                 |
            ImGuiWindowFlags_NoDecoration);

        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(4);

        // ── Tab state (owned by AppContext / EditorApplication) ───────────
        auto& s_tabs      = ctx.tabs;
        auto& s_activeTab = ctx.activeTab;

        // Open request from inside an editor panel (e.g. double-clicking a Material
        // Function node) — same find-or-push flow as the Content Browser double-click.
        if (const std::string req = MaterialEditorPanel::takeOpenRequest(); !req.empty())
        {
            auto it = std::find_if(s_tabs.begin(), s_tabs.end(),
                [&](const AppContext::EditorTab& t){ return t.assetPath == req; });
            if (it == s_tabs.end())
            {
                s_tabs.push_back({ std::filesystem::path(req).stem().string(), req, true, true });
                s_activeTab = static_cast<int>(s_tabs.size()) - 1;
            }
            else
                s_activeTab = static_cast<int>(std::distance(s_tabs.begin(), it));
            s_tabSelectRequest = s_activeTab;
        }

        if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

        if (ImGui::BeginTabBar("##MainTabBar",
            ImGuiTabBarFlags_Reorderable |
            ImGuiTabBarFlags_FittingPolicyScroll |
            ImGuiTabBarFlags_NoCloseWithMiddleMouseButton))
        {
            // Closing a tab drops its cached editor state (text buffers, graphs,
            // preview textures) so per-session cost stays flat no matter how many
            // tabs were opened. Dirty states are kept: reopening the tab restores
            // the unsaved edits instead of silently discarding them.
            auto forgetTabState = [&ctx](const AppContext::EditorTab& t){
                if (t.assetPath.empty()) return;
                // Assets a panel's PREVIEW streamed in go back regardless of the
                // dirty check below: an unsaved graph is worth keeping in memory,
                // the mesh its preview happened to sit on is not. Panels leave
                // anything the scene or another tab still uses alone.
                MaterialEditorPanel::releasePreviewAssets(ctx, t.assetPath);
                if (tabHasUnsavedEdits(t.assetPath)) return;
                // The panel list itself lives in discardPanelState — one place
                // to add a new panel to, rather than two that drift apart.
                discardPanelState(ctx, t.assetPath);
            };

            for (int i = 0; i < static_cast<int>(s_tabs.size()); )
            {
                auto& tab = s_tabs[i];
                if (!tab.open) { forgetTabState(tab); s_tabs.erase(s_tabs.begin() + i); continue; }

                ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
                // Force-select only on an explicit one-shot request (double-click). Using
                // s_activeTab here every frame is wrong: BeginTabItem mutates s_activeTab
                // mid-loop, so the Scene tab (rendered first) steals it back — and the
                // constant SetSelected also swallows manual tab clicks.
                if (i == s_tabSelectRequest) flags |= ImGuiTabItemFlags_SetSelected;

                bool pOpen = tab.closable ? tab.open : true;
                // Stable ID (### + assetPath) so appending a dirty marker to the visible
                // label never changes the tab's identity — which would reset its state.
                const bool tabDirty = tabHasUnsavedEdits(tab.assetPath);
                const std::string shown = tab.label + (tabDirty ? " *" : "")
                    + "###tab_" + (tab.assetPath.empty() ? std::string("scene") : tab.assetPath);
                if (ImGui::BeginTabItem(shown.c_str(), tab.closable ? &pOpen : nullptr, flags))
                {
                    s_activeTab = i;
                    ImGui::EndTabItem();
                }
                if (tab.closable) tab.open = pOpen;
                ++i;
            }
            // Remove closed tabs (dropping their cached editor state)
            for (const auto& t : s_tabs)
                if (t.closable && !t.open) forgetTabState(t);
            s_tabs.erase(
                std::remove_if(s_tabs.begin(), s_tabs.end(),
                    [](const AppContext::EditorTab& t){ return t.closable && !t.open; }),
                s_tabs.end());
            // Keep the active index valid after a tab closes (else it dangles or points
            // at the wrong tab). Fall back to the Scene tab (index 0) when out of range.
            if (s_activeTab >= static_cast<int>(s_tabs.size()))
                s_activeTab = static_cast<int>(s_tabs.size()) - 1;
            if (s_activeTab < 0) s_activeTab = 0;
            s_tabSelectRequest = -1;   // consume the one-shot select request

            ImGui::EndTabBar();
        }

        if (ctx.fontBody) ImGui::PopFont();

        ImGui::End();
    }

    // ── Top-level tab gating ───────────────────────────────────────────────────
    // The built-in "Scene" tab (empty assetPath) shows the dockspace + all panels
    // below. A script tab instead fills that same area with its code editor and we
    // skip the scene panels for this frame. Default to the scene when the active
    // index is out of range. (All ImGui windows above are already balanced, so the
    // early return is safe; modals/menus render before this point.)
    const bool sceneTabActive =
        ctx.activeTab < 0 || ctx.activeTab >= static_cast<int>(ctx.tabs.size())
        || ctx.tabs[ctx.activeTab].assetPath.empty();

    // ── Entity gestures: Ctrl+D / X / C / V and Delete ───────────────────────
    // Here rather than up with the save shortcuts because of the one guard that
    // matters: `sceneTabActive`. The material graph, the UI editor and the
    // HorizonCode canvas all bind these same keys for their OWN nodes, and each
    // of them IS a top-level tab — so gating on the scene tab is what keeps one
    // Ctrl+D from duplicating a material node and a scene entity at once.
    //
    // WantTextInput on top of it, or renaming an entity in the Details panel
    // would delete it at the first Delete keystroke, and IsAnyItemActive with
    // it: a field that holds keyboard focus without being "active" this frame
    // still owns what is typed into it (same rule as UIEditorPanel).
    //
    // Delete only, never Backspace: Backspace is the text-editing key and a
    // near-miss on it used to destroy the selection in the other editors.
    {
        const ImGuiIO& kio = ImGui::GetIO();
        const bool typing = kio.WantTextInput || ImGui::IsAnyItemActive();

        // The scene tab is not enough on its own: the Content Browser is docked
        // into it and binds Delete for its own asset deletion
        // (ContentBrowserPanel.cpp), so one press would delete an asset AND the
        // selected entity. Walk the child chain, because a focused child inside a
        // panel still belongs to that panel — same NavWindow lookup the tutorial
        // overlay uses.
        const auto panelOwnsKeys = [](const char* title) {
            const ImGuiContext* g = ImGui::GetCurrentContext();
            if (!g || !g->NavWindow) return false;
            for (const ImGuiWindow* w = g->NavWindow; w; w = w->ParentWindow)
                if (std::strcmp(w->Name, title) == 0) return true;
            return false;
        };
        if (sceneTabActive && !typing && !panelOwnsKeys("Content Browser"))
        {
            const bool mod = kio.KeyCtrl || kio.KeySuper;
            if (canEditEntity())
            {
                if (mod && ImGui::IsKeyPressed(ImGuiKey_D, false) && ctx.duplicateEntity)
                    ctx.duplicateEntity();
                if (mod && ImGui::IsKeyPressed(ImGuiKey_C, false) && ctx.copyEntity)
                    ctx.copyEntity();
                if (mod && ImGui::IsKeyPressed(ImGuiKey_X, false) && ctx.cutEntity)
                    ctx.cutEntity();
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && ctx.deleteEntity)
                    ctx.deleteEntity();
            }
            if (mod && ImGui::IsKeyPressed(ImGuiKey_V, false) && canPasteEntity() && ctx.pasteEntity)
                ctx.pasteEntity();
        }
    }

    // The dockspace itself is only drawn on the scene tab (below). On every other
    // tab it still has to be declared alive, or the windows docked into it that
    // ARE submitted on this tab — the View-menu panels, the play report — get
    // undocked by ImGui and start floating over the asset editor. Alive-but-not-
    // drawn hides them instead, which is what "docked = part of the scene tab"
    // means. Before the first such Begin, hence up here.
    if (!sceneTabActive) EditorDockState::keepMainDockspaceAlive();

    // Post-PIE report window (drawn before the tab gating so it shows on any tab).
    PlayReportPanel::drawPlayReport(ctx);

    if (!sceneTabActive)
    {
        // The scene viewport (and its RMB fly-look release) won't run this frame. If the
        // user switched here mid-look via a keyboard shortcut, force-release the capture so
        // the cursor isn't left hidden/pinned with ImGui mouse input disabled.
        ViewportPanel::releaseViewportLookCapture(ctx.window ? ctx.window->GetNativeWindow() : nullptr);

        const ImGuiViewport* vpTab = ImGui::GetMainViewport();
        const std::string& tabPath = ctx.tabs[ctx.activeTab].assetPath;
        ImVec2 tabPos(vpTab->WorkPos.x, vpTab->WorkPos.y + kTabBarH);
        ImVec2 tabSize(vpTab->WorkSize.x, vpTab->WorkSize.y - kFooterH - kTabBarH);

        // ── Collaboration: someone else is editing this asset ────────────────
        // The whole tab renders disabled while a peer holds the asset's lock —
        // "read-only for real", enforced at the one place every asset panel
        // passes through rather than audited into each of them. The banner says
        // WHO, so the user knows to coordinate instead of wondering why the
        // canvas ignores them.
        bool tabReadOnly = false;
        if (ctx.collab && ctx.collab->inSession() && ctx.contentManager)
        {
            // THE session key, not a content-relative path. Two things were
            // wrong with deriving it here:
            //
            //   * a C++ class is under Source, which the content root does not
            //     contain, so this was empty and the tab was never read-only —
            //     two people could type into one file with nothing arbitrating.
            //   * a mesh or texture tab got a NON-empty path but is not a kind
            //     that syncs, so nothing ever asked the host about it and
            //     assetEditState stayed Unknown for good: a permanent
            //     "Checking with the host…" over a tab that never came back.
            //
            // The key function answers both — empty means "nothing a session
            // carries", which is exactly the Editable case.
            const std::string rel = ctx.collabKeyForPath
                ? ctx.collabKeyForPath(tabPath, /*isFolder=*/false)
                : CollabController::projectRelativeAssetPath(
                      tabPath, ctx.contentManager->contentRoot());
            using EditState = CollabController::AssetEditState;
            const EditState edit = rel.empty() ? EditState::Editable
                                               : ctx.collab->assetEditState(rel);

            // Unknown = the tab just opened and the HOST has not answered who
            // holds this asset yet. That is a frame or two, and editing through
            // it is exactly the race this whole mechanism exists to remove — so
            // the canvas draws (you can read it) but takes no input.
            if (edit == EditState::Unknown)
            {
                tabReadOnly = true;
                ImGui::SetNextWindowPos(tabPos);
                ImGui::SetNextWindowSize(ImVec2(tabSize.x, kAssetLockBannerH));
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.13f, 1.0f));
                if (ImGui::Begin("##asset_lock_pending", nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar))
                {
                    // Wrapped at the banner's own right edge — a narrow tab (a
                    // docked asset editor can be half the window) would otherwise
                    // cut the sentence off mid-word, and this banner has no
                    // scrollbar to reveal the rest. Safe at 0.0f: the banner is a
                    // fixed-size window (SetNextWindowSize above), so wrapping to
                    // its width cannot feed back into its width.
                    EditorWidgets::WrapText wrap;
                    ImGui::TextDisabled("Checking with the host whether anyone is "
                                        "editing this asset…");
                }
                ImGui::End();
                ImGui::PopStyleColor();
                tabPos.y  += kAssetLockBannerH;
                tabSize.y -= kAssetLockBannerH;
                ImGui::BeginDisabled(true);
            }
            else if (edit == EditState::HeldByOther)
            {
                tabReadOnly = true;
                const HE::Net::LockInfo* lock = ctx.collab->assetLockInfo(rel);
                float rgb[3] = { 1.0f, 0.75f, 0.3f };
                if (lock) ctx.collab->colorFor(lock->owner, rgb);

                ImGui::SetNextWindowPos(tabPos);
                ImGui::SetNextWindowSize(ImVec2(tabSize.x, kAssetLockBannerH));
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.11f, 0.08f, 1.0f));
                if (ImGui::Begin("##asset_lock_banner", nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::AlignTextToFramePadding();
                    {
                        // The sentence has to stop where the "Ask to edit" button
                        // starts (SameLine below puts it 140px in from the right).
                        // Without that reservation it ran straight under the button
                        // and was cut off there — and what got cut was the tail,
                        // "Their changes appear here live", i.e. the half that says
                        // the tab is not frozen and does not need closing.
                        //
                        // Two things about the column itself. It is scoped to this
                        // sentence and not to the whole banner, because at the
                        // SameLine below the cursor sits PAST it, and ImGui clamps
                        // a wrap column left of the cursor to a 1px width — the
                        // "waiting for an answer…" line would come out one
                        // character per line. And it is floored so it can never go
                        // negative on a tab docked narrower than the button: a
                        // negative wrap position is not "wrap tightly", it is
                        // ImGui's way of spelling "do not wrap at all".
                        EditorWidgets::WrapText wrap(std::max(
                            ImGui::GetContentRegionMax().x - 148.0f,
                            ImGui::GetFontSize() * 8.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
                        ImGui::TextUnformatted(lock && !lock->ownerName.empty()
                            ? (lock->ownerName + " is editing this asset — it is read-only "
                               "for you until they are done. Their changes appear here live.").c_str()
                            : "Someone else is editing this asset — it is read-only for you.");
                        ImGui::PopStyleColor();
                    }

                    // The way out of read-only. Deliberately here and not in a
                    // menu: this banner is where the user is at the moment they
                    // want the asset, and the answer comes from the person the
                    // banner just named. Drawn BEFORE BeginDisabled below — the
                    // tab is inert, this is not.
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - 140.0f);
                    if (ctx.collab->hasAskedToEdit(rel))
                    {
                        ImGui::TextDisabled("waiting for an answer\xE2\x80\xA6");
                    }
                    else
                    {
                        // Its own scope: the button sits in the read-only banner of an asset
                        // tab, which is a collaboration state, not a menu.
                        HE::Ed::Help::Scope lockScope("Collaboration");
                        if (EditorWidgets::button("Ask to edit", ImVec2(130, 0)))
                            ctx.collab->requestAssetEdit(rel);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "Asks %s to hand this asset over. They decide.",
                                lock && !lock->ownerName.empty()
                                    ? lock->ownerName.c_str() : "whoever is editing it");
                    }
                }
                ImGui::End();
                ImGui::PopStyleColor();

                // The panel keeps its remaining space and renders disabled.
                tabPos.y  += kAssetLockBannerH;
                tabSize.y -= kAssetLockBannerH;
                ImGui::BeginDisabled(true);
            }
        }
        // Dispatch by asset type: material assets get the node-graph editor, script
        // assets the code editor. (Cheap header sniff; both panels cache their state.)
        // The Level Script + Game Instance are virtual tabs (no backing .hasset).
        if (tabPath == EditorSettingsPanel::kTabPath)
            EditorSettingsPanel::render(ctx, tabPos, tabSize);
        else if (tabPath == LevelScriptPanel::kTabPath)
            LevelScriptPanel::render(ctx, tabPos, tabSize);
        else if (tabPath == GameInstancePanel::kTabPath)
            GameInstancePanel::render(ctx, tabPos, tabSize);
        else if (MaterialEditorPanel::isMaterialAsset(tabPath) ||
            MaterialEditorPanel::isMaterialFunctionAsset(tabPath))
        {
            // Advanced Shader Effects off = this project does not author
            // materials (docs/he-apps-plan.md E1b). An existing one can still be
            // opened — the file is right there in the browser — but it says why
            // it is inert rather than showing an editor whose result nothing
            // would draw.
            const bool allowMaterials = !ctx.projectManager ||
                ctx.projectManager->currentProject().advancedShaderEffects;
            if (allowMaterials)
                MaterialEditorPanel::render(ctx, tabPath, tabPos, tabSize);
            else
            {
                ImGui::SetNextWindowPos(tabPos);
                ImGui::SetNextWindowSize(tabSize);
                ImGui::Begin("##MaterialsDisabled", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
                ImGui::TextWrapped(
                    "Advanced Shader Effects are switched off for this project, so materials "
                    "are not used. Widgets are styled with corner radius, borders, gradients "
                    "and shadows instead.");
                ImGui::Spacing();
                ImGui::TextDisabled("Turn them on in the project settings to edit this asset.");
                ImGui::End();
            }
        }
        else if (UIEditorPanel::isWidgetAsset(tabPath))
            UIEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (HorizonCodeClassPanel::isClassAsset(tabPath))
            HorizonCodeClassPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (InputAssetPanel::isInputAsset(tabPath))
            InputAssetPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (ThemeAssetPanel::isThemeAsset(tabPath))
            ThemeAssetPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (TypeAssetPanel::isTypeAsset(tabPath))
            TypeAssetPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (SkeletalMeshEditorPanel::isSkeletalMeshAsset(tabPath))
            SkeletalMeshEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (StaticMeshEditorPanel::isStaticMeshAsset(tabPath))
            StaticMeshEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (ParticleGraphEditorPanel::isParticleAsset(tabPath))
            ParticleGraphEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (AnimatorStateMachineEditorPanel::isAnimatorStateMachineAsset(tabPath))
            AnimatorStateMachineEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        // Audio .hasset AND raw .wav. Like the C++ viewer below it, the raw-file half
        // is an extension check, so it has to beat the ScriptEditorPanel fallthrough —
        // which would render megabytes of PCM as text.
        else if (AudioEditorPanel::isAudioAsset(tabPath))
            AudioEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        // C++ source/header (raw files, extension-based) → h/cpp class viewer. Must
        // come before the ScriptEditorPanel fallthrough, which assumes an HAsset.
        else if (CppClassEditorPanel::isCppSourceAsset(tabPath))
            CppClassEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else
            ScriptEditorPanel::render(ctx, tabPath, tabPos, tabSize);

        if (tabReadOnly) ImGui::EndDisabled();
        return;
    }

    // ── DockSpace (shrunk by footer + tabbar height so docked windows never overlap)
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + kTabBarH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - kFooterH - kTabBarH), ImGuiCond_Always);
        ImGui::SetNextWindowViewport(vp->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg,  ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

        // NoBringToFrontOnFocus is what keeps floating windows (Tutorial, Profiler,
        // Environment, any undocked panel) usable. Every docked panel's root is
        // THIS window, so focusing one of them made ImGui bring the whole dock tree
        // to the display front — the full-screen layout then painted over every
        // floating window. They stayed open but were completely covered, so they
        // could not even be clicked back to the front: gone for good. With the flag
        // the host stays at the back of the z-order, where a dockspace belongs.
        // (ImGui's own DockSpaceOverViewport() sets exactly these two flags.)
        ImGui::Begin(EditorDockState::kHostWindowName, nullptr,
            ImGuiWindowFlags_NoTitleBar             |
            ImGuiWindowFlags_NoResize               |
            ImGuiWindowFlags_NoMove                 |
            ImGuiWindowFlags_NoScrollbar            |
            ImGuiWindowFlags_NoSavedSettings        |
            ImGuiWindowFlags_NoFocusOnAppearing     |
            ImGuiWindowFlags_NoBringToFrontOnFocus  |
            ImGuiWindowFlags_NoNavFocus             |
            ImGuiWindowFlags_NoBackground);

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        // Same value as ImGui::GetID(kDockspaceLabel) right here — but spelled
        // in one place, because the asset-tab path needs it too (see
        // keepMainDockspaceAlive) and cannot call GetID from inside this window.
        const ImGuiID dockspaceId = EditorDockState::mainDockspaceId();
        // Build the default layout on first run (no saved layout in imgui.ini) or
        // on demand via View > Reset Layout. A layout loaded from imgui.ini
        // otherwise always wins, so user customisations persist.
        if (s_resetLayoutRequested || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        {
            BuildDefaultDockLayout(dockspaceId, ImGui::GetContentRegionAvail());
            s_resetLayoutRequested = false;
            s_sceneTabBarHandled   = false;   // fresh layout → hide the viewport's tab bar again
            // Persist immediately so the layout survives an early exit and becomes
            // the baseline the user then customises.
            if (const char* ini = ImGui::GetIO().IniFilename)
                ImGui::SaveIniSettingsToDisk(ini);
        }

        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
            ImGuiDockNodeFlags_PassthruCentralNode);

        ImGui::End();
    }

	// ── Scene viewport (offscreen render target as dockable window) ─────────
	// Toolbar, editor-camera navigation, scene extract, drag-drop spawn, gizmo,
	// picking and the Landscape brush all live in ViewportPanel.cpp.
	ViewportPanel::render(ctx, dt);
	// After the window exists this frame: its dock node is only reachable once
	// "Scene" has been submitted at least once.
	HideSceneTabBarOnce();


    // ── Landscape / Quick Settings panel ────────────────────────────────────
    // In Landscape mode this panel IS the Landscape tool panel (the whole
    // sculpt/paint toolset lives below); otherwise it shows the user-pinned
    // Quick Settings. The title says which, but everything after "###" is what
    // ImGui hashes into the window id — keeping it pinned to "Quick Settings"
    // means the docking layout in imgui.ini and BuildDefaultDockLayout's
    // DockBuilderDockWindow("Quick Settings") still find this window, across
    // both the mode switch and this rename.
    const bool landscapePanel = ctx.editorConfig.mode == EditorMode::Landscape && ctx.world;
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin(landscapePanel ? "Landscape###Quick Settings"
                                : "Quick Settings###Quick Settings");
    if (ctx.fontHeading) ImGui::PopFont();

    if (landscapePanel)
    {
        TerrainTools::renderPanel(ctx);
    }
    else
    {
        // Quick Settings = the engine settings the user pinned in Preferences.
        EditorSettingsPanel::DrawEngineSettings(ctx,
            EditorSettingsPanel::SettingsMode::QuickSettings);
    }

    ImGui::End();

    // ── World Outliner ──────────────────────────────────────────────────────
    // Hierarchy tree, selection, drag & drop reparenting and the entity
    // create/rename/delete menus — all in OutlinerPanel.cpp.
    OutlinerPanel::render(ctx);

    // The Details panel edits the selected ENTITY's components. An application
    // has no entities, and its widgets are edited in the UI designer tab, so the
    // panel would be permanently empty (docs/he-apps-plan.md E2).
    if (!ctx.appLivePreview)
        InspectorPanel::render(ctx);

    // Level Script + Game Instance now render as editor tabs (see the tab dispatch).

    // ── Content Browser ─────────────────────────────────────────────────────
    // Folder tree + asset grid over the three content roots, create/import/rename/
    // delete, drag-to-move and the double-click dispatch that opens an editor tab.
    // Lives in ContentBrowserPanel.cpp; it needs this function's one-shot tab-select
    // slot and the unsaved-changes-guarded scene open, so both are handed in.
    ContentBrowserPanel::render(ctx, s_tabSelectRequest,
        [&](const std::string& scenePath) { requestGuarded(GuardedAction::OpenScenePath, scenePath); });

#endif // HE_IMGUI_ENABLED
}

#ifdef HE_IMGUI_ENABLED
// Everything that must be on screen no matter which tab is active: the floating
// tool windows the View/Edit menus toggle, and the guided tour.
//
// These all used to sit at the bottom of renderEditor(), which returns early when
// an asset tab is active (a material graph, a particle graph, a widget, the Level
// Script). So: opening Preferences, the Profiler or the Environment window from
// inside an asset tab silently did nothing, and the tour vanished the moment the
// user opened one — and, worse, stopped SAMPLING there, so every step whose
// action is "open this asset's editor" could never see itself happen. Those were
// exactly the steps that looked like they were not being noticed.
//
// Drawn after renderEditor returns, so they are on top of both layouts. The menu
// toggles they read are file statics in this translation unit, hence the
// explicit hand-over into UiFlags.
//
// "On screen no matter which tab" is about the FLOATING case, which is what a
// window pulled up over a material graph is. A panel the user DOCKED is part of
// the scene layout and has no business hanging over another tab — it is still
// submitted here (this code cannot know which of the two it is, and ImGui would
// undock it if it stopped), but renderEditor has told ImGui the dockspace is
// alive-but-not-drawn, and ImGui hides everything docked into it. See
// EditorDockState::keepMainDockspaceAlive.
void EditorUI::renderOverlays(AppContext& ctx, float dt)
{
    ProfilerPanel::DrawProfilerWindow(ctx, s_showProfiler);
    EnvironmentPanel::DrawEnvironmentWindow(ctx, s_showEnvironment);
    CollabPanel::DrawCollabWindow(ctx, s_showCollab);
	SourceControlPanel::DrawSourceControlWindow(ctx, s_showSourceControl);
	// Here for the reason this whole function exists, and it earns it more than
	// most: output is watched from INSIDE a script or a graph tab, and left
	// FLOATING the console keeps working there — which is where renderEditor has
	// already returned.
	ConsolePanel::DrawConsoleWindow(ctx, s_showConsole);

	// The second half of revealFloatingWindow: the window a footer widget asked
	// for exists by now, so the focus request that was a no-op at click time
	// finally lands — on its dock tab if it has one.
	if (s_revealFocusFrames > 0 && s_revealTitle)
	{
		ImGui::SetWindowFocus(s_revealTitle);
		if (--s_revealFocusFrames == 0) s_revealTitle = nullptr;
	}

	// After the panels: the dock state read here is the one they just produced,
	// and a window closed by its own X has already cleared its flag.
	updatePanelVisibility(ctx);

    // Last of all, so the menus that hang off the footer clusters cannot end up
    // underneath a docked panel or a floating tool window. Both of these draw
    // nothing unless their own footer half ran this frame.
    CollabPresenceBar::DrawOverlay(ctx);
    NotificationBar::DrawOverlay(ctx);

    TutorialPanel::UiFlags tutFlags;
    tutFlags.profilerOpen      = s_showProfiler;
    tutFlags.environmentOpen   = s_showEnvironment;
    tutFlags.exportOpen        = ExportDialogPanel::isOpen();
    // Preferences is an editor tab now; "open" for the tour = it is the active tab.
    tutFlags.preferencesOpen   =
        ctx.activeTab >= 0 && ctx.activeTab < static_cast<int>(ctx.tabs.size()) &&
        ctx.tabs[ctx.activeTab].assetPath == EditorSettingsPanel::kTabPath;
    tutFlags.importDialogOpens = s_importDialogOpens;
    tutFlags.contentRootKind   = ContentBrowserPanel::browsedRootKind();
    TutorialPanel::render(ctx, dt, tutFlags);
}
#endif // HE_IMGUI_ENABLED
