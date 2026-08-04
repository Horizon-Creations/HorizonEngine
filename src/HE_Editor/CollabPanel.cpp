#include "CollabPanel.h"

#include "CollabController.h"

#include <Net/Socket.h>
#include "EditorApplication.h"

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
#include <string>

namespace CollabPanel
{
#ifdef HE_IMGUI_ENABLED
namespace
{
	// Panel-local input state. Not persisted across runs — a session ID is
	// short-lived by design.
	char s_displayName[64]  = "Horizon User";
	int  s_hostPort         = 7777;
	char s_joinSessionId[80] = "";
	char s_joinCode[80]      = "";

	void CopyableValue(const char* label, const std::string& value, const char* id)
	{
		ImGui::TextUnformatted(label);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.9f, 1.0f, 1.0f));
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::SmallButton(id)) ImGui::SetClipboardText(value.c_str());
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

	if (!collab->active())
	{
		ImGui::TextWrapped("Work on the same scene together. The host opens a session "
		                   "and shares its ID and join code; that is all a guest needs.");
		ImGui::Separator();
		ImGui::InputText("Display name", s_displayName, sizeof(s_displayName));

		// ── Host ──
		ImGui::SeparatorText("Host a session");
		ImGui::SetNextItemWidth(120);
		ImGui::InputInt("Port", &s_hostPort);
		s_hostPort = std::clamp(s_hostPort, 0, 65535);
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("0 lets the system pick a free port.");

		if (ImGui::Button("Open session", ImVec2(170, 0)))
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
		if (ImGui::Button("Join", ImVec2(170, 0)))
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

		ImGui::SeparatorText("Participants");
		const auto people = collab->participants();
		if (people.empty())
		{
			ImGui::TextDisabled("(none yet)");
		}
		else
		{
			const auto localId = collab->localParticipant();
			for (const auto& p : people)
			{
				ImGui::BulletText("%s%s%s", p.name.c_str(),
				                  p.isHost ? "  [host]" : "",
				                  p.id == localId ? "  (you)" : "");
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		if (ImGui::Button("Leave session", ImVec2(170, 0))) collab->leave();
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
