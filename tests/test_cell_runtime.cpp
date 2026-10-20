// HE::CellRuntime against a fake ICellHost (Thema 164, step 2c): the part of cell
// streaming the game and the editor's Play share. What the host is asked, when,
// and with which token; the manifest followed through changes; clear() against
// reset(); the bodies of a cell in and out of a real physics world.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/CellRuntime.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/NetworkComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/GameReplication.h>
#include <HorizonScene/Net/NetGameSession.h>
#include <HorizonScene/Net/SpawnReplicator.h>
#include <Net/LoopbackTransport.h>
#include <ContentManager/DefaultAssets.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
TransformComponent& tf(HorizonWorld& w, Entity e)
{
	return w.registry().get_or_emplace<TransformComponent>(e);
}

// A version-2 cell of `boxes` static boxes with a cube mesh, side by side in the
// middle of the 100 m square that starts at x0.
void writeBoxCell(const std::filesystem::path& file, float x0, int boxes)
{
	HorizonWorld cell;
	for (int i = 0; i < boxes; ++i)
	{
		const Entity b = cell.createEntity("Box " + std::to_string(i));
		tf(cell, b).position = { x0 + 40.0f + static_cast<float>(i) * 2.0f, 0.5f, 50.0f };
		MeshComponent mc;
		mc.meshAssetId = HE::kDefaultCubeMeshId;
		cell.addComponent(b, mc);
		RigidBodyComponent rb;
		rb.type = RigidBodyType::Static;
		cell.addComponent(b, rb);
	}
	std::filesystem::create_directories(file.parent_path());
	SceneSerializer ser;
	REQUIRE(ser.save(cell, file, SerializeFormat::JSON));
	nlohmann::json j;
	{
		std::ifstream in(file);
		j = nlohmann::json::parse(in);
	}
	j["streaming"] = { { "version", 2 } };
	std::ofstream out(file, std::ios::trunc);
	out << j.dump();
}

// A row of four 100 m cells along +X with six boxes each; the list names a fifth
// that has no file. Load within 60 m, unload beyond 120 m, two seconds ahead.
constexpr const char* kManifest =
	R"({"version": 2, "cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 2,
	    "dir": "W.cells", "list": [[0,0,6,6],[1,0,6,6],[2,0,6,6],[3,0,6,6],[4,0,6,6]]})";

std::filesystem::path writeRow(const char* name)
{
	const auto root = std::filesystem::temp_directory_path() / name;
	he_test::removeAllQuiet(root);
	for (int x = 0; x < 4; ++x)
		writeBoxCell(root / "W.cells" / ("cell_" + std::to_string(x) + "_0.hescene"), x * 100.0f, 6);
	return root;
}

// What an application tells the runtime, written down.
struct FakeHost final : HE::ICellHost
{
	std::filesystem::path root;
	PhysicsWorld*         physics = nullptr;
	int opened = 0, loadedCalls = 0, unloadingCalls = 0, assetCalls = 0;
	std::map<uint32_t, HE::CancelToken> tokenOf;           // entity → the token its slice came with
	std::map<uint32_t, std::vector<uint32_t>> cellOf;      // root → every entity of the cell
	std::vector<bool> tokenLiveAtUnloading;                // per unloading call
	bool bodiesThereAtUnloading = true;
	std::vector<HE::CellPlayer> players;                   // named as they are, every frame
	NetGameSession*             session = nullptr;         // and the session's, as the apps do

	std::function<bool(std::vector<uint8_t>&)> openCellFile(const std::string& path) override
	{
		++opened;
		const auto file = root / path;
		if (!std::filesystem::exists(file)) return {};
		return [file](std::vector<uint8_t>& out)
		{
			std::ifstream in(file, std::ios::binary);
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return !out.empty();
		};
	}
	PhysicsWorld* cellPhysics() override { return physics; }
	void streamCellAssets(HorizonWorld&, const HE::CancelToken& token, const std::vector<uint32_t>& entities) override
	{
		++assetCalls;
		CHECK(token.cancellable());
		CHECK_FALSE(token.cancelled());
		for (uint32_t e : entities) tokenOf[e] = token;
	}
	void cellLoaded(HorizonWorld& world, entt::entity root, const std::vector<entt::entity>& created) override
	{
		++loadedCalls;
		CHECK(world.registry().valid(root));
		std::vector<uint32_t>& ids = cellOf[static_cast<uint32_t>(root)];
		for (entt::entity e : created) ids.push_back(static_cast<uint32_t>(e));
	}
	void cellUnloading(HorizonWorld& world, entt::entity root) override
	{
		++unloadingCalls;
		CHECK(world.registry().valid(root));   // still whole when the host is told
		const auto t = tokenOf.find(static_cast<uint32_t>(root));
		tokenLiveAtUnloading.push_back(t != tokenOf.end() && !t->second.cancelled());
		if (physics) bodiesThereAtUnloading = bodiesThereAtUnloading && physics->bodyCount() > 0;
	}
	void cellPlayers(HorizonWorld& world, std::vector<HE::CellPlayer>& out) override
	{
		out.insert(out.end(), players.begin(), players.end());
		HE::sessionPlayers(session, world, out);
	}
};

Entity makeCamera(HorizonWorld& world, double x)
{
	const Entity cam = world.createEntity("Camera");
	tf(world, cam).position = { static_cast<float>(x), 1.7f, 50.0f };
	world.registry().emplace<CameraComponent>(cam);
	return cam;
}

size_t countMeshes(HorizonWorld& world)
{
	size_t n = 0;
	for (auto e : world.registry().view<MeshComponent>()) { (void)e; ++n; }
	return n;
}

// Updates (one frame apart, dt as given) until `done` and nothing reading, parsed or
// half built, or twenty seconds.
template <class Done>
void pump(HE::CellRuntime& rt, HorizonWorld& world, float dt, Done done)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < until)
	{
		rt.update(world, dt);
		const auto& st = rt.streamer().stats();
		if (done() && st.inFlight == 0 && st.ready == 0 && st.building == 0) return;
		std::this_thread::yield();
	}
}
} // namespace

TEST_CASE("CellRuntime: streams around the world's camera through its host, one token per cell")
{
	const auto root = writeRow("he_cell_runtime_row");
	FakeHost host;
	host.root = root;
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0);
	HE::CellRuntime rt(host);

	// No manifest: nothing to do, nothing asked.
	rt.update(world, 1.0f / 60.0f);
	CHECK_FALSE(rt.active());
	CHECK(host.opened == 0);

	// The manifest appears (a scene was loaded): it starts. Slices of three, so a
	// cell of six boxes comes in three pieces (its root alone, then two of three).
	rt.streamer().setSliceEntities(3);
	world.setCellManifestJson(kManifest);
	pump(rt, world, 0.0f, [&] { return rt.streamer().stats().loaded == 2; });
	REQUIRE(rt.active());
	CHECK(rt.streamer().isLoaded(0, 0));
	CHECK(rt.streamer().isLoaded(1, 0));
	CHECK_FALSE(rt.streamer().isLoaded(2, 0));
	CHECK(countMeshes(world) == 12);
	CHECK(host.loadedCalls == 2);
	CHECK(host.assetCalls >= 6);            // at least three slices per cell
	CHECK(rt.cellsWithLoads() == 2);

	// Every slice of one cell came with that cell's token, and the two cells with two.
	REQUIRE(host.cellOf.size() == 2);
	std::vector<HE::CancelToken> perCell;
	for (const auto& [r, ids] : host.cellOf)
	{
		REQUIRE(ids.size() == 7);   // root and six boxes
		REQUIRE(host.tokenOf.count(ids.front()));
		const HE::CancelToken t = host.tokenOf[ids.front()];
		for (uint32_t e : ids)
		{
			REQUIRE(host.tokenOf.count(e));
			CHECK(host.tokenOf[e] == t);
		}
		perCell.push_back(t);
	}
	CHECK(perCell[0] != perCell[1]);

	// The camera walks to x = 350: cells 0 and 1 go (the host hears it while its
	// token is still live, and the token is cancelled after), 2 and 3 come.
	const uint32_t firstRoot = host.cellOf.begin()->first;
	const HE::CancelToken firstToken = host.tokenOf[firstRoot];
	tf(world, cam).position.x = 350.0f;
	pump(rt, world, 0.0f, [&] { return rt.streamer().isLoaded(3, 0) && !rt.streamer().isLoaded(0, 0)
	                                   && !rt.streamer().isLoaded(1, 0); });
	CHECK(rt.streamer().isLoaded(2, 0));
	CHECK(host.unloadingCalls == 2);
	CHECK(host.tokenLiveAtUnloading == std::vector<bool>{ true, true });
	CHECK(firstToken.cancelled());
	CHECK(rt.cellsWithLoads() == 2);
	CHECK(countMeshes(world) == 12);

	// The cell with no file: asked for once, failed, never again.
	tf(world, cam).position.x = 450.0f;
	pump(rt, world, 0.0f, [&] { return rt.streamer().stats().failed == 1; });
	CHECK(rt.streamer().stats().failed == 1);
	CHECK_FALSE(rt.streamer().isLoaded(4, 0));

	// No camera: nothing changes, nothing more is read.
	tf(world, cam).position.x = 50.0f;
	world.registry().remove<CameraComponent>(cam);
	const int openedBefore = host.opened;
	const size_t loadedBefore = rt.streamer().stats().loaded;
	for (int i = 0; i < 20; ++i) rt.update(world, 1.0f / 60.0f);
	CHECK(host.opened == openedBefore);
	CHECK(rt.streamer().stats().loaded == loadedBefore);

	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellRuntime: the camera's motion is the lookahead, a jump is not")
{
	const auto root = writeRow("he_cell_runtime_ahead");
	FakeHost host;
	host.root = root;
	HorizonWorld world;
	const Entity cam = makeCamera(world, 30.0);
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	pump(rt, world, 0.1f, [&] { return rt.streamer().stats().loaded == 1; });
	CHECK(rt.streamer().isLoaded(0, 0));
	CHECK_FALSE(rt.streamer().isLoaded(1, 0));   // 70 m from x = 30

	// Walking +X at 100 m/s (a tenth of a metre a millisecond): two seconds ahead is
	// 200 m further, so cell 2 is wanted, and built, while the camera is still in
	// cell 0 — far from the 140 m where it would be wanted standing still.
	float x = 30.0f;
	bool  aheadInTime = false;
	for (int i = 0; i < 600 && !aheadInTime; ++i)
	{
		x += 0.1f;
		tf(world, cam).position.x = x;
		rt.update(world, 0.001f);
		aheadInTime = rt.streamer().isLoaded(2, 0) && x < 100.0f;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(aheadInTime);
	CHECK(x < 100.0f);
	rt.clear(world);

	// A jump is not a speed: anything over 300 m/s counts as standing. With one second
	// of lookahead the point ahead of a camera at x = 33 lands in cell 3 for any speed
	// up to ~360 m/s, so the two runs differ only in the clamp: 290 m/s starts reading
	// cell 3 in that very update (the control), 310 m/s reads nothing.
	std::string oneSecond = kManifest;
	oneSecond.replace(oneSecond.find("\"lookaheadSec\": 2"), 17, "\"lookaheadSec\": 1");
	for (const float speed : { 290.0f, 310.0f })
	{
		HorizonWorld world2;
		const Entity cam2 = makeCamera(world2, 30.0);
		world2.setCellManifestJson(oneSecond);
		pump(rt, world2, 0.01f, [&] { return rt.streamer().stats().loaded == 1; });
		REQUIRE(rt.streamer().isLoaded(0, 0));
		tf(world2, cam2).position.x = 30.0f + speed * 0.01f;
		rt.update(world2, 0.01f);
		const auto& st = rt.streamer().stats();
		const size_t touched = st.inFlight + st.ready + st.building + st.loaded;   // cell 0 alone: 1
		if (speed < 300.0f) CHECK(touched > 1);
		else                CHECK(touched == 1);
		rt.clear(world2);
	}
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellRuntime: a changed manifest or world starts over; clear() unloads, reset() forgets")
{
	const auto root = writeRow("he_cell_runtime_restart");
	FakeHost host;
	host.root = root;
	HorizonWorld world;
	makeCamera(world, 50.0);
	const size_t base = world.registry().storage<entt::entity>().in_use();
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	pump(rt, world, 0.0f, [&] { return rt.streamer().stats().loaded == 2; });
	REQUIRE(rt.streamer().stats().loaded == 2);
	const HE::CancelToken token = host.tokenOf.begin()->second;

	// clear(): every cell through the host, out of the world, the loads cancelled,
	// and the runtime idle until it is updated again.
	rt.clear(world);
	CHECK(host.unloadingCalls == 2);
	CHECK(token.cancelled());
	CHECK_FALSE(rt.active());
	CHECK(rt.cellsWithLoads() == 0);
	CHECK(world.registry().storage<entt::entity>().in_use() == base);
	CHECK(countMeshes(world) == 0);

	// ...and the next update starts again from the world's manifest.
	pump(rt, world, 0.0f, [&] { return rt.streamer().stats().loaded == 2; });
	CHECK(rt.streamer().stats().loaded == 2);
	CHECK(countMeshes(world) == 12);

	// The manifest goes (the scene was merged, or another without cells loaded):
	// the runtime stops and forgets, it does not destroy what the world holds — the
	// world that changed under it is the one that dropped them.
	std::vector<HE::CancelToken> live;
	for (const auto& [e, t] : host.tokenOf)
		if (!t.cancelled()) live.push_back(t);
	REQUIRE_FALSE(live.empty());
	const int unloadsBefore = host.unloadingCalls;
	world.setCellManifestJson("");
	rt.update(world, 0.0f);
	CHECK_FALSE(rt.active());
	CHECK(host.unloadingCalls == unloadsBefore);
	CHECK(rt.cellsWithLoads() == 0);
	CHECK(countMeshes(world) == 12);
	for (const HE::CancelToken& t : live) CHECK(t.cancelled());

	// A malformed list: logged, inactive, nothing read.
	const int openedBefore = host.opened;
	world.setCellManifestJson(R"({"cellSize": "big"})");
	rt.update(world, 0.0f);
	CHECK_FALSE(rt.active());
	CHECK(host.opened == openedBefore);

	// Another world with the same manifest is another start: nothing of the first
	// world's cells is touched by clear() on the second.
	HorizonWorld other;
	makeCamera(other, 50.0);
	other.setCellManifestJson(kManifest);
	world.setCellManifestJson(kManifest);
	pump(rt, other, 0.0f, [&] { return rt.streamer().stats().loaded == 2; });
	CHECK(countMeshes(other) == 12);
	const size_t inWorld = world.registry().storage<entt::entity>().in_use();
	rt.clear(world);   // not the world it streams into: forgets, destroys nothing
	CHECK(world.registry().storage<entt::entity>().in_use() == inWorld);
	CHECK(countMeshes(other) == 12);
	CHECK_FALSE(rt.active());

	// reset(): forgets without touching any world.
	pump(rt, other, 0.0f, [&] { return rt.streamer().stats().loaded == 2; });
	const size_t inOther = other.registry().storage<entt::entity>().in_use();
	const int unloads = host.unloadingCalls;
	rt.reset();
	CHECK(other.registry().storage<entt::entity>().in_use() == inOther);
	CHECK(host.unloadingCalls == unloads);
	CHECK_FALSE(rt.active());
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellRuntime: a cell's bodies go into the host's physics world with it and leave before it")
{
	const auto root = writeRow("he_cell_runtime_bodies");
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0);
	PhysicsWorld phys;
	phys.initialize(world);
	REQUIRE(phys.bodyCount() == 0u);

	FakeHost host;
	host.root    = root;
	host.physics = &phys;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	rt.streamer().setSliceEntities(3);
	pump(rt, world, 0.0f, [&] { return rt.streamer().stats().loaded == 2; });
	CHECK(phys.bodyCount() == 12u);   // six static boxes in each of two cells

	// Away from both: they go, and the host heard of each while its bodies were in.
	tf(world, cam).position.x = 1000.0f;
	pump(rt, world, 0.0f, [&] { return rt.streamer().stats().loaded == 0; });
	CHECK(host.unloadingCalls == 2);
	CHECK(host.bodiesThereAtUnloading);
	CHECK(phys.bodyCount() == 0u);

	// Back again, and clear(): the bodies come and go with the cells once more.
	tf(world, cam).position.x = 50.0f;
	pump(rt, world, 0.0f, [&] { return rt.streamer().stats().loaded == 2; });
	CHECK(phys.bodyCount() == 12u);
	rt.clear(world);
	CHECK(phys.bodyCount() == 0u);
	he_test::removeAllQuiet(root);
}

// ── The API's streaming group (Thema 183 step 3) ─────────────────────────────

namespace
{
// The runtime the streaming rows ask, installed for one test and taken away again.
struct Installed
{
	explicit Installed(HE::CellRuntime* rt) { HE::api::streaming::install(rt); }
	~Installed() { HE::api::streaming::install(nullptr); }
};

using HorizonCode::Value;
std::vector<Value> callRow(HE::api::Ctx& c, const char* id, std::vector<Value> args)
{
	const HE::api::ApiFn* fn = HE::api::find(id);
	REQUIRE(fn != nullptr);
	return fn->invoke(c, args);
}
} // namespace

TEST_CASE("CellRuntime: a script's pin keeps the cells around it loaded, and the streaming rows say what is there")
{
	const auto root = writeRow("he_cell_runtime_pin");
	FakeHost host;
	host.root = root;
	HorizonWorld world;
	// A world origin, so that a row that forgot to add it asks about the wrong square:
	// world position x = absolute x - 500.
	world.setOrigin({ 500.0, 0.0, 0.0 });
	const Entity cam = makeCamera(world, -2500.0);   // absolute -2000: far from every cell
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	const Installed installed(&rt);
	HE::api::Ctx c{ &world, nullptr, nullptr };
	CHECK(HE::api::isScriptGroup("streaming"));

	// Cell (2,0) spans absolute x 200..300; its middle, as a script sees it, is -250.
	const glm::vec3 inCell2{ -250.0f, 0.0f, 50.0f };
	// Pinned before the runtime has seen the manifest at all, as a BeginPlay would.
	const int handle = callRow(c, "streaming.pin", { Value::ofVec3(inCell2), Value::ofFloat(10.0f) })[0].i;
	REQUIRE(handle > 0);
	CHECK(rt.pinCount() == 1);

	pump(rt, world, 0.016f, [&] { return rt.streamer().isLoaded(2, 0); });
	CHECK(rt.pinCount() == 1);   // the start for the manifest kept it
	CHECK(callRow(c, "streaming.isLoaded", { Value::ofInt(2), Value::ofInt(0) })[0].b);
	// The pin's radius is its own (10 m), not the manifest's 60: the neighbour is 50 m off.
	CHECK_FALSE(callRow(c, "streaming.isLoaded", { Value::ofInt(1), Value::ofInt(0) })[0].b);
	CHECK(rt.streamer().stats().anchors == 2u);   // the camera and the pin

	const auto at = callRow(c, "streaming.cellAt", { Value::ofVec3(inCell2) });
	REQUIRE(at.size() == 3);
	CHECK(at[0].i == 2);
	CHECK(at[1].i == 0);
	CHECK(at[2].b);
	// Absolute -2000 is in square -20, where nothing was split out.
	const auto far = callRow(c, "streaming.cellAt", { Value::ofVec3({ -2500.0f, 0.0f, 50.0f }) });
	CHECK(far[0].i == -20);
	CHECK_FALSE(far[2].b);

	CHECK(callRow(c, "streaming.isSettled", { Value::ofVec3(inCell2), Value::ofFloat(10.0f) })[0].b);
	// Cell 0 (absolute 0..100) is not there.
	CHECK_FALSE(callRow(c, "streaming.isSettled", { Value::ofVec3({ -450.0f, 0.0f, 50.0f }), Value::ofFloat(10.0f) })[0].b);
	// Nowhere near a cell: nothing to wait for.
	CHECK(callRow(c, "streaming.isSettled", { Value::ofVec3({ -2500.0f, 0.0f, 50.0f }), Value::ofFloat(10.0f) })[0].b);

	// Unpinned, the cell goes: the camera does not want it.
	CHECK(callRow(c, "streaming.unpin", { Value::ofInt(handle) })[0].b);
	CHECK_FALSE(callRow(c, "streaming.unpin", { Value::ofInt(handle) })[0].b);
	pump(rt, world, 0.016f, [&] { return !rt.streamer().isLoaded(2, 0); });
	CHECK_FALSE(rt.streamer().isLoaded(2, 0));
	(void)cam;
	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellRuntime: pins go with the session, and the streaming rows answer neutrally without a runtime")
{
	FakeHost host;
	HE::CellRuntime rt(host);
	CHECK(rt.pin({ 1.0, 0.0, 1.0 }, 5.0f) > 0);
	rt.reset();
	CHECK(rt.pinCount() == 0);
	HorizonWorld world;
	CHECK(rt.pin({ 1.0, 0.0, 1.0 }, 5.0f) > 0);
	rt.clear(world);
	CHECK(rt.pinCount() == 0);

	// Nothing installed (the editor outside Play, an app without a scene): no cell, no
	// pin, and settled — a teleport gate must not hang where nothing streams.
	HE::api::streaming::install(nullptr);
	HE::api::Ctx c{ &world, nullptr, nullptr };
	CHECK(callRow(c, "streaming.pin", { Value::ofVec3({ 0.0f, 0.0f, 0.0f }), Value::ofFloat(10.0f) })[0].i == 0);
	CHECK_FALSE(callRow(c, "streaming.unpin", { Value::ofInt(1) })[0].b);
	CHECK_FALSE(callRow(c, "streaming.cellAt", { Value::ofVec3({ 0.0f, 0.0f, 0.0f }) })[2].b);
	CHECK_FALSE(callRow(c, "streaming.isLoaded", { Value::ofInt(0), Value::ofInt(0) })[0].b);
	CHECK(callRow(c, "streaming.isSettled", { Value::ofVec3({ 0.0f, 0.0f, 0.0f }), Value::ofFloat(50.0f) })[0].b);
	// Installed but the scene has no cells: the same answers.
	const Installed installed(&rt);
	CHECK(callRow(c, "streaming.isSettled", { Value::ofVec3({ 0.0f, 0.0f, 0.0f }), Value::ofFloat(50.0f) })[0].b);
	CHECK_FALSE(callRow(c, "streaming.cellAt", { Value::ofVec3({ 0.0f, 0.0f, 0.0f }) })[2].b);
}

// ── Players as anchors (Thema 183 step 4) ────────────────────────────────────

TEST_CASE("CellRuntime: the host's players are anchors without a camera, each with their own lookahead")
{
	const auto root = writeRow("he_cell_runtime_players");
	FakeHost host;
	host.root = root;
	HorizonWorld world;   // a server's: no camera
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);

	// Nobody there yet: nothing to stream around, nothing read.
	for (int i = 0; i < 5; ++i) rt.update(world, 0.016f);
	CHECK(rt.streamer().stats().loaded == 0u);
	CHECK(host.opened == 0);

	// Player 7 stands at x = 30 in cell 0; the neighbour is 70 m off.
	host.players = { { 7u, { 30.0, 0.0, 50.0 } } };
	pump(rt, world, 0.1f, [&] { return rt.streamer().stats().loaded == 1; });
	CHECK(rt.streamer().isLoaded(0, 0));
	CHECK_FALSE(rt.streamer().isLoaded(1, 0));
	CHECK(rt.streamer().stats().anchors == 1u);

	// Walking +X at 100 m/s, as the camera does in the lookahead case above: cell 2 is
	// built while the player is still in cell 0. Without their own lookahead it would
	// wait until x = 140.
	double x = 30.0;
	bool   aheadInTime = false;
	for (int i = 0; i < 600 && !aheadInTime; ++i)
	{
		x += 0.1;
		host.players[0].position.x = x;
		rt.update(world, 0.001f);
		aheadInTime = rt.streamer().isLoaded(2, 0) && x < 100.0;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(aheadInTime);

	// The player leaves; a second one stands in cell 3. What only the first wanted goes.
	host.players = { { 8u, { 350.0, 0.0, 50.0 } } };
	pump(rt, world, 0.016f, [&] { return !rt.streamer().isLoaded(0, 0) && rt.streamer().isLoaded(3, 0); });
	CHECK_FALSE(rt.streamer().isLoaded(0, 0));
	CHECK(rt.streamer().isLoaded(3, 0));
	CHECK(rt.streamer().stats().anchors == 1u);

	rt.clear(world);
	he_test::removeAllQuiet(root);
}

namespace
{
// A host and one client over a loopback pair, both on their own world: what a
// listen server and a joined game are, without sockets.
struct NetPair
{
	HorizonWorld   hostWorld, clientWorld;
	NetGameSession host, client;

	NetPair()
	{
		auto [a, b] = HE::Net::LoopbackTransport::createPair();
		host.setWorld(&hostWorld);
		client.setWorld(&clientWorld);
		// The client makes what the host spawns, like an application's createObject.
		client.setSpawnFunction([this](const std::string& cls, const glm::vec3& pos, const glm::vec3&,
		                               HE::Net::Game::PlayerId) -> Entity
		{
			const Entity e = clientWorld.createEntity(cls);
			clientWorld.registry().emplace_or_replace<TransformComponent>(e).position = pos;
			return e;
		});
		NetGameSession::HostOptions ho;
		ho.displayName = "Host";
		ho.announceLan = false;
		ho.projectId   = "cells";
		REQUIRE(host.hostOn(std::move(a), ho));
		NetGameSession::JoinOptions jo;
		jo.displayName = "Anna";
		jo.projectId   = "cells";
		REQUIRE(client.joinOn(std::move(b), jo));
		for (int i = 0; i < 60 && client.status() != NetGameSession::Status::Joined; ++i) step();
		REQUIRE(client.status() == NetGameSession::Status::Joined);
	}
	void step()
	{
		host.update(1.0f / 60.0f);
		client.update(1.0f / 60.0f);
	}
	// A character on the host for `player`, spawned through the session and given to them.
	Entity character(HE::Net::Game::PlayerId player, const glm::vec3& at)
	{
		const Entity e = hostWorld.createEntity("Character");
		hostWorld.registry().emplace_or_replace<TransformComponent>(e).position = at;
		REQUIRE(host.spawns()->notifySpawned(e, "classes/PlayerCharacter.hcclass", player) != 0u);
		REQUIRE(host.assignControl(player, e));
		return e;
	}
};
} // namespace

TEST_CASE("CellRuntime: on a host every player's character holds its cells; a client streams for none of them")
{
	const auto root = writeRow("he_cell_runtime_net");
	NetPair net;
	const HE::Net::Game::PlayerId anna = net.client.localPlayer();
	REQUIRE(anna != HE::Net::Game::kNoPlayer);
	REQUIRE(anna != HE::Net::Game::kHostPlayer);

	// A world origin, so that a player read without it stands in the wrong square:
	// world x = absolute x - 500. No camera on the host — a server's world.
	net.hostWorld.setOrigin({ 500.0, 0.0, 0.0 });
	net.hostWorld.setCellManifestJson(kManifest);
	// The host's own character in cell 0 (absolute 10), Anna's in cell 2 (absolute 250).
	const Entity mine  = net.character(HE::Net::Game::kHostPlayer, { -490.0f, 0.0f, 50.0f });
	const Entity hers  = net.character(anna, { -250.0f, 0.0f, 50.0f });
	for (int i = 0; i < 10; ++i) net.step();

	std::vector<HE::CellPlayer> players;
	HE::sessionPlayers(&net.host, net.hostWorld, players);
	REQUIRE(players.size() == 2u);
	std::map<uint32_t, double> xOf;
	for (const auto& p : players) xOf[p.id] = p.position.x;
	CHECK(xOf[HE::Net::Game::kHostPlayer] == doctest::Approx(10.0));
	CHECK(xOf[anna] == doctest::Approx(250.0));
	// Another world under the same net ids is somebody else's.
	players.clear();
	HE::sessionPlayers(&net.host, net.clientWorld, players);
	CHECK(players.empty());

	FakeHost host;
	host.root    = root;
	host.session = &net.host;
	HE::CellRuntime rt(host);
	pump(rt, net.hostWorld, 0.016f, [&] { return rt.streamer().isLoaded(0, 0) && rt.streamer().isLoaded(2, 0); });
	CHECK(rt.streamer().isLoaded(0, 0));
	CHECK(rt.streamer().isLoaded(2, 0));
	CHECK(rt.streamer().stats().anchors == 2u);

	// Anna walks off the row (a jump: standing); her cell goes, the host's stays.
	tf(net.hostWorld, hers).position.x = 1500.0f;
	pump(rt, net.hostWorld, 0.016f, [&] { return !rt.streamer().isLoaded(2, 0); });
	CHECK_FALSE(rt.streamer().isLoaded(2, 0));
	CHECK(rt.streamer().isLoaded(0, 0));
	// …and back, into cell 3 (absolute 350).
	tf(net.hostWorld, hers).position.x = -150.0f;
	pump(rt, net.hostWorld, 0.016f, [&] { return rt.streamer().isLoaded(3, 0); });
	CHECK(rt.streamer().isLoaded(3, 0));

	// The client: even with her character on its roster (which a client's roster
	// does not carry today), it is not the authority and names nobody.
	{
		const auto* mineOnClient = net.client.roster().find(anna);
		REQUIRE(mineOnClient != nullptr);
		const std::uint32_t netId = net.hostWorld.registry().get<NetworkComponent>(hers).netId;
		REQUIRE((net.client.replication()->entityOf(netId) != entt::null));
		const_cast<HE::Net::Game::PlayerInfo*>(mineOnClient)->characterNetId = netId;
		players.clear();
		HE::sessionPlayers(&net.client, net.clientWorld, players);
		CHECK(players.empty());
	}

	// Anna leaves: her roster entry goes, and with it the anchor; her cell unloads,
	// the host's own stays.
	net.client.leave();
	for (int i = 0; i < 10; ++i) net.step();
	CHECK(net.host.roster().size() == 1u);
	players.clear();
	HE::sessionPlayers(&net.host, net.hostWorld, players);
	REQUIRE(players.size() == 1u);
	CHECK(players[0].id == HE::Net::Game::kHostPlayer);
	pump(rt, net.hostWorld, 0.016f, [&] { return !rt.streamer().isLoaded(3, 0); });
	CHECK_FALSE(rt.streamer().isLoaded(3, 0));
	CHECK(rt.streamer().isLoaded(0, 0));
	CHECK(rt.streamer().stats().anchors == 1u);

	// The session over: no players at all.
	net.host.leave();
	players.clear();
	HE::sessionPlayers(&net.host, net.hostWorld, players);
	CHECK(players.empty());
	(void)mine;
	rt.clear(net.hostWorld);
	he_test::removeAllQuiet(root);
}
