#include "GraphEditor.h"
#include <algorithm>
#include <unordered_set>
#include <cmath>
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

    ImGui::InvisibleButton(id, size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    // The canvas button holds the active state while a right/middle drag that
    // STARTED on empty canvas is in progress — so panning keeps working even as
    // the cursor sweeps over nodes mid-drag.
    const bool canvasHeld = ImGui::IsItemActive();
    const ImVec2 mouse = ImGui::GetMousePos();

    // Host chrome that must interact before the nodes (e.g. comment boxes). It
    // returns true to consume the mouse for this frame.
    bool behindConsumed = false;
    if (model.interactBehind)
        behindConsumed = model.interactBehind(origin, st.pan, st.zoom, hovered);

    // Deliberately NOT gated on IsAnyItemActive: the canvas InvisibleButton
    // itself becomes the active item on press, so that guard would make
    // `interact` false on the very click that should start a node drag /
    // box-select. Hovering a node-body child window already turns `hovered`
    // off, so editing a body widget doesn't trigger canvas interaction.
    const bool interact = hovered && !st.suppressInteraction && !behindConsumed;

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

    // ── Zoom (wheel / touchpad over the canvas, about the cursor) ────────────
    if (interact)
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const ImVec2 before = toGraph(mouse);
            st.zoom = std::clamp(st.zoom * (1.0f + wheel * 0.1f), 0.3f, 2.5f);
            st.pan.x = mouse.x - origin.x - before.x * st.zoom;
            st.pan.y = mouse.y - origin.y - before.y * st.zoom;
        }
    }
    // ── Pan (right / middle drag). Latched to the canvas button so it keeps
    // panning as the cursor moves over nodes; not gated on `interact` so a
    // node-body widget being active elsewhere doesn't freeze the drag.
    if ((canvasHeld || (hovered && !st.suppressInteraction)) &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
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
    // No manual clip rect here: the host already draws the canvas inside a
    // clipping child window, and pushing a draw-list clip around the node loop
    // would collide with the BeginChild used for on-node body widgets.
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

    // ── Pin hit-test (topmost node first) ────────────────────────────────────
    auto pinAt = [&](const ImVec2& m, int& outNode, int& outPin, bool& outInput) -> bool
    {
        const float r = kPinR * st.zoom + 4.0f;
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
            for (size_t i = 0; i < it->pins.size(); ++i)
            {
                const ImVec2 pp = it->pinPos[i];
                const float dx = m.x - pp.x, dy = m.y - pp.y;
                if (dx*dx + dy*dy <= r*r)
                { outNode = it->id; outPin = it->pins[i].id; outInput = it->pins[i].input; return true; }
            }
        return false;
    };

    // ── Nodes ────────────────────────────────────────────────────────────────
    bool consumed = false;
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
            if (p.isExec)
            {
                const float s = kPinR * st.zoom;
                dl->AddTriangleFilled(ImVec2(pp.x - s, pp.y - s), ImVec2(pp.x - s, pp.y + s),
                                      ImVec2(pp.x + s, pp.y), p.color);
            }
            else if (p.isArray)
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
                const float ew = 58.0f * st.zoom, eh = 17.0f * st.zoom;
                ImGui::PushID(n.id * 4096 + p.id);
                ImGui::SetCursorScreenPos(ImVec2(labelEndX, pp.y - eh * 0.5f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::BeginChild("##pindefault", ImVec2(ew, eh), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
                ImGui::SetWindowFontScale(st.zoom);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * st.zoom, 1.0f * st.zoom));
                model.drawPinInlineEditor(n.id, p.id);
                ImGui::PopStyleVar();
                ImGui::SetWindowFontScale(1.0f);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopID();
            }
        }

        // On-node body widgets (material params etc.). Wrapped in a child window
        // anchored to the node so ImGui line breaks reset the cursor to the
        // node's left edge (not the canvas window's) — otherwise multi-widget
        // bodies flow down the left margin instead of stacking on the node.
        if (model.drawNodeBody && model.nodeBodyHeight && model.nodeBodyHeight(n.id) > 0.0f)
        {
            int left = 0, right = 0;
            for (const auto& p : n.pins) (p.input ? left : right)++;
            const float pinsBottom = n.pos.y + (n.gTitleH + std::max(left, right) * kRowH) * st.zoom;
            const ImVec2 bmin(n.pos.x + 6, pinsBottom + 2);
            const ImVec2 bmax(br.x - 6, br.y - 4);
            ImGui::PushID(n.id);
            ImGui::SetCursorScreenPos(bmin);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::BeginChild("##nodebody",
                ImVec2(std::max(bmax.x - bmin.x, 1.0f), std::max(bmax.y - bmin.y, 1.0f)),
                ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
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
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopID();
        }

        const bool overNode = mouse.x >= n.pos.x && mouse.x <= br.x &&
                              mouse.y >= n.pos.y && mouse.y <= br.y;
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

        // Body click → select + start move.
        if (interact && st.linkSrcNode == 0 && st.dragNode == 0 && !st.boxSel &&
            overNode && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            int pn = 0, pp2 = 0; bool pin_in = false;
            if (pinAt(mouse, pn, pp2, pin_in) && pn == n.id)
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
            }
            else
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
            }
            consumed = true;
        }
    }

    // ── Hover tooltip (after the cursor rests on a node briefly) ─────────────
    if (model.nodeTooltip)
    {
        const bool idle = interact && st.dragNode == 0 && st.linkSrcNode == 0 && !st.boxSel;
        if (idle && hoverNodeNow != 0)
        {
            if (st.hoverNode != hoverNodeNow) { st.hoverNode = hoverNodeNow; st.hoverTime = 0.0f; }
            else st.hoverTime += ImGui::GetIO().DeltaTime;
            if (st.hoverTime > 0.6f)
            {
                const std::string tip = model.nodeTooltip(hoverNodeNow);
                if (!tip.empty())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(tip.c_str());
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

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
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
    if (interact && model.removeNode &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
    {
        std::vector<int> doomed = st.selection;
        if (doomed.empty() && st.selected != 0) doomed.push_back(st.selected);
        for (int nid : doomed) model.removeNode(nid);
        if (!doomed.empty()) { st.selection.clear(); st.selected = 0; changed = true; }
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
