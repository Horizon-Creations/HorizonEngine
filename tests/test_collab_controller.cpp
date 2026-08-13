#include "doctest.h"

#include "../src/HE_Editor/CollabController.h"
#include "../src/HE_Editor/CollabDocSync.h"
#include "../src/HE_Editor/EditorAssetTypeCache.h"
#include "../src/HE_Editor/NotificationStore.h"

#include <ContentManager/HAsset.h>

#include <HorizonCode/HorizonCode.h>
#include <Platform/PathSafety.h>
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

// ─── One user action, one bundle ─────────────────────────────────────────────
// A multi-select delete of twenty assets used to reach the host as twenty
// unrelated rows: the same decision, twenty clicks, and nothing on screen
// saying they came from one keystroke. These drive the real thing — a client
// opening a batch around its loop, a host answering the bundle — over TCP.

namespace {

// Three-way pump, for the tests that need two clients. The two-argument version
// above cannot be reused: every session in the exchange has to be given a turn,
// and a client nobody pumps never even completes its join.
template <typename Fn>
bool pumpUntil3(CollabController& host, CollabController& c1, CollabController& c2,
                Fn done, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint64_t now = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        host.update(now); c1.update(now); c2.update(now);
        if (done()) return true;
        now += 16;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    host.update(now); c1.update(now); c2.update(now);
    return done();
}

} // namespace

TEST_CASE("Collab: a multi-select delete reaches the host as ONE bundle")
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

    // What the content browser's multi-delete loop does, unchanged except for
    // the two calls around it.
    const std::vector<std::string> sel = {
        "Content/A.hasset", "Content/B.hasset", "Content/C.hasset"
    };
    const std::uint32_t batch = client.beginAssetOpBatch();
    CHECK(batch != 0);
    CHECK(client.openAssetOpBatch() == batch);
    for (const std::string& rel : sel) CHECK(client.requestAssetDelete(rel));
    client.endAssetOpBatch();
    CHECK(client.openAssetOpBatch() == 0);

    REQUIRE(pumpUntil(host, client, [&] {
        return host.pendingAssetOps().size() == 3;
    }));

    // Three rows, ONE decision: same batch, same owner, and the owner is the
    // participant that asked rather than whatever the host calls itself.
    const auto& ops = host.pendingAssetOps();
    const HE::Net::ParticipantId asker = ops[0].batchOwner;
    CHECK(asker != 0);
    CHECK(asker != host.localParticipant());
    for (const auto& op : ops) {
        CHECK(op.inBatch());
        CHECK(op.batchId == ops[0].batchId);
        CHECK(op.batchOwner == asker);
    }

    // A request made outside the batch is its own row and joins no bundle —
    // otherwise every later delete would be swallowed by the last selection.
    CHECK(client.requestAssetDelete("Content/Lonely.hasset"));
    REQUIRE(pumpUntil(host, client, [&] {
        return host.pendingAssetOps().size() == 4;
    }));
    CHECK_FALSE(host.pendingAssetOps()[3].inBatch());

    // ── One approved, one denied, the rest as a bundle ──
    std::vector<std::string> appliedOnClient;
    client.onRemoteAssetOp([&](HE::Net::CollabSession::AssetOp op,
                               const std::string& path, const std::string&, bool) {
        if (op == HE::Net::CollabSession::AssetOp::Delete)
            appliedOnClient.push_back(path);
    });

    host.approveAssetOp(0);                    // A: yes
    REQUIRE(pumpUntil(host, client, [&] { return appliedOnClient.size() == 1; }));
    CHECK(appliedOnClient[0] == "Content/A.hasset");

    host.denyAssetOp(0);                       // B: no — index 0 again, A has gone
    REQUIRE(pumpUntil(host, client, [&] {
        return host.pendingAssetOps().size() == 2;
    }));

    // What is left of the bundle is answered as the bundle: C goes, and the
    // stray row that was never part of it stays.
    CHECK(host.approveAssetOpBatch(asker, batch) == 1);
    REQUIRE(pumpUntil(host, client, [&] { return appliedOnClient.size() == 2; }));
    CHECK(appliedOnClient[1] == "Content/C.hasset");
    REQUIRE(host.pendingAssetOps().size() == 1);
    CHECK(host.pendingAssetOps()[0].path == "Content/Lonely.hasset");

    // Every verdict got home, the denial included — three of the four asks are
    // settled, and the one still waiting is the row nobody answered rather than
    // an answer that went missing.
    REQUIRE(pumpUntil(host, client, [&] {
        return client.pendingRequestsOfOurs() == 1;
    }));
    CHECK(host.pendingAssetOps().size() == 1);
}

TEST_CASE("Collab: a bundle survives the requester walking out mid-batch")
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

    client.beginAssetOpBatch();
    CHECK(client.requestAssetDelete("Content/A.hasset"));
    CHECK(client.requestAssetDelete("Content/B.hasset"));
    REQUIRE(pumpUntil(host, client, [&] {
        return host.pendingAssetOps().size() == 2;
    }));
    const HE::Net::ParticipantId asker = host.pendingAssetOps()[0].batchOwner;
    const std::uint32_t          batch = host.pendingAssetOps()[0].batchId;

    // They leave with the batch still open and both rows unanswered.
    client.leave();
    REQUIRE(pumpUntil(host, client, [&] { return host.participants().size() == 1; }));

    // The rows stay: the host was asked, and the decision is still the host's
    // to make — the panel already draws an absent asker as "Someone". Answering
    // them must reach the verdict for a connection that is gone without going
    // anywhere near it.
    REQUIRE(host.pendingAssetOps().size() == 2);
    CHECK(host.approveAssetOpBatch(asker, batch) == 2);
    CHECK(host.pendingAssetOps().empty());

    // And the batch is not answerable twice.
    CHECK(host.approveAssetOpBatch(asker, batch) == 0);

    host.leave();
}

TEST_CASE("Collab: two batches wanting the same asset stay ONE row")
{
    // The per-asset coalescing predates batches — "three people wanting the
    // same file gone is one decision" — and a bundle must not break it: the
    // file that both selections contain has to keep being one row with two
    // requesters, or approving one bundle would leave the other's requester
    // waiting for a verdict about a file that is already gone.
    HorizonWorld hostWorld;
    hostWorld.createEntity("Cube");

    CollabController host;
    host.setWorld(&hostWorld);
    REQUIRE(host.startHosting(0, "Anna"));

    HorizonWorld w1, w2;
    CollabController bob, cara;
    bob.setWorld(&w1);   bob.onWorldReplaced([&]  { bob.seedNetIds(); });
    cara.setWorld(&w2);  cara.onWorldReplaced([&] { cara.seedNetIds(); });

    REQUIRE(bob.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));
    REQUIRE(cara.joinSession("127.0.0.1", host.port(), host.joinCode(), "Cara"));
    REQUIRE(pumpUntil3(host, bob, cara, [&] {
        return bob.status()  == CollabController::Status::Joined &&
               cara.status() == CollabController::Status::Joined;
    }));

    // Bob selects A and Shared; Cara selects Shared and C.
    bob.beginAssetOpBatch();
    CHECK(bob.requestAssetDelete("Content/A.hasset"));
    CHECK(bob.requestAssetDelete("Content/Shared.hasset"));
    bob.endAssetOpBatch();
    REQUIRE(pumpUntil3(host, bob, cara, [&] {
        return host.pendingAssetOps().size() == 2;
    }));

    cara.beginAssetOpBatch();
    CHECK(cara.requestAssetDelete("Content/Shared.hasset"));
    CHECK(cara.requestAssetDelete("Content/C.hasset"));
    cara.endAssetOpBatch();
    REQUIRE(pumpUntil3(host, bob, cara, [&] {
        return host.pendingAssetOps().size() == 3;
    }));

    // Three rows, not four: Shared folded, and it belongs to the batch that
    // named it first — the second batch does not steal a row it shares.
    const auto& ops = host.pendingAssetOps();
    const HE::Net::ParticipantId bobId = ops[0].batchOwner;
    std::size_t sharedRows = 0;
    for (const auto& op : ops) {
        if (op.path != "Content/Shared.hasset") continue;
        ++sharedRows;
        CHECK(op.requesters.size() == 2);          // both, one decision
        CHECK(op.batchOwner == bobId);
    }
    CHECK(sharedRows == 1);

    // Approving Bob's bundle settles A and Shared and leaves Cara's C — and
    // Cara hears yes about Shared, because her request was answered by the same
    // decision rather than left hanging.
    std::vector<std::string> caraApplied;
    cara.onRemoteAssetOp([&](HE::Net::CollabSession::AssetOp,
                             const std::string& path, const std::string&, bool) {
        caraApplied.push_back(path);
    });
    CHECK(host.approveAssetOpBatch(bobId, ops[0].batchId) == 2);
    REQUIRE(pumpUntil3(host, bob, cara, [&] { return caraApplied.size() == 2; }));
    // Cara asked for two files and one of them was in Bob's bundle: that one is
    // answered, and only her own unbundled C is still waiting.
    REQUIRE(pumpUntil3(host, bob, cara, [&] {
        return cara.pendingRequestsOfOurs() == 1;
    }));

    REQUIRE(host.pendingAssetOps().size() == 1);
    CHECK(host.pendingAssetOps()[0].path == "Content/C.hasset");

    bob.leave();
    cara.leave();
    host.leave();
}

// ─── Reimport, and the things that used to happen in silence ─────────────────

namespace {

// A minimal but REAL .hasset of a given kind, because the syncable/not decision
// is made by sniffing the file header — a made-up file would answer Unknown and
// the test would pass for the wrong reason.
std::filesystem::path writeHAsset(const std::string& filename, HE::AssetType type,
                                  const std::string& body) {
    std::vector<std::uint8_t> meta(body.begin(), body.end());
    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    const std::vector<std::uint8_t> bytes = w.toBytes(static_cast<std::uint16_t>(type));
    const std::filesystem::path p = fs::temp_directory_path() / filename;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    EditorAssetTypeCache::invalidate(p.string());   // the sniff is cached per path
    return p;
}

// A host and a joined client, for the tests below that all need one.
struct Session {
    HorizonWorld     hostWorld, clientWorld;
    CollabController host, client;
};

bool openSession(Session& s) {
    s.hostWorld.createEntity("Cube");
    s.host.setWorld(&s.hostWorld);
    if (!s.host.startHosting(0, "Anna")) return false;
    s.client.setWorld(&s.clientWorld);
    s.client.onWorldReplaced([&s] { s.client.seedNetIds(); });
    if (!s.client.joinSession("127.0.0.1", s.host.port(), s.host.joinCode(), "Bob"))
        return false;
    return pumpUntil(s.host, s.client, [&s] {
        return s.client.status() == CollabController::Status::Joined;
    });
}

// The one notification whose text contains `needle`, or an empty one.
HE::Ed::Notification findNote(const HE::Ed::NotificationStore& store,
                              const std::string& needle) {
    for (const HE::Ed::Notification& n : store.snapshot()) {
        if (n.text.find(needle) != std::string::npos) return n;
    }
    return {};
}

} // namespace

TEST_CASE("Reimport: refused while somebody else is holding the asset")
{
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore notes;
    s.client.setNotifications(&notes);

    const std::string asset = "Content/Meshes/Rock.hasset";

    // Free: a background write may go ahead, and asking does not post anything —
    // a menu item asks this every frame to decide whether to grey itself out.
    CHECK(s.client.assetWritableNow(asset));
    CHECK(s.client.beginBackgroundWrite(asset));
    CHECK(notes.snapshot().empty());

    // Anna starts editing it. The lock reaches Bob's replicated table, which is
    // what makes this answerable without a round trip.
    s.host.requestAssetLock(asset);
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.host.ownsAssetLock(asset); }));
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.client.assetLockedByOther(asset); }));

    CHECK_FALSE(s.client.assetWritableNow(asset));
    // The gate refuses BEFORE the local write. Rewriting it here would fork the
    // file with nothing to tell anyone — the bytes of a mesh never travel.
    CHECK_FALSE(s.client.beginBackgroundWrite(asset));

    const HE::Ed::Notification n = findNote(notes, "Rock.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Problem);
    // Naming the holder is what makes it actionable: the fix is to go and ask
    // Anna, not to click the menu item again.
    CHECK(n.text.find("Anna") != std::string::npos);

    s.client.leave();
    s.host.leave();
}

TEST_CASE("Reimport: a mesh does not travel — everyone is told to pull it instead")
{
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore clientNotes;
    s.client.setNotifications(&clientNotes);

    bool bytesArrived = false;
    s.client.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) {
        bytesArrived = true;
    });

    const std::string key  = "Content/Meshes/Rock.hasset";
    const fs::path    file = writeHAsset("he_collab_rock.hasset",
                                         HE::AssetType::StaticMesh, "rock-v2");
    s.host.setLocalPathResolver([&](const std::string& k) {
        return k == key ? file.string() : std::string{};
    });

    s.host.publishReimport(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return !clientNotes.snapshot().empty();
    }));

    const HE::Ed::Notification n = findNote(clientNotes, "Rock.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Info);
    CHECK(n.text.find("Anna") != std::string::npos);
    CHECK(n.text.find("source control") != std::string::npos);
    // The whole point: forty megabytes of mesh did NOT go down a link built for
    // graphs just because somebody re-read it from disk.
    CHECK_FALSE(bytesArrived);

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Reimport: an authored asset re-transmits like any other save")
{
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore clientNotes;
    s.client.setNotifications(&clientNotes);

    std::vector<std::uint8_t> got;
    std::string               gotPath;
    s.client.onRemoteAsset([&](const std::string& p, const std::vector<std::uint8_t>& b) {
        gotPath = p; got = b;
    });

    const std::string key  = "Content/Materials/Brick.hasset";
    const fs::path    file = writeHAsset("he_collab_brick.hasset",
                                         HE::AssetType::Material, "brick-v2");

    s.host.requestAssetLock(key);
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.host.ownsAssetLock(key); }));

    s.host.publishReimport(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !got.empty(); }));

    CHECK(gotPath == key);
    // A kind that travels sends its BYTES — telling a peer to go and pull a
    // material it could simply have been handed would be the wrong half.
    CHECK(std::string(got.begin(), got.end()).find("brick-v2") != std::string::npos);
    CHECK(clientNotes.snapshot().empty());

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Reimport: a client holding no lock still tells everyone something")
{
    // The half that used to vanish. publishAsset claims a free lock and then
    // returns empty-handed on a client, because the grant is a round trip away
    // and it has nothing to wait on — harmless for a save, which the next save
    // repeats, and silence for a reimport, which happens once. Either the bytes
    // arrive or the notice does; never neither.
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore hostNotes;
    s.host.setNotifications(&hostNotes);

    bool bytesArrived = false;
    s.host.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) {
        bytesArrived = true;
    });

    const std::string key  = "Content/Materials/Tile.hasset";
    const fs::path    file = writeHAsset("he_collab_tile.hasset",
                                         HE::AssetType::Material, "tile-v2");

    // Bob reimports something he never opened, so he holds no lock — the
    // ordinary case for a context-menu action in the content browser.
    REQUIRE_FALSE(s.client.ownsAssetLock(key));
    s.client.publishReimport(key, file.string());

    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return bytesArrived || !hostNotes.snapshot().empty();
    }));
    if (!bytesArrived) {
        const HE::Ed::Notification n = findNote(hostNotes, "Tile.hasset");
        CHECK(n.level == HE::Ed::NoteLevel::Info);
        CHECK(n.text.find("Bob") != std::string::npos);
    }

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Collab: a denied request and an unanswered one both reach the user")
{
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore notes;
    s.client.setNotifications(&notes);

    CHECK(s.client.requestAssetDelete("Content/Doomed.hasset"));
    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return s.host.pendingAssetOps().size() == 1;
    }));
    s.host.denyAssetOp(0);
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !notes.snapshot().empty(); }));

    const HE::Ed::Notification denied = findNote(notes, "Doomed.hasset");
    CHECK(denied.level == HE::Ed::NoteLevel::Warning);
    CHECK(denied.text.find("did not approve") != std::string::npos);

    // And the other silence: an ask that was never answered at all. The session
    // ending is the moment that stops being a wait and becomes a refusal.
    CHECK(s.client.requestAssetDelete("Content/Forgotten.hasset"));
    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return s.host.pendingAssetOps().size() == 1;
    }));

    HE::Ed::NotificationStore hostNotes;
    s.host.setNotifications(&hostNotes);

    s.client.leave();
    const HE::Ed::Notification stranded = findNote(notes, "Forgotten.hasset");
    CHECK(stranded.level == HE::Ed::NoteLevel::Warning);
    CHECK(stranded.text.find("Nobody answered") != std::string::npos);

    // The host had a decision in front of it that it never made, and its queue
    // has just emptied itself — said once, with a count.
    s.host.leave();
    const HE::Ed::Notification unmade = findNote(hostNotes, "still waiting");
    CHECK(unmade.level == HE::Ed::NoteLevel::Warning);
}

TEST_CASE("Collab: a delete that did not happen here stops being silent")
{
    // The apply handler takes a std::error_code from every filesystem call it
    // makes and looks at none of them, so a delete that removed nothing and one
    // that worked were the same event: silence, and two peers that disagree
    // about what exists until source control contradicts them.
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore notes;
    s.client.setNotifications(&notes);

    const std::string key  = "Content/Stubborn.hasset";
    const fs::path    file = writeHAsset("he_collab_stubborn.hasset",
                                         HE::AssetType::Material, "still-here");
    s.client.setLocalPathResolver([&](const std::string& k) {
        return k == key ? file.string() : std::string{};
    });
    // An apply handler that does nothing at all — which is what a delete that
    // hits nothing amounts to, and what the editor's own handler does when it
    // refuses the operation and returns early without ever setting an ec.
    s.client.onRemoteAssetOp([](HE::Net::CollabSession::AssetOp, const std::string&,
                                const std::string&, bool) {});

    CHECK(s.host.requestAssetDelete(key));   // host: applied straight away
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !notes.snapshot().empty(); }));

    const HE::Ed::Notification n = findNote(notes, "Stubborn.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Problem);
    CHECK(n.text.find("still here") != std::string::npos);
    // The row points at something the user can actually open.
    CHECK(n.assetPath == file.string());

    // And the ordinary case stays quiet: the file gone means the peers agree,
    // which is the whole condition being tested.
    notes.clear();
    he_test::removeQuiet(file);
    CHECK(s.host.requestAssetDelete(key));
    REQUIRE(pumpUntil(s.host, s.client, [&] { return false; },
                      std::chrono::milliseconds(400)) == false);
    CHECK(notes.snapshot().empty());

    // ── A rename that only changed the case of the name ──
    // On APFS and on Windows the OLD path still resolves after such a rename —
    // to the very file that was just moved. A bare existence test calls the one
    // rename that certainly worked a divergence, on every peer at once, which
    // is worse than the silence it replaced: a channel that cries wolf is one
    // people switch off.
    const fs::path lower = fs::temp_directory_path() / "he_collab_case.hasset";
    const fs::path upper = fs::temp_directory_path() / "he_collab_CASE.hasset";
    he_test::removeQuiet(lower);
    he_test::removeQuiet(upper);
    const fs::path made = writeHAsset("he_collab_case.hasset",
                                      HE::AssetType::Material, "same-file");
    const std::string fromKey = "Content/case.hasset";
    const std::string toKey   = "Content/CASE.hasset";
    s.client.setLocalPathResolver([&](const std::string& k) {
        if (k == fromKey) return lower.string();
        if (k == toKey)   return upper.string();
        return std::string{};
    });
    s.client.onRemoteAssetOp([&](HE::Net::CollabSession::AssetOp,
                                 const std::string&, const std::string&, bool) {
        // What the editor's handler does, minus everything that is not the move.
        std::error_code rc;
        fs::rename(lower, upper, rc);
    });
    notes.clear();
    CHECK(s.host.requestAssetRename(fromKey, toKey));
    REQUIRE(pumpUntil(s.host, s.client, [&] { return false; },
                      std::chrono::milliseconds(400)) == false);
    CHECK(notes.snapshot().empty());

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(made);
    he_test::removeQuiet(upper);
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

    // The three project-type assets. They were added to AssetType after this
    // switch was first written, fell through to false, and therefore never
    // replicated — not even on an ordinary save. The compiler had been saying so
    // all along (-Wswitch on the editor's copy); the answer now lives beside the
    // enum with no default label, so the next addition cannot repeat it.
    CHECK(CollabController::isSyncableAssetType(T::StructType));
    CHECK(CollabController::isSyncableAssetType(T::EnumType));
    CHECK(CollabController::isSyncableAssetType(T::SaveGameTemplate));

    CHECK_FALSE(CollabController::isSyncableAssetType(T::StaticMesh));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::SkeletalMesh));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Texture));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Audio));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Font));
    CHECK_FALSE(CollabController::isSyncableAssetType(T::Unknown));
    CHECK_FALSE(CollabController::isSyncableAsset("Content/Audio/Music.wav"));

    // The editor's entry point and the shared one are the same answer, which is
    // the whole reason the shared one exists.
    CHECK(CollabController::isSyncableAssetType(T::Material) ==
          HE::isCollabSyncableAssetType(T::Material));
    CHECK(CollabController::isSyncableAssetType(T::StaticMesh) ==
          HE::isCollabSyncableAssetType(T::StaticMesh));

    // Degenerate input must not be treated as a match.
    CHECK_FALSE(CollabController::isSyncableAsset("Content/NoExtension"));
    CHECK_FALSE(CollabController::isSyncableAsset(""));
}

TEST_CASE("A path off the wire cannot point outside the tree it claims to be in")
{
    namespace fs = std::filesystem;

    // The traversal this exists to stop. Every one of these concatenates onto a
    // root perfectly well and lands somewhere it must not.
    CHECK_FALSE(HE::isRelativePathContained("../secrets.txt"));
    CHECK_FALSE(HE::isRelativePathContained("Materials/../../../.ssh/authorized_keys"));
    CHECK_FALSE(HE::isRelativePathContained("a/b/../../.."));
#ifdef _WIN32
    CHECK_FALSE(HE::isRelativePathContained("C:/Windows/System32/drivers/etc/hosts"));
    CHECK_FALSE(HE::isRelativePathContained("C:foo"));       // relative, but to another drive
#else
    CHECK_FALSE(HE::isRelativePathContained("/etc/passwd"));
#endif
    CHECK_FALSE(HE::isRelativePathContained(""));

    // Ordinary paths, including ".." that stays inside — refusing those would
    // break legitimate names for no gain.
    CHECK(HE::isRelativePathContained("Materials/Stone.hasset"));
    CHECK(HE::isRelativePathContained("a/b/../c.hasset"));
    CHECK(HE::isRelativePathContained("./Widgets/HUD.hasset"));

    // Component-wise, not string-prefix: a sibling directory whose name merely
    // begins with the root's is NOT inside it. A prefix comparison passes this
    // and is the classic way to get the check wrong.
    CHECK(HE::isPathWithin("/proj/Content", "/proj/Content/Materials/Stone.hasset"));
    CHECK(HE::isPathWithin("/proj/Content", "/proj/Content"));
    CHECK_FALSE(HE::isPathWithin("/proj/Content", "/proj/Content-backup/Stone.hasset"));
    CHECK_FALSE(HE::isPathWithin("/proj/Content", "/proj"));
    CHECK_FALSE(HE::isPathWithin("/proj/Content", "/proj/Other/Stone.hasset"));

    // Normalised before comparing, so a path that wanders back in is accepted
    // and one that wanders out is not — whatever it looks like textually.
    CHECK(HE::isPathWithin("/proj/Content", "/proj/Content/x/../y.hasset"));
    CHECK_FALSE(HE::isPathWithin("/proj/Content", "/proj/Content/../../etc/passwd"));
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

// ─── Viewport markers ────────────────────────────────────────────────────────
// Where a participant's name tag lands on screen. The drawing is ImGui's and
// cannot be asserted here; the projection can, and it is the part that goes
// silently wrong — an inverted Y puts every tag on the wrong side of the image,
// and a mishandled behind-the-camera case puts somebody standing behind you in
// front of you, which is worse than not drawing them at all.

#include "../src/HE_Editor/CollabPresenceBar.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {

// A camera at the origin looking down -Z, glm's convention throughout the
// engine, with a 640×480 image at screen origin (100, 50).
struct Cam
{
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                                 glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 640.0f / 480.0f, 0.1f, 1000.0f);
    float minX = 100.0f, minY = 50.0f, maxX = 740.0f, maxY = 530.0f;
    float centreX() const { return (minX + maxX) * 0.5f; }
    float centreY() const { return (minY + maxY) * 0.5f; }

    CollabPresenceBar::MarkerPlacement place(const glm::vec3& p, float inset = 22.0f) const
    {
        return CollabPresenceBar::PlaceMarker(view, proj, p, minX, minY, maxX, maxY, inset);
    }
};

} // namespace

TEST_CASE("CollabPresenceBar: a point straight ahead lands in the middle")
{
    const Cam cam;
    const auto m = cam.place({ 0.0f, 0.0f, -10.0f });

    CHECK(m.onScreen);
    CHECK(m.x == doctest::Approx(cam.centreX()).epsilon(0.001));
    CHECK(m.y == doctest::Approx(cam.centreY()).epsilon(0.001));
}

TEST_CASE("CollabPresenceBar: up is up and right is right")
{
    // The Y flip in one assertion: a participant floating ABOVE the camera must
    // draw in the upper half of the image, where screen y is SMALLER.
    const Cam cam;

    const auto above = cam.place({ 0.0f, 2.0f, -10.0f });
    REQUIRE(above.onScreen);
    CHECK(above.y < cam.centreY());
    CHECK(above.x == doctest::Approx(cam.centreX()).epsilon(0.001));

    const auto below = cam.place({ 0.0f, -2.0f, -10.0f });
    REQUIRE(below.onScreen);
    CHECK(below.y > cam.centreY());

    const auto right = cam.place({ 2.0f, 0.0f, -10.0f });
    REQUIRE(right.onScreen);
    CHECK(right.x > cam.centreX());

    const auto left = cam.place({ -2.0f, 0.0f, -10.0f });
    REQUIRE(left.onScreen);
    CHECK(left.x < cam.centreX());
}

TEST_CASE("CollabPresenceBar: someone behind you is pinned, never drawn in front")
{
    // The failure this rules out: the projection mirrors a point behind the eye
    // through the origin, so somebody standing behind and to the LEFT would be
    // drawn in front and to the right — confidently, and wrongly.
    const Cam cam;
    const auto m = cam.place({ -3.0f, 0.0f, 10.0f });   // behind, to the left

    CHECK_FALSE(m.onScreen);
    // Pinned to the left edge…
    CHECK(m.x < cam.centreX());
    // …with the arrow pointing left (π radians, allowing for the wrap).
    CHECK(std::abs(std::cos(m.arrowAngle) + 1.0f) < 0.001f);
}

TEST_CASE("CollabPresenceBar: a pinned marker stays inside the image")
{
    const Cam cam;
    constexpr float inset = 22.0f;

    const glm::vec3 offscreen[] = {
        {  0.0f,  0.0f,  10.0f },   // directly behind
        { 60.0f,  0.0f, -10.0f },   // far right, in front
        {-60.0f,  0.0f, -10.0f },
        {  0.0f, 60.0f, -10.0f },
        {  0.0f,-60.0f, -10.0f },
        { 40.0f, 40.0f,  10.0f },   // behind and off to a corner
    };
    for (const glm::vec3& p : offscreen)
    {
        const auto m = cam.place(p, inset);
        CHECK_FALSE(m.onScreen);
        CHECK(m.x >= cam.minX + inset - 0.5f);
        CHECK(m.x <= cam.maxX - inset + 0.5f);
        CHECK(m.y >= cam.minY + inset - 0.5f);
        CHECK(m.y <= cam.maxY - inset + 0.5f);
    }
}

TEST_CASE("CollabPresenceBar: the arrow points the way you have to turn")
{
    const Cam cam;

    // In front but far off to the right: pinned right, arrow right.
    const auto right = cam.place({ 60.0f, 0.0f, -10.0f });
    REQUIRE_FALSE(right.onScreen);
    CHECK(std::cos(right.arrowAngle) > 0.9f);

    // Above: pinned up, arrow up — which in screen space is NEGATIVE y, so the
    // sine is negative. Getting this backwards sends people the wrong way.
    const auto up = cam.place({ 0.0f, 60.0f, -10.0f });
    REQUIRE_FALSE(up.onScreen);
    CHECK(std::sin(up.arrowAngle) < -0.9f);
}

TEST_CASE("CollabPresenceBar: directly behind and dead centre picks a direction")
{
    // Degenerate: no direction is more correct than any other. What matters is
    // that it does not produce a NaN and does not land outside the image.
    const Cam cam;
    const auto m = cam.place({ 0.0f, 0.0f, 10.0f });

    CHECK_FALSE(m.onScreen);
    CHECK(std::isfinite(m.x));
    CHECK(std::isfinite(m.y));
    CHECK(m.x >= cam.minX);
    CHECK(m.x <= cam.maxX);
    CHECK(m.y >= cam.minY);
    CHECK(m.y <= cam.maxY);
}

TEST_CASE("CollabPresenceBar: a degenerate viewport rectangle draws nothing")
{
    Cam cam;
    cam.maxX = cam.minX;
    const auto m = cam.place({ 0.0f, 0.0f, -10.0f });
    CHECK_FALSE(m.onScreen);
}

// ─── Larger assets over the session ──────────────────────────────────────────
// A session normally leaves meshes, textures and audio to source control. A
// HOST may decide otherwise, and then everything below changes at once: who is
// allowed in, what a save publishes, and what a reimport does. These drive the
// real two-controller path, because the interesting part is precisely that the
// two sides end up agreeing.

namespace {

// A host with the flag as given, and a client that has NOT joined yet — the
// tests below care about the join itself, so they start it themselves.
bool openHostOnly(Session& s, bool hostSyncsLarge) {
    s.hostWorld.createEntity("Cube");
    s.host.setWorld(&s.hostWorld);
    s.host.setSyncLargeAssets(hostSyncsLarge);
    s.client.setWorld(&s.clientWorld);
    s.client.onWorldReplaced([&s] { s.client.seedNetIds(); });
    return s.host.startHosting(0, "Anna");
}

} // namespace

TEST_CASE("Larger assets: a host that carries them turns away an editor that has not agreed")
{
    Session s;
    REQUIRE(openHostOnly(s, /*hostSyncsLarge=*/true));
    REQUIRE_FALSE(s.client.syncLargeAssetsSetting());   // the default, and the point

    REQUIRE(s.client.joinSession("127.0.0.1", s.host.port(), s.host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.client.largeAssetsPrompt(); }));

    // Refused, but as a QUESTION — the flag is what the panel turns into a
    // dialog. Reported as a plain failure it would be a dead end for someone who
    // would have agreed in a second.
    CHECK(s.client.status() == CollabController::Status::Failed);
    CHECK_FALSE(s.client.inSession());
    // The cost is what the user is deciding on, so it has to be in the sentence.
    CHECK(s.client.lastError().find("considerably more data") != std::string::npos);
    // And the host really did not admit them.
    CHECK(s.host.participants().size() == 1);

    s.client.leave();
    s.host.leave();
}

TEST_CASE("Larger assets: agreeing re-dials the same host and gets in")
{
    Session s;
    REQUIRE(openHostOnly(s, /*hostSyncsLarge=*/true));

    REQUIRE(s.client.joinSession("127.0.0.1", s.host.port(), s.host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.client.largeAssetsPrompt(); }));

    // What the panel's "Enable and join" does. The address, port, code and name
    // all come from what the controller remembered, which is the whole reason it
    // remembers them: the user answered a dialog, not a connection form.
    REQUIRE(s.client.retryJoinWithLargeAssets());
    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return s.client.status() == CollabController::Status::Joined;
    }));

    CHECK_FALSE(s.client.largeAssetsPrompt());   // answered, so no longer asked
    CHECK(s.client.syncLargeAssetsSetting());
    // Both sides agree on what the session is. That agreement is what every
    // publishing decision on either machine is about to be made against.
    CHECK(s.host.sessionSyncsLargeAssets());
    CHECK(s.client.sessionSyncsLargeAssets());

    s.client.leave();
    s.host.leave();
}

TEST_CASE("Larger assets: a guest willing to send them still follows a host that will not")
{
    // The reverse is not an error and must not be treated as one: the host
    // decides what the session carries, so the guest simply joins and holds its
    // meshes back. Refusing here would deny a join over a difference that costs
    // nobody anything.
    Session s;
    REQUIRE(openHostOnly(s, /*hostSyncsLarge=*/false));
    s.client.setSyncLargeAssets(true);

    REQUIRE(s.client.joinSession("127.0.0.1", s.host.port(), s.host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return s.client.status() == CollabController::Status::Joined;
    }));
    CHECK_FALSE(s.client.largeAssetsPrompt());

    // It kept its own SETTING — that is a persisted preference and no session
    // gets to rewrite it — but it obeys the SESSION.
    CHECK(s.client.syncLargeAssetsSetting());
    CHECK_FALSE(s.client.sessionSyncsLargeAssets());
    CHECK_FALSE(s.client.assetTypeTravels(HE::AssetType::StaticMesh));
    CHECK(s.client.assetTypeTravels(HE::AssetType::Material));

    // And it holds back in practice, not just in the predicate. Bob reimports a
    // mesh he holds the lock on: with the setting alone deciding, the bytes
    // would go out; with the session deciding, the fact does instead.
    HE::Ed::NotificationStore hostNotes;
    s.host.setNotifications(&hostNotes);

    bool bytesArrived = false;
    s.host.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) {
        bytesArrived = true;
    });

    const std::string key  = "Content/Meshes/Crate.hasset";
    const fs::path    file = writeHAsset("he_collab_crate.hasset",
                                         HE::AssetType::StaticMesh, "crate-v2");
    s.client.requestAssetLock(key);
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.client.ownsAssetLock(key); }));

    s.client.publishReimport(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !hostNotes.snapshot().empty(); }));

    CHECK_FALSE(bytesArrived);
    const HE::Ed::Notification n = findNote(hostNotes, "Crate.hasset");
    CHECK(n.text.find("source control") != std::string::npos);

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Larger assets: with the session carrying them, a reimported mesh travels")
{
    // The exact behaviour the previous commit left type-gated, and the reason
    // the setting is worth having: the peers no longer have to go to source
    // control for a file one of them just rebuilt.
    Session s;
    REQUIRE(openHostOnly(s, /*hostSyncsLarge=*/true));
    s.client.setSyncLargeAssets(true);

    REQUIRE(s.client.joinSession("127.0.0.1", s.host.port(), s.host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return s.client.status() == CollabController::Status::Joined;
    }));

    HE::Ed::NotificationStore clientNotes;
    s.client.setNotifications(&clientNotes);

    std::string               gotPath;
    std::vector<std::uint8_t> got;
    s.client.onRemoteAsset([&](const std::string& p, const std::vector<std::uint8_t>& b) {
        gotPath = p; got = b;
    });

    const std::string key  = "Content/Meshes/Boulder.hasset";
    const fs::path    file = writeHAsset("he_collab_boulder.hasset",
                                         HE::AssetType::StaticMesh, "boulder-v2");

    s.host.requestAssetLock(key);
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.host.ownsAssetLock(key); }));

    s.host.publishReimport(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !got.empty(); }));

    CHECK(gotPath == key);
    CHECK(std::string(got.begin(), got.end()).find("boulder-v2") != std::string::npos);
    // The bytes went, so nobody is told to go and fetch them — being sent a file
    // AND told to pull it is the contradiction this replaces.
    CHECK(clientNotes.snapshot().empty());

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Larger assets: an ordinary save of a mesh travels too once the session carries them")
{
    Session s;
    REQUIRE(openHostOnly(s, /*hostSyncsLarge=*/true));
    s.client.setSyncLargeAssets(true);

    REQUIRE(s.client.joinSession("127.0.0.1", s.host.port(), s.host.joinCode(), "Bob"));
    REQUIRE(pumpUntil(s.host, s.client, [&] {
        return s.client.status() == CollabController::Status::Joined;
    }));

    std::vector<std::uint8_t> got;
    s.client.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>& b) {
        got = b;
    });

    const std::string key  = "Content/Textures/Wall.hasset";
    const fs::path    file = writeHAsset("he_collab_wall.hasset",
                                         HE::AssetType::Texture, "wall-v2");

    // publishAsset, not publishReimport: the two used to make the same decision
    // through two copies of it, which is exactly how they drift apart.
    s.host.publishAsset(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !got.empty(); }));
    CHECK(std::string(got.begin(), got.end()).find("wall-v2") != std::string::npos);

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Larger assets: the rule cannot be changed while a session is running")
{
    // Half a session running one rule and half the other is a group that quietly
    // holds different files — which is why the setting is locked rather than
    // merely discouraged, and why the lock lives here and not only in the UI
    // that greys the checkbox out.
    Session s;
    REQUIRE(openHostOnly(s, /*hostSyncsLarge=*/false));

    CHECK(s.host.largeAssetSyncLocked());
    s.host.setSyncLargeAssets(true);
    CHECK_FALSE(s.host.syncLargeAssetsSetting());
    CHECK_FALSE(s.host.sessionSyncsLargeAssets());

    // Connecting counts as running for this: the join request that is already in
    // flight carries the answer, so the window closed when the connect started
    // and not when the snapshot landed.
    REQUIRE(s.client.joinSession("127.0.0.1", s.host.port(), s.host.joinCode(), "Bob"));
    CHECK(s.client.largeAssetSyncLocked());
    s.client.setSyncLargeAssets(true);
    CHECK_FALSE(s.client.syncLargeAssetsSetting());

    s.client.leave();
    s.host.leave();

    // And out of a session it is an ordinary setting again.
    CHECK_FALSE(s.host.largeAssetSyncLocked());
    s.host.setSyncLargeAssets(true);
    CHECK(s.host.syncLargeAssetsSetting());
}

TEST_CASE("Larger assets: outside a session the local setting is the answer")
{
    // assetTypeTravels is asked by code that also runs with no session at all.
    // Answering a hard "no" there would make the same question have two answers
    // depending only on when it was asked.
    CollabController c;
    CHECK(c.assetTypeTravels(HE::AssetType::Material));
    CHECK_FALSE(c.assetTypeTravels(HE::AssetType::StaticMesh));

    c.setSyncLargeAssets(true);
    CHECK(c.assetTypeTravels(HE::AssetType::StaticMesh));
    CHECK(c.assetTypeTravels(HE::AssetType::Audio));
    // The static type rule is untouched by any of this — it is HE_Core's answer
    // about the asset kind, and it has callers outside the editor.
    CHECK_FALSE(CollabController::isSyncableAssetType(HE::AssetType::StaticMesh));
}

// ─── How big a file may travel ───────────────────────────────────────────────
// The ceiling is the user's to set, and the two ends set it separately. What
// these check is the half a user never sees coming: whose limit refused a file,
// and whether anybody was told.

TEST_CASE("Transfer ceiling: a hand-edited config cannot ask for more than the cap")
{
    // This value comes out of config.json, which is a text file somebody can put
    // a 4000 in. Clamped rather than refused, because a refusal would leave the
    // editor running on a number it had already rejected.
    CollabController c;
    CHECK(c.maxAssetMB() == 64);   // the default, and what an untouched config says

    c.setMaxAssetMB(4000);
    CHECK(c.maxAssetMB() == CollabController::kMaxAssetMB);
    c.setMaxAssetMB(0);
    CHECK(c.maxAssetMB() == CollabController::kMinAssetMB);
    c.setMaxAssetMB(-1);
    CHECK(c.maxAssetMB() == CollabController::kMinAssetMB);

    c.setMaxAssetMB(128);
    CHECK(c.maxAssetMB() == 128);
}

TEST_CASE("Transfer ceiling: a file over this machine's limit is not sent, and the notice says where the limit is")
{
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore hostNotes;
    s.host.setNotifications(&hostNotes);

    bool bytesArrived = false;
    s.client.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) {
        bytesArrived = true;
    });

    // A material, so nothing about the large-asset rule is involved: this is
    // purely the size ceiling refusing a file of a kind that always travels.
    s.host.setMaxAssetMB(1);
    const std::string key  = "Content/Materials/Marble.hasset";
    // Half a megabyte over the ceiling, so the rounded-up "2 MB" in the sentence
    // is the file and not the header that HAsset::Writer puts in front of it.
    const fs::path    file = writeHAsset("he_collab_marble.hasset",
                                         HE::AssetType::Material,
                                         std::string(3u * 512u * 1024u, 'm'));

    s.host.publishAsset(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !hostNotes.snapshot().empty(); }));

    CHECK_FALSE(bytesArrived);
    const HE::Ed::Notification n = findNote(hostNotes, "Marble.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Problem);
    CHECK(n.text.find("was not sent") != std::string::npos);
    // The three things a reader needs and cannot work out: how big it was, what
    // the limit is, and where that limit is changed. Without the last one this is
    // a complaint rather than something anyone can act on.
    CHECK(n.detail.find("2 MB") != std::string::npos);
    CHECK(n.detail.find("1 MB") != std::string::npos);
    CHECK(n.detail.find("Preferences") != std::string::npos);

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Transfer ceiling: the receiving machine refuses a file its sender was happy to send")
{
    // The asymmetry that makes this worth a notification at all. Anna's ceiling
    // let the file go, so on her machine the save simply worked and nothing will
    // ever say otherwise. Bob's is the lower one, and his editor is the only
    // place in the session that knows the file is not coming.
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore clientNotes;
    s.client.setNotifications(&clientNotes);

    bool bytesArrived = false;
    s.client.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) {
        bytesArrived = true;
    });

    // Set INSIDE the session, which is also the test that it reaches the live
    // one: the notice tells people to raise this and try again, and that advice
    // is a lie if the new number only takes effect on the next join.
    s.client.setMaxAssetMB(1);

    const std::string key  = "Content/Materials/Granite.hasset";
    const fs::path    file = writeHAsset("he_collab_granite.hasset",
                                         HE::AssetType::Material,
                                         std::string(3u * 512u * 1024u, 'g'));

    s.host.publishAsset(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !clientNotes.snapshot().empty(); }));

    // Nothing was written here, so what is on this disk is still the old file —
    // and the notice has to say so, or the reader assumes a partial one landed.
    CHECK_FALSE(bytesArrived);
    const HE::Ed::Notification n = findNote(clientNotes, "Granite.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Problem);
    CHECK(n.text.find("Anna") != std::string::npos);          // who sent it
    CHECK(n.text.find("refused") != std::string::npos);
    CHECK(n.detail.find("their limit is higher") != std::string::npos);
    CHECK(n.detail.find("Preferences") != std::string::npos);

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Transfer ceiling: the sender is told when the far end would not take the file")
{
    // The half that used to be missing entirely. Bob's ceiling let the file go,
    // so nothing on Bob's machine failed and nothing on Bob's machine would ever
    // have said a word; Anna's is the lower one and hers was the only editor that
    // knew. Two people then held different files, and the one who could act — the
    // one still looking at the file — was the one nobody told.
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore clientNotes;
    s.client.setNotifications(&clientNotes);

    bool bytesArrived = false;
    s.host.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) {
        bytesArrived = true;
    });

    // Anna (the host) will not take more than a megabyte; Bob is at the default
    // and has no reason to think anything is wrong.
    s.host.setMaxAssetMB(1);

    const std::string key  = "Content/Materials/Basalt.hasset";
    const fs::path    file = writeHAsset("he_collab_basalt.hasset",
                                         HE::AssetType::Material,
                                         std::string(3u * 512u * 1024u, 'b'));

    s.client.requestAssetLock(key);
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.client.ownsAssetLock(key); }));

    s.client.publishAsset(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !clientNotes.snapshot().empty(); }));

    CHECK_FALSE(bytesArrived);
    const HE::Ed::Notification n = findNote(clientNotes, "Basalt.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Problem);
    CHECK(n.text.find("Anna") != std::string::npos);           // who refused it
    CHECK(n.text.find("could not accept") != std::string::npos);
    CHECK(n.detail.find("2 MB") != std::string::npos);          // how big it was
    // THEIR ceiling, which this machine has no other way of learning — and the
    // direction of the asymmetry, because the reader's own limit is fine and
    // raising it would achieve precisely nothing.
    CHECK(n.detail.find("1 MB") != std::string::npos);
    CHECK(n.detail.find("their limit is lower") != std::string::npos);
    // A save, so they still hold the older file — and the one channel that does
    // carry a file this size.
    CHECK(n.detail.find("older version") != std::string::npos);
    CHECK(n.detail.find("source control") != std::string::npos);

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Transfer ceiling: a third editor's refusal is carried back through the host to the sender")
{
    // The route two participants cannot exercise. Bob saves, Anna (host) takes it
    // happily, and Cara will not — and Cara cannot say so to Bob, because clients
    // never address one another. The refusal goes to Anna, who is the only peer
    // that can reach Bob, and Anna forwards it. Bob is left with a file that
    // reached one collaborator and not the other, which is precisely the state
    // nobody was being told about.
    HorizonWorld annaWorld, bobWorld, caraWorld;
    annaWorld.createEntity("Cube");

    CollabController anna, bob, cara;
    anna.setWorld(&annaWorld);
    REQUIRE(anna.startHosting(0, "Anna"));

    bob.setWorld(&bobWorld);
    bob.onWorldReplaced([&bob] { bob.seedNetIds(); });
    cara.setWorld(&caraWorld);
    cara.onWorldReplaced([&cara] { cara.seedNetIds(); });

    REQUIRE(bob.joinSession("127.0.0.1", anna.port(), anna.joinCode(), "Bob"));
    REQUIRE(cara.joinSession("127.0.0.1", anna.port(), anna.joinCode(), "Cara"));
    REQUIRE(pumpUntil3(anna, bob, cara, [&] {
        return bob.status()  == CollabController::Status::Joined &&
               cara.status() == CollabController::Status::Joined &&
               anna.participants().size() == 3;
    }));

    HE::Ed::NotificationStore bobNotes, annaNotes, caraNotes;
    bob.setNotifications(&bobNotes);
    anna.setNotifications(&annaNotes);
    cara.setNotifications(&caraNotes);

    int annaGot = 0, caraGot = 0;
    anna.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) { ++annaGot; });
    cara.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) { ++caraGot; });

    // Only Cara is tight. Bob and Anna are at the default, so from both of their
    // points of view this save is entirely ordinary.
    cara.setMaxAssetMB(1);

    const std::string key  = "Content/Materials/Slate.hasset";
    const fs::path    file = writeHAsset("he_collab_slate.hasset",
                                         HE::AssetType::Material,
                                         std::string(3u * 512u * 1024u, 's'));

    bob.requestAssetLock(key);
    REQUIRE(pumpUntil3(anna, bob, cara, [&] { return bob.ownsAssetLock(key); }));

    bob.publishAsset(key, file.string());
    REQUIRE(pumpUntil3(anna, bob, cara, [&] { return !bobNotes.snapshot().empty(); }));

    // It really did reach one of them and not the other — the split this notice
    // exists to describe.
    CHECK(annaGot == 1);
    CHECK(caraGot == 0);

    const HE::Ed::Notification n = findNote(bobNotes, "Slate.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Problem);
    // Named, and named correctly: the refusal came from Cara and was merely
    // carried by Anna. A message that blamed the messenger would send Bob to ask
    // the wrong person to change a setting.
    CHECK(n.text.find("Cara") != std::string::npos);
    CHECK(n.text.find("Anna") == std::string::npos);
    CHECK(n.detail.find("2 MB") != std::string::npos);
    CHECK(n.detail.find("1 MB") != std::string::npos);

    // Anna forwarded it and was not herself refused anything, so she has nothing
    // to be told. A refusal that fanned out like an asset would have landed here.
    CHECK(annaNotes.snapshot().empty());

    // Cara still gets her own side of it: her machine refused a file, and that is
    // a different sentence with a different fix from the one Bob is reading.
    const HE::Ed::Notification c = findNote(caraNotes, "Slate.hasset");
    CHECK(c.level == HE::Ed::NoteLevel::Problem);
    CHECK(c.text.find("refused") != std::string::npos);

    // And exactly one notice each: a refusal that could be refused in turn, or
    // that travelled the asset path, would still be going round.
    CHECK(bobNotes.snapshot().size()  == 1);
    CHECK(caraNotes.snapshot().size() == 1);

    cara.leave();
    bob.leave();
    anna.leave();
    he_test::removeQuiet(file);
}

TEST_CASE("Transfer ceiling: a refused CREATE is settled by the refusal, not left waiting for ever")
{
    // The state a refusal used to leave behind, and the reason this is not just a
    // missing notification. A create is remembered under its key until the host's
    // verdict comes back — but a create refused on ARRIVAL never reaches the
    // arbitration that would send one, because the host drops the frame on its
    // size before a byte is assembled. The entry then waits for an answer that
    // was never going to be sent, and while it waits it parks every OTHER peer's
    // create for that same path. One oversized file would quietly stop anybody in
    // the session from ever creating that asset. The refusal is the verdict.
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore clientNotes;
    s.client.setNotifications(&clientNotes);

    // Two resolvers pointing at two different names, because both editors are on
    // this one machine: sharing a path would have the host find its own copy of
    // the file and call the name taken.
    const fs::path dir = fs::temp_directory_path();
    s.host.setLocalPathResolver([&dir](const std::string& key) {
        return (dir / ("he_anna_" + fs::path(key).filename().string())).string();
    });
    s.client.setLocalPathResolver([&dir](const std::string& key) {
        return (dir / ("he_bob_" + fs::path(key).filename().string())).string();
    });

    s.host.setMaxAssetMB(1);

    const std::string key = "Content/Materials/Fresh.hasset";
    const fs::path    big = writeHAsset("he_collab_fresh_big.hasset",
                                        HE::AssetType::Material,
                                        std::string(3u * 512u * 1024u, 'f'));

    s.client.publishAssetCreate(key, big.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !clientNotes.snapshot().empty(); }));

    const HE::Ed::Notification n = findNote(clientNotes, "Fresh.hasset");
    CHECK(n.level == HE::Ed::NoteLevel::Problem);
    // A create, so the clause about what they are left holding is the other one:
    // there is no older version over there to fall back on.
    CHECK(n.detail.find("does not exist at all") != std::string::npos);
    CHECK(n.detail.find("older version") == std::string::npos);

    // And now the part that proves the entry is gone. Anna creates something at
    // the SAME key; Bob has no create of his own outstanding any more, so the
    // bytes are applied instead of being held back behind one. With the entry
    // still dangling this callback never fires at all.
    std::string applied;
    s.client.onRemoteAsset([&](const std::string& p, const std::vector<std::uint8_t>&) {
        applied = p;
    });

    const fs::path small = writeHAsset("he_collab_fresh_small.hasset",
                                       HE::AssetType::Material, "fresh-v1");
    s.host.publishAssetCreate(key, small.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return !applied.empty(); }));
    CHECK(applied == key);

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(big);
    he_test::removeQuiet(small);
}

TEST_CASE("Transfer ceiling: a file everyone can take produces no notice anywhere")
{
    // The silence that has to survive all of this. A refusal path that fired on
    // an ordinary save would put a Problem in front of every collaborator for
    // every file that worked — worse than the silence it was built to end.
    Session s;
    REQUIRE(openSession(s));

    HE::Ed::NotificationStore hostNotes, clientNotes;
    s.host.setNotifications(&hostNotes);
    s.client.setNotifications(&clientNotes);

    bool bytesArrived = false;
    s.client.onRemoteAsset([&](const std::string&, const std::vector<std::uint8_t>&) {
        bytesArrived = true;
    });

    const std::string key  = "Content/Materials/Chalk.hasset";
    const fs::path    file = writeHAsset("he_collab_chalk.hasset",
                                         HE::AssetType::Material, "chalk-v1");

    s.host.requestAssetLock(key);
    REQUIRE(pumpUntil(s.host, s.client, [&] { return s.host.ownsAssetLock(key); }));

    s.host.publishAsset(key, file.string());
    REQUIRE(pumpUntil(s.host, s.client, [&] { return bytesArrived; }));

    CHECK(hostNotes.snapshot().empty());
    CHECK(clientNotes.snapshot().empty());

    s.client.leave();
    s.host.leave();
    he_test::removeQuiet(file);
}
