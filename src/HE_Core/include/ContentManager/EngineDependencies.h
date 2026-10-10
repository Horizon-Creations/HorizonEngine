#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Does a project have everything it needs from the EngineContent library?
//
//  A project reaches into the engine's shared library without owning any of it: a
//  scene places the default cube, a material samples "Engine/Textures/…" arrays and
//  calls "Engine/MaterialFunctions/…", and each of those engine assets points on at
//  more (an engine material at its textures). On a machine that has not downloaded
//  them yet the project opens with grey surfaces and nothing says why.
//
//  This answers the question BEFORE the project is opened, from the files alone —
//  no ContentManager, no renderer, nothing loaded: scan the project for references
//  into "Engine/", follow them through the engine's own files (fetching the ones that
//  are not here yet, which the caller provides), and report what could not be had.
//  The Editor runs it on the start screen and asks the user what to do about a
//  non-empty answer (EditorApplication / ProjectPreflight).
//
//  Deliberately free of the network: the server's catalogue and the download
//  itself are handed in, so the algorithm is the same in the editor, a command-line
//  tool and a test with a fake server.
//
//  What counts as a reference, by file kind:
//    • any project or engine file: the text "Engine/…​.hasset" found anywhere in
//      its bytes (a path stored as a string — .hasset chunks, node-graph JSON,
//      widget trees, HorizonCode, scripts, the .heproj);
//    • a .hescene (JSON): additionally an asset id, [hi, lo] or {"hi":…,"lo":…},
//      that the catalogue knows — scenes address assets by UUID.
//  Texture / audio / font assets are not read (they point at nothing); every other
//  .hasset only up to maxAssetBytes (references sit in its leading chunks, a mesh's
//  vertex data behind them is the bulk).
// ─────────────────────────────────────────────────────────────────────────────

namespace HE::EngineDeps
{

// One asset the server holds: its path relative to the EngineContent root
// ("Textures/Landscape/T_Landscape_Albedo_Array.hasset") and, for a .hasset, its id.
struct CatalogueEntry
{
	std::string path;
	HE::UUID    uuid{};
};

struct Progress
{
	std::string stage;        // "Reading the project", "Downloading", "Checking"
	std::string detail;       // the file being fetched / scanned, may be empty
	std::size_t done  = 0;    // within the stage, when it has a count
	std::size_t total = 0;
};

struct Options
{
	std::string projectFile;           // absolute path of the .heproj
	std::string projectContentRoot;    // absolute "<project>/Content"
	// Where an "Engine/<rest>" path can already be: the shipped / checked-out library
	// next to the editor, and the download cache. (A project override,
	// "<projectContentRoot>/Engine/<rest>", is always looked at first.)
	std::string engineRoot;
	std::string cacheRoot;

	// The server's catalogue. `catalogueKnown` false = no server and no cached copy:
	// nothing can be fetched, and a missing file says so.
	std::vector<CatalogueEntry> catalogue;
	bool                        catalogueKnown = false;

	// Starts the download of one catalogue entry (`path` as in CatalogueEntry) and
	// calls `done(true)` — from any thread — once it is in cacheRoot, `done(false)`
	// if it cannot be had. Null = nothing can be fetched.
	std::function<void(const std::string& path, std::function<void(bool)> done)> fetch;

	std::function<bool()>                 cancelled;
	std::function<void(const Progress&)>  progress;

	// Limits (see the file comment).
	std::size_t maxAssetBytes = 1u << 20;      // a .hasset is read up to this much
	std::size_t maxTextBytes  = 64u << 20;     // a scene / graph / script, whole, up to this
};

enum class MissingReason : std::uint8_t
{
	NotOnServer,      // the catalogue is known and does not list it
	NoCatalogue,      // there is no catalogue to ask (offline, nothing cached)
	DownloadFailed,   // listed, but fetching it failed
	NoFetcher,        // listed, but this build has no way to download
};

struct Missing
{
	std::string   path;        // "Engine/Textures/…" as references spell it
	MissingReason reason = MissingReason::NotOnServer;
	std::string   neededBy;    // the first file found pointing at it (display path)
};

struct Result
{
	std::vector<Missing> missing;
	std::size_t filesScanned = 0;   // project + engine files read
	std::size_t referenced   = 0;   // distinct engine assets the project reaches
	std::size_t alreadyHere  = 0;   // of those, found locally
	std::size_t downloaded   = 0;   // of those, fetched now
	bool        cancelled    = false;
	bool ok() const { return !cancelled && missing.empty(); }
};

// Runs the whole walk. Blocking; belongs on a worker thread. `fetch`'s completion may
// arrive on any thread — this call waits for it.
HE_API Result resolve(const Options& options);

// The reference extractor on its own (tests, tools): every "Engine/….hasset" path in
// `bytes`, in order of first appearance, de-duplicated.
HE_API std::vector<std::string> findEngineReferences(const char* bytes, std::size_t size);

// Where "Engine/<rest>" lives on this machine right now: the project's override, the
// shipped library, the download cache — in that order, the same one
// ContentManager::resolveAbsolutePath uses. Empty when it is nowhere.
HE_API std::string locateEngineFile(const std::string& enginePath, const Options& options);

} // namespace HE::EngineDeps
