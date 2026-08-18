#include "CollabPanel.h"
#include "EditorWidgets.h"    // primary/danger/cancel buttons
#include "EditorTheme.h"      // brand palette (emphasis text)

#include "CollabController.h"

#include "CollabPresenceBar.h"

#include <Net/Socket.h>
#include "EditorApplication.h"

#include <SDL3/SDL_dialog.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace CollabPanel
{
#ifdef HE_IMGUI_ENABLED
namespace
{
	// Panel-local input state. The session ID and join code are deliberately NOT
	// persisted — a session is short-lived by design — but the display name is,
	// alongside the profile picture, so identity is set once rather than retyped
	// on every join (see CollabController::Identity).
	char s_displayName[64]  = "";
	int  s_hostPort         = 7777;
	char s_joinSessionId[80] = "";
	char s_joinCode[80]      = "";
	// Which discovered session the user clicked, by announcing-instance rather
	// than by list position: the list re-sorts itself as beacons arrive and
	// expire, so an index would move under the selection.
	std::uint64_t s_lanPicked = 0;
	// Its own buffer, NOT s_joinCode. Both are on screen at once, and sharing
	// one made each field echo into the other — and, worse, could arm the
	// by-ID "Join" button with a code typed for a session on this network while
	// a stale session ID sat in the field above it, sending the user somewhere
	// they never chose.
	char s_lanJoinCode[80] = "";

	// Why the last picture could not be used, shown until the next attempt.
	std::string s_avatarError;
	// Set when agreeing to large-asset sync could not re-dial by itself, so the
	// user is told the setting took effect and only the join is left to redo.
	// Cleared the moment a join is started, since it is then out of date.
	std::string s_joinRetryHint;
	// Result slot for the picture file dialog. SDL may deliver the callback on
	// another thread, so the flag is atomic and is set AFTER the path — the
	// reader sees a complete path or no path at all, never half of one.
	std::string             s_avatarPickPath;
	std::atomic<bool>       s_avatarPickReady { false };

	// Pull the stored name into the input box once per run, so the field shows
	// what will actually be sent rather than a placeholder that silently differs.
	void SyncDisplayNameFromIdentity()
	{
		if (s_displayName[0] != '\0') return;
		const std::string& stored = CollabController::localIdentity().name;
		std::snprintf(s_displayName, sizeof(s_displayName), "%s", stored.c_str());
	}

	void CopyableValue(const char* label, const std::string& value, const char* id)
	{
		ImGui::TextUnformatted(label);
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::SmallButton(id)) ImGui::SetClipboardText(value.c_str());
	}

	// ── Does THAT session carry the big media? ───────────────────────────────
	// Straight from the host's own announcement, which publishes the same value
	// the handshake will answer a joiner from.
	//
	// It must NEVER fall back to ctx.editorConfig.CollabSyncLargeAssets. That is
	// what THIS editor agreed to, and painting it onto a stranger's row would
	// tell a guest with the setting on that every session on the network carries
	// meshes — including the ones that do not — which is worse than saying
	// nothing, because it is a claim about somebody else's session.
	//
	// And it is a HINT, not a gate: the datagram is unauthenticated and can be
	// stale or forged, so the join still decides. A row from an older peer reads
	// false because that build could not say — which is harmless here, because
	// the list refuses to join a mismatched protocol anyway.
	bool SessionAnnouncesLargeAssets(const HE::Net::LanBeacon::Browser::Session& s)
	{
		return s.syncsLargeAssets;
	}

	// ── Colour ───────────────────────────────────────────────────────────────
	// The colour this user is drawn in throughout the session: viewport marker,
	// selection highlight, lock badges, footer avatar. Presets first because they
	// are the answer for almost everyone and they are guaranteed to be legible
	// against the viewport; the free picker is there for the person who wants
	// their own and knows what they are doing.
	//
	// Nothing here is a promise. The host settles collisions, so someone who
	// picks a taken colour is quietly given a free one — which is why the panel
	// also shows what was actually assigned once a session is running.
	void DrawColorChoice(AppContext& ctx, bool editable)
	{
		using HE::Net::ParticipantColor;

		ImGui::Spacing();
		ImGui::TextUnformatted("Colour");
		ImGui::BeginDisabled(!editable);

		const ParticipantColor mine = CollabController::localIdentity().color;

		constexpr float kSwatch = 20.0f;
		int index = 0;
		for (const ParticipantColor& preset : HE::Net::kParticipantPalette)
		{
			ImGui::PushID(index++);
			const ImVec4 col(preset.r / 255.0f, preset.g / 255.0f, preset.b / 255.0f, 1.0f);
			const bool   chosen = !mine.unset() && mine.r == preset.r &&
			                      mine.g == preset.g && mine.b == preset.b;

			// The current pick gets a border thick enough to read at 20 px; a
			// tick mark would not survive on a saturated background.
			ImGui::PushStyleColor(ImGuiCol_Button,        col);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col);
			ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(1, 1, 1, chosen ? 1.0f : 0.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, chosen ? 2.0f : 0.0f);
			if (ImGui::Button("##swatch", ImVec2(kSwatch, kSwatch)))
				CollabController::setLocalColor(preset);
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(4);
			ImGui::PopID();
			ImGui::SameLine(0.0f, 4.0f);
		}

		// "Automatic" is a real choice, not the absence of one: it means the host
		// hands out whatever is free, which is the right answer for a user who
		// does not care and the only answer that never collides.
		const bool automatic = mine.unset();
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, automatic ? 2.0f : 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, automatic ? 1.0f : 0.0f));
		if (ImGui::Button("Auto", ImVec2(0.0f, kSwatch)))
			CollabController::setLocalColor(ParticipantColor{});
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Let the host pick a colour that is still free.");

		float custom[3] = { mine.r / 255.0f, mine.g / 255.0f, mine.b / 255.0f };
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::ColorEdit3("Custom", custom,
		                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
		{
			ParticipantColor picked;
			picked.r = static_cast<std::uint8_t>(std::clamp(custom[0], 0.0f, 1.0f) * 255.0f + 0.5f);
			picked.g = static_cast<std::uint8_t>(std::clamp(custom[1], 0.0f, 1.0f) * 255.0f + 0.5f);
			picked.b = static_cast<std::uint8_t>(std::clamp(custom[2], 0.0f, 1.0f) * 255.0f + 0.5f);
			// Pure black is the "no preference" sentinel and is unusable as a
			// marker anyway, so it is nudged rather than silently meaning Auto.
			if (picked.unset()) picked.r = picked.g = picked.b = 1;
			CollabController::setLocalColor(picked);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("custom");

		ImGui::EndDisabled();

		// What was actually handed out — which is the interesting number the
		// moment somebody else already had the colour you asked for.
		if (ctx.collab && ctx.collab->inSession())
		{
			const auto localId = ctx.collab->localParticipant();
			for (const HE::Net::Participant& p : ctx.collab->participants())
			{
				if (p.id != localId || p.color.unset()) continue;
				const bool asWished = !mine.unset() && p.color.r == mine.r &&
				                      p.color.g == mine.g && p.color.b == mine.b;
				if (!asWished)
				{
					ImGui::TextDisabled("The colour you picked was taken — you were "
					                    "given a free one for this session.");
				}
				break;
			}
		}
	}

	// ── Identity editor ──────────────────────────────────────────────────────
	// The name and picture everyone else in a session sees. Persisted, so this is
	// set once rather than on every join — and shown here rather than buried in
	// Preferences because this window is where a user is already thinking about
	// how they appear to other people.
	//
	// `editable` is false during a live session: the identity is read when the
	// session STARTS, so changing it mid-session would edit a value nobody will
	// look at again until the next join. Saying so beats silently doing nothing.
	void DrawIdentity(AppContext& ctx, bool editable)
	{
		SyncDisplayNameFromIdentity();

		ImGui::SeparatorText("You");

		const float avatarSize = 56.0f;
		CollabPresenceBar::DrawLocalAvatar(ctx, avatarSize);
		ImGui::SameLine(0.0f, 12.0f);

		ImGui::BeginGroup();
		ImGui::BeginDisabled(!editable);

		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputText("Display name", s_displayName, sizeof(s_displayName));
		// Written when the field is left, not on every keystroke: setLocalName
		// rewrites config.json, and a name is a dozen characters.
		if (ImGui::IsItemDeactivatedAfterEdit()) CollabController::setLocalName(s_displayName);

		const bool hasPicture = CollabController::localIdentity().avatarSize > 0;
		if (ImGui::Button(hasPicture ? "Change picture..." : "Choose picture..."))
		{
			s_avatarError.clear();
			SDL_DialogFileFilter filters[] = {
				{ "Images", "png;jpg;jpeg;bmp;tga;gif" },
			};
			SDL_ShowOpenFileDialog(
				[](void* /*userdata*/, const char* const* filelist, int /*filter*/)
				{
					// The panel's own result slot, deliberately NOT the shared
					// ctx.dialogBridge: that one is consumed by the scene/project
					// handler in EditorUI, which runs BEFORE this window is drawn
					// and would interpret a chosen portrait as a scene to open.
					if (filelist && filelist[0]) s_avatarPickPath = filelist[0];
					s_avatarPickReady.store(true, std::memory_order_release);
				},
				nullptr,
				ctx.window ? ctx.window->GetNativeWindow() : nullptr,
				filters, 1, nullptr, false);
		}
		if (hasPicture)
		{
			ImGui::SameLine();
			if (EditorWidgets::dangerSmallButton("Remove")) CollabController::clearLocalAvatar();
		}

		ImGui::EndDisabled();
		ImGui::EndGroup();

		DrawColorChoice(ctx, editable);

		if (!editable)
			ImGui::TextDisabled("Changes take effect the next time you join a session.");
		if (!s_avatarError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
			ImGui::TextWrapped("%s", s_avatarError.c_str());
			ImGui::PopStyleColor();
		}
	}

	// ── "This session sends the big files — is that all right?" ──────────────
	// The host decides what its session carries, but the cost of the big assets
	// lands on the GUEST's connection, so a guest who has not agreed is refused
	// at the handshake (JoinRejectReason::LargeAssetsRequired) rather than
	// quietly signed up. That refusal is a question, and this is where it gets
	// asked: the alternative is a failed join with a paragraph in a status line,
	// which reads as a dead end to somebody who would have said yes immediately.
	//
	// Modal on purpose, unlike the host's request queue next door. That queue is
	// other people's work arriving while you get on with yours; this is the
	// answer to something the user asked for two seconds ago and is waiting on,
	// and there is nothing else to do in this window until it is answered.
	const char* kLargePopup = "Larger assets##collab";
	// Whether OpenPopup has already been called for the pending question. ImGui
	// pushes a popup that is opened again while it is up, which — with a modal —
	// closes and reopens it under the cursor and can eat the click that was
	// about to answer it. So it is opened on the EDGE, once.
	bool s_largePopupArmed = false;

	void DrawLargeAssetPrompt(AppContext& ctx)
	{
		CollabController* collab = ctx.collab;
		if (!collab) return;

		const bool pending = collab->largeAssetsPrompt();
		if (pending && !s_largePopupArmed)
		{
			ImGui::OpenPopup(kLargePopup);
			s_largePopupArmed = true;
		}
		if (!pending)
		{
			s_largePopupArmed = false;
			return;
		}

		// Centred rather than wherever the window happens to sit: it is asking
		// about something the user cannot see yet, so it should not look like a
		// footnote to the panel behind it.
		const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(kLargePopup, nullptr,
		                            ImGuiWindowFlags_AlwaysAutoResize))
		{
			// Armed, still pending, and yet not on screen: ImGui closed it
			// itself, which for a modal means Escape. That has to count as
			// Cancel. Without this the controller's flag stays set while the
			// arm-latch also stays set, so the popup is never reopened and the
			// question can never be answered again — the panel would sit there
			// refusing to join with no way to say yes.
			collab->clearLargeAssetsPrompt();
			s_largePopupArmed = false;
			return;
		}

		// ── Everything in this window wraps at its right edge ────────────────
		// This dialog asks the user to agree to a cost, and it says so in whole
		// sentences. A sentence whose second half runs off the edge is worse than
		// no sentence at all: the reader is being asked to agree to something they
		// can only see the beginning of. TextWrapped covers the two paragraphs,
		// but not the dimmed line about the setting being remembered — and that is
		// the line that says the choice outlives this join. One wrap position for
		// the whole popup covers both, and covers whatever is added here later.
		//
		// In a scope of its own on purpose, and the same is true of every other
		// WrapText in this file: the pop has to happen while this popup is still
		// the current window. Left to the end of the function it would run after
		// EndPopup(), and pop the wrap position off whichever window is current by
		// then — which, for a dialog raised from outside any window, is none.
		//
		// An absolute column and NOT the window edge, because this popup is
		// AlwaysAutoResize and its SetNextWindowSize is only ImGuiCond_Appearing.
		// Wrapping at the edge of a window that sizes itself to its widest line is
		// a feedback loop: each frame the text refits to the current width, the
		// window refits to the text, and the dialog visibly narrows toward ImGui's
		// minimum — while the user is being asked to read it. A window-space
		// constant reaches its fixed point on the first frame instead.
		{
			constexpr float kPopupW = 430.0f;   // the SetNextWindowSize above
			EditorWidgets::WrapText wrap(kPopupW - ImGui::GetStyle().WindowPadding.x);

			ImGui::TextWrapped("This session also transfers larger assets — meshes, "
			                   "textures and audio — instead of leaving them to source "
			                   "control.");
			ImGui::Spacing();
			// The sentence the user is actually deciding on, and the reason the
			// setting exists at all. Coloured because it is the cost, not the
			// description: somebody on a phone hotspot has to be able to see it
			// without reading the paragraph above.
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
			ImGui::TextWrapped("It can use considerably more data than an ordinary "
			                   "session. On a metered or slow connection, that is worth "
			                   "thinking about before you agree.");
			ImGui::PopStyleColor();
			ImGui::Spacing();
			ImGui::TextDisabled("The setting is remembered, and applies to every session "
			                    "you host or join from now on. You can turn it off again "
			                    "in Preferences while you are not in a session.");
			ImGui::Spacing();

			if (EditorWidgets::primaryButton("Enable and join", ImVec2(150, 0)))
			{
				// Two writes, because they answer two different questions. The
				// config is the persisted setting (the editor writes it out and
				// reads it back on the next launch) and it is what the Preferences
				// checkbox shows; the controller is what actually goes on the wire
				// for the retry, which happens now and cannot wait for the editor to
				// push its config down on the next frame.
				ctx.editorConfig.CollabSyncLargeAssets = true;
				collab->setSyncLargeAssets(true);
				if (!collab->retryJoinWithLargeAssets())
				{
					// Nothing remembered to dial — the join was never begun through
					// this controller. The setting is on regardless, so joining
					// again by hand now works; saying nothing would look like the
					// button did nothing at all.
					s_joinRetryHint = "The setting is on. Join the session again.";
				}
				collab->clearLargeAssetsPrompt();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(110, 0)))
			{
				// Refusing is a real answer, not a failure to answer: nothing is
				// turned on, no join is retried, and the panel goes back to the join
				// form with the session's own refusal text still in the error line.
				collab->clearLargeAssetsPrompt();
				ImGui::CloseCurrentPopup();
			}
		}

		ImGui::EndPopup();
	}

	// Pick up a picture the OS dialog delivered, whichever thread it arrived on.
	void ConsumePendingAvatarPick()
	{
		if (!s_avatarPickReady.load(std::memory_order_acquire)) return;
		s_avatarPickReady.store(false, std::memory_order_relaxed);

		const std::string path = s_avatarPickPath;
		s_avatarPickPath.clear();
		if (path.empty()) return;   // the user cancelled

		std::string error;
		if (!CollabController::setLocalAvatarFromFile(path, error)) s_avatarError = error;
		else                                                        s_avatarError.clear();
	}
} // namespace
#endif

void DrawCollabWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
	// A refused join that the user can still say yes to reopens this window.
	// The question is drawn from inside it, and somebody who started a join and
	// then closed the panel would otherwise never see it — the join would simply
	// have failed, for a reason they had already agreed to in their head.
	if (ctx.collab && ctx.collab->largeAssetsPrompt()) open = true;
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(440, 400), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Collaboration", &open)) { ImGui::End(); return; }

	CollabController* collab = ctx.collab;
	if (!collab)
	{
		ImGui::TextDisabled("(collaboration unavailable in this build)");
		ImGui::End();
		return;
	}

	ConsumePendingAvatarPick();
	// Before the branches below, so it is asked whatever state the panel is in.
	// A rejection leaves the controller Failed — the "not active" branch — but
	// binding the question to that branch would tie a decision the user has to
	// make to a status they never see.
	DrawLargeAssetPrompt(ctx);

	if (!collab->active())
	{
		// One wrap position per branch of this window rather than one for the
		// window itself: the panel's own ImGui::End() sits at the bottom of the
		// function, so a guard opened next to Begin() would pop after it. Each
		// branch is a closed block, which is exactly the lifetime that is wanted —
		// and between them they cover everything this window draws. Without one,
		// every unwrapped line here (the row under a discovered session, the
		// router's verdict, the "both fields are required" note) is cut off at the
		// panel's edge, and this panel is docked narrow by default.
		EditorWidgets::WrapText wrap;

		ImGui::TextWrapped("Work on the same scene together. The host opens a session "
		                   "and shares its ID and join code; that is all a guest needs.");

		DrawIdentity(ctx, true);

		// ── Host ──
		ImGui::SeparatorText("Host a session");
		ImGui::SetNextItemWidth(120);
		ImGui::InputInt("Port", &s_hostPort);
		s_hostPort = std::clamp(s_hostPort, 0, 65535);
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("0 lets the system pick a free port.");

		if (EditorWidgets::primaryButton("Open session", ImVec2(170, 0)))
			collab->startHosting(static_cast<std::uint16_t>(s_hostPort), s_displayName);

		// What the startup probe already found out about this network, said BEFORE
		// the session is opened. Hosting used to be the only way to learn that the
		// router does not forward ports — by which point the user had a live
		// session nobody could reach and no idea why.
		if (!ctx.routerProbe)
		{
			ImGui::TextDisabled("Checking this network…");
		}
		else
		{
			const HE::Net::RouterProbe& r = *ctx.routerProbe;
			if (r.localNetworkBlocked)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.45f, 1.0f));
				ImGui::TextWrapped("This application may not talk to the local network, so the "
				                   "router cannot be reached at all. On macOS, allow it under "
				                   "Privacy & Security > Local Network.");
				ImGui::PopStyleColor();
			}
			else if (r.portForwardingAvailable())
			{
				ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
					"Router forwards ports (%s).",
					r.upnpFound ? "UPnP" : "NAT-PMP");
			}
			else if (!r.globalIPv6.empty())
			{
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
					"The router does not offer port forwarding, but this machine has a "
					"global IPv6 address — guests on IPv6 can still reach it.");
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
				ImGui::TextWrapped("The router answered neither UPnP nor NAT-PMP — guests will "
				                   "probably not reach this machine without a hand-made port "
				                   "forward. Opening a session still works; see "
				                   "Preferences > Tools > Status.");
				ImGui::PopStyleColor();
			}
			if (r.carrierNat)
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
					"Your connection is behind carrier-grade NAT — port forwarding cannot "
					"help here.");

			// The way out of every warning above. Said HERE, next to the bad
			// news, because "guests will not reach this machine" is exactly the
			// moment someone needs to know that people in the same room still
			// can.
			if (!r.portForwardingAvailable() || r.carrierNat)
			{
				ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
					"Anyone on this network can still find and join the session.");
			}
		}

		// One switch for both halves of discovery — announcing when you host,
		// listening when you do not. It is one feature to the person using it.
		{
			// The config is the one that is written, never the controller: the
			// editor pushes it down every frame, so this both takes effect
			// immediately and survives a restart.
			bool lanOn = ctx.editorConfig.CollabLanDiscovery;
			if (ImGui::Checkbox("Announce this session on the local network", &lanOn))
				ctx.editorConfig.CollabLanDiscovery = lanOn;

			// Whether anything is actually leaving this machine. Without it,
			// "they cannot see me" is unanswerable from here: a refused send
			// looks exactly like a send nobody happened to hear.
			if (lanOn)
			{
				const auto st = collab->lanStats();
				if (st.failed > 0 && st.sent == 0)
				{
					ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
						"The system is refusing to send them (%u attempts).", st.failed);
					ImGui::TextWrapped("On macOS this is the Local Network permission: "
					                   "allow it under Privacy & Security > Local Network. "
					                   "Otherwise it is a firewall.");
				}
				else if (st.sent > 0)
				{
					ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
						"Announced %u time%s.", st.sent, st.sent == 1 ? "" : "s");
					// The other direction, which says whether the two machines
					// can hear each other at all — answerable now without
					// giving up this session to go and look.
					const std::size_t others = collab->lanSessions().size();
					if (others > 0)
						ImGui::TextDisabled("%zu other session%s audible from here.",
						                    others, others == 1 ? "" : "s");
					else if (st.heard > 0)
						ImGui::TextDisabled("Announcements from this machine come "
						                    "back to it, so the network carries them.");
				}
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"People on the same network see your session in their list and\n"
					"join without an address. Your join code is NOT announced.\n"
					"Turn this off on a network you do not want to be seen on.");
			}
		}

		// ── Join ──
		ImGui::SeparatorText("Join a session");

		// ── Sessions on this network ──
		// Above the manual fields on purpose: when it works it is the answer,
		// and the session id exists for the case where it does not.
		{
			// Same switch as the host side's — one feature, one setting. Written
			// to the config, which the editor pushes into the controller.
			bool lanOn = ctx.editorConfig.CollabLanDiscovery;
			if (ImGui::Checkbox("Look for sessions on this network", &lanOn))
			{
				ctx.editorConfig.CollabLanDiscovery = lanOn;
				// The list is gone either way; a selection into it is not worth
				// carrying across the switch.
				s_lanPicked = 0;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Hosts announce themselves on the local network, so a session\n"
					"here needs no address and no session ID. The join code is\n"
					"never announced — you still get that from the host.");
			}

			if (lanOn)
			{
				const auto& found = collab->lanSessions();
				if (collab->lanBlocked())
				{
					ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
						"This application may not use the local network.");
					ImGui::TextWrapped("On macOS, allow it under Privacy & Security > "
					                   "Local Network. Until then, sessions here cannot "
					                   "be found — the session ID below still works.");
				}
				else if (found.empty())
				{
					// Said plainly, because an empty box reads as broken. Hosts
					// speak every couple of seconds, so "nothing yet" is the
					// normal first impression.
					ImGui::TextDisabled("No sessions found on this network yet.");
					// But an empty list has two very different causes, and only
					// this number tells them apart: nothing reaching us at all
					// (a firewall, or macOS's Local Network permission) is a
					// different problem from nobody hosting.
					const auto st = collab->lanStats();
					if (st.heard == 0)
					{
						ImGui::TextDisabled(
							"Nothing at all has reached this machine on port %u. If someone "
							"IS hosting nearby, this is a firewall or, on macOS, the Local "
							"Network permission — not the session.",
							unsigned(HE::Net::LanBeacon::kPort));
					}
				}
				else
				{
					for (std::size_t i = 0; i < found.size(); ++i)
					{
						const auto& s = found[i];
						ImGui::PushID(static_cast<int>(2000 + i));

						const bool sameProject =
							s.projectKey.empty() || collab->projectId().empty() ||
							s.projectKey == collab->projectId();
						const bool sameProtocol =
							s.protocol == HE::Net::kCollabProtocolVersion;
						const bool joinable = sameProject && sameProtocol;

						ImGui::BeginDisabled(!joinable);
						// One click fills the address in; the code is still
						// typed, because it is the only thing keeping strangers
						// on this network out.
						if (ImGui::Selectable(
								(s.hostName.empty() ? std::string("Someone") : s.hostName).c_str(),
								s_lanPicked == s.instance, 0, ImVec2(0, 0)))
						{
							s_lanPicked = s.instance;
							// The ID goes into the field below too, so the two
							// ways in are visibly ONE session rather than two
							// unrelated forms — and whichever button gets
							// pressed does the same thing, because joining by
							// ID now notices that this session is on the
							// network and takes the announced address.
							std::strncpy(s_joinSessionId, s.sessionId.c_str(),
							             sizeof(s_joinSessionId) - 1);
							s_joinSessionId[sizeof(s_joinSessionId) - 1] = '\0';
						}
						ImGui::EndDisabled();

						ImGui::SameLine();
						ImGui::TextDisabled("%s  ·  %u %s",
							s.projectLabel.empty() ? "unnamed project" : s.projectLabel.c_str(),
							unsigned(s.participants),
							s.participants == 1 ? "person" : "people");

						// Why a row cannot be clicked, said on the row. A
						// session that is simply missing sends people to ask
						// each other what is wrong.
						if (!sameProtocol)
						{
							ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
								"    different engine version — one of you needs to update");
						}
						else if (!sameProject)
						{
							ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
								"    a different project");
						}

						// What this session will cost you, on the row, before the
						// click. Only for the sessions that say so — a notice on
						// every row is a notice nobody reads, and the ones that
						// leave the big media to source control have nothing to
						// warn about.
						//
						// A tinted block rather than another column: this window
						// is narrow, and the sentence does not fit beside a
						// project name and a head count.
						if (joinable && SessionAnnouncesLargeAssets(s))
						{
							// The list's own amber, at an alpha that reads as a
							// tint behind text rather than as a second selection
							// highlight — a row already has one of those.
							const ImVec4 amber(1.0f, 0.78f, 0.35f, 1.0f);
							ImGui::PushStyleColor(ImGuiCol_ChildBg,
							                      ImVec4(amber.x, amber.y, amber.z, 0.12f));
							// AutoResizeY, because the text wraps: a fixed height
							// clips the second line on a narrow window, which is
							// exactly the window this panel is usually docked in.
							ImGui::BeginChild("##bigassets", ImVec2(0, 0),
							                  ImGuiChildFlags_AutoResizeY |
							                  ImGuiChildFlags_AlwaysUseWindowPadding);
							ImGui::PushStyleColor(ImGuiCol_Text, amber);
							// The last clause is only true while the setting is
							// off — that is when joining raises the question and
							// turns it on. Saying it to someone who already has
							// it on would promise a dialog that never comes.
							ImGui::TextWrapped(
								ctx.editorConfig.CollabSyncLargeAssets
									? "Also transfers meshes, textures and audio — this "
									  "session can use considerably more data."
									: "Also transfers meshes, textures and audio — this "
									  "session can use considerably more data. Joining "
									  "turns that on for you too.");
							ImGui::PopStyleColor();
							ImGui::EndChild();
							ImGui::PopStyleColor();
						}
						ImGui::PopID();
					}

					// The picked row, joined with the code typed below it.
					const auto picked = std::find_if(found.begin(), found.end(),
						[](const auto& s) { return s.instance == s_lanPicked; });
					if (picked != found.end())
					{
						ImGui::Spacing();
						ImGui::SetNextItemWidth(220);
						ImGui::InputText("Join code##lan", s_lanJoinCode,
						                 sizeof(s_lanJoinCode));
						ImGui::BeginDisabled(s_lanJoinCode[0] == '\0');
						if (EditorWidgets::primaryButton("Join this session", ImVec2(190, 0)))
						{
							s_joinRetryHint.clear();
							collab->joinSession(picked->address, picked->port,
							                    s_lanJoinCode, s_displayName);
						}
						ImGui::EndDisabled();
						if (s_lanJoinCode[0] == '\0')
							ImGui::TextDisabled("Ask the host for the join code.");
					}
					else if (s_lanPicked != 0)
					{
						// It expired or said goodbye while the code was being
						// typed. The field and the button disappearing without
						// a word looks like the editor ate the input.
						ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
							"That session is no longer being announced.");
						s_lanPicked = 0;
					}
				}
			}
		}

		ImGui::Spacing();
		ImGui::TextDisabled("Or join by ID:");
		ImGui::InputText("Session ID", s_joinSessionId, sizeof(s_joinSessionId));
		ImGui::InputText("Join code",  s_joinCode,      sizeof(s_joinCode));

		const bool canJoin = s_joinSessionId[0] != '\0' && s_joinCode[0] != '\0';
		ImGui::BeginDisabled(!canJoin);
		if (EditorWidgets::primaryButton("Join", ImVec2(170, 0)))
		{
			s_joinRetryHint.clear();
			collab->joinBySessionId(s_joinSessionId, s_joinCode, s_displayName);
		}
		ImGui::EndDisabled();
		if (!canJoin)
			ImGui::TextDisabled("Both the session ID and the join code are required.");
		if (!s_joinRetryHint.empty())
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "%s",
			                   s_joinRetryHint.c_str());

		// What THIS editor has agreed to, said where a join is started rather
		// than only in Preferences. It changes which sessions will have us —
		// a host that carries the big assets refuses an editor that has not
		// agreed — and being refused is a worse way to find that out.
		if (ctx.editorConfig.CollabSyncLargeAssets)
		{
			ImGui::TextDisabled("Larger assets (meshes, textures, audio) are shared in "
			                    "your sessions. This can use considerably more data.");
		}

		// Joining REPLACES the open scene, which a button labelled "Join" does
		// not convey — say so before it happens, not after.
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextWrapped("Joining replaces your open scene with the host's. "
		                   "Save anything you want to keep first.");
		ImGui::PopStyleColor();
	}
	else if (collab->isHost())
	{
		EditorWidgets::WrapText wrap;

		ImGui::SeparatorText("Hosting");

		// WHERE this session actually is. It was never shown, and without it the
		// only way to answer "which port is the host on?" is to read a socket
		// list — where the discovery socket (UDP 47823) sits next to the session
		// listener and looks just as much like the answer. They are different
		// sockets on different protocols, and confusing them sends you looking
		// for a bug in the wrong place.
		ImGui::Text("Listening on %s:%u",
		            collab->localAddress().empty() ? "this machine"
		                                           : collab->localAddress().c_str(),
		            unsigned(collab->port()));
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(
				"This is the session itself (TCP), and the port guests connect to.\n"
				"Local-network discovery is a separate UDP socket on port %u —\n"
				"it only carries the \"a session exists here\" announcements.",
				unsigned(HE::Net::LanBeacon::kPort));
		}
		ImGui::Spacing();

		if (collab->directoryBusy())
		{
			ImGui::TextDisabled("Publishing session...");
		}
		else if (!collab->sessionId().empty())
		{
			// These two are everything a guest needs.
			CopyableValue("Session ID", collab->sessionId(), "Copy##sid");
			ImGui::Spacing();
			CopyableValue("Join code", collab->joinCode(), "Copy##code");
			ImGui::TextDisabled("Share both with the people you want to invite.");
		}

		// Port forwarding is reported separately from reachability: the two can
		// disagree, and the user needs to know WHICH step failed to know what to
		// do about it.
		if (!collab->portMapStatus().empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text,
			                      collab->portMapped() ? ImVec4(0.6f, 0.85f, 0.6f, 1.0f)
			                                           : ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
			ImGui::TextWrapped("%s", collab->portMapStatus().c_str());
			ImGui::PopStyleColor();
			if (collab->portMapped())
				ImGui::TextDisabled("The forward is removed again when you leave.");
		}

		if (!collab->directoryStatus().empty())
		{
			ImGui::Spacing();
			// Reachability decides whether anyone outside this network can get in,
			// so an unreachable host is called out rather than left to fail as an
			// unexplained timeout on the guest's side.
			const bool bad = collab->reachabilityKnown() && !collab->reachable();
			ImGui::PushStyleColor(ImGuiCol_Text, bad ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f)
			                                         : ImVec4(0.6f, 0.85f, 0.6f, 1.0f));
			ImGui::TextWrapped("%s", collab->directoryStatus().c_str());
			ImGui::PopStyleColor();
		}

		// The two lines above state different facts, but share one remedy — so it
		// is printed once here rather than appended to each, which had the panel
		// repeating the same paragraph back to back.
		if (!collab->connectivityAdvice().empty())
		{
			ImGui::Spacing();
			ImGui::TextWrapped("%s", collab->connectivityAdvice().c_str());
		}

		ImGui::Spacing();
		// Two different machines' addresses, so they are labelled as such. Showing
		// the directory-observed address as "local" implied this machine sits on
		// the internet, when on any NAT it is the router that does.
		ImGui::TextDisabled("This machine: %s:%u", collab->localAddress().c_str(),
		                    static_cast<unsigned>(collab->port()));
		if (!collab->publicAddress().empty())
		{
			// Whose address this is cannot be told from its family. "IPv6, so it
			// must be the machine" holds only while nothing translates — and
			// NAT66 / prefix translation, though rare on consumer routers, is
			// real. Asserting otherwise would tell the user no forwarding is
			// needed in exactly the case where it is.
			//
			// So it is decided by asking whether this machine actually holds the
			// address, compared against ALL of its addresses: with privacy
			// extensions a machine has a stable AND a temporary global address
			// and reaches the outside under the temporary one, so checking only
			// the address hosting prefers would call our own address foreign.
			const std::string& seen = collab->publicAddress();
			const bool ours   = HE::Net::socketOwnsAddress(seen);
			const bool isIPv6 = seen.find(':') != std::string::npos;

			if (ours)
				ImGui::TextDisabled("Reachable from outside as: %s (this machine — no "
				                    "forwarding needed)", seen.c_str());
			else if (isIPv6)
				// IPv6 and yet not ours: something between here and the internet
				// is rewriting addresses, so this behaves like NAT after all.
				ImGui::TextDisabled("Seen from outside as: %s (your router — it is "
				                    "translating IPv6 addresses)", seen.c_str());
			else
				ImGui::TextDisabled("Seen from outside as: %s (your router)", seen.c_str());
		}
	}
	else
	{
		EditorWidgets::WrapText wrap;

		ImGui::SeparatorText("Joining");

		if (collab->directoryBusy())
		{
			ImGui::TextUnformatted("Looking up session...");
		}
		else if (collab->snapshotInProgress())
		{
			ImGui::TextUnformatted("Receiving scene...");
			ImGui::ProgressBar(collab->snapshotProgress(), ImVec2(-1, 0));
		}
		else if (collab->status() == CollabController::Status::Joined)
		{
			ImGui::TextUnformatted("Connected.");
		}
		else
		{
			ImGui::TextUnformatted("Connecting...");
		}

		if (!collab->directoryStatus().empty())
			ImGui::TextDisabled("%s", collab->directoryStatus().c_str());
	}

	if (collab->active())
	{
		EditorWidgets::WrapText wrap;

		// Identical on every participant's screen, so a mismatch means someone is
		// not in the session they think they are.
		if (const std::string fp = collab->sessionFingerprint(); !fp.empty())
		{
			ImGui::Spacing();
			ImGui::Text("Session fingerprint: %s", fp.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Should read the same for everyone in the session.");
		}

		// The HOST's rule, which is what a guest is actually living under — its
		// own setting decided only whether it was let in. Shown for both roles
		// and from the same accessor, so a guest reading "larger assets are
		// shared" is reading the session and not its own preference.
		if (collab->sessionSyncsLargeAssets())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
				"This session also transfers larger assets (meshes, textures, audio).");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"The host decided this when the session was opened, and it cannot\n"
					"change while the session runs. It can use considerably more data\n"
					"than an ordinary session.");
			}
		}

		// The identity is fixed for the life of the session (it was read when the
		// session started), so it is shown but not editable.
		DrawIdentity(ctx, false);

		ImGui::SeparatorText("Participants");
		// Same list, same actions as the footer's hover menu — one implementation,
		// so the two cannot end up offering different buttons.
		CollabPresenceBar::DrawRoster(ctx);

		// ── Requests waiting on you ───────────────────────────────────────
		// A list, deliberately, and not a dialog: with several participants a
		// modal per request would turn hosting into clicking things away. Rows
		// accumulate, they wait as long as they have to, and none of them takes
		// the keyboard.
		if (collab->isHost() && !collab->pendingAssetOps().empty())
		{
			const auto& ops = collab->pendingAssetOps();
			ImGui::Spacing();
			const std::string title = ops.size() == 1
				? std::string("1 request")
				: std::to_string(ops.size()) + " requests";
			ImGui::SeparatorText(title.c_str());
			ImGui::TextDisabled("Nothing happens to these until you say so.");
			ImGui::Spacing();

			// Answered AFTER the loop: approving mutates the vector being drawn.
			int approveIdx = -1, denyIdx = -1;
			// The same, for a whole bundle. Kept as (owner, batch) rather than as
			// a list of indices for the reason approveAssetOpBatch exists: every
			// answer erases a row and shifts the ones behind it.
			HE::Net::ParticipantId batchOwner = 0;
			std::uint32_t          batchId    = 0;
			bool                   batchApprove = false;

			// ── One user action, one row ──────────────────────────────────
			// Twenty assets selected and deleted at once used to arrive as
			// twenty rows: the same decision, twenty clicks, and nothing on
			// screen saying they belonged together. Rows that share a batch are
			// drawn as ONE collapsible row with its own Approve all / Deny all,
			// openable into the files — where each still has its own buttons,
			// because "all but that one" is a real answer and the queue would be
			// worse for making it impossible.
			//
			// A bundle of one is not a bundle: it is drawn exactly like a row
			// that was asked for on its own, which is what it is to the person
			// looking at it.
			std::vector<char> drawn(ops.size(), 0);   // rows a bundle already took

			// Per-file row, shared by the two shapes so a file inside a bundle
			// reads the same as a file on its own.
			auto drawOpRow = [&](std::size_t i) {
				const auto& op = ops[i];
				ImGui::PushID(static_cast<int>(i));

				const std::string name =
					std::filesystem::path(op.path).filename().string();
				const bool isDelete =
					op.op == HE::Net::CollabSession::AssetOp::Delete;
				// "Delete folder" is a different decision from "Delete" and has
				// to read like one: approving it removes everything underneath,
				// with no trash and no undo. The row said neither until a review
				// pointed out that the flag was carried and never shown.
				ImGui::TextColored(isDelete ? ImVec4(1.00f, 0.55f, 0.45f, 1.0f)
				                            : ImVec4(1.00f, 0.80f, 0.45f, 1.0f),
				                   isDelete ? (op.folder ? "Delete folder" : "Delete")
				                            : (op.folder ? "Rename folder" : "Rename"));
				ImGui::SameLine();
				ImGui::TextUnformatted(name.c_str());
				if (op.folder && isDelete)
				{
					ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.45f, 1.0f),
						"    and everything inside it");
				}
				if (!isDelete && !op.newPath.empty())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("\xE2\x86\x92 %s",
						std::filesystem::path(op.newPath).filename().string().c_str());
				}
				// The age, so a request that has been sitting there looks like it
				// has, instead of looking freshly arrived every time you glance.
				if (op.firstAskedMs > 0 && collab->nowMs() > op.firstAskedMs)
				{
					const unsigned long long secs =
						(collab->nowMs() - op.firstAskedMs) / 1000ull;
					ImGui::SameLine();
					if (secs < 60) ImGui::TextDisabled("(%llus)", secs);
					else           ImGui::TextDisabled("(%llum)", secs / 60ull);
				}

				// Who asked, as faces. A name is a string; a face is the person
				// you can go and ask about it. Drawn with the roster's own
				// helper, so a request shows the same picture the participant
				// list does rather than a second rendering of it.
				for (const auto& rq : op.requesters)
				{
					for (const HE::Net::Participant& p : collab->participants())
					{
						if (p.id != rq.id) continue;
						CollabPresenceBar::DrawAvatar(ctx, p, 22.0f);
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s asked for this", p.name.c_str());
						ImGui::SameLine();
						break;
					}
				}
				// The faces were all drawn with SameLine after them; close the row.
				ImGui::NewLine();

				if (EditorWidgets::primaryButton("Approve", ImVec2(110, 0)))
					approveIdx = static_cast<int>(i);
				ImGui::SameLine();
				if (EditorWidgets::cancelButton("Deny", ImVec2(110, 0)))
					denyIdx = static_cast<int>(i);

				ImGui::PopID();
			};

			for (std::size_t i = 0; i < ops.size(); ++i)
			{
				if (drawn[i]) continue;
				const auto& op = ops[i];

				// Everything else in this bundle. Collected first so the header
				// can say how many there are before any of them is drawn.
				std::vector<std::size_t> group;
				group.push_back(i);
				if (op.inBatch())
				{
					for (std::size_t j = i + 1; j < ops.size(); ++j)
					{
						if (ops[j].batchId != op.batchId ||
						    ops[j].batchOwner != op.batchOwner) continue;
						group.push_back(j);
						drawn[j] = 1;
					}
				}

				if (group.size() == 1)
				{
					drawOpRow(i);
					if (i + 1 < ops.size()) ImGui::Separator();
					continue;
				}

				// All deletes, all renames, or a mixture — the header has to say
				// what is being agreed to, and "20 requests" says the least of
				// the three.
				bool allDelete = true, allRename = true;
				for (const std::size_t k : group)
				{
					if (ops[k].op == HE::Net::CollabSession::AssetOp::Delete)
						allRename = false;
					else
						allDelete = false;
				}
				// Verb first, like the single rows above it, and never the bare
				// count on its own: "20 requests" is the one phrasing that does
				// not say what agreeing to it would do.
				const std::string headline =
					(allDelete ? "Delete " : allRename ? "Rename " : "Answer ") +
					std::to_string(group.size()) + " assets";

				ImGui::PushID(static_cast<int>(2000 + i));
				const bool open = ImGui::TreeNodeEx(
					headline.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
				// Who asked, once for the bundle: every row in it came from the
				// same person by construction (the batch id is theirs), so
				// repeating the face on each file would be twenty copies of one
				// fact.
				for (const HE::Net::Participant& p : collab->participants())
				{
					if (p.id != op.batchOwner) continue;
					ImGui::SameLine();
					CollabPresenceBar::DrawAvatar(ctx, p, 22.0f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s asked for all of these", p.name.c_str());
					break;
				}
				// On the header's own line, not under it: an open node has
				// already indented everything that follows, so buttons drawn
				// below would sit somewhere different depending on whether the
				// bundle happens to be open — and they answer it either way.
				ImGui::SameLine();
				if (EditorWidgets::primaryButton("Approve all", ImVec2(110, 0)))
				{
					batchOwner   = op.batchOwner;
					batchId      = op.batchId;
					batchApprove = true;
				}
				ImGui::SameLine();
				if (EditorWidgets::cancelButton("Deny all", ImVec2(110, 0)))
				{
					batchOwner   = op.batchOwner;
					batchId      = op.batchId;
					batchApprove = false;
				}
				if (open)
				{
					for (std::size_t k = 0; k < group.size(); ++k)
					{
						drawOpRow(group[k]);
						if (k + 1 < group.size()) ImGui::Separator();
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
				if (i + 1 < ops.size()) ImGui::Separator();
			}
			// ONE answer per frame, bundle answers included: each of these
			// rewrites the vector the loop above just walked, and a second
			// answer would be aimed at rows that have moved.
			if (batchId != 0)
			{
				if (batchApprove) collab->approveAssetOpBatch(batchOwner, batchId);
				else              collab->denyAssetOpBatch(batchOwner, batchId);
			}
			else if (approveIdx >= 0)
				collab->approveAssetOp(static_cast<std::size_t>(approveIdx));
			else if (denyIdx >= 0)
				collab->denyAssetOp(static_cast<std::size_t>(denyIdx));
		}
		// ── Assets someone wants from you ────────────────────────────────
		// Not gated on being host: this queue belongs to whoever HOLDS the
		// asset, and holding a lock is not a role. The host answers deletes
		// and renames because they destroy work; an edit request interrupts
		// work, so it goes to the person being interrupted.
		if (!collab->pendingEditRequests().empty())
		{
			const auto& reqs = collab->pendingEditRequests();
			ImGui::Spacing();
			const std::string reqTitle = reqs.size() == 1
				? std::string("1 asset request")
				: std::to_string(reqs.size()) + " asset requests";
			ImGui::SeparatorText(reqTitle.c_str());
			ImGui::TextDisabled("You are editing these. Nothing changes hands "
			                    "unless you say so.");
			ImGui::Spacing();

			int allowIdx = -1, refuseIdx = -1;
			for (std::size_t i = 0; i < reqs.size(); ++i)
			{
				const auto& rq = reqs[i];
				ImGui::PushID(static_cast<int>(1000 + i));

				bool drewAsker = false;
				for (const HE::Net::Participant& p : collab->participants())
				{
					if (p.id != rq.id) continue;
					CollabPresenceBar::DrawAvatar(ctx, p, 22.0f);
					ImGui::SameLine();
					ImGui::TextUnformatted(p.name.c_str());
					ImGui::SameLine();
					drewAsker = true;
					break;
				}
				// Left the session between asking and now. The row stays: the
				// request is still answerable, and an unlabelled one would read
				// as a bug rather than as somebody who stepped away.
				if (!drewAsker) { ImGui::TextDisabled("Someone"); ImGui::SameLine(); }
				ImGui::TextDisabled("wants");
				ImGui::SameLine();
				ImGui::TextUnformatted(
					std::filesystem::path(rq.path).filename().string().c_str());
				if (rq.askedMs > 0 && collab->nowMs() > rq.askedMs)
				{
					const unsigned long long secs =
						(collab->nowMs() - rq.askedMs) / 1000ull;
					ImGui::SameLine();
					if (secs < 60) ImGui::TextDisabled("(%llus)", secs);
					else           ImGui::TextDisabled("(%llum)", secs / 60ull);
				}

				// "Hand over", not "Approve": saying yes here gives the asset
				// away, and the button should say what it does.
				if (EditorWidgets::primaryButton("Hand over", ImVec2(110, 0)))
					allowIdx = static_cast<int>(i);
				ImGui::SameLine();
				if (EditorWidgets::cancelButton("Keep it", ImVec2(110, 0)))
					refuseIdx = static_cast<int>(i);

				ImGui::PopID();
				if (i + 1 < reqs.size()) ImGui::Separator();
			}
			if (allowIdx >= 0)
				collab->answerEditRequest(static_cast<std::size_t>(allowIdx), true);
			else if (refuseIdx >= 0)
				collab->answerEditRequest(static_cast<std::size_t>(refuseIdx), false);
		}

		// The other side of the same exchange: what WE are waiting for. The
		// host can be waiting too now — it asks the holder for an asset like
		// anyone else — so this is no longer a client-only line, and it must
		// not name the host as the one deciding.
		if (collab->hasPendingRequestOfOurs())
		{
			ImGui::Spacing();
			if (collab->pendingRequestsOfOurs() == 1)
				ImGui::TextDisabled("1 request of yours is waiting for an answer.");
			else
				ImGui::TextDisabled("%zu requests of yours are waiting for an answer.",
				                    collab->pendingRequestsOfOurs());
		}

		ImGui::Spacing();
		ImGui::Separator();
		if (ImGui::Button("Leave session", ImVec2(170, 0))) collab->leave();
	}

	// Being thrown out looks exactly like the host's network dying unless it is
	// said out loud, so it is said out loud — above the generic error below,
	// which would otherwise be the only thing on screen.
	if (!collab->removalNotice().empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextWrapped("%s", collab->removalNotice().c_str());
		ImGui::PopStyleColor();
		if (ImGui::Button("OK##removal")) collab->clearRemovalNotice();
	}

	if (collab->status() == CollabController::Status::Failed &&
	    !collab->lastError().empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
		ImGui::TextWrapped("%s", collab->lastError().c_str());
		ImGui::PopStyleColor();
		if (ImGui::Button("Dismiss")) collab->leave();
	}

	ImGui::End();
#else
	(void)ctx; (void)open;
#endif
}

} // namespace CollabPanel
