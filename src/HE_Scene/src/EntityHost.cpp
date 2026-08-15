#include "HorizonScene/EntityHost.h"
#include <cstdint>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcCompiledLoader.h>   // HorizonCode::compiledClasses()
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonCode/HcClassResolve.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <Diagnostics/Logger.h>
#include <vector>

namespace
{
// The class asset behind a ScriptComponent, or null when that component points
// at something else (a .lua/.py script — ScriptContext's business) or at nothing
// at all. Deliberately quiet: every entity in the world is offered here, and
// "not mine" is the common answer, not a problem.
const HorizonCodeClassAsset* classAssetOf(ContentManager& cm, const ScriptComponent& sc)
{
    if (!sc.enabled) return nullptr;
    if (cm.assetType(sc.scriptAssetId) != HE::AssetType::HorizonCodeClass) return nullptr;
    return cm.getHorizonCodeClass(sc.scriptAssetId);
}
}

void EntityHost::begin(HorizonCode::Runtime& runtime, HorizonWorld& world, ContentManager& cm)
{
	end();
	m_runtime = &runtime;
	m_world   = &world;
	m_content = &cm;

	// Same walk ScriptContext::startWorldScripts does, over the same component —
	// the two split on the referenced asset's TYPE, so an entity belongs to
	// exactly one of them and neither needs to know about the other.
	size_t bound = 0;
	for (auto [entity, sc] : world.registry().view<ScriptComponent>().each())
	{
		const HorizonCodeClassAsset* a = classAssetOf(cm, sc);
		if (!a) continue;
		if (bind(entity, a->path) != 0) ++bound;
	}
	if (bound > 0)
		HE_LOG_INFO(HorizonCode, "EntityHost: %zu entity class instance(s) running", bound);
}

int EntityHost::bindFor(const std::vector<Entity>& entities)
{
	if (!m_runtime || !m_world || !m_content) return 0;
	int bound = 0;
	for (const Entity e : entities)
	{
		if (!m_world->registry().valid(e)) continue;
		const auto* sc = m_world->registry().try_get<ScriptComponent>(e);
		if (!sc) continue;
		const HorizonCodeClassAsset* a = classAssetOf(*m_content, *sc);
		if (!a) continue;
		if (bind(e, a->path) != 0) ++bound;
	}
	return bound;
}

HorizonCode::InstanceId EntityHost::bind(Entity entity, const std::string& classPath)
{
	if (!m_runtime || !m_content || !m_world) return 0;
	if (!m_world->registry().valid(entity)) return 0;

	const HE::UUID id = m_content->loadAsset(classPath);
	const HorizonCodeClassAsset* a = m_content->getHorizonCodeClass(id);
	if (!a)
	{
		HE_LOG_ERROR(HorizonCode, "Entity %u: HorizonCode class '%s' was not found — "
		                          "no logic will run on this entity",
		             static_cast<uint32_t>(entity), classPath.c_str());
		return 0;
	}

	// Resolve the inheritance chain and flatten it: the graph that runs is this
	// class's own PLUS everything it inherits, with its overrides in place.
	// The identity carries the ancestry so a Cast to a parent class can be
	// answered without the runtime ever reading an asset.
	HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(*m_content, a->path);
	const HorizonCode::ClassIdentity cls{ a->path, rc.engineBase, rc.chain };
	HorizonCode::InstanceId inst = 0;
	if (auto compiled = HorizonCode::compiledClasses().create(a->path))
		inst = m_runtime->addCompiled(std::move(compiled), {}, cls);
	else
		inst = m_runtime->addLevels(std::move(rc.levels), {}, cls);
	if (!inst) return 0;

	const uint32_t raw = static_cast<uint32_t>(entity);
	m_runtime->setOwnedEntity(inst, raw);
	m_byEntity[raw]    = inst;
	m_byInstance[inst] = raw;

	m_runtime->fireConstruct(inst);
	m_runtime->fireBeginPlay(inst);
	return inst;
}

EntityHost::Spawned EntityHost::spawn(const std::string& classPath, Entity parent)
{
	Spawned out;
	if (!m_runtime || !m_content || !m_world) return out;

	const HE::UUID id = m_content->loadAsset(classPath);
	const HorizonCodeClassAsset* a = m_content->getHorizonCodeClass(id);
	if (!a) return out;

	// The entity the class brings with it: its authored component list, which is
	// stored in the prefab payload format precisely so this is one call rather
	// than a second deserializer. A class with no components still gets a bare
	// named entity — it is an Entity class, so it has a place in the world even
	// before anything has been put on it.
	if (!a->componentBlob.empty())
	{
		SceneSerializer ser;
		out.entity = ser.instantiatePrefab(*m_world, a->componentBlob, parent);
	}
	if (out.entity == entt::null)
	{
		out.entity = m_world->createEntity(a->name.empty() ? "Entity" : a->name);
		if (parent != entt::null && m_world->registry().valid(parent))
			m_world->reparentEntity(out.entity, parent);
	}

	out.instance = bind(out.entity, classPath);
	if (!out.instance)
	{
		// Never leave a body without its logic standing in the scene.
		m_world->destroyEntity(out.entity);
		out.entity = entt::null;
	}
	return out;
}

void EntityHost::tick(float dt)
{
	if (!m_runtime || !m_world) return;

	// Reap first, tick second. An instance whose entity is gone — anything can
	// destroy an entity, from entity.destroy in a graph to the outliner's delete
	// — would otherwise keep ticking, keep answering a Cast, and hand out a
	// dangling entity id from entityOf. Watching the registry here means every
	// path that removes an entity is covered without any of them knowing about
	// this host.
	std::vector<HorizonCode::InstanceId> dead;
	for (const auto& [raw, inst] : m_byEntity)
		if (!m_world->registry().valid(static_cast<Entity>(raw))) dead.push_back(inst);
	for (const HorizonCode::InstanceId inst : dead) unbind(inst);

	// Tick over a SNAPSHOT, never over the live map. A graph is running here,
	// and a graph may Create Object (inserting an entity class, possibly
	// rehashing) or destroy one (erasing) — iterating m_byEntity across that is
	// undefined behaviour, and the kind that survives every test and crashes in
	// a shipped game. Anything spawned during this pass ticks from the NEXT
	// frame, which is also the more predictable rule.
	m_tickScratch.clear();
	m_tickScratch.reserve(m_byEntity.size());
	for (const auto& [raw, inst] : m_byEntity) m_tickScratch.push_back(inst);
	for (const HorizonCode::InstanceId inst : m_tickScratch)
		if (m_runtime->alive(inst)) m_runtime->fireTick(inst, dt);
}

void EntityHost::unbind(HorizonCode::InstanceId instance)
{
	const auto it = m_byInstance.find(instance);
	if (it == m_byInstance.end()) return;
	const uint32_t raw = it->second;
	m_byInstance.erase(it);
	m_byEntity.erase(raw);
	if (m_runtime) m_runtime->destroy(instance);   // fires Destruct
}

void EntityHost::end()
{
	if (m_runtime)
	{
		// Snapshot for the same reason tick() does: Destruct is graph code, and
		// it can create or destroy other entity classes while this loop runs.
		std::vector<HorizonCode::InstanceId> all;
		all.reserve(m_byEntity.size());
		for (const auto& [raw, inst] : m_byEntity) all.push_back(inst);
		// Cleared FIRST so anything a Destruct handler spawns lands in a map
		// this teardown is no longer walking.
		m_byEntity.clear();
		m_byInstance.clear();
		for (const HorizonCode::InstanceId inst : all) m_runtime->destroy(inst);
	}
	m_byEntity.clear();
	m_byInstance.clear();
	m_runtime = nullptr;
	m_world   = nullptr;
	m_content = nullptr;
}

HorizonCode::InstanceId EntityHost::instanceOf(Entity entity) const
{
	const auto it = m_byEntity.find(static_cast<uint32_t>(entity));
	return it != m_byEntity.end() ? it->second : 0;
}

Entity EntityHost::entityOf(HorizonCode::InstanceId instance) const
{
	const auto it = m_byInstance.find(instance);
	return it != m_byInstance.end() ? static_cast<Entity>(it->second) : entt::null;
}

std::vector<uint8_t> EntityHost::defaultComponents(const std::string& baseClass)
{
	// Nothing above Entity has a body, so nothing above Entity gets components.
	if (!HorizonCode::engineClassIsA(baseClass, "Entity")) return {};
	// A PlayerController is an Entity in the taxonomy — that is what keeps Cast,
	// possession and the event catalog to ONE chain — but it is not something you
	// place in a level. Handing it a transform would only put a component in its
	// Components tab that nobody ever wants and everybody has to delete.
	if (HorizonCode::engineClassIsA(baseClass, "PlayerController")) return {};

	// A throwaway world just to author the subtree: serializeSubtree is the one
	// encoder for this format, and going through it here means the class blob and
	// a prefab can never disagree about what they contain.
	HorizonWorld scratch;
	const Entity root = scratch.createEntity(baseClass.empty() ? "Entity" : baseClass);
	scratch.addComponent(root, TransformComponent{});

	if (HorizonCode::engineClassIsA(baseClass, "PlayerCharacter"))
	{
		// The minimum a player needs to be moved and to be hit. The mesh stays
		// unassigned on purpose — which mesh it is is the whole point of the
		// class, so guessing one would only be something to delete.
		scratch.addComponent(root, CharacterControllerComponent{});
		ColliderComponent col;
		col.shape  = ColliderShape::Capsule;
		col.radius = 0.35f;
		col.height = 1.8f;
		scratch.addComponent(root, col);
		RigidBodyComponent rb;
		rb.type = RigidBodyType::Kinematic;   // the character controller drives it
		scratch.addComponent(root, rb);
		scratch.addComponent(root, SkeletalMeshComponent{});
	}

	SceneSerializer ser;
	return ser.serializeSubtree(scratch, root);
}
