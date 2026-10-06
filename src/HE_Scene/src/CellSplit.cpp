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
	std::function<void(const json&)> visit = [&](const json& parent)
	{
		const json* kids = childrenOf(parent);
		if (!kids) return;
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

	SceneSerializer ser;
	auto& reg = world.registry();
	const glm::vec3 shift = -glm::vec3(world.origin());   // absolute in the file, relative in the world
	size_t merged = 0;
	for (const json& scene : scenes)
	{
		std::vector<Entity> created;
		if (!ser.loadAdditiveFromJson(world, scene, &created)) continue;
		// The cell's own root goes; its children move up to the world root.
		Entity cellRoot = entt::null;
		for (Entity e : created)
			if (const auto* h = reg.try_get<HierarchyComponent>(e); h && h->parent == world.rootEntity())
			{
				cellRoot = e;
				break;
			}
		if (cellRoot == entt::null) continue;
		const std::vector<Entity> kids = reg.get<HierarchyComponent>(cellRoot).children;
		for (Entity k : kids)
		{
			world.reparentEntity(k, world.rootEntity());
			if (shift != glm::vec3(0.0f))
				if (auto* t = reg.try_get<TransformComponent>(k)) t->position += shift;
		}
		world.destroyEntity(cellRoot);
		merged += created.size() - 1;
	}
	world.setCellManifestJson(std::string());
	world.markHierarchyDirty();
	if (mergedEntities) *mergedEntities = merged;
	HE_LOG_INFO(World, "Merged %zu streaming cell(s) back into the scene: %zu entities",
	            manifest.cells.size(), merged);
	return true;
}

} // namespace HE
