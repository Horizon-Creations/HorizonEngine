#include "TerrainSync.h"

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <utility>

namespace HE::Ed
{
namespace
{
using json = nlohmann::json;

// Grids larger than this are not a landscape this editor could have made; a
// patch that claims one is refused before anything is allocated for it.
constexpr std::uint32_t kMaxGridRes = 8192;

std::uint32_t elemOf(std::uint8_t f)
{
	return f == TerrainSync::FoliageMask ? 1u : 4u;
}

bool isGrid(std::uint8_t f)
{
	return f == TerrainSync::Heights || f == TerrainSync::Weights ||
	       f == TerrainSync::Weights2 || f == TerrainSync::FoliageMask;
}

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes)
{
	std::uint64_t h = 1469598103934665603ull;
	for (const std::uint8_t b : bytes) { h ^= b; h *= 1099511628211ull; }
	return h;
}

// One grid of one landscape as it stands now. `bytes` 0 = there is none.
struct GridView
{
	std::uint8_t* data  = nullptr;
	std::size_t   bytes = 0;
	std::uint32_t res   = 0;
};

// False when the field does not exist here, or is not settled: heights whose
// count does not fit `resolution` are mid-snap (TerrainSystem resamples them to
// 2ⁿ+1 on its next tick), and sending them would send a shape nobody has.
bool viewGrid(entt::registry& reg, Entity e, std::uint8_t f, GridView& out)
{
	auto* t = reg.try_get<TerrainComponent>(e);
	if (!t) return false;
	switch (f)
	{
	case TerrainSync::Heights:
		out.data  = reinterpret_cast<std::uint8_t*>(t->sculptHeights.data());
		out.bytes = t->sculptHeights.size() * sizeof(float);
		out.res   = t->resolution;
		break;
	case TerrainSync::Weights:
		out.data  = t->layerWeights.data();
		out.bytes = t->layerWeights.size();
		out.res   = t->weightRes;
		break;
	case TerrainSync::Weights2:
		out.data  = t->layerWeights2.data();
		out.bytes = t->layerWeights2.size();
		out.res   = t->weightRes;
		break;
	case TerrainSync::FoliageMask:
	{
		auto* fol = reg.try_get<FoliageComponent>(e);
		if (!fol) return false;
		out.data  = fol->densityMask.data();
		out.bytes = fol->densityMask.size();
		out.res   = fol->maskRes;
		break;
	}
	default:
		return false;
	}
	if (out.bytes == 0) { out.res = 0; return true; }
	return out.bytes == static_cast<std::size_t>(out.res) * out.res * elemOf(f);
}

// A fresh grid of `res`² zeroed elements (res 0: no grid), with the side length
// the component keeps for it. False if the component is not there.
bool reallocateGrid(entt::registry& reg, Entity e, std::uint8_t f, std::uint32_t res)
{
	auto* t = reg.try_get<TerrainComponent>(e);
	if (!t) return false;
	const std::size_t n = static_cast<std::size_t>(res) * res;
	switch (f)
	{
	case TerrainSync::Heights:
		t->sculptHeights.assign(n, 0.0f);
		if (res > 0) t->resolution = res;
		t->dirty = true;   // a new grid: every chunk
		return true;
	case TerrainSync::Weights:
	case TerrainSync::Weights2:
		(f == TerrainSync::Weights ? t->layerWeights : t->layerWeights2).assign(n * 4, 0);
		if (res > 0) t->weightRes = res;
		t->weightsDirty = true;
		return true;
	case TerrainSync::FoliageMask:
		if (auto* fol = reg.try_get<FoliageComponent>(e))
		{
			fol->densityMask.assign(n, 0);
			if (res > 0) fol->maskRes = res;
			fol->dirty = true;
			return true;
		}
		return false;
	default:
		return false;
	}
}

// What the receiver does once bytes landed in a grid: the same flags the tool
// that made them sets on the machine it ran on.
void markWritten(entt::registry& reg, Entity e, std::uint8_t f, const TerrainSync::Patch& p)
{
	auto* t = reg.try_get<TerrainComponent>(e);
	if (!t) return;
	switch (f)
	{
	case TerrainSync::Heights:
	{
		// Grid → terrain-local XZ (TerrainSculpt's mapping), one cell of margin so
		// the chunks either side of a seam both rebuild.
		const float res   = static_cast<float>(std::max<std::uint32_t>(p.res, 2) - 1);
		const float stepX = t->sizeX / res, stepZ = t->sizeZ / res;
		const float hx = t->sizeX * 0.5f, hz = t->sizeZ * 0.5f;
		TerrainSculpt::markRegionDirty(*t,
			-hx + (static_cast<float>(p.x0) - 1.0f) * stepX,
			-hz + (static_cast<float>(p.z0) - 1.0f) * stepZ,
			-hx + static_cast<float>(p.x0 + p.w) * stepX,
			-hz + static_cast<float>(p.z0 + p.h) * stepZ);
		break;
	}
	case TerrainSync::Weights:
	case TerrainSync::Weights2:
		t->weightsDirty = true;
		break;
	case TerrainSync::FoliageMask:
		if (auto* fol = reg.try_get<FoliageComponent>(e)) fol->dirty = true;
		break;
	default:
		break;
	}
}

// ── The grids, set aside while an entity is serialized ──────────────────────
// Swapped out and back (no copy), so the serializer writes everything else
// and none of the base64.
struct GridsAside
{
	TerrainComponent* t = nullptr;
	FoliageComponent* f = nullptr;
	std::vector<float>        heights;
	std::vector<std::uint8_t> w1, w2, mask;

	GridsAside(entt::registry& reg, Entity e)
	{
		t = reg.try_get<TerrainComponent>(e);
		f = reg.try_get<FoliageComponent>(e);
		if (t)
		{
			heights.swap(t->sculptHeights);
			w1.swap(t->layerWeights);
			w2.swap(t->layerWeights2);
		}
		if (f) mask.swap(f->densityMask);
	}
	~GridsAside()
	{
		if (t)
		{
			heights.swap(t->sculptHeights);
			w1.swap(t->layerWeights);
			w2.swap(t->layerWeights2);
		}
		if (f) mask.swap(f->densityMask);
	}
	GridsAside(const GridsAside&) = delete;
	GridsAside& operator=(const GridsAside&) = delete;
};

// Anything that changes what the chunks are built from. Not the grids (they
// say so themselves) and not the material (read per draw).
bool shapeDiffers(const TerrainComponent& a, const TerrainComponent& b)
{
	return a.sizeX != b.sizeX || a.sizeZ != b.sizeZ || a.resolution != b.resolution ||
	       a.heightScale != b.heightScale || a.seed != b.seed ||
	       a.lodDistanceScale != b.lodDistanceScale || a.octaves != b.octaves ||
	       a.frequency != b.frequency || a.lacunarity != b.lacunarity || a.gain != b.gain ||
	       a.uvTiling != b.uvTiling || a.tessellationFactor != b.tessellationFactor ||
	       a.tessellationDistance != b.tessellationDistance ||
	       a.displacementTexture != b.displacementTexture ||
	       a.displacementStrength != b.displacementStrength ||
	       a.displacementTiling != b.displacementTiling;
}

// `n` was just built from a blob, `o` is what the entity had before. The grids
// the blob did not carry come back from `o`, and so does the runtime state: a
// settings change must not rebuild and re-upload every chunk (the loader's fresh
// component says dirty, builtRes 0), unless it changed the shape.
void restoreTerrain(TerrainComponent& n, TerrainComponent&& o)
{
	const bool heightsCame = !n.sculptHeights.empty();
	const bool weightsCame = !n.layerWeights.empty();
	if (!heightsCame) n.sculptHeights = std::move(o.sculptHeights);
	if (!weightsCame)
	{
		n.layerWeights  = std::move(o.layerWeights);
		n.layerWeights2 = std::move(o.layerWeights2);
		n.weightRes     = o.weightRes;   // not written without the weights
	}
	n.weightmapTextureId = o.weightmapTextureId;
	std::copy(std::begin(o.avgLayerWeights), std::end(o.avgLayerWeights),
	          std::begin(n.avgLayerWeights));
	n.weightsDirty       = o.weightsDirty || weightsCame;
	n.builtRes           = o.builtRes;
	n.builtChunksPerSide = o.builtChunksPerSide;
	n.regionDirty        = o.regionDirty;
	n.dirtyMinX = o.dirtyMinX; n.dirtyMaxX = o.dirtyMaxX;
	n.dirtyMinZ = o.dirtyMinZ; n.dirtyMaxZ = o.dirtyMaxZ;
	n.dirty              = o.dirty || heightsCame || shapeDiffers(n, o);
	// The loaded field is a new one; a cache keyed on the revision must see it.
	n.water.revision     = o.water.revision + 1;
}

// Same for the foliage layer. Its scatter is only rebuilt when something it is
// scattered from changed; otherwise the old component, scatter and all, stays.
void restoreFoliage(FoliageComponent& n, FoliageComponent&& o)
{
	const bool maskCame = !n.densityMask.empty();
	const bool same = n.visible == o.visible && n.meshAssetId == o.meshAssetId &&
	                  n.materialAssetId == o.materialAssetId && n.density == o.density &&
	                  n.seed == o.seed && n.minScale == o.minScale &&
	                  n.maxScale == o.maxScale && n.drawDistance == o.drawDistance &&
	                  n.maskRes == o.maskRes;
	if (!maskCame && same)
	{
		n = std::move(o);
		return;
	}
	if (!maskCame && n.maskRes == o.maskRes) n.densityMask = std::move(o.densityMask);
	n.dirty    = true;
	n.revision = o.revision + 1;   // a retained RenderWorld keys on it
}

// Apply a component blob to `e` (replace or update) with the landscape's grids
// and runtime state carried over. The stash is taken only for the components the
// blob rebuilds: one it lacks is left alone (update) or removed (replace).
bool applyKeepingGrids(HorizonWorld& world, Entity e, const std::vector<std::uint8_t>& blob,
                       bool replace)
{
	auto& reg = world.registry();
	if (!reg.valid(e) || blob.empty()) return false;

	const json comps = json::from_cbor(blob, /*strict=*/true, /*allow_exceptions=*/false);
	if (comps.is_discarded() || !comps.is_object()) return false;

	std::optional<TerrainComponent> oldT;
	std::optional<FoliageComponent> oldF;
	if (comps.contains("terrain"))
		if (auto* t = reg.try_get<TerrainComponent>(e)) oldT.emplace(std::move(*t));
	if (comps.contains("foliage"))
		if (auto* f = reg.try_get<FoliageComponent>(e)) oldF.emplace(std::move(*f));

	SceneSerializer serializer;
	const bool ok = replace ? serializer.replaceEntityComponents(world, e, blob)
	                        : serializer.applyEntityComponents(world, e, blob);
	if (!ok)
	{
		// Nothing was applied: give back what was moved out.
		if (oldT) if (auto* t = reg.try_get<TerrainComponent>(e)) *t = std::move(*oldT);
		if (oldF) if (auto* f = reg.try_get<FoliageComponent>(e)) *f = std::move(*oldF);
		return false;
	}
	if (oldT) if (auto* t = reg.try_get<TerrainComponent>(e)) restoreTerrain(*t, std::move(*oldT));
	if (oldF) if (auto* f = reg.try_get<FoliageComponent>(e)) restoreFoliage(*f, std::move(*oldF));
	return true;
}
} // namespace

// ── Blob helpers ─────────────────────────────────────────────────────────────

std::vector<std::uint8_t> TerrainSync::serializeWithoutGrids(HorizonWorld& world, Entity e)
{
	auto& reg = world.registry();
	if (!reg.valid(e)) return {};
	GridsAside aside(reg, e);
	return SceneSerializer().serializeEntityComponents(world, e);
}

bool TerrainSync::replaceKeepingGrids(HorizonWorld& world, Entity e,
                                      const std::vector<std::uint8_t>& blob)
{
	return applyKeepingGrids(world, e, blob, /*replace=*/true);
}

std::vector<std::uint8_t> TerrainSync::settingsBlob(HorizonWorld& world, Entity e)
{
	const std::vector<std::uint8_t> all = serializeWithoutGrids(world, e);
	if (all.empty()) return {};
	const json comps = json::from_cbor(all, /*strict=*/true, /*allow_exceptions=*/false);
	if (comps.is_discarded() || !comps.is_object()) return {};
	json out = json::object();
	if (auto it = comps.find("terrain"); it != comps.end()) out["terrain"] = *it;
	if (auto it = comps.find("foliage"); it != comps.end()) out["foliage"] = *it;
	if (out.empty()) return {};
	return json::to_cbor(out);
}

// ── Session state ────────────────────────────────────────────────────────────

void TerrainSync::reset()
{
	m_mirrors.clear();
	m_pending.clear();
}

void TerrainSync::baseline(entt::registry& reg, Entity e, Mirror& m)
{
	for (std::uint8_t f = Heights; f <= FoliageMask; ++f)
	{
		GridView v;
		m.grid[f].clear();
		m.res[f] = 0;
		// An unsettled grid stays empty here, so the first settled look at it
		// sends it whole — what it settles into is what the peers must have.
		if (viewGrid(reg, e, f, v) && v.bytes > 0)
		{
			m.grid[f].assign(v.data, v.data + v.bytes);
			m.res[f] = v.res;
		}
	}
}

bool TerrainSync::emit(Entity e, Patch& p, const Send& send)
{
	p.seq = ++m_seq;
	if (!send(e, p)) return false;
	if (m_expectEcho)
	{
		m_pending.push_back({ e, p });
		// An echo that never comes (a host that stopped relaying) must not grow
		// this without end. The oldest goes: it is the one most surely placed.
		if (m_pending.size() > kMaxPending) m_pending.pop_front();
	}
	return true;
}

void TerrainSync::collect(HorizonWorld& world, std::uint64_t nowMs, const Send& send)
{
	auto& reg = world.registry();

	// Landscapes that are gone take their mirror with them.
	for (auto it = m_mirrors.begin(); it != m_mirrors.end();)
	{
		if (!reg.valid(it->first) || !reg.all_of<TerrainComponent>(it->first))
			it = m_mirrors.erase(it);
		else
			++it;
	}

	for (const Entity e : reg.view<TerrainComponent>())
	{
		auto [it, fresh] = m_mirrors.try_emplace(e);
		Mirror& m = it->second;
		if (fresh)
		{
			// First sight (a snapshot, a peer's create, our own create, which the
			// structural diff sent whole): what is there now is what everyone has.
			baseline(reg, e, m);
			m.settingsHash = fnv1a(settingsBlob(world, e));
			continue;
		}

		// Settings before grids: a resolution travels with them.
		if (nowMs >= m.nextAt[Settings])
		{
			m.nextAt[Settings] = nowMs + kSettingsIntervalMs;
			std::vector<std::uint8_t> blob = settingsBlob(world, e);
			const std::uint64_t hash = fnv1a(blob);
			if (hash != m.settingsHash && !blob.empty())
			{
				Patch p;
				p.field = Settings;
				p.bytes = std::move(blob);
				// Not sent (the link is down): it is still a change, next time.
				if (emit(e, p, send)) m.settingsHash = hash;
			}
		}

		for (std::uint8_t f = Heights; f <= FoliageMask; ++f)
			collectGrid(reg, e, m, static_cast<Field>(f), nowMs, send);
	}
}

void TerrainSync::collectGrid(entt::registry& reg, Entity e, Mirror& m, Field f,
                              std::uint64_t nowMs, const Send& send)
{
	if (nowMs < m.nextAt[f]) return;
	GridView v;
	if (!viewGrid(reg, e, f, v)) return;

	const std::uint32_t elem = elemOf(f);
	std::vector<std::uint8_t>& mg = m.grid[f];
	std::size_t sent = 0;

	// Rows [z0, z0+h) × columns [x0, x0+w) of the live grid, in bands of at most
	// kMaxPatchBytes; the first band of a whole grid says so. False if any band
	// did not go out: the mirror then stays behind and the rect goes again next
	// time (a band that did arrive twice is harmless, it carries the same bytes).
	const auto sendRect = [&](std::uint32_t x0, std::uint32_t z0, std::uint32_t w,
	                          std::uint32_t h, bool whole) {
		const std::size_t rowBytes = static_cast<std::size_t>(w) * elem;
		const std::uint32_t band = static_cast<std::uint32_t>(
			std::max<std::size_t>(1, kMaxPatchBytes / rowBytes));
		bool all = true;
		for (std::uint32_t z = z0; z < z0 + h && all; z += band)
		{
			Patch p;
			p.field = static_cast<std::uint8_t>(f | (whole && z == z0 ? kWholeGrid : 0));
			p.res = v.res;
			p.x0 = x0; p.z0 = z; p.w = w; p.h = std::min(band, z0 + h - z);
			p.bytes.resize(rowBytes * p.h);
			for (std::uint32_t r = 0; r < p.h; ++r)
			{
				const std::size_t src = (static_cast<std::size_t>(z + r) * v.res + x0) * elem;
				std::memcpy(p.bytes.data() + r * rowBytes, v.data + src, rowBytes);
			}
			sent += p.bytes.size();
			all = emit(e, p, send);
		}
		return all;
	};

	if (v.bytes != mg.size() || v.res != m.res[f])
	{
		// A grid that appeared, vanished or changed its side goes whole.
		bool ok = true;
		if (v.bytes == 0)
		{
			Patch p;
			p.field = static_cast<std::uint8_t>(f | kWholeGrid);   // res 0: cleared
			ok = emit(e, p, send);
		}
		else
		{
			ok = sendRect(0, 0, v.res, v.res, /*whole=*/true);
		}
		if (ok)
		{
			mg.assign(v.data, v.data + v.bytes);
			m.res[f] = v.res;
		}
	}
	else if (v.bytes > 0 && std::memcmp(v.data, mg.data(), v.bytes) != 0)
	{
		// The bounding rectangle of what differs: rows first, then the widest
		// byte span over those rows, rounded out to whole elements.
		const std::size_t rowBytes = static_cast<std::size_t>(v.res) * elem;
		std::uint32_t z0 = v.res, z1 = 0;
		std::size_t   b0 = rowBytes, b1 = 0;
		for (std::uint32_t z = 0; z < v.res; ++z)
		{
			const std::uint8_t* a = v.data + z * rowBytes;
			const std::uint8_t* b = mg.data() + z * rowBytes;
			if (std::memcmp(a, b, rowBytes) == 0) continue;
			z0 = std::min(z0, z);
			z1 = z;
			std::size_t lo = 0;
			while (lo < b0 && a[lo] == b[lo]) ++lo;
			std::size_t hi = rowBytes - 1;
			while (hi > b1 && a[hi] == b[hi]) --hi;
			b0 = std::min(b0, lo);
			b1 = std::max(b1, hi);
		}
		const std::uint32_t x0 = static_cast<std::uint32_t>(b0 / elem);
		const std::uint32_t x1 = static_cast<std::uint32_t>(b1 / elem);
		const std::uint32_t w = x1 - x0 + 1, h = z1 - z0 + 1;
		if (sendRect(x0, z0, w, h, /*whole=*/false))
		{
			for (std::uint32_t z = z0; z <= z1; ++z)
			{
				const std::size_t off = (static_cast<std::size_t>(z) * v.res + x0) * elem;
				std::memcpy(mg.data() + off, v.data + off, static_cast<std::size_t>(w) * elem);
			}
		}
	}

	// Also when nothing changed: comparing a 4 MB grid every frame buys nothing
	// a look every kMinIntervalMs does not.
	m.nextAt[f] = nowMs + std::max<std::uint64_t>(kMinIntervalMs, sent / kBytesPerMs);
}

// ── Receiving ────────────────────────────────────────────────────────────────

bool TerrainSync::applyOne(HorizonWorld& world, Entity e, const Patch& p)
{
	auto& reg = world.registry();
	if (!reg.valid(e) || !reg.all_of<TerrainComponent>(e)) return false;
	const std::uint8_t f     = static_cast<std::uint8_t>(p.field & ~kWholeGrid);
	const bool         whole = (p.field & kWholeGrid) != 0;
	const auto mirror = m_mirrors.find(e);

	if (f == Settings)
	{
		if (!applyKeepingGrids(world, e, p.bytes, /*replace=*/false)) return false;
		// What we now hold is what everybody holds: no change of ours to send.
		if (mirror != m_mirrors.end()) mirror->second.settingsHash = fnv1a(settingsBlob(world, e));
		return true;
	}
	if (!isGrid(f)) return false;

	const std::uint32_t elem = elemOf(f);
	if (whole)
	{
		if (p.res > kMaxGridRes || (f == Heights && p.res > kTerrainMaxResolution)) return false;
		if (f == Heights && p.res == 1) return false;
		if (p.res == 0)
		{
			if (!reallocateGrid(reg, e, f, 0)) return false;
			if (mirror != m_mirrors.end())
			{
				mirror->second.grid[f].clear();
				mirror->second.res[f] = 0;
			}
			return true;
		}
	}

	// Check everything before the first write: a rejected patch changes nothing.
	if (p.w == 0 || p.h == 0 || p.x0 >= p.res || p.z0 >= p.res ||
	    p.w > p.res - p.x0 || p.h > p.res - p.z0 ||
	    p.bytes.size() != static_cast<std::size_t>(p.w) * p.h * elem)
		return false;

	GridView v;
	const bool settled = viewGrid(reg, e, f, v);
	if (whole)
	{
		if (!reallocateGrid(reg, e, f, p.res)) return false;
	}
	else if (!settled || v.bytes == 0 || v.res != p.res)
	{
		// A patch for a grid of another size: one from before a resize this
		// side already has. Dropping it is what keeps the new grid whole.
		return false;
	}
	if (!viewGrid(reg, e, f, v) || v.res != p.res || v.bytes == 0) return false;

	const std::size_t rowBytes = static_cast<std::size_t>(p.w) * elem;
	for (std::uint32_t r = 0; r < p.h; ++r)
	{
		const std::size_t dst = (static_cast<std::size_t>(p.z0 + r) * p.res + p.x0) * elem;
		std::memcpy(v.data + dst, p.bytes.data() + r * rowBytes, rowBytes);
	}
	markWritten(reg, e, f, p);

	// The mirror follows, so the next collect does not send this back.
	if (mirror != m_mirrors.end())
	{
		Mirror& m = mirror->second;
		if (m.grid[f].size() != v.bytes)
		{
			m.grid[f].assign(v.data, v.data + v.bytes);
		}
		else
		{
			for (std::uint32_t r = 0; r < p.h; ++r)
			{
				const std::size_t off = (static_cast<std::size_t>(p.z0 + r) * p.res + p.x0) * elem;
				std::memcpy(m.grid[f].data() + off, v.data + off, rowBytes);
			}
		}
		m.res[f] = p.res;
	}
	return true;
}

bool TerrainSync::applyRemote(HorizonWorld& world, Entity e, const Patch& patch)
{
	const bool fresh = m_mirrors.find(e) == m_mirrors.end();
	if (!applyOne(world, e, patch)) return false;

	// Ours that the host has not placed yet come after this one in its order, so
	// they go on top of it here too. One that no longer fits (the peer resized
	// the grid under it) is dropped, as the host's other peers drop it.
	for (auto it = m_pending.begin(); it != m_pending.end();)
	{
		if (it->entity == e && !applyOne(world, e, it->patch))
			it = m_pending.erase(it);
		else
			++it;
	}

	// A landscape seen for the first time through a peer's patch: its mirror
	// starts from the result, or the next collect would send it all back.
	if (fresh)
	{
		Mirror& m = m_mirrors[e];
		baseline(world.registry(), e, m);
		m.settingsHash = fnv1a(settingsBlob(world, e));
	}
	return true;
}

void TerrainSync::acknowledge(std::uint32_t seq)
{
	// The host relays in order, so echoes come back in the order we sent.
	while (!m_pending.empty() &&
	       static_cast<std::int32_t>(m_pending.front().patch.seq - seq) <= 0)
		m_pending.pop_front();
}
} // namespace HE::Ed
