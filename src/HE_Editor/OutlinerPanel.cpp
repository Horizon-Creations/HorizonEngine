#include "OutlinerPanel.h"
#include "EditorApplication.h"           // AppContext, HorizonWorld, EditorUndo
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include <HorizonScene/HorizonScene.h>
#include <Diagnostics/Logger.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>   // ImRect + BeginDragDropTargetCustom for the root drop zone
#endif

namespace OutlinerPanel
{

void render(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
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
                lock = ctx.collab->lockFor(static_cast<std::uint64_t>(
                    entt::to_integral(node.entity)));
                lockedByMe = lock && lock->owner == ctx.collab->localParticipant();
            }

            if (lock && !lockedByMe)
            {
                // Dim the row: it is not yours to edit right now.
                float rgb[3];
                CollabController::participantColor(lock->owner, rgb);
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
                    CollabController::participantColor(lock->owner, rgb);
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
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

} // namespace OutlinerPanel
