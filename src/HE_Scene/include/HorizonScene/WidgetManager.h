#pragma once
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIWidgetGraph.h>
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
// callWidgetFunction. Each instance carries its own copy of the widget tree
// (mutable state: text/colors/layout/visibility) plus the logic graph, whose
// events fire from pointer input and the frame tick.
class WidgetManager
{
public:
    // Instantiate a widget asset (content-relative path). Resolves per-node
    // material references, fires EventConstruct, returns the widget id
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

    // Route a script call to a graph function. False when the widget or the
    // function is missing — or the function is not public (access modifier).
    bool callFunction(int id, const std::string& name);

    // Fire EventTick on every visible widget.
    void tick(float dt);

    // Pointer input in render-target pixels: hit-tests interactive elements
    // (buttons + elements bound by pointer-event nodes), drives button visual
    // states and fires Click/HoverEnter/HoverExit graph events. `valid` false
    // (mouse captured / off-viewport) clears hover. Returns true when the
    // pointer is over an interactive element (callers may swallow the click).
    bool processPointer(float vpWidth, float vpHeight,
                        float mouseX, float mouseY,
                        bool primaryDown, bool valid);

    // Append draw quads for all visible widgets, sorted by (zOrder, layer,
    // depth). Called AFTER the entity-UI extraction, so widgets draw on top.
    void extract(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out);

    void clear() { m_instances.clear(); }

private:
    struct Instance
    {
        int id = 0;
        int zOrder = 0;
        HE::UIWidgetTree      tree;   // live copy (graph + scripts mutate it)
        HE::UIWidgetGraph     graph;
        HE::UIWidgetSelfState self;   // visible flag (ShowSelf/HideSelf)
        bool constructed = false;
        // Pointer state
        int hoveredNode = 0;
        int pressedNode = 0;
        // Per-node button visual state: 0 normal, 1 hovered, 2 pressed.
        std::unordered_map<int, uint8_t> buttonState;
        // Resolved material references (tree node id → material asset).
        std::unordered_map<int, HE::UUID> materials;
    };

    Instance*       find(int id);
    const Instance* find(int id) const;
    // True when the element can receive pointer events (button, or bound by a
    // pointer-event node; an event node with elem 0 makes every element hot).
    static bool isInteractive(const Instance& w, const HE::UIWidgetNode& n);

    std::vector<Instance> m_instances;
    int  m_nextId  = 1;
    bool m_wasDown = false;
};
