#include "ProjectHubPanel.h"
#include "NotificationStore.h"   // the reason a project did not open is a notification
#include "EditorApplication.h"           // AppContext, ProjectManager, EditorConfig
#include "EditorWidgets.h"
#include "ProjectPreflight.h"           // engine-content check in front of every open
#include "ProjectThumbnail.h"           // the picture on a recent project's card
#include "EditorHelp.h"                  // "Project Hub/<label>" for its controls
#include "EditorTheme.h"                 // brand palette — the hub is the first
                                         // surface after the splash, so it is the
                                         // one that must not look like a different
                                         // product than the window that preceded it
#include "TutorialPanel.h"               // Help ▸ Interactive Tutorial → sandbox offer
#include "DocsPanel.h"                   // Help ▸ Documentation — readable before a project exists
#include "GitCloneDialog.h"              // Projects ▸ Clone from GitHub...
#include "ToolSetupDialog.h"             // "C++ needs cmake and a compiler" notice with its Set Up… button
#include "ProjectLaunchOpen.h"           // a .heproj double-clicked in the file manager
#include "HorizonVersion.h"
#ifdef __APPLE__
#include "MacMenuBar.h"   // native system menu bar (replaces the ImGui menu row)
#endif
#include <HorizonScene/HcCodegen.h>      // HorizonCode → C++ codegen (compile-on-export)
#include <Application/AppIcon.h>         // heLoadPngRGBA — the card pictures
#include <Renderer/IRenderer.h>          // CreateImGuiTexture
#include <Types/Enums.h>

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#include <shobjidl.h>
#endif

#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>        // RenderTextEllipsis — a card's path is cut, not wrapped
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#endif

namespace fs = std::filesystem;

namespace ProjectHubPanel
{

namespace
{
    TextureUpload  s_upload;
    TextureRelease s_release;

    // One texture per recent project, re-read only when its file changes (a
    // project closed since the hub last drew it). Never per frame: on Vulkan a
    // texture costs a slot of a fixed descriptor pool that is not given back.
    struct Thumb
    {
        void*               tex = nullptr;
        fs::file_time_type  stamp{};
        bool                probed = false;
    };
    std::map<std::string, Thumb> s_thumbs;
    // Replaced textures wait one frame before they go: the frame that drew them
    // last may still be on the GPU (AssetThumbnailCache defers the same way).
    std::vector<void*> s_retired;

    void destroyTexture(IRenderer* r, void* t)
    {
        if (!t) return;
        if (s_release) s_release(t);
        else if (r) r->DestroyImGuiTexture(t);
    }

    void* thumbnailFor(AppContext& ctx, const std::string& heproj)
    {
        Thumb& t = s_thumbs[heproj];
        std::error_code ec;
        const fs::path file = ProjectThumbnail::fileFor(heproj);
        const fs::file_time_type stamp = fs::last_write_time(file, ec);
        if (ec)
        {
            if (t.tex) s_retired.push_back(t.tex);
            t = {};
            return nullptr;
        }
        if (t.probed && stamp == t.stamp) return t.tex;
        if (t.tex) s_retired.push_back(t.tex);
        t.tex = nullptr; t.probed = true; t.stamp = stamp;
        std::vector<std::uint8_t> px;
        int w = 0, h = 0;
        if (HE::heLoadPngRGBA(file, px, w, h) && w > 0 && h > 0)
            t.tex = s_upload ? s_upload(px.data(), w, h)
                  : ctx.renderer ? ctx.renderer->CreateImGuiTexture(px.data(), w, h) : nullptr;
        return t.tex;
    }

    // "/Users/me/HorizonEngineProjects/X/X.heproj" → "~/HorizonEngineProjects/X":
    // the card says where the project is, the .heproj name is the card's title.
    std::string displayFolder(const std::string& heproj)
    {
        std::string dir = fs::path(heproj).parent_path().string();
#ifndef _WIN32
        if (const char* home = std::getenv("HOME"); home && *home)
        {
            const std::string h = home;
            if (dir.size() >= h.size() && dir.compare(0, h.size(), h) == 0 &&
                (dir.size() == h.size() || dir[h.size()] == '/'))
                dir = "~" + dir.substr(h.size());
        }
#endif
        return dir;
    }
}

void setTextureHooks(TextureUpload upload, TextureRelease release)
{
    s_upload  = std::move(upload);
    s_release = std::move(release);
}

void releaseTextures(IRenderer* renderer)
{
    for (void* t : s_retired) destroyTexture(renderer, t);
    s_retired.clear();
    for (auto& [path, t] : s_thumbs) destroyTexture(renderer, t.tex);
    s_thumbs.clear();
}

#ifdef HE_IMGUI_ENABLED
namespace
{
    using namespace HE::Ed;
    inline ImU32 col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

    // ── Template pictures ────────────────────────────────────────────────────
    // Drawn rather than shipped as images: the App templates ARE layouts, so a
    // small wireframe of the layout is the most honest picture of them, and
    // drawing keeps them sharp at any size and in the brand palette, with no
    // files for the packager to forget.
    struct Art
    {
        ImDrawList* dl;
        ImVec2 a, b;
        ImVec2 p(float x, float y) const { return ImVec2(a.x + (b.x - a.x) * x, a.y + (b.y - a.y) * y); }
        float  w() const { return b.x - a.x; }
        void rect(float x0, float y0, float x1, float y1, const ImVec4& c, float r = 0.0f) const
        { dl->AddRectFilled(p(x0, y0), p(x1, y1), col(c), r); }
    };

    void drawSky(const Art& g)
    {
        const ImU32 top = col(Theme::mix(Theme::warm(0.16f), Theme::Amber, 0.10f));
        const ImU32 bot = col(Theme::mix(Theme::warm(0.20f), Theme::Amber, 0.55f));
        g.dl->AddRectFilledMultiColor(g.p(0, 0), g.p(1, 0.72f), top, top, bot, bot);
        g.rect(0, 0.72f, 1, 1, Theme::warm(0.10f));
    }

    // An application window: frame, title bar, three dots. Returns nothing —
    // the content area is 0.12..0.88 × 0.27..0.86 in every App picture.
    void drawWindow(const Art& g)
    {
        g.rect(0, 0, 1, 1, Theme::warm(0.12f));
        g.rect(0.08f, 0.10f, 0.92f, 0.90f, Theme::warm(0.24f), 3.0f);
        g.rect(0.08f, 0.10f, 0.92f, 0.22f, Theme::warm(0.31f), 3.0f);
        const float r = std::max(1.5f, g.w() * 0.012f);
        for (int i = 0; i < 3; ++i)
            g.dl->AddCircleFilled(g.p(0.13f + i * 0.035f, 0.16f), r,
                                  col(i == 0 ? Theme::Amber : Theme::warm(0.45f)));
    }

    void drawTemplateArt(ImDrawList* dl, ImVec2 a, ImVec2 b, int preset)
    {
        const Art g{ dl, a, b };
        const ImVec4 line  = Theme::warm(0.40f);
        const ImVec4 block = Theme::warm(0.33f);
        const ImVec4 amber = Theme::Amber;
        switch (static_cast<ProjectPreset>(preset))
        {
        case ProjectPreset::Empty:
        {
            g.rect(0, 0, 1, 1, Theme::warm(0.13f));
            dl->AddRect(g.p(0.30f, 0.22f), g.p(0.70f, 0.78f), col(line), 4.0f, 0, 1.5f);
            const float t = std::max(1.5f, g.w() * 0.015f);
            dl->AddLine(g.p(0.43f, 0.50f), g.p(0.57f, 0.50f), col(amber), t);
            dl->AddLine(g.p(0.50f, 0.39f), g.p(0.50f, 0.61f), col(amber), t);
            break;
        }
        case ProjectPreset::Game:
            drawSky(g);
            dl->AddCircleFilled(g.p(0.70f, 0.40f), g.w() * 0.09f, col(Theme::Gold));
            dl->AddTriangleFilled(g.p(0.02f, 0.72f), g.p(0.30f, 0.30f), g.p(0.58f, 0.72f), col(Theme::warm(0.15f)));
            dl->AddTriangleFilled(g.p(0.38f, 0.72f), g.p(0.62f, 0.44f), g.p(0.90f, 0.72f), col(Theme::warm(0.19f)));
            break;
        case ProjectPreset::Simulation:
        {
            g.rect(0, 0, 1, 1, Theme::warm(0.13f));
            for (int i = 1; i < 4; ++i)
            {
                dl->AddLine(g.p(0.08f, i * 0.25f), g.p(0.92f, i * 0.25f), col(Theme::warm(0.20f)));
                dl->AddLine(g.p(i * 0.25f, 0.10f), g.p(i * 0.25f, 0.90f), col(Theme::warm(0.20f)));
            }
            ImVec2 pts[24];
            for (int i = 0; i < 24; ++i)
            {
                const float x = i / 23.0f;
                pts[i] = g.p(0.08f + x * 0.84f, 0.52f - 0.28f * std::sin(x * 6.0f) * (1.0f - 0.5f * x));
            }
            dl->AddPolyline(pts, 24, col(amber), 0, std::max(1.5f, g.w() * 0.014f));
            dl->AddCircleFilled(pts[17], g.w() * 0.025f, col(Theme::Gold));
            break;
        }
        case ProjectPreset::Tool:
        {
            g.rect(0, 0, 1, 1, Theme::warm(0.13f));
            const ImVec2 c = g.p(0.5f, 0.5f);
            const float  r = (b.y - a.y) * 0.24f;
            for (int i = 0; i < 8; ++i)
            {
                const float an = i * 3.14159265f / 4.0f;
                const ImVec2 d(std::cos(an), std::sin(an)), n(-d.y, d.x);
                const float tw = r * 0.28f, to = r * 1.35f;
                dl->AddQuadFilled(ImVec2(c.x + d.x * r * 0.8f - n.x * tw, c.y + d.y * r * 0.8f - n.y * tw),
                                  ImVec2(c.x + d.x * to - n.x * tw,       c.y + d.y * to - n.y * tw),
                                  ImVec2(c.x + d.x * to + n.x * tw,       c.y + d.y * to + n.y * tw),
                                  ImVec2(c.x + d.x * r * 0.8f + n.x * tw, c.y + d.y * r * 0.8f + n.y * tw),
                                  col(amber));
            }
            dl->AddCircleFilled(c, r, col(amber));
            dl->AddCircleFilled(c, r * 0.45f, col(Theme::warm(0.13f)));
            break;
        }
        case ProjectPreset::Tutorial:
        {
            drawSky(g);
            dl->AddCircleFilled(g.p(0.18f, 0.25f), g.w() * 0.05f, col(Theme::Gold));
            // A cube on the ground: three faces, lit from the sun's side.
            const ImVec2 t0 = g.p(0.50f, 0.30f), t1 = g.p(0.66f, 0.38f), t2 = g.p(0.50f, 0.46f), t3 = g.p(0.34f, 0.38f);
            const ImVec2 b1 = g.p(0.66f, 0.66f), b2 = g.p(0.50f, 0.74f), b3 = g.p(0.34f, 0.66f);
            dl->AddQuadFilled(t0, t1, t2, t3, col(Theme::Ivory));
            dl->AddQuadFilled(t3, t2, b2, b3, col(Theme::mix(Theme::Ivory, Theme::Amber, 0.45f)));
            dl->AddQuadFilled(t2, t1, b1, b2, col(Theme::mix(Theme::warm(0.30f), Theme::Amber, 0.35f)));
            break;
        }
        case ProjectPreset::ThirdPerson:
        {
            drawSky(g);
            const float h = b.y - a.y;
            dl->AddEllipseFilled(g.p(0.5f, 0.80f), ImVec2(g.w() * 0.12f, h * 0.04f), col(Theme::warm(0.06f)));
            dl->AddRectFilled(g.p(0.44f, 0.42f), g.p(0.56f, 0.80f), col(Theme::Ivory), g.w() * 0.05f);
            dl->AddCircleFilled(g.p(0.5f, 0.32f), h * 0.085f, col(Theme::Ivory));
            // The follow camera's view: a bracket behind the character.
            dl->AddLine(g.p(0.18f, 0.30f), g.p(0.34f, 0.46f), col(amber), 2.0f);
            dl->AddLine(g.p(0.18f, 0.62f), g.p(0.34f, 0.46f), col(amber), 2.0f);
            break;
        }
        case ProjectPreset::Application:
            drawWindow(g);
            g.rect(0.14f, 0.32f, 0.62f, 0.38f, line, 2.0f);
            g.rect(0.14f, 0.44f, 0.80f, 0.49f, block, 2.0f);
            g.rect(0.14f, 0.54f, 0.72f, 0.59f, block, 2.0f);
            g.rect(0.62f, 0.72f, 0.86f, 0.82f, amber, 3.0f);
            break;
        case ProjectPreset::AppSidebar:
            drawWindow(g);
            g.rect(0.08f, 0.22f, 0.34f, 0.90f, Theme::warm(0.19f));
            for (int i = 0; i < 4; ++i)
                g.rect(0.12f, 0.29f + i * 0.12f, 0.30f, 0.35f + i * 0.12f, i == 1 ? amber : block, 2.0f);
            g.rect(0.34f, 0.22f, 0.35f, 0.90f, Theme::warm(0.45f));
            g.rect(0.41f, 0.30f, 0.86f, 0.52f, block, 2.0f);
            g.rect(0.41f, 0.58f, 0.86f, 0.82f, block, 2.0f);
            break;
        case ProjectPreset::AppWizard:
            drawWindow(g);
            g.rect(0.14f, 0.29f, 0.86f, 0.33f, Theme::warm(0.17f), 2.0f);
            g.rect(0.14f, 0.29f, 0.52f, 0.33f, amber, 2.0f);
            g.rect(0.22f, 0.42f, 0.78f, 0.48f, line, 2.0f);
            g.rect(0.22f, 0.53f, 0.70f, 0.58f, block, 2.0f);
            g.rect(0.14f, 0.73f, 0.34f, 0.82f, block, 3.0f);
            g.rect(0.66f, 0.73f, 0.86f, 0.82f, amber, 3.0f);
            break;
        case ProjectPreset::AppDashboard:
            drawWindow(g);
            for (int r = 0; r < 2; ++r)
                for (int c = 0; c < 3; ++c)
                {
                    const float x0 = 0.13f + c * 0.255f, y0 = 0.28f + r * 0.30f;
                    g.rect(x0, y0, x0 + 0.225f, y0 + 0.26f, Theme::warm(0.30f), 3.0f);
                    if (r == 0) g.rect(x0 + 0.03f, y0 + 0.05f, x0 + 0.12f, y0 + 0.12f, c == 0 ? amber : line, 1.5f);
                    else for (int k = 0; k < 4; ++k)
                    {
                        const float hgt = 0.05f + 0.035f * ((k * 7 + c * 3) % 5);
                        g.rect(x0 + 0.03f + k * 0.045f, y0 + 0.22f - hgt, x0 + 0.06f + k * 0.045f, y0 + 0.22f,
                               k == 2 ? amber : line);
                    }
                }
            break;
        case ProjectPreset::AppForm:
            drawWindow(g);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 2; ++c)
                {
                    const float x0 = 0.14f + c * 0.37f, y0 = 0.28f + r * 0.14f;
                    g.rect(x0, y0, x0 + 0.14f, y0 + 0.03f, line, 1.0f);
                    g.rect(x0, y0 + 0.05f, x0 + 0.33f, y0 + 0.10f, Theme::warm(0.17f), 2.0f);
                }
            g.rect(0.08f, 0.72f, 0.92f, 0.90f, Theme::warm(0.20f));
            g.rect(0.50f, 0.75f, 0.66f, 0.86f, block, 3.0f);
            g.rect(0.70f, 0.75f, 0.86f, 0.86f, amber, 3.0f);
            break;
        case ProjectPreset::AppTool:
            drawWindow(g);
            for (int i = 0; i < 5; ++i)
                g.rect(0.12f + i * 0.07f, 0.26f, 0.17f + i * 0.07f, 0.32f, i == 0 ? amber : block, 1.5f);
            g.rect(0.12f, 0.37f, 0.34f, 0.78f, Theme::warm(0.19f), 2.0f);
            for (int i = 0; i < 4; ++i)
                g.rect(0.15f, 0.41f + i * 0.09f, 0.31f, 0.45f + i * 0.09f, i == 0 ? line : block, 1.0f);
            g.rect(0.38f, 0.37f, 0.88f, 0.78f, Theme::warm(0.17f), 2.0f);
            g.rect(0.08f, 0.82f, 0.92f, 0.90f, Theme::warm(0.30f));
            break;
        default:
            g.rect(0, 0, 1, 1, Theme::warm(0.13f));
            break;
        }
    }

    // A project without a picture yet (never closed since this version, or an
    // application, whose viewport is empty): the brand's horizon instead.
    void drawPlaceholder(ImDrawList* dl, ImVec2 a, ImVec2 b)
    {
        const ImU32 top = col(Theme::warm(0.17f)), bot = col(Theme::warm(0.11f));
        dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
        const float hy = a.y + (b.y - a.y) * 0.64f, cx = (a.x + b.x) * 0.5f;
        const float r  = (b.y - a.y) * 0.22f;
        dl->PushClipRect(a, ImVec2(b.x, hy), true);
        dl->AddCircleFilled(ImVec2(cx, hy), r, col(Theme::alpha(Theme::Amber, 0.55f)), 48);
        dl->PopClipRect();
        dl->AddLine(ImVec2(a.x + (b.x - a.x) * 0.12f, hy), ImVec2(b.x - (b.x - a.x) * 0.12f, hy),
                    col(Theme::alpha(Theme::Gold, 0.6f)), 1.5f);
    }

    // Picture with its rounded corners, cropped (not stretched) to the slot.
    void drawPicture(ImDrawList* dl, void* tex, ImVec2 a, ImVec2 b)
    {
        const float slotAspect = (b.x - a.x) / std::max(1.0f, b.y - a.y);
        const float picAspect  = float(ProjectThumbnail::kWidth) / float(ProjectThumbnail::kHeight);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (slotAspect > picAspect) { const float v = picAspect / slotAspect; uv0.y = (1 - v) * 0.5f; uv1.y = 1 - uv0.y; }
        else                        { const float u = slotAspect / picAspect; uv0.x = (1 - u) * 0.5f; uv1.x = 1 - uv0.x; }
        dl->AddImageRounded(ImTextureRef(reinterpret_cast<ImTextureID>(tex)), a, b, uv0, uv1,
                            IM_COL32_WHITE, 4.0f, ImDrawFlags_RoundCornersTop);
    }

    // A card's frame: fill, then the hover/selection outline the brand accent says.
    void drawCardFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered, bool selected)
    {
        dl->AddRectFilled(a, b, col(hovered ? Theme::warm(0.22f) : Theme::warm(0.175f)), 6.0f);
        if (selected)      dl->AddRect(a, b, col(Theme::Amber), 6.0f, 0, 2.0f);
        else if (hovered)  dl->AddRect(a, b, col(Theme::alpha(Theme::AccentHi, 0.6f)), 6.0f, 0, 1.0f);
    }

    void sectionTitle(AppContext& ctx, const char* text, float x)
    {
        ImGui::SetCursorPosX(x);
        if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
        ImGui::TextUnformatted(text);
        if (ctx.fontSubheading) ImGui::PopFont();
    }

    void groupLabel(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
}

bool drawTemplateGrid(int& selectedPreset, float width)
{
    constexpr float kGap = 10.0f, kMinTile = 104.0f;
    const int   cols  = std::clamp(int((width + kGap) / (kMinTile + kGap)), 1, 6);
    const float tileW = std::floor((width - kGap * (cols - 1)) / cols);
    const float artH  = std::floor(tileW * 0.60f);
    const float labelH = ImGui::GetTextLineHeight() + 12.0f;
    const float x0 = ImGui::GetCursorPosX();
    bool clicked = false;

    for (int group = 0; group < 2; ++group)
    {
        ImGui::SetCursorPosX(x0);
        groupLabel(group == 0 ? "Games & Scenes" : "Applications");
        int column = 0;
        for (int i = 0; i < kPresetCount; ++i)
        {
            if (isAppPreset(static_cast<ProjectPreset>(i)) != (group == 1)) continue;
            if (column == cols) column = 0;
            if (column > 0) ImGui::SameLine(0.0f, kGap);
            else         ImGui::SetCursorPosX(x0);
            ++column;

            ImGui::PushID(i);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##tpl", ImVec2(tileW, artH + labelH)))
            {
                selectedPreset = i;
                clicked = true;
            }
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) ImGui::SetTooltip("%s", kPresetDescs[i]);
            ImGui::PopID();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 b(a.x + tileW, a.y + artH + labelH);
            const bool sel = (selectedPreset == i);
            drawCardFrame(dl, a, b, hovered, sel);
            const ImVec2 artA(a.x + 4, a.y + 4), artB(b.x - 4, a.y + artH);
            dl->PushClipRect(artA, artB, true);
            drawTemplateArt(dl, artA, artB, i);
            dl->PopClipRect();
            const ImVec2 tp(a.x + 8, a.y + artH + 6);
            // A name a little too long for the tile is set a little smaller
            // rather than cut ("Tutorial Sandb…"); only a much longer one still
            // ends in an ellipsis.
            const float room = b.x - 6 - tp.x;
            const float full = ImGui::CalcTextSize(kPresetNames[i]).x;
            if (full > room && full * 0.78f <= room)
            {
                const float fs = ImGui::GetFontSize() * (room / full);
                dl->AddText(ImGui::GetFont(), fs, ImVec2(tp.x, tp.y + (ImGui::GetFontSize() - fs) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), kPresetNames[i]);
            }
            else
                ImGui::RenderTextEllipsis(dl, tp, ImVec2(b.x - 6, b.y), b.x - 6, kPresetNames[i], nullptr, nullptr);
            // The amber frame (drawCardFrame) already says "selected"; no second mark.
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }
    return clicked;
}
#else
bool drawTemplateGrid(int&, float) { return false; }
#endif

// ─── Project Hub ──────────────────────────────────────────────────────────────
void render(AppContext& ctx)
{
    // The first screen anyone sees: "Project Hub/<label>".
    HE::Ed::Help::Scope helpScope("Project Hub");
    for (void* t : s_retired) destroyTexture(ctx.renderer, t);
    s_retired.clear();
#ifdef HE_IMGUI_ENABLED
    using namespace HE::Ed;
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // Set by the native File ▸ Open Project menu item below; consumed by the
    // "Browse .heproj…" button handler in the Projects column, so the menu
    // triggers the same file dialog. Unconditional so the button condition
    // compiles on all platforms (only ever set on macOS).
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
            case MC::NewProject:  // fresh New Project form
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::warm(0.115f));
    ImGui::Begin("##ProjectHub", nullptr, kHubFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    // ── Header bar ────────────────────────────────────────────────────────────
    const float headerH = 56.0f;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::warm(0.095f));
    ImGui::BeginChild("##HubHeader", ImVec2(vp->Size.x, headerH), false,
        ImGuiWindowFlags_NoScrollbar);

    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    const char* title = "Horizon Engine " HE_VERSION_STRING " \"" HE_VERSION_CODENAME "\"  —  Project Hub";

    const float logoDisplayH = headerH - 16.0f;
    if (ctx.logoTexture && ctx.logoW > 0 && ctx.logoH > 0)
    {
        const float logoDisplayW = logoDisplayH * (static_cast<float>(ctx.logoW) / static_cast<float>(ctx.logoH));
        ImGui::SetCursorPos(ImVec2(16.0f, (headerH - logoDisplayH) * 0.5f));
        ImGui::Image(ctx.logoTexture, ImVec2(logoDisplayW, logoDisplayH));
        ImGui::SameLine();
        ImGui::SetCursorPosY((headerH - ImGui::GetTextLineHeight()) * 0.5f);
    }
    else
    {
        ImGui::SetCursorPos(ImVec2(24.0f, (headerH - ImGui::GetTextLineHeight()) * 0.5f));
    }
    ImGui::Text("%s", title);
    if (ctx.fontHeading) ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    {
        // The brand's horizon under the header: amber fading out to the right.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = vp->Pos;
        const ImU32 a = col(Theme::Amber), z = col(Theme::alpha(Theme::Amber, 0.0f));
        dl->AddRectFilledMultiColor(ImVec2(o.x, o.y + headerH), ImVec2(o.x + vp->Size.x * 0.6f, o.y + headerH + 2.0f), a, z, z, a);
    }

    // ── Two columns: Projects | New Project ───────────────────────────────────
    const float bodyY   = headerH + 2.0f;
    const float bodyH   = vp->Size.y - bodyY;
    const float leftW   = std::floor(vp->Size.x * 0.42f);
    const float rightW  = vp->Size.x - leftW;
    const float padding = 24.0f;

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
    // LEFT — Projects: the recent ones as picture cards, and the two ways to
    // open one the list does not know (from disk, from GitHub). They used to be
    // two columns, "Recent Projects" and "Open Project"; they answer the same
    // question — "which project do I work on?" — so they are one place now.
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(0.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::warm(0.105f));
    ImGui::BeginChild("##PanelProjects", ImVec2(leftW, bodyH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(padding, padding));
    sectionTitle(ctx, "Projects", padding);
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    if (!ctx.hubOpenError.empty())
    {
        // Whatever went wrong opening a project is a dialog, said once (the slot is
        // emptied): the hub shows the form, not a red sentence that outlives it.
        HE::Ed::NoteSpec note;
        note.level = HE::Ed::NoteLevel::Critical;
        note.title = "Could not open the project";
        note.text  = ctx.hubOpenError;
        HE::Ed::notify(std::move(note));
        ctx.hubOpenError.clear();
    }

    const float innerW = leftW - padding * 2.0f;
    const float halfW  = std::floor((innerW - 8.0f) * 0.5f);
    ImGui::SetCursorPosX(padding);
    if (EditorWidgets::button("Browse .heproj...", ImVec2(halfW, 34.0f))
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
    // The counterpart of Preferences ▸ Source Control's "Create & push": a
    // repository that already exists on the user's account, made into a project
    // here. The dialog is drawn by EditorUI (over both screens); the project it
    // clones comes back through the same load as a browsed .heproj, below.
    ImGui::SameLine(0.0f, 8.0f);
    if (EditorWidgets::button("Clone from GitHub...", ImVec2(innerW - halfW - 8.0f, 34.0f)))
        GitCloneDialog::requestOpen();

    std::string cloned;
    const bool clonedReady = GitCloneDialog::takeOpenRequest(cloned);
    // A .heproj double-clicked in the file manager (ProjectLaunchOpen.h) takes
    // the same load as the dialog's choice. One per frame: a clone that landed
    // at the same moment goes first and this one waits in its slot.
    std::string launched;
    const bool launchedReady = !clonedReady && ProjectLaunchOpen::take(launched);
    if (ctx.pendingFileReady || clonedReady || launchedReady)
    {
        std::string chosen = clonedReady ? cloned : launchedReady ? launched : ctx.pendingFileResult;
        if (launchedReady)
            HE_LOG_INFO(Editor, "Project Hub: opening %s (handed over by the system)", chosen.c_str());
        if (ctx.pendingFileReady && !clonedReady && !launchedReady)
        {
            ctx.pendingFileReady = false;
            ctx.pendingFileResult.clear();
        }
        ctx.hubOpenError.clear();
        ProjectPreflight::request(ctx, chosen);
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(padding);
    groupLabel("Recent");

    const auto& known = ctx.globalState->getKnownProjects();
    if (known.empty())
    {
        EditorWidgets::WrapText wrap(leftW - padding);
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
        ImGui::TextWrapped("No projects yet. Create one on the right, or open an existing "
                           "project with the buttons above.");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::SetCursorPosX(padding);
        const float listH = std::max(80.0f, bodyH - ImGui::GetCursorPosY() - padding * 0.5f);
        // The list is at most GlobalState's ten, so every card is drawn; a card
        // keeps one height so the grid stays a grid.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));   // the column's own fill
        ImGui::BeginChild("##KnownList", ImVec2(leftW - padding, listH), false);
        ImGui::PopStyleColor();

        constexpr float kGap = 12.0f, kMinCard = 170.0f;
        const float avail  = innerW;
        const int   cols   = std::max(1, int((avail + kGap) / (kMinCard + kGap)));
        const float cardW  = std::floor((avail - kGap * (cols - 1)) / cols);
        const float picH   = std::floor(cardW * 9.0f / 16.0f);
        const float lineH  = ImGui::GetTextLineHeight();
        const float cardH  = picH + lineH * 2.0f + 18.0f;

        for (int i = 0; i < static_cast<int>(known.size()); ++i)
        {
            const fs::path p(known[i]);
            std::string name = p.stem().string();
            if (name.empty()) name = known[i];
            std::error_code ec;
            const bool exists = fs::exists(p, ec);

            if (i % cols != 0) ImGui::SameLine(0.0f, kGap);
            ImGui::PushID(i);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) ImGui::SetTooltip("%s%s", known[i].c_str(),
                                           exists ? "" : "\nNot found: moved, renamed or deleted.");

            // Through the preflight: the project's engine-content references are fetched
            // (and a missing one asked about) BEFORE it loads — see ProjectPreflight.h.
            if (ImGui::IsItemClicked() && exists && !ProjectPreflight::busy())
                ProjectPreflight::request(ctx, known[i]);

            if (ImGui::BeginPopupContextItem("##KnownCtx"))
            {
                if (EditorWidgets::menuItem("Remove from list"))
                {
                    ctx.hubRemoveIndex     = i;
                    ctx.hubRemoveRequested = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 b(a.x + cardW, a.y + cardH);
            drawCardFrame(dl, a, b, hovered && exists, false);
            const ImVec2 picA(a.x, a.y), picB(b.x, a.y + picH);
            if (void* tex = exists ? thumbnailFor(ctx, known[i]) : nullptr)
                drawPicture(dl, tex, picA, picB);
            else
            {
                dl->PushClipRect(picA, picB, true);
                drawPlaceholder(dl, picA, picB);
                dl->PopClipRect();
            }
            if (!exists)
            {
                dl->AddRectFilled(picA, picB, col(Theme::alpha(Theme::warm(0.05f), 0.65f)), 6.0f,
                                  ImDrawFlags_RoundCornersTop);
                const char* miss = "Not found";
                const ImVec2 ts = ImGui::CalcTextSize(miss);
                dl->AddText(ImVec2((picA.x + picB.x - ts.x) * 0.5f, (picA.y + picB.y - ts.y) * 0.5f),
                            col(Theme::Bad), miss);
            }
            const float tx = a.x + 10.0f, tr = b.x - 10.0f;
            float ty = a.y + picH + 7.0f;
            ImGui::PushStyleColor(ImGuiCol_Text, exists ? Theme::Text : Theme::Bad);
            ImGui::RenderTextEllipsis(dl, ImVec2(tx, ty), ImVec2(tr, ty + lineH), tr, name.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            ty += lineH + 2.0f;
            // Too long for the card: cut from the LEFT at a separator, because the
            // end of the path is the project's own folder, the part worth seeing
            // ("…/Projects/MyGame"). The right-hand ellipsis below stays the last
            // resort for a single folder name wider than the card.
            std::string folder = displayFolder(known[i]);
            if (ImGui::CalcTextSize(folder.c_str()).x > tr - tx)
                for (size_t s = folder.find_first_of("/\\", 1); s != std::string::npos;
                     s = folder.find_first_of("/\\", s + 1))
                {
                    const std::string tail = "\xe2\x80\xa6" + folder.substr(s);
                    if (ImGui::CalcTextSize(tail.c_str()).x <= tr - tx ||
                        folder.find_first_of("/\\", s + 1) == std::string::npos)
                    {
                        folder = tail;
                        break;
                    }
                }
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
            ImGui::RenderTextEllipsis(dl, ImVec2(tx, ty), ImVec2(tr, ty + lineH), tr, folder.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            if (i % cols == cols - 1 || i + 1 == static_cast<int>(known.size()))
                ImGui::Dummy(ImVec2(0.0f, kGap - ImGui::GetStyle().ItemSpacing.y));
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
                    fs::path rp(known[ctx.hubRemoveIndex]);
                    ImGui::Text("Remove this project from the list?");
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", rp.stem().string().c_str());
                    ImGui::TextDisabled("%s", known[ctx.hubRemoveIndex].c_str());
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (EditorWidgets::removeButton("Remove", ImVec2(120, 0)))
                    {
                        if (auto it = s_thumbs.find(known[ctx.hubRemoveIndex]); it != s_thumbs.end())
                        {
                            if (it->second.tex) s_retired.push_back(it->second.tex);
                            s_thumbs.erase(it);
                        }
                        ctx.globalState->removeKnownProject(known[ctx.hubRemoveIndex]);
                        ctx.globalState->writeConfig();
                        ctx.hubRemoveIndex = -1;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (EditorWidgets::button("Cancel", ImVec2(120, 0)))
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
    // RIGHT — New Project: template tiles, then the form for the chosen one.
    // Scrolls when the window is short, so Create is always reachable.
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(leftW, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::warm(0.13f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##PanelCreate", ImVec2(rightW, bodyH), false);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const float formW = rightW - padding * 2.0f - ImGui::GetStyle().ScrollbarSize;
    ImGui::SetCursorPos(ImVec2(padding, padding));
    sectionTitle(ctx, "New Project", padding);
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    // Every template visible at once, as a picture: a template you have to
    // scroll to, or read a list to imagine, is a template nobody picks.
    ImGui::SetCursorPosX(padding);
    drawTemplateGrid(ctx.hubSelectedPreset, formW);

    {
        // Every line in this block is a full sentence describing a choice; the
        // wrap sits at the column's right margin so the text keeps the same
        // margin on the right that every control here has on the left.
        EditorWidgets::WrapText wrap(padding + formW);

        ImGui::Spacing();
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextHeading);
        ImGui::TextUnformatted(kPresetNames[ctx.hubSelectedPreset]);
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
        ImGui::TextWrapped("%s", kPresetDescs[ctx.hubSelectedPreset]);
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::SetCursorPosX(padding);
        ImGui::Text("Scripting Language");

        // A C++ project is useless without cmake + a working compiler, so drop it from the
        // picker when the startup probe couldn't find them. A null probe = it hasn't
        // finished yet → don't hide prematurely. Index 3 == Cpp (matches kLangNames /
        // ProjectScriptLanguage). See ToolSetupDialog / HcCodegen::probeToolchain.
        const bool cppToolchainOk =
            !ctx.toolchainProbe ||
            (ctx.toolchainProbe->cmakeFound && ctx.toolchainProbe->compilerFound);
        if (!cppToolchainOk && ctx.hubSelectedLang == 3)
            ctx.hubSelectedLang = 0; // C++ no longer offered — fall back to HorizonCode

        ImGui::SetCursorPosX(padding);
        ImGui::PushItemWidth(formW);
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
            const ToolSetupDialog::Tool missing =
                (ctx.toolchainProbe && !ctx.toolchainProbe->cmakeFound)
                    ? ToolSetupDialog::Tool::Cmake : ToolSetupDialog::Tool::Compiler;
            ToolSetupDialog::drawMissingBanner(ctx, missing,
                "C++ needs cmake and a C++ compiler, and neither is set up on this machine.");
        }

        // Advanced Shader Effects — applications only, and forced on otherwise.
        // See the New Project popup's twin in EditorUI.cpp for why it is not
        // offered for a game.
        if (isAppPreset(static_cast<ProjectPreset>(ctx.hubSelectedPreset)))
        {
            ImGui::Spacing();
            ImGui::SetCursorPosX(padding);
            ImGui::Checkbox("Advanced Shader Effects", &ctx.hubAdvancedShaderFx);
            ImGui::SetCursorPosX(padding);
            ImGui::TextDisabled("%s", ctx.hubAdvancedShaderFx
                ? "Every material node is available."
                : "Widget materials only; ships the software renderer, no GPU needed.");
        }
        else
            ctx.hubAdvancedShaderFx = true;
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::alpha(Theme::TextHeading, 0.8f));
        ImGui::TextWrapped("Template and language apply to the whole project and can't be "
                           "changed after it's created.");
        ImGui::PopStyleColor();
    }

    // Name and folder side by side: the two things every new project needs.
    ImGui::Spacing();
    ImGui::Spacing();
    const float nameW = std::floor(formW * 0.38f);
    const float dirX  = padding + nameW + 12.0f;
    const float dirW  = formW - nameW - 12.0f;
    const float rowY  = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(padding, rowY));
    ImGui::Text("Project Name");
    ImGui::SetCursorPos(ImVec2(dirX, rowY));
    ImGui::Text("Project Directory");
    const float fieldY = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(padding, fieldY));
    ImGui::PushItemWidth(nameW);
    ImGui::InputText("##ProjName", ctx.hubProjectName, ctx.hubProjectNameSize);
    ImGui::PopItemWidth();
    ImGui::SetCursorPos(ImVec2(dirX, fieldY));
    ImGui::PushItemWidth(dirW - 80.0f);
    ImGui::InputText("##ProjDir", ctx.hubProjectDir, ctx.hubProjectDirSize);
    ImGui::PopItemWidth();
    ImGui::SameLine(0.0f, 8.0f);
    if (EditorWidgets::button("Browse##dir", ImVec2(72.0f, 0.0f)))
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
        EditorWidgets::WrapText wrap(padding + formW);
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
        ImGui::TextWrapped("%s", ctx.hubCreateError.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::Spacing();
    ImGui::SetCursorPosX(padding);
    if (EditorWidgets::primaryButton("Create##create", ImVec2(formW, 40.0f)))
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
            fs::path projRoot = fs::path(dir) / name;
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
                ctx.projectLoaded = true;
            }
            else
            {
                // An outcome, not a mistake in a field: a notification (the two checks
                // above stay inline, they point at the field to fix).
                HE::Ed::notify(HE::Ed::NoteLevel::Important, "Could not create the project",
                               "Check that the folder exists and is writable.");
            }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, padding));

    if (ctx.fontBody) ImGui::PopFont();
    ImGui::EndChild();

    ImGui::End();
#endif // HE_IMGUI_ENABLED
}

} // namespace ProjectHubPanel
