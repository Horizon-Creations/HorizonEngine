#include "HorizonScene/LandscapeExport.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeEdit.h"
#include "HorizonScene/LandscapeRuntime.h"
#include "HorizonScene/Components/LandscapeWorldComponent.h"
#include "HorizonScene/Components/LandscapeStampComponent.h"
#include "HorizonScene/LandscapeStamps.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/TextureBlockDecode.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

#include <algorithm>
#include <fstream>

namespace HE
{

std::filesystem::path findLandscapePack(const std::string& pack, const std::filesystem::path& projectRoot)
{
	std::error_code ec;
	const std::filesystem::path p(pack);
	if (p.is_absolute()) return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path{};
	if (!projectRoot.empty())
		if (const std::filesystem::path q = projectRoot / p; std::filesystem::is_regular_file(q, ec)) return q;
	return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path{};
}

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

namespace
{
	// The mean of slice `slice` of the array at `path`: from the pack-time means if the asset carries them, else from its pixels.
	bool sliceMean(ContentManager& content, const char* path, uint32_t slice, float rgb[3])
	{
		if (!path || !*path) return false;
		const UUID id = content.idForPath(path);
		if (id == UUID{}) return false;
		content.ensureResident(id);
		const TextureAsset* t = content.getTexture(id);
		if (!t || slice >= t->layers) return false;
		if (t->sliceMeans.size() == static_cast<size_t>(t->layers) * 3)
		{
			for (int i = 0; i < 3; ++i) rgb[i] = t->sliceMeans[static_cast<size_t>(slice) * 3 + static_cast<size_t>(i)];
			return true;
		}
		return textureArraySliceMean(*t, slice, rgb);
	}

	// Bakes the far image of one pack (once an export), painting only the regions that are not in the cache as they are now.
	void bakeFarImage(LandscapeExportPack& pack, int textureGroup, ContentManager& content, const LandscapeExportOptions& options,
	                  LandscapeExportPlan& plan, const std::function<void(int, const std::string&)>& say)
	{
		std::string err;
		const auto source = LandscapePackSource::open(pack.source.string(), &err);
		if (!source) { say(1, "Landscape far image: not baked, the pack cannot be read (" + err + ")"); return; }

		const LandscapeFarPaint paint = landscapeFarPaintFor(textureGroup, content);
		LandscapeFarBakeOptions bo;
		bo.budgetBytes = options.farBudgetBytes;
		bo.painter = landscapeFarWeightPainter(paint);
		bo.painterHash = landscapeFarPaintHash(paint);
		bo.progress = options.farProgress;

		// The last export's table, else what the pack itself carries: both are only a cache, every region is checked by its hash.
		LandscapeFarTable previous;
		const std::filesystem::path cacheFile = options.farCacheDir.empty() ? std::filesystem::path{}
		                                                                    : options.farCacheDir / (pack.source.stem().generic_string() + ".far");
		std::vector<uint8_t> bytes;
		if (cacheFile.empty() || !readFile(cacheFile, bytes) || !LandscapeFarTable::decode(bytes, previous))
			if (source->farImage()) previous = *source->farImage();

		// The world as its author sees it: the pack with its edits (LandscapeEdit.h) on top. An edited tile hashes by its own bytes.
		LandscapeEditSource edited(source);
		edited.setWorldId(source->identity());
		// The stamps lie over both: a stamped tile is painted as it is shown and hashes by what it shows.
		const size_t editedTiles = (pack.editsFile.empty() ? 0 : edited.loadFile(pack.editsFile))
		                         + edited.setStamps(pack.stamps, 3000.0f, /*patchCoarse=*/false).size();
		const LandscapeFarTileHash packHash = landscapePackTileHash(*source);
		const LandscapeFarTileHash hash = editedTiles ? LandscapeFarTileHash([&](LandscapeTileKey k) { return edited.contentHash(k, packHash); }) : packHash;
		const LandscapeTileSource& world = editedTiles ? static_cast<const LandscapeTileSource&>(edited) : *source;

		LandscapeFarTable table;
		LandscapeFarBakeStats st;
		bakeLandscapeFar(world, hash, bo, previous.empty() ? nullptr : &previous, table, st);
		plan.farBake.regions += st.regions;
		plan.farBake.baked += st.baked;
		plan.farBake.reused += st.reused;
		plan.farBake.failed += st.failed;
		plan.farBake.cancelled = plan.farBake.cancelled || st.cancelled;
		if (st.cancelled) { say(1, "Landscape far image: cancelled after " + std::to_string(st.baked + st.reused) + " of " + std::to_string(st.regions) + " region(s), the pack ships without one"); return; }

		const std::vector<uint8_t> encoded = table.encode();
		if (!cacheFile.empty() && st.baked > 0)
		{
			// Beside, then renamed over: a crash leaves the old cache or the new one.
			std::error_code ec;
			std::filesystem::create_directories(cacheFile.parent_path(), ec);
			const std::filesystem::path tmp = cacheFile.string() + ".tmp";
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
			out.close();
			if (out) std::filesystem::rename(tmp, cacheFile, ec);
			if (!out || ec) say(1, "Landscape far image: the cache " + cacheFile.generic_string() + " could not be written, the next export paints everything again");
		}
		const float texelM = source->geometry().tileM / static_cast<float>(table.layout.texelsPerTile);
		say(0, "Landscape far image: " + std::to_string(st.regions) + " region(s) of " + std::to_string(table.layout.regionTiles) + " x "
		       + std::to_string(table.layout.regionTiles) + " tiles: " + std::to_string(st.baked) + " baked, " + std::to_string(st.reused) + " unchanged"
		       + (st.failed ? ", " + std::to_string(st.failed) + " failed" : std::string()) + "; " + std::to_string(table.layout.width()) + " x "
		       + std::to_string(table.layout.height()) + " px (" + std::to_string(static_cast<int>(texelM)) + " m a texel), "
		       + std::to_string(encoded.size() / 1024) + " KB in the pack");
		if (!table.empty()) pack.farTable = std::make_shared<LandscapeFarTable>(std::move(table));
	}
}

LandscapeFarPaint landscapeFarPaintFor(int textureGroup, ContentManager& content)
{
	LandscapeFarPaint paint;
	const std::array<glm::vec4, 12> flat = LandscapeRuntimeConfig::defaultLayerColors();
	for (size_t i = 0; i < 12; ++i) paint.layerColor[i] = glm::vec3(flat[i]);
	paint.layerAo.fill(1.0f);
	if (textureGroup < 0 || textureGroup >= kAutoLandscapeGroupCount) return paint;
	// The tile shader reads slices 0-3 of the group's albedo and mask arrays for layers 0-3 (kLandscapeFS). All four or none: a mix of
	// mean and flat colours would match neither state of the runtime.
	const AutoLandscapeGroup& grp = autoLandscapeGroup(static_cast<AutoLandscapeGroupId>(textureGroup));
	float albedo[4][3], mask[4][3];
	bool haveAlbedo = true, haveMask = true;
	for (uint32_t s = 0; s < 4; ++s)
	{
		haveAlbedo = haveAlbedo && sliceMean(content, grp.arrayPaths[0], s, albedo[s]);
		haveMask = haveMask && sliceMean(content, grp.arrayPaths[2], s, mask[s]);
	}
	if (haveAlbedo)
		for (size_t s = 0; s < 4; ++s)
		{
			paint.layerColor[s] = { albedo[s][0], albedo[s][1], albedo[s][2] };
			if (haveMask) paint.layerAo[s] = mask[s][0];   // R = AO
		}
	return paint;
}

size_t prepareLandscapePacksForExport(HorizonWorld& world, const std::filesystem::path& projectRoot, ContentManager& content,
                                      const LandscapeExportOptions& options, LandscapeExportPlan& plan, const LandscapeExportLog& log)
{
	const auto say = [&](int level, const std::string& text) { if (log) log(level, text); };
	size_t rewritten = 0;

	CellManifest manifest;
	const bool hasCells = options.bakeHlod && !world.cellManifestJson().empty() && CellManifest::parse(world.cellManifestJson(), manifest);

	auto& reg = world.registry();
	bool stampsTaken = false;   // this scene's stamps went to its first pack
	for (auto e : reg.view<LandscapeWorldComponent>())
	{
		LandscapeWorldComponent& lw = reg.get<LandscapeWorldComponent>(e);
		if (!lw.enabled || lw.pack.empty()) continue;

		const std::filesystem::path source = findLandscapePack(lw.pack, projectRoot);
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
			// Sculpted and painted tiles live beside the pack (LandscapeEdit.h); the shipped copy gets them baked in, so a game needs no second file.
			std::error_code ec;
			if (const std::filesystem::path edits = landscapeEditsFile(source); std::filesystem::is_regular_file(edits, ec))
			{
				p.editsFile = edits;
				say(0, "Landscape edits: " + edits.filename().generic_string() + " is baked into the shipped pack");
			}
			plan.packs.push_back(std::move(p));
			it = plan.packs.end() - 1;
		}

		// The scene's stamps (LandscapeStamps.h) go into the shipped pack like brush edits. Taken BEFORE the far image is painted:
		// it is made from the world as its author sees it, stamps included.
		// shortcut: a second scene that adds stamps to a pack whose far image an earlier scene baked gets them into the tiles only
		if (!stampsTaken)
		{
			stampsTaken = true;
			LandscapeStampImages images;
			std::vector<LandscapeStamp> stamps = landscapeStampsOfWorld(world, content, images);
			if (!stamps.empty())
			{
				say(0, "Landscape stamps: " + std::to_string(stamps.size()) + " baked into the shipped pack");
				it->stamps.insert(it->stamps.end(), std::make_move_iterator(stamps.begin()), std::make_move_iterator(stamps.end()));
			}
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

		if (options.bakeFar && !it->farDone)
		{
			it->farDone = true;
			bakeFarImage(*it, lw.textureGroup, content, options, plan, say);
		}

		if (lw.pack != it->dest)
		{
			lw.pack = it->dest;
			++rewritten;
		}
	}
	// Out of the shipped scene: a game that found the stamps would lay them over the baked ground a second time.
	if (stampsTaken && !reg.view<LandscapeStampComponent>().empty()) { reg.clear<LandscapeStampComponent>(); ++rewritten; }
	return rewritten;
}

bool finishExportedPack(const LandscapeExportPack& pack, const std::filesystem::path& copied, std::string* error)
{
	// The edits first: the tiles of the copy become the edited ones (the overlay reads the PROJECT's pack, the copy is what is rewritten).
	if (!pack.editsFile.empty() || !pack.stamps.empty())
	{
		std::string err;
		auto base = LandscapePackSource::open(pack.source.string(), &err);
		if (!base) { if (error) *error = "landscape edits: " + err; return false; }
		const uint64_t world = base->identity();
		LandscapeEditSource edits(std::move(base));
		edits.setWorldId(world);
		const size_t loaded = pack.editsFile.empty() ? 0 : edits.loadFile(pack.editsFile);
		edits.setStamps(pack.stamps, 3000.0f);   // a pack says its own height range; the number is for a source that does not
		if ((loaded > 0 || edits.stampCount() > 0) && !landscapePackBakeEdits(copied.string(), edits, error)) return false;
	}
	const bool hlod = pack.hlod && pack.hlod->cellCount() > 0;
	const bool farImage = pack.farTable && !pack.farTable->empty();
	if (!hlod && !farImage) return true;
	// One rewrite of the copy for both sections. What was not baked in this export is left as the project's pack has it.
	return landscapePackSetTails(copied.string(), hlod ? pack.hlod.get() : nullptr, farImage ? pack.farTable.get() : nullptr, error);
}

} // namespace HE
