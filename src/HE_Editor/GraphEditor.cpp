#include "GraphEditor.h"
#include "EditorInput.h" // pointer-device grammar (trackpad swipe pans, modifier-scroll zooms)
#include "EditorWidgets.h" // WrapText — header-only, so the test binary needs no extra link
#include <cstdint>
#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace GraphEditor {

ImU32 categoryColor(const char* category)
{
    const std::string c = category ? category : "";
    if (c == "Material")   return IM_COL32(140,  60,  60, 255);
    if (c == "Input")      return IM_COL32( 60, 100, 140, 255);
    if (c == "Math")       return IM_COL32( 60, 120,  80, 255);
    if (c == "Texture")    return IM_COL32(120,  90, 150, 255);
    if (c == "Parameter")  return IM_COL32(160, 110,  50, 255);
    if (c == "Procedural") return IM_COL32( 90, 130, 130, 255);
    if (c == "Channels")   return IM_COL32( 90,  90, 130, 255);
    if (c == "Function" || c == "Functions") return IM_COL32(150,  70, 110, 255);
    if (c == "Events")     return IM_COL32(150,  55,  55, 255);
    if (c == "Flow")       return IM_COL32( 92,  92,  98, 255);
    if (c == "Property")   return IM_COL32( 58, 100, 150, 255);
    if (c == "Widget")     return IM_COL32(150, 118,  55, 255);
    if (c == "Literals")   return IM_COL32( 58, 128,  82, 255);
    if (c == "Logic")      return IM_COL32( 48, 108,  70, 255);
    if (c == "String")     return IM_COL32(118,  68, 140, 255);
    if (c == "Debug")      return IM_COL32(120, 120,  68, 255);
    return IM_COL32(110, 110, 70, 255);
}

float pinHitRadius(float zoom)
{
    // Grows with the zoom like the dot itself, but never below a radius the
    // hand can actually aim at. 10 px at zoom 1 against a 5 px dot: double the
    // visible target, and exactly half of kRowH — so at zoom 1 the circles of
    // two neighbouring rows touch without overlapping, which is the largest a
    // pin can get before it starts arguing with the pin below it. Below zoom 1
    // the floor does make them overlap; that is what the nearest-centre rule in
    // pinAt is for.
    return std::max(kPinR * zoom + 5.0f, kRowH * 0.5f);
}

namespace
{
// Exec-less nodes (pure getters / literals) draw compactly: no colored header
// bar, a slim title, and a fit-to-content width — Unreal-style compact getters.
constexpr float kCompactTitleH = 15.0f;

// One node laid out for this frame.
struct Drawn
{
    int    id;
    ImVec2 pos;    // screen top-left
    ImVec2 size;   // screen
    float  gw = kNodeW;      // graph-space width
    float  gTitleH = kTitleH;// graph-space title-bar height
    bool   compact = false;  // no exec pins → compact style
    std::vector<Pin>    pins;
    std::vector<ImVec2> pinPos; // parallel to pins (screen)
};

bool isCompactNode(const std::vector<Pin>& pins)
{
    for (const auto& p : pins) if (p.isExec) return false;
    return !pins.empty(); // a node with no pins at all keeps the normal frame
}

float nodeGraphHeight(const Model& m, int id, const std::vector<Pin>& pins, float titleH)
{
    int left = 0, right = 0;
    for (const auto& p : pins) (p.input ? left : right)++;
    const int rows = std::max(left, right);
    float body = m.nodeBodyHeight ? m.nodeBodyHeight(id) : 0.0f;
    return titleH + rows * kRowH + body + 6.0f;
}

// Fit-to-content width for a compact node (title + widest input/output labels),
// clamped so it stays readable but never wider than a normal node.
float compactGraphWidth(const std::string& title, const std::vector<Pin>& pins)
{
    float tw = ImGui::CalcTextSize(title.c_str()).x;
    float maxL = 0.0f, maxR = 0.0f;
    for (const auto& p : pins)
    {
        const float w = p.label.empty() ? 0.0f : ImGui::CalcTextSize(p.label.c_str()).x;
        if (p.input) maxL = std::max(maxL, w); else maxR = std::max(maxR, w);
    }
    const float rowW = maxL + maxR + 30.0f; // pin circles + inner gap
    return std::clamp(std::max(tw + 18.0f, rowW), 92.0f, kNodeW);
}

void drawLink(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 col, float thick)
{
    const float dx = std::max(30.0f, std::fabs(b.x - a.x) * 0.5f);
    dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, col, thick);
}

const ImVec2* findPin(const Drawn& n, int pinId, bool input)
{
    for (size_t i = 0; i < n.pins.size(); ++i)
        if (n.pins[i].id == pinId && n.pins[i].input == input)
            return &n.pinPos[i];
    return nullptr;
}
} // namespace

bool draw(const char* id, const Model& model, State& st, const ImVec2& size)
{
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // The canvas button covers everything and is submitted FIRST, so without
    // this every on-node widget would be dead: ItemHoverable refuses a later
    // item while an earlier one owns HoveredId/ActiveId unless that earlier one
    // allowed overlap (imgui.cpp, ItemHoverable). The on-node editors used to
    // live in child windows, which sidestepped the question — the child was the
    // hovered WINDOW, so this button was never hoverable under it. They are
    // groups in this window now (that is what stopped them floating above every
    // node), which brings the question back and this is the answer to it.
    //
    // It also restores the other half of what the child window did: with
    // AllowOverlap, IsItemHovered() below reports false while a later item
    // covers the cursor, so editing a body widget does not also drag the node.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(id, size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    // The canvas button holds the active state while a right/middle drag that
    // STARTED on empty canvas is in progress — so panning keeps working even as
    // the cursor sweeps over nodes mid-drag.
    const bool canvasHeld = ImGui::IsItemActive();
    // Hover that survives a disabled scope. The whole tab renders inside
    // BeginDisabled while a collaboration peer holds the asset's lock, and a
    // disabled item is neither hovered nor active — so `hovered` and
    // `canvasHeld` above both go false and the canvas became impossible to move
    // around. Editing is what the lock forbids; LOOKING is not, and a graph you
    // cannot pan is one you cannot read.
    const bool hoveredEvenIfInert =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const ImVec2 mouse = ImGui::GetMousePos();

    st.liveEdit = false;   // set below if a gesture is mid-flight

    // Host chrome that must interact before the nodes (e.g. comment boxes). It
    // returns true to consume the mouse for this frame.
    bool behindConsumed = false;
    if (model.interactBehind)
        behindConsumed = model.interactBehind(origin, st.pan, st.zoom, hovered);

    // Deliberately NOT gated on IsAnyItemActive: the canvas InvisibleButton
    // itself becomes the active item on press, so that guard would make
    // `interact` false on the very click that should start a node drag /
    // box-select. Hovering an on-node widget already turns `hovered` off — see
    // the SetNextItemAllowOverlap above — so editing a body widget doesn't
    // trigger canvas interaction.
    const bool interact = hovered && !st.suppressInteraction && !behindConsumed;

    // Owning the keyboard is NOT the same as `interact`: that only means the
    // cursor is over the canvas. With a text field active anywhere (a node's
    // rename box, a variable name, the search field of a popup) the cursor is
    // usually still over the canvas — so an ordinary edit keystroke would
    // destroy the selection or drop a node. Every shortcut below is gated on
    // this, the same guard the material editor's shortcuts already use.
    //
    // The canvas button itself does NOT count as "something else is active" —
    // exactly the trap the comment above warns about for `interact`. Pressing
    // the mouse makes the InvisibleButton the active item, so a plain
    // IsAnyItemActive() is false precisely on the frame a hold-a-key-and-click
    // shortcut fires, which killed every one of them.
    const bool kbOwned = !ImGui::GetIO().WantTextInput &&
                         (!ImGui::IsAnyItemActive() || canvasHeld);

    // F is bound twice (see State::fSpawned): a fresh press starts out "not
    // spawned yet", and only a release still in that state frames the selection.
    // Key REPEAT has to be off here — a held F keeps re-firing IsKeyPressed by
    // default, which would wipe the "already spawned" mark while the key is
    // still down and frame the view the moment it comes up.
    if (kbOwned && ImGui::IsKeyPressed(ImGuiKey_F, /*repeat*/false)) st.fSpawned = false;

    // The quick-spawn entry whose key is held right now (modifiers must match
    // exactly, so Cmd+D stays Duplicate and never drops a Delay).
    auto heldSpawn = [&](bool pressedOnly) -> const QuickSpawn*
    {
        const ImGuiIO& kio = ImGui::GetIO();
        if (kio.KeyCtrl || kio.KeySuper || kio.KeyAlt) return nullptr;
        for (const QuickSpawn& qs : model.quickSpawns)
            if (qs.shift == kio.KeyShift &&
                (pressedOnly ? ImGui::IsKeyPressed(qs.key, /*repeat*/false)
                             : ImGui::IsKeyDown(qs.key)))
                return &qs;
        return nullptr;
    };

    // Canvas drop targets (elements, variables, …) — bound to the InvisibleButton.
    if (!model.dropPayloads.empty() && model.onDrop && ImGui::BeginDragDropTarget())
    {
        const ImVec2 gp((mouse.x - origin.x - st.pan.x) / st.zoom,
                        (mouse.y - origin.y - st.pan.y) / st.zoom);
        for (const char* pt : model.dropPayloads)
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(pt))
                { model.onDrop(pt, p->Data, gp); break; }
        ImGui::EndDragDropTarget();
    }

    auto toScreen = [&](float gx, float gy) {
        return ImVec2(origin.x + st.pan.x + gx * st.zoom, origin.y + st.pan.y + gy * st.zoom);
    };
    auto toGraph = [&](const ImVec2& p) {
        return ImVec2((p.x - origin.x - st.pan.x) / st.zoom, (p.y - origin.y - st.pan.y) / st.zoom);
    };

    // ── Layout every node for this frame ─────────────────────────────────────
    const std::vector<int> ids = model.nodeIds ? model.nodeIds() : std::vector<int>{};

    // ── The selection may only ever name nodes this graph is SHOWING ─────────
    // State is shared across everything drawn with it — the sub-graphs of one
    // graph (event graph vs. each function) and, in the HorizonCode panels, the
    // Level Script, the Game Instance and every class tab. Nothing cleared the
    // selection on a switch, so an id selected in one of them survived into the
    // next, where the same small integer is almost certainly a DIFFERENT node.
    //
    // Every consumer below takes the selection at its word: Delete and Cut
    // destroy it, Duplicate clones it, a drag writes positions into it. So a
    // selection made in one graph, followed by a switch and a single Delete,
    // silently removed nodes somewhere the user was not even looking — they
    // only found out on going back. Dropping what is not on screen is the one
    // place that fixes all of them at once.
    if (!st.selection.empty() || st.selected != 0)
    {
        const std::unordered_set<int> visible(ids.begin(), ids.end());
        st.selection.erase(std::remove_if(st.selection.begin(), st.selection.end(),
            [&](int id){ return !visible.count(id); }), st.selection.end());
        if (st.selected != 0 && !visible.count(st.selected))
            st.selected = st.selection.empty() ? 0 : st.selection.front();
    }

    // Links are needed twice: for the wires and to know which INPUT pins are
    // wired (unwired simple inputs show an inline default editor).
    const std::vector<std::array<int,4>> links =
        model.links ? model.links() : std::vector<std::array<int,4>>{};
    std::unordered_set<uint64_t> wiredInputs;
    wiredInputs.reserve(links.size());
    for (const auto& l : links)
        wiredInputs.insert(((uint64_t)(uint32_t)l[2] << 32) | (uint32_t)l[3]);
    auto pinInlineEditor = [&](int nid, int pin) {
        return model.pinHasInlineEditor && model.drawPinInlineEditor &&
               !wiredInputs.count(((uint64_t)(uint32_t)nid << 32) | (uint32_t)pin) &&
               model.pinHasInlineEditor(nid, pin);
    };

    std::vector<Drawn> nodes;
    nodes.reserve(ids.size());
    for (int nid : ids)
    {
        Drawn d;
        d.id = nid;
        float gx = 0, gy = 0;
        model.getPos(nid, gx, gy);
        d.pos  = toScreen(gx, gy);
        d.pins = model.pins(nid);
        const bool hasBody = model.nodeBodyHeight && model.nodeBodyHeight(nid) > 0.0f;
        d.compact = model.compactPureNodes && !hasBody && isCompactNode(d.pins);
        d.gTitleH = d.compact ? kCompactTitleH : kTitleH;
        d.gw = d.compact ? compactGraphWidth(model.title(nid), d.pins) : kNodeW;
        if (d.compact)
            for (const auto& p : d.pins)
                if (p.input && !p.isExec && pinInlineEditor(nid, p.id)) { d.gw += 64.0f; break; }
        const float gh = nodeGraphHeight(model, nid, d.pins, d.gTitleH);
        d.size = ImVec2(d.gw * st.zoom, gh * st.zoom);

        // Pin screen positions (top-to-bottom per side).
        const float w = d.gw * st.zoom, titleH = d.gTitleH * st.zoom, rowH = kRowH * st.zoom;
        int leftRow = 0, rightRow = 0;
        d.pinPos.resize(d.pins.size());
        for (size_t i = 0; i < d.pins.size(); ++i)
        {
            const bool inp = d.pins[i].input;
            const int  row = inp ? leftRow++ : rightRow++;
            d.pinPos[i] = ImVec2(inp ? d.pos.x : d.pos.x + w,
                                 d.pos.y + titleH + (row + 0.5f) * rowH);
        }
        nodes.push_back(std::move(d));
    }
    auto findNode = [&](int nid) -> Drawn* {
        for (auto& n : nodes) if (n.id == nid) return &n;
        return nullptr;
    };

    // ── Recenter on a focus node ─────────────────────────────────────────────
    if (st.focusNode != 0)
    {
        float gx = 0, gy = 0;
        model.getPos(st.focusNode, gx, gy);
        st.pan.x = size.x * 0.5f - gx * st.zoom - (kNodeW * st.zoom) * 0.5f;
        st.pan.y = size.y * 0.4f - gy * st.zoom;
        st.focusNode = 0;
    }

    // ── Wheel over the canvas ────────────────────────────────────────────────
    // Mouse grammar: wheel zooms about the cursor. Trackpad grammar: the
    // two-finger SWIPE pans (panning is the constant gesture on a graph, and
    // holding the pad pressed for a right-drag the whole time is what made it
    // exhausting), zoom moves behind Cmd/Ctrl+scroll — modifier checked first,
    // so a zoom can never fall through into a pan.
    //
    // Gated on ITS OWN hover test, not on `interact`: `interact` rides on the
    // canvas item's hover, which any node (or node-body widget) under the
    // cursor turns off — wheel input would die the moment the mouse crossed a
    // node. The wheel belongs to the canvas REGION: mouse inside the canvas
    // rect and over this window or a node-body child of it. An open popup (a
    // node's combo, the add-menu) is its own window, so IsWindowHovered goes
    // false and the popup keeps its wheel.
    //
    // An open popup takes the wheel outright: its list scrolls INSIDE a child,
    // and once that child hits its end ImGui hands the leftover wheel on — which
    // landed on the canvas and zoomed it out from under the menu. While a menu
    // is up the canvas is not what the wheel is for.
    const bool popupOpen =
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool wheelHovered =
        !popupOpen &&
        mouse.x >= origin.x && mouse.y >= origin.y &&
        mouse.x <  origin.x + size.x && mouse.y < origin.y + size.y &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (wheelHovered && !st.suppressInteraction && !behindConsumed)
    {
        ImGuiIO& gio = ImGui::GetIO();
        const bool zoomMod = gio.KeyCtrl || gio.KeySuper;
        if (EditorInput::trackpadActive() && !zoomMod)
        {
            constexpr float kSwipeToPx = 16.0f; // wheel units → canvas pixels
            st.pan.x += gio.MouseWheelH * kSwipeToPx;
            st.pan.y += gio.MouseWheel  * kSwipeToPx;
        }
        else if (gio.MouseWheel != 0.0f)
        {
            const ImVec2 before = toGraph(mouse);
            st.zoom = std::clamp(st.zoom * (1.0f + gio.MouseWheel * 0.1f), 0.3f, 2.5f);
            st.pan.x = mouse.x - origin.x - before.x * st.zoom;
            st.pan.y = mouse.y - origin.y - before.y * st.zoom;
        }
    }
    // ── Pan (right / middle drag) ────────────────────────────────────────────
    // Latched, so it keeps panning as the cursor sweeps over nodes; not gated
    // on `interact` so a node-body widget being active elsewhere doesn't freeze
    // the drag.
    //
    // The latch is our own rather than the canvas button's active state: a tab
    // a peer holds renders disabled, and then the button is never active and
    // never hovered. hoveredEvenIfInert is what keeps navigation alive there —
    // the disabled flag stops edits, and panning is not one.
    const bool panDrag = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                         ImGui::IsMouseDragging(ImGuiMouseButton_Right);
    if (!panDrag)
        st.panning = false;
    else if (!st.panning &&
             (canvasHeld || ((hovered || hoveredEvenIfInert) && !st.suppressInteraction)))
        st.panning = true;
    if (st.panning)
    {
        st.pan.x += ImGui::GetIO().MouseDelta.x;
        st.pan.y += ImGui::GetIO().MouseDelta.y;
    }

    // ── Background + grid ────────────────────────────────────────────────────
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(24, 24, 28, 255));
    const float grid = 32.0f * st.zoom;
    if (grid > 8.0f)
    {
        const float ox = std::fmod(st.pan.x, grid), oy = std::fmod(st.pan.y, grid);
        for (float x = origin.x + ox; x < origin.x + size.x; x += grid)
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), IM_COL32(255,255,255,10));
        for (float y = origin.y + oy; y < origin.y + size.y; y += grid)
            dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), IM_COL32(255,255,255,10));
    }
    // No manual clip rect around the whole node loop: the host already draws the
    // canvas inside a clipping child window. The on-node widgets push their own
    // rect per node instead (see the body block below) — they used to sit in
    // child windows, which is what put every one of them above every node.
    if (model.drawBehind) model.drawBehind(dl, origin, st.pan, st.zoom);

    // ── Links (behind nodes) ─────────────────────────────────────────────────
    // (links were fetched before the node loop)
    for (const auto& l : links)
    {
        const Drawn* sn = findNode(l[0]);
        const Drawn* dn = findNode(l[2]);
        if (!sn || !dn) continue;
        const ImVec2* a = findPin(*sn, l[1], false);
        const ImVec2* b = findPin(*dn, l[3], true);
        if (!a || !b) continue;
        ImU32 col = IM_COL32(200,200,200,220);
        for (size_t i = 0; i < sn->pins.size(); ++i)
            if (!sn->pins[i].input && sn->pins[i].id == l[1]) { col = sn->pins[i].color; break; }
        const bool exec = a->x == sn->pos.x + sn->size.x &&
                          [&]{ for (const auto& p : sn->pins) if (!p.input && p.id == l[1]) return p.isExec; return false; }();
        drawLink(dl, *a, *b, exec ? IM_COL32(235,235,235,220) : col, exec ? 3.0f : 2.0f);
    }

    // ── Pin hit-test (nearest centre wins) ───────────────────────────────────
    // NOT "first circle the cursor happens to be inside": the hit radius is
    // wider than the gap between two rows at low zoom, so the circles overlap
    // and a first-match test would hand every click in the overlap to whichever
    // node was drawn last. Nearest centre is the rule that lets the targets grow
    // without any pin stealing its neighbour's clicks.
    //
    // Ties (a cursor exactly between two pins) go to the topmost node, which is
    // why the walk is still back-to-front and the comparison strict.
    auto pinAt = [&](const ImVec2& m, int& outNode, int& outPin, bool& outInput) -> bool
    {
        const float r = pinHitRadius(st.zoom);
        float best = std::numeric_limits<float>::max();
        bool  found = false;
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
            for (size_t i = 0; i < it->pins.size(); ++i)
            {
                const ImVec2 pp = it->pinPos[i];
                const float dx = m.x - pp.x, dy = m.y - pp.y;
                const float d2 = dx*dx + dy*dy;
                if (d2 > r*r || d2 >= best) continue;
                best = d2;
                outNode = it->id; outPin = it->pins[i].id; outInput = it->pins[i].input;
                found = true;
            }
        return found;
    };

    // ── Pin grab, BEFORE the node loop ───────────────────────────────────────
    // A pin sits centred on the node's edge, so half of its hit circle is
    // outside the node rectangle. This used to live inside the node loop behind
    // an `overNode` test, which made that outer half dead — on an INPUT pin
    // that is the half the cursor comes from, which is why inputs were the
    // hardest thing in the editor to hit. The test runs over all nodes here, so
    // where the cursor is relative to any node's box no longer matters.
    bool consumed = false;
    if (interact && st.linkSrcNode == 0 && st.dragNode == 0 && !st.boxSel &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        int pn = 0, pp2 = 0; bool pin_in = false;
        if (pinAt(mouse, pn, pp2, pin_in))
        {
            st.linkGrab = false;
            if (ImGui::GetIO().KeyAlt) { if (model.clearPinLinks) { model.clearPinLinks(pn, pp2, pin_in); changed = true; } }
            else if (pin_in)
            {
                // Grab-and-drag: clicking a CONNECTED input pin detaches its
                // link and re-anchors the drag to the source, so you can drop
                // it elsewhere to rewire or on empty canvas to delete it.
                int srcN = 0, srcP = 0; bool found = false;
                for (const auto& l : links)
                    if (l[2] == pn && l[3] == pp2) { srcN = l[0]; srcP = l[1]; found = true; break; }
                if (found)
                {
                    if (model.clearPinLinks) model.clearPinLinks(pn, pp2, true);
                    st.linkSrcNode = srcN; st.linkSrcPin = srcP; st.linkSrcInput = false;
                    st.linkGrab = true; changed = true;
                }
                else { st.linkSrcNode = pn; st.linkSrcPin = pp2; st.linkSrcInput = true; }
            }
            else { st.linkSrcNode = pn; st.linkSrcPin = pp2; st.linkSrcInput = false; }
            // The same press must not also start a box-select or drop a
            // quick-spawn node: both of those read "no node under the cursor" as
            // "empty canvas", and a pin grab off the outer half of a pin is
            // exactly that case.
            consumed = true;
        }
    }

    // ── Nodes ────────────────────────────────────────────────────────────────
    int hoverNodeNow = 0; // topmost node under the cursor (nodes draw back-to-front)
    for (const Drawn& n : nodes)
    {
        const ImVec2 br(n.pos.x + n.size.x, n.pos.y + n.size.y);
        const bool sel = st.selected == n.id ||
            std::find(st.selection.begin(), st.selection.end(), n.id) != st.selection.end();

        const std::string title = model.title(n.id);
        if (n.compact)
        {
            // No colored header bar: a subtly accent-tinted rounded body + a small
            // centered title, so pure getters/literals read as compact chips.
            const ImU32 acc = model.headerColor(n.id);
            const ImU32 bodyCol = IM_COL32(
                40 + ((acc >> IM_COL32_R_SHIFT) & 0xFF) / 5,
                40 + ((acc >> IM_COL32_G_SHIFT) & 0xFF) / 5,
                44 + ((acc >> IM_COL32_B_SHIFT) & 0xFF) / 5, 245);
            dl->AddRectFilled(n.pos, br, bodyCol, 6.0f);
            dl->AddRect(n.pos, br, sel ? IM_COL32(255, 170, 40, 255) : IM_COL32(20, 20, 24, 255),
                        6.0f, 0, sel ? 2.0f : 1.0f);
            if (model.nodeOutline)
                if (const ImU32 oc = model.nodeOutline(n.id))
                    dl->AddRect(ImVec2(n.pos.x - 3, n.pos.y - 3), ImVec2(br.x + 3, br.y + 3),
                                oc, 7.0f, 0, 3.0f);
            // Title scales linearly with zoom (like the node box), and the centering
            // uses the SCALED text width — mixing the default-size CalcTextSize with
            // a scaled draw put the title off-center at any zoom ≠ 1.
            const float fs = 12.0f * st.zoom;
            const float tw = ImGui::CalcTextSize(title.c_str()).x * (fs / ImGui::GetFontSize());
            dl->AddText(nullptr, fs,
                        ImVec2(n.pos.x + (n.size.x - tw) * 0.5f, n.pos.y + 2 * st.zoom),
                        IM_COL32(225, 225, 228, 255), title.c_str());
        }
        else
        {
            dl->AddRectFilled(n.pos, br, IM_COL32(52, 52, 56, 245), 5.0f);
            dl->AddRectFilled(n.pos, ImVec2(br.x, n.pos.y + n.gTitleH * st.zoom),
                              model.headerColor(n.id), 5.0f, ImDrawFlags_RoundCornersTop);
            dl->AddRect(n.pos, br, sel ? IM_COL32(255, 170, 40, 255) : IM_COL32(20, 20, 24, 255),
                        5.0f, 0, sel ? 2.0f : 1.0f);
            if (model.nodeOutline)
                if (const ImU32 oc = model.nodeOutline(n.id))
                    dl->AddRect(ImVec2(n.pos.x - 3, n.pos.y - 3), ImVec2(br.x + 3, br.y + 3),
                                oc, 7.0f, 0, 3.0f);
            dl->AddText(nullptr, 13.0f * st.zoom,
                        ImVec2(n.pos.x + 6 * st.zoom, n.pos.y + 4 * st.zoom), IM_COL32(240,240,240,255), title.c_str());
        }

        for (size_t i = 0; i < n.pins.size(); ++i)
        {
            const Pin& p = n.pins[i];
            const ImVec2 pp = n.pinPos[i];
            // A data pin's shape says WHICH container it carries — the type system
            // tells an Array<T>, a Set<T> and a Map<K,T> apart, so the pin has to
            // as well, or the canvas hides the one thing a wire can fail on. All
            // three fill the same ±0.97·kPinR box the array grid always did, so a
            // node's pin column keeps its rhythm, and all three scale with the
            // zoom like every other glyph here. Only the GLYPH changes: the pin's
            // position and its hit circle are untouched (see pinAt).
            const Container ck = containerOf(p);
            if (p.isExec)
            {
                const float s = kPinR * st.zoom;
                dl->AddTriangleFilled(ImVec2(pp.x - s, pp.y - s), ImVec2(pp.x - s, pp.y + s),
                                      ImVec2(pp.x + s, pp.y), p.color);
            }
            else if (ck == Container::Array)
            {
                // Array pin: a 2×2 grid of small squares (Unreal-style container pin)
                // so a list-of-T is visually distinct from a scalar T of the same color.
                const float o = kPinR * st.zoom * 0.55f;   // center offset of each square
                const float h = kPinR * st.zoom * 0.42f;   // square half-size
                for (int gy = -1; gy <= 1; gy += 2)
                    for (int gx = -1; gx <= 1; gx += 2)
                        dl->AddRectFilled(ImVec2(pp.x + gx * o - h, pp.y + gy * o - h),
                                          ImVec2(pp.x + gx * o + h, pp.y + gy * o + h), p.color);
            }
            else if (ck == Container::Set)
            {
                // Set pin: three of the SCALAR dot, clustered in a triangle. Round
                // because a set holds exactly what the round pin holds; clustered
                // and un-stacked because a set has no order and no index — which is
                // the whole difference from the array's tidy grid.
                const float d = kPinR * st.zoom * 0.55f;   // center distance
                const float r = kPinR * st.zoom * 0.42f;   // dot radius
                dl->AddCircleFilled(ImVec2(pp.x, pp.y - d), r, p.color);
                dl->AddCircleFilled(ImVec2(pp.x - d * 0.866f, pp.y + d * 0.5f), r, p.color);
                dl->AddCircleFilled(ImVec2(pp.x + d * 0.866f, pp.y + d * 0.5f), r, p.color);
            }
            else if (ck == Container::Map)
            {
                // Map pin: two columns, KEY color left and value color right. A map
                // pin used to wear the value type's color alone, which drew
                // Map<String,Int> and Map<Int,Int> identically — the key type was
                // not readable off the node at all. Hosts that supply no key color
                // fall back to the value color on both columns.
                const float e = kPinR * st.zoom * 0.97f;   // outer half-extent
                const float g = kPinR * st.zoom * 0.08f;   // half the gap between columns
                dl->AddRectFilled(ImVec2(pp.x - e, pp.y - e), ImVec2(pp.x - g, pp.y + e),
                                  p.keyColor ? p.keyColor : p.color);
                dl->AddRectFilled(ImVec2(pp.x + g, pp.y - e), ImVec2(pp.x + e, pp.y + e), p.color);
            }
            else
                dl->AddCircleFilled(pp, kPinR * st.zoom, p.color);

            float labelEndX = pp.x + 8 * st.zoom;   // where an inline editor may start
            if (!p.label.empty())
            {
                // Pin labels scale with zoom like everything else on the node (they
                // were drawn at the fixed default font size before, so they overflowed
                // the node when zoomed out and looked tiny when zoomed in).
                const float fs = 13.0f * st.zoom;
                const float scale = fs / ImGui::GetFontSize();
                const ImVec2 ts0 = ImGui::CalcTextSize(p.label.c_str());
                const float ty = pp.y - ts0.y * scale * 0.5f;
                if (p.input) dl->AddText(nullptr, fs, ImVec2(pp.x + 8 * st.zoom, ty),
                                         IM_COL32(200,200,200,200), p.label.c_str());
                else         dl->AddText(nullptr, fs, ImVec2(pp.x - 8 * st.zoom - ts0.x * scale, ty),
                                         IM_COL32(200,200,200,200), p.label.c_str());
                if (p.input) labelEndX = pp.x + 8 * st.zoom + ts0.x * scale + 5 * st.zoom;
            }

            // Inline default editor on an unwired simple input: a small widget
            // right next to the pin label, so constants don't need literal nodes.
            // Hosted in a child window (like node bodies) so it gets the mouse
            // before the canvas and edits don't start a node drag.
            if (p.input && !p.isExec && pinInlineEditor(n.id, p.id))
            {
                // A GROUP, not a child window, and that is the whole point.
                // AddWindowToDrawData (imgui.cpp) appends a window's own draw
                // list FIRST and every child window after it, so anything drawn
                // in a child sits above ALL of the parent's primitives — every
                // node background on this canvas included. That is why an inline
                // editor used to shine through the node lying on top of it.
                // In the parent's list it is just one more command in node
                // order, and since nodes draw back-to-front the node in front
                // covers it, which is what a reader expects.
                //
                // The group is what a child window was really being used for:
                // BeginGroup sets DC.Indent to the cursor column, so a line
                // break returns to the group's left edge instead of the canvas's.
                // The clip rect replaces the child's other job, keeping a wide
                // widget inside the node — for hit-testing too, since ItemAdd
                // honours it.
                const float ew = 58.0f * st.zoom, eh = 17.0f * st.zoom;
                const ImVec2 emin(labelEndX, pp.y - eh * 0.5f);
                ImGui::PushID(n.id * 4096 + p.id);
                ImGui::PushClipRect(emin, ImVec2(emin.x + ew, emin.y + eh), true);
                ImGui::SetCursorScreenPos(emin);
                ImGui::BeginGroup();
                ImGui::SetWindowFontScale(st.zoom);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * st.zoom, 1.0f * st.zoom));
                model.drawPinInlineEditor(n.id, p.id);
                ImGui::PopStyleVar();
                ImGui::SetWindowFontScale(1.0f);
                ImGui::EndGroup();
                ImGui::PopClipRect();
                ImGui::PopID();
            }
        }

        // On-node body widgets (material params, HorizonCode literals). A GROUP
        // anchored to the node, so ImGui line breaks reset the cursor to the
        // node's left edge (not the canvas window's) — BeginGroup sets DC.Indent
        // to the cursor column, which is exactly that. It used to be a child
        // WINDOW, and that was the bug: AddWindowToDrawData appends a window's
        // own draw list first and its children afterwards, so every widget in a
        // child floated above every node background on the canvas. Drawn into
        // the parent's list it takes its place in node order instead, and the
        // node in front covers it.
        if (model.drawNodeBody && model.nodeBodyHeight && model.nodeBodyHeight(n.id) > 0.0f)
        {
            int left = 0, right = 0;
            for (const auto& p : n.pins) (p.input ? left : right)++;
            const float pinsBottom = n.pos.y + (n.gTitleH + std::max(left, right) * kRowH) * st.zoom;
            const ImVec2 bmin(n.pos.x + 6, pinsBottom + 2);
            const ImVec2 bmax(br.x - 6, br.y - 4);
            ImGui::PushID(n.id);
            // The clip rect does the other half of the child window's old job:
            // a body widget wider than the node stays inside it, and ItemAdd
            // honours the same rect, so a clipped-away control is not clickable
            // either.
            ImGui::PushClipRect(bmin, ImVec2(std::max(bmax.x, bmin.x + 1.0f),
                                             std::max(bmax.y, bmin.y + 1.0f)), true);
            ImGui::SetCursorScreenPos(bmin);
            ImGui::BeginGroup();
            // Body widgets scale with the canvas zoom like everything else on the
            // node: font via the window font scale, paddings via style vars (the
            // body rect is already zoom-sized, so unscaled widgets overflow it
            // when zoomed out and rattle around in it when zoomed in).
            ImGui::SetWindowFontScale(st.zoom);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * st.zoom, 2.0f * st.zoom));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(8.0f * st.zoom, 4.0f * st.zoom));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(4.0f * st.zoom, 4.0f * st.zoom));
            model.drawNodeBody(n.id, bmin, bmax, st.zoom);
            ImGui::PopStyleVar(3);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::EndGroup();
            ImGui::PopClipRect();
            ImGui::PopID();
        }

        // Grown by kNodeHitPad: the border pixel itself belongs to the node, not
        // to the canvas behind it.
        const bool overNode = mouse.x >= n.pos.x - kNodeHitPad && mouse.x <= br.x + kNodeHitPad &&
                              mouse.y >= n.pos.y - kNodeHitPad && mouse.y <= br.y + kNodeHitPad;
        if (overNode) hoverNodeNow = n.id;

        // Double-click a node (open a referenced function, …).
        if (interact && overNode && model.onNodeDoubleClick &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            model.onNodeDoubleClick(n.id);

        // Right-click a node → per-node context menu. If the node is already part
        // of a multi-selection, keep the whole group so the menu (Delete/Duplicate
        // Selection) acts on all of them; otherwise select just this one.
        if (interact && overNode && model.drawNodeContextMenu &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            st.ctxNode = n.id;
            const bool inSel =
                std::find(st.selection.begin(), st.selection.end(), n.id) != st.selection.end();
            if (!inSel) { st.selected = n.id; st.selection = { n.id }; }
            else        { st.selected = n.id; }
            ImGui::OpenPopup("##ge_nodectx");
            consumed = true;
        }

        // Body click → select + start move. A pin grab was already resolved
        // above and consumed the press, so this is the "not a pin" case.
        if (interact && !consumed && st.linkSrcNode == 0 && st.dragNode == 0 && !st.boxSel &&
            overNode && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const bool add = ImGui::GetIO().KeyShift && model.multiSelect;
            const bool inSel =
                std::find(st.selection.begin(), st.selection.end(), n.id) != st.selection.end();
            st.selectClickNode = 0;
            if (add)
            {
                if (!inSel) st.selection.push_back(n.id);
            }
            else if (inSel)
            {
                // Clicked a node already in the multi-selection: keep the whole
                // group so the drag moves all of it. A click with no drag
                // collapses to just this node (on mouse release).
                st.selectClickNode = n.id;
            }
            else { st.selection.clear(); st.selection.push_back(n.id); }
            st.selected = n.id;
            float gx = 0, gy = 0; model.getPos(n.id, gx, gy);
            st.dragNode = n.id; st.dragStartMouse = mouse; st.dragStartPos = ImVec2(gx, gy);
            st.dragMoved = false;
            consumed = true;
        }
    }

    // ── Quick spawn: hold a bound key, click empty canvas ────────────────────
    // Runs after the node loop (so `hoverNodeNow` is known) and before
    // box-select, and consumes the click — otherwise the same press would also
    // start a selection rectangle under the node it just dropped.
    if (interact && kbOwned && !consumed && !model.quickSpawns.empty() &&
        st.linkSrcNode == 0 && st.dragNode == 0 && hoverNodeNow == 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (const QuickSpawn* qs = heldSpawn(/*pressedOnly*/false))
        {
            QuickSpawnCtx ctx;
            ctx.pos = toGraph(mouse);
            const int created = qs->spawn(ctx);
            if (created != 0) { st.selected = created; st.selection = { created }; changed = true; }
            if (qs->key == ImGuiKey_F) st.fSpawned = true;
            consumed = true;
        }
    }

    // ── Hover tooltip (after the cursor rests on a node briefly) ─────────────
    if (model.drawNodeTooltip)
    {
        const bool idle = interact && st.dragNode == 0 && st.linkSrcNode == 0 && !st.boxSel;
        if (idle && hoverNodeNow != 0)
        {
            if (st.hoverNode != hoverNodeNow) { st.hoverNode = hoverNodeNow; st.hoverTime = 0.0f; }
            else st.hoverTime += ImGui::GetIO().DeltaTime;
            if (st.hoverTime > 0.6f)
            {
                // The one tooltip in the editor that is a paragraph rather than
                // a label: for an engine-API node this is the call's category
                // and registry id, its documentation, and a line per parameter.
                // Unwrapped, a tooltip sizes itself to its widest line, so that
                // became a bar running off the side of a laptop screen with the
                // parameter semantics — the reason anyone hovers a node — past
                // the edge and unreachable.
                //
                // An absolute column because a tooltip window has no width of
                // its own to wrap at, and the inner block so the pop happens
                // while the tooltip is still the current window: EndTooltip()
                // below would otherwise have moved on. The `if` is imgui.h's
                // rule — EndTooltip is only valid when BeginTooltip returned
                // true, which today it always does and one upgrade from now
                // may not.
                if (ImGui::BeginTooltip())
                {
                    {
                        EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
                        model.drawNodeTooltip(hoverNodeNow);
                    }
                    ImGui::EndTooltip();
                }
            }
        }
        else { st.hoverNode = 0; st.hoverTime = 0.0f; }
    }

    // ── Node move (moves the whole selection) ────────────────────────────────
    if (st.dragNode != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float dx = (mouse.x - st.dragStartMouse.x) / st.zoom;
        const float dy = (mouse.y - st.dragStartMouse.y) / st.zoom;
        if (std::fabs(dx) > 0.5f || std::fabs(dy) > 0.5f) st.dragMoved = true;
        // Moving, button still down. The document has changed — it is unsaved
        // work from this instant, and for a collaborated graph it is what a
        // peer should be seeing right now. The RETURN value still waits for the
        // release, because that is what pushes an undo snapshot and a drag is
        // one step, not sixty.
        if (st.dragMoved) st.liveEdit = true;
        // Delta from the primary node's start; apply to all selected.
        float px = 0, py = 0; model.getPos(st.dragNode, px, py);
        const float ndx = (st.dragStartPos.x + dx) - px;
        const float ndy = (st.dragStartPos.y + dy) - py;
        for (int sid : st.selection.empty() ? std::vector<int>{ st.dragNode } : st.selection)
        {
            float gx = 0, gy = 0; model.getPos(sid, gx, gy);
            model.setPos(sid, gx + ndx, gy + ndy);
        }
    }
    if (st.dragNode != 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (st.dragMoved) changed = true;
        else if (st.selectClickNode == st.dragNode)
        {
            // A plain click (no drag) on a group member → collapse to just it.
            st.selection.clear();
            st.selection.push_back(st.dragNode);
            st.selected = st.dragNode;
        }
        st.dragNode = 0;
        st.selectClickNode = 0;
    }

    // ── Active link drag ─────────────────────────────────────────────────────
    if (st.linkSrcNode != 0)
    {
        if (const Drawn* sn = findNode(st.linkSrcNode))
            if (const ImVec2* a = findPin(*sn, st.linkSrcPin, st.linkSrcInput))
            {
                // drawLink()'s first arg always tangents rightward (departure),
                // its second tangents leftward (arrival) — correct for a normal
                // output→input link. Dragging off an INPUT (left side of a node)
                // searches backward for a source, so the pin is the arrival end:
                // swap the args or the preview curve bulges the wrong way.
                if (st.linkSrcInput) drawLink(dl, mouse, *a, IM_COL32(255, 210, 120, 220), 2.0f);
                else                 drawLink(dl, *a, mouse, IM_COL32(255, 210, 120, 220), 2.0f);
            }

        // A bound key hit mid-drag drops that node ALREADY WIRED to the pin in
        // hand — the drag-off menu's result without the menu. There is no click
        // to gate on here, so this reacts to the key edge, not to it being held.
        if (kbOwned && !model.quickSpawns.empty())
        {
            if (const QuickSpawn* qs = heldSpawn(/*pressedOnly*/true); qs && qs->wireable)
            {
                QuickSpawnCtx ctx;
                ctx.pos       = toGraph(mouse);
                ctx.linkNode  = st.linkSrcNode;
                ctx.linkPin   = st.linkSrcPin;
                ctx.linkInput = st.linkSrcInput;
                const int created = qs->spawn(ctx);
                if (created != 0) { st.selected = created; st.selection = { created }; changed = true; }
                if (qs->key == ImGuiKey_F) st.fSpawned = true;
                // The drag is over either way; the mouse button is still down,
                // so clearing the source here also stops the release below from
                // opening the drag-off menu on top of the node just created.
                st.linkSrcNode = 0;
                st.linkGrab = false;
            }
        }

        if (st.linkSrcNode != 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            int tn = 0, tp = 0; bool ti = false;
            if (pinAt(mouse, tn, tp, ti))
            {
                if (tn != st.linkSrcNode && model.connect)
                {
                    bool ok = false;
                    if (!st.linkSrcInput && ti)      ok = model.connect(st.linkSrcNode, st.linkSrcPin, tn, tp);
                    else if (st.linkSrcInput && !ti) ok = model.connect(tn, tp, st.linkSrcNode, st.linkSrcPin);
                    if (ok) changed = true;
                }
            }
            else if (model.drawPinDragMenu && !st.linkGrab)
            {
                // Released on empty canvas → offer a filtered "drag off a pin"
                // menu, which creates a compatible node and connects it. Skipped
                // when this drag detached an existing link (that's a removal).
                st.dragOffNode  = st.linkSrcNode;
                st.dragOffPin   = st.linkSrcPin;
                st.dragOffInput = st.linkSrcInput;
                st.addMenuGraphPos = toGraph(mouse);
                ImGui::OpenPopup("##ge_pindrag");
            }
            st.linkSrcNode = 0;
            st.linkGrab = false;
        }
    }

    // Ghost link while the filtered "drag off a pin" popup is open: the drag
    // itself already ended (linkSrcNode was cleared above), but the drop point
    // survives in dragOffNode/Pin/Input + addMenuGraphPos — keep the pending
    // connection visible while the user is still picking what to create, not
    // just during the drag, so it doesn't look like the drag was abandoned.
    if (st.dragOffNode != 0 && ImGui::IsPopupOpen("##ge_pindrag"))
    {
        if (const Drawn* sn = findNode(st.dragOffNode))
            if (const ImVec2* a = findPin(*sn, st.dragOffPin, st.dragOffInput))
            {
                const ImVec2 drop = toScreen(st.addMenuGraphPos.x, st.addMenuGraphPos.y);
                if (st.dragOffInput) drawLink(dl, drop, *a, IM_COL32(255, 210, 120, 220), 2.0f);
                else                 drawLink(dl, *a, drop, IM_COL32(255, 210, 120, 220), 2.0f);
            }
    }

    // ── Box-select ───────────────────────────────────────────────────────────
    if (model.multiSelect)
    {
        if (interact && !consumed && st.linkSrcNode == 0 && st.dragNode == 0 &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            st.boxSel = true; st.boxStart = mouse;
            if (!ImGui::GetIO().KeyShift) { st.selection.clear(); st.selected = 0; }
        }
        if (st.boxSel)
        {
            const ImVec2 mn(std::min(st.boxStart.x, mouse.x), std::min(st.boxStart.y, mouse.y));
            const ImVec2 mx(std::max(st.boxStart.x, mouse.x), std::max(st.boxStart.y, mouse.y));
            dl->AddRectFilled(mn, mx, IM_COL32(255, 170, 40, 40));
            dl->AddRect(mn, mx, IM_COL32(255, 170, 40, 160));
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                for (const Drawn& n : nodes)
                {
                    const ImVec2 br(n.pos.x + n.size.x, n.pos.y + n.size.y);
                    if (n.pos.x < mx.x && br.x > mn.x && n.pos.y < mx.y && br.y > mn.y)
                        if (std::find(st.selection.begin(), st.selection.end(), n.id) == st.selection.end())
                            st.selection.push_back(n.id);
                }
                if (!st.selection.empty()) st.selected = st.selection.front();
                st.boxSel = false;
            }
        }
    }
    else if (interact && !consumed && st.linkSrcNode == 0 && st.dragNode == 0 &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        st.selected = 0; st.selection.clear();
    }

    if (model.drawFront) model.drawFront(dl, origin, st.pan, st.zoom);

    // ── Delete selection ─────────────────────────────────────────────────────
    // Gated on kbOwned (see its definition): with a text field active the cursor
    // is usually still over the canvas, so an ordinary edit keystroke used to
    // destroy the selected nodes.
    //
    // Backspace is deliberately NOT a delete key here: it is THE text-editing
    // key, so binding it to node destruction makes every near-miss fatal. The
    // node context menu's "Delete Node/Selection" is the always-available path.
    if (interact && kbOwned && model.removeNode && ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        std::vector<int> doomed = st.selection;
        if (doomed.empty() && st.selected != 0) doomed.push_back(st.selected);
        for (int nid : doomed) model.removeNode(nid);
        if (!doomed.empty()) { st.selection.clear(); st.selected = 0; changed = true; }
    }

    // ── Select all (in the visible sub-graph) ────────────────────────────────
    {
        const ImGuiIO& kio = ImGui::GetIO();
        if (interact && kbOwned && model.multiSelect && (kio.KeyCtrl || kio.KeySuper) &&
            ImGui::IsKeyPressed(ImGuiKey_A, /*repeat*/false))
        {
            st.selection = ids;   // `ids` is already only what this graph shows
            st.selected  = st.selection.empty() ? 0 : st.selection.front();
        }
    }

    // ── Framing: Home fits the whole graph, a plain F tap the selection ──────
    // Fits `which` (empty = every node) into the canvas. Zoom is capped at 1:1 —
    // framing a single small node should bring it into view, not magnify it.
    auto frameNodes = [&](const std::vector<int>& which)
    {
        constexpr float kBig = std::numeric_limits<float>::max();
        ImVec2 mn(kBig, kBig), mx(-kBig, -kBig);
        for (const Drawn& n : nodes)
        {
            if (!which.empty() &&
                std::find(which.begin(), which.end(), n.id) == which.end()) continue;
            float gx = 0, gy = 0; model.getPos(n.id, gx, gy);
            mn.x = std::min(mn.x, gx);                       mn.y = std::min(mn.y, gy);
            mx.x = std::max(mx.x, gx + n.size.x / st.zoom);  mx.y = std::max(mx.y, gy + n.size.y / st.zoom);
        }
        if (mn.x > mx.x) return;   // nothing to frame
        const float pad = 60.0f;
        const float w = (mx.x - mn.x) + pad * 2.0f, h = (mx.y - mn.y) + pad * 2.0f;
        st.zoom  = std::clamp(std::min(size.x / w, size.y / h), 0.3f, 1.0f);
        st.pan.x = size.x * 0.5f - (mn.x + mx.x) * 0.5f * st.zoom;
        st.pan.y = size.y * 0.5f - (mn.y + mx.y) * 0.5f * st.zoom;
    };
    if (interact && kbOwned && ImGui::IsKeyPressed(ImGuiKey_Home, /*repeat*/false)) frameNodes({});
    if (interact && kbOwned && !st.fSpawned && ImGui::IsKeyReleased(ImGuiKey_F))
    {
        std::vector<int> sel = st.selection;
        if (sel.empty() && st.selected != 0) sel.push_back(st.selected);
        frameNodes(sel);   // nothing selected → frame everything
    }

    // ── Straighten connections (Q) ───────────────────────────────────────────
    // Snaps the selection's wires horizontal: for every link INSIDE the
    // selection the downstream node slides vertically until its input pin lines
    // up with the source's output pin. With a single node selected it is the
    // other end of each of its links that moves instead, so the node you aimed
    // at stays where you put it.
    if (interact && kbOwned && model.setPos && ImGui::IsKeyPressed(ImGuiKey_Q, /*repeat*/false))
    {
        std::vector<int> sel = st.selection;
        if (sel.empty() && st.selected != 0) sel.push_back(st.selected);
        const bool single = sel.size() == 1;
        auto inSel = [&](int nid){ return std::find(sel.begin(), sel.end(), nid) != sel.end(); };
        // A pin's offset from its node's top edge in graph units — unlike the
        // laid-out pin positions this stays valid while the nodes move.
        auto pinOffsetY = [&](int nid, int pinId, bool input) -> float {
            const Drawn* n = findNode(nid); if (!n) return 0.0f;
            const ImVec2* p = findPin(*n, pinId, input); if (!p) return 0.0f;
            return (p->y - n->pos.y) / st.zoom;
        };
        auto align = [&](int anchor, int anchorPin, int mover, int moverPin, bool moverIsDst) {
            float ax = 0, ay = 0, mx2 = 0, my = 0;
            model.getPos(anchor, ax, ay);
            model.getPos(mover,  mx2, my);
            const float want = ay + pinOffsetY(anchor, anchorPin, !moverIsDst)
                                  - pinOffsetY(mover,  moverPin,   moverIsDst);
            if (std::fabs(want - my) > 0.01f) { model.setPos(mover, mx2, want); changed = true; }
        };
        // Left to right, so a chain settles in one sweep; a second pass catches
        // the nodes whose anchor only moved during the first.
        std::vector<std::array<int,4>> ordered;
        for (const auto& l : links)
            if (single ? (l[0] == sel.front() || l[2] == sel.front())
                       : (inSel(l[0]) && inSel(l[2])))
                ordered.push_back(l);
        std::sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) {
            float ax = 0, ay = 0, bx = 0, by = 0;
            model.getPos(a[0], ax, ay); model.getPos(b[0], bx, by);
            return ax < bx;
        });
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& l : ordered)
            {
                if (single && l[2] == sel.front()) align(l[2], l[3], l[0], l[1], false);
                else                               align(l[0], l[1], l[2], l[3], true);
            }
    }

    // ── Add-node popup (right-click on EMPTY canvas, no pan drag) ─────────────
    // Skipped when the cursor is over a node (that opens the node context menu
    // instead), so a node right-click never opens both popups.
    bool overAnyNode = false;
    for (const Drawn& n : nodes)
        if (mouse.x >= n.pos.x && mouse.x <= n.pos.x + n.size.x &&
            mouse.y >= n.pos.y && mouse.y <= n.pos.y + n.size.y) { overAnyNode = true; break; }
    if (model.drawAddMenu && interact && !overAnyNode &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 36.0f)
    {
        st.addMenuGraphPos = toGraph(mouse);
        ImGui::OpenPopup("##ge_add");
    }
    // Space is the keyboard route to the same palette: the popup opens at the
    // cursor with its search field focused, so "space, three letters, Enter"
    // never needs the mouse to aim at anything.
    if (model.drawAddMenu && interact && kbOwned && ImGui::IsKeyPressed(ImGuiKey_Space, /*repeat*/false))
    {
        st.addMenuGraphPos = toGraph(mouse);
        ImGui::OpenPopup("##ge_add");
    }
    if (ImGui::BeginPopup("##ge_add"))
    {
        const int created = model.drawAddMenu();
        if (created != 0) { st.selected = created; st.selection = { created }; changed = true; }
        ImGui::EndPopup();
    }

    // Filtered "drag off a pin" menu (opened above on an empty-canvas release).
    if (model.drawPinDragMenu && ImGui::BeginPopup("##ge_pindrag"))
    {
        const int created = model.drawPinDragMenu(st.dragOffNode, st.dragOffPin,
                                                  st.dragOffInput, st.addMenuGraphPos);
        if (created != 0) { st.selected = created; st.selection = { created }; changed = true; }
        ImGui::EndPopup();
    }

    // Per-node context menu.
    if (model.drawNodeContextMenu && ImGui::BeginPopup("##ge_nodectx"))
    {
        if (st.ctxNode != 0) model.drawNodeContextMenu(st.ctxNode);
        ImGui::EndPopup();
    }

    return changed;
}

} // namespace GraphEditor
