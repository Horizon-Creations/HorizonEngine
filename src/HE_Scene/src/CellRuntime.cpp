#include "HorizonScene/CellRuntime.h"
#include "HorizonScene/CellPhysics.h"
#include "HorizonScene/EngineApi.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>
#include <glm/geometric.hpp>

namespace HE
{

CellRuntime::CellRuntime(ICellHost& host) : m_host(host) {}

namespace
{
// The context the cells' state goes through: the world streamed into, the host's
// physics world, and whatever the host adds (its script runtime).
api::Ctx stateCtx(ICellHost& host, HorizonWorld& world)
{
	api::Ctx c;
	c.world   = &world;
	c.physics = host.cellPhysics();
	host.cellApiCtx(c);
	c.world = &world;   // the one the cells are in, whatever the host thinks
	return c;
}
} // namespace

CellRuntime::~CellRuntime()
{
	dropTokens();
}

void CellRuntime::dropTokens()
{
	for (auto& [root, token] : m_tokens) token.cancel();
	m_tokens.clear();
}

void CellRuntime::begin(HorizonWorld& world, const std::string& manifestJson)
{
	dropTokens();
	m_streamer.reset();
	m_state.reset();
	m_world         = &world;
	m_manifestJson  = manifestJson;
	m_hasLastCamera = false;
	if (manifestJson.empty()) return;
	CellManifest manifest;
	if (!CellManifest::parse(manifestJson, manifest) || manifest.empty())
	{
		HE_LOG_WARN(World, "%s", "CellRuntime: the scene's cell manifest is malformed; "
		                         "only the base scene is loaded");
		return;
	}

	auto reader = [this](const std::string& path) { return m_host.openCellFile(path); };
	CellStreamer::Hooks hooks;
	// What the session has seen go is not built again (CellState's tombstones).
	hooks.skip = [this](const HE::UUID& id) { return m_state.isTombstoned(id); };
	// A cell is built in slices, and each brings its own entities: their bodies and
	// their asset loads start with the slice, inside the frame's budget, instead of
	// all at once when the last one is done. One token per cell, made with its first
	// slice, so that unloading the cell cancels the loads of every slice.
	hooks.loadedSlice = [this](entt::entity root, const std::vector<entt::entity>& created)
	{
		// First what the session remembers of what changed in the cell: a thing that
		// moved stands where it was before its body is made, so the body is made there.
		api::Ctx ctx = stateCtx(m_host, *m_world);
		m_state.sliceBuilt(ctx, root, created, m_streamer.keepsIds(root));
		// The slice's bodies go in as one charge (cellSliceBodies is the call the
		// tests make too).
		if (PhysicsWorld* physics = m_host.cellPhysics())
			cellSliceBodies(*physics, *m_world, created);
		std::vector<uint32_t> ids;
		ids.reserve(created.size());
		for (entt::entity e : created) ids.push_back(static_cast<uint32_t>(e));
		auto token = m_tokens.find(static_cast<uint32_t>(root));
		if (token == m_tokens.end())
			token = m_tokens.emplace(static_cast<uint32_t>(root), CancelToken::create()).first;
		m_host.streamCellAssets(*m_world, token->second, ids);
	};
	hooks.loaded = [this](entt::entity root, const std::vector<entt::entity>& all)
	{
		// Only what is still there: a script of the base may have destroyed some of
		// it between two slices.
		const entt::registry& reg = m_world->registry();
		std::vector<entt::entity> created;
		created.reserve(all.size());
		for (entt::entity e : all)
			if (reg.valid(e)) created.push_back(e);
		m_host.cellLoaded(*m_world, root, created);
		// The Save Game variables last: they need the class instances, which are the
		// host's to start.
		api::Ctx ctx = stateCtx(m_host, *m_world);
		m_state.cellBuilt(ctx, root);
	};
	hooks.unloading = [this](entt::entity root)
	{
		// What the session keeps of the cell first, while its scripts still run (their
		// Save Game variables) — unless the session ends with it. Then the host, while
		// the cell is whole. Then the bodies, in a pass of their own, with the joints
		// that name them kept for the cell's return (a door of the base hinged to a
		// frame in the cell). Then the loads nobody wants any more.
		if (m_clearing)
			m_state.forgetCell(root);
		else
		{
			api::Ctx ctx = stateCtx(m_host, *m_world);
			m_state.cellUnloading(ctx, root);
		}
		m_host.cellUnloading(*m_world, root);
		if (PhysicsWorld* physics = m_host.cellPhysics())
			cellUnloadBodies(*physics, *m_world, root);
		if (const auto t = m_tokens.find(static_cast<uint32_t>(root)); t != m_tokens.end())
		{
			t->second.cancel();
			m_tokens.erase(t);
		}
	};
	// A cell is not started when the physics world has no room for its bodies.
	hooks.bodiesFit = [this](uint32_t bodies)
	{
		const PhysicsWorld* physics = m_host.cellPhysics();
		return !physics || cellBodiesFit(*physics, bodies);
	};
	m_streamer.begin(manifest, std::move(reader), std::move(hooks));
}

void CellRuntime::update(HorizonWorld& world, float dt, double budgetMs)
{
	const std::string& manifestJson = world.cellManifestJson();
	if (m_world != &world || manifestJson != m_manifestJson)
		begin(world, manifestJson);
	if (!m_streamer.active()) return;

	glm::dvec3 camera(0.0);
	if (!cameraAnchor(world, camera)) return;
	// The lookahead follows the camera's motion; a jump (a teleport, a respawn)
	// is not a speed, so anything faster than an airliner counts as standing.
	glm::vec3 velocity(0.0f);
	if (m_hasLastCamera && dt > 0.0f)
	{
		velocity = glm::vec3((camera - m_lastCamera) / static_cast<double>(dt));
		if (glm::length(velocity) > 300.0f) velocity = glm::vec3(0.0f);
	}
	m_lastCamera    = camera;
	m_hasLastCamera = true;
	m_streamer.update(world, camera, velocity, budgetMs);
}

void CellRuntime::clear(HorizonWorld& world)
{
	// The streamer only has cells in the world it streamed into; another world's
	// entities are not its to destroy.
	if (m_world == &world)
	{
		m_clearing = true;
		m_streamer.clear(world);
		m_clearing = false;
	}
	reset();
}

void CellRuntime::reset()
{
	dropTokens();
	m_streamer.reset();
	m_state.reset();
	m_world = nullptr;
	m_manifestJson.clear();
	m_hasLastCamera = false;
}

bool CellRuntime::cameraAnchor(HorizonWorld& world, glm::dvec3& absolute)
{
	for (auto e : world.registry().view<TransformComponent, CameraComponent>())
	{
		absolute = glm::dvec3(worldPositionOf(world, e)) + world.origin();
		return true;
	}
	return false;
}

} // namespace HE
