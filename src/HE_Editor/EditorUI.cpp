#include "EditorUI.h"
#include "EditorApplication.h"
#include <HorizonScene/HorizonScene.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <Math/AABB.h>
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
#include <array>

// Forward declaration — defined in EditorApplication.cpp
std::string getRHIName(HE::RendererBackend backend);

namespace
{
	// The async SDL file slot (pendingFileReady/Result) is shared across project
	// and scene operations; this records which one is currently in flight so the
	// single result handler can dispatch correctly.
	enum class PendingFileOp { OpenProject, OpenScene, SaveScene };

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

// Requested by fast local content edits (create/rename) that want the file list
// updated this frame without the heavyweight "##ContentRefresh" progress modal.
// Consumed at the top of render(), outside the content-folder shared lock.
static bool s_quietContentRefresh = false;

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

static void DrawPreferencesWindow(AppContext& ctx, bool& open)
{
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
	                        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	if (ImGui::Begin("Preferences", &open, ImGuiWindowFlags_NoCollapse))
	{
		EditorConfig& cfg = ctx.editorConfig;

		ImGui::SeparatorText("Appearance");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("UI Font Scale", &cfg.UiFontScale, 0.5f, 2.0f, "%.2fx");
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset##fontscale")) cfg.UiFontScale = 1.0f;

		ImGui::SeparatorText("Viewport");
		ImGui::Checkbox("Show Grid", &cfg.ShowGrid);
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::SliderFloat("Camera Speed", &cfg.EditorCameraSpeed, 1.0f, 50.0f, "%.1f u/s")
		    && ctx.editorCamera)
			ctx.editorCamera->setFlySpeed(cfg.EditorCameraSpeed);

		ImGui::SeparatorText("Rendering");
		if (ImGui::Checkbox("VSync", &ctx.vsync))
			ApplyVSync(ctx);

		// Bloom — pushed to the renderer each frame from these prefs.
		ImGui::Checkbox("Bloom", &cfg.BloomEnabled);
		ImGui::BeginDisabled(!cfg.BloomEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Threshold", &cfg.BloomThreshold, 0.0f, 4.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Intensity", &cfg.BloomIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();

		// SSAO (screen-space ambient occlusion) — darkens the ambient in crevices.
		ImGui::Checkbox("SSAO", &cfg.SSAOEnabled);
		ImGui::BeginDisabled(!cfg.SSAOEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("SSAO Radius", &cfg.SSAORadius, 0.05f, 2.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("SSAO Intensity", &cfg.SSAOIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();

		ImGui::SeparatorText("Content Browser");
		ImGui::Checkbox("Keep CPU Asset Cache", &cfg.KeepCPUAssets);
		ImGui::SetNextItemWidth(120.0f);
		ImGui::InputInt("Refresh Interval (s)", &cfg.ContentBrowserRefreshRate);
		if (cfg.ContentBrowserRefreshRate < 0) cfg.ContentBrowserRefreshRate = 0;

		ImGui::Separator();
		ImGui::TextDisabled("Preferences are saved when the editor exits.");
		if (ImGui::Button("Restore Defaults"))
		{
			cfg.UiFontScale       = 1.0f;
			cfg.EditorCameraSpeed = 6.0f;
			cfg.ShowGrid          = true;
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
		if (ImGui::MenuItem("Import Asset...")) {}
		if (ImGui::MenuItem("Refresh Assets")) {}
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
		ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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

				// ── Ground grid (world XZ plane) ────────────────────────────
				if (ctx.editorConfig.ShowGrid && !ctx.isPlaying)
				{
					ImGuizmo::SetOrthographic(false);
					ImGuizmo::SetDrawlist();
					ImGuizmo::SetRect(rectMin.x, rectMin.y,
					                  rectMax.x - rectMin.x, rectMax.y - rectMin.y);
					const glm::mat4 gridXform(1.0f);
					ImGuizmo::DrawGrid(&s_sceneSnapshot.camera.view[0][0],
					                   &s_sceneSnapshot.camera.projection[0][0],
					                   &gridXform[0][0], 20.0f);
				}

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
					// (but not while flying — W/A/S/D drive the camera then).
					static ImGuizmo::OPERATION s_op = ImGuizmo::TRANSLATE;
					if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput && !navigating)
					{
						if (ImGui::IsKeyPressed(ImGuiKey_W)) s_op = ImGuizmo::TRANSLATE;
						if (ImGui::IsKeyPressed(ImGuiKey_E)) s_op = ImGuizmo::ROTATE;
						if (ImGui::IsKeyPressed(ImGuiKey_R)) s_op = ImGuizmo::SCALE;
					}

					ImGuizmo::SetOrthographic(false);
					ImGuizmo::SetDrawlist();
					ImGuizmo::SetRect(rectMin.x, rectMin.y,
					                  rectMax.x - rectMin.x, rectMax.y - rectMin.y);

					// Pre-state for undo — captured while hovering, before
					// the first Manipulate of a drag session mutates anything.
					if (ctx.undoSys && !ImGuizmo::IsUsing())
						ctx.undoSys->capturePre();

					glm::mat4 world = t->worldMatrix;
					ImGuizmo::Manipulate(
						&s_sceneSnapshot.camera.view[0][0],
						&s_sceneSnapshot.camera.projection[0][0],
						s_op, ImGuizmo::LOCAL, &world[0][0]);

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

					// Local-space AABBs per mesh asset, cached. Entities
					// without an asset use the built-in fallback cube's box.
					static std::unordered_map<HE::UUID, HE::AABB> s_aabbCache;
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

    // ── Quick Settings panel ─────────────────────────────────────────────────
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("Quick Settings");
    if (ctx.fontHeading) ImGui::PopFont();

    ImGui::SetNextItemOpen(ctx.editorConfig.QsRendererOpen, ImGuiCond_Always);
    ctx.editorConfig.QsRendererOpen = ImGui::CollapsingHeader("Renderer");
    if (ctx.editorConfig.QsRendererOpen)
    {
        ImGui::PushFont(ctx.fontBody);
        ImGui::Text(("Selected RHI: " + ctx.backendName).c_str());
        if (ImGui::BeginCombo("Backend", ctx.backendName.c_str()))
        {
            if (ImGui::Selectable("OpenGL"))
            {
                ctx.globalState->setSelectedRHI(HE::GraphicsAPI::OpenGL);
                ctx.backendName = getRHIName(HE::GraphicsAPI::OpenGL);
            }
            if (ImGui::Selectable("Vulkan"))
            {
                ctx.globalState->setSelectedRHI(HE::GraphicsAPI::Vulkan);
                ctx.backendName = getRHIName(HE::GraphicsAPI::Vulkan);
            }
            if (ImGui::Selectable("DirectX11"))
            {
                ctx.globalState->setSelectedRHI(HE::GraphicsAPI::D3D11);
                ctx.backendName = getRHIName(HE::GraphicsAPI::D3D11);
            }
            if (ImGui::Selectable("DirectX12"))
            {
                ctx.globalState->setSelectedRHI(HE::GraphicsAPI::D3D12);
                ctx.backendName = getRHIName(HE::GraphicsAPI::D3D12);
            }
#ifdef __APPLE__
            if (ImGui::Selectable("Metal"))
            {
                ctx.globalState->setSelectedRHI(HE::GraphicsAPI::Metal);
                ctx.backendName = getRHIName(HE::GraphicsAPI::Metal);
            }
#endif
            ImGui::EndCombo();
        }
        // VSync toggle
        ImGui::Spacing();
        if (ImGui::Checkbox("VSync", &ctx.vsync))
            ApplyVSync(ctx);
        ImGui::PopFont();
    }
    ImGui::SetNextItemOpen(ctx.editorConfig.QsEditorOpen, ImGuiCond_Always);
    ctx.editorConfig.QsEditorOpen = ImGui::CollapsingHeader("Editor");
    if (ctx.editorConfig.QsEditorOpen)
    {
        ImGui::Checkbox("Show Grid", &ctx.editorConfig.ShowGrid);
        ImGui::Checkbox("Keep CPU Asset Cache", &ctx.editorConfig.KeepCPUAssets);

        if (ctx.editorConfig.KeepCPUAssets && !ctx.editorConfig.KeepCPUAssetsInfoAcknoleged)
            ImGui::OpenPopup("##KeepCPUAssetsInfo");

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("##KeepCPUAssetsInfo", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("Keeping CPU copies of assets allows for faster reloads and access at runtime, but uses more RAM. It is recommended to keep this enabled unless you are very tight on memory.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float buttonWidth = 120.0f;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float offset = (ImGui::GetContentRegionAvail().x - buttonWidth * 2 - spacing) * 0.5f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

            if (ImGui::Button("OK", ImVec2(buttonWidth, 0)))
            {
                ctx.editorConfig.KeepCPUAssetsInfoAcknoleged = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Nevermind", ImVec2(buttonWidth, 0)))
            {
                ctx.editorConfig.KeepCPUAssets = false;
                ctx.editorConfig.KeepCPUAssetsInfoAcknoleged = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(25.0f);
        ImGui::InputInt("Content Browser Refresh interval (Seconds)", &ctx.editorConfig.ContentBrowserRefreshRate, 0, 0);
    }

    // ── Environment: day-night cycle ──────────────────────────────────────────
    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Day-Night Cycle", &ctx.editorConfig.DayNightCycle);

        // Format the 0..1 time as a HH:MM clock shown inside the slider.
        int minutes = static_cast<int>(ctx.editorConfig.TimeOfDay * 1440.0f) % 1440;
        if (minutes < 0) minutes += 1440;
        char clock[8];
        std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);

        // Always draggable; moving it starts the cycle so the change is visible.
        // NoRoundToFormat is essential here: the HH:MM string is a *display*
        // format with no numeric specifier, so without this flag ImGui rounds the
        // value by re-parsing the clock (e.g. "12:00" → 12) and the thumb snaps to
        // the extremes instead of dragging.
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##timeofday", &ctx.editorConfig.TimeOfDay, 0.0f, 1.0f,
                               clock, ImGuiSliderFlags_NoRoundToFormat))
            ctx.editorConfig.DayNightCycle = true;

        ImGui::TextDisabled(ctx.editorConfig.DayNightCycle
            ? "Drives the sun, sky & shadows."
            : "Move the slider to start a day-night cycle.");

        // Auto-advance: time flows on its own at an adjustable speed.
        if (ImGui::Checkbox("Auto-Advance", &ctx.editorConfig.DayNightAutoAdvance)
            && ctx.editorConfig.DayNightAutoAdvance)
            ctx.editorConfig.DayNightCycle = true; // animating implies the cycle is on
        ImGui::BeginDisabled(!ctx.editorConfig.DayNightAutoAdvance);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cyclelen", &ctx.editorConfig.DayNightCycleSeconds,
                           5.0f, 600.0f, "Full day: %.0f s", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();

        // Sun & moon light colour + brightness (drive the day-night lights).
        ImGui::SeparatorText("Sun & Moon Light");
        ImGui::ColorEdit3("Sun Color",  &ctx.editorConfig.SunColor.x,
                          ImGuiColorEditFlags_NoInputs);
        ImGui::SliderFloat("Sun Brightness",  &ctx.editorConfig.SunIntensity,
                           0.0f, 10.0f, "%.2f");
        ImGui::ColorEdit3("Moon Color", &ctx.editorConfig.MoonColor.x,
                          ImGuiColorEditFlags_NoInputs);
        ImGui::SliderFloat("Moon Brightness", &ctx.editorConfig.MoonIntensity,
                           0.0f, 10.0f, "%.2f");

        // Cloud amount: 0 = clear sky … 1 = full overcast. At full overcast the
        // sun/moon directional light is switched off and replaced by ambient.
        ImGui::SeparatorText("Clouds");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudcoverage", &ctx.editorConfig.CloudCoverage,
                           0.0f, 1.0f, "Coverage: %.2f");
        ImGui::TextDisabled("Full overcast dims the sun & fills with ambient light.");
        // Wind: the compass direction the clouds drift toward + how fast.
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##winddir", &ctx.editorConfig.WindDirection,
                           0.0f, 360.0f, "Wind direction: %.0f\xc2\xb0");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##windspeed", &ctx.editorConfig.WindSpeed,
                           0.0f, 4.0f, "Wind speed: %.2f");

        // Atmospheric fog / aerial perspective: distant geometry melts into the
        // sky in its view direction. 0 density = off. Height falloff pools the
        // fog near the ground (only meaningful when fog is on).
        ImGui::SeparatorText("Atmospheric Fog");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##fogdensity", &ctx.editorConfig.FogDensity,
                           0.0f, 0.15f, "Density: %.3f");
        ImGui::BeginDisabled(ctx.editorConfig.FogDensity <= 0.0f);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##fogheight", &ctx.editorConfig.FogHeightFalloff,
                           0.0f, 1.0f, "Ground hugging: %.2f");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Distant objects blend into the horizon (warm at sunset).");

        // Night sky: stars + the dense Milky Way band and the space nebula rotate
        // with time-of-day; the aurora is drifting ribbons that sweep the sky.
        ImGui::SeparatorText("Night Sky");
        // (Milky Way intensity slider removed — it read as inert after the band was
        // reworked into dense stars along the galactic plane; the Milky Way still
        // renders at EditorConfig::MilkyWayIntensity.)
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##nebula", &ctx.editorConfig.NebulaIntensity,
                           0.0f, 1.0f, "Space Nebula: %.2f");
        ImGui::ColorEdit3("Nebula Color", &ctx.editorConfig.NebulaColor.x);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##aurora", &ctx.editorConfig.AuroraIntensity,
                           0.0f, 1.0f, "Aurora: %.2f");
        ImGui::ColorEdit3("Aurora Color", &ctx.editorConfig.AuroraColor.x);
        ImGui::TextDisabled("Stars, Milky Way & nebula turn with the day; aurora drifts.");
    }

    ImGui::End();

    // World Outliner
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("World Outliner");
    if (ctx.fontHeading) ImGui::PopFont();

    // ── Outliner world-pointer diagnostic (logs once per change) ─────────
    {
        static HorizonWorld* s_loggedWorld   = reinterpret_cast<HorizonWorld*>(~0ull); // sentinel
        static bool          s_loggedLoaded  = false;
        if (ctx.world != s_loggedWorld || ctx.projectLoaded != s_loggedLoaded)
        {
            s_loggedWorld  = ctx.world;
            s_loggedLoaded = ctx.projectLoaded;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "[Outliner] world ptr=%s  projectLoaded=%s",
                ctx.world   ? "valid (non-null)" : "NULL",
                ctx.projectLoaded ? "true" : "false");
            Logger::Log(HE::LogLevel::Info, buf);
        }
    }

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
            Logger::Log(HE::LogLevel::Info, "World has changed or is dirty... rebuilding cache");
            s_lastWorld = ctx.world;
            s_outlinerCache.clear();

            auto& registry = ctx.world->registry();
            Entity root    = ctx.world->rootEntity();

            // ── Logging: pre-build diagnostics ───────────────────────────
            {
                // Count all entities in the registry
                size_t totalEntities = 0;
                for (auto e : registry.view<entt::entity>()) { (void)e; ++totalEntities; }
                bool rootValid = (root != entt::null) && registry.valid(root);
                auto* rootHier = rootValid ? registry.try_get<HierarchyComponent>(root) : nullptr;
                auto* rootName = rootValid ? registry.try_get<NameComponent>(root)      : nullptr;

                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "[Outliner] Cache rebuild — total registry entities: %zu | root valid: %s | root name: '%s' | root children: %zu",
                    totalEntities,
                    rootValid ? "yes" : "NO",
                    rootName  ? rootName->name.c_str() : "(none)",
                    rootHier  ? rootHier->children.size() : 0u);
                Logger::Log(HE::LogLevel::Info, buf);
            }

            int maxDepth = -1;
            std::function<void(Entity, int)> collect = [&](Entity entity, int depth)
            {
                bool valid = registry.valid(entity);
                auto* name = valid ? registry.try_get<NameComponent>(entity)      : nullptr;
                auto* hier = valid ? registry.try_get<HierarchyComponent>(entity) : nullptr;

                // Per-node log
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "[Outliner]   entity=%u valid=%s depth=%d name='%s' children=%zu",
                        static_cast<uint32_t>(entity),
                        valid ? "yes" : "NO",
                        depth,
                        name ? name->name.c_str() : "(none)",
                        hier ? hier->children.size() : 0u);
                    Logger::Log(HE::LogLevel::Info, buf);
                }

                if (!valid) return;

                s_outlinerCache.push_back({
                    entity,
                    name ? name->name : "(unnamed)",
                    depth,
                    hier && !hier->children.empty()
                });
                if (depth > maxDepth) maxDepth = depth;
                if (hier)
                    for (Entity child : hier->children)
                        collect(child, depth + 1);
            };

            collect(root, 0);

            // ── Logging: post-build summary ───────────────────────────────
            {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "[Outliner] Cache built — %zu nodes, max depth: %d",
                    s_outlinerCache.size(), maxDepth);
                Logger::Log(HE::LogLevel::Info, buf);
            }

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

	// ── Script ──────────────────────────────────────────────────────────────
	if (auto* s = registry.try_get<ScriptComponent>(entity))
	{
		if (componentHeader("Script", true, removed))
		{
			char buf[256];
			std::strncpy(buf, s->moduleName.c_str(), sizeof(buf) - 1);
			buf[sizeof(buf) - 1] = '\0';
			if (ImGui::InputText("Module", buf, sizeof(buf)))
				s->moduleName = buf;
			trackEdit();
			ImGui::Checkbox("Enabled", &s->enabled); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ScriptComponent>(entity); }
	}

	// ── Add Component ───────────────────────────────────────────────────────
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
		addItem("Mesh",         MeshComponent{});
		addItem("Material",     MaterialComponent{});
		addItem("Camera",       CameraComponent{});
		addItem("Light",        LightComponent{});
		addItem("Rigid Body",   RigidBodyComponent{});
		addItem("Script",       ScriptComponent{});
		ImGui::EndPopup();
	}

	ImGui::End();
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}