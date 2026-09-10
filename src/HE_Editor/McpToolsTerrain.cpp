#include "McpToolRegistry.h"

#include "EditorCommands.h"
#include "McpToolCommon.h"

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// ─── Shaping the ground from outside the editor ──────────────────────────────
// See McpToolRegistry.h for why terrain gets four tools of its own instead of
// riding on entity_get / entity_set_components. In short: the component is two
// base64 blobs, and base64 is not an interface.
//
// ── One coordinate system on this whole interface: WORLD ─────────────────────
// Every x, z and height here is world space, including the numbers that come
// back out of terrain_heightmap. The component stores heights terrain-LOCAL
// (mesh vertices are local to the entity), and the brushes take terrain-local
// XZ — so the conversion happens exactly once per tool, at the edge, against
// HE::worldPositionOf. Never against `tc.worldMatrix`: that field is only as
// fresh as the last propagateTransforms, and a terrain created in this same
// frame answers with the identity. This repository has been bitten by that four
// times (see docs/ and TransformHierarchy.h).
//
// Handing a client local coordinates would have been less code and a worse
// interface: it already knows where things are in the world from
// entity_get.worldPosition and from physics raycasts, and a landscape that
// happens to sit at the origin — as most do — would make the bug invisible
// until the one project where it does not.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── Refusals ─────────────────────────────────────────────────────────────────
// The gateway's vocabulary, so a client branches on the same codes here as it
// does on the entity tools.
ToolResult failFor(CmdError e, const std::string& what)
{
	const std::string code = errorName(e);
	switch (e)
	{
	case CmdError::NoWorld:
		return ToolResult::fail(code, "No scene is open in the editor. Call scene_info "
		                              "first — until a project and a scene are loaded "
		                              "there is no landscape to shape.");
	case CmdError::NotFound:
		return ToolResult::fail(code, "No entity with uuid '" + what + "' in the open "
		                              "scene. Use terrain_info with no argument to see "
		                              "every landscape there is.");
	case CmdError::PlayMode:
		return ToolResult::fail(code, "Play-in-editor is running. Ground shaped now would "
		                              "be thrown away when it stops, so it is refused "
		                              "rather than lost. Ask the user to stop play mode.");
	case CmdError::InvalidPayload:
		return ToolResult::fail(code, what);
	case CmdError::LockedByOther:
		return ToolResult::fail(code, "Another participant in the collaboration session "
		                              "holds '" + what + "' right now. Wait until they "
		                              "let go, or work on something else.");
	case CmdError::LockPending:
		return ToolResult::fail(code, "The lock on '" + what + "' has been requested from "
		                              "the session host and the answer is still in "
		                              "flight. Retry next frame — this is a wait, not a "
		                              "refusal.");
	case CmdError::Builtin:
		return ToolResult::fail(code, "'" + what + "' is a built-in entity. The editor "
		                              "does not let a human edit it either.");
	case CmdError::Failed:
		return ToolResult::fail(code, "The editor refused the terrain edit on '" + what +
		                              "' — the patched component did not come back "
		                              "through the scene loader.");
	case CmdError::None:
		break;
	}
	return ToolResult::fail(code, what);
}

// ── The addressed landscape ──────────────────────────────────────────────────
// Three separate mistakes, three answers: no world, an unknown uuid, and an
// entity that exists but is not a landscape. The last one is the interesting
// one — a client that reached for the wrong entity has to read that rather than
// a "not found" it would answer by re-listing the same entity.
struct Terrain
{
	HorizonWorld*     world = nullptr;
	Entity            entity = entt::null;
	TerrainComponent* tc = nullptr;
	glm::vec3         worldPos{ 0.0f };
	bool              ok = false;
	ToolResult        failure = ToolResult::ok(json::object());
};

Terrain resolveTerrain(EditorCommands& cmds, const json& args)
{
	Terrain t;
	t.world = cmds.world();
	if (!t.world) { t.failure = failFor(CmdError::NoWorld, {}); return t; }

	const std::string uuid = strArg(args, "uuid");
	if (uuid.empty())
	{
		t.failure = failFor(CmdError::InvalidPayload,
		                    "'uuid' is required and must be the uuid of a landscape "
		                    "entity — terrain_info with no argument lists them.");
		return t;
	}
	t.entity = entityByUuid(*t.world, uuid);
	if (t.entity == entt::null) { t.failure = failFor(CmdError::NotFound, uuid); return t; }

	t.tc = t.world->registry().try_get<TerrainComponent>(t.entity);
	if (!t.tc)
	{
		t.failure = failFor(CmdError::InvalidPayload,
		                    "Entity '" + uuid + "' has no terrain component, so it is not "
		                    "a landscape. Create one in the editor's Landscape mode, or "
		                    "with entity_create carrying a 'terrain' component.");
		return t;
	}
	// Composed by walking the parent chain, NOT read off worldMatrix — see the
	// file header.
	t.worldPos = HE::worldPositionOf(*t.world, t.entity);
	t.ok = true;
	return t;
}

std::string nameOf(HorizonWorld& world, Entity e)
{
	const auto* n = world.registry().try_get<NameComponent>(e);
	return n ? n->name : std::string();
}

// The component state of one entity as the scene format's own JSON. Read back
// through the ONE serializer rather than described a second time here: a field
// added to the terrain component appears in this patch the moment it appears in
// a saved scene, and never one release later.
json componentsOf(HorizonWorld& world, Entity e)
{
	SceneSerializer ser;
	const std::vector<std::uint8_t> cbor = ser.serializeEntityComponents(world, e);
	json j = json::from_cbor(cbor, /*strict=*/true, /*allow_exceptions=*/false);
	return j.is_object() ? j : json::object();
}

// The resolution the chunk builder will snap to (2ⁿ+1, so LOD0 vertices land on
// source grid points). Reported by terrain_info so a client is not surprised by
// a grid step that changes under it the first time it sculpts.
std::uint32_t snappedResolution(std::uint32_t res)
{
	const std::uint32_t r0 = std::clamp(res, 2u, 1024u);
	std::uint32_t cells = r0 - 1, p = 1;
	while (p < cells) p <<= 1;
	return p + 1;
}

json vec3Json(const glm::vec3& v) { return json::array({ v.x, v.y, v.z }); }

// ── Writing the edited component back ────────────────────────────────────────
// The brush ran on a copy; this is what turns that copy into a change the editor
// made. `patch` is the terrain object the caller already read out of
// `componentsOf` and modified, so nothing here re-describes the component.
//
// The runtime carry-over afterwards is the one write outside the gateway, and
// the header says why. Note what is NOT carried: `heightmapTexture`, which the
// scene writer has never emitted (nothing sets it either — it is a Phase 2
// placeholder). Carrying it here would be inventing persistence for a field the
// editor loses on every save, which is a fix that belongs with the serializer.
struct CarryOver
{
	HE::UUID      weightmapTextureId{};
	std::uint32_t builtRes = 0;
	std::uint32_t builtChunksPerSide = 0;
	bool          fullRebuild = false;   // resolution snap, or no chunk grid yet
	bool          weightsDirty = false;
	bool          regionDirty = false;
	float         minX = 0, minZ = 0, maxX = 0, maxZ = 0;
};

ToolResult writeBack(EditorCommands& cmds, Terrain& t, json terrainPatch,
                     const CarryOver& carry, const McpTerrainHooks& hooks)
{
	const std::string uuid = uuidOf(*t.world, t.entity);
	json write = json::object();
	write["terrain"] = std::move(terrainPatch);

	const Result res = cmds.execute(
		Command::setComponents(t.entity, json::to_cbor(write)), Origin::External);
	if (!res.ok()) return failFor(res.error, uuid);

	// The component is a NEW one now — emplace_or_replace built it from the JSON —
	// so the pointer captured before the command is stale by construction.
	TerrainComponent* now = t.world->registry().try_get<TerrainComponent>(t.entity);
	if (!now) return failFor(CmdError::Failed, uuid);
	t.tc = now;

	now->weightmapTextureId  = carry.weightmapTextureId;
	now->builtRes            = carry.builtRes;
	now->builtChunksPerSide  = carry.builtChunksPerSide;
	now->weightsDirty        = carry.weightsDirty;
	now->dirty               = carry.fullRebuild;
	now->regionDirty         = carry.regionDirty;
	now->dirtyMinX = carry.minX; now->dirtyMaxX = carry.maxX;
	now->dirtyMinZ = carry.minZ; now->dirtyMaxZ = carry.maxZ;

	if (hooks.regenerate) hooks.regenerate();
	return ToolResult::ok(json::object());
}

} // namespace

// ─── The tools ───────────────────────────────────────────────────────────────

void registerTerrainTools(McpToolRegistry& registry, EditorCommands& cmds,
                          McpTerrainHooks hooks)
{
	EditorCommands* c = &cmds;
	auto h = std::make_shared<McpTerrainHooks>(std::move(hooks));

	// ── terrain_info ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "terrain_info";
		t.description =
			"Describe the landscapes in the open scene: world footprint, height grid, "
			"noise parameters and whether the ground has been sculpted or painted. With "
			"no argument it lists every one of them, which is how a client learns the "
			"uuid the other three terrain tools take. Every coordinate and height on "
			"this interface is WORLD space.";
		t.inputSchema = objectSchema(json{
			{ "uuid", stringProp("Uuid of one landscape entity. Omit to describe all "
			                     "of them.") },
		}, {});
		t.handler = [c](const json& args) -> ToolResult {
			HorizonWorld* world = c->world();
			if (!world) return failFor(CmdError::NoWorld, {});

			std::vector<Entity> subjects;
			if (hasArg(args, "uuid"))
			{
				const Terrain r = resolveTerrain(*c, args);
				if (!r.ok) return r.failure;
				subjects.push_back(r.entity);
			}
			else
			{
				// Collected first, walked second — the same rule the entity tools
				// follow: reading components creates component storage, and adding
				// pools to the registry while iterating one of them is a crash.
				for (auto e : world->registry().view<TerrainComponent>())
					subjects.push_back(e);
			}

			json list = json::array();
			for (Entity e : subjects)
			{
				const auto& tc = world->registry().get<TerrainComponent>(e);
				const glm::vec3 wp = HE::worldPositionOf(*world, e);
				const std::uint32_t res = std::clamp(tc.resolution, 2u, 1024u);

				// Over the WHOLE field, which is the master heightfield the chunks
				// are built from — sculpted heights when they exist, the fBm
				// otherwise, flat when the seed is 0.
				const std::vector<float> field = computeTerrainHeightField(tc);
				float mn = 0.0f, mx = 0.0f;
				if (!field.empty())
				{
					mn = *std::min_element(field.begin(), field.end());
					mx = *std::max_element(field.begin(), field.end());
				}

				json j{
					{ "uuid",          uuidOf(*world, e) },
					{ "name",          nameOf(*world, e) },
					{ "worldPosition", vec3Json(wp) },
					{ "sizeX",         tc.sizeX },
					{ "sizeZ",         tc.sizeZ },
					{ "resolution",    res },
					// World XZ rectangle the landscape covers. A brush position
					// outside it changes nothing, so this is the first thing to
					// check when a sculpt reports changed = 0.
					{ "bounds", json{
						{ "minX", wp.x - tc.sizeX * 0.5f },
						{ "maxX", wp.x + tc.sizeX * 0.5f },
						{ "minZ", wp.z - tc.sizeZ * 0.5f },
						{ "maxZ", wp.z + tc.sizeZ * 0.5f } } },
					{ "gridStep",   json::array({ tc.sizeX / static_cast<float>(res - 1),
					                              tc.sizeZ / static_cast<float>(res - 1) }) },
					{ "heightRange", json::array({ wp.y + mn, wp.y + mx }) },
					{ "sculpted",   tc.sculptHeights.size() ==
					                static_cast<size_t>(res) * res },
					{ "heightScale", tc.heightScale },
					{ "seed",        tc.seed },
					{ "octaves",     tc.octaves },
					{ "frequency",   tc.frequency },
					{ "lacunarity",  tc.lacunarity },
					{ "gain",        tc.gain },
					{ "uvTiling",    tc.uvTiling },
					{ "lodDistanceScale", tc.lodDistanceScale },
					{ "painted",     !tc.layerWeights.empty() },
					{ "weightRes",   tc.weightRes },
					// The mean layer mix over the whole landscape, normalised.
					// Unpainted reads as [1, 0, 0, 0], which is what the shader
					// falls back to.
					{ "layerAverage", json::array({ tc.avgLayerWeights[0], tc.avgLayerWeights[1],
					                                tc.avgLayerWeights[2], tc.avgLayerWeights[3] }) },
				};
				// Only when it is a surprise. The chunk builder snaps the grid to
				// 2ⁿ+1 the first time it runs, resampling the heights — a client
				// that measured its grid step at 128 would find it moved.
				if (const std::uint32_t snap = snappedResolution(res); snap != res)
					j["resolutionSnapsTo"] = snap;
				list.push_back(std::move(j));
			}

			return ToolResult::ok(json{
				{ "terrains", std::move(list) },
				{ "count",    static_cast<int>(subjects.size()) },
			});
		};
		registry.add(std::move(t));
	}

	// ── terrain_heightmap ────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "terrain_heightmap";
		t.description =
			"Read the ground height over a rectangle as plain numbers — the answer "
			"entity_get cannot give, because it carries the height field as one base64 "
			"blob. Heights are WORLD Y, sampled bilinearly on a regular grid, row-major "
			"with Z as the outer axis: heights[row * samples + column], row 0 at minZ, "
			"column 0 at minX. Feed a value straight back to terrain_sculpt with "
			"op 'set'. The grid is capped, so a full landscape comes back as a coarse "
			"survey; narrow the rectangle to look closely.";
		t.inputSchema = objectSchema(json{
			{ "uuid",    stringProp("Uuid of the landscape, from terrain_info.") },
			{ "minX",    numberProp("World X of the rectangle's low corner. Omit all "
			                        "four to survey the whole landscape.") },
			{ "minZ",    numberProp("World Z of the rectangle's low corner.") },
			{ "maxX",    numberProp("World X of the rectangle's high corner.") },
			{ "maxZ",    numberProp("World Z of the rectangle's high corner.") },
			{ "samples", json{ { "type", "integer" }, { "minimum", 2 }, { "maximum", 128 },
			                   { "description", "Grid points per side (default 33, at "
			                                    "most 128). The answer holds samples² "
			                                    "numbers." } } },
		}, { "uuid" });
		t.handler = [c](const json& args) -> ToolResult {
			const Terrain r = resolveTerrain(*c, args);
			if (!r.ok) return r.failure;
			const TerrainComponent& tc = *r.tc;

			const float bMinX = r.worldPos.x - tc.sizeX * 0.5f;
			const float bMaxX = r.worldPos.x + tc.sizeX * 0.5f;
			const float bMinZ = r.worldPos.z - tc.sizeZ * 0.5f;
			const float bMaxZ = r.worldPos.z + tc.sizeZ * 0.5f;

			float minX = static_cast<float>(numArg(args, "minX", bMinX));
			float maxX = static_cast<float>(numArg(args, "maxX", bMaxX));
			float minZ = static_cast<float>(numArg(args, "minZ", bMinZ));
			float maxZ = static_cast<float>(numArg(args, "maxZ", bMaxZ));
			if (minX > maxX) std::swap(minX, maxX);
			if (minZ > maxZ) std::swap(minZ, maxZ);
			// Clamped rather than refused: "give me the ground around the player"
			// legitimately reaches over the edge, and a refusal there would make the
			// tool unusable near a border. The rect that was actually read comes
			// back in the result, so nothing is hidden.
			minX = std::clamp(minX, bMinX, bMaxX); maxX = std::clamp(maxX, bMinX, bMaxX);
			minZ = std::clamp(minZ, bMinZ, bMaxZ); maxZ = std::clamp(maxZ, bMinZ, bMaxZ);

			const int n = std::clamp(intArg(args, "samples", 33), 2, 128);

			// A degenerate rect (a line, or a request entirely off the landscape
			// that clamped to zero width) still samples: every column then reads
			// the same X, which is a legal question about a cross-section.
			const float stepX = (n > 1) ? (maxX - minX) / static_cast<float>(n - 1) : 0.0f;
			const float stepZ = (n > 1) ? (maxZ - minZ) / static_cast<float>(n - 1) : 0.0f;

			json heights = json::array();
			float mn = std::numeric_limits<float>::max();
			float mx = std::numeric_limits<float>::lowest();
			for (int zi = 0; zi < n; ++zi)
			{
				const float wz = minZ + static_cast<float>(zi) * stepZ;
				for (int xi = 0; xi < n; ++xi)
				{
					const float wx = minX + static_cast<float>(xi) * stepX;
					// terrainHeightAt takes terrain-LOCAL XZ and returns a local Y.
					const float y = r.worldPos.y +
						terrainHeightAt(tc, wx - r.worldPos.x, wz - r.worldPos.z);
					heights.push_back(y);
					mn = std::min(mn, y);
					mx = std::max(mx, y);
				}
			}

			return ToolResult::ok(json{
				{ "uuid",    uuidOf(*r.world, r.entity) },
				{ "samples", n },
				{ "rect", json{ { "minX", minX }, { "maxX", maxX },
				                { "minZ", minZ }, { "maxZ", maxZ } } },
				{ "step",    json::array({ stepX, stepZ }) },
				// Where heights[0] was read, so a client can map an index back to a
				// world position without re-deriving the step.
				{ "origin",  json::array({ minX, minZ }) },
				{ "min",     heights.empty() ? 0.0f : mn },
				{ "max",     heights.empty() ? 0.0f : mx },
				{ "heights", std::move(heights) },
			});
		};
		registry.add(std::move(t));
	}

	// ── terrain_sculpt ───────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "terrain_sculpt";
		t.description =
			"Shape the ground with one brush dab at a WORLD position: raise, lower, set "
			"to an exact height, flatten toward what is under the centre, smooth, or "
			"roughen. Full strength inside `radius`, fading linearly across `falloff`. "
			"Undoable in the editor like any other change. Read the result back with "
			"terrain_heightmap — a dab that landed outside the landscape reports "
			"changed 0 rather than failing.";
		t.inputSchema = objectSchema(json{
			{ "uuid",   stringProp("Uuid of the landscape, from terrain_info.") },
			{ "x",      numberProp("World X of the brush centre.") },
			{ "z",      numberProp("World Z of the brush centre.") },
			{ "op",     json{ { "type", "string" },
			                  { "enum", json::array({ "raise", "lower", "set", "flatten",
			                                          "smooth", "roughen" }) },
			                  { "description", "What the brush does. Default 'raise'." } } },
			{ "radius", numberProp("Full-strength radius in world units. Default 10.") },
			{ "falloff",numberProp("Width of the linear fade outside `radius`, in world "
			                       "units. Default 5. Use 0 for a hard edge.") },
			{ "amount", numberProp("raise/lower: metres of height at full strength. "
			                       "roughen: bump amplitude in metres. smooth: blend "
			                       "toward the neighbourhood mean, 0..1. Ignored by "
			                       "'set' and 'flatten'. Default 1.") },
			{ "height", numberProp("op 'set' only, and required for it: the WORLD Y the "
			                       "ground lands on inside `radius`.") },
		}, { "uuid", "x", "z" });
		t.mutates = true;
		t.handler = [c, h](const json& args) -> ToolResult {
			Terrain r = resolveTerrain(*c, args);
			if (!r.ok) return r.failure;

			if (!hasArg(args, "x") || !hasArg(args, "z"))
				return failFor(CmdError::InvalidPayload,
				               "'x' and 'z' are required: the WORLD position of the brush "
				               "centre. terrain_info reports the landscape's bounds.");

			TerrainSculpt::Op op = TerrainSculpt::Op::Raise;
			if (const std::string opName = strArg(args, "op"); !opName.empty() &&
			    !TerrainSculpt::opFromName(opName.c_str(), op))
				return failFor(CmdError::InvalidPayload,
				               "'" + opName + "' is not a brush operation. Use one of "
				               "raise, lower, set, flatten, smooth, roughen.");

			if (op == TerrainSculpt::Op::Set && !hasArg(args, "height"))
				return failFor(CmdError::InvalidPayload,
				               "op 'set' needs 'height' — the world Y the ground should "
				               "land on. Without it the tool would have to guess a target "
				               "and would flatten the ground to zero.");

			const float wx      = static_cast<float>(numArg(args, "x", 0.0));
			const float wz      = static_cast<float>(numArg(args, "z", 0.0));
			const float radius  = static_cast<float>(numArg(args, "radius", 10.0));
			const float falloff = static_cast<float>(numArg(args, "falloff", 5.0));
			if (radius < 0.0 || falloff < 0.0 || radius + falloff <= 0.0f)
				return failFor(CmdError::InvalidPayload,
				               "'radius' and 'falloff' must not be negative and must not "
				               "both be zero — a brush with no extent touches nothing.");

			// Heights are stored terrain-local, so a world target loses the entity's
			// own Y. Deltas (raise/lower/roughen) and the smooth blend do not.
			const float amount = (op == TerrainSculpt::Op::Set)
				? static_cast<float>(numArg(args, "height", 0.0)) - r.worldPos.y
				: static_cast<float>(numArg(args, "amount", 1.0));

			// The brush runs on a COPY: the world is only written through the
			// gateway, and a refused command must leave nothing behind.
			TerrainComponent work = *r.tc;
			const HE::UUID      keepWeightmap = work.weightmapTextureId;
			const std::uint32_t keepBuiltRes  = work.builtRes;
			const std::uint32_t keepBuiltCps  = work.builtChunksPerSide;
			const bool          wasDirty      = work.dirty;
			work.dirty = false;   // ensureHeights sets it again if it snaps the grid

			const TerrainSculpt::Result sr = TerrainSculpt::apply(
				work, wx - r.worldPos.x, wz - r.worldPos.z, op, radius, falloff, amount);
			if (!sr.ok)
				return failFor(CmdError::InvalidPayload,
				               "The landscape has no extent (sizeX/sizeZ must be "
				               "positive), so there is nothing for a brush to touch.");

			const std::string uuid = uuidOf(*r.world, r.entity);
			json result{
				{ "uuid",       uuid },
				{ "op",         TerrainSculpt::opName(op) },
				{ "changed",    static_cast<int>(sr.changed) },
				{ "resolution", work.resolution },
				{ "center",     json::array({ wx, wz }) },
				{ "radius",     radius },
				{ "falloff",    falloff },
				// World Y under the brush centre afterwards — the cheapest way for a
				// client to see that a 'set' actually landed on what it asked for.
				{ "heightAtCenter", r.worldPos.y + sr.centerHeight },
			};
			if (sr.changed > 0)
			{
				result["touchedMin"] = r.worldPos.y + sr.minHeight;
				result["touchedMax"] = r.worldPos.y + sr.maxHeight;
			}

			// Nothing moved — off the landscape, or an amount of zero. Reported as a
			// success with changed 0 rather than pushed through the gateway: an undo
			// entry for a change that did not happen is an undo step that appears to
			// do nothing when a human presses it.
			if (sr.changed == 0)
			{
				result["regenerated"] = false;
				return ToolResult::ok(std::move(result));
			}

			json terrain = componentsOf(*r.world, r.entity)["terrain"];
			if (!terrain.is_object())
				return failFor(CmdError::Failed, uuid);
			terrain["resolution"] = work.resolution;
			terrain["sculptHeightsB64"] = SceneSerializer::encodeBase64(
				reinterpret_cast<const std::uint8_t*>(work.sculptHeights.data()),
				work.sculptHeights.size() * sizeof(float));

			CarryOver carry;
			carry.weightmapTextureId = keepWeightmap;
			carry.builtRes           = keepBuiltRes;
			carry.builtChunksPerSide = keepBuiltCps;
			// A grid that was never built, or one whose resolution just snapped, has
			// to be rebuilt whole; anything else is the handful of chunks under the
			// brush.
			carry.fullRebuild  = wasDirty || work.dirty || keepBuiltRes == 0;
			carry.weightsDirty = false;
			carry.regionDirty  = work.regionDirty;
			carry.minX = work.dirtyMinX; carry.maxX = work.dirtyMaxX;
			carry.minZ = work.dirtyMinZ; carry.maxZ = work.dirtyMaxZ;

			const ToolResult wb = writeBack(*c, r, std::move(terrain), carry, *h);
			if (wb.isError) return wb;

			result["regenerated"] = static_cast<bool>(h->regenerate);
			return ToolResult::ok(std::move(result));
		};
		registry.add(std::move(t));
	}

	// ── terrain_paint ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "terrain_paint";
		t.description =
			"Paint one of the landscape's four material layers at a WORLD position. The "
			"layers are named by the material's Landscape Layer Blend node; the weights "
			"under the brush stay normalised, so painting a different layer over a spot "
			"undoes the first. Undoable in the editor like any other change. The first "
			"paint on an unpainted landscape allocates the weightmap, which changes "
			"nothing visually on its own.";
		t.inputSchema = objectSchema(json{
			{ "uuid",  stringProp("Uuid of the landscape, from terrain_info.") },
			{ "x",     numberProp("World X of the brush centre.") },
			{ "z",     numberProp("World Z of the brush centre.") },
			{ "layer", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", 3 },
			                 { "description", "Which of the four layers to paint, 0..3. "
			                                  "0 is what an unpainted landscape shows." } } },
			{ "radius",  numberProp("Full-strength radius in world units. Default 10.") },
			{ "falloff", numberProp("Width of the linear fade outside `radius`. "
			                        "Default 5.") },
			{ "strength",numberProp("How far the texels move toward the layer per call, "
			                        "0..1. Default 1, which paints it solid in one dab.") },
		}, { "uuid", "x", "z", "layer" });
		t.mutates = true;
		t.handler = [c, h](const json& args) -> ToolResult {
			Terrain r = resolveTerrain(*c, args);
			if (!r.ok) return r.failure;

			if (!hasArg(args, "x") || !hasArg(args, "z"))
				return failFor(CmdError::InvalidPayload,
				               "'x' and 'z' are required: the WORLD position of the brush "
				               "centre. terrain_info reports the landscape's bounds.");

			const int layer = intArg(args, "layer", -1);
			if (layer < 0 || layer > 3)
				return failFor(CmdError::InvalidPayload,
				               "'layer' must be 0, 1, 2 or 3 — a landscape blends exactly "
				               "four material layers.");

			const float wx       = static_cast<float>(numArg(args, "x", 0.0));
			const float wz       = static_cast<float>(numArg(args, "z", 0.0));
			const float radius   = static_cast<float>(numArg(args, "radius", 10.0));
			const float falloff  = static_cast<float>(numArg(args, "falloff", 5.0));
			const float strength = static_cast<float>(numArg(args, "strength", 1.0));
			if (radius < 0.0f || falloff < 0.0f || radius + falloff <= 0.0f)
				return failFor(CmdError::InvalidPayload,
				               "'radius' and 'falloff' must not be negative and must not "
				               "both be zero — a brush with no extent touches nothing.");
			if (strength <= 0.0f || strength > 1.0f)
				return failFor(CmdError::InvalidPayload,
				               "'strength' must be greater than 0 and at most 1.");

			TerrainComponent work = *r.tc;
			const HE::UUID      keepWeightmap = work.weightmapTextureId;
			const std::uint32_t keepBuiltRes  = work.builtRes;
			const std::uint32_t keepBuiltCps  = work.builtChunksPerSide;
			const bool          wasDirty      = work.dirty;

			const std::vector<std::uint8_t> before = work.layerWeights;
			// paint() answers false for two different things: a landscape with no
			// extent, and a dab that fell entirely off the weightmap. Only the first
			// is a mistake the client made — the second is the same "nothing
			// happened" that terrain_sculpt reports as changed 0, and refusing it
			// would make the two tools disagree about the same request.
			if (!TerrainPaint::paint(work, wx - r.worldPos.x, wz - r.worldPos.z,
			                         layer, radius, falloff, strength) &&
			    (work.sizeX <= 0.0f || work.sizeZ <= 0.0f))
				return failFor(CmdError::InvalidPayload,
				               "The landscape has no extent (sizeX/sizeZ must be "
				               "positive), so there is nothing for a brush to touch.");

			const std::string uuid = uuidOf(*r.world, r.entity);
			// The mix at the brush centre afterwards, normalised to 1 — what the
			// shader will blend by, and the only readable proof the dab landed.
			json mix = json::array({ 0.0f, 0.0f, 0.0f, 0.0f });
			{
				const std::uint32_t wr = work.weightRes;
				const float u = (wx - r.worldPos.x + work.sizeX * 0.5f) / work.sizeX;
				const float v = (wz - r.worldPos.z + work.sizeZ * 0.5f) / work.sizeZ;
				if (u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f &&
				    work.layerWeights.size() == static_cast<size_t>(wr) * wr * 4)
				{
					const auto tx = static_cast<std::uint32_t>(u * static_cast<float>(wr));
					const auto tz = static_cast<std::uint32_t>(v * static_cast<float>(wr));
					const std::uint8_t* px =
						&work.layerWeights[(static_cast<size_t>(tz) * wr + tx) * 4];
					const float sum = static_cast<float>(px[0] + px[1] + px[2] + px[3]);
					for (int k = 0; k < 4; ++k)
						mix[k] = sum > 0.0f ? static_cast<float>(px[k]) / sum : 0.0f;
				}
			}

			json result{
				{ "uuid",      uuid },
				{ "layer",     layer },
				{ "weightRes", work.weightRes },
				{ "center",    json::array({ wx, wz }) },
				{ "radius",    radius },
				{ "falloff",   falloff },
				{ "mixAtCenter", std::move(mix) },
			};

			// Allocating the weightmap IS a change even when no texel moved: the
			// landscape goes from "unpainted, shader falls back to layer 0" to a
			// real map, and that has to be written or the next call re-allocates it.
			if (work.layerWeights == before)
			{
				result["changed"]     = false;
				result["regenerated"] = false;
				return ToolResult::ok(std::move(result));
			}
			result["changed"] = true;

			json terrain = componentsOf(*r.world, r.entity)["terrain"];
			if (!terrain.is_object())
				return failFor(CmdError::Failed, uuid);
			terrain["weightRes"] = work.weightRes;
			terrain["layerWeightsB64"] = SceneSerializer::encodeBase64(
				work.layerWeights.data(), work.layerWeights.size());

			CarryOver carry;
			// The texture is REPLACED in place when its uuid survives, and registered
			// afresh when it does not — carrying it over is what keeps a hundred
			// paint calls from leaving a hundred abandoned textures behind.
			carry.weightmapTextureId = keepWeightmap;
			carry.builtRes           = keepBuiltRes;
			carry.builtChunksPerSide = keepBuiltCps;
			carry.fullRebuild        = wasDirty || keepBuiltRes == 0;
			carry.weightsDirty       = true;
			// Paint changes no heights, so no chunk mesh has to be rebuilt at all.
			carry.regionDirty        = false;

			const ToolResult wb = writeBack(*c, r, std::move(terrain), carry, *h);
			if (wb.isError) return wb;

			result["regenerated"] = static_cast<bool>(h->regenerate);
			return ToolResult::ok(std::move(result));
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
