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
    int    linkSrcNode = 0, linkSrcPin = 0;
    bool   linkSrcInput = false;
    bool   boxSel = false;
    ImVec2 boxStart;                 // screen-space
    ImVec2 addMenuGraphPos;          // graph-space drop point for the add popup
    int    ctxNode = 0;              // node whose right-click context menu is open
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
    // Decorations drawn behind / in front of the nodes in the canvas transform
    // (comments, preview halos). origin/pan/zoom map graph→screen:
    // screen = origin + pan + graph*zoom.
    std::function<void(ImDrawList*, ImVec2 origin, ImVec2 pan, float zoom)> drawBehind;
    std::function<void(ImDrawList*, ImVec2 origin, ImVec2 pan, float zoom)> drawFront;
    // Runs BEFORE node interaction (host-owned chrome such as comment boxes).
    // `hovered` = the canvas is hovered this frame; return true to consume the
    // mouse so the component skips its own node/selection/pan handling.
    std::function<bool(ImVec2 origin, ImVec2 pan, float zoom, bool hovered)> interactBehind;
    // Right-click on a node opens a popup; the host draws its items here.
    std::function<void(int nodeId)> drawNodeContextMenu;
    // Double-click on a node (e.g. open a referenced function).
    std::function<void(int nodeId)> onNodeDoubleClick;

    // Accept ImGui drag-drop payloads dropped onto the canvas (e.g. an element or
    // a variable). The component makes the canvas a drop target for each listed
    // payload type and calls `onDrop` with the matched type + data + graph point.
    std::vector<const char*> dropPayloads;
    std::function<void(const char* type, const void* data, ImVec2 graphPos)> onDrop;

    // Feature flags.
    bool multiSelect = false;
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
