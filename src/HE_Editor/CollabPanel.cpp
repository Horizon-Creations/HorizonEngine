#include "CollabPanel.h"
#include "EditorWidgets.h"    // primary/danger/cancel buttons
#include "EditorHelp.h"       // "Collaboration Session/<label>" scope for the tooltips
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

	// "Label  value  [Copy]" on ONE line. The value used to sit on a line of its
	// own under the label, which made the two things a guest needs (ID and code)
	// take four lines plus a gap between them; the labels share a column so the
	// values still line up.
	void CopyableValue(const char* label, const std::string& value, const char* id)
	{
		ImGui::TextDisabled("%s", label);
		ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (EditorWidgets::smallButton(id)) ImGui::SetClipboardText(value.c_str());
	}

	// A status line: one coloured sentence, with the longer explanation in a
	// tooltip rather than a paragraph under it. The panel is docked narrow and
	// most of what it says here is "all fine" or "here is why not" — a verdict
	// first, the reasoning on demand.
	void StatusLine(const ImVec4& colour, const char* verdict, const char* detail = nullptr)
	{
		ImGui::TextColored(colour, "%s", verdict);
		if (detail && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", detail);
	}

	// SameLine only when the next item still fits in the row. A row of swatches
	// and buttons wraps to a second line on a narrow dock instead of running out
	// of the window, which is what a bare SameLine did.
	void SameLineIfFits(float nextWidth, float spacing = -1.0f)
	{
		const float sp = spacing < 0.0f ? ImGui::GetStyle().ItemSpacing.x : spacing;
		// After an item the cursor is back at the start of the NEXT line, so the
		// right edge is cursor + available, and the room left in this row is
		// measured from where the last item ended.
		const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		if (ImGui::GetItemRectMax().x + sp + nextWidth <= right)
			ImGui::SameLine(0.0f, sp);
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

	// What was actually handed out — which is the interesting number the
	// moment somebody else already had the colour you asked for.
	void ColorTakenNotice(AppContext& ctx)
	{
		if (!ctx.collab || !ctx.collab->inSession()) return;
		const HE::Net::ParticipantColor mine = CollabController::localIdentity().color;
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
		// Not "Collaboration": that scope belongs to the viewport's lock banner
		// and already owns keys of its own.
		HE::Ed::Help::Scope helpScope("Collaboration Session");
		using HE::Net::ParticipantColor;

		// One row: the label, the eight presets, Auto and the free picker. It used
		// to be a label line, a swatch line and a picker with a caption of its
		// own — the same choice spread over three lines. Each item asks whether it
		// still fits, so a narrow dock wraps the row instead of cutting it off.
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Colour");
		ImGui::BeginDisabled(!editable);

		const ParticipantColor mine = CollabController::localIdentity().color;

		constexpr float kSwatch = 18.0f;
		int index = 0;
		for (const ParticipantColor& preset : HE::Net::kParticipantPalette)
		{
			if (index == 0) ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
			else            SameLineIfFits(kSwatch, 3.0f);
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
		}

		// "Automatic" is a real choice, not the absence of one: it means the host
		// hands out whatever is free, which is the right answer for a user who
		// does not care and the only answer that never collides.
		const bool automatic = mine.unset();
		SameLineIfFits(ImGui::CalcTextSize("Auto").x + ImGui::GetStyle().FramePadding.x * 2.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, automatic ? 2.0f : 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, automatic ? 1.0f : 0.0f));
		if (EditorWidgets::button("Auto", ImVec2(0.0f, kSwatch)))
			CollabController::setLocalColor(ParticipantColor{});
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		float custom[3] = { mine.r / 255.0f, mine.g / 255.0f, mine.b / 255.0f };
		SameLineIfFits(kSwatch);
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
		EditorWidgets::helpForLabel("Custom");

		ImGui::EndDisabled();
		ColorTakenNotice(ctx);
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
		HE::Ed::Help::Scope helpScope("Collaboration Session");
		SyncDisplayNameFromIdentity();

		// In a session the identity is fixed (it was read when the session started),
		// so it shrinks to a single line: the roster below already shows the
		// avatar and colour at full size, and a disabled form here would only
		// repeat it.
		if (!editable)
		{
			const float small = ImGui::GetFrameHeight();
			CollabPresenceBar::DrawLocalAvatar(ctx, small);
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(s_displayName[0] ? s_displayName : "You");
			ImGui::SameLine();
			ImGui::TextDisabled("(changes apply at the next join)");
			ColorTakenNotice(ctx);
			if (!s_avatarError.empty())
				ImGui::TextColored(HE::Ed::Theme::Bad, "%s", s_avatarError.c_str());
			return;
		}

		ImGui::SeparatorText("You");

		// Two rows tall: the name on top, the picture buttons under it. The avatar
		// is exactly that height, so the block reads as one unit instead of a
		// 56 px portrait with a form hanging off it.
		const float avatarSize = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
		CollabPresenceBar::DrawLocalAvatar(ctx, avatarSize);
		ImGui::SameLine();

		ImGui::BeginGroup();

		// The label is the hint inside the field, not text beside it: beside it the
		// label took a third of a narrow dock and left the field cramped. The
		// help entry is attached by name, since there is no visible label to find.
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##displayname", "Display name", s_displayName, sizeof(s_displayName));
		// Written when the field is left, not on every keystroke: setLocalName
		// rewrites config.json, and a name is a dozen characters.
		if (ImGui::IsItemDeactivatedAfterEdit()) CollabController::setLocalName(s_displayName);
		EditorWidgets::helpForLabel("Display name");

		const bool hasPicture = CollabController::localIdentity().avatarSize > 0;
		if (EditorWidgets::button(hasPicture ? "Change picture..." : "Choose picture..."))
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

		ImGui::EndGroup();

		DrawColorChoice(ctx, true);

		if (!s_avatarError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Bad);
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
		HE::Ed::Help::Scope helpScope("Collaboration Session");
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
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Warn);
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
	HE::Ed::Help::Scope helpScope("Collaboration Session");

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

		DrawIdentity(ctx, true);

		// Discovery is one switch for both halves — announcing when you host,
		// listening when you do not — and it lives in Preferences > Team
		// ("Find Sessions on the Local Network"). It used to be repeated here as
		// a checkbox, which only duplicated the setting; this window just reads it.
		const bool lanOn = ctx.editorConfig.CollabLanDiscovery;
		if (!lanOn) s_lanPicked = 0;

		ImGui::Spacing();
		if (ImGui::BeginTabBar("##collabmode"))
		{
		// ── Host ──
		if (ImGui::BeginTabItem("Host"))
		{
		// Port and the button share a row: the port is a detail of the one thing
		// this tab does, not a form of its own.
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Port");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
		ImGui::InputInt("##hostport", &s_hostPort, 0, 0);
		s_hostPort = std::clamp(s_hostPort, 0, 65535);
		EditorWidgets::helpForLabel("Port");
		ImGui::SameLine();
		if (EditorWidgets::primaryButton("Open session"))
			collab->startHosting(static_cast<std::uint16_t>(s_hostPort), s_displayName);
		// The hand-rolled "(?)" that sat here carried a SetTooltip of its own,
		// which would now be a second tooltip beside the field's. helpMarker
		// draws the same mark from the same entry, so there is one sentence and
		// one place to change it.
		ImGui::SameLine();
		EditorWidgets::helpMarker("Collaboration Session/Port");

		// What the startup probe already found out about this network, said BEFORE
		// the session is opened. Hosting used to be the only way to learn that the
		// router does not forward ports — by which point the user had a live
		// session nobody could reach and no idea why.
		//
		// A verdict on one line with the reasoning in its tooltip: four sentences of
		// advice appeared here before the user had done anything at all.
		if (!ctx.routerProbe)
		{
			ImGui::TextDisabled("Checking this network…");
		}
		else
		{
			const HE::Net::RouterProbe& r = *ctx.routerProbe;
			// The way out of the warnings below is said in their tooltips, next to
			// the bad news, because "guests will not reach this machine" is exactly
			// the moment someone needs to know that people in the same room still can.
			if (r.localNetworkBlocked)
			{
				StatusLine(HE::Ed::Theme::Bad, "Local network access is blocked",
					"This application may not talk to the local network, so the router "
					"cannot be reached at all. On macOS, allow it under "
					"Privacy & Security > Local Network.");
			}
			else if (r.portForwardingAvailable())
			{
				ImGui::TextColored(HE::Ed::Theme::Ok, "Router forwards ports (%s)",
					r.upnpFound ? "UPnP" : "NAT-PMP");
			}
			else if (!r.globalIPv6.empty())
			{
				StatusLine(HE::Ed::Theme::Warn, "No port forwarding; IPv6 guests can still connect",
					"The router does not offer port forwarding, but this machine has a "
					"global IPv6 address, so guests on IPv6 can still reach it.");
			}
			else
			{
				StatusLine(HE::Ed::Theme::Warn, "No port forwarding; remote guests may not connect",
					"The router answered neither UPnP nor NAT-PMP, so guests will "
					"probably not reach this machine without a hand-made port forward. "
					"Opening a session still works; see Preferences > Editor > Tool Status. "
					"Anyone on this network can still find and join the session.");
			}
			if (r.carrierNat)
				StatusLine(HE::Ed::Theme::Warn, "Behind carrier-grade NAT",
					"Your connection is behind carrier-grade NAT, so port forwarding "
					"cannot help here. Anyone on this network can still find and join "
					"the session.");
		}

		// Whether anything is actually leaving this machine. Without it,
		// "they cannot see me" is unanswerable from here: a refused send
		// looks exactly like a send nobody happened to hear.
		if (lanOn)
		{
			const auto st = collab->lanStats();
			if (st.failed > 0 && st.sent == 0)
			{
				StatusLine(HE::Ed::Theme::Bad, "The system is refusing to send announcements",
					"On macOS this is the Local Network permission: allow it under "
					"Privacy & Security > Local Network. Otherwise it is a firewall.");
			}
			else if (st.sent > 0)
			{
				// The other direction too, which says whether the two machines
				// can hear each other at all — answerable now without giving up
				// this session to go and look.
				const std::size_t others = collab->lanSessions().size();
				if (others > 0)
					ImGui::TextDisabled("Announced %u time%s  ·  %zu other session%s audible",
					                    st.sent, st.sent == 1 ? "" : "s",
					                    others, others == 1 ? "" : "s");
				else if (st.heard > 0)
					ImGui::TextDisabled("Announced %u time%s  ·  the network carries them",
					                    st.sent, st.sent == 1 ? "" : "s");
				else
					ImGui::TextDisabled("Announced %u time%s", st.sent, st.sent == 1 ? "" : "s");
			}
		}
		ImGui::EndTabItem();
		}

		// ── Join ──
		if (ImGui::BeginTabItem("Join"))
		{

		// ── Sessions on this network ──
		// Above the manual fields on purpose: when it works it is the answer,
		// and the session id exists for the case where it does not.
		{
			if (lanOn)
			{
				const auto& found = collab->lanSessions();
				if (collab->lanBlocked())
				{
					StatusLine(HE::Ed::Theme::Bad, "Local network access is blocked",
						"This application may not use the local network. On macOS, allow "
						"it under Privacy & Security > Local Network. Until then, sessions "
						"here cannot be found — the session ID below still works.");
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
						char why[320];
						std::snprintf(why, sizeof(why),
							"Nothing at all has reached this machine on port %u. If someone "
							"IS hosting nearby, this is a firewall or, on macOS, the Local "
							"Network permission — not the session.",
							unsigned(HE::Net::LanBeacon::kPort));
						StatusLine(HE::Ed::Theme::Warn, "Nothing is reaching this machine", why);
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
							StatusLine(HE::Ed::Theme::Warn, "    different engine version",
								"One of you needs to update before this session can be joined.");
						else if (!sameProject)
							StatusLine(HE::Ed::Theme::Warn, "    a different project",
								"This session is working on another project than the one "
								"open here.");

						// What this session will cost you, on the row, before the
						// click. Only for the sessions that say so — a notice on
						// every row is a notice nobody reads, and the ones that
						// leave the big media to source control have nothing to
						// warn about.
						//
						// One amber line with the sentence in its tooltip: a tinted
						// three-line block per row made a short list as tall as the
						// whole panel.
						if (joinable && SessionAnnouncesLargeAssets(s))
						{
							// The last clause is only true while the setting is
							// off — that is when joining raises the question and
							// turns it on. Saying it to someone who already has
							// it on would promise a dialog that never comes.
							StatusLine(HE::Ed::Theme::Warn,
								"    also transfers meshes, textures and audio",
								ctx.editorConfig.CollabSyncLargeAssets
									? "This session can use considerably more data."
									: "This session can use considerably more data. Joining "
									  "turns large-asset sync on for you too.");
						}
						ImGui::PopID();
					}

					// The picked row, joined with the code typed below it.
					const auto picked = std::find_if(found.begin(), found.end(),
						[](const auto& s) { return s.instance == s_lanPicked; });
					if (picked != found.end())
					{
						ImGui::Spacing();
						// Field full width, button under it — the same shape as every
						// other form in this window. The field's hint is the "ask the
						// host" line that used to sit under them.
						ImGui::SetNextItemWidth(-FLT_MIN);
						ImGui::InputTextWithHint("##lancode", "Join code from the host",
						                         s_lanJoinCode, sizeof(s_lanJoinCode));
						EditorWidgets::helpForLabel("Join code");
						ImGui::BeginDisabled(s_lanJoinCode[0] == '\0');
						if (EditorWidgets::primaryButton("Join this session"))
						{
							s_joinRetryHint.clear();
							collab->joinSession(picked->address, picked->port,
							                    s_lanJoinCode, s_displayName);
						}
						ImGui::EndDisabled();
					}
					else if (s_lanPicked != 0)
					{
						// It expired or said goodbye while the code was being
						// typed. The field and the button disappearing without
						// a word looks like the editor ate the input.
						ImGui::TextColored(HE::Ed::Theme::Warn,
							"That session is no longer being announced.");
						s_lanPicked = 0;
					}
				}
			}
		}

		// Join by ID — the way in when discovery finds nothing, and the only one
		// through a relay server (the team server, set in Preferences > Team). Two
		// full-width fields with the button under them; the names are hints inside
		// the fields rather than labels beside them, for the width.
		ImGui::Spacing();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##joinid", "Session ID", s_joinSessionId, sizeof(s_joinSessionId));
		EditorWidgets::helpForLabel("Session ID");

		const bool canJoin = s_joinSessionId[0] != '\0' && s_joinCode[0] != '\0';
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##joincode", "Join code", s_joinCode, sizeof(s_joinCode));
		EditorWidgets::helpForLabel("Join code");
		ImGui::BeginDisabled(!canJoin);
		if (EditorWidgets::primaryButton("Join"))
		{
			s_joinRetryHint.clear();
			collab->joinBySessionId(s_joinSessionId, s_joinCode, s_displayName);
		}
		ImGui::EndDisabled();
		// Why the button is grey, on the button — AllowWhenDisabled, since a
		// tooltip that only exists on the enabled control would never be seen.
		if (!canJoin && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Both the session ID and the join code are required.");
		if (!s_joinRetryHint.empty())
			ImGui::TextColored(HE::Ed::Theme::Ok, "%s", s_joinRetryHint.c_str());

		// Joining REPLACES the open scene, which a button labelled "Join" does
		// not convey — say so before it happens, not after.
		ImGui::Spacing();
		ImGui::TextColored(HE::Ed::Theme::Warn,
			"Joining replaces your open scene with the host's. Save anything you "
			"want to keep first.");

		// What THIS editor has agreed to, said where a join is started rather
		// than only in Preferences. It changes which sessions will have us —
		// a host that carries the big assets refuses an editor that has not
		// agreed — and being refused is a worse way to find that out.
		if (ctx.editorConfig.CollabSyncLargeAssets)
		{
			ImGui::TextDisabled("Large-asset sync is on: meshes, textures and audio are "
			                    "shared, which can use considerably more data.");
		}
		ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
		}
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
		if (collab->viaRelay())
		{
			// No port here: guests reach the session through the relay by its id.
			ImGui::Text("Hosting through the relay server %s",
			            HE::Net::effectiveRelayAddress(collab->relayAddress()).c_str());
		}
		else
		{
			ImGui::Text("Listening on %s:%u",
			            collab->localAddress().empty() ? "this machine"
			                                           : collab->localAddress().c_str(),
			            unsigned(collab->port()));
		}
		if (!collab->viaRelay() && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(
				"This is the session itself (TCP), and the port guests connect to.\n"
				"Local-network discovery is a separate UDP socket on port %u —\n"
				"it only carries the \"a session exists here\" announcements.",
				unsigned(HE::Net::LanBeacon::kPort));
		}

		if (collab->directoryBusy())
		{
			ImGui::TextDisabled("Publishing session...");
		}
		else if (!collab->sessionId().empty())
		{
			// These two are everything a guest needs.
			CopyableValue("Session ID", collab->sessionId(), "Copy##sid");
			CopyableValue("Join code", collab->joinCode(), "Copy##code");
			ImGui::TextDisabled("Share both with the people you want to invite.");
		}

		// Port forwarding is reported separately from reachability: the two can
		// disagree, and the user needs to know WHICH step failed to know what to
		// do about it.
		if (!collab->portMapStatus().empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text,
			                      collab->portMapped() ? HE::Ed::Theme::Ok : HE::Ed::Theme::Warn);
			ImGui::TextWrapped("%s", collab->portMapStatus().c_str());
			ImGui::PopStyleColor();
			if (collab->portMapped() && ImGui::IsItemHovered())
				ImGui::SetTooltip("The forward is removed again when you leave.");
		}

		if (!collab->directoryStatus().empty())
		{
			// Reachability decides whether anyone outside this network can get in,
			// so an unreachable host is called out rather than left to fail as an
			// unexplained timeout on the guest's side.
			const bool bad = collab->reachabilityKnown() && !collab->reachable();
			ImGui::PushStyleColor(ImGuiCol_Text, bad ? HE::Ed::Theme::Warn : HE::Ed::Theme::Ok);
			ImGui::TextWrapped("%s", collab->directoryStatus().c_str());
			ImGui::PopStyleColor();
		}

		// The two lines above state different facts, but share one remedy — so it
		// is printed once here rather than appended to each, which had the panel
		// repeating the same paragraph back to back.
		if (!collab->connectivityAdvice().empty())
			ImGui::TextWrapped("%s", collab->connectivityAdvice().c_str());

		// Through a relay this machine has no port anybody dials; what guests
		// need is the session ID and the code above, and the relay to be the same.
		// (The address and port of a direct session are in the "Listening on" line
		// at the top; they were repeated here as "This machine".)
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
			ImGui::TextDisabled("Session fingerprint: %s", fp.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Should read the same for everyone in the session.");
		}

		// The HOST's rule, which is what a guest is actually living under — its
		// own setting decided only whether it was let in. Shown for both roles
		// and from the same accessor, so a guest reading "larger assets are
		// shared" is reading the session and not its own preference.
		if (collab->sessionSyncsLargeAssets())
		{
			ImGui::TextColored(HE::Ed::Theme::Warn,
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

				const std::string name = collab->opDisplayName(op);
				const bool isDelete =
					op.op == HE::Net::CollabSession::AssetOp::Delete;
				// "Delete folder" is a different decision from "Delete" and has
				// to read like one: approving it removes everything underneath,
				// with no trash and no undo. The row said neither until a review
				// pointed out that the flag was carried and never shown. An
				// entity says so too: it is a scene object, not a file.
				ImGui::TextColored(isDelete ? HE::Ed::Theme::Bad
				                            : HE::Ed::Theme::Warn,
				                   isDelete ? (op.folder     ? "Delete folder"
				                               : op.isEntity() ? "Delete entity"
				                                               : "Delete")
				                            : (op.folder     ? "Rename folder"
				                               : op.isEntity() ? "Rename entity"
				                                               : "Rename"));
				ImGui::SameLine();
				ImGui::TextUnformatted(name.c_str());
				if (op.folder && isDelete)
				{
					ImGui::TextColored(HE::Ed::Theme::Bad,
						"    and everything inside it");
				}
				if (!isDelete && !op.newPath.empty())
				{
					// An entity's newPath IS the new name; a file's is a path.
					ImGui::SameLine();
					ImGui::TextDisabled("\xE2\x86\x92 %s",
						op.isEntity()
							? op.newPath.c_str()
							: std::filesystem::path(op.newPath).filename().string().c_str());
				}
				// The conflict itself. Somebody is in the middle of this right
				// now, and approving takes it out from under them: a delete
				// throws their open work away, a rename changes the thing they
				// are looking at. Said on the row, before the click — the
				// requester saw the same name when they were told to ask.
				const std::string heldBy = collab->opHeldBy(op);
				if (!heldBy.empty())
				{
					ImGui::TextColored(HE::Ed::Theme::Warn,
						isDelete ? "    %s is editing it \xE2\x80\x94 their open changes go with it"
						         : "    %s is editing it right now",
						heldBy.c_str());
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

				if (EditorWidgets::primaryButton("Approve"))
					approveIdx = static_cast<int>(i);
				ImGui::SameLine();
				if (EditorWidgets::cancelButton("Deny"))
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
				bool allEntity = true, anyEntity = false;
				for (const std::size_t k : group)
				{
					if (ops[k].op == HE::Net::CollabSession::AssetOp::Delete)
						allRename = false;
					else
						allDelete = false;
					if (ops[k].isEntity()) anyEntity = true;
					else                   allEntity = false;
				}
				// Verb first, like the single rows above it, and never the bare
				// count on its own: "20 requests" is the one phrasing that does
				// not say what agreeing to it would do.
				const std::string headline =
					(allDelete ? "Delete " : allRename ? "Rename " : "Answer ") +
					std::to_string(group.size()) +
					(allEntity ? " entities" : anyEntity ? " items" : " assets");

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
				if (EditorWidgets::primaryButton("Approve all"))
				{
					batchOwner   = op.batchOwner;
					batchId      = op.batchId;
					batchApprove = true;
				}
				ImGui::SameLine();
				if (EditorWidgets::cancelButton("Deny all"))
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
				if (EditorWidgets::primaryButton("Hand over"))
					allowIdx = static_cast<int>(i);
				ImGui::SameLine();
				if (EditorWidgets::cancelButton("Keep it"))
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
		if (EditorWidgets::button("Leave session")) collab->leave();
	}

	// Being thrown out looks exactly like the host's network dying unless it is
	// said out loud, so it is said out loud — above the generic error below,
	// which would otherwise be the only thing on screen.
	if (!collab->removalNotice().empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Warn);
		ImGui::TextWrapped("%s", collab->removalNotice().c_str());
		ImGui::PopStyleColor();
		if (ImGui::Button("OK##removal")) collab->clearRemovalNotice();
	}

	if (collab->status() == CollabController::Status::Failed &&
	    !collab->lastError().empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Bad);
		ImGui::TextWrapped("%s", collab->lastError().c_str());
		ImGui::PopStyleColor();
		// A dropped link is the one failure worth offering a way straight back
		// into: the host catches a rejoiner up on everything it missed.
		if (collab->canRejoin())
		{
			if (EditorWidgets::primaryButton("Rejoin")) collab->rejoinLastSession();
			ImGui::SameLine();
		}
		if (EditorWidgets::button("Dismiss")) collab->leave();
	}

	ImGui::End();
#else
	(void)ctx; (void)open;
#endif
}

} // namespace CollabPanel
