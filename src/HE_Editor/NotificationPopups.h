#pragma once

// ── The loud end of the notification channel ─────────────────────────────────
// NotificationStore decides WHAT is due; this draws it.
//
//   Important  → a toast in the bottom-right corner. It never takes keyboard
//                focus (typing into a field keeps typing), stays while the mouse
//                is on it, and goes away on its own — the notification centre
//                keeps it. A toast can carry buttons (an invitation: Join / Ignore).
//   Critical   → a modal dialog, one at a time in the order they were posted.
//                It must be acknowledged, and it waits for any other modal that is
//                already open instead of replacing it (OpenPopup at root level
//                replaces whatever is open — see AssetRecoveryDialog.cpp).
//   Info       → nothing here. The bell is its only home.
//
// DrawOverlay belongs at the very end of the frame, after the bell's flyout, so
// the toasts are above every docked panel and the modal is above everything.

struct AppContext;

namespace HE::Ed { struct Notification; }

namespace NotificationPopups
{
	void DrawOverlay(AppContext& ctx);

	// The buttons of one notification, in the order the poster listed them, on one
	// line (the caller positions the cursor). Used by the toast, the modal and the
	// notification centre's rows, so a button looks and behaves the same wherever
	// it is met. Draws nothing for a notification that has no actions or has
	// already been handled. Returns true on the frame a button was pressed.
	bool DrawActions(AppContext& ctx, const HE::Ed::Notification& n);
}
