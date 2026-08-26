#include "ContentBrowserPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip
#include "EditorTheme.h"     // brand palette (selected tiles, labels)
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "EditorApplication.h"           // AppContext, GlobalState folders, ProjectManager
#include "EditorWidgets.h"
#include "EditorHelp.h"               // pinDialogToEditorWindow
#include "EditorUI.h"                    // discardPanelState on delete
#include "ScriptEditorPanel.h"
#include "CppClassEditorPanel.h"
#include "MaterialEditorPanel.h"
#include "UIEditorPanel.h"
#include "HorizonCodeClassPanel.h"
#include "InputAssetPanel.h"
#include "TypeAssetPanel.h"
#include "SkeletalMeshEditorPanel.h"
#include "StaticMeshEditorPanel.h"
#include "ParticleGraphEditorPanel.h"
#include "AnimatorStateMachineEditorPanel.h"
#include "AudioEditorPanel.h"
#include "EditorAssetTypeCache.h"
#include "GitController.h"        // per-file source-control status for the tile badge
#include "AssetThumbnailCache.h"         // rendered mesh/material tiles for the grid
#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentSync.h> // remote-only asset download queue
#endif
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/AssetRefScan.h>    // "what still points at this?" for the delete dialogs
#include <ContentManager/Assets.h>
#include <HorizonScene/Components/MaterialComponent.h> // placing a mesh follows its MREF material
#include <JobSystem/JobSystem.h>            // globalPool — the reference scan runs off the frame thread
#include <UIWidget/UIWidgetTree.h>       // starter tree for freshly created UI widgets
#include <HorizonCode/HorizonCode.h>     // starter graph for freshly created HorizonCode classes
#include <Types/Enums.h>
#include "MeshImporter.h"
#include "SkeletalMeshImporter.h"
#include "AnimationClipImporter.h"
#include "TextureImporter.h"
#include "MaterialImporter.h"
#include "AudioImporter.h"
#include "FontImporter.h"
#include "ImporterCommon.h"   // Importer::gltfHasSkin — static vs. skeletal routing

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#endif

#include <Diagnostics/Logger.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#endif

// File-local alias, as in EditorUI.cpp.
namespace fs = std::filesystem;

namespace ContentBrowserPanel
{

static bool s_quietContentRefresh = false;

// The inverse of mergeManifestInto's fullPath synthesis (GlobalState.cpp):
// a remote-only File's fullPath is always engineContentCacheDir()/relativePath,
// so this recovers the SFTP-side relative path (== the manifest's own "path"
// field, and — per the deployment's remote layout — the same string EngineContent
// sync downloads from) straight back out of it.
//
// Empty for anything that is NOT under the cache. fs::relative does not say so on
// its own — it happily answers "../../EditorDeps/EngineContent/Meshes/Cube.hasset"
// for a shipped default — so the leading-".." check is what makes this a
// containment test rather than just a subtraction. It used to be called only on
// nodes already known to be cache-rooted; the cache-copy predicate below asks it
// about arbitrary files.
static std::string engineRelativePath(const std::string& fullPath)
{
	std::error_code ec;
	const fs::path rel = fs::relative(fullPath, GlobalState::engineContentCacheDir(), ec);
	if (ec || rel.empty() || rel.native()[0] == '.') return std::string();
	return rel.generic_string();
}

// Is this file the shared download cache's copy of an EngineContent asset — the
// one kind of "engine" file a project may legitimately remove?
//
// The three EngineContent tiers are disjoint directories and a File node carries
// exactly one path, so the tier IS the path: a shipped default lives under the
// engine root, a project override under <contentRoot>/Engine, and everything else
// that resolves as "Engine/…" is a download. Ordered like
// ContentManager::resolveAbsolutePath's own precedence so the two can never
// disagree about which file is meant.
static bool isEngineCacheCopy(const ContentManager* cm, const std::string& fullPath)
{
	if (!cm || fullPath.empty()) return false;
	if (cm->isEngineDefaultPath(fullPath) || cm->isEngineOverridePath(fullPath)) return false;
	if (engineRelativePath(fullPath).empty()) return false;
	std::error_code ec;
	return fs::is_regular_file(fullPath, ec);   // a remote-only placeholder has nothing to remove
}

// Which root the tree/grid is showing: 0=Content, 1=Engine, 2=Source. At
// namespace scope rather than inside render() only so browsedRootKind() can read
// it — everything that writes it still lives in render().
static int s_selectedRootKind = 0;

// ─── "What still points at this?" ────────────────────────────────────────────
// The delete dialogs used to state, flatly, that anything referencing the asset
// would keep a broken reference — true, and unanswerable from where the user is
// standing. This runs the actual scan (HE::AssetRefs::findReferrers) so the
// dialog can name the files instead.
//
// It reads every candidate file's bytes, which on a real project is far too much
// for the frame it was clicked in: the dialog opens IMMEDIATELY saying it is
// looking, a worker fills the answer in, and the generation counter drops results
// that arrive for a dialog the user already closed (or for a different asset they
// right-clicked meanwhile). Same producer/consumer split as the download queue's
// tab-open handoff below, one mutex, no engine state touched off-thread.
namespace
{
	struct RefScanState
	{
		std::mutex                  mutex;
		// Atomic because the WORKER reads it too: a scan whose generation is no
		// longer the current one has nobody waiting for its answer, and polling
		// this is how it stops walking instead of grinding to the end of a large
		// project (or holding up editor shutdown, which joins the pool).
		std::atomic<std::uint64_t>  requested{0};
		std::uint64_t               completed = 0;   // worker: the generation `result` belongs to
		HE::AssetRefs::ScanResult   result;
	};
	RefScanState s_refScan;

	// Everything the worker needs, by value — it must never read ctx or the
	// content manager live (both belong to the frame thread).
	struct RefScanJob
	{
		std::string contentRoot;
		std::string projectRoot;
		std::string contentDirName;
		// Plural: a multi-asset delete asks ONE question — "what breaks if all of
		// these go" — and answering it per asset would run N tree walks and leave
		// the user to work out the union of N lists themselves.
		std::vector<std::string> targetsAbs;
		std::vector<std::string> targetsRel;   // content-relative, "Engine/…" included
		bool        isFolder = false;
	};

	// The UUIDs of every asset under `folderAbs` — what a folder delete takes with
	// it, and therefore what scenes may still address by id. Reading each file's
	// META is why this runs on the worker and not at menu time.
	//
	// `lost` is the load-bearing part. A target whose id could not be read is a
	// target the scan will not look for, and scene components reference meshes and
	// materials by id ALONE — so dropping one silently turns a scene full of live
	// references into "Nothing else references it". The flag turns that into "some
	// files could not be checked" instead.
	std::vector<HE::UUID> containedAssetUuids(const std::string& folderAbs,
	                                          const std::function<bool()>& cancelled,
	                                          bool& lost)
	{
		std::vector<HE::UUID> out;
		std::error_code ec;
		fs::recursive_directory_iterator it(
			folderAbs, fs::directory_options::skip_permission_denied, ec);
		const fs::recursive_directory_iterator end;
		if (ec) { lost = true; return out; }
		for (; !ec && it != end; it.increment(ec))
		{
			// Polled here too, not only inside findReferrers: for a folder target
			// this walk runs FIRST, so a scan cancelled during it would otherwise
			// read the whole subtree before noticing — including while the editor is
			// shutting down and joining the pool.
			if (cancelled && cancelled()) { lost = true; return out; }
			std::error_code fec;
			if (!it->is_regular_file(fec) || fec) { fec.clear(); continue; }
			if (it->path().extension() != ".hasset" && it->path().extension() != ".hescene") continue;
			bool unreadable = false;
			const HE::UUID id = HE::AssetRefs::assetUuidOfFile(it->path().string(), &unreadable);
			if (unreadable) lost = true;
			if (!(id == HE::UUID{})) out.push_back(id);
		}
		if (ec) lost = true;   // the walk broke off — the target set is incomplete
		return out;
	}

	std::uint64_t startReferenceScan(RefScanJob job)
	{
		const std::uint64_t generation =
			s_refScan.requested.fetch_add(1, std::memory_order_acq_rel) + 1;
		// Fire-and-forget: the pool job may outlive the dialog, so it captures the
		// job by value and publishes only through the file-scope state above.
		globalPool().submit([job = std::move(job), generation]
		{
			HE::AssetRefs::ScanTargets targets;
			HE::AssetRefs::ScanRequest request;
			request.contentRoot    = job.contentRoot;
			request.projectRoot    = job.projectRoot;
			request.contentDirName = job.contentDirName;
			// Superseded means abandoned: closing the dialog and asking about
			// something else bumps the counter, and this walk stops at the next
			// file rather than reading a whole project for nobody.
			request.isCancelled    = [generation]
			{
				return s_refScan.requested.load(std::memory_order_acquire) != generation;
			};

			// A target whose id could not be read is one the scan cannot look for.
			// The result has to admit that, or the dialog states the strongest thing
			// it can say — "nothing references it" — about a question it never asked.
			bool lostTarget = false;
			for (std::size_t ti = 0; ti < job.targetsAbs.size(); ++ti)
			{
				const std::string& abs = job.targetsAbs[ti];
				const std::string  rel = ti < job.targetsRel.size() ? job.targetsRel[ti] : std::string{};
				if (job.isFolder)
				{
					if (!rel.empty()) targets.pathPrefixes.push_back(rel);
					bool lostHere = false;
					const std::vector<HE::UUID> inside =
						containedAssetUuids(abs, request.isCancelled, lostHere);
					targets.uuids.insert(targets.uuids.end(), inside.begin(), inside.end());
					if (lostHere) lostTarget = true;
					// A folder's own assets referencing each other says nothing about
					// what breaks OUTSIDE it — they are going away together.
					request.excludeUnder.push_back(abs);
				}
				else
				{
					if (!rel.empty()) targets.paths.push_back(rel);
					bool lostHere = false;
					const HE::UUID id = HE::AssetRefs::assetUuidOfFile(abs, &lostHere);
					if (!(id == HE::UUID{})) targets.uuids.push_back(id);
					if (lostHere) lostTarget = true;
					// Assets in the same selection reference each other all the time
					// (a mesh and the material it names): those are going together, so
					// they are not what "this still breaks" means.
					request.excludeFiles.push_back(abs);
				}
			}

			HE::AssetRefs::ScanResult result = HE::AssetRefs::findReferrers(targets, request);
			if (lostTarget) result.incomplete = true;

			// A newer dialog already asked something else — this answer is about a
			// question nobody is looking at any more (and, having been cancelled, it
			// is a partial one).
			if (s_refScan.requested.load(std::memory_order_acquire) != generation) return;
			std::lock_guard<std::mutex> lock(s_refScan.mutex);
			if (generation < s_refScan.completed) return;
			s_refScan.completed = generation;
			s_refScan.result    = std::move(result);
		}, "AssetRefScan");
		return generation;
	}

	// The dialog is gone: nothing is waiting for the answer any more, so let the
	// walk stop where it is. (Bumping the counter is all it takes — the worker
	// polls it.)
	void cancelReferenceScan()
	{
		s_refScan.requested.fetch_add(1, std::memory_order_acq_rel);
	}

	// The dialog's view of the scan it started. False while the worker is still
	// walking — the caller says so rather than showing an empty list, which would
	// read as "nothing references this".
	bool referenceScanReady(std::uint64_t generation, HE::AssetRefs::ScanResult& out)
	{
		std::lock_guard<std::mutex> lock(s_refScan.mutex);
		if (generation == 0 || s_refScan.completed != generation) return false;
		out = s_refScan.result;
		return true;
	}

#ifdef HE_IMGUI_ENABLED
	// The reference section every delete-ish dialog shows. `consequence` is what
	// happens to those referrers if the user goes ahead — which is NOT the same
	// sentence for a deletion (they break) and for a cache eviction (they cause a
	// re-download), so the caller supplies it.
	void drawReferenceSection(std::uint64_t generation, const char* consequence)
	{
		// Every line here is a whole sentence, and every dialog that shows this
		// section is pinned to 460 px and cannot be resized. Unwrapped, "Some files
		// could not be checked — there may be references." is cut off around
		// "references" — the clause that says a reference may still exist is exactly
		// the half that disappears, and what is left reads like a completed check.
		//
		// The guard rather than a Push/Pop pair because three branches below return
		// early: a hand-rolled Pop would be skipped on two of them, leaving the
		// dialog's wrap stack unbalanced for the rest of the frame. Scoped to the
		// function, so it pops while the caller's popup is still the current window.
		EditorWidgets::WrapText wrap;

		// No scan was startable — a C++ class under Source/, or a path outside every
		// known root. Saying so is the point: an empty section here would read as
		// "checked, found nothing", which is a stronger claim than the old flat
		// warning this replaced.
		if (generation == 0)
		{
			ImGui::TextDisabled("References were not checked for this file.");
			return;
		}

		HE::AssetRefs::ScanResult scan;
		if (!referenceScanReady(generation, scan))
		{
			// Three dots that move, so a slow scan on a big project reads as
			// "working" rather than "stuck".
			static const char* kDots[] = { "", ".", "..", "..." };
			const int phase = static_cast<int>(ImGui::GetTime() * 3.0) & 3;
			ImGui::TextDisabled("Checking what references it%s", kDots[phase]);
			return;
		}

		if (scan.referrers.empty())
		{
			if (scan.incomplete)
				ImGui::TextDisabled("Some files could not be checked — there may be references.");
			else
				ImGui::TextDisabled("Nothing else references it.");
			return;
		}

		const int n = static_cast<int>(scan.referrers.size());
		ImGui::TextColored(ImVec4(1.00f, 0.80f, 0.35f, 1.0f),
			"%d file%s reference%s it%s", n, n == 1 ? "" : "s", n == 1 ? "s" : "",
			scan.truncated ? " (first matches shown)" : "");
		if (consequence && *consequence) ImGui::TextDisabled("%s", consequence);

		// Two lines' worth at minimum: a path that wraps in a box sized for one
		// line is a path with its tail cut off, which is the opposite of what a
		// list of referrers is for.
		const float lineH = ImGui::GetTextLineHeightWithSpacing();
		const float listH = std::clamp(lineH * static_cast<float>(n) + 8.0f,
		                               lineH * 2.0f, 140.0f);
		ImGui::BeginChild("##cb_ref_list", ImVec2(-1.0f, listH), true);
		// Wrapped, not scrolled sideways. A content path is long enough to need
		// one or the other, and a horizontal scrollbar hides the end of every row
		// behind a gesture nobody makes — the reader sees a truncated list and no
		// sign that it is truncated. Set once for the whole child, so the dimmed
		// kind tag below wraps with the path instead of being clipped off the
		// right edge. The child is its own ImGui window and starts with no wrap
		// position at all, so the guard above does not reach in here.
		//
		// In a block of its own because the pop has to land on the CHILD: at the
		// end of the enclosing scope it would run after EndChild(), i.e. on the
		// dialog window, and take that one's wrap position away instead.
		{
			EditorWidgets::WrapText listWrap;
			for (const HE::AssetRefs::Referrer& r : scan.referrers)
			{
				ImGui::TextUnformatted(r.displayPath.c_str());
				// How it points at the asset decides whether a rename could have
				// saved it: a stored path can be retargeted, a scene's asset id
				// cannot be anything but dangling once the asset is gone.
				ImGui::SameLine();
				ImGui::TextDisabled(r.kind == HE::AssetRefs::RefKind::Uuid ? "(asset id)" : "(path)");
			}
		}
		ImGui::EndChild();
		if (scan.incomplete)
			ImGui::TextDisabled("Some files could not be checked — there may be more.");
	}
#endif
}

// A mirror of the grid's current folder, kept at namespace scope purely so
// browsedFolderPath() can reach it — everything that WRITES it still lives in
// render(), next to the state it is derived from.
static std::string s_browsedFolderPath;

bool quietRefreshRequested()  { return s_quietContentRefresh; }
void clearQuietRefreshRequest() { s_quietContentRefresh = false; }
int  browsedRootKind()          { return s_selectedRootKind; }
std::string browsedFolderPath() { return s_browsedFolderPath; }

// Starter template for a freshly created script, by language (0 = Lua, 1 = Python).
static const char* scriptStarterTemplate(int lang)
{
	static const char* kLua =
		"local M = {}\n\n"
		"function M.onStart(self)\nend\n\n"
		"function M.onUpdate(self, dt)\nend\n\n"
		"return M\n";
	static const char* kPy =
		"import horizon\n\n"
		"class NewScript(horizon.Behavior):\n"
		"    def on_start(self):\n        pass\n\n"
		"    def on_update(self, dt):\n        pass\n";
	return (lang == 1) ? kPy : kLua;
}

// An item just moved on disk: hand the old/new ABSOLUTE paths to the content
// manager as the content-relative ones references are stored in, so everything
// that points at the item follows it. Silently a no-op for anything outside the
// content/engine roots (the C++ Source tree, which holds no asset references).
static void retargetReferences(AppContext& ctx, const std::string& oldAbs,
                               const std::string& newAbs, bool folder)
{
	if (!ctx.contentManager) return;
	const std::string oldRel = ctx.contentManager->toContentRelativePath(oldAbs);
	const std::string newRel = ctx.contentManager->toContentRelativePath(newAbs);
	if (oldRel.empty() || newRel.empty()) return;

	// The two halves go to two different places, and that is the whole point.
	//
	// IN MEMORY, here and now: until it has run, the content manager still
	// believes the asset lives at the old path, and the very next save would
	// write it back there. It touches unguarded maps the frame reads, so it
	// cannot go anywhere else.
	ctx.contentManager->retargetAssetReferencesInMemory(oldRel, newRel, folder);
	// ON DISK, on the editor's single retarget queue. This used to run inline,
	// which was both a stall (a full walk of the project, per moved asset) and a
	// race: the collaboration path already rewrites the same files from a worker,
	// and two walks over one file lose one of the two rewrites outright. The
	// queue serialises every writer and batches independent moves into one walk.
	if (ctx.enqueueRetarget) ctx.enqueueRetarget(oldRel, newRel, folder);
	else                     ctx.contentManager->retargetAssetReferencesOnDisk(oldRel, newRel, folder);
}

// The key a collaboration session addresses this file by, or empty when it is
// nothing a session carries.
//
// This exists because "content-relative path" is NOT that key for every file the
// browser shows. A C++ class lives under <project>/Source, a SIBLING of Content,
// so toContentRelativePath returns empty for it — and every caller here read
// that empty string as "no session", quietly turning a C++ class's create and
// delete into local-only operations while its edits travelled normally. The
// editor owns the mapping (EditorApplication::collabSyncKey); this reaches it.
static std::string collabKeyFor(AppContext& ctx, const std::string& absPath,
                                bool isFolder = false)
{
	if (ctx.collabKeyForPath) return ctx.collabKeyForPath(absPath, isFolder);
	// No editor behind the context (tests, tooling): fall back to the content
	// form, which is right for everything except the Source tree.
	return ctx.contentManager ? ctx.contentManager->toContentRelativePath(absPath)
	                          : std::string();
}

// Is `path` inside `folder`? Both are absolute disk paths, but they are not
// built the same way everywhere in the panel (the folder scan uses the native
// separator, the create handlers append '/'), so the boundary character is
// accepted in either form rather than compared against one separator.
static bool isUnderFolder(const std::string& path, const std::string& folder)
{
	if (folder.empty() || path.size() <= folder.size()) return false;
	if (path.compare(0, folder.size(), folder) != 0)    return false;
	const char sep = path[folder.size()];
	return sep == '/' || sep == '\\';
}

// Every file below `folderAbs`, as paths relative to it — what a folder delete
// would take with it, for the confirmation dialog. Dotfiles/dotfolders are
// skipped for the same reason GlobalState's content scan hides them: they are
// VCS/OS bookkeeping (.gitkeep, .DS_Store), not content the user put there. The
// deletion itself removes them regardless; this list only decides what the
// warning shows and whether a folder counts as empty enough to go without one.
static std::vector<std::string> collectFolderContents(const std::string& folderAbs)
{
	std::vector<std::string> out;
	std::error_code ec;
	std::filesystem::recursive_directory_iterator it(
		folderAbs, std::filesystem::directory_options::skip_permission_denied, ec);
	if (ec) return out;

	const std::filesystem::recursive_directory_iterator end;
	for (; it != end; it.increment(ec))
	{
		if (ec) break; // iteration broke down — warn with what was readable
		const std::filesystem::path p = it->path();
		if (p.filename().string().rfind('.', 0) == 0)
		{
			std::error_code dirEc;
			if (it->is_directory(dirEc)) it.disable_recursion_pending();
			continue;
		}
		std::error_code fileEc;
		if (!it->is_regular_file(fileEc)) continue;
		std::error_code relEc;
		const std::filesystem::path rel = std::filesystem::relative(p, folderAbs, relEc);
		out.push_back(relEc ? p.filename().string() : rel.generic_string());
	}
	std::sort(out.begin(), out.end());
	return out;
}

void render(AppContext& ctx, int& tabSelectRequest,
            const std::function<void(const std::string&)>& openSceneGuarded)
{
#ifdef HE_IMGUI_ENABLED
    //Content Browser
	auto [contentFolder, contentLock] = ctx.globalState->lockContentFolder();
	auto [engineFolder, engineLock]   = ctx.globalState->lockEngineFolder();
	auto [sourceFolder, sourceLock]   = ctx.globalState->lockSourceFolder();
	// Only C++ projects author gameplay as native source, so only they show the
	// Source root (its .h/.cpp classes + the h/cpp viewer).
	const bool cbShowSource = ctx.projectManager &&
		ctx.projectManager->currentProject().scriptLanguage == ProjectScriptLanguage::Cpp;
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("Content Browser", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (ctx.fontHeading) ImGui::PopFont();

    {
        const float totalWidth    = ImGui::GetContentRegionAvail().x;
        const float totalHeight   = ImGui::GetContentRegionAvail().y;
        const float splitterW     = 6.0f;
        const float minPaneWidth  = 60.0f;

        if (ctx.cbTreeWidth < 0.0f)
            ctx.cbTreeWidth = totalWidth * 0.25f;

        ctx.cbTreeWidth = std::clamp(ctx.cbTreeWidth, minPaneWidth, totalWidth - minPaneWidth - splitterW);

		ImGui::BeginChild("##cb_tree", ImVec2(ctx.cbTreeWidth, totalHeight), false);

		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::Text("Content");
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Separator();

		// ── Tree: single-click = expand/collapse, double-click = navigate ──
		static const HE::Folder* s_selectedTreeFolder = nullptr;
		// s_selectedRootKind (namespace scope, above) says which root
		// s_selectedTreeFolder/s_gridFolder belongs to. Needed because nullptr used
		// to unambiguously mean "the Content root" — three roots now exist, so
		// nullptr is ambiguous and every place that treats it as a root sentinel
		// must also check this tag.
		// The Folder backing each root kind (structured-binding refs captured above).
		auto cbRootFolder = [&](int kind) -> const HE::Folder&
		{ return kind == 1 ? engineFolder : kind == 2 ? sourceFolder : contentFolder; };
		// If the Source root is hidden (non-C++ project) but was last selected,
		// fall back to Content so the grid never shows a stale/empty Source view.
		if (s_selectedRootKind == 2 && !cbShowSource) { s_selectedRootKind = 0; s_selectedTreeFolder = nullptr; }

		// ── Selection + context-menu target ──────────────────────────────
		// Declared up here because BOTH panes write them: the grid tiles below and
		// the folder tree, which raises the same context menu rather than having
		// one of its own.
		static std::string s_selectedItem;
		static bool        s_selectedIsFolder = false;
		// Everything selected, s_selectedItem included — that one stays the ANCHOR
		// (what a rename targets, where a Shift-range starts) so every single-item
		// path keeps working unchanged. Deleting and moving read the whole set.
		//
		// Paths, not File*: the tree is rebuilt and freed on every content refresh,
		// and a selection outlives that by design.
		static std::vector<std::string> s_selection;
		// Set when a press lands on an already-selected tile: the collapse to that
		// one item is deferred to the release, so a drag can still carry the set.
		// Empty means nothing is pending.
		static std::string s_pendingCollapseTo;
		static bool        s_pendingCollapseDragged = false;
		auto isSelected = [&](const std::string& p)
		{ return std::find(s_selection.begin(), s_selection.end(), p) != s_selection.end(); };
		// Leaving a root behind invalidates the whole selection: the paths stay
		// valid strings but name things the grid no longer shows, and the next
		// Ctrl-click would silently extend the set ACROSS roots — which is how an
		// engine default ends up in the same delete as a project asset. Every place
		// that changes root goes through this, so there is one answer to "when does
		// a selection end" instead of six.
		auto clearSelection = [&]
		{
			s_selection.clear();
			s_selectedItem.clear();
			s_selectedIsFolder  = false;
			s_pendingCollapseTo.clear();
			s_pendingCollapseDragged = false;
		};

		// Read-only ground, asked PER PATH. The context menu computes its own
		// version of this for the item under the pointer, and that was enough while
		// every operation acted on that one item. It stopped being enough the moment
		// a whole selection could be deleted: the selection survives navigation
		// between roots, so a Ctrl-click could put a shipped engine default (or a
		// download-cache copy) in the same set as an ordinary project asset, and the
		// anchor's answer would wave the whole set through — deleting a file out of
		// the shared engine install that every project on the machine reads.
		auto isReadOnlyGround = [&](const std::string& p)
		{
			if (p.empty()) return false;
			if (!engineRelativePath(p).empty()) return true;    // the download cache
			if (!ctx.contentManager) return false;
			if (ContentManager::isEngineContentDevMode()) return false;
			return ctx.contentManager->isEngineDefaultPath(p) ||
			       ctx.contentManager->isEngineOverridePath(p);
		};
		static std::string s_ctxMenuItem;
		static bool        s_ctxMenuIsFolder  = false;
		// A downloaded EngineContent asset, i.e. one whose only local existence is a
		// copy in the shared per-machine download cache. Captured where the File*
		// is still in scope: the context menu only remembers a path, and the
		// cache's manifest uuid cannot be re-derived from that alone.
		static bool        s_ctxMenuIsCacheCopy = false;
		static HE::UUID    s_ctxMenuRemoteUuid;
		static bool        s_rightClickOnItem = false;

		// Drag-to-move: an asset dragged from the grid ("HE_ASSET_PATH") and
		// dropped onto a folder — a grid folder item or a tree node — records
		// the request here. It executes AFTER the draw loops (below the file
		// grid), so the folder tree is never mutated mid-iteration and the
		// Folder* pointers stay valid for the whole frame.
		// Plural, because a drag can carry a whole selection. The PAYLOAD stays one
		// path on purpose: every other drop target in the editor — the material
		// slot in the Inspector, the mesh slot, the viewport — wants exactly one
		// asset, and widening the payload would break all of them. Only the folder
		// target, whose meaning is "move these", consults the selection.
		static std::vector<std::string> s_pendingMoveSrcs;
		static std::string s_pendingMoveDst;   // absolute path of the target folder
		auto folderDropTarget = [&](const std::string& folderAbs)
		{
			if (!ImGui::BeginDragDropTarget()) return;
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
			{
				const std::string dragged(static_cast<const char*>(p->Data));
				// Dragging one OF the selected items moves all of them; dragging
				// something outside the selection moves just that one.
				if (isSelected(dragged) && s_selection.size() > 1)
					s_pendingMoveSrcs = s_selection;
				else
					s_pendingMoveSrcs.assign(1, dragged);
				s_pendingMoveDst = folderAbs;
			}
			ImGui::EndDragDropTarget();
		};

		std::function<void(const HE::Folder*, int, int)> renderTree = [&](const HE::Folder* folder, int depth, int rootKind)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
									 | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (folder->subfolders.empty())
				flags |= ImGuiTreeNodeFlags_Leaf;
			// The tree and the grid used to disagree about where you are: the panel
			// knew perfectly well which folder was on screen, and showed it only in
			// the breadcrumb. Marking the node is the whole fix.
			if (folder == s_selectedTreeFolder && rootKind == s_selectedRootKind)
				flags |= ImGuiTreeNodeFlags_Selected;

			// ID by full path: sibling subtrees may repeat folder names, and the
			// drop target below must land on THIS node, not a same-named twin.
			ImGui::PushID(folder->fullPath.c_str());
			bool open = ImGui::TreeNodeEx(folder->name.c_str(), flags);
			// Same rollup the grid tiles use: one hash lookup against the
			// precomputed dirty-folder set, so the tree costs nothing extra.
			if (ctx.git && ctx.git->isRepo() && ctx.git->folderHasChanges(folder->fullPath))
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 0.95f), "•");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Contains uncommitted changes");
			}
			folderDropTarget(folder->fullPath); // move dragged assets into this folder

			// A single click navigates. It used to take a double-click, which is the
			// gesture for "open" everywhere else — in a tree, clicking a folder IS
			// the request to look inside it. OpenOnArrow keeps the two apart: the
			// arrow still expands without moving the grid, so the tree can be
			// explored and navigated independently, which is exactly what the flag
			// is for.
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
			{
				s_selectedTreeFolder = folder;
				s_selectedRootKind   = rootKind;
				clearSelection();
			}
			// Right-click reaches the same menu the grid tiles raise. Until now the
			// tree had no menu at all: renaming or deleting a folder meant first
			// navigating into its parent so the folder appeared as a tile.
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem       = folder->fullPath;
				s_selectedIsFolder   = true;
				s_selection.assign(1, folder->fullPath);
				s_ctxMenuItem        = folder->fullPath;
				s_ctxMenuIsFolder    = true;
				s_ctxMenuIsCacheCopy = false;
				s_ctxMenuRemoteUuid  = HE::UUID{};
				// The GRID has to follow, not just the root tag: setting the tag
				// alone left the panel claiming to be in the Engine root while still
				// drawing Content, and every path derived from "the current root"
				// (create, import, the read-only gate) then answered for the wrong
				// one.
				s_selectedTreeFolder = folder;
				s_selectedRootKind   = rootKind;
				s_rightClickOnItem   = true;
			}

			if (open)
			{
				for (auto* sub : folder->subfolders)
					renderTree(sub, depth + 1, rootKind);
				ImGui::TreePop();
			}
			ImGui::PopID();
		};

		// Root "Content" node
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_DefaultOpen
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Content", rootFlags);
			folderDropTarget(contentFolder.fullPath); // move to the content root
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_selectedTreeFolder = nullptr; // back to root
				s_selectedRootKind   = 0;
				clearSelection();
			}
			if (rootOpen)
			{
				for (auto* sub : contentFolder.subfolders)
					renderTree(sub, 1, 0);
				ImGui::TreePop();
			}
		}

		// Root "Engine" node — engine-wide default content (EditorDeps/EngineContent),
		// sibling of "Content". Same tree/drag-drop/create/rename/delete machinery;
		// only the backing Folder differs.
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Engine", rootFlags);
			// The one place the three roots are visible next to each other, and
			// the question "why can I not edit anything in here" is answered.
			EditorWidgets::helpForKey("content.roots");
			folderDropTarget(engineFolder.fullPath); // move to the engine root
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_selectedTreeFolder = nullptr; // back to engine root
				s_selectedRootKind   = 1;
				clearSelection();
			}
			if (rootOpen)
			{
				for (auto* sub : engineFolder.subfolders)
					renderTree(sub, 1, 1);
				ImGui::TreePop();
			}
		}

		// Root "Source" node — the C++ project's native gameplay tree (<root>/Source),
		// sibling of Content. Only shown for C++ projects. Same tree machinery; the
		// grid groups each class's .h/.cpp into one item (see the file loop).
		if (cbShowSource)
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Source", rootFlags);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_selectedTreeFolder = nullptr; // back to source root
				s_selectedRootKind   = 2;
				clearSelection();
			}
			if (rootOpen)
			{
				for (auto* sub : sourceFolder.subfolders)
					renderTree(sub, 1, 2);
				ImGui::TreePop();
			}
		}

		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

		ImGui::Button("##cb_splitter", ImVec2(splitterW, totalHeight));

		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

		if (ImGui::IsItemActive())
			ctx.cbTreeWidth += ImGui::GetIO().MouseDelta.x;

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		ImGui::SameLine();

		ImGui::BeginChild("##cb_content", ImVec2(0.0f, totalHeight), false);

		// ── Determine which folder's content to show ──────────────────────
		// s_selectedTreeFolder nullptr → root; otherwise the selected sub-folder
		static const HE::Folder* s_gridFolder = nullptr;

		// refreshContentFolder()/refreshEngineFolder() (asset create/rename/delete,
		// project load, periodic external-change poll) delete and rebuild every
		// Folder node of THEIR OWN tree, so the navigation statics above dangle
		// after each refresh — dereferencing them was a use-after-free crash when
		// creating an asset inside a sub-folder. Re-resolve the remembered path in
		// the fresh tree; if the folder no longer exists, fall back to the root.
		// Two independent version counters (Content/Engine refresh independently),
		// re-resolution triggers if EITHER changed; s_selectedIsEngine (set by the
		// tree above) says which tree to search — the two roots never share a
		// fullPath, so there is no ambiguity in picking one.
		static uint64_t    s_treeVersionSeen   = ~0ull;
		static uint64_t    s_engineVersionSeen = ~0ull;
		static uint64_t    s_sourceVersionSeen = ~0ull;
		static std::string s_gridFolderPath;
		const uint64_t treeVersion =
			ctx.globalState ? ctx.globalState->contentFolderVersion.load(std::memory_order_acquire) : 0;
		const uint64_t engineVersion =
			ctx.globalState ? ctx.globalState->engineFolderVersion.load(std::memory_order_acquire) : 0;
		const uint64_t sourceVersion =
			ctx.globalState ? ctx.globalState->sourceFolderVersion.load(std::memory_order_acquire) : 0;
		if (treeVersion != s_treeVersionSeen || engineVersion != s_engineVersionSeen ||
		    sourceVersion != s_sourceVersionSeen)
		{
			s_treeVersionSeen   = treeVersion;
			s_engineVersionSeen = engineVersion;
			s_sourceVersionSeen = sourceVersion;
			const HE::Folder* fresh = nullptr;
			if (!s_gridFolderPath.empty())
			{
				std::function<const HE::Folder*(const HE::Folder*)> findByPath =
					[&](const HE::Folder* cur) -> const HE::Folder*
				{
					if (cur->fullPath == s_gridFolderPath) return cur;
					for (const HE::Folder* sub : cur->subfolders)
						if (const HE::Folder* hit = findByPath(sub)) return hit;
					return nullptr;
				};
				const HE::Folder& searchRoot = cbRootFolder(s_selectedRootKind);
				fresh = findByPath(&searchRoot);
				if (fresh == &searchRoot) fresh = nullptr; // root is the null state
			}
			s_gridFolder         = fresh;
			s_selectedTreeFolder = fresh;
		}

		// Sync from tree double-click
		if (s_selectedTreeFolder != s_gridFolder)
			s_gridFolder = s_selectedTreeFolder;

		const HE::Folder* displayFolder = s_gridFolder ? s_gridFolder : &cbRootFolder(s_selectedRootKind);
		s_gridFolderPath = s_gridFolder ? s_gridFolder->fullPath : std::string{};
		// Published for browsedFolderPath(). The FULL path of what is on screen,
		// root included — an import needs somewhere to write, and "" at a root would
		// make every caller re-derive which root that was.
		s_browsedFolderPath = displayFolder->fullPath;

		// ── Toolbar: where you are ────────────────────────────────────────
		// A breadcrumb, not a row of tools — so it keeps its own semantics and
		// only borrows the editor's toolbar surface: the same band, the same
		// well, the same cells the Scene and Source Control bars use, so the
		// browser stops being the one panel with its own idea of a header.
		{
			namespace T = EditorToolbar;

			// Ancestor chain to the folder on screen, by DFS from the active root.
			std::vector<const HE::Folder*> crumbs;
			if (s_gridFolder)
			{
				std::function<bool(const HE::Folder*)> findPath = [&](const HE::Folder* cur) -> bool
				{
					if (cur == s_gridFolder) return true;
					for (auto* sub : cur->subfolders)
					{
						crumbs.push_back(sub);
						if (findPath(sub)) return true;
						crumbs.pop_back();
					}
					return false;
				};
				findPath(&cbRootFolder(s_selectedRootKind));
			}

			const char* rootLabel = s_selectedRootKind == 1 ? "Engine"
			                      : s_selectedRootKind == 2 ? "Source"
			                                                : "Content";

			T::Bar bar;
			bar.group();
			if (bar.item("##bc_root", T::iconFolder, rootLabel, crumbs.empty(), true,
			             "Back to the top of this root"))
			{
				s_gridFolder         = nullptr;
				s_selectedTreeFolder = nullptr;
			}
			for (int ci = 0; ci < static_cast<int>(crumbs.size()); ++ci)
			{
				const HE::Folder*  crumb  = crumbs[ci];
				const bool         isLast = (ci == static_cast<int>(crumbs.size()) - 1);
				const std::string  id     = "##bc_" + std::to_string(ci);
				// The folder you are IN is armed rather than disabled: it is where
				// you are, not something that failed to be available.
				if (bar.item(id.c_str(), nullptr, crumb->name.c_str(), isLast, !isLast,
				             isLast ? nullptr : "Go up to this folder"))
				{
					s_gridFolder         = crumb;
					s_selectedTreeFolder = crumb;
				}
			}
			bar.endGroup();
		}

		// ── Find it ───────────────────────────────────────────────────────
		// The browser could show assets but not FIND them: with a couple of
		// thousand of them the only way to reach one was to remember which folder
		// it was in and click down to it. The row is deliberately separate from
		// the breadcrumb above — that one says where you ARE, this one says what
		// you are LOOKING FOR, and the two answer different questions.
		//
		// Typing searches the whole subtree under the folder on screen, not just
		// its top level. A search that only looks in the folder you already opened
		// is worth nothing: you would have to know the answer to ask the question.
		static std::string s_searchText;
		static int         s_typeFilter = 0;

		// The types worth narrowing to, in the order the Create menu offers them —
		// so the two lists read the same way round. Unknown is the "all" entry.
		struct TypeFilter { const char* label; HE::AssetType type; };
		static const TypeFilter kTypeFilters[] = {
			{ "All types",         HE::AssetType::Unknown },
			{ "Scene",             HE::AssetType::Scene },
			{ "Material",          HE::AssetType::Material },
			{ "Material Function", HE::AssetType::MaterialFunction },
			{ "Static Mesh",       HE::AssetType::StaticMesh },
			{ "Skeletal Mesh",     HE::AssetType::SkeletalMesh },
			{ "Texture",           HE::AssetType::Texture },
			{ "Prefab",            HE::AssetType::Prefab },
			{ "Script",            HE::AssetType::Script },
			{ "HorizonCode Class", HE::AssetType::HorizonCodeClass },
			{ "UI Widget",         HE::AssetType::Widget },
			{ "Particle System",   HE::AssetType::ParticleSystem },
			{ "Animator",          HE::AssetType::AnimatorStateMachine },
			{ "Animation Clip",    HE::AssetType::AnimationClip },
			{ "Input Action",      HE::AssetType::InputAction },
			{ "Input Mapping",     HE::AssetType::InputMappingContext },
			{ "Audio",             HE::AssetType::Audio },
			{ "Font",              HE::AssetType::Font },
			{ "Shader",            HE::AssetType::Shader },
			{ "Struct",            HE::AssetType::StructType },
			{ "Enum",              HE::AssetType::EnumType },
			{ "SaveGame Template", HE::AssetType::SaveGameTemplate },
		};
		constexpr int kTypeFilterCount = static_cast<int>(sizeof(kTypeFilters) / sizeof(kTypeFilters[0]));
		if (s_typeFilter < 0 || s_typeFilter >= kTypeFilterCount) s_typeFilter = 0;

		{
			const float comboW = 150.0f;
			const float clearW = ImGui::GetFrameHeight();
			const float avail  = ImGui::GetContentRegionAvail().x;
			ImGui::SetNextItemWidth(std::max(80.0f, avail - comboW - clearW -
			                                 ImGui::GetStyle().ItemSpacing.x * 2.0f));
			ImGui::InputTextWithHint("##cb_search", "Search this folder and everything under it",
			                         &s_searchText);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(comboW);
			if (ImGui::BeginCombo("##cb_type", kTypeFilters[s_typeFilter].label))
			{
				for (int i = 0; i < kTypeFilterCount; ++i)
					if (ImGui::Selectable(kTypeFilters[i].label, i == s_typeFilter))
						s_typeFilter = i;
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			// One click back to "everything", because clearing two controls by hand
			// to undo one search is the kind of friction that stops people using it.
			const bool anyFilter = !s_searchText.empty() || s_typeFilter != 0;
			ImGui::BeginDisabled(!anyFilter);
			if (ImGui::Button("\xC3\x97##cb_clear_filter", ImVec2(clearW, 0.0f)))
			{
				s_searchText.clear();
				s_typeFilter = 0;
			}
			ImGui::EndDisabled();
			if (anyFilter && ImGui::IsItemHovered()) ImGui::SetTooltip("Clear the search and the filter");
		}
		const bool filterActive = !s_searchText.empty() || s_typeFilter != 0;

		// ── Grid ──────────────────────────────────────────────────────────
		constexpr float k_cellSize    = 72.0f;
		constexpr float k_iconPad     = 6.0f;   // padding inside ImageButton
		constexpr float k_iconSize    = k_cellSize - k_iconPad * 2.0f;
		constexpr float k_padding     = 8.0f;

		// ── Icon + tint for one grid item ─────────────────────────────────
		// Keyed on what the file IS rather than what it is called. Engine assets
		// are all named ".hasset", so the old extension-only lookup matched none
		// of them and every one rendered as a blank button — the cached HAsset
		// header sniff is what actually identifies them. Loose source files
		// (.png/.obj/.lua/…) are not HAssets and fall through to the extension
		// map below, which now exists only for them.
		//
		// The tint travels WITH the icon instead of being recovered afterwards by
		// comparing texture handles: related types share a hue (meshes blue,
		// material-ish green, scripting yellow, animation pink, input steel) and
		// that grouping is only legible if the colour sits next to the type.
		struct AssetVisual { ImTextureID icon = 0; ImVec4 tint{ 0.75f, 0.85f, 1.0f, 1.0f }; };
		auto pickAssetVisual = [&](const HE::File* file) -> AssetVisual
		{
			const auto& I = ctx.cbIcons;
			switch (EditorAssetTypeCache::assetTypeOf(file->fullPath))
			{
				case HE::AssetType::StaticMesh:          return { I.model3d,              {0.70f, 0.80f, 1.00f, 1.0f} };
				case HE::AssetType::SkeletalMesh:        return { I.model3d,              {0.62f, 0.86f, 1.00f, 1.0f} };
				case HE::AssetType::Prefab:              return { I.prefab,               {0.58f, 0.78f, 1.00f, 1.0f} };
				case HE::AssetType::Material:            return { I.material,             {0.60f, 0.90f, 0.60f, 1.0f} };
				case HE::AssetType::MaterialFunction:    return { I.materialFunction,     {0.55f, 0.88f, 0.75f, 1.0f} };
				case HE::AssetType::Shader:              return { I.shader,               {0.50f, 0.88f, 0.82f, 1.0f} };
				case HE::AssetType::Texture:             return { I.texture,              {0.90f, 0.75f, 0.60f, 1.0f} };
				case HE::AssetType::ParticleSystem:      return { I.particleSystem,       {1.00f, 0.82f, 0.55f, 1.0f} };
				case HE::AssetType::Scene:               return { I.scene,                {0.75f, 0.65f, 1.00f, 1.0f} };
				case HE::AssetType::Widget:              return { I.widget,               {0.82f, 0.72f, 1.00f, 1.0f} };
				case HE::AssetType::Script:              return { I.script,               {0.90f, 0.90f, 0.50f, 1.0f} };
				case HE::AssetType::HorizonCodeClass:    return { I.horizonCodeClass,     {0.95f, 0.85f, 0.55f, 1.0f} };
				case HE::AssetType::AnimationClip:       return { I.animationClip,        {1.00f, 0.70f, 0.85f, 1.0f} };
				case HE::AssetType::PropertyAnimClip:    return { I.propertyAnimClip,     {0.96f, 0.76f, 0.94f, 1.0f} };
				case HE::AssetType::AnimatorStateMachine:return { I.animatorStateMachine, {0.98f, 0.72f, 0.78f, 1.0f} };
				case HE::AssetType::InputAction:         return { I.inputAction,          {0.70f, 0.88f, 0.96f, 1.0f} };
				case HE::AssetType::InputMappingContext: return { I.inputMappingContext,  {0.60f, 0.82f, 0.96f, 1.0f} };
				case HE::AssetType::Audio:               return { I.sound,                {0.60f, 0.90f, 0.90f, 1.0f} };
				case HE::AssetType::Font:                return { I.font,                 {0.92f, 0.88f, 0.80f, 1.0f} };
				// User-defined types share the HC-class glyph, tinted apart until
				// they earn their own .tga.
				case HE::AssetType::StructType:          return { I.horizonCodeClass,     {0.60f, 0.95f, 0.80f, 1.0f} };
				case HE::AssetType::EnumType:            return { I.horizonCodeClass,     {0.80f, 0.95f, 0.60f, 1.0f} };
				case HE::AssetType::SaveGameTemplate:    return { I.horizonCodeClass,     {0.95f, 0.85f, 0.95f, 1.0f} };
				case HE::AssetType::Unknown: break; // not an HAsset — try the extension
			}

			std::string e = file->extension;
			for (auto& c : e) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
			if (e == ".mat")                                   return { I.material, {0.60f, 0.90f, 0.60f, 1.0f} };
			if (e == ".obj" || e == ".fbx" || e == ".gltf"
				|| e == ".glb" || e == ".dae")                 return { I.model3d,  {0.70f, 0.80f, 1.00f, 1.0f} };
			if (e == ".svg" || e == ".ai")                     return { I.model2d,  {0.80f, 0.70f, 1.00f, 1.0f} };
			if (e == ".cs"  || e == ".lua" || e == ".py"
				|| e == ".js")                                  return { I.script,   {0.90f, 0.90f, 0.50f, 1.0f} };
			if (e == ".h"   || e == ".hpp" || e == ".hh" || e == ".hxx"
				|| e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c")
				                                                return { I.script,   {0.90f, 0.90f, 0.50f, 1.0f} };
			if (e == ".wav" || e == ".mp3" || e == ".ogg"
				|| e == ".flac")                                return { I.sound,    {0.60f, 0.90f, 0.90f, 1.0f} };
			if (e == ".png" || e == ".jpg" || e == ".jpeg"
				|| e == ".bmp" || e == ".tga" || e == ".hdr")   return { I.texture,  {0.90f, 0.75f, 0.60f, 1.0f} };
			if (e == ".hescene")                               return { I.scene,    {0.75f, 0.65f, 1.00f, 1.0f} };
			if (e == ".ttf" || e == ".otf")                    return { I.font,     {0.92f, 0.88f, 0.80f, 1.0f} };
			return {};
		};

		// ── Rendered thumbnails for mesh/material assets ──────────────────
		// Point the cache at the loaded project (a no-op after the first frame)
		// and refill its per-frame budget, then let each tile ask for its image.
		// A miss just falls back to the extension icon below.
		if (ctx.projectManager)
		{
			// Through the shared derivation: setContext compares the string, so a
			// second caller spelling the same directory differently would make the
			// two drop each other's textures on every call.
			AssetThumbnailCache::setContext(ctx.renderer, ctx.contentManager,
				AssetThumbnailCache::cacheDirForProject(ctx.projectManager->currentProject().path));
		}
		AssetThumbnailCache::beginFrame(ImGui::GetTime());

		// Column count from the HORIZONTAL stride only: an item is k_cellSize
		// wide (icon button = icon + 2×frame padding) with k_padding ItemSpacing
		// between columns; the label renders BELOW the icon and adds height, not
		// width. N items fit when N*cell + (N-1)*spacing <= avail.
		const float availW     = ImGui::GetContentRegionAvail().x;
		const int   columns    = (std::max)(1, static_cast<int>(
			(availW + k_padding) / (k_cellSize + k_padding)));

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(k_padding, k_padding));

		int col = 0;

		// ── Shared selection/rename/context state ────────────────────────
		// (The four above are declared before the tree, which raises the same
		// context menu the grid does.)
		static std::string s_renameTarget;
		static bool        s_renameIsFolder   = false;
		static char        s_renameBuf[256]   = {};
		static bool        s_openRenamePopup  = false;
		static bool        s_renameIsCreate   = false; // naming a freshly created item
	// Why the last OK was refused (empty = nothing to say). Shown under the
	// field, because a dialog that simply ignores the button reads as broken.
	static std::string s_renameError;
		// A created asset is announced to the session when its name is FINAL,
		// not when the file appears. tryCreate writes "NewMaterial.hasset" and
		// then opens the naming dialog; publishing at the write would put the
		// placeholder on every peer and, a second later, a rename — two events
		// for one user action, and the rename half needs the host's approval.
		// Holds the path from the write until the dialog resolves, whichever way
		// it resolves.
		static std::string s_pendingCreatePublish;
		// A folder travels a different way — it has no bytes — so the announcing
		// block has to know which of the two it is holding.
		static bool        s_pendingCreateIsFolder = false;
		static int         s_renameScriptLang = -1;    // creating a script: 0=Lua 1=Python; -1=not a script
		// Delete-folder confirmation: the dialog outlives the frame its context
		// menu closed in, so the target and the list of files that go with it are
		// kept as STRINGS — a Folder* would dangle across the content refresh
		// (same use-after-free the grid navigation statics above guard against).
		static std::string              s_deleteFolderTarget;
		static std::vector<std::string> s_deleteFolderFiles;
		static bool                     s_openDeleteFolderPopup = false;
		// A single asset used to go on one click of a red menu item — no dialog,
		// no undo, and nothing on screen afterwards to say what had been there.
		// Same STRING-not-pointer rule as the folder case: this outlives the
		// frame the popup is opened on.
		static std::vector<std::string> s_deleteAssetTargets;
		static bool                     s_deleteAssetIsSource = false;   // .h/.cpp pair
		static bool                     s_openDeleteAssetPopup = false;
		// Which reference scan each dialog is showing. 0 = none started (a C++
		// class, which holds no asset references, or a scan that could not be
		// built) — the section is simply omitted then.
		static std::uint64_t            s_deleteAssetScanGen  = 0;
		static std::uint64_t            s_deleteFolderScanGen = 0;
		// The same scan, asked as a question rather than as a step on the way to
		// deleting something.
		// Pattern rename over a selection: the rule, and what it applies to.
		static std::vector<std::string> s_patternTargets;
		static char                     s_patternFind[128]    = {};
		static char                     s_patternReplace[128] = {};
		static char                     s_patternPrefix[64]   = {};
		static char                     s_patternSuffix[64]   = {};
		static bool                     s_patternNumber = false;
		static int                      s_patternStart  = 1;
		static bool                     s_openPatternRenamePopup = false;
		static std::string              s_referencesTarget;
		static bool                     s_referencesIsFolder  = false;
		static std::uint64_t            s_referencesScanGen   = 0;
		static bool                     s_openReferencesPopup = false;
#ifdef HE_HAVE_LIBSSH2
		// "Remove Local Copy" confirmation — the inverse of the download popup, and
		// the same plain-data rule: the tree is rebuilt underneath this dialog.
		static std::string   s_removeCacheFullPath;
		static std::string   s_removeCacheRelPath;   // cache/SFTP-relative, the manifest's own spelling
		static HE::UUID      s_removeCacheUuid;
		static bool          s_openRemoveCachePopup = false;
		static std::uint64_t s_removeCacheScanGen   = 0;
#endif

		// Everything the scan needs about WHERE to look, filled from the content
		// manager on the frame thread and then owned by the worker.
		auto makeScanJob = [&](const std::vector<std::string>& targetsAbs, bool isFolder) -> RefScanJob
		{
			RefScanJob job;
			if (!ctx.contentManager) return job;
			job.contentRoot    = ctx.contentManager->contentRoot();
			job.projectRoot    = fs::path(job.contentRoot).parent_path().string();
			job.contentDirName = fs::path(job.contentRoot).filename().generic_string();
			job.isFolder       = isFolder;
			for (const std::string& abs : targetsAbs)
			{
				const std::string rel = ctx.contentManager->toContentRelativePath(abs);
				// A target with no content-relative path is nothing the scan can look
				// for — the Source root's C++ classes are the real case. Dropped here
				// rather than refused wholesale, so one such file in a mixed selection
				// does not cost the answer for all the others.
				if (rel.empty()) continue;
				job.targetsAbs.push_back(abs);
				job.targetsRel.push_back(rel);
			}
			return job;
		};
		// Returns 0 when there is nothing to look for, which the dialog reads as
		// "no section" rather than as "nothing references it".
		auto beginScanForMany = [&](const std::vector<std::string>& targetsAbs,
		                            bool isFolder) -> std::uint64_t
		{
			RefScanJob job = makeScanJob(targetsAbs, isFolder);
			if (job.contentRoot.empty() || job.targetsAbs.empty()) return 0;
			return startReferenceScan(std::move(job));
		};
		auto beginScanFor = [&](const std::string& targetAbs, bool isFolder) -> std::uint64_t
		{
			return beginScanForMany(std::vector<std::string>{ targetAbs }, isFolder);
		};
#ifdef HE_HAVE_LIBSSH2
		// Remote-download confirmation: same "outlives the popup's opening frame,
		// so keep it as plain data, never a File*" reasoning as the delete-folder
		// state above — a content refresh (which the download itself triggers)
		// would dangle a pointer into the old tree.
		static std::string s_remoteDownloadRelativePath;
		static HE::UUID    s_remoteDownloadUuid;
		static std::string s_remoteDownloadTabLabel;
		static std::string s_remoteDownloadFullPath;
		static bool        s_openRemoteDownloadPopup = false;
		// A completed download's onComplete callback (see the confirmation popup
		// below) runs on a WORKER thread and must not touch ctx.tabs directly —
		// it drops the path here instead, and the drain right below (main thread,
		// once per frame) does the actual opening. Same shape as ContentManager's
		// AsyncSink: producer on a worker, consumer on the main thread, never both.
		static std::mutex               s_pendingOpenMutex;
		static std::vector<std::string> s_pendingOpenFullPaths;
#endif

		// Opens (or focuses) a tab for `fullPath`, exactly what a double-click on a
		// known asset type does. Factored out so a completed EngineContent download
		// can auto-open the tab the user originally asked for, without duplicating
		// the type dispatch below.
		auto openAssetTab = [&](const std::string& fullPath)
		{
			if (std::filesystem::path(fullPath).extension() == ".hescene")
			{
				openSceneGuarded(fullPath);
				return;
			}
			// Script assets open the code editor tab, material assets the node-graph
			// editor tab. Other asset types have no dedicated editor yet → no-op.
			// C++ source/header (Source root) opens the h/cpp class viewer; a raw
			// .wav opens the audio tab (auditioning a source file must not require
			// importing it first). Both predicates are raw extension checks, not
			// HAsset sniffs, so they must be tested explicitly here.
			if (!(CppClassEditorPanel::isCppSourceAsset(fullPath) ||
			      AudioEditorPanel::isAudioAsset(fullPath) ||
			      ScriptEditorPanel::isScriptAsset(fullPath) ||
			      MaterialEditorPanel::isMaterialAsset(fullPath) ||
			      MaterialEditorPanel::isMaterialFunctionAsset(fullPath) ||
			      UIEditorPanel::isWidgetAsset(fullPath) ||
			      HorizonCodeClassPanel::isClassAsset(fullPath) ||
			      InputAssetPanel::isInputAsset(fullPath) ||
			      TypeAssetPanel::isTypeAsset(fullPath) ||
			      SkeletalMeshEditorPanel::isSkeletalMeshAsset(fullPath) ||
			      StaticMeshEditorPanel::isStaticMeshAsset(fullPath) ||
			      ParticleGraphEditorPanel::isParticleAsset(fullPath) ||
			      AnimatorStateMachineEditorPanel::isAnimatorStateMachineAsset(fullPath)))
				return; // no dedicated editor for this type — same no-op the old inline dispatch had

			const std::string tabLabel = std::filesystem::path(fullPath).stem().string();
			auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
				[&](const AppContext::EditorTab& t){ return t.assetPath == fullPath; });
			if (it == ctx.tabs.end())
			{
				ctx.tabs.push_back({ tabLabel, fullPath, true, true });
				ctx.activeTab = static_cast<int>(ctx.tabs.size()) - 1;
			}
			else
			{
				ctx.activeTab = static_cast<int>(std::distance(ctx.tabs.begin(), it));
			}
			// Force the tab bar to select this tab next frame (else ImGui keeps the
			// Scene tab selected and the editor never opens).
			tabSelectRequest = ctx.activeTab;
		};

#ifdef HE_HAVE_LIBSSH2
		// Drain downloads that finished since the last frame — once per frame,
		// before the grid loop, so it can never re-enter mid-iteration.
		{
			std::vector<std::string> ready;
			{
				std::lock_guard<std::mutex> lock(s_pendingOpenMutex);
				ready.swap(s_pendingOpenFullPaths);
			}
			for (const std::string& p : ready) openAssetTab(p);
		}
#endif
		// Name-a-C++-class popup (C++ projects only): the create menu stages a
		// class name here, then this popup writes Source/<Name>.{h,cpp}.
		static bool        s_openCppClassPopup = false;
		static char        s_cppClassName[128] = "GameplayClass";

		// ── Folders first ─────────────────────────────────────────────────
		// Hidden while a search is running: the answer to "where is Rock_Cliff" is
		// a list of assets, and folders in the middle of it are noise you have to
		// read past. The breadcrumb still says which subtree is being searched.
		static const std::vector<HE::Folder*> kNoFolders;
		for (auto* sub : filterActive ? kNoFolders : displayFolder->subfolders)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = isSelected(sub->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(sub->fullPath.c_str());

			if (isSel)
			{
				using namespace HE::Ed::Theme;
				ImGui::PushStyleColor(ImGuiCol_Button,        mix(warm(0.16f), Accent, 0.62f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(warm(0.16f), AccentHi, 0.78f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  mix(warm(0.16f), Accent, 0.90f));
			}
			else
			{
				using namespace HE::Ed::Theme;
				ImGui::PushStyleColor(ImGuiCol_Button,        warm(0.18f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, warm(0.28f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  warm(0.38f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			if (ctx.cbIcons.folder)
			{
				ImGui::ImageButton("##icon", ctx.cbIcons.folder,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0),
					ImVec4(0.96f, 0.78f, 0.26f, 1.0f));
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}
			// A folder with changes somewhere beneath it gets a small dot, so a
			// user can find modified assets without opening every folder. One hash
			// lookup: the rollup is precomputed on the worker rather than walked
			// per tile per frame.
			if (ctx.git && ctx.git->isRepo() && ctx.git->folderHasChanges(sub->fullPath))
			{
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const float r = 4.5f;
				const ImVec2 c(mn.x + r + 4.0f, mx.y - r - 4.0f);
				dl->AddCircleFilled(c, r, IM_COL32(255, 200, 90, 235));
				dl->AddCircle(c, r, IM_COL32(20, 20, 22, 220), 0, 1.0f);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("Contains uncommitted changes");
			}

			folderDropTarget(sub->fullPath); // move dragged assets into this folder

			// Left click → select. A folder is always a selection of one: the
			// multi-select gestures below operate on assets, and mixing a folder
			// into that set would make "delete the selection" mean two very
			// different scopes at once.
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
				s_selection.assign(1, sub->fullPath);
			}
			// Double-click → navigate into folder
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_gridFolder         = sub;
				s_selectedTreeFolder = sub;
				// The folder you just entered stays selected otherwise, and a
				// Ctrl-click on an asset inside then builds a set holding BOTH — one
				// that "delete the selection" cannot mean anything sensible about,
				// since a folder and an asset are removed by different operations
				// with different confirmations.
				clearSelection();
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
				s_ctxMenuItem      = sub->fullPath;
				s_ctxMenuIsFolder  = true;
				// A folder is never a single cache copy — clearing this is what keeps
				// the previous right-click's answer from leaking into this menu.
				s_ctxMenuIsCacheCopy = false;
				s_ctxMenuRemoteUuid  = HE::UUID{};
				s_rightClickOnItem = true;
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
			ImGui::PopID();

			// Centered label (truncated)
			const float labelW = k_cellSize;
			std::string label  = sub->name;
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(HE::Ed::Theme::TextHeading, "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());

			ImGui::EndGroup();
			++col;
		}

		// ── Files ─────────────────────────────────────────────────────────
		// In the Source root a class's header and .cpp are ONE item: drop a .cpp
		// when a same-stem header sits in the same folder, so the header stands in
		// for the pair (double-click opens both in the C++ viewer). Lone .cpp files
		// (e.g. GameLogic.cpp) and non-C++ files pass through unchanged.
		auto cbLowerExt = [](const std::string& e){ std::string s=e; for(auto&c:s) c=(char)::tolower((unsigned char)c); return s; };
		auto cbIsHeaderExt = [&](const std::string& e){ std::string s=cbLowerExt(e); return s==".h"||s==".hpp"||s==".hh"||s==".hxx"; };
		auto cbIsSourceExt = [&](const std::string& e){ std::string s=cbLowerExt(e); return s==".cpp"||s==".cc"||s==".cxx"||s==".c"; };
		std::vector<const HE::File*> gridFiles;
		// A file is dropped when its own .h sits beside it — the pair is one item.
		// Takes the containing folder rather than reading displayFolder, because a
		// search walks folders the grid is not standing in.
		auto cbCollapsedIntoHeader = [&](const HE::Folder* owner, const HE::File* f)
		{
			if (s_selectedRootKind != 2 || !cbIsSourceExt(f->extension)) return false;
			const std::string stem = std::filesystem::path(f->name).stem().string();
			for (auto* g : owner->files)
				if (cbIsHeaderExt(g->extension) &&
				    std::filesystem::path(g->name).stem().string() == stem)
					return true;
			return false;
		};

		// Where a search hit actually lives, relative to the folder on screen —
		// without it a flat list of twelve "Rock" tiles says nothing about which
		// one you want.
		std::unordered_map<const HE::File*, std::string> foundIn;

		if (!filterActive)
		{
			gridFiles.reserve(displayFolder->files.size());
			for (auto* f : displayFolder->files)
				if (!cbCollapsedIntoHeader(displayFolder, f)) gridFiles.push_back(f);
		}
		else
		{
			// A search walks the whole subtree and lowercases every name it looks
			// at. Doing that per frame is an allocation per asset per frame — on a
			// project with a couple of thousand of them, the panel would spend the
			// frame budget re-deriving an answer that only changes when the query,
			// the folder, or the content itself does. So it is derived once and kept
			// until one of those four inputs moves.
			//
			// The cached vector holds File* into the folder tree, which a refresh
			// FREES — that is exactly why the tree version is part of the key.
			static std::string  s_cachedNeedle;
			static int          s_cachedType    = -1;
			static std::string  s_cachedFolder;
			// The three counters are compared SEPARATELY. Folding them into one
			// number (an XOR of shifted values, say) is not a freshness key: three
			// independent counters can move in a way that leaves the fold unchanged,
			// and the cache then hands back File* into a folder tree the refresh has
			// already freed. Nothing else in this panel folds them either — the
			// re-resolution block at the top compares all three, for the same reason.
			static std::uint64_t s_cachedTreeVersion   = ~0ull;
			static std::uint64_t s_cachedEngineVersion = ~0ull;
			static std::uint64_t s_cachedSourceVersion = ~0ull;
			static std::vector<const HE::File*> s_cachedHits;
			static std::unordered_map<const HE::File*, std::string> s_cachedFoundIn;
			if (s_cachedNeedle == s_searchText && s_cachedType == s_typeFilter &&
			    s_cachedFolder == displayFolder->fullPath &&
			    s_cachedTreeVersion == treeVersion && s_cachedEngineVersion == engineVersion &&
			    s_cachedSourceVersion == sourceVersion)
			{
				gridFiles = s_cachedHits;
				foundIn   = s_cachedFoundIn;
			}
			else
			{
			// Case-insensitive substring, which is what people expect from a box
			// that says "search" — not a prefix match and not a glob.
			std::string needle = s_searchText;
			for (char& c : needle) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
			const HE::AssetType wantType = kTypeFilters[s_typeFilter].type;

			std::function<void(const HE::Folder*, const std::string&)> collect =
				[&](const HE::Folder* folder, const std::string& rel)
			{
				for (auto* f : folder->files)
				{
					if (cbCollapsedIntoHeader(folder, f)) continue;
					if (!needle.empty())
					{
						std::string name = f->name;
						for (char& c : name) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
						if (name.find(needle) == std::string::npos) continue;
					}
					// The type sniff is cached per path, so filtering a large tree
					// costs one header read per asset ONCE, not once per frame.
					if (wantType != HE::AssetType::Unknown &&
					    EditorAssetTypeCache::assetTypeOf(f->fullPath) != wantType) continue;
					gridFiles.push_back(f);
					if (!rel.empty()) foundIn[f] = rel;
				}
				for (auto* sub : folder->subfolders)
					collect(sub, rel.empty() ? sub->name : rel + "/" + sub->name);
			};
			collect(displayFolder, std::string{});

			s_cachedNeedle        = s_searchText;
			s_cachedType          = s_typeFilter;
			s_cachedFolder        = displayFolder->fullPath;
			s_cachedTreeVersion   = treeVersion;
			s_cachedEngineVersion = engineVersion;
			s_cachedSourceVersion = sourceVersion;
			s_cachedHits          = gridFiles;
			s_cachedFoundIn       = foundIn;
			}
		}

		if (filterActive)
		{
			// The one place the grid pane speaks in sentences, and the sentence that
			// matters is the empty one: unwrapped, "Nothing here matches. The search
			// covers this folder and everything under it." is cut off somewhere around
			// "covers", so the reader is told there are no hits and NOT told that the
			// whole subtree was searched — which is the difference between "it is not
			// here" and "it is nowhere below here".
			//
			// Deliberately scoped to this block rather than to the whole ##cb_content
			// child: this if-body closes long before EndChild(), whereas a guard at
			// the top of the child would pop after it and land on the panel window.
			// The tiles below want no part of it either — their labels are truncated
			// to the cell width by hand, and a wrap position that ever caught one
			// would push it onto a second line and break the row's alignment.
			EditorWidgets::WrapText wrap;
			const int hits = static_cast<int>(gridFiles.size());
			if (hits == 0)
				ImGui::TextDisabled("Nothing here matches. The search covers this folder and everything under it.");
			else
				ImGui::TextDisabled("%d match%s", hits, hits == 1 ? "" : "es");
			ImGui::Spacing();
		}

		for (auto* file : gridFiles)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = isSelected(file->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(file->fullPath.c_str());

			if (isSel)
			{
				using namespace HE::Ed::Theme;
				ImGui::PushStyleColor(ImGuiCol_Button,        mix(warm(0.16f), Accent, 0.62f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(warm(0.16f), AccentHi, 0.78f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  mix(warm(0.16f), Accent, 0.90f));
			}
			else
			{
				using namespace HE::Ed::Theme;
				ImGui::PushStyleColor(ImGuiCol_Button,        warm(0.18f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, warm(0.28f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  warm(0.38f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			// A rendered thumbnail wins over the generic icon and is drawn untinted,
			// since its colours ARE the information. Types without one — and assets
			// whose tile has not been rendered yet — keep the per-type glyph.
			ImTextureID thumb = reinterpret_cast<ImTextureID>(
				AssetThumbnailCache::get(file->fullPath));
			const AssetVisual vis = thumb ? AssetVisual{ thumb, ImVec4(1, 1, 1, 1) }
			                              : pickAssetVisual(file);
			if (vis.icon)
			{
				ImGui::ImageButton("##icon", vis.icon,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0), vis.tint);
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}

			// A material FUNCTION renders as the sphere its own editor tab shows —
			// which is exactly a material's tile. The corner badge is what tells
			// the two apart at a glance; drawn over the tile rather than baked into
			// the image so the cached thumbnail stays a plain picture of the shader.
			if (thumb && EditorAssetTypeCache::is(file->fullPath, HE::AssetType::MaterialFunction))
			{
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const float r = 9.0f;
				const ImVec2 c(mx.x - r - 3.0f, mn.y + r + 3.0f);
				dl->AddCircleFilled(c, r, IM_COL32(24, 26, 24, 225));
				dl->AddCircle(c, r, IM_COL32(150, 225, 190, 235), 0, 1.5f);
				const ImVec2 ts = ImGui::CalcTextSize("f");
				dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
				            IM_COL32(180, 240, 210, 255), "f");
			}

			// EngineContent SFTP remote-only badge, TOP-left (the material-function
			// badge owns the top-right, the git-status badge owns the bottom-left —
			// see below). A plain downward-triangle-in-a-circle rather than a text
			// glyph, so it never depends on the loaded font's glyph ranges.
			if (file->isRemoteOnly)
			{
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const float r = 9.0f;
				const ImVec2 c(mn.x + r + 3.0f, mn.y + r + 3.0f);
				dl->AddCircleFilled(c, r, IM_COL32(24, 26, 30, 225));
				dl->AddCircle(c, r, IM_COL32(120, 170, 230, 235), 0, 1.5f);
				const float tr = 3.5f;
				dl->AddTriangleFilled(
					ImVec2(c.x - tr, c.y - tr * 0.6f),
					ImVec2(c.x + tr, c.y - tr * 0.6f),
					ImVec2(c.x,      c.y + tr * 0.9f),
					IM_COL32(150, 195, 240, 255));
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("On the EngineContent server — double-click to download");
			}
			// The same corner, the opposite state: this one HAS been downloaded, and
			// is therefore the only kind of engine asset a project may remove again.
			// Without a mark of its own a downloaded asset looked exactly like a
			// shipped default, so "Remove Local Copy" appeared on some engine tiles
			// and not others with nothing on screen explaining the difference. Drawn
			// as an outline rather than a filled triangle — present, not incoming.
			else if (file->isLocalCacheCopy)
			{
				const ImVec2 mn = ImGui::GetItemRectMin();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const float r = 9.0f;
				const ImVec2 c(mn.x + r + 3.0f, mn.y + r + 3.0f);
				dl->AddCircleFilled(c, r, IM_COL32(24, 30, 26, 225));
				dl->AddCircle(c, r, IM_COL32(120, 200, 160, 235), 0, 1.5f);
				dl->AddCircleFilled(c, 3.0f, IM_COL32(150, 225, 190, 255));
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("Downloaded to this machine's shared EngineContent cache");
			}

			// Source-control status, drawn in the BOTTOM-left so it cannot collide
			// with the material-function badge in the top-right. Same technique:
			// over the tile via the item rect, so the cached thumbnail stays a
			// plain picture.
			//
			// Looked up by absolute path rather than by caching a pointer into the
			// status map — the Content Browser rebuilds its File* nodes wholesale
			// on every refresh, so anything held across a frame dangles.
			if (ctx.git && ctx.git->isRepo())
			{
				if (const HE::Sc::FileEntry* e = ctx.git->entryForAbsolutePath(file->fullPath))
				{
					if (e->dirty())
					{
						// The worktree state is what the user is looking at; the
						// staged state matters in the panel, not on a tile.
						const HE::Sc::FileState st =
							(e->worktree != HE::Sc::FileState::Unmodified) ? e->worktree : e->index;
						const char* glyph = "?";
						ImU32 colour = IM_COL32(170, 170, 170, 255);
						switch (st)
						{
						case HE::Sc::FileState::Added:      glyph = "+"; colour = IM_COL32(140, 215, 140, 255); break;
						case HE::Sc::FileState::Modified:   glyph = "M"; colour = IM_COL32(255, 200, 90,  255); break;
						case HE::Sc::FileState::Deleted:    glyph = "-"; colour = IM_COL32(240, 130, 115, 255); break;
						case HE::Sc::FileState::Renamed:
						case HE::Sc::FileState::Copied:     glyph = "R"; colour = IM_COL32(150, 190, 255, 255); break;
						case HE::Sc::FileState::Conflicted: glyph = "!"; colour = IM_COL32(255, 100, 100, 255); break;
						case HE::Sc::FileState::Untracked:  glyph = "?"; colour = IM_COL32(170, 170, 170, 255); break;
						default: break;
						}
						const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
						ImDrawList* dl = ImGui::GetWindowDrawList();
						const float r = 8.0f;
						const ImVec2 c(mn.x + r + 3.0f, mx.y - r - 3.0f);
						dl->AddCircleFilled(c, r, IM_COL32(20, 20, 22, 230));
						dl->AddCircle(c, r, colour, 0, 1.5f);
						const ImVec2 ts = ImGui::CalcTextSize(glyph);
						dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), colour, glyph);

						// Words on hover, not just a letter: the glyph alphabet
						// is git's, and not everyone reads it.
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
						{
							const char* what = "Changed";
							switch (st)
							{
							case HE::Sc::FileState::Added:      what = "Added — staged for the next commit"; break;
							case HE::Sc::FileState::Modified:   what = "Modified since the last commit"; break;
							case HE::Sc::FileState::Deleted:    what = "Deleted"; break;
							case HE::Sc::FileState::Renamed:
							case HE::Sc::FileState::Copied:     what = "Renamed"; break;
							case HE::Sc::FileState::Conflicted: what = "Merge conflict — resolve before committing"; break;
							case HE::Sc::FileState::Untracked:  what = "Not in source control yet"; break;
							default: break;
							}
							ImGui::SetTooltip("%s", what);
						}
					}
				}
			}

			// ── Somebody is editing this asset right now ──────────────────
			// The lock table already knows; until now it only showed up once
			// you had opened the asset and found the tab read-only. On the tile
			// it is worth something else entirely: you see it BEFORE you invest
			// in opening it, and you can go and ask them instead.
			//
			// Drawn in the holder's own colour — the same one their cursor,
			// their presence dot and the outliner use, so "orange means Anna"
			// holds everywhere rather than being learned per panel.
			if (ctx.collab && ctx.collab->inSession() && ctx.contentManager && file)
			{
				// The same key the tab and the lock table use — a C++ class has
				// no content-relative form, so deriving it that way meant the
				// one file kind whose tile most needs a padlock never got one.
				const std::string rel = collabKeyFor(ctx, file->fullPath);
				const HE::Net::LockInfo* lock =
					rel.empty() ? nullptr : ctx.collab->assetLockInfo(rel);
				if (lock && lock->owner != ctx.collab->localParticipant())
				{
					float rgb[3] = { 1.0f, 0.75f, 0.3f };
					ctx.collab->colorFor(lock->owner, rgb);
					const ImU32 col = IM_COL32(int(rgb[0] * 255), int(rgb[1] * 255),
					                           int(rgb[2] * 255), 255);
					const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
					ImDrawList* dl = ImGui::GetWindowDrawList();
					// Bottom-RIGHT, which is the last free corner: the git dot owns
					// the bottom-left, the download/cache badge the top-left, and the
					// material-function "f" the top-right — and that last one drew in
					// exactly this spot, so a locked material function showed one
					// badge on top of the other and neither was readable.
					const float r = 8.0f;
					const ImVec2 c(mx.x - r - 3.0f, mx.y - r - 3.0f);
					dl->AddCircleFilled(c, r, IM_COL32(20, 20, 22, 230));
					dl->AddCircle(c, r, col, 0, 1.5f);
					// A padlock as two shapes rather than a glyph: the font's
					// glyph ranges are not guaranteed to carry one, and a missing
					// glyph would draw an empty box that means nothing.
					dl->AddRectFilled(ImVec2(c.x - 3.5f, c.y - 0.5f),
					                  ImVec2(c.x + 3.5f, c.y + 4.0f), col, 1.0f);
					constexpr float kPi = 3.14159265f;   // IM_PI is imgui_internal
					dl->PathArcTo(ImVec2(c.x, c.y - 0.5f), 2.6f, kPi, kPi * 2.0f, 12);
					dl->PathStroke(col, 0, 1.4f);

					if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					{
						ImGui::SetTooltip("%s is editing this — it is read-only for you.",
							lock->ownerName.empty() ? "Someone" : lock->ownerName.c_str());
					}
				}
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);

			// Drag source — carries the asset's absolute path so drop targets
			// (e.g. the Material slot in the inspector) can load it. Disabled in the
			// Source root: a class item stands for a .h/.cpp pair, and moving just
			// the one file would orphan its sibling.
			if (s_selectedRootKind != 2 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				// The press that started this deferred its "collapse to just me"
				// (see the click handler): a drag DID happen, so the selection
				// stands. Recorded here because this is the one place that knows,
				// and by the time the button comes up the drag is already over.
				s_pendingCollapseDragged = true;
				ImGui::SetDragDropPayload("HE_ASSET_PATH",
					file->fullPath.c_str(), file->fullPath.size() + 1);
				// A drag carrying a whole selection should say so, or the user has
				// to remember what they picked while the tiles are under a cursor.
				const std::string dragLabel =
					(isSelected(file->fullPath) && s_selection.size() > 1)
						? std::to_string(s_selection.size()) + " assets"
						: std::filesystem::path(file->name).stem().string();
				ImGui::TextUnformatted(dragLabel.c_str());
				ImGui::EndDragDropSource();
			}

			// Left click → select. Ctrl/Cmd adds or removes one, Shift takes the
			// run from the anchor to here — the two gestures every file manager
			// uses, so nobody has to be told. A plain click still replaces the
			// selection, which is what makes them safe to reach for.
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				const ImGuiIO& io = ImGui::GetIO();
				const bool additive = io.KeyCtrl || io.KeySuper;
				const bool ranged   = io.KeyShift;
				if (additive)
				{
					auto it = std::find(s_selection.begin(), s_selection.end(), file->fullPath);
					if (it != s_selection.end())
					{
						s_selection.erase(it);
						// The anchor cannot be a file that is no longer selected: the
						// next Shift-click would range from something invisible.
						if (s_selectedItem == file->fullPath)
							s_selectedItem = s_selection.empty() ? std::string{} : s_selection.back();
					}
					else
					{
						s_selection.push_back(file->fullPath);
						s_selectedItem = file->fullPath;
					}
					s_selectedIsFolder = false;
				}
				else if (ranged && !s_selectedItem.empty())
				{
					// The run is taken over the list AS DRAWN, so a Shift-click after
					// a search selects what the user can see, not what the folder
					// happens to hold.
					int from = -1, to = -1;
					for (int gi = 0; gi < static_cast<int>(gridFiles.size()); ++gi)
					{
						if (gridFiles[gi]->fullPath == s_selectedItem) from = gi;
						if (gridFiles[gi]->fullPath == file->fullPath)  to   = gi;
					}
					if (from >= 0 && to >= 0)
					{
						if (from > to) std::swap(from, to);
						s_selection.clear();
						for (int gi = from; gi <= to; ++gi)
							s_selection.push_back(gridFiles[gi]->fullPath);
					}
					else
					{
						s_selection.assign(1, file->fullPath);
						s_selectedItem = file->fullPath;
					}
					s_selectedIsFolder = false;
				}
				else if (isSelected(file->fullPath) && s_selection.size() > 1)
				{
					// Pressing on something that is ALREADY selected does not collapse
					// the selection yet — that is what makes dragging a set possible at
					// all. ImGui cannot start a drag on the press frame (it needs the
					// mouse to travel past the drag threshold first), so collapsing
					// here reduced the selection to one before BeginDragDropSource
					// ever activated: the documented "drag the selection onto a
					// folder" gesture could never move more than a single asset.
					//
					// The collapse still happens — on RELEASE, and only if no drag
					// started. That is the file-manager rule, and it keeps a plain
					// click on one of many selected items meaning "now just this one".
					s_pendingCollapseTo = file->fullPath;
					s_selectedItem      = file->fullPath;
					s_selectedIsFolder  = false;
				}
				else
				{
					s_selectedItem     = file->fullPath;
					s_selectedIsFolder = false;
					s_selection.assign(1, file->fullPath);
				}
			}
			// Double-click → open tab (or, for an EngineContent asset that only
			// exists on the SFTP server so far, ask before downloading it — see
			// the "##cb_remote_download_popup" modal below).
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
#ifdef HE_HAVE_LIBSSH2
				if (file->isRemoteOnly)
				{
					s_remoteDownloadRelativePath = engineRelativePath(file->fullPath);
					s_remoteDownloadUuid         = file->remoteUuid;
					s_remoteDownloadTabLabel     = std::filesystem::path(file->name).stem().string();
					s_remoteDownloadFullPath     = file->fullPath;
					s_openRemoteDownloadPopup    = true;
				}
				else
#endif
					openAssetTab(file->fullPath);
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				// Right-clicking INSIDE the selection keeps it — that is the whole
				// point of having selected twelve things before reaching for the
				// menu. Right-clicking outside it selects just that one, the way a
				// left click would.
				if (!isSelected(file->fullPath))
					s_selection.assign(1, file->fullPath);
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
				s_ctxMenuItem      = file->fullPath;
				s_ctxMenuIsFolder  = false;
				// The manifest uuid travels with the path: the context menu keeps
				// only a string, and a cache copy's uuid is not recoverable from the
				// file alone for raw (non-.hasset) entries. The path predicate is
				// asked as well, so a stale tree node (the file removed behind the
				// editor's back) cannot offer to remove something that is gone.
				s_ctxMenuIsCacheCopy = file->isLocalCacheCopy &&
				                       isEngineCacheCopy(ctx.contentManager, file->fullPath);
				s_ctxMenuRemoteUuid  = file->remoteUuid;
				s_rightClickOnItem = true;
			}

			ImGui::PopID();

			// Centered label (stem only, truncated)
			const float labelW = k_cellSize;
			const std::string fullLabel = std::filesystem::path(file->name).stem().string();
			std::string label  = fullLabel;
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(HE::Ed::Theme::TextHeading, "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());
			// A 72px tile truncates almost every real asset name to about ten
			// characters, and until now there was nowhere at all to read the rest —
			// two assets differing in their suffix looked identical. The search
			// makes it worse, since a hit can come from any depth, so the folder it
			// was found in belongs here too.
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			{
				const auto whereIt = foundIn.find(file);
				if (whereIt != foundIn.end())
					ImGui::SetTooltip("%s\nin %s", fullLabel.c_str(), whereIt->second.c_str());
				else if (label != fullLabel)
					ImGui::SetTooltip("%s", fullLabel.c_str());
			}

			ImGui::EndGroup();
			++col;
		}

		ImGui::PopStyleVar();

		// ── Execute a drag-to-move recorded by a folder drop target ───────
		// Same operation as the rename popup's OK handler (a rename IS a move
		// within one folder): plain disk rename + selection/tab fixups + a
		// quiet content refresh. Skipped when the drop lands on the asset's
		// own folder or the name already exists in the target.
		//
		// And in a session it goes through the host, exactly as a rename does.
		// It used to be the ONE operation that did not: create, rename and delete
		// all ask, while dragging a file into another folder happened here and
		// nowhere else — so every peer kept the old path, and the next scene save
		// handed them a reference to a file they did not have. That is the same
		// failure the rename path was fixed for; the drag is a rename whose new
		// name happens to sit in a different folder, and the protocol already has
		// the message for it.
		//
		// One batch for the whole drop, so twelve dragged assets are one row in
		// the host's in-tray rather than twelve.
		if (!s_pendingMoveSrcs.empty() && !s_pendingMoveDst.empty() && ctx.collab)
			ctx.collab->beginAssetOpBatch();
		for (const std::string& moveSrc : s_pendingMoveDst.empty()
		                                 ? std::vector<std::string>{} : s_pendingMoveSrcs)
		{
			const std::filesystem::path src(moveSrc);
			const std::filesystem::path dst =
				std::filesystem::path(s_pendingMoveDst) / src.filename();
			std::error_code ec;
			const bool sameFolder =
				std::filesystem::equivalent(src.parent_path(), s_pendingMoveDst, ec);
			// Engine defaults/overrides don't move via drag in normal mode —
			// same "read-only ground" rule as create/rename/delete above.
			//
			// The two path predicates alone are NOT that rule, and the gap is
			// reachable in three directions. (1) Neither answers true for a file in
			// the EngineContent download cache, so a downloaded asset could be
			// dragged OUT of the machine-wide cache into the project: every other
			// project on the machine loses its copy, the references are rewritten to
			// point at the moved file, and none of removeCachedCopyNow's bookkeeping
			// runs. (2) The same in reverse — a project asset dragged INTO a
			// materialized cache folder leaves the project tree entirely (out of
			// source control, out of the packaged build). (3) Both predicates are
			// containment tests built on fs::relative, which answers "." for a path
			// compared with itself and is then rejected by their leading-dot check —
			// so the engine root and <contentRoot>/Engine, as drop targets
			// THEMSELVES, were never covered by either.
			//
			// The cache half is deliberately OUTSIDE the dev-mode escape hatch, the
			// same way the context menu has it: HE_ENGINE_CONTENT_EDITABLE=1 exists
			// so engine authors can edit the shared DEFAULTS in place, and even they
			// never author into a per-machine download mirror.
			auto isDownloadCacheGround = [&](const std::string& p)
			{
				if (!engineRelativePath(p).empty()) return true;
				return fs::path(GlobalState::engineContentCacheDir()) == fs::path(p);
			};
			auto engineReadOnlyGround = [&](const std::string& p)
			{
				if (!ctx.contentManager) return false;
				if (ctx.contentManager->isEngineDefaultPath(p))  return true;
				if (ctx.contentManager->isEngineOverridePath(p)) return true;
				std::error_code cmpEc;
				const std::string engineRoot   = ctx.contentManager->engineContentRoot();
				const std::string overrideRoot = ctx.contentManager->contentRoot() + "/Engine";
				if (!engineRoot.empty() && std::filesystem::exists(p, cmpEc) &&
				    std::filesystem::equivalent(p, engineRoot, cmpEc) && !cmpEc) return true;
				cmpEc.clear();
				if (std::filesystem::exists(overrideRoot, cmpEc) && std::filesystem::exists(p, cmpEc) &&
				    std::filesystem::equivalent(p, overrideRoot, cmpEc) && !cmpEc) return true;
				return false;
			};
			const bool engineLocked =
				isDownloadCacheGround(moveSrc) || isDownloadCacheGround(s_pendingMoveDst) ||
				(!ContentManager::isEngineContentDevMode() &&
				 (engineReadOnlyGround(moveSrc) || engineReadOnlyGround(s_pendingMoveDst)));
			const bool moveIsAllowed =
				!engineLocked && !sameFolder && std::filesystem::exists(src) &&
				!std::filesystem::exists(dst);

			// Ask the host — the same call the rename dialog makes, for the same
			// reason, and deliberately NOT gated on being a client: as host,
			// requestAssetRename broadcasts the move instead of asking, so leaving
			// the host out would move the file here and nowhere else.
			//
			// folder=false is not laziness. A folder can never be in this loop: the
			// drag source sits on file tiles only, and selecting a folder assigns a
			// one-element selection, so a multi-drag is files by construction.
			//
			// The engine/read-only and collision checks run BEFORE the ask, not
			// after: a drop the host would have to undo anyway should never reach
			// the host's in-tray, and a request whose local half we then refuse to
			// perform would desync us from the peers who did perform it.
			bool requested = false;
			if (moveIsAllowed && ctx.collab && ctx.contentManager &&
			    ctx.collab->inSession())
			{
				const std::string relOld = ctx.contentManager->toContentRelativePath(moveSrc);
				const std::string relNew =
					ctx.contentManager->toContentRelativePath(dst.string());
				requested = !relOld.empty() && !relNew.empty() &&
				            ctx.collab->requestAssetRename(relOld, relNew, /*folder=*/false);
			}

			// Nothing moves locally when it was asked for; it moves when the host
			// says so, on every machine at once.
			if (moveIsAllowed && !requested)
			{
				ec.clear();
				std::filesystem::rename(src, dst, ec);
				if (!ec)
				{
					// Everything that pointed at the old path — other assets' stored
					// references and the asset's own embedded path — follows it here.
					retargetReferences(ctx, moveSrc, dst.string(), /*folder=*/false);
					// Same reasoning as the rename popup: the asset left one path and
					// landed on another, so both cached type sniffs are now lies.
					EditorAssetTypeCache::invalidate(moveSrc);
					EditorAssetTypeCache::invalidate(dst.string());
					// The thumbnail is keyed by the asset's path, so the old key would
					// keep a .hthumb nobody can reach again.
					AssetThumbnailCache::invalidate(moveSrc);
					AssetThumbnailCache::invalidate(dst.string());
					if (s_selectedItem == moveSrc) s_selectedItem = dst.string();
					// The selection follows the move: dragging twelve assets into a
					// folder and finding nothing selected afterwards (or, worse, the
					// twelve paths they no longer live at) is a broken gesture.
					for (std::string& sel : s_selection)
						if (sel == moveSrc) sel = dst.string();
					for (auto& t : ctx.tabs)
						if (t.assetPath == moveSrc)
							t.assetPath = dst.string();
					s_quietContentRefresh = true;
				}
			}
		}
		if (!s_pendingMoveSrcs.empty() && !s_pendingMoveDst.empty() && ctx.collab)
			ctx.collab->endAssetOpBatch();
		if (!s_pendingMoveSrcs.empty() || !s_pendingMoveDst.empty())
		{
			s_pendingMoveSrcs.clear();
			s_pendingMoveDst.clear();
		}

		// ── Open item context menu after loops ────────────────────────────
		// ── Shared "Create Asset" menu body ─────────────────────────────────
		// Used by BOTH the background right-click popup and the item context
		// menu's Create submenu, so creating an asset works wherever the user
		// right-clicks in the Content Browser.
		// Making a folder, from wherever it was asked for. It used to live inline in
		// the background right-click menu, which is why the item context menu could
		// not offer it — the code was not reachable from there.
		auto createFolderIn = [&](const std::string& targetFolder)
		{
			std::string base = targetFolder + "/NewFolder";
			std::string dir  = base;
			int counter = 1;
			while (std::filesystem::exists(dir))
				dir = base + std::to_string(counter++);
			std::error_code mkEc;
			std::filesystem::create_directory(dir, mkEc);
			if (mkEc)
			{
				HE_LOG_ERROR(Editor, "%s",
					("Editor: could not create folder '" + dir + "': " + mkEc.message()).c_str());
				return;
			}

			// Show it now and let the user name it straight away. Same reasoning as
			// the asset case: the selection follows, or F2 renames something else.
			const std::string folderName = std::filesystem::path(dir).filename().string();
			s_selectedItem     = dir;
			s_selectedIsFolder = true;
			s_selection.assign(1, dir);
			s_renameTarget    = dir;
			// Same choke point as an asset: announced when the name is
			// final, not when the directory appears.
			s_pendingCreatePublish  = dir;
			s_pendingCreateIsFolder = true;
			s_renameIsFolder  = true;
			s_renameIsCreate  = true;
			std::strncpy(s_renameBuf, folderName.c_str(), sizeof(s_renameBuf) - 1);
			s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
			s_openRenamePopup = true;
			s_quietContentRefresh = true;
		};

		// The create menu, drawn from two places (the context menu and the "+"
		// button), so the scope belongs to the list rather than to either caller.
		// Its rows are asset KINDS — the one place the editor says what each kind
		// is for, rather than only what it is called.
		auto drawCreateAssetItems = [&](const std::string& targetFolder)
		{
			HE::Ed::Help::Scope helpScope("New Asset");
			auto tryCreate = [&](const char* defaultName, const char* ext, HE::AssetType type,
			                     HE::ScriptLanguage scriptLang = HE::ScriptLanguage::Lua,
			                     const char* hcBaseClass = nullptr)
			{
				// Build a path that does not yet exist
				std::string base = targetFolder + "/" + defaultName;
				std::string path = base + ext;
				int counter = 1;
				while (std::filesystem::exists(path))
					path = base + std::to_string(counter++) + ext;

				// Create an empty asset file via ContentManager. Root-aware (Content
				// vs. reserved "Engine/" namespace) so an asset created inside the
				// Engine tree gets a path other panels' pickers can resolve back.
				std::string relative = ctx.contentManager->toContentRelativePath(path);

				// Write a minimal binary asset stub so the file exists on disk.
				// The UUID minted here is the asset's permanent identity.
				{
					const HE::UUID assetId = HE::UUID::generate();
					HAsset::Writer w;
					std::vector<uint8_t> meta;
					HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
					HAsset::Writer::appendPOD(meta, assetId.hi);
					HAsset::Writer::appendPOD(meta, assetId.lo);
					HAsset::Writer::appendString(meta, defaultName);
					HAsset::Writer::appendString(meta, relative);
					w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
					// Scripts are born with a language and a starter template. The
					// language byte (CHUNK_SLNG) is the single source of truth for
					// routing Lua vs Python, so it must be written here at birth —
					// this stub bypasses the ContentManager save path.
					if (type == HE::AssetType::Script)
					{
						const int lang = static_cast<int>(scriptLang);
						const char* starter = scriptStarterTemplate(lang);
						w.addChunk(HAsset::CHUNK_SRC, starter, std::char_traits<char>::length(starter));
						const uint8_t lb = static_cast<uint8_t>(lang);
						w.addChunk(HAsset::CHUNK_SLNG, &lb, 1);
					}
					// UI widgets are born with an empty 1920×1080 tree so the widget
					// editor has valid JSON to open straight away.
					if (type == HE::AssetType::Widget)
					{
						const std::string tree = HE::uiWidgetTreeToJson(HE::UIWidgetTree{});
						w.addChunk(HAsset::CHUNK_UIWT, tree.data(), tree.size());
					}
					if (type == HE::AssetType::HorizonCodeClass)
					{
						const std::string graph = HorizonCode::toJson(HorizonCode::Graph{});
						w.addChunk(HAsset::CHUNK_HCGR, graph.data(), graph.size());
						// The base class decides the event catalog (e.g. input events on
						// PlayerController/PlayerCharacter), so it is part of the asset's
						// identity from birth. Absent chunk = plain Object.
						if (hcBaseClass && *hcBaseClass)
							w.addChunk(HAsset::CHUNK_HCBC, hcBaseClass, std::strlen(hcBaseClass));
					}
					// Input assets are born with valid minimal JSON so their editors and
					// the runtime parser never see an empty payload.
					if (type == HE::AssetType::InputAction)
					{
						const char* json = "{\"valueType\":\"Button\"}";
						w.addChunk(HAsset::CHUNK_IACT, json, std::strlen(json));
					}
					if (type == HE::AssetType::InputMappingContext)
					{
						const char* json = "{\"entries\":[]}";
						w.addChunk(HAsset::CHUNK_IMAP, json, std::strlen(json));
					}
					// Type-definition assets are born with valid empty JSON so the
					// TypeAssetPanel and the TypeRegistry never see an empty payload.
					if (type == HE::AssetType::StructType)
					{
						const char* json = "{\"fields\":[]}";
						w.addChunk(HAsset::CHUNK_STDF, json, std::strlen(json));
					}
					if (type == HE::AssetType::EnumType)
					{
						const char* json = "{\"entries\":[]}";
						w.addChunk(HAsset::CHUNK_ENDF, json, std::strlen(json));
					}
					if (type == HE::AssetType::SaveGameTemplate)
					{
						const char* json = "{\"fields\":[]}";
						w.addChunk(HAsset::CHUNK_SGTP, json, std::strlen(json));
					}
					w.write(path, static_cast<uint16_t>(type));
				}
				// A path that was probed while it was still free (or held a deleted
				// asset) has a stale entry in the shared type cache — this asset is
				// the one that decides its type now.
				EditorAssetTypeCache::invalidate(path);

				// Show it now (don't wait for the next auto-refresh) and let the
				// user name it straight away via the rename/name dialog.
				//
				// The SET moves with the anchor, not just the anchor: leaving the
				// previous selection standing meant the highlighted tiles and the
				// thing F2 or Delete would act on were two different assets.
				s_selectedItem    = path;
				s_selectedIsFolder = false;
				s_selection.assign(1, path);
				s_renameTarget    = path;
				s_renameIsFolder  = false;
				s_renameIsCreate  = true;
				// Announced once the dialog below settles on the final name.
				s_pendingCreatePublish  = path;
				s_pendingCreateIsFolder = false;
				// A script is born in the project's fixed language (Lua or Python) —
				// no per-asset language picker, so this stays -1 (combo hidden).
				s_renameScriptLang = -1;
				std::strncpy(s_renameBuf, defaultName, sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				s_quietContentRefresh = true;
				ImGui::CloseCurrentPopup();
			};

			// ── Grouped, because a flat list of sixteen is a list you read every
			// time instead of aiming at. The two that stay loose are the ones you
			// make most and open as documents in their own right; everything else
			// sits under the thing it belongs to.
			if (EditorWidgets::menuItem("Scene"))     tryCreate("NewScene",  ".hescene", HE::AssetType::Scene);
			if (EditorWidgets::menuItem("UI Widget")) tryCreate("NewWidget", ".hasset",  HE::AssetType::Widget);
			ImGui::Separator();

			// ── Gameplay logic — restricted to the project's chosen language ──────
			// The project's scriptLanguage (picked in the New Project wizard) is
			// authoritative: only the matching logic creator appears, so a project
			// stays single-language. HorizonCode → class/player graph assets; Lua/
			// Python → a Script asset born in that language (no per-asset picker);
			// C++ → a native source class under Source/ (see the C++ Class popup).
			const ProjectScriptLanguage projLang = ctx.projectManager
				? ctx.projectManager->currentProject().scriptLanguage
				: ProjectScriptLanguage::HorizonCode;
			if (ImGui::BeginMenu("Gameplay"))
			{
				switch (projLang)
				{
				case ProjectScriptLanguage::HorizonCode:
					// One entry per row of the engine class taxonomy
					// (HorizonCode.h engineClasses()). "Object" is the plain class
					// and stores an EMPTY baseClass — the spelling every asset
					// predating the taxonomy already carries.
					if (EditorWidgets::menuItem("HorizonCode Class")) tryCreate("NewClass", ".hasset", HE::AssetType::HorizonCodeClass);
					if (EditorWidgets::menuItem("Entity"))
						tryCreate("NewEntity", ".hasset", HE::AssetType::HorizonCodeClass, HE::ScriptLanguage::Lua, "Entity");
					if (EditorWidgets::menuItem("Player Controller"))
						tryCreate("NewPlayerController", ".hasset", HE::AssetType::HorizonCodeClass, HE::ScriptLanguage::Lua, "PlayerController");
					if (EditorWidgets::menuItem("Player Character"))
						tryCreate("NewPlayerCharacter", ".hasset", HE::AssetType::HorizonCodeClass, HE::ScriptLanguage::Lua, "PlayerCharacter");
					break;
				case ProjectScriptLanguage::Lua:
					if (EditorWidgets::menuItem("Script")) tryCreate("NewScript", ".hasset", HE::AssetType::Script, HE::ScriptLanguage::Lua);
					break;
				case ProjectScriptLanguage::Python:
					if (EditorWidgets::menuItem("Script")) tryCreate("NewScript", ".hasset", HE::AssetType::Script, HE::ScriptLanguage::Python);
					break;
				case ProjectScriptLanguage::Cpp:
					if (EditorWidgets::menuItem("C++ Class"))
					{
						std::strncpy(s_cppClassName, "GameplayClass", sizeof(s_cppClassName) - 1);
						s_cppClassName[sizeof(s_cppClassName) - 1] = '\0';
						s_openCppClassPopup = true;
						ImGui::CloseCurrentPopup();
					}
					break;
				}
				// Authored next to character logic and only ever used by it, so it
				// sits here rather than in a submenu of its own.
				ImGui::Separator();
				if (EditorWidgets::menuItem("Animator State Machine")) tryCreate("NewStateMachine", ".hasset", HE::AssetType::AnimatorStateMachine);
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Input"))
			{
				if (EditorWidgets::menuItem("Input Action"))          tryCreate("NewInputAction",  ".hasset", HE::AssetType::InputAction);
				if (EditorWidgets::menuItem("Input Mapping Context")) tryCreate("NewInputMapping", ".hasset", HE::AssetType::InputMappingContext);
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Rendering"))
			{
				if (EditorWidgets::menuItem("Material"))          tryCreate("NewMaterial", ".hasset", HE::AssetType::Material);
				if (EditorWidgets::menuItem("Material Function")) tryCreate("NewMaterialFunction", ".hasset", HE::AssetType::MaterialFunction);
				if (EditorWidgets::menuItem("Particle System"))   tryCreate("NewParticleSystem", ".hasset", HE::AssetType::ParticleSystem);
				ImGui::EndMenu();
			}

			// Type definitions are language-neutral (savegame templates, script
			// constants, C++ codegen all consume them) — offered in every project.
			if (ImGui::BeginMenu("Data"))
			{
				if (EditorWidgets::menuItem("Struct")) tryCreate("NewStruct", ".hasset", HE::AssetType::StructType);
				if (EditorWidgets::menuItem("Enum"))   tryCreate("NewEnum",   ".hasset", HE::AssetType::EnumType);
				if (EditorWidgets::menuItem("SaveGame Template")) tryCreate("NewSaveTemplate", ".hasset", HE::AssetType::SaveGameTemplate);
				ImGui::EndMenu();
			}

			// A folder belongs in the list of things you can make here. It used to
			// exist ONLY in the background right-click menu, so right-clicking a
			// folder could create a Material inside it but not another folder —
			// you had to navigate into it first and then click the empty space.
			ImGui::Separator();
			if (EditorWidgets::menuItem("Folder")) createFolderIn(targetFolder);

			// Texture, Static Mesh, Skeletal Mesh, Shader, Audio and Font used to
			// stand here too, and every one of them wrote a file that could never
			// hold anything: nothing in the editor authors those types, so the stub
			// stayed empty forever, opened nothing on double-click (or reported a
			// load failure), and had to be deleted again. They arrive by import, so
			// that is what the menu says now.
			ImGui::Separator();
			ImGui::TextDisabled("Meshes, textures, audio and fonts");
			ImGui::TextDisabled("arrive via Assets \xE2\x96\xB8 Import Asset.");
		};

		// ── Delete one asset ──────────────────────────────────────────────
		// Split out from the menu item so the confirmation dialog and the actual
		// deletion are separate things — the menu item now only asks.
		auto deleteAssetNow = [&](const std::string& assetAbs, bool isSourceClass)
		{
			std::error_code ec;
			std::filesystem::remove(assetAbs, ec);
			if (ec)
			{
				HE_LOG_ERROR(Editor, "%s",
					("Editor: could not delete '" + assetAbs + "': " + ec.message()).c_str());
				return;
			}
			// The path is free again: a NEW asset of a different type may be created
			// there next, and the panels' cached header sniff would still name the
			// deleted asset's type (→ double-click opens the wrong editor).
			EditorAssetTypeCache::invalidate(assetAbs);
			AssetThumbnailCache::invalidate(assetAbs); // + its cached tile
			// In the Source root, delete BOTH halves of the class's .h/.cpp pair.
			if (isSourceClass)
			{
				const std::filesystem::path p(assetAbs);
				const std::string stem = p.stem().string();
				const std::filesystem::path dir = p.parent_path();
				for (const char* e : { ".h", ".hpp", ".hh", ".hxx", ".cpp", ".cc", ".cxx", ".c" })
				{
					std::filesystem::path sib = dir / (stem + e);
					if (sib != p)
					{
						std::error_code e2;
						std::filesystem::remove(sib, e2);
						EditorAssetTypeCache::invalidate(sib.string());
					}
				}
			}
			if (s_selectedItem == assetAbs) s_selectedItem.clear();
			// A tab still open on it is showing a file that is gone, and the
			// panel behind that tab outlives it — dirty flag and all. Save All
			// works off the panel, so leaving it would write the asset straight
			// back. Both go.
			for (auto& t : ctx.tabs)
				if (t.closable && t.assetPath == assetAbs) t.open = false;
			EditorUI::discardPanelState(ctx, assetAbs);
			ctx.contentRefreshPending = true;
		};

#ifdef HE_HAVE_LIBSSH2
		// ── Give a downloaded EngineContent copy back to the server ───────
		// Not a deletion: the file exists on the SFTP endpoint, this only drops the
		// machine-wide cached copy of it. Getting that to actually mean "downloads
		// again next time" takes more than unlink, because a materialized asset has
		// MOVED from the remote map into the disk registry — and every resolution
		// route consults the registry first. Leaving the stale entry there gives the
		// worst of both worlds: the file is gone AND the UUID never re-downloads,
		// it just stops resolving.
		auto removeCachedCopyNow = [&](const std::string& absPath,
		                               const std::string& cacheRel, HE::UUID uuid) -> bool
		{
			if (!ctx.contentManager || cacheRel.empty()) return false;

			// A download landing right after the unlink would recreate the file and
			// leave the tree claiming it is gone. Only the in-flight job is
			// observable, which covers the case a user can actually see happening.
			const HE::Cs::DownloadQueueStatus dl = HE::Cs::EngineContentSync::instance().status();
			if (dl.active && dl.currentRelativePath == cacheRel)
			{
				HE_LOG_WARN(Editor, "%s",
					("Editor: '" + cacheRel + "' is downloading right now — not removing it").c_str());
				return false;
			}

			const std::string rel = std::string("Engine/") + cacheRel;
			// The manifest's uuid for raw (non-.hasset) entries is null by design;
			// the file's own META is the fallback, and a null id after both simply
			// means "nothing was ever registered under an id", which is fine.
			if (uuid == HE::UUID{}) uuid = HE::AssetRefs::assetUuidOfFile(absPath);

			// The file goes first: on Windows a removal can fail (a sharing
			// violation while something still reads it), and unwinding the
			// bookkeeping afterwards is harder than not having done it yet.
			std::error_code ec;
			std::filesystem::remove(absPath, ec);
			if (ec)
			{
				HE_LOG_ERROR(Editor, "%s",
					("Editor: could not remove the local copy of '" + cacheRel + "': " + ec.message()).c_str());
				return false;
			}

			EditorAssetTypeCache::invalidate(absPath);
			AssetThumbnailCache::invalidate(absPath);

			// An open tab would write the asset back on its next Save — and for an
			// "Engine/…" path a save does NOT go to the cache, it creates a project
			// override, which then shadows the server copy permanently. That would
			// turn "remove my local copy" into "fork it into this project", so the
			// tab and its panel state go with the file.
			for (auto& t : ctx.tabs)
				if (t.closable && t.assetPath == absPath) t.open = false;
			EditorUI::discardPanelState(ctx, absPath);
			if (s_selectedItem == absPath) s_selectedItem.clear();

			if (!(uuid == HE::UUID{}))
			{
				// unloadAsset answers false for two unrelated reasons — the asset is
				// pinned, and the asset was never loaded at all — so the "still in
				// use" note is only true when it WAS loaded. The ordinary case for
				// this command is an asset nobody has touched, and reporting that as
				// in-use contradicts the dialog the user just confirmed.
				const bool wasLoaded = ctx.contentManager->isLoaded(uuid);
				if (!ctx.contentManager->unloadAsset(uuid) && wasLoaded)
					HE_LOG_INFO(Editor, "%s",
						("Editor: '" + rel + "' is still in use — its loaded copy stays until the next reload").c_str());
				// Both halves matter: forgetting the disk entry is what lets
				// registerRemoteAsset take the route back (it refuses while the UUID
				// still looks resolvable from disk).
				ctx.contentManager->forgetDiskAsset(uuid);

				const std::string remotePath = cacheRel;
				const HE::UUID    id         = uuid;
				const std::string relPath = rel;
				ctx.contentManager->registerRemoteAsset(id, rel,
					[remotePath, relPath, id](std::function<void(bool)> done)
					{
						HE::Cs::EngineContentSync::instance().enqueueDownload(
							remotePath, id, HE::Cs::DownloadTrigger::Passive,
							[done, remotePath, relPath](bool success)
							{
								// Passive route, so nothing on screen is waiting on
								// this: whatever asked for the asset simply goes
								// without it. Told to the user by name, or the local
								// copy they just removed looks like it took the
								// asset with it for good.
								if (!success)
									HE::Ed::notify(HE::Ed::NoteLevel::Warning,
										"\"" + std::filesystem::path(remotePath).filename().string()
											+ "\" could not be downloaded again.",
										"Its local copy was removed and the server did not hand "
										"it back. It stays missing until a later attempt "
										"succeeds.",
										relPath);
								done(success);
							});
					});
			}

			// The catalogue has to keep listing the asset, or the tile does not turn
			// back into a remote-only placeholder — it VANISHES, and an asset you
			// cannot see is one you cannot get back. Writing the cached manifest is
			// what makes that survive a restart and an offline session.
			//
			// ALL OF IT ON A WORKER, and not because it is slow. Both
			// setEngineRemoteAssets and refreshEngineFolder take the engine-folder
			// mutex EXCLUSIVELY, and this code runs inside ContentBrowserPanel::render,
			// which holds that same (non-recursive) shared_mutex for its entire body:
			// calling either one from here deadlocks the frame thread against itself.
			// The download-completion callback right below already refreshes from a
			// worker for the same reason — the worker simply waits out the frame.
			GlobalState* gs = ctx.globalState;
			const std::string engineRoot  = ctx.contentManager->engineContentRoot();
			const std::string contentRoot = ctx.contentManager->contentRoot();
			const std::string relPathCopy = cacheRel;
			const HE::UUID    uuidCopy    = uuid;
			globalPool().submit([gs, engineRoot, contentRoot, relPathCopy, uuidCopy]
			{
				HE::Cs::EngineContentSync::instance().noteLocalCopyRemoved(relPathCopy, uuidCopy);
				if (!gs || engineRoot.empty()) return;
				std::vector<HE::RemoteEngineAsset> remote;
				for (const auto& e : HE::Cs::EngineContentSync::instance().manifest().entries)
					remote.push_back(HE::RemoteEngineAsset{ e.relativePath, e.uuid });
				gs->setEngineRemoteAssets(std::move(remote));
				// Bumps the engine-folder version, which is what makes the panel
				// re-resolve its nodes and show the placeholder.
				gs->refreshEngineFolder(engineRoot, contentRoot);
			}, "EngineFolderRefresh");

			HE_LOG_INFO(Editor, "%s",
				("Editor: removed the local copy of '" + cacheRel + "' — it will download again when needed").c_str());
			return true;
		};
#endif

		// ── Delete a folder and everything under it ───────────────────────
		// Shared by the two ways in: a folder with nothing in it goes straight
		// away, one holding assets only gets here after the confirmation dialog.
		auto deleteFolderNow = [&](const std::string& folderAbs)
		{
			std::error_code ec;
			std::filesystem::remove_all(folderAbs, ec);
			if (ec)
			{
				HE_LOG_ERROR(Editor, "%s",
					("Editor: could not delete folder '" + folderAbs + "': " + ec.message()).c_str());
				return;
			}
			// Both caches are keyed by path and every asset below the folder just
			// lost its path — same call the folder RENAME makes, which drops the
			// whole map rather than walking the subtree it no longer owns.
			EditorAssetTypeCache::invalidateAll();
			AssetThumbnailCache::clear();

			// An editor tab still open on one of those assets would write the file
			// back into the folder that was just deleted on its next Save, so close
			// them here; EditorUI erases them next frame.
			//
			// Closing is only half of it: EditorUI deliberately KEEPS a dirty
			// panel's state when a tab closes, so reopening restores what was
			// typed — and Save All saves from the panel, not the tab. For an
			// asset that no longer exists that is exactly wrong, so the state
			// goes with the file.
			for (auto& t : ctx.tabs)
			{
				if (!t.closable || t.assetPath.empty()) continue;
				if (!isUnderFolder(t.assetPath, folderAbs)) continue;
				t.open = false;
			}
			// Covers the tabs above AND the assets that have no tab left to
			// close — a tab closed while dirty keeps its panel state behind.
			EditorUI::discardPanelStateUnder(ctx, folderAbs);

			if (s_selectedItem == folderAbs || isUnderFolder(s_selectedItem, folderAbs))
				s_selectedItem.clear();
			s_ctxMenuItem.clear();
			// The grid may be standing inside the folder that just went away: the
			// refresh bumps the folder version, the re-resolution at the top of the
			// panel fails to find the remembered path and falls back to the root.
			ctx.contentRefreshPending = true;
			HE_LOG_INFO(Editor, "%s", ("Editor: deleted folder " + folderAbs).c_str());
		};

		if (s_rightClickOnItem)
		{
			ImGui::OpenPopup("##cb_item_ctx");
			s_rightClickOnItem = false;
		}

		// ── Item context menu (folder + file) ─────────────────────────────
		if (ImGui::BeginPopup("##cb_item_ctx"))
		{
			// "Content Browser/<label>" for everything this menu offers.
			HE::Ed::Help::Scope helpScope("Content Browser");
			std::string displayName = s_ctxMenuIsFolder
				? std::filesystem::path(s_ctxMenuItem).filename().string()
				: std::filesystem::path(s_ctxMenuItem).stem().string();
			ImGui::TextDisabled("%s", displayName.c_str());

			// Engine default assets are read-only from a project's point of view
			// (HE_ENGINE_CONTENT_EDITABLE=1 lifts this for engine-authoring
			// builds): no create/rename/delete on the shared default or its
			// project override — "Revert to Default" is the only mutation an
			// override gets, and editing+Saving one (in its own graph/asset
			// editor tab) is what CREATES an override in the first place, via
			// ContentManager::saveAsset's redirect, not through this menu.
			const bool isEngineOverride = ctx.contentManager && ctx.contentManager->isEngineOverridePath(s_ctxMenuItem);
			const bool isEngineDefault  = ctx.contentManager && ctx.contentManager->isEngineDefaultPath(s_ctxMenuItem);
			// A downloaded copy is neither of those two, so it used to fall through
			// as ordinary project content: the menu offered Rename (which moves the
			// file inside the cache, so the manifest path stops resolving to it and
			// the orphan stays forever) and Delete (which destroys the machine-wide
			// copy with none of the bookkeeping a re-download needs). It is read-only
			// ground for the same reason a shipped default is — "Remove Local Copy"
			// below is the one thing that may happen to it.
			//
			// The test is "does this path live in the download cache", not "is it a
			// file": the Engine tree also SYNTHESIZES folders for manifest entries
			// that exist only on the server (walkOrCreateFolderPath roots those at
			// the cache too). Such a folder is neither an override nor a default
			// either, so it fell through as ordinary content — offering Delete
			// (remove_all over the machine-wide cache, with none of the bookkeeping
			// below), Rename, and a Create Asset submenu that would write project
			// assets into the cache directory.
			const bool isCacheRooted    = !engineRelativePath(s_ctxMenuItem).empty();
			const bool isCacheCopy      = !s_ctxMenuIsFolder && s_ctxMenuIsCacheCopy;
			const bool engineLocked     = ((isEngineOverride || isEngineDefault) &&
			                               !ContentManager::isEngineContentDevMode()) || isCacheRooted;
			if (engineLocked)
				ImGui::TextDisabled(isCacheCopy      ? "downloaded engine asset (shared local copy)"
				                  : isCacheRooted    ? "EngineContent from the server"
				                  : isEngineOverride ? "project override of an engine default"
				                                     : "engine default asset (read-only)");
			ImGui::Separator();

			if (isEngineOverride && !ContentManager::isEngineContentDevMode() && !s_ctxMenuIsFolder &&
			    EditorWidgets::menuItem("Revert to Default"))
			{
				const std::string relPath = ctx.contentManager->toContentRelativePath(s_ctxMenuItem);
				if (!relPath.empty())
				{
					const HE::UUID id = ctx.contentManager->loadAsset(relPath); // current (override) id
					std::error_code ec;
					std::filesystem::remove(s_ctxMenuItem, ec);
					if (!ec)
					{
						// Nothing lives at that path any more — drop the cached type sniff
						// and the rendered tile of the override that just went away.
						EditorAssetTypeCache::invalidate(s_ctxMenuItem);
						AssetThumbnailCache::invalidate(s_ctxMenuItem);
						if (id != HE::UUID{}) ctx.contentManager->unloadAsset(id);
						if (s_selectedItem == s_ctxMenuItem) s_selectedItem.clear();
						s_ctxMenuItem.clear();
						ctx.contentRefreshPending = true;
						HE_LOG_INFO(Editor, "%s", ("Editor: reverted '" + relPath + "' to its engine default").c_str());
					}
					else
						HE_LOG_ERROR(Editor, "%s", ("Editor: could not remove override for '" + relPath + "'").c_str());
				}
				ImGui::CloseCurrentPopup();
			}

#ifdef HE_HAVE_LIBSSH2
			// ── Remove a downloaded EngineContent copy ───────────────────
			// Deliberately not spelled "Delete": nothing is lost. The asset goes
			// on existing on the server, the tile turns back into the remote-only
			// placeholder it was before, and the next use fetches it again. What
			// this reclaims is disk — the cache is shared by every project on the
			// machine, so it only ever grows.
			if (isCacheCopy && EditorWidgets::dangerMenuItem("Remove Local Copy"))
			{
				s_removeCacheFullPath = s_ctxMenuItem;
				s_removeCacheRelPath  = engineRelativePath(s_ctxMenuItem);
				s_removeCacheUuid     = s_ctxMenuRemoteUuid;
				s_removeCacheScanGen  = beginScanFor(s_removeCacheFullPath, /*isFolder=*/false);
				s_openRemoveCachePopup = true;
				ImGui::CloseCurrentPopup();
			}
			if (isCacheCopy && ImGui::IsItemHovered())
				ImGui::SetTooltip("Frees the disk space. The asset stays on the server "
				                  "and downloads again when something needs it.");
#endif

			// ── Import source file → .hasset ─────────────────────────────
			if (!s_ctxMenuIsFolder)
			{
				const std::filesystem::path srcPath(s_ctxMenuItem);
				const std::string ext = std::filesystem::path(s_ctxMenuItem).extension().string();

				// Which extensions can be imported, and which importer each one
				// belongs to, is asked ONCE for the whole editor now
				// (Importer::isImportableSource / importSource). It used to be
				// spelled out here AND in the File ▸ Import Asset handler, and the
				// two had already drifted: fonts imported from this menu and not
				// from that one.
				if (!engineLocked && Importer::isImportableSource(srcPath) &&
				    EditorWidgets::menuItem("Import"))
				{
					// The item being imported lives in whichever root is currently
					// browsed (s_selectedRootKind) — NOT always Content.
					const std::filesystem::path root = cbRootFolder(s_selectedRootKind).fullPath;
					std::error_code ec;
					std::filesystem::path relDir =
						std::filesystem::relative(srcPath.parent_path(), root, ec);
					if (ec || relDir == ".") relDir.clear();

					if (!Importer::importSource(srcPath, root, relDir))
						HE_LOG_ERROR(Editor, "%s",
							("Editor: import failed for " + srcPath.string()).c_str());
					ctx.contentRefreshPending = true;
					ImGui::CloseCurrentPopup();
				}

				// ── Reimport ─────────────────────────────────────────────
				// An asset now records the file it was imported FROM, which is what
				// makes this possible at all. Before it, updating a mesh meant
				// finding the source in the OS dialog again — and, since that path
				// wrote to the content root, ending up with a SECOND asset carrying
				// a new uuid while every scene still referenced the old one.
				// Reimport writes back into the asset's own folder and keeps its
				// uuid, so references survive.
				if (!engineLocked && ext == ".hasset" && ctx.contentManager)
				{
					const std::string recordedSource = Importer::sourceFileOf(s_ctxMenuItem);
					if (!recordedSource.empty() && EditorWidgets::menuItem("Reimport"))
					{
						// A reimport REPLACES a file everyone in the session shares,
						// without anybody asking for it — so the lock is taken BEFORE
						// the local write, not before the send. Checking it on the way
						// out would mean clobbering a colleague's asset here and only
						// then discovering we were not allowed to.
						//
						// A named lease, not `if (!collab->beginBackgroundWrite(k))`.
						// A temporary dies at the end of its own expression, which
						// would give the lock back before the import has read a single
						// byte and restore precisely the check-then-write gap this
						// exists to close. It lives until the end of this block, so it
						// still covers publishReimport — the send is part of the write,
						// and handing the asset over between writing it and announcing
						// it would let the next holder's version arrive first.
						//
						// The static two-argument form is the null-safe door: no
						// controller and no key both mean "nothing to arbitrate", which
						// has to PERMIT the write, and a lease built by hand for that
						// case would default to refusing and silently kill reimport
						// everywhere outside a session.
						const std::string collabKey = collabKeyFor(ctx, s_ctxMenuItem);
						CollabController::AssetWriteLease lease =
							CollabController::beginBackgroundWrite(ctx.collab, collabKey);
						if (lease)
						{
							if (!Importer::reimport(s_ctxMenuItem, ctx.contentManager->contentRoot()))
								HE_LOG_ERROR(Editor, "%s",
									("Editor: reimport failed for " + s_ctxMenuItem).c_str());
							// The new bytes have to reach the others, or every peer
							// keeps the old asset with no sign that it changed. What
							// actually travels depends on the type: authored assets go
							// over the wire, imported binary media is announced instead
							// (source control carries those — see publishReimport).
							else if (ctx.collab && !collabKey.empty())
								ctx.collab->publishReimport(collabKey, s_ctxMenuItem);
							ctx.contentRefreshPending = true;
						}
						ImGui::CloseCurrentPopup();
					}
					else if (!recordedSource.empty() && ImGui::IsItemHovered())
						ImGui::SetTooltip("Re-read %s", recordedSource.c_str());
				}

				// ── Material → create a child INSTANCE (params/switches only) ──
				// Allowed even for an engine-default material: it creates a NEW,
				// separate asset (a plain project one, not "Engine/..."-namespaced)
				// rather than mutating the default.
				if (ext == ".hasset" && ctx.contentManager &&
				    MaterialEditorPanel::isMaterialAsset(s_ctxMenuItem) &&
				    EditorWidgets::menuItem("Create Material Instance"))
				{
					// registerMaterial() below stores inst.path directly (no later
					// loadAsset() to correct it), so it MUST carry the "Engine/"
					// prefix already when the parent lives under the engine root —
					// toContentRelativePath(), not a manual root-relative fs::relative.
					const std::string parentRel = ctx.contentManager->toContentRelativePath(srcPath.string());
					if (!parentRel.empty())
					{
						// Unique sibling: <stem>_Inst[.N].hasset
						std::filesystem::path dst =
							srcPath.parent_path() / (srcPath.stem().string() + "_Inst.hasset");
						for (int k = 2; std::filesystem::exists(dst) && k < 100; ++k)
							dst = srcPath.parent_path() /
								(srcPath.stem().string() + "_Inst" + std::to_string(k) + ".hasset");
						MaterialAsset inst;
						inst.type = HE::AssetType::Material;
						inst.name = dst.stem().string();
						inst.path = ctx.contentManager->toContentRelativePath(dst.string());
						inst.parentMaterialPath = parentRel;
						const HE::UUID iid = ctx.contentManager->registerMaterial(std::move(inst));
						ctx.contentManager->syncMaterialInstance(iid); // derive shader/params
						if (MaterialAsset* mi = ctx.contentManager->getMaterialMutable(iid))
							ctx.contentManager->saveAsset(*mi);
						ctx.contentRefreshPending = true;
						HE_LOG_INFO(Editor, "%s",
							("Editor: created material instance of '" + parentRel + "'").c_str());
					}
					ImGui::CloseCurrentPopup();
				}

				// ── Add a StaticMesh .hasset to the scene ─────────────────
				// Offered only for the ONE type it can actually place. It used to
				// appear on every .hasset and, for all the others, do nothing at all
				// except write a line into a log nobody has open — a menu item that
				// silently no-ops reads as a broken editor. The type is already known
				// per tile from the cached header sniff, so asking costs nothing.
				if (ext == ".hasset" && ctx.world && ctx.contentManager &&
				    EditorAssetTypeCache::is(s_ctxMenuItem, HE::AssetType::StaticMesh) &&
				    EditorWidgets::menuItem("Add to Scene"))
				{
					std::string rel = ctx.contentManager->toContentRelativePath(srcPath.string());
					if (!rel.empty())
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (const StaticMeshAsset* mesh = ctx.contentManager->getStaticMesh(id))
						{
							// Copied out BEFORE any further loadAsset: the asset stores live
							// in vector-backed SlotMaps, so registering another asset can
							// reallocate them and dangle `mesh`.
							const std::string meshName = mesh->name;
							const std::string matRel   = mesh->materialPath;

							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							Entity e = ctx.world->createEntity(meshName);
							ctx.world->addComponent(e, TransformComponent{});
							ctx.world->addComponent(e, MeshComponent{ .meshAssetId = id });
							// …and the material the mesh itself names (chunk MREF — what an
							// import writes). Without a MaterialComponent the draw carries NO
							// material at all (RenderExtractor sets matId only from this
							// component); the mesh's own reference is then read for one thing
							// only, uploading its base-colour texture. Everything an imported
							// PBR material actually is — the generated shader, its normal and
							// ORM maps, the alpha-mask cutout — stays inert, so an imported
							// tree arrives untextured-looking and its leaf cards render as
							// solid quads. Placing the link here is what makes the import's
							// material reference mean something.
							if (!matRel.empty())
							{
								const HE::UUID matId = ctx.contentManager->loadAsset(matRel);
								if (matId != HE::UUID{})
									ctx.world->addComponent(e, MaterialComponent{ matId });
								else
									HE_LOG_WARN(Editor, "%s",
										("Editor: '" + meshName + "' names material '" + matRel
										 + "', which did not load — placed without it").c_str());
							}
							ctx.world->markHierarchyDirty();
							HE_LOG_INFO(Editor, "%s",
								("Editor: added '" + meshName + "' to scene").c_str());
						}
						else
							HE_LOG_WARN(Editor, "%s",
								("Editor: " + rel + " is not a loadable static mesh").c_str());
					}
					ImGui::CloseCurrentPopup();
				}
			}

			// ── Someone else is editing this: ask them for it ────────────────
			// The tile already carries a padlock badge saying so; this is what
			// you can DO about it, in the place you right-clicked to act. Same
			// path derivation as the badge, so the entry appears exactly where
			// the padlock does and never on a tile without one.
			if (!s_ctxMenuIsFolder && ctx.collab && ctx.collab->inSession() &&
			    ctx.contentManager)
			{
				const std::string rel = collabKeyFor(ctx, s_ctxMenuItem);
				if (!rel.empty() && ctx.collab->assetLockedByOther(rel))
				{
					const HE::Net::LockInfo* lock = ctx.collab->assetLockInfo(rel);
					const std::string who = lock && !lock->ownerName.empty()
						? lock->ownerName : std::string("whoever is editing it");
					if (ctx.collab->hasAskedToEdit(rel))
					{
						ImGui::TextDisabled("waiting for %s to answer\xE2\x80\xA6",
						                    who.c_str());
					}
					else if (EditorWidgets::menuItem("Ask to Edit"))
					{
						ctx.collab->requestAssetEdit(rel);
						ImGui::CloseCurrentPopup();
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s is editing this. Asking hands the "
						                  "decision to them.", who.c_str());
					ImGui::Separator();
				}
			}

			// ── What points at this? ─────────────────────────────────────
			// The scan behind the delete dialogs answers a question people ask far
			// more often than "may I delete this": before renaming, before moving,
			// before changing a material everything shares. It was reachable only by
			// starting a deletion and then backing out — so the safe way to ask was
			// to pretend to do the dangerous thing.
			if (EditorWidgets::menuItem("Find References"))
			{
				s_referencesTarget    = s_ctxMenuItem;
				s_referencesIsFolder  = s_ctxMenuIsFolder;
				s_referencesScanGen   = beginScanFor(s_ctxMenuItem, s_ctxMenuIsFolder);
				s_openReferencesPopup = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::Separator();

			// In the Source root a C++ class is a .h/.cpp pair; renaming it means
			// renaming both files AND rewriting the class name/registration inside —
			// a refactor best left to the user's C++ toolchain, so Rename is hidden.
			const bool renameShown = !engineLocked && s_selectedRootKind != 2;
			const bool doRename    = renameShown && EditorWidgets::menuItem("Rename");
			if (renameShown) EditorWidgets::helpForKey("content.rename");
			if (doRename)
			{
				s_renameTarget   = s_ctxMenuItem;
				s_renameIsFolder = s_ctxMenuIsFolder;
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				std::strncpy(s_renameBuf, displayName.c_str(), sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				ImGui::CloseCurrentPopup();
			}
			// ── Renaming MANY ────────────────────────────────────────────
			// One name for twelve files is meaningless, which is why plain Rename
			// stays single. What a set actually needs is a RULE — add a prefix,
			// strip a suffix, replace a word, number them — which is what every
			// DCC tool offers and what the batched retarget makes cheap: N pairs,
			// one walk of the project.
			if (!engineLocked && s_selectedRootKind != 2 && !s_ctxMenuIsFolder &&
			    isSelected(s_ctxMenuItem) && s_selection.size() > 1)
			{
				const std::string label =
					"Rename " + std::to_string(s_selection.size()) + " Assets\xE2\x80\xA6";
				if (EditorWidgets::menuItem(label.c_str()))
				{
					s_patternTargets.clear();
					for (const std::string& p : s_selection)
						if (!isReadOnlyGround(p))
						{
							std::error_code dirEc;
							if (!std::filesystem::is_directory(p, dirEc)) s_patternTargets.push_back(p);
						}
					s_patternFind[0] = '\0';
					s_patternReplace[0] = '\0';
					s_patternPrefix[0] = '\0';
					s_patternSuffix[0] = '\0';
					s_patternNumber = false;
					s_patternStart  = 1;
					s_openPatternRenamePopup = !s_patternTargets.empty();
					ImGui::CloseCurrentPopup();
				}
			}
			// Deleting an asset asks first. It used to happen on this click, and
			// a red menu item is not a confirmation: the pointer is already over
			// Delete when the menu opens under it, the file has no undo, and
			// afterwards nothing on screen says which one is gone. The dialog
			// names it, which is the whole point.
			//
			// The whole selection goes, not just the item under the pointer: the
			// menu was raised on one OF the selected tiles, and deleting only that
			// one would silently ignore the eleven the user had just picked out.
			// Built HERE rather than in the dialog, and filtered per path: a
			// selection can hold assets from a root the grid is no longer showing,
			// and the anchor's read-only answer says nothing about theirs.
			const std::vector<std::string> deletableSelection = [&]
			{
				std::vector<std::string> out;
				if (s_ctxMenuIsFolder) return out;
				const std::vector<std::string>& src = isSelected(s_ctxMenuItem)
					? s_selection : std::vector<std::string>{ s_ctxMenuItem };
				for (const std::string& p : src)
				{
					if (isReadOnlyGround(p)) continue;
					// A directory that slipped into the set is not an asset: this
					// dialog's confirmation says "the file", deleteAssetNow calls
					// fs::remove (which refuses a non-empty directory anyway), and the
					// folder path has its own dialog that lists what goes with it.
					std::error_code dirEc;
					if (std::filesystem::is_directory(p, dirEc)) continue;
					out.push_back(p);
				}
				return out;
			}();
			const int selectedAssetCount = static_cast<int>(deletableSelection.size());
			const std::string deleteLabel = selectedAssetCount > 1
				? "Delete " + std::to_string(selectedAssetCount) + " Assets" : std::string("Delete");
			if (!engineLocked && !s_ctxMenuIsFolder && selectedAssetCount > 0 &&
			    EditorWidgets::dangerMenuItem(deleteLabel.c_str()))
			{
				s_deleteAssetTargets  = deletableSelection;
				s_deleteAssetIsSource = s_selectedRootKind == 2;
				// Started here rather than in the dialog body: the dialog runs every
				// frame it is open, and starting from there would launch a scan per
				// frame. The answer arrives while the user is still reading.
				s_deleteAssetScanGen  = beginScanForMany(s_deleteAssetTargets, /*isFolder=*/false);
				s_openDeleteAssetPopup = true;
				s_ctxMenuItem.clear();
				ImGui::CloseCurrentPopup();
			}

			// A folder deletes as a unit — the whole subtree goes with it. That is
			// the one browser operation that destroys work the user cannot see from
			// where they clicked, so anything but an empty folder has to pass a
			// confirmation that names every asset going with it.
			const bool deleteShown = !engineLocked && s_ctxMenuIsFolder;
			const bool doDelete    = deleteShown && EditorWidgets::dangerMenuItem("Delete");
			if (deleteShown) EditorWidgets::helpForKey("content.delete");
			if (doDelete)
			{
				s_deleteFolderTarget = s_ctxMenuItem;
				s_deleteFolderFiles  = collectFolderContents(s_ctxMenuItem);
				if (s_deleteFolderFiles.empty())
				{
					// Nothing to lose (an empty folder, or one holding only empty
					// sub-folders) — no dialog worth the interruption. It still
					// has to REPLICATE though: this shortcut skipped the dialog
					// and with it the whole session path, so an empty folder was
					// the one deletion that only ever happened locally.
					const std::string rel =
						collabKeyFor(ctx, s_deleteFolderTarget, /*isFolder=*/true);
					if (!(ctx.collab && !rel.empty() &&
					      ctx.collab->requestAssetDelete(rel, /*folder=*/true)))
					{
						deleteFolderNow(s_deleteFolderTarget);
					}
					s_deleteFolderTarget.clear();
				}
				else
				{
					s_deleteFolderScanGen = beginScanFor(s_deleteFolderTarget, /*isFolder=*/true);
					s_openDeleteFolderPopup = true;
				}
				ImGui::CloseCurrentPopup();
			}

			// The asset-create submenu makes no sense in the Source root (it would
			// write .hasset files there) — the background right-click there offers
			// "C++ Class" instead.
			if (!engineLocked && s_selectedRootKind != 2)
			{
				ImGui::Separator();
				const bool createOpen = ImGui::BeginMenu("Create Asset");
				EditorWidgets::helpForKey("content.create");
				if (createOpen)
				{
					const std::string createDir = s_ctxMenuIsFolder
						? s_ctxMenuItem
						: std::filesystem::path(s_ctxMenuItem).parent_path().string();
					drawCreateAssetItems(createDir);
					ImGui::EndMenu();
				}
			}

			ImGui::EndPopup();
		}

		// Trigger rename popup outside item context popup. Gated on the content
		// refresh having settled so it never competes with the ##ContentRefresh
		// modal (e.g. right after a create, which requests both).
		if (s_openRenamePopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_rename_popup");
			s_openRenamePopup = false;
		}

		// ── Rename / name-on-create popup ─────────────────────────────────
		ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_rename_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			// 320 px wide, fixed by the SetNextWindowSize above and NoResize — about
			// forty characters. s_renameError is what runs past that: the "already
			// exists here." refusal starts with the name it is refusing, so on any
			// ordinary asset name the reason is the half that goes over the edge, and
			// "Could not rename: " + the filesystem's own message is longer still.
			// The one line that explains why the dialog stayed open was the one line
			// the user could not read. It does not auto-resize, so the window edge is
			// the right column to wrap at.
			//
			// A block of its own, closing before the EndPopup() at the foot of this
			// body: EndPopup() makes the browser's grid child the current window
			// again, and a pop running after it would take the wrap position off THAT
			// window instead. Everything below the block is the OK/Cancel handling,
			// which draws no text.
			bool confirm = false;
			{
				EditorWidgets::WrapText wrap;
				const char* verb = s_renameIsCreate ? "Name" : "Rename";
				ImGui::Text("%s %s", verb, s_renameIsFolder ? "Folder" : "Asset");
				ImGui::Separator();
				ImGui::Spacing();

				// Focus the field as the dialog opens so the user can type at once.
				// The error goes with it: Escape closes a modal without running
				// either button's branch, so clearing it on the way IN is the one
				// place that catches every way out.
				if (ImGui::IsWindowAppearing())
				{
					s_renameError.clear();
					ImGui::SetKeyboardFocusHere();
				}
				ImGui::SetNextItemWidth(-1.0f);
				confirm = ImGui::InputText("##rename_input", s_renameBuf, sizeof(s_renameBuf),
					ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
				// Why the last OK did nothing. Cleared on every keystroke, so the
				// message belongs to the name in the box and not to one two edits ago.
				if (ImGui::IsItemEdited()) s_renameError.clear();
				if (!s_renameError.empty())
				{
					ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.45f, 1.0f), "%s",
					                   s_renameError.c_str());
				}
				ImGui::Spacing();
			}

			// (A script's language is fixed by the project, chosen in the New
			// Project wizard — there is no per-asset language picker here.)

			if (EditorWidgets::primaryButton("OK", ImVec2(140, 0)) || confirm)
			{
				std::string newName(s_renameBuf);

				// Would this land on something that is already there?
				// std::filesystem::rename REPLACES an existing destination
				// without a word — so "rename Foo to Bar" silently destroyed
				// Bar, and in a session it destroyed Bar on every machine at
				// once. Checked here, before anything is asked for or moved.
				bool blocked = newName.empty() || s_renameTarget.empty();
				// A NAME, not a path. The field took anything, and the result was
				// pasted straight onto the parent directory — so typing "../Foo" or
				// "sub/Foo" moved the asset somewhere the browser never showed it,
				// and "../../../Foo" moved it out of the project entirely, taking its
				// retargeted references with it. Refused here rather than sanitised,
				// because silently changing what someone typed is its own surprise.
				if (!blocked && (newName.find('/') != std::string::npos ||
				                 newName.find('\\') != std::string::npos ||
				                 newName == "." || newName == ".."))
				{
					s_renameError = "A name cannot contain a path.";
					blocked = true;
				}
				if (!blocked)
				{
					const std::filesystem::path oldP(s_renameTarget);
					const std::filesystem::path newP = s_renameIsFolder
						? oldP.parent_path() / newName
						: oldP.parent_path() / (newName + oldP.extension().string());
					std::error_code exEc;
					// equivalent(), not a path comparison: on a case-insensitive
					// disk "foo" and "Foo" ARE the same file, and refusing to
					// change a name's capitalisation would be wrong.
					if (std::filesystem::exists(newP, exEc) &&
					    !std::filesystem::equivalent(oldP, newP, exEc))
					{
						s_renameError = "\"" + newP.filename().string() +
						                "\" already exists here.";
						blocked = true;
					}
				}

				if (!blocked)
				{
					std::filesystem::path oldPath(s_renameTarget);
					std::filesystem::path newPath;
					if (s_renameIsFolder)
						newPath = oldPath.parent_path() / newName;
					else
						newPath = oldPath.parent_path() / (newName + oldPath.extension().string());

					// Renaming an existing asset in a session is a REQUEST, the
					// same as deleting one: it breaks every reference to the old
					// name, which is as much somebody else's problem as ours.
					// Naming a freshly created asset is not — nothing refers to
					// it yet, and it is announced under its final name anyway.
					//
					// NOT an early return, however tempting: this popup sits
					// inside the panel's own Begin/End, and leaving from here
					// would skip the End and unbalance the ImGui window stack —
					// which nothing but a runtime assertion would ever tell us.
					// NOT gated on being a client. requestAssetRename answers the
					// host by broadcasting it — that branch is the whole reason
					// it exists — and excluding the host here meant a host's
					// rename went to its own disk and nowhere else: peers kept
					// the old path, and the next scene save handed them a
					// reference to a file they did not have. The delete paths
					// call unconditionally for exactly this reason; this one now
					// matches them.
					bool requested = false;
					if (!s_renameIsCreate && ctx.collab &&
					    ctx.contentManager && ctx.collab->inSession())
					{
						const std::string relOld =
							ctx.contentManager->toContentRelativePath(oldPath.string());
						const std::string relNew =
							ctx.contentManager->toContentRelativePath(newPath.string());
						// Folders included: renaming one moves every asset under
						// it, so it breaks references on a far larger scale than
						// renaming a single file does.
						requested = !relOld.empty() && !relNew.empty() &&
						            ctx.collab->requestAssetRename(relOld, relNew,
						                                          s_renameIsFolder);
					}

					// Nothing moves locally when it was asked for. It moves when
					// the host says so, on every machine at once — renaming here
					// first would leave us out of step until the answer, and out
					// of step for good if the answer is no.
					std::error_code ec;
					if (!requested) std::filesystem::rename(oldPath, newPath, ec);
					// A rename that FAILS used to close the dialog and say nothing —
					// no message, no log line — so a name the filesystem refuses
					// (permissions, a lock, a case-only change on a case-insensitive
					// volume) looked exactly like one that worked, until the tile came
					// back under its old name. The dialog already keeps itself open
					// with a reason for the failures IT detects; one from the
					// filesystem deserves the same.
					if (!requested && ec)
					{
						s_renameError = "Could not rename: " + ec.message();
						HE_LOG_ERROR(Editor, "%s",
							("Editor: rename of '" + s_renameTarget + "' failed: " + ec.message()).c_str());
					}
					if (!requested && !ec)
					{
						// Carry every stored reference to the old path (or, for a
						// folder, to anything under it) over to the new one — an
						// asset renamed out from under its referrers is exactly how
						// a project silently loses its materials/textures. Skipped
						// on create: a brand-new asset has no referrers yet.
						if (!s_renameIsCreate)
							retargetReferences(ctx, s_renameTarget, newPath.string(), s_renameIsFolder);
						// Both paths now hold something else than the type cache believes:
						// the old one nothing, the new one possibly a stale negative entry
						// from before it existed. A FOLDER rename moves every asset below
						// it, so there the whole map has to go.
						if (s_renameIsFolder)
						{
							EditorAssetTypeCache::invalidateAll();
							// Same for the tiles: every asset under the folder is now
							// addressed by a path that no longer exists. Their .hthumb
							// files are left behind as orphans rather than walking the
							// whole subtree to delete them — they are never looked up
							// again, and the cache directory is disposable by design.
							AssetThumbnailCache::clear();
						}
						else
						{
							EditorAssetTypeCache::invalidate(s_renameTarget);
							EditorAssetTypeCache::invalidate(newPath.string());
							AssetThumbnailCache::invalidate(s_renameTarget);
							AssetThumbnailCache::invalidate(newPath.string());
						}
						if (s_selectedItem == s_renameTarget)
							s_selectedItem = newPath.string();
						// The name the session will hear about. Only meaningful on
						// create; on a rename this is empty and stays so.
						if (s_renameIsCreate && !s_pendingCreatePublish.empty())
							s_pendingCreatePublish = newPath.string();
						if (!s_renameIsFolder)
						{
							for (auto& t : ctx.tabs)
								if (t.assetPath == s_renameTarget)
								{
									t.assetPath = newPath.string();
									t.label     = newName;
								}
						}
						s_quietContentRefresh = true;

						// In a C++ project, a freshly created scene gets a matching
						// native level script (Source/<Scene>LevelScript.{h,cpp}) with
						// event stubs — the C++ mirror of the HorizonCode Level Script
						// graph. Uses the scene's final name.
						if (s_renameIsCreate && ctx.projectManager &&
						    ctx.projectManager->currentProject().scriptLanguage == ProjectScriptLanguage::Cpp &&
						    newPath.extension() == ".hescene")
						{
							const std::filesystem::path projRoot =
								std::filesystem::path(ctx.projectManager->currentProject().path).parent_path();
							if (writeCppLevelScript(projRoot.string(), newName))
								HE_LOG_INFO(Editor, "%s",
									("Editor: generated C++ level script for scene '" + newName + "'").c_str());
						}
					}
				}
				// Left open when the name was refused: the dialog is where the
				// reason is, and closing it would look like the rename worked.
				if (!blocked)
				{
					s_renameTarget.clear();
					s_renameIsCreate = false;
					s_renameScriptLang = -1;
					s_renameError.clear();
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(140, 0)))
			{
				// Cancel means cancel. The asset is written BEFORE this dialog opens
				// (the name is the only thing still missing), so leaving it there
				// turned "no, never mind" into a NewMaterial.hasset the user then had
				// to hunt down and delete through the confirmation dialog — more work
				// than going through with the creation would have been.
				//
				// Only for a CREATE: cancelling a rename must obviously not delete
				// the asset being renamed.
				if (s_renameIsCreate && !s_renameTarget.empty())
				{
					std::error_code rmEc;
					if (s_renameIsFolder) std::filesystem::remove_all(s_renameTarget, rmEc);
					else                  std::filesystem::remove(s_renameTarget, rmEc);
					if (rmEc)
						HE_LOG_WARN(Editor, "%s",
							("Editor: cancelled create left '" + s_renameTarget + "' behind: " +
							 rmEc.message()).c_str());
					else
					{
						EditorAssetTypeCache::invalidate(s_renameTarget);
						AssetThumbnailCache::invalidate(s_renameTarget);
						if (s_selectedItem == s_renameTarget) s_selectedItem.clear();
						s_quietContentRefresh = true;
					}
					// Nothing was created after all, so nothing may be announced to
					// the session — the publish below fires on whatever this holds.
					s_pendingCreatePublish.clear();
					s_pendingCreateIsFolder = false;
				}
				s_renameTarget.clear();
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				s_renameError.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
		// ── The created asset is announced HERE ───────────────────────────
		// After the popup block, so it runs however the dialog ended: OK with a
		// new name, Cancel keeping the default, or Escape — which closes a modal
		// without running either branch and used to leave s_renameTarget and
		// s_renameIsCreate standing. Waiting for it to be closed AND not merely
		// waiting to open (the content-refresh gate can hold it back a frame) is
		// what keeps this from firing on the frame between the two.
		else if (!s_pendingCreatePublish.empty() && !s_openRenamePopup)
		{
			if (s_renameIsCreate)   // Escape: neither branch cleared this
			{
				s_renameTarget.clear();
				s_renameIsCreate   = false;
				s_renameScriptLang = -1;
			}
			// A new asset has no referrers and no lock, so there is nothing to
			// hold and nothing to retarget — the host decides whether the name
			// is free and hands it on.
			if (ctx.collab && ctx.contentManager)
			{
				// collabKeyFor, not toContentRelativePath: anything under Source
				// — a C++ class OR a folder in that tree — has no
				// content-relative form, and the empty string read as "no
				// session" meant the create never left this machine.
				const std::string rel =
					collabKeyFor(ctx, s_pendingCreatePublish, s_pendingCreateIsFolder);
				if (rel.empty()) { /* nothing a session carries */ }
				else if (s_pendingCreateIsFolder) ctx.collab->publishFolderCreate(rel);
				else ctx.collab->publishAssetCreate(rel, s_pendingCreatePublish);
			}
			s_pendingCreatePublish.clear();
			s_pendingCreateIsFolder = false;
		}

		// ── Delete-asset confirmation ─────────────────────────────────────
		// Opened here rather than from the context menu for the same reason as
		// the two below: a modal cannot be opened from inside the context
		// popup's ID stack.
		if (s_openDeleteAssetPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_delete_asset_popup");
			s_openDeleteAssetPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_delete_asset_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			// In a session this is a REQUEST, not a deletion — for everyone but
			// the host, who has just answered the only question a request asks.
			// Read before the wrapped block below because the button at the foot of
			// the dialog is labelled from it, and the block has to close before
			// EndPopup() (see the note inside it).
			const bool needsApproval = ctx.collab && ctx.collab->inSession() &&
			                           !ctx.collab->isHost();
			// 460 px holds about sixty-five characters and the red line below is
			// seventy-three: unwrapped it ends around "cannot be", so the dialog
			// shows "This deletes the file. It does not go to the trash and" and
			// swallows the clause saying the deletion cannot be undone — the single
			// fact the confirmation exists to state. Fixed width and NoResize, so
			// wrapping at the window edge cannot feed back into the dialog's size;
			// only its height grows.
			//
			// In a block, because EndPopup() below hands the current window back to
			// the browser's grid child: a pop after that point would strip the wrap
			// position from the panel instead of from this dialog. The button row
			// after the block draws no text of its own — a button label is clipped by
			// ImGui, never wrapped.
			{
				EditorWidgets::WrapText wrap;
				const int assetCount = static_cast<int>(s_deleteAssetTargets.size());
				if (assetCount == 1)
					ImGui::Text("Delete \"%s\"",
						std::filesystem::path(s_deleteAssetTargets.front()).filename().string().c_str());
				else
					ImGui::Text("Delete %d assets", assetCount);
				ImGui::Separator();
				ImGui::Spacing();
				// Two facts the user cannot get back afterwards, so they belong here
				// rather than in a log line nobody reads.
				ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.45f, 1.0f),
					assetCount == 1
						? "This deletes the file. It does not go to the trash and cannot be undone."
						: "This deletes the files. They do not go to the trash and cannot be undone.");
				if (s_deleteAssetIsSource)
					ImGui::TextDisabled("Both halves of the class (.h and .cpp) are deleted.");
				// Naming them is the point of a confirmation for a SET: "delete 12
				// assets" is not something anyone can check, and the selection may have
				// been built with a Shift-click over a run nobody read.
				if (assetCount > 1)
				{
					const float lineH = ImGui::GetTextLineHeightWithSpacing();
					const float listH = std::clamp(lineH * static_cast<float>(assetCount) + 8.0f,
					                               lineH * 2.0f, 140.0f);
					ImGui::Spacing();
					ImGui::BeginChild("##cb_delete_asset_list", ImVec2(-1.0f, listH), true);
					// Wrapped rather than scrolled sideways — see the referrer list.
					// This is the list somebody reads to decide whether to destroy
					// twelve files, and a name whose end is off-screen is a name they
					// cannot check. Its own guard: the child starts with no wrap
					// position, and this one has to pop before EndChild() hands the
					// dialog window back.
					{
						EditorWidgets::WrapText listWrap;
						for (const std::string& p : s_deleteAssetTargets)
							ImGui::TextUnformatted(std::filesystem::path(p).filename().string().c_str());
					}
					ImGui::EndChild();
				}
				ImGui::Spacing();
				// Which files break, by name — the question the old flat "anything still
				// referencing it keeps a broken reference" line raised and left hanging.
				drawReferenceSection(s_deleteAssetScanGen,
					"They keep a reference to something that is no longer there.");
				ImGui::Spacing();

				if (needsApproval)
				{
					ImGui::Spacing();
					ImGui::TextDisabled("The host has to approve this before it happens.");
					ImGui::Spacing();
				}
			}
			if (EditorWidgets::dangerButton(needsApproval ? "Ask the host" : "Delete",
			                                ImVec2(210, 0)))
			{
				// One confirmation, N deletions — and, in a session, ONE thing for
				// the host to answer. The requests still travel individually (each
				// keeps its own id, which is what lets the host approve nine of
				// twelve), but they carry a shared batch id, so the in-tray shows a
				// single "Delete 12 assets" row that opens into the list instead of
				// twelve rows to click through.
				if (ctx.collab) ctx.collab->beginAssetOpBatch();
				for (const std::string& target : s_deleteAssetTargets)
				{
					// Asked again per path, not trusted from the frame the dialog was
					// opened on: the browser can refresh underneath an open modal.
					if (isReadOnlyGround(target)) continue;
					// "Is this a C++ class?" is a property of the FILE, not of the
					// root the grid happened to be showing. One flag for a whole set
					// either orphans the .cpp half of a class or runs the sibling
					// sweep next to a plain .hasset.
					const bool isSourceClass = isUnderFolder(target, sourceFolder.fullPath);
					// collabKeyFor, not toContentRelativePath: a C++ class is not
					// under the content root, and reading its empty content-relative
					// path as "no session" deleted it here and nowhere else.
					const std::string rel = collabKeyFor(ctx, target);
					// requestOrPerformAssetOp answers false when there is no session,
					// which is the ordinary case and means: just delete it.
					if (!(ctx.collab && !rel.empty() &&
					      ctx.collab->requestAssetDelete(rel)))
					{
						deleteAssetNow(target, isSourceClass);
					}
				}
				if (ctx.collab) ctx.collab->endAssetOpBatch();
				s_deleteAssetTargets.clear();
				s_selection.clear();
				s_deleteAssetScanGen = 0;
				cancelReferenceScan();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(210, 0)))
			{
				s_deleteAssetTargets.clear();
				s_deleteAssetScanGen = 0;
				cancelReferenceScan();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		// Escape closes a modal without running either branch, and a target left
		// behind would be deleted by the NEXT confirmation. Same stale-state trap
		// the create/rename popup has (see the note there) — and the scan the
		// dialog started has nobody left to answer, so it is called off too.
		else if (!s_deleteAssetTargets.empty() && !s_openDeleteAssetPopup)
		{
			s_deleteAssetTargets.clear();
			s_deleteAssetScanGen = 0;
			cancelReferenceScan();
		}

		// ── Pattern rename ────────────────────────────────────────────────
		// A rule, applied to a set, with the result on screen BEFORE anything
		// happens. The preview is not a nicety: a find-and-replace over twelve
		// files is exactly the operation where "I meant the other Rock" is only
		// visible once you see the twelve new names next to the old ones — and
		// afterwards it is twelve renames to undo by hand.
		if (s_openPatternRenamePopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_pattern_rename_popup");
			s_openPatternRenamePopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_pattern_rename_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::Text("Rename %d assets", static_cast<int>(s_patternTargets.size()));
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::SetNextItemWidth(180.0f);
			ImGui::InputTextWithHint("##pat_find", "find", s_patternFind, sizeof(s_patternFind));
			ImGui::SameLine();
			ImGui::TextDisabled("\xE2\x86\x92");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(180.0f);
			ImGui::InputTextWithHint("##pat_repl", "replace with",
			                         s_patternReplace, sizeof(s_patternReplace));

			ImGui::SetNextItemWidth(180.0f);
			ImGui::InputTextWithHint("##pat_pre", "prefix", s_patternPrefix, sizeof(s_patternPrefix));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(180.0f);
			ImGui::InputTextWithHint("##pat_suf", "suffix", s_patternSuffix, sizeof(s_patternSuffix));

			ImGui::Checkbox("Number them", &s_patternNumber);
			if (s_patternNumber)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(90.0f);
				ImGui::InputInt("starting at", &s_patternStart);
				if (s_patternStart < 0) s_patternStart = 0;
			}
			ImGui::Spacing();

			// The rule, in one place, so the preview and the commit below cannot
			// drift into meaning two different things.
			auto newStemFor = [&](const std::string& absPath, int index) -> std::string
			{
				std::string stem = std::filesystem::path(absPath).stem().string();
				const std::string find(s_patternFind);
				if (!find.empty())
				{
					const std::string repl(s_patternReplace);
					std::string out;
					out.reserve(stem.size());
					for (std::size_t i = 0; i < stem.size(); )
					{
						if (stem.compare(i, find.size(), find) == 0)
						{ out += repl; i += find.size(); }
						else out += stem[i++];
					}
					stem = out;
				}
				stem = std::string(s_patternPrefix) + stem + std::string(s_patternSuffix);
				if (s_patternNumber) stem += std::to_string(s_patternStart + index);
				return stem;
			};

			// Refused for the same reasons a single rename is, plus the one a set
			// adds: two rules can collapse two names onto one, and the second
			// rename would then silently replace the first one's file.
			std::string blockReason;
			std::vector<std::pair<std::string, std::string>> plan;   // abs -> new abs
			{
				std::unordered_map<std::string, int> seen;
				for (std::size_t i = 0; i < s_patternTargets.size(); ++i)
				{
					const std::filesystem::path old(s_patternTargets[i]);
					const std::string stem = newStemFor(s_patternTargets[i], static_cast<int>(i));
					if (stem.empty())
					{ blockReason = "That leaves at least one asset with no name."; break; }
					if (stem.find('/') != std::string::npos || stem.find('\\') != std::string::npos)
					{ blockReason = "A name cannot contain a path."; break; }
					const std::filesystem::path dst =
						old.parent_path() / (stem + old.extension().string());
					if (++seen[dst.string()] > 1)
					{ blockReason = "Two assets would end up with the same name."; break; }
					std::error_code exEc;
					if (std::filesystem::exists(dst, exEc) &&
					    !std::filesystem::equivalent(old, dst, exEc))
					{ blockReason = "\"" + dst.filename().string() + "\" already exists here."; break; }
					plan.emplace_back(s_patternTargets[i], dst.string());
				}
			}

			// Old → new, for as many as fit. Unchanged rows are dimmed: with a
			// rule that only matches half the selection, which half is the thing
			// worth seeing.
			const float lineH = ImGui::GetTextLineHeightWithSpacing();
			const float listH = std::clamp(lineH * static_cast<float>(s_patternTargets.size()) + 8.0f,
			                               lineH * 2.0f, 200.0f);
			ImGui::BeginChild("##pat_preview", ImVec2(-1.0f, listH), true);
			// Wrapped, not scrolled sideways. The whole value of this preview is
			// reading the NEW name before committing to it, and that is the half
			// that sits furthest right — exactly the half a horizontal scrollbar
			// hides. The wrap position covers the arrow and both names, so a long
			// pair breaks onto a second line instead of running off the edge.
			//
			// In a block, so the pop lands on this child rather than on the dialog
			// window EndChild() restores below it.
			{
				EditorWidgets::WrapText wrap;
				for (std::size_t i = 0; i < s_patternTargets.size(); ++i)
				{
					const std::filesystem::path old(s_patternTargets[i]);
					const std::string oldStem = old.stem().string();
					const std::string newStem = newStemFor(s_patternTargets[i], static_cast<int>(i));
					if (oldStem == newStem)
						ImGui::TextDisabled("%s  (unchanged)", oldStem.c_str());
					else
					{
						ImGui::TextDisabled("%s", oldStem.c_str());
						ImGui::SameLine();
						ImGui::TextDisabled("\xE2\x86\x92");
						ImGui::SameLine();
						ImGui::TextUnformatted(newStem.c_str());
					}
				}
			}
			ImGui::EndChild();

			int changed = 0;
			for (std::size_t i = 0; i < s_patternTargets.size(); ++i)
				if (std::filesystem::path(s_patternTargets[i]).stem().string() !=
				    newStemFor(s_patternTargets[i], static_cast<int>(i))) ++changed;

			// The refusal carries a filename: "\"Rock_Cliff_Weathered_02.hasset\"
			// already exists here." passes the 560 px this dialog is pinned to, and
			// the part that gets cut is "already exists here" — leaving a quoted name
			// and no reason next to a Rename button that has gone grey for no visible
			// cause. Fixed width and NoResize, so wrapping cannot shrink the dialog.
			//
			// Only these two lines, and in a block: everything between the header and
			// here is either an input (which ImGui does not wrap anyway) or the plan
			// that the button below reads, and the pop has to happen before the
			// EndPopup() at the foot of this body — after it, the current window is
			// the browser's grid child.
			{
				EditorWidgets::WrapText wrap;
				if (!blockReason.empty())
					ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.45f, 1.0f), "%s", blockReason.c_str());
				else
					ImGui::TextDisabled("References follow the assets, as with a single rename.");
			}
			ImGui::Spacing();

			const bool canApply = blockReason.empty() && changed > 0 &&
			                      plan.size() == s_patternTargets.size();
			ImGui::BeginDisabled(!canApply);
			if (EditorWidgets::primaryButton(
					changed > 0 ? ("Rename " + std::to_string(changed)).c_str() : "Rename",
					ImVec2(210, 0)))
			{
				// One gesture, so one batch for the session and one retarget walk.
				if (ctx.collab) ctx.collab->beginAssetOpBatch();
				for (const auto& [oldAbs, newAbs] : plan)
				{
					if (oldAbs == newAbs) continue;
					// A session decides renames centrally, exactly as the single
					// rename does; without a session this is just a move.
					const std::string oldKey = collabKeyFor(ctx, oldAbs);
					const std::string newKey = collabKeyFor(ctx, newAbs);
					if (ctx.collab && !oldKey.empty() && !newKey.empty() &&
					    ctx.collab->requestAssetRename(oldKey, newKey))
						continue;   // asked for; the host's answer moves it everywhere

					std::error_code ec;
					std::filesystem::rename(oldAbs, newAbs, ec);
					if (ec)
					{
						HE_LOG_ERROR(Editor, "%s",
							("Editor: rename of '" + oldAbs + "' failed: " + ec.message()).c_str());
						continue;
					}
					retargetReferences(ctx, oldAbs, newAbs, /*folder=*/false);
					EditorAssetTypeCache::invalidate(oldAbs);
					EditorAssetTypeCache::invalidate(newAbs);
					AssetThumbnailCache::invalidate(oldAbs);
					AssetThumbnailCache::invalidate(newAbs);
					for (auto& t : ctx.tabs)
						if (t.assetPath == oldAbs) t.assetPath = newAbs;
					for (std::string& sel : s_selection)
						if (sel == oldAbs) sel = newAbs;
					if (s_selectedItem == oldAbs) s_selectedItem = newAbs;
				}
				if (ctx.collab) ctx.collab->endAssetOpBatch();
				s_quietContentRefresh = true;
				s_patternTargets.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(210, 0)))
			{
				s_patternTargets.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		else if (!s_patternTargets.empty() && !s_openPatternRenamePopup)
			s_patternTargets.clear();

		// ── "Find References" result ──────────────────────────────────────
		// Not a confirmation: nothing is about to happen, so there is one way out
		// and no red button. Opened out here for the same ID-stack reason as the
		// dialogs around it.
		if (s_openReferencesPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_references_popup");
			s_openReferencesPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_references_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			// The title carries an asset name the user did not choose the length of:
			// "What uses anything in \"Environment_Cliffs_Weathered\"?" is past the
			// 460 px this dialog is pinned to, and the question mark — the part that
			// makes it a question about THAT folder — is the first thing lost. Fixed
			// width, NoResize, so the window edge is the column to wrap at.
			//
			// A block, since EndPopup() at the foot of this body restores the
			// browser's grid child as the current window.
			{
				EditorWidgets::WrapText wrap;
				const std::string name = s_referencesIsFolder
					? std::filesystem::path(s_referencesTarget).filename().string()
					: std::filesystem::path(s_referencesTarget).stem().string();
				ImGui::Text(s_referencesIsFolder ? "What uses anything in \"%s\"?" : "What uses \"%s\"?",
				            name.c_str());
				ImGui::Separator();
				ImGui::Spacing();
				drawReferenceSection(s_referencesScanGen,
					s_referencesIsFolder ? "Renaming or moving the folder carries these along."
					                     : "Renaming or moving it carries these along; deleting it breaks them.");
				ImGui::Spacing();
			}
			if (EditorWidgets::primaryButton("Close", ImVec2(210, 0)))
			{
				s_referencesTarget.clear();
				s_referencesScanGen = 0;
				cancelReferenceScan();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		else if (!s_referencesTarget.empty() && !s_openReferencesPopup)
		{
			s_referencesTarget.clear();
			s_referencesScanGen = 0;
			cancelReferenceScan();
		}

		// ── Delete-folder confirmation ────────────────────────────────────
		// Raised from the item context menu, opened here for the same reason the
		// rename dialog is: a modal cannot be opened from inside the context
		// popup's ID stack, and the content-refresh gate keeps it from competing
		// with the "##ContentRefresh" modal (which a delete requests as well).
		if (s_openDeleteFolderPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_delete_folder_popup");
			s_openDeleteFolderPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_delete_folder_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			// A folder is the biggest yes in this panel — everything under it
			// goes — so in a session it is asked for like any other deletion. Read
			// out here because the button below is labelled from it, and the wrapped
			// block has to close before EndPopup() (see the note inside it).
			const bool folderNeedsApproval =
				ctx.collab && ctx.collab->inSession() && !ctx.collab->isHost();
			// A folder name is as long as the user made it, and this dialog is pinned
			// to 460 px: "Delete Folder \"Environment_Cliffs_Weathered\"" already runs
			// out of room, and the name — the only thing distinguishing this dialog
			// from the one about another folder — is what gets cut. Fixed width and
			// NoResize, so the window edge is the right column and wrapping only ever
			// makes the dialog taller.
			//
			// In a block: EndPopup() at the foot of this body makes the browser's
			// grid child current again, so a pop after it would land on the panel.
			{
				EditorWidgets::WrapText wrap;
				const std::string folderName =
					std::filesystem::path(s_deleteFolderTarget).filename().string();
				const int fileCount = static_cast<int>(s_deleteFolderFiles.size());

				ImGui::Text("Delete Folder \"%s\"", folderName.c_str());
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.45f, 1.0f),
					"This deletes the folder and the %d asset%s inside it.",
					fileCount, fileCount == 1 ? "" : "s");
				ImGui::Spacing();
				// Scanned for the whole subtree at once — every asset under the folder is
				// a target, and referrers INSIDE the folder are left out: they are going
				// away with it, so they say nothing about what breaks.
				drawReferenceSection(s_deleteFolderScanGen,
					"They keep references to assets that are no longer there.");
				ImGui::Spacing();

				// The list is what makes the warning worth reading, so it is shown in
				// full up to a point — a folder with thousands of assets would cost a
				// line of text per entry per frame, and the tail says nothing the count
				// above has not already said.
				constexpr int k_maxListed = 200;
				const int listed = (std::min)(fileCount, k_maxListed);
				const float lineH  = ImGui::GetTextLineHeightWithSpacing();
				const float listH  = std::clamp(lineH * static_cast<float>(listed) + 8.0f,
				                                lineH * 2.0f, 220.0f);
				ImGui::BeginChild("##cb_delete_folder_list", ImVec2(-1.0f, listH), true);
				// Wrapped, not scrolled sideways. These are paths relative to the
				// content root and several folders deep, so this is the list that
				// overflowed worst — and it is the one attached to the biggest yes in
				// the panel. Its own guard, in its own block: the child window starts
				// without a wrap position, and this one has to pop before EndChild()
				// makes the dialog current again.
				{
					EditorWidgets::WrapText listWrap;
					for (int i = 0; i < listed; ++i)
						ImGui::TextUnformatted(s_deleteFolderFiles[static_cast<size_t>(i)].c_str());
					if (fileCount > listed)
						ImGui::TextDisabled("... and %d more", fileCount - listed);
				}
				ImGui::EndChild();
				ImGui::Spacing();

				if (folderNeedsApproval)
				{
					ImGui::TextDisabled("The host has to approve this before it happens.");
					ImGui::Spacing();
				}
			}
			if (EditorWidgets::dangerButton(folderNeedsApproval ? "Ask the host" : "Delete",
			                                ImVec2(210, 0)))
			{
				const std::string rel =
					collabKeyFor(ctx, s_deleteFolderTarget, /*isFolder=*/true);
				if (!(ctx.collab && !rel.empty() &&
				      ctx.collab->requestAssetDelete(rel, /*folder=*/true)))
				{
					deleteFolderNow(s_deleteFolderTarget);
				}
				s_deleteFolderTarget.clear();
				s_deleteFolderFiles.clear();
				s_deleteFolderScanGen = 0;
				cancelReferenceScan();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(210, 0)))
			{
				s_deleteFolderTarget.clear();
				s_deleteFolderFiles.clear();
				s_deleteFolderScanGen = 0;
				cancelReferenceScan();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		// Same Escape trap as the asset dialog above: the folder target survives a
		// dismissal that ran neither branch, and the scan keeps walking for a
		// dialog nobody is looking at.
		else if (!s_deleteFolderTarget.empty() && !s_openDeleteFolderPopup)
		{
			s_deleteFolderTarget.clear();
			s_deleteFolderFiles.clear();
			s_deleteFolderScanGen = 0;
			cancelReferenceScan();
		}

#ifdef HE_HAVE_LIBSSH2
		// ── EngineContent remote-download confirmation ─────────────────────
		// Raised from the file-grid double-click above. Same "cannot open a modal
		// from inside another ID stack" reason the delete-folder dialog is opened
		// out here instead of at the click site.
		if (s_openRemoteDownloadPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_remote_download_popup");
			s_openRemoteDownloadPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_remote_download_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			// TextWrapped already covers the paragraph; the title is the line that
			// does not. At the 420 px this dialog is pinned to, "Download
			// \"SM_Rock_Cliff_Weathered_Large\"?" loses its closing quote and its
			// question mark, so the one dialog that asks about a NAMED asset stops
			// showing the whole name. Fixed width and NoResize — the window edge is
			// the right column, and wrapping only grows the height.
			//
			// A block, because EndPopup() below restores the browser's grid child as
			// the current window and the pop must happen before that.
			{
				EditorWidgets::WrapText wrap;
				ImGui::Text("Download \"%s\"?", s_remoteDownloadTabLabel.c_str());
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::TextWrapped(
					"This EngineContent asset is not on this machine yet — it lives on the "
					"EngineContent server. Downloading it saves a copy in the shared "
					"EngineContent cache, so every project (not just this one) can use it "
					"from now on without downloading it again.");
				ImGui::Spacing();
			}

			if (EditorWidgets::primaryButton("Download", ImVec2(200, 0)))
			{
				const std::string relPath           = s_remoteDownloadRelativePath;
				const std::string fullPath          = s_remoteDownloadFullPath;
				GlobalState*      gs                = ctx.globalState;
				const std::string engineContentPath = ctx.contentManager->engineContentRoot();
				const std::string projectContentRoot = ctx.contentManager->contentRoot();
				HE::Cs::EngineContentSync::instance().enqueueDownload(
					relPath, s_remoteDownloadUuid, HE::Cs::DownloadTrigger::Explicit,
					[fullPath, relPath, gs, engineContentPath, projectContentRoot](bool success)
					{
						// Worker thread — must not touch ctx/ImGui. GlobalState's own
						// refresh is safe here (mutex-guarded, filesystem-only, same
						// fact startSftpProbe's manifest refresh already relies on).
						// The re-merge is what clears this asset's isRemoteOnly flag:
						// mergeManifestInto now sees the freshly-downloaded file in the
						// cache and emits a normal node instead of a remote placeholder.
						// The tab-open request, which DOES need the main thread, goes
						// through s_pendingOpenFullPaths (static storage, so this
						// reference stays valid regardless of when the callback fires).
						//
						// This is the EXPLICIT route: the user pressed Download and
						// is waiting for a tab to open. By the time it fails the
						// dialog has closed and the footer's progress bar is gone —
						// so without a word here, the answer to "I clicked Download"
						// is nothing at all, forever.
						if (!success)
						{
							HE::Ed::notify(HE::Ed::NoteLevel::Problem,
								"\"" + std::filesystem::path(relPath).filename().string()
									+ "\" could not be downloaded.",
								"The EngineContent server did not hand over the file. Check "
								"the connection and start the download again.",
								fullPath);
							return;
						}
						if (gs && !engineContentPath.empty())
							gs->refreshEngineFolder(engineContentPath, projectContentRoot);
						std::lock_guard<std::mutex> lock(s_pendingOpenMutex);
						s_pendingOpenFullPaths.push_back(fullPath);
					});
				s_remoteDownloadRelativePath.clear();
				s_remoteDownloadTabLabel.clear();
				s_remoteDownloadFullPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(200, 0)))
			{
				s_remoteDownloadRelativePath.clear();
				s_remoteDownloadTabLabel.clear();
				s_remoteDownloadFullPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// ── "Remove Local Copy" confirmation ──────────────────────────────
		// The mirror image of the download dialog above, and worded as such: this
		// asks about DISK, not about the asset. Nothing here is destructive in the
		// way the two delete dialogs are, so it says what will happen next (a
		// re-download) rather than what will be lost.
		if (s_openRemoveCachePopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_remove_cache_popup");
			s_openRemoveCachePopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_remove_cache_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			// Two lines here are longer than the 460 px this dialog is pinned to. The
			// title carries a full filename, and the dimmed line below is
			// seventy-eight characters: unwrapped it ends at "the copy in memory", so
			// the caveat says something is in memory and never says until when — the
			// only part of it that is actionable. Fixed width and NoResize, so the
			// window edge is the column and wrapping only makes the dialog taller.
			//
			// In a block, so the pop happens before the EndPopup() at the foot of
			// this body — after it the current window is the browser's grid child.
			{
				EditorWidgets::WrapText wrap;
				const std::string assetName =
					std::filesystem::path(s_removeCacheFullPath).filename().string();

				ImGui::Text("Remove the local copy of \"%s\"?", assetName.c_str());
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::TextWrapped(
					"This deletes the downloaded file from the shared EngineContent cache "
					"on this machine — every project here loses its local copy. The asset "
					"itself stays on the server and is downloaded again the next time "
					"something needs it.");
				ImGui::Spacing();
				// Same scan the delete dialogs run, different conclusion: these files do
				// not break, they simply trigger the download again.
				drawReferenceSection(s_removeCacheScanGen,
					"They keep working — the asset downloads again when they need it.");
				ImGui::Spacing();
				// Honest about the half that a file removal cannot undo: an asset that is
				// live in the open scene stays in memory until it is reloaded.
				if (ctx.contentManager && !(s_removeCacheUuid == HE::UUID{}) &&
				    ctx.contentManager->isLoaded(s_removeCacheUuid))
				{
					ImGui::TextDisabled("It is loaded right now — the copy in memory stays until the scene is reloaded.");
					ImGui::Spacing();
				}
			}

			if (EditorWidgets::dangerButton("Remove Local Copy", ImVec2(210, 0)))
			{
				removeCachedCopyNow(s_removeCacheFullPath, s_removeCacheRelPath, s_removeCacheUuid);
				s_removeCacheFullPath.clear();
				s_removeCacheRelPath.clear();
				s_removeCacheUuid   = HE::UUID{};
				s_removeCacheScanGen = 0;
				cancelReferenceScan();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(210, 0)))
			{
				s_removeCacheFullPath.clear();
				s_removeCacheRelPath.clear();
				s_removeCacheUuid   = HE::UUID{};
				s_removeCacheScanGen = 0;
				cancelReferenceScan();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		// The same Escape trap the two delete dialogs guard against: a target left
		// behind would be removed by the NEXT confirmation, and the scan would keep
		// walking a whole project for a dialog that is gone.
		else if (!s_removeCacheFullPath.empty() && !s_openRemoveCachePopup)
		{
			s_removeCacheFullPath.clear();
			s_removeCacheRelPath.clear();
			s_removeCacheUuid    = HE::UUID{};
			s_removeCacheScanGen = 0;
			cancelReferenceScan();
		}
#endif

		// ── Name-a-C++-class popup (C++ projects only) ────────────────────
		// The Create menu's "C++ Class" item stages a default name and raises
		// this; on confirm it writes Source/<Name>.{h,cpp}. Those files live
		// outside Content/, so they don't appear in the browser — the user edits
		// them in their own C++ toolchain (which is the C++ workflow).
		if (s_openCppClassPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_cpp_class_popup");
			s_openCppClassPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_cpp_class_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			// The narrowest dialog in the panel, and the only one whose lines fit at
			// the 340 px it asks for. They do not always get 340: pinDialogToEditorWindow
			// caps every dialog to the editor's work area and floors that cap at
			// 240 px, so on a small editor window this one is squeezed and "Creates
			// Source/<Name>.h and .cpp" loses its tail — the half that says a class is
			// TWO files, which is the entire content of the line. Wrapping at the
			// window edge follows the cap wherever it lands; the dialog does not
			// auto-resize, so there is nothing for the wrap to feed back into.
			//
			// In a block that closes before the EndPopup() below, which is what hands
			// the current window back to the browser's grid child.
			bool confirm = false;
			{
				EditorWidgets::WrapText wrap;
				ImGui::TextUnformatted("New C++ Class");
				ImGui::Separator();
				ImGui::Spacing();
				if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(-1.0f);
				confirm = ImGui::InputText("##cpp_class_input", s_cppClassName, sizeof(s_cppClassName),
					ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
				ImGui::TextDisabled("Creates Source/<Name>.h and .cpp");
				ImGui::Spacing();
			}
			const bool canCreate = s_cppClassName[0] != '\0' && ctx.projectManager &&
				!ctx.projectManager->currentProject().path.empty();
			if ((EditorWidgets::primaryButton("Create", ImVec2(150, 0)) || confirm) && canCreate)
			{
				const std::filesystem::path projRoot =
					std::filesystem::path(ctx.projectManager->currentProject().path).parent_path();
				std::string created;
				if (writeCppClass(projRoot.string(), s_cppClassName, &created))
				{
					HE_LOG_INFO(Editor, "%s",
						("Editor: created C++ class at " + created).c_str());
					// BOTH halves. A class is a .h and a .cpp, and a peer that
					// received only the header would have a declaration with
					// nothing behind it — which compiles right up until it does
					// not. They travel under the reserved ::Source:: key, since
					// they live beside Content rather than inside it.
					if (ctx.collab && ctx.collab->inSession())
					{
						const std::string srcRoot = projRoot.string() + "/Source";
						const std::filesystem::path h(created);
						for (const char* ext : { ".h", ".cpp" })
						{
							const std::filesystem::path f =
								h.parent_path() / (h.stem().string() + ext);
							if (!std::filesystem::exists(f)) continue;
							const std::string rel =
								CollabController::projectRelativeAssetPath(f.string(), srcRoot);
							if (rel.empty()) continue;
							ctx.collab->publishAssetCreate("::Source::" + rel, f.string());
						}
					}
				}
				else
					HE_LOG_ERROR(Editor, "%s", "Editor: failed to create C++ class");
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(150, 0)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		// ── Background left-click → clear selection ───────────────────────
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			!ImGui::IsAnyItemHovered())
		{
			s_selectedItem.clear();
			s_selection.clear();
		}

		// ── Resolve a deferred collapse ───────────────────────────────────
		// The press on an already-selected tile parked its "just this one" here.
		// A drag consumed the gesture instead, so the set stands; otherwise the
		// click meant what a click means.
		if (!s_pendingCollapseTo.empty() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			if (!s_pendingCollapseDragged)
				s_selection.assign(1, s_pendingCollapseTo);
			s_pendingCollapseTo.clear();
			s_pendingCollapseDragged = false;
		}
		// A press that ended anywhere else (the drag landed, focus moved, the panel
		// was rebuilt) must not leave the decision hanging for the next click.
		else if (!s_pendingCollapseTo.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			s_pendingCollapseTo.clear();
			s_pendingCollapseDragged = false;
		}

		// ── Keyboard ──────────────────────────────────────────────────────
		// The panel answered no key at all: Delete did nothing, F2 did nothing,
		// Enter did nothing. Every one of those is muscle memory from the file
		// manager the user came from, and a browser that ignores them reads as
		// half-finished.
		//
		// Gated on IsAnyItemActive so the search box keeps its own keys — typing
		// "Delete" into it must not delete the selection. A modal takes focus away
		// from this window, so an open dialog swallows these already.
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		    !ImGui::IsAnyItemActive() && !s_selection.empty())
		{
			const bool onEngineGround = isReadOnlyGround(s_selectedItem);

			if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !onEngineGround)
			{
				// Same route as the menu item, dialog included — a keystroke must not
				// be the one path that destroys work without asking.
				if (s_selectedIsFolder)
				{
					s_deleteFolderTarget = s_selectedItem;
					s_deleteFolderFiles  = collectFolderContents(s_selectedItem);
					if (!s_deleteFolderFiles.empty())
					{
						s_deleteFolderScanGen   = beginScanFor(s_deleteFolderTarget, /*isFolder=*/true);
						s_openDeleteFolderPopup = true;
					}
					else s_deleteFolderTarget.clear();
				}
				else
				{
					// Same per-path filter the menu applies — a keystroke must not be
					// the one route that reaches read-only ground.
					s_deleteAssetTargets.clear();
					for (const std::string& p : s_selection)
						if (!isReadOnlyGround(p)) s_deleteAssetTargets.push_back(p);
					if (!s_deleteAssetTargets.empty())
					{
						s_deleteAssetIsSource  = s_selectedRootKind == 2;
						s_deleteAssetScanGen   = beginScanForMany(s_deleteAssetTargets, /*isFolder=*/false);
						s_openDeleteAssetPopup = true;
					}
				}
			}
			// Rename and open are single-item gestures by nature, so they act on the
			// anchor rather than on the whole selection.
			if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && !onEngineGround &&
			    s_selectedRootKind != 2 && !s_selectedItem.empty())
			{
				s_renameTarget     = s_selectedItem;
				s_renameIsFolder   = s_selectedIsFolder;
				s_renameIsCreate   = false;
				s_renameScriptLang = -1;
				const std::string shown = s_selectedIsFolder
					? std::filesystem::path(s_selectedItem).filename().string()
					: std::filesystem::path(s_selectedItem).stem().string();
				std::strncpy(s_renameBuf, shown.c_str(), sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !s_selectedItem.empty())
			{
				if (s_selectedIsFolder)
				{
					// Enter on a folder means "go in", which is the same thing the
					// tree and a double-click do.
					for (auto* sub : displayFolder->subfolders)
						if (sub->fullPath == s_selectedItem)
						{ s_gridFolder = sub; s_selectedTreeFolder = sub; break; }
				}
				else openAssetTab(s_selectedItem);
			}
		}

		// ── Background right-click context menu
		// Only open when clicking on the panel background (not on any item)
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
			!ImGui::IsAnyItemHovered())
		{
			ImGui::OpenPopup("##cb_create_ctx");
		}

		if (ImGui::BeginPopup("##cb_create_ctx"))
		{
			// Normal mode: the Engine tree is read-only ground — no creating
			// new "default" content from a project. (HE_ENGINE_CONTENT_EDITABLE=1
			// lifts this for engine-authoring builds.) Editing an existing
			// default and hitting Save is what creates a project override —
			// see ContentManager::saveAsset's redirect, not this menu.
			if (s_selectedRootKind == 1 && !ContentManager::isEngineContentDevMode())
			{
				ImGui::TextDisabled("Engine default assets are read-only here.");
				ImGui::TextDisabled("Open one in its editor tab and Save to");
				ImGui::TextDisabled("create a project-local override instead.");
			}
			else if (s_selectedRootKind == 2)
			{
				// Source root holds native C++ files, not engine assets — offer only
				// "C++ Class" (writes to this project's Source/). The .hasset list
				// would create engine assets under Source/, which doesn't belong here.
				// The Source root creates native classes, not engine assets — its own
				// scope, because the entry is about the C++ build, not the content tree.
				HE::Ed::Help::Scope helpScope("Source Root");
				ImGui::TextDisabled("Create C++");
				ImGui::Separator();
				if (EditorWidgets::menuItem("C++ Class"))
				{
					std::strncpy(s_cppClassName, "GameplayClass", sizeof(s_cppClassName) - 1);
					s_cppClassName[sizeof(s_cppClassName) - 1] = '\0';
					s_openCppClassPopup = true;
					ImGui::CloseCurrentPopup();
				}
			}
			else
			{
				ImGui::TextDisabled("Create Asset");
				ImGui::Separator();

				const std::string targetFolder = displayFolder ? displayFolder->fullPath
															   : contentFolder.fullPath;
				drawCreateAssetItems(targetFolder);
			}

			ImGui::EndPopup();
		}

		ImGui::EndChild();
    }

    ImGui::End();
#else
	(void)ctx; (void)tabSelectRequest; (void)openSceneGuarded;
#endif // HE_IMGUI_ENABLED
}

} // namespace ContentBrowserPanel
