#include "TaskBoardPanel.h"
#include "TaskBoard.h"               // the data and its file
#include "TaskBoardTeam.h"           // assignees and notices, with a team server
#include "NotificationStore.h"       // the steady clock the team bridge is pumped with
#include "EditorApplication.h"       // AppContext, ProjectManager
#include "EditorAssetTypeCache.h"    // which icon an asset chip gets
#include "AssetThumbnailCache.h"     // ...and the rendered preview that beats it
#include "EditorCommands.h"          // entityByUuid / uuidOf: an entity's lasting address
#include "EditorSelection.h"
#include "EditorTabs.h"              // sameAsset: "is that the scene that is open?"
#include "EditorWidgets.h"           // the help-aware buttons and rows
#include "EditorHelp.h"              // "Task Board/<label>" scope for its controls
#include "EditorTheme.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/AssetRefScan.h>   // assetUuidOfFile: a dropped asset's id, without loading it
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NameComponent.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>          // RenderTextEllipsis: a title cut with "..." at the card's edge
#include <misc/cpp/imgui_stdlib.h>   // InputTextWithHint over std::string
#endif

namespace fs = std::filesystem;
using HE::Task;
using HE::TaskBoard;
using HE::TaskLink;
using HE::TaskLinkKind;
using HE::TaskPriority;
using HE::TaskStatus;

namespace TaskBoardPanel
{

// ═════════════════════════════════════════════════════════════════════════════
// The board: one per open project, and its file
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
	struct Store
	{
		std::string root;            // project root the board belongs to ("" = none loaded)
		TaskBoard   board;
		// What the file looked like when the board last read or wrote it, to
		// notice a pull without reading the bytes every frame.
		bool            hadFile = false;
		fs::file_time_type mtime{};
		std::uintmax_t  size = 0;
		std::chrono::steady_clock::time_point nextPoll{};
		// The file did not parse: the board shown is the last good one (or
		// empty), and nothing is saved until a Reload succeeds.
		bool            unreadable = false;
		// An edit is in memory that the file does not have (a save failed).
		// While it is, a changed file is NOT reloaded under it — the banner
		// asks instead.
		bool            unsaved = false;
		std::string     error;
		// Bumped whenever `board` is replaced from disk, so the details' edit
		// buffers know to re-read their task.
		unsigned        generation = 0;
	};
	Store       s_store;
	std::string s_author;

	// The team bridge (setTeam), and whether s_author is the member name it
	// set — taken back when the link signs out, so edits are not recorded
	// under a team name the editor is no longer signed in as.
	HE::TaskBoardTeam* s_team = nullptr;
	bool               s_teamAuthor = false;
	// The From the Team list: notices that arrived after this were not looked at.
	std::uint64_t      s_teamSeenMs = 0;
	std::function<std::uint64_t()> s_teamClock;   // empty: NotificationStore::nowMs
	std::uint64_t teamNow() { return s_teamClock ? s_teamClock() : HE::Ed::NotificationStore::nowMs(); }

	Jump        s_jump;

	// The details' subject and the board's view.
	HE::UUID    s_selected;
	View        s_view = View::List;
	std::string s_filter;

	// An entity jump that had to open its scene first: selected once the scene
	// is the open one, or forgotten after a while (the open was cancelled).
	struct PendingSelect
	{
		HE::UUID    entity;
		std::string sceneAbs;
		std::chrono::steady_clock::time_point until{};
	};
	PendingSelect s_pendingSelect;

	// Edits asked for WHILE the board is drawn. Applied after it: an edit can
	// remove a task or reload the whole board from disk, and the loops that
	// draw it hold references into the task list.
	std::vector<std::function<bool(TaskBoard&)>> s_posted;
	void post(std::function<bool(TaskBoard&)> fn) { s_posted.push_back(std::move(fn)); }

	// What the last frame drew (cardRect / chipRect / lastDrawStats).
	std::unordered_map<HE::UUID, Rect>              s_cardRects;
	std::unordered_map<HE::UUID, std::vector<Rect>> s_chipRects;
	std::unordered_map<int, std::vector<Rect>>      s_teamRects;   // by TeamItem
	DrawStats                                       s_stats;

	// When the orphan marks were last refreshed (refreshOrphans below).
	std::chrono::steady_clock::time_point s_orphansNext{};
	unsigned    s_orphansGen = ~0u;
	std::size_t s_orphansLinks = ~std::size_t(0);

	void stampDisk(Store& s)
	{
		std::error_code ec;
		const fs::path file = HE::taskBoardPath(s.root);
		s.hadFile = fs::exists(file, ec);
		s.mtime   = s.hadFile ? fs::last_write_time(file, ec) : fs::file_time_type{};
		s.size    = s.hadFile ? fs::file_size(file, ec) : 0;
	}

	// Read the file into the store. A file that does not parse leaves the board
	// that was there (the last good state, or empty) and says why: loading into
	// a scratch board first is what keeps a merge conflict from blanking the
	// list on screen.
	bool loadFromDisk(Store& s)
	{
		TaskBoard fresh;
		const TaskBoard::LoadResult r = HE::loadTaskBoard(s.root, fresh);
		stampDisk(s);
		if (!r.ok)
		{
			s.unreadable = true;
			s.error = "Config/TaskBoard.json cannot be read (" + r.error +
			          "). Fix the file, then Reload. Nothing is saved until then.";
			return false;
		}
		s.board      = std::move(fresh);
		s.unreadable = false;
		s.unsaved    = false;
		s.error.clear();
		++s.generation;
		return true;
	}

	bool diskChanged(const Store& s)
	{
		std::error_code ec;
		const fs::path file = HE::taskBoardPath(s.root);
		const bool has = fs::exists(file, ec);
		if (has != s.hadFile) return true;
		if (!has) return false;
		return fs::last_write_time(file, ec) != s.mtime || fs::file_size(file, ec) != s.size;
	}

	std::string projectRootOf(AppContext& ctx)
	{
		if (!ctx.projectLoaded || !ctx.projectManager) return {};
		return ctx.projectManager->projectRoot();
	}

	void forgetPanelState()
	{
		s_selected = HE::UUID{};
		s_jump = Jump{};
		s_pendingSelect = PendingSelect{};
		s_posted.clear();
		s_cardRects.clear();
		s_chipRects.clear();
		s_teamRects.clear();
		s_stats = DrawStats{};
		s_orphansNext = {};
		s_orphansGen = ~0u;
		s_orphansLinks = ~std::size_t(0);
	}
}

TaskBoard* board(AppContext& ctx)
{
	const std::string root = projectRootOf(ctx);
	if (root.empty())
	{
		if (!s_store.root.empty()) { s_store = Store{}; forgetPanelState(); }
		return nullptr;
	}
	if (root != s_store.root)
	{
		s_store = Store{};
		s_store.root = root;
		forgetPanelState();
		loadFromDisk(s_store);
		s_store.nextPoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		return &s_store.board;
	}
	// A pull, a checkout, a teammate's edit synced in: read it — unless this
	// editor holds an edit the file does not have yet, which the banner
	// settles instead of a silent reload throwing it away.
	const auto now = std::chrono::steady_clock::now();
	if (now >= s_store.nextPoll)
	{
		s_store.nextPoll = now + std::chrono::milliseconds(500);
		if (!s_store.unsaved && diskChanged(s_store)) loadFromDisk(s_store);
	}
	return &s_store.board;
}

namespace
{
	// After a saved edit: the team hears about the updates it added. Only a
	// SAVED edit — one the file does not have yet would be announced before
	// anybody could pull it.
	void tellTeam(const TaskBoard& before, const TaskBoard& after)
	{
		if (!s_team || !s_team->connected()) return;
		s_team->afterEdit(before, after, fs::path(s_store.root).filename().string(), teamNow());
	}
}

bool edit(AppContext& ctx, const std::function<bool(TaskBoard&)>& fn)
{
	TaskBoard* b = board(ctx);
	if (!b || !fn) return false;
	// The board as it was, for tellTeam — copied only while there is a team.
	const bool tell = s_team && s_team->connected();
	TaskBoard before;
	if (tell) before = *b;
	if (!fn(*b)) return false;   // nothing changed (no such task, a duplicate link)
	if (s_store.unreadable)
	{
		// Saving over a file nobody can read would destroy the only copy of
		// the tasks in it; the edit waits in memory.
		s_store.unsaved = true;
		return false;
	}
	std::string err;
	if (HE::saveTaskBoard(s_store.root, *b, &err))
	{
		stampDisk(s_store); s_store.unsaved = false; s_store.error.clear();
		if (tell) tellTeam(before, *b);
		return true;
	}

	// Refused. If the file changed under the board, take what arrived and put
	// the edit on top of it — once.
	if (diskChanged(s_store) && loadFromDisk(s_store))
	{
		// What arrived is not ours to announce: compare against it, not the
		// board from before the pull.
		if (tell) before = s_store.board;
		fn(s_store.board);
		if (HE::saveTaskBoard(s_store.root, s_store.board, &err))
		{
			stampDisk(s_store);
			if (tell) tellTeam(before, s_store.board);
			return true;
		}
	}
	s_store.unsaved = true;
	s_store.error = "The task board could not be saved: " + err;
	return false;
}

const std::string& lastError() { return s_store.error; }

void setAuthor(std::string name) { s_author = std::move(name); s_teamAuthor = false; }

void               setTeam(HE::TaskBoardTeam* team) { s_team = team; }
HE::TaskBoardTeam* team() { return s_team; }

void pumpTeam()
{
	const std::string me = s_team ? s_team->self() : std::string{};
	if (s_team) s_team->update(teamNow());
	// Edits are recorded under the member name while signed in — and the
	// footer's "mine" counts by the same name.
	if (!me.empty()) { s_author = me; s_teamAuthor = true; }
	else if (s_teamAuthor) { s_author.clear(); s_teamAuthor = false; }
}

void setTeamClock(std::function<std::uint64_t()> now) { s_teamClock = std::move(now); }

std::string author()
{
	if (!s_author.empty()) return s_author;
	for (const char* var : { "USERNAME", "USER", "LOGNAME" })
		if (const char* v = std::getenv(var); v && *v) return v;
	return "Me";
}

Jump takeJump() { Jump j = std::move(s_jump); s_jump = Jump{}; return j; }

HE::UUID selectedTask() { return s_selected; }
void     selectTask(const HE::UUID& id) { s_selected = id; }
View     view() { return s_view; }
void     setView(View v) { s_view = v; }

bool cardRect(const HE::UUID& task, Rect& out)
{
	const auto it = s_cardRects.find(task);
	if (it == s_cardRects.end()) return false;
	out = it->second;
	return true;
}

bool chipRect(const HE::UUID& task, int link, Rect& out)
{
	const auto it = s_chipRects.find(task);
	if (it == s_chipRects.end() || link < 0 || link >= (int)it->second.size()) return false;
	out = it->second[(size_t)link];
	return true;
}

bool teamRect(TeamItem item, int index, Rect& out)
{
	const auto it = s_teamRects.find((int)item);
	if (it == s_teamRects.end() || index < 0 || index >= (int)it->second.size()) return false;
	out = it->second[(size_t)index];
	return true;
}

DrawStats lastDrawStats() { return s_stats; }

void reset()
{
	s_store = Store{};
	s_author.clear();
	s_team = nullptr;
	s_teamAuthor = false;
	s_teamSeenMs = 0;
	s_teamClock = {};
	s_filter.clear();
	s_view = View::List;
	forgetPanelState();
}

#ifdef HE_IMGUI_ENABLED

// ═════════════════════════════════════════════════════════════════════════════
// The window
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
	// The windows a Panel link can name — the ones EditorUI's docsPanelOpener
	// knows how to raise, which is where a Panel jump ends up.
	const char* const kPanels[] = {
		"Scene", "World Outliner", "Details", "Content Browser", "Quick Settings",
		"Console", "Performance Profiler", "Environment", "Collaboration",
		"Source Control", "Audio Mixer", "Undo History", "Watch", "Task Board",
	};

	const TaskStatus kColumns[] = { TaskStatus::Open, TaskStatus::InProgress,
	                                TaskStatus::Blocked, TaskStatus::Done };
	const char* const kStatusLabels[]   = { "Open", "In Progress", "Blocked", "Done" };
	const char* const kPriorityLabels[] = { "Low", "Normal", "High", "Urgent" };

	// Card geometry, in units of the frame height so a larger UI font scales it.
	struct CardMetrics { float h, pad, line, chipH; };

	// Defined below the window, whose "Task Board" help scope their controls
	// belong to — the help audit reads a file top to bottom and files a
	// control under the last scope it saw.
	void drawToolbar(AppContext& ctx, TaskBoard& b);
	void drawList(AppContext& ctx, TaskBoard& b, float height);
	void drawColumns(AppContext& ctx, TaskBoard& b, float height);
	void drawDetails(AppContext& ctx, TaskBoard& b, Task& t);
	void resolvePendingSelect(AppContext& ctx);
	void refreshOrphans(AppContext& ctx, TaskBoard& b);
}

void DrawTaskBoardWindow(AppContext& ctx, bool& open)
{
	// Every control here is looked up as "Task Board/<its label>".
	HE::Ed::Help::Scope helpScope("Task Board");
	pumpTeam();   // notices arrive while the window is closed, too
	resolvePendingSelect(ctx);
	if (!open) return;

	s_cardRects.clear();
	s_chipRects.clear();
	s_teamRects.clear();
	s_stats = DrawStats{};

	ImGui::SetNextWindowSize(ImVec2(520.0f, 600.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Task Board", &open)) { ImGui::End(); return; }

	TaskBoard* b = board(ctx);
	if (!b)
	{
		ImGui::TextDisabled("No project is open.");
		ImGui::End();
		return;
	}
	refreshOrphans(ctx, *b);
	drawToolbar(ctx, *b);

	// The selected task may have gone with a reload or a delete.
	Task* sel = b->find(s_selected);
	if (!sel) s_selected = HE::UUID{};

	const float avail = ImGui::GetContentRegionAvail().y;
	const float detailH = sel ? std::max(220.0f, avail * 0.48f) : 0.0f;
	const float boardH  = std::max(80.0f, avail - detailH - (sel ? ImGui::GetStyle().ItemSpacing.y : 0.0f));

	if (s_view == View::List) drawList(ctx, *b, boardH);
	else                      drawColumns(ctx, *b, boardH);

	sel = b->find(s_selected);
	if (sel)
	{
		ImGui::BeginChild("##taskDetails", ImVec2(0, 0), ImGuiChildFlags_Borders);
		drawDetails(ctx, *b, *sel);
		ImGui::EndChild();
	}
	ImGui::End();

	std::vector<std::function<bool(TaskBoard&)>> posted;
	posted.swap(s_posted);
	for (const auto& fn : posted) edit(ctx, fn);
}

namespace
{
	// ── Small helpers ─────────────────────────────────────────────────────────
	std::string lower(std::string v)
	{
		for (char& c : v) c = (char)std::tolower((unsigned char)c);
		return v;
	}

	bool matches(const Task& t, const std::string& lowerFilter)
	{
		if (lowerFilter.empty()) return true;
		auto has = [&](const std::string& s) { return lower(s).find(lowerFilter) != std::string::npos; };
		if (has(t.title) || has(t.description)) return true;
		for (const TaskLink& l : t.links) if (has(l.label)) return true;
		for (const std::string& a : t.assignees) if (has(a)) return true;
		return false;
	}

	// The item just submitted, for teamRect().
	void noteTeamRect(TeamItem item)
	{
		const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
		s_teamRects[(int)item].push_back(Rect{ a.x, a.y, z.x, z.y });
	}

	int columnOf(const Task& t)
	{
		for (int i = 0; i < 4; ++i) if (t.status == kColumns[i]) return i;
		return 0;   // a status word this build does not know is shown with Open
	}

	ImVec4 priorityColour(const Task& t)
	{
		switch (t.priority)
		{
			case TaskPriority::Low:    return HE::Ed::Theme::TextDim;
			case TaskPriority::High:   return HE::Ed::Theme::AccentBright;
			case TaskPriority::Urgent: return ImVec4(0.90f, 0.32f, 0.26f, 1.0f);
			default:                   return HE::Ed::Theme::Accent;
		}
	}

	std::string sceneAbs(const std::string& root, const std::string& projectRel)
	{
		return (fs::path(root) / fs::path(projectRel)).lexically_normal().string();
	}

	// The project-relative spelling a link stores; empty for a path outside
	// the project (an engine scene has no place on a project's board).
	// fs::relative, not lexically_relative: it goes through the file system,
	// so a scene path from a file dialog with another drive-letter case or the
	// other slash still lands inside the project root it is inside of — the
	// same rule ContentManager::toContentRelativePath uses.
	std::string projectRelative(const std::string& root, const std::string& abs)
	{
		if (root.empty() || abs.empty()) return {};
		std::error_code ec;
		const fs::path rel = fs::relative(fs::path(abs), fs::path(root), ec);
		if (ec) return {};
		const std::string s = rel.generic_string();
		if (s.empty() || s == "." || s.rfind("..", 0) == 0) return {};
		return s;
	}

	bool isCurrentScene(AppContext& ctx, const std::string& projectRel)
	{
		if (ctx.currentScenePath.empty() || projectRel.empty()) return false;
		return EditorTabs::sameAsset(ctx.currentScenePath, sceneAbs(s_store.root, projectRel));
	}

	std::string entityName(HorizonWorld& world, Entity e)
	{
		const auto* n = world.registry().try_get<NameComponent>(e);
		return (n && !n->name.empty()) ? n->name : std::string("Entity");
	}

	std::string assetAbsPath(AppContext& ctx, const HE::UUID& id)
	{
		if (!ctx.contentManager) return {};
		const std::string rel = ctx.contentManager->pathForId(id);
		return rel.empty() ? std::string{} : ctx.contentManager->resolveAbsolutePath(rel);
	}

	// ── Orphans: what is gone, checked a few times a second, not per frame ───
	void refreshOrphans(AppContext& ctx, TaskBoard& b)
	{
		const auto now = std::chrono::steady_clock::now();
		std::size_t links = 0;
		for (const Task& t : b.tasks) links += t.links.size();
		if (now < s_orphansNext && s_orphansGen == s_store.generation && s_orphansLinks == links) return;
		s_orphansNext  = now + std::chrono::seconds(1);
		s_orphansGen   = s_store.generation;
		s_orphansLinks = links;
		const std::string root = s_store.root;
		b.refreshOrphans([&](const TaskLink& l) -> TaskBoard::LinkCheck {
			using C = TaskBoard::LinkCheck;
			switch (l.kind)
			{
				case TaskLinkKind::Asset:
					if (!ctx.contentManager) return C::Unknown;
					return ctx.contentManager->pathForId(l.id).empty() ? C::Missing : C::Present;
				case TaskLinkKind::Scene:
				{
					std::error_code ec;
					return fs::exists(sceneAbs(root, l.scene), ec) ? C::Present : C::Missing;
				}
				case TaskLinkKind::Entity:
					// Only the open scene can say; any other scene is a guess.
					if (!ctx.world) return C::Unknown;
					if (l.scene.empty())
					{
						// Linked while its scene had never been saved: found in the
						// open world is found; not found says nothing (another scene).
						return HE::Ed::entityByUuid(*ctx.world, HE::uuidToHex(l.id)) == entt::null ? C::Unknown : C::Present;
					}
					if (!isCurrentScene(ctx, l.scene)) return C::Unknown;
					return HE::Ed::entityByUuid(*ctx.world, HE::uuidToHex(l.id)) == entt::null ? C::Missing : C::Present;
				case TaskLinkKind::Panel:
					for (const char* p : kPanels) if (l.panel == p) return C::Present;
					return C::Missing;
				default: return C::Missing;
			}
		});
	}

	// ── Jumps ─────────────────────────────────────────────────────────────────
	void jumpTo(AppContext& ctx, const TaskLink& l)
	{
		switch (l.kind)
		{
			case TaskLinkKind::Asset:
				if (std::string abs = assetAbsPath(ctx, l.id); !abs.empty())
					s_jump = Jump{ Jump::Kind::Asset, std::move(abs) };
				break;
			case TaskLinkKind::Scene:
				s_jump = Jump{ Jump::Kind::Scene, sceneAbs(s_store.root, l.scene) };
				break;
			case TaskLinkKind::Panel:
				s_jump = Jump{ Jump::Kind::Panel, l.panel };
				break;
			case TaskLinkKind::Entity:
				// An empty scene: linked in a scene that was not saved yet — the
				// open world is the only place it can be.
				if (ctx.world && (l.scene.empty() || isCurrentScene(ctx, l.scene)))
				{
					const Entity e = HE::Ed::entityByUuid(*ctx.world, HE::uuidToHex(l.id));
					if (e != entt::null) ctx.selection.set(e);
					s_jump = Jump{ Jump::Kind::Entity, {} };
				}
				else if (!l.scene.empty())
				{
					// Its scene first (through EditorUI's unsaved-changes prompt);
					// the selection follows once that scene is the open one.
					s_pendingSelect = PendingSelect{ l.id, sceneAbs(s_store.root, l.scene),
					                                 std::chrono::steady_clock::now() + std::chrono::seconds(60) };
					s_jump = Jump{ Jump::Kind::Scene, s_pendingSelect.sceneAbs };
				}
				break;
			default: break;
		}
	}

	void resolvePendingSelect(AppContext& ctx)
	{
		if (s_pendingSelect.entity == HE::UUID{}) return;
		if (std::chrono::steady_clock::now() > s_pendingSelect.until) { s_pendingSelect = PendingSelect{}; return; }
		if (!ctx.world || ctx.currentScenePath.empty() ||
		    !EditorTabs::sameAsset(ctx.currentScenePath, s_pendingSelect.sceneAbs))
			return;
		const Entity e = HE::Ed::entityByUuid(*ctx.world, HE::uuidToHex(s_pendingSelect.entity));
		if (e != entt::null)
		{
			ctx.selection.set(e);
			s_jump = Jump{ Jump::Kind::Entity, {} };
		}
		s_pendingSelect = PendingSelect{};
	}

	// ── Dropping things onto a task ───────────────────────────────────────────
	// The item submitted just above is the target. An asset or a scene from
	// the Content Browser (its absolute path), an entity from the World
	// Outliner (its handle in the open world, turned into its UUID here: the
	// handle means nothing after a reload). Returns whether a link was added.
	//
	// Another task's card dropped here (`moveTo` given) takes this card's
	// status: in the column view that is "into this lane", wherever in the lane
	// it lands — a busy lane has no empty space left below its cards.
	bool acceptLinkDrop(AppContext& ctx, const HE::UUID& taskId, const TaskStatus* moveTo = nullptr)
	{
		if (!ImGui::BeginDragDropTarget()) return false;
		bool added = false;
		std::vector<TaskLink> links;
		if (const ImGuiPayload* p = moveTo ? ImGui::AcceptDragDropPayload("HE_TASK") : nullptr)
		{
			HE::UUID moved;
			std::memcpy(&moved, p->Data, sizeof(HE::UUID));
			const TaskStatus s = *moveTo;
			if (moved != taskId)
				post([moved, s](TaskBoard& b) { return b.setStatus(moved, s, author(), HE::taskNowMs()); });
		}
		else if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
		{
			const std::string abs(static_cast<const char*>(p->Data));
			const fs::path path(abs);
			std::string ext = path.extension().string();
			ext = lower(ext);
			if (ext == ".hescene")
			{
				const std::string rel = projectRelative(s_store.root, abs);
				if (!rel.empty()) links.push_back(TaskLink::sceneLink(rel, path.stem().string()));
			}
			else if (ctx.contentManager)
			{
				// Addressed by UUID, so a rename or a move keeps the link. One
				// nothing has loaded yet: its header says (no load). Only when the
				// manager then still cannot name its file — created after the last
				// content scan — is it loaded, which registers it; the chip would
				// otherwise show it as missing.
				const std::string rel = ctx.contentManager->toContentRelativePath(abs);
				HE::UUID id = rel.empty() ? HE::UUID{} : ctx.contentManager->idForPath(rel);
				if (id == HE::UUID{} && !rel.empty()) id = HE::AssetRefs::assetUuidOfFile(abs);
				if (!rel.empty() && (id == HE::UUID{} || ctx.contentManager->pathForId(id).empty()))
					id = ctx.contentManager->loadAsset(rel);
				if (id != HE::UUID{}) links.push_back(TaskLink::asset(id, path.stem().string()));
			}
		}
		else if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ENTITY"))
		{
			Entity e = entt::null;
			std::memcpy(&e, p->Data, sizeof(Entity));
			// The Outliner drags its own row; a multi-selection that contains it
			// links every entity in it.
			std::vector<Entity> ents;
			if (ctx.selection.contains(e)) ents = ctx.selection.entities();
			else ents.push_back(e);
			if (ctx.world)
			{
				const std::string scene = projectRelative(s_store.root, ctx.currentScenePath);
				for (Entity x : ents)
				{
					HE::UUID id;
					if (HE::uuidFromHex(HE::Ed::uuidOf(*ctx.world, x), id))
						links.push_back(TaskLink::entity(id, scene, entityName(*ctx.world, x)));
				}
			}
		}
		ImGui::EndDragDropTarget();
		if (!links.empty())
		{
			post([taskId, links](TaskBoard& b) {
				bool any = false;
				for (const TaskLink& l : links) any |= b.addLink(taskId, l, HE::taskNowMs());
				return any;
			});
			s_selected = taskId;
			added = true;
		}
		return added;
	}

	// ── A chip: icon or thumbnail, the name, clipped ──────────────────────────
	struct ChipVisual { ImTextureID tex = 0; bool thumb = false; char glyph = '?'; ImVec4 tint{ 0.75f, 0.85f, 1.0f, 1.0f }; };

	ChipVisual chipVisual(AppContext& ctx, const TaskLink& l)
	{
		ChipVisual v;
		const auto& I = ctx.cbIcons;
		switch (l.kind)
		{
			case TaskLinkKind::Entity: v.glyph = 'E'; v.tint = ImVec4(0.70f, 0.80f, 1.00f, 1.0f); return v;
			case TaskLinkKind::Panel:  v.glyph = 'P'; v.tint = ImVec4(0.80f, 0.80f, 0.80f, 1.0f); return v;
			case TaskLinkKind::Scene:
			{
				v.glyph = 'S'; v.tex = I.scene; v.tint = ImVec4(0.75f, 0.65f, 1.00f, 1.0f);
				// A scene has a tile too: the viewport capture its last save left.
				if (!l.orphaned)
					if (void* t = AssetThumbnailCache::get(sceneAbs(s_store.root, l.scene)))
					{ v.tex = (ImTextureID)(intptr_t)t; v.thumb = true; v.tint = ImVec4(1, 1, 1, 1); }
				return v;
			}
			case TaskLinkKind::Asset:  break;
			default: v.glyph = '?'; return v;
		}
		v.glyph = 'A';
		const std::string abs = l.orphaned ? std::string{} : assetAbsPath(ctx, l.id);
		if (abs.empty()) return v;
		if (void* t = AssetThumbnailCache::get(abs)) { v.tex = (ImTextureID)(intptr_t)t; v.thumb = true; v.tint = ImVec4(1, 1, 1, 1); return v; }
		switch (EditorAssetTypeCache::assetTypeOf(abs))
		{
			case HE::AssetType::StaticMesh:
			case HE::AssetType::SkeletalMesh: v.tex = I.model3d;  v.glyph = 'M'; v.tint = ImVec4(0.70f, 0.80f, 1.00f, 1.0f); break;
			case HE::AssetType::Prefab:       v.tex = I.prefab;   v.glyph = 'P'; v.tint = ImVec4(0.58f, 0.78f, 1.00f, 1.0f); break;
			case HE::AssetType::Material:     v.tex = I.material; v.glyph = 'M'; v.tint = ImVec4(0.60f, 0.90f, 0.60f, 1.0f); break;
			case HE::AssetType::Texture:      v.tex = I.texture;  v.glyph = 'T'; v.tint = ImVec4(0.90f, 0.75f, 0.60f, 1.0f); break;
			case HE::AssetType::Audio:        v.tex = I.sound;    v.glyph = 'A'; v.tint = ImVec4(0.60f, 0.90f, 0.90f, 1.0f); break;
			case HE::AssetType::Script:       v.tex = I.script;   v.glyph = 'S'; v.tint = ImVec4(0.90f, 0.90f, 0.50f, 1.0f); break;
			case HE::AssetType::HorizonCodeClass: v.tex = I.horizonCodeClass; v.glyph = 'C'; v.tint = ImVec4(0.95f, 0.85f, 0.55f, 1.0f); break;
			case HE::AssetType::Widget:       v.tex = I.widget;   v.glyph = 'W'; v.tint = ImVec4(0.82f, 0.72f, 1.00f, 1.0f); break;
			case HE::AssetType::AnimationClip: v.tex = I.animationClip; v.glyph = 'A'; v.tint = ImVec4(1.00f, 0.70f, 0.85f, 1.0f); break;
			case HE::AssetType::ParticleSystem: v.tex = I.particleSystem; v.glyph = 'F'; v.tint = ImVec4(1.00f, 0.82f, 0.55f, 1.0f); break;
			default: break;
		}
		return v;
	}

	std::string chipText(const TaskLink& l)
	{
		std::string s = l.label;
		if (s.empty())
		{
			switch (l.kind)
			{
				case TaskLinkKind::Scene: s = fs::path(l.scene).stem().string(); break;
				case TaskLinkKind::Panel: s = l.panel; break;
				case TaskLinkKind::Other: s = l.kindOther; break;
				default: s = HE::uuidToHex(l.id).substr(0, 8); break;
			}
		}
		return s;
	}

	// Draws one chip at the cursor, at most `maxW` wide, and returns whether it
	// was clicked. The name is cut with "..." at the chip's edge, never past
	// it. An orphaned link is dimmed and struck through: the target is gone,
	// the name it had is what is left of it.
	bool chip(AppContext& ctx, const TaskLink& l, int index, float maxW, float h)
	{
		const std::string text = chipText(l);
		const float icon = h - 4.0f;
		const float textW = ImGui::CalcTextSize(text.c_str()).x;
		const float w = std::min(maxW, 4.0f + icon + 5.0f + textW + 8.0f);
		ImGui::PushID(index);
		const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(std::max(w, h), h));
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ChipVisual v = chipVisual(ctx, l);
		const float alpha = l.orphaned ? 0.45f : 1.0f;
		dl->AddRectFilled(a, z, ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_FrameBg, alpha), h * 0.5f);
		const ImVec2 ia(a.x + 2.0f, a.y + 2.0f), iz(ia.x + icon, ia.y + icon);
		if (v.tex)
			dl->AddImage(ImTextureRef(v.tex), ia, iz, ImVec2(0, 0), ImVec2(1, 1),
			             ImGui::GetColorU32(HE::Ed::Theme::alpha(v.tint, alpha)));
		else
		{
			dl->AddRectFilled(ia, iz, ImGui::GetColorU32(HE::Ed::Theme::alpha(v.tint, 0.85f * alpha)), 3.0f);
			const char g[2] = { v.glyph, 0 };
			const ImVec2 gs = ImGui::CalcTextSize(g);
			dl->AddText(ImVec2(ia.x + (icon - gs.x) * 0.5f, ia.y + (icon - gs.y) * 0.5f),
			            IM_COL32(20, 18, 15, 255), g);
		}
		const ImVec2 ta(iz.x + 5.0f, a.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
		const ImVec2 tz(z.x - 6.0f, ta.y + ImGui::GetTextLineHeight());
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(l.orphaned ? ImGuiCol_TextDisabled : ImGuiCol_Text));
		ImGui::RenderTextEllipsis(dl, ta, tz, tz.x, text.c_str(), nullptr, nullptr);
		ImGui::PopStyleColor();
		if (l.orphaned)
			dl->AddLine(ImVec2(ta.x, (ta.y + tz.y) * 0.5f), ImVec2(std::min(tz.x, ta.x + textW), (ta.y + tz.y) * 0.5f),
			            ImGui::GetColorU32(ImGuiCol_TextDisabled), 1.0f);
		if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(text.c_str());
			const char* kind = HE::taskLinkKindName(l.kind);
			if (l.kind == TaskLinkKind::Other) kind = l.kindOther.c_str();
			ImGui::TextDisabled("%s%s%s", kind, l.scene.empty() ? "" : "  in ", l.scene.c_str());
			if (l.orphaned) ImGui::TextDisabled("Not found — deleted, renamed away, or not pulled yet.");
			else if (l.kind == TaskLinkKind::Asset) ImGui::TextDisabled("Click to show it in the Content Browser.");
			else if (l.kind == TaskLinkKind::Entity) ImGui::TextDisabled("Click to select it in the World Outliner.");
			else if (l.kind == TaskLinkKind::Scene)  ImGui::TextDisabled("Click to open the scene.");
			else if (l.kind == TaskLinkKind::Panel)  ImGui::TextDisabled("Click to bring the panel up.");
			ImGui::EndTooltip();
		}
		ImGui::PopID();
		return clicked && !l.orphaned;
	}

	// ── A task card ───────────────────────────────────────────────────────────
	// Fixed height — title, a meta line, a row of chips — so a card scrolled
	// out of the panel can be skipped with a Dummy of the same size and nothing
	// inside it submitted. A board with hundreds of tasks costs what is on
	// screen (OutlinerPanel's rows, the same rule).
	CardMetrics cardMetrics()
	{
		const float fh = ImGui::GetFrameHeight();
		const float line = ImGui::GetTextLineHeight();
		CardMetrics m;
		m.pad = 6.0f;
		m.line = line;
		m.chipH = std::floor(fh * 0.9f);
		m.h = m.pad * 2.0f + line * 2.0f + 4.0f + m.chipH;
		return m;
	}

	void drawCard(AppContext& ctx, TaskBoard& b, Task& t, float width)
	{
		const CardMetrics m = cardMetrics();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		if (!ImGui::IsRectVisible(pos, ImVec2(pos.x + width, pos.y + m.h)))
		{
			ImGui::Dummy(ImVec2(width, m.h));
			++s_stats.skipped;
			return;
		}
		++s_stats.drawn;
		const HE::UUID id = t.id;
		s_cardRects[id] = Rect{ pos.x, pos.y, pos.x + width, pos.y + m.h };
		std::vector<Rect>& chipRects = s_chipRects[id];
		chipRects.clear();
		ImGui::PushID((int)(std::uint32_t)(id.lo ^ (id.lo >> 32) ^ id.hi ^ (id.hi >> 32)));
		const bool selected = s_selected == id;
		// A second click on the selected card closes its details again.
		if (ImGui::Selectable("##card", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, m.h)))
			s_selected = selected ? HE::UUID{} : id;
		// Drag the card to another column to change its status.
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
		{
			ImGui::SetDragDropPayload("HE_TASK", &id, sizeof(HE::UUID));
			ImGui::TextUnformatted(t.title.c_str());
			ImGui::EndDragDropSource();
		}
		const TaskStatus lane = kColumns[columnOf(t)];
		acceptLinkDrop(ctx, id, &lane);
		if (ImGui::BeginPopupContextItem("##cardMenu"))
		{
			s_selected = id;
			for (int i = 0; i < 4; ++i)
			{
				const bool is = t.status == kColumns[i];
				if (ImGui::MenuItem(kStatusLabels[i], nullptr, is, !is))
					post([id, i](TaskBoard& bb) { return bb.setStatus(id, kColumns[i], author(), HE::taskNowMs()); });
			}
			ImGui::Separator();
			if (EditorWidgets::dangerMenuItem("Delete Task"))
				post([id](TaskBoard& bb) { return bb.remove(id); });
			ImGui::EndPopup();
		}

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = pos, z(pos.x + width, pos.y + m.h);
		dl->AddRect(a, z, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);
		// Priority as a stripe down the left edge.
		dl->AddRectFilled(a, ImVec2(a.x + 3.0f, z.y), ImGui::GetColorU32(priorityColour(t)), 4.0f, ImDrawFlags_RoundCornersLeft);

		const float x0 = a.x + m.pad + 4.0f, x1 = z.x - m.pad;
		float y = a.y + m.pad;
		// Everything inside is cut at the card's border, chips included. The
		// texts are ellipsised as well, but a lane of the column view can be
		// narrower than any one word of them, and a card that bleeds into the
		// next lane reads as belonging to it.
		ImGui::PushClipRect(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(z.x - 1.0f, z.y - 1.0f), true);
		{
			const std::string title = t.title.empty() ? std::string("(untitled)") : t.title;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(t.isDone() ? ImGuiCol_TextDisabled : ImGuiCol_Text));
			ImGui::RenderTextEllipsis(dl, ImVec2(x0, y), ImVec2(x1, y + m.line), x1, title.c_str(), nullptr, nullptr);
			ImGui::PopStyleColor();
		}
		y += m.line + 2.0f;
		{
			char meta[160];
			const char* status = t.status == TaskStatus::Other ? t.statusOther.c_str() : kStatusLabels[columnOf(t)];
			const char* prio = t.priority == TaskPriority::Other ? t.priorityOther.c_str()
			                                                     : kPriorityLabels[(int)t.priority];
			const int orphans = t.orphanedLinkCount();
			std::snprintf(meta, sizeof(meta), "%s  ·  %s%s%s", status, prio,
			              orphans ? "  ·  " : "", orphans ? (std::to_string(orphans) + " missing").c_str() : "");
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::RenderTextEllipsis(dl, ImVec2(x0, y), ImVec2(x1, y + m.line), x1, meta, nullptr, nullptr);
			ImGui::PopStyleColor();
		}
		y += m.line + 2.0f;

		// The chips that fit on one line; the rest as "+N".
		ImGui::SetCursorScreenPos(ImVec2(x0, y));
		float x = x0;
		const float spacing = 4.0f;
		for (std::size_t i = 0; i < t.links.size(); ++i)
		{
			const float room = x1 - x;
			const float plusW = ImGui::CalcTextSize("+99").x + 6.0f;
			const bool last = i + 1 == t.links.size();
			if (room < m.chipH * 2.5f + (last ? 0.0f : plusW))
			{
				char more[16];
				std::snprintf(more, sizeof(more), "+%zu", t.links.size() - i);
				dl->AddText(ImVec2(x, y + (m.chipH - m.line) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), more);
				break;
			}
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			const float maxW = std::min(room - (last ? 0.0f : plusW), 170.0f);
			if (chip(ctx, t.links[i], (int)i, maxW, m.chipH)) { s_selected = id; jumpTo(ctx, t.links[i]); }
			chipRects.push_back(Rect{ ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
			                          ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y });
			x = ImGui::GetItemRectMax().x + spacing;
		}
		if (t.links.empty())
		{
			const float ty = y + (m.chipH - m.line) * 0.5f;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::RenderTextEllipsis(dl, ImVec2(x0, ty), ImVec2(x1, ty + m.line), x1,
			                          "drop an asset or entity here to link it", nullptr, nullptr);
			ImGui::PopStyleColor();
		}
		ImGui::PopClipRect();
		// Back to below the card, as one item of the card's size.
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + m.h));
		ImGui::Dummy(ImVec2(0, 0));
		ImGui::PopID();
	}

	// ── Toolbar ────────────────────────────────────────────────────────────────
	void drawToolbar(AppContext& ctx, TaskBoard& b)
	{
		if (EditorWidgets::primaryButton("New Task"))
		{
			HE::UUID made;
			edit(ctx, [&](TaskBoard& bb) {
				Task* t = bb.add("New task", author(), HE::taskNowMs());
				if (t) made = t->id;
				return t != nullptr;
			});
			if (made != HE::UUID{}) s_selected = made;
		}
		ImGui::SameLine();
		if (s_view == View::List) { if (EditorWidgets::button("Columns")) s_view = View::Columns; }
		else                      { if (EditorWidgets::button("List"))    s_view = View::List; }
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x));
		ImGui::InputTextWithHint("##taskFilter", "Filter", &s_filter);

		int count[4] = {};
		for (const Task& t : b.tasks) ++count[columnOf(t)];
		ImGui::TextDisabled("%d open  ·  %d in progress  ·  %d blocked  ·  %d done",
		                    count[0], count[1], count[2], count[3]);

		// ── From the Team: what the others did to tasks that concern you ──
		// Only once something has come in; the bell has the same lines, this
		// list is the one that takes you to the task.
		if (s_team && !s_team->recent().empty())
		{
			int fresh = 0;
			for (const auto& r : s_team->recent()) fresh += r.atMs > s_teamSeenMs;
			const bool openList = EditorWidgets::smallButton("From the Team");
			noteTeamRect(TeamItem::FromTeam);
			// Under the button, not at the mouse, where it would cover it.
			const ImVec2 listAt(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
			if (openList)
			{
				ImGui::OpenPopup("##fromTeam");
				s_teamSeenMs = s_team->recent().front().atMs;
			}
			ImGui::SameLine();
			if (fresh > 0) ImGui::TextColored(HE::Ed::Theme::AccentBright, "%d new", fresh);
			else           ImGui::TextDisabled("%d", (int)s_team->recent().size());
			ImGui::SetNextWindowPos(listAt, ImGuiCond_Appearing);
			if (ImGui::BeginPopup("##fromTeam"))
			{
				int row = 0;
				for (const auto& r : s_team->recent())
				{
					const bool here = b.find(r.notice.task) != nullptr;
					ImGui::PushID(row++);
					const bool clicked = ImGui::Selectable(r.text.c_str(), false, here ? 0 : ImGuiSelectableFlags_Disabled);
					noteTeamRect(TeamItem::FromTeamRow);
					if (clicked && here) s_selected = r.notice.task;
					ImGui::PopID();
					if (!r.notice.text.empty())
					{
						ImGui::Indent();
						ImGui::TextDisabled("%s", r.notice.text.c_str());
						ImGui::Unindent();
					}
					// A task created on another machine is on this board after the
					// next pull; until then there is nothing to open. One from
					// another of the team's projects is on that project's board.
					if (!here)
					{
						const std::string project = fs::path(s_store.root).filename().string();
						ImGui::Indent();
						if (!r.notice.project.empty() && r.notice.project != project)
							ImGui::TextDisabled("In project %s.", r.notice.project.c_str());
						else
							ImGui::TextDisabled("Not on this board yet: pull to see it.");
						ImGui::Unindent();
					}
				}
				ImGui::EndPopup();
			}
		}

		if (!s_store.error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.40f, 1.0f));
			{
				EditorWidgets::WrapText wrap;
				ImGui::TextUnformatted(s_store.error.c_str());
			}
			ImGui::PopStyleColor();
			if (EditorWidgets::smallButton("Reload"))
				loadFromDisk(s_store);
			if (s_store.unsaved && !s_store.unreadable)
			{
				ImGui::SameLine();
				if (EditorWidgets::smallButton("Retry Save"))
				{
					std::string err;
					if (HE::saveTaskBoard(s_store.root, s_store.board, &err))
					{ stampDisk(s_store); s_store.unsaved = false; s_store.error.clear(); }
					else s_store.error = "The task board could not be saved: " + err;
				}
			}
		}
		ImGui::Separator();
	}

	// ── List view: grouped by status ──────────────────────────────────────────
	void drawList(AppContext& ctx, TaskBoard& b, float height)
	{
		ImGui::BeginChild("##taskList", ImVec2(0, height), ImGuiChildFlags_None);
		const std::string f = lower(s_filter);
		if (b.tasks.empty())
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled("No tasks yet. New Task adds one; drag an asset from the Content "
			                    "Browser or an entity from the World Outliner onto a task to link it.");
		}
		for (int c = 0; c < 4 && !b.tasks.empty(); ++c)
		{
			int n = 0;
			for (const Task& t : b.tasks) n += columnOf(t) == c && matches(t, f);
			char head[64];
			std::snprintf(head, sizeof(head), "%s (%d)###status%d", kStatusLabels[c], n, c);
			// Done is folded until asked for: what is finished is the least
			// interesting part of a to-do list.
			if (!ImGui::CollapsingHeader(head, c == 3 ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) continue;
			const float w = ImGui::GetContentRegionAvail().x;
			for (Task& t : b.tasks)
				if (columnOf(t) == c && matches(t, f)) drawCard(ctx, b, t, w);
		}
		ImGui::EndChild();
	}

	// ── Columns view: four lanes, drag a card across to change its status ─────
	void drawColumns(AppContext& ctx, TaskBoard& b, float height)
	{
		const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
		                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings;
		if (!ImGui::BeginTable("##taskColumns", 4, flags, ImVec2(0, height))) return;
		ImGui::TableSetupScrollFreeze(0, 1);
		for (const char* l : kStatusLabels) ImGui::TableSetupColumn(l);
		ImGui::TableHeadersRow();
		const std::string f = lower(s_filter);
		ImGui::TableNextRow();
		for (int c = 0; c < 4; ++c)
		{
			ImGui::TableNextColumn();
			ImGui::PushID(c);
			const float w = ImGui::GetContentRegionAvail().x;
			for (Task& t : b.tasks)
				if (columnOf(t) == c && matches(t, f)) drawCard(ctx, b, t, w);
			// The rest of the lane is the drop zone for a card from another one.
			const float rest = std::max(ImGui::GetFrameHeight() * 2.0f,
			                            ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetCursorScreenPos().y - 4.0f);
			ImGui::Dummy(ImVec2(w, rest));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_TASK"))
				{
					HE::UUID id;
					std::memcpy(&id, p->Data, sizeof(HE::UUID));
					post([id, c](TaskBoard& bb) { return bb.setStatus(id, kColumns[c], author(), HE::taskNowMs()); });
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	// ── Details of the selected task ──────────────────────────────────────────
	void drawDetails(AppContext& ctx, TaskBoard& b, Task& t)
	{
		// Edit buffers: re-read when the task changes or the board came back
		// from disk, never while the field is being typed in.
		static HE::UUID    s_for;
		static unsigned    s_gen = ~0u;
		static std::string s_title, s_desc, s_comment;
		const HE::UUID id = t.id;
		if (s_for != id || s_gen != s_store.generation)
		{
			s_for = id; s_gen = s_store.generation;
			s_title = t.title; s_desc = t.description; s_comment.clear();
		}

		EditorWidgets::Row::inputText("Title", &s_title);
		if (ImGui::IsItemDeactivatedAfterEdit() && s_title != t.title)
		{
			post([id, v = s_title](TaskBoard& bb) { return bb.setTitle(id, v, HE::taskNowMs()); });
		}
		else if (!ImGui::IsItemActive()) s_title = t.title;

		int status = columnOf(t);
		if (EditorWidgets::Row::combo("Status", &status, kStatusLabels, 4))
			post([id, status](TaskBoard& bb) { return bb.setStatus(id, kColumns[status], author(), HE::taskNowMs()); });
		int prio = t.priority == TaskPriority::Other ? 1 : (int)t.priority;
		if (EditorWidgets::Row::combo("Priority", &prio, kPriorityLabels, 4))
			post([id, prio](TaskBoard& bb) { return bb.setPriority(id, (TaskPriority)prio, HE::taskNowMs()); });

		// ── Assigned To: only with a team to assign to ──
		// Without a team server there is nobody to pick from, and a name typed
		// by hand would reach nobody: the field is not drawn at all.
		if (s_team && s_team->connected())
		{
			ImGui::TextUnformatted("Assigned To");
			const bool labelHelp = EditorWidgets::helpForLabel("Assigned To");
			std::string preview;
			for (std::size_t k = 0; k < t.assignees.size(); ++k) preview += (k ? ", " : "") + t.assignees[k];
			if (preview.empty()) preview = "Nobody";
			ImGui::SetNextItemWidth(-FLT_MIN);
			const bool listOpen = ImGui::BeginCombo("##assignedTo", preview.c_str());
			noteTeamRect(TeamItem::AssignedTo);
			if (listOpen)
			{
				for (const HE::TaskTeamMember& m : s_team->members())
				{
					const bool on = std::any_of(t.assignees.begin(), t.assignees.end(),
					                            [&](const std::string& a) { return HE::sameTaskMember(a, m.name); });
					const std::string label = m.name + (m.online ? "" : "  (offline)") + "##member";
					ImGui::PushID(m.name.c_str());
					// Stays open: assigning two people is two ticks, not two trips.
					const bool picked = ImGui::Selectable(label.c_str(), on, ImGuiSelectableFlags_DontClosePopups);
					noteTeamRect(TeamItem::Assignee);
					if (picked)
					{
						std::vector<std::string> next;
						for (const std::string& a : t.assignees)
							if (!HE::sameTaskMember(a, m.name)) next.push_back(a);
						if (!on) next.push_back(m.name);
						post([id, next](TaskBoard& bb) { return bb.assign(id, next, author(), HE::taskNowMs()); });
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			if (!labelHelp) EditorWidgets::helpForLabel("Assigned To");
		}

		EditorWidgets::Row::inputTextMultiline("Description", &s_desc, ImGui::GetTextLineHeight() * 3.5f);
		if (ImGui::IsItemDeactivatedAfterEdit() && s_desc != t.description)
		{
			post([id, v = s_desc](TaskBoard& bb) { return bb.setDescription(id, v, HE::taskNowMs()); });
		}
		else if (!ImGui::IsItemActive()) s_desc = t.description;

		// ── Links ──
		EditorWidgets::subHeading("Links");
		{
			// Chips flow left to right and wrap; each with its × to unlink.
			const float chipH = std::floor(ImGui::GetFrameHeight() * 0.9f);
			const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
			const float x0 = ImGui::GetCursorScreenPos().x;
			const float rmW = ImGui::GetFrameHeight();
			for (std::size_t i = 0; i < t.links.size(); ++i)
			{
				const float maxW = std::max(chipH * 2.0f, std::min(220.0f, right - x0 - rmW - 2.0f));
				const float want = std::min(maxW, chipH + 9.0f + ImGui::CalcTextSize(chipText(t.links[i]).c_str()).x + 8.0f);
				if (i > 0 && ImGui::GetItemRectMax().x + 8.0f + want + 2.0f + rmW <= right) ImGui::SameLine(0, 8.0f);
				if (chip(ctx, t.links[i], (int)i, maxW, chipH)) jumpTo(ctx, t.links[i]);
				ImGui::SameLine(0, 2.0f);
				ImGui::PushID((int)i);
				if (EditorWidgets::dangerSmallButton("×##unlink"))
					post([id, i](TaskBoard& bb) { return bb.removeLink(id, i, HE::taskNowMs()); });
				ImGui::PopID();
			}
		}
		// The drop zone, spelled out: the card accepts the same drops, but a
		// target that says what it takes is what makes the gesture findable.
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			const float h = ImGui::GetFrameHeight() * 1.4f;
			ImGui::Dummy(ImVec2(w, h));
			const bool dragging = ImGui::GetDragDropPayload() != nullptr;
			ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + w, p.y + h),
				ImGui::GetColorU32(dragging ? HE::Ed::Theme::AccentBright : HE::Ed::Theme::TextDim), 4.0f, 0, dragging ? 2.0f : 1.0f);
			const char* hint = "Drop an asset, a scene or an entity here";
			const ImVec2 hs = ImGui::CalcTextSize(hint);
			ImGui::GetWindowDrawList()->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
			ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + std::max(6.0f, (w - hs.x) * 0.5f), p.y + (h - hs.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
			ImGui::GetWindowDrawList()->PopClipRect();
			acceptLinkDrop(ctx, id);
		}
		{
			const bool canEntity = ctx.world && !ctx.selection.empty();
			ImGui::BeginDisabled(!canEntity);
			if (EditorWidgets::button("Link Selection"))
			{
				std::vector<TaskLink> links;
				const std::string scene = projectRelative(s_store.root, ctx.currentScenePath);
				for (Entity e : ctx.selection.entities())
				{
					HE::UUID eid;
					if (HE::uuidFromHex(HE::Ed::uuidOf(*ctx.world, e), eid))
						links.push_back(TaskLink::entity(eid, scene, entityName(*ctx.world, e)));
				}
				post([id, links](TaskBoard& bb) {
					bool any = false;
					for (const TaskLink& l : links) any |= bb.addLink(id, l, HE::taskNowMs());
					return any;
				});
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			const std::string scene = projectRelative(s_store.root, ctx.currentScenePath);
			ImGui::BeginDisabled(scene.empty());
			if (EditorWidgets::button("Link Scene"))
			{
				const TaskLink l = TaskLink::sceneLink(scene, fs::path(scene).stem().string());
				post([id, l](TaskBoard& bb) { return bb.addLink(id, l, HE::taskNowMs()); });
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (EditorWidgets::button("Link Panel")) ImGui::OpenPopup("##linkPanel");
			if (ImGui::BeginPopup("##linkPanel"))
			{
				for (const char* p : kPanels)
					if (ImGui::Selectable(p, false))
					{
						const TaskLink l = TaskLink::panelLink(p, p);
						post([id, l](TaskBoard& bb) { return bb.addLink(id, l, HE::taskNowMs()); });
					}
				ImGui::EndPopup();
			}
		}

		// ── History and a comment ──
		EditorWidgets::subHeading("History");
		{
			ImGui::TextDisabled("Created by %s, %s", t.createdBy.empty() ? "someone" : t.createdBy.c_str(),
			                    HE::taskTimeToIso(t.createdMs).substr(0, 16).c_str());
			const std::size_t shown = std::min<std::size_t>(t.updates.size(), 6);
			EditorWidgets::WrapText wrap;
			for (std::size_t i = t.updates.size() - shown; i < t.updates.size(); ++i)
			{
				const HE::TaskUpdate& u = t.updates[i];
				std::string what;
				switch (u.kind)
				{
					case HE::TaskUpdateKind::Status:     what = "set it to " + u.status; break;
					case HE::TaskUpdateKind::Assignment:
					{
						what = "assigned it to ";
						for (std::size_t k = 0; k < u.assignees.size(); ++k) what += (k ? ", " : "") + u.assignees[k];
						if (u.assignees.empty()) what = "unassigned it";
						break;
					}
					case HE::TaskUpdateKind::Comment:    what = "commented"; break;
					default:                             what = u.kindOther; break;
				}
				ImGui::TextDisabled("%s  %s %s", HE::taskTimeToIso(u.timeMs).substr(0, 16).c_str(),
				                    u.author.c_str(), what.c_str());
				if (!u.text.empty()) { ImGui::Indent(); ImGui::TextUnformatted(u.text.c_str()); ImGui::Unindent(); }
			}
		}
		ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 120.0f));
		ImGui::InputTextWithHint("##taskComment", "Write a comment", &s_comment);
		ImGui::SameLine();
		ImGui::BeginDisabled(s_comment.empty());
		if (EditorWidgets::button("Add Comment"))
		{
			post([id, text = s_comment](TaskBoard& bb) { return bb.comment(id, author(), text, HE::taskNowMs()) != nullptr; });
			s_comment.clear();
		}
		ImGui::EndDisabled();

		ImGui::Spacing();
		if (EditorWidgets::dangerButton("Delete Task")) ImGui::OpenPopup("##confirmDeleteTask");
		if (ImGui::BeginPopup("##confirmDeleteTask"))
		{
			ImGui::Text("Delete \"%s\"?", t.title.c_str());
			ImGui::TextDisabled("It goes from the board and from Config/TaskBoard.json.");
			if (EditorWidgets::dangerButton("Delete"))
			{
				post([id](TaskBoard& bb) { return bb.remove(id); });
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		(void)b;
	}
}

#else
void DrawTaskBoardWindow(AppContext&, bool&) { pumpTeam(); }
#endif

} // namespace TaskBoardPanel
