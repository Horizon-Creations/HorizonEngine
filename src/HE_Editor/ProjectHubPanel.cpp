#include "ProjectHubPanel.h"
#include "EditorApplication.h"           // AppContext, ProjectManager, EditorConfig
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include "EditorTheme.h"                 // brand palette — the hub is the first
                                         // surface after the splash, so it is the
                                         // one that must not look like a different
                                         // product than the window that preceded it
#include "TutorialPanel.h"               // Help ▸ Interactive Tutorial → sandbox offer
#include "DocsPanel.h"                   // Help ▸ Documentation — readable before a project exists
#include "HorizonVersion.h"
#ifdef __APPLE__
#include "MacMenuBar.h"   // native system menu bar (replaces the ImGui menu row)
#endif
#include <HorizonScene/HcCodegen.h>      // HorizonCode → C++ codegen (compile-on-export)
#include <Types/Enums.h>

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#include <shobjidl.h>
#endif

#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#endif

namespace ProjectHubPanel
{

// ─── Project Hub ──────────────────────────────────────────────────────────────
void render(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // Set by the native File ▸ Open Project menu item below; consumed by the
    // "Browse .heproj…" button handler in Panel 3, so the menu triggers the
    // same file dialog. Unconditional so the button condition compiles on all
    // platforms (only ever set on macOS).
    static bool s_hubOpenBrowseRequested = false;

#ifdef __APPLE__
    // The macOS App-menu (Quit/About/New/Open) is installed AND pumped here too:
    // renderEditor — which normally does this — only runs once a project is
    // loaded, so on the Hub the menu queue was never drained and Cmd+Q (and the
    // other items) did nothing after a project had been opened and closed.
    MacMenuBar::install();                       // idempotent
    if (MacMenuBar::available())
    {
        MacMenuBar::setProjectLoaded(false);     // grey out project-only items
        using MC = MacMenuBar::Cmd;
        for (MC c; (c = MacMenuBar::take()) != MC::None; )
        {
            switch (c)
            {
            case MC::Quit:        if (ctx.quit) ctx.quit();          break;
            case MC::NewProject:  // fresh Create form (Panel 1)
                ctx.hubProjectName[0] = '\0';
                ctx.hubProjectDir[0]  = '\0';
                ctx.hubSelectedPreset = 0;
                ctx.hubSelectedLang   = 0;
                ctx.hubCreateError.clear();
                break;
            case MC::OpenProject: s_hubOpenBrowseRequested = true;   break;
            // No project to tour yet — the tutorial's answer here is its sandbox offer.
            case MC::OpenTutorial: TutorialPanel::showWelcome();      break;
            // The Help items need no project, so they answer on the Hub too — and
            // this is where they are needed most: "how do I start a project" is a
            // question you have before there is one. EditorUI draws the reader on
            // both screens for the same reason. The website URL is spelled out
            // rather than shared with EditorUI.cpp: kDocsUrl is a file-static
            // there, and one literal is cheaper than a header for it.
            case MC::Documentation:       DocsPanel::open();          break;
            case MC::SearchDocumentation: DocsPanel::openSearch("");  break;
            case MC::DocumentationOnline:
                SDL_OpenURL("https://horizoncreations.dev/HorizonEngineDocs/");
                break;
            default: break;   // project-scoped / editor-only items: no-op here
            }
        }
    }
#endif

    ImGui::SetNextWindowPos(vp->Pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);

    const ImGuiWindowFlags kHubFlags =
        ImGuiWindowFlags_NoTitleBar   |
        ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoScrollbar  |
        ImGuiWindowFlags_NoCollapse   |
        ImGuiWindowFlags_NoDocking    |
        // The Hub fills the viewport and is opaque, so anything it comes to the
        // front of is not merely behind it — it is gone. Without this flag,
        // clicking the Hub (into the project name field, onto a recent project)
        // raises it over the windows drawn ON TOP of it: the documentation
        // reader, which is meant to be readable here precisely because there is
        // no project yet, and the tutorial welcome card. A backdrop never needs
        // raising.
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##ProjectHub", nullptr, kHubFlags);
    ImGui::PopStyleVar(3);

    // ── Header bar ────────────────────────────────────────────────────────────
    const float headerH = 56.0f;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, HE::Ed::Theme::warm(0.105f));
    ImGui::BeginChild("##HubHeader", ImVec2(vp->Size.x, headerH), false,
        ImGuiWindowFlags_NoScrollbar);

    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    const char* title = "Horizon Engine " HE_VERSION_STRING " \"" HE_VERSION_CODENAME "\"  —  Project Hub";

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

    // Templates: see kPresetNames/kPresetDescs in the header (shared with the
    // in-editor File ▸ New Project popup).
    // Index order MUST match ProjectScriptLanguage (HorizonCode, Lua, Python, Cpp).
    static const std::array<const char*, 4> kLangNames = {
        "HorizonCode (Visual Scripting)",
        "Lua",
        "Python",
        "C++",
    };
    static const std::array<const char*, 4> kLangDesc = {
        "Node graphs; compiles to native C++ on export.",
        "Lightweight text scripting (default script backend).",
        "CPython scripting (needs a Python install on dev machines).",
        "Native GameLogic library, built with your own toolchain.",
    };

    // ════════════════════════════════════════════════════════════════════
    // PANEL 1 — Create Project
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(0.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, HE::Ed::Theme::warm(0.135f));
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

    {
        // Every line in this block is a full sentence describing a choice, and the
        // column it has to live in is a third of the window — the language
        // descriptions and the "no toolchain" note are both longer than that on a
        // 1280-wide screen. Without a wrap position ImGui runs them straight past
        // the panel's right edge and clips them there, so the reader is told
        // "CPython scripting (needs a Python install on dev" and left to guess.
        // The wrap sits at panelW - padding rather than at the window edge so the
        // text keeps the same margin on the right that every control here has on
        // the left; that is also what the two hand-rolled PushTextWrapPos pairs
        // that used to stand here computed, one setting at a time.
        EditorWidgets::WrapText wrap(panelW - padding);

        ImGui::SetCursorPosX(padding);
        ImGui::Text("Template");
        ImGui::SetCursorPosX(padding);
        ImGui::PushItemWidth(panelW - padding * 2.0f);
        ImGui::ListBox("##Presets", &ctx.hubSelectedPreset,
            kPresetNames, kPresetCount, 5);
        ImGui::PopItemWidth();

        ImGui::SetCursorPosX(padding);
        ImGui::TextDisabled("%s", kPresetDescs[ctx.hubSelectedPreset]);

        ImGui::Spacing();
        ImGui::SetCursorPosX(padding);
        ImGui::Text("Scripting Language");

        // A C++ project is useless without cmake + a working compiler, so drop it from the
        // picker when the startup probe couldn't find them. A null probe = it hasn't
        // finished yet → don't hide prematurely. Index 3 == Cpp (matches kLangNames /
        // ProjectScriptLanguage). See DrawToolchainDialog / HcCodegen::probeToolchain.
        const bool cppToolchainOk =
            !ctx.toolchainProbe ||
            (ctx.toolchainProbe->cmakeFound && ctx.toolchainProbe->compilerFound);
        if (!cppToolchainOk && ctx.hubSelectedLang == 3)
            ctx.hubSelectedLang = 0; // C++ no longer offered — fall back to HorizonCode

        ImGui::SetCursorPosX(padding);
        ImGui::PushItemWidth(panelW - padding * 2.0f);
        if (ImGui::BeginCombo("##HubLang", kLangNames[ctx.hubSelectedLang]))
        {
            for (int i = 0; i < static_cast<int>(kLangNames.size()); ++i)
            {
                if (i == 3 && !cppToolchainOk) continue; // hide C++ when no toolchain
                const bool sel = (ctx.hubSelectedLang == i);
                if (ImGui::Selectable(kLangNames[i], sel)) ctx.hubSelectedLang = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SetCursorPosX(padding);
        ImGui::TextDisabled("%s", kLangDesc[ctx.hubSelectedLang]);
        if (!cppToolchainOk)
        {
            ImGui::SetCursorPosX(padding);
            ImGui::TextDisabled("C++ needs cmake + a C++ compiler (not found on this machine).");
        }
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
        ImGui::TextWrapped("Applies to the whole project and can't be changed after it's created.");
        ImGui::PopStyleColor();
    }

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
        EditorWidgets::WrapText wrap(panelW - padding);
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", ctx.hubCreateError.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::SetCursorPosX(padding);
    const float btnW = panelW - padding * 2.0f;
    if (EditorWidgets::primaryButton("Create##create", ImVec2(btnW, 36.0f)))
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
                static_cast<ProjectPreset>(ctx.hubSelectedPreset),
                static_cast<ProjectScriptLanguage>(ctx.hubSelectedLang));
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, HE::Ed::Theme::warm(0.115f));
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
        EditorWidgets::pinDialogToEditorWindow();
        if (ImGui::BeginPopupModal("##ConfirmRemove", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            // The dialog auto-sizes to its widest line and one of its lines is a
            // full project path, so without a wrap it is as wide as the path is
            // long — a dialog stretched across the whole editor to ask a one-line
            // yes/no question. An absolute wrap column rather than 0.0f: on an
            // auto-resizing window "the content region's right edge" is derived
            // from the size the content asked for last frame, which is the thing
            // being decided here. The scope closes before EndPopup — the wrap
            // position belongs to this popup's window, and popping it after the
            // window is gone would take a wrap off whatever window is underneath.
            {
                EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 28.0f);

                if (ctx.hubRemoveIndex >= 0 && ctx.hubRemoveIndex < static_cast<int>(known.size()))
                {
                    std::filesystem::path rp(known[ctx.hubRemoveIndex]);
                    ImGui::Text("Remove this project from the list?");
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", rp.stem().string().c_str());
                    ImGui::TextDisabled("%s", known[ctx.hubRemoveIndex].c_str());
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Button("Remove", ImVec2(120, 0)))
                    {
                        ctx.globalState->removeKnownProject(known[ctx.hubRemoveIndex]);
                        ctx.globalState->writeConfig();
                        ctx.hubRemoveIndex = -1;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0)))
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, HE::Ed::Theme::warm(0.135f));
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

    {
        // Same wrap column as the Create panel: these two lines are indented by
        // `padding` on the left, and a bare TextWrapped would run them right up
        // against the panel's border on the other side.
        EditorWidgets::WrapText wrap(panelW - padding);

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
    }

    ImGui::SetCursorPosX(padding);
    if (ImGui::Button("Browse .heproj...", ImVec2(panelW - padding * 2.0f, 36.0f))
        || s_hubOpenBrowseRequested)
    {
        s_hubOpenBrowseRequested = false;
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

} // namespace ProjectHubPanel
