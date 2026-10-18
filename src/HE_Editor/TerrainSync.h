#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity
#include <Net/CollabSession.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

// ── Landscape sync over a collab session ─────────────────────────────────────
// A landscape keeps its shape, its paint and its foliage mask inline, as grids of
// up to 1025² floats and 512² RGBA bytes (TerrainComponent, FoliageComponent:
// one mask per foliage layer).
// The component blob cannot carry them: it is the whole entity, serialized and
// hashed every frame, megabytes of base64 for one brush dab, and only for the
// entity the user has selected — which the Landscape mode never does.
//
// So the grids travel on their own. Once per frame this compares every
// landscape's grids with a copy of what the peers have (the mirror) and sends
// the rectangle that differs, raw, at most every kMinIntervalMs per grid. It does
// not ask which tool made the change: brush, Mountain, heightmap import, Fill
// Layer, an MCP tool or an undo all look the same from here, and none of them
// has to remember to publish. The settings (both components without their
// grids, water included) go as one blob when their hash moves.
//
// No lock: two people shape one landscape at the same time. What keeps them
// equal is the host's order. The host applies patches as they arrive and echoes
// each to its sender as well. A client keeps what it sent until the echo comes
// back, and after applying a peer's patch it lays those again on top — exactly
// the order the host gave them, since the host relays in order over one stream.
// Overlapping strokes therefore end the same on every machine; the last ≤33 ms
// of a brush that were not sent yet lose to a peer's patch over the same texels.
//
// A grid that changed SIZE (a first stroke on an unsculpted landscape, a new
// weightmap resolution, a cleared sculpt) goes whole, its first band flagged
// kWholeGrid. Only such a band may re-allocate a grid on the receiver; a plain
// patch for a grid of another size is dropped, so a stray stroke from before a
// resize cannot wipe the new grid.
namespace HE::Ed
{
class TerrainSync
{
public:
	using Patch = HE::Net::CollabSession::TerrainPatch;

	// Patch::field. kWholeGrid is or-ed onto the first band of a whole grid.
	enum Field : std::uint8_t
	{
		Heights     = 1,   // TerrainComponent::sculptHeights, float, side = resolution
		Weights     = 2,   // layerWeights,  RGBA8, side = weightRes
		Weights2    = 3,   // layerWeights2, RGBA8, side = weightRes
		Settings    = 5,   // settingsBlob(): both components without their grids
		// FoliageLayer::densityMask of layer i is field FoliageMask + i, 1 byte,
		// side = maskRes. Layers are matched by index; a mask that moved to another
		// index with a removed layer arrives there as an ordinary patch.
		FoliageMask = 16,
	};
	static constexpr std::uint8_t kWholeGrid = 0x80;
	// One mask field per layer, FoliageComponent::kMaxLayers of them (checked in
	// the .cpp); the ids must stay below kWholeGrid.
	static constexpr std::uint8_t kMaxFoliageMasks = 32;
	static constexpr std::uint8_t kFieldCount = FoliageMask + kMaxFoliageMasks;
	static_assert(kFieldCount <= kWholeGrid, "a field id would collide with kWholeGrid");

	static constexpr std::uint64_t kMinIntervalMs      = 33;
	static constexpr std::uint64_t kSettingsIntervalMs = 200;
	static constexpr std::size_t   kMaxPatchBytes      = 1u << 20;   // per band
	// Average a grid may cost: a brush the size of the landscape must not send a
	// 4 MB height field 30 times a second. A big patch buys a longer pause.
	static constexpr std::uint64_t kBytesPerMs = 4096;               // ~4 MB/s
	static constexpr std::size_t   kMaxPending = 256;

	// Returns false if the patch did not go out (it is then not waited for).
	using Send = std::function<bool(Entity, const Patch&)>;

	// Forget every mirror and every unconfirmed patch. The next collect takes
	// each landscape as it then is as the shared state (session start, a
	// snapshot that replaced the world, leaving).
	void reset();

	// A client gets its patches echoed; the host does not (its order is the order).
	void setExpectEcho(bool expect) { m_expectEcho = expect; }

	// Send what changed on every landscape since the last call.
	void collect(HorizonWorld& world, std::uint64_t nowMs, const Send& send);

	// A peer's patch for `e`. Applies it, then our own unconfirmed patches for the
	// same landscape on top. false = dropped (unknown field, wrong size, no such
	// component); nothing changed then.
	bool applyRemote(HorizonWorld& world, Entity e, const Patch& patch);

	// The echo of our patch `seq` arrived: the host has placed it.
	void acknowledge(std::uint32_t seq);

	std::size_t pendingCount() const { return m_pending.size(); }

	// ── The component blob, without the grids ─────────────────────────────
	// What the other collab paths send for an entity: serializeEntityComponents
	// with the landscape grids left out (they cost megabytes and travel here).
	static std::vector<std::uint8_t> serializeWithoutGrids(HorizonWorld& world, Entity e);
	// The receiving half: replaceEntityComponents, but a landscape keeps the grids
	// the blob does not carry, and keeps its built chunks unless its shape changed.
	static bool replaceKeepingGrids(HorizonWorld& world, Entity e,
	                                const std::vector<std::uint8_t>& blob);
	// The Settings payload: only the "terrain" and "foliage" keys, no grids.
	static std::vector<std::uint8_t> settingsBlob(HorizonWorld& world, Entity e);

private:
	struct Mirror
	{
		std::vector<std::uint8_t> grid[kFieldCount];   // indexed by field id (unused ids stay empty)
		std::uint32_t             res[kFieldCount]{};
		std::uint64_t             settingsHash = 0;
		std::uint64_t             nextAt[kFieldCount]{};
		// How many foliage layers the peers know of: the settings blob creates a
		// layer on their side, so a layer's mask must not overtake it (it would be
		// dropped there, and the mirror would think it arrived). A mask of a layer
		// at or past this index waits; a new count sends the settings out of turn.
		std::size_t               foliageLayers = 0;
	};
	struct Pending
	{
		Entity entity;
		Patch  patch;
	};

	void baseline(entt::registry& reg, Entity e, Mirror& m);
	void collectGrid(entt::registry& reg, Entity e, Mirror& m, std::uint8_t f,
	                 std::uint64_t nowMs, const Send& send);
	bool emit(Entity e, Patch& p, const Send& send);
	bool applyOne(HorizonWorld& world, Entity e, const Patch& patch);

	std::unordered_map<Entity, Mirror> m_mirrors;
	std::deque<Pending>                m_pending;
	std::uint32_t                      m_seq = 0;
	bool                               m_expectEcho = false;
};
} // namespace HE::Ed
