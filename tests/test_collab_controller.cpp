#include "doctest.h"

#include "../src/HE_Editor/CollabController.h"
#include "../src/HE_Editor/CollabDocSync.h"

#include <HorizonCode/HorizonCode.h>
#include <UIWidget/UIWidgetTree.h>

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonScene.h>

#include "TestFsUtil.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// ─── Editor ↔ network integration ────────────────────────────────────────────
// The unit tests in test_net_collab.cpp exercise the protocol against a fake
// state provider. These drive the REAL path the editor uses: two CollabControllers
// over actual TCP, with SceneSerializer moving genuine HorizonWorld content
// between them. That is the seam a fake can never cover — a scene that
// serializes but fails to reload, or a snapshot merged into the joiner's world
// instead of replacing it, would pass every protocol test and still be broken.

namespace fs = std::filesystem;

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

TEST_CASE("CollabController: opening an editor asks the host, and waits for the answer")
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

    using EditState = CollabController::AssetEditState;
    const std::string asset = "Content/Graphs/Door.hcode";

    // Nothing asked yet: the tab must NOT assume it may edit.
    CHECK(client.assetEditState(asset) == EditState::Unknown);
    CHECK_FALSE(client.beginAssetEdit(asset));

    // Opening the tab asks the host. Until it answers the tab stays read-only —
    // that window is precisely the race this replaces.
    client.beginAssetEditSession(asset);
    REQUIRE(pumpUntil(host, client, [&] {
        return client.assetEditState(asset) == EditState::Editable;
    }));

    // The first edit claims it, and now it is a confirmation rather than a race.
    CHECK(client.beginAssetEdit(asset));
    REQUIRE(pumpUntil(host, client, [&] { return client.ownsAssetLock(asset); }));
    CHECK(client.assetEditState(asset) == EditState::Owned);
}

TEST_CASE("CollabController: a tab opened on a locked asset is told so by the host")
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

    using EditState = CollabController::AssetEditState;
    const std::string asset = "Content/Graphs/Door.hcode";

    host.requestAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] { return host.ownsAssetLock(asset); }));

    client.beginAssetEditSession(asset);
    REQUIRE(pumpUntil(host, client, [&] {
        return client.assetEditState(asset) == EditState::HeldByOther;
    }));
    CHECK_FALSE(client.beginAssetEdit(asset));

    // The live table wins while it still shows a holder: it is the most recent
    // thing this peer knows, and it is what flips an already-open tab to
    // read-only the moment someone else claims the asset.
    host.releaseAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] { return !client.assetLockedByOther(asset); }));

    // Closing and reopening asks the host again rather than trusting an answer
    // from whenever the tab happened to be opened.
    client.forgetAssetEditSession(asset);
    CHECK(client.assetEditState(asset) == EditState::Unknown);
    client.beginAssetEditSession(asset);
    REQUIRE(pumpUntil(host, client, [&] {
        return client.assetEditState(asset) == EditState::Editable;
    }));
}

TEST_CASE("CollabController: the host answers its own open without a round trip")
{
    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    using EditState = CollabController::AssetEditState;
    const std::string asset = "Content/Graphs/Door.hcode";

    // No pump: the host IS the lock table, so a tab opening there must not sit
    // read-only waiting for a message it would be sending to itself.
    host.beginAssetEditSession(asset);
    CHECK(host.assetEditState(asset) == EditState::Editable);
    CHECK(host.beginAssetEdit(asset));
}

TEST_CASE("CollabController: document deltas need the lock and reach the peer")
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

    std::string gotPath;
    std::vector<HE::Net::CollabSession::DocDelta> gotBatch;
    client.onRemoteDocDeltas([&](const std::string& p,
                                 const std::vector<HE::Net::CollabSession::DocDelta>& b) {
        gotPath = p; gotBatch = b;
    });

    HE::Net::CollabSession::DocDelta d;
    d.kind = 0; d.op = 0; d.itemId = 5; d.json = R"({"id":5,"type":"Print"})";
    const std::vector<HE::Net::CollabSession::DocDelta> batch{ d };

    // Without the lock nothing goes out — an editor that skipped its own gate
    // must not be able to rewrite a document someone else owns.
    CHECK_FALSE(host.publishDocDeltas(asset, batch));

    host.requestAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] { return host.ownsAssetLock(asset); }));

    CHECK(host.publishDocDeltas(asset, batch));
    REQUIRE(pumpUntil(host, client, [&] { return !gotBatch.empty(); }));
    CHECK(gotPath == asset);
    REQUIRE(gotBatch.size() == 1);
    CHECK(gotBatch[0].itemId == 5);
}

// ─── The whole live-editing pipeline, end to end ─────────────────────────────
// Two editors over a real socket, driving REAL documents through every stage the
// running editor uses: open → host lock query → first edit claims → diff →
// publish → receive → apply. The unit tests cover each stage; this is the one
// that would catch them being wired together wrongly.

namespace {

// What EditorApplication does per frame for one open tab, without the ImGui.
struct FakeTab {
    std::string                path;
    CollabDocSync::DocMirror   mirror;
    CollabController*          peer = nullptr;

    void open() { peer->beginAssetEditSession(path); }

    // Diff whatever the adapter sees and publish it, exactly as
    // EditorApplication::publishDocDeltas does.
    bool publish(CollabDocSync::IDocAdapter& doc, CollabDocSync::Scope scope) {
        if (!peer->ownsAssetLock(path)) {
            CollabDocSync::seed(doc, mirror, scope);   // watching, not editing
            return false;
        }
        std::vector<HE::Net::CollabSession::DocDelta> batch;
        CollabDocSync::diffInto(doc, mirror, scope, batch);
        if (batch.empty()) return false;
        return peer->publishDocDeltas(path, batch);
    }
};

} // namespace

TEST_CASE("Live sync: a HorizonCode graph edit reaches the peer node by node")
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

    const std::string asset = "Content/Scripts/Door.hasset";

    // Both open the same graph, starting from the same content.
    HorizonCode::Graph hostGraph, clientGraph;
    FakeTab hostTab{ asset, {}, &host };
    FakeTab clientTab{ asset, {}, &client };
    hostTab.open();
    clientTab.open();

    // The receiving side applies into its own live document.
    client.onRemoteDocDeltas([&](const std::string& p,
                                 const std::vector<HE::Net::CollabSession::DocDelta>& b) {
        REQUIRE(p == asset);
        auto doc = CollabDocSync::forHorizonCodeGraph(clientGraph);
        CollabDocSync::applyDeltas(*doc, clientTab.mirror,
                                   CollabDocSync::Scope::Primary, b);
    });

    using EditState = CollabController::AssetEditState;
    REQUIRE(pumpUntil(host, client, [&] {
        return host.assetEditState(asset)   == EditState::Editable &&
               client.assetEditState(asset) == EditState::Editable;
    }));

    // Anna types first: her first edit claims the lock...
    CHECK(host.beginAssetEdit(asset));
    REQUIRE(pumpUntil(host, client, [&] { return host.ownsAssetLock(asset); }));

    // ...and Bob's open tab goes read-only, without either of them having lost
    // an edit to a race.
    REQUIRE(pumpUntil(host, client, [&] {
        return client.assetEditState(asset) == EditState::HeldByOther;
    }));
    CHECK_FALSE(client.beginAssetEdit(asset));

    {
        auto doc = CollabDocSync::forHorizonCodeGraph(hostGraph);
        CollabDocSync::seed(*doc, hostTab.mirror, CollabDocSync::Scope::Primary);
    }

    HorizonCode::Node ev; ev.type = HorizonCode::NodeType::Event; ev.s = "OnUse";
    const int evId = hostGraph.addNode(ev);
    HorizonCode::Node pr; pr.type = HorizonCode::NodeType::Print;
    const int prId = hostGraph.addNode(pr);
    hostGraph.links.push_back({ evId, 0, prId, 0 });

    {
        auto doc = CollabDocSync::forHorizonCodeGraph(hostGraph);
        CHECK(hostTab.publish(*doc, CollabDocSync::Scope::Primary));
    }
    REQUIRE(pumpUntil(host, client, [&] { return clientGraph.nodes.size() == 2; }));
    CHECK(HorizonCode::toJson(clientGraph) == HorizonCode::toJson(hostGraph));

    // Dragging one node sends one node, and the peer's graph tracks it.
    hostGraph.findNode(prId)->x = 320.0f;
    {
        auto doc = CollabDocSync::forHorizonCodeGraph(hostGraph);
        CHECK(hostTab.publish(*doc, CollabDocSync::Scope::Primary));
    }
    REQUIRE(pumpUntil(host, client, [&] {
        return clientGraph.findNode(prId) && clientGraph.findNode(prId)->x == 320.0f;
    }));
    CHECK(HorizonCode::toJson(clientGraph) == HorizonCode::toJson(hostGraph));

    // Deleting takes the link with it on both sides.
    hostGraph.removeNode(prId);
    {
        auto doc = CollabDocSync::forHorizonCodeGraph(hostGraph);
        CHECK(hostTab.publish(*doc, CollabDocSync::Scope::Primary));
    }
    REQUIRE(pumpUntil(host, client, [&] { return clientGraph.nodes.size() == 1; }));
    CHECK(clientGraph.links.empty());
    CHECK(HorizonCode::toJson(clientGraph) == HorizonCode::toJson(hostGraph));

    // Anna closes her tab; Bob may now edit the same asset.
    host.releaseAssetLock(asset);
    REQUIRE(pumpUntil(host, client, [&] {
        return client.assetEditState(asset) == EditState::Editable;
    }));
    CHECK(client.beginAssetEdit(asset));
}

TEST_CASE("Live sync: the level script and the GameInstance graph travel like any document")
{
    // Neither is a file under Content: the level script lives in the world (it
    // reached peers only in the JOIN SNAPSHOT before), and the GameInstance graph
    // sits at <project>/GameInstance.hcode, outside the content root, so the
    // content-relative key could never name it. Both now sync under their
    // reserved tab path — one string per session, because every peer is in the
    // same scene and the same project.
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

    // The two reserved keys, exactly as EditorApplication::collabSyncKey produces
    // them (the panels' kTabPath constants).
    const std::string kLevelScript  = "::LevelScript::";
    const std::string kGameInstance = "::GameInstance::";

    // Distinct subjects, or one lock would arbitrate both documents.
    CHECK(CollabController::assetSubject(kLevelScript) !=
          CollabController::assetSubject(kGameInstance));

    HorizonCode::Graph hostLevel, clientLevel, hostGI, clientGI;
    CollabDocSync::DocMirror hLevelM, cLevelM, hGiM, cGiM;
    for (auto* pair : { &hLevelM, &cLevelM }) (void)pair;
    CollabDocSync::seed(*CollabDocSync::forHorizonCodeGraph(hostLevel),   hLevelM,
                        CollabDocSync::Scope::Primary);
    CollabDocSync::seed(*CollabDocSync::forHorizonCodeGraph(clientLevel), cLevelM,
                        CollabDocSync::Scope::Primary);
    CollabDocSync::seed(*CollabDocSync::forHorizonCodeGraph(hostGI),      hGiM,
                        CollabDocSync::Scope::Primary);
    CollabDocSync::seed(*CollabDocSync::forHorizonCodeGraph(clientGI),    cGiM,
                        CollabDocSync::Scope::Primary);

    client.onRemoteDocDeltas([&](const std::string& key,
                                 const std::vector<HE::Net::CollabSession::DocDelta>& b) {
        // The key is what routes a batch to the right document — the two graphs
        // are otherwise indistinguishable.
        if (key == kLevelScript)
            CollabDocSync::applyDeltas(*CollabDocSync::forHorizonCodeGraph(clientLevel),
                                       cLevelM, CollabDocSync::Scope::Primary, b);
        else if (key == kGameInstance)
            CollabDocSync::applyDeltas(*CollabDocSync::forHorizonCodeGraph(clientGI),
                                       cGiM, CollabDocSync::Scope::Primary, b);
    });

    for (const std::string& key : { kLevelScript, kGameInstance })
    {
        host.beginAssetEditSession(key);
        REQUIRE(pumpUntil(host, client, [&] {
            return host.assetEditState(key) ==
                   CollabController::AssetEditState::Editable;
        }));
        REQUIRE(host.beginAssetEdit(key));
        REQUIRE(pumpUntil(host, client, [&] { return host.ownsAssetLock(key); }));
    }

    HorizonCode::Node onLoaded;
    onLoaded.type = HorizonCode::NodeType::Event;
    onLoaded.s    = "OnLevelLoaded";
    hostLevel.addNode(onLoaded);

    HorizonCode::Node onInit;
    onInit.type = HorizonCode::NodeType::Event;
    onInit.s    = "OnInit";
    hostGI.addNode(onInit);

    std::vector<HE::Net::CollabSession::DocDelta> lvl, gi;
    CollabDocSync::diffInto(*CollabDocSync::forHorizonCodeGraph(hostLevel), hLevelM,
                            CollabDocSync::Scope::Primary, lvl);
    CollabDocSync::diffInto(*CollabDocSync::forHorizonCodeGraph(hostGI), hGiM,
                            CollabDocSync::Scope::Primary, gi);
    REQUIRE(host.publishDocDeltas(kLevelScript,  lvl));
    REQUIRE(host.publishDocDeltas(kGameInstance, gi));

    REQUIRE(pumpUntil(host, client, [&] {
        return clientLevel.nodes.size() == 1 && clientGI.nodes.size() == 1;
    }));
    CHECK(HorizonCode::toJson(clientLevel) == HorizonCode::toJson(hostLevel));
    CHECK(HorizonCode::toJson(clientGI)    == HorizonCode::toJson(hostGI));
    // Each landed in its OWN document — the routing key is doing real work.
    CHECK(clientLevel.nodes[0].s == "OnLevelLoaded");
    CHECK(clientGI.nodes[0].s    == "OnInit");
}

TEST_CASE("Live sync: a C++ class reaches the peer as a whole file")
{
    // Raw .h/.cpp text has no item structure to diff, so it takes the whole-file
    // path — but under the reserved Source key, because it lives outside Content
    // and resolving it against the content root would drop the peer's file into
    // the wrong tree.
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

    const std::string key = "::Source::PlayerPawn.h";

    std::string gotPath;
    std::vector<std::uint8_t> gotBytes;
    client.onRemoteAsset([&](const std::string& p, const std::vector<std::uint8_t>& b) {
        gotPath = p; gotBytes = b;
    });

    // Write a real file for publishAsset to read. BINARY, deliberately: an
    // ofstream in text mode translates \n to \r\n on Windows, so the bytes on
    // disk would not be the bytes compared against below — and publishAsset
    // reads binary, exactly as it must to transfer a file verbatim. (This is
    // what broke the Windows CI run: the two strings printed identically and
    // differed by a carriage return.)
    const fs::path tmp = fs::temp_directory_path() / "he_collab_src.h";
    const std::string contents = "#pragma once\nclass PlayerPawn { int hp = 100; };\n";
    { std::ofstream f(tmp, std::ios::binary); f << contents; }

    host.requestAssetLock(key);
    REQUIRE(pumpUntil(host, client, [&] { return host.ownsAssetLock(key); }));

    host.publishAsset(key, tmp.string());
    REQUIRE(pumpUntil(host, client, [&] { return !gotBytes.empty(); }));

    CHECK(gotPath == key);
    CHECK(std::string(gotBytes.begin(), gotBytes.end()) == contents);

    he_test::removeQuiet(tmp);
}

TEST_CASE("Live sync: the designer and the logic graph of one widget travel separately")
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

    const std::string asset = "Content/UI/Menu.hasset";

    HE::UIWidgetTree     hostTree,  clientTree;
    HorizonCode::Graph   hostGraph, clientGraph;
    CollabDocSync::DocMirror hTreeM, hGraphM, cTreeM, cGraphM;

    host.beginAssetEditSession(asset);
    using EditState = CollabController::AssetEditState;
    REQUIRE(pumpUntil(host, client, [&] {
        return host.assetEditState(asset) == EditState::Editable;
    }));
    REQUIRE(host.beginAssetEdit(asset));
    REQUIRE(pumpUntil(host, client, [&] { return host.ownsAssetLock(asset); }));

    client.onRemoteDocDeltas([&](const std::string&,
                                 const std::vector<HE::Net::CollabSession::DocDelta>& b) {
        auto tree  = CollabDocSync::forUIWidgetTree(clientTree);
        auto graph = CollabDocSync::forHorizonCodeGraph(clientGraph);
        CollabDocSync::applyDeltas(*tree,  cTreeM,  CollabDocSync::Scope::Primary,    b);
        CollabDocSync::applyDeltas(*graph, cGraphM, CollabDocSync::Scope::LogicGraph, b);
    });

    {
        auto t = CollabDocSync::forUIWidgetTree(hostTree);
        auto g = CollabDocSync::forHorizonCodeGraph(hostGraph);
        CollabDocSync::seed(*t, hTreeM, CollabDocSync::Scope::Primary);
        CollabDocSync::seed(*g, hGraphM, CollabDocSync::Scope::LogicGraph);
        CollabDocSync::seed(*CollabDocSync::forUIWidgetTree(clientTree), cTreeM,
                            CollabDocSync::Scope::Primary);
        CollabDocSync::seed(*CollabDocSync::forHorizonCodeGraph(clientGraph), cGraphM,
                            CollabDocSync::Scope::LogicGraph);
    }

    // One edit in each document, published as ONE batch — the asset has a single
    // lock, so the two documents are always in step over the wire.
    const int btn = hostTree.add(HE::UIWidgetType::Button);
    hostTree.find(btn)->name = "Start";
    HorizonCode::Node ev; ev.type = HorizonCode::NodeType::Event; ev.s = "OnClick";
    ev.elem = btn;
    hostGraph.addNode(ev);

    std::vector<HE::Net::CollabSession::DocDelta> batch;
    {
        auto t = CollabDocSync::forUIWidgetTree(hostTree);
        auto g = CollabDocSync::forHorizonCodeGraph(hostGraph);
        CollabDocSync::diffInto(*t, hTreeM, CollabDocSync::Scope::Primary,    batch);
        CollabDocSync::diffInto(*g, hGraphM, CollabDocSync::Scope::LogicGraph, batch);
    }
    REQUIRE(batch.size() == 2);
    CHECK(host.publishDocDeltas(asset, batch));

    REQUIRE(pumpUntil(host, client, [&] {
        return clientTree.elements.size() == 1 && clientGraph.nodes.size() == 1;
    }));
    CHECK(HE::uiWidgetTreeToJson(clientTree) == HE::uiWidgetTreeToJson(hostTree));
    CHECK(HorizonCode::toJson(clientGraph)   == HorizonCode::toJson(hostGraph));
    // The element id and the node id are both small integers; the scopes are
    // what keep one from landing in the other's document.
    CHECK(clientTree.find(btn) != nullptr);
    CHECK(clientGraph.nodes[0].elem == btn);
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
    // THE ENGINE WRITES ONE EXTENSION. Every authored asset — material, UI
    // widget, HorizonCode graph, particle graph, animator state machine, input
    // asset — is a `.hasset` container with the type in its header; the
    // ContentManager's own registry scan keys on exactly that. This list used to
    // name .hmat/.huiw/.hcode/.hpart/.hasm/.hinput, none of which the engine
    // produces, so it matched NOTHING in a real project and asset collaboration
    // was dead for every type but scenes and loose scripts.
    //
    // The test that was here asserted on those invented extensions, so it passed
    // while the feature did not work. It now asserts against the files the engine
    // really writes.
    CHECK(CollabController::isSyncableAsset("Content/Materials/Rock.hasset"));
    CHECK(CollabController::isSyncableAsset("Content/UI/Main.hasset"));
    CHECK(CollabController::isSyncableAsset("Content/Scripts/Door.hasset"));
    CHECK(CollabController::isSyncableAsset("Content/StartupScene.hescene"));
    CHECK(CollabController::isSyncableAsset("Content/Scripts/Player.lua"));
    CHECK(CollabController::isSyncableAsset("Content/Scripts/Ai.py"));

    // Case-insensitive: the extension is lowercased before comparison, and
    // Windows users do produce mixed-case names.
    CHECK(CollabController::isSyncableAsset("Content/Scenes/Level.HEScene"));
    CHECK(CollabController::isSyncableAsset("Content/UI/Main.HAsset"));

    // The big binaries stay out — not because they matter less, but because they
    // are the largest files in a project and belong on the source-control path.
    CHECK_FALSE(CollabController::isSyncableAsset("Content/Models/Hero.fbx"));
    CHECK_FALSE(CollabController::isSyncableAsset("Content/Textures/Rock.png"));
}

TEST_CASE("isSyncableAsset admits the C++ tree under its reserved key prefix")
{
    // C++ classes are raw .h/.cpp under <project>/Source — no HAsset header to
    // sniff, and outside the content root, so they travel under a reserved key
    // (EditorApplication::collabSyncKey). The extension gate has to let those
    // through, and the TYPE gate must not be applied to them: it would read a
    // header-less text file as Unknown and drop it.
    CHECK(CollabController::isSyncableAsset("::Source::GameLogicRuntime.h"));
    CHECK(CollabController::isSyncableAsset("::Source::Player/PlayerPawn.cpp"));

    // A bare Source-looking path is NOT the same thing — the prefix is what
    // keeps the two key spaces from colliding with a Content/Source folder.
    CHECK_FALSE(CollabController::isSyncableAsset("Source/Player.h"));
}

TEST_CASE("isSyncableAssetType is what actually keeps the big binaries out")
{
    // Since every authored asset shares the `.hasset` extension, so does an
    // IMPORTED one — a 40 MB mesh and a material are the same container. The
    // extension filter cannot tell them apart, so this is the gate that matters,
    // and it is applied wherever the caller has the file to sniff.
    using T = HE::AssetType;
    CHECK(CollabController::isSyncableAssetType(T::Material));
    CHECK(CollabController::isSyncableAssetType(T::MaterialFunction));
    CHECK(CollabController::isSyncableAssetType(T::Widget));
    CHECK(CollabController::isSyncableAssetType(T::HorizonCodeClass));
    CHECK(CollabController::isSyncableAssetType(T::ParticleSystem));
    CHECK(CollabController::isSyncableAssetType(T::AnimatorStateMachine));
    CHECK(CollabController::isSyncableAssetType(T::InputAction));
    CHECK(CollabController::isSyncableAssetType(T::InputMappingContext));
    CHECK(CollabController::isSyncableAssetType(T::Script));

    CHECK_FALSE(CollabController::isSyncableAssetType(T::StaticMesh));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::SkeletalMesh));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Texture));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Audio));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Font));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Unknown));
    CHECK_FALSE(CollabController::isSyncableAsset("Content/Audio/Music.wav"));

    // Degenerate input must not be treated as a match.
    CHECK_FALSE(CollabController::isSyncableAsset("Content/NoExtension"));
    CHECK_FALSE(CollabController::isSyncableAsset(""));
}

// ─── Profile pictures ────────────────────────────────────────────────────────
// The crop-and-scale step, which is the only part of the picture path with real
// arithmetic in it. Everything around it (the file dialog, stb_image, the
// settings directory) is either the OS's or would mean writing into the
// developer's own editor configuration to observe.

namespace {

// A w×h RGBA image whose red channel is the column index and green channel the
// row, so a resampled result says exactly which source pixels it came from.
std::vector<std::uint8_t> gradientRgba(int w, int h)
{
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4u);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * w + x) * 4u;
            p[0] = static_cast<std::uint8_t>(x);
            p[1] = static_cast<std::uint8_t>(y);
            p[2] = 0;
            p[3] = 255;
        }
    }
    return px;
}

} // namespace

TEST_CASE("CollabController: a picture is cropped to a square from the centre")
{
    // 4 wide, 2 tall → the 2×2 centre, i.e. columns 1 and 2. Cropping from the
    // corner instead would take columns 0 and 1 and behead every portrait that
    // is not already square.
    const auto src = gradientRgba(4, 2);
    const auto out = CollabController::resampleSquareRgba(src.data(), 4, 2, 2);

    // Row stride is dst * 4 bytes, so the second output row starts at byte 8.
    REQUIRE(out.size() == 2u * 2u * 4u);
    CHECK(out[0] == 1);        // top-left came from column 1
    CHECK(out[4] == 2);        // top-right from column 2
    CHECK(out[1] == 0);        // …both of row 0
    CHECK(out[8 + 1] == 1);    // and the second output row is source row 1
}

TEST_CASE("CollabController: a flat colour survives any rescale")
{
    // Averaging must not tint or darken. A portrait that comes back a shade off
    // is the kind of thing nobody reports and everybody notices.
    std::vector<std::uint8_t> src(64 * 64 * 4);
    for (std::size_t i = 0; i < src.size(); i += 4)
    {
        src[i + 0] = 200; src[i + 1] = 100; src[i + 2] = 50; src[i + 3] = 255;
    }

    for (const int size : { 1, 7, 16, 64, 128 })
    {
        const auto out = CollabController::resampleSquareRgba(src.data(), 64, 64, size);
        REQUIRE(out.size() == static_cast<std::size_t>(size) * size * 4u);
        for (std::size_t i = 0; i < out.size(); i += 4)
        {
            CHECK(out[i + 0] == 200);
            CHECK(out[i + 1] == 100);
            CHECK(out[i + 2] == 50);
            CHECK(out[i + 3] == 255);
        }
    }
}

TEST_CASE("CollabController: upscaling a one-pixel image fills the whole square")
{
    // The degenerate case: every destination box collapses onto the single
    // source pixel. Without the box floor it would collapse to nothing and the
    // result would be transparent black.
    const std::uint8_t one[4] = { 10, 20, 30, 40 };
    const auto out = CollabController::resampleSquareRgba(one, 1, 1, 4);

    REQUIRE(out.size() == 4u * 4u * 4u);
    for (std::size_t i = 0; i < out.size(); i += 4)
    {
        CHECK(out[i + 0] == 10);
        CHECK(out[i + 1] == 20);
        CHECK(out[i + 2] == 30);
        CHECK(out[i + 3] == 40);
    }
}

TEST_CASE("CollabController: a tall picture keeps its width, not its height")
{
    // 2 wide, 6 tall → the square side is 2, taken from rows 2 and 3.
    const auto src = gradientRgba(2, 6);
    const auto out = CollabController::resampleSquareRgba(src.data(), 2, 6, 2);

    REQUIRE(out.size() == 2u * 2u * 4u);
    CHECK(out[1] == 2);        // first output row is source row 2
    CHECK(out[8 + 1] == 3);    // second is row 3
}

TEST_CASE("CollabController: degenerate picture input yields nothing, not a crash")
{
    const std::uint8_t one[4] = { 1, 2, 3, 4 };
    CHECK(CollabController::resampleSquareRgba(nullptr, 8, 8, 4).empty());
    CHECK(CollabController::resampleSquareRgba(one, 0, 8, 4).empty());
    CHECK(CollabController::resampleSquareRgba(one, 8, 0, 4).empty());
    CHECK(CollabController::resampleSquareRgba(one, 1, 1, 0).empty());
}
