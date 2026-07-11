#pragma once
#include <imgui.h>
#include <array>
#include <functional>
#include <string>
#include <vector>

// ── GraphEditor ──────────────────────────────────────────────────────────────
// A shared ImGui node-graph canvas used by BOTH the material node editor and the
// HorizonCode (visual scripting) editor, so the two look and behave identically.
// The component owns the common frontend — pan/zoom, grid, node boxes, pins,
// bezier links, drag-to-connect, node move, single/multi/box selection, delete,
// and the right-click add-node popup — while the host adapts its own graph
// (material shader nodes vs HorizonCode code nodes) through the Model callbacks.
// Host-specific chrome (comments, per-node parameter widgets, previews) is drawn
// through the body/decoration hooks in the SAME canvas transform.

namespace GraphEditor {

// One pin as the host describes it for a node this frame. `id` is the host's own
// pin index (material: per-side index; HorizonCode: unified index) — the
// component passes it back verbatim to connect()/clearPinLinks() and matches
// link endpoints by (side, id). Pins are laid out top-to-bottom per side in the
// order the host returns them.
struct Pin
{
    int         id;
    std::string label;
    ImU32       color;
    bool        input;   // left column (true) or right column (false)
    bool        isExec;  // draw as a triangle (exec flow) vs a circle (data)
    bool        isArray = false; // draw as a 2×2 grid (array of the data type)
};

// Persistent per-graph canvas state. The host owns one and passes it each frame;
// the component reads and mutates it (pan/zoom/selection/drag).
struct State
{
    ImVec2 pan  = ImVec2(40.0f, 40.0f);
    float  zoom = 1.0f;

    int              selected = 0;   // primary selection (0 = none)
    std::vector<int> selection;      // all selected ids (multi-select)

    // Set by the host to recenter on a node next frame (e.g. selected from a
    // side panel); consumed by the component.
    int  focusNode = 0;
    // Host sets this before draw() to skip canvas mouse interaction for one
    // frame (e.g. while it is dragging a comment box). Drawing still happens.
    bool suppressInteraction = false;

    // ── Internal interaction state (component-owned) ──
    int    dragNode = 0;             // node being moved (0 = none)
    ImVec2 dragStartMouse;
    ImVec2 dragStartPos;             // graph-space
    bool   dragMoved = false;
    // Pressed a node that was ALREADY part of the multi-selection (no shift): the
    // whole group drags; a click with no drag collapses to just this node on
    // release. 0 = the press started a fresh single selection.
    int    selectClickNode = 0;
    int    linkSrcNode = 0, linkSrcPin = 0;
    bool   linkSrcInput = false;
    bool   linkGrab = false;         // this drag detached an existing link (removal)
    bool   boxSel = false;
    ImVec2 boxStart;                 // screen-space
    ImVec2 addMenuGraphPos;          // graph-space drop point for the add popup
    int    ctxNode = 0;              // node whose right-click context menu is open
    // Hover-tooltip bookkeeping: node under the cursor last frame + how long
    // the cursor has rested on it (the tooltip shows after a short delay).
    int    hoverNode = 0;
    float  hoverTime = 0.0f;
    // Source pin of a link drag that was released on empty canvas (opens the
    // filtered drag-off menu next frame).
    int    dragOffNode = 0, dragOffPin = 0;
    bool   dragOffInput = false;
};

// The host bridges its graph to the canvas through these callbacks. Required
// ones are marked; the rest are optional (leave empty to disable that feature).
struct Model
{
    // ── Required ──
    std::function<std::vector<int>()>         nodeIds;      // all node ids
    std::function<void(int id, float& x, float& y)> getPos; // graph-space position
    std::function<void(int id, float x, float y)>   setPos;
    std::function<std::string(int id)>        title;        // header text
    std::function<ImU32(int id)>              headerColor;
    std::function<std::vector<Pin>(int id)>   pins;
    // Links as (srcNode, srcPin, dstNode, dstPin); src is an OUTPUT pin, dst an INPUT.
    std::function<std::vector<std::array<int, 4>>()> links;
    // Connect an output→input pin (already oriented by the component). The host
    // validates types and returns whether it connected.
    std::function<bool(int outNode, int outPin, int inNode, int inPin)> connect;

    // ── Optional ──
    std::function<void(int node, int pin, bool input)> clearPinLinks; // Alt+click a pin
    std::function<void(int id)>  removeNode;                          // Delete
    // Draw the host's own add-node menu items (called inside a BeginPopup). The
    // graph-space drop point is in state.addMenuGraphPos. Return the new node id
    // (for auto-select), or 0.
    std::function<int()>         drawAddMenu;
    // Extra body height (graph units) to reserve under the pins for on-node
    // widgets, and the callback that draws them. bodyMin/Max are screen px.
    std::function<float(int id)> nodeBodyHeight;
    std::function<void(int id, ImVec2 bodyMin, ImVec2 bodyMax, float zoom)> drawNodeBody;
    // Inline default editors on UNCONNECTED data-input pins (simple types): the
    // component places a small widget next to the pin label and calls the draw
    // callback inside it (font already scaled to the zoom). Wired pins never
    // show one. `pinHasInlineEditor` decides per (node, pin).
    std::function<bool(int node, int pin)> pinHasInlineEditor;
    std::function<void(int node, int pin)> drawPinInlineEditor;
    // Decorations drawn behind / in front of the nodes in the canvas transform
    // (comments, preview halos). origin/pan/zoom map graph→screen:
    // screen = origin + pan + graph*zoom.
    std::function<void(ImDrawList*, ImVec2 origin, ImVec2 pan, float zoom)> drawBehind;
    std::function<void(ImDrawList*, ImVec2 origin, ImVec2 pan, float zoom)> drawFront;
    // Runs BEFORE node interaction (host-owned chrome such as comment boxes).
    // `hovered` = the canvas is hovered this frame; return true to consume the
    // mouse so the component skips its own node/selection/pan handling.
    std::function<bool(ImVec2 origin, ImVec2 pan, float zoom, bool hovered)> interactBehind;
    // Optional per-node outline color (0 = none): drawn as a thick halo around
    // the node box, over the normal border. Used for error/status markers
    // (e.g. a HorizonCode compile error anchored to a node).
    std::function<ImU32(int nodeId)> nodeOutline;
    // Hover tooltip: text shown after the cursor rests on a node for a moment
    // (what the node does, its inputs/outputs). Empty string = no tooltip.
    std::function<std::string(int nodeId)> nodeTooltip;
    // Right-click on a node opens a popup; the host draws its items here.
    std::function<void(int nodeId)> drawNodeContextMenu;
    // Double-click on a node (e.g. open a referenced function).
    std::function<void(int nodeId)> onNodeDoubleClick;
    // A link dragged off (srcNode, srcPin) and released on EMPTY canvas: the host
    // shows a menu filtered to nodes compatible with that pin, creates one, and
    // connects it. srcInput = the source pin is an input (so the new node feeds
    // it). Returns the new node id (auto-selected), or 0. When unset, an empty
    // drag just cancels.
    std::function<int(int srcNode, int srcPin, bool srcInput, ImVec2 graphPos)> drawPinDragMenu;

    // Accept ImGui drag-drop payloads dropped onto the canvas (e.g. an element or
    // a variable). The component makes the canvas a drop target for each listed
    // payload type and calls `onDrop` with the matched type + data + graph point.
    std::vector<const char*> dropPayloads;
    std::function<void(const char* type, const void* data, ImVec2 graphPos)> onDrop;

    // Feature flags.
    bool multiSelect = false;
    // Render exec-less, body-less nodes (pure getters / literals) as compact
    // chips (no header bar, fit-to-content width). Off by default so data-flow
    // graphs like the material editor keep their normal framed nodes.
    bool compactPureNodes = false;
};

// Draw the canvas + handle interaction inside the current window, filling
// `size`. `id` scopes ImGui ids + popups. Returns true when the host graph was
// mutated this frame (the host should snapshot/commit for undo).
bool draw(const char* id, const Model& model, State& state, const ImVec2& size);

// Shared visual constants (exposed so hosts can align their side panels / body
// widgets to the same metrics).
constexpr float kNodeW  = 176.0f;
constexpr float kTitleH = 24.0f;
constexpr float kRowH   = 20.0f;
constexpr float kPinR   = 5.0f;

// Standard category → header color and pin-type → color helpers, so both
// editors share one palette. Hosts map their own categories/types onto these.
ImU32 categoryColor(const char* category);

} // namespace GraphEditor
