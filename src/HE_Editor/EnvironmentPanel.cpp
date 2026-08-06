#include "EnvironmentPanel.h"
#include "EditorWidgets.h"    // danger buttons for deletion
#include "EditorApplication.h"           // AppContext, HorizonWorld, EditorUndo
#include <HorizonScene/HorizonScene.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace EnvironmentPanel
{

// View ▸ Environment — add / remove the scene's Sky and Weather entities. The sky and
// weather *settings* are edited in the Details panel when the "Sky" / "Weather" entity
// is selected in the Outliner; this window only manages their presence (and offers a
// shortcut to select each). Sky = an EnvironmentComponent entity, Weather = a
// WeatherComponent entity; removing the Sky leaves a flat background.
void DrawEnvironmentWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
    if (!open) return;

    ImGui::SetNextWindowSize(ImVec2(320, 220), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Environment", &open)) { ImGui::End(); return; }

    if (!ctx.world)
    {
        ImGui::TextDisabled("(no world loaded)");
        ImGui::End();
        return;
    }
    HorizonWorld& world = *ctx.world;
    auto snapshot = [&]{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); };

    ImGui::TextWrapped("Add or remove the scene's Sky and Weather. Select one to edit "
                       "its settings in the Details panel.");

    // ── Sky ──────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Sky");
    if (const Entity sky = world.environmentEntity(); sky != entt::null)
    {
        ImGui::TextUnformatted("Sky is in the scene.");
        if (ImGui::Button("Select##sky")) ctx.selectedEntity = sky;
        ImGui::SameLine();
        if (EditorWidgets::dangerButton("Remove##sky"))
        {
            snapshot();
            if (ctx.selectedEntity == sky) ctx.selectedEntity = entt::null;
            world.removeSky();
        }
    }
    else
    {
        ImGui::TextDisabled("No sky — the background is flat.");
        if (ImGui::Button("Add Sky"))
        {
            snapshot();
            ctx.selectedEntity = world.addSky();
        }
    }

    // ── Weather ───────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Weather");
    if (const Entity weather = world.weatherEntity(); weather != entt::null)
    {
        ImGui::TextUnformatted("Weather is in the scene.");
        if (world.environmentEntity() == entt::null)
            ImGui::TextDisabled("(needs a Sky to drive — add one above)");
        if (ImGui::Button("Select##weather")) ctx.selectedEntity = weather;
        ImGui::SameLine();
        if (EditorWidgets::dangerButton("Remove##weather"))
        {
            snapshot();
            if (ctx.selectedEntity == weather) ctx.selectedEntity = entt::null;
            world.removeWeather();
        }
    }
    else
    {
        ImGui::TextDisabled("No weather system.");
        if (ImGui::Button("Add Weather"))
        {
            snapshot();
            ctx.selectedEntity = world.addWeather();
        }
    }

    ImGui::End();
#else
    (void)ctx; (void)open;
#endif
}

} // namespace EnvironmentPanel
