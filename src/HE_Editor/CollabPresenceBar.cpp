#include "CollabPresenceBar.h"

#include "CollabController.h"
#include "EditorApplication.h"
#include "EditorWidgets.h"

#include <Renderer/IRenderer.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
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

	// The colour everybody in the session agrees this person is, straight from
	// the roster the host handed out.
	ImU32 participantColorU32(const AppContext& ctx, HE::Net::ParticipantId id,
	                          float alpha = 1.0f)
	{
		float rgb[3];
		if (ctx.collab) ctx.collab->colorFor(id, rgb);
		else            CollabController::participantColor(id, rgb);
		return ImGui::GetColorU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
	}

	// How thick a participant's colour ring is at a given avatar size. Scaled so
	// an 18 px footer face, a 28 px menu row and a 26 px viewport marker all carry
	// the same visual weight, with a floor that keeps it from vanishing entirely.
	float ringWidthFor(float size) { return std::max(2.0f, size * 0.12f); }

	// The fallback initial, sized from the FACE it sits in rather than from the
	// UI font: the footer face is 18 px and the UI font is not, so drawn at full
	// size the glyph overflows the little circle and its centring is at the
	// mercy of whatever font scale the user picked. Centring needs a second
	// correction on top: CalcTextSizeA's height is the full line box, and a
	// capital only fills the ascent — centring the box leaves the letter riding
	// high, so it is nudged down by a whisker of the size to centre the LETTER.
	void drawInitial(ImDrawList* dl, const ImVec2& centre, float faceSize,
	                 const std::string& name, ImU32 col)
	{
		const char initial[2] = {
			name.empty() ? '?'
			             : static_cast<char>(std::toupper(
			                   static_cast<unsigned char>(name[0]))), '\0' };
		ImFont*      font = ImGui::GetFont();
		const float  fs   = faceSize * 0.58f;
		const ImVec2 ts   = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, initial);
		dl->AddText(font, fs,
		            ImVec2(std::floor(centre.x - ts.x * 0.5f),
		                   std::floor(centre.y - ts.y * 0.5f + fs * 0.06f)),
		            col, initial);
	}

	// A round portrait inside a ring in the participant's colour — the SAME colour
	// as their camera marker in the viewport, their selection highlight and their
	// lock badges. That is the whole point of the ring: it is the thing that says
	// "the blue camera over there is this face down here", so the two are worth
	// nothing if they are not obviously the same blue.
	//
	// The picture is inset to sit entirely INSIDE the ring rather than sharing its
	// boundary. A stroke centred on the edge puts half its width under the image;
	// at 18 px that left roughly one pixel of visible colour, which is how a frame
	// that exists in the code manages not to exist on screen.
	void drawAvatarAt(const AppContext& ctx, ImDrawList* dl, ImVec2 topLeft, float size,
	                  ImTextureID tex, HE::Net::ParticipantId id, const std::string& name,
	                  bool dim)
	{
		const float  radius = size * 0.5f;
		const ImVec2 centre(topLeft.x + radius, topLeft.y + radius);
		const float  ring   = ringWidthFor(size);
		const float  inner  = std::max(1.0f, radius - ring);
		// Only the PICTURE dims for the local user — the ring never does. It is a
		// colour key, and a key drawn at half strength reads as a different colour
		// rather than as the same one, quieter.
		const float  alpha  = dim ? 0.72f : 1.0f;

		if (tex)
		{
			// Rounding equal to the radius turns the square into a circle.
			dl->AddImageRounded(tex, ImVec2(centre.x - inner, centre.y - inner),
			                    ImVec2(centre.x + inner, centre.y + inner),
			                    ImVec2(0, 0), ImVec2(1, 1),
			                    ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), inner);
		}
		else
		{
			// No picture: their colour, with the initial on top. Better than a
			// generic silhouette — the initial and the colour together are enough
			// to tell three people apart at 18 pixels.
			dl->AddCircleFilled(centre, inner, participantColorU32(ctx, id, 0.55f * alpha), 24);

			drawInitial(dl, centre, size, name,
			            ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)));
		}

		// Stroked at the mid-line of the band it should occupy, so the ring covers
		// exactly [radius - ring, radius] and meets the picture without a seam.
		dl->AddCircle(centre, radius - ring * 0.5f, participantColorU32(ctx, id, 1.0f),
		              28, ring);
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
	// Sized to the footer's inner height (24 px bar less its 4 px of vertical
	// padding). Bigger looked better and was quietly clipped along the bottom
	// edge, which ate the very ring this is here to show.
	constexpr float kFaceSize = 16.0f;
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
		drawAvatarAt(ctx, dl, at, kFaceSize,
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

void DrawViewportMarkers(AppContext& ctx, const glm::mat4& view, const glm::mat4& proj,
                         float rectMinX, float rectMinY, float rectMaxX, float rectMaxY)
{
#ifdef HE_IMGUI_ENABLED
	CollabController* collab = ctx.collab;
	if (!collab || !collab->inSession()) return;

	const float width  = rectMaxX - rectMinX;
	const float height = rectMaxY - rectMinY;
	if (width < 32.0f || height < 32.0f) return;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(ImVec2(rectMinX, rectMinY), ImVec2(rectMaxX, rectMaxY), true);

	const auto localId = collab->localParticipant();

	constexpr float kFace   = 26.0f;   // marker diameter
	constexpr float kInset  = 22.0f;   // how far inside the border a pinned marker sits

	for (const HE::Net::Participant& p : collab->participants())
	{
		if (p.id == localId) continue;   // we are not our own guest

		const HE::Net::PresenceState* pres = collab->presenceOf(p.id);
		if (!pres || !pres->valid) continue;

		const MarkerPlacement place = PlaceMarker(
			view, proj,
			glm::vec3(pres->cameraPos[0], pres->cameraPos[1], pres->cameraPos[2]),
			rectMinX, rectMinY, rectMaxX, rectMaxY, kInset);

		const float sx = place.x;
		const float sy = place.y;
		const bool  onScreen   = place.onScreen;
		const float arrowAngle = place.arrowAngle;

		float rgb[3];
		collab->colorFor(p.id, rgb);
		const ImU32 col     = ImGui::GetColorU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
		const ImU32 colSoft = ImGui::GetColorU32(ImVec4(rgb[0], rgb[1], rgb[2], 0.35f));

		const float  radius = kFace * 0.5f;
		const ImVec2 centre(sx, sy);

		// A dark halo under everything. The viewport can be any colour at all —
		// a bright sky, a white floor — and without this the marker is legible on
		// roughly half of them.
		dl->AddCircleFilled(centre, radius + 3.0f, IM_COL32(0, 0, 0, 110), 28);

		if (onScreen)
		{
			// A tail pointing at the exact spot, so the circle reads as a pin
			// stuck into the scene rather than as something floating near it.
			const ImVec2 tip(sx, sy + radius + 9.0f);
			dl->AddTriangleFilled(ImVec2(sx - 5.0f, sy + radius - 1.0f),
			                      ImVec2(sx + 5.0f, sy + radius - 1.0f), tip, col);
		}

		// Same construction as the footer face — picture inside the ring, ring in
		// their colour — because these two are meant to be recognised as the same
		// person, and a marker whose frame is drawn differently is a marker the eye
		// has to think about.
		const float ring  = ringWidthFor(kFace);
		const float inner = std::max(1.0f, radius - ring);

		if (const ImTextureID tex =
		        avatarTexture(ctx.renderer, p.id, p.avatar.rgba, p.avatar.size))
		{
			dl->AddImageRounded(tex, ImVec2(sx - inner, sy - inner),
			                    ImVec2(sx + inner, sy + inner),
			                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, inner);
		}
		else
		{
			dl->AddCircleFilled(centre, inner, colSoft, 28);
			drawInitial(dl, centre, kFace, p.name, IM_COL32_WHITE);
		}
		dl->AddCircle(centre, radius - ring * 0.5f, col, 28, ring);

		if (!onScreen)
		{
			// An outward arrowhead just past the ring: which way to turn.
			const float ax = std::cos(arrowAngle), ay = std::sin(arrowAngle);
			const float px = -ay, py = ax;
			const ImVec2 tip(sx + ax * (radius + 12.0f), sy + ay * (radius + 12.0f));
			const ImVec2 b0(sx + ax * (radius + 3.0f) + px * 7.0f,
			                sy + ay * (radius + 3.0f) + py * 7.0f);
			const ImVec2 b1(sx + ax * (radius + 3.0f) - px * 7.0f,
			                sy + ay * (radius + 3.0f) - py * 7.0f);
			dl->AddTriangleFilled(tip, b0, b1, col);
		}

		// The name, below an on-screen marker and above a pinned one — a pinned
		// marker sits at the border, where "below" would be off the image.
		const ImVec2 ts = ImGui::CalcTextSize(p.name.c_str());
		const float  padX = 6.0f, padY = 2.0f;
		const float  labelY = onScreen ? sy + radius + 12.0f
		                               : sy - radius - 12.0f - (ts.y + padY * 2.0f);
		const ImVec2 lmin(sx - ts.x * 0.5f - padX, labelY);
		const ImVec2 lmax(sx + ts.x * 0.5f + padX, labelY + ts.y + padY * 2.0f);

		// Dark pill with a coloured edge rather than a coloured fill: white on a
		// saturated background fails for half the palette, and dark-on-colour
		// fails for the other half.
		dl->AddRectFilled(lmin, lmax, IM_COL32(18, 18, 22, 220), 4.0f);
		dl->AddRect(lmin, lmax, col, 4.0f, 0, 1.5f);
		dl->AddText(ImVec2(lmin.x + padX, lmin.y + padY), IM_COL32(240, 240, 245, 255),
		            p.name.c_str());
	}

	dl->PopClipRect();
#else
	(void)ctx; (void)view; (void)proj;
	(void)rectMinX; (void)rectMinY; (void)rectMaxX; (void)rectMaxY;
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
	drawAvatarAt(ctx, ImGui::GetWindowDrawList(), at, size,
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
	drawAvatarAt(ctx, ImGui::GetWindowDrawList(), at, size,
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

// ─────────────────────────────────────────────────────────────────────────────
// Deliberately OUTSIDE the ImGui guard: this is geometry, it needs nothing but
// glm, and keeping it reachable is what lets the Y flip and the
// behind-the-camera case be asserted instead of eyeballed.

MarkerPlacement PlaceMarker(const glm::mat4& view, const glm::mat4& proj,
                            const glm::vec3& world,
                            float rectMinX, float rectMinY,
                            float rectMaxX, float rectMaxY, float inset)
{
	MarkerPlacement out;

	const float width  = rectMaxX - rectMinX;
	const float height = rectMaxY - rectMinY;
	if (width <= 0.0f || height <= 0.0f) return out;

	const glm::vec4 viewPos = view * glm::vec4(world, 1.0f);

	// Behind the eye plane the projection is worse than useless: it mirrors the
	// point through the origin, so someone standing behind you appears in front.
	// View space answers both questions honestly — glm's convention is -Z
	// forward, so z >= 0 is behind — and its x/y stay the direction to turn in.
	const bool behind = viewPos.z >= -0.0001f;

	if (!behind)
	{
		const glm::vec4 clip = proj * viewPos;
		if (std::abs(clip.w) > 1e-6f)
		{
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			out.x = rectMinX + (ndc.x * 0.5f + 0.5f) * width;
			// Screen y grows downwards while NDC y grows upwards. The editor's
			// picking code makes the same conversion in the other direction
			// (ndc.y = 1 - 2v), and the two have to agree or a click and a marker
			// disagree about where the same point is.
			out.y = rectMinY + (1.0f - (ndc.y * 0.5f + 0.5f)) * height;
			out.onScreen = std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f;
			if (out.onScreen) return out;
		}
	}

	// Pin to the border in the direction they lie in, so the marker also answers
	// "which way do I turn". View-space x/y is that direction in screen terms,
	// with y negated because screen y points down.
	const float cx = rectMinX + width * 0.5f;
	const float cy = rectMinY + height * 0.5f;
	float dx = viewPos.x;
	float dy = -viewPos.y;
	const float len = std::sqrt(dx * dx + dy * dy);
	// Directly behind and dead centre: no direction is more right than any other,
	// so pick down — the one place a marker is least likely to land on top of
	// whatever the user is working on.
	if (len < 1e-4f) { dx = 0.0f; dy = 1.0f; }
	else             { dx /= len; dy /= len; }

	// Grow the direction until it meets the inset rectangle; whichever axis runs
	// out first is the edge it leaves through.
	const float halfW = std::max(0.0f, width  * 0.5f - inset);
	const float halfH = std::max(0.0f, height * 0.5f - inset);
	const float t = std::min(halfW / std::max(std::abs(dx), 1e-4f),
	                         halfH / std::max(std::abs(dy), 1e-4f));
	out.x = cx + dx * t;
	out.y = cy + dy * t;
	out.onScreen   = false;
	out.arrowAngle = std::atan2(dy, dx);
	return out;
}

} // namespace CollabPresenceBar
