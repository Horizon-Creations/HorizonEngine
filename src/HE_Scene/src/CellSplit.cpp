#include "HorizonScene/CellSplit.h"
#include "HorizonScene/CellStreamer.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/SceneJsonParse.h"
#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using json = nlohmann::json;

namespace HE
{

namespace
{
// Components a placed thing may carry and still move into a cell — the
// splitter script's MOVABLE set. Anything else keeps its subtree in the base.
bool movableKey(const std::string& k)
{
	static const std::unordered_set<std::string> kMovable = {
		"transform", "mesh", "material", "light", "lod", "collider", "rigidbody", "decal", "inactive",
	};
	return kMovable.count(k) != 0;
}

// An entity reference ([hi, lo], or a legacy number) as a map key.
std::string idKey(const json& v) { return v.dump(); }

const json* componentsOf(const json& e)
{
	const auto it = e.find("components");
	return it != e.end() && it->is_object() ? &*it : nullptr;
}

const json* childrenOf(const json& e)
{
	const auto it = e.find("children");
	return it != e.end() && it->is_array() ? &*it : nullptr;
}

bool movableEntity(const json& e)
{
	const json* comps = componentsOf(e);
	if (!comps) return true;
	for (auto it = comps->begin(); it != comps->end(); ++it)
		if (!movableKey(it.key())) return false;
	if (const auto l = comps->find("light"); l != comps->end() && l->is_object()
	    && l->value("type", 0) == 0)
		return false;   // directional: lights the whole world
	if (const auto b = comps->find("rigidbody"); b != comps->end() && b->is_object()
	    && b->value("type", 0) != 0)
		return false;   // dynamic: would unload with the square it started in
	return true;
}

bool nearly(const json& arr, double want)
{
	if (!arr.is_array()) return false;
	for (const json& v : arr)
		if (!v.is_number() || std::abs(v.get<double>() - want) >= 1e-9) return false;
	return true;
}

bool isFolder(const json& e)
{
	const json* kids = childrenOf(e);
	if (!kids || kids->empty()) return false;
	const json* comps = componentsOf(e);
	if (!comps || comps->empty()) return true;
	if (comps->size() != 1 || !comps->contains("transform")) return false;
	const json& t = (*comps)["transform"];
	const auto field = [&t](const char* k, const json& fallback) -> const json&
	{
		const auto it = t.find(k);
		return it != t.end() ? *it : fallback;
	};
	static const json kZero = json::array({ 0, 0, 0 });
	static const json kOne  = json::array({ 1, 1, 1 });
	return nearly(field("position", kZero), 0.0) && nearly(field("rotation", kZero), 0.0)
	    && nearly(field("scale", kOne), 1.0);
}
} // namespace

CellSplitResult splitSceneIntoCells(const json& scene, const CellSplitOptions& options)
{
	CellSplitResult out;
	if (!(options.cellSize > 0.0f) || !std::isfinite(options.cellSize))
	{
		out.error = "the cell size must be above zero";
		return out;
	}
	if (options.dir.empty())
	{
		out.error = "no folder for the cell files";
		return out;
	}
	const auto ents = scene.find("entities");
	if (!scene.is_object() || ents == scene.end() || !ents->is_array())
	{
		out.error = "the scene has no entity list";
		return out;
	}
	if (scene.contains("cells"))
	{
		out.error = "the scene is split already; merge its cells first";
		return out;
	}

	std::unordered_map<std::string, const json*> byId;
	byId.reserve(ents->size());
	for (const json& e : *ents)
		if (e.is_object() && e.contains("uuid")) byId[idKey(e["uuid"])] = &e;
	std::vector<const json*> roots;
	for (const json& e : *ents)
	{
		if (!e.is_object() || !e.contains("uuid")) continue;
		const auto p = e.find("parent");
		if (p == e.end() || p->is_null() || !byId.count(idKey(*p))) roots.push_back(&e);
	}
	if (roots.size() != 1)
	{
		out.error = "expected one scene root, found " + std::to_string(roots.size());
		return out;
	}

	// Whole subtrees only: one unmovable entity anywhere below keeps the lot.
	std::unordered_map<std::string, bool> movableMemo;
	std::function<bool(const json&)> subtreeMovable = [&](const json& e) -> bool
	{
		const std::string k = idKey(e["uuid"]);
		if (const auto it = movableMemo.find(k); it != movableMemo.end()) return it->second;
		bool ok = movableEntity(e);
		if (ok)
			if (const json* kids = childrenOf(e))
				for (const json& c : *kids)
					if (const auto it = byId.find(idKey(c)); it != byId.end() && !subtreeMovable(*it->second))
					{
						ok = false;
						break;
					}
		movableMemo[k] = ok;
		return ok;
	};

	const double size = options.cellSize;
	std::map<std::pair<int, int>, std::vector<std::string>> units;   // ordered: files come out sorted
	std::unordered_set<std::string> moved;
	// The folder a moved subtree hung in, when not the scene root: the merge
	// puts it back there (and rebuilds the folder when the split dropped it).
	std::unordered_map<std::string, std::string> unitFolder;
	const std::string rootKey = idKey((*roots[0])["uuid"]);
	std::function<void(const json&)> visit = [&](const json& parent)
	{
		const json* kids = childrenOf(parent);
		if (!kids) return;
		const std::string parentKey = idKey(parent["uuid"]);
		for (const json& c : *kids)
		{
			const auto it = byId.find(idKey(c));
			if (it == byId.end()) continue;
			const json& child = *it->second;
			if (isFolder(child))
			{
				visit(child);
				continue;
			}
			if (parentKey != rootKey && subtreeMovable(child)) unitFolder[idKey(child["uuid"])] = parentKey;
			if (!subtreeMovable(child)) continue;
			double px = 0.0, pz = 0.0;
			if (const json* comps = componentsOf(child))
				if (const auto t = comps->find("transform"); t != comps->end() && t->is_object())
					if (const auto pos = t->find("position");
					    pos != t->end() && pos->is_array() && pos->size() >= 3
					    && (*pos)[0].is_number() && (*pos)[2].is_number())
					{
						px = (*pos)[0].get<double>();
						pz = (*pos)[2].get<double>();
					}
			const std::pair<int, int> cell{ CellManifest::cellIndex(px, options.cellSize),
			                                CellManifest::cellIndex(pz, options.cellSize) };
			const std::string k = idKey(child["uuid"]);
			units[cell].push_back(k);
			moved.insert(k);
		}
	};
	visit(*roots[0]);

	// Everything that leaves: the moved subtrees, whole.
	std::unordered_set<std::string> gone;
	std::function<void(const std::string&)> takeSubtree = [&](const std::string& k)
	{
		if (!gone.insert(k).second) return;
		if (const json* kids = childrenOf(*byId[k]))
			for (const json& c : *kids)
				if (byId.count(idKey(c))) takeSubtree(idKey(c));
	};
	for (const std::string& k : moved) takeSubtree(k);

	// What each staying entity still has as children; then folders that had
	// children and have none left, bottom-up.
	std::unordered_map<std::string, std::vector<json>> childrenLeft;
	for (const json& e : *ents)
	{
		if (!e.is_object() || !e.contains("uuid")) continue;
		const std::string k = idKey(e["uuid"]);
		if (gone.count(k)) continue;
		std::vector<json>& left = childrenLeft[k];
		if (const json* kids = childrenOf(e))
			for (const json& c : *kids)
				if (!gone.count(idKey(c))) left.push_back(c);
	}
	for (bool changed = true; changed;)
	{
		changed = false;
		for (const json& e : *ents)
		{
			if (!e.is_object() || !e.contains("uuid") || &e == roots[0]) continue;
			const std::string k = idKey(e["uuid"]);
			if (gone.count(k)) continue;
			const json* kids = childrenOf(e);
			if (!kids || kids->empty() || !childrenLeft[k].empty() || !isFolder(e)) continue;
			gone.insert(k);
			changed = true;
			if (const auto p = e.find("parent"); p != e.end() && !p->is_null())
				if (const auto pl = childrenLeft.find(idKey(*p)); pl != childrenLeft.end())
				{
					auto& v = pl->second;
					v.erase(std::remove_if(v.begin(), v.end(),
					                       [&k](const json& c) { return idKey(c) == k; }),
					        v.end());
				}
		}
	}

	// The base: the scene as it was, minus what left.
	out.base = json::object();
	for (auto it = scene.begin(); it != scene.end(); ++it)
		if (it.key() != "entities") out.base[it.key()] = it.value();
	json baseEntities = json::array();
	for (const json& e : *ents)
	{
		if (!e.is_object() || !e.contains("uuid"))
		{
			baseEntities.push_back(e);
			continue;
		}
		const std::string k = idKey(e["uuid"]);
		if (gone.count(k)) continue;
		json copy = e;
		if (e.contains("children")) copy["children"] = childrenLeft[k];
		baseEntities.push_back(std::move(copy));
	}
	out.base["entities"] = std::move(baseEntities);

	// One scene per cell: a root named after it, the subtrees under it.
	json listing = json::array();
	for (const auto& [cell, list] : units)
	{
		const auto [x, z] = cell;
		const uint64_t lo = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
		                  | static_cast<uint64_t>(static_cast<uint32_t>(z));
		const json rootId = json::array({ 0xCE11000000000000ULL, lo });
		json rootKids = json::array();
		for (const std::string& k : list) rootKids.push_back((*byId[k])["uuid"]);
		json cellEntities = json::array();
		cellEntities.push_back({ { "children", rootKids }, { "components", json::object() },
		                         { "name", "Cell " + std::to_string(x) + "," + std::to_string(z) },
		                         { "parent", nullptr }, { "uuid", rootId } });
		std::function<void(const std::string&, const json&)> add = [&](const std::string& k, const json& parentId)
		{
			json e = *byId[k];
			e["parent"] = parentId;
			const json id = e["uuid"];
			cellEntities.push_back(e);
			if (const json* kids = childrenOf(*byId[k]))
				for (const json& c : *kids)
					if (byId.count(idKey(c))) add(idKey(c), id);
		};
		for (const std::string& k : list) add(k, rootId);
		CellSplitResult::Cell c;
		c.x        = x;
		c.z        = z;
		c.entities = static_cast<uint32_t>(cellEntities.size() - 1);
		c.scene    = { { "entities", std::move(cellEntities) },
		               { "version", scene.value("version", std::string("1.1")) } };
		// For the merge only (the game's loader reads no such key): which folder
		// each subtree came from, and those folders as they were, up to the
		// scene root — the split may have dropped them from the base.
		json parents = json::array(), folders = json::array();
		std::unordered_set<std::string> folderSeen;
		for (const std::string& k : list)
		{
			const auto f = unitFolder.find(k);
			if (f == unitFolder.end()) continue;
			parents.push_back(json::array({ (*byId[k])["uuid"], (*byId[f->second])["uuid"] }));
			for (std::string up = f->second; up != rootKey && byId.count(up) && folderSeen.insert(up).second;)
			{
				const json& rec = *byId[up];
				folders.push_back(rec);
				const auto p = rec.find("parent");
				if (p == rec.end() || p->is_null()) break;
				up = idKey(*p);
			}
		}
		if (!parents.empty())
			c.scene["cellFolders"] = { { "parents", std::move(parents) }, { "folders", std::move(folders) } };
		out.moved += c.entities;
		listing.push_back(json::array({ x, z, c.entities }));
		out.cells.push_back(std::move(c));
	}

	const float load   = options.loadRadius > 0.0f ? options.loadRadius : 1.5f * options.cellSize;
	const float unload = options.unloadRadius > 0.0f ? std::max(options.unloadRadius, load) : 1.25f * load;
	out.base["cells"] = { { "cellSize", options.cellSize }, { "loadRadius", load },
	                      { "unloadRadius", unload }, { "lookaheadSec", options.lookaheadSec },
	                      { "dir", options.dir }, { "list", std::move(listing) } };
	return out;
}

CellSplitResult splitWorldIntoCells(HorizonWorld& world, const CellSplitOptions& options,
                                    const std::function<bool(const std::string&, const std::string&)>& write)
{
	CellSplitResult result;
	SceneSerializer ser;
	std::vector<uint8_t> snapshot;
	if (!ser.saveToMemory(world, snapshot))
	{
		result.error = "the scene could not be serialized";
		return result;
	}
	const json scene = parseSceneCbor(snapshot);
	if (scene.is_discarded())
	{
		result.error = "the scene could not be read back";
		return result;
	}
	result = splitSceneIntoCells(scene, options);
	if (!result.error.empty()) return result;
	if (result.cells.empty())
	{
		result.error = "nothing in the scene can stream: no placed meshes, lights or static bodies "
		               "outside the base";
		return result;
	}

	CellManifest manifest;
	CellManifest::parse(result.base["cells"].dump(), manifest);
	for (const CellSplitResult::Cell& c : result.cells)
		if (!write || !write(manifest.cellPath(c.x, c.z), c.scene.dump()))
		{
			result.error = "could not write " + manifest.cellPath(c.x, c.z);
			return result;
		}

	// The world becomes the base. The CBOR round trip is the undo system's own
	// path, so what the editor holds now is exactly what it would load.
	world.clear();
	if (!ser.loadFromMemory(world, json::to_cbor(result.base)))
	{
		// Put the whole scene back rather than leave half of it.
		world.clear();
		ser.loadFromMemory(world, snapshot);
		result.error = "the base scene did not load";
		return result;
	}
	HE_LOG_INFO(World, "Split into %zu streaming cell(s) of %.0f m: %zu entities moved to %s",
	            result.cells.size(), options.cellSize, result.moved, options.dir.c_str());
	return result;
}

bool mergeCellsIntoWorld(HorizonWorld& world,
                         const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read,
                         std::string* error, size_t* mergedEntities)
{
	const auto fail = [error](std::string why)
	{
		if (error) *error = std::move(why);
		return false;
	};
	CellManifest manifest;
	if (world.cellManifestJson().empty() || !CellManifest::parse(world.cellManifestJson(), manifest))
		return fail("the scene has no streaming cells");

	// Every cell read and parsed before the world changes: a merge that stops
	// half-way would leave a scene that is neither split nor whole.
	std::vector<json> scenes;
	scenes.reserve(manifest.cells.size());
	for (const CellManifest::Cell& c : manifest.cells)
	{
		const std::string path = manifest.cellPath(c.x, c.z);
		std::vector<uint8_t> bytes;
		if (!read || !read(path, bytes) || bytes.empty()) return fail("could not read " + path);
		const auto first = std::find_if(bytes.begin(), bytes.end(),
		                                [](uint8_t b) { return b != ' ' && b != '\t' && b != '\n' && b != '\r'; });
		json scene = first != bytes.end() && *first == '{'
		           ? parseSceneText(std::string(bytes.begin(), bytes.end()))
		           : parseSceneCbor(bytes);
		if (scene.is_discarded() || !scene.is_object()) return fail(path + " is not a scene");
		scenes.push_back(std::move(scene));
	}

	// Merged in the scene's JSON and loaded whole, not added entity by entity:
	// an additive load mints new ids (SceneSerializer, applyAdditiveJson), and
	// a merge has to give every entity back the identity it had before the
	// split, or whatever in the base refers to it (a joint, a script) would
	// point at nothing. The full load restores the stored ids.
	SceneSerializer ser;
	std::vector<uint8_t> snapshot;
	if (!ser.saveToMemory(world, snapshot)) return fail("the scene could not be serialized");
	json scene = parseSceneCbor(snapshot);
	if (scene.is_discarded() || !scene.contains("entities") || !scene["entities"].is_array())
		return fail("the scene could not be read back");
	// Indices, not pointers, into the entity array: it grows below.
	json& ents = scene["entities"];
	std::unordered_map<std::string, size_t> at;
	size_t rootAt = SIZE_MAX;
	for (size_t i = 0; i < ents.size(); ++i)
	{
		json& e = ents[i];
		if (!e.is_object() || !e.contains("uuid")) continue;
		at[idKey(e["uuid"])] = i;
		if (rootAt == SIZE_MAX && (!e.contains("parent") || e["parent"].is_null())) rootAt = i;
	}
	if (rootAt == SIZE_MAX) return fail("the scene has no root");
	const auto adopt = [&ents](size_t parent, const json& childId)
	{
		json& p = ents[parent];
		if (!p.contains("children") || !p["children"].is_array()) p["children"] = json::array();
		p["children"].push_back(childId);
	};

	// The folders the subtrees came from (cellFolders, written by the split):
	// where each went, and the folders as they were.
	std::unordered_map<std::string, json> unitFolder;     // subtree → its folder's uuid
	std::unordered_map<std::string, json> folderRecord;   // folder → its record before the split
	for (const json& cell : scenes)
		if (const auto cf = cell.find("cellFolders"); cf != cell.end() && cf->is_object())
		{
			if (const auto p = cf->find("parents"); p != cf->end() && p->is_array())
				for (const json& pair : *p)
					if (pair.is_array() && pair.size() == 2) unitFolder[idKey(pair[0])] = pair[1];
			if (const auto f = cf->find("folders"); f != cf->end() && f->is_array())
				for (const json& rec : *f)
					if (rec.is_object() && rec.contains("uuid")) folderRecord[idKey(rec["uuid"])] = rec;
		}
	// A folder that is still in the base is used as it is; one the split
	// dropped comes back from its record, under its own parent first.
	std::function<size_t(const std::string&, int)> ensureFolder = [&](const std::string& key, int depth) -> size_t
	{
		if (const auto it = at.find(key); it != at.end()) return it->second;
		const auto rec = folderRecord.find(key);
		if (rec == folderRecord.end() || depth > 64) return rootAt;
		const json& up = rec->second.contains("parent") ? rec->second["parent"] : json();
		const size_t parent = up.is_null() ? rootAt : ensureFolder(idKey(up), depth + 1);
		json folder = rec->second;
		folder["parent"]   = ents[parent]["uuid"];
		folder["children"] = json::array();
		ents.push_back(std::move(folder));
		const size_t here = ents.size() - 1;
		at[key] = here;
		adopt(parent, ents[here]["uuid"]);
		return here;
	};

	// Absolute in the cell files, relative to the origin in the world.
	const glm::dvec3 origin = world.origin();
	size_t merged = 0;
	for (const json& cell : scenes)
	{
		const auto cellEnts = cell.find("entities");
		if (cellEnts == cell.end() || !cellEnts->is_array()) continue;
		std::string cellRootKey;
		for (const json& e : *cellEnts)
			if (e.is_object() && (!e.contains("parent") || e["parent"].is_null()) && e.contains("uuid"))
				cellRootKey = idKey(e["uuid"]);
		for (const json& e : *cellEnts)
		{
			if (!e.is_object() || !e.contains("uuid") || idKey(e["uuid"]) == cellRootKey) continue;
			json copy = e;
			if (copy.contains("parent") && idKey(copy["parent"]) == cellRootKey)
			{
				const auto f = unitFolder.find(idKey(copy["uuid"]));
				const size_t parent = f != unitFolder.end() ? ensureFolder(idKey(f->second), 0) : rootAt;
				copy["parent"] = ents[parent]["uuid"];
				adopt(parent, copy["uuid"]);
				if (origin != glm::dvec3(0.0))
					if (auto c = copy.find("components"); c != copy.end() && c->contains("transform"))
						if (json& p = (*c)["transform"]["position"]; p.is_array() && p.size() >= 3)
							for (int i = 0; i < 3; ++i)
								p[i] = p[i].get<double>() - origin[i];
			}
			ents.push_back(std::move(copy));
			at[idKey(ents.back()["uuid"])] = ents.size() - 1;
			++merged;
		}
	}

	// Inside a folder the split knew, the children go back into the order they
	// had (sibling order is authored data); anything added since stays behind.
	for (const auto& [key, rec] : folderRecord)
	{
		const auto it = at.find(key);
		const auto orig = rec.find("children");
		if (it == at.end() || orig == rec.end() || !orig->is_array()) continue;
		json& kids = ents[it->second]["children"];
		if (!kids.is_array()) continue;
		std::unordered_map<std::string, size_t> rank;
		for (size_t i = 0; i < orig->size(); ++i) rank[idKey((*orig)[i])] = i;
		std::vector<json> sorted(kids.begin(), kids.end());
		std::stable_sort(sorted.begin(), sorted.end(), [&rank](const json& a, const json& b)
		{
			const auto ra = rank.find(idKey(a)), rb = rank.find(idKey(b));
			return (ra != rank.end() ? ra->second : SIZE_MAX) < (rb != rank.end() ? rb->second : SIZE_MAX);
		});
		kids = json(std::move(sorted));
	}
	scene.erase("cells");

	world.clear();
	if (!ser.loadFromMemory(world, json::to_cbor(scene)))
	{
		world.clear();
		ser.loadFromMemory(world, snapshot);
		return fail("the merged scene did not load");
	}
	world.markHierarchyDirty();
	if (mergedEntities) *mergedEntities = merged;
	HE_LOG_INFO(World, "Merged %zu streaming cell(s) back into the scene: %zu entities",
	            manifest.cells.size(), merged);
	return true;
}

} // namespace HE
