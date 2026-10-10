#include "OutlinerPanel.h"
#include "OutlinerFilter.h"              // the search/type rule behind the header row
#include "EditorApplication.h"           // AppContext, HorizonWorld, EditorUndo
#include "EditorWidgets.h"
#include "EditorShortcuts.h"   // the clipboard rows print the live chords
#include "EditorHelp.h"                  // scopes for the context and create menus
#include "EditorTheme.h"                 // the accent the prefab badge is drawn in
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/EntityVisibility.h> // what the eye on a row flips, and reads
#include <HorizonScene/EntityActive.h>     // the Details panel's Active switch dims a row
#include <ContentManager/ContentManager.h> // the prefab badge names the asset
#include <ContentManager/Assets.h>
#include <UIWidget/WidgetManager.h>   // application projects list widgets, not entities
#include <algorithm>                     // find/min/max/reverse for the Shift-click range
#include <functional>
#include <Diagnostics/Logger.h>
#include <Diagnostics/Profiler.h>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>   // std::size
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>   // ImRect + BeginDragDropTargetCustom for the root drop zone
#include <misc/cpp/imgui_stdlib.h> // InputText over std::string for the search box
#endif

namespace OutlinerPanel
{

#ifdef HE_IMGUI_ENABLED
namespace
{
    // ── "Create ▸" ───────────────────────────────────────────────────────────
    // Until now the Outliner offered exactly one thing to create: an entity with
    // a name and a transform. Everything else — a light, a camera, a cube — meant
    // knowing which components add up to it, which is knowledge the menu was
    // hiding rather than teaching. This is where people coming from other engines
    // look first, and it is where the tutorial points them.
    //
    // Each entry is a whole recipe, not a component. "Camera (Third Person)" is
    // the one that matters most: a rig already defaults to third person and to
    // following the possessed player, so the recipe is the entire setup.
    enum class Preset
    {
        Empty, Cube,
        CameraThirdPerson, CameraFirstPerson, CameraPlain,
        LightDirectional, LightPoint, LightSpot,
        Rope, Trail,
    };

    Entity createPreset(HorizonWorld& world, Preset p)
    {
        const char* name = "Entity";
        switch (p)
        {
            case Preset::Cube:              name = "Cube";              break;
            case Preset::CameraThirdPerson:
            case Preset::CameraFirstPerson:
            case Preset::CameraPlain:       name = "Camera";            break;
            case Preset::LightDirectional:  name = "Directional Light"; break;
            case Preset::LightPoint:        name = "Point Light";       break;
            case Preset::LightSpot:         name = "Spot Light";        break;
            case Preset::Rope:              name = "Rope";              break;
            case Preset::Trail:             name = "Trail";             break;
            default:                                                    break;
        }

        const Entity e = world.createEntity(name);
        world.addComponent(e, TransformComponent{});

        switch (p)
        {
            case Preset::Cube:
                world.addComponent(e, MeshComponent{ .meshAssetId = HE::kDefaultCubeMeshId });
                break;
            case Preset::CameraThirdPerson:
            case Preset::CameraFirstPerson:
            case Preset::CameraPlain:
            {
                CameraComponent cam;
                // A camera you just asked for is the one you want to look through.
                cam.isMain = true;
                world.addComponent(e, cam);
                if (p != Preset::CameraPlain)
                {
                    CameraRigComponent rig;
                    rig.mode = (p == Preset::CameraFirstPerson)
                        ? CameraRigComponent::Mode::FirstPerson
                        : CameraRigComponent::Mode::ThirdPerson;
                    // First person only makes sense with the head turning along.
                    if (rig.mode == CameraRigComponent::Mode::FirstPerson)
                        rig.targetYaw = CameraRigComponent::TargetYaw::Follow;
                    world.addComponent(e, rig);
                }
                break;
            }
            case Preset::LightDirectional:
            case Preset::LightPoint:
            case Preset::LightSpot:
            {
                LightComponent l;
                l.type = p == Preset::LightDirectional ? LightType::Directional
                       : p == Preset::LightPoint       ? LightType::Point
                                                       : LightType::Spot;
                world.addComponent(e, l);
                break;
            }
            // A rope arrives with the two control points its component defaults
            // to — a metre of line hanging straight down — so it is visible in
            // the viewport the moment it is made, and the first thing to do with
            // it is drag an end somewhere. A trail arrives emitting and lays a
            // band as soon as the entity is moved, which is the only way to see
            // one at all.
            case Preset::Rope:  world.addComponent(e, RopeComponent{});  break;
            case Preset::Trail: world.addComponent(e, TrailComponent{}); break;
            default: break;
        }
        return e;
    }

    // The menu body, shared by the background menu and the per-entity "create a
    // child" menu — one list, so the two can never drift apart.
    bool drawCreateMenu(Preset& out)
    {
        // Both callers — the background menu and "Create Child" — draw this one
        // list, so the scope belongs to the list rather than to either of them.
        HE::Ed::Help::Scope helpScope("New Entity");
        bool picked = false;
        auto item = [&](const char* label, Preset p)
        { if (EditorWidgets::menuItem(label)) { out = p; picked = true; } };

        item("Empty", Preset::Empty);
        item("Cube",  Preset::Cube);
        ImGui::Separator();
        if (ImGui::BeginMenu("Camera"))
        {
            item("Third Person", Preset::CameraThirdPerson);
            item("First Person", Preset::CameraFirstPerson);
            ImGui::Separator();
            item("Plain (no rig)", Preset::CameraPlain);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Light"))
        {
            item("Directional", Preset::LightDirectional);
            item("Point",       Preset::LightPoint);
            item("Spot",        Preset::LightSpot);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item("Rope",  Preset::Rope);
        item("Trail", Preset::Trail);
        return picked;
    }


    // ── "Save as Prefab" ─────────────────────────────────────────────────────
    // The subtree under `entity` written as Content/Prefabs/<name>.hasset —
    // shared by the row's context menu and the main bar's Entity menu.
    void savePrefabOf(AppContext& ctx, Entity entity, const std::string& name0)
    {
        if (!ctx.world || !ctx.contentManager) return;
        // Entity names are free text, and a '/' in one would reach
        // saveAsset's create_directories: "Arm/Left" would silently
        // land in Content/Prefabs/Arm instead of where the user is
        // looking for it.
        std::string base = name0;
        for (char& c : base)
            if (c == '/' || c == '\\') c = '_';
        if (base.empty()) base = "Prefab";

        // Unique "<Name>.hasset" under Content/Prefabs — the folder
        // ProjectManager seeds for exactly this. Without the counter
        // a second "Save as Prefab" on a same-named entity would
        // overwrite the first one with no warning.
        const std::string dirAbs = ctx.contentManager->contentRoot() + "/Prefabs";
        std::error_code ec;
        std::filesystem::create_directories(dirAbs, ec);
        std::string name = base;
        for (int n = 1; std::filesystem::exists(dirAbs + "/" + name + ".hasset", ec); ++n)
            name = base + std::to_string(n);

        SceneSerializer ser;
        PrefabAsset prefab;
        prefab.type = HE::AssetType::Prefab;
        prefab.name = name;
        prefab.path = "Prefabs/" + name + ".hasset";
        prefab.data = ser.serializeSubtree(*ctx.world, entity);

        // Write the file BEFORE registering it. Registering alone is
        // what this menu item used to do, and an asset that lives only
        // in the SlotMap never reaches the Content Browser and is gone
        // at shutdown — the save looked like it worked and wasn't.
        const std::string relPath = prefab.path;
        // The save hook publishes an UPDATE for anything written
        // through saveAsset, and this file is a create — held across
        // the write so the create below is the only announcement, and
        // so the update's lock claim never lands on a path the host
        // may be about to rename out from under us.
        const CollabController::CreatingAsset creating(ctx.collab, relPath);
        if (ctx.contentManager->saveAsset(prefab))
        {
            // Registering the in-memory copy keeps the UUID that was
            // just written to disk (registerRuntimeAsset only mints one
            // when there is none), so the path→UUID entry it adds and
            // the file agree — a later drop of this prefab resolves it
            // without re-reading it. The refresh flag is what makes the
            // new file appear in the Content Browser.
            const std::string fullPath = dirAbs + "/" + name + ".hasset";
            // The source becomes a placement of what it was just saved as, so
            // it follows the asset from here on like a dropped copy would —
            // before, it stayed an unlinked copy and had to be placed again.
            // Not during play (that world is thrown away) and not in a
            // collaboration session: the link is written straight into the
            // world, which does not replicate, for the reason the prefab
            // sync sits sessions out too.
            const bool inSession = ctx.collab && ctx.collab->inSession();
            if (!ctx.isPlaying && !inSession)
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow("Save as Prefab");
                if (!SceneSerializer::linkPrefabSource(*ctx.world, entity, prefab.id, prefab.data))
                    HE_LOG_WARN(Editor, "%s", ("Editor: saved prefab " + relPath +
                                               ", but could not link the source to it").c_str());
            }
            else if (inSession)
                HE_LOG_INFO(Editor, "%s", ("Editor: saved prefab " + relPath + "; the source stays "
                                           "unlinked during a collaboration session").c_str());
            ctx.contentManager->registerPrefab(std::move(prefab));
            ctx.contentRefreshPending = true;
            // Announce it as a CREATE, which is what it is. Without
            // this the file reached the others only through the
            // ordinary whole-file save path — as an UPDATE to an
            // asset they had never heard of, and with no name
            // arbitration at all. The uniquifier above only ever
            // consults this machine's disk, so two people saving an
            // entity called "Arm" at the same moment both pick
            // Prefabs/Arm.hasset, and whichever update lands second
            // silently replaces the first person's prefab. The
            // create path is where the host settles a taken name and
            // tells the loser their asset was renamed; the content
            // browser has gone through it since creates began
            // replicating, and this menu item never did.
            if (ctx.collab) ctx.collab->publishAssetCreate(relPath, fullPath);
            HE_LOG_INFO(Editor, "%s", ("Editor: saved prefab " + relPath).c_str());
        }
        else
            HE_LOG_ERROR(Editor, "%s", ("Editor: failed to save prefab " + relPath).c_str());
    }

    // ── The eye and the padlock ──────────────────────────────────────────────
    // Drawn from primitives rather than glyphs: the editor font has no eye or
    // lock in it, and a shape drawn through GetColorU32 dims with the row the
    // way a glyph would not. `min` is the top-left of a `size`×`size` box.
    void drawEye(ImDrawList* dl, const ImVec2& min, float size, ImU32 col, bool open)
    {
        const ImVec2 c(min.x + size * 0.5f, min.y + size * 0.5f);
        const float  rx = size * 0.44f, ry = size * 0.27f;
        const float  th = std::max(1.0f, size * 0.09f);
        dl->AddEllipse(c, ImVec2(rx, ry), col, 0.0f, 0, th);
        if (open)
            dl->AddCircleFilled(c, size * 0.14f, col);
        else
            // Shut: a slash through it, corner to corner, the universal "not".
            dl->AddLine(ImVec2(c.x - rx, c.y + ry), ImVec2(c.x + rx, c.y - ry), col, th);
    }

    void drawPadlock(ImDrawList* dl, const ImVec2& min, float size, ImU32 col, bool locked)
    {
        const float th = std::max(1.0f, size * 0.09f);
        const float w  = size * 0.56f;                 // body width
        const float x0 = min.x + (size - w) * 0.5f, x1 = x0 + w;
        const float y1 = min.y + size * 0.92f;         // body bottom
        const float y0 = min.y + size * 0.50f;         // body top
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, size * 0.08f);
        // The shackle: an arc over the body when locked; when open it stands
        // beside it, one leg lifted, so the two states read apart at a glance.
        const float r  = w * 0.32f;
        const float cx = locked ? (x0 + x1) * 0.5f : x1 - r * 0.15f;
        const float cy = y0 - r * 0.15f;
        dl->PathClear();
        dl->PathArcTo(ImVec2(cx, cy), r, IM_PI, IM_PI * 2.0f, 12);
        if (locked)
        {
            dl->PathLineTo(ImVec2(cx + r, y0));
            dl->PathStroke(col, ImDrawFlags_None, th);
            dl->PathLineTo(ImVec2(cx - r, cy));
            dl->PathLineTo(ImVec2(cx - r, y0));
            dl->PathStroke(col, ImDrawFlags_None, th);
        }
        else
        {
            dl->PathLineTo(ImVec2(cx + r, cy + r * 0.4f));
            dl->PathStroke(col, ImDrawFlags_None, th);
            dl->PathLineTo(ImVec2(cx - r, cy));
            dl->PathLineTo(ImVec2(cx - r, y0));
            dl->PathStroke(col, ImDrawFlags_None, th);
        }
    }

    // ── Row furniture ────────────────────────────────────────────────────────
    // Everything below is drawn into the window's draw list on top of the row
    // ImGui lays out, at fixed offsets from its rectangle. The row itself is a
    // TreeNodeEx with no label (so its arrow, its click and its fold state are
    // ImGui's), `rowHeight()` tall — one height for every row, which is what the
    // row clipping in render() measures and relies on.
    constexpr float kRowGap      = 1.0f;   // between two rows
    constexpr float kRowPadRight = 6.0f;   // right edge → padlock

    float rowHeight() { return std::floor(ImGui::GetFontSize() * 1.6f); }

    ImVec4 colorOf(unsigned rgb)
    {
        return ImVec4(float((rgb >> 16) & 255u) / 255.0f, float((rgb >> 8) & 255u) / 255.0f,
                      float(rgb & 255u) / 255.0f, 1.0f);
    }
    ImVec4 kindColor(int kind) { return colorOf(OutlinerFilter::kindColor(kind)); }
    // `c` at a fraction of its own alpha, as a draw-list colour.
    ImU32 withAlpha(const ImVec4& c, float a) { return ImGui::GetColorU32(HE::Ed::Theme::alpha(c, c.w * a)); }

    // The little square in front of a name: the entity's kind as a colour and a
    // letter. A kind of 0 is "just a group": an empty frame.
    void drawTypeIcon(ImDrawList* dl, const ImVec2& min, float size, int kind, float alpha)
    {
        const ImVec4 k   = kindColor(kind);
        const ImVec2 max(min.x + size, min.y + size);
        dl->AddRectFilled(min, max, withAlpha(k, 0.20f * alpha), 4.0f);
        dl->AddRect(min, max, withAlpha(k, 0.70f * alpha), 4.0f, 0, 1.0f);
        if (kind == OutlinerFilter::kAllKinds)
        {
            const float in = std::floor(size * 0.30f);
            dl->AddRect(ImVec2(min.x + in, min.y + in), ImVec2(max.x - in, max.y - in),
                        withAlpha(k, 0.80f * alpha), 1.0f, 0, 1.5f);
            return;
        }
        const char*  glyph = OutlinerFilter::kindGlyph(kind);
        ImFont*      font  = ImGui::GetFont();
        const float  fs    = ImGui::GetFontSize() * 0.78f;
        const ImVec2 ts    = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, glyph);
        dl->AddText(font, fs, ImVec2(std::floor(min.x + (size - ts.x) * 0.5f),
                                     std::floor(min.y + (size - ts.y) * 0.5f)),
                    withAlpha(k, alpha), glyph);
    }

    // A pill after the name ([Prefab], [+], a collaborator). `h` is its height.
    float chipWidth(const char* text) { return std::ceil(ImGui::CalcTextSize(text).x) + 12.0f; }
    void drawChip(ImDrawList* dl, const ImVec2& min, float w, float h, const char* text,
                  ImU32 fill, ImU32 edge, ImU32 ink)
    {
        const ImVec2 max(min.x + w, min.y + h);
        dl->AddRectFilled(min, max, fill, h * 0.5f);
        if (edge) dl->AddRect(min, max, edge, h * 0.5f, 0, 1.0f);
        dl->AddText(ImVec2(std::floor(min.x + 6.0f), std::floor(min.y + (h - ImGui::GetFontSize()) * 0.5f)),
                    ink, text);
    }

    // The two buttons at the right edge of one Outliner row, over the row. They
    // are items of their own (ItemAdd + ButtonBehavior — an InvisibleButton
    // without the cursor movement, which this layout does not want), so a click
    // on one is not a click on the row. `hot`: the pointer is on this row; a
    // visible eye and an open padlock only show then, while a hidden entity's
    // slashed eye and a locked one's padlock stay, in the accent, as the row's
    // state. The row's right edge is `right`; `rowH` its height.
    void drawRowIcons(AppContext& ctx, Entity entity, HE::Visibility vis, const ImVec2& rowMin,
                      float right, float rowH, bool hot)
    {
        auto& reg = ctx.world->registry();
        const float sz = std::floor(rowH * 0.6f);
        const float y  = std::floor(rowMin.y + (rowH - sz) * 0.5f);
        const ImVec2 lockMin(right - kRowPadRight - sz, y);
        const ImVec2 eyeMin(lockMin.x - 2.0f - sz, y);

        // A row scrolled out of the panel: nothing drawn and no subtree walk. The
        // panel lists every row, and the eye and padlock are ImDrawList paths that
        // ImGui does not clip away by itself — at 10k entities they were most of
        // the editor's frame (docs/world-streaming-baseline-2026-10-06.md §3.3).
        // An item nobody can see cannot be clicked either, so nothing else is lost.
        if (!ImGui::IsRectVisible(eyeMin, ImVec2(lockMin.x + sz, y + sz)))
            return;

        ImGuiWindow* win = ImGui::GetCurrentWindow();
        ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
        ImDrawList* dl = win->DrawList;
        const bool editable = !ctx.isPlaying;

        // ── Eye ──
        // Nothing to draw anywhere below: no eye, and no item to click either.
        if (vis != HE::Visibility::None)
        {
            const bool shown = vis == HE::Visibility::Visible;
            const ImRect bb(eyeMin, ImVec2(eyeMin.x + sz, eyeMin.y + sz));
            const ImGuiID id = win->GetID("##eye");
            if (ImGui::ItemAdd(bb, id))
            {
                bool hovered = false, held = false;
                ImGui::ButtonBehavior(bb, id, &hovered, &held);
                const bool over = editable && ImGui::IsItemHovered();
                if (!shown || hot || over)
                {
                    const ImU32 col = over   ? ImGui::GetColorU32(HE::Ed::Theme::AccentBright)
                                    : !shown ? ImGui::GetColorU32(HE::Ed::Theme::Accent)
                                             : ImGui::GetColorU32(HE::Ed::Theme::alpha(
                                                   ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.55f));
                    drawEye(dl, bb.Min, sz, col, shown);
                }
                EditorWidgets::helpForKey("outliner.visibility");
                if (editable && ImGui::IsItemClicked(ImGuiMouseButton_Left))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow(shown ? "Hide Entity" : "Show Entity");
                    HE::setSubtreeVisible(reg, entity, !shown);
                    // Every entity touched, named for the prefab-override
                    // recording: none of them need be selected (see
                    // AppContext::noteEntityEdited).
                    if (ctx.noteEntityEdited)
                    {
                        std::function<void(Entity)> note = [&](Entity e)
                        {
                            if (!reg.valid(e)) return;
                            ctx.noteEntityEdited(e);
                            if (const auto* h = reg.try_get<HierarchyComponent>(e))
                                for (const Entity c : h->children) note(c);
                        };
                        note(entity);
                    }
                }
            }
        }

        // ── Padlock ──
        {
            const bool locked = reg.all_of<EditorLockComponent>(entity);
            const ImRect bb(lockMin, ImVec2(lockMin.x + sz, lockMin.y + sz));
            const ImGuiID id = win->GetID("##lock");
            if (ImGui::ItemAdd(bb, id))
            {
                bool hovered = false, held = false;
                ImGui::ButtonBehavior(bb, id, &hovered, &held);
                const bool over = editable && ImGui::IsItemHovered();
                if (locked || hot || over)
                {
                    // Locked is loud, an open padlock barely there: the row's
                    // normal state should not wear a badge.
                    const ImU32 col = over   ? ImGui::GetColorU32(HE::Ed::Theme::AccentBright)
                                    : locked ? ImGui::GetColorU32(HE::Ed::Theme::Accent)
                                             : ImGui::GetColorU32(HE::Ed::Theme::alpha(
                                                   ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.35f));
                    drawPadlock(dl, bb.Min, sz, col, locked);
                }
                EditorWidgets::helpForKey("outliner.lock");
                if (editable && ImGui::IsItemClicked(ImGuiMouseButton_Left))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow(locked ? "Unlock Entity" : "Lock Entity");
                    if (locked) reg.remove<EditorLockComponent>(entity);
                    else        reg.emplace_or_replace<EditorLockComponent>(entity);
                }
            }
        }
        ImGui::PopID();
    }
}
#endif

static bool s_rowClipping = true;
void setRowClipping(bool on) { s_rowClipping = on; }

void render(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    HE_PROFILE_SCOPE_N("OutlinerPanel::render");
    // Whatever this panel pushes onto the undo stack without a label of its
    // own (Move Up, Sort Children, the row menu's Lock) reads as "Outliner" in
    // the history window rather than as a bare "Edit".
    EditorUndo::Context undoScope(ctx.undoSys, "Outliner");
    // World Outliner
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("World Outliner");
    if (ctx.fontHeading) ImGui::PopFont();

    // ── Application projects: the widget hierarchy, not the world ───────────
    // An app has no entities to list (docs/he-apps-plan.md E2). What it does have
    // is whatever its GameInstance built, so the panel shows THAT — the live
    // instances and their elements, read from the manager's own copies, which is
    // also what makes it a diagnosis: an empty panel says "nothing was created",
    // which is exactly the question an empty preview raises.
    if (ctx.appLivePreview && ctx.world)
    {
        // The wrap guard gets its OWN scope, closing before ImGui::End() below.
        // Sharing a scope with the End() means the pop runs after it — on
        // whatever window is current by then, which for a top-level panel is
        // ImGui's Debug window, and ImGui says so with "Calling PopTextWrapPos()
        // too many times!". The entity branch below documents the same trap; this
        // branch has an early return, which is what made it easy to walk into.
        {
        EditorWidgets::WrapText wrap;
        const WidgetManager& wm = ctx.world->widgets();
        const std::vector<int> ids = wm.liveIds();
        if (ids.empty())
        {
            ImGui::TextDisabled("No widgets.");
            ImGui::Spacing();
            ImGui::TextWrapped("The Game Instance creates the interface in its OnInit. "
                               "If this stays empty, that graph is not reaching a "
                               "Create Widget node.");
        }
        // Draw one element and its children. Recursive by lambda so it stays next
        // to the only place that uses it.
        std::function<void(const HE::UIWidgetTree&, int)> drawElem =
            [&](const HE::UIWidgetTree& tree, int parentId)
        {
            for (const auto& ep : tree.elements)
            {
                if (!ep || ep->parentId != parentId) continue;
                const HE::UIElement& e = *ep;
                bool hasChildren = false;
                for (const auto& c : tree.elements)
                    if (c && c->parentId == e.id) { hasChildren = true; break; }

                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                                           ImGuiTreeNodeFlags_DefaultOpen;
                if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf |
                                           ImGuiTreeNodeFlags_NoTreePushOnOpen;
                const std::string label = (e.name.empty() ? std::string(e.typeName())
                                                          : e.name) +
                                          "##el" + std::to_string(e.id);
                const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
                // The type beside the name, dimmed: two elements called "Row" are
                // told apart by what they ARE, and the name alone will not say.
                ImGui::SameLine();
                ImGui::TextDisabled("%s%s", e.typeName(), e.visible ? "" : " (hidden)");
                if (open && hasChildren) { drawElem(tree, e.id); ImGui::TreePop(); }
            }
        };

        for (const int id : ids)
        {
            const HE::UIWidgetTree* tree = wm.tree(id);
            if (!tree) continue;
            const std::string title = "Widget " + std::to_string(id) +
                                      (wm.isVisible(id) ? "" : " (hidden)") +
                                      "##w" + std::to_string(id);
            if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                                 ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                drawElem(*tree, 0);
                ImGui::TreePop();
            }
        }
        }   // wrap guard pops HERE, while this panel is still the current window
        ImGui::End();
        return;
    }

    if (ctx.world)
    {
        // Text this panel writes beside the tree — above all a collaborator's name
        // on a locked row — wraps to the panel instead of running off its right
        // edge. The Outliner is one of the narrowest docks in the editor, and a
        // name that is merely cut off tells the reader nothing about WHO holds the
        // lock, which is the only reason that badge exists. The guard lives inside
        // this block on purpose: it has to be popped before the ImGui::End() below,
        // and a PopTextWrapPos after End() would land on whatever window is current
        // by then — for a top-level panel, none at all. Tree labels themselves are
        // drawn by RenderText, which ignores the wrap position, so entity names are
        // unaffected either way.
        EditorWidgets::WrapText wrap;

        // ── Cached hierarchy snapshot ─────────────────────────────────────
        struct OutlinerNode
        {
            Entity      entity;
            std::string name;
            int         depth;
            bool        hasChildren;
            // Placed prefabs, decided at rebuild time because both are
            // questions of structure, and structure is what dirties the
            // hierarchy: on an instance root, how many children were deleted
            // or added here; on a row under one, whether it is the topmost
            // entity of something added here (no placement binds it).
            size_t      structuralChanges = 0;
            bool        addedHere         = false;
            // Rows directly under this one, as listed (the hidden built-ins are
            // not counted) — what a folded row says is inside it.
            int         childCount        = 0;
        };
        static std::vector<OutlinerNode> s_outlinerCache;
        static HorizonWorld*             s_lastWorld = nullptr;

        // Rebuild cache only when the hierarchy changed or the world switched
        bool cacheRebuilt = false;
        if (ctx.world->isHierarchyDirty() || s_lastWorld != ctx.world)
        {
            cacheRebuilt = true;
            s_lastWorld = ctx.world;
            s_outlinerCache.clear();

            auto& registry = ctx.world->registry();
            Entity root    = ctx.world->rootEntity();

            // `underInstance`: some ancestor is a placement root; `parentAdded`:
            // the parent is itself added here, so this row is inside an added
            // subtree rather than the top of one.
            std::function<bool(Entity, int, bool, bool)> collect =
                [&](Entity entity, int depth, bool underInstance, bool parentAdded) -> bool
            {
                if (!registry.valid(entity)) return false;
                // The built-in environment sun/moon lights belong to the World's
                // Environment, and runtime terrain chunks are generated from the
                // TerrainComponent — hide both from the Outliner.
                if (entity != ctx.world->rootEntity() &&
                    (registry.all_of<EnvironmentLightComponent>(entity) ||
                     registry.all_of<TerrainChunkComponent>(entity)))
                    return false;
                auto* name = registry.try_get<NameComponent>(entity);
                auto* hier = registry.try_get<HierarchyComponent>(entity);
                OutlinerNode node{
                    entity,
                    name ? name->name : "(unnamed)",
                    depth,
                    hier && !hier->children.empty()
                };
                const bool isInstance = registry.all_of<PrefabInstanceComponent>(entity);
                if (isInstance)
                    node.structuralChanges = SceneSerializer::prefabRemovedHereCount(*ctx.world, entity)
                                           + SceneSerializer::prefabAddedHereCount(*ctx.world, entity);
                bool added = false;
                if (underInstance && !isInstance)
                    added = SceneSerializer::prefabInstancesBinding(*ctx.world, entity).empty();
                node.addedHere = added && !parentAdded;
                const size_t self = s_outlinerCache.size();
                s_outlinerCache.push_back(std::move(node));
                int shownChildren = 0;
                if (hier)
                    for (Entity child : hier->children)
                        if (collect(child, depth + 1, underInstance || isInstance, added))
                            ++shownChildren;
                s_outlinerCache[self].childCount = shownChildren;
                return true;
            };
            collect(root, 0, false, false);

            char buf[96];
            std::snprintf(buf, sizeof(buf), "[Outliner] rebuilt: %zu nodes", s_outlinerCache.size());
            HE_LOG_INFO(Editor, "%s", buf);

            ctx.world->clearHierarchyDirty();
        }

        // ── Inline rename state ───────────────────────────────────────────
        // A double-click on a name, F2 or the row menu's Rename turns that row's
        // name into a text box (see the row loop). One entity at a time.
        static Entity      s_renameEntity = entt::null;
        static std::string s_renameBuf;
        static bool        s_renameFocus  = false;   // grab the keyboard on the first frame
        static bool        s_renameActive = false;   // the box has been active at least once
        const auto beginRename = [&](Entity e, const std::string& name)
        {
            s_renameEntity = e;
            s_renameBuf    = name;
            s_renameFocus  = true;
            s_renameActive = false;
        };
        if (s_renameEntity != entt::null &&
            (!ctx.world->registry().valid(s_renameEntity) || ctx.isPlaying))
            s_renameEntity = entt::null;

        // F2 renames the primary selection — while this panel is the focused one
        // and no text box is being typed in.
        if (!ctx.isPlaying && s_renameEntity == entt::null &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false))
        {
            const Entity primary = ctx.selection.primary();
            if (primary != entt::null && primary != ctx.world->rootEntity() &&
                ctx.world->registry().valid(primary))
                if (const auto* nc = ctx.world->registry().try_get<NameComponent>(primary))
                    beginRename(primary, nc->name);
        }

        // ── What the scene is made of, for the chips ───────────────────────
        // Counted a slice of the cache per frame and published when the sweep
        // completes, then started over: a Light added in Details does not dirty
        // the hierarchy, so the numbers cannot be tied to the rebuild alone, and
        // sweeping 50k entities in one frame is the hitch the row clipping below
        // exists to avoid. A small scene is counted in its first frame.
        static std::vector<int> s_kindCounts;
        static std::vector<int> s_kindSweep;
        static size_t           s_sweepAt = 0;
        {
            const size_t kinds = static_cast<size_t>(OutlinerFilter::kindCount());
            if (s_kindCounts.size() != kinds) { s_kindCounts.assign(kinds, 0); s_kindSweep.assign(kinds, 0); s_sweepAt = 0; }
            if (cacheRebuilt) { std::fill(s_kindSweep.begin(), s_kindSweep.end(), 0); s_sweepAt = 0; }
            constexpr size_t kSlice = 2048;
            const size_t end = std::min(s_outlinerCache.size(), s_sweepAt + kSlice);
            const auto& reg = ctx.world->registry();
            for (; s_sweepAt < end; ++s_sweepAt)
            {
                const Entity e = s_outlinerCache[s_sweepAt].entity;
                if (e != ctx.world->rootEntity() && reg.valid(e))
                    OutlinerFilter::tally(reg, e, s_kindSweep.data());
            }
            if (s_sweepAt >= s_outlinerCache.size())
            {
                s_kindCounts = s_kindSweep;
                std::fill(s_kindSweep.begin(), s_kindSweep.end(), 0);
                s_sweepAt = 0;
            }
        }

        // ── Search + type filter ──────────────────────────────────────────
        // The same header the Content Browser has, for the same reason: a
        // panel that can show a few hundred rows but not find one among them
        // is a panel people scroll instead of use. Typing narrows by name, the
        // chips below by what the entity IS (see OutlinerFilter.h); both at once
        // means both. State is per editor, not per world — a search survives a
        // scene switch, which is what a search typed for "the torch in every
        // level" needs.
        static std::string s_searchText;
        static int         s_typeFilter = OutlinerFilter::kAllKinds;
        {
            const float clearW = ImGui::GetFrameHeight();
            const float addW   = clearW;
            const float avail  = ImGui::GetContentRegionAvail().x;

            // ── "+": a new entity, from the header ────────────────────────
            // The same Create menu the empty space's right-click and the
            // Entity menu open, with a fixed address in the panel: in a full
            // Outliner there IS no empty space to right-click, and the
            // background menu was the only place in the panel that made one.
            // Greyed while playing, like Entity ▸ Create.
            ImGui::BeginDisabled(ctx.isPlaying);
            if (ImGui::Button("+##outliner_add", ImVec2(addW, 0.0f)))
                ImGui::OpenPopup("##outliner_add_menu");
            ImGui::EndDisabled();
            EditorWidgets::helpForKey("outliner.create");
            if (ImGui::BeginPopup("##outliner_add_menu"))
            {
                drawCreateEntityMenu(ctx);
                ImGui::EndPopup();
            }
            ImGui::SameLine();

            ImGui::SetNextItemWidth(std::max(60.0f, avail - addW - clearW -
                                             ImGui::GetStyle().ItemSpacing.x * 2.0f));
            ImGui::InputTextWithHint("##outliner_search", "Search entities", &s_searchText);
            EditorWidgets::helpForKey("outliner.search");
            ImGui::SameLine();
            // One click back to the whole tree — undoing a search by clearing
            // two controls is the friction that stops people searching.
            const bool anyFilter = !s_searchText.empty() ||
                                   s_typeFilter != OutlinerFilter::kAllKinds;
            ImGui::BeginDisabled(!anyFilter);
            if (ImGui::Button("\xC3\x97##outliner_clear_filter", ImVec2(clearW, 0.0f)))
            {
                s_searchText.clear();
                s_typeFilter = OutlinerFilter::kAllKinds;
            }
            ImGui::EndDisabled();
            if (anyFilter && ImGui::IsItemHovered())
                ImGui::SetTooltip("Clear the search and the type filter");

            // ── Type chips ────────────────────────────────────────────────
            // What the scene contains, one pill per kind that is in it (and the
            // one filtered on, even at zero), with its count: a click shows only
            // those, a second click shows everything again. Replaces a 20-entry
            // dropdown that listed kinds the scene has none of. Three lines at
            // most; the rest fold into a "+N" pill that opens them as a list.
            {
                struct Chip { int kind; float w; };
                std::vector<Chip> chips;
                const float chipH = std::floor(ImGui::GetFontSize() * 1.35f);
                const float gap   = 4.0f;
                char label[64];
                const auto chipLabelW = [&](int kind)
                {
                    const char* name = kind == OutlinerFilter::kAllKinds ? "All" : OutlinerFilter::kindAt(kind).label;
                    std::snprintf(label, sizeof(label), "%d", s_kindCounts[static_cast<size_t>(kind)]);
                    return (kind == OutlinerFilter::kAllKinds ? 0.0f : 14.0f) +
                           std::ceil(ImGui::CalcTextSize(name).x) + 6.0f +
                           std::ceil(ImGui::CalcTextSize(label).x) + 16.0f;
                };
                for (int k = 0; k < OutlinerFilter::kindCount(); ++k)
                    if (k == OutlinerFilter::kAllKinds || k == s_typeFilter ||
                        s_kindCounts[static_cast<size_t>(k)] > 0)
                        chips.push_back({ k, chipLabelW(k) });

                // Greedy layout: how many fit on three lines, leaving room for the
                // overflow pill at the end of the third.
                constexpr int kMaxLines = 3;
                const float overflowW = ImGui::CalcTextSize("+99").x + 16.0f;
                std::vector<int> lineOf(chips.size(), 0);
                size_t fit = chips.size();
                {
                    int line = 0; float x = 0.0f;
                    for (size_t i = 0; i < chips.size(); ++i)
                    {
                        if (x > 0.0f && x + gap + chips[i].w > avail) { ++line; x = 0.0f; }
                        if (line >= kMaxLines) { fit = i; break; }
                        x += (x > 0.0f ? gap : 0.0f) + chips[i].w;
                        lineOf[i] = line;
                    }
                    if (fit < chips.size())
                    {
                        // Make room for the overflow pill on the last line.
                        while (fit > 0)
                        {
                            float lastX = 0.0f;
                            for (size_t i = 0; i < fit; ++i)
                                if (lineOf[i] == kMaxLines - 1) lastX += (lastX > 0.0f ? gap : 0.0f) + chips[i].w;
                            if (lastX + gap + overflowW <= avail) break;
                            --fit;
                        }
                    }
                }

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 3.0f));
                int curLine = -1;
                const auto place = [&](size_t i, float w) -> bool
                {
                    const int line = i < chips.size() ? lineOf[i] : kMaxLines - 1;
                    if (line == curLine) ImGui::SameLine();
                    curLine = line;
                    (void)w;
                    return true;
                };
                for (size_t i = 0; i < fit; ++i)
                {
                    const int kind = chips[i].kind;
                    place(i, chips[i].w);
                    ImGui::PushID(kind);
                    ImGui::InvisibleButton("##chip", ImVec2(chips[i].w, chipH));
                    ImGui::PopID();
                    const bool hov = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                        s_typeFilter = (kind == s_typeFilter && kind != OutlinerFilter::kAllKinds)
                                           ? OutlinerFilter::kAllKinds : kind;
                    EditorWidgets::helpForKey("outliner.type-filter");
                    const bool on = kind == s_typeFilter;
                    const ImVec4 kc = kindColor(kind);
                    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                    dl->AddRectFilled(mn, mx, withAlpha(kc, on ? 0.34f : hov ? 0.22f : 0.12f), chipH * 0.5f);
                    if (on) dl->AddRect(mn, mx, withAlpha(kc, 0.85f), chipH * 0.5f, 0, 1.0f);
                    float tx = mn.x + 8.0f;
                    if (kind != OutlinerFilter::kAllKinds)
                    {
                        dl->AddCircleFilled(ImVec2(tx + 3.0f, (mn.y + mx.y) * 0.5f), 3.0f, withAlpha(kc, 1.0f));
                        tx += 14.0f;
                    }
                    const char* name = kind == OutlinerFilter::kAllKinds ? "All" : OutlinerFilter::kindAt(kind).label;
                    const float ty = std::floor(mn.y + (chipH - ImGui::GetFontSize()) * 0.5f);
                    dl->AddText(ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text), name);
                    std::snprintf(label, sizeof(label), "%d", s_kindCounts[static_cast<size_t>(kind)]);
                    dl->AddText(ImVec2(tx + std::ceil(ImGui::CalcTextSize(name).x) + 6.0f, ty),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
                }
                if (fit < chips.size())
                {
                    place(chips.size(), overflowW);
                    char more[16];
                    std::snprintf(more, sizeof(more), "+%d", static_cast<int>(chips.size() - fit));
                    const float w = ImGui::CalcTextSize(more).x + 16.0f;
                    if (ImGui::InvisibleButton("##chip_more", ImVec2(w, chipH)))
                        ImGui::OpenPopup("##outliner_more_kinds");
                    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button),
                                      chipH * 0.5f);
                    dl->AddText(ImVec2(mn.x + 8.0f, std::floor(mn.y + (chipH - ImGui::GetFontSize()) * 0.5f)),
                                ImGui::GetColorU32(ImGuiCol_Text), more);
                    EditorWidgets::helpForKey("outliner.type-filter");
                }
                ImGui::PopStyleVar();
                if (ImGui::BeginPopup("##outliner_more_kinds"))
                {
                    for (size_t i = fit; i < chips.size(); ++i)
                    {
                        const int kind = chips[i].kind;
                        std::snprintf(label, sizeof(label), "%s  %d", OutlinerFilter::kindAt(kind).label,
                                      s_kindCounts[static_cast<size_t>(kind)]);
                        if (ImGui::Selectable(label, kind == s_typeFilter))
                            s_typeFilter = kind;
                    }
                    ImGui::EndPopup();
                }
            }
        }
        const bool filterActive = !s_searchText.empty() ||
                                  s_typeFilter != OutlinerFilter::kAllKinds;

        // Which rows survive, and as what. Run every frame while a filter is
        // on: the type test reads components straight from the registry, and
        // a component added in Details does not dirty the hierarchy cache.
        // The World root is never a hit of its own — it is the tree's
        // built-in top, not something the user placed — so it only ever
        // appears as the dimmed path to whatever was found under it.
        std::vector<OutlinerFilter::Shown> shown;
        size_t hitCount = 0;
        if (filterActive)
        {
            std::vector<OutlinerFilter::Row> rows;
            rows.reserve(s_outlinerCache.size());
            const auto& reg = ctx.world->registry();
            const OutlinerFilter::Kind& kind = OutlinerFilter::kindAt(s_typeFilter);
            for (const auto& node : s_outlinerCache)
            {
                const bool match = node.entity != ctx.world->rootEntity() &&
                                   reg.valid(node.entity) &&
                                   OutlinerFilter::nameMatches(node.name, s_searchText) &&
                                   kind.test(reg, node.entity);
                rows.push_back({ node.depth, match });
                if (match) ++hitCount;
            }
            shown = OutlinerFilter::apply(rows);
        }

        // ── Status line: how much is shown, and what is selected ──────────
        // Left, the count (of the whole scene while a filter narrows it); right,
        // where the selection lives — its path from the World down, with the
        // front of it dropped when the panel is too narrow, because the end is
        // the part that says which entity it is.
        {
            const size_t total = s_outlinerCache.empty() ? 0 : s_outlinerCache.size() - 1;
            char left[64];
            if (filterActive) std::snprintf(left, sizeof(left), "%zu of %zu entities", hitCount, total);
            else              std::snprintf(left, sizeof(left), "%zu entities", total);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::TextDisabled("%s", left);

            std::string right;
            const size_t picked = ctx.selection.entities().size();
            const auto& reg = ctx.world->registry();
            if (picked > 1)
                right = std::to_string(picked) + " selected";
            else if (picked == 1)
            {
                std::vector<std::string> names;
                for (Entity e = ctx.selection.primary();
                     e != entt::null && reg.valid(e) && e != ctx.world->rootEntity() && names.size() < 64;)
                {
                    const auto* nc = reg.try_get<NameComponent>(e);
                    names.push_back(nc ? nc->name : std::string("(unnamed)"));
                    const auto* hc = reg.try_get<HierarchyComponent>(e);
                    e = hc ? hc->parent : entt::null;
                }
                const float room = ImGui::GetCurrentWindow()->WorkRect.Max.x -
                                   (at.x + ImGui::CalcTextSize(left).x + 16.0f);
                for (size_t from = 0; from < names.size(); ++from)
                {
                    std::string t = from > 0 ? "\xE2\x80\xA6 \xE2\x80\xBA " : "";
                    for (size_t i = names.size() - from; i-- > 0;)
                    {
                        t += names[i];
                        if (i > 0) t += " \xE2\x80\xBA ";
                    }
                    right = t;
                    if (ImGui::CalcTextSize(t.c_str()).x <= room || from + 1 == names.size()) break;
                }
            }
            if (!right.empty())
            {
                const float rightEdge = ImGui::GetCurrentWindow()->WorkRect.Max.x;
                const float minX = at.x + ImGui::CalcTextSize(left).x + 16.0f;
                const float w = ImGui::CalcTextSize(right.c_str()).x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(ImVec2(minX, at.y), ImVec2(rightEdge, at.y + ImGui::GetTextLineHeight() + 2.0f), true);
                dl->AddText(ImVec2(std::max(minX, rightEdge - w), at.y),
                            ImGui::GetColorU32(HE::Ed::Theme::alpha(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.70f)),
                            right.c_str());
                dl->PopClipRect();
            }
            ImGui::Separator();
            if (filterActive && hitCount == 0)
                ImGui::TextDisabled("Nothing matches. The search covers every entity "
                                    "in the scene, open or folded.");
        }

        // ── Render from cache ─────────────────────────────────────────────
        int skipBelowDepth = INT_MAX; // skip children of closed nodes

        // While a filter is on, the tree's open/closed state lives in its own
        // ID namespace, and every branch on the way to a hit is forced open
        // there: a hit under a folded parent is a hit nobody sees. Doing it in
        // a separate namespace is what keeps the user's own folds — the state
        // under the unfiltered IDs — untouched, so clearing the search puts
        // the tree back exactly as it was.
        if (filterActive) ImGui::PushID("##outliner_filtered");

        // ── Rows scrolled out of the panel are not submitted ──────────────
        // Every row used to go through TreeNodeEx, which measures its label
        // even when ImGui clips it away: at 50k entities the Outliner was a
        // quarter of the editor's frame (23 ms, Thema 153 §11). A row outside
        // the panel now only answers the two questions the rows after it
        // depend on — is it open, and what is its ID — and adds its height to
        // a run that is laid out as ONE Dummy before the next row that shows.
        //
        // The ID is the one TreeNodeEx would compute (a pointer id hashed with
        // the ID of the open row above, which is what TreeNodeEx pushes), so
        // the open/closed state read from the window's storage is the user's
        // own, and a row scrolling into view keeps its fold. A visible row
        // under clipped parents gets their levels pushed first (indent + ID),
        // exactly as their own TreeNodeEx would have left them. The theme
        // draws no tree lines, so TreeNodeEx keeps no per-level data that a
        // bare TreePushOverrideID would miss.
        //
        // The row height is measured off two consecutive drawn rows; until
        // then (the first frame) everything is drawn, as before. The World
        // root's row is measured on its own: it has no eye or padlock, and the
        // padlock is what sets the other rows' height.
        static float s_rowStep  = 0.0f;
        static float s_rootStep = 0.0f;
        ImGuiWindow* const outlinerWindow = ImGui::GetCurrentWindow();
        const ImRect  rowClip   = outlinerWindow->ClipRect;
        const ImGuiID rowSeed0  = outlinerWindow->IDStack.back();
        const float   rowStep   = s_rowClipping && s_rootStep > 0.0f ? s_rowStep : 0.0f;
        const float   rootStep  = s_rootStep;
        bool measureRoot = false;   // the row measureFrom belongs to is the root
        std::vector<ImGuiID> pathIds;   // pathIds[d]: the open row at depth d on the current path
        int   pathDepth   = -1;         // deepest open level on the path
        int   pushedDepth = -1;         // deepest level really pushed in ImGui
        float skipped     = 0.0f;       // height of clipped rows not laid out yet
        float measureFrom = -1.0f;      // where the previous row started, when it was drawn
        // The clipped run as one Dummy, with the gap a row has: a row's gap is
        // kRowGap, so the run's is too, or the content ends a few pixels short of
        // what drawing every row gives (and the scroll range with it).
        const auto skippedRun = [](float height)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, kRowGap));
            ImGui::Dummy(ImVec2(1.0f, height - kRowGap));
            ImGui::PopStyleVar();
        };
        const auto rowId = [&](Entity e, int depth) -> ImGuiID
        {
            const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(e)));
            const ImGuiID seed = depth > 0 && depth - 1 < static_cast<int>(pathIds.size())
                               ? pathIds[static_cast<size_t>(depth - 1)] : rowSeed0;
            return ImHashData(&ptr, sizeof(void*), seed);
        };
        std::vector<Entity> pathEnt;
        const auto enterPath = [&](int depth, ImGuiID id, Entity who)
        {
            if (static_cast<int>(pathIds.size()) <= depth) pathIds.resize(static_cast<size_t>(depth) + 1);
            pathIds[static_cast<size_t>(depth)] = id;
            if (static_cast<int>(pathEnt.size()) <= depth) pathEnt.resize(static_cast<size_t>(depth) + 1, entt::null);
            pathEnt[static_cast<size_t>(depth)] = who;
            pathDepth = depth;
        };

        // The rows that were actually DRAWN this frame, top to bottom. A
        // Shift-click selects "everything between the anchor and this row", and
        // "between" means in the order the user sees — closed subtrees are not
        // in it. Collected during the loop and resolved after it, because the
        // clicked row may lie above the anchor.
        std::vector<Entity> visibleRows;
        visibleRows.reserve(s_outlinerCache.size());
        Entity shiftRangeTarget = entt::null;

        // ── Row look, decided once per frame ──────────────────────────────
        const float rowH  = rowHeight();
        const float padY  = std::floor((rowH - ImGui::GetFontSize()) * 0.5f);
        const float indentStep = ImGui::GetStyle().IndentSpacing;
        // The pointer is on this panel (and no menu or drag has taken it): the
        // rows under it show their eye and padlock.
        const bool paneHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        // Every ancestor of a selected entity: the guide lines on the way to the
        // selection are drawn in the accent, so a deep selection can be followed
        // up through the folds. Capped, so a select-all costs nothing extra.
        std::unordered_set<Entity> selectedPath;
        {
            const auto& reg = ctx.world->registry();
            size_t seen = 0;
            for (const Entity picked : ctx.selection.entities())
            {
                if (++seen > 256) break;
                if (!reg.valid(picked)) continue;
                const auto* hc = reg.try_get<HierarchyComponent>(picked);
                for (Entity up = hc ? hc->parent : entt::null; up != entt::null && reg.valid(up);)
                {
                    if (!selectedPath.insert(up).second) break;   // the rest of the way is known
                    const auto* uh = reg.try_get<HierarchyComponent>(up);
                    up = uh ? uh->parent : entt::null;
                }
            }
        }
        const std::string_view searchNeedle = s_searchText;

        for (size_t nodeIndex = 0; nodeIndex < s_outlinerCache.size(); ++nodeIndex)
        {
            const auto& node = s_outlinerCache[nodeIndex];
            // If a parent was closed, skip all its children
            if (node.depth > skipBelowDepth)
                continue;
            skipBelowDepth = INT_MAX; // back at or above the closed level → reset

            // Filtered out. Skipped BEFORE this row counts as visible, so a
            // Shift-range never spans rows the user cannot see — and before
            // the depth bookkeeping, which is safe because the filter result
            // is closed under "parent of": everything under a hidden row is
            // hidden too, so no open level is ever left dangling by it.
            const OutlinerFilter::Show show =
                filterActive ? shown[nodeIndex].show : OutlinerFilter::Show::Hit;
            if (show == OutlinerFilter::Show::Hidden)
                continue;
            // Under a filter, "has children" means "has children on screen":
            // a hit whose children all fell away is a leaf, arrow and all.
            const bool hasChildren = filterActive ? shown[nodeIndex].childShown
                                                  : node.hasChildren;
            visibleRows.push_back(node.entity);

            // Close tree levels we've left: on the path, and in ImGui as far
            // as they were really pushed.
            while (pathDepth >= node.depth) --pathDepth;
            while (pushedDepth > pathDepth)
            {
                ImGui::TreePop();
                --pushedDepth;
            }

            const float cursorY = ImGui::GetCursorScreenPos().y;
            if (measureFrom >= 0.0f && skipped == 0.0f && cursorY > measureFrom)
                (measureRoot ? s_rootStep : s_rowStep) = cursorY - measureFrom;
            measureFrom = -1.0f;
            const bool  isRootRow = node.entity == ctx.world->rootEntity();
            const float step      = isRootRow ? rootStep : rowStep;
            const float rowTop    = cursorY + skipped;
            if (rowStep > 0.0f && (rowTop + step < rowClip.Min.y || rowTop > rowClip.Max.y))
            {
                // Out of view (see above). A leaf is open the way TreeNodeEx's
                // Leaf flag makes it; under a filter every branch is.
                const ImGuiID id = rowId(node.entity, node.depth);
                const bool open = !hasChildren || filterActive
                               || ImGui::GetStateStorage()->GetInt(id, 1) != 0;   // 1: DefaultOpen
                skipped += step;
                if (open) enterPath(node.depth, id, node.entity);
                else      skipBelowDepth = node.depth;
                continue;
            }
            // In view: first the clipped run above it, then the levels it opened.
            if (skipped > 0.0f)
            {
                skippedRun(skipped);
                skipped = 0.0f;
            }
            while (pushedDepth < pathDepth)
            {
                ++pushedDepth;
                ImGui::TreePushOverrideID(pathIds[static_cast<size_t>(pushedDepth)]);
            }
            measureFrom = ImGui::GetCursorScreenPos().y;
            measureRoot = isRootRow;
            const ImGuiID drawnId = rowId(node.entity, node.depth);

            // AllowOverlap: the eye and the lock sit at the row's right edge,
            // ON the row (SpanFullWidth stretches it under them), and this
            // is what lets them take the click instead of the row. FramePadding
            // is what makes the row `rowH` tall (see the push below).
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanFullWidth
                                     | ImGuiTreeNodeFlags_FramePadding
                                     | ImGuiTreeNodeFlags_DefaultOpen
                                     | ImGuiTreeNodeFlags_AllowOverlap;
            if (!hasChildren)
                flags |= ImGuiTreeNodeFlags_Leaf;
            if (ctx.selection.contains(node.entity))
                flags |= ImGuiTreeNodeFlags_Selected;

            // ── Collaboration lock state ──────────────────────────────────
            // Someone else editing this entity must be visible *before* the
            // click, not after — that is the entire point of holding locks.
            const HE::Net::LockInfo* lock = nullptr;
            bool lockedByMe = false;
            if (ctx.collab && ctx.collab->inSession())
            {
                lock = ctx.collab->lockFor(ctx.collab->subjectFor(
                    static_cast<std::uint32_t>(entt::to_integral(node.entity))));
                lockedByMe = lock && lock->owner == ctx.collab->localParticipant();
            }

            // One text colour push at most: a peer's lock outranks the
            // filter's dimming, because "not yours" matters more than "only
            // here for the path".
            // The same dimming for an entity that is switched off (its own Active
            // box in the Details panel, or a parent's): it is in the scene file
            // but not in the game, and the row should look like that.
            const bool dimRow = show == OutlinerFilter::Show::Context ||
                                !HE::isEntityActive(ctx.world->registry(), node.entity);
            bool pushedText = false;
            if (lock && !lockedByMe)
            {
                // Dim the row: it is not yours to edit right now.
                float rgb[3];
                ctx.collab->colorFor(lock->owner, rgb);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
                pushedText = true;
            }
            else if (dimRow)
            {
                // Not a hit itself — the path to one. Dimmed so the eye lands
                // on what was searched for, not on the folders around it. The
                // same dimming for an entity that is switched off (its own
                // Active box in the Details panel, or a parent's): it is in
                // the scene file but not in the game, and the row should look
                // like that.
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                pushedText = true;
            }

            // Every branch with something shown under it is open while the
            // filter runs (in the filtered ID namespace, see above): the
            // filter is what decides what is visible, not last week's folds.
            if (filterActive && hasChildren)
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);

            // ── Where the row is, and the lines behind it ─────────────────
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImRect rowRect(ImVec2(outlinerWindow->WorkRect.Min.x, cursor.y),
                                 ImVec2(outlinerWindow->WorkRect.Max.x, cursor.y + rowH));
            const bool rowHot = paneHovered && ImGui::IsMouseHoveringRect(rowRect.Min, rowRect.Max);
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            // Indent guides: one hairline per level above this row, through the
            // arrow of the ancestor it belongs to. The ones on the way to the
            // selection are in the accent.
            for (int level = 0; level < node.depth; ++level)
            {
                const float gx = std::floor(cursor.x - float(node.depth - level) * indentStep +
                                            ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() * 0.5f) + 0.5f;
                const Entity owner = static_cast<size_t>(level) < pathEnt.size() ? pathEnt[static_cast<size_t>(level)] : Entity(entt::null);
                const bool onSelection = owner != entt::null && selectedPath.count(owner) != 0;
                dl->AddLine(ImVec2(gx, rowRect.Min.y), ImVec2(gx, rowRect.Max.y + kRowGap),
                            onSelection ? ImGui::GetColorU32(HE::Ed::Theme::alpha(HE::Ed::Theme::Accent, 0.55f))
                                        : ImGui::GetColorU32(HE::Ed::Theme::alpha(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.10f)),
                            onSelection ? 1.5f : 1.0f);
            }

            // Both halves of "overlap": the tree-node FLAG only reaches the
            // row's own button behaviour, while the IsItemClicked() below
            // asks IsItemHovered(), which reads the ITEM flags — and those
            // are set by this call alone. Without it a click on the eye also
            // selected the row (found by tests/test_outliner_ui.cpp).
            //
            // The row is a label-less TreeNodeEx: its arrow, its click and its
            // fold are ImGui's, and the name, the type icon and the badges are
            // drawn over it below. The frame padding is what sets its height
            // and the item spacing the gap to the next row, both only for this
            // call — the menus opened from the row keep the theme's.
            ImGui::SetNextItemAllowOverlap();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, padY));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, kRowGap));
            bool open = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(
                    static_cast<uint32_t>(node.entity))),
                flags, "%s", "");
            ImGui::PopStyleVar(2);

            if (pushedText) ImGui::PopStyleColor();

            // Alt on the arrow opens (or closes) the whole branch under it, the
            // way a file manager does: the fold state of every descendant is set
            // in the same storage the rows read it from. Not under a filter,
            // where every branch is open already.
            if (ImGui::IsItemToggledOpen() && ImGui::GetIO().KeyAlt && !filterActive && node.hasChildren)
            {
                ImGuiStorage* storage = ImGui::GetStateStorage();
                const auto& reg = ctx.world->registry();
                std::function<void(Entity, ImGuiID)> setBranch = [&](Entity e, ImGuiID id)
                {
                    storage->SetInt(id, open ? 1 : 0);
                    if (const auto* hc = reg.try_get<HierarchyComponent>(e))
                        for (const Entity c : hc->children)
                        {
                            const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(c)));
                            setBranch(c, ImHashData(&ptr, sizeof(void*), id));
                        }
                };
                setBranch(node.entity, drawnId);
            }

            const float fs    = ImGui::GetFontSize();
            const float textX = cursor.x + fs + ImGui::GetStyle().FramePadding.x * 2.0f;   // where ImGui would start a label

            // Everything below that asks about "the item just submitted" —
            // the click, the drag, the context menu — runs NOW, while that
            // item is still the row. The badges and the eye/lock buttons are
            // drawn after all of it (further down), because they are items of
            // their own: a click test placed behind a "[Prefab]" badge would
            // be asking about the badge, and BeginPopupContextItem behind a
            // Text item has no id to hang the popup on.

            // Click (not on the arrow) → select. Ctrl (Cmd on macOS — ImGui
            // swaps the two under ConfigMacOSXBehaviors, so io.KeyCtrl is the
            // platform's multi-select key either way) toggles the row in and
            // out of the set; Shift extends from the anchor to this row; a
            // plain click replaces the set with this row and moves the anchor.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
            {
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyShift && ctx.selection.anchor() != entt::null)
                    shiftRangeTarget = node.entity;
                else if (io.KeyCtrl)
                    ctx.selection.toggle(node.entity);
                else
                    ctx.selection.set(node.entity);
            }

            // ── Drag & drop reparenting ───────────────────────────────────
            const bool isRoot = (node.entity == ctx.world->rootEntity());
            // A double-click on the name (not on the arrow) renames it in place.
            if (!isRoot && !ctx.isPlaying && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                ImGui::GetIO().MousePos.x >= textX - 2.0f)
                beginRename(node.entity, node.name);
            if (!isRoot && ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("HE_ENTITY", &node.entity, sizeof(Entity));
                ImGui::TextUnformatted(node.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                // Where on the row the drop lands: the top quarter puts the
                // entity before this one, the bottom quarter after it (an open
                // branch's bottom edge is its first child, i.e. "into"), the
                // middle makes it a child. The root only takes children.
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                        "HE_ENTITY", ImGuiDragDropFlags_AcceptBeforeDelivery |
                                     ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                {
                    Entity dragged{};
                    std::memcpy(&dragged, payload->Data, sizeof(Entity));
                    const float f = (ImGui::GetIO().MousePos.y - rowRect.Min.y) / rowH;
                    enum class Drop { Into, Before, After } mode = Drop::Into;
                    if (!isRoot)
                    {
                        if (f < 0.25f)                                mode = Drop::Before;
                        else if (f > 0.75f && !(open && hasChildren)) mode = Drop::After;
                    }
                    const bool legal = dragged != node.entity && ctx.world->registry().valid(dragged) &&
                                       !ctx.world->isBuiltin(dragged) &&
                                       !ctx.world->isAncestorOf(dragged, node.entity);
                    if (legal)
                    {
                        const ImU32 cue = ImGui::GetColorU32(HE::Ed::Theme::AccentBright);
                        if (mode == Drop::Into)
                        {
                            dl->AddRectFilled(rowRect.Min, rowRect.Max,
                                              ImGui::GetColorU32(HE::Ed::Theme::alpha(HE::Ed::Theme::Accent, 0.18f)), 4.0f);
                            dl->AddRect(rowRect.Min, rowRect.Max, cue, 4.0f, 0, 1.5f);
                        }
                        else
                        {
                            const float ly = mode == Drop::Before ? rowRect.Min.y : rowRect.Max.y;
                            const float lx = cursor.x + 8.0f;
                            dl->AddLine(ImVec2(lx, ly), ImVec2(rowRect.Max.x, ly), cue, 2.0f);
                            dl->AddCircleFilled(ImVec2(lx, ly), 3.5f, cue);
                        }
                    }
                    if (payload->IsDelivery() && legal)
                    {
                        if (ctx.undoSys)
                            ctx.undoSys->snapshotNow(mode == Drop::Into ? "Reparent Entity" : "Move Entity");
                        if (mode == Drop::Into)
                            ctx.world->reparentEntity(dragged, node.entity);
                        else
                            ctx.world->placeNextTo(dragged, node.entity, mode == Drop::After);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // ── Per-entity context menu ───────────────────────────────────
            if (ImGui::BeginPopupContextItem())
            {
                // Every row is looked up as "World Outliner/<its label>".
                HE::Ed::Help::Scope helpScope("World Outliner");
                // Right-clicking a row that is already part of the selection
                // keeps the selection: the menu then acts on all of it (Delete
                // takes the set). A row outside it becomes the selection, as a
                // left-click would.
                if (!ctx.selection.contains(node.entity))
                    ctx.selection.set(node.entity);
                if (ImGui::BeginMenu("Create Child"))
                {
                    Preset preset{};
                    if (drawCreateMenu(preset))
                    {
                        if (ctx.undoSys) ctx.undoSys->snapshotNow("Create Entity");
                        const Entity child = createPreset(*ctx.world, preset);
                        ctx.world->reparentEntity(child, node.entity);
                        ctx.selection.set(child);
                        ctx.world->markHierarchyDirty();
                    }
                    ImGui::EndMenu();
                }
                if (EditorWidgets::menuItem("Rename", "F2", false, !ctx.isPlaying))
                    beginRename(node.entity, node.name);
                // ── Sibling order ─────────────────────────────────────────
                // The order under a parent is authored data (it is the order
                // the file lists the children in), and until now the only way
                // to change it was to delete and recreate. Up/down move this
                // row one place among its siblings; the sort puts this row's
                // DIRECT children A→Z. Not while playing — same rule as every
                // other edit in this menu.
                {
                    int siblingIndex = -1, siblingCount = 0;
                    if (!isRoot)
                        if (const auto* hier = ctx.world->registry().try_get<HierarchyComponent>(node.entity);
                            hier && hier->parent != entt::null)
                            if (const auto* ph = ctx.world->registry().try_get<HierarchyComponent>(hier->parent))
                            {
                                const auto& ch = ph->children;
                                const auto it = std::find(ch.begin(), ch.end(), node.entity);
                                siblingCount = static_cast<int>(ch.size());
                                siblingIndex = it == ch.end() ? -1 : static_cast<int>(it - ch.begin());
                            }
                    ImGui::Separator();
                    const bool canUp   = !ctx.isPlaying && siblingIndex > 0;
                    const bool canDown = !ctx.isPlaying && siblingIndex >= 0 &&
                                         siblingIndex < siblingCount - 1;
                    if (EditorWidgets::menuItem("Move Up", nullptr, false, canUp))
                    {
                        if (ctx.undoSys) ctx.undoSys->snapshotNow();
                        ctx.world->moveChild(node.entity, -1);
                    }
                    if (EditorWidgets::menuItem("Move Down", nullptr, false, canDown))
                    {
                        if (ctx.undoSys) ctx.undoSys->snapshotNow();
                        ctx.world->moveChild(node.entity, +1);
                    }
                    if (EditorWidgets::menuItem("Sort Children by Name", nullptr, false,
                                                !ctx.isPlaying && node.hasChildren))
                    {
                        if (ctx.undoSys) ctx.undoSys->snapshotNow();
                        ctx.world->sortChildrenByName(node.entity);
                    }
                }
                // ── Lock / unlock the whole selection ─────────────────────
                // The padlock on the row does one entity; this does every
                // selected one at once, the way Delete does. The verb is
                // decided by THIS row: a locked row offers Unlock, and the
                // rest of the selection follows it either way.
                if (!isRoot)
                {
                    auto& reg = ctx.world->registry();
                    const bool thisLocked = reg.all_of<EditorLockComponent>(node.entity);
                    if (EditorWidgets::menuItem(thisLocked ? "Unlock" : "Lock", nullptr, false,
                                                !ctx.isPlaying))
                    {
                        if (ctx.undoSys) ctx.undoSys->snapshotNow();
                        for (const Entity e : ctx.selection.entities())
                        {
                            if (!reg.valid(e) || e == ctx.world->rootEntity()) continue;
                            if (thisLocked) reg.remove<EditorLockComponent>(e);
                            else            reg.emplace_or_replace<EditorLockComponent>(e);
                        }
                    }
                }
                // ── Duplicate / cut / copy / paste ────────────────────────
                // The SAME hooks the Edit menu and the keyboard use. They act on
                // the SELECTION, which is this row: opening this popup selects it
                // (top of the block), so the two can never disagree. Not while
                // playing — play runs on a copy of the world, thrown away on stop.
                {
                    const bool editable = !isRoot && !ctx.isPlaying;
                    ImGui::Separator();
                    const bool doDuplicate =
                        EditorWidgets::menuItem("Duplicate", EditorShortcuts::label("entity.duplicate").c_str(), false, editable);
                    EditorWidgets::helpForKey("outliner.duplicate");
                    if (doDuplicate && ctx.duplicateEntity)
                        ctx.duplicateEntity();
                    if (EditorWidgets::menuItem("Copy", EditorShortcuts::label("entity.copy").c_str(), false, editable) && ctx.copyEntity)
                        ctx.copyEntity();
                    if (EditorWidgets::menuItem("Cut", EditorShortcuts::label("entity.cut").c_str(), false, editable) && ctx.cutEntity)
                        ctx.cutEntity();
                    // Paste needs no row of its own to be meaningful — it lands
                    // beside this one, under the same parent.
                    if (EditorWidgets::menuItem("Paste", EditorShortcuts::label("entity.paste").c_str(), false,
                                        ctx.entityClipboardFull && !ctx.isPlaying) &&
                        ctx.pasteEntity)
                        ctx.pasteEntity();
                    ImGui::Separator();
                }
                const bool doPrefab = !isRoot && EditorWidgets::menuItem("Save as Prefab");
                if (!isRoot) EditorWidgets::helpForKey("outliner.prefab");
                if (doPrefab && ctx.contentManager)
                    savePrefabOf(ctx, node.entity, node.name);
                const bool doDelete = !isRoot && EditorWidgets::dangerMenuItem("Delete");
                if (!isRoot) EditorWidgets::helpForKey("outliner.delete");
                if (doDelete)
                {
                    // Through the shared gesture when it is bound: that one
                    // deletes the WHOLE selection (this row is part of it, see
                    // the top of the popup) under one undo step. The fallback
                    // is the single-row delete for a context without it.
                    if (ctx.deleteEntity)
                        ctx.deleteEntity();
                    else
                    {
                        ctx.selection.remove(node.entity);
                        if (ctx.undoSys) ctx.undoSys->snapshotNow();
                        ctx.world->destroyEntity(node.entity);
                    }
                }
                ImGui::EndPopup();
            }

            // ── Type icon, name, badges, eye and padlock ──────────────────
            // Drawn over the row ImGui laid out, at offsets from its rectangle,
            // after all of its own interaction (see the note above the click
            // handling): left to right, the arrow (ImGui's), the type icon, the
            // name with a search hit marked, a folded row's child count, the
            // badges, and fixed at the right edge the eye and the padlock.
            {
                const auto& regC = ctx.world->registry();
                const bool  selected = (flags & ImGuiTreeNodeFlags_Selected) != 0;
                const HE::Visibility vis = isRootRow ? HE::Visibility::None
                                                     : HE::subtreeVisibility(regC, node.entity);
                // A hidden entity is in the scene but not on screen: its row
                // fades, and the slashed eye stays to say why.
                const float fade = vis == HE::Visibility::Hidden ? 0.50f : 1.0f;
                const float top  = rowRect.Min.y;

                if (selected)
                    dl->AddRectFilled(ImVec2(rowRect.Min.x, top + 3.0f), ImVec2(rowRect.Min.x + 3.0f, top + rowH - 3.0f),
                                      ImGui::GetColorU32(HE::Ed::Theme::Accent), 2.0f);

                float x = textX;
                if (!isRootRow)
                {
                    const float iconS = rowH - 8.0f;
                    drawTypeIcon(dl, ImVec2(x, std::floor(top + (rowH - iconS) * 0.5f)), iconS,
                                 OutlinerFilter::primaryKind(regC, node.entity),
                                 fade * (dimRow ? 0.55f : 1.0f));
                    x += iconS + 6.0f;
                }

                // Room for the name and what follows it: up to the eye.
                const float btn    = std::floor(rowH * 0.6f);
                const float limitX = isRootRow ? rowRect.Max.x - 8.0f
                                               : rowRect.Max.x - kRowPadRight - btn * 2.0f - 2.0f - 6.0f;
                const float ty     = std::floor(top + padY);

                ImVec4 nameCol = isRootRow ? HE::Ed::Theme::TextHeading : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                if (lock && !lockedByMe)
                {
                    float rgb[3];
                    ctx.collab->colorFor(lock->owner, rgb);
                    nameCol = ImVec4(rgb[0], rgb[1], rgb[2], 1.0f);
                }
                else if (dimRow)
                    nameCol = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                nameCol.w *= fade;

                if (s_renameEntity == node.entity)
                {
                    // ── The name as a text box ────────────────────────────
                    // On the row's own line (SameLine, the way the old eye and
                    // padlock sat there), shorter than the row so the line keeps
                    // its height. Enter or a click elsewhere keeps the name, Esc
                    // drops it, an empty name changes nothing.
                    const ImGuiStyle& st = ImGui::GetStyle();
                    ImGui::SameLine(x - (outlinerWindow->Pos.x - outlinerWindow->Scroll.x +
                                         outlinerWindow->DC.GroupOffset.x + outlinerWindow->DC.ColumnsOffset.x), 0.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, 2.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, kRowGap));
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowH - (fs + 4.0f)) * 0.5f));
                    ImGui::SetNextItemWidth(std::max(48.0f, limitX - x));
                    if (s_renameFocus) { ImGui::SetKeyboardFocusHere(); s_renameFocus = false; }
                    const bool enter = ImGui::InputText("##inline_rename", &s_renameBuf,
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsItemActive()) s_renameActive = true;
                    EditorWidgets::helpForKey("outliner.rename");
                    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                    const bool away   = !enter && !cancel && s_renameActive && ImGui::IsItemDeactivated();
                    ImGui::PopStyleVar(2);
                    if (cancel)
                        s_renameEntity = entt::null;
                    else if (enter || away)
                    {
                        if (!s_renameBuf.empty() && s_renameBuf != node.name)
                        {
                            if (ctx.undoSys) ctx.undoSys->snapshotNow("Rename Entity");
                            ctx.world->renameEntity(node.entity, s_renameBuf);
                        }
                        s_renameEntity = entt::null;
                    }
                }
                else
                {
                    // ── Badges: pills after the name ──────────────────────
                    // Placed prefabs: the root of a placement wears the asset's
                    // name, filled when something on the placement was changed
                    // here — that is the row a reader looks at to know whether
                    // the thing under it is still what the prefab says. Read from
                    // the registry every frame rather than the hierarchy cache
                    // above: an override comes and goes without the hierarchy
                    // ever being dirty.
                    struct Badge { std::string text; ImU32 fill, edge, ink; float w; };
                    Badge badges[3];
                    int   nb = 0;
                    const PrefabAsset* prefabAsset = nullptr;
                    size_t prefabChanged = 0;
                    int prefabBadge = -1, addedBadge = -1, lockBadge = -1;
                    const ImVec4 accent = HE::Ed::Theme::Accent;
                    if (const auto* inst = regC.try_get<PrefabInstanceComponent>(node.entity))
                    {
                        prefabAsset = ctx.contentManager ? ctx.contentManager->getPrefab(inst->asset) : nullptr;
                        prefabChanged = inst->overrides.size() + node.structuralChanges;
                        Badge& b = badges[nb];
                        b.text = prefabAsset && !prefabAsset->name.empty() ? prefabAsset->name : "Prefab";
                        b.fill = ImGui::GetColorU32(HE::Ed::Theme::alpha(accent, prefabChanged ? 0.80f : 0.14f));
                        b.edge = prefabChanged ? 0u : ImGui::GetColorU32(HE::Ed::Theme::alpha(accent, 0.55f));
                        b.ink  = prefabChanged ? ImGui::GetColorU32(ImVec4(0.10f, 0.08f, 0.05f, 1.0f))
                                               : ImGui::GetColorU32(HE::Ed::Theme::AccentHi);
                        prefabBadge = nb++;
                    }
                    else if (node.addedHere)
                    {
                        // The top of something added to a placement here: the
                        // prefab knows nothing of it, and a push would write it in.
                        Badge& b = badges[nb];
                        b.text = "+";
                        b.fill = ImGui::GetColorU32(HE::Ed::Theme::alpha(accent, 0.10f));
                        b.edge = ImGui::GetColorU32(HE::Ed::Theme::alpha(accent, 0.55f));
                        b.ink  = ImGui::GetColorU32(HE::Ed::Theme::AccentHi);
                        addedBadge = nb++;
                    }
                    if (lock)
                    {
                        Badge& b = badges[nb];
                        if (lockedByMe)
                        {
                            b.text = "you";
                            b.fill = ImGui::GetColorU32(HE::Ed::Theme::alpha(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.08f));
                            b.edge = 0u;
                            b.ink  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                        }
                        else
                        {
                            float rgb[3];
                            ctx.collab->colorFor(lock->owner, rgb);
                            const ImVec4 oc(rgb[0], rgb[1], rgb[2], 1.0f);
                            b.text = lock->ownerName;
                            b.fill = withAlpha(oc, 0.22f);
                            b.edge = withAlpha(oc, 0.70f);
                            b.ink  = withAlpha(oc, 1.0f);
                        }
                        lockBadge = nb++;
                    }
                    const float chipH = rowH - 9.0f;
                    for (int i = 0; i < nb; ++i) badges[i].w = chipWidth(badges[i].text.c_str());

                    // A folded row says how many rows are inside it.
                    char count[16] = {};
                    float countW = 0.0f;
                    if (open == false && node.childCount > 0 && !filterActive)
                    {
                        std::snprintf(count, sizeof(count), "(%d)", node.childCount);
                        countW = std::ceil(ImGui::CalcTextSize(count).x) + 6.0f;
                    }
                    const auto badgesW = [&](int n) { float w = 0.0f; for (int i = 0; i < n; ++i) w += 4.0f + badges[i].w; return w; };

                    const float room  = std::max(0.0f, limitX - x);
                    const float fullW = std::ceil(ImGui::CalcTextSize(node.name.c_str()).x);
                    // What follows the name is dropped, the last badge first and
                    // then the count, until the name keeps at least a stub; the
                    // name takes whatever is left.
                    const float stub = std::min(fullW, 56.0f);
                    while (nb > 0 && stub + countW + badgesW(nb) > room) --nb;
                    if (stub + countW + badgesW(nb) > room) countW = 0.0f;
                    const float nameW = std::min(fullW, std::max(0.0f, room - countW - badgesW(nb)));

                    if (nameW > 1.0f)
                    {
                        dl->PushClipRect(ImVec2(x, top), ImVec2(x + nameW, top + rowH), true);
                        // The search hit, marked where it sits in the name.
                        if (show == OutlinerFilter::Show::Hit && !searchNeedle.empty())
                        {
                            const OutlinerFilter::Span hit = OutlinerFilter::matchSpan(node.name, searchNeedle);
                            if (hit.pos != std::string_view::npos)
                            {
                                const char* n0 = node.name.c_str();
                                const float h0 = x + ImGui::CalcTextSize(n0, n0 + hit.pos).x;
                                const float h1 = h0 + ImGui::CalcTextSize(n0 + hit.pos, n0 + hit.pos + hit.len).x;
                                dl->AddRectFilled(ImVec2(h0 - 1.0f, top + 4.0f), ImVec2(h1 + 1.0f, top + rowH - 4.0f),
                                                  ImGui::GetColorU32(HE::Ed::Theme::alpha(accent, 0.38f)), 3.0f);
                            }
                        }
                        dl->PopClipRect();
                        ImGui::PushStyleColor(ImGuiCol_Text, nameCol);
                        ImGui::RenderTextEllipsis(dl, ImVec2(x, ty), ImVec2(x + nameW, ty + fs + 2.0f), x + nameW,
                                                  node.name.c_str(), nullptr, nullptr);
                        ImGui::PopStyleColor();
                    }
                    float bx = x + nameW;
                    if (countW > 0.0f)
                    {
                        dl->AddText(ImVec2(bx + 6.0f, ty),
                                    ImGui::GetColorU32(HE::Ed::Theme::alpha(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), fade)),
                                    count);
                        bx += countW;
                    }
                    for (int i = 0; i < nb; ++i)
                    {
                        bx += 4.0f;
                        const ImVec2 mn(bx, std::floor(top + (rowH - chipH) * 0.5f));
                        drawChip(dl, mn, badges[i].w, chipH, badges[i].text.c_str(), badges[i].fill, badges[i].edge, badges[i].ink);
                        if (rowHot && ImGui::IsMouseHoveringRect(mn, ImVec2(mn.x + badges[i].w, mn.y + chipH)))
                        {
                            if (i == prefabBadge)
                            {
                                if (!prefabAsset)
                                    ImGui::SetTooltip("Placed from a prefab that is not loaded.");
                                else if (prefabChanged)
                                    ImGui::SetTooltip("Placed from %s\n%zu change(s) made here — the prefab's "
                                                      "values do not reach those. See Prefab Instance in the "
                                                      "Details panel.",
                                                      prefabAsset->path.c_str(), prefabChanged);
                                else
                                    ImGui::SetTooltip("Placed from %s\nExactly what the prefab says.",
                                                      prefabAsset->path.c_str());
                            }
                            else if (i == addedBadge)
                                ImGui::SetTooltip("Added to a placed prefab here — not part of the prefab. "
                                                  "See Prefab Instance in the Details panel.");
                            else if (i == lockBadge)
                                ImGui::SetTooltip(lockedByMe ? "You are editing this." : "Someone else is editing this.");
                        }
                        bx += badges[i].w;
                    }
                }

                // ── Eye and padlock at the right edge ─────────────────────
                // The eye flips `visible` on every renderable in the subtree
                // (EntityVisibility.h — there is no entity-level flag, and none
                // inherited, so "hide the house" has to reach the doors); a row
                // with nothing to draw anywhere under it has no eye. The padlock
                // sets EditorLockComponent on this row: not clicked, not framed,
                // not moved in the viewport. Both are edits and take an undo
                // step; neither runs while playing (play runs on a copy, thrown
                // away on stop). The state is read from the registry every frame:
                // a `visible` flip never dirties the hierarchy cache.
                if (!isRootRow)
                    drawRowIcons(ctx, node.entity, vis, rowRect.Min, rowRect.Max.x, rowH, rowHot);
            }

            if (open)
            {
                // TreeNodeEx pushed this row's level itself.
                enterPath(node.depth, drawnId, node.entity);
                pushedDepth = node.depth;
            }
            else
                skipBelowDepth = node.depth; // don't enter children
        }
        // The clipped run at the bottom still counts towards the scroll range.
        if (skipped > 0.0f)
            skippedRun(skipped);
        // Close remaining open levels
        while (pushedDepth >= 0)
        {
            ImGui::TreePop();
            --pushedDepth;
        }
        if (filterActive) ImGui::PopID();

        // ── Shift-click range, now that the visible order is complete ─────
        // Anchor and target both have to be visible rows; if the anchor's
        // subtree was folded away since it was set, fall back to a plain
        // select of the clicked row rather than guessing a range.
        if (shiftRangeTarget != entt::null)
        {
            const auto anchorIt = std::find(visibleRows.begin(), visibleRows.end(),
                                            ctx.selection.anchor());
            const auto targetIt = std::find(visibleRows.begin(), visibleRows.end(),
                                            shiftRangeTarget);
            if (anchorIt == visibleRows.end() || targetIt == visibleRows.end())
                ctx.selection.set(shiftRangeTarget);
            else
            {
                const auto first = std::min(anchorIt, targetIt);
                const auto last  = std::max(anchorIt, targetIt);
                std::vector<Entity> range(first, last + 1);
                // Anchor first so it is the one the range grows from, target
                // last so it is the primary — the row just clicked.
                if (targetIt < anchorIt) std::reverse(range.begin(), range.end());
                ctx.selection.setMany(range);
            }
        }

        // ── Drop onto the empty area below the tree → un-parent to the World root ──
        // The root is not a normal drop target (it's a built-in), so dragging an entity
        // onto the outliner background is how you detach a child back to the top level.
        // A rect-based target (not a Dummy item) so it doesn't suppress the background
        // right-click "Create Entity" menu (which uses NoOpenOverItems).
        {
            const ImVec2 dropMin = ImGui::GetCursorScreenPos();
            const ImVec2 avail   = ImGui::GetContentRegionAvail();
            const ImRect dropBB(dropMin, ImVec2(dropMin.x + avail.x,
                                                dropMin.y + std::max(avail.y, 24.0f)));
            if (ImGui::BeginDragDropTargetCustom(dropBB, ImGui::GetID("##outliner_root_drop")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HE_ENTITY"))
                {
                    Entity dragged{};
                    std::memcpy(&dragged, payload->Data, sizeof(Entity));
                    if (ctx.undoSys) ctx.undoSys->snapshotNow("Reparent Entity");
                    ctx.world->reparentEntity(dragged, ctx.world->rootEntity());
                }
                ImGui::EndDragDropTarget();
            }
        }

        // ── Background context menu: create entity at root level ──────────
        if (ImGui::BeginPopupContextWindow("##outliner_bg_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            Preset preset{};
            if (drawCreateMenu(preset))
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow("Create Entity");
                ctx.selection.set(createPreset(*ctx.world, preset));
                ctx.world->markHierarchyDirty();
            }
            ImGui::EndPopup();
        }

    }
    else
    {
        ImGui::TextDisabled("(no world loaded)");
    }

    ImGui::End();
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

#ifdef HE_IMGUI_ENABLED
bool drawCreateEntityMenu(AppContext& ctx)
{
    Preset preset{};
    // The rows draw either way (a menu that vanishes with the world is a menu
    // that reads as broken); only the creation needs a world.
    if (!drawCreateMenu(preset) || !ctx.world) return false;
    if (ctx.undoSys) ctx.undoSys->snapshotNow("Create Entity");
    ctx.selection.set(createPreset(*ctx.world, preset));
    ctx.world->markHierarchyDirty();
    return true;
}

// One row per Preset, in the order drawCreateMenu draws them; the native
// macOS bar builds its Entity ▸ Create submenu from this table.
static const struct { EntityPresetRow row; Preset preset; } kPresetTable[] = {
    { { "Empty",          ""       }, Preset::Empty             },
    { { "Cube",           ""       }, Preset::Cube              },
    { { "Third Person",   "Camera" }, Preset::CameraThirdPerson },
    { { "First Person",   "Camera" }, Preset::CameraFirstPerson },
    { { "Plain (no rig)", "Camera" }, Preset::CameraPlain       },
    { { "Directional",    "Light"  }, Preset::LightDirectional  },
    { { "Point",          "Light"  }, Preset::LightPoint        },
    { { "Spot",           "Light"  }, Preset::LightSpot         },
    { { "Rope",           ""       }, Preset::Rope              },
    { { "Trail",          ""       }, Preset::Trail             },
};

const EntityPresetRow* entityPresetTable(int& outCount)
{
    static const EntityPresetRow* rows = [] {
        static EntityPresetRow r[std::size(kPresetTable)];
        for (std::size_t i = 0; i < std::size(kPresetTable); ++i) r[i] = kPresetTable[i].row;
        return r;
    }();
    outCount = static_cast<int>(std::size(kPresetTable));
    return rows;
}

void createEntityPreset(AppContext& ctx, int index)
{
    if (!ctx.world || index < 0 || index >= static_cast<int>(std::size(kPresetTable))) return;
    if (ctx.undoSys) ctx.undoSys->snapshotNow("Create Entity");
    ctx.selection.set(createPreset(*ctx.world, kPresetTable[index].preset));
    ctx.world->markHierarchyDirty();
}

void saveSelectionAsPrefab(AppContext& ctx)
{
    if (!ctx.world) return;
    auto& registry = ctx.world->registry();
    const Entity primary = ctx.selection.primary();
    if (primary == entt::null || !registry.valid(primary) || primary == ctx.world->rootEntity())
        return;
    const auto* name = registry.try_get<NameComponent>(primary);
    savePrefabOf(ctx, primary, name ? name->name : std::string{});
}
#endif // HE_IMGUI_ENABLED

} // namespace OutlinerPanel
