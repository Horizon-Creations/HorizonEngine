#pragma once

struct AppContext;

// ── "An earlier session left unsaved work behind" ────────────────────────────
// The other half of SceneAutosave: the timer writes a recovery copy of the
// scene while the editor runs, and this is what the NEXT start does with one
// that was still there. Self-triggering like GitMissingDialog — it raises
// itself while AppContext::recoveryOffer is set, and that pointer is null
// until the project-loaded callback has looked, so nothing flashes during
// startup and a project without a leftover never sees it.
//
// Three answers, and each one is spelled out in the dialog because they are
// not symmetric:
//   Restore         loads the copy over the scene it came from as one undo
//                   step (Undo = back to the file), leaves the scene dirty;
//   Delete Snapshot removes the copy for good;
//   Keep for Later  closes without touching it — offered again next start.
// Escape is Keep for Later: the one answer that cannot lose anything.
namespace SceneRecoveryDialog
{
	void Draw(AppContext& ctx);
}
