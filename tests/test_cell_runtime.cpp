// HE::CellRuntime against a fake ICellHost (Thema 164, step 2c): the part of cell
// streaming the game and the editor's Play share. What the host is asked, when,
// and with which token; the manifest followed through changes; clear() against
// reset(); the bodies of a cell in and out of a real physics world.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/CellRuntime.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
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
