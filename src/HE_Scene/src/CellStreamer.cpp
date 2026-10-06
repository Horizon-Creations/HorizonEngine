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

// Frees a parsed cell on a worker: a cell of thousands of entities is a big
// tree, and freeing it cost the main thread more than a tenth of a load before.
void releaseOnWorker(json&& tree)
{
	auto holder = std::make_shared<json>(std::move(tree));
	globalPool().post([holder]() mutable { holder.reset(); }, "CellJsonRelease", 1, JobPriority::Low);
}
} // namespace

struct CellStreamer::Pending
{
	int              x = 0, z = 0;
	CancelToken      token = CancelToken::create();
	std::atomic<int> state{ 0 };   // 0 running, 1 parsed, 2 failed
	json             scene;
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
	std::vector<Key> loaded;
	loaded.reserve(m_loaded.size());
	for (const auto& [k, root] : m_loaded) loaded.push_back(k);
	for (Key k : loaded) unloadCell(world, k);
	reset();
}

bool CellStreamer::isLoaded(int x, int z) const
{
	return m_loaded.count(key(x, z)) != 0;
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

void CellStreamer::update(HorizonWorld& world, const glm::dvec3& camera, const glm::vec3& velocity,
                          double budgetMs)
{
	if (!active()) return;
	using Clock = std::chrono::steady_clock;
	const Clock::time_point start = Clock::now();
	const double     size   = m_manifest.cellSize;
	const glm::dvec3 ahead  = camera + glm::dvec3(velocity) * static_cast<double>(m_manifest.lookaheadSec);
	const auto       nearer = [&](int x, int z)
	{
		return std::min(squareDistance(camera, x, z, size), squareDistance(ahead, x, z, size));
	};

	// 0) Cells somebody else destroyed (world.clear() on a level change) are no
	//    longer loaded; they come back like any other once wanted.
	for (auto it = m_loaded.begin(); it != m_loaded.end();)
		it = world.registry().valid(it->second) ? std::next(it) : m_loaded.erase(it);

	// 1) Cells that should be there: within the load radius of the camera or of
	//    where it is heading. Only the grid squares around both are looked at,
	//    so a manifest of a hundred thousand cells costs what a few dozen do.
	std::vector<std::pair<double, Key>> wanted;
	const auto scan = [&](const glm::dvec3& p)
	{
		const int r  = static_cast<int>(std::ceil(m_manifest.loadRadius / size)) + 1;
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
	scan(camera);
	if (ahead != camera) scan(ahead);
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
		globalPool().schedule([p, read = std::move(read)]
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
			p->scene = std::move(scene);
			p->state.store(1, std::memory_order_release);
		}, std::move(desc));
		m_pending[k] = std::move(p);
	}

	// 3) Reads the camera has turned away from are dropped (same hysteresis as
	//    loaded cells), parsed ones are collected for building.
	std::vector<std::pair<double, Key>> ready;
	for (auto it = m_pending.begin(); it != m_pending.end();)
	{
		Pending& p = *it->second;
		const double d = nearer(p.x, p.z);
		const int    s = p.state.load(std::memory_order_acquire);
		if (d > m_manifest.unloadRadius)
		{
			p.token.cancel();
			if (s == 1) releaseOnWorker(std::move(p.scene));
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

	// 4) Build parsed cells, nearest first: one always, more while time is left.
	std::sort(ready.begin(), ready.end());
	SceneSerializer ser;
	for (size_t i = 0; i < ready.size(); ++i)
	{
		if (i > 0 && std::chrono::duration<double, std::milli>(Clock::now() - start).count() >= budgetMs)
			break;
		const Key k = ready[i].second;
		std::shared_ptr<Pending> p = std::move(m_pending[k]);
		m_pending.erase(k);
		std::vector<entt::entity> created;
		const bool ok = ser.loadAdditiveFromJson(world, p->scene, &created);
		releaseOnWorker(std::move(p->scene));
		entt::entity root = entt::null;
		auto& reg = world.registry();
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
			m_failed[k] = true;
			++m_stats.failed;
			continue;
		}
		// The file holds absolute positions; the world is relative to its origin.
		reg.get_or_emplace<TransformComponent>(root).position = -glm::vec3(world.origin());
		world.markHierarchyDirty();
		m_loaded[k] = root;
		++m_stats.loadsDone;
		if (m_hooks.loaded) m_hooks.loaded(root, created);
	}

	// 5) Cells both the camera and its lookahead have left behind.
	std::vector<Key> gone;
	for (const auto& [k, root] : m_loaded)
	{
		const CellManifest::Cell& cell = m_manifest.cells[m_index[k]];
		if (nearer(cell.x, cell.z) > m_manifest.unloadRadius) gone.push_back(k);
	}
	for (Key k : gone) unloadCell(world, k);

	m_stats.loaded   = m_loaded.size();
	m_stats.ready    = 0;
	m_stats.inFlight = 0;
	for (const auto& [k, p] : m_pending)
		(p->state.load(std::memory_order_acquire) == 1 ? m_stats.ready : m_stats.inFlight)++;
}

} // namespace HE
