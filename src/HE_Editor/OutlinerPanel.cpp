#include "OutlinerPanel.h"
#include "EditorApplication.h"           // AppContext, HorizonWorld, EditorUndo
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include <HorizonScene/HorizonScene.h>
#include <Diagnostics/Logger.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>   // ImRect + BeginDragDropTargetCustom for the root drop zone
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
            default: break;
        }
        return e;
    }

    // The menu body, shared by the background menu and the per-entity "create a
    // child" menu — one list, so the two can never drift apart.
    bool drawCreateMenu(Preset& out)
    {
        bool picked = false;
        auto item = [&](const char* label, Preset p)
        { if (ImGui::MenuItem(label)) { out = p; picked = true; } };

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
        return picked;
    }
}
#endif

void render(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    // World Outliner
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("World Outliner");
    if (ctx.fontHeading) ImGui::PopFont();

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
                // Environment, and runtime terrain chunks are generated from the
                // TerrainComponent — hide both from the Outliner.
                if (entity != ctx.world->rootEntity() &&
                    (registry.all_of<EnvironmentLightComponent>(entity) ||
                     registry.all_of<TerrainChunkComponent>(entity)))
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
            HE_LOG_INFO(Editor, "%s", buf);

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

            if (lock && !lockedByMe)
            {
                // Dim the row: it is not yours to edit right now.
                float rgb[3];
                ctx.collab->colorFor(lock->owner, rgb);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
            }

            bool open = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(
                    static_cast<uint32_t>(node.entity))),
                flags, "%s", node.name.c_str());

            if (lock && !lockedByMe) ImGui::PopStyleColor();

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
                if (ImGui::BeginMenu("Create Child"))
                {
                    Preset preset{};
                    if (drawCreateMenu(preset))
                    {
                        if (ctx.undoSys) ctx.undoSys->snapshotNow();
                        const Entity child = createPreset(*ctx.world, preset);
                        ctx.world->reparentEntity(child, node.entity);
                        ctx.selectedEntity = child;
                        ctx.world->markHierarchyDirty();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Rename"))
                {
                    s_renameEntity = node.entity;
                    std::strncpy(s_entityRenameBuf, node.name.c_str(), sizeof(s_entityRenameBuf) - 1);
                    s_entityRenameBuf[sizeof(s_entityRenameBuf) - 1] = '\0';
                    s_openEntityRename = true;
                }
                // ── Duplicate / cut / copy / paste ────────────────────────
                // The SAME hooks the Edit menu and the keyboard use. They act on
                // the SELECTION, which is this row: opening this popup selects it
                // (top of the block), so the two can never disagree. Not while
                // playing — play runs on a copy of the world, thrown away on stop.
                {
                    const bool editable = !isRoot && !ctx.isPlaying;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editable) &&
                        ctx.duplicateEntity)
                        ctx.duplicateEntity();
                    if (ImGui::MenuItem("Copy", "Ctrl+C", false, editable) && ctx.copyEntity)
                        ctx.copyEntity();
                    if (ImGui::MenuItem("Cut", "Ctrl+X", false, editable) && ctx.cutEntity)
                        ctx.cutEntity();
                    // Paste needs no row of its own to be meaningful — it lands
                    // beside this one, under the same parent.
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                                        ctx.entityClipboardFull && !ctx.isPlaying) &&
                        ctx.pasteEntity)
                        ctx.pasteEntity();
                    ImGui::Separator();
                }
                if (!isRoot && ImGui::MenuItem("Save as Prefab") && ctx.contentManager)
                {
                    // Entity names are free text, and a '/' in one would reach
                    // saveAsset's create_directories: "Arm/Left" would silently
                    // land in Content/Prefabs/Arm instead of where the user is
                    // looking for it.
                    std::string base = node.name;
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
                    prefab.data = ser.serializeSubtree(*ctx.world, node.entity);

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
                if (!isRoot && EditorWidgets::dangerMenuItem("Delete"))
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
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
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
                if (ctx.undoSys) ctx.undoSys->snapshotNow();
                ctx.selectedEntity = createPreset(*ctx.world, preset);
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
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
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

} // namespace OutlinerPanel
