#include "HorizonScene/ScriptApi.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/NameComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/UIElementComponent.h"
#include "HorizonScene/Components/UITextComponent.h"
#include "HorizonScene/Components/UIImageComponent.h"
#include "HorizonScene/Components/UIButtonComponent.h"
#include "ContentManager/ContentManager.h"
#include <cstdio>

namespace {

entt::entity toEntity(uint32_t v) { return static_cast<entt::entity>(v); }

TransformComponent* transformOf(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	return reg.valid(e) ? reg.try_get<TransformComponent>(e) : nullptr;
}

} // namespace

namespace ScriptApi
{

void log(const char* message)
{
	std::printf("[Script] %s\n", message ? message : "");
}

std::string getName(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return {};
	const auto* nc = reg.try_get<NameComponent>(e);
	return nc ? nc->name : std::string{};
}

glm::vec3 getPosition(HorizonWorld& world, uint32_t entityId)
{
	const auto* t = transformOf(world, entityId);
	return t ? t->position : glm::vec3(0.0f);
}

void setPosition(HorizonWorld& world, uint32_t entityId, const glm::vec3& p)
{
	if (auto* t = transformOf(world, entityId)) { t->position = p; t->dirty = true; }
}

glm::vec3 getRotation(HorizonWorld& world, uint32_t entityId)
{
	const auto* t = transformOf(world, entityId);
	return t ? t->rotation : glm::vec3(0.0f);
}

void setRotation(HorizonWorld& world, uint32_t entityId, const glm::vec3& r)
{
	if (auto* t = transformOf(world, entityId)) { t->rotation = r; t->dirty = true; }
}

glm::vec3 getScale(HorizonWorld& world, uint32_t entityId)
{
	const auto* t = transformOf(world, entityId);
	return t ? t->scale : glm::vec3(1.0f);
}

void setScale(HorizonWorld& world, uint32_t entityId, const glm::vec3& s)
{
	if (auto* t = transformOf(world, entityId)) { t->scale = s; t->dirty = true; }
}

uint32_t spawn(HorizonWorld& world, uint32_t parentId, const std::string& name)
{
	entt::entity parent = toEntity(parentId);
	entt::entity child  = world.createEntity(name.empty() ? "Entity" : name);
	if (world.registry().valid(parent))
		world.reparentEntity(child, parent);
	return static_cast<uint32_t>(child);
}

void destroy(HorizonWorld& world, uint32_t entityId)
{
	entt::entity e = toEntity(entityId);
	if (world.registry().valid(e))
		world.destroyEntity(e);
}

RaycastResult raycast(PhysicsWorld* physics, const glm::vec3& origin,
                      const glm::vec3& dir, float maxDist)
{
	RaycastResult out;
	if (!physics) return out;
	PhysicsWorld::RaycastHit hit = physics->raycast(origin, dir, maxDist);
	if (!hit.hit) return out;
	out.hit      = true;
	out.entityId = hit.entityId;
	out.point    = hit.point;
	out.normal   = hit.normal;
	out.distance = hit.distance;
	return out;
}

void setVelocity(PhysicsWorld* physics, uint32_t entityId, const glm::vec3& v)
{
	if (physics) physics->setCharacterVelocity(entityId, v);
}

bool isGrounded(PhysicsWorld* physics, uint32_t entityId)
{
	return physics && physics->isCharacterGrounded(entityId);
}

bool setMaterialParam(HorizonWorld& world, ContentManager* content,
                      uint32_t entityId, const std::string& name, const glm::vec4& value)
{
	if (!content) return false;
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return false;
	const auto* mc = reg.try_get<MaterialComponent>(e);
	if (!mc) return false;
	const float v[4] = { value.x, value.y, value.z, value.w };
	return content->setMaterialParam(mc->materialAssetId, name, v, 4);
}

glm::vec4 getMaterialParam(HorizonWorld& world, ContentManager* content,
                           uint32_t entityId, const std::string& name)
{
	if (!content) return glm::vec4(0.0f);
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return glm::vec4(0.0f);
	const auto* mc = reg.try_get<MaterialComponent>(e);
	if (!mc) return glm::vec4(0.0f);
	float out[4] = { 0, 0, 0, 0 };
	content->getMaterialParam(mc->materialAssetId, name, out);
	return glm::vec4(out[0], out[1], out[2], out[3]);
}

// ── In-game UI ────────────────────────────────────────────────────────────────

void setUIText(HorizonWorld& world, uint32_t entityId, const std::string& text)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return;
	if (auto* t = reg.try_get<UITextComponent>(e)) t->text = text;
}

std::string getUIText(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return {};
	const auto* t = reg.try_get<UITextComponent>(e);
	return t ? t->text : std::string();
}

void setUIColor(HorizonWorld& world, uint32_t entityId, const glm::vec4& c)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return;
	if (auto* img = reg.try_get<UIImageComponent>(e))  { img->tint = c; return; }
	if (auto* txt = reg.try_get<UITextComponent>(e))   { txt->color = c; return; }
	if (auto* btn = reg.try_get<UIButtonComponent>(e)) { btn->normalColor = c; }
}

glm::vec4 getUIColor(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return glm::vec4(1.0f);
	if (const auto* img = reg.try_get<UIImageComponent>(e))  return img->tint;
	if (const auto* txt = reg.try_get<UITextComponent>(e))   return txt->color;
	if (const auto* btn = reg.try_get<UIButtonComponent>(e)) return btn->normalColor;
	return glm::vec4(1.0f);
}

void setUIVisible(HorizonWorld& world, uint32_t entityId, bool visible)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return;
	if (auto* el = reg.try_get<UIElementComponent>(e)) el->active = visible;
}

bool isUIVisible(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return false;
	const auto* el = reg.try_get<UIElementComponent>(e);
	return el && el->active;
}

void setUIPosition(HorizonWorld& world, uint32_t entityId, const glm::vec2& p)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return;
	if (auto* el = reg.try_get<UIElementComponent>(e)) el->position = p;
}

glm::vec2 getUIPosition(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return glm::vec2(0.0f);
	const auto* el = reg.try_get<UIElementComponent>(e);
	return el ? el->position : glm::vec2(0.0f);
}

void setUISize(HorizonWorld& world, uint32_t entityId, const glm::vec2& sz)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return;
	if (auto* el = reg.try_get<UIElementComponent>(e)) el->size = sz;
}

glm::vec2 getUISize(HorizonWorld& world, uint32_t entityId)
{
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return glm::vec2(0.0f);
	const auto* el = reg.try_get<UIElementComponent>(e);
	return el ? el->size : glm::vec2(0.0f);
}

bool setUIMaterialParam(HorizonWorld& world, ContentManager* content,
                        uint32_t entityId, const std::string& name, const glm::vec4& value)
{
	if (!content) return false;
	auto& reg = world.registry();
	entt::entity e = toEntity(entityId);
	if (!reg.valid(e)) return false;
	const auto* img = reg.try_get<UIImageComponent>(e);
	if (!img) return false;
	const float v[4] = { value.x, value.y, value.z, value.w };
	return content->setMaterialParam(img->materialAssetId, name, v, 4);
}

// ── Live widgets ──────────────────────────────────────────────────────────────

int createWidget(HorizonWorld& world, ContentManager* content, const std::string& path)
{
	if (!content) return 0;
	return world.widgets().createWidget(*content, path);
}

void destroyWidget(HorizonWorld& world, int widgetId)   { world.widgets().destroyWidget(widgetId); }
void showWidget(HorizonWorld& world, int widgetId)      { world.widgets().showWidget(widgetId); }
void hideWidget(HorizonWorld& world, int widgetId)      { world.widgets().hideWidget(widgetId); }
void setWidgetZOrder(HorizonWorld& world, int widgetId, int z) { world.widgets().setZOrder(widgetId, z); }
bool isWidgetVisible(HorizonWorld& world, int widgetId) { return world.widgets().isVisible(widgetId); }

bool callWidgetFunction(HorizonWorld& world, int widgetId, const std::string& fn)
{
	return world.widgets().callFunction(widgetId, fn);
}

// ── Cursor ────────────────────────────────────────────────────────────────────

namespace { std::function<void(bool)> g_cursorHook; }

void setCursorHook(std::function<void(bool)> hook) { g_cursorHook = std::move(hook); }
void setCursorVisible(bool show) { if (g_cursorHook) g_cursorHook(show); }

} // namespace ScriptApi


