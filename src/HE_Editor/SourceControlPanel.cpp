#include "SourceControlPanel.h"
#include "EditorApplication.h"    // AppContext
#include "EditorSettingsPanel.h"  // repo/remote setup lives in the Preferences tab
#include "EditorToolbar.h"     // shared toolbar look (Scene bar uses the same)
#include "EditorTheme.h"       // brand palette (emphasis text)
#include "EditorHelp.h"        // "Source Control Panel/<label>" scope for the tooltips
#include "EditorWidgets.h"     // primary/danger/cancel buttons
#include "GitController.h"
#include "ContentBrowserPanel.h"   // revealAsset: a changed file, shown in the Content Browser
#include "EditorAssetTypeCache.h" // asset type of a .hasset, for the type icon and filter

#include <Diagnostics/GlobalState.h>
#include <SourceControl/GitCli.h>
#include <SourceControl/GitProbe.h>
#include <SourceControl/RepoStatus.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace SourceControlPanel
{

#ifdef HE_IMGUI_ENABLED
namespace {

// Panel-local input state. A commit message is short-lived by design. (Repo
// init and remote/GitHub setup live in Preferences ▸ Editor ▸ Source Control now.)
char s_commitMessage[2048] = "";
bool s_prefsLoaded        = false;

// The settings the CONTROLLER acts on but the Preferences page owns: auto-push
// (read by requestCommitAll) and the background-fetch schedule (read by
// update()). Both have to be in the controller before the user visits that page,
// or the first commit of a session ignores auto-push and the fetch timer never
// starts for anyone who never opens Preferences.
//
// Called from the panel AND from the footer status, because the footer runs on
// every frame with a project open while the panel may never be opened at all —
// and a guest in a collaboration session, who sees no commit UI, still wants the
// ahead/behind counters to stay honest.
void ensureSettingsLoaded(GitController& git)
{
	if (s_prefsLoaded) return;
	s_prefsLoaded = true;
	GlobalState& gs = GlobalState::getInstance();
	git.autoPushAfterCommit = gs.getCustomConfigBool("GitAutoPushAfterCommit", false);
	git.autoFetch           = gs.getCustomConfigBool("GitAutoFetch", false);
	git.autoFetchMinutes    = gs.getCustomConfigInt("GitAutoFetchMinutes", 15);
}
// Changes as a folder tree (VS Code's tree mode) or a flat list. Persisted.
bool s_treeView       = true;
bool s_treeViewLoaded = false;
bool s_wantBranchesTab = false;   // "Manage branches…" in the branch popup
// Restore confirmation. Destructive enough to deserve a modal that states what
// will happen in full before it happens.
std::string s_restoreOid;
std::string s_restoreSubject;
// Branch creation. The start commit is empty when branching off the current
// state rather than off a specific commit in the history.
std::string s_branchFromOid;
std::string s_branchFromSubject;
char        s_branchName[128] = "";
bool        s_branchCheckout  = true;
bool        s_branchDialog    = false;

// Colours chosen so the meaning survives a glance: green for "will be
// committed", amber for "changed but not staged", grey for "git does not know
// about this", red for "needs a human".
ImVec4 colourFor(HE::Sc::FileState s)
{
	using FS = HE::Sc::FileState;
	switch (s)
	{
	case FS::Added:       return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
	case FS::Modified:    return ImVec4(1.00f, 0.78f, 0.35f, 1.0f);
	case FS::Deleted:     return ImVec4(0.95f, 0.50f, 0.45f, 1.0f);
	case FS::Renamed:
	case FS::Copied:      return ImVec4(0.60f, 0.75f, 1.00f, 1.0f);
	case FS::TypeChanged: return ImVec4(0.80f, 0.70f, 1.00f, 1.0f);
	case FS::Untracked:   return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
	case FS::Conflicted:  return ImVec4(1.00f, 0.40f, 0.40f, 1.0f);
	case FS::Ignored:
	case FS::Unmodified:  break;
	}
	return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

// One letter per state, the same alphabet git itself uses, so anyone who has
// read `git status` output already knows this.
const char* letterFor(HE::Sc::FileState s)
{
	using FS = HE::Sc::FileState;
	switch (s)
	{
	case FS::Added:       return "A";
	case FS::Modified:    return "M";
	case FS::Deleted:     return "D";
	case FS::Renamed:     return "R";
	case FS::Copied:      return "C";
	case FS::TypeChanged: return "T";
	case FS::Untracked:   return "?";
	case FS::Conflicted:  return "!";
	case FS::Ignored:     return "i";
	case FS::Unmodified:  break;
	}
	return " ";
}

// ImGui pitfall these two modals kept walking into: AlwaysAutoResize together
// with TextWrapped has no width to wrap AGAINST, so the window grows to the
// longest unbroken run of text and gets clipped by the viewport edge. Pinning
// the width turns the flag back into what it is wanted for — auto HEIGHT.
//
// Sized in multiples of the font size rather than pixels, so it follows the
// editor's font scale instead of assuming one.
void beginModalSizing(float widthInChars = 26.0f)
{
	const float w = ImGui::GetFontSize() * widthInChars;
	ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, FLT_MAX));
}

// Two equal buttons filling the row, so they scale with the dialog rather than
// spilling out of a font-scaled window at a hardcoded 120 px.
bool modalButtonRow(const char* confirmLabel, const char* cancelLabel,
                    bool confirmDisabled, bool& outCancelled, bool danger = false)
{
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float w       = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;

	// The way out first, the commitment second — reading order ends on the
	// action, and the filled button sits where the eye stops.
	outCancelled = EditorWidgets::cancelButton(cancelLabel, ImVec2(w, 0.0f));
	ImGui::SameLine();
	ImGui::BeginDisabled(confirmDisabled);
	const bool confirmed = danger
		? EditorWidgets::dangerButton(confirmLabel, ImVec2(w, 0.0f))
		: EditorWidgets::primaryButton(confirmLabel, ImVec2(w, 0.0f));
	ImGui::EndDisabled();
	return confirmed;
}

// ── What the Changes tab shows ───────────────────────────────────────────────
// Rows are grouped by what kind of asset they are, not just by git state: a freshly
// imported texture set adds dozens of "untracked" rows, and the two scene edits the
// user actually cares about vanish between them. The category comes from the file
// (extension, or the HAsset header for .hasset) and drives the type icon and the
// filter chips.
enum class Cat : int { Scene, Material, Texture, Mesh, Code, Audio, Other, Count };
constexpr int kCatCount = static_cast<int>(Cat::Count);

const char* catName(Cat c)
{
	switch (c)
	{
	case Cat::Scene:    return "Scenes";
	case Cat::Material: return "Materials";
	case Cat::Texture:  return "Textures";
	case Cat::Mesh:     return "Meshes";
	case Cat::Code:     return "Code";
	case Cat::Audio:    return "Audio";
	case Cat::Other:
	case Cat::Count:    break;
	}
	return "Other";
}

const char* catLetter(Cat c)
{
	switch (c)
	{
	case Cat::Scene:    return "S";
	case Cat::Material: return "M";
	case Cat::Texture:  return "T";
	case Cat::Mesh:     return "G";
	case Cat::Code:     return "C";
	case Cat::Audio:    return "A";
	case Cat::Other:
	case Cat::Count:    break;
	}
	return "·";
}

ImU32 catColour(Cat c)
{
	switch (c)
	{
	case Cat::Scene:    return IM_COL32(106,  90, 214, 255);
	case Cat::Material: return IM_COL32(194,  90, 138, 255);
	case Cat::Texture:  return IM_COL32( 58, 155, 143, 255);
	case Cat::Mesh:     return IM_COL32(199, 138,  44, 255);
	case Cat::Code:     return IM_COL32( 74, 120, 201, 255);
	case Cat::Audio:    return IM_COL32(150, 110, 190, 255);
	case Cat::Other:
	case Cat::Count:    break;
	}
	return IM_COL32(123, 132, 152, 255);
}

std::string lowerCopy(std::string s)
{
	for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return s;
}

Cat categoryOf(const std::string& absPath, const std::string& rel)
{
	const std::size_t dot = rel.rfind('.');
	const std::string ext = dot == std::string::npos ? std::string{} : lowerCopy(rel.substr(dot));
	if (ext == ".hescene") return Cat::Scene;
	if (ext == ".hasset")
	{
		switch (EditorAssetTypeCache::assetTypeOf(absPath))
		{
		case HE::AssetType::StaticMesh:
		case HE::AssetType::SkeletalMesh:      return Cat::Mesh;
		case HE::AssetType::Texture:           return Cat::Texture;
		case HE::AssetType::Material:
		case HE::AssetType::MaterialFunction:  return Cat::Material;
		case HE::AssetType::Scene:             return Cat::Scene;
		case HE::AssetType::Script:
		case HE::AssetType::HorizonCodeClass:  return Cat::Code;
		case HE::AssetType::Audio:             return Cat::Audio;
		default:                               return Cat::Other;
		}
	}
	static const char* const kCode[]    = { ".hcode", ".cpp", ".h", ".hpp", ".c", ".cc", ".lua", ".py", ".cs" };
	static const char* const kTexture[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".exr", ".hdr", ".tif", ".tiff", ".psd" };
	static const char* const kMesh[]    = { ".fbx", ".gltf", ".glb", ".obj", ".dae", ".blend" };
	static const char* const kAudio[]   = { ".wav", ".ogg", ".mp3", ".flac" };
	auto in = [&](const char* const* list, std::size_t n) {
		for (std::size_t i = 0; i < n; ++i) if (ext == list[i]) return true;
		return false;
	};
	if (in(kCode,    sizeof(kCode)    / sizeof(kCode[0])))    return Cat::Code;
	if (in(kTexture, sizeof(kTexture) / sizeof(kTexture[0]))) return Cat::Texture;
	if (in(kMesh,    sizeof(kMesh)    / sizeof(kMesh[0])))    return Cat::Mesh;
	if (in(kAudio,   sizeof(kAudio)   / sizeof(kAudio[0])))   return Cat::Audio;
	return Cat::Other;
}

struct ChangeRow
{
	std::string        path;        // repository-relative, as git keys it
	std::string        name;        // last component
	std::string        dir;         // everything before it, with the trailing slash
	std::string        origPath;    // renames
	HE::Sc::FileState  state = HE::Sc::FileState::Modified;   // the state THIS section shows
	Cat                cat   = Cat::Other;
};

// The status, regrouped for drawing. Rebuilt only when the status snapshot, the
// filter or the search changes: the lists are sorted and filtered, and the Content
// Browser's cached asset-type lookups are cheap but not free on thousands of rows.
struct Model
{
	std::vector<ChangeRow> conflicts, staged, unstaged, untracked;
	int         catCounts[kCatCount] = {};
	std::size_t total       = 0;     // every dirty file, whatever the filter hides
	std::size_t stagedTotal = 0;
	std::string root;

	const void*   source     = nullptr;
	std::uint64_t generation = ~0ull;
	int           filter     = -2;
	std::string   search;
};

Model s_model;
int   s_filter = -1;                 // a Cat, or -1 for all
char  s_search[96] = "";
bool  s_sectionOpen[4] = { true, true, true, true };   // conflicts, staged, changes, new

void rebuildModel(const HE::Sc::RepoStatus& st)
{
	using FS = HE::Sc::FileState;
	Model m;
	m.source     = &st;
	m.generation = st.generation;
	m.filter     = s_filter;
	m.search     = s_search;
	m.root       = st.root;
	const std::string needle = lowerCopy(s_search);

	for (const auto& [path, e] : st.files)
	{
		if (!e.dirty()) continue;            // ignored files are not changes
		const std::string abs = st.root + "/" + path;
		const Cat cat = categoryOf(abs, path);
		++m.catCounts[static_cast<int>(cat)];
		++m.total;
		if (e.staged()) ++m.stagedTotal;
		if (s_filter >= 0 && static_cast<int>(cat) != s_filter) continue;
		if (!needle.empty() && lowerCopy(path).find(needle) == std::string::npos) continue;

		ChangeRow r;
		r.path     = path;
		r.origPath = e.origPath;
		r.cat      = cat;
		const std::size_t slash = path.rfind('/');
		r.name = slash == std::string::npos ? path : path.substr(slash + 1);
		r.dir  = slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);

		auto add = [&](std::vector<ChangeRow>& into, FS state) { r.state = state; into.push_back(r); };
		if (e.conflicted())                 add(m.conflicts, FS::Conflicted);
		else if (e.worktree == FS::Untracked) add(m.untracked, FS::Untracked);
		else
		{
			if (e.staged()) add(m.staged, e.index);
			if (e.worktree != FS::Unmodified && e.worktree != FS::Ignored) add(m.unstaged, e.worktree);
		}
	}
	auto byPath = [](const ChangeRow& a, const ChangeRow& b) { return a.path < b.path; };
	std::sort(m.conflicts.begin(), m.conflicts.end(), byPath);
	std::sort(m.staged.begin(),    m.staged.end(),    byPath);
	std::sort(m.unstaged.begin(),  m.unstaged.end(),  byPath);
	std::sort(m.untracked.begin(), m.untracked.end(), byPath);
	s_model = std::move(m);
}

const Model& modelFor(const HE::Sc::RepoStatus& st)
{
	if (s_model.source != &st || s_model.generation != st.generation ||
	    s_model.filter != s_filter || s_model.search != s_search)
		rebuildModel(st);
	return s_model;
}

// ── Dialog state for the destructive / branching actions ─────────────────────
std::vector<std::string> s_discardPaths;     // non-empty = the confirm dialog is up
std::string              s_discardHeadline;
std::vector<std::string> s_discardSample;    // a few names, so the dialog is concrete
bool                     s_discardHasNew = false;

std::string s_switchTarget;                  // non-empty = the "you have changes" dialog is up

void askDiscard(const std::vector<ChangeRow>& rows, const std::string& headline)
{
	s_discardPaths.clear();
	s_discardSample.clear();
	s_discardHasNew = false;
	for (const ChangeRow& r : rows)
	{
		s_discardPaths.push_back(r.path);
		if (s_discardSample.size() < 8) s_discardSample.push_back(r.path);
		if (r.state == HE::Sc::FileState::Untracked || r.state == HE::Sc::FileState::Added)
			s_discardHasNew = true;
	}
	s_discardHeadline = headline;
}

// Switching replaces the working tree, so with local changes it asks first. A clean
// project switches at once.
void beginSwitch(GitController& git, const HE::Sc::RepoStatus& st, const std::string& name)
{
	if (name.empty() || name == st.branch) return;
	if (st.dirtyCount() == 0) git.requestSwitchBranch(name, /*stashFirst=*/false);
	else s_switchTarget = name;
}

// ── Small drawing helpers ────────────────────────────────────────────────────
// A text button without a frame: the label, a rounded wash on hover. Drawn at the
// cursor, which it advances past itself. Used for the actions that live INSIDE
// rows and section headers, where a framed button per row would drown the list.
bool inlineButton(const char* id, const char* label, ImU32 colour, const char* helpKey)
{
	const ImVec2 ts = ImGui::CalcTextSize(label);
	const ImVec2 sz(ts.x + 10.0f, ImGui::GetFrameHeight());
	const ImVec2 p  = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::InvisibleButton(id, sz);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (ImGui::IsItemHovered())
		dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(255, 255, 255, 28), 4.0f);
	dl->AddText(ImVec2(p.x + 5.0f, p.y + (sz.y - ts.y) * 0.5f), colour, label);
	if (helpKey) EditorWidgets::helpForKey(helpKey);
	return clicked;
}

struct RowResult
{
	bool toggle  = false;     // stage or unstage, depending on the section
	bool discard = false;
	bool reveal  = false;
	int  resolve = 0;         // conflicts: 1 = keep mine, 2 = take theirs
};

enum class RowKind { Conflict, Staged, Unstaged, Untracked };

// One file. Layout, left to right:
//   [ ☐ ] [ T ] name  folder/…………………………  (hover: ⌖ ⌫)  M
// The whole row is a Selectable (hover wash, double-click) and everything on it is
// drawn over it and hit-tested with InvisibleButtons that overlap it, so a click on
// the checkbox is a click on the checkbox and not also on the row.
RowResult drawRow(const ChangeRow& r, RowKind kind, bool showDir, bool mayWrite,
                  const std::string& root)
{
	namespace T = EditorToolbar;
	RowResult res;
	ImGui::PushID(static_cast<int>(kind));
	ImGui::PushID(r.path.c_str());

	ImDrawList*  dl = ImGui::GetWindowDrawList();
	const float  h  = ImGui::GetFrameHeight();
	const ImVec2 p  = ImGui::GetCursorScreenPos();
	const float  w  = ImGui::GetContentRegionAvail().x;
	const ImVec2 pmax(p.x + w, p.y + h);
	const float  pad = ImGui::GetStyle().FramePadding.x;

	ImGui::SetNextItemAllowOverlap();
	ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, h));
	const bool rowHovered = ImGui::IsMouseHoveringRect(p, pmax) &&
	                        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
	bool overButton = false;

	const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
	const ImU32 dimCol  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	const float cy      = p.y + h * 0.5f;
	float x = p.x + pad;

	// ── Checkbox ─────────────────────────────────────────────────────────────
	if (mayWrite && kind != RowKind::Conflict)
	{
		const bool checked = kind == RowKind::Staged;
		ImGui::SetCursorScreenPos(ImVec2(x, p.y));
		if (ImGui::InvisibleButton("##chk", ImVec2(h, h))) res.toggle = true;
		if (ImGui::IsItemHovered()) overButton = true;
		EditorWidgets::helpForKey(checked ? "sc.row.unstage" : "sc.row.stage");
		const float b = h * 0.56f;
		const ImVec2 a(x + (h - b) * 0.5f, cy - b * 0.5f);
		const ImVec2 z(a.x + b, a.y + b);
		if (checked)
		{
			dl->AddRectFilled(a, z, ImGui::GetColorU32(ImGuiCol_CheckMark), 3.0f);
			T::iconCheck(dl, ImVec2(x + h * 0.5f, cy), h * 0.8f, IM_COL32(255, 255, 255, 255));
		}
		else
		{
			dl->AddRect(a, z, ImGui::IsItemHovered() ? textCol : dimCol, 3.0f, 0, 1.5f);
		}
		x += h;
	}
	else
	{
		x += 4.0f;
	}

	// ── Type icon ────────────────────────────────────────────────────────────
	{
		const float s = h * 0.66f;
		const ImVec2 a(x, cy - s * 0.5f);
		dl->AddRectFilled(a, ImVec2(a.x + s, a.y + s), catColour(r.cat), 3.0f);
		const char* letter = catLetter(r.cat);
		const float fs = ImGui::GetFontSize() * 0.74f;
		const ImVec2 ls = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, letter);
		dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + (s - ls.x) * 0.5f, a.y + (s - ls.y) * 0.5f),
		            IM_COL32(255, 255, 255, 255), letter);
		x += s + 6.0f;
	}

	// ── Right edge: state letter, then the hover actions to its left ─────────
	const ImVec4 stateCol = colourFor(r.state);
	const char*  letter   = letterFor(r.state);
	const float  letterW  = ImGui::CalcTextSize(letter).x;
	float rightEdge = pmax.x - pad - letterW;
	dl->AddText(ImVec2(rightEdge, cy - ImGui::GetTextLineHeight() * 0.5f),
	            ImGui::GetColorU32(stateCol), letter);
	rightEdge -= 6.0f;

	if (r.state == HE::Sc::FileState::Conflicted)
	{
		if (mayWrite)
		{
			const float bw = ImGui::CalcTextSize("theirs").x + 10.0f;
			const float mw = ImGui::CalcTextSize("mine").x + 10.0f;
			ImGui::SetCursorScreenPos(ImVec2(rightEdge - bw, p.y));
			if (inlineButton("##theirs", "theirs", IM_COL32(240, 190, 90, 255), "sc.row.theirs")) res.resolve = 2;
			if (ImGui::IsItemHovered()) overButton = true;
			ImGui::SetCursorScreenPos(ImVec2(rightEdge - bw - mw, p.y));
			if (inlineButton("##mine", "mine", IM_COL32(240, 190, 90, 255), "sc.row.mine")) res.resolve = 1;
			if (ImGui::IsItemHovered()) overButton = true;
			rightEdge -= bw + mw + 4.0f;
		}
	}
	else if (rowHovered && mayWrite)
	{
		// Icon cells, each as wide as the row is tall.
		auto cell = [&](const char* id, void (*icon)(ImDrawList*, const ImVec2&, float, ImU32),
		                ImU32 col, const char* helpKey) {
			rightEdge -= h;
			ImGui::SetCursorScreenPos(ImVec2(rightEdge, p.y));
			const bool clicked = ImGui::InvisibleButton(id, ImVec2(h, h));
			const bool hov = ImGui::IsItemHovered();
			if (hov) { overButton = true; dl->AddRectFilled(ImVec2(rightEdge, p.y + 1), ImVec2(rightEdge + h, pmax.y - 1),
			                                                 IM_COL32(255, 255, 255, 30), 4.0f); }
			icon(dl, ImVec2(rightEdge + h * 0.5f, cy), h * 0.74f, hov ? col : dimCol);
			EditorWidgets::helpForKey(helpKey);
			return clicked;
		};
		if (cell("##discard", T::iconTrash, IM_COL32(240, 110, 100, 255),
		         kind == RowKind::Untracked ? "sc.row.delete" : "sc.row.discard"))
			res.discard = true;
	}
	if (rowHovered && r.state != HE::Sc::FileState::Deleted && r.state != HE::Sc::FileState::Conflicted)
	{
		auto cell = [&](const char* id, void (*icon)(ImDrawList*, const ImVec2&, float, ImU32),
		                const char* helpKey) {
			rightEdge -= h;
			ImGui::SetCursorScreenPos(ImVec2(rightEdge, p.y));
			const bool clicked = ImGui::InvisibleButton(id, ImVec2(h, h));
			const bool hov = ImGui::IsItemHovered();
			if (hov) { overButton = true; dl->AddRectFilled(ImVec2(rightEdge, p.y + 1), ImVec2(rightEdge + h, pmax.y - 1),
			                                                 IM_COL32(255, 255, 255, 30), 4.0f); }
			icon(dl, ImVec2(rightEdge + h * 0.5f, cy), h * 0.74f, hov ? textCol : dimCol);
			EditorWidgets::helpForKey(helpKey);
			return clicked;
		};
		if (cell("##reveal", T::iconFolder, "sc.row.reveal")) res.reveal = true;
	}

	// ── Name and folder ──────────────────────────────────────────────────────
	const ImVec2 nameSize = ImGui::CalcTextSize(r.name.c_str());
	const float  ty = cy - nameSize.y * 0.5f;
	const float  textRight = rightEdge - 4.0f;
	dl->PushClipRect(ImVec2(x, p.y), ImVec2(std::max(x, textRight), pmax.y), true);
	dl->AddText(ImVec2(x, ty), textCol, r.name.c_str());
	if (r.state == HE::Sc::FileState::Deleted)
		dl->AddLine(ImVec2(x, cy), ImVec2(x + nameSize.x, cy), dimCol, 1.0f);
	if (showDir && !r.dir.empty())
		dl->AddText(ImVec2(x + nameSize.x + 8.0f, ty), dimCol, r.dir.c_str());
	dl->PopClipRect();

	// ── Double-click reveals, hovering for a moment names the file in full ────
	if (doubleClicked && !overButton && r.state != HE::Sc::FileState::Deleted) res.reveal = true;

	static std::uint32_t s_hoverId   = 0;
	static double        s_hoverFrom = 0.0;
	const std::uint32_t myId = ImGui::GetID("##tip");
	if (rowHovered && !overButton)
	{
		if (s_hoverId != myId) { s_hoverId = myId; s_hoverFrom = ImGui::GetTime(); }
		if (ImGui::GetTime() - s_hoverFrom > 0.45 && ImGui::BeginTooltip())
		{
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
				if (!r.origPath.empty())
					ImGui::Text("%s\nrenamed from %s", r.path.c_str(), r.origPath.c_str());
				else
					ImGui::TextUnformatted(r.path.c_str());
			}
			ImGui::EndTooltip();
		}
	}
	else if (s_hoverId == myId)
	{
		s_hoverId = 0;
	}
	(void)root;

	// The selectable already moved the cursor on; the buttons moved it around. Put it
	// back and let one item of the row's size account for the line.
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(w, h));
	ImGui::PopID();
	ImGui::PopID();
	return res;
}

// A section heading: fold arrow, title, count, and up to two actions on the right.
enum class Bulk { None, Primary, Secondary };

Bulk drawSectionHeader(const char* title, std::size_t count, bool& open, bool showActions,
                       const char* primaryLabel, const char* primaryKey,
                       const char* secondaryLabel, const char* secondaryKey)
{
	Bulk result = Bulk::None;
	ImGui::PushID(title);
	ImDrawList*  dl = ImGui::GetWindowDrawList();
	const float  h  = ImGui::GetFrameHeight();
	const ImVec2 p  = ImGui::GetCursorScreenPos();
	const float  w  = ImGui::GetContentRegionAvail().x;
	const ImVec2 pmax(p.x + w, p.y + h);

	dl->AddRectFilled(p, pmax, ImGui::GetColorU32(ImGuiCol_Header, 0.45f), 3.0f);
	ImGui::SetNextItemAllowOverlap();
	if (ImGui::Selectable("##sec", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, h)))
		open = !open;

	const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	const float cy = p.y + h * 0.5f;
	// Fold arrow
	{
		const float a = h * 0.16f;
		const float ax = p.x + h * 0.5f;
		if (open) dl->AddTriangleFilled(ImVec2(ax - a, cy - a * 0.6f), ImVec2(ax + a, cy - a * 0.6f), ImVec2(ax, cy + a), dimCol);
		else      dl->AddTriangleFilled(ImVec2(ax - a * 0.6f, cy - a), ImVec2(ax - a * 0.6f, cy + a), ImVec2(ax + a, cy), dimCol);
	}
	float x = p.x + h;
	dl->AddText(ImVec2(x, cy - ImGui::GetTextLineHeight() * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), title);
	x += ImGui::CalcTextSize(title).x + 8.0f;
	{
		char n[24];
		std::snprintf(n, sizeof(n), "%zu", count);
		const ImVec2 ns = ImGui::CalcTextSize(n);
		dl->AddRectFilled(ImVec2(x, cy - ns.y * 0.5f - 1), ImVec2(x + ns.x + 10.0f, cy + ns.y * 0.5f + 1),
		                  IM_COL32(128, 136, 156, 70), 8.0f);
		dl->AddText(ImVec2(x + 5.0f, cy - ns.y * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), n);
	}

	if (showActions)
	{
		float rx = pmax.x - 2.0f;
		if (secondaryLabel)
		{
			const float bw = ImGui::CalcTextSize(secondaryLabel).x + 10.0f;
			rx -= bw;
			ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
			if (inlineButton("##secondary", secondaryLabel, IM_COL32(240, 110, 100, 255), secondaryKey))
				result = Bulk::Secondary;
		}
		if (primaryLabel)
		{
			const float bw = ImGui::CalcTextSize(primaryLabel).x + 10.0f;
			rx -= bw;
			ImGui::SetCursorScreenPos(ImVec2(rx, p.y));
			if (inlineButton("##primary", primaryLabel, ImGui::GetColorU32(ImGuiCol_Text), primaryKey))
				result = Bulk::Primary;
		}
	}
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(w, h));
	ImGui::PopID();
	return result;
}

struct DirNode
{
	std::map<std::string, DirNode> dirs;
	std::vector<const ChangeRow*>  files;
};

void insertRow(DirNode& root, const ChangeRow& r)
{
	DirNode* node = &root;
	std::size_t start = 0;
	while (true)
	{
		const std::size_t slash = r.path.find('/', start);
		if (slash == std::string::npos) { node->files.push_back(&r); return; }
		node = &node->dirs[r.path.substr(start, slash - start)];
		start = slash + 1;
	}
}

template <class Fn>
void drawDirNode(const std::string& name, const DirNode& node, Fn&& rowFn)
{
	// Single-child chains fold into one node ("Content/Meshes/Props"), the way VS Code
	// does it.
	const DirNode* n = &node;
	std::string label = name;
	while (n->files.empty() && n->dirs.size() == 1)
	{
		label += "/" + n->dirs.begin()->first;
		n = &n->dirs.begin()->second;
	}
	ImGui::PushID(label.c_str());
	if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
	{
		for (const auto& [sub, child] : n->dirs) drawDirNode(sub, child, rowFn);
		for (const ChangeRow* r : n->files) rowFn(*r);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

// One section's rows, as a tree or flat, with the results of every row gathered.
template <class Fn>
void drawRows(const std::vector<ChangeRow>& rows, Fn&& rowFn)
{
	if (s_treeView)
	{
		DirNode root;
		for (const ChangeRow& r : rows) insertRow(root, r);
		for (const auto& [name, sub] : root.dirs) drawDirNode(name, sub, rowFn);
		for (const ChangeRow* r : root.files) rowFn(*r);
	}
	else
	{
		// Clipped: a freshly imported project can have thousands of untracked rows.
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(rows.size()), ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);
		while (clip.Step())
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) rowFn(rows[static_cast<std::size_t>(i)]);
	}
}

std::vector<std::string> pathsOf(const std::vector<ChangeRow>& rows)
{
	std::vector<std::string> out;
	out.reserve(rows.size());
	for (const ChangeRow& r : rows) out.push_back(r.path);
	return out;
}

// ── Commit box ───────────────────────────────────────────────────────────────
void drawCommitBox(GitController& git, AppContext& ctx, const HE::Sc::RepoStatus& st, const Model& m)
{
	const bool identityOk = !ctx.gitProbe || ctx.gitProbe->identityConfigured;
	if (!identityOk)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
		ImGui::TextWrapped("git has no user.name / user.email configured — commits "
		                   "will fail until they are set.");
		ImGui::PopStyleColor();
	}

	const float lineH = ImGui::GetTextLineHeight();
	const ImVec2 boxPos = ImGui::GetCursorScreenPos();
	ImGui::InputTextMultiline("##commitmsg", s_commitMessage, sizeof(s_commitMessage),
	                          ImVec2(-FLT_MIN, lineH * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f));
	if (s_commitMessage[0] == '\0' && !ImGui::IsItemActive())
	{
		ImGui::GetWindowDrawList()->AddText(
			ImVec2(boxPos.x + ImGui::GetStyle().FramePadding.x, boxPos.y + ImGui::GetStyle().FramePadding.y),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), "Message for this commit");
	}

	const std::size_t dirty  = m.total;
	const std::size_t staged = m.stagedTotal;
	const bool conflicts = st.hasConflicts();
	const bool hasMsg    = s_commitMessage[0] != '\0';
	const bool hasRemote = !git.remoteUrl().empty();
	const bool idle      = !git.busy();
	const auto& commits  = git.recentCommits();
	// A commit that is already on the remote is not rewritten from here: that is a
	// force-push waiting to happen to whoever else pulled it.
	const bool canAmend = !commits.empty() && !st.initialCommit && (commits.front().unpushed || !hasRemote);

	const bool canCommit = idle && hasMsg && dirty > 0 && !conflicts;

	char label[64];
	if (dirty == 0)       std::snprintf(label, sizeof(label), "Nothing to commit");
	else if (staged > 0)  std::snprintf(label, sizeof(label), "Commit %zu staged", staged);
	else if (dirty == 1)  std::snprintf(label, sizeof(label), "Commit 1 change");
	else                  std::snprintf(label, sizeof(label), "Commit all %zu changes", dirty);

	const float arrowW = ImGui::GetFrameHeight();
	const float mainW  = std::max(40.0f, ImGui::GetContentRegionAvail().x - arrowW - 2.0f);
	ImGui::BeginDisabled(!canCommit);
	if (EditorWidgets::primaryButton(label, ImVec2(mainW, 0.0f)))
	{
		// Staged files = exactly those. Nothing staged = everything, the way the button
		// always worked, so the one-click habit costs nothing.
		if (staged > 0) git.requestCommitStaged(s_commitMessage, git.autoPushAfterCommit, false);
		else            git.requestCommitAll(s_commitMessage);
		s_commitMessage[0] = '\0';
	}
	ImGui::EndDisabled();
	EditorWidgets::helpForKey("sc.commit");
	ImGui::SameLine(0.0f, 2.0f);
	if (ImGui::ArrowButton("##commitmore", ImGuiDir_Down)) ImGui::OpenPopup("##commitMenu");
	EditorWidgets::helpForKey("sc.commit.more");
	if (ImGui::BeginPopup("##commitMenu"))
	{
		HE::Ed::Help::Scope helpScope("Source Control Panel");
		if (EditorWidgets::menuItem("Commit & Push", nullptr, false,
		                            canCommit && hasRemote))
		{
			if (staged > 0) git.requestCommitStaged(s_commitMessage, true, false);
			else            git.requestCommitAll(s_commitMessage, /*forcePush=*/true);
			s_commitMessage[0] = '\0';
		}
		if (EditorWidgets::menuItem("Commit everything (stage all first)", nullptr, false,
		                            canCommit && staged > 0))
		{
			git.requestCommitAll(s_commitMessage);
			s_commitMessage[0] = '\0';
		}
		if (EditorWidgets::menuItem("Amend last commit", nullptr, false,
		                            idle && canAmend && !conflicts && (staged > 0 || hasMsg)))
		{
			git.requestCommitStaged(s_commitMessage, false, true);
			s_commitMessage[0] = '\0';
		}
		ImGui::EndPopup();
	}

	// One quiet line saying why the button is not live, or what it will do.
	if (conflicts)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
		ImGui::TextWrapped("Conflicts must be resolved before committing.");
		ImGui::PopStyleColor();
	}
	else if (dirty > 0 && !hasMsg)
	{
		ImGui::TextDisabled("Write a message to commit.");
	}
	else if (dirty > 0 && staged == 0)
	{
		ImGui::TextDisabled("Nothing is staged, so this commits everything. Tick files to commit only those.");
	}
	else if (st.initialCommit && !hasRemote)
	{
		ImGui::TextDisabled("No remote yet — set one up in Preferences \xe2\x96\xb8 Source Control.");
	}
}

// ── Filter chips and search ──────────────────────────────────────────────────
void drawFilterBar(const Model& m)
{
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::InputTextWithHint("##scsearch", "Filter changes…", s_search, sizeof(s_search)))
	{
		// model rebuilds next draw: the cache key includes the text
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 1.0f));
	bool first = true;
	auto chip = [&](const char* name, int value, int count) {
		char text[48];
		if (count >= 0) std::snprintf(text, sizeof(text), "%s %d", name, count);
		else            std::snprintf(text, sizeof(text), "%s", name);
		const float w = ImGui::CalcTextSize(text).x + 16.0f;
		if (!first)
		{
			if (ImGui::GetContentRegionAvail().x > w + 4.0f) ImGui::SameLine(0.0f, 4.0f);
		}
		first = false;
		const bool on = s_filter == value;
		if (on)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		}
		if (ImGui::SmallButton(text)) s_filter = value;
		ImGui::PopStyleColor(on ? 1 : 2);
		EditorWidgets::helpForKey("sc.filter");
	};
	chip("All", -1, static_cast<int>(m.total));
	for (int c = 0; c < kCatCount; ++c)
		if (m.catCounts[c] > 0) chip(catName(static_cast<Cat>(c)), c, m.catCounts[c]);
	ImGui::PopStyleVar(2);
	// A filter whose category has emptied out would hide everything with no way to tell.
	if (s_filter >= 0 && m.catCounts[s_filter] == 0) s_filter = -1;
}

// ── The Changes tab ──────────────────────────────────────────────────────────
void drawChangesTab(GitController& git, AppContext& ctx, const HE::Sc::RepoStatus& st, bool mayWrite)
{
	const Model& m = modelFor(st);

	if (mayWrite) drawCommitBox(git, ctx, st, m);
	if (m.total > 0)
	{
		ImGui::Spacing();
		drawFilterBar(m);
	}

	if (!ImGui::BeginChild("##changes", ImVec2(0.0f, 0.0f), false))
	{
		ImGui::EndChild();
		return;
	}
	EditorWidgets::WrapText wrap;

	if (m.total == 0)
	{
		ImGui::TextDisabled("Nothing has changed.");
	}
	else if (m.conflicts.empty() && m.staged.empty() && m.unstaged.empty() && m.untracked.empty())
	{
		ImGui::TextDisabled("No changes match the filter.");
	}
	else
	{
		const bool showDir = !s_treeView;
		std::vector<std::string> toStage, toUnstage;

		auto section = [&](int idx, const char* title, const std::vector<ChangeRow>& rows,
		                   RowKind kind, const char* primaryLabel, const char* primaryKey,
		                   const char* secondaryLabel, const char* secondaryKey) {
			if (rows.empty()) return;
			const Bulk b = drawSectionHeader(title, rows.size(), s_sectionOpen[idx], mayWrite,
			                                 primaryLabel, primaryKey, secondaryLabel, secondaryKey);
			if (b == Bulk::Primary)
			{
				auto paths = pathsOf(rows);
				if (kind == RowKind::Staged) git.requestUnstage(std::move(paths));
				else                         git.requestStage(std::move(paths));
			}
			else if (b == Bulk::Secondary)
			{
				askDiscard(rows, kind == RowKind::Untracked ? "Delete these new files?" : "Discard these changes?");
			}
			if (!s_sectionOpen[idx]) return;
			ImGui::PushID(idx);
			drawRows(rows, [&](const ChangeRow& r) {
				const RowResult res = drawRow(r, kind, showDir, mayWrite, m.root);
				if (res.toggle)
				{
					if (kind == RowKind::Staged) toUnstage.push_back(r.path);
					else                         toStage.push_back(r.path);
				}
				if (res.discard)
					askDiscard({ r }, kind == RowKind::Untracked ? "Delete this new file?" : "Discard this change?");
				if (res.reveal) ContentBrowserPanel::revealAsset(m.root + "/" + r.path);
				if (res.resolve != 0) git.requestResolveConflict(r.path, res.resolve == 1);
			});
			ImGui::PopID();
			ImGui::Spacing();
		};

		// Conflicts first - they block a commit entirely, so burying them under a long
		// list of ordinary changes would be exactly wrong.
		section(0, "Conflicts", m.conflicts, RowKind::Conflict, nullptr, nullptr, nullptr, nullptr);
		section(1, "Staged",    m.staged,    RowKind::Staged,   "Unstage all", "sc.unstage_all", nullptr, nullptr);
		section(2, "Changes",   m.unstaged,  RowKind::Unstaged, "Stage all",   "sc.stage_all",   "Discard all", "sc.discard_all");
		section(3, "New files", m.untracked, RowKind::Untracked,"Stage all",   "sc.stage_all",   "Delete all",  "sc.delete_all");

		if (!toStage.empty())   git.requestStage(std::move(toStage));
		if (!toUnstage.empty()) git.requestUnstage(std::move(toUnstage));
	}
	ImGui::EndChild();
}

// ── The History tab ──────────────────────────────────────────────────────────
std::set<std::string> s_expandedCommits;

void drawHistoryTab(GitController& git, const HE::Sc::RepoStatus& st, bool mayWrite)
{
	if (!ImGui::BeginChild("##history", ImVec2(0.0f, 0.0f), false))
	{
		ImGui::EndChild();
		return;
	}
	EditorWidgets::WrapText wrap;

	const auto& commits = git.recentCommits();
	if (commits.empty()) ImGui::TextDisabled("No commits yet.");

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float gut = ImGui::GetFontSize() * 1.4f;
	const float lineH = ImGui::GetTextLineHeight();
	const ImU32 lineCol = ImGui::GetColorU32(ImGuiCol_Border);
	const ImU32 dimCol  = ImGui::GetColorU32(ImGuiCol_TextDisabled);

	for (std::size_t i = 0; i < commits.size(); ++i)
	{
		const auto& c = commits[i];
		ImGui::PushID(c.shortOid.c_str());
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float  w = ImGui::GetContentRegionAvail().x;
		const float  rowH = lineH * 2.0f + 4.0f;
		const bool expanded = s_expandedCommits.count(c.shortOid) != 0;

		ImGui::SetNextItemAllowOverlap();
		if (ImGui::Selectable("##commit", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, rowH)))
		{
			if (expanded) s_expandedCommits.erase(c.shortOid);
			else
			{
				s_expandedCommits.insert(c.shortOid);
				if (!git.commitFiles(c.shortOid)) git.requestCommitFiles(c.shortOid);
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s — %s\n(click to list its files, right-click for more)", c.author.c_str(), c.relTime.c_str());

		if (ImGui::BeginPopupContextItem("##commitctx"))
		{
			// Branching is non-destructive as long as it only writes a ref, so it
			// stays available with a dirty tree - the dialog's "switch to it" is what
			// the guard applies to.
			ImGui::BeginDisabled(git.busy() || !mayWrite);
			if (EditorWidgets::menuItem("Create branch from this commit…"))
			{
				s_branchFromOid     = c.shortOid;
				s_branchFromSubject = c.subject;
				s_branchName[0]     = '\0';
				s_branchCheckout    = st.dirtyCount() == 0;
				s_branchDialog      = true;
			}
			ImGui::EndDisabled();

			ImGui::BeginDisabled(git.busy() || st.dirtyCount() != 0 || !mayWrite);
			if (EditorWidgets::menuItem("Restore project to this commit…"))
			{
				s_restoreOid     = c.shortOid;
				s_restoreSubject = c.subject;
			}
			ImGui::EndDisabled();
			if (st.dirtyCount() != 0)
			{
				EditorWidgets::WrapText menuWrap(ImGui::GetFontSize() * 26.0f);
				ImGui::TextDisabled("Restoring needs a clean project — commit or "
				                    "discard your changes first.");
			}
			ImGui::EndPopup();
		}

		// Text over the selectable
		const float tx = p.x + gut;
		dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + w, p.y + rowH), true);
		dl->AddText(ImVec2(tx, p.y + 2.0f), ImGui::GetColorU32(ImGuiCol_Text), c.subject.c_str());
		char meta[256];
		std::snprintf(meta, sizeof(meta), "%s · %s · %s", c.shortOid.c_str(), c.author.c_str(), c.relTime.c_str());
		dl->AddText(ImVec2(tx, p.y + 2.0f + lineH), dimCol, meta);
		if (c.unpushed)
		{
			const char* tag = "not pushed";
			const float tw = ImGui::CalcTextSize(tag).x;
			dl->AddText(ImVec2(p.x + w - tw - 6.0f, p.y + 2.0f), IM_COL32(110, 200, 140, 255), tag);
		}
		dl->PopClipRect();

		// The files of an expanded commit, indented under it
		if (expanded)
		{
			ImGui::Indent(gut + 6.0f);
			if (const auto* files = git.commitFiles(c.shortOid))
			{
				constexpr std::size_t kShown = 200;
				for (std::size_t f = 0; f < files->size() && f < kShown; ++f)
				{
					const auto& cf = (*files)[f];
					const HE::Sc::FileState fs =
						cf.state == 'A' ? HE::Sc::FileState::Added :
						cf.state == 'D' ? HE::Sc::FileState::Deleted :
						cf.state == 'R' ? HE::Sc::FileState::Renamed :
						cf.state == 'C' ? HE::Sc::FileState::Copied :
						cf.state == 'T' ? HE::Sc::FileState::TypeChanged : HE::Sc::FileState::Modified;
					ImGui::TextColored(colourFor(fs), "%s", letterFor(fs));
					ImGui::SameLine();
					if (cf.origPath.empty()) ImGui::TextUnformatted(cf.path.c_str());
					else ImGui::Text("%s ← %s", cf.path.c_str(), cf.origPath.c_str());
				}
				if (files->size() > kShown) ImGui::TextDisabled("… and %zu more", files->size() - kShown);
				if (files->empty()) ImGui::TextDisabled("No files.");
			}
			else
			{
				ImGui::TextDisabled("Loading…");
			}
			ImGui::Unindent(gut + 6.0f);
		}

		// The strand: a line down the gutter, a dot per commit
		const float y1 = ImGui::GetCursorScreenPos().y;
		const float gx = p.x + gut * 0.5f;
		const float dotY = p.y + 2.0f + lineH * 0.5f;
		dl->AddLine(ImVec2(gx, i == 0 ? dotY : p.y), ImVec2(gx, y1), lineCol, 2.0f);
		if (c.unpushed) dl->AddCircleFilled(ImVec2(gx, dotY), 4.5f, IM_COL32(110, 200, 140, 255));
		else            dl->AddCircle(ImVec2(gx, dotY), 4.0f, ImGui::GetColorU32(ImGuiCol_CheckMark), 0, 2.0f);
		ImGui::PopID();
	}
	ImGui::EndChild();
}

// ── The Branches tab ─────────────────────────────────────────────────────────
void drawBranchesTab(GitController& git, const HE::Sc::RepoStatus& st, bool mayWrite)
{
	if (!ImGui::BeginChild("##branches", ImVec2(0.0f, 0.0f), false))
	{
		ImGui::EndChild();
		return;
	}
	EditorWidgets::WrapText wrap;
	const bool idle = !git.busy();

	ImGui::BeginDisabled(!(mayWrite && idle && !st.initialCommit));
	if (EditorWidgets::button("New branch…"))
	{
		s_branchFromOid.clear();
		s_branchFromSubject.clear();
		s_branchName[0]  = '\0';
		s_branchCheckout = st.dirtyCount() == 0;
		s_branchDialog   = true;
	}
	ImGui::EndDisabled();
	if (st.initialCommit) { ImGui::SameLine(); ImGui::TextDisabled("Make the first commit first."); }

	ImGui::SeparatorText("On this computer");
	if (git.branches().empty()) ImGui::TextDisabled("(none yet)");
	for (const std::string& b : git.branches())
	{
		ImGui::PushID(b.c_str());
		const bool current = b == st.branch;
		if (current) ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.55f, 1.0f), "\xe2\x97\x8f");
		else         ImGui::TextDisabled(" ");
		ImGui::SameLine();
		ImGui::TextUnformatted(b.c_str());
		if (current && (st.ahead > 0 || st.behind > 0))
		{
			ImGui::SameLine();
			ImGui::TextDisabled("\xe2\x86\x93%d \xe2\x86\x91%d", st.behind, st.ahead);
		}
		if (!current && mayWrite)
		{
			ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Switch").x - 16.0f);
			ImGui::BeginDisabled(!idle);
			if (EditorWidgets::smallButton("Switch")) beginSwitch(git, st, b);
			ImGui::EndDisabled();
		}
		ImGui::PopID();
	}

	// Branches the remote has that this computer does not
	std::vector<const std::string*> remoteOnly;
	for (const std::string& rb : git.remoteBranches())
	{
		const std::size_t slash = rb.find('/');
		const std::string shortName = slash == std::string::npos ? rb : rb.substr(slash + 1);
		const auto& locals = git.branches();
		if (std::find(locals.begin(), locals.end(), shortName) == locals.end()) remoteOnly.push_back(&rb);
	}
	if (!remoteOnly.empty())
	{
		ImGui::SeparatorText("Only on the server");
		for (const std::string* rb : remoteOnly)
		{
			ImGui::PushID(rb->c_str());
			ImGui::TextUnformatted(rb->c_str());
			if (mayWrite)
			{
				ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Check out").x - 16.0f);
				ImGui::BeginDisabled(!idle);
				if (EditorWidgets::smallButton("Check out")) beginSwitch(git, st, *rb);
				ImGui::EndDisabled();
			}
			ImGui::PopID();
		}
	}

	if (!git.stashes().empty())
	{
		ImGui::SeparatorText("Stashed changes");
		for (const std::string& s : git.stashes()) ImGui::TextDisabled("%s", s.c_str());
		if (mayWrite)
		{
			ImGui::BeginDisabled(!idle);
			if (EditorWidgets::button("Bring back stashed changes")) git.requestStashPop();
			ImGui::EndDisabled();
		}
	}
	ImGui::EndChild();
}

// ── Header bar ───────────────────────────────────────────────────────────────
// Built from the same primitives as the Scene toolbar (EditorToolbar), so the
// two read as one editor rather than as two applications sharing a window:
//
//   [ ⑂ main ]   [ ⟳ │ ☁ │ ↓ 2 │ ↑ 1 ]                    [ tree │ ⚙ ]
//    ← where you are   ← what to do about the remote        ← how it looks
//
// It shrinks in defined steps rather than overflowing — first the sync labels
// go, then the view toggle, then the branch name — and everything that gets
// dropped stays reachable in the ⚙ popup, which is never dropped.

void drawBranchPopup(GitController& git, const HE::Sc::RepoStatus& st)
{
	// "Source Control Panel", not "Source Control": that scope belongs to the
	// Preferences page that INSTALLS git, and this is the window that uses it.
	HE::Ed::Help::Scope helpScope("Source Control Panel");
	const bool mayWrite = git.mayModify();
	ImGui::TextDisabled("Switch branch");
	ImGui::Separator();
	if (git.branches().empty())
	{
		ImGui::TextDisabled("(none yet)");
	}
	else
	{
		// A clean project switches at once; with changes it asks (beginSwitch).
		for (const std::string& b : git.branches())
			if (EditorWidgets::menuItem(b.c_str(), nullptr, b == st.branch, mayWrite && !git.busy()))
				beginSwitch(git, st, b);
	}
	ImGui::Separator();
	if (EditorWidgets::menuItem("Manage branches…")) s_wantBranchesTab = true;
	ImGui::BeginDisabled(git.busy() || st.initialCommit || !mayWrite);
	if (EditorWidgets::menuItem("New branch…"))
	{
		s_branchFromOid.clear();          // empty start = branch off HEAD
		s_branchFromSubject.clear();
		s_branchName[0]  = '\0';
		s_branchCheckout = st.dirtyCount() == 0;
		s_branchDialog   = true;
	}
	ImGui::EndDisabled();
	if (st.initialCommit)
		ImGui::TextDisabled("Make the first commit first.");
}

void drawOptionsPopup(GitController& git, const HE::Sc::RepoStatus& st)
{
	// Wrapped at a fixed column rather than at the window edge, which is the one
	// case where the panel-wide rule does not apply: a popup sizes itself to its
	// contents, so "wrap where the window ends" is circular — the window ends
	// wherever the longest line put it. A remote URL is one unbroken run of
	// eighty characters, and the popup grew to fit it, right across the editor.
	// Twenty-six ems is the same measure the dialogs in this file are pinned to,
	// so the two look like they belong to one program.
	//
	// The scope is the function body, which ends before the caller's EndPopup() —
	// the pop has to happen while this popup is still the current window.
	EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 26.0f);
	HE::Ed::Help::Scope helpScope("Source Control Panel");

	ImGui::TextDisabled("Changes");
	ImGui::Separator();
	if (EditorWidgets::menuItem("Tree view", nullptr, s_treeView))
	{
		s_treeView = true;
		GlobalState::getInstance().setCustomConfigEntry("GitChangesTreeView", true);
	}
	if (EditorWidgets::menuItem("Flat list", nullptr, !s_treeView))
	{
		s_treeView = false;
		GlobalState::getInstance().setCustomConfigEntry("GitChangesTreeView", false);
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Remote");
	ImGui::Separator();
	if (git.remoteUrl().empty())
	{
		ImGui::TextDisabled("None configured");
	}
	else
	{
		ImGui::TextDisabled("origin: %s", git.remoteUrl().c_str());
		if (!st.upstream.empty())
			ImGui::TextDisabled("tracking %s", st.upstream.c_str());
		// The schedule itself is a preference, not a per-panel switch — this is
		// only the readout, so the panel never disagrees with the settings page.
		ImGui::TextDisabled(git.autoFetch
			? "Fetching automatically every %d min"
			: "Automatic fetching is off", std::max(GitController::kMinFetchMinutes,
			                                        git.autoFetchMinutes));
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (EditorWidgets::menuItem("Open source-control settings…"))
		EditorSettingsPanel::requestOpen(EditorSettingsPanel::Page::Repository);
}

void drawHeaderBar(GitController& git, const HE::Sc::RepoStatus& st)
{
	namespace T = EditorToolbar;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  barW   = ImGui::GetContentRegionAvail().x;
	const T::Metrics m  = T::metrics(origin.y);

	const bool conflicts = st.hasConflicts();
	// A repository with unresolved conflicts is not in a state to be synced, and
	// that is worth seeing before reading a single filename — so it washes the
	// whole strip, exactly as play mode does in the Scene bar.
	T::bar(origin, barW, m,
	       conflicts ? T::kBadWell : 0u,
	       conflicts ? T::kBad : T::kBarLine,
	       conflicts ? 2.0f : 1.0f);

	const bool mayWrite  = git.mayModify();
	const bool hasRemote = !git.remoteUrl().empty();
	const bool idle      = !git.busy();

	const char* branchLabel = st.detached ? "detached"
	                        : st.branch.empty() ? "(no branch)"
	                                            : st.branch.c_str();

	char aheadTxt[16], behindTxt[16];
	std::snprintf(aheadTxt,  sizeof(aheadTxt),  "%d", st.ahead);
	std::snprintf(behindTxt, sizeof(behindTxt), "%d", st.behind);

	// ── Fit ─────────────────────────────────────────────────────────────────
	// Measured before anything is drawn, so the shrink steps are decided once
	// and the row cannot reflow halfway through.
	const float branchW = T::cellWidth(m, branchLabel);
	auto syncWidth = [&](bool labels)
	{
		float w = T::kWellPad * 2.0f + m.cell;                        // refresh
		if (hasRemote) w += T::kSegGap + m.cell;                      // fetch
		if (hasRemote && mayWrite)
		{
			w += T::kSegGap + (labels ? T::cellWidth(m, behindTxt) : m.cell);
			w += T::kSegGap + (labels ? T::cellWidth(m, aheadTxt)  : m.cell);
		}
		return w;
	};
	auto rightWidth = [&](bool viewToggle)
	{
		float w = T::kWellPad * 2.0f + m.cell;                        // options
		if (viewToggle) w += T::kSegGap + m.cell;
		return w;
	};

	bool labels = true, viewToggle = true, branchName = true;
	auto leftWidth = [&](bool name)
	{
		return T::kWellPad * 2.0f + (name ? branchW : m.cell);
	};
	auto fits = [&] {
		return leftWidth(branchName) + T::kGroupGap + syncWidth(labels) +
		       T::kGroupGap + rightWidth(viewToggle) + T::kEdgeGap * 2.0f <= barW;
	};
	if (!fits()) labels     = false;
	if (!fits()) viewToggle = false;
	if (!fits()) branchName = false;

	// ── Left: where you are ─────────────────────────────────────────────────
	float x = origin.x + T::kEdgeGap;
	{
		const float w = leftWidth(branchName);
		T::well(m, x, w);
		if (T::cell(m, x + T::kWellPad, w - T::kWellPad * 2.0f, "##branch",
		            T::iconBranch, branchName ? branchLabel : nullptr,
		            false, true,
		            st.detached ? "Detached HEAD — commits here belong to no branch"
		                        : "Branch. Click for the branch list."))
		{
			ImGui::OpenPopup("##branchPopup");
		}
		if (ImGui::BeginPopup("##branchPopup")) { drawBranchPopup(git, st); ImGui::EndPopup(); }
		x += w + T::kGroupGap;
	}

	// ── Middle: the remote ──────────────────────────────────────────────────
	{
		const float w = syncWidth(labels);
		T::well(m, x, w);
		float cx = x + T::kWellPad;

		if (T::cell(m, cx, m.cell, "##refresh", T::iconRefresh, nullptr, false, idle,
		            "Refresh status"))
		{
			git.requestRefresh();
		}
		cx += m.cell + T::kSegGap;

		if (hasRemote)
		{
			if (T::cell(m, cx, m.cell, "##fetch", T::iconCloud, nullptr, false, idle,
			            "Fetch — update what the remote has, without changing anything "
			            "here.\nAutomatic fetching is set up in Preferences \xe2\x96\xb8 "
			            "Source Control."))
			{
				git.requestFetch();
			}
			cx += m.cell + T::kSegGap;
		}

		if (hasRemote && mayWrite)
		{
			// Ahead and behind ARE the pull and push buttons: the count is the
			// reason you would press them, so putting it anywhere else means
			// reading one thing and clicking another.
			const float pullW = labels ? T::cellWidth(m, behindTxt) : m.cell;
			const bool  canPull = idle && !st.initialCommit;
			if (T::cellTinted(m, cx, pullW, "##pull", T::iconArrowDown,
			                  labels ? behindTxt : nullptr,
			                  st.behind > 0 ? T::kWarn : T::kFg, canPull,
			                  st.behind > 0
			                      ? "Pull — fast-forward only; a diverged branch is "
			                        "reported, never auto-merged."
			                      : "Pull (nothing to pull)"))
			{
				git.requestPull();
			}
			cx += pullW + T::kSegGap;

			const float pushW = labels ? T::cellWidth(m, aheadTxt) : m.cell;
			if (T::cellTinted(m, cx, pushW, "##push", T::iconArrowUp,
			                  labels ? aheadTxt : nullptr,
			                  st.ahead > 0 ? T::kGood : T::kFg, canPull,
			                  st.initialCommit ? "Make the first commit before pushing"
			                                   : "Push"))
			{
				git.requestPush();
			}
		}
		x += w + T::kGroupGap;
	}

	// ── Right: how it looks ─────────────────────────────────────────────────
	{
		const float w  = rightWidth(viewToggle);
		const float rx = origin.x + barW - T::kEdgeGap - w;
		T::well(m, rx, w);
		float cx = rx + T::kWellPad;

		if (viewToggle)
		{
			if (T::cell(m, cx, m.cell, "##view", s_treeView ? T::iconTree : T::iconList,
			            nullptr, false, true,
			            s_treeView ? "Showing changes as a folder tree — click for a flat list"
			                       : "Showing changes as a flat list — click for a folder tree"))
			{
				s_treeView = !s_treeView;
				GlobalState::getInstance().setCustomConfigEntry("GitChangesTreeView", s_treeView);
			}
			cx += m.cell + T::kSegGap;
		}

		if (T::cell(m, cx, m.cell, "##scopts", T::iconGear, nullptr, false, true, "Options"))
			ImGui::OpenPopup("##scOptions");
		if (ImGui::BeginPopup("##scOptions")) { drawOptionsPopup(git, st); ImGui::EndPopup(); }
	}

	// Busy and conflicts, on the strip itself rather than as a line under it —
	// they are states of the bar's own controls, and a line that appears and
	// disappears shoves the whole panel up and down.
	if (!idle || conflicts)
	{
		const char* note = conflicts ? "conflicts" : "working…";
		const float noteW = ImGui::CalcTextSize(note).x;
		const float slot  = origin.x + barW - T::kEdgeGap - rightWidth(viewToggle)
		                  - T::kGroupGap - noteW;
		if (slot > x)
		{
			ImGui::GetWindowDrawList()->AddText(
				ImVec2(std::floor(slot), std::floor(m.cy - ImGui::GetFontSize() * 0.5f)),
				conflicts ? T::kBad : T::kFgDim, note);
		}
	}

	// Hand the rest of the window to the panel body, exactly as the Scene bar
	// hands it to the viewport image.
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + m.bar));
}

} // namespace
#endif // HE_IMGUI_ENABLED

void DrawSourceControlWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
	GitController* git = ctx.git;
	// The controller only polls often while the panel is on screen, so it has to
	// be told even on the frame where the window is closed. The Preferences tab's
	// Source Control pages count as "on screen" too — they show the same status.
	if (git)
	{
		git->setPanelVisible(open || EditorSettingsPanel::sourceControlPageActive());
		ensureSettingsLoaded(*git);
	}
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(460.0f, 620.0f), ImGuiCond_FirstUseEver);
	// Passing &open lets the window's own X clear the View-menu toggle.
	if (!ImGui::Begin("Source Control", &open))
	{
		ImGui::End();
		return;
	}
	HE::Ed::Help::Scope helpScope("Source Control Panel");

	if (!git)
	{
		ImGui::TextDisabled("Source control is unavailable in this build.");
		ImGui::End();
		return;
	}

	if (!ctx.projectLoaded)
	{
		ImGui::TextWrapped("Open a project to see its source-control status.");
		ImGui::End();
		return;
	}

	const HE::Sc::RepoStatus& st = git->status();

	if (!git->isRepo())
	{
		ImGui::TextWrapped("This project is not in a git repository yet.");
		ImGui::Spacing();

		if (git->blockedByCollabSession())
		{
			ImGui::TextDisabled("The session host manages source control for this "
			                    "project.");
		}
		else
		{
			ImGui::TextWrapped("Repository setup (init, remote, GitHub sign-in) lives "
			                   "in Preferences \xe2\x96\xb8 Source Control.");
			ImGui::Spacing();
			if (EditorWidgets::button("Set up in Preferences…", ImVec2(240.0f, 0.0f)))
				EditorSettingsPanel::requestOpen(EditorSettingsPanel::Page::Repository);
		}

		if (!git->lastError().empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
			ImGui::TextWrapped("%s", git->lastError().c_str());
			ImGui::PopStyleColor();
		}
		if (git->busy()) { ImGui::Spacing(); ImGui::TextDisabled("Working…"); }
		ImGui::End();
		return;
	}

	// ── Header bar ───────────────────────────────────────────────────────────
	// Branch, sync and view, in the same visual language as the Scene toolbar:
	// related controls in one rounded well, groups separated by a gap, cells
	// icon-first with a tooltip. The row used to be a text line, three loose
	// buttons and a SeparatorText per section, spread over a third of the panel
	// before the first change was visible.
	// Tree ⇄ flat, the same choice VS Code's source-control view offers. Sticky
	// across sessions — a layout preference, not a per-repo fact. Loaded before
	// the bar, which is where the toggle now lives.
	if (!s_treeViewLoaded)
	{
		s_treeViewLoaded = true;
		s_treeView = GlobalState::getInstance().getCustomConfigBool("GitChangesTreeView", true);
	}

	drawHeaderBar(*git, st);

	// ── What is in the way ───────────────────────────────────────────────────
	// Errors and the guest notice sit directly under the bar and nowhere else.
	// The old layout scattered status text between every section, so the panel
	// grew and shrank as things happened and the change list moved with it.
	if (git->blockedByCollabSession())
	{
		EditorWidgets::WrapText wrap;
		// Explained rather than silently absent: a user who cannot find the
		// commit button should learn why, not conclude the feature is broken.
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
		ImGui::TextWrapped("You are a guest in a collaboration session. The host manages "
		                   "source control for this project — your changes reach the "
		                   "others through the session.");
		ImGui::PopStyleColor();
	}
	if (!git->lastError().empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
		ImGui::TextWrapped("%s", git->lastError().c_str());
		ImGui::PopStyleColor();
	}
	else if (!git->lastInfo().empty())
	{
		// The one line here that was not already wrapped, and it is a whole
		// sentence from git ("Fetched: 3 new commits on origin/main") in a panel
		// docked at 420 px.
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("%s", git->lastInfo().c_str());
	}

	// ── Tabs ─────────────────────────────────────────────────────────────────
	// Changes, History and Branches each get the whole height instead of sharing it:
	// the old layout gave the history a fixed slice and the change list whatever was
	// left, so neither was ever the right size. A guest in a collaboration session
	// sees the same tabs read-only (no checkboxes, commit box or switch buttons).
	const bool mayWrite = git->mayModify();
	{
		EditorWidgets::WrapText wrap;
		if (ImGui::BeginTabBar("##sctabs", ImGuiTabBarFlags_NoTooltip))
		{
			const Model& model = modelFor(st);
			char changesLabel[48];
			if (model.total > 0)
				std::snprintf(changesLabel, sizeof(changesLabel), "Changes (%zu)###sc_changes", model.total);
			else
				std::snprintf(changesLabel, sizeof(changesLabel), "Changes###sc_changes");

			ImGuiTabItemFlags branchesFlags = ImGuiTabItemFlags_None;
			if (s_wantBranchesTab)
			{
				branchesFlags = ImGuiTabItemFlags_SetSelected;
				s_wantBranchesTab = false;
			}
			if (ImGui::BeginTabItem(changesLabel))
			{
				drawChangesTab(*git, ctx, st, mayWrite);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("History###sc_history"))
			{
				drawHistoryTab(*git, st, mayWrite);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Branches###sc_branches", nullptr, branchesFlags))
			{
				drawBranchesTab(*git, st, mayWrite);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}

	// ── Create branch ────────────────────────────────────────────────────────
	if (s_branchDialog) ImGui::OpenPopup("Create branch");
	beginModalSizing();
	if (ImGui::BeginPopupModal("Create branch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		// beginModalSizing() pinned the width, so wrapping at the window edge means
		// something here — and the two lines that were not already wrapped are the
		// ones that matter: the commit subject this branch starts from, and the
		// note about a dirty project, which was hand-broken at a column that stops
		// being right the moment the editor font is scaled.
		{
			EditorWidgets::WrapText wrap;

			if (s_branchFromOid.empty())
			{
				ImGui::TextWrapped("New branch from the current state (%s).",
				                   st.branch.empty() ? "no branch" : st.branch.c_str());
			}
			else
			{
				ImGui::TextWrapped("New branch starting at:");
				// One wrapped run, not TextDisabled + SameLine + TextWrapped: at a
				// fixed dialog width the second half would wrap back under the
				// first and read as two unrelated lines.
				ImGui::TextWrapped("%s  %s", s_branchFromOid.c_str(),
				                   s_branchFromSubject.c_str());
			}
			ImGui::Spacing();

			ImGui::SetNextItemWidth(-FLT_MIN);   // fill the (now fixed) width
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextWithHint("##branchname", "feature/new-lighting",
			                         s_branchName, sizeof(s_branchName));

			// Validate while typing: git's own rules, asked of git, so the answer
			// cannot drift from what the command will accept.
			const bool empty = s_branchName[0] == '\0';
			bool nameOk = false, taken = false;
			if (!empty && !git->projectRoot().empty())
			{
				nameOk = HE::Sc::GitCli::isValidBranchName(git->projectRoot(), s_branchName);
				taken  = nameOk && HE::Sc::GitCli::branchExists(git->projectRoot(), s_branchName);
			}
			if (!empty && !nameOk)
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f),
				                   "Not a usable branch name.");
			else if (taken)
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f),
				                   "A branch with this name already exists.");

			const bool dirty = st.dirtyCount() != 0;
			ImGui::BeginDisabled(dirty);
			EditorWidgets::checkbox("Switch to it right away", &s_branchCheckout);
			ImGui::EndDisabled();
			if (dirty)
			{
				// Creating the ref is still fine — it touches nothing on disk — so
				// the useful half stays available and only the switch is blocked.
				s_branchCheckout = false;
				ImGui::TextDisabled("Switching needs a clean project; the branch is created\n"
				                    "anyway and you can switch to it once you have committed.");
			}

			ImGui::Spacing();
			bool cancelled = false;
			const bool create = modalButtonRow(
				"Create", "Cancel", empty || !nameOk || taken || git->busy(), cancelled);
			if (create)
				git->requestCreateBranch(s_branchName, s_branchFromOid, s_branchCheckout);
			if (create || cancelled)
			{
				s_branchDialog = false;
				s_branchFromOid.clear();
				s_branchFromSubject.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	// ── Restore confirmation ─────────────────────────────────────────────────
	// Every file in the project changes at once, so the modal spells out the
	// full consequence AND the reassurance: the restore is recorded as a new
	// commit, so nothing in the history is lost and this itself is undoable.
	if (!s_restoreOid.empty()) ImGui::OpenPopup("Restore project?");
	beginModalSizing();
	if (ImGui::BeginPopupModal("Restore project?", nullptr,
	                           ImGuiWindowFlags_AlwaysAutoResize))
	{
		// Every paragraph here is already wrapped by hand; the guard is for the
		// next line somebody adds to this dialog, which will be a TextDisabled or
		// a TextColored and will otherwise be the one sentence that runs off the
		// edge — in the dialog that spells out an irreversible-looking change.
		{
			EditorWidgets::WrapText wrap;

			ImGui::TextWrapped("Every file in the project folder will be put back to how "
			                   "it was at this commit:");
			ImGui::Spacing();
			ImGui::TextWrapped("%s  %s", s_restoreOid.c_str(), s_restoreSubject.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("Files added since are removed, changed files are reverted, "
			                   "deleted files come back.");
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 0.6f, 1.0f));
			ImGui::TextWrapped("Your history is kept: this is recorded as a new commit, so "
			                   "you can undo it by restoring to a later one.");
			ImGui::PopStyleColor();
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
			ImGui::TextWrapped("Save and close what you are working on first — open scenes "
			                   "and assets in the editor still hold the old contents and "
			                   "would write them back.");
			ImGui::PopStyleColor();
			ImGui::Spacing();

			bool cancelled = false;
			const bool restore = modalButtonRow("Restore", "Cancel", git->busy(), cancelled,
			                                    /*danger=*/true);
			if (restore) git->requestRestoreTo(s_restoreOid, s_restoreOid);
			if (restore || cancelled)
			{
				s_restoreOid.clear();
				s_restoreSubject.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	// ── Discard confirmation ─────────────────────────────────────────────────
	// Irreversible: the changes are not in any commit. Says which files, and says
	// what happens to new ones (deleted from disk, not "reverted").
	if (!s_discardPaths.empty()) ImGui::OpenPopup("Discard changes?");
	beginModalSizing();
	if (ImGui::BeginPopupModal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextWrapped("%s", s_discardHeadline.c_str());
			ImGui::Spacing();
			for (const std::string& f : s_discardSample) ImGui::BulletText("%s", f.c_str());
			if (s_discardPaths.size() > s_discardSample.size())
				ImGui::TextDisabled("… and %zu more", s_discardPaths.size() - s_discardSample.size());
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
			ImGui::TextWrapped(s_discardHasNew
				? "Edits are thrown away. New files are deleted from disk, and because they "
				  "are not in any commit there is no getting them back."
				: "The edits are thrown away and the files go back to how the last commit has "
				  "them. This cannot be undone.");
			ImGui::PopStyleColor();
			ImGui::Spacing();

			bool cancelled = false;
			const bool discard = modalButtonRow("Discard", "Cancel", git->busy(), cancelled, /*danger=*/true);
			if (discard) git->requestDiscard(s_discardPaths);
			if (discard || cancelled)
			{
				s_discardPaths.clear();
				s_discardSample.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	// ── Switch branch with local changes ─────────────────────────────────────
	if (!s_switchTarget.empty()) ImGui::OpenPopup("Switch branch?");
	beginModalSizing(30.0f);
	if (ImGui::BeginPopupModal("Switch branch?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextWrapped("Switching to \"%s\" replaces the files in the project folder, and "
			                   "there are %zu uncommitted change(s).",
			                   s_switchTarget.c_str(), st.dirtyCount());
			ImGui::Spacing();
			ImGui::TextWrapped("Stash puts them aside and switches to a clean project; you can bring "
			                   "them back from the Branches tab. Carrying them over works only if the "
			                   "other branch does not touch the same files.");
			ImGui::Spacing();

			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float bw = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
			bool close = false;
			if (EditorWidgets::cancelButton("Cancel", ImVec2(bw, 0.0f))) close = true;
			ImGui::SameLine();
			ImGui::BeginDisabled(git->busy());
			if (EditorWidgets::button("Carry them over", ImVec2(bw, 0.0f)))
			{
				git->requestSwitchBranch(s_switchTarget, false);
				close = true;
			}
			ImGui::SameLine();
			if (EditorWidgets::primaryButton("Stash & switch", ImVec2(bw, 0.0f)))
			{
				git->requestSwitchBranch(s_switchTarget, true);
				close = true;
			}
			ImGui::EndDisabled();
			if (close)
			{
				s_switchTarget.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	ImGui::End();
#else
	(void)ctx; (void)open;
#endif
}

bool DrawFooterStatus(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	GitController* git = ctx.git;
	// No project, no build support, or discovery has not finished: draw nothing
	// at all rather than a placeholder. A footer that permanently reads "no
	// repository" for people who do not use git is noise in every session.
	if (!git || !ctx.projectLoaded) return false;
	ensureSettingsLoaded(*git);

	const HE::Sc::RepoStatus& st = git->status();

	// dirtyCount(), hasConflicts() and hasStagedChanges() each walk the whole
	// file map. That is nothing for a handful of edits and quite a lot in a
	// freshly created project where every asset is still untracked — and the
	// footer asks every frame, forever. RepoStatus::generation exists precisely
	// so a consumer can tell "same answer as last time" without comparing maps.
	static std::uint64_t s_generation = ~0ull;
	static const void*   s_source     = nullptr;
	static std::size_t   s_dirty      = 0;
	static bool          s_conflicts  = false;
	static bool          s_staged     = false;
	// The generation restarts at 0 for a different project, so the identity of
	// the status object is part of the key — otherwise switching projects could
	// leave the previous one's counts on screen.
	if (s_generation != st.generation || s_source != static_cast<const void*>(&st))
	{
		s_generation = st.generation;
		s_source     = &st;
		s_dirty      = st.dirtyCount();
		s_conflicts  = st.hasConflicts();
		s_staged     = st.hasStagedChanges();
	}

	// Colour carries the state, the text carries the detail. Amber for "there is
	// uncommitted work", red for conflicts, muted for a clean tree — a status bar
	// that is bright when nothing is wrong trains people to ignore it.
	ImVec4      tint  = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	std::string label;
	std::string tip;

	if (!git->isRepo())
	{
		label = "No repository";
		tip   = "This project is not under source control.\nClick to open Source Control.";
	}
	else
	{
		const std::size_t dirty     = s_dirty;
		const bool        conflicts = s_conflicts;
		const std::string branch    = st.detached ? std::string("detached")
		                            : st.branch.empty() ? std::string("(no branch)")
		                                                : st.branch;

		label = branch + "   ";
		if (conflicts)
		{
			label += "conflicts";
			tint   = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
		}
		else if (dirty == 0)
		{
			label += "no changes";
		}
		else
		{
			label += std::to_string(dirty) + (dirty == 1 ? " change" : " changes");
			tint   = ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
		}

		// Ahead/behind belongs in the tooltip rather than the line: it matters
		// when you go looking, and it is one more thing to read past when you do
		// not. Same for the upstream and the last error.
		tip = "Branch: " + branch;
		if (!st.upstream.empty()) tip += "\nUpstream: " + st.upstream;
		if (st.ahead > 0 || st.behind > 0)
		{
			tip += "\n" + std::to_string(st.ahead) + " to push, " +
			       std::to_string(st.behind) + " to pull";
		}
		if (st.initialCommit) tip += "\nNothing committed yet";
		tip += "\n" + std::to_string(dirty) + " changed file" + (dirty == 1 ? "" : "s");
		if (s_staged) tip += " (some staged)";
		if (!git->lastError().empty()) tip += "\n\n" + git->lastError();
		tip += "\n\nClick to open Source Control.";
	}

	// A flat, borderless button so the footer keeps reading as a status strip
	// rather than sprouting a toolbar.
	ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.20f));
	ImGui::PushStyleColor(ImGuiCol_Text,          tint);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 0.0f));

	const bool clicked = ImGui::Button(label.c_str());

	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);

	// Spelled out rather than SetTooltip, because this tooltip carries git's own
	// error text when there is one, and a git error is a full line of prose with
	// a path in it. A tooltip is sized by its longest line, so that one line
	// stretched the box across the whole screen — the panel's other lines then sat
	// alone on a strip of grey, which is the "cheap" look this pass is about. A
	// fixed column instead of the window edge for the reason a tooltip cannot be
	// asked where its edge is: it is wherever its contents put it.
	if (ImGui::IsItemHovered())
	{
		// Asked as a question for the reason imgui.h gives: EndTooltip belongs to a
		// BeginTooltip that returned true.
		if (ImGui::BeginTooltip())
		{
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
				ImGui::TextUnformatted(tip.c_str());
			}
			ImGui::EndTooltip();
		}
	}
	return clicked;
#else
	(void)ctx;
	return false;
#endif
}

} // namespace SourceControlPanel
