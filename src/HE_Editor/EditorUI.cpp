#include "EditorUI.h"
#include "EditorApplication.h"
#include <HorizonScene/HorizonScene.h>
#include <ContentManager/HAsset.h>

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

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
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
		if (ImGui::MenuItem("Close Project", "Ctrl+W"))
		{
			ctx.projectManager->closeProject();
			ctx.globalState->setLastProjectPath("");
			ctx.globalState->writeConfig();
			ctx.projectLoaded = false;
		}
        ImGui::Separator();
        if (ImGui::MenuItem("Save Selected", "Ctrl+S")) {}
        if (ImGui::MenuItem("Save all", "Ctrl+Shift+S")) {}
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            ctx.quit();
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
		if (ImGui::MenuItem("Preferences", "Ctrl+,")) {}
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Toggle Fullscreen", "F11")) {}
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
            ImGui::OpenPopup("##EditorOpenError");
        }
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

			if (ctx.toolbarIcons.undo)
				ImGui::ImageButton("##footerUndo", ctx.toolbarIcons.undo, ImVec2(btnSize, btnSize));
			else
				ImGui::Button("Undo");

			ImGui::SameLine(0.0f, 4.0f);

			if (ctx.toolbarIcons.redo)
				ImGui::ImageButton("##footerRedo", ctx.toolbarIcons.redo, ImVec2(btnSize, btnSize));
			else
				ImGui::Button("Redo");

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

        ImGui::DockSpace(ImGui::GetID("##MainDockSpace"), ImVec2(0.0f, 0.0f),
            ImGuiDockNodeFlags_PassthruCentralNode);

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
			static bool s_playing = false;
			constexpr float btnSize = 20.0f;
			const float centerX = (ImGui::GetContentRegionAvail().x - 120 - btnSize) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

			ImTextureID icon = s_playing ? ctx.toolbarIcons.stop : ctx.toolbarIcons.play;
			ImVec4 tint = s_playing
				? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
				: ImVec4(0.35f, 1.0f, 0.55f, 1.0f);

			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.08f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.16f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));

			if (icon)
			{
				if (ImGui::ImageButton("##tbPlay", icon, ImVec2(btnSize, btnSize),
					ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint))
					s_playing = !s_playing;
			}
			else
			{
				if (ImGui::Button(s_playing ? "Stop" : "Play", ImVec2(btnSize * 2.0f, btnSize)))
					s_playing = !s_playing;
			}

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
        {
            if (ctx.window)
                ctx.window->SetVSync(ctx.vsync);
        }
        ImGui::PopFont();
    }
    ImGui::SetNextItemOpen(ctx.editorConfig.QsEditorOpen, ImGuiCond_Always);
    ctx.editorConfig.QsEditorOpen = ImGui::CollapsingHeader("Editor");
    if (ctx.editorConfig.QsEditorOpen)
    {
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

            bool open = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(
                    static_cast<uint32_t>(node.entity))),
                flags, "%s", node.name.c_str());

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
    }
    else
    {
        ImGui::TextDisabled("(no world loaded)");
    }

    ImGui::End();

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

			// Left click → select
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
			}
			// Double-click → open tab
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
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

			if (ImGui::MenuItem("Rename"))
			{
				s_renameTarget   = s_ctxMenuItem;
				s_renameIsFolder = s_ctxMenuIsFolder;
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

		// Trigger rename popup outside item context popup
		if (s_openRenamePopup)
		{
			ImGui::OpenPopup("##cb_rename_popup");
			s_openRenamePopup = false;
		}

		// ── Rename popup ──────────────────────────────────────────────────
		ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
		if (ImGui::BeginPopupModal("##cb_rename_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::TextUnformatted(s_renameIsFolder ? "Rename Folder" : "Rename Asset");
			ImGui::Separator();
			ImGui::Spacing();

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
						ctx.contentRefreshPending = true;
					}
				}
				s_renameTarget.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(140, 0)))
			{
				s_renameTarget.clear();
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

				// Write a minimal binary asset stub so the file exists on disk
				{
					HAsset::Writer w;
					std::vector<uint8_t> meta;
					HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
					HAsset::Writer::appendString(meta, defaultName);
					HAsset::Writer::appendString(meta, relative);
					w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
					w.write(path, static_cast<uint16_t>(type));
				}
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
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		ImGui::EndChild();
    }

    ImGui::End();
#endif // HE_IMGUI_ENABLED
}