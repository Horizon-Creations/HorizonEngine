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
// The same without creating it: nullptr in a world nothing ever walked in.
HE_API Runtime* tryRuntime(HorizonWorld& world);

// The trails as a JSON document ({"v":1,"clock":..,"data":"<base64 of TrailField::serialize()>"}): what the savegame
// stores (EngineApi trail::save) and a streamed tile can carry. Trails are runtime state of a play session, so they
// live in the save, not in the .hescene. Clean ground gives a document with empty data (loading it clears).
HE_API std::string toJson(HorizonWorld& world);
// Replaces the world's trails with a document toJson wrote. `elapsedSeconds` is the game time that passed since it was
// written: that much healing is applied in one go with the CURRENT weather (capped at a game day, an approximation: the
// weather in between is unknown). False, with the trails untouched, for a document that is not one of ours.
HE_API bool fromJson(HorizonWorld& world, const std::string& json, double elapsedSeconds, const WeatherComponent* weather);

// Once per frame: heals with the weather (nullptr = calm and dry), then brings the clipmap texture in line
// with the field around the camera. Registers the texture on the first call, even with nothing trodden, so
// the landscape material can resolve it from the first frame.
HE_API void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                   const glm::vec3& cameraPos, float dt, const WeatherComponent* weather);
} // namespace TrailSystem
