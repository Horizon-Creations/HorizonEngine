#include "EnvironmentPanel.h"
#include "EditorHelp.h"       // "Environment Window/<label>" scope for the tooltips
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

    // This window is 320 px wide by default and the user is free to make it
    // narrower; every line in it is a sentence, not a label — "No sky — the
    // background is flat", "(needs a Sky to drive — add one above)". Clipped at
    // the right edge those become half-sentences that read as if the panel were
    // broken, and the parenthetical is precisely the part that would be lost.
    // So: wrapped, panel-wide. The brace is what makes it safe — the guard has
    // to pop before the ImGui::End() below, and a PopTextWrapPos issued after
    // End() would act on whatever window is current by then, which for a
    // top-level panel is none at all.
    {
        EditorWidgets::WrapText wrap;
        // Not "Environment" — that scope is the Details panel's component of the
        // same name and already owns some sixty keys.
        HE::Ed::Help::Scope helpScope("Environment Window");

        ImGui::TextWrapped("Add or remove the scene's Sky and Weather. Select one to edit "
                           "its settings in the Details panel.");

        // ── Sky ──────────────────────────────────────────────────────────────
        ImGui::SeparatorText("Sky");
        if (const Entity sky = world.environmentEntity(); sky != entt::null)
        {
            ImGui::TextUnformatted("Sky is in the scene.");
            if (EditorWidgets::button("Select##sky")) ctx.selectedEntity = sky;
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
            if (EditorWidgets::button("Add Sky"))
            {
                snapshot();
                ctx.selectedEntity = world.addSky();
            }
        }

        // ── Weather ──────────────────────────────────────────────────────────
        ImGui::SeparatorText("Weather");
        if (const Entity weather = world.weatherEntity(); weather != entt::null)
        {
            ImGui::TextUnformatted("Weather is in the scene.");
            if (world.environmentEntity() == entt::null)
                ImGui::TextDisabled("(needs a Sky to drive — add one above)");
            if (EditorWidgets::button("Select##weather")) ctx.selectedEntity = weather;
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
            if (EditorWidgets::button("Add Weather"))
            {
                snapshot();
                ctx.selectedEntity = world.addWeather();
            }
        }
    }

    ImGui::End();
#else
    (void)ctx; (void)open;
#endif
}

} // namespace EnvironmentPanel
