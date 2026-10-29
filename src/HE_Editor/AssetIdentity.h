#pragma once
#include <string>

class ContentManager;

// ── Which asset is this tab? ─────────────────────────────────────────────────
// A tab and the panel state behind it used to be tied together by the tab's path
// STRING. That is two problems in one:
//
//   * the same file has several spellings (native separators from the Content
//     Browser's directory walk, "/" from ContentManager::resolveSavePath, a
//     "./" somewhere). A collaboration peer's edit arrives as a content-relative
//     key, is turned back into a path with resolveSavePath, and the panel then
//     looked it up by exact string — one backslash and "nobody has that tab
//     open".
//   * a path is not the asset. Rename or move it and the tab follows (EditorTabs::
//     retarget) but the panel state stayed behind under the old string: the
//     edits, the undo history and the dirty flag were stranded, and a new empty
//     state grew at the new path.
//
// What does not change when a file moves is the UUID in its header. This is the
// one place that turns "a path somebody holds" into "the asset behind it", and
// the ContentManager is the authority on where that asset lives right now
// (ContentManager::pathForId); nothing else in the editor compares paths to decide
// whether two holders mean the same asset.
//
// UI-thread only, like the panel states that use it.
namespace HE::Ed::AssetIdentity
{
	// The ContentManager that answers "who owns this UUID, and where". Bound once;
	// it outlives every project and play session. Without one (tests, tools) the
	// file's own UUID still identifies it — only the duplicate check below is skipped.
	void bind(const ContentManager* content);

	// The identity of the asset at `assetPath`:
	//   "uuid:<32 hex>"   the file carries a UUID, and the ContentManager does not
	//                     place that UUID in a different file (a copy made outside
	//                     the editor carries the original's id — two files must
	//                     not share one panel state).
	//   "path:<normalised>" everything else: raw files, C++ sources, a file with no
	//                     UUID yet. Separators and "./" are folded, so every
	//                     spelling of one path agrees.
	//   the string itself  for the editor's virtual tabs (":Settings" ...) and "".
	//
	// Cheap enough to ask many times a frame: answers are cached per spelling and
	// re-checked against the file's mtime/size a few times a second.
	std::string keyFor(const std::string& assetPath);

	// The "path:" key keyFor falls back to, whatever the file carries. A panel
	// state created before the file had a UUID sits under this one, and is moved
	// to the UUID key when it appears.
	std::string pathKeyFor(const std::string& assetPath);

	// The content-relative path ("Engine/…" for engine content) the ContentManager
	// knows `assetPath` by, or empty (no ContentManager bound, or outside both
	// roots). What panel states cache as `relPath` and have to refresh after a move.
	std::string contentRelative(const std::string& assetPath);

	// Two spellings of one path (separators, "./"), no disk. For the question
	// "has this tab's path CHANGED" — a rename, as opposed to being asked twice
	// in different spellings.
	bool samePathSpelling(const std::string& a, const std::string& b);

	// Forget every cached answer. For tests that rewrite files faster than the
	// re-check interval; the editor never needs it.
	void resetCache();
}
