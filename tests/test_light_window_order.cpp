#include "doctest.h"
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/LightPacking.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>

// Thema 125: the forward shaders shade only the first kMaxLightWindow lights of
// RenderWorld::lights. The day-night sun/moon used to land BEHIND the point
// lights (ECS order, or appended by applyDayNight), so eight point lights cost a
// forward frame its whole directional term. The deferred resolve keeps
// directionals in its window and stayed bright, which is how it showed: the
// MANYLIGHTS floor at roughly a third of its deferred brightness.

namespace
{
void addPoints(HorizonWorld& world, int n)
{
	for (int i = 0; i < n; ++i)
	{
		const Entity e = world.createEntity("Point");
		TransformComponent t;
		t.position = glm::vec3(static_cast<float>(i) * 3.0f, 1.0f, -5.0f);
		world.addComponent(e, t);
		LightComponent lc;
		lc.type      = HE::LightType::Point;
		lc.intensity = 6.0f;
		lc.range     = 2.4f;
		world.addComponent(e, lc);
	}
}

void setDayNightAt(RenderExtractor& ex, float timeOfDay)
{
	ex.setDayNight(true, timeOfDay, glm::vec3(1.0f, 0.97f, 0.90f), 2.2f,
	               glm::vec3(0.55f, 0.65f, 0.95f), 0.66f, /*cloudCoverage=*/0.0f);
}

// Index of the directional light that actually shines, or -1.
int shiningDirectional(const RenderWorld& rw)
{
	for (size_t i = 0; i < rw.lights.size(); ++i)
		if (rw.lights[i].type == 0 && rw.lights[i].intensity > 0.0f) return static_cast<int>(i);
	return -1;
}
} // namespace

TEST_CASE("the synthesised moon stays inside the light window past eight point lights")
{
	HorizonWorld world;
	addPoints(world, 16);

	RenderExtractor ex;
	setDayNightAt(ex, 0.0f); // midnight → the moon shines, the sun is off
	RenderWorld rw;
	ex.extract(world, rw, 1.0f);

	REQUIRE(rw.lights.size() == 18u);      // 16 points + synthesised sun + moon
	const int moon = shiningDirectional(rw);
	REQUIRE(moon >= 0);
	CHECK(rw.lights[moon].envRole == 2);
	CHECK(moon < HE::kMaxLightWindow);     // the forward shaders see it…
	CHECK(moon == 0);                      // …ahead of every point light

	// The switched-off sun shades nothing: it goes to the back rather than
	// taking a point light's slot, so the window still carries 7 points.
	CHECK(rw.lights.back().type == 0);
	CHECK(rw.lights.back().intensity == 0.0f);
	int pointsInWindow = 0;
	for (int i = 0; i < HE::kMaxLightWindow; ++i)
		if (rw.lights[i].type != 0) ++pointsInWindow;
	CHECK(pointsInWindow == HE::kMaxLightWindow - 1);
}

TEST_CASE("an authored sun created before the point lights is moved to the front")
{
	// entt hands the light pool out newest-first, so the sun created FIRST is
	// the one that lands behind all nine point lights without the reorder.
	HorizonWorld world;
	const Entity sun = world.createEntity("Sun");
	world.addComponent(sun, TransformComponent{});
	LightComponent dl;
	dl.type      = HE::LightType::Directional;
	dl.intensity = 3.0f;
	world.addComponent(sun, dl);
	addPoints(world, 9);

	RenderExtractor ex; // day-night off: the authored sun keeps shining
	RenderWorld rw;
	ex.extract(world, rw, 1.0f);

	REQUIRE(rw.lights.size() == 10u);
	CHECK(rw.lights[0].type == 0);
	CHECK(rw.lights[0].entityId == static_cast<uint32_t>(sun));
	CHECK(shiningDirectional(rw) == 0);
	// The point lights keep their relative order behind it (stable).
	for (size_t i = 1; i < rw.lights.size(); ++i)
		CHECK(rw.lights[i].type != 0);

	// The graph-material window then carries the sun as its slot 0.
	HE::MaterialShaderLibrary::Lighting ml{};
	HE::FillMaterialLightWindow(rw, ml, /*localShadowsActive=*/false);
	CHECK(ml.counts[0] == doctest::Approx(static_cast<float>(HE::kMaxLightWindow)));
	CHECK(ml.lightPos[0][3] == doctest::Approx(0.0f)); // type 0 = directional
}
