#pragma once
#include "GraphEditor.h"
#include <HorizonCode/HorizonCode.h>
#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

class ContentManager;

// ── HcGraphHost ──────────────────────────────────────────────────────────────
// The HorizonCode half of the shared node canvas. GraphEditor draws ANY graph
// (it is shared with the material editor); this layer is the part that knows
// HorizonCode — unified pin layout, the searchable add-node palette, the
// filtered drag-off-a-pin menu and the node clipboard shortcuts — and it is the
// same for every HorizonCode editor: the Level Script / Game Instance /
// HorizonCode Class tabs and the UI Widget graph.
//
// On the CANVAS the frontends genuinely differ in only three things, so those
// are all the hooks there are: how a node is titled (the widget editor appends
// the bound element), how an edit is recorded (a scene-undo snapshot vs the
// widget's own undo stack) and which node types the menus offer (a level script
// has no self-widget and no element properties).
//
// The SIDE PANELS are only partly shared: drawCommonNodeDetails (below) draws the
// node-detail rows that every frontend spells identically, but each frontend
// still owns its remaining detail rows and its whole variables/functions list —
// those differ in layout, and the widget editor's list also enumerates the UI
// elements. See the comment on drawCommonNodeDetails for the exact split.
namespace HcGraphHost
{
namespace HC = HorizonCode;

// ── Node plumbing (all derived from HC::signatureOf) ─────────────────────────

// Unified pin index layout: [execIns][execOuts][dataIns][dataOuts].
struct PinRanges { int execIn0, execOut0, dataIn0, dataOut0, end; };
PinRanges pinRanges(const HC::Node& n);

// Display name for a HorizonCode data pin type (used by the variables UI).
const char* pinTypeName(HC::PinType t);

// Pins for the GraphEditor, in unified index order (positions are laid out by
// the canvas itself, so only id/label/type/side/exec-ness are provided).
std::vector<GraphEditor::Pin> nodePins(const HC::Node& n);

void removePinLinks(HC::Graph& g, int nodeId, int pin);

std::string uniqueFunctionName(const HC::Graph& g);
std::string uniqueVarName(const HC::Graph& g);

// How a variable's type reads in a list ("Float", "PlayerState", "Int[]") — an
// Object variable shows its class, not a bare "Object".
std::string variableTypeLabel(const HC::Variable& v);

// Add a node at `pos`, owned by the visible sub-graph.
int addNode(HC::Graph& g, HC::NodeType type, const ImVec2& pos, int subgraph);

// Search helper for the menus.
std::string lower(std::string v);

// Load a class/widget asset's graph (for enumerating its public members). A
// widget is a first-class object too, so its logic graph counts as a "class".
bool loadClassGraph(ContentManager* content, const std::string& path, HC::Graph& out);

// The class graph the Ref output of `srcNode` points to (self / GameInstance /
// a typed Object variable / Create Object), or null when the class is unknown.
const HC::Graph* resolveClassGraph(const HC::Node& srcNode, const HC::Graph& selfGraph,
                                   const HC::Graph* giGraph, ContentManager* content,
                                   HC::Graph& scratch);

// The ENGINE BASE CLASS behind that same Ref output ("" when unknown or plain
// Object). The graph above carries a class's authored members; this carries the
// ones its base class brings with it, which are HE::api rows rather than nodes
// in any graph — see HorizonCode::engineClassMembers.
std::string resolveClassBase(const HC::Node& srcNode, const HC::Graph& selfGraph,
                             const std::string& selfBaseClass, ContentManager* content);

// ── Host bindings ────────────────────────────────────────────────────────────

// Which node types each frontend offers. Kept as plain data (not behaviour) so
// the menus themselves stay shared.
struct MenuOpts
{
	// Add-menu sections, in display order (HC::nodeCategory names).
	std::vector<const char*> addCategories;
	// Node types the add menu never offers generically (they are created
	// through a dedicated path, or make no sense in this frontend).
	std::vector<HC::NodeType> addExcluded;
	// Same for the drag-off menu, which walks the whole registry rather than
	// per category — so every exclusion here is live.
	std::vector<HC::NodeType> dragExcluded;
};

// What the shared canvas needs from the editor embedding it.
struct Host
{
	HC::Graph*          graph        = nullptr; // the graph being edited
	GraphEditor::State* ge           = nullptr; // shared canvas state
	int*                selectedNode = nullptr; // host-side primary selection
	int                 currentGraph = 0;       // visible sub-graph (0 = event graph)
	ContentManager*     content      = nullptr;
	const HC::Graph*    giGraph      = nullptr; // for Get Game Instance member menus
	// The engine base class the edited graph derives from ("" = Object). Lets a
	// drag off Get Self offer the base's built-in members — Possess on a
	// PlayerController, Get Owning Entity on any Entity. Only the class tab has
	// one; a widget, a level script and the GameInstance leave it empty.
	std::string         selfBaseClass;
	// Node whose compile error gets a red halo (0 = none).
	int                 errorNode    = 0;
	// Header text for a node.
	std::function<std::string(const HC::Node&)> title;
	// An edit happened. committed = this is an undo/snapshot point (a finished
	// edit); false = a value is still being dragged (mark dirty, don't snapshot).
	std::function<void(bool committed)> onEdit;
	const MenuOpts*     menus        = nullptr;
};

// The fully wired canvas model. The host only adds its own add-menu
// (`drawAddMenu`) and drag-drop payloads afterwards; `h` must outlive the
// GraphEditor::draw call that uses the returned model.
GraphEditor::Model buildModel(const Host& h);

// ── Add menu ─────────────────────────────────────────────────────────────────
// Split in three so each host can slot its OWN Events section in the middle
// (world events / lifecycle events differ per frontend) without duplicating the
// search box or the rest of the palette.
// Draws the search field and opens the scrolling list; returns the lowercased
// query for `drawAddMenuTail` and the host's own matching.
std::string beginAddMenu();
// Generic node categories + Call/Return + the engine API + Get/Set Variable.
// Returns the id of a node created this frame, else 0.
int  drawAddMenuTail(const Host& h, const std::string& lowerQuery);
void endAddMenu();

// ── Node details ─────────────────────────────────────────────────────────────
// The detail rows that are word-for-word the same in every HorizonCode frontend,
// drawn from `h.graph` / `h.content` and reported through `h.onEdit`. Returns
// true when `n` was drawn here; false means the frontend has to draw it itself.
//
// Covered: the array element-type picker (ArrayMake…ForEach), every Const*
// literal, Get/Set Variable, Function Return, Call External, Create Widget,
// Create Object, Get/Set External and Engine Call.
//
// NOT covered, because the frontends genuinely say different things and each
// still has its own case for them:
//   Event          — a level script / class picks from an event catalog (or names
//                    its own); a widget binds an ELEMENT and that element's events.
//   FunctionEntry  — different access labels and different "who can call this"
//                    hint (Lua/Python vs horizon.callWidgetFunction).
//   FunctionCall   — the level script hides unnamed functions, the widget does not.
//   BindEvent /
//   EmitEvent      — same widgets, but the hint says "script" vs "widget".
//   Get/SetProperty— widget-only (there are no UI elements in a level script).
// Adding a case here is only correct while every frontend wants it identically.
bool drawCommonNodeDetails(const Host& h, HC::Node& n);

// The event picker Emit/Bind Event share. Lists the graph's DECLARED events
// (Graph::events) so a name is chosen, not typed — the three places that used to
// spell the same string are now one list. Typing a new name declares it.
// Returns true when the node changed.
bool drawEventPicker(HC::Graph& g, HC::Node& n, const char* label);

// A link dragged off a pin and released on empty canvas: a menu filtered to
// everything that can take that pin, auto-wired on pick. Returns the new node
// id (auto-selected), or 0. Wired into Model::drawPinDragMenu by buildModel.
int drawPinDragMenu(const Host& h, int srcNode, int srcPin, bool srcInput, const ImVec2& pos);

// Everything keyboard-driven that has to happen AFTER GraphEditor::draw, in the
// canvas window: the node clipboard + duplicate shortcuts (Cmd on macOS, Ctrl
// elsewhere) and the quick-pick popup the G / Shift+G / E shortcuts request from
// inside the canvas. `canvasOrigin`/`avail` are the canvas rect (paste lands
// under the mouse when it is over the canvas, else in its centre).
//
// The rest of the shortcuts live where they belong: the node keys (B/S/D/…) are
// GraphEditor::Model::quickSpawns entries filled by buildModel, and the canvas
// itself owns Delete, Space, Ctrl+A, Home, F and Q.
void handleGraphKeys(const Host& h, const ImVec2& canvasOrigin, const ImVec2& avail);

} // namespace HcGraphHost
