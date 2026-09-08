#pragma once
#include "CollabDocSync.h"
#include <imgui.h>
#include <string>
#include <vector>

struct AppContext;

// ── HorizonCode Class editor ─────────────────────────────────────────────────
// A tab editor for a standalone HorizonCode class asset (AssetType::
// HorizonCodeClass). Shares the graph window (canvas + variables + functions)
// with the Level Script / Game Instance editors; a class names its own events
// freely (no fixed catalog). State is cached per asset path across tab switches.
namespace HorizonCodeClassPanel
{
	// Fill the tab rect with the editor for the class asset at `assetPath`.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// True if the .hasset at `path` is a HorizonCode class (header sniff).
	bool isClassAsset(const std::string& path);
	// True if the cached editor for `path` has unsaved edits (tab dirty mark).
	bool isDirty(const std::string& path);

	// Re-read the file on the next frame (collab: a peer's change landed).
	bool reloadFromDisk(const std::string& assetPath);
	// Paths of every unsaved tab this panel holds, open or already closed.
	// See AssetPanelState::appendDirtyPaths — a closed dirty tab keeps its
	// state but leaves the tab vector, so the quit guard must ask here.
	void appendDirtyPaths(std::vector<std::string>& out);
	// Write this tab's graph to disk, exactly like the header's Save button — so
	// the close/quit prompt can save this asset without the user having to walk
	// back into the tab. Returns true when nothing is left unsaved for this path
	// (including the "this panel never held it" case).
	bool save(AppContext& ctx, const std::string& path);
	// Drop the cached editor state (tab closed without unsaved edits).
	void forget(const std::string& path);

	// The live documents behind this tab, for collaboration's item-level sync.
	// Empty when this panel does not hold `assetPath` — same "ask everyone, the
	// owner answers" dispatch as save() and reloadFromDisk().
	CollabDocSync::DocBindings collabDocs(const std::string& assetPath);

	// ── Authoring a class from outside the editor (McpToolRegistry.h) ────────
	// The same state collabDocs wraps, handed out raw so the MCP tools can edit
	// it. Null unless this panel already HOLDS the asset, which is the point: a
	// class asset loaded behind the panel's back would be a second copy, and the
	// human's next Save from the tab would write over everything MCP did to it.
	// The caller does not load one — it asks for what is open and reports the
	// rest as not addressable.
	//
	// These four take the CONTENT-RELATIVE path, not the tab key the functions
	// above take: that is the address every other MCP tool uses, and a full
	// filesystem path on that interface would put the user's home directory into
	// a conversation for no gain.
	HorizonCode::Graph* liveGraph(const std::string& contentPath);
	// This tab has unsaved edits (an MCP edit is an edit; without this the tab
	// shows no "*" and save() above refuses, believing it has nothing to write).
	void markDirty(const std::string& contentPath);
	// Every loaded class this panel holds, open or closed — what is addressable
	// at all. Dirty comes along because the caller wants both and asking twice
	// would mean scanning the map twice with a second path convention.
	struct Held { std::string contentPath; bool dirty = false; };
	void appendHeld(std::vector<Held>& out);
	// save(), addressed the same way. True when the write went through.
	bool saveByContentPath(AppContext& ctx, const std::string& contentPath);

}
