#include "HorizonScene/EntityHost.h"
#include <cstdint>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcCompiledLoader.h>   // HorizonCode::compiledClasses()
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonCode/HcClassResolve.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/MovementComponent.h>
#include <Diagnostics/Logger.h>
#include <filesystem>
#include <string>
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

// BY VALUE, not by const reference, and that is load-bearing. Two of this
// function's own callers pass `a->path` — a string owned by an asset in the
// content manager's pool — and the first thing done with it is a load that can
// insert into that pool. The pool is a dense vector, so an insert moves every
// asset in it and the argument would be pointing at freed memory inside the very
// call that was handed it. One copy per bind is nothing; the alternative is a
// use-after-free that only fires when the class happens not to be loaded yet.
HorizonCode::InstanceId EntityHost::bind(Entity entity, std::string classPath)
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
	// Same reason again, one level in: resolveClassAsset below loads every
	// ancestor of this class, and `a` does not survive that.
	const std::string assetPath = a->path;

	// Resolve the inheritance chain and flatten it: the graph that runs is this
	// class's own PLUS everything it inherits, with its overrides in place.
	// The identity carries the ancestry so a Cast to a parent class can be
	// answered without the runtime ever reading an asset.
	HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(*m_content, assetPath);
	const HorizonCode::ClassIdentity cls{ assetPath, rc.engineBase, rc.chain };
	HorizonCode::InstanceId inst = 0;
	if (auto compiled = HorizonCode::compiledClasses().create(assetPath))
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

EntityHost::Spawned EntityHost::spawn(const std::string& classPath, Entity parent,
                                      const float* position, const float* rotationEuler)
{
	Spawned out;
	if (!m_runtime || !m_content || !m_world) return out;

	const HE::UUID id = m_content->loadAsset(classPath);
	const HorizonCodeClassAsset* a = m_content->getHorizonCodeClass(id);
	if (!a) return out;

	// Everything this function still needs from the asset, taken NOW. Asset
	// pointers live in a dense vector, so the next load of anything not yet
	// registered moves them — and the very next line loads this class's whole
	// ancestor chain. `a` must be treated as dead from here on.
	const std::string assetPath = a->path;
	// Whether the body below is this class's OWN or something it inherits. Only
	// meaningful before the first load, like everything else read off `a`.
	const bool ownComponents = !a->componentBlob.empty();

	// The entity the class brings with it: its component list, which is stored in
	// the prefab payload format precisely so this is one call rather than a
	// second deserializer. INHERITED, not just its own — a class whose Components
	// tab was never opened has no blob of its own, and reading only that made a
	// freshly created Player Character spawn as a bare transform while the editor
	// showed it furnished. A class with nothing to inherit either still gets a
	// bare named entity: it is an Entity class, so it has a place in the world
	// even before anything has been put on it.
	if (const std::vector<uint8_t> comps = inheritedComponents(*m_content, *a); !comps.empty())
	{
		SceneSerializer ser;
		out.entity = ser.instantiatePrefab(*m_world, comps, parent);
		// An inherited body carries the NAME of whoever authored it — the
		// ancestor class, or the engine base ("Entity", "PlayerCharacter"). That
		// name is about the wrong thing: a spawned Goblin would stand in the
		// outliner as "Entity". The class's own file stem is the answer, and it
		// is the same one the bare-entity path below already gives.
		if (!ownComponents && out.entity != entt::null)
			if (auto* nc = m_world->registry().try_get<NameComponent>(out.entity))
				if (const std::string stem = std::filesystem::path(assetPath).stem().string();
				    !stem.empty())
					nc->name = stem;
	}
	if (out.entity == entt::null)
	{
		// The file stem, not the asset's stored `name` — that one is the META
		// chunk's, written once at creation and never rewritten by a rename, so
		// every spawned Goblin would stand in the outliner as "NewClass".
		const std::string stem = std::filesystem::path(assetPath).stem().string();
		out.entity = m_world->createEntity(stem.empty() ? "Entity" : stem);
		if (parent != entt::null && m_world->registry().valid(parent))
			m_world->reparentEntity(out.entity, parent);
	}

	// Placement BEFORE bind(), because bind() fires Construct and BeginPlay: a
	// graph that reads its own position, raycasts down for the ground or hands
	// its transform to physics has to see the spawn point, not the origin it
	// would be teleported away from a frame later. Each pointer is honoured on
	// its own — a wired Location with an authored Rotation is a legal request.
	// get_or_emplace, not try_get: a class whose component list is empty comes
	// out of createEntity() above without a transform, and asking for a placement
	// is what makes it need one.
	if (position || rotationEuler)
	{
		auto& t = m_world->registry().get_or_emplace<TransformComponent>(out.entity);
		if (position)      t.position = glm::vec3(position[0], position[1], position[2]);
		if (rotationEuler) t.rotation = glm::vec3(rotationEuler[0], rotationEuler[1], rotationEuler[2]);
		t.dirty = true;
	}

	// Physics BEFORE bind(), for the same reason the placement is: bind() fires
	// Construct and BeginPlay before it returns, and a graph's opening move is
	// routinely a physics one — raycast down for the ground, add an impulse to a
	// projectile, ask whether it is grounded. Building the body afterwards would
	// answer every one of those against a bodiless world for exactly the frame in
	// which the answer matters most.
	//
	// Wired here rather than in the applications' Create Object service because
	// this is the only point that both precedes bind() and knows the new entity:
	// both services keep only the instance id and throw Spawned::entity away.
	//
	// The whole subtree: the class's component list is a prefab, and a
	// PlayerCharacter's arrives with children.
	if (m_physics)
		m_physics->addEntityTree(*m_world, static_cast<uint32_t>(out.entity));

	out.instance = bind(out.entity, classPath);
	if (!out.instance)
	{
		// Never leave a body without its logic standing in the scene — and that
		// now means the physics body too. Before destroyEntity, while the
		// hierarchy this walks still exists.
		if (m_physics)
			m_physics->removeEntityTree(*m_world, static_cast<uint32_t>(out.entity));
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
		if (!m_world->registry().valid(static_cast<Entity>(raw)))
		{
			dead.push_back(inst);
			// The entity went away by some path that did not hand its body back
			// (a graph's entity.destroy, the outliner). PhysicsWorld sweeps for
			// these itself, but only on its next step — until then the collider
			// stands there invisibly, blocking and answering raycasts. Returning
			// it the moment we notice costs nothing and closes that window.
			// Safe inside the loop: this touches Jolt, never m_byEntity.
			if (m_physics) m_physics->removeEntity(raw);
		}
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

std::vector<uint8_t> EntityHost::inheritedComponents(ContentManager& content,
                                                     const HorizonCodeClassAsset& asset)
{
	if (!asset.componentBlob.empty()) return asset.componentBlob;

	// COPIED before the first resolve, and this is not tidiness. ContentManager
	// hands out pointers into a dense vector (SlotMap), so loading an asset that
	// is not registered yet reallocates it and moves every outstanding pointer —
	// including `asset` itself, and including the string this call is reading
	// from. resolveClassAsset loads every ancestor it walks, so the reference
	// would be dead inside the call that was given it.
	const std::string path = asset.path;

	// The chain, nearest ancestor first. Walked to the END rather than stopping
	// at the immediate parent: if a Goblin derives from an Enemy that itself
	// never opened its Components tab, the body it means to inherit is the one
	// further up, and stopping early would silently hand it the bare engine
	// default instead.
	HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(content, path);
	for (const std::string& ancestor : rc.chain)
		if (const HorizonCodeClassAsset* parent =
		        content.getHorizonCodeClass(content.loadAsset(ancestor)))
			if (!parent->componentBlob.empty()) return parent->componentBlob;

	return defaultComponents(rc.engineBase);
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
		// What it is doing, in the form an animator reads it. Without this the
		// character controller is the only source, and every project rederives
		// "how fast" and "on the ground" by hand — differently each time.
		scratch.addComponent(root, MovementComponent{});

		// …and a camera to see it with. A character class without one is a
		// character nobody can look at: the author has to know that a camera is a
		// separate entity, that a rig aims it, and that the rig's target defaults
		// to the possessed player. Shipping it wired up answers all three by
		// showing the answer — in the Outliner it reads "Player → Camera", which
		// is the relationship, spelled out.
		//
		// A CHILD, because defaultComponents serialises a subtree and the class is
		// its root. The rig writes its camera's transform in parent space, so
		// being parented to the very thing it follows is fine — it is the same
		// arithmetic, just with a non-identity parent.
		const Entity camera = scratch.createEntity("Camera");
		scratch.addComponent(camera, TransformComponent{});
		CameraComponent cam;
		cam.isMain = true;   // this is the camera the game renders through
		scratch.addComponent(camera, cam);
		// Target stays empty = "the possessed player", which is this very class.
		scratch.addComponent(camera, CameraRigComponent{});
		scratch.reparentEntity(camera, root);
	}

	SceneSerializer ser;
	return ser.serializeSubtree(scratch, root);
}
