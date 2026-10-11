#include "StreamingDebugView.h"
#include "EditorApplication.h"   // AppContext, ProjectManager, EditorCamera
#include "EditorUndo.h"
#include "CollabController.h"    // no split or merge inside a session
#include "ViewportPanel.h"       // the Streaming Cells show flag
#include <HorizonScene/CellSplit.h>
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <JobSystem/JobSystem.h>
#include <ContentManager/ContentManager.h>
#include <DebugDraw/DebugDraw.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include "EditorWidgets.h"   // rows look themselves up under the profiler's "Profiler" help scope
#endif

namespace StreamingDebugView
{

// ── The manifest, parsed once per change ─────────────────────────────────────
// Asked every frame by the overlay and the tab; the text only changes when a
// scene is opened, split or merged. One cache is enough: there is one world in
// the editor at a time.
const HE::CellManifest* manifestOf(const HorizonWorld& world)
{
	static std::string      s_text;
	static HE::CellManifest s_manifest;
	static bool             s_ok = false;
	const std::string& text = world.cellManifestJson();
	if (text != s_text)
	{
		s_text = text;
		s_ok   = !text.empty() && HE::CellManifest::parse(text, s_manifest) && !s_manifest.empty();
		// The preview anchors are absolute positions in the scene they were set in; another
		// scene (or the same one split or merged) would draw them somewhere that means nothing.
		previewPins().clear();
	}
	return s_ok ? &s_manifest : nullptr;
}

std::vector<HE::CellAnchor>& previewPins()
{
	static std::vector<HE::CellAnchor> s_pins;
	return s_pins;
}

// ── The cells in the Scene window ────────────────────────────────────────────
void appendCellLines(const HorizonWorld& world, const glm::vec3& eye, DebugDrawBuffer& out)
{
	appendCellLines(world, eye, {}, out);
}

void appendCellLines(const HorizonWorld& world, const glm::vec3& eye,
                     const std::vector<HE::CellAnchor>& pins, DebugDrawBuffer& out)
{
	const HE::CellManifest* m = manifestOf(world);
	if (!m) return;
	// The manifest is in absolute positions, the world relative to its origin
	// (zero in the editor, but the rule costs nothing).
	const glm::dvec3 origin  = world.origin();
	const glm::dvec3 eyeAbs  = glm::dvec3(eye) + origin;
	const double     size    = m->cellSize;
	// Far enough to see a ring of dropped cells around the kept ones, and never
	// so far that a fine grid draws tens of thousands of squares.
	const double range = std::max(static_cast<double>(m->unloadRadius) * 1.5, size * 2.0);
	// The camera is an anchor like the pins: the nearest of them decides a cell's colour.
	// A viewpoint has no velocity, so the lookahead does not enter.
	std::vector<HE::CellAnchor> anchors;
	anchors.reserve(1 + pins.size());
	HE::CellAnchor cam;
	cam.position = eyeAbs;
	anchors.push_back(cam);
	anchors.insert(anchors.end(), pins.begin(), pins.end());
	const std::vector<HE::CellManifest::View> cells = m->around(anchors, range);

	const glm::vec3 kLoad(0.25f, 0.85f, 0.35f);   // the game builds it from here
	const glm::vec3 kKeep(0.95f, 0.70f, 0.20f);   // kept once built, not loaded
	const glm::vec3 kOut (0.45f, 0.45f, 0.50f);   // dropped
	// Just above the ground plane, so the squares do not z-fight with the grid.
	const float y = static_cast<float>(0.05 - origin.y);
	// Inset a little, so two neighbours of different colour both stay visible.
	const double inset = size * 0.01;
	constexpr size_t kMaxSquares = 2048;
	size_t drawn = 0;
	for (const HE::CellManifest::View& c : cells)
	{
		if (++drawn > kMaxSquares) break;
		const glm::vec3 col = c.reach == HE::CellManifest::View::Reach::Load ? kLoad
		                    : c.reach == HE::CellManifest::View::Reach::Keep ? kKeep : kOut;
		const float x0 = static_cast<float>(c.x * size + inset - origin.x);
		const float z0 = static_cast<float>(c.z * size + inset - origin.z);
		const float x1 = static_cast<float>((c.x + 1) * size - inset - origin.x);
		const float z1 = static_cast<float>((c.z + 1) * size - inset - origin.z);
		out.line({ x0, y, z0 }, { x1, y, z0 }, col);
		out.line({ x1, y, z0 }, { x1, y, z1 }, col);
		out.line({ x1, y, z1 }, { x0, y, z1 }, col);
		out.line({ x0, y, z1 }, { x0, y, z0 }, col);
	}

	// The two radii around each anchor's ground point: the camera's, and a pin's
	// stretched by its radiusScale.
	const auto ring = [&](float cx, float cz, float radius, const glm::vec3& col)
	{
		constexpr int kSegments = 96;
		for (int i = 0; i < kSegments; ++i)
		{
			const float a0 = 6.2831853f * static_cast<float>(i) / kSegments;
			const float a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
			out.line({ cx + radius * std::cos(a0), y, cz + radius * std::sin(a0) },
			         { cx + radius * std::cos(a1), y, cz + radius * std::sin(a1) }, col);
		}
	};
	for (const HE::CellAnchor& a : anchors)
	{
		const float scale = a.radiusScale > 0.0f ? a.radiusScale : 1.0f;
		const float cx = static_cast<float>(a.position.x - origin.x);
		const float cz = static_cast<float>(a.position.z - origin.z);
		ring(cx, cz, m->loadRadius * scale, kLoad);
		if (m->unloadRadius > m->loadRadius) ring(cx, cz, m->unloadRadius * scale, kKeep);
	}
}

// ── Split and merge, on the open scene ───────────────────────────────────────
// Both change the world in place, behind one undo entry, and leave the scene
// unsaved: the base reaches the disk with the next Save, and until then the
// saved file still holds the whole scene and names no cells. Only the cell
// files are written right away — the base's manifest points at them.
namespace
{
// The folder cell paths are relative to: the project's, the one above Content
// (how the game's reader resolves them, GameApplication::updateCellStreaming).
// Both replace the whole world, which a collaboration session does not
// replicate (it carries edits, not a rebuilt scene): the peers would keep the
// old one. Not offered in a session, like opening another scene.
bool inCollabSession(const AppContext& ctx)
{
	return ctx.collab && ctx.collab->inSession();
}

std::filesystem::path projectRootOf(AppContext& ctx)
{
	if (!ctx.contentManager || ctx.contentManager->contentRoot().empty()) return {};
	return std::filesystem::path(ctx.contentManager->contentRoot()).parent_path();
}

bool readFileBytes(const std::filesystem::path& file, std::vector<uint8_t>& out)
{
	std::ifstream in(file, std::ios::binary);
	if (!in) return false;
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return !out.empty();
}
} // namespace

bool splitOpenScene(AppContext& ctx, const HE::CellSplitOptions& base, std::string& message)
{
	if (!ctx.world || ctx.isPlaying) { message = "Stop playing first."; return false; }
	if (inCollabSession(ctx)) { message = "Not inside a collaboration session: leave it first."; return false; }
	if (ctx.currentScenePath.empty()) { message = "Save the scene first: the cells go next to it."; return false; }
	const std::filesystem::path root = projectRootOf(ctx);
	if (root.empty()) { message = "No project is open."; return false; }

	const std::filesystem::path scene(ctx.currentScenePath);
	const std::filesystem::path cellsDir = scene.parent_path() / (scene.stem().string() + ".cells");
	std::error_code ec;
	const std::filesystem::path rel = std::filesystem::relative(cellsDir, root, ec);
	if (ec || rel.empty() || rel.begin()->string() == "..")
	{
		message = "The scene is not inside the project folder.";
		return false;
	}
	HE::CellSplitOptions o = base;
	o.dir = rel.generic_string();

	// Pushed onto the history only once the split happened (see merge below).
	std::vector<uint8_t> before;
	SceneSerializer().saveToMemory(*ctx.world, before);
	std::filesystem::create_directories(cellsDir, ec);
	std::vector<std::string> written;
	const HE::CellSplitResult r = HE::splitWorldIntoCells(*ctx.world, o,
		[&](const std::string& path, const std::string& text)
		{
			std::ofstream out(root / path, std::ios::binary | std::ios::trunc);
			out << text;
			if (!out) return false;
			written.push_back(std::filesystem::path(path).filename().string());
			return true;
		});
	if (!r.error.empty())
	{
		message = "Not split: " + r.error + ".";
		return false;
	}
	if (ctx.undoSys) ctx.undoSys->pushSnapshot(std::move(before), "Split into Streaming Cells");
	// Cells of an earlier split with another grid would otherwise stay on disk
	// beside the new ones. Only files named the way a split names them go.
	static const std::regex kCellFile(R"(cell_-?\d+_-?\d+\.hescene)");
	for (const auto& entry : std::filesystem::directory_iterator(cellsDir, ec))
	{
		const std::string name = entry.path().filename().string();
		if (std::regex_match(name, kCellFile)
		    && std::find(written.begin(), written.end(), name) == written.end())
			std::filesystem::remove(entry.path(), ec);
	}
	ctx.selection.clear();
	char buf[256];
	std::snprintf(buf, sizeof(buf), "Split into %zu cells, %zu entities moved to %s. Save the scene to keep it.",
	              r.cells.size(), r.moved, o.dir.c_str());
	message = buf;
	return true;
}

bool mergeOpenScene(AppContext& ctx, std::string& message)
{
	if (!ctx.world || ctx.isPlaying) { message = "Stop playing first."; return false; }
	if (inCollabSession(ctx)) { message = "Not inside a collaboration session: leave it first."; return false; }
	const std::filesystem::path root = projectRootOf(ctx);
	if (root.empty()) { message = "No project is open."; return false; }
	// The undo entry is taken only once every cell has been read: a merge that
	// cannot happen must not leave an empty step in the history.
	std::vector<uint8_t> before;
	SceneSerializer().saveToMemory(*ctx.world, before);
	std::string error;
	size_t merged = 0;
	const bool ok = HE::mergeCellsIntoWorld(*ctx.world,
		[&root](const std::string& path, std::vector<uint8_t>& out) { return readFileBytes(root / path, out); },
		&error, &merged);
	if (!ok)
	{
		message = "Not merged: " + error + ".";
		return false;
	}
	if (ctx.undoSys) ctx.undoSys->pushSnapshot(std::move(before), "Merge Streaming Cells");
	ctx.selection.clear();
	message = "Merged " + std::to_string(merged) + " entities back into the scene. The cell files stay "
	          "on disk until the next split; save the scene to keep it whole.";
	return true;
}

#ifdef HE_IMGUI_ENABLED
namespace
{
// Per-second rates from two stats snapshots half a second apart: a rate read
// off one frame is noise, and the tab is for reading, not for plotting.
struct PoolRates
{
	double jobsPerSec[HE::kJobPriorityCount] = {};
	double poolShare[HE::kJobPriorityCount]  = {};   // busy time / (wall time × workers)
};

const PoolRates& poolRates(const HE::ThreadPoolStats& now)
{
	using Clock = std::chrono::steady_clock;
	static PoolRates           s_rates;
	static HE::ThreadPoolStats s_prev;
	static Clock::time_point   s_prevAt;
	static bool                s_have = false;
	const Clock::time_point t = Clock::now();
	if (!s_have)
	{
		s_prev = now; s_prevAt = t; s_have = true;
		return s_rates;
	}
	const double dt = std::chrono::duration<double>(t - s_prevAt).count();
	if (dt < 0.5) return s_rates;
	const double workers = static_cast<double>(std::max<size_t>(1, now.threads));
	for (size_t p = 0; p < HE::kJobPriorityCount; ++p)
	{
		const auto& a = s_prev.lanes[p];
		const auto& b = now.lanes[p];
		s_rates.jobsPerSec[p] = static_cast<double>(b.executed - a.executed) / dt;
		s_rates.poolShare[p]  = static_cast<double>(b.busyNs - a.busyNs) * 1e-9 / (dt * workers);
	}
	s_prev = now; s_prevAt = t;
	return s_rates;
}

const char* reachName(HE::CellManifest::View::Reach r)
{
	switch (r)
	{
	case HE::CellManifest::View::Reach::Load: return "load";
	case HE::CellManifest::View::Reach::Keep: return "keep";
	default:                                  return "out";
	}
}

void drawJobs()
{
	ImGui::SeparatorText("Job system");
	ThreadPool& pool = globalPool();
	const HE::ThreadPoolStats s = pool.stats();
	const PoolRates& r = poolRates(s);
	ImGui::Text("%zu workers", s.threads);
	constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
	                                 | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("##jobs", 8, kFlags)) return;
	for (const char* h : { "Priority", "Queued", "Running", "Cap", "Jobs/s", "Pool", "Cancelled", "Failed" })
		ImGui::TableSetupColumn(h);
	ImGui::TableHeadersRow();
	static const char* kNames[HE::kJobPriorityCount] = { "High", "Normal", "Low" };
	for (size_t p = 0; p < HE::kJobPriorityCount; ++p)
	{
		const HE::ThreadPoolStats::Lane& l = s.lanes[p];
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextUnformatted(kNames[p]);
		ImGui::TableNextColumn(); ImGui::Text("%zu", l.queued);
		ImGui::TableNextColumn(); ImGui::Text("%zu", l.running);
		ImGui::TableNextColumn();
		if (l.limit >= s.threads) ImGui::TextDisabled("none");
		else                      ImGui::Text("%zu", l.limit);
		ImGui::TableNextColumn(); ImGui::Text("%.0f", r.jobsPerSec[p]);
		ImGui::TableNextColumn(); ImGui::Text("%.0f %%", r.poolShare[p] * 100.0);
		ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(l.cancelled));
		ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(l.failed));
	}
	ImGui::EndTable();
	EditorWidgets::WrapText wrap;
	ImGui::TextDisabled("Pool is the share of all workers' time the priority took over the last "
	                    "half second. Low is background streaming; its cap keeps workers free "
	                    "for the frame. The Timeline tab names every job on the worker lanes.");
}

void drawAssets(ContentManager& cm)
{
	ImGui::SeparatorText("Asset streaming");
	const ContentManager::AsyncPollStats& ps = cm.asyncPollStats();
	ImGui::Text("%zu in flight, %zu resident", cm.asyncInFlightCount(), cm.assetCount());
	ImGui::Text("Loaded %llu, failed %llu, dropped %llu, restarted %llu",
	            static_cast<unsigned long long>(ps.registered), static_cast<unsigned long long>(ps.failed),
	            static_cast<unsigned long long>(ps.dropped), static_cast<unsigned long long>(ps.restarted));
	ImGui::Text("Last poll: %.2f ms on the main thread, %zu taken, %zu left for the next frame",
	            ps.lastPollMs, ps.lastPollHandled, ps.lastPollLeft);
	constexpr size_t kShown = 12;
	const std::vector<std::string> paths = cm.asyncInFlightPaths(kShown);
	for (const std::string& p : paths)
	{
		std::uint64_t read = 0, total = 0;
		if (cm.asyncProgress(p, read, total) && total > 0)
		{
			char overlay[64];
			std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f MB",
			              static_cast<double>(read) / 1048576.0, static_cast<double>(total) / 1048576.0);
			ImGui::ProgressBar(static_cast<float>(static_cast<double>(read) / static_cast<double>(total)),
			                   ImVec2(160.0f, 0.0f), overlay);
			ImGui::SameLine();
		}
		ImGui::TextUnformatted(p.c_str());
	}
	if (cm.asyncInFlightCount() > paths.size())
		ImGui::TextDisabled("…and %zu more", cm.asyncInFlightCount() - paths.size());
}

void drawScene(AppContext& ctx)
{
	ImGui::SeparatorText("Scene");
	if (ctx.world)
	{
		size_t n = 0;
		ctx.world->registry().view<entt::entity>().each([&](auto) { ++n; });
		ImGui::Text("Entities in the open scene: %zu", n);
	}
	const SceneSerializer::LoadTiming t = SceneSerializer::lastLoadTiming();
	if (t.path.empty())
	{
		ImGui::TextDisabled("No scene loaded from disk yet.");
		return;
	}
	EditorWidgets::WrapText wrap;
	ImGui::Text("Last load: %zu entities, %.0f ms parse + %.0f ms build (%s)",
	            t.entities, t.parseMs, t.buildMs, t.binary ? "binary" : "JSON");
	ImGui::TextDisabled("%s", t.path.c_str());
}

void drawCells(AppContext& ctx)
{
	ImGui::SeparatorText("Streaming cells");
	EditorWidgets::WrapText wrap;
	const HE::CellManifest* m = ctx.world ? manifestOf(*ctx.world) : nullptr;
	static std::string s_message;   // what the last split or merge said
	const bool editable = ctx.world && !ctx.isPlaying && !inCollabSession(ctx);
	if (!m)
	{
		if (ctx.world && !ctx.world->cellManifestJson().empty())
		{
			ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
			                   "The scene's cell list is malformed; the game loads only the base scene.");
			return;
		}
		ImGui::TextDisabled("This scene does not stream in cells: the game loads all of it at once. "
		                    "Splitting moves its placed meshes, point and spot lights, static bodies, "
		                    "decals and placed prefabs into one file per square; the game then loads "
		                    "the squares around the camera. Play in the editor shows only what stays "
		                    "in the scene.");
		static HE::CellSplitOptions s_options;
		EditorWidgets::Row::dragFloat("Cell size (m)##cellsplit", &s_options.cellSize, 8.0f, 16.0f, 100000.0f, "%.0f");
		EditorWidgets::Row::dragFloat("Load radius (m), 0 = 1.5 cells##cellsplit", &s_options.loadRadius, 8.0f,
		                              0.0f, 1.0e6f, "%.0f");
		EditorWidgets::Row::dragFloat("Unload radius (m), 0 = 1.25 x load##cellsplit", &s_options.unloadRadius,
		                              8.0f, 0.0f, 1.0e6f, "%.0f");
		EditorWidgets::Row::dragFloat("Look ahead (s)##cellsplit", &s_options.lookaheadSec, 0.1f, 0.0f, 30.0f, "%.1f");
		ImGui::BeginDisabled(!editable);
		if (EditorWidgets::button("Split into Streaming Cells")) splitOpenScene(ctx, s_options, s_message);
		ImGui::EndDisabled();
		if (!s_message.empty()) ImGui::TextUnformatted(s_message.c_str());
		return;
	}
	uint64_t inCells = 0;
	for (const HE::CellManifest::Cell& c : m->cells) inCells += c.entities;
	ImGui::Text("%zu cells of %.0f m with %llu entities, in %s", m->cells.size(), m->cellSize,
	            static_cast<unsigned long long>(inCells), m->dir.c_str());
	ImGui::Text("Load within %.0f m, unload beyond %.0f m, look ahead %.1f s",
	            m->loadRadius, m->unloadRadius, m->lookaheadSec);

	ViewportPanel::ShowFlags& flags = ViewportPanel::showFlags();
	EditorWidgets::checkbox("Show the cells in the Scene window", &flags.streamingCells);
	// The way to edit what is in the cells: back into one scene, edit, split again.
	ImGui::BeginDisabled(!editable);
	if (EditorWidgets::button("Merge Cells into the Scene")) mergeOpenScene(ctx, s_message);
	ImGui::EndDisabled();
	if (!s_message.empty()) ImGui::TextUnformatted(s_message.c_str());

	if (!ctx.editorCamera) return;
	const glm::dvec3 eye = glm::dvec3(ctx.editorCamera->position()) + ctx.world->origin();

	// Preview anchors: the game keeps a cell while ANY anchor is near it (the camera,
	// a player, a pin a script sets). The editor has one camera, so a pin here is a way
	// to look at what two places hold together; the Scene window draws them as well.
	std::vector<HE::CellAnchor>& pins = previewPins();
	if (EditorWidgets::button("Pin an anchor at the editor camera"))
	{
		HE::CellAnchor pin;
		pin.position = eye;
		pins.push_back(pin);
	}
	ImGui::BeginDisabled(pins.empty());
	ImGui::SameLine();
	if (EditorWidgets::button("Clear anchors")) pins.clear();
	ImGui::EndDisabled();
	if (!pins.empty())
		ImGui::Text("%zu preview anchor(s) besides the camera.", pins.size());

	std::vector<HE::CellAnchor> anchors;
	anchors.reserve(1 + pins.size());
	HE::CellAnchor cam;
	cam.position = eye;
	anchors.push_back(cam);
	anchors.insert(anchors.end(), pins.begin(), pins.end());
	const std::vector<HE::CellManifest::View> nearby = m->around(anchors, m->unloadRadius);
	size_t   loadCells = 0, keepCells = 0;
	uint64_t loadEnts = 0, keepEnts = 0;
	uint32_t mostSlices = 0;
	for (const HE::CellManifest::View& v : nearby)
	{
		if (v.reach == HE::CellManifest::View::Reach::Load) { ++loadCells; loadEnts += v.entities; }
		else if (v.reach == HE::CellManifest::View::Reach::Keep) { ++keepCells; keepEnts += v.entities; }
		mostSlices = std::max(mostSlices, v.slicesEstimate);
	}
	ImGui::Text("From %s the game would build %zu cells (%llu entities) and keep %zu more (%llu) once built.",
	            pins.empty() ? "the editor camera" : "the camera and the anchors", loadCells,
	            static_cast<unsigned long long>(loadEnts), keepCells, static_cast<unsigned long long>(keepEnts));
	ImGui::TextDisabled("A cell is built in slices of whole objects, up to %zu entities each, as many per frame as "
	                    "fit its budget; the largest cell in reach here takes about %u slices. It is dropped only "
	                    "once every anchor is beyond the unload radius.",
	                    HE::kDefaultCellSliceEntities, mostSlices);

	constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
	                                 | ImGuiTableFlags_SizingStretchProp;
	if (nearby.empty() || !ImGui::BeginTable("##cells", 5, kFlags)) return;
	for (const char* h : { "Cell", "Entities", "Slices", "Distance", "Game" }) ImGui::TableSetupColumn(h);
	ImGui::TableHeadersRow();
	constexpr size_t kRows = 16;
	for (size_t i = 0; i < nearby.size() && i < kRows; ++i)
	{
		const HE::CellManifest::View& v = nearby[i];
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::Text("%d, %d", v.x, v.z);
		ImGui::TableNextColumn(); ImGui::Text("%u", v.entities);
		ImGui::TableNextColumn(); ImGui::Text("~%u", v.slicesEstimate);
		ImGui::TableNextColumn(); ImGui::Text("%.0f m", v.distance);
		ImGui::TableNextColumn(); ImGui::TextUnformatted(reachName(v.reach));
	}
	ImGui::EndTable();
}

void drawWorldSize(AppContext& ctx)
{
	ImGui::SeparatorText("World size");
	EditorWidgets::WrapText wrap;
	if (ctx.projectManager && !ctx.projectManager->currentProject().path.empty())
	{
		const float radius = ctx.projectManager->currentProject().settings.physics.floatingOriginRadius;
		if (radius > 0.0f)
			ImGui::Text("Floating origin: the game moves the world back every %.0f m.", radius);
		else
			ImGui::TextDisabled("Floating origin is off (Project Settings > Physics > Simulation).");
	}
	if (ctx.world)
	{
		const glm::dvec3& o = ctx.world->origin();
		if (o != glm::dvec3(0.0))
			ImGui::Text("World origin: %.1f, %.1f, %.1f", o.x, o.y, o.z);
	}
	if (!ctx.editorCamera) return;
	// How finely a float can place something where the camera is: the gap to
	// the next representable value at the largest coordinate. At 10 km that is
	// a millimetre, at 100 km eight; past that, things visibly snap.
	const glm::vec3 p = ctx.editorCamera->position();
	const float farthest = std::max({ std::abs(p.x), std::abs(p.y), std::abs(p.z) });
	const float step = std::nextafter(farthest, INFINITY) - farthest;
	ImGui::Text("Editor camera at %.0f m from the origin; positions there move in steps of %.3g mm.",
	            static_cast<double>(glm::length(p)), static_cast<double>(step) * 1000.0);
}
} // namespace
#endif // HE_IMGUI_ENABLED

void draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	drawJobs();
	if (ctx.contentManager) drawAssets(*ctx.contentManager);
	drawScene(ctx);
	drawCells(ctx);
	drawWorldSize(ctx);
#else
	(void)ctx;
#endif
}

} // namespace StreamingDebugView
