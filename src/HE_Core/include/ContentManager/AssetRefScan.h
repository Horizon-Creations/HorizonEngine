#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  "What still points at this?" — the read-only twin of AssetRefRetarget
//
//  Deleting an asset is the one Content Browser operation with no undo, and the
//  confirmation used to say only "anything still referencing it keeps a broken
//  reference" — true, useless, and unanswerable from where the user is standing.
//  This answers it: which files reference the thing about to go away.
//
//  Two encodings have to be searched, because the engine stores references in
//  two shapes and a scan that knows only one is worse than none (it reports
//  "nothing uses this" for the most common case):
//
//    • BY PATH — inside .hasset chunks: mesh → material (MREF), material →
//      shader/textures/parent (MTRL), node-graph JSON, widget trees → textures
//      and fonts, input mapping contexts → input actions, HorizonCode graphs →
//      classes/widgets/scenes. Also loose .lua/.py/.hcode text and the .heproj
//      manifest's startup scene.
//    • BY UUID — inside .hescene JSON, where scene COMPONENTS address assets as
//      [hi, lo] pairs, and inside the two graph chunks that store asset ids as
//      {"hi":…,"lo":…} objects (ASMG clip ids, PTGR mesh/material ids).
//
//  Both spellings also occur BINARY-ENCODED, in a prefab's CBOR payload (PFAB,
//  SceneSerializer::serializeSubtree): an entity subtree with components, where
//  an id is eight raw big-endian bytes and a path is a length-tagged CBOR
//  string. Neither has a text form, so the payload is decoded and walked as the
//  JSON document it is — see isCborPayloadChunk in the .cpp, and the gate that
//  has to let prefabs through because no needle can rule one out.
//
//  Deliberately NOT built on retargetBlob(): that one answers a bool for a whole
//  file, cannot tell a genuine referrer from the asset's own META path (every
//  asset stores its own path, so every asset would report itself), and pays a
//  full re-serialisation of every chunk to do it. The scan below skips META,
//  stops at the first hit in a file, and never constructs a Writer.
// ─────────────────────────────────────────────────────────────────────────────

namespace HE::AssetRefs
{

// What is being looked for. `paths` are content-relative and match a stored
// reference WHOLE ("Engine/…" prefix included, exactly as references spell it);
// `pathPrefixes` are the folder form — the folder itself plus everything under
// it. `uuids` are asset ids; a null id is ignored rather than matching every
// unset reference (an absent reference serialises as [0,0], not as nothing).
struct ScanTargets
{
	std::vector<std::string> paths;
	std::vector<std::string> pathPrefixes;
	std::vector<HE::UUID>    uuids;

	bool empty() const { return paths.empty() && pathPrefixes.empty() && uuids.empty(); }
};

// How the reference was stored — shown in the dialog, because "a scene uses it"
// and "a material names its file" are different kinds of breakage.
enum class RefKind : std::uint8_t
{
	Path = 0,   // a stored path string
	Uuid = 1,   // an asset id (scene component, graph chunk)
};

struct Referrer
{
	std::string absolutePath;
	std::string displayPath;   // content-relative when it is under a known root
	RefKind     kind = RefKind::Path;
};

struct ScanRequest
{
	std::string contentRoot;      // absolute; the tree that is walked
	std::string projectRoot;      // absolute; .heproj + a project-root GameInstance.hcode ("" to skip)
	std::string contentDirName;   // "Content" — adds the project-relative rule form ("" to skip)

	// Files that must never be reported: the deletion targets themselves, and —
	// for a folder — everything inside it (a folder's own assets referencing each
	// other says nothing about what breaks OUTSIDE it). Absolute paths.
	std::vector<std::string> excludeFiles;
	std::vector<std::string> excludeUnder;

	// A dialog can show a few dozen lines usefully; past that the count is the
	// message. The walk stops early once this many DISTINCT referrers are found.
	std::size_t maxReferrers = 200;

	// Polled between files. A scan started for a dialog the user has since closed
	// is answering a question nobody asked any more, and on a large project it
	// would otherwise keep a worker (and, at exit, the shutdown) busy for as long
	// as the walk takes. A cancelled scan reports `incomplete`, never an empty
	// "nothing references this".
	std::function<bool()> isCancelled;
};

struct ScanResult
{
	std::vector<Referrer> referrers;
	std::size_t           filesScanned = 0;
	// More referrers exist than `maxReferrers` — the list is a sample, the answer
	// is still "yes, this is referenced".
	bool truncated = false;
	// The directory walk hit an error and stopped, or a file could not be read:
	// the answer is a LOWER BOUND, and a dialog must not read an empty list as
	// "nothing references this".
	bool incomplete = false;
};

// Walks `contentRoot` (and the project root's manifest/GameInstance graph) and
// reports every file holding a reference to one of `targets`. Pure filesystem +
// CPU work, no engine state touched — safe on a worker thread; the caller
// marshals the result back itself.
HE_API ScanResult findReferrers(const ScanTargets& targets, const ScanRequest& request);

// The asset id a loose .hasset persists in its META chunk, or a null id when the
// file carries none. Used to turn "this file is being deleted" into a UUID the
// scene scan can look for.
//
// `unreadable` separates the two ways of getting a null id back, and a caller
// that is about to state "nothing references this" MUST pass it. A file that
// simply has no id (a JSON .hescene, a pre-v2 asset) is a legitimate null; a
// file that could not be OPENED or whose header did not parse is a target the
// scan will now silently fail to look for — and scenes reference meshes and
// materials by id alone, so the answer would come back empty and confident.
HE_API HE::UUID assetUuidOfFile(const std::string& absolutePath, bool* unreadable = nullptr);

} // namespace HE::AssetRefs
