#include "CollabPanel.h"
#include "EditorWidgets.h"    // primary/danger/cancel buttons

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

	// Why the last picture could not be used, shown until the next attempt.
	std::string s_avatarError;
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
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.9f, 1.0f, 1.0f));
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::SmallButton(id)) ImGui::SetClipboardText(value.c_str());
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

	if (!collab->active())
	{
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
		}

		// ── Join ──
		ImGui::SeparatorText("Join a session");
		ImGui::InputText("Session ID", s_joinSessionId, sizeof(s_joinSessionId));
		ImGui::InputText("Join code",  s_joinCode,      sizeof(s_joinCode));

		const bool canJoin = s_joinSessionId[0] != '\0' && s_joinCode[0] != '\0';
		ImGui::BeginDisabled(!canJoin);
		if (EditorWidgets::primaryButton("Join", ImVec2(170, 0)))
			collab->joinBySessionId(s_joinSessionId, s_joinCode, s_displayName);
		ImGui::EndDisabled();
		if (!canJoin)
			ImGui::TextDisabled("Both the session ID and the join code are required.");

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
		ImGui::SeparatorText("Hosting");

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
		// Identical on every participant's screen, so a mismatch means someone is
		// not in the session they think they are.
		if (const std::string fp = collab->sessionFingerprint(); !fp.empty())
		{
			ImGui::Spacing();
			ImGui::Text("Session fingerprint: %s", fp.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Should read the same for everyone in the session.");
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
			for (std::size_t i = 0; i < ops.size(); ++i)
			{
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
				if (i + 1 < ops.size()) ImGui::Separator();
			}
			if (approveIdx >= 0)   collab->approveAssetOp(static_cast<std::size_t>(approveIdx));
			else if (denyIdx >= 0) collab->denyAssetOp(static_cast<std::size_t>(denyIdx));
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
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextWrapped("%s", collab->removalNotice().c_str());
		ImGui::PopStyleColor();
		if (ImGui::Button("OK##removal")) collab->clearRemovalNotice();
	}

	if (collab->status() == CollabController::Status::Failed &&
	    !collab->lastError().empty())
	{
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
