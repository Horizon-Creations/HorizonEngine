#include "doctest.h"

#include "ViewportPick.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Renderer/IRenderer.h>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>

// ── Click picking in the Scene window ────────────────────────────────────────
// A light, a camera or an audio source draws nothing, so until the extractor
// started pushing icon quads for them there was nothing under the cursor to
// select. Now there is — and whether a click on the symbol actually lands on
// the entity is a question about geometry the Scene window cannot be asked
// headless (it needs a GPU to be looked at). ViewportPick is that geometry on
// its own, fed here with a snapshot the REAL extractor produced under a real
// editor camera, and clicked at pixels derived from the camera rather than
// typed in.

namespace
{
constexpr glm::vec2 kRectMin(40.0f, 24.0f);
constexpr glm::vec2 kRectSize(900.0f, 600.0f);

// A perspective editor camera at `eye`, looking down -Z (the icon test's).
EditorCameraOverride editorCamAt(const glm::vec3& eye)
{
	EditorCameraOverride cam;
	cam.active     = true;
	cam.position   = eye;
	cam.view       = glm::lookAt(eye, eye + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
	cam.fovDegrees = 60.0f;
	return cam;
}

Entity placeEntity(HorizonWorld& world, const char* name, const glm::vec3& pos,
                   const glm::vec3& scale = glm::vec3(1.0f))
{
	const Entity e = world.createEntity(name);
	TransformComponent t;
	t.position = pos;
	t.scale    = scale;
	world.registry().emplace_or_replace<TransformComponent>(e, t);
	return e;
}

// World point → the pixel the picture shows it at, through the snapshot's own
// matrices: what a user aims the mouse at.
glm::vec2 screenOf(const RenderWorld& rw, const glm::vec3& world)
{
	const glm::vec4 clip = rw.camera.projection * rw.camera.view * glm::vec4(world, 1.0f);
	REQUIRE(clip.w > 1e-6f);
	const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
	return kRectMin + glm::vec2((ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f) * kRectSize;
}

// The box lookup ViewportPanel serves from its cache: a mesh asset's local
// bounds out of the content manager. The quad's box is flat — z = 0 exactly —
// which is the shape a click on an icon is tested against.
struct Boxes
{
	ContentManager& cm;
	std::unordered_map<HE::UUID, HE::AABB> cache;
	const HE::AABB* operator()(const HE::UUID& id)
	{
		auto it = cache.find(id);
		if (it == cache.end())
		{
			const StaticMeshAsset* mesh = cm.getStaticMesh(id);
			if (!mesh) return nullptr;
			it = cache.emplace(id, HE::AABB::fromPositions(mesh->vertices.data(),
			                                               mesh->vertices.size() / 3)).first;
		}
		return &it->second;
	}
};

// One click, through the same call the Scene window makes.
Entity clickAt(const RenderWorld& rw, entt::registry& reg, Boxes& boxes, const glm::vec2& px)
{
	return ViewportPick::pickAtScreen(rw, reg, std::ref(boxes),
		rw.camera.projection * rw.camera.view, kRectMin, kRectSize, px);
}
} // namespace

TEST_CASE("ViewportPick: a click on an icon selects the light, camera or audio source")
{
	HorizonWorld world;
	auto& reg = world.registry();
	ContentManager cm;
	Boxes boxes{ cm };

	const Entity point = placeEntity(world, "Point", { 0.0f, 2.0f, -10.0f });
	reg.emplace<LightComponent>(point, LightComponent{});
	const Entity camE = placeEntity(world, "Cam", { 3.0f, 0.0f, -10.0f });
	reg.emplace<CameraComponent>(camE, CameraComponent{});
	const Entity audio = placeEntity(world, "Audio", { -3.0f, 0.0f, -10.0f });
	reg.emplace<AudioSourceComponent>(audio, AudioSourceComponent{});
	// A plain cube on the same depth plane, to prove icons and meshes share one list.
	const Entity cube = placeEntity(world, "Cube", { 0.0f, -2.0f, -10.0f });
	reg.emplace<MeshComponent>(cube, MeshComponent{ HE::kDefaultCubeMeshId });

	RenderExtractor ex;
	RenderWorld     rw;
	const EditorCameraOverride cam = editorCamAt({ 0, 0, 0 });
	ex.extract(world, rw, kRectSize.x / kRectSize.y, &cam);
	REQUIRE(rw.objects.size() == 4);   // cube + 3 icons

	// Dead centre of each symbol.
	CHECK(clickAt(rw, reg, boxes, screenOf(rw, { 0.0f,  2.0f, -10.0f })) == point);
	CHECK(clickAt(rw, reg, boxes, screenOf(rw, { 3.0f,  0.0f, -10.0f })) == camE);
	CHECK(clickAt(rw, reg, boxes, screenOf(rw, { -3.0f, 0.0f, -10.0f })) == audio);
	CHECK(clickAt(rw, reg, boxes, screenOf(rw, { 0.0f, -2.0f, -10.0f })) == cube);

	// The symbol is kEditorIconScreenFraction of the picture's height: a click
	// inside that square hits, one just past its edge misses. Both measured in
	// the picture's own pixels, so the claim is about screen size — the whole
	// point of a constant-size icon.
	const float halfIcon = HE::kEditorIconScreenFraction * kRectSize.y * 0.5f;
	const glm::vec2 centre = screenOf(rw, { 0.0f, 2.0f, -10.0f });
	CHECK(clickAt(rw, reg, boxes, centre + glm::vec2(halfIcon * 0.8f,  halfIcon * 0.8f)) == point);
	CHECK(clickAt(rw, reg, boxes, centre + glm::vec2(-halfIcon * 0.8f, halfIcon * 0.8f)) == point);
	CHECK((clickAt(rw, reg, boxes, centre + glm::vec2(halfIcon * 1.3f, 0.0f)) == entt::null));
	CHECK((clickAt(rw, reg, boxes, centre + glm::vec2(0.0f, -halfIcon * 1.3f)) == entt::null));

	// Empty sky: nothing.
	CHECK((clickAt(rw, reg, boxes, kRectMin + glm::vec2(10.0f, 10.0f)) == entt::null));
}

TEST_CASE("ViewportPick: an icon behind a mesh loses to the mesh, one in front of it wins")
{
	HorizonWorld world;
	auto& reg = world.registry();
	ContentManager cm;
	Boxes boxes{ cm };

	// A 3 m cube at depth 10 with a light hidden BEHIND it on the same line of
	// sight, and another light floating in FRONT of it. Nearest hit wins, the
	// same way it does between two meshes — the icon gets no special priority
	// over the geometry that covers it.
	const Entity wall = placeEntity(world, "Wall", { 0.0f, 0.0f, -10.0f }, glm::vec3(3.0f));
	reg.emplace<MeshComponent>(wall, MeshComponent{ HE::kDefaultCubeMeshId });
	const Entity behind = placeEntity(world, "Behind", { 0.0f, 0.0f, -14.0f });
	reg.emplace<LightComponent>(behind, LightComponent{});
	const Entity front = placeEntity(world, "Front", { 0.0f, 0.0f, -6.0f });
	reg.emplace<LightComponent>(front, LightComponent{});

	RenderExtractor ex;
	RenderWorld     rw;
	const EditorCameraOverride cam = editorCamAt({ 0, 0, 0 });
	ex.extract(world, rw, kRectSize.x / kRectSize.y, &cam);
	REQUIRE(rw.objects.size() == 3);

	// Both lights project onto the picture's centre, exactly where the wall is.
	const glm::vec2 centre = screenOf(rw, { 0.0f, 0.0f, -10.0f });
	CHECK(clickAt(rw, reg, boxes, centre) == front);

	// Take the front light away: now the wall covers the one behind.
	reg.get<LightComponent>(front).visible = false;
	ex.extract(world, rw, kRectSize.x / kRectSize.y, &cam);
	REQUIRE(rw.objects.size() == 2);
	CHECK(clickAt(rw, reg, boxes, centre) == wall);
}

TEST_CASE("ViewportPick: no icons without the editor camera, so nothing to click")
{
	// Play mode hands the extractor no override; the picker then sees only
	// the meshes — a click where the light is selects nothing.
	HorizonWorld world;
	auto& reg = world.registry();
	ContentManager cm;
	Boxes boxes{ cm };
	const Entity point = placeEntity(world, "Point", { 0.0f, 2.0f, -10.0f });
	reg.emplace<LightComponent>(point, LightComponent{});

	RenderExtractor ex;
	RenderWorld     rw;
	ex.extract(world, rw, kRectSize.x / kRectSize.y, nullptr);
	CHECK(rw.objects.empty());
	// The play camera is whatever the scene has (none here) — use the editor
	// camera's matrices only to aim, the snapshot itself has no icon.
	const EditorCameraOverride cam = editorCamAt({ 0, 0, 0 });
	rw.camera.view       = cam.view;
	rw.camera.projection = glm::perspective(glm::radians(60.0f), kRectSize.x / kRectSize.y, 0.1f, 100.0f);
	CHECK((clickAt(rw, reg, boxes, screenOf(rw, { 0.0f, 2.0f, -10.0f })) == entt::null));
}

TEST_CASE("ViewportPick: terrain only answers when nothing else is under the cursor, as its owner")
{
	// The rule the Scene window always had, kept through the move into a
	// module: a chunk's box is huge and loose, so a prop resting on it must win
	// even though the chunk's near face is closer — and when the ground alone
	// is hit, the answer is the Landscape entity, never the chunk.
	HorizonWorld world;
	auto& reg = world.registry();

	const Entity land  = world.createEntity("Landscape");
	reg.emplace<TerrainComponent>(land, TerrainComponent{});
	// The chunk's box reaches up to y = 2 — a hill somewhere in it — so it
	// encloses the prop standing at y = 0.5 on flat ground, and a ray from
	// above enters the chunk's box before the prop's.
	const Entity chunk = placeEntity(world, "Chunk", { 0.0f, -1.0f, -10.0f }, glm::vec3(40.0f, 6.0f, 40.0f));
	reg.emplace<TerrainChunkComponent>(chunk, TerrainChunkComponent{ land, 0, 0 });
	const Entity prop  = placeEntity(world, "Prop", { 0.0f, 0.5f, -10.0f });

	// Hand-built snapshot: the two boxes under their transforms, as the
	// extractor would emit them, with the fallback cube as the local box.
	RenderWorld rw;
	rw.camera.view       = glm::lookAt(glm::vec3(0, 8, 0), glm::vec3(0, 0, -10), glm::vec3(0, 1, 0));
	rw.camera.projection = glm::perspective(glm::radians(60.0f), kRectSize.x / kRectSize.y, 0.1f, 100.0f);
	auto push = [&](Entity e)
	{
		RenderObject o;
		o.entityId  = static_cast<uint32_t>(e);
		o.transform = reg.get<TransformComponent>(e).worldMatrix;
		rw.objects.push_back(o);
	};
	// worldMatrix is only composed by the hierarchy pass; compose it here.
	for (const Entity e : { chunk, prop })
	{
		auto& t = reg.get<TransformComponent>(e);
		t.worldMatrix = glm::scale(glm::translate(glm::mat4(1.0f), t.position), t.scale);
	}
	push(chunk);
	push(prop);

	const ViewportPick::BoxLookup none = [](const HE::UUID&) -> const HE::AABB* { return nullptr; };
	const glm::mat4 vp = rw.camera.projection * rw.camera.view;

	// Straight at the prop: the prop, although the ray enters the chunk's
	// loose box first. Mesh before terrain.
	CHECK(ViewportPick::pickAtScreen(rw, reg, none, vp, kRectMin, kRectSize,
		screenOf(rw, { 0.0f, 0.5f, -10.0f })) == prop);
	// Off to the side over bare ground: the Landscape, not the chunk.
	CHECK(ViewportPick::pickAtScreen(rw, reg, none, vp, kRectMin, kRectSize,
		screenOf(rw, { 8.0f, 0.0f, -14.0f })) == land);
}
