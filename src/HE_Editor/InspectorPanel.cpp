#include "InspectorPanel.h"
#include <cstdint>
#include "EditorApplication.h"           // AppContext, EditorUndo, panel plumbing
#include "EditorWidgets.h"               // shared Content-Browser asset drop slot
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/NavigationSystem.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/AnimationStateMachineSystem.h>
#include <Scripting/ScriptEngine.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <MaterialGraph/MaterialGraph.h> // HE::MatParamKind for the entity param editor
#include <Types/Enums.h>
#include <glm/gtc/type_ptr.hpp>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#endif

namespace InspectorPanel
{

// ─── Inspector (Details panel) ────────────────────────────────────────────────
void render(AppContext& ctx)
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

	// ── Collaboration: is someone else editing this? ─────────────────────────
	// Shown before any field, because the useful moment is *before* the user
	// starts typing into something their change would fight over.
	if (ctx.collab && ctx.collab->inSession())
	{
		const auto subject = static_cast<std::uint64_t>(
			entt::to_integral(ctx.selectedEntity));
		if (const HE::Net::LockInfo* lock = ctx.collab->lockFor(subject);
		    lock && lock->owner != ctx.collab->localParticipant())
		{
			float rgb[3];
			CollabController::participantColor(lock->owner, rgb);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
			ImGui::TextWrapped("%s is editing this entity — your changes would "
			                   "collide with theirs.",
			                   lock->ownerName.empty() ? "Someone else"
			                                           : lock->ownerName.c_str());
			ImGui::PopStyleColor();
			ImGui::Separator();
		}
	}

	auto&  registry = ctx.world->registry();
	Entity entity   = ctx.selectedEntity;

	// Pre-frame world state for undo. capturePre() serializes the WHOLE world, so it
	// must NOT run every frame — doing so dropped the editor to ~15 ms the instant any
	// entity was selected (the terrain's sculptHeights alone is 263k floats). An edit
	// can only START on a mouse press inside this panel, so capture the pre-state only
	// then; the widget's IsItemActivated (same frame) stashes it.
	if (ctx.undoSys
	    && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
	    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
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

	// ── Environment / Sky (the "Sky" scene entity's EnvironmentComponent) ────
	// Shown whenever the selected entity carries an EnvironmentComponent (the Sky
	// entity). Edited here so it persists with the scene; pushed to the renderer each
	// frame by EditorApplication::pushEnvironment. Add/remove the Sky entity itself
	// from the View ▸ Environment window.
	if (auto* env = registry.try_get<EnvironmentComponent>(entity))
	{
		if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Day-Night Cycle", &env->dayNightCycle); trackEdit();

			// Format the 0..1 time as a HH:MM clock shown inside the slider.
			int minutes = static_cast<int>(env->timeOfDay * 1440.0f) % 1440;
			if (minutes < 0) minutes += 1440;
			char clock[8];
			std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::SliderFloat("##timeofday", &env->timeOfDay, 0.0f, 1.0f, clock,
			                       ImGuiSliderFlags_NoRoundToFormat))
				env->dayNightCycle = true;
			trackEdit();
			ImGui::TextDisabled(env->dayNightCycle
				? "Drives the sun, sky & shadows."
				: "Move the slider to start a day-night cycle.");

			if (ImGui::Checkbox("Auto-Advance", &env->autoAdvance) && env->autoAdvance)
				env->dayNightCycle = true;
			trackEdit();
			// Day length is a property of the WORLD (how long a day lasts once time
			// runs), not of the switch that starts it — so it stays editable with
			// Auto-Advance off. Greying it out forced the user to enable the cycle
			// just to dial the length in, then switch it back off.
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cyclelen", &env->cycleSeconds, 5.0f, 600.0f,
			                   "Full day: %.0f s", ImGuiSliderFlags_Logarithmic); trackEdit();
			if (!env->autoAdvance)
				ImGui::TextDisabled("Takes effect once Auto-Advance is on.");

			if (ImGui::TreeNodeEx("Sun & Moon", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::ColorEdit3("Sun Color",  &env->sunColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SliderFloat("Sun Brightness",  &env->sunIntensity,  0.0f, 10.0f, "%.2f"); trackEdit();
			ImGui::ColorEdit3("Moon Color", &env->moonColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SliderFloat("Moon Brightness", &env->moonIntensity, 0.0f, 10.0f, "%.2f"); trackEdit();

			ImGui::SeparatorText("Moon Phase");
			{
				float mp = env->moonPhase;
				const char* nm = (mp < 0.03f || mp > 0.97f) ? "New Moon" :
				                 mp < 0.22f ? "Waxing Crescent" :
				                 mp < 0.28f ? "First Quarter" :
				                 mp < 0.47f ? "Waxing Gibbous" :
				                 mp < 0.53f ? "Full Moon" :
				                 mp < 0.72f ? "Waning Gibbous" :
				                 mp < 0.78f ? "Last Quarter" : "Waning Crescent";
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("##moonphase", &env->moonPhase, 0.0f, 1.0f, "Phase: %.3f")) trackEdit();
				ImGui::TextDisabled("%s", nm);
				if (ImGui::Checkbox("Auto Lunar Cycle", &env->moonPhaseAuto)) trackEdit();
				ImGui::SameLine(); ImGui::TextDisabled("(needs Auto-Advance)");
				ImGui::BeginDisabled(!env->moonPhaseAuto);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("##mooncycledays", &env->moonCycleDays, 1.0f, 60.0f, "Lunar cycle: %.1f days")) trackEdit();
				ImGui::EndDisabled();
			}

			ImGui::TreePop(); } // end Sun & Moon

			// These are always editable. A Weather preset (below) sets a whole set of
			// these values when applied / transitioning; otherwise they're yours to move.
			if (ImGui::TreeNodeEx("Clouds", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cloudcoverage", &env->cloudCoverage, 0.0f, 1.0f, "Coverage: %.2f"); trackEdit();
			ImGui::TextDisabled("Full overcast dims the sun & fills with ambient light.");
			// Cloud render mode (OpenGL backend): sky-dome (cheap, infinite — no parallax)
			// vs 3D volumetric (world-anchored — clouds parallax as you move through the
			// scene). 3D exposes a height slider to match the world's unit scale.
			{
				const char* cloudModes[] = { "Sky-dome (default)", "3D volumetric (parallax)" };
				int cmode = (env->cloudMode == 1) ? 1 : 0;
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##cloudmode", &cmode, cloudModes, 2)) { env->cloudMode = cmode; trackEdit(); }
				if (env->cloudMode == 1)
				{
					ImGui::SetNextItemWidth(-1.0f);
					ImGui::SliderFloat("##cloudheight", &env->cloudHeight, 20.0f, 2000.0f,
					                   "3D height: %.0f"); trackEdit();
					ImGui::TextDisabled("Lifts the cloud band higher in the sky (clear sky opens toward the\nhorizon); the clouds keep the same size & shape (OpenGL only).");
				}
			}
			// Cloud quality (performance): scales the raymarch step counts + sun
			// light-march. Drop to Low on integrated GPUs / Apple Silicon Air if the
			// clouds are costing frames. (Metal first; other backends follow.)
			{
				const char* cloudQ[] = { "Low (fastest)", "Medium", "High (best)" };
				int q = (env->cloudQuality < 0) ? 0 : (env->cloudQuality > 2 ? 2 : env->cloudQuality);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##cloudquality", &q, cloudQ, 3)) { env->cloudQuality = q; trackEdit(); }
				ImGui::TextDisabled("Lower = cheaper. Clouds are a top GPU cost; Low ~halves their step count.");
				if (ImGui::Checkbox("Low-res clouds (quarter-res pass)", &env->lowResClouds)) trackEdit();
				ImGui::TextDisabled("Raymarch clouds at 1/4 res + upsample. Big win in open-sky views.\nToggle + F9 to A/B the cost. (Metal first.)");
			}
			// Cloud appearance: tweak the look without re-rolling the pattern.
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##clouddensity", &env->cloudDensity, 0.2f, 2.5f, "Density: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cloudfluffy", &env->cloudFluffiness, 0.0f, 1.0f, "Fluffiness: %.2f"); trackEdit();
			ImGui::ColorEdit3("Cloud Tint", &env->cloudTint.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::TextDisabled("Density thickens, fluffiness breaks the bodies into puffy cauliflower lumps.");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##winddir", &env->windDirection, 0.0f, 360.0f, "Wind direction: %.0f\xc2\xb0"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##windspeed", &env->windSpeed, 0.0f, 4.0f, "Wind speed: %.2f"); trackEdit();

			ImGui::TreePop(); } // end Clouds

			if (ImGui::TreeNodeEx("Contrails & Cirrus")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##contrails", &env->contrailAmount, 0.0f, 1.0f, "Contrails: %.2f"); trackEdit();
			ImGui::TextDisabled("Scattered vapour-trail lines to fill a clear daytime sky; fade as clouds build.");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cirrus", &env->cirrusAmount, 0.0f, 1.0f, "Cirrus: %.2f"); trackEdit();
			ImGui::BeginDisabled(env->cirrusAmount <= 0.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cirrusseed", &env->cirrusSeed, 0.0f, 50.0f, "Cirrus seed: %.1f"); trackEdit();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Thin high wispy clouds. Intensity = cover, seed re-rolls the pattern (OpenGL).");

			ImGui::TreePop(); } // end Contrails & Cirrus

			if (ImGui::TreeNodeEx("Sun Effects")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##godrays", &env->godRays, 0.0f, 1.0f, "God rays: %.2f"); trackEdit();
			ImGui::TextDisabled("Warm crepuscular glow where sunlight breaks through gaps in the cloud cover. Needs broken cloud (Coverage > 0) and the sun up; off when overcast or clear.");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##lensflare", &env->lensFlare, 0.0f, 1.0f, "Lens flare: %.2f"); trackEdit();
			ImGui::TextDisabled("Camera lens flare for the sun: core, ghost discs and a halo along the sun\xe2\x86\x92screen-centre axis. Fades when the sun is off-screen, below the horizon, or occluded. A camera artifact.");

			ImGui::TreePop(); } // end Sun Effects

			if (ImGui::TreeNodeEx("Atmospheric Fog")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##fogdensity", &env->fogDensity, 0.0f, 0.15f, "Density: %.3f"); trackEdit();
			ImGui::BeginDisabled(env->fogDensity <= 0.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##fogheight", &env->fogHeightFalloff, 0.0f, 1.0f, "Ground hugging: %.2f"); trackEdit();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Distant objects blend into the horizon (warm at sunset).");

			ImGui::TreePop(); } // end Atmospheric Fog

			if (ImGui::TreeNodeEx("Precipitation & Ground")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##rain", &env->rainAmount, 0.0f, 1.0f, "Rain: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##snow", &env->snowAmount, 0.0f, 1.0f, "Snow: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##wetness", &env->wetness, 0.0f, 1.0f, "Wetness: %.2f"); trackEdit();
			ImGui::TextDisabled("Rain/snow spawn particles; wetness darkens & snow whitens the ground.");

			ImGui::TreePop(); } // end Precipitation & Ground

			if (ImGui::TreeNodeEx("Stars & Milky Way")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starbright", &env->starBrightness, 0.0f, 3.0f, "Star Brightness: %.2f"); trackEdit();
			ImGui::ColorEdit3("Star Color", &env->starColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##stardensity", &env->starDensity, 0.0f, 1.0f, "Star Amount: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starsize", &env->starSize, 0.3f, 2.5f, "Star Size: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starsizevar", &env->starSizeVariation, 0.0f, 1.0f, "Size Variation: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starglow", &env->starGlow, 0.0f, 3.0f, "Star Glow: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##startwinkle", &env->starTwinkle, 0.0f, 1.0f, "Twinkle: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##milkyway", &env->milkyWayIntensity, 0.0f, 1.0f, "Milky Way: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##shootingstars", &env->shootingStars, 0.0f, 1.0f, "Shooting Stars: %.2f"); trackEdit();
			ImGui::TextDisabled("Occasional meteors streak across the night sky; higher = more frequent. Night only.");
			ImGui::TextDisabled("Stars, Milky Way & nebula turn with the day-night cycle.");

			ImGui::TreePop(); } // end Stars & Milky Way

			if (ImGui::TreeNodeEx("Nebula")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##nebula", &env->nebulaIntensity, 0.0f, 1.0f, "Intensity: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##nebulacover", &env->nebulaCoverage, 0.0f, 1.0f, "Coverage: %.2f"); trackEdit();
			{
				// Combo index == nebulaQuality (0 Performance, 1 High, 2 Max).
				int nebQ = env->nebulaQuality < 0 ? 0 : (env->nebulaQuality > 2 ? 2 : env->nebulaQuality);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##nebulafidelity", &nebQ,
				    "High Performance (lighter)\0High Fidelity (detailed)\0Max Quality (most detail)\0"))
				{ env->nebulaQuality = nebQ; trackEdit(); }
				if (nebQ == 2)
					ImGui::TextDisabled("Extra filament octaves + crisper lines (night sky; pricier). Metal/OpenGL.");
			}
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##nebulaseed", &env->nebulaSeed, 0.0f, 50.0f, "Seed: %.1f"); trackEdit();
			ImGui::ColorEdit3("Nebula Color 1", &env->nebulaColor.x,  ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::ColorEdit3("Nebula Color 2", &env->nebulaColor2.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::ColorEdit3("Nebula Color 3", &env->nebulaColor3.x, ImGuiColorEditFlags_NoInputs); trackEdit();

			ImGui::TreePop(); } // end Nebula

			if (ImGui::TreeNodeEx("Aurora")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##aurora", &env->auroraIntensity, 0.0f, 1.0f, "Intensity: %.2f"); trackEdit();
			ImGui::ColorEdit3("Color (base)", &env->auroraColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::ColorEdit3("Color (top)",  &env->auroraColorTop.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::BeginDisabled(env->auroraIntensity <= 0.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##auroraheight", &env->auroraHeight, 0.0f, 1.0f, "Height: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##aurorafrag", &env->auroraFragmentation, 0.0f, 1.0f, "Fragmentation: %.2f"); trackEdit();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Night only — fades out as the sun rises. Height sets the band's");
			ImGui::TextDisabled("altitude, Fragmentation how much the curtain breaks up.");
			ImGui::TreePop(); } // end Aurora
		}
		ImGui::Separator();
	}

	// ── Weather (its own "Weather" scene entity; drives the Sky's clouds/fog/wind) ──
	if (registry.all_of<WeatherComponent>(entity))
	{
		if (auto* w = registry.try_get<WeatherComponent>(entity))
		{
			if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen))
			{
				const char* kinds[] = { "Clear","Cloudy","Overcast","Foggy","Rain","Storm","Snow" };
				int target = static_cast<int>(w->targetKind);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##weatherkind", &target, kinds, IM_ARRAYSIZE(kinds)))
				{
					w->targetKind = static_cast<WeatherKind>(target);
					trackEdit();
				}
				ImGui::SliderFloat("Intensity",  &w->intensity, 0.0f, 1.0f, "%.2f"); trackEdit();
				ImGui::SliderFloat("Transition", &w->transitionDuration, 0.0f, 30.0f, "%.1f s"); trackEdit();
				ImGui::Checkbox("Auto-Cycle", &w->autoCycle); trackEdit();
				ImGui::BeginDisabled(!w->autoCycle);
				ImGui::SliderFloat("Cycle Time", &w->cycleSeconds, 5.0f, 600.0f, "%.0f s",
				                   ImGuiSliderFlags_Logarithmic); trackEdit();
				ImGui::EndDisabled();
				ImGui::TextDisabled("Picking a preset sets clouds/fog/wind/precip; the sliders above\nstay editable, so you can nudge any value afterwards.");

				if (w->currentKind != w->targetKind)
					ImGui::Text("Transitioning %s -> %s",
					            kinds[static_cast<int>(w->currentKind)],
					            kinds[static_cast<int>(w->targetKind)]);
				else
					ImGui::Text("Current: %s", kinds[static_cast<int>(w->currentKind)]);
				ImGui::TextDisabled("Cloud %.2f  Fog %.3f  Wind %.2f  Precip %.2f",
				                    w->curCloudCoverage, w->curFogDensity,
				                    w->curWindSpeed, w->curPrecip);

				ImGui::SeparatorText("Precipitation");
				ImGui::DragInt("Max Rain", &w->maxRainParticles, 10.0f, 0, 20000); trackEdit();
				ImGui::DragInt("Max Snow", &w->maxSnowParticles, 10.0f, 0, 20000); trackEdit();
				ImGui::DragFloat("Ground Y", &w->groundLevel, 0.1f, -1000.0f, 1000.0f,
				                 "%.1f (fallback floor)"); trackEdit();
				ImGui::TextDisabled("Drops collide via physics in Play; else die at Ground Y.");

				// Thunder sound — drop an audio .hasset here (played on each strike).
				EditorWidgets::assetDropSlot(ctx, "Thunder", w->thunderSound,
					HE::AssetType::Audio, "thunder", "(none — drop audio)",
					/*rejectNoun=*/nullptr, /*showClear=*/true);

			}
			ImGui::Separator();
		}
	}

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
			// ── Mesh asset slot — drop a StaticMesh .hasset here ──────────────
			// This was a read-only label, so a Mesh component added from "Add
			// Component" could never be pointed at an asset: it kept a null UUID
			// and silently rendered the fallback cube — which reads as "my engine
			// cube didn't come back" after a save/reload. Same drop-target shape
			// as the Material slot further down.
			EditorWidgets::assetDropSlot(ctx, "Asset", m->meshAssetId,
				HE::AssetType::StaticMesh, "meshslot",
				"(none — drop a mesh here; renders fallback cube)", "static mesh",
				/*showClear=*/true);
			int lod = m->lodBias;
			if (ImGui::InputInt("LOD Bias", &lod))
				m->lodBias = static_cast<uint8_t>(std::clamp(lod, 0, 255));
			ImGui::Checkbox("Visible",         &m->visible); trackEdit();
			ImGui::Checkbox("Casts Shadow",    &m->castsShadow); trackEdit();
			ImGui::Checkbox("Receives Shadow", &m->receivesShadow); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<MeshComponent>(entity); }
	}

	// ── Skeletal Mesh ────────────────────────────────────────────────────────
	if (auto* sm = registry.try_get<SkeletalMeshComponent>(entity))
	{
		if (componentHeader("Skeletal Mesh", true, removed))
		{
			if (sm->meshAssetId == HE::UUID{})
				ImGui::TextDisabled("Asset: (none)");
			else if (ctx.contentManager)
			{
				const SkeletalMeshAsset* asset = ctx.contentManager->getSkeletalMesh(sm->meshAssetId);
				ImGui::Text("Asset: %s", asset ? asset->name.c_str() : "(not loaded)");
				if (asset)
					ImGui::Text("Joints: %d | Bone matrices: %d",
					    (int)asset->skeleton.size(), (int)sm->boneMatrices.size());
			}
			ImGui::Checkbox("Visible",         &sm->visible);        trackEdit();
			ImGui::Checkbox("Casts Shadow",    &sm->castsShadow);    trackEdit();
			ImGui::Checkbox("Receives Shadow", &sm->receivesShadow); trackEdit();

			// Drag-drop asset slot
			if (EditorWidgets::assetDropSlot(ctx, "Asset", sm->meshAssetId,
					HE::AssetType::SkeletalMesh, "smslot")
				== EditorWidgets::SlotAction::Assigned)
				sm->dirty = true;
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<SkeletalMeshComponent>(entity); }
	}

	// ── Animator ────────────────────────────────────────────────────────────
	if (auto* an = registry.try_get<AnimatorComponent>(entity))
	{
		if (componentHeader("Animator", true, removed))
		{
			// Clip asset slot
			EditorWidgets::assetDropSlot(ctx, "Clip", an->clipAssetId,
				HE::AssetType::AnimationClip, "animslot");
			const AnimationClipAsset* cur = (an->clipAssetId != HE::UUID{} && ctx.contentManager)
				? ctx.contentManager->getAnimationClip(an->clipAssetId) : nullptr;

			// Playback controls
			ImGui::DragFloat("Speed##an",    &an->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##an",     &an->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##an",   &an->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##an",   &an->playing); trackEdit();

			if (cur)
				ImGui::Text("Duration: %.3f s  |  Channels: %d",
					cur->duration, (int)cur->channels.size());
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorComponent>(entity); }
	}

	// ── Animator Blend ───────────────────────────────────────────────────────
	if (auto* ab = registry.try_get<AnimatorBlendComponent>(entity))
	{
		if (componentHeader("Animator Blend", true, removed))
		{
			auto clipSlot = [&](const char* label, HE::UUID& slotId)
			{
				EditorWidgets::assetDropSlot(ctx, label, slotId,
					HE::AssetType::AnimationClip, /*idSuffix=*/label);
			};

			clipSlot("Clip A", ab->clipAId);
			clipSlot("Clip B", ab->clipBId);
			ImGui::SliderFloat("Blend##ab",  &ab->blendAlpha,    0.0f, 1.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Speed##ab",    &ab->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##ab",     &ab->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##ab",   &ab->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##ab",   &ab->playing); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorBlendComponent>(entity); }
	}

	// ── Animator State Machine ──────────────────────────────────────────────
	// The graph (states/transitions/default params) lives in an
	// AnimatorStateMachineAsset (authored in the Animator State Machine Editor
	// tab, opened by double-clicking the .hasset in the Content Browser — same
	// "asset instead of inline fields" move Material/ParticleSystem made) — this
	// section is just the asset slot + per-instance runtime state.
	if (auto* asm_ = registry.try_get<AnimatorStateMachineComponent>(entity))
	{
		if (componentHeader("Animator State Machine", true, removed))
		{
			// Both assigning and clearing re-resolve the state machine's config.
			// Slot id "asmslot", not "smslot": the Skeletal Mesh section above uses
			// that one, and an entity with both components would put two items with
			// the same ImGui id in this window as soon as their labels matched.
			if (EditorWidgets::assetDropSlot(ctx, "Asset", asm_->stateMachineAssetId,
					HE::AssetType::AnimatorStateMachine, "asmslot",
					"(none — drop a state machine here)", "animator state machine",
					/*showClear=*/true) != EditorWidgets::SlotAction::None)
				AnimationStateMachineSystem::markConfigDirty(*asm_);

			ImGui::LabelText("Current##sm", "%s",
				asm_->currentStateName.empty() ? "(none)" : asm_->currentStateName.c_str());
			ImGui::DragFloat("Speed##sm", &asm_->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			if (asm_->inTransition)
			{
				ImGui::LabelText("-> ##sm", "%s", asm_->transitionTarget.c_str());
				const float pct = asm_->transitionDuration > 0.0f
					? asm_->transitionElapsed / asm_->transitionDuration : 0.0f;
				ImGui::ProgressBar(std::min(pct, 1.0f), ImVec2(-1, 0), "crossfade");
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorStateMachineComponent>(entity); }
	}

	// ── Property Animator ───────────────────────────────────────────────────
	if (auto* pa = registry.try_get<PropertyAnimatorComponent>(entity))
	{
		if (componentHeader("Property Animator", true, removed))
		{
			// Clip drag-drop slot
			EditorWidgets::assetDropSlot(ctx, "Clip", pa->clipId,
				HE::AssetType::PropertyAnimClip, "pac");
			const PropertyAnimClipAsset* cur = (pa->clipId != HE::UUID{} && ctx.contentManager)
				? ctx.contentManager->getPropertyAnimClip(pa->clipId) : nullptr;

			ImGui::DragFloat("Speed##pa",  &pa->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##pa",   &pa->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##pa", &pa->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##pa", &pa->playing); trackEdit();

			if (cur)
			{
				ImGui::Separator();
				ImGui::Text("Duration: %.2f s | Channels: %zu", cur->duration, cur->channels.size());
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<PropertyAnimatorComponent>(entity); }
	}

	// ── NavMesh ─────────────────────────────────────────────────────────────
	if (auto* nmc = registry.try_get<NavMeshComponent>(entity))
	{
		if (componentHeader("Nav Mesh", true, removed))
		{
			ImGui::DragFloat("Cell Size##nm",       &nmc->config.cellSize,      0.01f, 0.05f, 2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Cell Height##nm",     &nmc->config.cellHeight,    0.01f, 0.05f, 2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Height##nm",     &nmc->config.walkableHeight,0.1f,  0.5f,  5.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Climb##nm",      &nmc->config.walkableClimb, 0.1f,  0.0f,  2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Radius##nm",     &nmc->config.walkableRadius,0.05f, 0.0f,  2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Max Slope##nm",       &nmc->config.maxSlope,      1.0f,  0.0f,  90.0f,  "%.1f°"); trackEdit();
			ImGui::Separator();
			ImGui::Text("Geometry: %zu verts  %zu tris",
				nmc->geometry.verts.size() / 3,
				nmc->geometry.tris.size()  / 3);
			const bool baked = (bool)nmc->navMesh;
			ImGui::Text("NavMesh: %s", baked ? "baked" : "not baked");
			if (ImGui::Button("Bake##nm"))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				NavigationSystem::bake(*nmc);
			}
			ImGui::SameLine();
			ImGui::Checkbox("Show NavMesh##nm", &nmc->showDebugMesh);
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<NavMeshComponent>(entity); }
	}

	// ── NavAgent ────────────────────────────────────────────────────────────
	if (auto* na = registry.try_get<NavAgentComponent>(entity))
	{
		if (componentHeader("Nav Agent", true, removed))
		{
			ImGui::DragFloat3("Target##na",     glm::value_ptr(na->targetPos), 0.1f); trackEdit();
			ImGui::DragFloat("Speed##na",       &na->speed,        0.1f, 0.0f, 20.0f, "%.1f m/s"); trackEdit();
			ImGui::DragFloat("Stop Dist##na",   &na->stoppingDist, 0.01f,0.0f, 2.0f,  "%.2f m"); trackEdit();
			ImGui::Separator();
			ImGui::Text("Path: %zu pts  idx=%zu  %s",
				na->path.size(), na->pathIdx,
				na->moving ? "MOVING" : "stopped");
			if (ImGui::Button("Go##na"))
			{ na->hasPath = false; na->moving = true; }
			ImGui::SameLine();
			if (ImGui::Button("Stop##na"))
			{ na->moving = false; na->hasPath = false; }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<NavAgentComponent>(entity); }
	}

	// ── Material ────────────────────────────────────────────────────────────
	if (auto* m = registry.try_get<MaterialComponent>(entity))
	{
		if (componentHeader("Material", true, removed))
		{
			// ── Material asset slot — drop a material .hasset here ────────────
			// Assigning also drops the renderer's cached pipeline for the NEW
			// material; clearing only needs the component re-resolved.
			if (const EditorWidgets::SlotAction act = EditorWidgets::assetDropSlot(
					ctx, "Asset", m->materialAssetId, HE::AssetType::Material, "matslot",
					"(none — drop a material here)", "material", /*showClear=*/true);
				act != EditorWidgets::SlotAction::None)
			{
				m->dirty = true;
				if (act == EditorWidgets::SlotAction::Assigned && ctx.renderer)
					ctx.renderer->InvalidateMaterial(m->materialAssetId);
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
				// Opacity < 1 routes the object into the sorted, alpha-blended
				// transparency pass.
				ImGui::SliderFloat("Opacity",   &mat->opacity,   0.0f, 1.0f, "%.2f");

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
					// Path-valued slot (the target is a string, not a UUID), so only the
					// drop RESOLUTION is shared — assetDropSlot draws a UUID slot and does
					// not fit here. Going through acceptAssetDrop also adds the asset-type
					// check this copy never had: it used to write ANY dropped asset path
					// (a mesh, an audio clip, a scene) into a texture slot, silently.
					if (const EditorWidgets::AssetDrop drop =
							EditorWidgets::acceptAssetDrop(ctx, HE::AssetType::Texture, "texture"))
					{
						mat->texturePaths[i] = drop.relPath;
						if (ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
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

				// ── Node-graph parameters (per-ENTITY override) ──────────────────
				// Live-tweak this entity's exposed material parameters without opening
				// the material editor. Values write to MaterialComponent::paramOverrides
				// (this entity only) — the shared material asset is untouched. The
				// extractor merges them onto the material's defaults each frame.
				if (!mat->graphParamNames.empty())
				{
					ImGui::SeparatorText("Material Parameters (this entity)");
					// Index of the override for `nm`, or -1. Index-based (never a pointer
					// into the vector) so a push_back/erase this frame can't dangle.
					auto overrideIndex = [&](const std::string& nm) -> int {
						for (size_t j = 0; j < m->paramOverrides.size(); ++j)
							if (m->paramOverrides[j].name == nm) return (int)j;
						return -1;
					};
					int resetIndex = -1; // deferred erase until after the loop
					for (size_t i = 0; i < mat->graphParamNames.size(); ++i)
					{
						const std::string nm = mat->graphParamNames[i]; // copy — vector may move
						const HE::MatParamKind kind = (i < mat->graphParamTypes.size())
							? static_cast<HE::MatParamKind>(mat->graphParamTypes[i])
							: HE::MatParamKind::Float;
						const int ovi = overrideIndex(nm);
						// Working value: the override if present, else the material default.
						float val[4] = { 0, 0, 0, 0 };
						if (ovi >= 0) { for (int k = 0; k < 4; ++k) val[k] = m->paramOverrides[ovi].value[k]; }
						else if (i * 4 + 3 < mat->shaderParamData.size())
							for (int k = 0; k < 4; ++k) val[k] = mat->shaderParamData[i * 4 + k];

						ImGui::PushID(static_cast<int>(i));
						bool edited = false;
						ImGui::SetNextItemWidth(-60.0f);
						const char* label = nm.empty() ? "param" : nm.c_str();
						switch (kind)
						{
							case HE::MatParamKind::Color:
								edited = ImGui::ColorEdit3(label, val, ImGuiColorEditFlags_Float); break;
							case HE::MatParamKind::Vec2:
								edited = ImGui::DragFloat2(label, val, 0.01f); break;
							case HE::MatParamKind::Vec4:
								edited = ImGui::DragFloat4(label, val, 0.01f); break;
							case HE::MatParamKind::Bool:
							{
								bool b = val[0] > 0.5f;
								if (ImGui::Checkbox(label, &b)) { val[0] = b ? 1.0f : 0.0f; edited = true; }
								break;
							}
							default: // Float
								edited = ImGui::DragFloat(label, val, 0.01f); break;
						}
						if (edited)
						{
							int w = ovi;
							if (w < 0) { MaterialParamOverride ov; ov.name = nm; m->paramOverrides.push_back(ov); w = (int)m->paramOverrides.size() - 1; }
							for (int k = 0; k < 4; ++k) m->paramOverrides[w].value[k] = val[k];
							m->dirty = true;
						}
						if (ovi >= 0)
						{
							ImGui::SameLine();
							if (ImGui::SmallButton("Reset")) resetIndex = ovi;
						}
						ImGui::PopID();
					}
					if (resetIndex >= 0 && resetIndex < (int)m->paramOverrides.size())
					{
						m->paramOverrides.erase(m->paramOverrides.begin() + resetIndex);
						m->dirty = true;
					}
					if (!m->paramOverrides.empty())
						ImGui::TextDisabled("%zu override(s) on this entity", m->paramOverrides.size());
				}

				// Custom shader (fragment GLSL). Empty → built-in PBR. When set, the
				// renderer cross-compiles it (shared MaterialShaderLibrary) and draws this
				// material with its own pipeline. Edited in a separate buffer so the pipeline
				// isn't recompiled on every keystroke — applied on focus-loss / Apply, then
				// picked up live (the renderer re-resolves the shader each frame).
				ImGui::SeparatorText("Custom Shader (Fragment GLSL)");
				ImGui::TextDisabled("in vec3 vNormal (loc0), vColor (loc1)  ->  out vec4 oColor (loc0)");
				static std::string s_shaderEdit;
				static HE::UUID    s_shaderEditFor{};
				if (!(s_shaderEditFor == m->materialAssetId))
				{
					s_shaderEdit    = mat->customShaderFragGlsl;
					s_shaderEditFor = m->materialAssetId;
				}
				ImGui::InputTextMultiline("##customShader", &s_shaderEdit, ImVec2(-1.0f, 160.0f));
				bool applyShader = ImGui::IsItemDeactivatedAfterEdit();
				if (ImGui::Button("Apply Shader")) applyShader = true;
				ImGui::SameLine();
				if (ImGui::Button("Clear (use PBR)")) { s_shaderEdit.clear(); applyShader = true; }
				ImGui::SameLine();
				ImGui::TextDisabled(mat->customShaderFragGlsl.empty() ? "(built-in PBR)" : "(custom active)");
				if (applyShader) mat->customShaderFragGlsl = s_shaderEdit;

				ImGui::Spacing();
				// Built-in defaults (DefaultMaterial / DefaultTerrainMaterial) are
				// virtual "mem://" assets with no file behind them AND a fixed UUID
				// that initDefaultAssets() re-seeds from hardcoded values on every
				// start. Saving one in place therefore cannot survive a restart —
				// that is why a recoloured Landscape came back grey. So for those,
				// Save writes a NEW project material (fresh UUID + real path under
				// Content/Materials) and re-points this entity at it; from then on
				// it round-trips like any other asset.
				const bool isBuiltIn = mat->path.rfind("mem://", 0) == 0;
				if (ImGui::Button(isBuiltIn ? "Save as Project Material" : "Save Material"))
				{
					if (isBuiltIn)
					{
						MaterialAsset copy = *mat;
						copy.id = HE::UUID{};                  // saveAsset mints a fresh one
						// Unique "<Name>.hasset" under Content/Materials.
						std::string base = mat->name.empty() ? std::string("Material") : mat->name;
						if (base.rfind("Default", 0) == 0) base = base.substr(7); // DefaultTerrainMaterial → TerrainMaterial
						const std::string dir = ctx.contentManager->contentRoot() + "/Materials";
						std::error_code mkec; std::filesystem::create_directories(dir, mkec);
						std::string name = base;
						for (int n = 1; std::filesystem::exists(dir + "/" + name + ".hasset"); ++n)
							name = base + std::to_string(n);
						copy.name = name;
						copy.path = "Materials/" + name + ".hasset";

						const HE::UUID newId = ctx.contentManager->registerMaterial(std::move(copy));
						MaterialAsset* saved = ctx.contentManager->getMaterialMutable(newId);
						const bool ok = saved && ctx.contentManager->saveAsset(*saved);
						if (ok)
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							m->materialAssetId = newId;   // this entity now owns a real asset
							m->dirty = true;
							ctx.contentRefreshPending = true;
							if (ctx.renderer) ctx.renderer->InvalidateMaterial(newId);
						}
						Logger::Log(ok ? Logger::LogLevel::Info : Logger::LogLevel::Error,
							("Editor: " + std::string(ok ? "saved built-in material as project asset "
							                             : "failed to save built-in material as ")
							 + (saved ? saved->path : std::string())).c_str());
					}
					else
					{
						const bool ok = ctx.contentManager->saveAsset(*mat);
						if (ok && ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
						Logger::Log(ok ? Logger::LogLevel::Info : Logger::LogLevel::Error,
							("Editor: " + std::string(ok ? "saved" : "failed to save")
							 + " material '" + mat->name + "'").c_str());
					}
				}
				ImGui::SameLine();
				ImGui::TextDisabled(isBuiltIn
					? "(engine default — Save makes a project copy)"
					: "(edits apply live; Save writes to disk)");
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
			if (l->type != LightType::Directional)
			{
				ImGui::DragFloat("Cull Distance", &l->cullDistance, 0.5f, 0.0f, 100000.0f); trackEdit();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Deactivate this light beyond this camera distance (0 = never)");
			}
			ImGui::Checkbox("Visible##light",      &l->visible);     trackEdit();
			ImGui::Checkbox("Casts Shadow##light", &l->castsShadow); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<LightComponent>(entity); }
	}

	// ── Decal ───────────────────────────────────────────────────────────────
	if (auto* d = registry.try_get<DecalComponent>(entity))
	{
		if (componentHeader("Decal", true, removed))
		{
			ImGui::ColorEdit4("Color##decal", &d->color.x); trackEdit();
			EditorWidgets::assetDropSlot(ctx, "Texture", d->textureId, HE::AssetType::Texture,
			                             "decaltex", "(none — drop a texture here)", "texture", true);
			ImGui::TextDisabled("Projects along the entity's local Y through its scaled box.\n"
			                    "Renders in the Deferred path (Metal).");
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<DecalComponent>(entity); }
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

	// ── Collider ──────────────────────────────────────────────────────────────
	if (auto* col = registry.try_get<ColliderComponent>(entity))
	{
		if (componentHeader("Collider", true, removed))
		{
			static const char* kShapes[] = { "Box", "Sphere", "Capsule" };
			int shape = static_cast<int>(col->shape);
			if (ImGui::Combo("Shape", &shape, kShapes, 3))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				col->shape = static_cast<ColliderShape>(shape);
			}
			switch (col->shape)
			{
			case ColliderShape::Box:
				ImGui::DragFloat3("Half Extents", &col->halfExtents.x, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			case ColliderShape::Sphere:
				ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			case ColliderShape::Capsule:
				ImGui::DragFloat("Radius",       &col->radius, 0.01f, 0.001f, 100.0f); trackEdit();
				ImGui::DragFloat("Total Height", &col->height, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			}
			ImGui::Checkbox("Is Trigger", &col->isTrigger); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ColliderComponent>(entity); }
	}

	// ── Character Controller ──────────────────────────────────────────────────
	if (auto* cc = registry.try_get<CharacterControllerComponent>(entity))
	{
		if (componentHeader("Character Controller", true, removed))
		{
			ImGui::DragFloat("Slope Limit (deg)", &cc->slopeLimit, 0.5f, 1.0f, 90.0f); trackEdit();
			ImGui::DragFloat("Step Height (m)",   &cc->stepHeight, 0.01f, 0.0f, 2.0f); trackEdit();
			ImGui::DragFloat("Skin Width (m)",    &cc->skinWidth,  0.001f, 0.001f, 0.5f); trackEdit();
			ImGui::DragFloat("Mass (kg)",          &cc->mass,       0.5f, 1.0f, 500.0f); trackEdit();
			ImGui::DragFloat("Gravity (m/s²)",     &cc->gravity,    0.1f, 0.0f, 30.0f); trackEdit();
			ImGui::Separator();
			ImGui::BeginDisabled(true);
			ImGui::Checkbox("Is Grounded", &cc->isGrounded);
			float v[3] = { cc->velocity.x, cc->velocity.y, cc->velocity.z };
			ImGui::DragFloat3("Velocity", v);
			ImGui::EndDisabled();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<CharacterControllerComponent>(entity); }
	}

	// ── Script (Lua) ────────────────────────────────────────────────────────
	if (auto* s = registry.try_get<ScriptComponent>(entity))
	{
		if (componentHeader("Script", true, removed))
		{
			char buf[256];
			std::strncpy(buf, s->moduleName.c_str(), sizeof(buf) - 1);
			buf[sizeof(buf) - 1] = '\0';
			if (ImGui::InputText("Script Name", buf, sizeof(buf)))
			{
				s->moduleName = buf;
				trackEdit();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Logical name matching ScriptEngine::loadScript(name, source).\n"
				                  "Script must export onStart(self) and/or onUpdate(self, dt).");
			ImGui::Checkbox("Enabled", &s->enabled); trackEdit();

			// ── Declared properties (M.properties table) ──────────────────
			if (ctx.propScriptEngine && ctx.contentManager && !s->moduleName.empty())
			{
				const ScriptAsset* asset = nullptr;
				if (s->scriptAssetId != HE::UUID{})
					asset = ctx.contentManager->getScript(s->scriptAssetId);
				if (asset && !asset->sourceCode.empty())
				{
					if (!ctx.propScriptEngine->isScriptLoaded(s->moduleName))
						ctx.propScriptEngine->loadScript(s->moduleName, asset->sourceCode);
					auto defs = ctx.propScriptEngine->getScriptProperties(s->moduleName);
					if (!defs.empty())
					{
						ImGui::Separator();
						ImGui::TextDisabled("Properties");
						for (const auto& def : defs)
						{
							auto it = s->properties.find(def.name);
							if (it == s->properties.end())
							{
								s->properties[def.name] = def.defaultVal;
								it = s->properties.find(def.name);
							}
							ScriptPropValue& val = it->second;
							switch (val.type)
							{
							case ScriptPropType::Float:
								if (ImGui::DragFloat(def.name.c_str(), &val.f, 0.1f)) trackEdit();
								break;
							case ScriptPropType::Int:
								if (ImGui::DragInt(def.name.c_str(), &val.i)) trackEdit();
								break;
							case ScriptPropType::Bool:
								if (ImGui::Checkbox(def.name.c_str(), &val.b)) trackEdit();
								break;
							case ScriptPropType::String:
							{
								char sbuf[256];
								std::strncpy(sbuf, val.s.c_str(), sizeof(sbuf) - 1);
								sbuf[sizeof(sbuf) - 1] = '\0';
								if (ImGui::InputText(def.name.c_str(), sbuf, sizeof(sbuf)))
								{
									val.s = sbuf;
									trackEdit();
								}
								break;
							}
							}
						}
					}
				}
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ScriptComponent>(entity); }
	}

	// ── Terrain ─────────────────────────────────────────────────────────────
	if (auto* t = registry.try_get<TerrainComponent>(entity))
	{
		if (componentHeader("Terrain", true, removed))
		{
			bool changed = false;
			changed |= ImGui::DragFloat("Width (X)##tc",    &t->sizeX,      1.0f,  1.0f, 10000.0f, "%.1f m"); trackEdit();
			changed |= ImGui::DragFloat("Depth (Z)##tc",    &t->sizeZ,      1.0f,  1.0f, 10000.0f, "%.1f m"); trackEdit();
			int res = static_cast<int>(t->resolution);
			if (ImGui::SliderInt("Resolution##tc", &res, 2, 512)) { t->resolution = static_cast<uint32_t>(res); changed = true; }
			trackEdit();
			changed |= ImGui::DragFloat("Height Scale##tc", &t->heightScale, 0.5f,  0.0f, 1000.0f,  "%.1f m"); trackEdit();

			// The generated UVs run 0..uvTiling over the WHOLE landscape, so at 1
			// a texture is stretched across every metre of it. This is the knob
			// that makes a terrain texture tile instead of smear.
			changed |= ImGui::DragFloat("Texture Tiling##tc", &t->uvTiling, 0.25f, 0.01f, 4096.0f, "%.2f x"); trackEdit();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How often the material's texture repeats across the whole terrain.\n"
				                  "For a texture that should cover N metres, use Width / N.");
			ImGui::SameLine();
			if (ImGui::SmallButton("4 m##tcuv"))
				{ t->uvTiling = std::max(0.01f, t->sizeX / 4.0f); changed = true; trackEdit(); }
			// Authored LOD aggressiveness — now persisted with the scene.
			changed |= ImGui::DragFloat("LOD Distance##tc", &t->lodDistanceScale, 0.05f, 0.1f, 20.0f, "%.2f x"); trackEdit();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Higher = keep full detail farther from the camera.");

			// Noise is a one-time creation input: it is baked into editable
			// heights when the landscape is created, so these are read-only here
			// (shown for reference) and can no longer change the terrain.
			ImGui::SeparatorText("Noise (set at creation)");
			ImGui::BeginDisabled();
			ImGui::InputInt  ("Seed##tc",       &t->seed);
			int oct = t->octaves;
			ImGui::SliderInt ("Octaves##tc",    &oct, 1, 8);
			ImGui::DragFloat ("Frequency##tc",  &t->frequency,  0.01f, 0.01f, 16.0f, "%.2f");
			ImGui::DragFloat ("Lacunarity##tc", &t->lacunarity, 0.01f, 1.0f,  8.0f,  "%.2f");
			ImGui::DragFloat ("Gain##tc",       &t->gain,       0.01f, 0.0f,  1.0f,  "%.2f");
			ImGui::EndDisabled();

			if (changed) t->dirty = true;
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<TerrainComponent>(entity); }
	}

	// ── Audio Source ────────────────────────────────────────────────────────
	if (auto* a = registry.try_get<AudioSourceComponent>(entity))
	{
		if (componentHeader("Audio Source", true, removed))
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "%llu:%llu", (unsigned long long)a->assetId.hi,
			         (unsigned long long)a->assetId.lo);
			ImGui::LabelText("Asset ID", "%s", buf);
			char busBuf[64];
			std::strncpy(busBuf, a->busName.c_str(), sizeof(busBuf) - 1);
			busBuf[sizeof(busBuf) - 1] = '\0';
			if (ImGui::InputText("Bus##as", busBuf, sizeof(busBuf))) { a->busName = busBuf; trackEdit(); }
			ImGui::DragFloat("Volume##as", &a->volume, 0.01f, 0.0f, 2.0f); trackEdit();
			ImGui::DragFloat("Pitch##as",  &a->pitch,  0.01f, 0.1f, 4.0f); trackEdit();
			ImGui::Checkbox("Loop##as",        &a->loop);        trackEdit();
			ImGui::Checkbox("Play on Start##as",&a->playOnStart); trackEdit();
			ImGui::Checkbox("Spatial##as",     &a->spatial);     trackEdit();
			if (a->spatial)
			{
				ImGui::DragFloat("Inner Range##as",   &a->innerRange,    0.1f, 0.0f, 1000.0f, "%.1f m"); trackEdit();
				ImGui::DragFloat("Range##as",         &a->range,         0.5f, 0.0f, 1000.0f, "%.1f m"); trackEdit();
				ImGui::DragFloat("Rolloff Factor##as", &a->rolloffFactor, 0.1f, 0.0f, 10.0f,  "%.2f");   trackEdit();
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AudioSourceComponent>(entity); }
	}

	// ── Audio Listener ──────────────────────────────────────────────────────
	if (auto* l = registry.try_get<AudioListenerComponent>(entity))
	{
		if (componentHeader("Audio Listener", true, removed))
		{
			ImGui::DragFloat("Master Volume##al", &l->masterVolume, 0.01f, 0.0f, 2.0f); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AudioListenerComponent>(entity); }
	}

	// ── Particle System ─────────────────────────────────────────────────────
	// The emitter config lives in a ParticleGraphAsset (authored in the Particle
	// Graph Editor tab, same "asset instead of inline fields" move Material made) —
	// this section is just the asset slot + per-instance runtime controls.
	if (auto* ps = registry.try_get<ParticleSystemComponent>(entity))
	{
		if (componentHeader("Particle System", true, removed))
		{
			// Both assigning and clearing re-resolve the emitter config.
			if (EditorWidgets::assetDropSlot(ctx, "Asset", ps->particleAssetId,
					HE::AssetType::ParticleSystem, "psslot",
					"(none — drop a particle system here)", "particle system",
					/*showClear=*/true) != EditorWidgets::SlotAction::None)
				ParticleSystem::markConfigDirty(*ps);

			ImGui::Checkbox("Playing##ps", &ps->playing); trackEdit();
			ImGui::Text("Live: %zu", ps->particles.size());
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ParticleSystemComponent>(entity); }
	}

	// ── Foliage ──────────────────────────────────────────────────────────────
	if (auto* fol = registry.try_get<FoliageComponent>(entity))
	{
		if (componentHeader("Foliage", true, removed))
		{
			ImGui::DragFloat("Density##fol",       &fol->density,      0.01f, 0.001f, 10.f); if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragFloat("Draw Distance##fol", &fol->drawDistance, 1.0f,  1.0f,  500.f); trackEdit();
			ImGui::DragFloat("Min Scale##fol",     &fol->minScale,     0.01f, 0.01f, 10.f);  if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragFloat("Max Scale##fol",     &fol->maxScale,     0.01f, 0.01f, 10.f);  if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragInt  ("Seed##fol",          &fol->seed,         1);                    if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::Text("Instances: %zu", fol->cachedInstances.size());
			if (ImGui::Button("Regenerate")) { fol->dirty = true; trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<FoliageComponent>(entity); }
	}

	// ── LOD ──────────────────────────────────────────────────────────────────
	if (auto* lod = registry.try_get<LODComponent>(entity))
	{
		if (componentHeader("LOD", true, removed))
		{
			ImGui::Text("Levels: %zu   Active: %u", lod->levels.size(), lod->current);
			ImGui::Spacing();
			for (int li = 0; li < static_cast<int>(lod->levels.size()); ++li)
			{
				auto& lvl = lod->levels[static_cast<size_t>(li)];
				ImGui::PushID(li);
				ImGui::Text("LOD %d", li);
				ImGui::SameLine();
				char distBuf[32];
				std::snprintf(distBuf, sizeof(distBuf), "%.1f", lvl.maxDistance);
				ImGui::SetNextItemWidth(80.f);
				if (ImGui::InputText("##maxDist", distBuf, sizeof(distBuf),
				                     ImGuiInputTextFlags_EnterReturnsTrue))
				{
					lvl.maxDistance = std::strtof(distBuf, nullptr);
					trackEdit();
				}
				ImGui::SameLine();
				ImGui::TextDisabled("UUID %llx", static_cast<unsigned long long>(lvl.meshId.hi));
				if (ImGui::Button("Remove##lodlvl")) { lod->levels.erase(lod->levels.begin() + li); trackEdit(); --li; }
				ImGui::PopID();
			}
			if (ImGui::Button("+ Level")) { lod->levels.push_back({}); trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<LODComponent>(entity); }
	}

	// ── UI Canvas ───────────────────────────────────────────────────────────
	if (auto* cv = registry.try_get<UICanvasComponent>(entity))
	{
		if (componentHeader("UI Canvas", true, removed))
		{
			ImGui::DragFloat("Width##cv",  &cv->width,  1.0f, 1.0f, 7680.0f); trackEdit();
			ImGui::DragFloat("Height##cv", &cv->height, 1.0f, 1.0f, 4320.0f); trackEdit();
			int rm = static_cast<int>(cv->renderMode);
			if (ImGui::Combo("Render Mode##cv", &rm, "Screen Space\0World Space\0")) {
				cv->renderMode = static_cast<UIRenderMode>(rm); trackEdit();
			}
			ImGui::Checkbox("Active##cv", &cv->active); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UICanvasComponent>(entity); }
	}

	// ── UI Element ──────────────────────────────────────────────────────────
	if (auto* el = registry.try_get<UIElementComponent>(entity))
	{
		if (componentHeader("UI Element", true, removed))
		{
			ImGui::DragFloat2("Position##el", glm::value_ptr(el->position), 1.0f); trackEdit();
			ImGui::DragFloat2("Size##el",     glm::value_ptr(el->size),     1.0f, 0.0f, 10000.0f); trackEdit();
			ImGui::DragFloat2("Pivot##el",    glm::value_ptr(el->pivot),    0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat("Rotation##el",  &el->rotation, 0.5f); trackEdit();
			int anch = static_cast<int>(el->anchor);
			const char* anchNames = "Top Left\0Top Center\0Top Right\0"
			                        "Mid Left\0Mid Center\0Mid Right\0"
			                        "Bot Left\0Bot Center\0Bot Right\0";
			if (ImGui::Combo("Anchor##el", &anch, anchNames)) {
				el->anchor = static_cast<UIAnchor>(anch); trackEdit();
			}
			ImGui::DragInt("Layer##el",  &el->layer, 1); trackEdit();
			ImGui::Checkbox("Active##el", &el->active); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIElementComponent>(entity); }
	}

	// ── UI Text ─────────────────────────────────────────────────────────────
	if (auto* txt = registry.try_get<UITextComponent>(entity))
	{
		if (componentHeader("UI Text", true, removed))
		{
			char buf[256];
			strncpy(buf, txt->text.c_str(), sizeof(buf) - 1); buf[sizeof(buf)-1] = '\0';
			if (ImGui::InputText("Text##txt", buf, sizeof(buf))) { txt->text = buf; trackEdit(); }
			ImGui::DragFloat("Font Size##txt", &txt->fontSize, 0.5f, 4.0f, 256.0f); trackEdit();
			ImGui::ColorEdit4("Color##txt",    glm::value_ptr(txt->color)); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UITextComponent>(entity); }
	}

	// ── UI Image ─────────────────────────────────────────────────────────────
	if (auto* img = registry.try_get<UIImageComponent>(entity))
	{
		if (componentHeader("UI Image", true, removed))
		{
			ImGui::ColorEdit4("Tint##img", glm::value_ptr(img->tint)); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIImageComponent>(entity); }
	}

	// ── UI Button ───────────────────────────────────────────────────────────
	if (auto* btn = registry.try_get<UIButtonComponent>(entity))
	{
		if (componentHeader("UI Button", true, removed))
		{
			ImGui::ColorEdit4("Normal##btn",  glm::value_ptr(btn->normalColor)); trackEdit();
			ImGui::ColorEdit4("Hovered##btn", glm::value_ptr(btn->hoveredColor)); trackEdit();
			ImGui::ColorEdit4("Pressed##btn", glm::value_ptr(btn->pressedColor)); trackEdit();
			char buf[128];
			strncpy(buf, btn->onClickFunction.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
			if (ImGui::InputText("OnClick##btn", buf, sizeof(buf))) { btn->onClickFunction = buf; trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIButtonComponent>(entity); }
	}

	// ── Add Component ───────────────────────────────────────────────────────
	// Not for the World root — it only carries the scene's Environment, no
	// arbitrary components (and the built-in sun/moon are managed automatically).
	if (!ctx.world->isBuiltin(entity))
	{
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
			addItem("Mesh",          MeshComponent{});
			addItem("Skeletal Mesh", SkeletalMeshComponent{});
			addItem("Nav Mesh",                NavMeshComponent{});
			addItem("Nav Agent",               NavAgentComponent{});
			addItem("Material",     MaterialComponent{});
			addItem("Camera",       CameraComponent{});
			addItem("Light",        LightComponent{});
			addItem("Decal",        DecalComponent{});
			addItem("Rigid Body",          RigidBodyComponent{});
			addItem("Collider",            ColliderComponent{});
			// A ScriptComponent points at a Lua/Python Script asset, so it's only
			// offered when the project is authored in one of those languages
			// (HorizonCode drives entities via player/level graphs; C++ via native
			// GameLogic). Entities that already carry the component still edit fine.
			{
				const ProjectScriptLanguage plang = ctx.projectManager
					? ctx.projectManager->currentProject().scriptLanguage
					: ProjectScriptLanguage::HorizonCode;
				if (plang == ProjectScriptLanguage::Lua || plang == ProjectScriptLanguage::Python)
					addItem("Script",         ScriptComponent{});
			}
			addItem("Audio Source",    AudioSourceComponent{});
			addItem("Audio Listener",  AudioListenerComponent{});
			addItem("Particle System", ParticleSystemComponent{});
			addItem("LOD",             LODComponent{});
			addItem("Foliage",         FoliageComponent{});
			// Animator / Animator Blend / Animator State Machine / Property
			// Animator, Character Controller, and the UI components are
			// intentionally not offered here — they're meaningless bolted onto
			// an arbitrary entity and are set up through their owning asset
			// workflow instead (Skeletal Mesh + Animation State Machine editor
			// tabs, the player/character setup, the UI Widget designer). The
			// component types and their Inspector panels above still work for
			// entities that already carry them (e.g. older scenes).
			ImGui::EndPopup();
		}
	}

	ImGui::End();
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

} // namespace InspectorPanel
