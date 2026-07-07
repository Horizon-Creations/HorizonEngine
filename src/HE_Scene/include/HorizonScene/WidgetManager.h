#pragma once
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetBinding.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Renderer/UIRenderObject.h>
#include <Types/UUID.h>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager;

// Live UI widgets — created from UI Widget assets, existing OUTSIDE the entity
// world and rendered directly (no components, no entities). Owned by
// HorizonWorld only for lifetime convenience (cleared with the world, so PIE
// stop drops play-created widgets); scripts drive it through the horizon API:
// createWidget / showWidget / hideWidget / destroyWidget / setWidgetZOrder /
// callWidgetFunction. Each instance carries its own deep copy of the widget
// tree (mutable state: text/colors/layout/visibility) plus its HorizonCode
// graph, whose events fire from pointer input, keyboard and the frame tick.
class WidgetManager
{
public:
    // Instantiate a widget asset (content-relative path). Resolves per-element
    // material references, fires the "Construct" event, returns the widget id
    // (0 = asset missing or invalid tree).
    int createWidget(ContentManager& content, const std::string& assetPath);

    void destroyWidget(int id);
    void showWidget(int id);
    void hideWidget(int id);
    void setZOrder(int id, int z);

    bool isAlive(int id) const;
    bool isVisible(int id) const;
    int  zOrder(int id) const;
    size_t count() const { return m_instances.size(); }

    // Route a script call to a HorizonCode function. False when the widget or
    // the function is missing — or the function is not public (access modifier).
    bool callFunction(int id, const std::string& name);

    // Fire the "Tick" event (Float arg = dt) on every visible widget.
    void tick(float dt);

    // Pointer input in render-target pixels: hit-tests interactive elements
    // (buttons/checkboxes/sliders/combos/text fields + elements bound by
    // pointer-event nodes), drives element visual state and fires the matching
    // HorizonCode events. `valid` false (mouse captured / off-viewport) clears
    // hover. Returns true when the pointer is over an interactive element
    // (callers may swallow the click).
    bool processPointer(float vpWidth, float vpHeight,
                        float mouseX, float mouseY,
                        bool primaryDown, bool valid);

    // Keyboard routing for the focused TextInput (the apps call these). All are
    // no-ops when no TextInput is focused.
    void inputText(const std::string& utf8); // append text, fire OnTextChanged
    void inputBackspace();                    // drop last char, fire OnTextChanged
    void inputSubmit();                       // fire OnTextCommitted
    // True while a TextInput has keyboard focus — the apps use it to decide
    // whether to route text/keys here instead of to gameplay/camera.
    bool hasFocusedTextField() const { return m_focusWidget != 0; }

    // Append draw quads for all visible widgets, sorted by (zOrder, layer,
    // depth). Called AFTER the entity-UI extraction, so widgets draw on top.
    void extract(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out);

    void clear();

    // Route every widget's HorizonCode through this central runtime instead of
    // the manager's own. HorizonWorld injects the scene-wide runtime so widgets,
    // the level script (and later the GameInstance) share one interpreter and can
    // reference each other. Null (the default) falls back to an internal runtime
    // for standalone use / tests.
    void setRuntime(HorizonCode::Runtime* r) { m_runtime = r; }

private:
    struct Instance
    {
        int id = 0;
        int zOrder = 0;
        HE::UIWidgetTree  tree;    // live deep copy (scripts mutate it)
        // This widget's script instance in the runtime (owns the graph + the
        // private variable store); 0 = no logic graph.
        HorizonCode::InstanceId scriptId = 0;
        bool visible = true;       // ShowWidget/HideWidget nodes flip this
        // Transient interaction state (element ids; 0 = none).
        int hoveredElem   = 0;
        int pressedElem   = 0;
        int focusedElem   = 0;     // focused TextInput
        int draggingSlider = 0;    // slider being dragged
        // Resolved material references (element id → material asset).
        std::unordered_map<int, HE::UUID> materials;
    };

    Instance*       find(int id);
    const Instance* find(int id) const;
    Instance*       findByScript(HorizonCode::InstanceId scriptId);

    // The runtime widgets run on: the injected shared one, else the internal
    // fallback. Resolved on each call (never a stored self-pointer), so the
    // manager stays movable.
    HorizonCode::Runtime&       rt()       { return m_runtime ? *m_runtime : m_ownRuntime; }
    const HorizonCode::Runtime& rt() const { return m_runtime ? *m_runtime : m_ownRuntime; }
    // Host bindings shared by every widget instance (property get/set + show/
    // hide), disambiguated by the runtime InstanceId.
    HorizonCode::HostBindings makeBindings();

    // True when the element can receive pointer events: interactive by type, or
    // bound by a pointer-event Event node (an Event with elem 0 makes every
    // element hot).
    bool isInteractive(const Instance& w, const HE::UIElement& e) const;

    std::vector<Instance> m_instances;
    HorizonCode::Runtime  m_ownRuntime;        // fallback when none is injected
    HorizonCode::Runtime* m_runtime = nullptr; // injected shared runtime (null → own)
    bool m_wasDown = false;
    int  m_focusWidget = 0;        // widget id owning the focused TextInput
};
