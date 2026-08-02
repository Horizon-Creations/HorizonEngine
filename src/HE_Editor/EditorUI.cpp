#include "EditorUI.h"
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
#include "SkeletalMeshEditorPanel.h"
#include "StaticMeshEditorPanel.h"
#include "ParticleGraphEditorPanel.h"
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
#include "EnvironmentPanel.h"
#include "CollabPanel.h"            // View > Collaboration (host / join a live session)
#include "EditorSettingsPanel.h"         // engine-settings catalog + Preferences window
#include "ToolchainDialog.h"             // startup cmake/compiler check
#include "PlayReportPanel.h"             // post-PIE warning/error report
#include "EditorAssetTypeCache.h"        // shared path → AssetType sniff (invalidated below)
#include "EditorWidgets.h"               // dialog placement + detached-modal raise
#ifdef __APPLE__
#include "MacMenuBar.h"   // native system menu bar (replaces the ImGui menu row)
#endif
#include <HorizonScene/HorizonScene.h>
#include <ContentManager/ContentManager.h>
#include <Types/Enums.h>
#include "MeshImporter.h"
#include "SkeletalMeshImporter.h"
#include "AnimationClipImporter.h"
#include "TextureImporter.h"
#include "MaterialImporter.h"
#include "AudioImporter.h"
#include "FontImporter.h"
#include "ImporterCommon.h"   // Importer::gltfHasSkin — static vs. skeletal routing

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


// Times Assets > Import Asset opened the file dialog this run (guided tour signal).
static int s_importDialogOpens = 0;

// Toggled by Edit > Preferences (Ctrl+,); drives the Preferences window.
static bool s_showPreferences = false;

// Menu toggle for a floating (non-docked) panel. On open it also pulls the window
// to the front: it may still exist from an earlier session, sitting underneath
// another floating window, in which case ticking the menu item would otherwise
// look like it did nothing. Unknown title = no-op (the window is created this
// frame and comes up on top anyway).
static void toggleFloatingWindow(bool& open, const char* title)
{
    open = !open;
    if (open) ImGui::SetWindowFocus(title);
}

// Toggled by View > Performance Profiler; drives the profiler panel.
static bool s_showProfiler = false;

// Toggled by View > Environment; drives the Sky/Weather add-remove window.
static bool s_showEnvironment = false;

// Toggled by View > Collaboration; drives the live-session panel.
static bool s_showCollab = false;

// (Level Script + Game Instance open as editor tabs, not toggled windows.)

// Build > Export Project — dialog state, the packing worker thread and the
// cook-time shader precompilers all live in ExportDialogPanel.cpp.
void EditorUI::joinPendingExport()
{
	ExportDialogPanel::joinPendingExport();
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
	ImGui::DockBuilderDockWindow("Scene",          dockMain);
	ImGui::DockBuilderFinish(dockspaceId);
}


#endif // HE_IMGUI_ENABLED

// ─── render ───────────────────────────────────────────────────────────────────

void EditorUI::render(AppContext& ctx, float dt)
{
#ifdef HE_IMGUI_ENABLED
    if (!ctx.imguiReady) return;

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
            float textY = (100.0f - ImGui::GetTextLineHeightWithSpacing() * 2.0f) * 0.5f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textY);
            ImGui::SetCursorPosX((360.0f - ImGui::CalcTextSize("Projektdaten werden aktualisiert...").x) * 0.5f);
            ImGui::TextUnformatted("Projektdaten werden aktualisiert...");

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
                ctx.projectLoaded      = true;
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

    // ── Startup toolchain check — overlays either screen below ───────────────
    ToolchainDialog::DrawToolchainDialog(ctx);

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
	ok = ParticleGraphEditorPanel::save(ctx, assetPath)              && ok;
	ok = AnimatorStateMachineEditorPanel::save(ctx, assetPath)       && ok;
	// The panels are the authority on their own dirty flag; re-asking also catches
	// a save that reported success but left the state dirty.
	return ok && !tabHasUnsavedEdits(assetPath);
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
	auto triggerOpenProject = [&]()
	{
		ctx.hubOpenError.clear();
		s_pendingFileOp = PendingFileOp::OpenProject;
		SDL_DialogFileFilter filters[] = { { "HorizonEngine Project", "heproj" } };
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, nullptr, false);
	};
	auto triggerImportAsset = [&]()
	{
		if (!ctx.projectLoaded || !ctx.contentManager) return;
		// Counted for the guided tour's "open Assets > Import Asset" step: the file
		// dialog is the OS's, so opening it is the only part the editor can observe
		// (and cancelling it must still count — the step teaches where it lives).
		++s_importDialogOpens;
		s_pendingFileOp = PendingFileOp::ImportAsset;
		SDL_DialogFileFilter filters[] = {
			{ "All Supported Assets", "gltf;glb;png;jpg;jpeg;tga;bmp;hdr;wav;hmat" },
			{ "3D Models",            "gltf;glb" },
			{ "Textures",             "png;jpg;jpeg;tga;bmp;hdr" },
			{ "Audio",                "wav" },
			{ "Materials",            "hmat" },
		};
		const std::string root = ctx.contentManager->contentRoot();
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 5,
			root.empty() ? nullptr : root.c_str(),
			false);
	};
	auto doCloseProject = [&]()
	{
		ctx.projectManager->closeProject();
		ctx.globalState->setLastProjectPath("");
		ctx.globalState->writeConfig();
		ctx.projectLoaded = false;
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
	auto requestGuarded = [&](GuardedAction a, const std::string& arg = std::string{})
	{
		const bool tabsDirty = endsSession(a) && !unsavedTabEntries().empty();
		if (!ctx.sceneDirty && !tabsDirty) { runGuardedAction(a, arg); return; }
		s_guardAction      = a;
		s_guardArg         = arg;
		s_guardSaveThenAct = false;
		s_guardSaveError.clear();
		s_openUnsavedModal = true;
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
	auto openExportDialog = [&]() { ExportDialogPanel::open(ctx); };
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
#ifdef __APPLE__
	MacMenuBar::install();   // idempotent; needs NSApp, which SDL created long ago
	nativeMenu = MacMenuBar::available();
	if (nativeMenu)
	{
		MacMenuBar::setProjectLoaded(ctx.projectLoaded);
		using MC = MacMenuBar::Cmd;
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
			case MC::SaveScene:       doSaveScene();                                         break;
			case MC::SaveSceneAs:     triggerSaveSceneAs();                                  break;
			case MC::Quit:            requestGuarded(GuardedAction::Quit);                   break;
			case MC::Preferences:     s_showPreferences = true;                              break;
			case MC::ResetLayout:     s_resetLayoutRequested = true;                         break;
			case MC::ToggleProfiler:  toggleFloatingWindow(s_showProfiler, "Performance Profiler"); break;
			case MC::ToggleEnvironment: toggleFloatingWindow(s_showEnvironment, "Environment"); break;
			case MC::ToggleCollab:      toggleFloatingWindow(s_showCollab, "Collaboration");    break;
			case MC::OpenLevelScript:
				if (ctx.projectLoaded) openVirtualTab("Level Script", LevelScriptPanel::kTabPath);
				break;
			case MC::OpenGameInstance:
				if (ctx.projectLoaded) openVirtualTab("Game Instance", GameInstancePanel::kTabPath);
				break;
			case MC::ImportAsset:     triggerImportAsset();                                  break;
			case MC::ExportProject:   if (ctx.projectLoaded) openExportDialog();             break;
			case MC::OpenTutorial:    TutorialPanel::open();                                 break;
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
		if (ImGui::MenuItem("New Project", "Ctrl+N"))
		{
			beginNewProject();
			openNewProjectPopup = true;
		}
        if (ImGui::MenuItem("Open Project", "Ctrl+O"))
            requestGuarded(GuardedAction::OpenProjectDialog);
		if (ImGui::MenuItem("Close Project", "Ctrl+W"))
			requestGuarded(GuardedAction::CloseProject);
        ImGui::Separator();
        if (ImGui::MenuItem("New Scene"))            requestGuarded(GuardedAction::NewScene);
        if (ImGui::MenuItem("Open Scene..."))        requestGuarded(GuardedAction::OpenSceneDialog);
        if (ImGui::MenuItem("Add Scene Additive...")) triggerAddSceneAdditive();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) doSaveScene();
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) triggerSaveSceneAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            requestGuarded(GuardedAction::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
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

            if (ImGui::MenuItem(uLabel.c_str(), "Ctrl+Z", false,
                                ctx.collabUndo->canUndo()))
                ctx.collabUndo->undo();
            if (ImGui::MenuItem(rLabel.c_str(), "Ctrl+Y", false,
                                ctx.collabUndo->canRedo()))
                ctx.collabUndo->redo();

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("While collaborating, undo applies only to your own changes.");
        }
        else
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut",   "Ctrl+X")) {}
        if (ImGui::MenuItem("Copy",  "Ctrl+C")) {}
        if (ImGui::MenuItem("Paste", "Ctrl+V")) {}
        ImGui::Separator();
		if (ImGui::MenuItem("Preferences", "Ctrl+,")) s_showPreferences = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Toggle Fullscreen", "F11")) {}
        if (ImGui::MenuItem("Reset Layout")) { s_resetLayoutRequested = true; }
        if (ImGui::MenuItem("Performance Profiler", nullptr, s_showProfiler))
            toggleFloatingWindow(s_showProfiler, "Performance Profiler");
        if (ImGui::MenuItem("Environment", nullptr, s_showEnvironment))
            toggleFloatingWindow(s_showEnvironment, "Environment");
        if (ImGui::MenuItem("Collaboration", nullptr, s_showCollab))
            toggleFloatingWindow(s_showCollab, "Collaboration");
        if (ImGui::MenuItem("Level Script", nullptr, false, ctx.projectLoaded))
            openVirtualTab("Level Script", LevelScriptPanel::kTabPath);
        if (ImGui::MenuItem("Game Instance", nullptr, false, ctx.projectLoaded))
            openVirtualTab("Game Instance", GameInstancePanel::kTabPath);
        ImGui::EndMenu();
    }
	if (ImGui::BeginMenu("Assets"))
	{
		if (ImGui::MenuItem("Import Asset...", nullptr, false, ctx.projectLoaded))
			triggerImportAsset();
		if (ImGui::MenuItem("Refresh Assets")) {}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Build", ctx.projectLoaded))
	{
		if (ImGui::MenuItem("Export Project..."))
			openExportDialog();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help"))
	{
		if (ImGui::MenuItem("Interactive Tutorial", nullptr, TutorialPanel::isOpen()))
			TutorialPanel::open();
		ImGui::Separator();
		if (ImGui::MenuItem("Documentation")) {}
		if (ImGui::MenuItem("About")) {}
		ImGui::EndMenu();
	}
    ImGui::EndMainMenuBar();
    ImGui::PopFont();
	}

    if (openNewProjectPopup)
        ImGui::OpenPopup("##NewProjectPopup");

    // ── Export Project modal ────────────────────────────────────────────────
    ExportDialogPanel::render(ctx);

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
            if (ctx.sceneDirty)
                ImGui::Text("Save changes to \"%s\" before continuing?", sceneName.c_str());
            else if (!dirtyTabs.empty())
                ImGui::Text("%d editor tab(s) have unsaved changes.",
                            static_cast<int>(dirtyTabs.size()));
            else
                ImGui::TextUnformatted("Everything is saved.");
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
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
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
            if (ImGui::Button(!anythingDirty ? "Continue"
                                             : (ctx.sceneDirty ? "Don't Save" : "Discard"),
                              ImVec2(110, 0)))
            {
                runGuardedAction(action, arg);
                s_guardAction = GuardedAction::None;
                s_guardSaveError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110, 0)) ||
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
            if (!chosen.empty() && ctx.contentManager)
            {
                const std::filesystem::path srcPath(chosen);
                std::string ext = srcPath.extension().string();
                for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

                const bool isMeshSrc    = (ext == ".gltf" || ext == ".glb");
                const bool isTextureSrc = (ext == ".png"  || ext == ".jpg" || ext == ".jpeg" ||
                                           ext == ".tga"  || ext == ".bmp" || ext == ".hdr");
                const bool isAudioSrc   = (ext == ".wav");
                const bool isMatSrc     = (ext == ".hmat");
                const bool isFontSrc    = (ext == ".ttf" || ext == ".otf");

                const std::filesystem::path root(ctx.contentManager->contentRoot());
                bool ok = false;
                if (isMeshSrc && Importer::gltfHasSkin(srcPath))
                {
                    // Rigged source: MeshImporter would drop the skeleton and the
                    // per-vertex joints/weights and register bind-pose geometry as a
                    // StaticMesh, so the mesh could never be picked as a SkeletalMesh.
                    ok = SkeletalMeshImporter::import(srcPath, root) != nullptr;
                    // Animations usually live in the same glTF and become their own
                    // assets (referenced by the AnimatorStateMachine).
                    if (ok) AnimationClipImporter::importAndWrite(srcPath, root);
                }
                else if (isMeshSrc)    ok = MeshImporter::import(srcPath, root)     != nullptr;
                else if (isTextureSrc) ok = TextureImporter::import(srcPath, root)  != nullptr;
                else if (isAudioSrc)   ok = AudioImporter::import(srcPath, root)    != nullptr;
                else if (isMatSrc)     ok = MaterialImporter::import(srcPath, root) != nullptr;
                else if (isFontSrc)    ok = FontImporter::import(srcPath, root)     != nullptr;

                if (!ok)
                    HE_LOG_ERROR(Editor, "%s",
                        ("Editor: import failed for " + srcPath.string()).c_str());
                ctx.contentRefreshPending = true;
            }
        }
        else // OpenProject
        {
            if (ctx.projectManager->loadProject(chosen))
            {
                ctx.globalState->addKnownProject(chosen);
                ctx.globalState->writeConfig();
                ctx.contentRefreshPending = true;
                ctx.projectLoaded = true;
            }
            else
            {
                ctx.hubOpenError = "Failed to load project file.";
                ImGui::OpenPopup("##EditorOpenError");
            }
        }
        s_pendingFileOp = PendingFileOp::OpenProject; // reset to default
    }

    // ── Scene shortcuts: Cmd/Ctrl+S save, Shift+Cmd/Ctrl+S save as ─────────
    {
        const ImGuiIO& kio = ImGui::GetIO();
        const bool mod = kio.KeyCtrl || kio.KeySuper;
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            if (kio.KeyShift) triggerSaveSceneAs();
            else              doSaveScene();
        }
        // Ctrl/Cmd+, opens Preferences (matches the Edit menu shortcut label).
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Comma, false))
            s_showPreferences = true;
    }

    if (!ctx.hubOpenError.empty())
    {
        EditorWidgets::pinDialogToEditorWindow();
        if (ImGui::BeginPopupModal("##EditorOpenError", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextUnformatted(ctx.hubOpenError.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0)))
            {
                ctx.hubOpenError.clear();
                ImGui::CloseCurrentPopup();
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
            ImGui::ListBox("##npPresets", &ctx.hubSelectedPreset,
                ProjectHubPanel::kPresetNames, ProjectHubPanel::kPresetCount, 5);
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
            if (ImGui::Button("Create", ImVec2(btnW, 0)))
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
                        static_cast<ProjectScriptLanguage>(ctx.hubSelectedLang));
                    if (ok)
                    {
                        const std::string& heprojPath = ctx.projectManager->currentProject().path;
                        ctx.globalState->addKnownProject(heprojPath);
                        ctx.globalState->writeConfig();
                        ctx.contentRefreshPending = true;
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        ctx.hubCreateError = "Failed to create project. Check path/permissions.";
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
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
        ImGui::PushStyleColor(ImGuiCol_WindowBg,   ImVec4(0.11f, 0.11f, 0.13f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Tab,        ImVec4(0.16f, 0.16f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.28f, 0.28f, 0.36f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabActive,  ImVec4(0.22f, 0.22f, 0.30f, 1.0f));

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
            auto forgetTabState = [](const AppContext::EditorTab& t){
                if (t.assetPath.empty()) return;
                if (tabHasUnsavedEdits(t.assetPath)) return;
                ScriptEditorPanel::forget(t.assetPath);
                CppClassEditorPanel::forget(t.assetPath);
                MaterialEditorPanel::forget(t.assetPath);
                UIEditorPanel::forget(t.assetPath);
                HorizonCodeClassPanel::forget(t.assetPath);
                InputAssetPanel::forget(t.assetPath);
                ParticleGraphEditorPanel::forget(t.assetPath);
                AnimatorStateMachineEditorPanel::forget(t.assetPath);
                StaticMeshEditorPanel::forget(t.assetPath);      // view-only, never dirty
                SkeletalMeshEditorPanel::forget(t.assetPath);    // view-only, never dirty
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

    // Post-PIE report window (drawn before the tab gating so it shows on any tab).
    PlayReportPanel::drawPlayReport(ctx);

    // ── Top-level tab gating ───────────────────────────────────────────────────
    // The built-in "Scene" tab (empty assetPath) shows the dockspace + all panels
    // below. A script tab instead fills that same area with its code editor and we
    // skip the scene panels for this frame. Default to the scene when the active
    // index is out of range. (All ImGui windows above are already balanced, so the
    // early return is safe; modals/menus render before this point.)
    const bool sceneTabActive =
        ctx.activeTab < 0 || ctx.activeTab >= static_cast<int>(ctx.tabs.size())
        || ctx.tabs[ctx.activeTab].assetPath.empty();
    if (!sceneTabActive)
    {
        // The scene viewport (and its RMB fly-look release) won't run this frame. If the
        // user switched here mid-look via a keyboard shortcut, force-release the capture so
        // the cursor isn't left hidden/pinned with ImGui mouse input disabled.
        ViewportPanel::releaseViewportLookCapture(ctx.window ? ctx.window->GetNativeWindow() : nullptr);

        const ImGuiViewport* vpTab = ImGui::GetMainViewport();
        const std::string& tabPath = ctx.tabs[ctx.activeTab].assetPath;
        const ImVec2 tabPos(vpTab->WorkPos.x, vpTab->WorkPos.y + kTabBarH);
        const ImVec2 tabSize(vpTab->WorkSize.x, vpTab->WorkSize.y - kFooterH - kTabBarH);
        // Dispatch by asset type: material assets get the node-graph editor, script
        // assets the code editor. (Cheap header sniff; both panels cache their state.)
        // The Level Script + Game Instance are virtual tabs (no backing .hasset).
        if (tabPath == LevelScriptPanel::kTabPath)
            LevelScriptPanel::render(ctx, tabPos, tabSize);
        else if (tabPath == GameInstancePanel::kTabPath)
            GameInstancePanel::render(ctx, tabPos, tabSize);
        else if (MaterialEditorPanel::isMaterialAsset(tabPath) ||
            MaterialEditorPanel::isMaterialFunctionAsset(tabPath))
            MaterialEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (UIEditorPanel::isWidgetAsset(tabPath))
            UIEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (HorizonCodeClassPanel::isClassAsset(tabPath))
            HorizonCodeClassPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (InputAssetPanel::isInputAsset(tabPath))
            InputAssetPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (SkeletalMeshEditorPanel::isSkeletalMeshAsset(tabPath))
            SkeletalMeshEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (StaticMeshEditorPanel::isStaticMeshAsset(tabPath))
            StaticMeshEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (ParticleGraphEditorPanel::isParticleAsset(tabPath))
            ParticleGraphEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (AnimatorStateMachineEditorPanel::isAnimatorStateMachineAsset(tabPath))
            AnimatorStateMachineEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        // C++ source/header (raw files, extension-based) → h/cpp class viewer. Must
        // come before the ScriptEditorPanel fallthrough, which assumes an HAsset.
        else if (CppClassEditorPanel::isCppSourceAsset(tabPath))
            CppClassEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else
            ScriptEditorPanel::render(ctx, tabPath, tabPos, tabSize);
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
        ImGui::Begin("##EditorDockSpace", nullptr,
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

        const ImGuiID dockspaceId = ImGui::GetID("##MainDockSpace");
        // Build the default layout on first run (no saved layout in imgui.ini) or
        // on demand via View > Reset Layout. A layout loaded from imgui.ini
        // otherwise always wins, so user customisations persist.
        if (s_resetLayoutRequested || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        {
            BuildDefaultDockLayout(dockspaceId, ImGui::GetContentRegionAvail());
            s_resetLayoutRequested = false;
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
void EditorUI::renderOverlays(AppContext& ctx, float dt)
{
    EditorSettingsPanel::DrawPreferencesWindow(ctx, s_showPreferences);
    ProfilerPanel::DrawProfilerWindow(ctx, s_showProfiler);
    EnvironmentPanel::DrawEnvironmentWindow(ctx, s_showEnvironment);
    CollabPanel::DrawCollabWindow(ctx, s_showCollab);

    TutorialPanel::UiFlags tutFlags;
    tutFlags.profilerOpen      = s_showProfiler;
    tutFlags.environmentOpen   = s_showEnvironment;
    tutFlags.exportOpen        = ExportDialogPanel::isOpen();
    tutFlags.preferencesOpen   = EditorSettingsPanel::preferencesOpen();
    tutFlags.importDialogOpens = s_importDialogOpens;
    tutFlags.contentRootKind   = ContentBrowserPanel::browsedRootKind();
    TutorialPanel::render(ctx, dt, tutFlags);
}
#endif // HE_IMGUI_ENABLED
