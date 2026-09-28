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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>   // std::size
#include <string>
#include <system_error>
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

    // The two buttons at the right edge of one Outliner row. Called after
    // the row and its badges, on the same line; places itself at the edge.
    void drawRowIcons(AppContext& ctx, Entity entity)
    {
        auto& reg = ctx.world->registry();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float sz    = ImGui::GetTextLineHeight();
        const float total = sz * 2.0f + style.ItemInnerSpacing.x;

        ImGui::SameLine();
        // To the right edge, or as far right as the name leaves room for.
        const float slack = ImGui::GetContentRegionAvail().x - total;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, slack));
        // Down to the text's line, less a pixel: the glyphs' optical centre
        // sits above the line box's middle (descenders), and the icons are
        // drawn about their box's middle.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.FramePadding.y - 1.0f);

        ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool editable = !ctx.isPlaying;

        // ── Eye ──
        const HE::Visibility vis = HE::subtreeVisibility(reg, entity);
        if (vis == HE::Visibility::None)
        {
            // Nothing to draw anywhere below: keep the column, show no eye.
            ImGui::Dummy(ImVec2(sz, sz));
        }
        else
        {
            const bool shown = vis == HE::Visibility::Visible;
            ImGui::InvisibleButton("##eye", ImVec2(sz, sz));
            const bool hot = editable && ImGui::IsItemHovered();
            const ImVec4 base = shown ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                      : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            const ImU32 col = ImGui::GetColorU32(hot ? HE::Ed::Theme::AccentBright
                                                     : HE::Ed::Theme::alpha(base, shown ? 0.85f : 1.0f));
            drawEye(dl, ImGui::GetItemRectMin(), sz, col, shown);
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

        // ── Padlock ──
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        const bool locked = reg.all_of<EditorLockComponent>(entity);
        ImGui::InvisibleButton("##lock", ImVec2(sz, sz));
        {
            const bool hot = editable && ImGui::IsItemHovered();
            // Locked is loud, unlocked barely there: the row's normal state
            // should not wear a badge.
            const ImU32 col = ImGui::GetColorU32(
                hot    ? HE::Ed::Theme::AccentBright
              : locked ? HE::Ed::Theme::Accent
                       : HE::Ed::Theme::alpha(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), 0.45f));
            drawPadlock(dl, ImGui::GetItemRectMin(), sz, col, locked);
            EditorWidgets::helpForKey("outliner.lock");
            if (editable && ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow(locked ? "Unlock Entity" : "Lock Entity");
                if (locked) reg.remove<EditorLockComponent>(entity);
                else        reg.emplace_or_replace<EditorLockComponent>(entity);
            }
        }
        ImGui::PopID();
    }
}
#endif

void render(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
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

            // `underInstance`: some ancestor is a placement root; `parentAdded`:
            // the parent is itself added here, so this row is inside an added
            // subtree rather than the top of one.
            std::function<void(Entity, int, bool, bool)> collect =
                [&](Entity entity, int depth, bool underInstance, bool parentAdded)
            {
                if (!registry.valid(entity)) return;
                // The built-in environment sun/moon lights belong to the World's
                // Environment, and runtime terrain chunks are generated from the
                // TerrainComponent — hide both from the Outliner.
                if (entity != ctx.world->rootEntity() &&
                    (registry.all_of<EnvironmentLightComponent>(entity) ||
                     registry.all_of<TerrainChunkComponent>(entity)))
                    return;
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
                s_outlinerCache.push_back(std::move(node));
                if (hier)
                    for (Entity child : hier->children)
                        collect(child, depth + 1, underInstance || isInstance, added);
            };
            collect(root, 0, false, false);

            char buf[96];
            std::snprintf(buf, sizeof(buf), "[Outliner] rebuilt: %zu nodes", s_outlinerCache.size());
            HE_LOG_INFO(Editor, "%s", buf);

            ctx.world->clearHierarchyDirty();
        }

        // ── Entity rename popup state ─────────────────────────────────────
        static Entity s_renameEntity     = entt::null;
        static char   s_entityRenameBuf[256] = {};
        static bool   s_openEntityRename = false;

        // ── Search + type filter ──────────────────────────────────────────
        // The same header the Content Browser has, for the same reason: a
        // panel that can show a few hundred rows but not find one among them
        // is a panel people scroll instead of use. Typing narrows by name,
        // the dropdown by what the entity IS (see OutlinerFilter.h); both at
        // once means both. State is per editor, not per world — a search
        // survives a scene switch, which is what a search typed for "the
        // torch in every level" needs.
        static std::string s_searchText;
        static int         s_typeFilter = OutlinerFilter::kAllKinds;
        {
            const float comboW = 130.0f;
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

            ImGui::SetNextItemWidth(std::max(60.0f, avail - addW - comboW - clearW -
                                             ImGui::GetStyle().ItemSpacing.x * 3.0f));
            ImGui::InputTextWithHint("##outliner_search", "Search entities", &s_searchText);
            EditorWidgets::helpForKey("outliner.search");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(comboW);
            // Help only while the dropdown is closed: once it is open, the
            // last item is the popup's, not the button's (same idiom as the
            // Class picker in the Details panel).
            const bool typeOpen =
                ImGui::BeginCombo("##outliner_type", OutlinerFilter::kindAt(s_typeFilter).label);
            if (!typeOpen) EditorWidgets::helpForKey("outliner.type-filter");
            if (typeOpen)
            {
                for (int i = 0; i < OutlinerFilter::kindCount(); ++i)
                    if (ImGui::Selectable(OutlinerFilter::kindAt(i).label, i == s_typeFilter))
                        s_typeFilter = i;
                ImGui::EndCombo();
            }
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
            ImGui::Separator();
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
            if (hitCount == 0)
                ImGui::TextDisabled("Nothing matches. The search covers every entity "
                                    "in the scene, open or folded.");
        }

        // ── Render from cache ─────────────────────────────────────────────
        int prevDepth      = -1;
        int skipBelowDepth = INT_MAX; // skip children of closed nodes

        // While a filter is on, the tree's open/closed state lives in its own
        // ID namespace, and every branch on the way to a hit is forced open
        // there: a hit under a folded parent is a hit nobody sees. Doing it in
        // a separate namespace is what keeps the user's own folds — the state
        // under the unfiltered IDs — untouched, so clearing the search puts
        // the tree back exactly as it was.
        if (filterActive) ImGui::PushID("##outliner_filtered");

        // The rows that were actually DRAWN this frame, top to bottom. A
        // Shift-click selects "everything between the anchor and this row", and
        // "between" means in the order the user sees — closed subtrees are not
        // in it. Collected during the loop and resolved after it, because the
        // clicked row may lie above the anchor.
        std::vector<Entity> visibleRows;
        visibleRows.reserve(s_outlinerCache.size());
        Entity shiftRangeTarget = entt::null;

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

            // Close tree levels we've left
            while (prevDepth >= node.depth)
            {
                ImGui::TreePop();
                --prevDepth;
            }

            // AllowOverlap: the eye and the lock sit at the row's right edge,
            // ON the row (SpanAvailWidth stretches it under them), and this
            // is what lets them take the click instead of the row.
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanAvailWidth
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
            bool pushedText = false;
            if (lock && !lockedByMe)
            {
                // Dim the row: it is not yours to edit right now.
                float rgb[3];
                ctx.collab->colorFor(lock->owner, rgb);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
                pushedText = true;
            }
            else if (show == OutlinerFilter::Show::Context ||
                     !HE::isEntityActive(ctx.world->registry(), node.entity))
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

            // Both halves of "overlap": the tree-node FLAG only reaches the
            // row's own button behaviour, while the IsItemClicked() below
            // asks IsItemHovered(), which reads the ITEM flags — and those
            // are set by this call alone. Without it a click on the eye also
            // selected the row (found by tests/test_outliner_ui.cpp).
            ImGui::SetNextItemAllowOverlap();
            bool open = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(
                    static_cast<uint32_t>(node.entity))),
                flags, "%s", node.name.c_str());

            if (pushedText) ImGui::PopStyleColor();

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
                    if (ctx.undoSys) ctx.undoSys->snapshotNow("Reparent Entity");
                    ctx.world->reparentEntity(dragged, node.entity);
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
                if (EditorWidgets::menuItem("Rename"))
                {
                    s_renameEntity = node.entity;
                    std::strncpy(s_entityRenameBuf, node.name.c_str(), sizeof(s_entityRenameBuf) - 1);
                    s_entityRenameBuf[sizeof(s_entityRenameBuf) - 1] = '\0';
                    s_openEntityRename = true;
                }
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

            // ── Badges beside the name ────────────────────────────────────
            // Drawn after the row's own interaction (see the note above the
            // click handling); on screen they follow the name as before.
            //
            // Placed prefabs: the root of a placement wears the asset's name,
            // and a mark when something on the placement was changed here —
            // that is the row a reader looks at to know whether the thing
            // under it is still what the prefab says. Read from the registry
            // every frame rather than the hierarchy cache above: an override
            // comes and goes without the hierarchy ever being dirty.
            if (const auto* inst = ctx.world->registry().try_get<PrefabInstanceComponent>(node.entity))
            {
                const PrefabAsset* asset =
                    ctx.contentManager ? ctx.contentManager->getPrefab(inst->asset) : nullptr;
                const size_t changed = inst->overrides.size() + node.structuralChanges;
                ImGui::SameLine();
                ImGui::TextColored(changed ? HE::Ed::Theme::AccentBright
                                           : HE::Ed::Theme::alpha(HE::Ed::Theme::Accent, 0.75f),
                                   changed ? "[%s *]" : "[%s]",
                                   asset && !asset->name.empty() ? asset->name.c_str() : "Prefab");
                if (ImGui::IsItemHovered())
                {
                    if (!asset)
                        ImGui::SetTooltip("Placed from a prefab that is not loaded.");
                    else if (changed)
                        ImGui::SetTooltip("Placed from %s\n%zu change(s) made here — the prefab's "
                                          "values do not reach those. See Prefab Instance in the "
                                          "Details panel.",
                                          asset->path.c_str(), changed);
                    else
                        ImGui::SetTooltip("Placed from %s\nExactly what the prefab says.",
                                          asset->path.c_str());
                }
            }
            else if (node.addedHere)
            {
                // The top of something added to a placement here: the prefab
                // knows nothing of it, and a push would write it in.
                ImGui::SameLine();
                ImGui::TextColored(HE::Ed::Theme::alpha(HE::Ed::Theme::Accent, 0.75f), "[+]");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Added to a placed prefab here — not part of the prefab. "
                                      "See Prefab Instance in the Details panel.");
            }

            if (lock)
            {
                ImGui::SameLine();
                if (lockedByMe)
                {
                    ImGui::TextDisabled("[you]");
                }
                else
                {
                    float rgb[3];
                    ctx.collab->colorFor(lock->owner, rgb);
                    ImGui::TextColored(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f),
                                       "[%s]", lock->ownerName.c_str());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(lockedByMe ? "You are editing this."
                                                 : "Someone else is editing this.");
            }

            // ── Eye and padlock at the right edge ─────────────────────────
            // Two buttons per row, right-aligned, the way every outliner
            // since Unreal's draws them. The eye flips `visible` on every
            // renderable in the subtree (EntityVisibility.h — there is no
            // entity-level flag, and none inherited, so "hide the house"
            // has to reach the doors); a row with nothing to draw anywhere
            // under it has no eye. The padlock sets EditorLockComponent on
            // this row: not clicked, not framed, not moved in the viewport.
            // Both are edits and take an undo step; neither runs while
            // playing (play runs on a copy, thrown away on stop).
            // The state is read from the registry every frame: a `visible`
            // flip never dirties the hierarchy cache.
            if (!isRoot)
                drawRowIcons(ctx, node.entity);

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

        // ── Entity rename popup ────────────────────────────────────────────
        if (s_openEntityRename)
        {
            ImGui::OpenPopup("##entity_rename_popup");
            s_openEntityRename = false;
        }
        ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
        EditorWidgets::pinDialogToEditorWindow();
        if (ImGui::BeginPopupModal("##entity_rename_popup", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::TextUnformatted("Rename Entity");
            ImGui::Separator();
            ImGui::SetNextItemWidth(-1.0f);
            bool confirm = ImGui::InputText("##entity_rename_input",
                s_entityRenameBuf, sizeof(s_entityRenameBuf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (EditorWidgets::primaryButton("OK", ImVec2(140, 0)) || confirm)
            {
                if (s_entityRenameBuf[0] != '\0' &&
                    ctx.world->registry().valid(s_renameEntity))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow("Rename Entity");
                    ctx.world->renameEntity(s_renameEntity, s_entityRenameBuf);
                }
                s_renameEntity = entt::null;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (EditorWidgets::cancelButton("Cancel", ImVec2(140, 0)))
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
