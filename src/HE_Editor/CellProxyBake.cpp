#include "CellProxyBake.h"

#include "EditorApplication.h"   // AppContext
#include "ProjectManager.h"
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>
#include <Diagnostics/Profiler.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeCellHlodBake.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

namespace CellProxyBake
{
namespace
{
	namespace fs = std::filesystem;

	constexpr double kBudgetMs   = 2.0;    // the main thread's content work a frame (mesh and material reads)
	constexpr double kCheckEvery = 0.5;    // s between looks at the scene's cell list and the cache's identity
	constexpr double kPollEvery  = 10.0;   // s between the Automatic checks of the cell files (a stat a cell, on a worker)

	struct Load  { std::atomic<bool> done{ false }; HE::CellHlodCache cache; };
	struct Write { std::atomic<bool> done{ false }; };

	HE::CellHlodCache     s_cache;         // main thread's; a worker reads it only while s_write runs, and nothing changes it then
	HE::CellHlodBakeJob   s_job;
	std::shared_ptr<Load>  s_load;
	std::shared_ptr<Write> s_write;
	bool                  s_loaded = false;   // s_cache is what the file had (or nothing yet: a first bake)
	std::string           s_manifestText;
	HE::CellManifest      s_manifest;
	bool                  s_hasCells = false;
	bool                  s_manual = false;   // the running bake was asked for by the button
	fs::path              s_cacheFile;
	fs::path              s_witnessRoot;   // setWitnessRoot
	double                s_nextCheck = 0.0, s_nextPoll = 0.0, s_started = 0.0;
	size_t                s_stampsBefore = 0;
	std::string           s_summary;

	double now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	fs::path projectRoot(const AppContext& ctx)
	{
		if (!s_witnessRoot.empty()) return s_witnessRoot;
		// The folder above the content root, where the cells' paths are relative to (PlayCellStreaming::openCellFile).
		const std::string& content = ctx.contentManager->contentRoot();
		return content.empty() ? fs::path{} : fs::path(content).parent_path();
	}

	fs::path cacheFileFor(const AppContext& ctx, const HE::CellManifest& m)
	{
		fs::path p = s_witnessRoot;
		if (p.empty())
		{
			p = ctx.projectManager->currentProject().path;
			if (fs::is_regular_file(p)) p = p.parent_path();
		}
		std::string name = m.dir;
		std::replace(name.begin(), name.end(), '/', '_');
		return p / "Saved" / "LandscapeHlod" / (name + ".hlod");
	}

	bool usable(const AppContext& ctx)
	{
		if (!ctx.world || !ctx.contentManager) return false;
		return !s_witnessRoot.empty() || (ctx.projectLoaded && ctx.projectManager && !projectRoot(ctx).empty());
	}

	void forget()
	{
		s_job.cancel();
		// The write reads s_cache: it is over before s_cache is touched again.
		while (s_write && !s_write->done.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		s_write.reset();
		s_load.reset();   // a load in flight fills its own Load, nothing of ours
		s_cache = {};
		s_loaded = false;
		s_cacheFile.clear();
		s_nextPoll = 0.0;
	}

	// The scene's cell list, parsed when its text changed. False: the scene has none.
	bool refreshManifest(const AppContext& ctx)
	{
		const std::string& text = ctx.world->cellManifestJson();
		if (text != s_manifestText)
		{
			s_manifestText = text;
			s_hasCells = !text.empty() && HE::CellManifest::parse(text, s_manifest) && !s_manifest.empty();
		}
		return s_hasCells;
	}

	void startWrite()
	{
		auto w = s_write = std::make_shared<Write>();
		globalPool().post([w, cache = &s_cache, file = s_cacheFile]
		{
			std::error_code ec;
			fs::create_directories(file.parent_path(), ec);
			const std::vector<uint8_t> bytes = cache->encode();
			std::ofstream(file, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()),
			                                                              static_cast<std::streamsize>(bytes.size()));
			w->done.store(true, std::memory_order_release);
		}, "CellHlodWrite", 1, HE::JobPriority::Low);
	}

	void startLoad()
	{
		auto l = s_load = std::make_shared<Load>();
		globalPool().post([l, file = s_cacheFile]
		{
			std::ifstream in(file, std::ios::binary);
			if (in)
			{
				const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				if (!HE::CellHlodCache::decode(bytes, l->cache)) l->cache = {};
			}
			l->done.store(true, std::memory_order_release);
		}, "CellHlodLoad", 1, HE::JobPriority::Low);
	}

	void startBake(const AppContext& ctx, bool manual)
	{
		const fs::path root = projectRoot(ctx);
		s_manual = manual;
		s_stampsBefore = s_cache.stamps.size();
		s_started = now();
		s_job.start(s_manifest, HE::cellFileReader(root), HE::cellFileStamper(root), {}, s_cache);
	}

	void finishBake()
	{
		const HE::CellHlodBakeStats& st = s_job.stats();
		// Nothing changed: no write, and an Automatic check says nothing (the button's answer is "up to date").
		const bool changed = st.reused != st.cells || s_cache.stamps.size() != s_stampsBefore;
		if (!changed && !s_manual) return;
		char line[240];
		std::snprintf(line, sizeof line, "Cell proxies: %zu cell(s) baked, %zu without static meshes, %zu unreadable, %zu kept as they were; %zu triangles, %zu KB, in %.1f s.",
		              st.baked, st.empty, st.failed, st.reused, st.triangles, st.bytes / 1024, now() - s_started);
		s_summary = line;
		HE_LOG_INFO(Editor, "CellProxyBake: %s", line);
		if (changed) startWrite();
	}
}

void tick(AppContext& ctx, bool userBusy)
{
	const int mode = ctx.editorConfig.LandscapeFarBake;
	if (mode == 0) { s_job.cancel(); return; }
	if (s_job.active())
	{
		HE_PROFILE_SCOPE_N("CellProxyBake");
		s_job.pump(*ctx.contentManager, kBudgetMs);
		if (s_job.finished()) finishBake();
		return;
	}
	const double t = now();
	if (t < s_nextCheck) return;
	s_nextCheck = t + kCheckEvery;
	if (!usable(ctx) || !refreshManifest(ctx)) { if (s_loaded || s_load) forget(); return; }
	if (const fs::path file = cacheFileFor(ctx, s_manifest); file != s_cacheFile) { forget(); s_cacheFile = file; }
	if (s_write && s_write->done.load(std::memory_order_acquire)) s_write.reset();
	if (!s_loaded)
	{
		if (!s_load) startLoad();
		else if (s_load->done.load(std::memory_order_acquire)) { s_cache = std::move(s_load->cache); s_load.reset(); s_loaded = true; }
		return;
	}
	if (mode == 2 && !userBusy && !s_write && t >= s_nextPoll)
	{
		s_nextPoll = t + kPollEvery;
		startBake(ctx, false);
	}
}

bool bakeNow(AppContext& ctx)
{
	if (ctx.editorConfig.LandscapeFarBake == 0 || !usable(ctx) || !s_hasCells || !s_loaded || s_job.active() || s_write) return false;
	startBake(ctx, true);
	s_summary = "Cell proxies: baking...";
	return true;
}

const HE::LandscapeHlodTable* table(int farBakeMode)
{
	return farBakeMode == 0 || !s_loaded || s_cache.table.cellCount() == 0 ? nullptr : &s_cache.table;
}

void setWitnessRoot(const std::filesystem::path& root) { s_witnessRoot = root; }

bool baking() { return s_job.active(); }
size_t cellsLeft() { return s_job.cellsLeft(); }
const std::string& summary() { return s_summary; }

void reset()
{
	forget();
	s_manifestText.clear();
	s_manifest = {};
	s_hasCells = false;
	s_summary.clear();
	s_nextCheck = 0.0;
}
}
