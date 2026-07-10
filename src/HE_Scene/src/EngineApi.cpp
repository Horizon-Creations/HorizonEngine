#include "HorizonScene/EngineApi.h"
#include "HorizonScene/ScriptApi.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/AudioEngine.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "HorizonScene/Components/NameComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include <Hpak/ProjectExporter.h>   // sceneUuidForPath (packed scene index)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <DebugDraw/DebugDraw.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <utility>
#include <unordered_map>
#include <unordered_set>

// The engine surface (transform/physics/material/ui/widget/cursor/entity) is a
// thin promotion of the language-neutral ScriptApi — same behavior, now bundled
// behind an explicit Ctx and grouped by subsystem, so one call reads the same in
// every frontend. The math group is native (pure C++). A later phase inverts the
// dependency (ScriptApi becomes a shim over HE::api) and adds the subsystems that
// need new Ctx providers: audio, input, camera, time, scene, fs/save.

namespace HE::api {

// ── Debug ────────────────────────────────────────────────────────────────────
void log(Ctx&, const std::string& message) { ScriptApi::log(message.c_str()); }

// ── Entities ─────────────────────────────────────────────────────────────────
namespace entity {
std::string getName(Ctx& c, Entity e)                          { return c.world ? ScriptApi::getName(*c.world, e) : std::string(); }
Entity      spawn(Ctx& c, Entity parent, const std::string& n) { return c.world ? ScriptApi::spawn(*c.world, parent, n) : 0u; }
void        destroy(Ctx& c, Entity e)                          { if (c.world) ScriptApi::destroy(*c.world, e); }
float       distance(Ctx& c, Entity a, Entity b)
{
    if (!c.world) return -1.0f;
    return glm::length(ScriptApi::getPosition(*c.world, a) - ScriptApi::getPosition(*c.world, b));
}
Entity findByName(Ctx& c, const std::string& name)
{
    if (!c.world || name.empty()) return 0u;
    auto view = c.world->registry().view<NameComponent>();
    for (auto [e, nc] : view.each())
        if (nc.name == name) return (Entity)e;
    return 0u;
}
bool exists(Ctx& c, Entity e)
{
    return c.world && c.world->registry().valid((entt::entity)e);
}
} // namespace entity

// ── Transform ────────────────────────────────────────────────────────────────
namespace transform {
glm::vec3 getPosition(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getPosition(*c.world, e) : glm::vec3(0.0f); }
void      setPosition(Ctx& c, Entity e, const glm::vec3& p){ if (c.world) ScriptApi::setPosition(*c.world, e, p); }
glm::vec3 getRotation(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getRotation(*c.world, e) : glm::vec3(0.0f); }
void      setRotation(Ctx& c, Entity e, const glm::vec3& r){ if (c.world) ScriptApi::setRotation(*c.world, e, r); }
glm::vec3 getScale(Ctx& c, Entity e)                       { return c.world ? ScriptApi::getScale(*c.world, e) : glm::vec3(1.0f); }
void      setScale(Ctx& c, Entity e, const glm::vec3& s)   { if (c.world) ScriptApi::setScale(*c.world, e, s); }
} // namespace transform

// ── Physics ──────────────────────────────────────────────────────────────────
namespace physics {
RaycastHit raycast(Ctx& c, const glm::vec3& o, const glm::vec3& d, float maxDist)
{
    const auto r = ScriptApi::raycast(c.physics, o, d, maxDist);
    return { r.hit, r.entityId, r.point, r.normal, r.distance };
}
void setVelocity(Ctx& c, Entity e, const glm::vec3& v) { ScriptApi::setVelocity(c.physics, e, v); }
bool isGrounded(Ctx& c, Entity e)                      { return ScriptApi::isGrounded(c.physics, e); }
} // namespace physics

// ── Materials ────────────────────────────────────────────────────────────────
namespace material {
glm::vec4 getParam(Ctx& c, Entity e, const std::string& n)                     { return c.world ? ScriptApi::getMaterialParam(*c.world, c.content, e, n) : glm::vec4(0.0f); }
bool      setParam(Ctx& c, Entity e, const std::string& n, const glm::vec4& v) { return c.world ? ScriptApi::setMaterialParam(*c.world, c.content, e, n, v) : false; }
} // namespace material

// ── Entity UI ────────────────────────────────────────────────────────────────
namespace ui {
std::string getText(Ctx& c, Entity e)                        { return c.world ? ScriptApi::getUIText(*c.world, e) : std::string(); }
void        setText(Ctx& c, Entity e, const std::string& t)  { if (c.world) ScriptApi::setUIText(*c.world, e, t); }
glm::vec4   getColor(Ctx& c, Entity e)                       { return c.world ? ScriptApi::getUIColor(*c.world, e) : glm::vec4(1.0f); }
void        setColor(Ctx& c, Entity e, const glm::vec4& col) { if (c.world) ScriptApi::setUIColor(*c.world, e, col); }
bool        getVisible(Ctx& c, Entity e)                     { return c.world ? ScriptApi::isUIVisible(*c.world, e) : false; }
void        setVisible(Ctx& c, Entity e, bool v)             { if (c.world) ScriptApi::setUIVisible(*c.world, e, v); }
glm::vec2   getPosition(Ctx& c, Entity e)                    { return c.world ? ScriptApi::getUIPosition(*c.world, e) : glm::vec2(0.0f); }
void        setPosition(Ctx& c, Entity e, const glm::vec2& p){ if (c.world) ScriptApi::setUIPosition(*c.world, e, p); }
glm::vec2   getSize(Ctx& c, Entity e)                        { return c.world ? ScriptApi::getUISize(*c.world, e) : glm::vec2(0.0f); }
void        setSize(Ctx& c, Entity e, const glm::vec2& s)    { if (c.world) ScriptApi::setUISize(*c.world, e, s); }
bool        setMaterialParam(Ctx& c, Entity e, const std::string& n, const glm::vec4& v) { return c.world ? ScriptApi::setUIMaterialParam(*c.world, c.content, e, n, v) : false; }
} // namespace ui

// ── Live widgets ─────────────────────────────────────────────────────────────
namespace widget {
int  create(Ctx& c, const std::string& p)          { return c.world ? ScriptApi::createWidget(*c.world, c.content, p) : 0; }
void destroy(Ctx& c, int id)                        { if (c.world) ScriptApi::destroyWidget(*c.world, id); }
void show(Ctx& c, int id)                           { if (c.world) ScriptApi::showWidget(*c.world, id); }
void hide(Ctx& c, int id)                           { if (c.world) ScriptApi::hideWidget(*c.world, id); }
void setZOrder(Ctx& c, int id, int z)               { if (c.world) ScriptApi::setWidgetZOrder(*c.world, id, z); }
bool isVisible(Ctx& c, int id)                      { return c.world ? ScriptApi::isWidgetVisible(*c.world, id) : false; }
bool callFunction(Ctx& c, int id, const std::string& fn) { return c.world ? ScriptApi::callWidgetFunction(*c.world, id, fn) : false; }
} // namespace widget

// ── Cursor ───────────────────────────────────────────────────────────────────
namespace cursor {
void setVisible(Ctx&, bool show) { ScriptApi::setCursorVisible(show); }
} // namespace cursor

// ── Camera ───────────────────────────────────────────────────────────────────
namespace {
// The world's main camera: isMain wins, else the first CameraComponent.
entt::entity mainCameraEntity(HorizonWorld* w)
{
    if (!w) return entt::null;
    entt::entity first = entt::null;
    auto view = w->registry().view<CameraComponent>();
    for (auto [e, cc] : view.each())
    {
        if (cc.isMain) return e;
        if (first == entt::null) first = e;
    }
    return first;
}
} // namespace
namespace camera {
glm::vec3 getPosition(Ctx& c)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return glm::vec3(0.0f);
    const auto* t = c.world->registry().try_get<TransformComponent>(e);
    return t ? t->position : glm::vec3(0.0f);
}
void setPosition(Ctx& c, const glm::vec3& p)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return;
    c.world->registry().get_or_emplace<TransformComponent>(e).position = p;
}
glm::vec3 getRotation(Ctx& c)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return glm::vec3(0.0f);
    const auto* t = c.world->registry().try_get<TransformComponent>(e);
    return t ? t->rotation : glm::vec3(0.0f);
}
void setRotation(Ctx& c, const glm::vec3& r)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return;
    c.world->registry().get_or_emplace<TransformComponent>(e).rotation = r;
}
float getFov(Ctx& c)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return 0.0f;
    return c.world->registry().get<CameraComponent>(e).fovDegrees;
}
void setFov(Ctx& c, float degrees)
{
    const entt::entity e = mainCameraEntity(c.world);
    if (e == entt::null) return;
    c.world->registry().get<CameraComponent>(e).fovDegrees = degrees;
}
} // namespace camera

// ── Environment ──────────────────────────────────────────────────────────────
namespace {
EnvironmentComponent* envOf(HorizonWorld* w)
{
    if (!w) return nullptr;
    auto view = w->registry().view<EnvironmentComponent>();
    for (auto e : view) return &view.get<EnvironmentComponent>(e);
    return nullptr;
}
} // namespace
namespace env {
float getTimeOfDay(Ctx& c)            { const auto* e = envOf(c.world); return e ? e->timeOfDay : 0.0f; }
void  setTimeOfDay(Ctx& c, float t)   { if (auto* e = envOf(c.world)) e->timeOfDay = t; }
float getCloudCoverage(Ctx& c)        { const auto* e = envOf(c.world); return e ? e->cloudCoverage : 0.0f; }
void  setCloudCoverage(Ctx& c, float v){ if (auto* e = envOf(c.world)) e->cloudCoverage = v; }
float getFogDensity(Ctx& c)           { const auto* e = envOf(c.world); return e ? e->fogDensity : 0.0f; }
void  setFogDensity(Ctx& c, float v)  { if (auto* e = envOf(c.world)) e->fogDensity = v; }
float getWindDirection(Ctx& c)        { const auto* e = envOf(c.world); return e ? e->windDirection : 0.0f; }
void  setWindDirection(Ctx& c, float v){ if (auto* e = envOf(c.world)) e->windDirection = v; }
float getWindSpeed(Ctx& c)            { const auto* e = envOf(c.world); return e ? e->windSpeed : 0.0f; }
void  setWindSpeed(Ctx& c, float v)   { if (auto* e = envOf(c.world)) e->windSpeed = v; }
} // namespace env

// ── Audio ────────────────────────────────────────────────────────────────────
namespace audio {
namespace {
const AudioAsset* audioAsset(Ctx& c, const std::string& path)
{
    if (!c.content || path.empty()) return nullptr;
    const HE::UUID id = c.content->loadAsset(path);
    const AudioAsset* a = c.content->getAudio(id);
    return (a && !a->audioData.empty()) ? a : nullptr;
}
} // namespace
int play(Ctx& c, const std::string& path, float volume, float pitch, bool loop)
{
    const AudioAsset* a = audioAsset(c, path);
    if (!c.audio || !c.audio->isInitialized() || !a) return 0;
    return (int)c.audio->play(a->audioData, a->sampleRate, a->channels, volume, pitch, loop);
}
int playAt(Ctx& c, const std::string& path, const glm::vec3& pos,
           float volume, float pitch, bool loop, float minDist, float maxDist)
{
    const AudioAsset* a = audioAsset(c, path);
    if (!c.audio || !c.audio->isInitialized() || !a) return 0;
    return (int)c.audio->playSpatial(a->audioData, a->sampleRate, a->channels,
                                     volume, pitch, loop, pos.x, pos.y, pos.z, minDist, maxDist);
}
void stop(Ctx& c, int handle)      { if (c.audio) c.audio->stop((uint64_t)(uint32_t)handle); }
void stopAll(Ctx& c)               { if (c.audio) c.audio->stopAll(); }
bool isPlaying(Ctx& c, int handle) { return c.audio && c.audio->isPlaying((uint64_t)(uint32_t)handle); }
void setBusVolume(Ctx& c, const std::string& bus, float volume)
{
    if (!c.audio || !c.audio->isInitialized()) return;
    if (!c.audio->hasBus(bus)) c.audio->createBus(bus, volume);
    c.audio->setBusVolume(bus, volume);
}
void setSoundPosition(Ctx& c, int handle, const glm::vec3& pos)
{
    if (c.audio) c.audio->setSoundPosition((uint64_t)(uint32_t)handle, pos.x, pos.y, pos.z);
}
} // namespace audio

// ── Debug draw ───────────────────────────────────────────────────────────────
namespace debug {
namespace {
struct Timed { DebugLine seg; float ttl; };
std::vector<Timed>& queue() { static std::vector<Timed> q; return q; }
void push(const glm::vec3& a, const glm::vec3& b, const glm::vec3& col, float ttl)
{ queue().push_back({ { a, b, col }, ttl }); }
} // namespace
void line(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color, float seconds)
{ push(a, b, color, seconds); }
void sphere(const glm::vec3& c0, float r, const glm::vec3& color, float seconds)
{
    // Expand to segments at submit time (three great circles), sharing one ttl.
    DebugDrawBuffer buf;
    buf.sphere(c0, r, color);
    for (const DebugLine& l : buf.lines()) queue().push_back({ l, seconds });
}
void box(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& color, float seconds)
{
    DebugDrawBuffer buf;
    buf.aabb(mn, mx, color);
    for (const DebugLine& l : buf.lines()) queue().push_back({ l, seconds });
}
void clear() { queue().clear(); }
void collect(float dt, std::vector<DebugLine>& out)
{
    auto& q = queue();
    for (const Timed& t : q) out.push_back(t.seg);     // draw everything alive NOW
    for (Timed& t : q) t.ttl -= dt;                     // then age
    q.erase(std::remove_if(q.begin(), q.end(),
        [](const Timed& t){ return t.ttl < 0.0f; }), q.end());
}
} // namespace debug

// ── Sandboxed fs + save store ────────────────────────────────────────────────
namespace fs {
namespace {
std::string& root() { static std::string r; return r; }
// A sandbox-relative path is valid when it has no root and no ".." component.
bool validRel(const std::string& rel)
{
    if (rel.empty()) return false;
    const std::filesystem::path p(rel);
    if (p.is_absolute() || p.has_root_name()) return false;
    for (const auto& part : p)
        if (part == "..") return false;
    return true;
}
std::filesystem::path resolved(const std::string& rel)
{
    if (root().empty() || !validRel(rel)) return {};
    return std::filesystem::path(root()) / rel;
}
} // namespace
void setSandboxRoot(const std::string& absDir) { root() = absDir; }
std::string sandboxRoot() { return root(); }
bool writeText(const std::string& rel, const std::string& text)
{
    const auto p = resolved(rel);
    if (p.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
}
std::string readText(const std::string& rel)
{
    const auto p = resolved(rel);
    if (p.empty()) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
}
bool exists(const std::string& rel)
{
    const auto p = resolved(rel);
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(p, ec);
}
bool remove(const std::string& rel)
{
    const auto p = resolved(rel);
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec)
        && std::filesystem::remove(p, ec);
}
bool makeDir(const std::string& rel)
{
    const auto p = resolved(rel);
    std::error_code ec;
    return !p.empty() && (std::filesystem::create_directories(p, ec) ||
                          std::filesystem::is_directory(p, ec));
}
} // namespace fs

namespace save {
namespace {
// One in-memory store; values kept as tagged strings for a trivial JSON shape.
struct Entry { int kind; float f; bool b; std::string s; }; // 0 num, 1 str, 2 bool
std::unordered_map<std::string, Entry>& store() { static std::unordered_map<std::string, Entry> s; return s; }
std::string slotPath(int slot) { return "Saves/slot" + std::to_string(slot) + ".json"; }
} // namespace
void  setNumber(const std::string& k, float v)                { store()[k] = { 0, v, false, {} }; }
float getNumber(const std::string& k, float def)
{ auto it = store().find(k); return it != store().end() && it->second.kind == 0 ? it->second.f : def; }
void  setString(const std::string& k, const std::string& v)   { store()[k] = { 1, 0, false, v }; }
std::string getString(const std::string& k, const std::string& def)
{ auto it = store().find(k); return it != store().end() && it->second.kind == 1 ? it->second.s : def; }
void  setBool(const std::string& k, bool v)                   { store()[k] = { 2, 0, v, {} }; }
bool  getBool(const std::string& k, bool def)
{ auto it = store().find(k); return it != store().end() && it->second.kind == 2 ? it->second.b : def; }
bool  hasKey(const std::string& k)     { return store().count(k) != 0; }
void  deleteKey(const std::string& k)  { store().erase(k); }
void  clearAll()                       { store().clear(); }
bool saveToSlot(int slot)
{
    nlohmann::json j;
    for (const auto& [k, e] : store())
    {
        if (e.kind == 0)      j[k] = e.f;
        else if (e.kind == 1) j[k] = e.s;
        else                  j[k] = e.b;
    }
    return fs::writeText(slotPath(slot), j.dump(2));
}
bool loadFromSlot(int slot)
{
    const std::string text = fs::readText(slotPath(slot));
    if (text.empty()) return false;
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    store().clear();
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (it->is_number())      setNumber(it.key(), it->get<float>());
        else if (it->is_string()) setString(it.key(), it->get<std::string>());
        else if (it->is_boolean()) setBool(it.key(), it->get<bool>());
    }
    return true;
}
bool slotExists(int slot) { return fs::exists(slotPath(slot)); }
} // namespace save

// ── Scene transitions ────────────────────────────────────────────────────────
namespace scene {
namespace {
std::vector<Request>& requests() { static std::vector<Request> q; return q; }
int& nextZone() { static int z = 1; return z; }
std::unordered_map<int, ZoneInfo>& zones() { static std::unordered_map<int, ZoneInfo> z; return z; }
bool& pendingLevel() { static bool p = false; return p; }
} // namespace
void load(const std::string& scenePath, bool hidden)
{ requests().push_back({ 0, scenePath, 0, hidden }); }
int loadAdditive(const std::string& scenePath, bool hidden)
{
    const int zone = nextZone()++;
    requests().push_back({ 1, scenePath, zone, hidden });
    return zone;
}
void unloadZone(int zone)
{ requests().push_back({ 2, {}, zone, false }); }
void activate()
{ requests().push_back({ 3, {}, 0, false }); }
std::vector<Request> takeRequests()
{
    std::vector<Request> out = std::move(requests());
    requests().clear();
    return out;
}

// ── Zone table (maintained by the app after executing requests) ──────────────
void noteZoneLoaded(int zone, ZoneInfo info) { zones()[zone] = std::move(info); }
void noteZoneUnloaded(int zone)              { zones().erase(zone); }
void clearZones()                            { zones().clear(); }
const ZoneInfo* zoneInfo(int zone)
{
    auto it = zones().find(zone);
    return it == zones().end() ? nullptr : &it->second;
}
void notePendingLevel(bool pending) { pendingLevel() = pending; }
bool hasPendingLevel()              { return pendingLevel(); }

std::vector<int> loadedZones()
{
    std::vector<int> out;
    out.reserve(zones().size());
    for (const auto& [id, z] : zones()) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}
std::string zoneScene(int zone)
{
    const ZoneInfo* z = zoneInfo(zone);
    return z ? z->path : std::string();
}
glm::vec3 zonePosition(Ctx& c, int zone)
{
    const ZoneInfo* z = zoneInfo(zone);
    if (!z || !c.world) return glm::vec3(0.0f);
    const auto e = (entt::entity)z->root;
    if (!c.world->registry().valid(e)) return glm::vec3(0.0f);
    const auto* t = c.world->registry().try_get<TransformComponent>(e);
    return t ? t->position : glm::vec3(0.0f);
}
void setZonePosition(Ctx& c, int zone, const glm::vec3& p)
{
    const ZoneInfo* z = zoneInfo(zone);
    if (!z || !c.world) return;
    const auto e = (entt::entity)z->root;
    if (!c.world->registry().valid(e)) return;
    // Children follow via the hierarchy's world-matrix composition — moving the
    // zone's sub-root moves the whole zone.
    c.world->registry().get_or_emplace<TransformComponent>(e).position = p;
}
void setZoneVisible(Ctx& c, int zone, bool visible)
{
    const ZoneInfo* z = zoneInfo(zone);
    if (!z || !c.world) return;
    auto& reg = c.world->registry();
    for (uint32_t id : z->entities)
    {
        const auto e = (entt::entity)id;
        if (!reg.valid(e)) continue;
        if (auto* m = reg.try_get<MeshComponent>(e)) m->visible = visible;
    }
}
std::vector<std::string> availableScenes(Ctx& c)
{
    std::vector<std::string> out;
    if (!c.content) return out;
    // Shipped builds: the packed scene index (a JSON string array).
    const auto bytes = c.content->readMountedEntry(sceneUuidForPath(kSceneIndexEntry));
    if (!bytes.empty())
    {
        const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_array())
        {
            for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
            return out;
        }
    }
    // Dev builds: scan the project (parent of the content root) for .hescene.
    const std::string root = c.content->contentRoot();
    if (root.empty()) return out;
    const auto projRoot = std::filesystem::path(root).parent_path();
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        projRoot, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end)
    {
        const bool regular = it->is_regular_file(ec);
        if (!ec && regular && it->path().extension() == ".hescene")
            out.push_back(it->path().lexically_relative(projRoot).generic_string());
        ec.clear();
        it.increment(ec);
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace scene

// ── String library ───────────────────────────────────────────────────────────
namespace str {
int length(const std::string& s) { return (int)s.size(); }
std::string substring(const std::string& s, int start, int count)
{
    if (start < 0) { count += start; start = 0; }
    if (start >= (int)s.size() || count <= 0) return {};
    return s.substr((size_t)start, (size_t)count);
}
bool contains(const std::string& s, const std::string& needle)
{ return needle.empty() || s.find(needle) != std::string::npos; }
int find(const std::string& s, const std::string& needle)
{
    const size_t p = s.find(needle);
    return p == std::string::npos ? -1 : (int)p;
}
std::string replace(const std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    std::string out; out.reserve(s.size());
    size_t pos = 0;
    while (true)
    {
        const size_t hit = s.find(from, pos);
        if (hit == std::string::npos) { out.append(s, pos, std::string::npos); return out; }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}
std::string toUpper(std::string const& s)
{ std::string r = s; for (char& ch : r) ch = (char)std::toupper((unsigned char)ch); return r; }
std::string toLower(std::string const& s)
{ std::string r = s; for (char& ch : r) ch = (char)std::tolower((unsigned char)ch); return r; }
std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}
bool startsWith(const std::string& s, const std::string& p)
{ return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p)
{ return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
float toNumber(const std::string& s)
{
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? 0.0f : v;
}
} // namespace str

// ── Math ─────────────────────────────────────────────────────────────────────
namespace math {
float sin(float x)   { return std::sin(x); }
float cos(float x)   { return std::cos(x); }
float tan(float x)   { return std::tan(x); }
float sqrt(float x)  { return std::sqrt(x); }
float abs(float x)   { return std::fabs(x); }
float floor(float x) { return std::floor(x); }
float ceil(float x)  { return std::ceil(x); }
float round(float x) { return std::round(x); }
float sign(float x)  { return (float)((x > 0.0f) - (x < 0.0f)); }
float pow(float b, float e) { return std::pow(b, e); }
float mod(float a, float b) { return b != 0.0f ? std::fmod(a, b) : 0.0f; }
float atan2(float y, float x) { return std::atan2(y, x); }
float min(float a, float b) { return a < b ? a : b; }
float max(float a, float b) { return a > b ? a : b; }
float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
float lerp(float a, float b, float t)    { return a + (b - a) * t; }
float length(const glm::vec2& v)                   { return glm::length(v); }
float distance(const glm::vec2& a, const glm::vec2& b) { return glm::length(a - b); }
} // namespace math

// ── Random ───────────────────────────────────────────────────────────────────
namespace random {
static std::mt19937& gen() { static std::mt19937 g(0x9E3779B9u); return g; } // fixed default seed → reproducible
void  seed(uint32_t s)          { gen().seed(s); }
float value()                   { return std::uniform_real_distribution<float>(0.0f, 1.0f)(gen()); }
float range(float lo, float hi) { if (hi < lo) std::swap(lo, hi);
                                  return lo == hi ? lo : std::uniform_real_distribution<float>(lo, hi)(gen()); }
int   rangeInt(int lo, int hi)  { if (hi < lo) std::swap(lo, hi);
                                  return std::uniform_int_distribution<int>(lo, hi)(gen()); }
bool  chance(float p)           { return value() < (p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p)); }
} // namespace random

// ── Time / frame ─────────────────────────────────────────────────────────────
namespace time {
namespace { struct Clock { float delta = 0.0f; double elapsed = 0.0; uint64_t frame = 0; }; Clock& clk() { static Clock c; return c; } }
void  advance(float dt) { Clock& c = clk(); c.delta = dt; c.elapsed += dt; ++c.frame; }
void  reset()           { clk() = Clock{}; }
float deltaTime()       { return clk().delta; }
float elapsed()         { return (float)clk().elapsed; }
int   frameCount()      { return (int)clk().frame; }
} // namespace time

// ── Input ────────────────────────────────────────────────────────────────────
namespace input {
namespace {
struct Snapshot { std::unordered_set<std::string> keys; uint32_t buttons = 0; glm::vec2 pos{0.0f}, delta{0.0f}; float scroll = 0.0f; };
Snapshot& snap() { static Snapshot s; return s; }
}
void setMouse(const glm::vec2& p, const glm::vec2& d, uint32_t mask, float sc)
{ Snapshot& s = snap(); s.pos = p; s.delta = d; s.buttons = mask; s.scroll = sc; }
void setKeysDown(const std::vector<std::string>& names)
{ Snapshot& s = snap(); s.keys.clear(); for (const auto& n : names) s.keys.insert(n); }
void clear() { snap() = Snapshot{}; }
bool      keyDown(const std::string& n) { return snap().keys.count(n) != 0; }
bool      mouseButton(int i)            { return i >= 0 && i < 32 && (snap().buttons & (1u << i)) != 0; }
glm::vec2 mousePosition()               { return snap().pos; }
glm::vec2 mouseDelta()                  { return snap().delta; }
float     scrollDelta()                 { return snap().scroll; }
} // namespace input

// ── Registry ─────────────────────────────────────────────────────────────────
namespace {

using P  = PinType;
using VV = std::vector<Value>;

// Value readers — tolerant of missing args (return the type's zero).
float       aF (const VV& a, size_t k) { return k < a.size() ? a[k].f   : 0.0f; }
bool        aB (const VV& a, size_t k) { return k < a.size() ? a[k].b   : false; }
int         aI (const VV& a, size_t k) { return k < a.size() ? a[k].i   : 0; }
std::string aS (const VV& a, size_t k) { return k < a.size() ? a[k].s   : std::string(); }
glm::vec2   aV2(const VV& a, size_t k) { return k < a.size() ? a[k].v2  : glm::vec2(0.0f); }
glm::vec4   aV4(const VV& a, size_t k) { return k < a.size() ? a[k].col : glm::vec4(0.0f); }
// vec3 rides in a Color value's xyz (HorizonCode has no Vec3 pin yet).
glm::vec3   aV3(const VV& a, size_t k) { return k < a.size() ? glm::vec3(a[k].col) : glm::vec3(0.0f); }
Value       v3 (const glm::vec3& v)    { return Value::ofColor(glm::vec4(v, 0.0f)); }

} // namespace

const std::vector<ApiFn>& registry()
{
    static const std::vector<ApiFn> table = []
    {
        std::vector<ApiFn> t;

        // Debug
        t.push_back({ "log", "Debug", true, {{"message", P::String}}, {}, "HE::api::log",
            [](Ctx& c, const VV& a){ log(c, aS(a, 0)); return VV{}; } });

        // Entities
        t.push_back({ "entity.getName", "Entity", false, {{"entity", P::Int}}, {{"name", P::String}}, "HE::api::entity::getName",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(entity::getName(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "entity.spawn", "Entity", true, {{"parent", P::Int}, {"name", P::String}}, {{"entity", P::Int}}, "HE::api::entity::spawn",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt((int)entity::spawn(c, (Entity)aI(a, 0), aS(a, 1))) }; } });
        t.push_back({ "entity.destroy", "Entity", true, {{"entity", P::Int}}, {}, "HE::api::entity::destroy",
            [](Ctx& c, const VV& a){ entity::destroy(c, (Entity)aI(a, 0)); return VV{}; } });
        t.push_back({ "entity.distance", "Entity", false, {{"a", P::Int}, {"b", P::Int}}, {{"distance", P::Float}}, "HE::api::entity::distance",
            [](Ctx& c, const VV& a){ return VV{ Value::ofFloat(entity::distance(c, (Entity)aI(a, 0), (Entity)aI(a, 1))) }; } });

        // Transform
        t.push_back({ "transform.getPosition", "Transform", false, {{"entity", P::Int}}, {{"position", P::Color}}, "HE::api::transform::getPosition",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getPosition(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setPosition", "Transform", true, {{"entity", P::Int}, {"position", P::Color}}, {}, "HE::api::transform::setPosition",
            [](Ctx& c, const VV& a){ transform::setPosition(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "transform.getRotation", "Transform", false, {{"entity", P::Int}}, {{"rotation", P::Color}}, "HE::api::transform::getRotation",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getRotation(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setRotation", "Transform", true, {{"entity", P::Int}, {"rotation", P::Color}}, {}, "HE::api::transform::setRotation",
            [](Ctx& c, const VV& a){ transform::setRotation(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "transform.getScale", "Transform", false, {{"entity", P::Int}}, {{"scale", P::Color}}, "HE::api::transform::getScale",
            [](Ctx& c, const VV& a){ return VV{ v3(transform::getScale(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "transform.setScale", "Transform", true, {{"entity", P::Int}, {"scale", P::Color}}, {}, "HE::api::transform::setScale",
            [](Ctx& c, const VV& a){ transform::setScale(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });

        // Physics
        t.push_back({ "physics.raycast", "Physics", false,
            {{"origin", P::Color}, {"direction", P::Color}, {"maxDistance", P::Float}},
            {{"hit", P::Bool}, {"entity", P::Int}, {"point", P::Color}, {"normal", P::Color}, {"distance", P::Float}},
            "HE::api::physics::raycast",
            [](Ctx& c, const VV& a){ auto r = physics::raycast(c, aV3(a, 0), aV3(a, 1), aF(a, 2));
                return VV{ Value::ofBool(r.hit), Value::ofInt((int)r.entity), v3(r.point), v3(r.normal), Value::ofFloat(r.distance) }; } });
        t.push_back({ "physics.setVelocity", "Physics", true, {{"entity", P::Int}, {"velocity", P::Color}}, {}, "HE::api::physics::setVelocity",
            [](Ctx& c, const VV& a){ physics::setVelocity(c, (Entity)aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "physics.isGrounded", "Physics", false, {{"entity", P::Int}}, {{"grounded", P::Bool}}, "HE::api::physics::isGrounded",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(physics::isGrounded(c, (Entity)aI(a, 0))) }; } });

        // Materials
        t.push_back({ "material.getParam", "Material", false, {{"entity", P::Int}, {"name", P::String}}, {{"value", P::Color}}, "HE::api::material::getParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofColor(material::getParam(c, (Entity)aI(a, 0), aS(a, 1))) }; } });
        t.push_back({ "material.setParam", "Material", true, {{"entity", P::Int}, {"name", P::String}, {"value", P::Color}}, {{"ok", P::Bool}}, "HE::api::material::setParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(material::setParam(c, (Entity)aI(a, 0), aS(a, 1), aV4(a, 2))) }; } });

        // Entity UI
        t.push_back({ "ui.getText", "UI", false, {{"entity", P::Int}}, {{"text", P::String}}, "HE::api::ui::getText",
            [](Ctx& c, const VV& a){ return VV{ Value::ofString(ui::getText(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setText", "UI", true, {{"entity", P::Int}, {"text", P::String}}, {}, "HE::api::ui::setText",
            [](Ctx& c, const VV& a){ ui::setText(c, (Entity)aI(a, 0), aS(a, 1)); return VV{}; } });
        t.push_back({ "ui.getColor", "UI", false, {{"entity", P::Int}}, {{"color", P::Color}}, "HE::api::ui::getColor",
            [](Ctx& c, const VV& a){ return VV{ Value::ofColor(ui::getColor(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setColor", "UI", true, {{"entity", P::Int}, {"color", P::Color}}, {}, "HE::api::ui::setColor",
            [](Ctx& c, const VV& a){ ui::setColor(c, (Entity)aI(a, 0), aV4(a, 1)); return VV{}; } });
        t.push_back({ "ui.getVisible", "UI", false, {{"entity", P::Int}}, {{"visible", P::Bool}}, "HE::api::ui::getVisible",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(ui::getVisible(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setVisible", "UI", true, {{"entity", P::Int}, {"visible", P::Bool}}, {}, "HE::api::ui::setVisible",
            [](Ctx& c, const VV& a){ ui::setVisible(c, (Entity)aI(a, 0), aB(a, 1)); return VV{}; } });
        t.push_back({ "ui.getPosition", "UI", false, {{"entity", P::Int}}, {{"position", P::Vec2}}, "HE::api::ui::getPosition",
            [](Ctx& c, const VV& a){ return VV{ Value::ofVec2(ui::getPosition(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setPosition", "UI", true, {{"entity", P::Int}, {"position", P::Vec2}}, {}, "HE::api::ui::setPosition",
            [](Ctx& c, const VV& a){ ui::setPosition(c, (Entity)aI(a, 0), aV2(a, 1)); return VV{}; } });
        t.push_back({ "ui.getSize", "UI", false, {{"entity", P::Int}}, {{"size", P::Vec2}}, "HE::api::ui::getSize",
            [](Ctx& c, const VV& a){ return VV{ Value::ofVec2(ui::getSize(c, (Entity)aI(a, 0))) }; } });
        t.push_back({ "ui.setSize", "UI", true, {{"entity", P::Int}, {"size", P::Vec2}}, {}, "HE::api::ui::setSize",
            [](Ctx& c, const VV& a){ ui::setSize(c, (Entity)aI(a, 0), aV2(a, 1)); return VV{}; } });
        t.push_back({ "ui.setMaterialParam", "UI", true, {{"entity", P::Int}, {"name", P::String}, {"value", P::Color}}, {{"ok", P::Bool}}, "HE::api::ui::setMaterialParam",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(ui::setMaterialParam(c, (Entity)aI(a, 0), aS(a, 1), aV4(a, 2))) }; } });

        // Live widgets
        t.push_back({ "widget.create", "Widget", true, {{"path", P::String}}, {{"widget", P::Int}}, "HE::api::widget::create",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(widget::create(c, aS(a, 0))) }; } });
        t.push_back({ "widget.destroy", "Widget", true, {{"widget", P::Int}}, {}, "HE::api::widget::destroy",
            [](Ctx& c, const VV& a){ widget::destroy(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "widget.show", "Widget", true, {{"widget", P::Int}}, {}, "HE::api::widget::show",
            [](Ctx& c, const VV& a){ widget::show(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "widget.hide", "Widget", true, {{"widget", P::Int}}, {}, "HE::api::widget::hide",
            [](Ctx& c, const VV& a){ widget::hide(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "widget.setZOrder", "Widget", true, {{"widget", P::Int}, {"z", P::Int}}, {}, "HE::api::widget::setZOrder",
            [](Ctx& c, const VV& a){ widget::setZOrder(c, aI(a, 0), aI(a, 1)); return VV{}; } });
        t.push_back({ "widget.isVisible", "Widget", false, {{"widget", P::Int}}, {{"visible", P::Bool}}, "HE::api::widget::isVisible",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(widget::isVisible(c, aI(a, 0))) }; } });
        t.push_back({ "widget.callFunction", "Widget", true, {{"widget", P::Int}, {"function", P::String}}, {{"ok", P::Bool}}, "HE::api::widget::callFunction",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(widget::callFunction(c, aI(a, 0), aS(a, 1))) }; } });

        // Cursor
        t.push_back({ "cursor.setVisible", "Cursor", true, {{"show", P::Bool}}, {}, "HE::api::cursor::setVisible",
            [](Ctx& c, const VV& a){ cursor::setVisible(c, aB(a, 0)); return VV{}; } });

        // Math (pure)
        auto unary  = [&](const char* id, const char* cpp, float(*fn)(float)) {
            t.push_back({ id, "Math", false, {{"x", P::Float}}, {{"result", P::Float}}, cpp,
                [fn](Ctx&, const VV& a){ return VV{ Value::ofFloat(fn(aF(a, 0))) }; } }); };
        auto binary = [&](const char* id, const char* cpp, float(*fn)(float, float),
                          const char* p0, const char* p1) {
            t.push_back({ id, "Math", false, {{p0, P::Float}, {p1, P::Float}}, {{"result", P::Float}}, cpp,
                [fn](Ctx&, const VV& a){ return VV{ Value::ofFloat(fn(aF(a, 0), aF(a, 1))) }; } }); };

        unary("math.sin",   "HE::api::math::sin",   math::sin);
        unary("math.cos",   "HE::api::math::cos",   math::cos);
        unary("math.tan",   "HE::api::math::tan",   math::tan);
        unary("math.sqrt",  "HE::api::math::sqrt",  math::sqrt);
        unary("math.abs",   "HE::api::math::abs",   math::abs);
        unary("math.floor", "HE::api::math::floor", math::floor);
        unary("math.ceil",  "HE::api::math::ceil",  math::ceil);
        unary("math.round", "HE::api::math::round", math::round);
        unary("math.sign",  "HE::api::math::sign",  math::sign);
        binary("math.pow",   "HE::api::math::pow",   math::pow,   "base", "exp");
        binary("math.mod",   "HE::api::math::mod",   math::mod,   "a", "b");
        binary("math.atan2", "HE::api::math::atan2", math::atan2, "y", "x");
        binary("math.min",   "HE::api::math::min",   math::min,   "a", "b");
        binary("math.max",   "HE::api::math::max",   math::max,   "a", "b");
        t.push_back({ "math.clamp", "Math", false, {{"x", P::Float}, {"lo", P::Float}, {"hi", P::Float}}, {{"result", P::Float}}, "HE::api::math::clamp",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::clamp(aF(a, 0), aF(a, 1), aF(a, 2))) }; } });
        t.push_back({ "math.lerp", "Math", false, {{"a", P::Float}, {"b", P::Float}, {"t", P::Float}}, {{"result", P::Float}}, "HE::api::math::lerp",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::lerp(aF(a, 0), aF(a, 1), aF(a, 2))) }; } });
        t.push_back({ "math.length", "Math", false, {{"v", P::Vec2}}, {{"result", P::Float}}, "HE::api::math::length",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::length(aV2(a, 0))) }; } });
        t.push_back({ "math.distance", "Math", false, {{"a", P::Vec2}, {"b", P::Vec2}}, {{"result", P::Float}}, "HE::api::math::distance",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(math::distance(aV2(a, 0), aV2(a, 1))) }; } });

        // Random (stateful → isExec, so a HorizonCode node caches one draw per run)
        t.push_back({ "random.seed", "Random", true, {{"seed", P::Int}}, {}, "HE::api::random::seed",
            [](Ctx&, const VV& a){ random::seed((uint32_t)aI(a, 0)); return VV{}; } });
        t.push_back({ "random.value", "Random", true, {}, {{"value", P::Float}}, "HE::api::random::value",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(random::value()) }; } });
        t.push_back({ "random.range", "Random", true, {{"min", P::Float}, {"max", P::Float}}, {{"value", P::Float}}, "HE::api::random::range",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(random::range(aF(a, 0), aF(a, 1))) }; } });
        t.push_back({ "random.rangeInt", "Random", true, {{"min", P::Int}, {"max", P::Int}}, {{"value", P::Int}}, "HE::api::random::rangeInt",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(random::rangeInt(aI(a, 0), aI(a, 1))) }; } });
        t.push_back({ "random.chance", "Random", true, {{"p", P::Float}}, {{"value", P::Bool}}, "HE::api::random::chance",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(random::chance(aF(a, 0))) }; } });

        // Time / frame (pure getters; the app advances the clock each frame)
        t.push_back({ "time.deltaTime", "Time", false, {}, {{"dt", P::Float}}, "HE::api::time::deltaTime",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(time::deltaTime()) }; } });
        t.push_back({ "time.elapsed", "Time", false, {}, {{"seconds", P::Float}}, "HE::api::time::elapsed",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(time::elapsed()) }; } });
        t.push_back({ "time.frameCount", "Time", false, {}, {{"frame", P::Int}}, "HE::api::time::frameCount",
            [](Ctx&, const VV&){ return VV{ Value::ofInt(time::frameCount()) }; } });

        // Input (pure getters; the app pushes the snapshot each frame)
        t.push_back({ "input.keyDown", "Input", false, {{"key", P::String}}, {{"down", P::Bool}}, "HE::api::input::keyDown",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(input::keyDown(aS(a, 0))) }; } });
        t.push_back({ "input.mouseButton", "Input", false, {{"button", P::Int}}, {{"down", P::Bool}}, "HE::api::input::mouseButton",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(input::mouseButton(aI(a, 0))) }; } });
        t.push_back({ "input.mousePosition", "Input", false, {}, {{"position", P::Vec2}}, "HE::api::input::mousePosition",
            [](Ctx&, const VV&){ return VV{ Value::ofVec2(input::mousePosition()) }; } });
        t.push_back({ "input.mouseDelta", "Input", false, {}, {{"delta", P::Vec2}}, "HE::api::input::mouseDelta",
            [](Ctx&, const VV&){ return VV{ Value::ofVec2(input::mouseDelta()) }; } });
        t.push_back({ "input.scrollDelta", "Input", false, {}, {{"scroll", P::Float}}, "HE::api::input::scrollDelta",
            [](Ctx&, const VV&){ return VV{ Value::ofFloat(input::scrollDelta()) }; } });

        // Entity queries
        t.push_back({ "entity.findByName", "Entity", false, {{"name", P::String}}, {{"entity", P::Int}}, "HE::api::entity::findByName",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt((int)entity::findByName(c, aS(a, 0))) }; } });
        t.push_back({ "entity.exists", "Entity", false, {{"entity", P::Int}}, {{"exists", P::Bool}}, "HE::api::entity::exists",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(entity::exists(c, (Entity)aI(a, 0))) }; } });

        // Camera (the world's main camera)
        t.push_back({ "camera.getPosition", "Camera", false, {}, {{"position", P::Color}}, "HE::api::camera::getPosition",
            [](Ctx& c, const VV&){ return VV{ v3(camera::getPosition(c)) }; } });
        t.push_back({ "camera.setPosition", "Camera", true, {{"position", P::Color}}, {}, "HE::api::camera::setPosition",
            [](Ctx& c, const VV& a){ camera::setPosition(c, aV3(a, 0)); return VV{}; } });
        t.push_back({ "camera.getRotation", "Camera", false, {}, {{"rotation", P::Color}}, "HE::api::camera::getRotation",
            [](Ctx& c, const VV&){ return VV{ v3(camera::getRotation(c)) }; } });
        t.push_back({ "camera.setRotation", "Camera", true, {{"rotation", P::Color}}, {}, "HE::api::camera::setRotation",
            [](Ctx& c, const VV& a){ camera::setRotation(c, aV3(a, 0)); return VV{}; } });
        t.push_back({ "camera.getFov", "Camera", false, {}, {{"degrees", P::Float}}, "HE::api::camera::getFov",
            [](Ctx& c, const VV&){ return VV{ Value::ofFloat(camera::getFov(c)) }; } });
        t.push_back({ "camera.setFov", "Camera", true, {{"degrees", P::Float}}, {}, "HE::api::camera::setFov",
            [](Ctx& c, const VV& a){ camera::setFov(c, aF(a, 0)); return VV{}; } });

        // Environment (sky / fog / wind knobs)
        auto envGet = [&](const char* id, const char* cpp, float(*fn)(Ctx&)) {
            t.push_back({ id, "Environment", false, {}, {{"value", P::Float}}, cpp,
                [fn](Ctx& c, const VV&){ return VV{ Value::ofFloat(fn(c)) }; } }); };
        auto envSet = [&](const char* id, const char* cpp, void(*fn)(Ctx&, float)) {
            t.push_back({ id, "Environment", true, {{"value", P::Float}}, {}, cpp,
                [fn](Ctx& c, const VV& a){ fn(c, aF(a, 0)); return VV{}; } }); };
        envGet("env.getTimeOfDay",     "HE::api::env::getTimeOfDay",     env::getTimeOfDay);
        envSet("env.setTimeOfDay",     "HE::api::env::setTimeOfDay",     env::setTimeOfDay);
        envGet("env.getCloudCoverage", "HE::api::env::getCloudCoverage", env::getCloudCoverage);
        envSet("env.setCloudCoverage", "HE::api::env::setCloudCoverage", env::setCloudCoverage);
        envGet("env.getFogDensity",    "HE::api::env::getFogDensity",    env::getFogDensity);
        envSet("env.setFogDensity",    "HE::api::env::setFogDensity",    env::setFogDensity);
        envGet("env.getWindDirection", "HE::api::env::getWindDirection", env::getWindDirection);
        envSet("env.setWindDirection", "HE::api::env::setWindDirection", env::setWindDirection);
        envGet("env.getWindSpeed",     "HE::api::env::getWindSpeed",     env::getWindSpeed);
        envSet("env.setWindSpeed",     "HE::api::env::setWindSpeed",     env::setWindSpeed);

        // Audio
        t.push_back({ "audio.play", "Audio", true,
            {{"asset", P::String}, {"volume", P::Float}, {"pitch", P::Float}, {"loop", P::Bool}},
            {{"handle", P::Int}}, "HE::api::audio::play",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(audio::play(c, aS(a, 0),
                a.size() > 1 ? aF(a, 1) : 1.0f, a.size() > 2 ? aF(a, 2) : 1.0f, aB(a, 3))) }; } });
        t.push_back({ "audio.playAt", "Audio", true,
            {{"asset", P::String}, {"position", P::Color}, {"volume", P::Float}, {"pitch", P::Float},
             {"loop", P::Bool}, {"minDist", P::Float}, {"maxDist", P::Float}},
            {{"handle", P::Int}}, "HE::api::audio::playAt",
            [](Ctx& c, const VV& a){ return VV{ Value::ofInt(audio::playAt(c, aS(a, 0), aV3(a, 1),
                a.size() > 2 ? aF(a, 2) : 1.0f, a.size() > 3 ? aF(a, 3) : 1.0f, aB(a, 4),
                a.size() > 5 ? aF(a, 5) : 1.0f, a.size() > 6 ? aF(a, 6) : 20.0f)) }; } });
        t.push_back({ "audio.stop", "Audio", true, {{"handle", P::Int}}, {}, "HE::api::audio::stop",
            [](Ctx& c, const VV& a){ audio::stop(c, aI(a, 0)); return VV{}; } });
        t.push_back({ "audio.stopAll", "Audio", true, {}, {}, "HE::api::audio::stopAll",
            [](Ctx& c, const VV&){ audio::stopAll(c); return VV{}; } });
        t.push_back({ "audio.isPlaying", "Audio", false, {{"handle", P::Int}}, {{"playing", P::Bool}}, "HE::api::audio::isPlaying",
            [](Ctx& c, const VV& a){ return VV{ Value::ofBool(audio::isPlaying(c, aI(a, 0))) }; } });
        t.push_back({ "audio.setBusVolume", "Audio", true, {{"bus", P::String}, {"volume", P::Float}}, {}, "HE::api::audio::setBusVolume",
            [](Ctx& c, const VV& a){ audio::setBusVolume(c, aS(a, 0), aF(a, 1)); return VV{}; } });
        t.push_back({ "audio.setSoundPosition", "Audio", true, {{"handle", P::Int}, {"position", P::Color}}, {}, "HE::api::audio::setSoundPosition",
            [](Ctx& c, const VV& a){ audio::setSoundPosition(c, aI(a, 0), aV3(a, 1)); return VV{}; } });

        // Debug draw (timed world-space primitives; drained by the app per frame)
        t.push_back({ "debug.line", "Debug", true,
            {{"from", P::Color}, {"to", P::Color}, {"color", P::Color}, {"seconds", P::Float}}, {}, "HE::api::debug::line",
            [](Ctx&, const VV& a){ debug::line(aV3(a, 0), aV3(a, 1),
                a.size() > 2 ? aV3(a, 2) : glm::vec3(1.0f, 1.0f, 0.0f), aF(a, 3)); return VV{}; } });
        t.push_back({ "debug.sphere", "Debug", true,
            {{"center", P::Color}, {"radius", P::Float}, {"color", P::Color}, {"seconds", P::Float}}, {}, "HE::api::debug::sphere",
            [](Ctx&, const VV& a){ debug::sphere(aV3(a, 0), a.size() > 1 ? aF(a, 1) : 1.0f,
                a.size() > 2 ? aV3(a, 2) : glm::vec3(1.0f, 1.0f, 0.0f), aF(a, 3)); return VV{}; } });
        t.push_back({ "debug.box", "Debug", true,
            {{"min", P::Color}, {"max", P::Color}, {"color", P::Color}, {"seconds", P::Float}}, {}, "HE::api::debug::box",
            [](Ctx&, const VV& a){ debug::box(aV3(a, 0), aV3(a, 1),
                a.size() > 2 ? aV3(a, 2) : glm::vec3(1.0f, 1.0f, 0.0f), aF(a, 3)); return VV{}; } });
        t.push_back({ "debug.clear", "Debug", true, {}, {}, "HE::api::debug::clear",
            [](Ctx&, const VV&){ debug::clear(); return VV{}; } });

        // Sandboxed file I/O
        t.push_back({ "fs.writeText", "File", true, {{"path", P::String}, {"text", P::String}}, {{"ok", P::Bool}}, "HE::api::fs::writeText",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::writeText(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "fs.readText", "File", true, {{"path", P::String}}, {{"text", P::String}}, "HE::api::fs::readText",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(fs::readText(aS(a, 0))) }; } });
        t.push_back({ "fs.exists", "File", false, {{"path", P::String}}, {{"exists", P::Bool}}, "HE::api::fs::exists",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::exists(aS(a, 0))) }; } });
        t.push_back({ "fs.remove", "File", true, {{"path", P::String}}, {{"ok", P::Bool}}, "HE::api::fs::remove",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::remove(aS(a, 0))) }; } });
        t.push_back({ "fs.makeDir", "File", true, {{"path", P::String}}, {{"ok", P::Bool}}, "HE::api::fs::makeDir",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(fs::makeDir(aS(a, 0))) }; } });

        // Save-game store
        t.push_back({ "save.setNumber", "Save", true, {{"key", P::String}, {"value", P::Float}}, {}, "HE::api::save::setNumber",
            [](Ctx&, const VV& a){ save::setNumber(aS(a, 0), aF(a, 1)); return VV{}; } });
        t.push_back({ "save.getNumber", "Save", false, {{"key", P::String}, {"default", P::Float}}, {{"value", P::Float}}, "HE::api::save::getNumber",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(save::getNumber(aS(a, 0), aF(a, 1))) }; } });
        t.push_back({ "save.setString", "Save", true, {{"key", P::String}, {"value", P::String}}, {}, "HE::api::save::setString",
            [](Ctx&, const VV& a){ save::setString(aS(a, 0), aS(a, 1)); return VV{}; } });
        t.push_back({ "save.getString", "Save", false, {{"key", P::String}, {"default", P::String}}, {{"value", P::String}}, "HE::api::save::getString",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(save::getString(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "save.setBool", "Save", true, {{"key", P::String}, {"value", P::Bool}}, {}, "HE::api::save::setBool",
            [](Ctx&, const VV& a){ save::setBool(aS(a, 0), aB(a, 1)); return VV{}; } });
        t.push_back({ "save.getBool", "Save", false, {{"key", P::String}, {"default", P::Bool}}, {{"value", P::Bool}}, "HE::api::save::getBool",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::getBool(aS(a, 0), aB(a, 1))) }; } });
        t.push_back({ "save.hasKey", "Save", false, {{"key", P::String}}, {{"has", P::Bool}}, "HE::api::save::hasKey",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::hasKey(aS(a, 0))) }; } });
        t.push_back({ "save.deleteKey", "Save", true, {{"key", P::String}}, {}, "HE::api::save::deleteKey",
            [](Ctx&, const VV& a){ save::deleteKey(aS(a, 0)); return VV{}; } });
        t.push_back({ "save.saveToSlot", "Save", true, {{"slot", P::Int}}, {{"ok", P::Bool}}, "HE::api::save::saveToSlot",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::saveToSlot(aI(a, 0))) }; } });
        t.push_back({ "save.loadFromSlot", "Save", true, {{"slot", P::Int}}, {{"ok", P::Bool}}, "HE::api::save::loadFromSlot",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::loadFromSlot(aI(a, 0))) }; } });
        t.push_back({ "save.slotExists", "Save", false, {{"slot", P::Int}}, {{"exists", P::Bool}}, "HE::api::save::slotExists",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(save::slotExists(aI(a, 0))) }; } });

        // Scene transitions (deferred; the app runtime executes the requests).
        // "hidden": unwired = false = present immediately (today's behavior); true
        // defers presentation — a hidden zone loads invisible until Show Zone, a
        // hidden level PRELOADS and swaps in on Activate.
        t.push_back({ "scene.load", "Scene", true, {{"scene", P::String}, {"hidden", P::Bool}}, {}, "HE::api::scene::load",
            [](Ctx&, const VV& a){ scene::load(aS(a, 0), aB(a, 1)); return VV{}; } });
        t.push_back({ "scene.loadAdditive", "Scene", true, {{"scene", P::String}, {"hidden", P::Bool}}, {{"zone", P::Int}}, "HE::api::scene::loadAdditive",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(scene::loadAdditive(aS(a, 0), aB(a, 1))) }; } });
        t.push_back({ "scene.unloadZone", "Scene", true, {{"zone", P::Int}}, {}, "HE::api::scene::unloadZone",
            [](Ctx&, const VV& a){ scene::unloadZone(aI(a, 0)); return VV{}; } });
        t.push_back({ "scene.activate", "Scene", true, {}, {}, "HE::api::scene::activate",
            [](Ctx&, const VV&){ scene::activate(); return VV{}; } });
        t.push_back({ "scene.hasPendingLevel", "Scene", false, {}, {{"pending", P::Bool}}, "HE::api::scene::hasPendingLevel",
            [](Ctx&, const VV&){ return VV{ Value::ofBool(scene::hasPendingLevel()) }; } });
        t.push_back({ "scene.showZone", "Scene", true, {{"zone", P::Int}}, {}, "HE::api::scene::setZoneVisible",
            [](Ctx& c, const VV& a){ scene::setZoneVisible(c, aI(a, 0), true); return VV{}; } });
        t.push_back({ "scene.hideZone", "Scene", true, {{"zone", P::Int}}, {}, "HE::api::scene::setZoneVisible",
            [](Ctx& c, const VV& a){ scene::setZoneVisible(c, aI(a, 0), false); return VV{}; } });
        t.push_back({ "scene.zonePosition", "Scene", false, {{"zone", P::Int}}, {{"position", P::Color}}, "HE::api::scene::zonePosition",
            [](Ctx& c, const VV& a){ return VV{ v3(scene::zonePosition(c, aI(a, 0))) }; } });
        t.push_back({ "scene.setZonePosition", "Scene", true, {{"zone", P::Int}, {"position", P::Color}}, {}, "HE::api::scene::setZonePosition",
            [](Ctx& c, const VV& a){ scene::setZonePosition(c, aI(a, 0), aV3(a, 1)); return VV{}; } });
        t.push_back({ "scene.zoneScene", "Scene", false, {{"zone", P::Int}}, {{"scene", P::String}}, "HE::api::scene::zoneScene",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(scene::zoneScene(aI(a, 0))) }; } });
        t.push_back({ "scene.loadedZones", "Scene", false, {}, {{"zones", P::Int, /*isArray=*/true}}, "HE::api::scene::loadedZones",
            [](Ctx&, const VV&){
                Value arr; arr.isArray = true; arr.type = P::Int;
                for (int z : scene::loadedZones()) arr.items.push_back(Value::ofInt(z));
                return VV{ std::move(arr) }; } });
        t.push_back({ "scene.available", "Scene", false, {}, {{"scenes", P::String, /*isArray=*/true}}, "HE::api::scene::availableScenes",
            [](Ctx& c, const VV&){
                Value arr; arr.isArray = true; arr.type = P::String;
                for (auto& s : scene::availableScenes(c)) arr.items.push_back(Value::ofString(std::move(s)));
                return VV{ std::move(arr) }; } });

        // String library (pure)
        t.push_back({ "string.length", "String", false, {{"s", P::String}}, {{"length", P::Int}}, "HE::api::str::length",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(str::length(aS(a, 0))) }; } });
        t.push_back({ "string.substring", "String", false,
            {{"s", P::String}, {"start", P::Int}, {"count", P::Int}}, {{"result", P::String}}, "HE::api::str::substring",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::substring(aS(a, 0), aI(a, 1), aI(a, 2))) }; } });
        t.push_back({ "string.contains", "String", false,
            {{"s", P::String}, {"needle", P::String}}, {{"contains", P::Bool}}, "HE::api::str::contains",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(str::contains(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.find", "String", false,
            {{"s", P::String}, {"needle", P::String}}, {{"index", P::Int}}, "HE::api::str::find",
            [](Ctx&, const VV& a){ return VV{ Value::ofInt(str::find(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.replace", "String", false,
            {{"s", P::String}, {"from", P::String}, {"to", P::String}}, {{"result", P::String}}, "HE::api::str::replace",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::replace(aS(a, 0), aS(a, 1), aS(a, 2))) }; } });
        t.push_back({ "string.toUpper", "String", false, {{"s", P::String}}, {{"result", P::String}}, "HE::api::str::toUpper",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::toUpper(aS(a, 0))) }; } });
        t.push_back({ "string.toLower", "String", false, {{"s", P::String}}, {{"result", P::String}}, "HE::api::str::toLower",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::toLower(aS(a, 0))) }; } });
        t.push_back({ "string.trim", "String", false, {{"s", P::String}}, {{"result", P::String}}, "HE::api::str::trim",
            [](Ctx&, const VV& a){ return VV{ Value::ofString(str::trim(aS(a, 0))) }; } });
        t.push_back({ "string.startsWith", "String", false,
            {{"s", P::String}, {"prefix", P::String}}, {{"result", P::Bool}}, "HE::api::str::startsWith",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(str::startsWith(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.endsWith", "String", false,
            {{"s", P::String}, {"suffix", P::String}}, {{"result", P::Bool}}, "HE::api::str::endsWith",
            [](Ctx&, const VV& a){ return VV{ Value::ofBool(str::endsWith(aS(a, 0), aS(a, 1))) }; } });
        t.push_back({ "string.toNumber", "String", false, {{"s", P::String}}, {{"number", P::Float}}, "HE::api::str::toNumber",
            [](Ctx&, const VV& a){ return VV{ Value::ofFloat(str::toNumber(aS(a, 0))) }; } });

        // ── Readable editor names (post-pass; id stays the stable identifier) ──
        // What menus and node titles show — "Sine" under Math, not "math.sin".
        static const std::pair<const char*, const char*> kNames[] = {
            { "log", "Log" },
            { "entity.getName", "Get Name" },       { "entity.spawn", "Spawn Entity" },
            { "entity.destroy", "Destroy Entity" }, { "entity.distance", "Distance Between" },
            { "transform.getPosition", "Get Position" }, { "transform.setPosition", "Set Position" },
            { "transform.getRotation", "Get Rotation" }, { "transform.setRotation", "Set Rotation" },
            { "transform.getScale", "Get Scale" },       { "transform.setScale", "Set Scale" },
            { "physics.raycast", "Raycast" }, { "physics.setVelocity", "Set Velocity" },
            { "physics.isGrounded", "Is Grounded" },
            { "material.getParam", "Get Material Param" }, { "material.setParam", "Set Material Param" },
            { "ui.getText", "Get UI Text" },        { "ui.setText", "Set UI Text" },
            { "ui.getColor", "Get UI Color" },      { "ui.setColor", "Set UI Color" },
            { "ui.getVisible", "Get UI Visible" },  { "ui.setVisible", "Set UI Visible" },
            { "ui.getPosition", "Get UI Position" },{ "ui.setPosition", "Set UI Position" },
            { "ui.getSize", "Get UI Size" },        { "ui.setSize", "Set UI Size" },
            { "ui.setMaterialParam", "Set UI Material Param" },
            { "widget.create", "Create Widget" },   { "widget.destroy", "Destroy Widget" },
            { "widget.show", "Show Widget" },       { "widget.hide", "Hide Widget" },
            { "widget.setZOrder", "Set Widget Z-Order" }, { "widget.isVisible", "Is Widget Visible" },
            { "widget.callFunction", "Call Widget Function" },
            { "cursor.setVisible", "Set Cursor Visible" },
            { "math.sin", "Sine" },   { "math.cos", "Cosine" }, { "math.tan", "Tangent" },
            { "math.sqrt", "Square Root" }, { "math.abs", "Absolute" },
            { "math.floor", "Floor" }, { "math.ceil", "Ceil" }, { "math.round", "Round" },
            { "math.sign", "Sign" },   { "math.pow", "Power" }, { "math.mod", "Modulo" },
            { "math.atan2", "Atan2" }, { "math.min", "Min" },   { "math.max", "Max" },
            { "math.clamp", "Clamp" }, { "math.lerp", "Lerp" },
            { "math.length", "Length (Vec2)" }, { "math.distance", "Distance (Vec2)" },
            { "random.seed", "Seed Random" },   { "random.value", "Random Value" },
            { "random.range", "Random Range" }, { "random.rangeInt", "Random Range (Int)" },
            { "random.chance", "Random Chance" },
            { "time.deltaTime", "Delta Time" }, { "time.elapsed", "Elapsed Time" },
            { "time.frameCount", "Frame Count" },
            { "input.keyDown", "Key Down" },          { "input.mouseButton", "Mouse Button" },
            { "input.mousePosition", "Mouse Position" }, { "input.mouseDelta", "Mouse Delta" },
            { "input.scrollDelta", "Scroll Delta" },
            { "entity.findByName", "Find By Name" },  { "entity.exists", "Entity Exists" },
            { "camera.getPosition", "Get Camera Position" }, { "camera.setPosition", "Set Camera Position" },
            { "camera.getRotation", "Get Camera Rotation" }, { "camera.setRotation", "Set Camera Rotation" },
            { "camera.getFov", "Get Camera FOV" },           { "camera.setFov", "Set Camera FOV" },
            { "env.getTimeOfDay", "Get Time Of Day" },       { "env.setTimeOfDay", "Set Time Of Day" },
            { "env.getCloudCoverage", "Get Cloud Coverage" },{ "env.setCloudCoverage", "Set Cloud Coverage" },
            { "env.getFogDensity", "Get Fog Density" },      { "env.setFogDensity", "Set Fog Density" },
            { "env.getWindDirection", "Get Wind Direction" },{ "env.setWindDirection", "Set Wind Direction" },
            { "env.getWindSpeed", "Get Wind Speed" },        { "env.setWindSpeed", "Set Wind Speed" },
            { "audio.play", "Play Sound" },        { "audio.playAt", "Play Sound At" },
            { "audio.stop", "Stop Sound" },        { "audio.stopAll", "Stop All Sounds" },
            { "audio.isPlaying", "Is Sound Playing" }, { "audio.setBusVolume", "Set Bus Volume" },
            { "string.length", "String Length" },  { "string.substring", "Substring" },
            { "string.contains", "String Contains" }, { "string.find", "String Find" },
            { "string.replace", "String Replace" },   { "string.toUpper", "To Upper" },
            { "string.toLower", "To Lower" },         { "string.trim", "Trim" },
            { "string.startsWith", "Starts With" },   { "string.endsWith", "Ends With" },
            { "string.toNumber", "To Number" },
            { "audio.setSoundPosition", "Set Sound Position" },
            { "debug.line", "Draw Debug Line" },   { "debug.sphere", "Draw Debug Sphere" },
            { "debug.box", "Draw Debug Box" },     { "debug.clear", "Clear Debug Draw" },
            { "fs.writeText", "Write Text File" }, { "fs.readText", "Read Text File" },
            { "fs.exists", "File Exists" },        { "fs.remove", "Delete File" },
            { "fs.makeDir", "Make Directory" },
            { "save.setNumber", "Save Set Number" }, { "save.getNumber", "Save Get Number" },
            { "save.setString", "Save Set String" }, { "save.getString", "Save Get String" },
            { "save.setBool", "Save Set Bool" },     { "save.getBool", "Save Get Bool" },
            { "save.hasKey", "Save Has Key" },       { "save.deleteKey", "Save Delete Key" },
            { "save.saveToSlot", "Save To Slot" },   { "save.loadFromSlot", "Load From Slot" },
            { "save.slotExists", "Save Slot Exists" },
            { "scene.load", "Load Scene" },          { "scene.loadAdditive", "Load Scene Additive" },
            { "scene.unloadZone", "Unload Zone" },   { "scene.activate", "Activate Loaded Scene" },
            { "scene.hasPendingLevel", "Has Pending Scene" },
            { "scene.showZone", "Show Zone" },       { "scene.hideZone", "Hide Zone" },
            { "scene.zonePosition", "Get Zone Position" },
            { "scene.setZonePosition", "Set Zone Position" },
            { "scene.zoneScene", "Get Zone Scene" }, { "scene.loadedZones", "Loaded Zones" },
            { "scene.available", "Available Scenes" },
        };
        for (auto& fn : t)
        {
            fn.displayName = fn.id; // fallback: never null, worst case the id shows
            for (const auto& [id, name] : kNames)
                if (std::strcmp(fn.id, id) == 0) { fn.displayName = name; break; }
        }

        return t;
    }();
    return table;
}

const ApiFn* find(const std::string& id)
{
    for (const auto& fn : registry())
        if (id == fn.id) return &fn;
    return nullptr;
}

} // namespace HE::api
