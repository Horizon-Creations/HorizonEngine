#pragma once

struct AppContext;

// ── "An earlier session left unsaved asset edits behind" ────────────────────
// The other half of AssetAutosave, and SceneRecoveryDialog's sibling: one row
// per recovery copy of a script, material, widget, class, ... file that was
// dirty when the last session ended without asking. Self-triggering on
// AppContext::assetRecoveryOffers, and it waits for the scene's dialog (and
// every other root-level modal) to be answered first — only one can be open.
//
// Per row, and for all at once:
//   Restore      writes the copy over the file. The version it replaces is
//                copied to Saved/Autosave/Assets/Replaced first, and the dialog
//                names that folder, because this — unlike the scene's Restore —
//                does write the user's file;
//   Delete Copy  removes the copy for good;
//   Keep for Later (all only) closes without touching anything — offered again
//                next start. Escape is Keep for Later.
// A row whose file changed after the copy was taken, or is gone, says so: the
// copy is then older than the file (or the only version left).
namespace AssetRecoveryDialog
{
	void Draw(AppContext& ctx);
}
