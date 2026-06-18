#include "EditorUI.h"
#include "EditorApplication.h"
#include <Hpak/ProjectExporter.h>
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/LODSystem.h>
#include <HorizonScene/NavigationSystem.h>
#include <Scripting/ScriptEngine.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <Math/AABB.h>
#include <glm/gtc/type_ptr.hpp>
#include "MeshImporter.h"
#include "TextureImporter.h"
#include "MaterialImporter.h"
#include "AudioImporter.h"

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#endif

#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <filesystem>
#include <string>
#include <sstream>
#include <vector>
#include <cstring>
#include <array>
#include <unordered_map>
#include <cmath>

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

// Builds the editor's default dock layout into the given dockspace node. Only
// called when the imgui.ini did not already provide a layout (DockBuilderGetNode
// == nullptr), so a saved arrangement always wins. Windows the ini doesn't place
// fall into this default instead of floating loose. Mirrors the panel layout in
// the reference screenshots: thin toolbar floats on top; Quick Settings left,
// World Outliner + Details stacked right, Content Browser bottom, Scene centre.
// Set by View > Reset Layout; consumed by the dockspace block in RenderEditor()
// to force a rebuild of the default layout even when a layout is already loaded.
static bool s_resetLayoutRequested = false;

// Toggled by Edit > Preferences (Ctrl+,); drives the Preferences window.
static bool s_showPreferences = false;

// Build > Export Project modal state
static bool   s_showExportModal   = false;
static char   s_exportOutputDir[512] = {};
static bool   s_exportCompress    = true;
static bool   s_exportEncrypt     = false;
static bool   s_exportRunning     = false;
static std::string s_exportResult;

// Active manipulation tool, shared by the viewport toolbar buttons and the
// W/E/R shortcuts and consumed by the gizmo (Move / Rotate / Scale).
static ImGuizmo::OPERATION s_gizmoOp   = ImGuizmo::TRANSLATE;
// Gizmo orientation: LOCAL (object axes) or WORLD (axis-aligned). Toolbar toggle.
static ImGuizmo::MODE      s_gizmoMode = ImGuizmo::LOCAL;
// Show ImGuizmo's outer "screen-space" rotation ring (rotate about the view axis).
// Off by default — its viewport-relative behaviour is confusing.
static bool                s_rotateScreen = false;

// Requested by fast local content edits (create/rename) that want the file list
// updated this frame without the heavyweight "##ContentRefresh" progress modal.
// Consumed at the top of render(), outside the content-folder shared lock.
static bool s_quietContentRefresh = false;

// Landscape sculpt tool state (shared between the panel and viewport)
enum class TerrainTool { Raise, Lower, Smooth };
static TerrainTool s_terrainTool     = TerrainTool::Raise;
static float       s_brushRadius     = 10.0f;  // inner full-strength radius (m)
static float       s_falloffRadius   = 5.0f;   // transition width — strength falls linearly to 0
static float       s_brushStrength   = 5.0f;
static bool        s_brushWasDown    = false;   // tracks LMB edge for undo

// Picking + sculpt AABB cache (keyed by mesh asset UUID)
static std::unordered_map<HE::UUID, HE::AABB> s_aabbCache;

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

// ─── Preferences window (Edit > Preferences) ────────────────────────────────
// Central place for editor settings. Values live in EditorConfig (a reference in
// AppContext) and are persisted to config.json on exit. Changes that need an
// immediate side effect (camera speed, vsync) are applied here on edit; the font
// scale is applied every frame from EditorConfig in render().

// VSync routing: OpenGL toggles the SDL swap interval on the Window, while
// Metal / Vulkan / D3D switch their present mode in the renderer. Call both so
// the toggle takes effect on every backend.
static void ApplyVSync(AppContext& ctx)
{
	if (ctx.window)   ctx.window->SetVSync(ctx.vsync);
	if (ctx.renderer) ctx.renderer->SetVSync(ctx.vsync);
}

// ─── Engine-settings catalog + Quick-Settings favourites ────────────────────
// One catalog of pinnable engine settings, rendered in two modes: Preferences
// shows every setting with a "pin" toggle; Quick Settings shows only the pinned
// ones. Favourites are a comma-separated list of stable keys in
// EditorConfig::QuickSettingsFavorites (persisted to config.json). (Scene
// environment settings are NOT here — those live on the World entity.)
enum class SettingsMode { Preferences, QuickSettings };

static bool isFavorite(const EditorConfig& cfg, const char* key)
{
	const std::string hay = "," + cfg.QuickSettingsFavorites + ",";
	return hay.find("," + std::string(key) + ",") != std::string::npos;
}
static void toggleFavorite(EditorConfig& cfg, const char* key)
{
	std::vector<std::string> keys;
	std::stringstream ss(cfg.QuickSettingsFavorites);
	std::string tok;
	bool had = false;
	while (std::getline(ss, tok, ','))
	{
		if (tok.empty()) continue;
		if (tok == key) { had = true; continue; } // drop it (toggle off)
		keys.push_back(tok);
	}
	if (!had) keys.push_back(key);              // add it (toggle on)
	cfg.QuickSettingsFavorites.clear();
	for (size_t i = 0; i < keys.size(); ++i)
		cfg.QuickSettingsFavorites += (i ? "," : "") + keys[i];
}

#ifdef HE_IMGUI_ENABLED
// Renders the engine-settings catalog. Each `row(key, category, widget)` is a
// logical setting group; `widget` draws its control(s).
static void DrawEngineSettings(AppContext& ctx, SettingsMode mode)
{
	EditorConfig& cfg = ctx.editorConfig;
	const char* lastCat = nullptr;
	int shown = 0;
	auto row = [&](const char* key, const char* cat, auto&& widget)
	{
		const bool fav = isFavorite(cfg, key);
		if (mode == SettingsMode::QuickSettings && !fav) return;
		if (!lastCat || std::strcmp(lastCat, cat) != 0) { ImGui::SeparatorText(cat); lastCat = cat; }
		if (mode == SettingsMode::Preferences)
		{
			ImGui::PushID(key);
			bool f = fav;
			if (ImGui::Checkbox("##pin", &f)) toggleFavorite(cfg, key);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(fav ? "Unpin from Quick Settings" : "Pin to Quick Settings");
			ImGui::PopID();
			ImGui::SameLine();
		}
		widget();
		++shown;
	};

	row("backend", "Renderer", [&]{
		ImGui::TextUnformatted("Backend");
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##backend", ctx.backendName.c_str()))
		{
			auto pick = [&](const char* label, HE::GraphicsAPI api){
				if (ImGui::Selectable(label)) { ctx.globalState->setSelectedRHI(api); ctx.backendName = getRHIName(api); }
			};
			pick("OpenGL", HE::GraphicsAPI::OpenGL);
			pick("Vulkan", HE::GraphicsAPI::Vulkan);
			pick("DirectX11", HE::GraphicsAPI::D3D11);
			pick("DirectX12", HE::GraphicsAPI::D3D12);
#ifdef __APPLE__
			pick("Metal", HE::GraphicsAPI::Metal);
#endif
			ImGui::EndCombo();
		}
	});
	row("vsync", "Renderer", [&]{ if (ImGui::Checkbox("VSync", &ctx.vsync)) ApplyVSync(ctx); });

	row("bloom", "Post-processing", [&]{
		ImGui::Checkbox("Bloom", &cfg.BloomEnabled);
		ImGui::BeginDisabled(!cfg.BloomEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Threshold", &cfg.BloomThreshold, 0.0f, 4.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Intensity", &cfg.BloomIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();
	});
	row("ssao", "Post-processing", [&]{
		ImGui::Checkbox("SSAO", &cfg.SSAOEnabled);
		ImGui::BeginDisabled(!cfg.SSAOEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("SSAO Radius", &cfg.SSAORadius, 0.05f, 2.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("SSAO Intensity", &cfg.SSAOIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();
	});

	row("camspeed", "Viewport", [&]{
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::SliderFloat("Camera Speed", &cfg.EditorCameraSpeed, 1.0f, 50.0f, "%.1f u/s") && ctx.editorCamera)
			ctx.editorCamera->setFlySpeed(cfg.EditorCameraSpeed);
	});

	row("fontscale", "Appearance", [&]{
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("UI Font Scale", &cfg.UiFontScale, 0.5f, 2.0f, "%.2fx");
	});

	row("cpucache", "Content Browser", [&]{ ImGui::Checkbox("Keep CPU Asset Cache", &cfg.KeepCPUAssets); });
	row("cbrefresh", "Content Browser", [&]{
		ImGui::SetNextItemWidth(120.0f);
		ImGui::InputInt("Refresh Interval (s)", &cfg.ContentBrowserRefreshRate, 0, 0);
		if (cfg.ContentBrowserRefreshRate < 0) cfg.ContentBrowserRefreshRate = 0;
	});

	if (mode == SettingsMode::QuickSettings && shown == 0)
		ImGui::TextDisabled("Pin engine settings in Preferences\n(Edit \xe2\x96\xb8 Preferences) to show them here.");
}
#endif // HE_IMGUI_ENABLED

static void DrawPreferencesWindow(AppContext& ctx, bool& open)
{
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
	                        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	if (ImGui::Begin("Preferences", &open, ImGuiWindowFlags_NoCollapse))
	{
		EditorConfig& cfg = ctx.editorConfig;

		ImGui::TextDisabled("Tick the pin on a setting to show it in Quick Settings.");
		ImGui::Spacing();

		// The full engine-settings catalog (each row has a pin toggle here).
		DrawEngineSettings(ctx, SettingsMode::Preferences);

		ImGui::Separator();
		ImGui::TextDisabled("Preferences are saved when the editor exits.");
		if (ImGui::Button("Restore Defaults"))
		{
			cfg.UiFontScale       = 1.0f;
			cfg.EditorCameraSpeed = 6.0f;
			cfg.KeepCPUAssets     = false;
			cfg.ContentBrowserRefreshRate = 60;
			cfg.BloomEnabled      = true;
			cfg.BloomThreshold    = 1.0f;
			cfg.BloomIntensity    = 0.6f;
			cfg.SSAOEnabled       = true;
			cfg.SSAORadius        = 0.5f;
			cfg.SSAOIntensity     = 1.0f;
			if (ctx.editorCamera) ctx.editorCamera->setFlySpeed(cfg.EditorCameraSpeed);
		}
		ImGui::SameLine();
		if (ImGui::Button("Close")) open = false;
	}
	ImGui::End();
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
    case RendererFactory::Backend::OpenGL:
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
#ifdef _WIN32
    case RendererFactory::Backend::D3D11:
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
    case RendererFactory::Backend::D3D12:
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
    case RendererFactory::Backend::Vulkan:
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
#endif
#ifdef HE_IMGUI_METAL_ENABLED
    case RendererFactory::Backend::Metal:
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
    // Runs here, before RenderEditor acquires the content-folder shared lock, so
    // refreshContentFolder()'s unique_lock can't deadlock against it.
    if (s_quietContentRefresh && ctx.globalState)
    {
        ctx.globalState->refreshContentFolder();
        s_quietContentRefresh = false;
    }

    // ── Route to either the Project Hub or the full Editor UI ─────────────────
    if (ctx.projectLoaded)
        RenderEditor(ctx, dt);
    else
        RenderProjectHub(ctx);

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
    }
#endif // HE_IMGUI_ENABLED
}

// ─── Project Hub ──────────────────────────────────────────────────────────────
void EditorUI::RenderProjectHub(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);

    const ImGuiWindowFlags kHubFlags =
        ImGuiWindowFlags_NoTitleBar   |
        ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoScrollbar  |
        ImGuiWindowFlags_NoCollapse   |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##ProjectHub", nullptr, kHubFlags);
    ImGui::PopStyleVar(3);

    // ── Header bar ────────────────────────────────────────────────────────────
    const float headerH = 56.0f;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    ImGui::BeginChild("##HubHeader", ImVec2(vp->Size.x, headerH), false,
        ImGuiWindowFlags_NoScrollbar);

    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    const char* title = "HorizonEngine  —  Project Hub";

    const float logoDisplayH = headerH - 16.0f;
    if (ctx.logoTexture && ctx.logoW > 0 && ctx.logoH > 0)
    {
        const float logoDisplayW = logoDisplayH * (static_cast<float>(ctx.logoW) / static_cast<float>(ctx.logoH));
        ImGui::SetCursorPos(ImVec2(12.0f, (headerH - logoDisplayH) * 0.5f));
        ImGui::Image(ctx.logoTexture, ImVec2(logoDisplayW, logoDisplayH));
        ImGui::SameLine();
        ImGui::SetCursorPosY((headerH - ImGui::GetTextLineHeight()) * 0.5f);
    }
    else
    {
        ImGui::SetCursorPos(ImVec2(20.0f, (headerH - ImGui::GetTextLineHeight()) * 0.5f));
    }
    ImGui::Text("%s", title);
    if (ctx.fontHeading) ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ── Three panels ──────────────────────────────────────────────────────────
    const float bodyY   = headerH + 1.0f;
    const float bodyH   = vp->Size.y - bodyY;
    const float panelW  = vp->Size.x / 3.0f;
    const float padding = 16.0f;

    static const std::array<const char*, 4> kPresetNames = {
        "Empty Project",
        "Game",
        "Simulation",
        "Tool",
    };
    static const std::array<const char*, 4> kPresetDesc = {
        "Only the basic folder skeleton, no extra content.",
        "Assets, Scenes and Scripts folders + a sample scene file.",
        "Assets, Scenes and Data folders.",
        "Assets and Source folders.",
    };

    // ════════════════════════════════════════════════════════════════════
    // PANEL 1 — Create Project
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(0.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild("##PanelCreate", ImVec2(panelW - 1.0f, bodyH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(padding, padding));
    if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
    ImGui::Text("Create Project");
    if (ctx.fontSubheading) ImGui::PopFont();
    ImGui::SetCursorPosX(padding);
    ImGui::Separator();
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    ImGui::SetCursorPosX(padding);
    ImGui::Text("Template");
    ImGui::SetCursorPosX(padding);
    ImGui::PushItemWidth(panelW - padding * 2.0f);
    ImGui::ListBox("##Presets", &ctx.hubSelectedPreset,
        kPresetNames.data(), static_cast<int>(kPresetNames.size()), 4);
    ImGui::PopItemWidth();

    ImGui::SetCursorPosX(padding);
    ImGui::TextDisabled("%s", kPresetDesc[ctx.hubSelectedPreset]);

    ImGui::Spacing();
    ImGui::SetCursorPosX(padding);
    ImGui::Text("Project Name");
    ImGui::SetCursorPosX(padding);
    ImGui::PushItemWidth(panelW - padding * 2.0f);
    ImGui::InputText("##ProjName", ctx.hubProjectName, ctx.hubProjectNameSize);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::SetCursorPosX(padding);
    ImGui::Text("Project Directory");
    ImGui::SetCursorPosX(padding);
    ImGui::PushItemWidth(panelW - padding * 2.0f - 70.0f);
    ImGui::InputText("##ProjDir", ctx.hubProjectDir, ctx.hubProjectDirSize);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse##dir"))
    {
#ifdef _WIN32
        {
            IFileOpenDialog* pDlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg))))
            {
                DWORD dwOpts = 0;
                pDlg->GetOptions(&dwOpts);
                pDlg->SetOptions(dwOpts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

                HWND hwnd = nullptr;
                if (ctx.window)
                {
                    hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                        SDL_GetWindowProperties(ctx.window->GetNativeWindow()),
                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
                }

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
                            if (len > 0 && len <= (int)ctx.hubProjectDirSize)
                            {
                                WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
                                    ctx.hubProjectDir, ctx.hubProjectDirSize,
                                    nullptr, nullptr);
                            }
                            CoTaskMemFree(pPath);
                        }
                        pItem->Release();
                    }
                }
                pDlg->Release();
            }
        }
#else
        SDL_ShowOpenFolderDialog(
            [](void* userdata, const char* const* filelist, int /*filter*/)
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
            nullptr,
            false);
#endif
    }

    if (ctx.pendingDirReady)
    {
        strncpy(ctx.hubProjectDir, ctx.pendingDirResult.c_str(),
                ctx.hubProjectDirSize - 1);
        ctx.hubProjectDir[ctx.hubProjectDirSize - 1] = '\0';
        ctx.pendingDirReady  = false;
        ctx.pendingDirResult.clear();
    }

    ImGui::Spacing();
    if (!ctx.hubCreateError.empty())
    {
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", ctx.hubCreateError.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::SetCursorPosX(padding);
    const float btnW = panelW - padding * 2.0f;
    if (ImGui::Button("Create##create", ImVec2(btnW, 36.0f)))
    {
        ctx.hubCreateError.clear();
        std::string name = ctx.hubProjectName;
        std::string dir  = ctx.hubProjectDir;
        if (name.empty())
        {
            ctx.hubCreateError = "Please enter a project name.";
        }
        else if (dir.empty())
        {
            ctx.hubCreateError = "Please select a project directory.";
        }
        else
        {
            std::filesystem::path projRoot = std::filesystem::path(dir) / name;
            bool ok = ctx.projectManager->createNewProject(
                projRoot.string(), name,
                static_cast<ProjectPreset>(ctx.hubSelectedPreset));
            if (ok)
            {
                const std::string& heprojPath = ctx.projectManager->currentProject().path;
                ctx.globalState->addKnownProject(heprojPath);
                ctx.globalState->writeConfig();
                ctx.contentRefreshPending = true;
                ctx.projectLoaded = true;
            }
            else
            {
                ctx.hubCreateError = "Failed to create project. Check path/permissions.";
            }
        }
    }

    if (ctx.fontBody) ImGui::PopFont();
    ImGui::EndChild();

    // ════════════════════════════════════════════════════════════════════
    // PANEL 2 — Known Projects
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(panelW + 1.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.13f, 1.0f));
    ImGui::BeginChild("##PanelKnown", ImVec2(panelW - 2.0f, bodyH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(padding, padding));
    if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
    ImGui::Text("Recent Projects");
    if (ctx.fontSubheading) ImGui::PopFont();
    ImGui::SetCursorPosX(padding);
    ImGui::Separator();
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    const auto& known = ctx.globalState->getKnownProjects();
    if (known.empty())
    {
        ImGui::SetCursorPosX(padding);
        ImGui::TextDisabled("No recent projects.");
    }
    else
    {
        const float listAreaH = bodyH - 90.0f;
        ImGui::SetCursorPosX(padding);
        ImGui::BeginChild("##KnownList",
            ImVec2(panelW - padding * 2.0f, listAreaH), true);

        for (int i = 0; i < static_cast<int>(known.size()); ++i)
        {
            std::filesystem::path p(known[i]);
            std::string name = p.stem().string();
            if (name.empty()) name = known[i];
            std::string label = name + "\n" + known[i];
            bool exists = std::filesystem::exists(p);

            ImGui::PushID(i);

            if (!exists)
            {
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.4f, 0.15f, 0.15f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.5f, 0.18f, 0.18f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.45f,0.16f, 0.16f, 1.0f));
            }

            ImGui::Selectable(label.c_str(), false,
                ImGuiSelectableFlags_None, ImVec2(0, 40.0f));

            if (!exists) ImGui::PopStyleColor(4);

            if (ImGui::IsItemClicked() && exists && ctx.projectManager->loadProject(known[i]))
            {
                ctx.globalState->addKnownProject(known[i]);
                ctx.globalState->writeConfig();
                ctx.contentRefreshPending = true;
                ctx.projectLoaded = true;
            }

            if (ImGui::BeginPopupContextItem("##KnownCtx"))
            {
                if (ImGui::MenuItem("Aus Liste entfernen"))
                {
                    ctx.hubRemoveIndex     = i;
                    ctx.hubRemoveRequested = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::Separator();
        }

        if (ctx.hubRemoveRequested)
        {
            ctx.hubRemoveRequested = false;
            ImGui::OpenPopup("##ConfirmRemove");
        }
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("##ConfirmRemove", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            if (ctx.hubRemoveIndex >= 0 && ctx.hubRemoveIndex < static_cast<int>(known.size()))
            {
                std::filesystem::path rp(known[ctx.hubRemoveIndex]);
                ImGui::Text("Projekt aus der Liste entfernen?");
                ImGui::Spacing();
                ImGui::TextDisabled("%s", rp.stem().string().c_str());
                ImGui::TextDisabled("%s", known[ctx.hubRemoveIndex].c_str());
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("Entfernen", ImVec2(120, 0)))
                {
                    ctx.globalState->removeKnownProject(known[ctx.hubRemoveIndex]);
                    ctx.globalState->writeConfig();
                    ctx.hubRemoveIndex = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Abbrechen", ImVec2(120, 0)))
                {
                    ctx.hubRemoveIndex = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                ctx.hubRemoveIndex = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    if (ctx.fontBody) ImGui::PopFont();
    ImGui::EndChild();

    // ════════════════════════════════════════════════════════════════════
    // PANEL 3 — Open Project
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(panelW * 2.0f + 1.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild("##PanelOpen", ImVec2(panelW, bodyH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(padding, padding));
    if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
    ImGui::Text("Open Project");
    if (ctx.fontSubheading) ImGui::PopFont();
    ImGui::SetCursorPosX(padding);
    ImGui::Separator();
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    ImGui::SetCursorPosX(padding);
    ImGui::TextWrapped(
        "Select an existing HorizonEngine project file (.heproj) "
        "to open it in the editor.");
    ImGui::Spacing();

    if (!ctx.hubOpenError.empty())
    {
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", ctx.hubOpenError.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::SetCursorPosX(padding);
    if (ImGui::Button("Browse .heproj...", ImVec2(panelW - padding * 2.0f, 36.0f)))
    {
        ctx.hubOpenError.clear();
        SDL_DialogFileFilter filters[] = {
            { "HorizonEngine Project", "heproj" },
        };
        SDL_ShowOpenFileDialog(
            [](void* userdata, const char* const* filelist, int /*filter*/)
            {
                auto* b = static_cast<SDLDialogBridge*>(userdata);
                if (filelist && filelist[0])
                {
                    *b->pendingFileResult = filelist[0];
                    *b->pendingFileReady  = true;
                }
            },
            ctx.dialogBridge,
            ctx.window ? ctx.window->GetNativeWindow() : nullptr,
            filters, 1,
            nullptr,
            false);
    }

    if (ctx.pendingFileReady)
    {
        ctx.pendingFileReady = false;
        std::string chosen = ctx.pendingFileResult;
        ctx.pendingFileResult.clear();
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
        }
    }

    if (ctx.fontBody) ImGui::PopFont();
    ImGui::EndChild();

    ImGui::End();
#endif // HE_IMGUI_ENABLED
}

// ─── Full Editor UI ───────────────────────────────────────────────────────────
void EditorUI::RenderEditor(AppContext& ctx, float dt)
{
#ifdef HE_IMGUI_ENABLED
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
	auto requestGuarded = [&](GuardedAction a, const std::string& arg = std::string{})
	{
		if (!ctx.sceneDirty) { runGuardedAction(a, arg); return; }
		s_guardAction      = a;
		s_guardArg         = arg;
		s_guardSaveThenAct = false;
		s_openUnsavedModal = true;
	};

	// An OS-level close (window X / Cmd+Q) that EditorApplication vetoed because of
	// unsaved changes surfaces here as a guarded Quit request.
	if (ctx.exitRequested)
	{
		ctx.exitRequested = false;
		requestGuarded(GuardedAction::Quit);
	}

	ImGui::PushFont(ctx.fontSubheading);
	ImGui::BeginMainMenuBar();
	bool openNewProjectPopup = false;
	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("New Project", "Ctrl+N"))
		{
			ctx.hubProjectName[0] = '\0';
			ctx.hubProjectDir[0]  = '\0';
			ctx.hubSelectedPreset = 0;
			ctx.hubCreateError.clear();
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
        if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
        if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
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
        if (ImGui::MenuItem("Reset Layout")) s_resetLayoutRequested = true;
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
		{
			if (s_exportOutputDir[0] == '\0' && ctx.projectManager)
			{
				const auto& proj = ctx.projectManager->currentProject();
				const auto outPath = std::filesystem::path(proj.path) / "Export";
				std::strncpy(s_exportOutputDir, outPath.string().c_str(), sizeof(s_exportOutputDir) - 1);
			}
			s_exportResult.clear();
			s_exportRunning = false;
			s_showExportModal = true;
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help"))
	{
		if (ImGui::MenuItem("Documentation")) {}
		if (ImGui::MenuItem("About")) {}
		ImGui::EndMenu();
	}
    ImGui::EndMainMenuBar();
    ImGui::PopFont();

    if (openNewProjectPopup)
        ImGui::OpenPopup("##NewProjectPopup");

    // ── Export Project modal ────────────────────────────────────────────────
    if (s_showExportModal)
    {
        ImGui::OpenPopup("Export Project##build");
        s_showExportModal = false;
    }
    {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Export Project##build", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::Text("Output Directory:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##exportDir", s_exportOutputDir, sizeof(s_exportOutputDir));
            ImGui::Spacing();
            ImGui::Checkbox("Compress assets (LZ4)", &s_exportCompress);
            ImGui::Checkbox("Encrypt assets",        &s_exportEncrypt);
            if (s_exportEncrypt)
                ImGui::TextDisabled("Note: encryption key management is the project's responsibility.");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            if (!s_exportResult.empty())
            {
                const bool ok = s_exportResult.rfind("OK:", 0) == 0;
                if (ok) ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.f), "%s", s_exportResult.c_str());
                else    ImGui::TextColored(ImVec4(1.f,  0.3f, 0.3f, 1.f), "%s", s_exportResult.c_str());
                ImGui::Spacing();
            }

            const bool canExport = s_exportOutputDir[0] != '\0'
                                && ctx.contentManager && !s_exportRunning;
            if (!canExport) ImGui::BeginDisabled();
            if (ImGui::Button("Export", ImVec2(110, 0)))
            {
                s_exportRunning = true;
                const std::string projName = ctx.projectManager
                    ? ctx.projectManager->currentProject().name : "Game";
                const std::string contentDir = ctx.contentManager
                    ? ctx.contentManager->contentRoot() : "";
                std::string sceneName;
                if (!ctx.currentScenePath.empty())
                    sceneName = std::filesystem::path(ctx.currentScenePath).filename().string();

                ExportSettings es;
                es.compress = s_exportCompress;
                es.encrypt  = s_exportEncrypt;
                // Resolve game runtime dir from editor executable location (../Game/)
                if (const char* base = SDL_GetBasePath())
                    es.gameRuntimeDir = std::filesystem::path(base) / ".." / "Game";
                const auto res = ProjectExporter::exportProject(
                    contentDir, projName, sceneName,
                    std::filesystem::path(s_exportOutputDir), es);

                if (res.success)
                    s_exportResult = "OK: " + std::to_string(res.assetsPacked)
                                   + " asset(s) packed, " + std::to_string(res.binaryFilesCopied)
                                   + " binary file(s) → " + s_exportOutputDir;
                else
                    s_exportResult = "Error: " + res.errorMessage;
                s_exportRunning = false;
            }
            if (!canExport) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(80, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ── Unsaved-changes modal ───────────────────────────────────────────────
    // Raised by requestGuarded() when a scene-discarding action is attempted with
    // a dirty scene. Save → write (Save-As if untitled) then run the action;
    // Don't Save → run it straight away; Cancel → abandon it.
    if (s_openUnsavedModal)
    {
        ImGui::OpenPopup("Unsaved Changes##scene");
        s_openUnsavedModal = false;
    }
    {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Unsaved Changes##scene", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            const std::string sceneName = ctx.currentScenePath.empty()
                ? std::string("Untitled")
                : std::filesystem::path(ctx.currentScenePath).stem().string();
            ImGui::Text("Save changes to \"%s\" before continuing?", sceneName.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Your unsaved changes will be lost otherwise.");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            // Snapshot the stashed action — the buttons may clear it.
            const GuardedAction action = s_guardAction;
            const std::string   arg    = s_guardArg;

            if (ImGui::Button("Save", ImVec2(110, 0)))
            {
                const bool hadPath = !ctx.currentScenePath.empty();
                doSaveScene(); // synchronous if a path exists, else async Save-As
                if (hadPath)
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
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(110, 0)))
            {
                runGuardedAction(action, arg);
                s_guardAction = GuardedAction::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                s_guardAction      = GuardedAction::None;
                s_guardSaveThenAct = false;
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

                const std::filesystem::path root(ctx.contentManager->contentRoot());
                bool ok = false;
                if      (isMeshSrc)    ok = MeshImporter::import(srcPath, root)     != nullptr;
                else if (isTextureSrc) ok = TextureImporter::import(srcPath, root)  != nullptr;
                else if (isAudioSrc)   ok = AudioImporter::import(srcPath, root)    != nullptr;
                else if (isMatSrc)     ok = MaterialImporter::import(srcPath, root) != nullptr;

                if (!ok)
                    Logger::Log(Logger::LogLevel::Error,
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
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
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
        static const std::array<const char*, 4> kPresetNames = {
            "Empty Project", "Game", "Simulation", "Tool",
        };
        static const std::array<const char*, 4> kPresetDesc = {
            "Only the basic folder skeleton, no extra content.",
            "Assets, Scenes and Scripts folders + a sample scene file.",
            "Assets, Scenes and Data folders.",
            "Assets and Source folders.",
        };

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Always);
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
                kPresetNames.data(), static_cast<int>(kPresetNames.size()), 4);
            ImGui::TextDisabled("%s", kPresetDesc[ctx.hubSelectedPreset]);

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
                        static_cast<ProjectPreset>(ctx.hubSelectedPreset));
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

        ImGui::Begin("##EditorFooter", nullptr,
            ImGuiWindowFlags_NoTitleBar         |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoScrollbar        |
            ImGuiWindowFlags_NoSavedSettings    |
            ImGuiWindowFlags_NoDocking          |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav              |
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

		// Right — FPS (drawn before SameLine so GetWindowWidth() is stable)
		const std::string fpsText = "FPS: " + std::to_string(static_cast<int>(ctx.smoothFps));
		const float       fpsW   = ImGui::CalcTextSize(fpsText.c_str()).x;
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

        ImGui::Begin("##EditorTabBar", nullptr,
            ImGuiWindowFlags_NoTitleBar         |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoScrollbar        |
            ImGuiWindowFlags_NoSavedSettings    |
            ImGuiWindowFlags_NoDocking          |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav              |
            ImGuiWindowFlags_NoDecoration);

        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(4);

        // ── Tab state (owned by AppContext / EditorApplication) ───────────
        auto& s_tabs      = ctx.tabs;
        auto& s_activeTab = ctx.activeTab;

        if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

        if (ImGui::BeginTabBar("##MainTabBar",
            ImGuiTabBarFlags_Reorderable |
            ImGuiTabBarFlags_FittingPolicyScroll |
            ImGuiTabBarFlags_NoCloseWithMiddleMouseButton))
        {
            for (int i = 0; i < static_cast<int>(s_tabs.size()); )
            {
                auto& tab = s_tabs[i];
                if (!tab.open) { s_tabs.erase(s_tabs.begin() + i); continue; }

                ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
                if (i == s_activeTab) flags |= ImGuiTabItemFlags_SetSelected;

                bool pOpen = tab.closable ? tab.open : true;
                if (ImGui::BeginTabItem(tab.label.c_str(), tab.closable ? &pOpen : nullptr, flags))
                {
                    s_activeTab = i;
                    ImGui::EndTabItem();
                }
                if (tab.closable) tab.open = pOpen;
                ++i;
            }
            // Remove closed tabs
            s_tabs.erase(
                std::remove_if(s_tabs.begin(), s_tabs.end(),
                    [](const AppContext::EditorTab& t){ return t.closable && !t.open; }),
                s_tabs.end());

            ImGui::EndTabBar();
        }

        if (ctx.fontBody) ImGui::PopFont();

        ImGui::End();
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

        ImGui::Begin("##EditorDockSpace", nullptr,
            ImGuiWindowFlags_NoTitleBar         |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoScrollbar        |
            ImGuiWindowFlags_NoSavedSettings    |
            ImGuiWindowFlags_NoFocusOnAppearing |
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
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		// NoMove is essential: the viewport content is a plain ImGui::Image (an
		// id-less item), so a click-drag on it would otherwise be treated as a
		// click on empty window space and start an ImGui window/dock move. That
		// move fights the ImGuizmo drag for the same mouse button, so the gizmo
		// never manipulates the object (translate/rotate/scale all dead). The
		// docked Scene window is still relocated via its tab, so NoMove costs
		// nothing here.
		ImGui::Begin("Scene", nullptr,
		             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		             ImGuiWindowFlags_NoMove);
		ImGui::PopStyleVar();

		ImVec2 avail = ImGui::GetContentRegionAvail();

		// HE_VIEWPORT_RESIZE_STRESS=1 oscillates the viewport size every frame
		// to stress-test render-target recreation — a crash here means a
		// texture-lifetime bug in the backend (retired textures must outlive
		// the ImGui draw list that references them).
		static const bool kResizeStress = std::getenv("HE_VIEWPORT_RESIZE_STRESS") != nullptr;
		if (kResizeStress)
		{
			static int s_stressFrame = 0;
			++s_stressFrame;
			avail.x = std::max(64.0f, avail.x - static_cast<float>((s_stressFrame % 13) * 16));
			avail.y = std::max(64.0f, avail.y - static_cast<float>((s_stressFrame %  7) * 16));
		}

		if (ctx.renderer && avail.x >= 1.0f && avail.y >= 1.0f)
		{
			// Render at framebuffer resolution (HiDPI aware)
			const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
			ctx.renderer->SetViewportSize(
				static_cast<uint32_t>(avail.x * fbScale.x),
				static_cast<uint32_t>(avail.y * fbScale.y));

			if (void* tex = ctx.renderer->GetViewportTexture())
			{
				// OpenGL FBO textures have a bottom-left origin — flip vertically
				const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
				ImGui::Image(reinterpret_cast<ImTextureID>(tex), avail,
				             flipY ? ImVec2(0, 1) : ImVec2(0, 0),
				             flipY ? ImVec2(1, 0) : ImVec2(1, 1));

				const ImVec2 rectMin = ImGui::GetItemRectMin();
				const ImVec2 rectMax = ImGui::GetItemRectMax();
				const bool viewportHovered = ImGui::IsItemHovered();
				ImGuiIO& io = ImGui::GetIO();

				// ── Editor camera: drive from viewport input ────────────────
				// In play mode the game's scene camera takes over, so the
				// override is cleared and editor navigation is disabled.
				bool navigating = false;
				if (ctx.editorCamera && ctx.isPlaying)
				{
					ctx.renderer->SetEditorCamera(EditorCameraOverride{}); // active=false
				}
				else if (ctx.editorCamera)
				{
					EditorCamera& cam = *ctx.editorCamera;
					const bool imageHovered = ImGui::IsItemHovered();
					const bool rmb    = ImGui::IsMouseDown(ImGuiMouseButton_Right);
					const bool mmb    = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
					const bool altLmb = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
					const bool anyNav = rmb || mmb || altLmb;

					// Latch navigation so a drag keeps going if the cursor leaves the image.
					static bool s_navActive = false;
					if (imageHovered && anyNav) s_navActive = true;
					if (!anyNav)                s_navActive = false;
					navigating = s_navActive;

					// RMB cursor capture: hide + lock while fly-looking, restore on release.
					static bool   s_rmbCaptured = false;
					static ImVec2 s_rmbStartPos{};
					if (SDL_Window* sdlWin = ctx.window ? ctx.window->GetNativeWindow() : nullptr)
					{
						if (rmb && imageHovered && !s_rmbCaptured)
						{
							s_rmbStartPos = io.MousePos;
							SDL_HideCursor();
							SDL_SetWindowRelativeMouseMode(sdlWin, true);
							s_rmbCaptured = true;
						}
						else if (!rmb && s_rmbCaptured)
						{
							SDL_SetWindowRelativeMouseMode(sdlWin, false);
							SDL_ShowCursor();
							SDL_WarpMouseInWindow(sdlWin, s_rmbStartPos.x, s_rmbStartPos.y);
							s_rmbCaptured = false;
						}
					}

					EditorCamera::Input cin;
					cin.dt             = dt;
					cin.viewportHeight = avail.y;
					if (navigating)
					{
						cin.orbit      = altLmb;
						cin.pan        = mmb && !altLmb;
						cin.look       = rmb && !altLmb;
						cin.mouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
						if (cin.look)
						{
							cin.fast = io.KeyShift;
							if (ImGui::IsKeyDown(ImGuiKey_D)) cin.moveAxis.x += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_A)) cin.moveAxis.x -= 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_E)) cin.moveAxis.y += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_Q)) cin.moveAxis.y -= 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_W)) cin.moveAxis.z += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_S)) cin.moveAxis.z -= 1.0f;
						}
					}
					// Wheel zoom works on hover without holding a button.
					if (imageHovered) cin.wheel = io.MouseWheel;

					// Focus on selection (F) — frame the selected entity.
					if (imageHovered && !io.WantTextInput && !navigating &&
					    ImGui::IsKeyPressed(ImGuiKey_F) &&
					    ctx.world && ctx.selectedEntity != entt::null &&
					    ctx.world->registry().valid(ctx.selectedEntity))
					{
						if (auto* t = ctx.world->registry().try_get<TransformComponent>(ctx.selectedEntity))
						{
							const glm::vec3 center = glm::vec3(t->worldMatrix[3]);
							const float     radius = glm::length(t->scale) * 0.75f + 0.5f;
							cam.focusOn(center, radius);
						}
					}

					cam.update(cin);
					// Push to the backend so this frame's render uses it.
					ctx.renderer->SetEditorCamera(cam.makeOverride());
				}

				// Camera + object snapshot, identical to what the backend
				// renders with (extractor recomputes world matrices). The
				// editor camera overrides any scene camera so the gizmo and
				// picking ray match exactly what is on screen.
				static RenderExtractor s_extractor;
				static RenderWorld     s_sceneSnapshot;
				const EditorCameraOverride camOverride =
					(ctx.editorCamera && !ctx.isPlaying) ? ctx.editorCamera->makeOverride()
					                                     : EditorCameraOverride{};
				if (ctx.world)
					s_extractor.extract(*ctx.world, s_sceneSnapshot, avail.x / avail.y,
					                    camOverride.active ? &camOverride : nullptr);


				// Suppress the gizmo while the camera is being driven so Alt+LMB
				// orbit and RMB fly-look don't fight the manipulator.
				ImGuizmo::Enable(!navigating && !io.KeyAlt);

				// ── Gizmo on the selected entity ────────────────────────────
				bool gizmoActive = false;
				if (ctx.world && ctx.selectedEntity != entt::null &&
				    ctx.world->registry().valid(ctx.selectedEntity))
				if (auto* t = ctx.world->registry().try_get<TransformComponent>(ctx.selectedEntity))
				{
					// W/E/R switch operation while the viewport is hovered
					// (but not while flying — W/A/S/D drive the camera then). The
					// viewport toolbar's Move/Rotate/Scale buttons set the same
					// shared s_gizmoOp.
					if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput && !navigating)
					{
						if (ImGui::IsKeyPressed(ImGuiKey_W)) s_gizmoOp = ImGuizmo::TRANSLATE;
						if (ImGui::IsKeyPressed(ImGuiKey_E)) s_gizmoOp = ImGuizmo::ROTATE;
						if (ImGui::IsKeyPressed(ImGuiKey_R)) s_gizmoOp = ImGuizmo::SCALE;
					}

					ImGuizmo::SetOrthographic(false);
					ImGuizmo::SetDrawlist();
					ImGuizmo::SetRect(rectMin.x, rectMin.y,
					                  rectMax.x - rectMin.x, rectMax.y - rectMin.y);

					// Pre-state for undo — captured while hovering, before
					// the first Manipulate of a drag session mutates anything.
					if (ctx.undoSys && !ImGuizmo::IsUsing())
						ctx.undoSys->capturePre();

					// For rotation, optionally drop ImGuizmo's outer screen-space
					// ring (rotate about the view axis) — it's the confusing white
					// circle. Toggled from the toolbar.
					ImGuizmo::OPERATION effectiveOp = s_gizmoOp;
					if (s_gizmoOp == ImGuizmo::ROTATE && !s_rotateScreen)
						effectiveOp = ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z;

					glm::mat4 world = t->worldMatrix;
					ImGuizmo::Manipulate(
						&s_sceneSnapshot.camera.view[0][0],
						&s_sceneSnapshot.camera.projection[0][0],
						effectiveOp, s_gizmoMode, &world[0][0]);

					// Undo session: one entry per drag
					static bool s_gizmoWasUsing = false;
					if (ctx.undoSys)
					{
						if (ImGuizmo::IsUsing() && !s_gizmoWasUsing) ctx.undoSys->stashPre();
						if (!ImGuizmo::IsUsing() && s_gizmoWasUsing) ctx.undoSys->commitPending();
					}
					s_gizmoWasUsing = ImGuizmo::IsUsing();

					gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

					if (ImGuizmo::IsUsing())
					{
						// world → local: divide out the parent's world matrix
						glm::mat4 parentWorld(1.0f);
						if (auto* h = ctx.world->registry().try_get<HierarchyComponent>(ctx.selectedEntity);
						    h && h->parent != entt::null)
							if (auto* pt = ctx.world->registry().try_get<TransformComponent>(h->parent))
								parentWorld = pt->worldMatrix;
						glm::mat4 local = glm::inverse(parentWorld) * world;

						float pos[3], rot[3], scale[3];
						ImGuizmo::DecomposeMatrixToComponents(&local[0][0], pos, rot, scale);
						t->position = { pos[0],   pos[1],   pos[2]   };
						t->rotation = { rot[0],   rot[1],   rot[2]   };
						t->scale    = { scale[0], scale[1], scale[2] };
						t->dirty    = true;
					}
				}

				// ── Picking: click in the viewport selects the hit entity ──
				if (ctx.world && !gizmoActive && !navigating && !io.KeyAlt &&
				    ImGui::IsItemClicked(ImGuiMouseButton_Left))
				{
					const ImVec2 mouse = ImGui::GetMousePos();
					const float  u = (mouse.x - rectMin.x) / (rectMax.x - rectMin.x);
					const float  v = (mouse.y - rectMin.y) / (rectMax.y - rectMin.y);

					// Unproject the click to a world-space ray
					const glm::mat4 invVP = glm::inverse(
						s_sceneSnapshot.camera.projection * s_sceneSnapshot.camera.view);
					const glm::vec4 ndcNear(2.0f * u - 1.0f, 1.0f - 2.0f * v, -1.0f, 1.0f);
					const glm::vec4 ndcFar (ndcNear.x, ndcNear.y, 1.0f, 1.0f);
					glm::vec4 pNear = invVP * ndcNear; pNear /= pNear.w;
					glm::vec4 pFar  = invVP * ndcFar;  pFar  /= pFar.w;
					const glm::vec3 rayOrigin(pNear);
					const glm::vec3 rayDir(glm::vec3(pFar) - glm::vec3(pNear));

					// Local-space AABBs per mesh asset, cached (file-scope).
					// Entities without an asset use the built-in fallback cube's box.
					static const HE::AABB s_cubeBox = []{
						HE::AABB b; b.expand({-0.5f,-0.5f,-0.5f}); b.expand({0.5f,0.5f,0.5f}); return b;
					}();

					Entity hit     = entt::null;
					float  hitDist = std::numeric_limits<float>::max();
					for (const RenderObject& obj : s_sceneSnapshot.objects)
					{
						HE::AABB box = s_cubeBox;
						if (obj.meshAssetId != HE::UUID{} && ctx.contentManager)
						{
							auto it = s_aabbCache.find(obj.meshAssetId);
							if (it == s_aabbCache.end())
							{
								if (const StaticMeshAsset* mesh =
									ctx.contentManager->getStaticMesh(obj.meshAssetId))
									it = s_aabbCache.emplace(obj.meshAssetId,
										HE::AABB::fromPositions(mesh->vertices.data(),
										                        mesh->vertices.size() / 3)).first;
							}
							if (it != s_aabbCache.end() && it->second.isValid())
								box = it->second;
						}

						// Ray → object space (exact test for rotated objects)
						const glm::mat4 invModel = glm::inverse(obj.transform);
						const glm::vec3 o = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
						const glm::vec3 d = glm::vec3(invModel * glm::vec4(rayDir,    0.0f));

						float t = 0.0f;
						if (box.intersectRay(o, d, t) && t < hitDist)
						{
							hitDist = t;
							hit     = static_cast<Entity>(obj.entityId);
						}
					}
					ctx.selectedEntity = hit; // miss = deselect
				}

				// ── Landscape brush cursor + sculpt ────────────────────────
				if (ctx.editorConfig.mode == EditorMode::Landscape && ctx.world)
				{
					auto& terrainReg = ctx.world->registry();
					auto  tvw        = terrainReg.view<TerrainComponent>();
					if (!tvw.empty())
					{
						Entity terrainEnt = tvw.front();
						auto&  tc         = terrainReg.get<TerrainComponent>(terrainEnt);

						float terrainWorldY = 0.0f;
						if (const auto* xf = terrainReg.try_get<TransformComponent>(terrainEnt))
							terrainWorldY = xf->position.y;

						// ── Terrain height sampler (bilinear, local space) ────────
						// Returns the sculpted height at world XZ; 0 if no sculpt data.
						const uint32_t tcRes   = std::clamp(tc.resolution, 2u, 1024u);
						const float    tcHalfX = tc.sizeX * 0.5f;
						const float    tcHalfZ = tc.sizeZ * 0.5f;
						const float    tcStepX = tc.sizeX / static_cast<float>(tcRes - 1);
						const float    tcStepZ = tc.sizeZ / static_cast<float>(tcRes - 1);

						auto sampleH = [&](float wx, float wz) -> float
						{
							if (tc.sculptHeights.empty()) return 0.0f;
							const float gx = std::clamp((wx + tcHalfX) / tcStepX,
							                            0.0f, static_cast<float>(tcRes - 1));
							const float gz = std::clamp((wz + tcHalfZ) / tcStepZ,
							                            0.0f, static_cast<float>(tcRes - 1));
							const int xi0 = static_cast<int>(gx);
							const int zi0 = static_cast<int>(gz);
							const int xi1 = std::min(xi0 + 1, static_cast<int>(tcRes) - 1);
							const int zi1 = std::min(zi0 + 1, static_cast<int>(tcRes) - 1);
							const float fx = gx - static_cast<float>(xi0);
							const float fz = gz - static_cast<float>(zi0);
							const float h00 = tc.sculptHeights[zi0 * tcRes + xi0];
							const float h10 = tc.sculptHeights[zi0 * tcRes + xi1];
							const float h01 = tc.sculptHeights[zi1 * tcRes + xi0];
							const float h11 = tc.sculptHeights[zi1 * tcRes + xi1];
							return h00*(1-fx)*(1-fz) + h10*fx*(1-fz)
							     + h01*(1-fx)*fz     + h11*fx*fz;
						};

						// ── Unproject mouse → refined terrain surface hit ─────────
						const ImVec2 mouse = ImGui::GetMousePos();
						const bool mouseInViewport =
							mouse.x >= rectMin.x && mouse.x <= rectMax.x &&
							mouse.y >= rectMin.y && mouse.y <= rectMax.y;

						bool      hasHit = false;
						glm::vec3 hitWS{};

						const glm::mat4 VP    = s_sceneSnapshot.camera.projection
						                      * s_sceneSnapshot.camera.view;
						const glm::mat4 invVP = glm::inverse(VP);

						if (mouseInViewport)
						{
							const float u = (mouse.x - rectMin.x) / (rectMax.x - rectMin.x);
							const float v = (mouse.y - rectMin.y) / (rectMax.y - rectMin.y);
							const glm::vec4 ndcNear(2.0f*u-1.0f, 1.0f-2.0f*v, -1.0f, 1.0f);
							const glm::vec4 ndcFar (ndcNear.x, ndcNear.y,       1.0f, 1.0f);
							glm::vec4 pNear = invVP * ndcNear; pNear /= pNear.w;
							glm::vec4 pFar  = invVP * ndcFar;  pFar  /= pFar.w;
							const glm::vec3 rayOrigin(pNear);
							const glm::vec3 rayDir = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));
							const float denom = rayDir.y;
							if (std::abs(denom) > 1e-5f)
							{
								// Step 1: intersect with the flat base plane (Y = terrainWorldY)
								const float t0 = (terrainWorldY - rayOrigin.y) / denom;
								if (t0 > 0.0f)
								{
									const glm::vec3 hit0 = rayOrigin + t0 * rayDir;
									// Step 2: one Newton refinement — intersect with
									// Y = terrainWorldY + h(hit0.xz).  For gradually
									// sloped terrain this converges in one step.
									const float h0 = sampleH(hit0.x, hit0.z);
									const float t1 = ((terrainWorldY + h0) - rayOrigin.y) / denom;
									if (t1 > 0.0f)
									{
										hitWS  = rayOrigin + t1 * rayDir;
										hasHit = true;
									}
								}
							}
						}

						// ── Draw brush circles on the terrain surface ─────────────
						if (hasHit && !navigating)
						{
							ImDrawList* dl    = ImGui::GetWindowDrawList();
							const float viewW = rectMax.x - rectMin.x;
							const float viewH = rectMax.y - rectMin.y;

							// Project a world XZ point at its actual terrain height → screen
							auto projectPt = [&](float wx, float wz, ImVec2& outPt) -> bool
							{
								const float wy = terrainWorldY + sampleH(wx, wz);
								glm::vec4 clip = VP * glm::vec4(wx, wy, wz, 1.0f);
								if (clip.w <= 0.0f) return false;
								clip /= clip.w;
								if (clip.z < -1.0f || clip.z > 1.0f) return false;
								outPt = ImVec2(
									rectMin.x + (clip.x * 0.5f + 0.5f) * viewW,
									rectMin.y + (0.5f   - clip.y * 0.5f) * viewH);
								return true;
							};

							constexpr int   kSeg = 48;
							constexpr float kPi2 = 6.28318530f;
							const float totalR   = s_brushRadius + s_falloffRadius;

							for (int ci = 0; ci < 2; ++ci)
							{
								const float r     = (ci == 0) ? s_brushRadius : totalR;
								const ImU32 col   = (ci == 0) ? IM_COL32(255,255,255,210)
								                              : IM_COL32(180,180,180,120);
								const float thick = (ci == 0) ? 1.5f : 1.0f;
								if (r < 0.01f) continue;

								ImVec2 prev{}; bool prevValid = false;
								for (int i = 0; i <= kSeg; ++i)
								{
									const float a = kPi2 * i / kSeg;
									ImVec2 cur{}; bool curValid = projectPt(
										hitWS.x + r * std::cos(a),
										hitWS.z + r * std::sin(a), cur);
									if (prevValid && curValid)
										dl->AddLine(prev, cur, col, thick);
									prev = cur; prevValid = curValid;
								}
							}
						}

						// ── Apply brush on LMB drag ───────────────────────────────
						const bool lmbDown =
							ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyAlt
							&& (viewportHovered || s_brushWasDown);

						if (lmbDown && !s_brushWasDown)
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							// Lazy-init sculptHeights from current terrain geometry
							if (tc.sculptHeights.empty())
							{
								const size_t nVerts = static_cast<size_t>(tcRes) * tcRes;
								const StaticMeshAsset tmp = generateTerrainMesh(tc);
								tc.sculptHeights.resize(nVerts);
								for (size_t vi = 0; vi < nVerts; ++vi)
									tc.sculptHeights[vi] = tmp.vertices[vi * 3 + 1];
							}
						}
						s_brushWasDown = lmbDown;

						if (lmbDown && hasHit && !tc.sculptHeights.empty())
						{
							const float totalR  = s_brushRadius + s_falloffRadius;
							const float totalR2 = totalR * totalR;
							const float delta   = s_brushStrength * static_cast<float>(dt);
							bool anyChange = false;

							auto brushWeight = [&](float dist2) -> float
							{
								if (dist2 >= totalR2) return 0.0f;
								const float dist = std::sqrt(dist2);
								if (dist <= s_brushRadius) return 1.0f;
								if (s_falloffRadius < 0.001f) return 0.0f;
								return 1.0f - (dist - s_brushRadius) / s_falloffRadius;
							};

							if (s_terrainTool == TerrainTool::Smooth)
							{
								std::vector<float> smoothed = tc.sculptHeights;
								for (uint32_t zi = 0; zi < tcRes; ++zi)
								{
									for (uint32_t xi = 0; xi < tcRes; ++xi)
									{
										const float wx = -tcHalfX + static_cast<float>(xi) * tcStepX;
										const float wz = -tcHalfZ + static_cast<float>(zi) * tcStepZ;
										const float d2 = (hitWS.x-wx)*(hitWS.x-wx)
										               + (hitWS.z-wz)*(hitWS.z-wz);
										const float w  = brushWeight(d2);
										if (w <= 0.0f) continue;
										float sum = 0.0f; int cnt = 0;
										for (int dzi = -1; dzi <= 1; ++dzi)
											for (int dxi = -1; dxi <= 1; ++dxi)
											{
												const int ni = static_cast<int>(zi) + dzi;
												const int nj = static_cast<int>(xi) + dxi;
												if (ni >= 0 && ni < static_cast<int>(tcRes) &&
												    nj >= 0 && nj < static_cast<int>(tcRes))
												{ sum += tc.sculptHeights[ni*tcRes+nj]; ++cnt; }
											}
										const float avg = cnt > 0 ? sum/cnt
										                          : tc.sculptHeights[zi*tcRes+xi];
										// Blend factor: 10× the raise/lower rate so smooth
										// is visually comparable in speed.
										const float blend = std::min(w * delta * 10.0f, w);
										smoothed[zi*tcRes+xi] = tc.sculptHeights[zi*tcRes+xi]
										    + blend * (avg - tc.sculptHeights[zi*tcRes+xi]);
										anyChange = true;
									}
								}
								if (anyChange) tc.sculptHeights = std::move(smoothed);
							}
							else
							{
								const float sign = (s_terrainTool == TerrainTool::Raise) ? 1.0f : -1.0f;
								for (uint32_t zi = 0; zi < tcRes; ++zi)
								{
									for (uint32_t xi = 0; xi < tcRes; ++xi)
									{
										const float wx = -tcHalfX + static_cast<float>(xi) * tcStepX;
										const float wz = -tcHalfZ + static_cast<float>(zi) * tcStepZ;
										const float d2 = (hitWS.x-wx)*(hitWS.x-wx)
										               + (hitWS.z-wz)*(hitWS.z-wz);
										const float w  = brushWeight(d2);
										if (w <= 0.0f) continue;
										tc.sculptHeights[zi*tcRes+xi] += sign * w * delta;
										anyChange = true;
									}
								}
							}

							if (anyChange && ctx.contentManager)
							{
								if (const auto* mc = terrainReg.try_get<MeshComponent>(terrainEnt))
									s_aabbCache.erase(mc->meshAssetId);
								tc.dirty = true;
								TerrainSystem::updateTerrains(*ctx.world, *ctx.contentManager,
								                              ctx.renderer);
							}
						}
					}
				}
			}
			else
				ImGui::TextDisabled("  Viewport not available on this backend yet.");
		}
		ImGui::End();
	}

	// topbar inside viewport (for quick actions) — position set on first use, freely movable after
	{
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + kTabBarH), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 36.0f), ImGuiCond_FirstUseEver);

		ImGui::Begin("Toolbar##ViewportTopBar", nullptr,
			ImGuiWindowFlags_NoTitleBar         |
			ImGuiWindowFlags_NoScrollbar        |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav              |
			ImGuiWindowFlags_NoDecoration
			);

		ImGui::SetNextItemWidth(120.0f);

        {
            if (ImGui::BeginCombo("##ModeSelector",ctx.editorConfig.modeString().c_str()))
            {
                if (ImGui::Selectable("View"))
                {
                    ctx.editorConfig.mode = EditorMode::View;
                }
                if (ImGui::Selectable("Landscape"))
                {
                    ctx.editorConfig.mode = EditorMode::Landscape;
                }
                ImGui::EndCombo();
            }
        }
		ImGui::SameLine();

		// ── Gizmo tools: Move / Rotate / Scale (mirror the W/E/R shortcuts) ──
		{
			auto toolBtn = [&](const char* label, ImGuizmo::OPERATION op, const char* tip)
			{
				const bool active = (s_gizmoOp == op);
				if (active)
					ImGui::PushStyleColor(ImGuiCol_Button,
						ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				if (ImGui::Button(label)) s_gizmoOp = op;
				if (active) ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
			};
			toolBtn("Move",   ImGuizmo::TRANSLATE, "Move (W)");   ImGui::SameLine();
			toolBtn("Rotate", ImGuizmo::ROTATE,    "Rotate (E)"); ImGui::SameLine();
			toolBtn("Scale",  ImGuizmo::SCALE,     "Scale (R)");  ImGui::SameLine();

			// Local / World gizmo orientation (dropdown)
			{
				int modeIdx = (s_gizmoMode == ImGuizmo::WORLD) ? 1 : 0;
				const char* modeItems[] = { "Local", "World" };
				ImGui::SetNextItemWidth(90.0f);
				if (ImGui::Combo("##GizmoMode", &modeIdx, modeItems, 2))
					s_gizmoMode = (modeIdx == 1) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Gizmo axis orientation:\nLocal (object axes) or World (axis-aligned)");
			}
			ImGui::SameLine();

			// Outer screen-space rotation ring (the white circle on the rotate
			// gizmo that spins about the view axis) — off by default.
			ImGui::Checkbox("Screen ring", &s_rotateScreen);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Show the rotate gizmo's outer screen-space ring\n"
					"(rotates about the view axis)");
			ImGui::SameLine();
		}

		// ── Play button centered ────────────────────────────────────────────
		{
			const bool playing = ctx.isPlaying;
			constexpr float btnSize = 20.0f;
			const float centerX = (ImGui::GetContentRegionAvail().x - 120 - btnSize) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

			ImTextureID icon = playing ? ctx.toolbarIcons.stop : ctx.toolbarIcons.play;
			ImVec4 tint = playing
				? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
				: ImVec4(0.35f, 1.0f, 0.55f, 1.0f);

			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.08f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.16f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));

			bool toggled = false;
			if (icon)
				toggled = ImGui::ImageButton("##tbPlay", icon, ImVec2(btnSize, btnSize),
					ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint);
			else
				toggled = ImGui::Button(playing ? "Stop" : "Play", ImVec2(btnSize * 2.0f, btnSize));

			if (toggled && ctx.setPlayMode)
				ctx.setPlayMode(!playing);

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
		}

		ImGui::End();
	}

    // ── Quick Settings / Landscape panel ────────────────────────────────────
    // When Landscape mode is active the panel becomes the Landscape tool panel;
    // otherwise it shows the user-pinned Quick Settings as normal.
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("Quick Settings");
    if (ctx.fontHeading) ImGui::PopFont();

    if (ctx.editorConfig.mode == EditorMode::Landscape && ctx.world)
    {
        // ── Check for an existing terrain entity ─────────────────────────────
        auto& reg = ctx.world->registry();
        auto terrainView = reg.view<TerrainComponent>();
        const bool hasTerrain = !terrainView.empty();

        if (!hasTerrain)
        {
            // ── No terrain yet — show creation form ──────────────────────────
            ImGui::SeparatorText("Create Landscape");

            static float  s_newSizeX      = 100.0f;
            static float  s_newSizeZ      = 100.0f;
            static int    s_newResolution = 128;
            static float  s_newHeight     = 20.0f;
            static int    s_newSeed       = 0;  // 0 = flat
            static int    s_newOctaves    = 4;
            static float  s_newFrequency  = 1.0f;
            static float  s_newLacunarity = 2.0f;
            static float  s_newGain       = 0.5f;

            ImGui::DragFloat("Width (X)",    &s_newSizeX,      1.0f,  1.0f, 10000.0f, "%.1f m");
            ImGui::DragFloat("Depth (Z)",    &s_newSizeZ,      1.0f,  1.0f, 10000.0f, "%.1f m");
            ImGui::DragInt  ("Resolution",   &s_newResolution, 1,     2,    512);
            ImGui::DragFloat("Height Scale", &s_newHeight,     0.5f,  0.0f, 1000.0f,  "%.1f m");
            ImGui::SeparatorText("Noise (seed 0 = flat)");
            ImGui::DragInt  ("Seed",         &s_newSeed,       1,     0,    0x7fffffff);
            ImGui::DragInt  ("Octaves",      &s_newOctaves,    1,     1,    8);
            ImGui::DragFloat("Frequency",    &s_newFrequency,  0.01f, 0.01f, 16.0f, "%.2f");
            ImGui::DragFloat("Lacunarity",   &s_newLacunarity, 0.01f, 1.0f,  8.0f,  "%.2f");
            ImGui::DragFloat("Gain",         &s_newGain,       0.01f, 0.0f,  1.0f,  "%.2f");

            ImGui::Spacing();
            const float btnW = 160.0f;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - btnW) * 0.5f
                                 + ImGui::GetCursorPosX());
            if (ImGui::Button("Create Landscape", ImVec2(btnW, 0)))
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow();

                TerrainComponent tc;
                tc.sizeX      = s_newSizeX;
                tc.sizeZ      = s_newSizeZ;
                tc.resolution = static_cast<uint32_t>(std::clamp(s_newResolution, 2, 1024));
                tc.heightScale= s_newHeight;
                tc.seed       = s_newSeed;
                tc.octaves    = s_newOctaves;
                tc.frequency  = s_newFrequency;
                tc.lacunarity = s_newLacunarity;
                tc.gain       = s_newGain;
                tc.dirty      = true;

                Entity e = ctx.world->createEntity("Terrain");
                ctx.world->addComponent(e, TransformComponent{});
                ctx.world->addComponent(e, tc);
                MaterialComponent mc;
                mc.materialAssetId = HE::kDefaultTerrainMaterialId;
                ctx.world->addComponent(e, mc);

                ctx.world->markHierarchyDirty();
                ctx.selectedEntity = e;
                Logger::Log(Logger::LogLevel::Info, "Editor: created Terrain entity");
            }
        }
        else
        {
            // ── Terrain exists — show sculpt tools ───────────────────────────
            ImGui::SeparatorText("Sculpt Tool");

            const char* toolLabels[] = { "Raise", "Lower", "Smooth" };
            int toolIdx = static_cast<int>(s_terrainTool);
            for (int i = 0; i < 3; ++i)
            {
                if (i > 0) ImGui::SameLine();
                if (ImGui::RadioButton(toolLabels[i], toolIdx == i))
                    s_terrainTool = static_cast<TerrainTool>(i);
            }

            ImGui::Spacing();
            ImGui::DragFloat("Radius##brush",   &s_brushRadius,   0.5f,  0.5f, 500.0f, "%.1f m");
            ImGui::DragFloat("Falloff##brush",  &s_falloffRadius, 0.5f,  0.0f, 500.0f, "%.1f m");
            ImGui::DragFloat("Strength##brush", &s_brushStrength, 0.1f,  0.1f,  50.0f, "%.2f");
            s_brushRadius   = std::max(0.5f, s_brushRadius);
            s_falloffRadius = std::max(0.0f, s_falloffRadius);

            ImGui::Spacing();
            ImGui::TextDisabled("LMB drag in viewport to sculpt");
            ImGui::TextDisabled("Ctrl+click a field to type a value");

            ImGui::Spacing();
            if (ImGui::Button("Reset Sculpting"))
            {
                Entity terrainEnt = terrainView.front();
                auto& tc = reg.get<TerrainComponent>(terrainEnt);
                if (ctx.undoSys) ctx.undoSys->snapshotNow();
                tc.sculptHeights.clear();
                tc.dirty = true;
            }
        }
    }
    else
    {
        // Quick Settings = the engine settings the user pinned in Preferences.
        DrawEngineSettings(ctx, SettingsMode::QuickSettings);
    }

    ImGui::End();

    // World Outliner
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("World Outliner");
    if (ctx.fontHeading) ImGui::PopFont();

    if (ctx.world)
    {
        // ── Cached hierarchy snapshot ─────────────────────────────────────
        struct OutlinerNode
        {
            Entity      entity;
            std::string name;
            int         depth;
            bool        hasChildren;
        };
        static std::vector<OutlinerNode> s_outlinerCache;
        static HorizonWorld*             s_lastWorld = nullptr;

        // Rebuild cache only when the hierarchy changed or the world switched
        if (ctx.world->isHierarchyDirty() || s_lastWorld != ctx.world)
        {
            s_lastWorld = ctx.world;
            s_outlinerCache.clear();

            auto& registry = ctx.world->registry();
            Entity root    = ctx.world->rootEntity();

            std::function<void(Entity, int)> collect = [&](Entity entity, int depth)
            {
                if (!registry.valid(entity)) return;
                // The built-in environment sun/moon lights belong to the World's
                // Environment, not the scene tree — hide them from the Outliner.
                if (entity != ctx.world->rootEntity() &&
                    registry.all_of<EnvironmentLightComponent>(entity))
                    return;
                auto* name = registry.try_get<NameComponent>(entity);
                auto* hier = registry.try_get<HierarchyComponent>(entity);
                s_outlinerCache.push_back({
                    entity,
                    name ? name->name : "(unnamed)",
                    depth,
                    hier && !hier->children.empty()
                });
                if (hier)
                    for (Entity child : hier->children)
                        collect(child, depth + 1);
            };
            collect(root, 0);

            char buf[96];
            std::snprintf(buf, sizeof(buf), "[Outliner] rebuilt: %zu nodes", s_outlinerCache.size());
            Logger::Log(HE::LogLevel::Info, buf);

            ctx.world->clearHierarchyDirty();
        }

        // ── Entity rename popup state ─────────────────────────────────────
        static Entity s_renameEntity     = entt::null;
        static char   s_entityRenameBuf[256] = {};
        static bool   s_openEntityRename = false;

        // ── Render from cache ─────────────────────────────────────────────
        int prevDepth      = -1;
        int skipBelowDepth = INT_MAX; // skip children of closed nodes

        for (const auto& node : s_outlinerCache)
        {
            // If a parent was closed, skip all its children
            if (node.depth > skipBelowDepth)
                continue;
            skipBelowDepth = INT_MAX; // back at or above the closed level → reset

            // Close tree levels we've left
            while (prevDepth >= node.depth)
            {
                ImGui::TreePop();
                --prevDepth;
            }

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanAvailWidth
                                     | ImGuiTreeNodeFlags_DefaultOpen;
            if (!node.hasChildren)
                flags |= ImGuiTreeNodeFlags_Leaf;
            if (node.entity == ctx.selectedEntity)
                flags |= ImGuiTreeNodeFlags_Selected;

            bool open = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(
                    static_cast<uint32_t>(node.entity))),
                flags, "%s", node.name.c_str());

            // Click (not on the arrow) → select
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                ctx.selectedEntity = node.entity;

            // ── Drag & drop reparenting ───────────────────────────────────
            const bool isRoot = (node.entity == ctx.world->rootEntity());
            if (!isRoot && ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("HE_ENTITY", &node.entity, sizeof(Entity));
                ImGui::TextUnformatted(node.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HE_ENTITY"))
                {
                    Entity dragged{};
                    std::memcpy(&dragged, payload->Data, sizeof(Entity));
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ctx.world->reparentEntity(dragged, node.entity);
                }
                ImGui::EndDragDropTarget();
            }

            // ── Per-entity context menu ───────────────────────────────────
            if (ImGui::BeginPopupContextItem())
            {
                ctx.selectedEntity = node.entity;
                if (ImGui::MenuItem("Create Child Entity"))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    Entity child = ctx.world->createEntity("Entity");
                    ctx.world->addComponent(child, TransformComponent{});
                    ctx.world->reparentEntity(child, node.entity);
                    ctx.selectedEntity = child;
                }
                if (ImGui::MenuItem("Rename"))
                {
                    s_renameEntity = node.entity;
                    std::strncpy(s_entityRenameBuf, node.name.c_str(), sizeof(s_entityRenameBuf) - 1);
                    s_entityRenameBuf[sizeof(s_entityRenameBuf) - 1] = '\0';
                    s_openEntityRename = true;
                }
                if (!isRoot && ImGui::MenuItem("Save as Prefab") && ctx.contentManager)
                {
                    SceneSerializer ser;
                    auto data = ser.serializeSubtree(*ctx.world, node.entity);
                    PrefabAsset prefab;
                    prefab.name = node.name;
                    prefab.data = std::move(data);
                    ctx.contentManager->registerPrefab(std::move(prefab));
                }
                if (!isRoot && ImGui::MenuItem("Delete"))
                {
                    if (ctx.selectedEntity == node.entity)
                        ctx.selectedEntity = entt::null;
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ctx.world->destroyEntity(node.entity);
                }
                ImGui::EndPopup();
            }

            if (open)
                prevDepth = node.depth;
            else
                skipBelowDepth = node.depth; // don't enter children
        }
        // Close remaining open levels
        while (prevDepth >= 0)
        {
            ImGui::TreePop();
            --prevDepth;
        }

        // ── Background context menu: create entity at root level ──────────
        if (ImGui::BeginPopupContextWindow("##outliner_bg_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::MenuItem("Create Entity"))
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow();
                Entity e = ctx.world->createEntity("Entity");
                ctx.world->addComponent(e, TransformComponent{});
                ctx.selectedEntity = e;
            }
            ImGui::EndPopup();
        }

        // ── Entity rename popup ────────────────────────────────────────────
        if (s_openEntityRename)
        {
            ImGui::OpenPopup("##entity_rename_popup");
            s_openEntityRename = false;
        }
        ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("##entity_rename_popup", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::TextUnformatted("Rename Entity");
            ImGui::Separator();
            ImGui::SetNextItemWidth(-1.0f);
            bool confirm = ImGui::InputText("##entity_rename_input",
                s_entityRenameBuf, sizeof(s_entityRenameBuf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::Button("OK", ImVec2(140, 0)) || confirm)
            {
                if (s_entityRenameBuf[0] != '\0' &&
                    ctx.world->registry().valid(s_renameEntity))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ctx.world->renameEntity(s_renameEntity, s_entityRenameBuf);
                }
                s_renameEntity = entt::null;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140, 0)))
            {
                s_renameEntity = entt::null;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    else
    {
        ImGui::TextDisabled("(no world loaded)");
    }

    ImGui::End();

    RenderInspector(ctx);

    DrawPreferencesWindow(ctx, s_showPreferences);

    //Content Browser
	auto [contentFolder, contentLock] = ctx.globalState->lockContentFolder();
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("Content Browser", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (ctx.fontHeading) ImGui::PopFont();

    {
        const float totalWidth    = ImGui::GetContentRegionAvail().x;
        const float totalHeight   = ImGui::GetContentRegionAvail().y;
        const float splitterW     = 6.0f;
        const float minPaneWidth  = 60.0f;

        if (ctx.cbTreeWidth < 0.0f)
            ctx.cbTreeWidth = totalWidth * 0.25f;

        ctx.cbTreeWidth = std::clamp(ctx.cbTreeWidth, minPaneWidth, totalWidth - minPaneWidth - splitterW);

		ImGui::BeginChild("##cb_tree", ImVec2(ctx.cbTreeWidth, totalHeight), false);

		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::Text("Content");
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Separator();

		// ── Tree: single-click = expand/collapse, double-click = navigate ──
		static const Folder* s_selectedTreeFolder = nullptr;

		std::function<void(const Folder*, int)> renderTree = [&](const Folder* folder, int depth)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
									 | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (folder->subfolders.empty())
				flags |= ImGuiTreeNodeFlags_Leaf;

			bool open = ImGui::TreeNodeEx(folder->name.c_str(), flags);

			// Double-click anywhere on the item → navigate grid to this folder
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				s_selectedTreeFolder = folder;

			if (open)
			{
				for (auto* sub : folder->subfolders)
					renderTree(sub, depth + 1);
				ImGui::TreePop();
			}
		};

		// Root "Content" node
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_DefaultOpen
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Content", rootFlags);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				s_selectedTreeFolder = nullptr; // back to root
			if (rootOpen)
			{
				for (auto* sub : contentFolder.subfolders)
					renderTree(sub, 1);
				ImGui::TreePop();
			}
		}

		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

		ImGui::Button("##cb_splitter", ImVec2(splitterW, totalHeight));

		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

		if (ImGui::IsItemActive())
			ctx.cbTreeWidth += ImGui::GetIO().MouseDelta.x;

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		ImGui::SameLine();

		ImGui::BeginChild("##cb_content", ImVec2(0.0f, totalHeight), false);

		// ── Determine which folder's content to show ──────────────────────
		// s_selectedTreeFolder nullptr → root; otherwise the selected sub-folder
		static const Folder* s_gridFolder = nullptr;

		// Sync from tree double-click
		if (s_selectedTreeFolder != s_gridFolder)
			s_gridFolder = s_selectedTreeFolder;

		const Folder* displayFolder = s_gridFolder ? s_gridFolder : &contentFolder;

		// ── Breadcrumb ────────────────────────────────────────────────────
		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::Text("Assets");
		if (ctx.fontSubheading) ImGui::PopFont();

		// Simple breadcrumb: root > ... > current folder name
		{
			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.5f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.4f,0.4f,0.4f,0.7f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 1));

			// Build ancestor chain via DFS
			std::vector<const Folder*> crumbs;
			if (s_gridFolder)
			{
				std::function<bool(const Folder*)> findPath = [&](const Folder* cur) -> bool
				{
					if (cur == s_gridFolder) return true;
					for (auto* sub : cur->subfolders)
					{
						crumbs.push_back(sub);
						if (findPath(sub)) return true;
						crumbs.pop_back();
					}
					return false;
				};
				findPath(&contentFolder);
			}

			if (ImGui::SmallButton("Content##bc_root"))
			{
				s_gridFolder         = nullptr;
				s_selectedTreeFolder = nullptr;
			}

			for (int ci = 0; ci < static_cast<int>(crumbs.size()); ++ci)
			{
				ImGui::SameLine(0, 2);
				ImGui::TextDisabled(">");
				ImGui::SameLine(0, 2);
				const Folder* crumb = crumbs[ci];
				bool isLast = (ci == static_cast<int>(crumbs.size()) - 1);
				if (isLast)
				{
					// Current folder – shown as a dimmed button (no navigation)
					ImGui::BeginDisabled();
					ImGui::SmallButton(crumb->name.c_str());
					ImGui::EndDisabled();
				}
				else
				{
					std::string btnId = crumb->name + "##bc_" + std::to_string(ci);
					if (ImGui::SmallButton(btnId.c_str()))
					{
						s_gridFolder         = crumb;
						s_selectedTreeFolder = crumb;
					}
				}
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
		}

		ImGui::Separator();

		// ── Grid ──────────────────────────────────────────────────────────
		constexpr float k_cellSize    = 72.0f;
		constexpr float k_iconPad     = 6.0f;   // padding inside ImageButton
		constexpr float k_iconSize    = k_cellSize - k_iconPad * 2.0f;
		constexpr float k_labelHeight = 16.0f;
		constexpr float k_padding     = 8.0f;
		constexpr float k_itemSize    = k_cellSize + k_labelHeight + k_padding;

		// Helper: pick an asset icon based on file extension
		auto pickAssetIcon = [&](const std::string& ext) -> ImTextureID
		{
			std::string e = ext;
			for (auto& c : e) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
			if (e == ".mat")                                   return ctx.cbIcons.material;
			if (e == ".obj" || e == ".fbx" || e == ".gltf"
				|| e == ".glb" || e == ".dae")                 return ctx.cbIcons.model3d;
			if (e == ".svg" || e == ".ai")                     return ctx.cbIcons.model2d;
			if (e == ".cs"  || e == ".lua" || e == ".py"
				|| e == ".js")                                  return ctx.cbIcons.script;
			if (e == ".wav" || e == ".mp3" || e == ".ogg"
				|| e == ".flac")                                return ctx.cbIcons.sound;
			if (e == ".png" || e == ".jpg" || e == ".jpeg"
					|| e == ".bmp" || e == ".tga" || e == ".hdr")  return ctx.cbIcons.texture;
				if (e == ".hescene")                               return ctx.cbIcons.scene;
				return 0;
		};

		const float availW     = ImGui::GetContentRegionAvail().x;
		const int   columns    = (std::max)(1, static_cast<int>(availW / k_itemSize));

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(k_padding, k_padding));

		int col = 0;

		// ── Shared selection/rename/context state ────────────────────────
		static std::string s_selectedItem;
		static bool        s_selectedIsFolder = false;
		static std::string s_ctxMenuItem;
		static bool        s_ctxMenuIsFolder  = false;
		static std::string s_renameTarget;
		static bool        s_renameIsFolder   = false;
		static char        s_renameBuf[256]   = {};
		static bool        s_openRenamePopup  = false;
		static bool        s_renameIsCreate   = false; // naming a freshly created item
		static bool        s_rightClickOnItem = false;

		// ── Folders first ─────────────────────────────────────────────────
		for (auto* sub : displayFolder->subfolders)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = (s_selectedItem == sub->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(sub->fullPath.c_str());

			if (isSel)
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.26f, 0.46f, 0.78f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.54f, 0.90f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.38f, 0.68f, 1.0f));
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			if (ctx.cbIcons.folder)
			{
				ImGui::ImageButton("##icon", ctx.cbIcons.folder,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0),
					ImVec4(0.96f, 0.78f, 0.26f, 1.0f));
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}

			// Left click → select
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
			}
			// Double-click → navigate into folder
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_gridFolder         = sub;
				s_selectedTreeFolder = sub;
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
				s_ctxMenuItem      = sub->fullPath;
				s_ctxMenuIsFolder  = true;
				s_rightClickOnItem = true;
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
			ImGui::PopID();

			// Centered label (truncated)
			const float labelW = k_cellSize;
			std::string label  = sub->name;
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());

			ImGui::EndGroup();
			++col;
		}

		// ── Files ─────────────────────────────────────────────────────────
		for (auto* file : displayFolder->files)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = (s_selectedItem == file->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(file->fullPath.c_str());

			if (isSel)
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.26f, 0.46f, 0.78f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.54f, 0.90f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.38f, 0.68f, 1.0f));
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			ImTextureID assetIcon = pickAssetIcon(file->extension);
			if (assetIcon)
			{
				ImVec4 tint{0.75f, 0.85f, 1.0f, 1.0f};
				if (assetIcon == ctx.cbIcons.material) tint = {0.60f, 0.90f, 0.60f, 1.0f};
				if (assetIcon == ctx.cbIcons.model3d)  tint = {0.70f, 0.80f, 1.00f, 1.0f};
				if (assetIcon == ctx.cbIcons.model2d)  tint = {0.80f, 0.70f, 1.00f, 1.0f};
				if (assetIcon == ctx.cbIcons.script)   tint = {0.90f, 0.90f, 0.50f, 1.0f};
				if (assetIcon == ctx.cbIcons.sound)    tint = {0.60f, 0.90f, 0.90f, 1.0f};
				if (assetIcon == ctx.cbIcons.texture)  tint = {0.90f, 0.75f, 0.60f, 1.0f};
				if (assetIcon == ctx.cbIcons.scene)    tint = {0.75f, 0.65f, 1.00f, 1.0f};

				ImGui::ImageButton("##icon", assetIcon,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0), tint);
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);

			// Drag source — carries the asset's absolute path so drop targets
			// (e.g. the Material slot in the inspector) can load it.
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				ImGui::SetDragDropPayload("HE_ASSET_PATH",
					file->fullPath.c_str(), file->fullPath.size() + 1);
				ImGui::TextUnformatted(
					std::filesystem::path(file->name).stem().string().c_str());
				ImGui::EndDragDropSource();
			}

			// Left click → select
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
			}
			// Double-click → open tab
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (std::filesystem::path(file->fullPath).extension() == ".hescene")
				{
					requestGuarded(GuardedAction::OpenScenePath, file->fullPath);
				}
				else
				{
				const std::string tabLabel = std::filesystem::path(file->name).stem().string();
				auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
					[&](const AppContext::EditorTab& t){ return t.assetPath == file->fullPath; });
				if (it == ctx.tabs.end())
				{
					ctx.tabs.push_back({ tabLabel, file->fullPath, true, true });
					ctx.activeTab = static_cast<int>(ctx.tabs.size()) - 1;
				}
				else
				{
					ctx.activeTab = static_cast<int>(std::distance(ctx.tabs.begin(), it));
				}
				}
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
				s_ctxMenuItem      = file->fullPath;
				s_ctxMenuIsFolder  = false;
				s_rightClickOnItem = true;
			}

			ImGui::PopID();

			// Centered label (stem only, truncated)
			const float labelW = k_cellSize;
			std::string label  = std::filesystem::path(file->name).stem().string();
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());

			ImGui::EndGroup();
			++col;
		}

		ImGui::PopStyleVar();

		// ── Open item context menu after loops ────────────────────────────
		if (s_rightClickOnItem)
		{
			ImGui::OpenPopup("##cb_item_ctx");
			s_rightClickOnItem = false;
		}

		// ── Item context menu (folder + file) ─────────────────────────────
		if (ImGui::BeginPopup("##cb_item_ctx"))
		{
			std::string displayName = s_ctxMenuIsFolder
				? std::filesystem::path(s_ctxMenuItem).filename().string()
				: std::filesystem::path(s_ctxMenuItem).stem().string();
			ImGui::TextDisabled("%s", displayName.c_str());
			ImGui::Separator();

			// ── Import source file → .hasset ─────────────────────────────
			if (!s_ctxMenuIsFolder)
			{
				const std::filesystem::path srcPath(s_ctxMenuItem);
				std::string ext = srcPath.extension().string();
				for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

				const bool isMeshSrc    = (ext == ".gltf" || ext == ".glb");
				const bool isTextureSrc = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
				                           ext == ".tga" || ext == ".bmp" || ext == ".hdr");
				const bool isAudioSrc   = (ext == ".wav");
				const bool isMatSrc     = (ext == ".hmat");

				if ((isMeshSrc || isTextureSrc || isAudioSrc || isMatSrc) &&
				    ImGui::MenuItem("Import"))
				{
					const std::filesystem::path root = contentFolder.fullPath;
					std::error_code ec;
					std::filesystem::path relDir =
						std::filesystem::relative(srcPath.parent_path(), root, ec);
					if (ec || relDir == ".") relDir.clear();

					bool ok = false;
					if      (isMeshSrc)    ok = MeshImporter::import(srcPath, root, relDir)     != nullptr;
					else if (isTextureSrc) ok = TextureImporter::import(srcPath, root, relDir)  != nullptr;
					else if (isAudioSrc)   ok = AudioImporter::import(srcPath, root, relDir)    != nullptr;
					else if (isMatSrc)     ok = MaterialImporter::import(srcPath, root, relDir) != nullptr;

					if (!ok)
						Logger::Log(Logger::LogLevel::Error,
							("Editor: import failed for " + srcPath.string()).c_str());
					ctx.contentRefreshPending = true;
					ImGui::CloseCurrentPopup();
				}

				// ── Add a StaticMesh .hasset to the scene ─────────────────
				if (ext == ".hasset" && ctx.world && ctx.contentManager &&
				    ImGui::MenuItem("Add to Scene"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
						srcPath, std::filesystem::path(contentFolder.fullPath), ec).generic_string();
					if (!ec)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (const StaticMeshAsset* mesh = ctx.contentManager->getStaticMesh(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							Entity e = ctx.world->createEntity(mesh->name);
							ctx.world->addComponent(e, TransformComponent{});
							ctx.world->addComponent(e, MeshComponent{ .meshAssetId = id });
							ctx.world->markHierarchyDirty();
							Logger::Log(Logger::LogLevel::Info,
								("Editor: added '" + mesh->name + "' to scene").c_str());
						}
						else
							Logger::Log(Logger::LogLevel::Warning,
								("Editor: " + rel + " is not a loadable static mesh").c_str());
					}
					ImGui::CloseCurrentPopup();
				}
			}

			if (ImGui::MenuItem("Rename"))
			{
				s_renameTarget   = s_ctxMenuItem;
				s_renameIsFolder = s_ctxMenuIsFolder;
				s_renameIsCreate = false;
				std::strncpy(s_renameBuf, displayName.c_str(), sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				ImGui::CloseCurrentPopup();
			}
			if (!s_ctxMenuIsFolder && ImGui::MenuItem("Delete"))
			{
				std::error_code ec;
				std::filesystem::remove(s_ctxMenuItem, ec);
				if (s_selectedItem == s_ctxMenuItem)
					s_selectedItem.clear();
				s_ctxMenuItem.clear();
				ctx.contentRefreshPending = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		// Trigger rename popup outside item context popup. Gated on the content
		// refresh having settled so it never competes with the ##ContentRefresh
		// modal (e.g. right after a create, which requests both).
		if (s_openRenamePopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_rename_popup");
			s_openRenamePopup = false;
		}

		// ── Rename / name-on-create popup ─────────────────────────────────
		ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
		if (ImGui::BeginPopupModal("##cb_rename_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			const char* verb = s_renameIsCreate ? "Name" : "Rename";
			ImGui::Text("%s %s", verb, s_renameIsFolder ? "Folder" : "Asset");
			ImGui::Separator();
			ImGui::Spacing();

			// Focus the field as the dialog opens so the user can type at once.
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(-1.0f);
			bool confirm = ImGui::InputText("##rename_input", s_renameBuf, sizeof(s_renameBuf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
			ImGui::Spacing();

			if (ImGui::Button("OK", ImVec2(140, 0)) || confirm)
			{
				std::string newName(s_renameBuf);
				if (!newName.empty() && !s_renameTarget.empty())
				{
					std::filesystem::path oldPath(s_renameTarget);
					std::filesystem::path newPath;
					if (s_renameIsFolder)
						newPath = oldPath.parent_path() / newName;
					else
						newPath = oldPath.parent_path() / (newName + oldPath.extension().string());
					std::error_code ec;
					std::filesystem::rename(oldPath, newPath, ec);
					if (!ec)
					{
						if (s_selectedItem == s_renameTarget)
							s_selectedItem = newPath.string();
						if (!s_renameIsFolder)
						{
							for (auto& t : ctx.tabs)
								if (t.assetPath == s_renameTarget)
								{
									t.assetPath = newPath.string();
									t.label     = newName;
								}
						}
						s_quietContentRefresh = true;
					}
				}
				s_renameTarget.clear();
				s_renameIsCreate = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(140, 0)))
			{
				// On create, Cancel just keeps the default name — the file already
				// exists on disk; nothing to undo.
				s_renameTarget.clear();
				s_renameIsCreate = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		// ── Background left-click → clear selection ───────────────────────
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			!ImGui::IsAnyItemHovered())
		{
			s_selectedItem.clear();
		}

		// ── Background right-click context menu
		// Only open when clicking on the panel background (not on any item)
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
			!ImGui::IsAnyItemHovered())
		{
			ImGui::OpenPopup("##cb_create_ctx");
		}

		if (ImGui::BeginPopup("##cb_create_ctx"))
		{
			ImGui::TextDisabled("Create Asset");
			ImGui::Separator();

			const std::string targetFolder = displayFolder ? displayFolder->fullPath
														   : contentFolder.fullPath;

			auto tryCreate = [&](const char* defaultName, const char* ext, HE::AssetType type)
			{
				// Build a path that does not yet exist
				std::string base = targetFolder + "/" + defaultName;
				std::string path = base + ext;
				int counter = 1;
				while (std::filesystem::exists(path))
					path = base + std::to_string(counter++) + ext;

				// Create an empty asset file via ContentManager
				std::string relative = std::filesystem::relative(
					path,
					std::filesystem::path(ctx.projectManager->currentProject().path).parent_path()
				).string();
				std::replace(relative.begin(), relative.end(), '\\', '/');

				// Write a minimal binary asset stub so the file exists on disk.
				// The UUID minted here is the asset's permanent identity.
				{
					const HE::UUID assetId = HE::UUID::generate();
					HAsset::Writer w;
					std::vector<uint8_t> meta;
					HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
					HAsset::Writer::appendPOD(meta, assetId.hi);
					HAsset::Writer::appendPOD(meta, assetId.lo);
					HAsset::Writer::appendString(meta, defaultName);
					HAsset::Writer::appendString(meta, relative);
					w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
					w.write(path, static_cast<uint16_t>(type));
				}

				// Show it now (don't wait for the next auto-refresh) and let the
				// user name it straight away via the rename/name dialog.
				s_selectedItem    = path;
				s_renameTarget    = path;
				s_renameIsFolder  = false;
				s_renameIsCreate  = true;
				std::strncpy(s_renameBuf, defaultName, sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				s_quietContentRefresh = true;
				ImGui::CloseCurrentPopup();
			};

			if (ImGui::MenuItem("Scene"))        tryCreate("NewScene",    ".hescene", HE::AssetType::Scene);
			if (ImGui::MenuItem("Material"))     tryCreate("NewMaterial", ".hasset",  HE::AssetType::Material);
			if (ImGui::MenuItem("Texture"))      tryCreate("NewTexture",  ".hasset",  HE::AssetType::Texture);
			if (ImGui::MenuItem("Static Mesh"))  tryCreate("NewMesh",     ".hasset",  HE::AssetType::StaticMesh);
			if (ImGui::MenuItem("Skeletal Mesh"))tryCreate("NewSkelMesh", ".hasset",  HE::AssetType::SkeletalMesh);
			if (ImGui::MenuItem("Script"))       tryCreate("NewScript",   ".hasset",  HE::AssetType::Script);
			if (ImGui::MenuItem("Shader"))       tryCreate("NewShader",   ".hasset",  HE::AssetType::Shader);
			if (ImGui::MenuItem("Audio"))        tryCreate("NewAudio",    ".hasset",  HE::AssetType::Audio);
			if (ImGui::MenuItem("Font"))         tryCreate("NewFont",     ".hasset",  HE::AssetType::Font);
			if (ImGui::MenuItem("Folder"))
			{
				std::string base = targetFolder + "/NewFolder";
				std::string dir  = base;
				int counter = 1;
				while (std::filesystem::exists(dir))
					dir = base + std::to_string(counter++);
				std::filesystem::create_directory(dir);

				// Show it now and let the user name it straight away.
				const std::string folderName = std::filesystem::path(dir).filename().string();
				s_selectedItem    = dir;
				s_renameTarget    = dir;
				s_renameIsFolder  = true;
				s_renameIsCreate  = true;
				std::strncpy(s_renameBuf, folderName.c_str(), sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				s_quietContentRefresh = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		ImGui::EndChild();
    }

    ImGui::End();
#endif // HE_IMGUI_ENABLED
}

// ─── Inspector (Details panel) ────────────────────────────────────────────────
void EditorUI::RenderInspector(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
	ImGui::Begin("Details");
	if (ctx.fontHeading) ImGui::PopFont();

	if (!ctx.world || ctx.selectedEntity == entt::null ||
	    !ctx.world->registry().valid(ctx.selectedEntity))
	{
		ImGui::TextDisabled("(no entity selected)");
		ImGui::End();
		return;
	}

	auto&  registry = ctx.world->registry();
	Entity entity   = ctx.selectedEntity;

	// Pre-frame world state for undo. Captured once — only one ImGui item
	// can become active per frame, and its pre-state is this snapshot.
	// Widgets stash it on activation and commit when the edit session ends.
	if (ctx.undoSys)
		ctx.undoSys->capturePre();
	auto trackEdit = [&]
	{
		if (!ctx.undoSys) return;
		if (ImGui::IsItemActivated())            ctx.undoSys->stashPre();
		if (ImGui::IsItemDeactivatedAfterEdit()) ctx.undoSys->commitPending();
	};

	// ── Name ────────────────────────────────────────────────────────────────
	if (auto* name = registry.try_get<NameComponent>(entity))
	{
		char buf[256];
		std::strncpy(buf, name->name.c_str(), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##entity_name", buf, sizeof(buf),
		                     ImGuiInputTextFlags_EnterReturnsTrue))
		{
			if (ctx.undoSys) ctx.undoSys->snapshotNow();
			ctx.world->renameEntity(entity, buf);
		}
	}
	ImGui::Separator();

	// ── Environment (scene-wide sky settings, on the World root entity) ──────
	// Edited here so it persists with the scene; pushed to the renderer each frame
	// by EditorApplication::pushEnvironment.
	if (auto* env = registry.try_get<EnvironmentComponent>(entity))
	{
		if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Day-Night Cycle", &env->dayNightCycle); trackEdit();

			// Format the 0..1 time as a HH:MM clock shown inside the slider.
			int minutes = static_cast<int>(env->timeOfDay * 1440.0f) % 1440;
			if (minutes < 0) minutes += 1440;
			char clock[8];
			std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::SliderFloat("##timeofday", &env->timeOfDay, 0.0f, 1.0f, clock,
			                       ImGuiSliderFlags_NoRoundToFormat))
				env->dayNightCycle = true;
			trackEdit();
			ImGui::TextDisabled(env->dayNightCycle
				? "Drives the sun, sky & shadows."
				: "Move the slider to start a day-night cycle.");

			if (ImGui::Checkbox("Auto-Advance", &env->autoAdvance) && env->autoAdvance)
				env->dayNightCycle = true;
			trackEdit();
			ImGui::BeginDisabled(!env->autoAdvance);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cyclelen", &env->cycleSeconds, 5.0f, 600.0f,
			                   "Full day: %.0f s", ImGuiSliderFlags_Logarithmic); trackEdit();
			ImGui::EndDisabled();

			ImGui::SeparatorText("Sun & Moon Light");
			ImGui::ColorEdit3("Sun Color",  &env->sunColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SliderFloat("Sun Brightness",  &env->sunIntensity,  0.0f, 10.0f, "%.2f"); trackEdit();
			ImGui::ColorEdit3("Moon Color", &env->moonColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SliderFloat("Moon Brightness", &env->moonIntensity, 0.0f, 10.0f, "%.2f"); trackEdit();

			ImGui::SeparatorText("Clouds");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cloudcoverage", &env->cloudCoverage, 0.0f, 1.0f, "Coverage: %.2f"); trackEdit();
			ImGui::TextDisabled("Full overcast dims the sun & fills with ambient light.");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##winddir", &env->windDirection, 0.0f, 360.0f, "Wind direction: %.0f\xc2\xb0"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##windspeed", &env->windSpeed, 0.0f, 4.0f, "Wind speed: %.2f"); trackEdit();

			ImGui::SeparatorText("Atmospheric Fog");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##fogdensity", &env->fogDensity, 0.0f, 0.15f, "Density: %.3f"); trackEdit();
			ImGui::BeginDisabled(env->fogDensity <= 0.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##fogheight", &env->fogHeightFalloff, 0.0f, 1.0f, "Ground hugging: %.2f"); trackEdit();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Distant objects blend into the horizon (warm at sunset).");

			ImGui::SeparatorText("Night Sky");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##nebula", &env->nebulaIntensity, 0.0f, 1.0f, "Space Nebula: %.2f"); trackEdit();
			ImGui::ColorEdit3("Nebula Color", &env->nebulaColor.x); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##aurora", &env->auroraIntensity, 0.0f, 1.0f, "Aurora: %.2f"); trackEdit();
			ImGui::ColorEdit3("Aurora Color", &env->auroraColor.x); trackEdit();
			ImGui::TextDisabled("Stars, Milky Way & nebula turn with the day; aurora drifts.");
		}
		ImGui::Separator();
	}

	// Header with a right-click "Remove Component" menu. Returns true when
	// the section is open; sets `removed` when the user removed the component.
	auto componentHeader = [&](const char* label, bool removable, bool& removed) -> bool
	{
		const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
		removed = false;
		if (removable && ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem("Remove Component"))
				removed = true;
			ImGui::EndPopup();
		}
		return open && !removed;
	};
	bool removed = false;

	// ── Transform ───────────────────────────────────────────────────────────
	if (auto* t = registry.try_get<TransformComponent>(entity))
	{
		if (componentHeader("Transform", true, removed))
		{
			bool changed = false;
			changed |= ImGui::DragFloat3("Position", &t->position.x, 0.05f); trackEdit();
			changed |= ImGui::DragFloat3("Rotation", &t->rotation.x, 0.5f);  trackEdit();
			changed |= ImGui::DragFloat3("Scale",    &t->scale.x,    0.05f); trackEdit();
			if (changed) t->dirty = true;
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<TransformComponent>(entity); }
	}

	// ── Transform 2D ────────────────────────────────────────────────────────
	if (auto* t = registry.try_get<Transform2DComponent>(entity))
	{
		if (componentHeader("Transform 2D", true, removed))
		{
			bool changed = false;
			changed |= ImGui::DragFloat2("Position##2d", &t->position.x, 0.05f); trackEdit();
			changed |= ImGui::DragFloat("Rotation##2d",  &t->rotation,   0.5f);  trackEdit();
			changed |= ImGui::DragFloat2("Scale##2d",    &t->scale.x,    0.05f); trackEdit();
			if (changed) t->dirty = true;
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<Transform2DComponent>(entity); }
	}

	// ── Mesh ────────────────────────────────────────────────────────────────
	if (auto* m = registry.try_get<MeshComponent>(entity))
	{
		if (componentHeader("Mesh", true, removed))
		{
			if (m->meshAssetId == HE::UUID{})
				ImGui::TextDisabled("Asset: (none — renders fallback cube)");
			else if (ctx.contentManager)
			{
				const StaticMeshAsset* asset = ctx.contentManager->getStaticMesh(m->meshAssetId);
				ImGui::Text("Asset: %s", asset ? asset->name.c_str() : "(not loaded)");
			}
			int lod = m->lodBias;
			if (ImGui::InputInt("LOD Bias", &lod))
				m->lodBias = static_cast<uint8_t>(std::clamp(lod, 0, 255));
			ImGui::Checkbox("Casts Shadow",    &m->castsShadow); trackEdit();
			ImGui::Checkbox("Receives Shadow", &m->receivesShadow); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<MeshComponent>(entity); }
	}

	// ── Skeletal Mesh ────────────────────────────────────────────────────────
	if (auto* sm = registry.try_get<SkeletalMeshComponent>(entity))
	{
		if (componentHeader("Skeletal Mesh", true, removed))
		{
			if (sm->meshAssetId == HE::UUID{})
				ImGui::TextDisabled("Asset: (none)");
			else if (ctx.contentManager)
			{
				const SkeletalMeshAsset* asset = ctx.contentManager->getSkeletalMesh(sm->meshAssetId);
				ImGui::Text("Asset: %s", asset ? asset->name.c_str() : "(not loaded)");
				if (asset)
					ImGui::Text("Joints: %d | Bone matrices: %d",
					    (int)asset->skeleton.size(), (int)sm->boneMatrices.size());
			}
			ImGui::Checkbox("Casts Shadow",    &sm->castsShadow);    trackEdit();
			ImGui::Checkbox("Receives Shadow", &sm->receivesShadow); trackEdit();

			// Drag-drop asset slot
			ImGui::TextUnformatted("Asset");
			ImGui::SameLine();
			const SkeletalMeshAsset* cur = (sm->meshAssetId != HE::UUID{} && ctx.contentManager)
			    ? ctx.contentManager->getSkeletalMesh(sm->meshAssetId) : nullptr;
			const std::string label = cur ? cur->name : (sm->meshAssetId == HE::UUID{} ? "(none)" : "(not loaded)");
			ImGui::Button((label + "##smslot").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
					    static_cast<const char*>(p->Data),
					    ctx.contentManager ? ctx.contentManager->contentRoot() : "",
					    ec).generic_string();
					if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (id != HE::UUID{} && ctx.contentManager->getSkeletalMesh(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							sm->meshAssetId = id;
							sm->dirty = true;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<SkeletalMeshComponent>(entity); }
	}

	// ── Animator ────────────────────────────────────────────────────────────
	if (auto* an = registry.try_get<AnimatorComponent>(entity))
	{
		if (componentHeader("Animator", true, removed))
		{
			// Clip asset slot
			ImGui::TextUnformatted("Clip");
			ImGui::SameLine();
			const AnimationClipAsset* cur = (an->clipAssetId != HE::UUID{} && ctx.contentManager)
				? ctx.contentManager->getAnimationClip(an->clipAssetId) : nullptr;
			const std::string clipLabel = cur ? cur->name
				: (an->clipAssetId == HE::UUID{} ? "(none)" : "(not loaded)");
			ImGui::Button((clipLabel + "##animslot").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
						static_cast<const char*>(p->Data),
						ctx.contentManager ? ctx.contentManager->contentRoot() : "",
						ec).generic_string();
					if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (id != HE::UUID{} && ctx.contentManager->getAnimationClip(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							an->clipAssetId = id;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			// Playback controls
			ImGui::DragFloat("Speed##an",    &an->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##an",     &an->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##an",   &an->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##an",   &an->playing); trackEdit();

			if (cur)
				ImGui::Text("Duration: %.3f s  |  Channels: %d",
					cur->duration, (int)cur->channels.size());
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorComponent>(entity); }
	}

	// ── Animator Blend ───────────────────────────────────────────────────────
	if (auto* ab = registry.try_get<AnimatorBlendComponent>(entity))
	{
		if (componentHeader("Animator Blend", true, removed))
		{
			auto clipSlot = [&](const char* label, HE::UUID& slotId)
			{
				ImGui::TextUnformatted(label);
				ImGui::SameLine();
				const AnimationClipAsset* cur = (slotId != HE::UUID{} && ctx.contentManager)
					? ctx.contentManager->getAnimationClip(slotId) : nullptr;
				const std::string clipLabel = cur ? cur->name
					: (slotId == HE::UUID{} ? "(none)" : "(not loaded)");
				ImGui::Button((clipLabel + "##" + label).c_str());
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
					{
						std::error_code ec;
						std::string rel = std::filesystem::relative(
							static_cast<const char*>(p->Data),
							ctx.contentManager ? ctx.contentManager->contentRoot() : "",
							ec).generic_string();
						if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
						{
							const HE::UUID id = ctx.contentManager->loadAsset(rel);
							if (id != HE::UUID{} && ctx.contentManager->getAnimationClip(id))
							{
								if (ctx.undoSys) ctx.undoSys->snapshotNow();
								slotId = id;
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
			};

			clipSlot("Clip A", ab->clipAId);
			clipSlot("Clip B", ab->clipBId);
			ImGui::SliderFloat("Blend##ab",  &ab->blendAlpha,    0.0f, 1.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Speed##ab",    &ab->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##ab",     &ab->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##ab",   &ab->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##ab",   &ab->playing); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorBlendComponent>(entity); }
	}

	// ── Animator State Machine ──────────────────────────────────────────────
	if (auto* asm_ = registry.try_get<AnimatorStateMachineComponent>(entity))
	{
		if (componentHeader("Animator State Machine", true, removed))
		{
			// Current state + params read-out
			ImGui::LabelText("State##sm",  asm_->currentStateName.empty() ? "(none)" : asm_->currentStateName.c_str());
			ImGui::DragFloat("Speed##sm",  &asm_->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##sm",   &asm_->clipTime,      0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			if (asm_->inTransition)
			{
				ImGui::LabelText("→##sm", asm_->transitionTarget.c_str());
				float pct = asm_->transitionDuration > 0.0f
					? asm_->transitionElapsed / asm_->transitionDuration : 0.0f;
				ImGui::ProgressBar(std::min(pct, 1.0f), ImVec2(-1, 0), "crossfade");
			}

			// States list
			if (ImGui::TreeNodeEx("States##sm", ImGuiTreeNodeFlags_DefaultOpen))
			{
				int stateToDelete = -1;
				for (int i = 0; i < static_cast<int>(asm_->states.size()); ++i)
				{
					auto& s = asm_->states[i];
					ImGui::PushID(i);
					char buf[128]; std::snprintf(buf, sizeof(buf), "%s", s.name.c_str());
					if (ImGui::InputText("Name##s", buf, sizeof(buf)))
					{ s.name = buf; trackEdit(); }
					ImGui::SameLine();
					ImGui::Checkbox("Loop##s", &s.looping); trackEdit();
					ImGui::SameLine();
					if (ImGui::SmallButton("Set##s"))
					{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->currentStateName = s.name; }
					ImGui::SameLine();
					if (ImGui::SmallButton("X##s")) stateToDelete = i;
					ImGui::PopID();
				}
				if (stateToDelete >= 0)
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->states.erase(asm_->states.begin() + stateToDelete); }
				if (ImGui::SmallButton("+ State##sm"))
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->states.push_back({"NewState", HE::UUID{}, true}); }
				ImGui::TreePop();
			}

			// Transitions list
			if (ImGui::TreeNodeEx("Transitions##sm", ImGuiTreeNodeFlags_None))
			{
				const char* opNames[] = { ">", "<", "==" };
				int transToDelete = -1;
				for (int i = 0; i < static_cast<int>(asm_->transitions.size()); ++i)
				{
					auto& t = asm_->transitions[i];
					ImGui::PushID(i + 1000);
					char fb[64], tb[64], pb[64];
					std::snprintf(fb, sizeof(fb), "%s", t.fromState.c_str());
					std::snprintf(tb, sizeof(tb), "%s", t.toState.c_str());
					std::snprintf(pb, sizeof(pb), "%s", t.paramName.c_str());
					if (ImGui::InputText("From##t", fb, sizeof(fb)))  { t.fromState  = fb; trackEdit(); }
					ImGui::SameLine();
					if (ImGui::InputText("To##t",   tb, sizeof(tb)))  { t.toState    = tb; trackEdit(); }
					int opIdx = static_cast<int>(t.op);
					if (ImGui::Combo("Op##t", &opIdx, opNames, 3))
					{ t.op = static_cast<TransitionOp>(opIdx); trackEdit(); }
					ImGui::SameLine();
					if (ImGui::InputText("Param##t", pb, sizeof(pb))) { t.paramName  = pb; trackEdit(); }
					ImGui::DragFloat("Thresh##t", &t.threshold, 0.01f, -999.0f, 999.0f, "%.2f"); trackEdit();
					ImGui::DragFloat("Duration##t", &t.duration, 0.01f, 0.0f, 10.0f, "%.2f s"); trackEdit();
					ImGui::SameLine();
					if (ImGui::SmallButton("X##t")) transToDelete = i;
					ImGui::PopID();
				}
				if (transToDelete >= 0)
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->transitions.erase(asm_->transitions.begin() + transToDelete); }
				if (ImGui::SmallButton("+ Transition##sm"))
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->transitions.push_back({}); }
				ImGui::TreePop();
			}

			// Float params
			if (ImGui::TreeNodeEx("Params##sm", ImGuiTreeNodeFlags_None))
			{
				for (auto& [k, v] : asm_->params)
				{
					ImGui::PushID(k.c_str());
					ImGui::DragFloat(k.c_str(), &v, 0.01f, -999.0f, 999.0f, "%.2f"); trackEdit();
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorStateMachineComponent>(entity); }
	}

	// ── Property Animator ───────────────────────────────────────────────────
	if (auto* pa = registry.try_get<PropertyAnimatorComponent>(entity))
	{
		if (componentHeader("Property Animator", true, removed))
		{
			// Clip drag-drop slot
			ImGui::TextUnformatted("Clip");
			ImGui::SameLine();
			const PropertyAnimClipAsset* cur = (pa->clipId != HE::UUID{} && ctx.contentManager)
				? ctx.contentManager->getPropertyAnimClip(pa->clipId) : nullptr;
			const std::string clipLabel = cur ? cur->name : (pa->clipId == HE::UUID{} ? "(none)" : "(not loaded)");
			ImGui::Button((clipLabel + "##pac").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
						static_cast<const char*>(p->Data),
						ctx.contentManager ? ctx.contentManager->contentRoot() : "",
						ec).generic_string();
					if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (id != HE::UUID{} && ctx.contentManager->getPropertyAnimClip(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							pa->clipId = id;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			ImGui::DragFloat("Speed##pa",  &pa->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##pa",   &pa->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##pa", &pa->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##pa", &pa->playing); trackEdit();

			if (cur)
			{
				ImGui::Separator();
				ImGui::Text("Duration: %.2f s | Channels: %zu", cur->duration, cur->channels.size());
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<PropertyAnimatorComponent>(entity); }
	}

	// ── NavMesh ─────────────────────────────────────────────────────────────
	if (auto* nmc = registry.try_get<NavMeshComponent>(entity))
	{
		if (componentHeader("Nav Mesh", true, removed))
		{
			ImGui::DragFloat("Cell Size##nm",       &nmc->config.cellSize,      0.01f, 0.05f, 2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Cell Height##nm",     &nmc->config.cellHeight,    0.01f, 0.05f, 2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Height##nm",     &nmc->config.walkableHeight,0.1f,  0.5f,  5.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Climb##nm",      &nmc->config.walkableClimb, 0.1f,  0.0f,  2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Radius##nm",     &nmc->config.walkableRadius,0.05f, 0.0f,  2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Max Slope##nm",       &nmc->config.maxSlope,      1.0f,  0.0f,  90.0f,  "%.1f°"); trackEdit();
			ImGui::Separator();
			ImGui::Text("Geometry: %zu verts  %zu tris",
				nmc->geometry.verts.size() / 3,
				nmc->geometry.tris.size()  / 3);
			const bool baked = (bool)nmc->navMesh;
			ImGui::Text("NavMesh: %s", baked ? "baked" : "not baked");
			if (ImGui::Button("Bake##nm"))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				NavigationSystem::bake(*nmc);
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<NavMeshComponent>(entity); }
	}

	// ── NavAgent ────────────────────────────────────────────────────────────
	if (auto* na = registry.try_get<NavAgentComponent>(entity))
	{
		if (componentHeader("Nav Agent", true, removed))
		{
			ImGui::DragFloat3("Target##na",     glm::value_ptr(na->targetPos), 0.1f); trackEdit();
			ImGui::DragFloat("Speed##na",       &na->speed,        0.1f, 0.0f, 20.0f, "%.1f m/s"); trackEdit();
			ImGui::DragFloat("Stop Dist##na",   &na->stoppingDist, 0.01f,0.0f, 2.0f,  "%.2f m"); trackEdit();
			ImGui::Separator();
			ImGui::Text("Path: %zu pts  idx=%zu  %s",
				na->path.size(), na->pathIdx,
				na->moving ? "MOVING" : "stopped");
			if (ImGui::Button("Go##na"))
			{ na->hasPath = false; na->moving = true; }
			ImGui::SameLine();
			if (ImGui::Button("Stop##na"))
			{ na->moving = false; na->hasPath = false; }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<NavAgentComponent>(entity); }
	}

	// ── Material ────────────────────────────────────────────────────────────
	if (auto* m = registry.try_get<MaterialComponent>(entity))
	{
		if (componentHeader("Material", true, removed))
		{
			// Resolve a Content-Browser drag (absolute path) to a content-root-
			// relative path, returning empty if it lives outside the root.
			auto toRelative = [&](const char* absPath) -> std::string
			{
				if (!ctx.contentManager) return {};
				std::error_code ec;
				std::string rel = std::filesystem::relative(
					absPath, ctx.contentManager->contentRoot(), ec).generic_string();
				if (ec || rel.empty() || rel.rfind("..", 0) == 0) return {};
				return rel;
			};

			// ── Material asset slot — drop a material .hasset here ────────────
			const MaterialAsset* asset = (m->materialAssetId == HE::UUID{} || !ctx.contentManager)
				? nullptr : ctx.contentManager->getMaterial(m->materialAssetId);
			const std::string slotLabel = (m->materialAssetId == HE::UUID{})
				? std::string("(none — drop a material here)")
				: (asset ? asset->name : std::string("(not loaded)"));

			ImGui::TextUnformatted("Asset");
			ImGui::SameLine();
			ImGui::Button((slotLabel + "##matslot").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					const std::string rel = toRelative(static_cast<const char*>(p->Data));
					const HE::UUID id = rel.empty() ? HE::UUID{}
					                                : ctx.contentManager->loadAsset(rel);
					if (id != HE::UUID{} && ctx.contentManager->getMaterial(id))
					{
						if (ctx.undoSys) ctx.undoSys->snapshotNow();
						m->materialAssetId = id;
						m->dirty = true;
						if (ctx.renderer) ctx.renderer->InvalidateMaterial(id);
					}
					else
						Logger::Log(Logger::LogLevel::Warning,
							"Editor: dropped asset is not a material");
				}
				ImGui::EndDragDropTarget();
			}
			if (m->materialAssetId != HE::UUID{})
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear"))
				{
					if (ctx.undoSys) ctx.undoSys->snapshotNow();
					m->materialAssetId = HE::UUID{};
					m->dirty = true;
				}
			}

			// ── Editable slots of the assigned material ──────────────────────
			MaterialAsset* mat = (m->materialAssetId == HE::UUID{} || !ctx.contentManager)
				? nullptr : ctx.contentManager->getMaterialMutable(m->materialAssetId);
			if (mat)
			{
				ImGui::Separator();
				ImGui::TextDisabled("%s", mat->path.c_str());

				// Shader path
				char sbuf[260];
				std::strncpy(sbuf, mat->shaderPath.c_str(), sizeof(sbuf) - 1);
				sbuf[sizeof(sbuf) - 1] = '\0';
				if (ImGui::InputText("Shader", sbuf, sizeof(sbuf)))
					mat->shaderPath = sbuf;

				// Surface (PBR scalars) — applied live (the renderer reads them
				// from the shared MaterialAsset each frame); "Save Material" persists.
				ImGui::SeparatorText("Surface");
				ImGui::ColorEdit3("Base Color", mat->baseColor);
				ImGui::SliderFloat("Metallic",  &mat->metallic,  0.0f, 1.0f, "%.2f");
				ImGui::SliderFloat("Roughness", &mat->roughness, 0.0f, 1.0f, "%.2f");
				// Opacity < 1 routes the object into the sorted, alpha-blended
				// transparency pass.
				ImGui::SliderFloat("Opacity",   &mat->opacity,   0.0f, 1.0f, "%.2f");

				// Texture slots — editable text + per-slot drop target + remove.
				ImGui::TextUnformatted("Textures");
				int removeSlot = -1;
				for (size_t i = 0; i < mat->texturePaths.size(); ++i)
				{
					ImGui::PushID(static_cast<int>(i));
					char tbuf[260];
					std::strncpy(tbuf, mat->texturePaths[i].c_str(), sizeof(tbuf) - 1);
					tbuf[sizeof(tbuf) - 1] = '\0';
					ImGui::SetNextItemWidth(-30.0f);
					if (ImGui::InputText("##tex", tbuf, sizeof(tbuf)))
						mat->texturePaths[i] = tbuf;
					if (ImGui::IsItemDeactivatedAfterEdit() && ctx.renderer)
						ctx.renderer->InvalidateMaterial(m->materialAssetId);
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
						{
							const std::string rel = toRelative(static_cast<const char*>(p->Data));
							if (!rel.empty())
							{
								mat->texturePaths[i] = rel;
								if (ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
							}
						}
						ImGui::EndDragDropTarget();
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("X")) removeSlot = static_cast<int>(i);
					ImGui::PopID();
				}
				if (removeSlot >= 0)
				{
					mat->texturePaths.erase(mat->texturePaths.begin() + removeSlot);
					if (ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
				}
				if (ImGui::SmallButton("+ Texture Slot"))
					mat->texturePaths.emplace_back();

				ImGui::Spacing();
				if (ImGui::Button("Save Material"))
				{
					const bool ok = ctx.contentManager->saveAsset(*mat);
					if (ok && ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
					Logger::Log(ok ? Logger::LogLevel::Info : Logger::LogLevel::Error,
						("Editor: " + std::string(ok ? "saved" : "failed to save")
						 + " material '" + mat->name + "'").c_str());
				}
				ImGui::SameLine();
				ImGui::TextDisabled("(edits apply live; Save writes to disk)");
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<MaterialComponent>(entity); }
	}

	// ── Camera ──────────────────────────────────────────────────────────────
	if (auto* c = registry.try_get<CameraComponent>(entity))
	{
		if (componentHeader("Camera", true, removed))
		{
			ImGui::DragFloat("FOV",        &c->fovDegrees, 0.5f, 1.0f, 179.0f); trackEdit();
			ImGui::DragFloat("Near Plane", &c->nearPlane,  0.01f, 0.001f, 100.0f); trackEdit();
			ImGui::DragFloat("Far Plane",  &c->farPlane,   1.0f,  0.1f, 100000.0f); trackEdit();
			ImGui::Checkbox("Main Camera", &c->isMain); trackEdit();
			ImGui::Checkbox("Orthographic", &c->orthographic); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<CameraComponent>(entity); }
	}

	// ── Light ───────────────────────────────────────────────────────────────
	if (auto* l = registry.try_get<LightComponent>(entity))
	{
		if (componentHeader("Light", true, removed))
		{
			static const char* kLightTypes[] = { "Directional", "Point", "Spot" };
			int type = static_cast<int>(l->type);
			if (ImGui::Combo("Type", &type, kLightTypes, 3))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				l->type = static_cast<LightType>(type);
			}
			ImGui::ColorEdit3("Color",    &l->color.x); trackEdit();
			ImGui::DragFloat("Intensity", &l->intensity, 0.05f, 0.0f, 1000.0f); trackEdit();
			if (l->type != LightType::Directional)
				ImGui::DragFloat("Range", &l->range, 0.1f, 0.0f, 10000.0f); trackEdit();
			if (l->type == LightType::Spot)
				ImGui::DragFloat("Spot Angle", &l->spotAngle, 0.5f, 1.0f, 179.0f); trackEdit();
			ImGui::Checkbox("Casts Shadow##light", &l->castsShadow); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<LightComponent>(entity); }
	}

	// ── Rigid Body ──────────────────────────────────────────────────────────
	if (auto* r = registry.try_get<RigidBodyComponent>(entity))
	{
		if (componentHeader("Rigid Body", true, removed))
		{
			static const char* kBodyTypes[] = { "Static", "Dynamic", "Kinematic" };
			int type = static_cast<int>(r->type);
			if (ImGui::Combo("Body Type", &type, kBodyTypes, 3))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				r->type = static_cast<RigidBodyType>(type);
			}
			ImGui::DragFloat("Mass",        &r->mass,        0.1f, 0.0f, 100000.0f); trackEdit();
			ImGui::DragFloat("Friction",    &r->friction,    0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat("Restitution", &r->restitution, 0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::Checkbox("2D Physics",   &r->is2D); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<RigidBodyComponent>(entity); }
	}

	// ── Collider ──────────────────────────────────────────────────────────────
	if (auto* col = registry.try_get<ColliderComponent>(entity))
	{
		if (componentHeader("Collider", true, removed))
		{
			static const char* kShapes[] = { "Box", "Sphere", "Capsule" };
			int shape = static_cast<int>(col->shape);
			if (ImGui::Combo("Shape", &shape, kShapes, 3))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				col->shape = static_cast<ColliderShape>(shape);
			}
			switch (col->shape)
			{
			case ColliderShape::Box:
				ImGui::DragFloat3("Half Extents", &col->halfExtents.x, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			case ColliderShape::Sphere:
				ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			case ColliderShape::Capsule:
				ImGui::DragFloat("Radius",       &col->radius, 0.01f, 0.001f, 100.0f); trackEdit();
				ImGui::DragFloat("Total Height", &col->height, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			}
			ImGui::Checkbox("Is Trigger", &col->isTrigger); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ColliderComponent>(entity); }
	}

	// ── Character Controller ──────────────────────────────────────────────────
	if (auto* cc = registry.try_get<CharacterControllerComponent>(entity))
	{
		if (componentHeader("Character Controller", true, removed))
		{
			ImGui::DragFloat("Slope Limit (deg)", &cc->slopeLimit, 0.5f, 1.0f, 90.0f); trackEdit();
			ImGui::DragFloat("Step Height (m)",   &cc->stepHeight, 0.01f, 0.0f, 2.0f); trackEdit();
			ImGui::DragFloat("Skin Width (m)",    &cc->skinWidth,  0.001f, 0.001f, 0.5f); trackEdit();
			ImGui::DragFloat("Mass (kg)",          &cc->mass,       0.5f, 1.0f, 500.0f); trackEdit();
			ImGui::DragFloat("Gravity (m/s²)",     &cc->gravity,    0.1f, 0.0f, 30.0f); trackEdit();
			ImGui::Separator();
			ImGui::BeginDisabled(true);
			ImGui::Checkbox("Is Grounded", &cc->isGrounded);
			float v[3] = { cc->velocity.x, cc->velocity.y, cc->velocity.z };
			ImGui::DragFloat3("Velocity", v);
			ImGui::EndDisabled();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<CharacterControllerComponent>(entity); }
	}

	// ── Script (Lua) ────────────────────────────────────────────────────────
	if (auto* s = registry.try_get<ScriptComponent>(entity))
	{
		if (componentHeader("Script", true, removed))
		{
			char buf[256];
			std::strncpy(buf, s->moduleName.c_str(), sizeof(buf) - 1);
			buf[sizeof(buf) - 1] = '\0';
			if (ImGui::InputText("Script Name", buf, sizeof(buf)))
			{
				s->moduleName = buf;
				trackEdit();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Logical name matching ScriptEngine::loadScript(name, source).\n"
				                  "Script must export onStart(self) and/or onUpdate(self, dt).");
			ImGui::Checkbox("Enabled", &s->enabled); trackEdit();

			// ── Declared properties (M.properties table) ──────────────────
			if (ctx.propScriptEngine && ctx.contentManager && !s->moduleName.empty())
			{
				const ScriptAsset* asset = nullptr;
				if (s->scriptAssetId != HE::UUID{})
					asset = ctx.contentManager->getScript(s->scriptAssetId);
				if (asset && !asset->sourceCode.empty())
				{
					if (!ctx.propScriptEngine->isScriptLoaded(s->moduleName))
						ctx.propScriptEngine->loadScript(s->moduleName, asset->sourceCode);
					auto defs = ctx.propScriptEngine->getScriptProperties(s->moduleName);
					if (!defs.empty())
					{
						ImGui::Separator();
						ImGui::TextDisabled("Properties");
						for (const auto& def : defs)
						{
							auto it = s->properties.find(def.name);
							if (it == s->properties.end())
							{
								s->properties[def.name] = def.defaultVal;
								it = s->properties.find(def.name);
							}
							ScriptPropValue& val = it->second;
							switch (val.type)
							{
							case ScriptPropType::Float:
								if (ImGui::DragFloat(def.name.c_str(), &val.f, 0.1f)) trackEdit();
								break;
							case ScriptPropType::Int:
								if (ImGui::DragInt(def.name.c_str(), &val.i)) trackEdit();
								break;
							case ScriptPropType::Bool:
								if (ImGui::Checkbox(def.name.c_str(), &val.b)) trackEdit();
								break;
							case ScriptPropType::String:
							{
								char sbuf[256];
								std::strncpy(sbuf, val.s.c_str(), sizeof(sbuf) - 1);
								sbuf[sizeof(sbuf) - 1] = '\0';
								if (ImGui::InputText(def.name.c_str(), sbuf, sizeof(sbuf)))
								{
									val.s = sbuf;
									trackEdit();
								}
								break;
							}
							}
						}
					}
				}
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ScriptComponent>(entity); }
	}

	// ── Terrain ─────────────────────────────────────────────────────────────
	if (auto* t = registry.try_get<TerrainComponent>(entity))
	{
		if (componentHeader("Terrain", true, removed))
		{
			bool changed = false;
			changed |= ImGui::DragFloat("Width (X)##tc",    &t->sizeX,      1.0f,  1.0f, 10000.0f, "%.1f m"); trackEdit();
			changed |= ImGui::DragFloat("Depth (Z)##tc",    &t->sizeZ,      1.0f,  1.0f, 10000.0f, "%.1f m"); trackEdit();
			int res = static_cast<int>(t->resolution);
			if (ImGui::SliderInt("Resolution##tc", &res, 2, 512)) { t->resolution = static_cast<uint32_t>(res); changed = true; }
			trackEdit();
			changed |= ImGui::DragFloat("Height Scale##tc", &t->heightScale, 0.5f,  0.0f, 1000.0f,  "%.1f m"); trackEdit();
			ImGui::SeparatorText("Noise");
			changed |= ImGui::InputInt("Seed##tc",          &t->seed);       trackEdit();
			int oct = t->octaves;
			if (ImGui::SliderInt("Octaves##tc",             &oct, 1, 8)) { t->octaves = oct; changed = true; }
			trackEdit();
			changed |= ImGui::DragFloat("Frequency##tc",    &t->frequency,  0.01f, 0.01f, 16.0f, "%.2f"); trackEdit();
			changed |= ImGui::DragFloat("Lacunarity##tc",   &t->lacunarity, 0.01f, 1.0f,  8.0f,  "%.2f"); trackEdit();
			changed |= ImGui::DragFloat("Gain##tc",         &t->gain,       0.01f, 0.0f,  1.0f,  "%.2f"); trackEdit();
			if (changed) t->dirty = true;
			ImGui::Spacing();
			if (ImGui::Button("Regenerate##tc"))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				t->dirty = true;
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<TerrainComponent>(entity); }
	}

	// ── Audio Source ────────────────────────────────────────────────────────
	if (auto* a = registry.try_get<AudioSourceComponent>(entity))
	{
		if (componentHeader("Audio Source", true, removed))
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "%llu:%llu", (unsigned long long)a->assetId.hi,
			         (unsigned long long)a->assetId.lo);
			ImGui::LabelText("Asset ID", "%s", buf);
			char busBuf[64];
			std::strncpy(busBuf, a->busName.c_str(), sizeof(busBuf) - 1);
			busBuf[sizeof(busBuf) - 1] = '\0';
			if (ImGui::InputText("Bus##as", busBuf, sizeof(busBuf))) { a->busName = busBuf; trackEdit(); }
			ImGui::DragFloat("Volume##as", &a->volume, 0.01f, 0.0f, 2.0f); trackEdit();
			ImGui::DragFloat("Pitch##as",  &a->pitch,  0.01f, 0.1f, 4.0f); trackEdit();
			ImGui::Checkbox("Loop##as",        &a->loop);        trackEdit();
			ImGui::Checkbox("Play on Start##as",&a->playOnStart); trackEdit();
			ImGui::Checkbox("Spatial##as",     &a->spatial);     trackEdit();
			if (a->spatial)
			{
				ImGui::DragFloat("Inner Range##as",   &a->innerRange,    0.1f, 0.0f, 1000.0f, "%.1f m"); trackEdit();
				ImGui::DragFloat("Range##as",         &a->range,         0.5f, 0.0f, 1000.0f, "%.1f m"); trackEdit();
				ImGui::DragFloat("Rolloff Factor##as", &a->rolloffFactor, 0.1f, 0.0f, 10.0f,  "%.2f");   trackEdit();
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AudioSourceComponent>(entity); }
	}

	// ── Audio Listener ──────────────────────────────────────────────────────
	if (auto* l = registry.try_get<AudioListenerComponent>(entity))
	{
		if (componentHeader("Audio Listener", true, removed))
		{
			ImGui::DragFloat("Master Volume##al", &l->masterVolume, 0.01f, 0.0f, 2.0f); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AudioListenerComponent>(entity); }
	}

	// ── Particle System ─────────────────────────────────────────────────────
	if (auto* ps = registry.try_get<ParticleSystemComponent>(entity))
	{
		if (componentHeader("Particle System", true, removed))
		{
			ImGui::DragFloat("Emit Rate##ps",      &ps->emitRate,      0.5f, 0.0f, 10000.0f); trackEdit();
			ImGui::DragFloat("Lifetime Min##ps",   &ps->lifetimeMin,   0.05f, 0.01f, 100.0f); trackEdit();
			ImGui::DragFloat("Lifetime Max##ps",   &ps->lifetimeMax,   0.05f, 0.01f, 100.0f); trackEdit();
			ImGui::DragFloat("Start Size##ps",     &ps->startSize,     0.01f, 0.0f, 100.0f); trackEdit();
			ImGui::DragFloat("End Size##ps",       &ps->endSize,       0.01f, 0.0f, 100.0f); trackEdit();
			ImGui::ColorEdit3("Start Color##ps",   glm::value_ptr(ps->startColor)); trackEdit();
			ImGui::ColorEdit3("End Color##ps",     glm::value_ptr(ps->endColor)); trackEdit();
			ImGui::DragFloat("Start Alpha##ps",    &ps->startAlpha,    0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat("End Alpha##ps",      &ps->endAlpha,      0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat3("Velocity##ps",      glm::value_ptr(ps->initialVelocity), 0.1f); trackEdit();
			ImGui::DragFloat("Spread (rad)##ps",   &ps->velocitySpread, 0.01f, 0.0f, 3.14f); trackEdit();
			ImGui::DragFloat3("Gravity##ps",       glm::value_ptr(ps->gravity), 0.1f); trackEdit();
			ImGui::DragInt("Max Particles##ps",    &ps->maxParticles, 1, 1, 10000); trackEdit();
			ImGui::Checkbox("Playing##ps",         &ps->playing); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Looping##ps",         &ps->looping); trackEdit();
			// Live particle count (read-only info).
			ImGui::Text("Live: %zu", ps->particles.size());
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ParticleSystemComponent>(entity); }
	}

	// ── Foliage ──────────────────────────────────────────────────────────────
	if (auto* fol = registry.try_get<FoliageComponent>(entity))
	{
		if (componentHeader("Foliage", true, removed))
		{
			ImGui::DragFloat("Density##fol",       &fol->density,      0.01f, 0.001f, 10.f); if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragFloat("Draw Distance##fol", &fol->drawDistance, 1.0f,  1.0f,  500.f); trackEdit();
			ImGui::DragFloat("Min Scale##fol",     &fol->minScale,     0.01f, 0.01f, 10.f);  if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragFloat("Max Scale##fol",     &fol->maxScale,     0.01f, 0.01f, 10.f);  if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragInt  ("Seed##fol",          &fol->seed,         1);                    if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::Text("Instances: %zu", fol->cachedInstances.size());
			if (ImGui::Button("Regenerate")) { fol->dirty = true; trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<FoliageComponent>(entity); }
	}

	// ── LOD ──────────────────────────────────────────────────────────────────
	if (auto* lod = registry.try_get<LODComponent>(entity))
	{
		if (componentHeader("LOD", true, removed))
		{
			ImGui::Text("Levels: %zu   Active: %u", lod->levels.size(), lod->current);
			ImGui::Spacing();
			for (int li = 0; li < static_cast<int>(lod->levels.size()); ++li)
			{
				auto& lvl = lod->levels[static_cast<size_t>(li)];
				ImGui::PushID(li);
				ImGui::Text("LOD %d", li);
				ImGui::SameLine();
				char distBuf[32];
				std::snprintf(distBuf, sizeof(distBuf), "%.1f", lvl.maxDistance);
				ImGui::SetNextItemWidth(80.f);
				if (ImGui::InputText("##maxDist", distBuf, sizeof(distBuf),
				                     ImGuiInputTextFlags_EnterReturnsTrue))
				{
					lvl.maxDistance = std::strtof(distBuf, nullptr);
					trackEdit();
				}
				ImGui::SameLine();
				ImGui::TextDisabled("UUID %llx", static_cast<unsigned long long>(lvl.meshId.hi));
				if (ImGui::Button("Remove##lodlvl")) { lod->levels.erase(lod->levels.begin() + li); trackEdit(); --li; }
				ImGui::PopID();
			}
			if (ImGui::Button("+ Level")) { lod->levels.push_back({}); trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<LODComponent>(entity); }
	}

	// ── UI Canvas ───────────────────────────────────────────────────────────
	if (auto* cv = registry.try_get<UICanvasComponent>(entity))
	{
		if (componentHeader("UI Canvas", true, removed))
		{
			ImGui::DragFloat("Width##cv",  &cv->width,  1.0f, 1.0f, 7680.0f); trackEdit();
			ImGui::DragFloat("Height##cv", &cv->height, 1.0f, 1.0f, 4320.0f); trackEdit();
			int rm = static_cast<int>(cv->renderMode);
			if (ImGui::Combo("Render Mode##cv", &rm, "Screen Space\0World Space\0")) {
				cv->renderMode = static_cast<UIRenderMode>(rm); trackEdit();
			}
			ImGui::Checkbox("Active##cv", &cv->active); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UICanvasComponent>(entity); }
	}

	// ── UI Element ──────────────────────────────────────────────────────────
	if (auto* el = registry.try_get<UIElementComponent>(entity))
	{
		if (componentHeader("UI Element", true, removed))
		{
			ImGui::DragFloat2("Position##el", glm::value_ptr(el->position), 1.0f); trackEdit();
			ImGui::DragFloat2("Size##el",     glm::value_ptr(el->size),     1.0f, 0.0f, 10000.0f); trackEdit();
			ImGui::DragFloat2("Pivot##el",    glm::value_ptr(el->pivot),    0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat("Rotation##el",  &el->rotation, 0.5f); trackEdit();
			int anch = static_cast<int>(el->anchor);
			const char* anchNames = "Top Left\0Top Center\0Top Right\0"
			                        "Mid Left\0Mid Center\0Mid Right\0"
			                        "Bot Left\0Bot Center\0Bot Right\0";
			if (ImGui::Combo("Anchor##el", &anch, anchNames)) {
				el->anchor = static_cast<UIAnchor>(anch); trackEdit();
			}
			ImGui::DragInt("Layer##el",  &el->layer, 1); trackEdit();
			ImGui::Checkbox("Active##el", &el->active); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIElementComponent>(entity); }
	}

	// ── UI Text ─────────────────────────────────────────────────────────────
	if (auto* txt = registry.try_get<UITextComponent>(entity))
	{
		if (componentHeader("UI Text", true, removed))
		{
			char buf[256];
			strncpy(buf, txt->text.c_str(), sizeof(buf) - 1); buf[sizeof(buf)-1] = '\0';
			if (ImGui::InputText("Text##txt", buf, sizeof(buf))) { txt->text = buf; trackEdit(); }
			ImGui::DragFloat("Font Size##txt", &txt->fontSize, 0.5f, 4.0f, 256.0f); trackEdit();
			ImGui::ColorEdit4("Color##txt",    glm::value_ptr(txt->color)); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UITextComponent>(entity); }
	}

	// ── UI Image ─────────────────────────────────────────────────────────────
	if (auto* img = registry.try_get<UIImageComponent>(entity))
	{
		if (componentHeader("UI Image", true, removed))
		{
			ImGui::ColorEdit4("Tint##img", glm::value_ptr(img->tint)); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIImageComponent>(entity); }
	}

	// ── UI Button ───────────────────────────────────────────────────────────
	if (auto* btn = registry.try_get<UIButtonComponent>(entity))
	{
		if (componentHeader("UI Button", true, removed))
		{
			ImGui::ColorEdit4("Normal##btn",  glm::value_ptr(btn->normalColor)); trackEdit();
			ImGui::ColorEdit4("Hovered##btn", glm::value_ptr(btn->hoveredColor)); trackEdit();
			ImGui::ColorEdit4("Pressed##btn", glm::value_ptr(btn->pressedColor)); trackEdit();
			char buf[128];
			strncpy(buf, btn->onClickFunction.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
			if (ImGui::InputText("OnClick##btn", buf, sizeof(buf))) { btn->onClickFunction = buf; trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIButtonComponent>(entity); }
	}

	// ── Add Component ───────────────────────────────────────────────────────
	// Not for the World root — it only carries the scene's Environment, no
	// arbitrary components (and the built-in sun/moon are managed automatically).
	if (!ctx.world->isBuiltin(entity))
	{
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		const float buttonW = 180.0f;
		ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttonW) * 0.5f
		                     + ImGui::GetCursorPosX());
		if (ImGui::Button("Add Component", ImVec2(buttonW, 0)))
			ImGui::OpenPopup("##add_component");

		if (ImGui::BeginPopup("##add_component"))
		{
			auto addItem = [&]<typename T>(const char* label, T)
			{
				if (!registry.all_of<T>(entity) && ImGui::MenuItem(label))
				{
					if (ctx.undoSys) ctx.undoSys->snapshotNow();
					registry.emplace<T>(entity);
				}
			};
			addItem("Transform",    TransformComponent{});
			addItem("Transform 2D", Transform2DComponent{});
			addItem("Mesh",          MeshComponent{});
			addItem("Skeletal Mesh", SkeletalMeshComponent{});
			addItem("Animator",       AnimatorComponent{});
			addItem("Animator Blend",          AnimatorBlendComponent{});
			addItem("Animator State Machine",  AnimatorStateMachineComponent{});
			addItem("Property Animator",       PropertyAnimatorComponent{});
			addItem("Nav Mesh",                NavMeshComponent{});
			addItem("Nav Agent",               NavAgentComponent{});
			addItem("Material",     MaterialComponent{});
			addItem("Camera",       CameraComponent{});
			addItem("Light",        LightComponent{});
			addItem("Rigid Body",          RigidBodyComponent{});
			addItem("Collider",            ColliderComponent{});
			addItem("Character Controller", CharacterControllerComponent{});
			addItem("Script",         ScriptComponent{});
			addItem("Audio Source",    AudioSourceComponent{});
			addItem("Audio Listener",  AudioListenerComponent{});
			addItem("Particle System", ParticleSystemComponent{});
			addItem("LOD",             LODComponent{});
			addItem("Foliage",         FoliageComponent{});
			addItem("UI Canvas",       UICanvasComponent{});
			addItem("UI Element",      UIElementComponent{});
			addItem("UI Text",         UITextComponent{});
			addItem("UI Image",        UIImageComponent{});
			addItem("UI Button",       UIButtonComponent{});
			ImGui::EndPopup();
		}
	}

	ImGui::End();
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}