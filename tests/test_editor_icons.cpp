#include "doctest.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Renderer/UIFont.h>
#include <Renderer/IRenderer.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

// ── Editor icons ─────────────────────────────────────────────────────────────
// A light, a camera or an audio source has no mesh, so the scene view could
// neither show nor pick it. Under an ACTIVE editor camera the extractor now
// pushes one camera-facing quad per such entity into RenderWorld::objects —
// the list the viewport's picker and marquee already walk — textured with a
// built-in icon material. Without the override (play mode, packaged game)
// nothing of the sort appears.

namespace
{
	// A perspective editor camera at `eye`, looking down -Z.
	EditorCameraOverride editorCamAt(const glm::vec3& eye, bool ortho = false)
	{
		EditorCameraOverride cam;
		cam.active       = true;
		cam.position     = eye;
		cam.view         = glm::lookAt(eye, eye + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
		cam.fovDegrees   = 60.0f;
		cam.orthographic = ortho;
		return cam;
	}

	Entity placeEntity(HorizonWorld& world, const char* name, const glm::vec3& pos)
	{
		const Entity e = world.createEntity(name);
		TransformComponent t;
		t.position = pos;
		world.registry().emplace_or_replace<TransformComponent>(e, t);
		return e;
	}

	const RenderObject* iconOf(const RenderWorld& rw, Entity e)
	{
		for (const RenderObject& o : rw.objects)
			if (o.entityId == static_cast<uint32_t>(e) && o.meshAssetId == HE::kDefaultQuadMeshId)
				return &o;
		return nullptr;
	}
}

TEST_CASE("Editor icons: one quad per light, camera and audio source, only under the editor camera")
{
	HorizonWorld world;
	auto& reg = world.registry();

	const Entity point = placeEntity(world, "Point", { 0.0f, 2.0f, -10.0f });
	reg.emplace<LightComponent>(point, LightComponent{});                 // Point by default
	const Entity sun = placeEntity(world, "Sun", { 5.0f, 40.0f, -10.0f });
	LightComponent dl; dl.type = HE::LightType::Directional;
	reg.emplace<LightComponent>(sun, dl);
	const Entity spot = placeEntity(world, "Spot", { -3.0f, 2.0f, -10.0f });
	LightComponent sl; sl.type = HE::LightType::Spot;
	reg.emplace<LightComponent>(spot, sl);
	const Entity cam = placeEntity(world, "Cam", { 2.0f, 1.0f, -10.0f });
	reg.emplace<CameraComponent>(cam, CameraComponent{});
	const Entity audio = placeEntity(world, "Audio", { -2.0f, 1.0f, -10.0f });
	reg.emplace<AudioSourceComponent>(audio, AudioSourceComponent{});
	// A plain mesh entity draws its own mesh and gets NO icon.
	const Entity mesh = placeEntity(world, "Mesh", { 0.0f, 0.0f, -10.0f });
	reg.emplace<MeshComponent>(mesh, MeshComponent{ HE::kDefaultCubeMeshId });

	RenderExtractor ex;
	RenderWorld     rw;

	SUBCASE("no override at all → the play-mode picture, no icons")
	{
		ex.extract(world, rw, 16.0f / 9.0f, nullptr);
		REQUIRE(rw.objects.size() == 1);
		CHECK(rw.objects[0].entityId == static_cast<uint32_t>(mesh));
		CHECK(rw.lights.size() == 3);   // the lights themselves are untouched
	}

	SUBCASE("an INACTIVE override is the same as none")
	{
		EditorCameraOverride off = editorCamAt({ 0, 0, 0 });
		off.active = false;
		ex.extract(world, rw, 16.0f / 9.0f, &off);
		CHECK(rw.objects.size() == 1);
	}

	SUBCASE("the active editor camera adds exactly one icon per entity")
	{
		const EditorCameraOverride cam0 = editorCamAt({ 0, 0, 0 });
		ex.extract(world, rw, 16.0f / 9.0f, &cam0);
		REQUIRE(rw.objects.size() == 6);   // mesh + 5 icons

		const RenderObject* ip = iconOf(rw, point);
		const RenderObject* is = iconOf(rw, sun);
		const RenderObject* io = iconOf(rw, spot);
		const RenderObject* ic = iconOf(rw, cam);
		const RenderObject* ia = iconOf(rw, audio);
		REQUIRE(ip); REQUIRE(is); REQUIRE(io); REQUIRE(ic); REQUIRE(ia);
		CHECK(iconOf(rw, mesh) == nullptr);

		// Each kind carries its own material, per light type for lights.
		CHECK(ip->materialAssetId == HE::kEditorIconPointLightMaterialId);
		CHECK(is->materialAssetId == HE::kEditorIconDirectionalLightMaterialId);
		CHECK(io->materialAssetId == HE::kEditorIconSpotLightMaterialId);
		CHECK(ic->materialAssetId == HE::kEditorIconCameraMaterialId);
		CHECK(ia->materialAssetId == HE::kEditorIconAudioSourceMaterialId);

		// Sits on the entity, casts nothing, darkens nothing, has bounds to pick by.
		for (const RenderObject* o : { ip, is, io, ic, ia })
		{
			CHECK(o->castsShadow   == false);
			CHECK(o->contributesAO == false);
			CHECK(o->worldBounds.isValid());
		}
		CHECK(glm::vec3(ip->transform[3]) == glm::vec3(0.0f, 2.0f, -10.0f));

		// The quad faces the viewer: its +Z column points back toward the camera
		// and its X/Y columns span the view plane.
		const glm::vec3 z = glm::normalize(glm::vec3(ip->transform[2]));
		CHECK(glm::dot(z, glm::vec3(0, 0, 1)) == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(glm::dot(glm::normalize(glm::vec3(ip->transform[0])), glm::vec3(1, 0, 0))
		      == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(glm::dot(glm::normalize(glm::vec3(ip->transform[1])), glm::vec3(0, 1, 0))
		      == doctest::Approx(1.0f).epsilon(1e-4));

		// The lights list is what it always was — icons are objects, not lights.
		CHECK(rw.lights.size() == 3);
	}

	SUBCASE("a light's icon wears the light's hue, everything else stays white")
	{
		// Dim orange: the icon is tinted by the hue with its brightest channel
		// at 1 (HE::lightDisplayColor), not by the dim value itself.
		reg.get<LightComponent>(point).color = { 0.5f, 0.25f, 0.0f };
		reg.get<LightComponent>(spot).color  = { 0.0f, 0.0f, 3.0f };
		const EditorCameraOverride cam0 = editorCamAt({ 0, 0, 0 });
		ex.extract(world, rw, 16.0f / 9.0f, &cam0);
		const RenderObject* ip = iconOf(rw, point);
		const RenderObject* io = iconOf(rw, spot);
		const RenderObject* is = iconOf(rw, sun);
		const RenderObject* ic = iconOf(rw, cam);
		const RenderObject* ia = iconOf(rw, audio);
		REQUIRE(ip); REQUIRE(io); REQUIRE(is); REQUIRE(ic); REQUIRE(ia);
		CHECK(ip->instanceTint == glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
		CHECK(io->instanceTint == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
		CHECK(is->instanceTint == glm::vec4(1.0f));   // default white light
		CHECK(ic->instanceTint == glm::vec4(1.0f));
		CHECK(ia->instanceTint == glm::vec4(1.0f));
	}

	SUBCASE("constant screen size: the quad grows with view depth")
	{
		const EditorCameraOverride cam0 = editorCamAt({ 0, 0, 0 });
		ex.extract(world, rw, 16.0f / 9.0f, &cam0);
		const RenderObject* near = iconOf(rw, point);   // depth 10
		REQUIRE(near);
		const float sizeAt10 = glm::length(glm::vec3(near->transform[1]));
		// Expected: 2 * tan(fov/2) * depth * fraction.
		const float expected = 2.0f * std::tan(glm::radians(30.0f)) * 10.0f
		                     * HE::kEditorIconScreenFraction;
		CHECK(sizeAt10 == doctest::Approx(expected).epsilon(1e-3));

		// Twice the depth → twice the world size, the same size on screen.
		const EditorCameraOverride camBack = editorCamAt({ 0, 0, 10 });
		ex.extract(world, rw, 16.0f / 9.0f, &camBack);
		const RenderObject* far = iconOf(rw, point);    // depth 20
		REQUIRE(far);
		CHECK(glm::length(glm::vec3(far->transform[1])) == doctest::Approx(2.0f * sizeAt10).epsilon(1e-3));
	}

	SUBCASE("orthographic: size does not depend on depth")
	{
		const EditorCameraOverride o1 = editorCamAt({ 0, 0, 0 }, true);
		ex.extract(world, rw, 16.0f / 9.0f, &o1);
		const float s1 = glm::length(glm::vec3(iconOf(rw, point)->transform[1]));
		const EditorCameraOverride o2 = editorCamAt({ 0, 0, 30 }, true);
		ex.extract(world, rw, 16.0f / 9.0f, &o2);
		const float s2 = glm::length(glm::vec3(iconOf(rw, point)->transform[1]));
		CHECK(s1 == doctest::Approx(s2));
		CHECK(s1 > 0.0f);
	}

	SUBCASE("a hidden light hides its icon with it")
	{
		reg.get<LightComponent>(point).visible = false;
		const EditorCameraOverride cam0 = editorCamAt({ 0, 0, 0 });
		ex.extract(world, rw, 16.0f / 9.0f, &cam0);
		CHECK(iconOf(rw, point) == nullptr);
		CHECK(rw.objects.size() == 5);
	}

	SUBCASE("the built-in environment Sun and Moon get no icon")
	{
		// Their transform is a placeholder (the environment drives the direction),
		// so an icon on it would only sit on the world origin of every scene.
		reg.emplace<EnvironmentLightComponent>(sun,
			EnvironmentLightComponent{ EnvironmentLightComponent::Role::Sun });
		const EditorCameraOverride cam0 = editorCamAt({ 0, 0, 0 });
		ex.extract(world, rw, 16.0f / 9.0f, &cam0);
		CHECK(iconOf(rw, sun) == nullptr);
		CHECK(rw.objects.size() == 5);
		CHECK(rw.lights.size() == 3);   // still a light, just no icon
	}

	SUBCASE("icons do not stretch the directional shadow fit")
	{
		// The sun icon hovers at y = 40; the only caster is the cube at the
		// origin. The fit must be sized by the cube alone — the same box with
		// and without the icons.
		ex.extract(world, rw, 16.0f / 9.0f, nullptr);
		REQUIRE(rw.shadow.enabled);
		const glm::mat4 without = rw.shadow.viewProj;
		const EditorCameraOverride cam0 = editorCamAt({ 0, 0, 0 });
		ex.extract(world, rw, 16.0f / 9.0f, &cam0);
		REQUIRE(rw.shadow.enabled);
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				CHECK(rw.shadow.viewProj[c][r] == doctest::Approx(without[c][r]).epsilon(1e-4));
	}
}

TEST_CASE("Editor icons: the icon textures and materials are built into every ContentManager")
{
	ContentManager cm;
	const int px = HE::kEditorIconTextureSize;

	struct Pair { const char* glyph; HE::UUID tex; HE::UUID mat; };
	const Pair pairs[] = {
		{ "lightbulb", HE::kEditorIconPointLightTextureId,       HE::kEditorIconPointLightMaterialId },
		{ "highlight", HE::kEditorIconSpotLightTextureId,        HE::kEditorIconSpotLightMaterialId },
		{ "wb_sunny",  HE::kEditorIconDirectionalLightTextureId, HE::kEditorIconDirectionalLightMaterialId },
		{ "videocam",  HE::kEditorIconCameraTextureId,           HE::kEditorIconCameraMaterialId },
		{ "volume_up", HE::kEditorIconAudioSourceTextureId,      HE::kEditorIconAudioSourceMaterialId },
	};
	for (const Pair& p : pairs)
	{
		CAPTURE(p.glyph);
		// The glyph exists in the icon face — otherwise the texture is blank.
		CHECK(HE::uiIconCodepoint(p.glyph) != 0);

		const TextureAsset* tex = cm.getTexture(p.tex);
		REQUIRE(tex);
		CHECK(tex->width    == static_cast<uint32_t>(px));
		CHECK(tex->height   == static_cast<uint32_t>(px));
		CHECK(tex->channels == 4);
		REQUIRE(tex->data.size() == static_cast<size_t>(px) * px * 4);
		// Something was drawn: opaque pixels exist, and so do transparent ones
		// (the corners of a glyph box are always empty).
		size_t opaque = 0, clear = 0;
		for (size_t i = 3; i < tex->data.size(); i += 4)
		{
			if (tex->data[i] > 200) ++opaque;
			if (tex->data[i] == 0)  ++clear;
		}
		CHECK(opaque > 50);
		CHECK(clear  > 50);
		CHECK(tex->data[3] == 0);   // bottom-left corner
		CHECK(tex->data[tex->data.size() - 1] == 0);   // top-right corner

		const MaterialAsset* mat = cm.getMaterial(p.mat);
		REQUIRE(mat);
		REQUIRE(mat->textureIds.size() == 1);
		CHECK(mat->textureIds[0] == p.tex);
		CHECK(mat->doubleSided);
		CHECK(mat->blendMode == 2);   // Translucent → the sorted blend pass
		// Unlit: the generated fragment writes the sample straight out, no heLitP.
		CHECK(!mat->customShaderFragGlsl.empty());
		CHECK(mat->customShaderFragGlsl.find("heLitP(") == std::string::npos);
		// …and samples at the interpolated UV. An unconnected UV pin bakes to
		// the constant (0, 0) — the transparent corner, over the whole quad —
		// which is exactly how the first capture came out blank.
		CHECK(mat->customShaderFragGlsl.find("texture(heTex0, ") != std::string::npos);
		CHECK(mat->customShaderFragGlsl.find("= vUV;") != std::string::npos);
		CHECK(mat->customShaderFragGlsl.find("texture(heTex0, vec2(") == std::string::npos);
		// The sample is multiplied by Vertex Color, the per-instance tint a
		// light's icon carries its hue in; without it every icon stays white.
		CHECK(mat->customShaderFragGlsl.find("= vColor;") != std::string::npos);
		CHECK(!mat->nodeGraphJson.empty());
	}
}
