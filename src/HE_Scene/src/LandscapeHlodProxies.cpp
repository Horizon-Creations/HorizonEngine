#include "HorizonScene/LandscapeHlodProxies.h"
#include "HorizonScene/CellStreamer.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/LandscapeProxyComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <ContentManager/ContentManager.h>
#include <Renderer/IRenderer.h>

#include <algorithm>
#include <unordered_set>

namespace HE
{

namespace
{
	bool alive(const entt::registry& reg, entt::entity e)
	{
		return e != entt::null && reg.valid(e) && reg.all_of<LandscapeProxyComponent>(e);
	}

	void place(entt::registry& reg, entt::entity e, const glm::dvec3& origin)
	{
		const glm::vec3 p = glm::vec3(-origin);   // the mesh is in absolute metres: the entity stands where a cell root does
		if (auto* tf = reg.try_get<TransformComponent>(e))
		{
			if (tf->position != p) { tf->position = p; tf->dirty = true; }
			return;
		}
		TransformComponent tf;
		tf.position = p;
		tf.dirty = true;
		reg.emplace<TransformComponent>(e, tf);
	}

	StaticMeshAsset meshOf(const LandscapeHlodMesh& m)
	{
		StaticMeshAsset a;
		a.vertices.reserve(m.vertices.size() * 3);
		a.normals.reserve(m.vertices.size() * 3);
		a.uvs.reserve(m.vertices.size() * 2);
		for (const LandscapeHlodVertex& v : m.vertices)
		{
			a.vertices.insert(a.vertices.end(), { v.position.x, v.position.y, v.position.z });
			a.normals.insert(a.normals.end(), { v.normal.x, v.normal.y, v.normal.z });
			a.uvs.insert(a.uvs.end(), { v.uv.x, v.uv.y });
		}
		a.indices = m.indices;
		return a;
	}
}

void LandscapeHlodProxies::removeEntity(HorizonWorld& world, ContentManager& content, IRenderer* renderer, entt::entity e)
{
	HE::UUID mesh{};
	if (const auto* p = world.registry().try_get<LandscapeProxyComponent>(e)) mesh = p->meshId;
	world.destroyEntity(e);
	++m_stats.destroyed;
	if (mesh != HE::UUID{})
	{
		if (renderer) renderer->InvalidateMesh(mesh);
		content.unloadAsset(mesh);
	}
}

void LandscapeHlodProxies::clear(HorizonWorld& world, ContentManager& content, IRenderer* renderer)
{
	if (m_world == &world)
		for (auto& kv : m_cells)
			if (alive(world.registry(), kv.second)) removeEntity(world, content, renderer, kv.second);
	m_cells.clear();
	m_stats.entities = 0;
	m_world = nullptr;
	m_frame = 0;
}

void LandscapeHlodProxies::sync(HorizonWorld& world, const LandscapeHlodTable& table, const CellStreamer& streamer,
                                const glm::dvec3& cameraAbsolute, ContentManager& content, IRenderer* renderer)
{
	auto& reg = world.registry();
	if (m_world != &world)
	{
		m_cells.clear();   // another world than ours: the entities are gone with it (or belong to it)
		m_stats.entities = 0;
		m_world = &world;
		m_frame = 0;
	}
	const glm::dvec3 origin = world.origin();

	const bool pass = m_frame++ % static_cast<unsigned>(std::max(1, m_config.everyFrames)) == 0;
	if (pass)
	{
		++m_stats.passes;
		const float cell = streamer.manifest().cellSize;
		std::unordered_set<LandscapeHlodCellKey, LandscapeHlodCellKeyHash> wanted;
		if (cell > 0.0f)
			for (const LandscapeHlodCellKey& k : hlodCellsToDraw(table, streamer))
			{
				// ground-plane distance from the camera to the cell's square, the distance CellStreamer loads by
				const double x0 = static_cast<double>(k.x) * cell, z0 = static_cast<double>(k.z) * cell;
				const double dx = std::max({ x0 - cameraAbsolute.x, 0.0, cameraAbsolute.x - (x0 + cell) });
				const double dz = std::max({ z0 - cameraAbsolute.z, 0.0, cameraAbsolute.z - (z0 + cell) });
				if (dx * dx + dz * dz > m_config.drawRadius * m_config.drawRadius) continue;
				const LandscapeHlodMesh* mesh = table.find(k.x, k.z);
				if (!mesh || mesh->empty()) continue;
				wanted.insert(k);
			}

		for (auto it = m_cells.begin(); it != m_cells.end();)
		{
			if (!wanted.count(it->first) || !alive(reg, it->second))
			{
				if (alive(reg, it->second)) removeEntity(world, content, renderer, it->second);
				it = m_cells.erase(it);
			}
			else ++it;
		}
		for (const LandscapeHlodCellKey& k : wanted)
		{
			if (m_cells.count(k)) continue;
			const HE::UUID meshId = content.registerStaticMesh(meshOf(*table.find(k.x, k.z)));
			const entt::entity e = world.createEntity("LandscapeHlod");
			LandscapeProxyComponent proxy;
			proxy.kind = LandscapeProxyComponent::Kind::Hlod;
			proxy.meshId = meshId;
			reg.emplace<LandscapeProxyComponent>(e, proxy);
			MeshComponent mc;
			mc.meshAssetId = meshId;
			mc.castsShadow = false;     // far content: its shadow would pop in with the cell's own
			mc.dirty = true;
			reg.emplace_or_replace<MeshComponent>(e, mc);
			m_cells[k] = e;
			++m_stats.created;
		}
	}
	for (auto& kv : m_cells)
		if (alive(reg, kv.second)) place(reg, kv.second, origin);
	m_stats.entities = m_cells.size();
}

} // namespace HE
