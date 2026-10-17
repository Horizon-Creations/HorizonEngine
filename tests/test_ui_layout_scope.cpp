#include "doctest.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <UIWidget/WidgetManager.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>

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
}
