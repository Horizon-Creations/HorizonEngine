#include "doctest.h"

#include "EditorCommands.h"
#include "EditorUndo.h"
#include "CollabUndo.h"

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>

#include <map>
#include <string>
#include <vector>

// ─── The one door into the scene ─────────────────────────────────────────────
// EditorCommands is what the UI's structural commands, a peer's edit and (later)
// an external MCP client all go through, so the questions worth asking here are
// about the DIFFERENCE between those callers: does a peer's edit stay off our
// undo stack and off the wire, does a human's edit land on the right one of the
// two stacks, and does undoing a delete bring the entity back under the identity
// every later edit names it by.

using HE::Ed::Command;
using HE::Ed::CmdError;
using HE::Ed::EditorCommands;
using HE::Ed::Origin;

// A refusal that fails a check should say WHICH refusal it was — the codes are
// the whole interface an external caller has for deciding what to do next, and
// "3 != 5" would be a worse message than the enum deserves.
namespace doctest {
template <>
struct StringMaker<HE::Ed::CmdError>
{
	static String convert(const HE::Ed::CmdError& e) { return HE::Ed::errorName(e); }
};
} // namespace doctest

namespace {

int authoredCount(HorizonWorld& w)
{
	auto& reg = w.registry();
	int n = 0;
	for (auto e : reg.view<entt::entity>())
		if (!reg.all_of<EnvironmentLightComponent>(e)) ++n;
	return n;
}

Entity findByName(HorizonWorld& w, const std::string& name)
{
	for (auto [e, n] : w.registry().view<NameComponent>().each())
		if (n.name == name) return e;
	return entt::null;
}

Entity parentOf(HorizonWorld& w, Entity e)
{
	const auto* h = w.registry().try_get<HierarchyComponent>(e);
	return (h && w.registry().valid(h->parent)) ? h->parent : entt::null;
}

// The gateway with everything around it faked, so what a command DID can be
// counted rather than inferred. Subjects are handed out here the way the collab
// controller hands them out — one stable number per entity uuid — because the
// undo entries are written in that currency and a test that used raw handles
// would not notice if the production code did too.
struct Harness
{
	HorizonWorld   world;
	EditorCommands cmds;
	EditorUndo     snapshotUndo;
	CollabUndo     collabUndo;

	HE::Ed::SnapshotUndoSink snapshotSink{ &snapshotUndo, [this] { return playing; } };
	HE::Ed::CollabUndoSink   collabSink{ &collabUndo };

	bool playing   = false;
	bool sessionOn = false;

	// Subject bookkeeping, keyed by uuid so it survives a destroy and the
	// re-create that undoes it — which is the whole point of not using handles.
	std::map<std::string, std::uint64_t> subjectByUuid;
	std::uint64_t nextSubject = 1000;

	std::vector<std::uint64_t> ownedLocks;
	std::vector<std::uint64_t> lockedElsewhere;
	std::vector<std::uint64_t> lockRequests;

	int transformPublishes  = 0;
	int componentPublishes  = 0;
	std::vector<Entity> created;
	std::vector<Entity> destroyed;
	int applied = 0;

	Harness()
	{
		snapshotUndo.setWorld(&world);
		cmds.setWorld(&world);

		EditorCommands::Hooks h;
		h.isPlaying = [this] { return playing; };
		h.inSession = [this] { return sessionOn; };
		h.nowMs     = [] { return std::uint64_t{ 0 }; };
		h.subjectFor = [this](std::uint32_t handle) { return subjectFor(handle); };
		h.ownsLock = [this](std::uint64_t s) {
			return std::find(ownedLocks.begin(), ownedLocks.end(), s) != ownedLocks.end();
		};
		h.lockedByOther = [this](std::uint64_t s) {
			return std::find(lockedElsewhere.begin(), lockedElsewhere.end(), s)
			       != lockedElsewhere.end();
		};
		h.requestLock = [this](std::uint64_t s) { lockRequests.push_back(s); return true; };
		h.publishTransform = [this](std::uint64_t, const float[3], const float[3],
		                            const float[3], std::uint64_t) { ++transformPublishes; };
		h.publishComponents = [this](std::uint32_t, const std::vector<std::uint8_t>&) {
			++componentPublishes;
		};
		h.afterCreate    = [this](Entity e, Origin) { created.push_back(e); };
		h.beforeDestroy  = [this](Entity e, Origin) {
			// Runs while the subtree still exists — that is the contract the
			// physics teardown in the editor depends on.
			REQUIRE(world.registry().valid(e));
			destroyed.push_back(e);
		};
		h.onApplied = [this](const Command&, Origin, const HE::Ed::Result&) { ++applied; };
		cmds.setHooks(std::move(h));

		cmds.setUndoSinks(&snapshotSink, &collabSink);

		// The inverse stack reaches the world the same way the editor wires it:
		// subject → handle, then the same operations the gateway itself applies.
		collabUndo.setHandlers(
			[this](std::uint64_t s, const float v[9]) {
				const Entity e = entityFor(s);
				if (e == entt::null) return;
				cmds.execute(Command::setTransform(e, v), Origin::Remote);
			},
			[](const std::string&, const std::vector<std::uint8_t>&) {},
			[this](std::uint64_t s) {
				return std::find(ownedLocks.begin(), ownedLocks.end(), s) != ownedLocks.end();
			});
		collabUndo.setStructuralHandlers(
			[this](std::uint64_t, const std::vector<std::uint8_t>& blob,
			       std::uint64_t parentSubject) {
				cmds.execute(Command::create(entityFor(parentSubject), blob,
				                             /*preserveIds=*/true), Origin::Remote);
			},
			[this](std::uint64_t s) {
				const Entity e = entityFor(s);
				if (e != entt::null) cmds.execute(Command::destroy(e), Origin::Remote);
			},
			[this](std::uint64_t s, std::uint64_t parentSubject) {
				const Entity e = entityFor(s);
				if (e != entt::null)
					cmds.execute(Command::reparent(e, entityFor(parentSubject)),
					             Origin::Remote);
			},
			[this](std::uint64_t s, const std::vector<std::uint8_t>& blob) {
				const Entity e = entityFor(s);
				if (e != entt::null)
					cmds.execute(Command::setComponents(e, blob), Origin::Remote);
			});
	}

	std::uint64_t subjectFor(std::uint32_t handle)
	{
		const auto e = static_cast<Entity>(static_cast<entt::id_type>(handle));
		const std::string uuid = HE::Ed::uuidOf(world, e);
		if (uuid.empty()) return 0;
		auto it = subjectByUuid.find(uuid);
		if (it != subjectByUuid.end()) return it->second;
		const std::uint64_t s = ++nextSubject;
		subjectByUuid.emplace(uuid, s);
		return s;
	}

	// The reverse direction, rebuilt from the world every time: after an undo
	// the entity is a different handle under the same uuid.
	Entity entityFor(std::uint64_t subject)
	{
		if (subject == 0) return entt::null;
		for (const auto& [uuid, s] : subjectByUuid)
		{
			if (s != subject) continue;
			return HE::Ed::entityByUuid(world, uuid);
		}
		return entt::null;
	}

	std::vector<std::uint8_t> subtreeOf(Entity e)
	{
		SceneSerializer s;
		return s.serializeSubtree(world, e);
	}
};

} // namespace

TEST_CASE("Gateway create and destroy, with snapshot undo outside a session")
{
	Harness h;
	const Entity src = h.world.createEntity("Crate");

	// Duplicate, the way EditorApplication does it: capture the subtree, then
	// hand the blob to the gateway with fresh uuids (two copies of one thing are
	// two identities).
	const auto blob = h.subtreeOf(src);
	REQUIRE_FALSE(blob.empty());

	const auto res = h.cmds.execute(
		Command::create(h.world.rootEntity(), blob, /*preserveIds=*/false), Origin::User);
	REQUIRE(res.ok());
	CHECK((res.root != entt::null));
	CHECK(h.created.size() == 1);
	CHECK(h.applied == 1);
	CHECK(authoredCount(h.world) == 3);   // root + Crate + copy

	// The snapshot went on the stack before the world was touched.
	REQUIRE(h.snapshotUndo.canUndo());
	CHECK(h.snapshotUndo.undo());
	CHECK(authoredCount(h.world) == 2);

	CHECK(h.snapshotUndo.redo());
	CHECK(authoredCount(h.world) == 3);
}

TEST_CASE("A peer's edit records nothing and publishes nothing")
{
	Harness h;
	h.sessionOn = true;
	const Entity e = h.world.createEntity("Crate");
	h.world.registry().emplace<TransformComponent>(e);

	const float v[9] = { 5, 6, 7, 0, 0, 0, 1, 1, 1 };
	REQUIRE(h.cmds.execute(Command::setTransform(e, v), Origin::Remote).ok());

	const auto& tc = h.world.registry().get<TransformComponent>(e);
	CHECK(tc.position.x == doctest::Approx(5.0f));
	CHECK(tc.dirty);

	// Neither stack, and nothing on the wire: this change already happened
	// elsewhere, and both would be an echo of somebody else's work.
	CHECK_FALSE(h.snapshotUndo.canUndo());
	CHECK(h.collabUndo.undoDepth() == 0);
	CHECK(h.transformPublishes == 0);

	// A destroy from a peer is the same story.
	REQUIRE(h.cmds.execute(Command::destroy(e), Origin::Remote).ok());
	CHECK(h.collabUndo.undoDepth() == 0);
	CHECK_FALSE(h.snapshotUndo.canUndo());
}

TEST_CASE("Own edits publish and land on the session stack while in a session")
{
	Harness h;
	h.sessionOn = true;
	const Entity e = h.world.createEntity("Crate");
	h.world.registry().emplace<TransformComponent>(e);

	const float v[9] = { 1, 2, 3, 0, 0, 0, 1, 1, 1 };
	REQUIRE(h.cmds.execute(Command::setTransform(e, v), Origin::User).ok());
	CHECK(h.transformPublishes == 1);
	CHECK(h.collabUndo.undoDepth() == 1);
	// The whole-world stack stays out of it: a snapshot undo inside a session
	// would revert everyone else's work too.
	CHECK_FALSE(h.snapshotUndo.canUndo());

	SceneSerializer ser;
	const auto comps = ser.serializeEntityComponents(h.world, e);
	REQUIRE_FALSE(comps.empty());
	REQUIRE(h.cmds.execute(Command::setComponents(e, comps), Origin::User).ok());
	CHECK(h.componentPublishes == 1);
	CHECK(h.collabUndo.undoDepth() == 2);
}

TEST_CASE("Undoing a delete in a session brings the entity back under its uuid")
{
	Harness h;
	h.sessionOn = true;

	const Entity e = h.world.createEntity("Crate");
	h.world.registry().emplace<TransformComponent>(e);
	const std::string uuid = HE::Ed::uuidOf(h.world, e);
	REQUIRE_FALSE(uuid.empty());
	const std::uint64_t subject = h.subjectFor(static_cast<std::uint32_t>(entt::to_integral(e)));
	h.ownedLocks.push_back(subject);

	REQUIRE(h.cmds.execute(Command::destroy(e), Origin::User).ok());
	CHECK(h.destroyed.size() == 1);
	CHECK((findByName(h.world, "Crate") == entt::null));
	REQUIRE(h.collabUndo.undoDepth() == 1);

	// The lock on a destroyed subject is gone by construction — nobody can hold
	// one on an entity that does not exist. If dropUnowned applied its rule
	// here, this entry would already be off the stack and a delete would be
	// permanent.
	h.ownedLocks.clear();

	REQUIRE(h.collabUndo.undo());
	const Entity back = HE::Ed::entityByUuid(h.world, uuid);
	CHECK((back != entt::null));
	CHECK(h.subjectFor(static_cast<std::uint32_t>(entt::to_integral(back))) == subject);

	// And forward again.
	REQUIRE(h.collabUndo.redo());
	CHECK((HE::Ed::entityByUuid(h.world, uuid) == entt::null));
}

TEST_CASE("Undoing a create in a session removes what it made")
{
	Harness h;
	h.sessionOn = true;
	const Entity src = h.world.createEntity("Crate");
	const auto blob = h.subtreeOf(src);

	const auto res = h.cmds.execute(
		Command::create(h.world.rootEntity(), blob, /*preserveIds=*/false), Origin::User);
	REQUIRE(res.ok());
	const std::string uuid = HE::Ed::uuidOf(h.world, res.root);
	CHECK(authoredCount(h.world) == 3);
	REQUIRE(h.collabUndo.undoDepth() == 1);

	REQUIRE(h.collabUndo.undo());
	CHECK(authoredCount(h.world) == 2);
	CHECK((HE::Ed::entityByUuid(h.world, uuid) == entt::null));

	// Redo puts it back under the SAME uuid — the entry replays with preserved
	// ids, so every other entry that names this subject still means it.
	REQUIRE(h.collabUndo.redo());
	CHECK(authoredCount(h.world) == 3);
	CHECK((HE::Ed::entityByUuid(h.world, uuid) != entt::null));
}

TEST_CASE("Reparent and its inverse")
{
	Harness h;
	h.sessionOn = true;
	const Entity a = h.world.createEntity("A");
	const Entity b = h.world.createEntity("B");
	const std::string uuidA = HE::Ed::uuidOf(h.world, a);

	h.ownedLocks.push_back(h.subjectFor(static_cast<std::uint32_t>(entt::to_integral(a))));

	REQUIRE(h.cmds.execute(Command::reparent(a, b), Origin::User).ok());
	CHECK((parentOf(h.world, a) == b));

	REQUIRE(h.collabUndo.undo());
	CHECK((parentOf(h.world, a) == h.world.rootEntity()));
	REQUIRE(h.collabUndo.redo());
	CHECK((parentOf(h.world, a) == b));

	// A cycle is refused by the world, and the gateway reports that rather than
	// leaving a half-applied hierarchy behind.
	const auto bad = h.cmds.execute(Command::reparent(b, a), Origin::User);
	CHECK(bad.error == CmdError::Failed);
	CHECK((parentOf(h.world, a) == b));
}

TEST_CASE("Component writes are invertible")
{
	Harness h;
	h.sessionOn = true;
	const Entity e = h.world.createEntity("Crate");
	auto& tc = h.world.registry().emplace<TransformComponent>(e);
	tc.position = { 1, 1, 1 };
	h.ownedLocks.push_back(h.subjectFor(static_cast<std::uint32_t>(entt::to_integral(e))));

	SceneSerializer ser;
	tc.position = { 9, 9, 9 };
	const auto after = ser.serializeEntityComponents(h.world, e);
	tc.position = { 1, 1, 1 };

	REQUIRE(h.cmds.execute(Command::setComponents(e, after), Origin::User).ok());
	CHECK(h.world.registry().get<TransformComponent>(e).position.x == doctest::Approx(9.0f));

	REQUIRE(h.collabUndo.undo());
	CHECK(h.world.registry().get<TransformComponent>(e).position.x == doctest::Approx(1.0f));
	REQUIRE(h.collabUndo.redo());
	CHECK(h.world.registry().get<TransformComponent>(e).position.x == doctest::Approx(9.0f));
}

TEST_CASE("Refusals name their reason and leave the world alone")
{
	Harness h;
	const Entity e = h.world.createEntity("Crate");
	h.world.registry().emplace<TransformComponent>(e);
	const float v[9] = { 1, 2, 3, 0, 0, 0, 1, 1, 1 };

	SUBCASE("an unknown entity")
	{
		const auto bad = h.cmds.execute(Command::destroy(static_cast<Entity>(9999)),
		                                Origin::User);
		CHECK(bad.error == CmdError::NotFound);
		CHECK(authoredCount(h.world) == 2);
	}

	SUBCASE("a built-in")
	{
		// The world root is one (HorizonWorld::isBuiltin), and it is the one a
		// mis-addressed command is most likely to hit.
		const Entity builtin = h.world.rootEntity();
		CHECK(h.cmds.execute(Command::destroy(builtin), Origin::User).error
		      == CmdError::Builtin);
		CHECK(h.world.registry().valid(builtin));

		// A peer's message is applied regardless: their editor already did it,
		// and refusing here would leave the two worlds silently different.
		// Reparenting under the root is the harmless way to see that.
		CHECK(h.cmds.execute(Command::reparent(e, builtin), Origin::Remote).ok());
	}

	SUBCASE("play mode, for an external caller only")
	{
		h.playing = true;
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::External).error
		      == CmdError::PlayMode);
		CHECK(h.world.registry().get<TransformComponent>(e).position.x == doctest::Approx(0.0f));

		// A human doing it is a spawn like any other, and the editor has always
		// allowed it — the play session simply gets no undo entry.
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::User).ok());
		CHECK_FALSE(h.snapshotUndo.canUndo());
	}

	SUBCASE("an empty payload")
	{
		CHECK(h.cmds.execute(Command::create(h.world.rootEntity(), {}, false), Origin::User)
		          .error == CmdError::InvalidPayload);
		CHECK(authoredCount(h.world) == 2);
	}

	SUBCASE("no world at all")
	{
		EditorCommands bare;
		CHECK(bare.execute(Command::destroy(e), Origin::User).error == CmdError::NoWorld);
	}
}

TEST_CASE("Session locks gate an external caller, not the editor's own commands")
{
	Harness h;
	h.sessionOn = true;
	const Entity e = h.world.createEntity("Crate");
	h.world.registry().emplace<TransformComponent>(e);
	const std::uint64_t subject =
		h.subjectFor(static_cast<std::uint32_t>(entt::to_integral(e)));
	const float v[9] = { 1, 2, 3, 0, 0, 0, 1, 1, 1 };

	SUBCASE("held by someone else")
	{
		h.lockedElsewhere.push_back(subject);
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::External).error
		      == CmdError::LockedByOther);
		CHECK(h.world.registry().get<TransformComponent>(e).position.x == doctest::Approx(0.0f));
		CHECK(h.lockRequests.empty());   // no point asking for what is taken
	}

	SUBCASE("free, but the grant is a round trip away")
	{
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::External).error
		      == CmdError::LockPending);
		REQUIRE(h.lockRequests.size() == 1);
		CHECK(h.lockRequests[0] == subject);

		// Once granted, the same command goes through.
		h.ownedLocks.push_back(subject);
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::External).ok());
	}

	SUBCASE("the editor's own commands are not gated, as before the gateway")
	{
		h.lockedElsewhere.push_back(subject);
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::User).ok());

		// …unless the editor asks for it, which is the switch step 3 flips once
		// the UI has something to say when the answer is "not yet".
		h.cmds.setLockUserCommands(true);
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::User).error
		      == CmdError::LockedByOther);
	}

	SUBCASE("outside a session nothing is gated")
	{
		h.sessionOn = false;
		h.lockedElsewhere.push_back(subject);
		CHECK(h.cmds.execute(Command::setTransform(e, v), Origin::External).ok());
	}
}

TEST_CASE("A destroy carries the whole subtree, and undo restores all of it")
{
	Harness h;
	h.sessionOn = true;
	const Entity parent = h.world.createEntity("Parent");
	const Entity child  = h.world.createEntity("Child");
	REQUIRE(h.world.reparentEntity(child, parent));
	const std::string childUuid = HE::Ed::uuidOf(h.world, child);

	h.ownedLocks.push_back(
		h.subjectFor(static_cast<std::uint32_t>(entt::to_integral(parent))));

	REQUIRE(h.cmds.execute(Command::destroy(parent), Origin::User).ok());
	CHECK((findByName(h.world, "Child") == entt::null));

	REQUIRE(h.collabUndo.undo());
	const Entity backChild = HE::Ed::entityByUuid(h.world, childUuid);
	CHECK((backChild != entt::null));
	// And back where it was in the hierarchy, not orphaned at the root.
	CHECK((parentOf(h.world, backChild) != h.world.rootEntity()));
	CHECK((parentOf(h.world, backChild) != entt::null));
}

TEST_CASE("entityByUuid finds what uuidOf named, and nothing else")
{
	Harness h;
	const Entity a = h.world.createEntity("A");
	const Entity b = h.world.createEntity("B");

	const std::string ua = HE::Ed::uuidOf(h.world, a);
	const std::string ub = HE::Ed::uuidOf(h.world, b);
	CHECK(ua.size() == 32);
	CHECK(ua != ub);
	CHECK((HE::Ed::entityByUuid(h.world, ua) == a));
	CHECK((HE::Ed::entityByUuid(h.world, ub) == b));
	CHECK((HE::Ed::entityByUuid(h.world, "") == entt::null));
	CHECK((HE::Ed::entityByUuid(h.world, std::string(32, '0')) == entt::null));
}
