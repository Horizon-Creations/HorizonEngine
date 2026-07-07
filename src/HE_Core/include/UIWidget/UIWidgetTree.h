#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace HE {

// ── UI widget tree ───────────────────────────────────────────────────────────
// Source-of-truth data model of a UI Widget asset (the UMG-style widget editor
// edits this; JSON in CHUNK_UIWT). At play start the tree is instantiated into
// regular UI entities (UIElementComponent + UIText/UIImage/UIButton +
// ScriptComponent), so rendering, scripting and the inspector all reuse the
// existing paths. HE_Core cannot see HE_Scene's UIAnchor enum, so anchor is a
// raw uint8_t with the same 9-point ordering (0 = TopLeft … 8 = BottomRight).

enum class UIWidgetType : uint8_t
{
    Panel  = 0,  // colored quad; container for children (alpha 0 = pure container)
    Image  = 1,  // quad with tint + optional material
    Text   = 2,  // text run
    Button = 3,  // interactive quad (normal/hovered/pressed) + optional label
};

struct UIWidgetNode
{
    int          id       = 0;
    int          parentId = 0;              // 0 = direct child of the canvas
    UIWidgetType type     = UIWidgetType::Panel;
    std::string  name;

    // Layout (canvas units; anchor is relative to the parent rect, or to the
    // canvas for root nodes).
    float   posX = 0.0f,   posY = 0.0f;
    float   sizeX = 100.0f, sizeY = 30.0f;
    float   pivotX = 0.5f, pivotY = 0.5f;
    uint8_t anchor = 0;                     // UIAnchor ordering
    int     layer  = 0;
    bool    visible = true;

    // Visuals. `color` is the tint (Panel/Image), text color (Text) or the
    // button's normal color (Button).
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float hoveredColor[4] = { 0.30f, 0.30f, 0.30f, 1.0f };
    float pressedColor[4] = { 0.10f, 0.10f, 0.10f, 1.0f };

    std::string text;                       // Text / Button label
    float       fontSize = 14.0f;
    float       textColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // Button label color

    // Content-relative asset paths, resolved to UUIDs at instantiation time.
    std::string materialPath;               // custom material on the quad
    std::string scriptPath;                 // per-element behavior script
};

struct UIWidgetTree
{
    float canvasWidth  = 1920.0f;
    float canvasHeight = 1080.0f;
    std::vector<UIWidgetNode> nodes;        // order = sibling draw/hierarchy order
    int nextId = 1;

    UIWidgetNode*       findNode(int id);
    const UIWidgetNode* findNode(int id) const;
    // All direct children of `parentId` (0 = canvas roots), in vector order.
    std::vector<int> childrenOf(int parentId) const;
    // True when `ancestorId` is `id` itself or one of its ancestors (guards
    // against reparent cycles).
    bool isDescendantOf(int id, int ancestorId) const;
    // Remove a node and its whole subtree.
    void removeSubtree(int id);
    int  addNode(UIWidgetNode node);        // assigns id from nextId, returns it
};

// JSON round-trip (schema-evolution friendly: unknown keys ignored, missing
// keys default). Returns false on parse failure, leaving `out` untouched.
std::string uiWidgetTreeToJson(const UIWidgetTree& tree);
bool        uiWidgetTreeFromJson(const std::string& json, UIWidgetTree& out);

// ── Layout (shared by the widget editor and the runtime) ────────────────────
// A node's rect in CANVAS units, resolved through the parent chain: the anchor
// point lies inside the parent rect (roots anchor to the canvas), position is
// the offset from it, pivot shifts the rect so the pivot point lands there.
struct UIWidgetRect { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; };
UIWidgetRect uiWidgetNodeRect(const UIWidgetTree& tree, const UIWidgetNode& n);
// False when the node or any ancestor is invisible.
bool uiWidgetNodeEffectiveVisible(const UIWidgetTree& tree, const UIWidgetNode& n);

} // namespace HE
