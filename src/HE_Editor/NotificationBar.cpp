#include "NotificationBar.h"

#include "EditorApplication.h"   // AppContext, and NotificationStore through it

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include "EditorWidgets.h"   // WrapText — everything a notification shows is a sentence
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace NotificationBar
{
#ifdef HE_IMGUI_ENABLED
namespace
{
	using HE::Ed::NoteLevel;
	using HE::Ed::Notification;

	// ── One snapshot per frame ───────────────────────────────────────────────
	// FooterWidth, DrawFooter and DrawOverlay all need the same list, and the
	// store is behind a mutex that worker threads also take. Asking it three
	// times a frame would be three lock acquisitions AND — worse — three chances
	// to disagree: a worker posting between the width call and the draw call
	// would widen the cluster after the footer had already right-aligned against
	// the old width, which is exactly the overlap the footer's layout contract
	// exists to prevent. So the list is pulled once, keyed on the ImGui frame,
	// and the badge count and its colour are derived from that same copy rather
	// than from a separate unseenCount() call.
	//
	// A copy per frame is what NotificationStore::snapshot() is for, and the cost
	// is bounded by its k_maxEntries — this is a surface with a couple of dozen
	// rows on a bad day, not a log.
	struct Cache
	{
		int                       frame = -1;
		std::vector<Notification> entries;      // oldest first, as the store keeps them
		std::size_t               unseen = 0;
		NoteLevel                 worstUnseen = NoteLevel::Info;
		std::uint64_t             nowMs = 0;    // the clock every age on screen is measured against
	};
	Cache s_cache;

	const Cache& refresh(AppContext& ctx)
	{
		const int frame = ImGui::GetFrameCount();
		if (s_cache.frame == frame) return s_cache;

		s_cache.frame       = frame;
		s_cache.unseen      = 0;
		s_cache.worstUnseen = NoteLevel::Info;
		s_cache.nowMs       = HE::Ed::NotificationStore::nowMs();
		s_cache.entries.clear();

		if (ctx.notifications)
		{
			s_cache.entries = ctx.notifications->snapshot();
			for (const Notification& n : s_cache.entries)
			{
				if (n.seen) continue;
				++s_cache.unseen;
				if (n.level > s_cache.worstUnseen) s_cache.worstUnseen = n.level;
			}
		}
		return s_cache;
	}

	// ── Colours ──────────────────────────────────────────────────────────────
	// The amber is the same literal the collab activity line uses for "you may
	// need to act": two footer widgets sitting inches apart that mean the same
	// thing in two different oranges would read as two different states.
	ImVec4 levelColor(NoteLevel level)
	{
		switch (level)
		{
			case NoteLevel::Problem: return ImVec4(0.95f, 0.36f, 0.33f, 1.0f);
			case NoteLevel::Warning: return ImVec4(1.00f, 0.72f, 0.35f, 1.0f);
			default:                 return ImVec4(0.55f, 0.70f, 0.95f, 1.0f);
		}
	}

	// What the bell itself is tinted. Grey unless something UNSEEN earns a
	// colour — an unread Info is worth a count, not an alarm, and a bell that is
	// permanently amber because of a week-old warning is a bell nobody looks at.
	ImU32 bellColor(const Cache& c)
	{
		if (c.unseen == 0) return ImGui::GetColorU32(ImGuiCol_TextDisabled);
		if (c.worstUnseen == NoteLevel::Info)
			return ImGui::GetColorU32(ImVec4(0.72f, 0.72f, 0.76f, 1.0f));
		return ImGui::GetColorU32(levelColor(c.worstUnseen));
	}

	// ── Footer layout ────────────────────────────────────────────────────────
	// Split out so FooterWidth() and DrawFooter() cannot disagree about the item
	// sequence. EngineContentSyncBar.cpp:67 records what happens when they do:
	// the footer right-aligns everything against the width this returns, so a
	// width that describes a different cluster than the one drawn puts the whole
	// right-hand group through the FPS counter.
	//
	// The slot is exactly one text line tall on purpose. The footer is 24px with
	// 4px of padding, and a default-height framed item does not fit — it gets
	// clipped along the bottom and shifts the text baseline of everything after
	// it on the line (EngineContentSyncBar.cpp:89 has the long version). An
	// InvisibleButton of GetTextLineHeight() is, layout-wise, indistinguishable
	// from a piece of text, so the bell is painted into it with the draw list.
	constexpr float kGap = 4.0f;   // between the bell and its count

	struct BellLayout
	{
		float bellW  = 0.0f;
		float slotH  = 0.0f;
		float countW = 0.0f;
		float width  = 0.0f;
		char  countLabel[16] = {};
	};

	BellLayout layoutFor(const Cache& c)
	{
		BellLayout l;
		// The bell is always there, empty store or not. It used to hide itself
		// until the first entry, on the argument that an icon which never means
		// anything trains the eye to skip its corner — but that argument only
		// holds for a surface people already know about. This one was invisible
		// for entire sessions, so when something finally did go wrong the user was
		// being asked to notice a control they had never seen, in a corner they
		// had no reason to watch. A permanently present, greyed-out bell is how
		// "there is a place where the editor tells you things" gets learned at
		// all; what carries the alarm is its colour and count, not its existence.
		l.slotH = ImGui::GetTextLineHeight();
		// A bell is taller than it is wide; matching the slot's height would
		// leave it floating in its own whitespace.
		l.bellW = std::floor(l.slotH * 0.78f);
		l.width = l.bellW;

		if (c.unseen > 0)
		{
			std::snprintf(l.countLabel, sizeof(l.countLabel), "%zu", c.unseen);
			l.countW = ImGui::CalcTextSize(l.countLabel).x;
			l.width += kGap + l.countW;
		}
		return l;
	}

	// The bell, by hand. Deliberately NOT a font glyph: the editor's loaded font
	// is chosen for its Latin text and its ranges are not guaranteed to carry
	// U+1F514 or any of the icon-font code points, and a missing glyph in the
	// footer is an empty box where the only channel for "something went wrong"
	// was supposed to be. Every other badge in this editor is drawn the same way
	// (the Content Browser tiles, the presence rings) for the same reason.
	void drawBell(ImDrawList* dl, ImVec2 origin, float w, float h, ImU32 col)
	{
		// Spelled out rather than IM_PI, which lives in imgui_internal.h — this
		// file has no business reaching into ImGui's private header for a number.
		constexpr float kPi = 3.14159265358979323846f;

		const float cx      = origin.x + w * 0.5f;
		const float lipHalf = w * 0.46f;
		const float top     = origin.y + h * 0.18f;   // crown of the dome
		const float lipY    = origin.y + h * 0.70f;   // where the skirt ends
		const float domeR   = std::max(1.0f, lipHalf * 0.72f);
		const float domeCy  = top + domeR;

		// Body: the flared skirt and the dome as one convex outline, so the two
		// meet without a seam at any UI scale. Drawing them as two shapes leaves
		// a hairline that is invisible at 100% and obvious at 200%.
		dl->PathClear();
		dl->PathLineTo(ImVec2(cx - lipHalf, lipY));
		dl->PathArcTo(ImVec2(cx, domeCy), domeR, kPi, kPi * 2.0f, 12);
		dl->PathLineTo(ImVec2(cx + lipHalf, lipY));
		dl->PathFillConvex(col);

		// Handle on top and clapper below — without them the silhouette reads as
		// a shopping bag at 12 pixels.
		dl->AddCircleFilled(ImVec2(cx, top - h * 0.07f), std::max(1.0f, h * 0.07f), col, 8);
		dl->AddCircleFilled(ImVec2(cx, lipY + h * 0.13f), std::max(1.0f, h * 0.10f), col, 8);
	}

	// ── Flyout state ─────────────────────────────────────────────────────────
	// Written by DrawFooter, read by DrawOverlay one step later in the same
	// frame (see the header for why the two halves are separate).
	ImVec2 s_bellMin { 0.0f, 0.0f };
	ImVec2 s_bellMax { 0.0f, 0.0f };
	bool   s_bellHovered = false;
	// The FRAME the bell was drawn in, not a bool. renderEditor has paths where
	// the footer never draws at all (the project hub returns before it) while
	// renderOverlays still runs — and unlike a collab session, notifications can
	// exist in that state, because workers post from anywhere. A bool set last
	// frame would anchor the flyout to a rectangle that is no longer on screen.
	int    s_bellFrame = -1;

	// Clicking pins the flyout open, so its buttons can be reached without
	// keeping the mouse inside a corridor the whole way there.
	bool   s_pinned = false;
	bool   s_open   = false;

	// Which row said "copied", and when, so the click has an answer. ImGui's
	// clock is right here and only here: this is a UI animation on the frame
	// thread, not a fact about when something happened.
	int    s_copiedRow = -1;
	double s_copiedAt  = 0.0;

	void closeFlyout()
	{
		s_open      = false;
		s_pinned    = false;
		s_copiedRow = -1;
	}

	// ── Ages ─────────────────────────────────────────────────────────────────
	// The store stamps a steady clock (see NotificationStore::nowMs) precisely
	// because its producers are worker threads where ImGui::GetTime() is not
	// valid. That means the epoch is arbitrary — only DIFFERENCES mean anything,
	// so nothing here may ever turn whenMs into a wall-clock time.
	const char* ageText(std::uint64_t whenMs, std::uint64_t nowMs, int repeats,
	                    char* buf, std::size_t bufSize)
	{
		const std::uint64_t deltaMs = nowMs > whenMs ? nowMs - whenMs : 0;
		const std::uint64_t secs    = deltaMs / 1000;

		char age[16];
		if      (secs < 10)   std::snprintf(age, sizeof(age), "now");
		else if (secs < 60)   std::snprintf(age, sizeof(age), "%llus",
		                                    static_cast<unsigned long long>(secs));
		else if (secs < 3600) std::snprintf(age, sizeof(age), "%llum",
		                                    static_cast<unsigned long long>(secs / 60));
		else if (secs < 172800) std::snprintf(age, sizeof(age), "%lluh",
		                                    static_cast<unsigned long long>(secs / 3600));
		else                  std::snprintf(age, sizeof(age), "%llud",
		                                    static_cast<unsigned long long>(secs / 86400));

		// A collapsed repeat carries its count here rather than beside the
		// message: both are dim, secondary facts, and putting them in one dim
		// place at the row's right edge keeps the sentence itself unbroken.
		if (repeats > 1) std::snprintf(buf, bufSize, "x%d  %s", repeats, age);
		else             std::snprintf(buf, bufSize, "%s", age);
		return buf;
	}

	// A path shown at 400px has to lose something; it must not be the end. The
	// filename and its folder are what identify the asset — the leading
	// /Users/…/Project part is the same for every row in the list.
	//
	// Binary search rather than trimming a byte at a time: this runs per row per
	// frame, and a linear trim would be one CalcTextSize per character of every
	// path on screen. The cut is then nudged forward off any UTF-8 continuation
	// byte, because half of a multi-byte character is not a shorter string, it is
	// a broken one — and asset paths carry accented names routinely.
	std::string elideFront(const std::string& path, float maxW)
	{
		if (maxW <= 0.0f) return {};
		if (ImGui::CalcTextSize(path.c_str()).x <= maxW) return path;

		static const char* const kEllipsis = "\xE2\x80\xA6";   // U+2026 HORIZONTAL ELLIPSIS
		const float room = maxW - ImGui::CalcTextSize(kEllipsis).x;
		if (room <= 0.0f) return kEllipsis;

		// Width falls monotonically as the cut moves right, and the empty tail
		// always fits — so the smallest cut that fits can be bisected for.
		std::size_t lo = 0, hi = path.size();
		while (lo < hi)
		{
			const std::size_t mid = lo + (hi - lo) / 2;
			if (ImGui::CalcTextSize(path.c_str() + mid).x <= room) hi = mid;
			else                                                   lo = mid + 1;
		}
		while (lo < path.size() &&
		       (static_cast<unsigned char>(path[lo]) & 0xC0) == 0x80) ++lo;

		return std::string(kEllipsis) + path.substr(lo);
	}

	// ── The flyout ───────────────────────────────────────────────────────────
	// Returns whether the mouse is inside it, which is what keeps it from
	// closing the instant the pointer leaves the bell.
	bool drawFlyout(AppContext& ctx, const Cache& c)
	{
		// Anchored bottom-left onto the bell with no gap, so the mouse can travel
		// from one to the other without crossing a third window and closing the
		// flyout out from under itself. Clamped to the viewport because the bell
		// sits near the right edge and the list is far wider than it is.
		constexpr float kWidth   = 400.0f;
		constexpr float kMaxList = 320.0f;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const float x = std::max(vp->WorkPos.x + 4.0f,
		                         std::min(s_bellMax.x - kWidth,
		                                  vp->WorkPos.x + vp->WorkSize.x - kWidth - 4.0f));

		ImGui::SetNextWindowPos(ImVec2(x, s_bellMin.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0f), ImGuiCond_Always);
		ImGui::SetNextWindowViewport(vp->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));

		bool hovered = false;
		if (ImGui::Begin("##notificationFlyout", nullptr,
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

			// Nothing in this window was composed with a 400px column in mind: the
			// text of a notification is whatever the worker thread that posted it
			// had to say, and the header carries a count that grows. Without a wrap
			// position ImGui draws such a line straight past the right edge and
			// clips it there, which hands the reader the first half of a sentence
			// and no sign that there is a second half. The wrap position lives on
			// the window, so the list child and the flyout each need their own —
			// this one covers the header line and the footer row; the rows inside
			// the child push a narrower one of their own, because they also have to
			// leave the age at the right edge somewhere to sit.
			EditorWidgets::WrapText wrap;

			ImGui::TextUnformatted("Notifications");
			ImGui::SameLine();
			if (c.unseen > 0)             ImGui::TextDisabled("%zu unread", c.unseen);
			else if (!c.entries.empty())  ImGui::TextDisabled("all read");
			ImGui::Separator();

			// The empty state, which is what the bell shows for most of a healthy
			// session. It says what the list is FOR rather than "no items": this is
			// the one moment the user is looking at the surface with nothing on it,
			// so it is the only chance to explain what will appear here later.
			if (c.entries.empty())
			{
				ImGui::Spacing();
				ImGui::TextDisabled(
					"Nothing to report. Anything that goes wrong in the background — a "
					"server that cannot be reached, a download that failed, a change a "
					"collaborator could not apply — is collected here.");
				ImGui::Spacing();
			}

			// The list grows with its contents up to a ceiling and scrolls after
			// that. A fixed-height box would leave a hole under a single entry;
			// an unbounded one would run 200 entries straight off the top of the
			// screen, since the store keeps that many. Drawn even when empty — the
			// child auto-resizes to nothing, and skipping it would mean an
			// ImGui::EndChild that no longer pairs with a BeginChild call.
			ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
			                                    ImVec2(FLT_MAX, kMaxList));
			if (ImGui::BeginChild("##notificationList", ImVec2(0.0f, 0.0f),
			                      ImGuiChildFlags_AutoResizeY))
			{
				const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				// Room for the "copied" acknowledgement is reserved on every path
				// row whether or not it is showing. Taking it only while the word
				// is on screen would re-elide the path for a second and a half and
				// then grow it back — a click that makes the thing you clicked
				// change shape reads as a mistake.
				const float copiedW = ImGui::CalcTextSize("copied").x + 8.0f;

				// Newest first — the store push_backs, so the list it hands over
				// is oldest first and has to be walked backwards. The thing that
				// just went wrong is the thing being looked for.
				for (std::size_t back = c.entries.size(); back-- > 0; )
				{
					const Notification& n = c.entries[back];
					const int rowId = static_cast<int>(back);
					ImGui::PushID(rowId);

					char ageBuf[24];
					const char* age = ageText(n.whenMs, c.nowMs, n.count,
					                          ageBuf, sizeof(ageBuf));
					const float ageW = ImGui::CalcTextSize(age).x;

					const ImVec2 rowTop   = ImGui::GetCursorScreenPos();
					const float  availW   = ImGui::GetContentRegionAvail().x;
					const float  rowRight = rowTop.x + availW;
					// Window-local, which is the space PushTextWrapPos works in.
					const float  wrapPos  = ImGui::GetCursorPosX() + availW - ageW - 10.0f;
					const float  lineH    = ImGui::GetTextLineHeight();

					ImDrawList* dl = ImGui::GetWindowDrawList();

					// The level dot, painted into a slot exactly one line tall so
					// it cannot perturb the line box the message inherits.
					const float dotR = std::max(2.0f, lineH * 0.20f);
					dl->AddCircleFilled(ImVec2(rowTop.x + dotR + 1.0f, rowTop.y + lineH * 0.5f),
					                    dotR, ImGui::GetColorU32(levelColor(n.level)), 12);
					ImGui::Dummy(ImVec2(dotR * 2.0f + 2.0f, lineH));
					ImGui::SameLine(0.0f, 8.0f);

					ImGui::BeginGroup();
					ImGui::PushTextWrapPos(wrapPos);
					ImGui::TextUnformatted(n.text.c_str());

					// The reason, one step quieter. It is the part you read only
					// after the sentence above it has failed to explain itself.
					if (!n.detail.empty())
					{
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
						ImGui::TextUnformatted(n.detail.c_str());
						ImGui::PopStyleColor();
					}
					ImGui::PopTextWrapPos();

					// What it is about, and the only action a row can offer today:
					// the path, and the clipboard. Revealing it in the Content
					// Browser would need a request slot on that panel that does
					// not exist yet — see the note at the end of this file.
					if (!n.assetPath.empty())
					{
						const std::string shown =
							elideFront(n.assetPath,
							           std::max(40.0f, wrapPos - ImGui::GetCursorPosX() - copiedW));
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.70f, 0.95f, 0.9f));
						ImGui::TextUnformatted(shown.c_str());
						ImGui::PopStyleColor();

						if (ImGui::IsItemHovered())
						{
							ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
							// A tooltip has no width of its own — it grows to its
							// widest line — and the whole point of this one is to
							// show the path in full, unelided. On a deep content
							// tree that is a strip of text straight across the
							// screen, which is the same complaint as a panel that
							// cannot hold its own text, only louder. Bounded to a
							// readable column and wrapped inside it instead.
							if (ImGui::BeginTooltip())
							{
								{
									EditorWidgets::WrapText tipWrap(ImGui::GetFontSize() * 30.0f);
									ImGui::Text("%s\n\nClick to copy this path.",
									            n.assetPath.c_str());
								}
								ImGui::EndTooltip();
							}
						}
						if (ImGui::IsItemClicked())
						{
							ImGui::SetClipboardText(n.assetPath.c_str());
							s_copiedRow = rowId;
							s_copiedAt  = ImGui::GetTime();
						}
						// A click that produces no visible change reads as a
						// click that did not register, and the clipboard is
						// invisible by nature.
						if (s_copiedRow == rowId && ImGui::GetTime() - s_copiedAt < 1.6)
						{
							ImGui::SameLine(0.0f, 8.0f);
							ImGui::TextDisabled("copied");
						}
					}
					ImGui::EndGroup();

					// The age, painted rather than laid out: it belongs at the
					// row's right edge on the FIRST line whatever the message
					// wraps to, and an ImGui item cannot be right-aligned onto a
					// wrapped block without disturbing it.
					dl->AddText(ImVec2(rowRight - ageW, rowTop.y), dimCol, age);

					ImGui::PopID();
					ImGui::Spacing();
				}
			}
			ImGui::EndChild();

			ImGui::Separator();

			// Marking seen is an ACTION, never a side effect of opening this.
			// Reading a list of twelve because you opened it to check one is how
			// a notification centre becomes a thing people stop opening.
			ImGui::BeginDisabled(c.unseen == 0);
			if (ImGui::SmallButton("Mark all as seen") && ctx.notifications)
				ctx.notifications->markAllSeen();
			ImGui::EndDisabled();

			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
			                     std::max(0.0f, ImGui::GetContentRegionAvail().x - 52.0f));
			ImGui::BeginDisabled(c.entries.empty());
			if (ImGui::SmallButton("Clear") && ctx.notifications)
			{
				// The store empties, so the list is replaced by the empty state
				// next frame — the bell itself stays, it is permanent now.
				ctx.notifications->clear();
			}
			ImGui::EndDisabled();
		}
		ImGui::End();
		ImGui::PopStyleVar();
		return hovered;
	}
} // namespace
#endif // HE_IMGUI_ENABLED

// ─────────────────────────────────────────────────────────────────────────────

float FooterWidth(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	// Null in a headless or test context — every caller of ctx.notifications has
	// to check, which is why this one does too.
	if (!ctx.notifications) return 0.0f;
	return layoutFor(refresh(ctx)).width;
#else
	(void)ctx;
	return 0.0f;
#endif
}

void DrawFooter(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.notifications) { closeFlyout(); return; }

	const Cache&     c      = refresh(ctx);
	const BellLayout layout = layoutFor(c);

	// One hit box for the whole cluster: the flyout belongs to the bell and its
	// count together, not to whichever of the two the mouse happens to land on.
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##notificationBell", ImVec2(layout.width, layout.slotH));
	const bool hovered = ImGui::IsItemHovered();
	if (ImGui::IsItemClicked()) s_pinned = !s_pinned;

	ImDrawList* dl  = ImGui::GetWindowDrawList();
	const ImU32 col = bellColor(c);
	drawBell(dl, origin, layout.bellW, layout.slotH, col);

	if (layout.countLabel[0] != '\0')
	{
		dl->AddText(ImVec2(origin.x + layout.bellW + kGap, origin.y), col,
		            layout.countLabel);
	}

	s_bellMin     = origin;
	s_bellMax     = ImVec2(origin.x + layout.width, origin.y + layout.slotH);
	s_bellHovered = hovered;
	s_bellFrame   = ImGui::GetFrameCount();
#else
	(void)ctx;
#endif
}

void DrawOverlay(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.notifications) { closeFlyout(); return; }

	// The bell did not draw this frame — the footer was skipped entirely (the
	// project hub does that), so there is no anchor to hang the list off.
	if (s_bellFrame != ImGui::GetFrameCount()) { closeFlyout(); return; }

	const Cache& c = refresh(ctx);

	// An empty store no longer closes this. The bell is permanent now, and a
	// permanent control whose click does nothing reads as broken — so the flyout
	// opens on an empty list too and says so in a sentence. That is also the only
	// moment the surface can explain itself, since by definition nothing has gone
	// wrong yet when someone first pokes at it.
	if (s_bellHovered) s_open = true;

	bool flyoutHovered = false;
	if (s_open || s_pinned) flyoutHovered = drawFlyout(ctx, c);

	if (!s_bellHovered && !flyoutHovered && !s_pinned) s_open = false;

	// A click anywhere else unpins, the way a menu behaves everywhere else.
	if (s_pinned && !s_bellHovered && !flyoutHovered &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		s_pinned = false;
		s_open   = false;
	}
#else
	(void)ctx;
#endif
}

} // namespace NotificationBar

// ── What a row still cannot do ───────────────────────────────────────────────
// A row that names an asset should take you TO that asset — select it in the
// Content Browser, the way double-clicking a search result does everywhere else.
// It copies the path instead, because there is no way in from here:
// ContentBrowserPanel's header exposes only "which root am I on" and "which
// folder am I showing", and all of its navigation state is file-static in its
// .cpp. Doing it properly needs the same one-shot request slot the editor
// already uses for this exact problem elsewhere — MaterialEditorPanel's
// takeOpenRequest(), drained by the shell each frame — i.e. a
// ContentBrowserPanel::requestReveal(absolutePath) plus a drain at the top of
// its render(), which would then also give the collab activity line and the
// asset-lock banner somewhere to send the user.
