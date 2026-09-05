#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;

// Language-neutral gameplay-script API — the single implementation behind the
// `horizon` module of every scripting backend (Lua today, Python next). Each
// backend is only a thin marshalling shim around these functions, which keeps
// the exposed API surface identical across languages by construction.
//
// Entity ids are the raw entt handle as uint32 (what scripts see as
// self.entityId / self.entity_id). Invalid ids are tolerated: getters return
// neutral defaults, setters are no-ops.
namespace ScriptApi
{
	void        log(const char* message);
	std::string getName(HorizonWorld& world, uint32_t entityId);

	glm::vec3 getPosition(HorizonWorld& world, uint32_t entityId); // default (0,0,0)
	void      setPosition(HorizonWorld& world, uint32_t entityId, const glm::vec3& p);
	glm::vec3 getRotation(HorizonWorld& world, uint32_t entityId); // Euler degrees, default (0,0,0)
	void      setRotation(HorizonWorld& world, uint32_t entityId, const glm::vec3& r);
	glm::vec3 getScale(HorizonWorld& world, uint32_t entityId);    // default (1,1,1)
	void      setScale(HorizonWorld& world, uint32_t entityId, const glm::vec3& s);

	// Create a named entity, reparented under parentId when valid. Returns the id.
	uint32_t spawn(HorizonWorld& world, uint32_t parentId, const std::string& name);
	void     destroy(HorizonWorld& world, uint32_t entityId);

	struct RaycastResult {
		bool      hit = false;
		uint32_t  entityId = 0;
		glm::vec3 point{0.0f};
		glm::vec3 normal{0.0f};
		float     distance = 0.0f;
	};
	// physics may be null (raycasting disabled) → miss.
	RaycastResult raycast(PhysicsWorld* physics, const glm::vec3& origin,
	                      const glm::vec3& dir, float maxDist);

	// Movement helpers; physics may be null (no-op / false). setVelocity drives
	// the entity's character controller when it has one and its rigid body
	// otherwise (PhysicsWorld::setVelocity decides) — so the call a script
	// already knows now pushes a crate too, without meaning anything different
	// on a character than it always did.
	void setVelocity(PhysicsWorld* physics, uint32_t entityId, const glm::vec3& v);
	bool isGrounded(PhysicsWorld* physics, uint32_t entityId);

	// Set/get a node-graph material parameter BY NAME on the entity's material at
	// runtime (the MaterialComponent's asset). All 4 vec4 components are written;
	// the shader reads only those its Param node uses, so passing a scalar as
	// (x,0,0,0) is safe. content may be null (no-op / false). Returns false if the
	// entity has no material or the parameter name is unknown.
	bool setMaterialParam(HorizonWorld& world, ContentManager* content,
	                      uint32_t entityId, const std::string& name, const glm::vec4& value);
	glm::vec4 getMaterialParam(HorizonWorld& world, ContentManager* content,
	                           uint32_t entityId, const std::string& name); // default (0,0,0,0)

	// ── In-game UI (entities carrying UI components) ───────────────────────
	// Text of a UITextComponent. Getter returns "" without one.
	void        setUIText(HorizonWorld& world, uint32_t entityId, const std::string& text);
	std::string getUIText(HorizonWorld& world, uint32_t entityId);
	// Primary color: image tint, text color, or button normal color (whichever
	// components exist — image first). Getter prefers the same order.
	void      setUIColor(HorizonWorld& world, uint32_t entityId, const glm::vec4& c);
	glm::vec4 getUIColor(HorizonWorld& world, uint32_t entityId); // default (1,1,1,1)
	// UIElementComponent active flag (hides the whole subtree when false).
	void setUIVisible(HorizonWorld& world, uint32_t entityId, bool visible);
	bool isUIVisible(HorizonWorld& world, uint32_t entityId);
	// UIElementComponent position/size in canvas units.
	void      setUIPosition(HorizonWorld& world, uint32_t entityId, const glm::vec2& p);
	glm::vec2 getUIPosition(HorizonWorld& world, uint32_t entityId);
	void      setUISize(HorizonWorld& world, uint32_t entityId, const glm::vec2& s);
	glm::vec2 getUISize(HorizonWorld& world, uint32_t entityId);
	// Set a node-graph material parameter on the entity's UI IMAGE material
	// (UIImageComponent.materialAssetId) — the UI counterpart of setMaterialParam.
	bool setUIMaterialParam(HorizonWorld& world, ContentManager* content,
	                        uint32_t entityId, const std::string& name, const glm::vec4& value);

	// ── Live widgets (WidgetManager — exist OUTSIDE the entity world) ───────
	// Instantiate a UI Widget asset by content-relative path; 0 on failure.
	int  createWidget(HorizonWorld& world, ContentManager* content, const std::string& path);
	void destroyWidget(HorizonWorld& world, int widgetId);
	void showWidget(HorizonWorld& world, int widgetId);
	void hideWidget(HorizonWorld& world, int widgetId);
	void setWidgetZOrder(HorizonWorld& world, int widgetId, int z);
	bool isWidgetVisible(HorizonWorld& world, int widgetId);
	// Call a PUBLIC graph function on the widget (the engine routes the call;
	// private functions and unknown names return false).
	bool callWidgetFunction(HorizonWorld& world, int widgetId, const std::string& fn);
	// Build the interface while it runs: graft a widget asset under the element
	// named `parentName` and get the new instance back, take one out again by
	// that id, or empty the parent. See WidgetManager::addChild — this is what
	// makes a list of arbitrary length possible at all.
	int  addWidgetChild(HorizonWorld& world, ContentManager* content, int widgetId,
	                    const std::string& parentName, const std::string& assetPath);
	bool removeWidgetChild(HorizonWorld& world, int widgetId, int childId);
	int  clearWidgetChildren(HorizonWorld& world, int widgetId, const std::string& parentName);

	// ── Lists (docs/he-apps-plan.md B2) ─────────────────────────────────────
	// A ListView holds a COUNT and a row template, never the items. Say how many
	// there are and fill in the rows it asks about (OnRowBind) — that is the
	// whole contract, and it is what makes ten thousand items cost ten rows.
	bool setListCount(HorizonWorld& world, int widgetId, const std::string& listName, int count);
	int  listCount(HorizonWorld& world, int widgetId, const std::string& listName);
	// The instance showing item `index` (0 = not realized right now), which is
	// what an OnRowBind handler writes into.
	int  listRow(HorizonWorld& world, int widgetId, const std::string& listName, int index);
	// Ask every realized row again — what a sort, a filter or an edit is written
	// with, since the list never saw the data in the first place.
	bool refreshList(HorizonWorld& world, int widgetId, const std::string& listName);
	bool setListSelected(HorizonWorld& world, int widgetId, const std::string& listName,
	                     int index, bool selected);
	int  listSelected(HorizonWorld& world, int widgetId, const std::string& listName);
	bool scrollListToItem(HorizonWorld& world, int widgetId, const std::string& listName,
	                      int index);

	// ── Animation (docs/he-apps-plan.md B8) ─────────────────────────────────
	// Move a property to a value over time, along a named curve. Three
	// entry points rather than one, because a number, a colour and a point are
	// three different pins in a graph and one row taking "some value" would be a
	// pin nobody can wire. The element is addressed by its designer NAME, like
	// every other script reach into a widget.
	bool animateNumber(HorizonWorld& world, int widgetId, const std::string& elemName,
	                   const std::string& prop, float to, float seconds,
	                   const std::string& easing);
	bool animateColor(HorizonWorld& world, int widgetId, const std::string& elemName,
	                  const std::string& prop, const glm::vec4& to, float seconds,
	                  const std::string& easing);
	bool animateVec2(HorizonWorld& world, int widgetId, const std::string& elemName,
	                 const std::string& prop, const glm::vec2& to, float seconds,
	                 const std::string& easing);
	// Stop what is running on one property, or on the whole element when `prop`
	// is empty. The value stays where it got to.
	int  stopAnimation(HorizonWorld& world, int widgetId, const std::string& elemName,
	                   const std::string& prop);

	// The widget's OWN named clips, made in the designer's timeline. `loop`
	// overrides what the clip says about looping.
	bool playClip(HorizonWorld& world, int widgetId, const std::string& clip, bool loop,
	              const std::string& direction = {});
	// `restore` puts the properties the clip drove back when it FINISHES (never
	// for a loop, which does not); `direction` is a UIAnimDirection name.
	bool playClipAsAuthored(HorizonWorld& world, int widgetId, const std::string& clip,
	                        bool restore = false, const std::string& direction = {});
	int  stopClip(HorizonWorld& world, int widgetId, const std::string& clip);
	bool isClipPlaying(HorizonWorld& world, int widgetId, const std::string& clip);
	// The widget a HorizonCode instance belongs to (0 = none). What lets a call
	// from a widget's own graph mean "this widget" with nothing wired.
	int  widgetOfScript(HorizonWorld& world, uint32_t scriptId);
	// The component embedded in the slot of that name, as a reference (0 = no
	// such slot). A component is another instance, so this is what its
	// functions, its events and its public variables are reached through.
	uint32_t childWidget(HorizonWorld& world, int widgetId, const std::string& element);
	// Everything moving in this widget, clips and single properties both.
	int  stopAllAnimations(HorizonWorld& world, int widgetId);
	// Stop everything and put every animated property back the way it was.
	int  restoreOriginalState(HorizonWorld& world, int widgetId);

	// ── Layers: dialogs, popups, menus (docs/he-apps-plan.md B4) ────────────
	// While one of these is up, input belongs to it and to nothing underneath.
	// A modal dims the screen and has to be answered; a popup dismisses itself
	// on a click somewhere else. closeTopLayer is what Escape does.
	void showModalWidget(HorizonWorld& world, int widgetId);
	void openWidgetPopup(HorizonWorld& world, int widgetId, float x, float y);
	void openWidgetPopupAtPointer(HorizonWorld& world, int widgetId);
	bool closeTopLayer(HorizonWorld& world);

	// ── Theme (what the whole application looks like) ───────────────────────
	// setThemeMode takes "Light", "Dark" or "System" — the last is a RULE, not a
	// colour, and is the default. setTheme loads a Theme asset by
	// content-relative path (false = no such asset or unreadable).
	bool setTheme(HorizonWorld& world, ContentManager* content, const std::string& path);
	void setThemeMode(HorizonWorld& world, const std::string& mode);
	std::string themeMode(HorizonWorld& world);       // resolved: "Light" or "Dark"
	std::string themePreference(HorizonWorld& world); // asked for: + "System"
	// The reader's text size: a factor on every authored font size and on
	// nothing else (docs/he-apps-plan.md B10). It sits beside the theme because
	// it is the same kind of setting — one switch a Preferences screen writes
	// for the whole application. Clamped to 0.5 … 3 on the way in, so what
	// fontScale() gives back is what is actually being drawn and not what was
	// asked for.
	void  setFontScale(HorizonWorld& world, float scale);
	float fontScale(HorizonWorld& world);
	// Is the pointer currently over an interactive widget element? The verdict
	// of the last WidgetManager::processPointer — what gameplay needs to skip a
	// click that belongs to the menu it just landed in.
	bool pointerOverUI(HorizonWorld& world);

	// ── Cursor (host-app hook) ──────────────────────────────────────────────
	// show = release the mouse capture and show the OS cursor (UI interaction);
	// hide = re-capture for FPS-style look. The host app (PIE / packaged game)
	// registers the hook; without one the calls are no-ops.
	void setCursorHook(std::function<void(bool)> hook);
	void setCursorVisible(bool show);
}
