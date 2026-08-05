#include "CollabPresenceBar.h"

#include "CollabController.h"
#include "EditorApplication.h"
#include "EditorWidgets.h"

#include <Renderer/IRenderer.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabPresenceBar
{
#ifdef HE_IMGUI_ENABLED
namespace
{
	// ── Avatar textures ──────────────────────────────────────────────────────
	// Pictures arrive as raw RGBA in the roster and have to be uploaded once to
	// be drawn. Keyed by participant id, with a hash of the pixels alongside so a
	// participant id being reused by a later session cannot leave the previous
	// person's face on screen.
	//
	// kLocalKey is outside the participant id space (ids start at 1), so the
	// local preview shares this cache instead of needing its own.
	constexpr std::uint32_t kLocalKey = 0xFFFFFFFFu;

	struct CachedAvatar
	{
		void*         handle = nullptr;   // renderer-owned; ImTextureID underneath
		std::uint64_t hash   = 0;
		int           size   = 0;
	};
	std::unordered_map<std::uint32_t, CachedAvatar> s_avatars;
	IRenderer* s_owner = nullptr;   // whose textures those are, for shutdown

	std::uint64_t hashPixels(const std::vector<std::uint8_t>& px)
	{
		// FNV-1a. Only ever compared against itself to answer "same picture?",
		// so speed matters and cryptographic strength does not.
		std::uint64_t h = 1469598103934665603ull;
		for (const std::uint8_t b : px)
		{
			h ^= b;
			h *= 1099511628211ull;
		}
		return h;
	}

	void releaseAll()
	{
		if (s_owner)
		{
			for (auto& [key, entry] : s_avatars)
			{
				if (entry.handle) s_owner->DestroyImGuiTexture(entry.handle);
			}
		}
		s_avatars.clear();
	}

	// The uploaded texture for a picture, uploading or re-uploading as needed.
	// Null when there is no picture, or no renderer to upload through.
	ImTextureID avatarTexture(IRenderer* renderer, std::uint32_t key,
	                          const std::vector<std::uint8_t>& rgba, int size)
	{
		if (!renderer || size <= 0 || rgba.empty()) return 0;

		// A renderer swap (project restart, backend change) invalidates every
		// handle we hold. Dropping them wholesale is right: the old renderer is
		// gone, so they cannot be released individually anyway.
		if (s_owner != renderer)
		{
			s_avatars.clear();
			s_owner = renderer;
		}

		const std::uint64_t hash = hashPixels(rgba);
		CachedAvatar& entry = s_avatars[key];
		if (entry.handle && entry.hash == hash && entry.size == size)
			return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(entry.handle));

		if (entry.handle) renderer->DestroyImGuiTexture(entry.handle);
		entry.handle = renderer->CreateImGuiTexture(rgba.data(), size, size);
		entry.hash   = hash;
		entry.size   = size;
		return entry.handle
			? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(entry.handle))
			: ImTextureID(0);
	}

	// Everyone whose picture we no longer have any reason to hold. Without this a
	// long-lived editor accumulates a texture per person who ever joined.
	void evictDeparted(const std::vector<HE::Net::Participant>& roster)
	{
		for (auto it = s_avatars.begin(); it != s_avatars.end(); )
		{
			if (it->first == kLocalKey) { ++it; continue; }
			const bool present = std::any_of(roster.begin(), roster.end(),
				[&](const HE::Net::Participant& p) { return p.id == it->first; });
			if (present) { ++it; continue; }
			if (s_owner && it->second.handle) s_owner->DestroyImGuiTexture(it->second.handle);
			it = s_avatars.erase(it);
		}
	}

	// ── Drawing ──────────────────────────────────────────────────────────────

	ImU32 participantColorU32(HE::Net::ParticipantId id, float alpha = 1.0f)
	{
		float rgb[3];
		CollabController::participantColor(id, rgb);
		return ImGui::GetColorU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
	}

	// A round portrait with a ring in the participant's colour — the same colour
	// their camera gizmo and lock badges use, so the footer and the viewport can
	// be read as one thing rather than two unrelated colour schemes.
	void drawAvatarAt(ImDrawList* dl, ImVec2 topLeft, float size, ImTextureID tex,
	                  HE::Net::ParticipantId id, const std::string& name, bool dim)
	{
		const float  radius = size * 0.5f;
		const ImVec2 centre(topLeft.x + radius, topLeft.y + radius);
		const float  alpha  = dim ? 0.45f : 1.0f;

		if (tex)
		{
			// Rounded to exactly half the side, which is a circle. Drawn slightly
			// inside the ring so the two do not fight over the same pixels.
			dl->AddImageRounded(tex, topLeft, ImVec2(topLeft.x + size, topLeft.y + size),
			                    ImVec2(0, 0), ImVec2(1, 1),
			                    ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), radius);
		}
		else
		{
			// No picture: their colour, with the initial on top. Better than a
			// generic silhouette — the initial and the colour together are enough
			// to tell three people apart at 18 pixels.
			dl->AddCircleFilled(centre, radius, participantColorU32(id, 0.55f * alpha), 24);

			const char  initial[2] = { name.empty() ? '?' : name[0], '\0' };
			const ImVec2 ts = ImGui::CalcTextSize(initial);
			dl->AddText(ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f),
			            ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), initial);
		}

		dl->AddCircle(centre, radius, participantColorU32(id, alpha), 24, 1.5f);
	}

	// ── Hover-menu state ─────────────────────────────────────────────────────
	// Written by DrawFooter, read by DrawOverlay one step later in the same frame
	// (see the header for why the two are separate).
	ImVec2 s_clusterMin { 0.0f, 0.0f };
	ImVec2 s_clusterMax { 0.0f, 0.0f };
	bool   s_clusterHovered = false;
	bool   s_clusterDrawn   = false;
	// Clicking pins the menu open, so its buttons can be reached without keeping
	// the mouse inside a corridor the whole way there.
	bool   s_pinned = false;
	bool   s_menuOpen = false;

	// The ban confirmation, raised from the menu. The id is resolved to a name
	// when the dialog opens rather than when it is confirmed: the participant may
	// leave while the question is on screen, and a dialog that suddenly says
	// "Ban ?" would be worse than one naming someone who is already gone.
	HE::Net::ParticipantId s_banCandidate = HE::Net::kInvalidParticipant;
	std::string            s_banCandidateName;
	bool                   s_openBanModal = false;

	// Cluster geometry. Enough faces to tell at a glance who is here, then a
	// count. A session is capped at eight, so the overflow is rare — but a footer
	// that grows a row of portraits until it collides with the FPS counter is not
	// a footer.
	constexpr int   kMaxShown = 4;
	constexpr float kFaceSize = 18.0f;
	// Set apart rather than overlapped in the usual stacked-avatar style: an
	// overlap needs each face plugged with the bar's own background colour to
	// keep the seam clean, and the footer pushes that colour and pops it again
	// before anything is drawn — so the plug would silently be the wrong shade
	// the day someone restyles the footer.
	constexpr float kFaceGap  = 3.0f;

	// Split out so FooterWidth() and DrawFooter() cannot disagree about the
	// layout — which would show up as a cluster that overlaps the FPS counter by
	// exactly one avatar.
	struct ClusterLayout
	{
		int   shown  = 0;
		int   extra  = 0;
		float width  = 0.0f;
		float extraW = 0.0f;
		char  extraLabel[16] = {};
	};

	ClusterLayout layoutFor(std::size_t participants)
	{
		ClusterLayout l;
		if (participants == 0) return l;
		l.shown = std::min<int>(kMaxShown, static_cast<int>(participants));
		l.extra = static_cast<int>(participants) - l.shown;
		if (l.extra > 0)
		{
			std::snprintf(l.extraLabel, sizeof(l.extraLabel), "+%d", l.extra);
			l.extraW = ImGui::CalcTextSize(l.extraLabel).x + 4.0f;
		}
		l.width = l.shown * kFaceSize + (l.shown - 1) * kFaceGap + l.extraW;
		return l;
	}

	const char* selectionSummary(const HE::Net::PresenceState* presence, char* buf,
	                             std::size_t bufSize)
	{
		if (!presence || !presence->valid) return "not reporting a view yet";
		const std::size_t n = presence->selection.size();
		if (n == 0) return "nothing selected";
		std::snprintf(buf, bufSize, "%zu object%s selected", n, n == 1 ? "" : "s");
		return buf;
	}
} // namespace
#endif // HE_IMGUI_ENABLED

// ─────────────────────────────────────────────────────────────────────────────

float FooterWidth(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	CollabController* collab = ctx.collab;
	if (!collab || !collab->inSession()) return 0.0f;
	return layoutFor(collab->participants().size()).width;
#else
	(void)ctx;
	return 0.0f;
#endif
}

void DrawFooter(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	s_clusterDrawn = false;

	CollabController* collab = ctx.collab;
	if (!collab || !collab->inSession()) { s_menuOpen = false; s_pinned = false; return; }

	const auto& roster = collab->participants();
	if (roster.empty()) return;
	evictDeparted(roster);

	const ClusterLayout layout = layoutFor(roster.size());

	// One hit box for the whole cluster: the menu belongs to the group, not to
	// whichever face the mouse happens to land on.
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##collabPresence", ImVec2(layout.width, kFaceSize));
	const bool hovered = ImGui::IsItemHovered();
	if (ImGui::IsItemClicked()) s_pinned = !s_pinned;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const auto localId = collab->localParticipant();
	for (int i = 0; i < layout.shown; ++i)
	{
		const HE::Net::Participant& p = roster[static_cast<std::size_t>(i)];
		const ImVec2 at(origin.x + i * (kFaceSize + kFaceGap), origin.y);
		drawAvatarAt(dl, at, kFaceSize,
		             avatarTexture(ctx.renderer, p.id, p.avatar.rgba, p.avatar.size),
		             p.id, p.name, p.id == localId);
	}
	if (layout.extra > 0)
	{
		dl->AddText(ImVec2(origin.x + layout.width - layout.extraW + 4.0f,
		                   origin.y + (kFaceSize - ImGui::GetTextLineHeight()) * 0.5f),
		            ImGui::GetColorU32(ImGuiCol_TextDisabled), layout.extraLabel);
	}

	s_clusterMin     = origin;
	s_clusterMax     = ImVec2(origin.x + layout.width, origin.y + kFaceSize);
	s_clusterHovered = hovered;
	s_clusterDrawn   = true;
#else
	(void)ctx;
#endif
}

#ifdef HE_IMGUI_ENABLED
namespace
{
	// The hover menu itself. Returns whether the mouse is inside it, which is
	// what keeps it from closing the instant the pointer leaves the cluster.
	bool drawMenuWindow(AppContext& ctx, CollabController& collab)
	{
		// Anchored to sit directly on top of the cluster with no gap, so the
		// mouse can travel from one to the other without crossing a third window
		// and closing the menu out from under itself.
		constexpr float kWidth = 320.0f;
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const float x = std::max(vp->WorkPos.x + 4.0f,
		                         std::min(s_clusterMax.x - kWidth,
		                                  vp->WorkPos.x + vp->WorkSize.x - kWidth - 4.0f));

		ImGui::SetNextWindowPos(ImVec2(x, s_clusterMin.y), ImGuiCond_Always,
		                        ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0f), ImGuiCond_Always);
		ImGui::SetNextWindowViewport(vp->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));

		bool hovered = false;
		if (ImGui::Begin("##collabPresenceMenu", nullptr,
		                 ImGuiWindowFlags_NoTitleBar         |
		                 ImGuiWindowFlags_NoResize           |
		                 ImGuiWindowFlags_NoMove             |
		                 ImGuiWindowFlags_NoScrollbar        |
		                 ImGuiWindowFlags_NoSavedSettings    |
		                 ImGuiWindowFlags_NoDocking          |
		                 ImGuiWindowFlags_NoFocusOnAppearing |
		                 ImGuiWindowFlags_AlwaysAutoResize))
		{
			hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
			                                 ImGuiHoveredFlags_ChildWindows);
			ImGui::TextDisabled("%zu in this session", collab.participants().size());
			ImGui::Separator();
			DrawRoster(ctx);
		}
		ImGui::End();
		ImGui::PopStyleVar();
		return hovered;
	}

	// The ban confirmation. Returns true while it is on screen, so the menu
	// underneath does not close and take the dialog's context with it.
	bool drawBanModal(CollabController& collab)
	{
		if (s_openBanModal)
		{
			// Opened here rather than at the button: a popup has to be opened at
			// the same ID-stack level it is drawn at, and the buttons live in two
			// different windows.
			ImGui::OpenPopup("Block participant##collab");
			s_openBanModal = false;
		}

		// Pinned like every other editor dialog: a modal that ImGui hands its own
		// OS window can end up behind the editor, invisible and still eating
		// every click (see EditorWidgets::raiseDetachedModals).
		EditorWidgets::pinDialogToEditorWindow(ImVec2(380.0f, 0.0f));
		if (!ImGui::BeginPopupModal("Block participant##collab", nullptr,
		                            ImGuiWindowFlags_AlwaysAutoResize))
		{
			return false;
		}

		ImGui::TextWrapped("Remove %s from this session and refuse them if they try to "
		                   "rejoin?", s_banCandidateName.c_str());
		ImGui::Spacing();
		ImGui::TextDisabled("The block lasts until you close this session. It does not "
		                    "affect a session you open later, and you can lift it again "
		                    "from the participant menu.");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
		{
			s_banCandidate = HE::Net::kInvalidParticipant;
			s_pinned       = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.60f, 0.20f, 0.20f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.26f, 0.26f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.52f, 0.16f, 0.16f, 1.0f));
		if (ImGui::Button("Block", ImVec2(110.0f, 0.0f)))
		{
			// They may have left while the question was on screen —
			// banParticipant says so by returning false, and there is nothing
			// left to do about it either way.
			collab.banParticipant(s_banCandidate);
			s_banCandidate = HE::Net::kInvalidParticipant;
			s_pinned       = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::PopStyleColor(3);
		ImGui::EndPopup();
		return true;
	}
} // namespace
#endif // HE_IMGUI_ENABLED

void DrawOverlay(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	CollabController* collab = ctx.collab;
	if (!collab || !collab->inSession())
	{
		s_menuOpen     = false;
		s_pinned       = false;
		s_banCandidate = HE::Net::kInvalidParticipant;
		return;
	}

	if (s_clusterHovered) s_menuOpen = true;

	bool menuHovered = false;
	// The menu is optional — the confirmation is not. It can be raised from the
	// Collaboration window too, where no footer cluster is involved at all.
	if (s_clusterDrawn && (s_menuOpen || s_pinned))
		menuHovered = drawMenuWindow(ctx, *collab);
	if (drawBanModal(*collab)) menuHovered = true;

	if (!s_clusterHovered && !menuHovered && !s_pinned) s_menuOpen = false;
	// A click anywhere else unpins, the way a menu behaves everywhere else.
	if (s_pinned && !s_clusterHovered && !menuHovered &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		s_pinned   = false;
		s_menuOpen = false;
	}
#else
	(void)ctx;
#endif
}

void DrawRoster(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	CollabController* collab = ctx.collab;
	if (!collab) return;

	const auto& roster  = collab->participants();
	const auto  localId = collab->localParticipant();
	const bool  isHost  = collab->isHost();

	if (roster.empty())
	{
		ImGui::TextDisabled("(nobody here yet)");
		return;
	}

	for (const HE::Net::Participant& p : roster)
	{
		ImGui::PushID(static_cast<int>(p.id));

		constexpr float kRow = 28.0f;
		DrawAvatar(ctx, p, kRow);
		ImGui::SameLine(0.0f, 8.0f);

		ImGui::BeginGroup();
		ImGui::TextUnformatted(p.name.c_str());
		if (p.isHost)
		{
			ImGui::SameLine(0.0f, 6.0f);
			ImGui::TextDisabled("host");
		}
		if (p.id == localId)
		{
			ImGui::SameLine(0.0f, 6.0f);
			ImGui::TextDisabled("you");
		}

		// What they are doing, from the presence they publish — the same data
		// the viewport draws their camera frustum from.
		char buf[64];
		ImGui::TextDisabled("%s", selectionSummary(collab->presenceOf(p.id), buf, sizeof(buf)));
		ImGui::EndGroup();

		// Only the host can remove anyone, and never itself: it holds the
		// registry every other peer's session depends on.
		if (isHost && p.id != localId)
		{
			constexpr float kBtnW = 62.0f;
			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
			                     std::max(0.0f, ImGui::GetContentRegionAvail().x
			                                    - kBtnW * 2.0f - 4.0f));
			if (ImGui::SmallButton("Remove")) collab->kickParticipant(p.id);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("End the session for %s. They can join again.",
				                  p.name.c_str());
			ImGui::SameLine(0.0f, 4.0f);
			if (ImGui::SmallButton("Block"))
			{
				RequestBan(p.id, p.name);
				s_pinned = true;   // keep the menu up behind the dialog
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Remove %s and refuse them if they try to rejoin this "
				                  "session.", p.name.c_str());
		}

		ImGui::PopID();
		ImGui::Spacing();
	}

	// The blocklist, so a ban made in the heat of the moment stays visible and
	// reversible instead of becoming an invisible rule the host has to remember.
	if (isHost && !collab->bans().empty())
	{
		ImGui::Separator();
		ImGui::TextDisabled("Blocked for this session");
		// Copied: letting someone back in mutates the list being walked.
		const std::vector<HE::Net::CollabSession::BanEntry> blocked = collab->bans();
		for (std::size_t i = 0; i < blocked.size(); ++i)
		{
			ImGui::PushID(static_cast<int>(i));
			ImGui::TextUnformatted(blocked[i].name.empty() ? "(unnamed)"
			                                               : blocked[i].name.c_str());
			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
			                     std::max(0.0f, ImGui::GetContentRegionAvail().x - 58.0f));
			if (ImGui::SmallButton("Allow")) collab->unban(blocked[i]);
			ImGui::PopID();
		}
	}
#else
	(void)ctx;
#endif
}

void RequestBan(std::uint32_t participantId, const std::string& name)
{
#ifdef HE_IMGUI_ENABLED
	s_banCandidate     = participantId;
	s_banCandidateName = name;
	s_openBanModal     = true;
#else
	(void)participantId; (void)name;
#endif
}

void DrawAvatar(AppContext& ctx, const HE::Net::Participant& participant, float size)
{
#ifdef HE_IMGUI_ENABLED
	const ImVec2 at = ImGui::GetCursorScreenPos();
	drawAvatarAt(ImGui::GetWindowDrawList(), at, size,
	             avatarTexture(ctx.renderer, participant.id,
	                           participant.avatar.rgba, participant.avatar.size),
	             participant.id, participant.name, false);
	ImGui::Dummy(ImVec2(size, size));
#else
	(void)ctx; (void)participant; (void)size;
#endif
}

void DrawLocalAvatar(AppContext& ctx, float size)
{
#ifdef HE_IMGUI_ENABLED
	const CollabController::Identity& me = CollabController::localIdentity();

	// The local id only exists inside a session; outside one, id 1 is used purely
	// to pick the fallback colour, which is also the colour a host gets.
	const HE::Net::ParticipantId id =
		(ctx.collab && ctx.collab->inSession()) ? ctx.collab->localParticipant() : 1u;

	const ImVec2 at = ImGui::GetCursorScreenPos();
	drawAvatarAt(ImGui::GetWindowDrawList(), at, size,
	             avatarTexture(ctx.renderer, kLocalKey, me.avatarRgba, me.avatarSize),
	             id, me.name, false);
	ImGui::Dummy(ImVec2(size, size));
#else
	(void)ctx; (void)size;
#endif
}

void Shutdown(IRenderer* renderer)
{
#ifdef HE_IMGUI_ENABLED
	// Only release through the renderer that made them; a swapped-out backend
	// has already taken its own handles with it.
	if (renderer && renderer == s_owner) releaseAll();
	s_avatars.clear();
	s_owner = nullptr;
#else
	(void)renderer;
#endif
}

} // namespace CollabPresenceBar
