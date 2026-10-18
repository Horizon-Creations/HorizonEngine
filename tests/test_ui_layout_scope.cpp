#include "doctest.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <UIWidget/WidgetManager.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UILayoutScope.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// ═══ One layout pass per frame (Thema 213, CORE-601/602/701-703) ═════════════
// The layout is a set of stateless questions ("where is this element", "is it
// on screen"), each answered by walking the parent chain with a linear id
// lookup and rescanning every sibling. Asked for every element every frame,
// that is O(n²) and worse. This file holds the measurement that showed it and
// the guard that the faster path answers EXACTLY what the old one did.

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            path = std::filesystem::temp_directory_path() / "he_test_ui_layout_scope";
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::filesystem::remove_all(path); }
    };

    HE::UIElement& addTo(HE::UIWidgetTree& t, int parent, HE::UIWidgetType type)
    {
        const int id = t.add(type);
        HE::UIElement* e = t.find(id);
        e->parentId = parent;
        return *e;
    }

    void fill(HE::UIElement& e)
    {
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
        e.posX = e.posY = 0.0f;
        e.sizeX = e.sizeY = 0.0f;
    }

    // An application shell of the kind the audit names: a Splitter at the root,
    // a Tab Box in one pane, a few hundred rows in boxes, a grid, a wrap box and
    // an accordion. About 1900 elements.
    void buildShell(HE::UIWidgetTree& t)
    {
        t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
        HE::UIElement& split = addTo(t, 0, HE::UIWidgetType::Splitter);
        fill(split);

        HE::UIElement& left = addTo(t, split.id, HE::UIWidgetType::VerticalBox);
        for (int i = 0; i < 300; ++i)
        {
            auto& tx = static_cast<HE::UIText&>(addTo(t, left.id, HE::UIWidgetType::Text));
            tx.text = "Label " + std::to_string(i);
        }

        auto& tabs = static_cast<HE::UITabBox&>(addTo(t, split.id, HE::UIWidgetType::TabBox));
        tabs.activeTab = 0;

        HE::UIElement& scroll = addTo(t, tabs.id, HE::UIWidgetType::ScrollBox);
        scroll.clipChildren = true;
        for (int i = 0; i < 400; ++i)
        {
            HE::UIElement& row = addTo(t, scroll.id, HE::UIWidgetType::HorizontalBox);
            row.sizeY = 28.0f;
            addTo(t, row.id, HE::UIWidgetType::Button);
            auto& tx = static_cast<HE::UIText&>(addTo(t, row.id, HE::UIWidgetType::Text));
            tx.text = "Row " + std::to_string(i);
        }

        auto& grid = static_cast<HE::UIGrid&>(addTo(t, tabs.id, HE::UIWidgetType::Grid));
        grid.columns = { "auto", "*", "100" };
        grid.reparse();
        for (int i = 0; i < 150; ++i)
            addTo(t, grid.id, i % 2 ? HE::UIWidgetType::Panel : HE::UIWidgetType::Text);

        HE::UIElement& wrap = addTo(t, tabs.id, HE::UIWidgetType::WrapBox);
        for (int i = 0; i < 150; ++i) addTo(t, wrap.id, HE::UIWidgetType::Button);

        HE::UIElement& acc = addTo(t, tabs.id, HE::UIWidgetType::Accordion);
        for (int s = 0; s < 6; ++s)
        {
            HE::UIElement& body = addTo(t, acc.id, HE::UIWidgetType::VerticalBox);
            for (int i = 0; i < 10; ++i) addTo(t, body.id, HE::UIWidgetType::CheckBox);
        }
    }

    // Every container the layout distinguishes, with the settings that take a
    // different branch in it: fills, alignments, margins, bounds, an invisible
    // box child, pinned and spanning grid cells, a wrap that breaks, list rows,
    // a tab box on its second page, a splitter with a third child, an
    // accordion with one section folded, an embedded widget that scales its
    // content, a designer slot, rotation, opacity, disabled and clipping
    // chains, and one element whose parent does not exist.
    void buildEverything(HE::UIWidgetTree& t)
    {
        t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;

        HE::UIElement& root = addTo(t, 0, HE::UIWidgetType::Panel);
        HE::uiSetAnchorPreset(root, 5);
        root.posX = 40.0f; root.posY = 30.0f; root.sizeX = 900.0f; root.sizeY = 600.0f;
        root.pivotX = 0.3f; root.pivotY = 0.6f;
        root.rotation = 12.0f;
        root.renderOpacity = 0.8f;
        root.clipChildren = true;

        auto& vbox = static_cast<HE::UIBoxBase&>(addTo(t, root.id, HE::UIWidgetType::VerticalBox));
        HE::uiSetAnchorPreset(vbox, 12);
        vbox.posX = 10.0f; vbox.sizeX = 300.0f; vbox.sizeY = -20.0f;
        vbox.padding = 6.0f; vbox.spacing = 4.0f;
        vbox.clipChildren = true;
        for (int i = 0; i < 8; ++i)
        {
            HE::UIElement& c = addTo(t, vbox.id, i % 3 ? HE::UIWidgetType::Button
                                                       : HE::UIWidgetType::Text);
            c.sizeX = 80.0f + 10.0f * i; c.sizeY = 20.0f + 3.0f * i;
            c.slotPadLeft = 2.0f * i; c.slotPadTop = 1.0f; c.slotPadRight = 3.0f;
            c.slotPadBottom = 0.5f * i;
            if (i == 2) c.slotFill = 1.0f;
            if (i == 5) c.slotFill = 2.5f;
            if (i == 3) c.visible = false;
            if (i == 4) { c.slotHAlign = HE::UISlotHAlign::Center; c.minSizeX = 150.0f; }
            if (i == 6) { c.slotHAlign = HE::UISlotHAlign::Right; c.maxSizeY = 12.0f; }
            if (i == 7) { c.rotation = -30.0f; c.enabled = false; }
        }
        HE::UIElement& hbox = addTo(t, vbox.id, HE::UIWidgetType::HorizontalBox);
        hbox.sizeY = 40.0f;
        for (int i = 0; i < 4; ++i)
        {
            HE::UIElement& c = addTo(t, hbox.id, HE::UIWidgetType::Panel);
            c.sizeX = 30.0f + 5.0f * i;
            c.slotVAlign = static_cast<HE::UISlotVAlign>(i);
            c.sizeY = 15.0f;
        }

        auto& scroll = static_cast<HE::UIScrollBox&>(addTo(t, root.id, HE::UIWidgetType::ScrollBox));
        HE::uiSetAnchorPreset(scroll, 2);
        scroll.posX = -10.0f; scroll.posY = 10.0f; scroll.sizeX = 250.0f; scroll.sizeY = 200.0f;
        scroll.pivotX = 1.0f; scroll.pivotY = 0.0f;
        scroll.clipChildren = true;
        scroll.scrollOffset = 37.5f;
        for (int i = 0; i < 20; ++i)
        {
            HE::UIElement& c = addTo(t, scroll.id, HE::UIWidgetType::TextInput);
            c.sizeY = 24.0f;
        }

        auto& grid = static_cast<HE::UIGrid&>(addTo(t, root.id, HE::UIWidgetType::Grid));
        HE::uiSetAnchorPreset(grid, 8);
        grid.posX = -20.0f; grid.posY = -20.0f; grid.pivotX = 1.0f; grid.pivotY = 1.0f;
        grid.sizeX = 360.0f; grid.sizeY = 220.0f;
        grid.columns = { "auto", "*", "60", "2*" };
        grid.rows = { "auto", "30", "auto" };
        grid.reparse();
        for (int i = 0; i < 17; ++i)
        {
            HE::UIElement& c = addTo(t, grid.id, i % 2 ? HE::UIWidgetType::Panel
                                                       : HE::UIWidgetType::CheckBox);
            c.sizeX = 20.0f + 4.0f * i; c.sizeY = 12.0f + 2.0f * (i % 5);
            if (i == 3) { c.gridColumn = 2; c.gridRow = 1; }
            if (i == 6) { c.gridColumnSpan = 2; }
            if (i == 9) { c.gridRowSpan = 2; c.slotVAlign = HE::UISlotVAlign::Bottom; }
            if (i == 11) c.visible = false;
        }

        auto& wrap = static_cast<HE::UIWrapBox&>(addTo(t, root.id, HE::UIWidgetType::WrapBox));
        HE::uiSetAnchorPreset(wrap, 6);
        wrap.posX = 330.0f; wrap.posY = -10.0f; wrap.pivotY = 1.0f;
        wrap.sizeX = 220.0f; wrap.sizeY = 160.0f;
        for (int i = 0; i < 14; ++i)
        {
            HE::UIElement& c = addTo(t, wrap.id, HE::UIWidgetType::Button);
            c.sizeX = 30.0f + 9.0f * (i % 4); c.sizeY = 18.0f + 6.0f * (i % 3);
            if (i % 5 == 1) c.slotVAlign = HE::UISlotVAlign::Center;
            if (i == 8) c.visible = false;
        }

        auto& list = static_cast<HE::UIListView&>(addTo(t, root.id, HE::UIWidgetType::ListView));
        HE::uiSetAnchorPreset(list, 4);
        list.sizeX = 200.0f; list.sizeY = 150.0f;
        list.itemCount = 50; list.scrollOffset = 61.0f;
        list.showHeader = true;
        list.clipChildren = true;
        for (int i = 0; i < 6; ++i)
        {
            auto& row = static_cast<HE::UIWidgetRef&>(addTo(t, list.id, HE::UIWidgetType::WidgetRef));
            row.rowIndex = i + 1;
            addTo(t, row.id, HE::UIWidgetType::Text);
        }
        addTo(t, list.id, HE::UIWidgetType::Panel);          // not a row

        auto& tabs = static_cast<HE::UITabBox&>(addTo(t, root.id, HE::UIWidgetType::TabBox));
        HE::uiSetAnchorPreset(tabs, 15);
        tabs.posX = 320.0f; tabs.posY = 220.0f; tabs.sizeX = -660.0f; tabs.sizeY = -400.0f;
        tabs.activeTab = 1;
        for (int p = 0; p < 3; ++p)
        {
            HE::UIElement& page = addTo(t, tabs.id, HE::UIWidgetType::VerticalBox);
            for (int i = 0; i < 3; ++i) addTo(t, page.id, HE::UIWidgetType::Slider);
        }

        auto& split = static_cast<HE::UISplitter&>(addTo(t, root.id, HE::UIWidgetType::Splitter));
        HE::uiSetAnchorPreset(split, 3);
        split.posY = 260.0f; split.sizeX = -40.0f; split.sizeY = 90.0f;
        split.vertical = true; split.ratio = 0.3f;
        for (int i = 0; i < 3; ++i)
        {
            HE::UIElement& pane = addTo(t, split.id, HE::UIWidgetType::Panel);
            HE::UIElement& in = addTo(t, pane.id, HE::UIWidgetType::Text);
            HE::uiSetAnchorPreset(in, 4);
        }

        auto& acc = static_cast<HE::UIAccordion&>(addTo(t, root.id, HE::UIWidgetType::Accordion));
        HE::uiSetAnchorPreset(acc, 0);
        acc.posX = 600.0f; acc.posY = 380.0f; acc.sizeX = 220.0f; acc.sizeY = 200.0f;
        acc.expanded = 0b101;
        acc.scrollOffset = 5.0f;
        for (int s = 0; s < 4; ++s)
        {
            HE::UIElement& body = addTo(t, acc.id, HE::UIWidgetType::Panel);
            body.sizeY = 30.0f + 10.0f * s;
            addTo(t, body.id, HE::UIWidgetType::RadioButton);
        }

        // An embedded widget: its content is authored on 400×300 and fitted
        // into the ref's rect, so everything under it has a unit scale.
        auto& ref = static_cast<HE::UIWidgetRef&>(addTo(t, root.id, HE::UIWidgetType::WidgetRef));
        HE::uiSetAnchorPreset(ref, 0);
        ref.posX = 20.0f; ref.posY = 420.0f; ref.sizeX = 260.0f; ref.sizeY = 150.0f;
        ref.contentW = 400.0f; ref.contentH = 300.0f;
        ref.contentMode = HE::UICanvasScaleMode::FitInside;
        ref.renderOpacity = 0.5f;
        HE::UIElement& slotRoot = addTo(t, ref.id, HE::UIWidgetType::NamedSlot);
        HE::uiSetAnchorPreset(slotRoot, HE::kUIAnchorFill);
        auto& inner = static_cast<HE::UIBoxBase&>(addTo(t, slotRoot.id, HE::UIWidgetType::HorizontalBox));
        inner.padding = 8.0f; inner.spacing = 5.0f;
        inner.minSizeX = 100.0f; inner.maxSizeX = 380.0f;
        for (int i = 0; i < 3; ++i) addTo(t, inner.id, HE::UIWidgetType::Image).sizeX = 50.0f;

        // …and one in the DESIGNER, whose child is placed through a design slot.
        auto& dref = static_cast<HE::UIWidgetRef&>(addTo(t, root.id, HE::UIWidgetType::WidgetRef));
        HE::uiSetAnchorPreset(dref, 0);
        dref.posX = 300.0f; dref.posY = 480.0f; dref.sizeX = 200.0f; dref.sizeY = 100.0f;
        dref.contentW = 300.0f; dref.contentH = 150.0f;
        dref.designSlots.push_back({ "Body", { 10.0f, 20.0f, 120.0f, 80.0f } });
        HE::UIElement& slotted = addTo(t, dref.id, HE::UIWidgetType::Panel);
        slotted.name = "Body";
        HE::uiSetAnchorPreset(slotted, HE::kUIAnchorFill);

        // A disabled, faded container with a rotated child of its own.
        HE::UIElement& off = addTo(t, 0, HE::UIWidgetType::Panel);
        HE::uiSetAnchorPreset(off, 8);
        off.sizeX = 200.0f; off.sizeY = 100.0f; off.enabled = false; off.renderOpacity = 0.25f;
        off.clipChildren = true;
        HE::UIElement& offKid = addTo(t, off.id, HE::UIWidgetType::Button);
        offKid.rotation = 45.0f; offKid.pivotX = 0.5f; offKid.pivotY = 0.5f;

        // A caption and a wrapping text that fills what the caption leaves: the
        // second one's width — and so the height it measures — depends on the
        // width the first one just measured. The case that shows whether
        // auto-size sees its own earlier writes.
        HE::UIElement& line = addTo(t, root.id, HE::UIWidgetType::HorizontalBox);
        line.name = "Line";
        HE::uiSetAnchorPreset(line, 0);
        line.posX = 100.0f; line.posY = 20.0f; line.sizeX = 900.0f; line.sizeY = 80.0f;
        auto& caption = static_cast<HE::UIText&>(addTo(t, line.id, HE::UIWidgetType::Text));
        caption.text = "Caption:";
        auto& body = static_cast<HE::UIText&>(addTo(t, line.id, HE::UIWidgetType::Text));
        body.text = "words that wrap at whatever width the caption leaves them, and a few "
                    "more of them so that every change of that width moves a line break";
        body.wordWrap = true;
        body.slotFill = 1.0f;

        // A parent that is not there.
        HE::UIElement& orphan = addTo(t, 0, HE::UIWidgetType::Panel);
        orphan.parentId = 9999;
        orphan.posX = 5.0f; orphan.posY = 6.0f;
    }

    bool sameRect(const HE::UIWidgetRect& a, const HE::UIWidgetRect& b)
    { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

    // Everything a frame asks about one element, in one record.
    struct Answers
    {
        HE::UIWidgetRect rect{}, unbounded{}, anchor{}, clip{};
        bool  clipped = false, rotated = false, visible = false, enabled = false;
        HE::UIRotation rot{};
        float opacity = 0.0f, usx = 0.0f, usy = 0.0f;
        int   childIndex = -2, childCount = -2;
        const HE::UIElement* found = nullptr;
        std::vector<int> children;
    };

    Answers ask(const HE::UIWidgetTree& t, const HE::UIElement& e, const HE::UIWidgetCanvas* c)
    {
        Answers a;
        a.rect      = HE::uiElementRect(t, e, c);
        a.unbounded = HE::uiElementRectUnbounded(t, e, c);
        a.anchor    = HE::uiElementAnchorRect(t, e, c);
        a.clipped   = HE::uiElementClipRect(t, e, a.clip, c);
        a.rotated   = HE::uiElementRotation(t, e, a.rot, c);
        a.visible   = HE::uiElementEffectiveVisible(t, e);
        a.enabled   = HE::uiElementEffectiveEnabled(t, e);
        a.opacity   = HE::uiElementEffectiveOpacity(t, e);
        HE::uiElementUnitScale(t, e, a.usx, a.usy, c);
        a.childIndex = HE::uiChildIndexOf(t, e.parentId, e.id);
        a.childCount = HE::uiChildCountOf(t, e.id);
        a.found      = t.find(e.id);
        a.children   = t.childrenOf(e.id);
        return a;
    }

    void checkSame(const Answers& want, const Answers& got, const HE::UIElement& e)
    {
        INFO("element " << e.id << " (" << std::string(e.typeName()) << ")");
        CHECK(sameRect(want.rect, got.rect));
        CHECK(sameRect(want.unbounded, got.unbounded));
        CHECK(sameRect(want.anchor, got.anchor));
        CHECK(want.clipped == got.clipped);
        if (want.clipped && got.clipped) CHECK(sameRect(want.clip, got.clip));
        CHECK(want.rotated == got.rotated);
        if (want.rotated && got.rotated)
        {
            CHECK(want.rot.degrees == got.rot.degrees);
            CHECK(want.rot.srcX == got.rot.srcX); CHECK(want.rot.srcY == got.rot.srcY);
            CHECK(want.rot.dstX == got.rot.dstX); CHECK(want.rot.dstY == got.rot.dstY);
        }
        CHECK(want.visible == got.visible);
        CHECK(want.enabled == got.enabled);
        CHECK(want.opacity == got.opacity);
        CHECK(want.usx == got.usx); CHECK(want.usy == got.usy);
        CHECK(want.childIndex == got.childIndex);
        CHECK(want.childCount == got.childCount);
        CHECK(want.found == got.found);
        CHECK(want.children == got.children);
    }

    // Answers outside any scope (the walk), then inside one — structure only,
    // and with values memoized in tree order and in reverse order, because the
    // memo is filled by whoever asks first (a box writes all its children's
    // slots when the first one asks) and the order must not matter.
    void checkScopedMatchesWalk(const HE::UIWidgetTree& t, const HE::UIWidgetCanvas* c)
    {
        std::vector<Answers> want;
        for (const auto& ep : t.elements) want.push_back(ask(t, *ep, c));

        for (int pass = 0; pass < 3; ++pass)
        {
            CAPTURE(pass);
            HE::UILayoutScope scope(t, c);
            if (pass > 0) scope.memoizeValues();
            const int n = static_cast<int>(t.elements.size());
            for (int k = 0; k < n; ++k)
            {
                const int i = pass == 2 ? n - 1 - k : k;
                const HE::UIElement& e = *t.elements[static_cast<std::size_t>(i)];
                checkSame(want[static_cast<std::size_t>(i)], ask(t, e, c), e);
            }
        }
    }

    double msSince(std::chrono::steady_clock::time_point t0, int frames)
    {
        const auto dt = std::chrono::steady_clock::now() - t0;
        return std::chrono::duration<double, std::milli>(dt).count() / frames;
    }
}

// The before/after numbers in docs/audit-core-fundamente-2026-10-17.md come from
// this case. Skipped in a normal run (it measures, it does not check, and a
// timing is no assertion): he_tests --source-file=*test_ui_layout_scope.cpp --no-skip
TEST_CASE("UI layout scope: frame cost of an application shell" * doctest::skip())
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    HE::UIWidgetTree tree;
    buildShell(tree);
    const int elements = static_cast<int>(tree.elements.size());

    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    a.path     = "mem://shell.hasset";
    cm.registerWidget(std::move(a));

    WidgetManager wm;
    const int id = wm.createWidget(cm, "mem://shell.hasset");
    REQUIRE(id != 0);
    wm.showWidget(id);

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);           // warm: font bake, list sync
    const std::size_t quads = out.size();

    const int frames = 20;
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f)
    {
        out.clear();
        wm.extract(1920.0f, 1080.0f, out);
    }
    const double extractMs = msSince(t0, frames);

    t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f)
        wm.processPointer(1920.0f, 1080.0f, 1200.0f + f * 7.0f, 300.0f + f * 11.0f,
                          false, true);
    const double pointerMs = msSince(t0, frames);

    std::printf("[ui-layout-bench] elements=%d quads=%zu extract=%.3f ms/frame "
                "processPointer=%.3f ms/call\n",
                elements, quads, extractMs, pointerMs);
    CHECK(quads > 0);

    // Where a frame goes, on a copy of the same tree: auto-size (structure
    // tables only, as extract runs it), then every question the draw asks of
    // every element, memoized. The rest of extract is the elements drawing.
    const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(tree, 1920.0f, 1080.0f);
    t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f)
    {
        HE::UILayoutScope scope(tree, &canvas);
        HE::uiApplyAutoSize(tree, &canvas, 1.0f);
        HE::uiUpdateScrollExtents(tree);
    }
    const double autoMs = msSince(t0, frames);
    t0 = std::chrono::steady_clock::now();
    int shown = 0;
    for (int f = 0; f < frames; ++f)
    {
        HE::UILayoutScope scope(tree, &canvas);
        scope.memoizeValues();
        for (const auto& ep : tree.elements)
        {
            if (!HE::uiElementEffectiveVisible(tree, *ep)) continue;
            ++shown;
            HE::UIWidgetRect clip{};
            HE::UIRotation rot;
            float us = 1.0f, vs = 1.0f;
            (void)HE::uiElementRect(tree, *ep, &canvas);
            (void)HE::uiElementClipRect(tree, *ep, clip, &canvas);
            (void)HE::uiElementEffectiveOpacity(tree, *ep);
            (void)HE::uiElementEffectiveEnabled(tree, *ep);
            (void)HE::uiElementRotation(tree, *ep, rot, &canvas);
            HE::uiElementUnitScale(tree, *ep, us, vs, &canvas);
        }
    }
    const double askMs = msSince(t0, frames);
    std::printf("[ui-layout-bench] autosize+scroll=%.3f ms/frame layout-questions=%.3f "
                "ms/frame (%d shown)\n", autoMs, askMs, shown / frames);
}

// ── The scope answers exactly what the walk answers ──────────────────────────
// Zero tolerance: the scoped path runs the same arithmetic in the same order,
// so any difference at all is a table that went wrong, not rounding.
TEST_CASE("UI layout scope: every answer matches the walk, on the authored canvas")
{
    HE::UIWidgetTree t;
    buildEverything(t);
    checkScopedMatchesWalk(t, nullptr);
}

TEST_CASE("UI layout scope: every answer matches the walk, on a scaled canvas")
{
    HE::UIWidgetTree t;
    buildEverything(t);
    const HE::UIWidgetCanvas c = HE::uiResolveCanvas(t, 1700.0f, 900.0f, 1.25f);
    checkScopedMatchesWalk(t, &c);
}

// A scope remembers rects for ONE canvas. Asked on another, it must not hand
// back what it remembered — the designer and the runtime ask on different ones.
TEST_CASE("UI layout scope: a question on another canvas is not answered from the memo")
{
    HE::UIWidgetTree t;
    buildEverything(t);
    const HE::UIWidgetCanvas c = HE::uiResolveCanvas(t, 1700.0f, 900.0f, 1.0f);
    std::vector<Answers> want;
    for (const auto& ep : t.elements) want.push_back(ask(t, *ep, nullptr));

    HE::UILayoutScope scope(t, &c);
    scope.memoizeValues();
    for (const auto& ep : t.elements) (void)ask(t, *ep, &c);       // fill the memo
    for (std::size_t i = 0; i < t.elements.size(); ++i)
        checkSame(want[i], ask(t, *t.elements[i], nullptr), *t.elements[i]);
}

// Auto-size WRITES sizes element by element, so it runs with the structure
// tables only. What it writes must be what it wrote without them.
TEST_CASE("UI layout scope: auto-size and scroll extents write the same sizes inside a scope")
{
    HE::UIWidgetTree base;
    buildEverything(base);
    // Containers that measure themselves, so the innermost-first pass and the
    // grid/wrap measurement run too.
    for (auto& ep : base.elements)
        if (auto* box = dynamic_cast<HE::UIBoxBase*>(ep.get()))
            if (ep->type() != HE::UIWidgetType::ScrollBox && ep->name != "Line")
                box->sizeToContent = true;

    HE::UIWidgetTree walked = base, scoped = base;
    const HE::UIWidgetCanvas c = HE::uiResolveCanvas(base, 1600.0f, 900.0f, 1.0f);

    // Three frames: the first one moves almost every size (the memo auto-size
    // borrows is forgotten write after write), the second is steady (nothing
    // forgotten, everything from the memo), the third follows the caption
    // getting longer — one write, and the wrapping text beside it has to see it.
    std::vector<float> bodyHeights;
    for (int frame = 0; frame < 3; ++frame)
    {
        CAPTURE(frame);
        if (frame == 2)
            for (HE::UIWidgetTree* t : { &walked, &scoped })
                for (auto& ep : t->elements)
                    if (auto* tx = dynamic_cast<HE::UIText*>(ep.get()); tx && tx->text == "Caption:")
                        tx->text = "A caption that grew:";
        HE::uiApplyAutoSize(walked, &c, 1.5f);
        HE::uiUpdateScrollExtents(walked);
        {
            HE::UILayoutScope scope(scoped, &c);
            HE::uiApplyAutoSize(scoped, &c, 1.5f);
            HE::uiUpdateScrollExtents(scoped);
        }
        CHECK(HE::uiWidgetTreeToJson(walked) == HE::uiWidgetTreeToJson(scoped));
        for (const auto& ep : walked.elements)
            if (const auto* tx = dynamic_cast<const HE::UIText*>(ep.get());
                tx && tx->text.rfind("words that wrap", 0) == 0)
                bodyHeights.push_back(tx->sizeY);
    }
    // The case is only a case if the caption's growth reached the wrapping text.
    REQUIRE(bodyHeights.size() == 3);
    CHECK(bodyHeights[2] != bodyHeights[1]);
    for (std::size_t i = 0; i < walked.elements.size(); ++i)
    {
        const HE::UIElement& a = *walked.elements[i];
        const HE::UIElement& b = *scoped.elements[i];
        INFO("element " << a.id);
        CHECK(a.sizeX == b.sizeX);
        CHECK(a.sizeY == b.sizeY);
        if (const auto* ta = dynamic_cast<const HE::UITabBox*>(&a))
            CHECK(ta->tabLabels == static_cast<const HE::UITabBox&>(b).tabLabels);
        if (const auto* aa = dynamic_cast<const HE::UIAccordion*>(&a))
        {
            const auto& ab = static_cast<const HE::UIAccordion&>(b);
            CHECK(aa->sectionLabels == ab.sectionLabels);
            CHECK(aa->sectionBodies == ab.sectionBodies);
            CHECK(aa->contentExtent == ab.contentExtent);
        }
        if (const auto* sa = dynamic_cast<const HE::UIScrollBox*>(&a))
        {
            CHECK(sa->contentExtent == static_cast<const HE::UIScrollBox&>(b).contentExtent);
            CHECK(sa->scrollOffset  == static_cast<const HE::UIScrollBox&>(b).scrollOffset);
        }
    }
}

// The scope belongs to one tree on one thread. Another tree asked while it is
// open walks as before, and a scope opened inside another hands the outer one
// back when it closes.
TEST_CASE("UI layout scope: only the scoped tree is answered from the tables")
{
    HE::UIWidgetTree a, b;
    buildEverything(a);
    buildShell(b);
    const HE::UIElement& probe = *b.elements[b.elements.size() / 2];
    const HE::UIWidgetRect bWalk = HE::uiElementRect(b, probe, nullptr);
    const int bIndex = HE::uiChildIndexOf(b, probe.parentId, probe.id);

    HE::UILayoutScope outer(a, nullptr);
    outer.memoizeValues();
    CHECK(sameRect(HE::uiElementRect(b, probe, nullptr), bWalk));
    CHECK(HE::uiChildIndexOf(b, probe.parentId, probe.id) == bIndex);
    {
        HE::UILayoutScope inner(b, nullptr);
        inner.memoizeValues();
        CHECK(sameRect(HE::uiElementRect(b, probe, nullptr), bWalk));
    }
    // The outer scope is still the one answering for `a`.
    for (const auto& ep : a.elements) CHECK(a.find(ep->id) == ep.get());
}

// ── A label measures again only when what it measures from changed (CORE-602)
// Each step compares the cached label against a FRESH one with the same inputs,
// which measures for certain: a key that misses an input shows up as a size
// that stayed behind.
TEST_CASE("UI text auto-size: a changed input re-measures in the same call")
{
    HE::UIText label;
    label.text = "Short";
    const auto expectSameAsFresh = [&](float width, float fontScale)
    {
        HE::UIText fresh;
        fresh.text = label.text; fresh.fontSize = label.fontSize;
        fresh.wordWrap = label.wordWrap; fresh.richText = label.richText;
        fresh.alignH = label.alignH; fresh.alignV = label.alignV;
        fresh.sizeX = label.sizeX; fresh.sizeY = label.sizeY;
        label.applyAutoSize(width, fontScale);
        fresh.applyAutoSize(width, fontScale);
        CHECK(label.sizeX == fresh.sizeX);
        CHECK(label.sizeY == fresh.sizeY);
    };

    expectSameAsFresh(200.0f, 1.0f);
    const float shortW = label.sizeX;
    expectSameAsFresh(200.0f, 1.0f);                  // nothing changed: the cache
    CHECK(label.sizeX == shortW);

    label.text = "A considerably longer label";        // the words
    expectSameAsFresh(200.0f, 1.0f);
    CHECK(label.sizeX > shortW);

    expectSameAsFresh(200.0f, 1.5f);                  // the reader's text size
    label.fontSize = 30.0f;                            // the authored size
    expectSameAsFresh(200.0f, 1.5f);

    label.wordWrap = true;                             // the wrap column
    label.sizeX = 120.0f;
    expectSameAsFresh(120.0f, 1.0f);
    const float wrappedH = label.sizeY;
    expectSameAsFresh(60.0f, 1.0f);
    CHECK(label.sizeY > wrappedH);

    label.richText = true;                             // the markup
    label.text = "A <b>bold</b> and <size=2>big</size> word";
    expectSameAsFresh(300.0f, 1.0f);

    // A script that wrote the size in between is overruled, as before: only the
    // measurement is cached, the write is not skipped.
    const float h = label.sizeY;
    label.sizeY = 3.0f;
    label.applyAutoSize(300.0f, 1.0f);
    CHECK(label.sizeY == h);
}
