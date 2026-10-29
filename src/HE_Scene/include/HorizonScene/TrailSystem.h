#pragma once
#include <Trails/TrailClipmap.h>
#include <Trails/TrailField.h>
#include <glm/vec3.hpp>

class HorizonWorld;
class ContentManager;
class IRenderer;
struct WeatherComponent;

// The world's trails in snow and sand (Thema 328): the field gameplay stamps into and queries, healed by
// the scene's weather, and the clipmap texture the landscape material reads. One per world, held in the
// registry's context so HorizonWorld itself does not carry it.
namespace TrailSystem
{
struct Runtime
{
    HE::TrailField    field;
    HE::TrailClipmap  clipmap;
    HE::TrailSettings settings;
    double            clock = 0.0;   // game seconds, the healing's clock
    bool              logged = false;
};

// The world's trails, created on first use.
HE_API Runtime& runtime(HorizonWorld& world);

// Once per frame: heals with the weather (nullptr = calm and dry), then brings the clipmap texture in line
// with the field around the camera. Registers the texture on the first call, even with nothing trodden, so
// the landscape material can resolve it from the first frame.
HE_API void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                   const glm::vec3& cameraPos, float dt, const WeatherComponent* weather);
} // namespace TrailSystem
