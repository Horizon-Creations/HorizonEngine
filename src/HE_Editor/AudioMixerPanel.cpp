#include "AudioMixerPanel.h"
#include "EditorApplication.h"     // AppContext, ProjectManager, AudioEngine
#include "EditorWidgets.h"         // the help-aware button / small button
#include "EditorHelp.h"            // "Audio Mixer/<label>" scope for its controls
#include "EditorTheme.h"           // the accent for a lit M / S
#include "NotificationStore.h"     // notify() when the .heproj cannot be written

#include <Audio/AudioBusConfig.h>
#include <HorizonScene/AudioEngine.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace AudioMixerPanel
{
#ifdef HE_IMGUI_ENABLED

namespace
{

// ── Session state: what is NOT in the project ────────────────────────────────
// Mute and solo, per bus name. Names rather than indices, so a bus removed and
// re-added in the same session comes back unmuted, and a project reopened
// starts clean — nothing here is ever written anywhere.
struct BusListen
{
	std::string name;
	bool        muted = false;
	bool        solo  = false;
};
std::vector<BusListen> s_listen;
bool                   s_masterMuted = false;

// The name being typed for a new bus. A scratch buffer rather than a field on
// the project: the project only learns about the bus once Add is pressed.
std::string s_newBusName;

BusListen& listenFor(const std::string& name)
{
	for (BusListen& l : s_listen)
		if (l.name == name) return l;
	s_listen.push_back({ name });
	return s_listen.back();
}

// Faders run 0..2 linear (unity in the middle, 6 dB of boost above it — what
// a source that was recorded too quiet needs) and READ in decibels, because
// "-6 dB" means the same thing to everybody and "0.5" does not.
constexpr float kFaderMax = 2.0f;

void formatDb(char* out, size_t n, float linear)
{
	if (linear <= 0.0005f) std::snprintf(out, n, "-inf");
	else                   std::snprintf(out, n, "%+.1f dB", 20.0f * std::log10(linear));
}

// Push the session's mute/solo state into the engine. Solo is computed, not
// stored: with any strip soloed every other bus is silenced, and the engine's
// own "muted" is the union of both so a bus that is muted AND soloed stays
// quiet — the way a desk behaves.
void applyListenState(AudioEngine& audio, const HE::AudioBusConfig& cfg)
{
	bool anySolo = false;
	for (const HE::AudioBusDef& b : cfg.buses)
		if (listenFor(b.name).solo) { anySolo = true; break; }
	for (const HE::AudioBusDef& b : cfg.buses)
	{
		const BusListen& l = listenFor(b.name);
		audio.setBusMuted(b.name, l.muted || (anySolo && !l.solo));
	}
	audio.setMasterVolume(s_masterMuted ? 0.0f : cfg.masterVolume);
}

// One vertical strip. Returns true when the fader was RELEASED after a drag —
// the moment the .heproj is written. `volume` is edited in place, and every
// change is pushed into `onVolume` at once so the engine follows the drag.
struct StripResult
{
	bool commit = false;   // write the project
	bool remove = false;   // the strip's Remove was pressed
};

StripResult drawStrip(const char* id, const char* title, float* volume, bool* muted, bool* solo,
                      int voices, bool removable, const std::function<void(float)>& onVolume,
                      const std::function<void()>& onListen)
{
	StripResult r;
	ImGui::PushID(id);
	const ImGuiStyle& st = ImGui::GetStyle();
	const float stripW = std::max(ImGui::CalcTextSize(title).x, 64.0f) + st.FramePadding.x * 2.0f;
	ImGui::BeginGroup();

	// The name, centred over the fader.
	{
		const float w = ImGui::CalcTextSize(title).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (stripW - w) * 0.5f);
		ImGui::TextUnformatted(title);
	}
	// The value in dB, dimmed, so the fader itself can stay unlabelled.
	{
		char db[24];
		formatDb(db, sizeof(db), *volume);
		const float w = ImGui::CalcTextSize(db).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (stripW - w) * 0.5f);
		ImGui::TextDisabled("%s", db);
	}

	// The fader. Unity sits in the middle of the travel; a double-click puts it
	// back there, which is the mixer's "reset" and needs no button.
	const float faderW = ImGui::GetFrameHeight() * 1.2f;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (stripW - faderW) * 0.5f);
	if (ImGui::VSliderFloat("##fader", ImVec2(faderW, 160.0f), volume, 0.0f, kFaderMax, ""))
		onVolume(*volume);
	// By key: the fader has no visible label of its own, the strip's title above
	// it is data (a bus name), and every strip explains itself the same way.
	EditorWidgets::helpForKey(removable ? "Audio Mixer/Fader" : "Audio Mixer/Master");
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		*volume = 1.0f;
		onVolume(*volume);
		r.commit = true;
	}
	r.commit |= ImGui::IsItemDeactivatedAfterEdit();

	// M and S, side by side under the fader, lit in the accent while active.
	// Plain buttons rather than checkboxes: a desk's mute is a lit button, and
	// the box would not fit the strip anyway.
	auto litButton = [&](const char* label, bool lit) {
		if (lit)
		{
			ImGui::PushStyleColor(ImGuiCol_Button,        HE::Ed::Theme::Accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, HE::Ed::Theme::AccentHi);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  HE::Ed::Theme::Accent);
		}
		const bool pressed = ImGui::Button(label, ImVec2(ImGui::GetFrameHeight() * 1.4f, 0.0f));
		if (lit) ImGui::PopStyleColor(3);
		return pressed;
	};
	const float pairW = ImGui::GetFrameHeight() * 1.4f * (solo ? 2.0f : 1.0f) + (solo ? st.ItemSpacing.x : 0.0f);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (stripW - pairW) * 0.5f);
	if (litButton("M", *muted)) { *muted = !*muted; onListen(); }
	EditorWidgets::helpForKey("Audio Mixer/M");
	if (solo)
	{
		ImGui::SameLine();
		if (litButton("S", *solo)) { *solo = !*solo; onListen(); }
		EditorWidgets::helpForKey("Audio Mixer/S");
	}

	// Activity: how many voices are on this bus right now.
	{
		char line[32];
		if (voices == 1) std::snprintf(line, sizeof(line), "1 voice");
		else             std::snprintf(line, sizeof(line), "%d voices", voices);
		const float w = ImGui::CalcTextSize(line).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (stripW - w) * 0.5f);
		if (voices > 0) ImGui::TextUnformatted(line);
		else            ImGui::TextDisabled("%s", line);
	}

	if (removable)
	{
		const float w = ImGui::CalcTextSize("Remove").x + st.FramePadding.x * 2.0f;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (stripW - w) * 0.5f);
		if (EditorWidgets::dangerSmallButton("Remove")) r.remove = true;
	}
	// Pins the group to the strip width whatever its widest item was, so the
	// strips sit at an even pitch and the centring above means something.
	ImGui::Dummy(ImVec2(stripW, 0.0f));

	ImGui::EndGroup();
	ImGui::PopID();
	return r;
}

} // namespace

void DrawAudioMixerWindow(AppContext& ctx, bool& open)
{
	// Every control here is looked up as "Audio Mixer/<its label>".
	HE::Ed::Help::Scope helpScope("Audio Mixer");
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(560.0f, 340.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Audio Mixer", &open)) { ImGui::End(); return; }

	if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty())
	{
		ImGui::TextDisabled("No project is open.");
		ImGui::End();
		return;
	}
	if (!ctx.audioEngine || !ctx.audioEngine->isInitialized())
	{
		ImGui::TextDisabled("The audio device could not be opened — there is nothing to mix.");
		ImGui::End();
		return;
	}
	ProjectData&        p     = ctx.projectManager->currentProject();
	HE::AudioBusConfig& cfg   = p.audioBuses;
	AudioEngine&        audio = *ctx.audioEngine;

	// The engine may be missing a bus the project names (a project just
	// opened, a bus added from the MCP side) — bring it in line before the
	// strips read voice counts from it. Cheap: createBus is idempotent.
	audio.applyBusConfig(cfg);

	bool        commit    = false;
	std::string removeBus;

	const auto listen = [&]{ applyListenState(audio, cfg); };

	// Strips scroll sideways rather than shrinking: a fader that gets narrower
	// with every bus added is a fader nobody can grab. The child is as tall as
	// a strip and no taller, so the row under it is where the strips end and
	// not at a fixed height that cut the voice count off in a short window.
	if (ImGui::BeginChild("##strips", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY,
	                      ImGuiWindowFlags_HorizontalScrollbar))
	{
		// Master first, set apart by a divider.
		{
			float master = cfg.masterVolume;
			const StripResult r = drawStrip("master", "Master", &master, &s_masterMuted, nullptr,
				audio.busVoiceCount(""), false,
				[&](float v) { cfg.masterVolume = v; if (!s_masterMuted) audio.setMasterVolume(v); },
				listen);
			commit |= r.commit;
		}
		// The divider, drawn by hand: ImGui::Separator in a horizontal layout
		// wants a height the group above it does not give it, and drew nothing.
		{
			ImGui::SameLine();
			const ImVec2 top = ImGui::GetCursorScreenPos();
			const float  h   = ImGui::GetItemRectSize().y;
			ImGui::GetWindowDrawList()->AddLine(top, ImVec2(top.x, top.y + h),
			                                    ImGui::GetColorU32(ImGuiCol_Separator));
			ImGui::Dummy(ImVec2(1.0f, h));
			ImGui::SameLine();
		}

		for (HE::AudioBusDef& b : cfg.buses)
		{
			BusListen& l = listenFor(b.name);
			const StripResult r = drawStrip(b.name.c_str(), b.name.c_str(), &b.volume,
				&l.muted, &l.solo, audio.busVoiceCount(b.name), true,
				[&](float v) { audio.setBusVolume(b.name, v); },
				listen);
			commit |= r.commit;
			if (r.remove) removeBus = b.name;
			ImGui::SameLine();
		}

		if (cfg.buses.empty())
		{
			ImGui::BeginGroup();
			ImGui::TextDisabled("No buses yet.");
			EditorWidgets::hint("A source's Bus field names one of these. Add the usual three, "
			                    "or type a name below.");
			// The three every game ends up with. One click rather than three
			// typed names, and the spelling a source most likely already used.
			if (EditorWidgets::button("Add Music, SFX and Voice"))
			{
				cfg.add("Music"); cfg.add("SFX"); cfg.add("Voice");
				commit = true;
			}
			ImGui::EndGroup();
		}
	}
	ImGui::EndChild();

	ImGui::Separator();

	// ── Adding a bus ─────────────────────────────────────────────────────────
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("New bus");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.0f);
	const bool entered = ImGui::InputTextWithHint("##newbus", "Name", &s_newBusName,
	                                              ImGuiInputTextFlags_EnterReturnsTrue);
	EditorWidgets::helpForKey("Audio Mixer/Name");
	ImGui::SameLine();
	const bool canAdd = !s_newBusName.empty() && !cfg.find(s_newBusName);
	ImGui::BeginDisabled(!canAdd);
	const bool addPressed = EditorWidgets::button("Add Bus");
	ImGui::EndDisabled();
	if ((entered || addPressed) && canAdd)
	{
		cfg.add(s_newBusName);
		s_newBusName.clear();
		commit = true;
	}
	else if (!s_newBusName.empty() && cfg.find(s_newBusName))
	{
		ImGui::SameLine();
		ImGui::TextDisabled("already exists");
	}

	if (!removeBus.empty())
	{
		// Engine first — it stops the voices on the bus — then the project.
		audio.removeBus(removeBus);
		cfg.remove(removeBus);
		s_listen.erase(std::remove_if(s_listen.begin(), s_listen.end(),
		                              [&](const BusListen& l) { return l.name == removeBus; }),
		               s_listen.end());
		commit = true;
	}

	if (commit)
	{
		// A bus added above exists in the project but not yet in the engine;
		// the next frame's applyBusConfig would catch it, but a strip that
		// appears one frame late with "0 voices" it cannot count yet is the
		// kind of flicker that reads as a bug.
		audio.applyBusConfig(cfg);
		listen();
		if (!ctx.projectManager->saveProject(p.path))
			HE::Ed::notify(HE::Ed::NoteLevel::Problem,
			               "Could not save the project's audio buses", p.path);
	}

	ImGui::End();
}

#else
void DrawAudioMixerWindow(AppContext&, bool&) {}
#endif

} // namespace AudioMixerPanel
