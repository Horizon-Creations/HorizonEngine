#pragma once

// ─── Remembering the editor window: size, position, maximized ────────────────
// The editor opened at 1600x900 wherever the OS felt like putting it, every
// time. This is the pure half of fixing that — no SDL, no window — so the rules
// that decide "is this saved rectangle still somewhere a person can reach" and
// "which rectangle do we remember while the window is maximized" can be asked in
// a test (test_editor_window_placement.cpp). EditorApplication feeds it SDL's
// numbers and writes the answer to config.json.
//
// All values are SDL window coordinates: points on macOS, pixels on Windows and
// X11. SDL_GetWindowPosition/Size and SDL_GetDisplayUsableBounds speak the same
// unit on one machine, which is the only comparison made here — nothing is
// converted, so Retina (points vs. drawable pixels) cannot creep in.

#include <algorithm>
#include <vector>

namespace HE::Ed
{
	struct PlacementRect
	{
		int x = 0, y = 0, w = 0, h = 0;
		bool operator==(const PlacementRect& o) const
		{ return x == o.x && y == o.y && w == o.w && h == o.h; }
	};

	// What config.json holds. `valid` is false until a window was ever recorded
	// (a config from before this existed, or a hidden run that never writes one).
	struct SavedWindowPlacement
	{
		bool          valid     = false;
		bool          maximized = false;
		PlacementRect normal;            // the NOT maximized rectangle
	};

	// What the window is asked to do at creation.
	struct ResolvedWindowPlacement
	{
		bool applied      = false;       // false → keep the editor's default window
		bool setPosition  = false;       // false → let the OS place it (centre)
		bool maximized    = false;
		PlacementRect rect;
	};

	// Smallest window worth restoring: a saved 0x0 or a sliver is a corrupt
	// value, not a preference.
	constexpr int kMinPlacementW = 640;
	constexpr int kMinPlacementH = 360;
	// How much of the title strip must lie on a display for the window to be
	// grabbable. A window whose bar is off every screen cannot be moved back.
	constexpr int kMinVisibleW   = 120;
	constexpr int kTitleStripH   = 40;

	inline int overlapLen(int a0, int a1, int b0, int b1)
	{
		return std::max(0, std::min(a1, b1) - std::max(a0, b0));
	}

	// True when enough of the window's top strip is on one of the displays.
	inline bool placementReachable(const PlacementRect& r, const std::vector<PlacementRect>& displays)
	{
		for (const PlacementRect& d : displays)
		{
			const int ow = overlapLen(r.x, r.x + r.w, d.x, d.x + d.w);
			const int oh = overlapLen(r.y, r.y + std::min(r.h, kTitleStripH), d.y, d.y + d.h);
			if (ow >= std::min(kMinVisibleW, r.w) && oh >= std::min(kTitleStripH, r.h) / 2)
				return true;
		}
		return false;
	}

	// The saved placement against the displays that exist NOW (usable bounds,
	// primary first). A position on a vanished monitor falls back to "no
	// position": the OS centres the window, the size is kept (shrunk to fit the
	// primary display). Maximized needs no position of its own — the OS picks the
	// display — but the normal rectangle is still handed on so un-maximizing
	// returns to where it was.
	inline ResolvedWindowPlacement resolveWindowPlacement(const SavedWindowPlacement& saved,
	                                                      const std::vector<PlacementRect>& displays)
	{
		ResolvedWindowPlacement out;
		if (!saved.valid || displays.empty()) return out;
		if (saved.normal.w < kMinPlacementW || saved.normal.h < kMinPlacementH) return out;

		out.applied   = true;
		out.maximized = saved.maximized;
		out.rect      = saved.normal;
		if (placementReachable(saved.normal, displays))
		{
			out.setPosition = true;
			return out;
		}
		const PlacementRect& primary = displays.front();
		out.rect.w = std::min(out.rect.w, primary.w);
		out.rect.h = std::min(out.rect.h, primary.h);
		out.rect.x = primary.x + (primary.w - out.rect.w) / 2;
		out.rect.y = primary.y + (primary.h - out.rect.h) / 2;
		out.setPosition = true;
		return out;
	}

	// One look at the live window per frame. The rectangle to remember is the
	// last one seen while the window was neither maximized, fullscreen nor
	// minimized — and only once it held still for a few frames, because the
	// maximize animation (macOS zoom, Windows) reports growing rectangles before
	// the maximized flag turns up, and that intermediate size must not become
	// "the size to come back to".
	class WindowPlacementTracker
	{
	public:
		static constexpr int kStableFrames = 4;

		void seed(const SavedWindowPlacement& s)
		{
			m_saved = s;
		}

		void observe(bool maximized, bool fullscreen, bool minimized, const PlacementRect& current)
		{
			// A minimized window says nothing about maximized (some platforms drop
			// the flag while it is in the Dock); fullscreen keeps what it was too.
			if (!minimized && !fullscreen) m_maximized = maximized;
			if (fullscreen || minimized || maximized)
			{
				m_stable = 0;
				return;
			}
			if (m_haveLast && m_last == current) ++m_stable;
			else                                 m_stable = 1;
			m_last = current;
			m_haveLast = true;
			if (m_stable >= kStableFrames && current.w >= kMinPlacementW && current.h >= kMinPlacementH)
			{
				m_saved.valid  = true;
				m_saved.normal = current;
			}
		}

		// What to write at shutdown. `valid` stays false when no usable normal
		// rectangle was ever seen, so a short or odd session never overwrites a
		// good saved value with nothing.
		SavedWindowPlacement result() const
		{
			SavedWindowPlacement r = m_saved;
			r.maximized = m_maximized && r.valid;
			return r;
		}

	private:
		SavedWindowPlacement m_saved;
		PlacementRect        m_last;
		bool                 m_haveLast  = false;
		bool                 m_maximized = false;
		int                  m_stable    = 0;
	};
}
