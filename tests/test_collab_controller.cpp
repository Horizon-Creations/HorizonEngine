#include "doctest.h"

#include "../src/HE_Editor/CollabController.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonScene.h>

#include <chrono>
#include <string>
#include <thread>

// ─── Editor ↔ network integration ────────────────────────────────────────────
// The unit tests in test_net_collab.cpp exercise the protocol against a fake
// state provider. These drive the REAL path the editor uses: two CollabControllers
// over actual TCP, with SceneSerializer moving genuine HorizonWorld content
// between them. That is the seam a fake can never cover — a scene that
// serializes but fails to reload, or a snapshot merged into the joiner's world
// instead of replacing it, would pass every protocol test and still be broken.

namespace {

// Drive both ends until `done` or the deadline. Sockets are asynchronous, so
// nothing here may be assumed to have happened without pumping.
template <typename Fn>
bool pumpUntil(CollabController& host, CollabController& client, Fn done,
               std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint64_t now = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        host.update(now);
        client.update(now);
        if (done()) return true;
        now += 16;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    host.update(now);
    client.update(now);
    return done();
}

std::size_t countEntities(HorizonWorld& world) {
    std::size_t n = 0;
    world.registry().view<entt::entity>().each([&](auto) { ++n; });
    return n;
}

} // namespace

TEST_CASE("CollabController: hosting opens a port and produces a join code")
{
    CollabController host;
    HorizonWorld world;
    host.setWorld(&world);

    REQUIRE(host.startHosting(0, "Anna"));   // 0 = OS picks a free port
    CHECK(host.status() == CollabController::Status::Hosting);
    CHECK(host.isHost());
    CHECK(host.port() != 0);
    CHECK_FALSE(host.localAddress().empty());

    // Machine-generated, not user-chosen: a weak passphrase would be
    // brute-forceable offline from a captured handshake.
    CHECK(host.joinCode().size() >= 16);

    host.leave();
    CHECK(host.status() == CollabController::Status::Idle);
    CHECK_FALSE(host.active());
}

TEST_CASE("CollabController: a joiner receives the host's actual scene")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");
    hostWorld.createEntity("Camera");
    hostWorld.createEntity("Sun");
    const std::size_t hostCount = countEntities(hostWorld);
    REQUIRE(hostCount >= 3);

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);

    bool worldReplaced = false;
    client.onWorldReplaced([&] { worldReplaced = true; });

    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));

    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    // The scene really crossed the wire and deserialized.
    CHECK(countEntities(clientWorld) == hostCount);

    // The editor must be told, or its selection and undo stack would still refer
    // to entities from the world that was just thrown away.
    CHECK(worldReplaced);

    // Both sides agree on who is present.
    CHECK(host.participants().size() == 2);
    CHECK(client.participants().size() == 2);
}

TEST_CASE("CollabController: joining replaces the local scene rather than merging into it")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("HostOnly");
    const std::size_t hostCount = countEntities(hostWorld);

    // The joiner already has unrelated content open.
    HorizonWorld clientWorld;
    clientWorld.createEntity("ClientA");
    clientWorld.createEntity("ClientB");
    clientWorld.createEntity("ClientC");
    REQUIRE(countEntities(clientWorld) > hostCount);

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    CollabController client;
    client.setWorld(&clientWorld);
    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));

    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    // SceneSerializer::loadFromMemory merges by design, so the controller has to
    // clear first. Without that the joiner would silently end up with both
    // scenes stacked on top of each other.
    CHECK(countEntities(clientWorld) == hostCount);
}

TEST_CASE("CollabController: a wrong join code never establishes a session")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("Secret");

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    // A fresh world is not empty — it already carries its root entity — so the
    // meaningful assertion is that the count does not change, not that it is 0.
    const std::size_t before = countEntities(clientWorld);

    CollabController client;
    client.setWorld(&clientWorld);
    REQUIRE(client.joinSession("127.0.0.1", host.port(), "WRONG-CODE-ENTIRELY", "Intruder"));

    // Give it ample time to fail rather than merely be slow.
    pumpUntil(host, client, [] { return false; }, std::chrono::seconds(3));

    CHECK(client.status() != CollabController::Status::Joined);
    CHECK(countEntities(clientWorld) == before);   // the scene never transferred
    CHECK(host.participants().size() == 1);        // nobody was admitted
}

TEST_CASE("CollabController: an empty host scene still completes the join")
{
    HorizonWorld hostWorld;            // brand-new project, nothing in it
    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    clientWorld.createEntity("Leftover");
    CollabController client;
    client.setWorld(&clientWorld);
    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));

    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    // An empty snapshot is a legitimate state, and clearing is then the whole job.
    CHECK(countEntities(clientWorld) == countEntities(hostWorld));
}

TEST_CASE("CollabController: leaving tears the session down on both sides")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");
    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);
    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    client.leave();
    CHECK_FALSE(client.active());

    // The host notices the peer is gone and drops it from the roster.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint64_t now = 0;
    while (std::chrono::steady_clock::now() < deadline && host.participants().size() > 1) {
        host.update(now);
        now += 16;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(host.participants().size() == 1);
}

TEST_CASE("CollabController: hosting without a world refuses joins instead of sharing nothing")
{
    CollabController host;   // deliberately no world set
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    clientWorld.createEntity("Existing");
    const std::size_t before = countEntities(clientWorld);

    CollabController client;
    client.setWorld(&clientWorld);
    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));

    pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Failed;
    }, std::chrono::seconds(5));

    // Admitting the peer would leave it believing it shares a scene it never got.
    CHECK(client.status() != CollabController::Status::Joined);
    CHECK(countEntities(clientWorld) == before);   // local scene untouched
}

// ─── Component replication through the real serializer ───────────────────────
// The protocol tests use opaque blobs; these check the part a fake can never
// cover — that arbitrary component state survives serialize → wire → apply on a
// genuine HorizonWorld.

TEST_CASE("CollabController: recycled host handles still address the right entity")
{
    // The field bug this pins down: the host edits its scene BEFORE hosting —
    // deletions leave recycled, version-bearing entt handles — and the client's
    // fresh deserialize numbers the same entities differently. Handle-keyed
    // subjects then miss silently ("changes only partly sync"). Uuid-derived
    // subjects must keep addressing the right entity regardless of either
    // side's allocator history.
    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");
    const Entity doomed = hostWorld.createEntity("Doomed");
    hostWorld.destroyEntity(doomed);
    const Entity target = hostWorld.createEntity("Target");   // recycles the slot
    // entt encodes a version in the handle, so this is now a handle a fresh
    // deserialize can never produce for this entity.

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);

    // Wire the client the way the editor does.
    std::uint32_t gotTransformFor = 0;
    float gotX = 0.0f;
    client.onRemoteTransform([&](std::uint64_t handle, const float pos[3],
                                 const float[3], const float[3]) {
        gotTransformFor = static_cast<std::uint32_t>(handle);
        gotX = pos[0];
    });
    std::uint32_t gotComponentsFor = 0;
    client.onRemoteComponents([&](std::uint32_t handle, const std::vector<std::uint8_t>& blob) {
        gotComponentsFor = handle;
        SceneSerializer s;
        s.applyEntityComponents(clientWorld, static_cast<Entity>(handle), blob);
    });
    client.onWorldReplaced([&] { client.seedNetIds(); });

    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    // The host edits "Target": take the lock (selection), move it, rename it.
    const auto targetHandle = static_cast<std::uint32_t>(entt::to_integral(target));
    const std::uint64_t subject = host.subjectFor(targetHandle);
    host.followSelection(subject);
    REQUIRE(pumpUntil(host, client, [&] { return host.ownsLock(subject); }));

    const float p[3] = { 42.0f, 0.0f, 0.0f };
    const float r[3] = { 0.0f, 0.0f, 0.0f };
    const float sc[3] = { 1.0f, 1.0f, 1.0f };
    std::uint64_t now = 1;
    REQUIRE(pumpUntil(host, client, [&] {
        host.publishTransform(subject, p, r, sc, now); now += 200;
        return gotTransformFor != 0;
    }));
    CHECK(gotX == doctest::Approx(42.0f));

    // The client applied it to the entity that IS "Target" over there — found
    // by identity, not by trusting the host's handle bits.
    auto& creg = clientWorld.registry();
    const auto clientEntity = static_cast<Entity>(static_cast<entt::id_type>(gotTransformFor));
    REQUIRE(creg.valid(clientEntity));
    REQUIRE(creg.all_of<NameComponent>(clientEntity));
    CHECK(creg.get<NameComponent>(clientEntity).name == "Target");

    // Components too — the path that previously required a structural
    // announcement no pre-session entity ever got.
    hostWorld.registry().get<NameComponent>(target).name = "Renamed";
    SceneSerializer ser;
    const auto blob = ser.serializeEntityComponents(hostWorld, target);
    REQUIRE_FALSE(blob.empty());
    REQUIRE(pumpUntil(host, client, [&] {
        host.publishComponents(targetHandle, blob);
        return gotComponentsFor != 0;
    }));
    CHECK(creg.get<NameComponent>(clientEntity).name == "Renamed");
}

TEST_CASE("CollabController: a replicated create keeps its identity on every peer")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("Existing");

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);
    client.onWorldReplaced([&] { client.seedNetIds(); });
    client.onRemoteCreate([&](std::uint32_t parentHandle,
                              const std::vector<std::uint8_t>& blob) -> std::uint32_t {
        SceneSerializer s;
        const Entity parent = parentHandle
            ? static_cast<Entity>(static_cast<entt::id_type>(parentHandle)) : entt::null;
        const Entity created = s.instantiatePrefab(clientWorld, blob, parent,
                                                   /*preserveIds=*/true);
        return created == entt::null
            ? 0u : static_cast<std::uint32_t>(entt::to_integral(created));
    });

    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    // Host creates a fresh entity mid-session and announces it.
    const Entity fresh = hostWorld.createEntity("Fresh");
    SceneSerializer ser;
    const auto blob = ser.serializeSubtree(hostWorld, fresh);
    REQUIRE_FALSE(blob.empty());
    const auto freshHandle = static_cast<std::uint32_t>(entt::to_integral(fresh));

    const std::size_t before = countEntities(clientWorld);
    REQUIRE(pumpUntil(host, client, [&] {
        host.publishCreate(freshHandle, 0, blob);
        return countEntities(clientWorld) > before;
    }));

    // Same wire identity on both sides: the subject the HOST derives for the
    // entity resolves on the CLIENT — which only holds if instantiation kept
    // the uuid instead of minting a new one.
    const std::uint64_t subject = host.subjectFor(freshHandle);
    CHECK(client.entityForNetId(subject) != 0);
}

TEST_CASE("CollabController: asset locks are lazy, visible to peers, and releasable")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);
    client.onWorldReplaced([&] { client.seedNetIds(); });

    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    const std::string asset = "Content/Graphs/Door.hcode";

    // Nothing is locked by merely being open — locks come from edits.
    CHECK_FALSE(host.assetLockedByOther(asset));
    CHECK_FALSE(client.assetLockedByOther(asset));

    // The host starts editing: lock requested, granted, and REPLICATED — the
    // client's editor must know before its user tries to type into the same
    // graph, that is the entire point of the table.
    host.requestAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] { return host.ownsAssetLock(asset); }));
    REQUIRE(pumpUntil(host, client, [&] { return client.assetLockedByOther(asset); }));

    const HE::Net::LockInfo* info = client.assetLockInfo(asset);
    REQUIRE(info != nullptr);
    CHECK(info->ownerName == "Anna");

    // While the table already SHOWS the other holder, a request is not even
    // sent — the short-circuit is the synchronous common case.
    client.requestAssetLock(asset);
    CHECK_FALSE(client.ownsAssetLock(asset));

    // Tab closed on the host: released, and the client may edit.
    host.releaseAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] { return !client.assetLockedByOther(asset); }));
    client.requestAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] { return client.ownsAssetLock(asset); }));
    client.releaseAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] { return !host.assetLockedByOther(asset); }));
}

TEST_CASE("CollabController: losing the asset-lock race fires the deny handler")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld clientWorld;
    CollabController client;
    client.setWorld(&clientWorld);
    client.onWorldReplaced([&] { client.seedNetIds(); });

    REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(host, client, [&] {
        return client.status() == CollabController::Status::Joined;
    }));

    const std::string asset = "Content/Graphs/Door.hcode";

    bool denied = false;
    client.onAssetLockDenied([&](const std::string& p) { denied = p == asset; });

    // Both sides start editing within one round trip of each other — neither
    // lock table shows anything yet, so both requests go out. The host is the
    // authority and wins; the client's optimistic edits must be rolled back,
    // which is exactly what the deny handler is for.
    host.requestAssetLock(asset);
    client.requestAssetLock(asset);

    REQUIRE(pumpUntil(host, client, [&] { return denied; }));
    CHECK(host.ownsAssetLock(asset));
    CHECK_FALSE(client.ownsAssetLock(asset));
    // The denied path is no longer tracked as held-or-pending.
    CHECK(client.heldAssetLocks().empty());
}

TEST_CASE("projectRelativeAssetPath: normalises separators and rejects outsiders")
{
    CHECK(CollabController::projectRelativeAssetPath(
              "/proj/Content/Graphs/A.hcode", "/proj") == "Content/Graphs/A.hcode");
    CHECK(CollabController::projectRelativeAssetPath(
              "C:\\proj\\Content\\A.hmat", "C:/proj") == "Content/A.hmat");
    CHECK(CollabController::projectRelativeAssetPath(
              "/elsewhere/Content/A.hmat", "/proj").empty());
    CHECK(CollabController::projectRelativeAssetPath("/proj", "/proj").empty());
}

TEST_CASE("SceneSerializer: entity components round-trip onto an existing entity")
{
    HorizonWorld world;
    const Entity e = world.createEntity("Prop");
    auto& reg = world.registry();

    auto& tc = reg.emplace_or_replace<TransformComponent>(e);
    tc.position = glm::vec3(1.0f, 2.0f, 3.0f);
    tc.scale    = glm::vec3(4.0f, 5.0f, 6.0f);

    SceneSerializer serializer;
    const auto blob = serializer.serializeEntityComponents(world, e);
    REQUIRE_FALSE(blob.empty());

    // Change it, then apply the captured state back — the entity must survive,
    // not be replaced, because peers reference it by handle.
    tc.position = glm::vec3(99.0f, 99.0f, 99.0f);
    REQUIRE(serializer.applyEntityComponents(world, e, blob));

    CHECK(reg.valid(e));   // same entity, not a new one
    auto* after = reg.try_get<TransformComponent>(e);
    REQUIRE(after != nullptr);
    CHECK(after->position.x == doctest::Approx(1.0f));
    CHECK(after->scale.y == doctest::Approx(5.0f));
}

TEST_CASE("SceneSerializer: a non-transform component replicates too")
{
    HorizonWorld world;
    const Entity e = world.createEntity("Lamp");
    auto& reg = world.registry();

    // A light is exactly the kind of edit a transform delta cannot carry.
    auto& lc = reg.emplace_or_replace<LightComponent>(e);
    lc.intensity = 7.5f;

    SceneSerializer serializer;
    const auto blob = serializer.serializeEntityComponents(world, e);
    REQUIRE_FALSE(blob.empty());

    lc.intensity = 0.1f;
    REQUIRE(serializer.applyEntityComponents(world, e, blob));

    auto* after = reg.try_get<LightComponent>(e);
    REQUIRE(after != nullptr);
    CHECK(after->intensity == doctest::Approx(7.5f));
}

TEST_CASE("SceneSerializer: applying to an invalid entity fails instead of creating one")
{
    HorizonWorld world;
    const Entity e = world.createEntity("Temp");
    SceneSerializer serializer;
    const auto blob = serializer.serializeEntityComponents(world, e);
    world.destroyEntity(e);

    // A peer may reference an entity we already deleted; silently resurrecting
    // it would diverge the scenes.
    CHECK_FALSE(serializer.applyEntityComponents(world, e, blob));
    CHECK_FALSE(serializer.applyEntityComponents(world, e, {}));
}

// ─── Which assets a live session carries ─────────────────────────────────────

TEST_CASE("isSyncableAsset accepts the extensions the engine actually writes")
{
    // The scene entry read ".hscene" for a long time while ProjectManager writes
    // ".hescene", so it could never match. It was harmless only because scenes
    // reach peers through the snapshot/delta path rather than the asset path —
    // a latent trap for whoever routes scene saves through ContentManager. This
    // test pins the spelling against the file the engine really produces.
    CHECK(CollabController::isSyncableAsset("Content/StartupScene.hescene"));
    CHECK(CollabController::isSyncableAsset("Content/UI/Main.huiw"));
    CHECK(CollabController::isSyncableAsset("Content/Materials/Rock.hmat"));
    CHECK(CollabController::isSyncableAsset("Content/Scripts/Player.lua"));
    CHECK(CollabController::isSyncableAsset("Content/Scripts/Ai.py"));

    // Case-insensitive: the extension is lowercased before comparison, and
    // Windows users do produce mixed-case names.
    CHECK(CollabController::isSyncableAsset("Content/Scenes/Level.HEScene"));

    // The big binaries stay out — not because they matter less, but because they
    // are the largest files in a project and belong on the source-control path.
    CHECK_FALSE(CollabController::isSyncableAsset("Content/Models/Hero.fbx"));
    CHECK_FALSE(CollabController::isSyncableAsset("Content/Textures/Rock.png"));
    CHECK_FALSE(CollabController::isSyncableAsset("Content/Audio/Music.wav"));

    // Degenerate input must not be treated as a match.
    CHECK_FALSE(CollabController::isSyncableAsset("Content/NoExtension"));
    CHECK_FALSE(CollabController::isSyncableAsset(""));
}
