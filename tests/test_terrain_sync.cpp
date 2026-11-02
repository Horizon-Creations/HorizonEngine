#include "doctest.h"

#include "../src/HE_Editor/CollabController.h"
#include "../src/HE_Editor/TerrainSync.h"

#include <HorizonScene/FoliagePaint.h>
#include <HorizonScene/FoliageSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/WaterLake.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <Types/UUID.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

// Landscape sync over a collab session (TerrainSync.h). The machines below are
// real worlds with real landscapes; only the network is a stand-in, one that
// keeps the one property the sync relies on: the host applies what reaches it
// in order and relays every patch, in that order, to every client — the sender
// included. The wire itself is covered in test_net_collab.cpp.

using HE::Ed::TerrainSync;
using Patch = TerrainSync::Patch;

namespace {

struct Machine
{
	HorizonWorld world;
	Entity       terrain = entt::null;
	TerrainSync  sync;
	int          id = 0;

	TerrainComponent& tc() { return world.registry().get<TerrainComponent>(terrain); }
};

// Every machine starts from the same landscape, as a session does (snapshot).
std::unique_ptr<Machine> makeMachine(int id, bool sculpted = true, bool foliage = false)
{
	auto m = std::make_unique<Machine>();
	m->id = id;
	m->terrain = m->world.createEntity("Landscape");
	TerrainComponent tc;
	tc.sizeX = tc.sizeZ = 64.0f;
	tc.resolution = 65;   // already on the 2ⁿ+1 ladder
	if (sculpted) tc.sculptHeights.assign(65u * 65u, 0.0f);
	tc.dirty = false;
	tc.builtRes = 65;
	m->world.registry().emplace<TerrainComponent>(m->terrain, std::move(tc));
	if (foliage)
	{
		FoliageComponent fol;
		fol.layers[0].maskRes = 32;
		fol.dirty = false;
		fol.layers[0].dirty = false;
		m->world.registry().emplace<FoliageComponent>(m->terrain, std::move(fol));
	}
	return m;
}

struct Wire
{
	int   from = 0;
	Patch patch;
};

// The host is machines[0].
struct Net
{
	std::vector<Machine*>          machines;
	std::deque<Wire>               toHost;
	std::vector<std::deque<Wire>>  toClient;
	std::uint64_t                  nowMs = 1000;
	std::vector<Patch>             sentBy[8];

	explicit Net(std::vector<Machine*> m) : machines(std::move(m)), toClient(machines.size()) {}

	void collect(Machine& x)
	{
		const bool host = &x == machines[0];
		x.sync.setExpectEcho(!host);
		x.sync.collect(x.world, nowMs, [&](Entity, const Patch& p) {
			sentBy[x.id].push_back(p);
			if (host)
				for (std::size_t i = 1; i < machines.size(); ++i) toClient[i].push_back({ x.id, p });
			else
				toHost.push_back({ x.id, p });
			return true;
		});
	}
	void collectAll()
	{
		for (Machine* m : machines) collect(*m);
	}
	// The host first (in arrival order), then each client's inbox.
	void deliver()
	{
		while (!toHost.empty() || anyClientMail())
		{
			while (!toHost.empty())
			{
				Wire w = std::move(toHost.front());
				toHost.pop_front();
				machines[0]->sync.applyRemote(machines[0]->world, machines[0]->terrain, w.patch);
				for (std::size_t i = 1; i < machines.size(); ++i) toClient[i].push_back(w);
			}
			for (std::size_t i = 1; i < machines.size(); ++i)
			{
				Machine& c = *machines[i];
				while (!toClient[i].empty())
				{
					Wire w = std::move(toClient[i].front());
					toClient[i].pop_front();
					if (w.from == c.id) c.sync.acknowledge(w.patch.seq);
					else c.sync.applyRemote(c.world, c.terrain, w.patch);
				}
			}
		}
	}
	bool anyClientMail() const
	{
		for (const auto& q : toClient) if (!q.empty()) return true;
		return false;
	}
	void tick(std::uint64_t ms = 250) { nowMs += ms; }
	void clearSent() { for (auto& s : sentBy) s.clear(); }
};

} // namespace

TEST_CASE("Landscape sync: a sculpt dab reaches the peer as one small rectangle")
{
	auto host = makeMachine(0), client = makeMachine(1);
	Net net({ host.get(), client.get() });
	net.collectAll();   // first sight: the shared state, nothing sent
	CHECK(net.sentBy[0].empty());
	CHECK(net.sentBy[1].empty());

	net.tick();
	TerrainSculpt::apply(host->tc(), 10.0f, -8.0f, TerrainSculpt::Op::Raise, 3.0f, 1.0f, 2.5f);
	net.collect(*host);
	REQUIRE(net.sentBy[0].size() == 1);
	const Patch& p = net.sentBy[0][0];
	CHECK(p.field == TerrainSync::Heights);   // a plain patch, not a whole grid
	CHECK(p.res == 65);
	CHECK(p.w < 16);
	CHECK(p.h < 16);
	CHECK(p.bytes.size() == static_cast<std::size_t>(p.w) * p.h * sizeof(float));

	net.deliver();
	CHECK(client->tc().sculptHeights == host->tc().sculptHeights);
	CHECK(client->tc().regionDirty);   // only the chunks under it rebuild
	CHECK_FALSE(client->tc().dirty);

	// The client's copy changed, but not by its own hand: nothing goes back.
	net.tick();
	net.collect(*client);
	CHECK(net.sentBy[1].empty());
}

TEST_CASE("Landscape sync: a first stroke on an unsculpted landscape sends the grid whole")
{
	auto host = makeMachine(0, /*sculpted=*/false), client = makeMachine(1, false);
	Net net({ host.get(), client.get() });
	net.collectAll();

	net.tick();
	TerrainSculpt::apply(client->tc(), 0.0f, 0.0f, TerrainSculpt::Op::Raise, 4.0f, 2.0f, 1.0f);
	REQUIRE(client->tc().sculptHeights.size() == 65u * 65u);
	net.collect(*client);
	REQUIRE_FALSE(net.sentBy[1].empty());
	CHECK((net.sentBy[1][0].field & TerrainSync::kWholeGrid) != 0);
	CHECK(net.sentBy[1][0].x0 == 0);
	CHECK(net.sentBy[1][0].w == 65);

	net.deliver();
	CHECK(host->tc().sculptHeights == client->tc().sculptHeights);
	CHECK(client->sync.pendingCount() == 0);   // the echo came back
}

TEST_CASE("Landscape sync: layer paint and the foliage mask travel the same way")
{
	auto host = makeMachine(0, true, /*foliage=*/true), client = makeMachine(1, true, true);
	Net net({ host.get(), client.get() });
	net.collectAll();

	net.tick();
	TerrainPaint::ensureWeightmap(host->tc());
	TerrainPaint::paint(host->tc(), 5.0f, 5.0f, /*layer*/2, 6.0f, 2.0f, 1.0f);
	TerrainPaint::paint(host->tc(), -9.0f, 4.0f, /*layer*/5, 4.0f, 1.0f, 1.0f);   // second page
	TerrainPaint::paint(host->tc(), 12.0f, -8.0f, /*layer*/9, 4.0f, 1.0f, 1.0f);  // third page
	auto& fol = host->world.registry().get<FoliageComponent>(host->terrain);
	FoliagePaint::paint(fol.layers[0], host->tc(), 0.0f, 0.0f, 8.0f, 2.0f, 1.0f, 0.0f);
	net.collect(*host);
	net.deliver();

	const auto& got = client->world.registry().get<FoliageComponent>(client->terrain);
	CHECK(client->tc().layerWeights == host->tc().layerWeights);
	CHECK(client->tc().layerWeights2 == host->tc().layerWeights2);
	REQUIRE(host->tc().layerWeights3.size() == host->tc().layerWeights.size());
	CHECK(client->tc().layerWeights3 == host->tc().layerWeights3);
	CHECK(client->tc().weightRes == host->tc().weightRes);
	CHECK(client->tc().weightsDirty);
	CHECK(got.layers[0].densityMask == fol.layers[0].densityMask);
	CHECK(got.layers[0].dirty);

	// A later dab goes as a rectangle of the weightmap, not the whole page again.
	// After the pause the whole 1 MB page bought (1 MB / kBytesPerMs = 256 ms).
	net.tick(300);
	net.clearSent();
	TerrainPaint::paint(host->tc(), 20.0f, -20.0f, 1, 2.0f, 1.0f, 1.0f);
	net.collect(*host);
	REQUIRE(net.sentBy[0].size() == 1);
	CHECK(net.sentBy[0][0].field == TerrainSync::Weights);
	CHECK(net.sentBy[0][0].w < host->tc().weightRes / 4);
	net.deliver();
	CHECK(client->tc().layerWeights == host->tc().layerWeights);
}

TEST_CASE("Landscape sync: three people sculpting the same spot end with the same ground")
{
	auto host = makeMachine(0), a = makeMachine(1), b = makeMachine(2);
	Net net({ host.get(), a.get(), b.get() });
	net.collectAll();

	// All three press at once, overlapping, before anybody hears of the others.
	for (int round = 0; round < 3; ++round)
	{
		net.tick(40);
		TerrainSculpt::apply(host->tc(), 1.0f, 0.0f, TerrainSculpt::Op::Set, 5.0f, 2.0f, 3.0f + round);
		TerrainSculpt::apply(a->tc(),    0.0f, 1.0f, TerrainSculpt::Op::Set, 5.0f, 2.0f, 7.0f + round);
		TerrainSculpt::apply(b->tc(),   -1.0f, 0.0f, TerrainSculpt::Op::Set, 5.0f, 2.0f, -4.0f - round);
		net.collectAll();
		if (round == 1) net.deliver();   // some of it crosses mid-stroke
	}
	net.deliver();

	CHECK(a->tc().sculptHeights == host->tc().sculptHeights);
	CHECK(b->tc().sculptHeights == host->tc().sculptHeights);
	CHECK(a->sync.pendingCount() == 0);
	CHECK(b->sync.pendingCount() == 0);

	// And nobody has anything left to say about it.
	net.tick();
	net.clearSent();
	net.collectAll();
	CHECK(net.sentBy[0].empty());
	CHECK(net.sentBy[1].empty());
	CHECK(net.sentBy[2].empty());
}

TEST_CASE("Landscape sync: settings travel without the grids and keep the built chunks")
{
	auto host = makeMachine(0), client = makeMachine(1);
	TerrainSculpt::apply(host->tc(), 0.0f, 0.0f, TerrainSculpt::Op::Raise, 6.0f, 2.0f, 4.0f);
	client->tc().sculptHeights = host->tc().sculptHeights;
	Net net({ host.get(), client.get() });
	net.collectAll();

	// Water's shore setting changes nothing the chunks are built from.
	net.tick();
	client->tc().water.shoreOvershoot = 0.75f;
	net.collect(*client);
	REQUIRE(net.sentBy[1].size() == 1);
	CHECK(net.sentBy[1][0].field == TerrainSync::Settings);
	CHECK(net.sentBy[1][0].bytes.size() < 4096);   // no base64 heights in it
	const std::vector<float> heightsBefore = host->tc().sculptHeights;
	net.deliver();
	CHECK(host->tc().water.shoreOvershoot == doctest::Approx(0.75f));
	CHECK(host->tc().sculptHeights == heightsBefore);   // kept, not flattened
	CHECK_FALSE(host->tc().dirty);
	CHECK(host->tc().builtRes == 65);

	// A height scale does: every chunk again.
	net.tick();
	client->tc().heightScale = 42.0f;
	net.collect(*client);
	net.deliver();
	CHECK(host->tc().heightScale == doctest::Approx(42.0f));
	CHECK(host->tc().dirty);
	CHECK(host->tc().sculptHeights == heightsBefore);

	// The host now holds what the client has: it sends nothing back.
	net.tick();
	net.clearSent();
	net.collect(*host);
	CHECK(net.sentBy[0].empty());
}

// ── Water: a lake's own dig bed (Thema 188 Schritt 2) ────────────────────────

TEST_CASE("Landscape sync: a settings patch on raised ground does not move the ground reference")
{
	auto host = makeMachine(0), client = makeMachine(1);
	for (float& h : host->tc().sculptHeights) h = 10.0f;
	client->tc().sculptHeights = host->tc().sculptHeights;
	Net net({ host.get(), client.get() });
	net.collectAll();

	net.tick();
	host->tc().water.shoreOvershoot = 0.75f;   // the only thing that forces a Settings patch
	net.collect(*host);
	REQUIRE(net.sentBy[0].size() == 1);
	CHECK(net.sentBy[0][0].field == TerrainSync::Settings);
	net.deliver();
	REQUIRE(client->tc().water.shoreOvershoot == doctest::Approx(0.75f));

	const std::vector<glm::vec2> poly = { { -10, -10 }, { 10, -10 }, { 10, 10 }, { -10, 10 } };
	CHECK(terrainHeightAt(client->tc(), 0.0f, 0.0f) == doctest::Approx(10.0f));
	CHECK(HE::water::lake::lowestGround(client->tc(), poly) == doctest::Approx(10.0f));
	CHECK(HE::water::lake::lowestGround(client->tc(), poly) ==
	      doctest::Approx(HE::water::lake::lowestGround(host->tc(), poly)));
}

TEST_CASE("Landscape sync: a client's own syncSplines must not re-dig a lake's bed from a stale patch")
{
	// Both machines run WaterLake::syncSplines on their own TerrainSystem tick
	// (TerrainSystem.cpp:535), unguarded by session or ownership — a spline move
	// that arrives from a peer looks exactly like one the local user made. For a
	// lake that digs its own bed (Body::digsBed) that means BOTH machines redig
	// the ground locally, each lifting its OWN recorded groundPatch first. If the
	// client's groundPatch is still the OLD one (Settings travels at 200 ms,
	// slower than the Heights that already carried the host's fresh dig at
	// 33 ms), the client's lift does not cancel what the host already dug, and
	// the fresh dig on top does not cover the leftover: a residual bite that
	// compounds on the next move.
	auto host = makeMachine(0), client = makeMachine(1);
	Net net({ host.get(), client.get() });
	net.collectAll();

	net.tick();
	const HE::UUID spline = HE::UUID::generate();
	const std::vector<glm::vec2> poly = { { -10, -10 }, { 10, -10 }, { 10, 10 }, { -10, 10 } };
	const uint16_t id = HE::water::lake::create(host->tc(), spline, poly, 0.0f);
	REQUIRE(id != HE::water::kNoBody);
	REQUIRE(HE::water::lake::dig(host->tc(), id, 2.0f, 4.0f).ok);
	net.collect(*host);
	net.deliver();
	REQUIRE(client->tc().water.findBody(id) != nullptr);
	REQUIRE(client->tc().sculptHeights == host->tc().sculptHeights);

	// Let the settings timer start fresh right before the move, so the window
	// between a 33 ms Heights send and the next allowed Settings send is real.
	net.tick(250);
	net.collectAll();
	net.deliver();

	// The oracle: one machine, one `follow` call, the answer both host and
	// client must converge to.
	TerrainComponent oracle = host->tc();
	// Shifted +12 m in x: the western part of the old outline and its bank
	// (as far out as x = -14) is now outside the new outline and its bank
	// (which starts at x = -2), so a correct lift must bring it all back to
	// plain ground.
	const std::vector<glm::vec2> moved = { { 2, -10 }, { 22, -10 }, { 22, 10 }, { 2, 10 } };
	REQUIRE(HE::water::lake::follow(oracle, id, moved).ok);

	// The host: the user actually dragged the point. This is the one true edit.
	REQUIRE(HE::water::lake::follow(host->tc(), id, moved).ok);
	net.tick(40);
	net.collect(*host);
	REQUIRE(std::any_of(net.sentBy[0].begin(), net.sentBy[0].end(),
	                    [](const Patch& p) { return (p.field & ~TerrainSync::kWholeGrid) == TerrainSync::Heights; }));
	net.deliver();

	// The client: its own syncSplines, reacting to the same spline move as if it
	// had arrived over the (separate, untested-here) transform channel — exactly
	// what TerrainSystem.cpp:535 does every tick, host or client, unconditionally.
	REQUIRE(HE::water::lake::follow(client->tc(), id, moved).ok);

	net.collectAll();
	net.deliver();
	net.tick(250);
	net.collectAll();
	net.deliver();

	CHECK(host->tc().sculptHeights == oracle.sculptHeights);
	CHECK(client->tc().sculptHeights == oracle.sculptHeights);
	// The old bed's floor, outside the new outline's reach, must be back at the
	// plain ground (0) on both — not a leftover bite from a stale lift.
	CHECK(terrainHeightAt(oracle, -6.0f, 0.0f) == doctest::Approx(0.0f));   // sanity: the oracle itself
	CHECK(terrainHeightAt(host->tc(), -6.0f, 0.0f) == doctest::Approx(0.0f));
	CHECK(terrainHeightAt(client->tc(), -6.0f, 0.0f) == doctest::Approx(0.0f));
}

TEST_CASE("Landscape sync: a lake's dig bed survives a landscape edit elsewhere")
{
	auto host = makeMachine(0), client = makeMachine(1);
	Net net({ host.get(), client.get() });
	net.collectAll();   // first sight: the shared blank state

	net.tick();
	const HE::UUID spline = HE::UUID::generate();
	const std::vector<glm::vec2> poly = { { -10, -10 }, { 10, -10 }, { 10, 10 }, { -10, 10 } };
	const uint16_t id = HE::water::lake::create(host->tc(), spline, poly, 0.0f);
	REQUIRE(id != HE::water::kNoBody);
	REQUIRE(HE::water::lake::dig(host->tc(), id, 2.0f, 4.0f).ok);

	const HE::water::Body* dug = host->tc().water.findBody(id);
	REQUIRE(dug != nullptr);
	REQUIRE(dug->digsBed);
	REQUIRE(dug->dugDepth == doctest::Approx(2.0f));
	REQUIRE(dug->dugBank == doctest::Approx(4.0f));
	REQUIRE_FALSE(dug->groundPatch.empty());
	const float dugDepth = dug->dugDepth, dugBank = dug->dugBank;

	net.collect(*host);
	net.deliver();

	// The client now holds the same lake, with the same dug bed.
	const HE::water::Body* onClient = client->tc().water.findBody(id);
	REQUIRE(onClient != nullptr);
	CHECK(onClient->digsBed);
	CHECK(onClient->dugDepth == doctest::Approx(dugDepth));
	CHECK(onClient->dugBank == doctest::Approx(dugBank));
	CHECK_FALSE(onClient->groundPatch.empty());
	CHECK(client->tc().sculptHeights == host->tc().sculptHeights);

	// The host edits the landscape somewhere else, far from the lake.
	net.tick();
	TerrainSculpt::apply(host->tc(), 25.0f, 25.0f, TerrainSculpt::Op::Raise, 3.0f, 1.0f, 5.0f);
	net.collect(*host);
	net.deliver();

	// The lake's own dig bed must still be there on the client, at the depth
	// and bank it was dug with — not the normal zero a fresh body starts at —
	// and the excavated ground must still match the host's.
	const HE::water::Body* afterEdit = client->tc().water.findBody(id);
	REQUIRE(afterEdit != nullptr);
	CHECK(afterEdit->digsBed);
	CHECK(afterEdit->dugDepth == doctest::Approx(dugDepth));
	CHECK(afterEdit->dugBank == doctest::Approx(dugBank));
	CHECK_FALSE(afterEdit->groundPatch.empty());
	CHECK(client->tc().sculptHeights == host->tc().sculptHeights);
}

TEST_CASE("Landscape sync: a patch that does not fit the grid changes nothing")
{
	auto m = makeMachine(1);
	TerrainSync sync;
	sync.collect(m->world, 0, [](Entity, const Patch&) { return true; });
	const std::vector<float> before = m->tc().sculptHeights;

	Patch p;
	p.field = TerrainSync::Heights;
	p.res = 65;
	p.x0 = 60; p.z0 = 0; p.w = 10; p.h = 1;   // runs off the right edge
	p.bytes.assign(10 * sizeof(float), 0x41);
	CHECK_FALSE(sync.applyRemote(m->world, m->terrain, p));

	p.x0 = 0; p.w = 10;
	p.bytes.resize(9 * sizeof(float));         // one element short
	CHECK_FALSE(sync.applyRemote(m->world, m->terrain, p));

	p.bytes.assign(10 * sizeof(float), 0x41);
	p.res = 129;                               // another grid, and not a whole one
	CHECK_FALSE(sync.applyRemote(m->world, m->terrain, p));

	p.field = 99;
	p.res = 65;
	CHECK_FALSE(sync.applyRemote(m->world, m->terrain, p));
	CHECK(m->tc().sculptHeights == before);

	// The first band of a whole grid is allowed to resize it.
	p.field = TerrainSync::Heights | TerrainSync::kWholeGrid;
	p.res = 129;
	CHECK(sync.applyRemote(m->world, m->terrain, p));
	CHECK(m->tc().resolution == 129);
	CHECK(m->tc().sculptHeights.size() == 129u * 129u);

	// And res 0 clears it.
	Patch clear;
	clear.field = TerrainSync::Heights | TerrainSync::kWholeGrid;
	CHECK(sync.applyRemote(m->world, m->terrain, clear));
	CHECK(m->tc().sculptHeights.empty());
}

TEST_CASE("Landscape sync: patches are paced, and nothing changed in the pause is lost")
{
	auto host = makeMachine(0), client = makeMachine(1);
	Net net({ host.get(), client.get() });
	net.collectAll();

	net.tick();
	for (float& h : host->tc().sculptHeights) h = 1.0f;   // the whole landscape at once
	net.collect(*host);
	REQUIRE_FALSE(net.sentBy[0].empty());

	net.clearSent();
	host->tc().sculptHeights[100] = 7.0f;
	net.tick(TerrainSync::kMinIntervalMs - 1);
	net.collect(*host);
	CHECK(net.sentBy[0].empty());              // not yet
	net.tick(1);
	net.collect(*host);
	CHECK(net.sentBy[0].size() == 1);          // nothing was lost meanwhile
	net.deliver();
	CHECK(client->tc().sculptHeights == host->tc().sculptHeights);
}

TEST_CASE("Landscape sync: the component blob leaves the grids out, and the receiver keeps its own")
{
	auto m = makeMachine(1, true, true);
	TerrainSculpt::apply(m->tc(), 0.0f, 0.0f, TerrainSculpt::Op::Raise, 6.0f, 2.0f, 4.0f);
	TerrainPaint::ensureWeightmap(m->tc());
	const std::vector<float> heights = m->tc().sculptHeights;

	const std::vector<std::uint8_t> blob = TerrainSync::serializeWithoutGrids(m->world, m->terrain);
	const nlohmann::json j = nlohmann::json::from_cbor(blob);
	REQUIRE(j.contains("terrain"));
	CHECK_FALSE(j["terrain"].contains("sculptHeightsB64"));
	CHECK_FALSE(j["terrain"].contains("layerWeightsB64"));
	CHECK(j.contains("foliage"));
	// Set aside, not taken away.
	CHECK(m->tc().sculptHeights == heights);
	CHECK_FALSE(m->tc().layerWeights.empty());

	// A peer's whole-entity blob: same settings, no grids.
	auto peer = makeMachine(2, true, true);
	peer->tc().sculptHeights = heights;
	peer->tc().layerWeights = m->tc().layerWeights;
	peer->tc().weightRes = m->tc().weightRes;
	peer->tc().weightsDirty = false;
	REQUIRE(TerrainSync::replaceKeepingGrids(peer->world, peer->terrain, blob));
	CHECK(peer->tc().sculptHeights == heights);
	CHECK(peer->tc().layerWeights == m->tc().layerWeights);
	CHECK_FALSE(peer->tc().dirty);
	CHECK(peer->tc().builtRes == 65);
	CHECK(peer->world.registry().all_of<FoliageComponent>(peer->terrain));

	// A full blob (an older sender, a session undo) still brings its grids.
	const std::vector<std::uint8_t> full = SceneSerializer().serializeEntityComponents(m->world, m->terrain);
	peer->tc().sculptHeights.assign(65u * 65u, 9.0f);
	REQUIRE(TerrainSync::replaceKeepingGrids(peer->world, peer->terrain, full));
	CHECK(peer->tc().sculptHeights == heights);
	CHECK(peer->tc().dirty);
}

TEST_CASE("Landscape sync: what could not be sent goes again, it is not taken as delivered")
{
	auto m = makeMachine(1);
	TerrainSync sync;
	sync.setExpectEcho(true);
	std::uint64_t now = 1000;
	sync.collect(m->world, now, [](Entity, const Patch&) { return true; });   // first sight

	TerrainSculpt::apply(m->tc(), 3.0f, 3.0f, TerrainSculpt::Op::Raise, 3.0f, 1.0f, 1.0f);
	m->tc().water.shoreOvershoot = 0.5f;   // a settings change as well
	int refused = 0;
	now += 300;
	sync.collect(m->world, now, [&](Entity, const Patch&) { ++refused; return false; });
	CHECK(refused == 2);                   // settings + heights tried
	CHECK(sync.pendingCount() == 0);       // nothing waits for an echo that cannot come

	std::vector<Patch> sent;
	now += 300;
	sync.collect(m->world, now, [&](Entity, const Patch& p) { sent.push_back(p); return true; });
	REQUIRE(sent.size() == 2);
	CHECK(sent[0].field == TerrainSync::Settings);
	CHECK(sent[1].field == TerrainSync::Heights);
	CHECK(sync.pendingCount() == 2);
}

// ── Through two real CollabControllers ──────────────────────────────────────
// The editor's own bridge over actual TCP: the snapshot hands the landscape over,
// the wire id comes from its uuid on both sides, and the client's echo comes
// back from a real host. Wired the way EditorApplication wires it.
TEST_CASE("Landscape sync: a client's stroke reaches the host through the real session")
{
	auto hostM = makeMachine(0);
	CollabController host;
	host.setWorld(&hostM->world);
	REQUIRE(host.startHosting(0, "Anna"));

	HorizonWorld clientWorld;
	CollabController client;
	client.setWorld(&clientWorld);
	TerrainSync clientSync;
	client.onWorldReplaced([&] { client.seedNetIds(); clientSync.reset(); });

	const auto wire = [](CollabController& c, HorizonWorld& w, TerrainSync& s) {
		c.onRemoteTerrainPatch([&c, &w, &s](std::uint32_t handle, const Patch& p, bool own) {
			if (own) { s.acknowledge(p.seq); return; }
			if (handle != 0)
				s.applyRemote(w, static_cast<Entity>(static_cast<entt::id_type>(handle)), p);
			(void)c;
		});
	};
	wire(host, hostM->world, hostM->sync);
	wire(client, clientWorld, clientSync);

	std::uint64_t now = 1000;
	const auto pump = [&](auto done) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			host.update(now);
			client.update(now);
			if (done()) return true;
			now += 16;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return done();
	};
	const auto collect = [&](CollabController& c, HorizonWorld& w, TerrainSync& s, bool isHost) {
		s.setExpectEcho(!isHost);
		s.collect(w, now, [&](Entity e, const Patch& p) {
			return c.publishTerrainPatch(static_cast<std::uint32_t>(entt::to_integral(e)), p);
		});
	};

	REQUIRE(client.joinSession("127.0.0.1", host.port(), host.joinCode(), "Bob"));
	REQUIRE(pump([&] { return client.status() == CollabController::Status::Joined; }));

	auto view = clientWorld.registry().view<TerrainComponent>();
	REQUIRE_FALSE(view.empty());
	const Entity clientTerrain = view.front();
	auto& ctc = clientWorld.registry().get<TerrainComponent>(clientTerrain);
	REQUIRE(ctc.sculptHeights == hostM->tc().sculptHeights);   // the snapshot

	collect(client, clientWorld, clientSync, false);   // first sight: nothing sent
	collect(host, hostM->world, hostM->sync, true);
	CHECK(clientSync.pendingCount() == 0);

	now += 100;
	TerrainSculpt::apply(ctc, -12.0f, 6.0f, TerrainSculpt::Op::Raise, 4.0f, 2.0f, 3.0f);
	collect(client, clientWorld, clientSync, false);
	CHECK(clientSync.pendingCount() == 1);

	REQUIRE(pump([&] {
		return hostM->tc().sculptHeights == ctc.sculptHeights && clientSync.pendingCount() == 0;
	}));
	CHECK(hostM->tc().regionDirty);

	// And back the other way: the host paints, the client gets it as a peer's.
	now += 100;
	TerrainSculpt::apply(hostM->tc(), 20.0f, -20.0f, TerrainSculpt::Op::Lower, 3.0f, 1.0f, 2.0f);
	collect(host, hostM->world, hostM->sync, true);
	REQUIRE(pump([&] { return hostM->tc().sculptHeights == ctc.sculptHeights; }));
}

// ── Foliage layers (Thema 182): one mask grid per layer ─────────────────────

namespace {
// A second foliage layer on a machine, the same on every machine (as a snapshot
// would have it): no mask yet, nothing to scatter.
FoliageComponent& addSecondLayer(Machine& m)
{
	auto& fol = m.world.registry().get<FoliageComponent>(m.terrain);
	fol.addLayer();
	fol.layers[1].maskRes = 32;
	fol.layers[1].dirty   = false;
	return fol;
}

std::vector<std::uint8_t> fieldsSent(const std::vector<Patch>& sent)
{
	std::vector<std::uint8_t> f;
	for (const Patch& p : sent) f.push_back(static_cast<std::uint8_t>(p.field & ~TerrainSync::kWholeGrid));
	return f;
}
} // namespace

TEST_CASE("Landscape sync: every foliage layer's mask travels on a field of its own")
{
	auto host = makeMachine(0, true, /*foliage=*/true), client = makeMachine(1, true, true);
	addSecondLayer(*host);
	addSecondLayer(*client);
	Net net({ host.get(), client.get() });
	net.collectAll();

	net.tick();
	net.clearSent();
	auto& fol = host->world.registry().get<FoliageComponent>(host->terrain);
	FoliagePaint::paint(fol.layers[1], host->tc(), 4.0f, -6.0f, 8.0f, 2.0f, 1.0f, 0.0f);
	net.collect(*host);
	const std::vector<std::uint8_t> fields = fieldsSent(net.sentBy[0]);
	CHECK(std::count(fields.begin(), fields.end(),
	                 static_cast<std::uint8_t>(TerrainSync::FoliageMask + 1)) >= 1);
	CHECK(std::count(fields.begin(), fields.end(), static_cast<std::uint8_t>(TerrainSync::FoliageMask)) == 0);
	net.deliver();

	const auto& got = client->world.registry().get<FoliageComponent>(client->terrain);
	REQUIRE(got.layers.size() == 2u);
	CHECK(got.layers[1].densityMask == fol.layers[1].densityMask);
	CHECK(got.layers[1].dirty);
	CHECK(got.layers[0].densityMask.empty());   // the other layer saw nothing
	CHECK_FALSE(got.layers[0].dirty);
}

TEST_CASE("Landscape sync: a foliage layer added, painted and removed ends the same on both sides")
{
	auto host = makeMachine(0, true, /*foliage=*/true), client = makeMachine(1, true, true);
	Net net({ host.get(), client.get() });
	net.collectAll();
	auto& hf = host->world.registry().get<FoliageComponent>(host->terrain);
	auto& cf = client->world.registry().get<FoliageComponent>(client->terrain);

	// The host adds a layer and paints it: the settings bring the layer, then its
	// mask arrives whole on the new field.
	net.tick();
	FoliageLayer* trees = hf.addLayer();
	REQUIRE(trees);
	trees->name        = "Trees";
	trees->meshAssetId = HE::UUID{ 0x71, 0x72 };
	trees->maskRes     = 32;
	FoliagePaint::paint(*trees, host->tc(), -8.0f, 8.0f, 10.0f, 2.0f, 1.0f, 0.0f);
	net.collect(*host);
	net.deliver();
	REQUIRE(cf.layers.size() == 2u);
	CHECK(cf.layers[1].name == "Trees");
	CHECK(cf.layers[1].meshAssetId == HE::UUID{ 0x71, 0x72 });
	CHECK(cf.layers[1].densityMask == hf.layers[1].densityMask);

	// The first layer goes: what was layer 1 is layer 0 now, mask and all.
	net.tick();
	hf.layers.erase(hf.layers.begin());
	net.collect(*host);
	net.deliver();
	REQUIRE(cf.layers.size() == 1u);
	CHECK(cf.layers[0].name == "Trees");
	CHECK(cf.layers[0].densityMask == hf.layers[0].densityMask);

	// Settled: neither side has anything left to send.
	net.tick();
	net.clearSent();
	net.collectAll();
	CHECK(net.sentBy[0].empty());
	CHECK(net.sentBy[1].empty());

	// A layer appearing again at index 1 sends its mask whole, not against the
	// mask a removed layer once had there.
	net.tick();
	FoliageLayer* bushes = hf.addLayer();
	REQUIRE(bushes);
	bushes->maskRes = 32;
	FoliagePaint::paint(*bushes, host->tc(), 0.0f, 0.0f, 4.0f, 1.0f, 1.0f, 0.0f);
	net.collect(*host);
	net.deliver();
	REQUIRE(cf.layers.size() == 2u);
	CHECK(cf.layers[1].densityMask == hf.layers[1].densityMask);
}

TEST_CASE("Landscape sync: a peer's change to how a layer is drawn sticks, and the scatter stays")
{
	auto host = makeMachine(0, true, /*foliage=*/true), client = makeMachine(1, true, true);
	for (Machine* m : { host.get(), client.get() })
	{
		auto& f = m->world.registry().get<FoliageComponent>(m->terrain);
		f.layers[0].meshAssetId = HE::UUID{ 0x61, 0x62 };
		f.layers[0].density     = 0.05f;
		f.dirty = true;
		FoliageSystem::update(m->world);
		REQUIRE_FALSE(f.layers[0].cachedInstances.empty());
	}
	Net net({ host.get(), client.get() });
	net.collectAll();
	auto& cf = client->world.registry().get<FoliageComponent>(client->terrain);
	const auto storeBefore = cf.layers[0].store;
	const size_t countBefore = cf.layers[0].cachedInstances.size();

	// Not a scatter input: the receiver takes the new value and keeps its scatter.
	net.tick();
	auto& hf = host->world.registry().get<FoliageComponent>(host->terrain);
	hf.layers[0].castsShadow    = false;
	hf.layers[0].shadowDistance = 30.0f;
	net.collect(*host);
	net.deliver();
	CHECK_FALSE(cf.layers[0].castsShadow);
	CHECK(cf.layers[0].shadowDistance == doctest::Approx(30.0f));
	CHECK(cf.layers[0].store == storeBefore);
	CHECK(cf.layers[0].cachedInstances.size() == countBefore);
	CHECK_FALSE(cf.layers[0].dirty);

	// A scatter input: the receiver scatters again.
	net.tick();
	hf.layers[0].seed = 1234;
	net.collect(*host);
	net.deliver();
	CHECK(cf.layers[0].seed == 1234);
	CHECK(cf.layers[0].dirty);

	// A placement rule (Thema 180) decides which candidates stay: it is a scatter input
	// too, or the receiver would keep a scatter made without the rule.
	FoliageSystem::update(client->world);
	REQUIRE_FALSE(cf.layers[0].dirty);
	net.tick();
	hf.layers[0].slopeMin = 0.2f;
	hf.layers[0].heightMax = 5.0f;
	hf.layers[0].weightLayer = 2;
	net.collect(*host);
	net.deliver();
	CHECK(cf.layers[0].slopeMin == doctest::Approx(0.2f));
	CHECK(cf.layers[0].heightMax == doctest::Approx(5.0f));
	CHECK(cf.layers[0].weightLayer == 2);
	CHECK(cf.layers[0].dirty);
}

TEST_CASE("Landscape sync: a layer that arrives with its mask is not overtaken by the mask")
{
	// An undo of Remove Layer, or an MCP call carrying two layers, brings the layer
	// and its mask in one frame. The mask is looked at every 33 ms, the settings that
	// create the layer on the peer only every 200 ms: the mask must wait for them.
	auto host = makeMachine(0, true, /*foliage=*/true), client = makeMachine(1, true, true);
	Net net({ host.get(), client.get() });
	net.collectAll();
	auto& hf = host->world.registry().get<FoliageComponent>(host->terrain);
	auto& cf = client->world.registry().get<FoliageComponent>(client->terrain);

	net.tick();
	hf.layers[0].drawDistance = 90.0f;   // the settings go now, and are not due again for 200 ms
	net.collect(*host);
	net.deliver();
	REQUIRE(cf.layers[0].drawDistance == doctest::Approx(90.0f));

	net.tick(40);
	FoliageLayer restored;
	restored.maskRes = 32;
	restored.densityMask.assign(32u * 32u, 255);
	restored.densityMask[100] = 0;
	hf.layers.push_back(restored);
	for (int i = 0; i < 8; ++i)   // a few frames, as the editor would run them
	{
		net.collect(*host);
		net.deliver();
		net.tick(40);
	}
	REQUIRE(cf.layers.size() == 2u);
	CHECK(cf.layers[1].densityMask == hf.layers[1].densityMask);
}
