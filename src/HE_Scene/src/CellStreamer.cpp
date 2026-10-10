#include "HorizonScene/CellStreamer.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/SceneJsonParse.h"
#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>

using json = nlohmann::json;

namespace HE
{

// ── Manifest ─────────────────────────────────────────────────────────────────

bool CellManifest::parse(const std::string& text, CellManifest& out)
{
	out = CellManifest{};
	const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object()) return false;
	CellManifest m;
	const auto num = [&j](const char* k, float fallback)
	{
		const auto it = j.find(k);
		return it != j.end() && it->is_number() ? it->get<float>() : fallback;
	};
	m.cellSize     = num("cellSize", 0.0f);
	m.loadRadius   = num("loadRadius", m.cellSize);
	m.unloadRadius = std::max(num("unloadRadius", m.loadRadius * 1.25f), m.loadRadius);
	m.lookaheadSec = std::max(num("lookaheadSec", 2.0f), 0.0f);
	// A manifest from before the format had a version is version 1; nothing below
	// depends on the number yet, but it is what a later reader will branch on.
	if (const auto it = j.find("version"); it != j.end() && it->is_number_integer())
		m.version = std::max(it->get<int>(), 1);
	if (const auto it = j.find("dir"); it != j.end() && it->is_string()) m.dir = it->get<std::string>();
	if (!(m.cellSize > 0.0f) || !std::isfinite(m.cellSize) || m.dir.empty()) return false;
	const auto list = j.find("list");
	if (list == j.end() || !list->is_array()) return false;
	m.cells.reserve(list->size());
	for (const json& c : *list)
	{
		if (!c.is_array() || c.size() < 2 || !c[0].is_number_integer() || !c[1].is_number_integer())
			return false;
		Cell cell;
		cell.x = c[0].get<int>();
		cell.z = c[1].get<int>();
		if (c.size() > 2 && c[2].is_number_unsigned()) cell.entities = c[2].get<uint32_t>();
		// The fourth column is new in version 2; an older manifest simply ends at three.
		if (c.size() > 3 && c[3].is_number_unsigned()) cell.bodies = c[3].get<uint32_t>();
		m.cells.push_back(cell);
	}
	out = std::move(m);
	return true;
}

std::string CellManifest::cellPath(int x, int z) const
{
	return dir + "/cell_" + std::to_string(x) + "_" + std::to_string(z) + ".hescene";
}

int CellManifest::cellIndex(double coord, float cellSize)
{
	return static_cast<int>(std::floor(coord / static_cast<double>(cellSize)));
}

// ── Streamer ─────────────────────────────────────────────────────────────────

namespace
{
// Distance in the ground plane from `p` to the square of cell (x, z).
double squareDistance(const glm::dvec3& p, int x, int z, double size)
{
	const double x0 = x * size, z0 = z * size;
	const double dx = std::max({ x0 - p.x, 0.0, p.x - (x0 + size) });
	const double dz = std::max({ z0 - p.z, 0.0, p.z - (z0 + size) });
	return std::sqrt(dx * dx + dz * dz);
}
} // namespace

double CellManifest::distanceTo(const glm::dvec3& p, int x, int z) const
{
	return squareDistance(p, x, z, static_cast<double>(cellSize));
}

namespace
{
// An anchor as the streamer measures with it: where it is, where it will be, and
// the factor its distances are divided by.
struct AnchorReach
{
	glm::dvec3 here, ahead;
	double     scale;
};

std::vector<AnchorReach> reachesOf(const std::vector<CellAnchor>& anchors, float lookaheadSec)
{
	std::vector<AnchorReach> out;
	out.reserve(anchors.size());
	for (const CellAnchor& a : anchors)
	{
		AnchorReach r;
		r.here  = a.position;
		r.ahead = a.position + glm::dvec3(a.velocity) * static_cast<double>(lookaheadSec);
		r.scale = a.radiusScale > 0.0f ? static_cast<double>(a.radiusScale) : 1.0;
		out.push_back(r);
	}
	return out;
}

// The distance to the cell's square from the nearest anchor, its radii taken as 1.
double nearestReach(const std::vector<AnchorReach>& reaches, int x, int z, double size)
{
	double best = std::numeric_limits<double>::infinity();
	for (const AnchorReach& r : reaches)
		best = std::min(best, std::min(squareDistance(r.here, x, z, size), squareDistance(r.ahead, x, z, size))
		                      / r.scale);
	return best;
}
} // namespace

double CellManifest::distanceTo(const std::vector<CellAnchor>& anchors, int x, int z) const
{
	return nearestReach(reachesOf(anchors, lookaheadSec), x, z, static_cast<double>(cellSize));
}

std::vector<CellManifest::View> CellManifest::around(const glm::dvec3& p, double range) const
{
	CellAnchor viewpoint;
	viewpoint.position = p;
	return around(std::vector<CellAnchor>{ viewpoint }, range);
}

std::vector<CellManifest::View> CellManifest::around(const std::vector<CellAnchor>& anchors, double range) const
{
	std::vector<View> out;
	if (empty()) return out;
	const std::vector<AnchorReach> reaches = reachesOf(anchors, lookaheadSec);
	for (const Cell& c : cells)
	{
		const double d = nearestReach(reaches, c.x, c.z, static_cast<double>(cellSize));
		if (d > range) continue;
		View v;
		v.x        = c.x;
		v.z        = c.z;
		v.entities = c.entities;
		v.distance = d;
		v.reach    = d <= loadRadius ? View::Reach::Load
		           : d <= unloadRadius ? View::Reach::Keep : View::Reach::Out;
		v.slicesEstimate = c.entities <= kDefaultCellSliceEntities ? 1u
		                 : 1u + static_cast<uint32_t>((c.entities + kDefaultCellSliceEntities - 1) / kDefaultCellSliceEntities);
		out.push_back(v);
	}
	std::sort(out.begin(), out.end(), [](const View& a, const View& b)
	{
		if (a.distance != b.distance) return a.distance < b.distance;
		return a.x != b.x ? a.x < b.x : a.z < b.z;
	});
	return out;
}

namespace
{

// Frees a parsed cell on a worker: a cell of thousands of entities is a big
// tree, and freeing it cost the main thread more than a tenth of a load before.
void releaseOnWorker(json&& tree)
{
	auto holder = std::make_shared<json>(std::move(tree));
	globalPool().post([holder]() mutable { holder.reset(); }, "CellJsonRelease", 1, JobPriority::Low);
}

// The format version a parsed cell file says it has: the "streaming" head's
// "version", or 1 for a file without one (a cell from scripts/split_scene_cells.py,
// or from an editor older than the head). Read off the parsed tree rather than
// from the world, on purpose: an additive load never touches the world's own head.
int cellFormatVersionOf(const json& scene)
{
	const auto head = scene.find("streaming");
	if (head == scene.end() || !head->is_object()) return 1;
	const auto version = head->find("version");
	return version != head->end() && version->is_number_integer() ? std::max(version->get<int>(), 1) : 1;
}
} // namespace

struct CellStreamer::Pending
{
	int              x = 0, z = 0;
	CancelToken      token = CancelToken::create();
	std::atomic<int> state{ 0 };   // 0 running, 1 parsed, 2 failed
	// Written by the worker before it sets state to 1, then the main thread's.
	bool               preserveIds = false;   // the cell's head says its ids are stable
	std::vector<json>  slices;                // see SceneSerializer::sliceForAdditiveLoad
	// The main thread's, once the cell is being built.
	size_t                    next = 0;            // slices built so far; > 0: the cell is under construction
	entt::entity              root = entt::null;   // the cell's root, from the first slice on
	std::vector<entt::entity> created;             // everything built so far
};

CellStreamer::CellStreamer() = default;

CellStreamer::~CellStreamer()
{
	for (auto& [k, p] : m_pending) p->token.cancel();
}

void CellStreamer::begin(const CellManifest& manifest, Reader reader, Hooks hooks)
{
	reset();
	m_manifest = manifest;
	m_reader   = std::move(reader);
	m_hooks    = std::move(hooks);
	for (size_t i = 0; i < m_manifest.cells.size(); ++i)
		m_index[key(m_manifest.cells[i].x, m_manifest.cells[i].z)] = i;
	HE_LOG_INFO(World, "Cell streaming: %zu cell(s) of %.0f m, load within %.0f m, unload beyond %.0f m",
	            m_manifest.cells.size(), m_manifest.cellSize, m_manifest.loadRadius,
	            m_manifest.unloadRadius);
}

void CellStreamer::reset()
{
	for (auto& [k, p] : m_pending) p->token.cancel();
	m_pending.clear();
	m_loaded.clear();
	m_index.clear();
	m_failed.clear();
	m_manifest = CellManifest{};
	m_stats    = Stats{};
}

void CellStreamer::clear(HorizonWorld& world)
{
	std::vector<Key> loaded, building;
	loaded.reserve(m_loaded.size());
	for (const auto& [k, root] : m_loaded) loaded.push_back(k);
	for (const auto& [k, p] : m_pending)
		if (p->next > 0) building.push_back(k);
	for (Key k : loaded) unloadCell(world, k);
	for (Key k : building) abandonCell(world, k);
	reset();
}

bool CellStreamer::isLoaded(int x, int z) const
{
	return m_loaded.count(key(x, z)) != 0;
}

bool CellStreamer::isBuilding(int x, int z) const
{
	const auto it = m_pending.find(key(x, z));
	return it != m_pending.end() && it->second->next > 0;
}

bool CellStreamer::isSettled(const glm::dvec3& position, double radius) const
{
	if (!active()) return true;
	const double size = m_manifest.cellSize;
	const int    r    = static_cast<int>(std::ceil(radius / size)) + 1;
	const int    cx   = CellManifest::cellIndex(position.x, m_manifest.cellSize);
	const int    cz   = CellManifest::cellIndex(position.z, m_manifest.cellSize);
	for (int x = cx - r; x <= cx + r; ++x)
		for (int z = cz - r; z <= cz + r; ++z)
		{
			const Key k = key(x, z);
			if (!m_index.count(k) || squareDistance(position, x, z, size) > radius) continue;
			if (!m_loaded.count(k) && !m_failed.count(k)) return false;
		}
	return true;
}

bool CellStreamer::holdsAt(const glm::dvec3& position) const
{
	if (!active()) return false;
	const Key k = key(CellManifest::cellIndex(position.x, m_manifest.cellSize),
	                  CellManifest::cellIndex(position.z, m_manifest.cellSize));
	// m_loaded is complete cells only: one with slices still to come is in m_pending, so
	// what stands in it is not all there and it holds. A cell that failed for good never
	// will be, and holding for it would freeze what stands there for ever.
	return m_index.count(k) && !m_loaded.count(k) && !m_failed.count(k);
}

void CellStreamer::unloadCell(HorizonWorld& world, Key k)
{
	const auto it = m_loaded.find(k);
	if (it == m_loaded.end()) return;
	const entt::entity root = it->second;
	m_loaded.erase(it);
	if (!world.registry().valid(root)) return;   // destroyed by somebody else
	if (m_hooks.unloading) m_hooks.unloading(root);
	world.destroyEntity(root);
	++m_stats.unloads;
}

void CellStreamer::abandonCell(HorizonWorld& world, Key k)
{
	const auto it = m_pending.find(k);
	if (it == m_pending.end()) return;
	const std::shared_ptr<Pending> p = it->second;
	m_pending.erase(it);
	p->token.cancel();
	for (size_t i = p->next; i < p->slices.size(); ++i) releaseOnWorker(std::move(p->slices[i]));
	if (p->next == 0 || p->root == entt::null || !world.registry().valid(p->root)) return;
	// Slices were built, so physics and asset streaming were told about them: the
	// same teardown as for a built cell.
	if (m_hooks.unloading) m_hooks.unloading(p->root);
	world.destroyEntity(p->root);
	++m_stats.abandoned;
}

void CellStreamer::update(HorizonWorld& world, const glm::dvec3& camera, const glm::vec3& velocity,
                          double budgetMs)
{
	CellAnchor cam;
	cam.position = camera;
	cam.velocity = velocity;
	update(world, std::vector<Anchor>{ cam }, budgetMs);
}

void CellStreamer::update(HorizonWorld& world, const std::vector<Anchor>& anchors, double budgetMs)
{
	if (!active()) return;
	m_stats.anchors = anchors.size();
	if (anchors.empty()) return;
	using Clock = std::chrono::steady_clock;
	const Clock::time_point start = Clock::now();
	const double     size   = m_manifest.cellSize;
	const std::vector<AnchorReach> reaches = reachesOf(anchors, m_manifest.lookaheadSec);
	// The distance a cell counts at: from the anchor nearest to it, in the units of
	// that anchor's radii (an anchor with radiusScale 2 is as far from a cell as one
	// half the distance away).
	const auto nearer = [&](int x, int z) { return nearestReach(reaches, x, z, size); };

	// 0) Cells somebody else destroyed (world.clear() on a level change) are no
	//    longer loaded, nor built in part; they come back like any other once wanted.
	for (auto it = m_loaded.begin(); it != m_loaded.end();)
		it = world.registry().valid(it->second) ? std::next(it) : m_loaded.erase(it);
	for (auto it = m_pending.begin(); it != m_pending.end();)
	{
		const Pending& p = *it->second;
		if (p.next > 0 && !world.registry().valid(p.root))
		{
			it->second->token.cancel();
			for (size_t i = p.next; i < p.slices.size(); ++i) releaseOnWorker(std::move(it->second->slices[i]));
			it = m_pending.erase(it);
		}
		else
			++it;
	}

	// 1) Cells that should be there: within the load radius of an anchor or of where
	//    it is heading. Only the grid squares around those are looked at, so a manifest
	//    of a hundred thousand cells costs what a few dozen do.
	std::vector<std::pair<double, Key>> wanted;
	const auto scan = [&](const glm::dvec3& p, double scale)
	{
		const int r  = static_cast<int>(std::ceil(m_manifest.loadRadius * scale / size)) + 1;
		const int cx = CellManifest::cellIndex(p.x, m_manifest.cellSize);
		const int cz = CellManifest::cellIndex(p.z, m_manifest.cellSize);
		for (int x = cx - r; x <= cx + r; ++x)
			for (int z = cz - r; z <= cz + r; ++z)
			{
				const Key k = key(x, z);
				if (!m_index.count(k) || m_loaded.count(k) || m_pending.count(k) || m_failed.count(k)) continue;
				const double d = nearer(x, z);
				if (d <= m_manifest.loadRadius) wanted.emplace_back(d, k);
			}
	};
	for (const AnchorReach& a : reaches)
	{
		scan(a.here, a.scale);
		if (a.ahead != a.here) scan(a.ahead, a.scale);
	}
	std::sort(wanted.begin(), wanted.end());
	wanted.erase(std::unique(wanted.begin(), wanted.end(),
	                         [](const auto& a, const auto& b) { return a.second == b.second; }),
	             wanted.end());

	// 2) Start reading them, nearest first. Reading and parsing are the worker's.
	for (const auto& [d, k] : wanted)
	{
		const CellManifest::Cell& cell = m_manifest.cells[m_index[k]];
		auto read = m_reader ? m_reader(m_manifest.cellPath(cell.x, cell.z))
		                     : std::function<bool(std::vector<uint8_t>&)>{};
		if (!read)
		{
			HE_LOG_WARN(World, "Cell streaming: cell %d,%d not found (%s)", cell.x, cell.z,
			            m_manifest.cellPath(cell.x, cell.z).c_str());
			m_failed[k] = true;
			++m_stats.failed;
			continue;
		}
		auto p = std::make_shared<Pending>();
		p->x = cell.x;
		p->z = cell.z;
		JobDesc desc;
		desc.name     = "CellLoad";
		// The square the camera stands in is needed now, the rest is ahead of it.
		desc.priority = d <= 0.0 ? JobPriority::High : JobPriority::Normal;
		desc.cancel   = p->token;
		globalPool().schedule([p, read = std::move(read), sliceEntities = m_sliceEntities]
		{
			std::vector<uint8_t> bytes;
			if (p->token.cancelled() || !read(bytes) || bytes.empty())
			{
				p->state.store(2, std::memory_order_release);
				return;
			}
			// A JSON scene opens with '{', a CBOR one with a map head (0xA0-0xBF).
			const auto first = std::find_if(bytes.begin(), bytes.end(),
			                                [](uint8_t b) { return b != ' ' && b != '\t' && b != '\n' && b != '\r'; });
			const bool text  = first != bytes.end() && *first == '{';
			json scene = text ? parseSceneText(std::string(bytes.begin(), bytes.end()))
			                  : parseSceneCbor(bytes);
			if (scene.is_discarded() || !scene.is_object())
			{
				p->state.store(2, std::memory_order_release);
				return;
			}
			// The head is read before the file is cut: a slice is a bare list of
			// records. Cutting is the worker's work, the tree is in its hands anyway.
			p->preserveIds = cellFormatVersionOf(scene) >= kCellFormatVersion;
			p->slices      = SceneSerializer::sliceForAdditiveLoad(std::move(scene), sliceEntities);
			p->state.store(1, std::memory_order_release);
		}, std::move(desc));
		m_pending[k] = std::move(p);
	}

	// 3) Reads every anchor has turned away from are dropped (same hysteresis as
	//    loaded cells, and a cell half built goes the same way, with what was built of
	//    it), parsed ones are collected for building.
	std::vector<std::pair<double, Key>> ready;
	std::vector<Key> turnedAway;
	for (auto it = m_pending.begin(); it != m_pending.end();)
	{
		Pending& p = *it->second;
		const double d = nearer(p.x, p.z);
		const int    s = p.state.load(std::memory_order_acquire);
		if (d > m_manifest.unloadRadius)
		{
			if (p.next > 0)
			{
				turnedAway.push_back(it->first);   // abandonCell takes it out of the map
				++it;
				continue;
			}
			p.token.cancel();
			if (s == 1)
				for (json& slice : p.slices) releaseOnWorker(std::move(slice));
			++m_stats.cancelled;
			it = m_pending.erase(it);
			continue;
		}
		if (s == 2)
		{
			HE_LOG_WARN(World, "Cell streaming: cell %d,%d could not be read or parsed", p.x, p.z);
			m_failed[it->first] = true;
			++m_stats.failed;
			it = m_pending.erase(it);
			continue;
		}
		if (s == 1) ready.emplace_back(d, it->first);
		++it;
	}
	for (Key k : turnedAway) abandonCell(world, k);

	// 4) Build parsed cells, nearest first, slice by slice: one slice always, more
	//    while time is left. A cell's slices go in order and a nearer cell is finished
	//    before a farther one is started, so the budget runs out between slices and
	//    the frame is over it by at most the largest slice.
	std::sort(ready.begin(), ready.end());
	SceneSerializer ser;
	bool builtOne = false;
	bool waiting  = false;   // a cell is waiting for room for its bodies: nothing new is started behind it
	const auto overBudget = [&]
	{
		return builtOne && std::chrono::duration<double, std::milli>(Clock::now() - start).count() >= budgetMs;
	};
	for (size_t i = 0; i < ready.size() && !overBudget(); ++i)
	{
		const Key k = ready[i].second;
		const std::shared_ptr<Pending> p = m_pending[k];
		auto& reg = world.registry();

		// Room for its bodies, asked before the first slice and never in the middle of
		// a cell: the physics world's body table is a fixed size, and a cell that does
		// not fit would be built into the errors of a table that is full. Nothing new
		// is started behind a cell that waits (the cells are in build order, nearest
		// first, and the nearest one must not wait while a farther one takes its room),
		// but a cell that is under way is finished: its room was asked for when it began.
		// It is the unloading below that makes room, a frame later. A world with no cell
		// built and none under way has nothing to wait for, so the load goes ahead.
		if (p->next == 0 && m_hooks.bodiesFit)
		{
			if (waiting) continue;
			const uint32_t bodies = m_manifest.cells[m_index[k]].bodies;
			if (bodies > 0 && !m_hooks.bodiesFit(bodies))
			{
				const bool somethingUp = !m_loaded.empty() ||
					std::any_of(m_pending.begin(), m_pending.end(),
					            [](const auto& e) { return e.second->next > 0; });
				if (somethingUp)
				{
					++m_stats.deferredForBodies;
					HE_LOG_THROTTLE(World, Warning, 5.0,
					                "Cell streaming: cell %d,%d (%u bodies) waits, the physics world has no room for it yet",
					                p->x, p->z, bodies);
					waiting = true;
					continue;
				}
			}
		}
		while (p->next < p->slices.size() && !overBudget())
		{
			const bool first = p->next == 0;
			std::vector<entt::entity> created;
			// A cell the C++ splitter wrote says so in its head, and its ids are then the
			// scene's own ids: kept, so that what refers to them (a placed prefab's
			// bindings above all) holds across an unload and a load. A cell without one
			// gets the ids minted at creation, as before.
			SceneSerializer::AdditiveOptions options;
			options.preserveIds  = p->preserveIds;
			options.idCollisions = &m_stats.idCollisions;
			// The later slices hang under the root the first one made.
			options.attachTo     = first ? entt::null : p->root;
			const bool ok = ser.loadAdditiveFromJson(world, p->slices[p->next], &created, options);
			releaseOnWorker(std::move(p->slices[p->next]));
			builtOne = true;
			if (first)
			{
				entt::entity root = entt::null;
				for (entt::entity e : created)
					if (const auto* h = reg.try_get<HierarchyComponent>(e); h && h->parent == world.rootEntity())
					{
						root = e;
						break;
					}
				if (!ok || root == entt::null)
				{
					for (entt::entity e : created)
						if (reg.valid(e) && reg.get<HierarchyComponent>(e).parent == world.rootEntity())
							world.destroyEntity(e);
					HE_LOG_WARN(World, "Cell streaming: cell %d,%d did not build", p->x, p->z);
					for (size_t s = 1; s < p->slices.size(); ++s) releaseOnWorker(std::move(p->slices[s]));
					m_pending.erase(k);
					m_failed[k] = true;
					++m_stats.failed;
					break;
				}
				// The file holds absolute positions; the world is relative to its origin.
				reg.get_or_emplace<TransformComponent>(root).position = -glm::vec3(world.origin());
				world.markHierarchyDirty();
				p->root = root;
			}
			++p->next;
			++m_stats.slicesDone;
			m_stats.largestSlice = std::max(m_stats.largestSlice, created.size());
			p->created.insert(p->created.end(), created.begin(), created.end());
			if (m_hooks.loadedSlice) m_hooks.loadedSlice(p->root, created);
		}
		if (p->next > 0 && p->next == p->slices.size())
		{
			m_loaded[k] = p->root;
			++m_stats.loadsDone;
			m_pending.erase(k);
			if (m_hooks.loaded) m_hooks.loaded(p->root, p->created);
		}
	}

	// 5) Cells every anchor and its lookahead have left behind.
	std::vector<Key> gone;
	for (const auto& [k, root] : m_loaded)
	{
		const CellManifest::Cell& cell = m_manifest.cells[m_index[k]];
		if (nearer(cell.x, cell.z) > m_manifest.unloadRadius) gone.push_back(k);
	}
	for (Key k : gone) unloadCell(world, k);

	m_stats.loaded   = m_loaded.size();
	m_stats.ready    = 0;
	m_stats.building = 0;
	m_stats.inFlight = 0;
	for (const auto& [k, p] : m_pending)
	{
		if (p->next > 0)                                           ++m_stats.building;
		else if (p->state.load(std::memory_order_acquire) == 1)    ++m_stats.ready;
		else                                                       ++m_stats.inFlight;
	}
}

} // namespace HE
