#include "HorizonScene/LandscapeExport.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/LandscapeWorldComponent.h"
#include <ContentManager/ContentManager.h>

#include <algorithm>
#include <fstream>

namespace HE
{

namespace
{
	bool readFile(const std::filesystem::path& file, std::vector<uint8_t>& out)
	{
		std::ifstream in(file, std::ios::binary | std::ios::ate);
		if (!in) return false;
		const std::streamoff size = in.tellg();
		if (size < 0) return false;
		out.resize(static_cast<size_t>(size));
		in.seekg(0);
		if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
		return static_cast<bool>(in);
	}

	// The file the scene means. The runtime opens the string as it is (the editor's working directory), the editor stores whatever the
	// author picked, so an absolute path, one under the project and one as given are all tried, in that order of how likely they are meant.
	std::filesystem::path findPack(const std::string& pack, const std::filesystem::path& projectRoot)
	{
		std::error_code ec;
		const std::filesystem::path p(pack);
		if (p.is_absolute()) return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path{};
		if (!projectRoot.empty())
			if (const std::filesystem::path q = projectRoot / p; std::filesystem::is_regular_file(q, ec)) return q;
		return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path{};
	}

	std::string sameFile(const std::filesystem::path& p)
	{
		std::error_code ec;
		const std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
		return (ec ? p : c).generic_string();
	}

	// "Landscape/<file name>"; two packs of the same file name in different folders get a number.
	std::string destinationFor(const std::filesystem::path& source, const LandscapeExportPlan& plan)
	{
		const std::string stem = source.stem().generic_string();
		const std::string ext = source.extension().generic_string();
		for (int n = 0;; ++n)
		{
			const std::string dest = "Landscape/" + stem + (n ? "_" + std::to_string(n) : std::string()) + ext;
			const bool taken = std::any_of(plan.packs.begin(), plan.packs.end(), [&](const LandscapeExportPack& q) { return q.dest == dest; });
			if (!taken) return dest;
		}
	}

	void sum(CellHlodBakeStats& into, const CellHlodBakeStats& s)
	{
		into.cells += s.cells;
		into.baked += s.baked;
		into.empty += s.empty;
		into.failed += s.failed;
		into.instances += s.instances;
		into.instancesDropped += s.instancesDropped;
		into.meshesMissing += s.meshesMissing;
		into.triangles += s.triangles;
		into.bytes += s.bytes;
	}
}

size_t prepareLandscapePacksForExport(HorizonWorld& world, const std::filesystem::path& projectRoot, ContentManager& content,
                                      const LandscapeExportOptions& options, LandscapeExportPlan& plan, const LandscapeExportLog& log)
{
	const auto say = [&](int level, const std::string& text) { if (log) log(level, text); };
	size_t rewritten = 0;

	CellManifest manifest;
	const bool hasCells = options.bakeHlod && !world.cellManifestJson().empty() && CellManifest::parse(world.cellManifestJson(), manifest);

	auto& reg = world.registry();
	for (auto e : reg.view<LandscapeWorldComponent>())
	{
		LandscapeWorldComponent& lw = reg.get<LandscapeWorldComponent>(e);
		if (!lw.enabled || lw.pack.empty()) continue;

		const std::filesystem::path source = findPack(lw.pack, projectRoot);
		if (source.empty())
		{
			say(1, "Landscape pack '" + lw.pack + "' was not found (beside the project or as written): the scene keeps naming it and the game will "
			       "not find it either");
			continue;
		}

		const std::string key = sameFile(source);
		auto it = std::find_if(plan.packs.begin(), plan.packs.end(), [&](const LandscapeExportPack& q) { return sameFile(q.source) == key; });
		if (it == plan.packs.end())
		{
			LandscapeExportPack p;
			p.source = source;
			p.dest = destinationFor(source, plan);
			plan.packs.push_back(std::move(p));
			it = plan.packs.end() - 1;
		}

		if (hasCells && !it->hlodDone)
		{
			it->hlodDone = true;
			CellHlodBakeStats st;
			LandscapeHlodTable table = bakeCellProxies(
				manifest,
				[&projectRoot](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)> {
					const std::filesystem::path file = projectRoot / path;
					std::error_code ec;
					if (!std::filesystem::is_regular_file(file, ec)) return {};
					return [file](std::vector<uint8_t>& out) { return readFile(file, out); };
				},
				content, options.bakeOptions, &st);
			sum(plan.bake, st);
			if (table.cellCount() > 0) it->hlod = std::make_shared<LandscapeHlodTable>(std::move(table));
			say(0, "Landscape HLOD: " + std::to_string(st.baked) + " of " + std::to_string(st.cells) + " cell(s) baked ("
			       + std::to_string(st.instances) + " mesh instance(s), " + std::to_string(st.triangles) + " triangle(s), "
			       + std::to_string(st.bytes / 1024) + " KB)"
			       + (st.failed ? ", " + std::to_string(st.failed) + " cell file(s) unreadable" : std::string())
			       + (st.meshesMissing ? ", " + std::to_string(st.meshesMissing) + " mesh asset(s) missing" : std::string()));
		}

		if (lw.pack != it->dest)
		{
			lw.pack = it->dest;
			++rewritten;
		}
	}
	return rewritten;
}

size_t resolveLandscapePacks(HorizonWorld& world, const std::filesystem::path& dataDir)
{
	if (dataDir.empty()) return 0;
	size_t changed = 0;
	auto& reg = world.registry();
	for (auto e : reg.view<LandscapeWorldComponent>())
	{
		LandscapeWorldComponent& lw = reg.get<LandscapeWorldComponent>(e);
		if (lw.pack.empty()) continue;
		const std::filesystem::path p(lw.pack);
		if (p.is_absolute()) continue;
		std::error_code ec;
		const std::filesystem::path inData = dataDir / p;
		if (!std::filesystem::is_regular_file(inData, ec)) continue;
		lw.pack = inData.string();
		++changed;
	}
	return changed;
}

bool finishExportedPack(const LandscapeExportPack& pack, const std::filesystem::path& copied, std::string* error)
{
	if (!pack.hlod || pack.hlod->cellCount() == 0) return true;
	return landscapePackSetHlod(copied.string(), *pack.hlod, error);
}

} // namespace HE
