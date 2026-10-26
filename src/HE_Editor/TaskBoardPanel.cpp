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
#include <imgui_internal.h>          // RenderTextEllipsis: a chip name cut with "..." at the chip's edge
#endif

namespace fs = std::filesystem;
using HE::Task;
using HE::TaskBoard;
using HE::TaskLink;
using HE::TaskLinkKind;
using HE::TaskPriority;
using HE::TaskStatus;

// ── The Task Board's data core ───────────────────────────────────────────────
// What is left of the Task Board PANEL after the Planner took over its window
// (docs/planner-plan-2026-10-22.md, P1b): the one copy of the board per open
// project and the one writer of its file (board / edit), the team bridge's pump,
// the jumps a link chip asks for, and the pieces of link UI the Planner's Tasks
// database reuses (chips, drops). The window, its cards and its details are gone.
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

	// The task the Planner shows details for; a zero UUID for none.
	HE::UUID    s_selected;

	// An entity jump that had to open its scene first: selected once the scene
	// is the open one, or forgotten after a while (the open was cancelled).
	struct PendingSelect
	{
		HE::UUID    entity;
		std::string sceneAbs;
		std::chrono::steady_clock::time_point until{};
	};
	PendingSelect s_pendingSelect;

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
	++s_store.generation;
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
		++s_store.generation;
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

void reloadBoard(AppContext& ctx)
{
	if (board(ctx)) loadFromDisk(s_store);
}

bool retrySave()
{
	if (!s_store.unsaved || s_store.unreadable) return false;
	std::string err;
	if (HE::saveTaskBoard(s_store.root, s_store.board, &err))
	{
		stampDisk(s_store);
		s_store.unsaved = false;
		s_store.error.clear();
		return true;
	}
	s_store.error = "The task board could not be saved: " + err;
	return false;
}

bool hasUnsavedEdit() { return s_store.unsaved && !s_store.unreadable; }

std::uint64_t teamSeen() { return s_teamSeenMs; }
void          markTeamSeen(std::uint64_t atMs) { s_teamSeenMs = atMs; }
std::string   projectName() { return fs::path(s_store.root).filename().string(); }

void reset()
{
	s_store = Store{};
	s_author.clear();
	s_team = nullptr;
	s_teamAuthor = false;
	s_teamSeenMs = 0;
	s_teamClock = {};
	forgetPanelState();
}

// ═════════════════════════════════════════════════════════════════════════════
// Links: where they point, whether the target is still there, how to go there
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
	// The windows a Panel link can name — the ones EditorUI's docsPanelOpener
	// knows how to raise, which is where a Panel jump ends up. "Task Board" is
	// not one of them any more: it is the Planner now, and an old link to the
	// Task Board panel opens the Planner tab (EditorUI's jump handler).
	const char* const kPanels[] = {
		"Scene", "World Outliner", "Details", "Content Browser", "Quick Settings",
		"Console", "Performance Profiler", "Environment", "Collaboration",
		"Source Control", "Audio Mixer", "Undo History", "Watch", "Planner",
	};

	std::string lower(std::string v)
	{
		for (char& c : v) c = (char)std::tolower((unsigned char)c);
		return v;
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
						return HE::Ed::entityByUuid(*ctx.world, l.id) == entt::null ? C::Unknown : C::Present;
					}
					if (!isCurrentScene(ctx, l.scene)) return C::Unknown;
					return HE::Ed::entityByUuid(*ctx.world, l.id) == entt::null ? C::Missing : C::Present;
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
					const Entity e = HE::Ed::entityByUuid(*ctx.world, l.id);
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
		const Entity e = HE::Ed::entityByUuid(*ctx.world, s_pendingSelect.entity);
		if (e != entt::null)
		{
			ctx.selection.set(e);
			s_jump = Jump{ Jump::Kind::Entity, {} };
		}
		s_pendingSelect = PendingSelect{};
	}
}

void tick(AppContext& ctx)
{
	pumpTeam();   // notices arrive while the Planner is closed, too
	resolvePendingSelect(ctx);
}

unsigned generation() { return s_store.generation; }

void refreshOrphans(AppContext& ctx)
{
	if (TaskBoard* b = board(ctx)) refreshOrphans(ctx, *b);
}

void jumpToLink(AppContext& ctx, const HE::TaskLink& l) { jumpTo(ctx, l); }

std::vector<const char*> linkablePanels()
{
	return std::vector<const char*>(std::begin(kPanels), std::end(kPanels));
}

std::vector<HE::TaskLink> selectionLinks(AppContext& ctx)
{
	std::vector<HE::TaskLink> links;
	if (!ctx.world) return links;
	const std::string scene = projectRelative(s_store.root, ctx.currentScenePath);
	for (Entity e : ctx.selection.entities())
	{
		HE::UUID eid;
		if (HE::uuidFromHex(HE::Ed::uuidOf(*ctx.world, e), eid))
			links.push_back(TaskLink::entity(eid, scene, entityName(*ctx.world, e)));
	}
	return links;
}

bool currentSceneLink(AppContext& ctx, HE::TaskLink& out)
{
	const std::string scene = projectRelative(s_store.root, ctx.currentScenePath);
	if (scene.empty()) return false;
	out = TaskLink::sceneLink(scene, fs::path(scene).stem().string());
	return true;
}

#ifdef HE_IMGUI_ENABLED

// ═════════════════════════════════════════════════════════════════════════════
// Link chips and drops, for the Planner's Links cells
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
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
}

float linkChipWidth(const HE::TaskLink& l, float maxW, float h)
{
	return std::min(maxW, h + 9.0f + ImGui::CalcTextSize(chipText(l).c_str()).x + 8.0f);
}

	// Draws one chip at the cursor, at most `maxW` wide, and returns whether it
// was clicked. The name is cut with "..." at the chip's edge, never past
// it. An orphaned link is dimmed and struck through: the target is gone,
// the name it had is what is left of it.
bool linkChip(AppContext& ctx, const TaskLink& l, int index, float maxW, float h)
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

// The item submitted just above is the target. An asset or a scene from the
// Content Browser (its absolute path), an entity from the World Outliner (its
// handle in the open world, turned into its UUID here: the handle means nothing
// after a reload). Returns the links to add; the caller edits.
std::vector<HE::TaskLink> acceptDroppedLinks(AppContext& ctx)
{
	std::vector<TaskLink> links;
	if (!ImGui::BeginDragDropTarget()) return links;
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
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
	return links;
}

#else
std::vector<HE::TaskLink> acceptDroppedLinks(AppContext&) { return {}; }
#endif

} // namespace TaskBoardPanel
