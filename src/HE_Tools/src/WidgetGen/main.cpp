// widget_gen — generates the engine's built-in component library as loose
// .hasset UI Widget assets, written via ContentManager::saveAsset so the byte
// layout is identical to editor-authored widgets.
//
// Usage:  widget_gen <output-dir>
//   <output-dir> is the folder the .hasset files are written into (e.g.
//   EditorDeps/EngineContent/Widgets). It is used verbatim as the ContentManager
//   content root, and each component is saved under "<Name>.hasset".
//
// Same bargain mesh_gen makes: output is deterministic (every component gets a
// stable, well-known UUID), the results are COMMITTED, and this is not part of
// the normal build graph. Re-running produces byte-identical files, so a page
// that embeds "Engine/Widgets/Card.hasset" keeps working across regenerations.
//
// ── Why a generator and not twelve hand-saved assets ─────────────────────────
// A .hasset is a binary container. Twelve of them in the repository would be
// twelve blobs nobody can review, diff, or fix without opening the editor — and
// the library is exactly the thing that has to be reviewable, because every
// project inherits it. Here, a component is a function.
//
// ── Two rules every component here follows ───────────────────────────────────
// 1. COLOURS AND SIZES COME FROM THE THEME. Every colour binds a role and every
//    text size a level (see role()/textLevel()); the literal beside it is the
//    default theme's value, written so the DESIGNER — which runs no theme pass —
//    shows the component the way it will look. The literal is the preview, the
//    role is the truth.
// 2. A COMPONENT IS A WHOLE THING, NOT A FRAME WITH A HOLE. A WidgetRef grafts a
//    finished tree; a host cannot place its own children inside it. So there is
//    no "form row you put your control in" — there is a text row, a toggle row
//    and a choice row, each complete. That limit is worth naming rather than
//    designing around: the alternative is a slot mechanism, and this library is
//    what tells us whether one is actually needed.

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonCode/HorizonCode.h>
#include <Types/UUID.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UITheme.h>
#include <UIWidget/UIWidgetTree.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
using namespace HE;

// Well-known UUID base for the built-in components. `hi` stays far below the
// version-4 bit pattern UUID::generate() enforces and clear of both the
// DefaultAssets sentinels (1..7) and mesh_gen's 0x100 block.
constexpr uint64_t kWidgetBaseHi = 0x0000000000000200ULL;

// ── The builder ──────────────────────────────────────────────────────────────
// Thin on purpose: it is the tree model with shorter names, not a second layout
// system. Anything it cannot say is said by touching the element directly.
struct Comp
{
    UIWidgetTree t;

    Comp(float w, float h)
    {
        t.canvasWidth = w; t.canvasHeight = h;
        // One unit is one pixel, and the WidgetRef's slot is the screen: the
        // component's anchors then place it against the real slot instead of
        // its contents being scaled to fit. A component that is SCALED into its
        // slot has text that changes size with the layout, which is exactly
        // what nobody wants from a form row.
        t.scaleMode = UICanvasScaleMode::ConstantPixel;
    }

    int add(UIWidgetType type, int parent, const char* name)
    {
        const int id = t.add(type);
        UIElement& e = *t.find(id);
        e.parentId = parent;
        e.name     = name;
        return id;
    }
    UIElement& at(int id) { return *t.find(id); }

    // Fill the parent with the given insets. The commonest thing a component
    // element does, and four numbers rather than an anchor preset plus two
    // inset calls every time.
    void fill(int id, float l = 0.0f, float top = 0.0f, float r = 0.0f, float b = 0.0f)
    {
        UIElement& e = at(id);
        uiSetAnchorPreset(e, kUIAnchorFill);
        uiSetAnchorInsetsX(e, l, r);
        uiSetAnchorInsetsY(e, top, b);
    }
    // A fixed-height strip across the parent's whole width, `y` from the top.
    void strip(int id, float y, float h, float l = 0.0f, float r = 0.0f)
    {
        UIElement& e = at(id);
        uiSetAnchorPreset(e, 3);          // whole top side
        uiSetAnchorInsetsX(e, l, r);
        e.pivotY = 0.0f; e.posY = y; e.sizeY = h;
    }

    void prop(int id, const char* name, const UIPropValue& v) { at(id).setPropAny(name, v); }
    void str(int id, const char* name, const std::string& v)
    { prop(id, name, UIPropValue::ofString(v)); }
    void num(int id, const char* name, float v)  { prop(id, name, UIPropValue::ofFloat(v)); }
    void flag(int id, const char* name, bool v)  { prop(id, name, UIPropValue::ofBool(v)); }
    void whole(int id, const char* name, int v)  { prop(id, name, UIPropValue::ofInt(v)); }

    // Bind a colour property to a theme role. The literal is filled in at the
    // end by bake(), from the default theme.
    void role(int id, const char* prop, UIThemeRole r)
    { at(id).setThemeRole(prop, uiThemeRoleName(r)); }
    // …and a text size to a typography level. Same map, and the PROPERTY is what
    // decides which vocabulary the name belongs to (see uiApplyTheme).
    void textLevel(int id, UIThemeTextLevel lvl)
    { at(id).setThemeRole("FontSize", uiThemeTextLevelName(lvl)); }
    void radius(int id, UIThemeSize step)
    { at(id).setThemeRole("Corner Radius", uiThemeSizeName(step)); }

    void param(const char* name, int elem, const char* property, const char* help)
    {
        UIWidgetParam p;
        p.name = name; p.elementId = elem; p.property = property; p.help = help;
        t.params.push_back(std::move(p));
    }

    // A caption on a Button. A Button is a surface; what sits on it is a child,
    // and that child must never take the click meant for the button under it.
    int caption(int button, const char* name, const std::string& text)
    {
        const int id = add(UIWidgetType::Text, button, name);
        fill(id);
        str(id, "Text", text);
        whole(id, "Align H", 1); whole(id, "Align V", 1);   // centred both ways
        role(id, "Color", UIThemeRole::Text);
        textLevel(id, UIThemeTextLevel::Body);
        at(id).hitTestable = false;
        return id;
    }

    // Write every bound role's value into the ordinary property. The runtime
    // does this on creation against the app's own theme; doing it here is what
    // makes the DESIGNER — which runs no theme pass at all — show the component
    // as it will look instead of as a pile of white rectangles.
    void bake() { uiApplyTheme(t, uiDefaultTheme(), UIThemeMode::Light); }
};

// ── The components ───────────────────────────────────────────────────────────

// The three form rows. A settings page is these stacked in a vertical box, and
// they are three rather than one because a component cannot be handed a control
// to put in its middle (see the header).
//
// The shape they share: a label column of fixed width, the control beside it,
// and a help line under both that is hidden until somebody writes help. Fixed
// and not `auto`, because each row is its own widget and an `auto` column would
// measure only its own label — twenty rows would come out twenty widths.
Comp formRowShell(int& labelId, int& helpId, int& lineId)
{
    Comp c(480.0f, 62.0f);
    const int rootId = c.add(UIWidgetType::VerticalBox, 0, "Row");
    c.fill(rootId);
    c.num(rootId, "Padding", 0.0f);
    c.num(rootId, "Spacing", 2.0f);

    lineId = c.add(UIWidgetType::HorizontalBox, rootId, "Line");
    c.at(lineId).sizeY = 32.0f;
    c.num(lineId, "Padding", 0.0f);
    c.num(lineId, "Spacing", 12.0f);

    labelId = c.add(UIWidgetType::Text, lineId, "Label");
    c.at(labelId).sizeX = 150.0f;
    c.str(labelId, "Text", "Label");
    c.whole(labelId, "Align V", 1);
    c.role(labelId, "Color", UIThemeRole::Text);
    c.textLevel(labelId, UIThemeTextLevel::Body);

    helpId = c.add(UIWidgetType::Text, rootId, "Help");
    c.at(helpId).sizeY = 16.0f;
    c.str(helpId, "Text", "");
    // Hidden until told otherwise: a component that always shows an empty line
    // makes every row in a settings page taller for nothing.
    c.at(helpId).visible = false;
    c.role(helpId, "Color", UIThemeRole::MutedText);
    c.textLevel(helpId, UIThemeTextLevel::Small);
    return c;
}

void addHelpParams(Comp& c, int labelId, int helpId)
{
    c.param("Label", labelId, "Text", "What the row is called.");
    c.param("Help", helpId, "Text",
            "One line under the row. Turn Show Help on as well, or it is written "
            "into a line nobody sees.");
    c.param("Show Help", helpId, "Visible",
            "Whether the help line takes up space at all. Off by default, so a "
            "row without help is as tall as its control.");
}

Comp formRowText()
{
    int label = 0, help = 0, line = 0;
    Comp c = formRowShell(label, help, line);
    const int field = c.add(UIWidgetType::TextInput, line, "Field");
    c.at(field).slotFill = 1.0f;
    c.str(field, "Placeholder", "");
    c.role(field, "Back Color", UIThemeRole::Surface);
    c.role(field, "Text Color", UIThemeRole::Text);
    c.role(field, "Selection Color", UIThemeRole::Accent);
    c.textLevel(field, UIThemeTextLevel::Body);
    c.radius(field, UIThemeSize::Small);
    c.at(field).borderWidth = 1.0f;
    c.role(field, "Border Color", UIThemeRole::Border);

    addHelpParams(c, label, help);
    c.param("Value", field, "Text", "What the field starts out holding.");
    c.param("Placeholder", field, "Placeholder",
            "Greyed-out text shown while the field is empty. A hint, not a value.");
    c.param("Password", field, "Password", "Draw the text as dots.");
    c.bake();
    return c;
}

Comp formRowToggle()
{
    int label = 0, help = 0, line = 0;
    Comp c = formRowShell(label, help, line);
    const int box = c.add(UIWidgetType::CheckBox, line, "Toggle");
    c.at(box).slotFill = 1.0f;
    // The checkbox's own label stays empty: the row already has one on the left,
    // and two would be two places to change the same word.
    c.str(box, "Label", "");
    c.role(box, "Box Color", UIThemeRole::Surface);
    c.role(box, "Check Color", UIThemeRole::Accent);
    c.role(box, "Text Color", UIThemeRole::Text);
    c.textLevel(box, UIThemeTextLevel::Body);

    addHelpParams(c, label, help);
    c.param("Checked", box, "Checked", "Whether the switch starts on.");
    c.bake();
    return c;
}

Comp formRowChoice()
{
    int label = 0, help = 0, line = 0;
    Comp c = formRowShell(label, help, line);
    const int combo = c.add(UIWidgetType::ComboBox, line, "Choice");
    c.at(combo).slotFill = 1.0f;
    UIPropValue opts;
    opts.type = UIPropType::StringList;
    opts.list = { "One", "Two", "Three" };
    c.prop(combo, "Options", opts);
    c.role(combo, "Back Color", UIThemeRole::Surface);
    c.role(combo, "Text Color", UIThemeRole::Text);
    c.role(combo, "Highlight Color", UIThemeRole::Accent);
    c.textLevel(combo, UIThemeTextLevel::Body);
    c.radius(combo, UIThemeSize::Small);

    addHelpParams(c, label, help);
    c.param("Options", combo, "Options", "The list to choose from, one entry per line.");
    c.param("Selected", combo, "Selected Index",
            "Which entry is picked to start with, counting from zero.");
    c.bake();
    return c;
}

// A heading with a rule under it. What turns a long form into a settings page.
Comp sectionHeader()
{
    Comp c(480.0f, 40.0f);
    const int root = c.add(UIWidgetType::VerticalBox, 0, "Section");
    c.fill(root);
    c.num(root, "Padding", 0.0f);
    c.num(root, "Spacing", 6.0f);

    const int title = c.add(UIWidgetType::Text, root, "Title");
    c.at(title).sizeY = 24.0f;
    c.str(title, "Text", "Section");
    c.role(title, "Color", UIThemeRole::Text);
    c.textLevel(title, UIThemeTextLevel::Heading);

    const int rule = c.add(UIWidgetType::Panel, root, "Rule");
    c.at(rule).sizeY = 1.0f;
    c.role(rule, "Color", UIThemeRole::Border);

    c.param("Title", title, "Text", "The section's name.");
    c.param("Show Rule", rule, "Visible",
            "The hairline under the heading. Off for the first section of a page, "
            "where the window's own edge already does the separating.");
    c.bake();
    return c;
}

// A raised surface with a heading and a paragraph. The dashboard tile.
Comp card()
{
    Comp c(320.0f, 180.0f);
    const int surface = c.add(UIWidgetType::Panel, 0, "Surface");
    c.fill(surface);
    c.role(surface, "Color", UIThemeRole::Surface);
    c.radius(surface, UIThemeSize::Medium);
    c.at(surface).borderWidth = 1.0f;
    c.role(surface, "Border Color", UIThemeRole::Border);
    c.at(surface).shadow = true;
    c.at(surface).shadowBlur = 10.0f;
    c.at(surface).shadowOffsetY = 3.0f;

    const int body = c.add(UIWidgetType::VerticalBox, surface, "Body");
    c.fill(body);
    c.num(body, "Padding", 16.0f);
    c.num(body, "Spacing", 8.0f);

    const int title = c.add(UIWidgetType::Text, body, "Title");
    c.at(title).sizeY = 26.0f;
    c.str(title, "Text", "Card");
    c.role(title, "Color", UIThemeRole::Text);
    c.textLevel(title, UIThemeTextLevel::Heading);

    const int text = c.add(UIWidgetType::Text, body, "Body");
    c.at(text).slotFill = 1.0f;
    c.str(text, "Text", "What this card is about.");
    c.flag(text, "WordWrap", true);
    c.role(text, "Color", UIThemeRole::MutedText);
    c.textLevel(text, UIThemeTextLevel::Body);

    c.param("Title", title, "Text", "The card's heading.");
    c.param("Body", text, "Text", "The paragraph under it. Wraps on its own.");
    c.bake();
    return c;
}

// Title, subtitle, and a value on the right. Built to be a ListView's Row
// Widget: it paints no background of its own, because hover and selection are
// the list's business and a row that painted over them would hide them.
Comp listRow()
{
    Comp c(400.0f, 56.0f);
    const int row = c.add(UIWidgetType::HorizontalBox, 0, "Row");
    c.fill(row);
    c.num(row, "Padding", 10.0f);
    c.num(row, "Spacing", 12.0f);

    const int texts = c.add(UIWidgetType::VerticalBox, row, "Texts");
    c.at(texts).slotFill = 1.0f;
    c.num(texts, "Padding", 0.0f);
    c.num(texts, "Spacing", 2.0f);

    const int title = c.add(UIWidgetType::Text, texts, "Title");
    c.at(title).sizeY = 20.0f;
    c.str(title, "Text", "Title");
    c.role(title, "Color", UIThemeRole::Text);
    c.textLevel(title, UIThemeTextLevel::Body);

    const int sub = c.add(UIWidgetType::Text, texts, "Subtitle");
    c.at(sub).sizeY = 16.0f;
    c.str(sub, "Text", "");
    c.at(sub).visible = false;
    c.role(sub, "Color", UIThemeRole::MutedText);
    c.textLevel(sub, UIThemeTextLevel::Small);

    const int value = c.add(UIWidgetType::Text, row, "Value");
    c.at(value).sizeX = 90.0f;
    c.str(value, "Text", "");
    c.whole(value, "Align H", 2); c.whole(value, "Align V", 1);   // right, middle
    c.role(value, "Color", UIThemeRole::MutedText);
    c.textLevel(value, UIThemeTextLevel::Body);

    c.param("Title", title, "Text", "The row's main line.");
    c.param("Subtitle", sub, "Text", "A second, quieter line under it.");
    c.param("Show Subtitle", sub, "Visible",
            "Whether the second line takes up space. Off by default, so a plain "
            "list stays one line per row.");
    c.param("Value", value, "Text", "What is shown on the right — a size, a date, a count.");
    c.bake();
    return c;
}

// The bar a borderless window draws instead of the system's. Its buttons emit
// nothing on their own: what "close" means is the application's decision, so the
// page that places this wires its own graph to them.
Comp titleBar()
{
    Comp c(900.0f, 36.0f);
    const int bar = c.add(UIWidgetType::Panel, 0, "Bar");
    c.fill(bar);
    c.role(bar, "Color", UIThemeRole::Surface);

    const int title = c.add(UIWidgetType::Text, bar, "Title");
    c.fill(title, 12.0f, 0.0f, 150.0f, 0.0f);
    c.str(title, "Text", "Application");
    c.whole(title, "Align V", 1);
    c.role(title, "Color", UIThemeRole::Text);
    c.textLevel(title, UIThemeTextLevel::Body);

    const int buttons = c.add(UIWidgetType::HorizontalBox, bar, "Buttons");
    { UIElement& e = c.at(buttons);
      uiSetAnchorPreset(e, 14);            // right edge, stretched down
      uiSetAnchorInsetsY(e, 0.0f, 0.0f);
      e.pivotX = 1.0f; e.posX = 0.0f; e.sizeX = 132.0f; }
    c.num(buttons, "Padding", 0.0f);
    c.num(buttons, "Spacing", 0.0f);

    // Plain ASCII captions on purpose. A glyph the shipped font does not have
    // draws as nothing, and a close button that is blank is worse than one that
    // says X.
    struct Btn { const char* name; const char* text; };
    const Btn kBtns[] = { { "Minimise", "-" }, { "Maximise", "[]" }, { "Close", "X" } };
    int ids[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        ids[i] = c.add(UIWidgetType::Button, buttons, kBtns[i].name);
        c.at(ids[i]).sizeX = 44.0f;
        c.at(ids[i]).cornerRadius = glm::vec4(0.0f);
        c.role(ids[i], "Normal Color", UIThemeRole::Surface);
        c.role(ids[i], "Hovered Color", UIThemeRole::Border);
        c.role(ids[i], "Pressed Color", UIThemeRole::Accent);
        c.caption(ids[i], "Caption", kBtns[i].text);
    }
    // The close button is the one that should read as dangerous when pressed.
    c.role(ids[2], "Hovered Color", UIThemeRole::Error);
    c.role(ids[2], "Pressed Color", UIThemeRole::Error);

    c.param("Title", title, "Text", "What the window is called.");
    c.param("Show Minimise", ids[0], "Visible", "Whether the minimise button is there.");
    c.param("Show Maximise", ids[1], "Visible",
            "Whether the maximise button is there. Off for a window that has one "
            "size and means it.");
    c.bake();
    return c;
}

// Three labelled buttons in a strip. Three and not four because a toolbar that
// needs a fourth needs a real one, and this is the shape most windows want.
Comp toolbar()
{
    Comp c(900.0f, 44.0f);
    const int bar = c.add(UIWidgetType::Panel, 0, "Bar");
    c.fill(bar);
    c.role(bar, "Color", UIThemeRole::Surface);

    const int rule = c.add(UIWidgetType::Panel, bar, "Rule");
    { UIElement& e = c.at(rule);
      uiSetAnchorPreset(e, 11);            // whole bottom side
      uiSetAnchorInsetsX(e, 0.0f, 0.0f);
      e.pivotY = 1.0f; e.posY = 0.0f; e.sizeY = 1.0f; }
    c.role(rule, "Color", UIThemeRole::Border);

    const int row = c.add(UIWidgetType::HorizontalBox, bar, "Actions");
    c.fill(row, 8.0f, 6.0f, 8.0f, 7.0f);
    c.num(row, "Padding", 0.0f);
    c.num(row, "Spacing", 6.0f);

    const char* kNames[3] = { "Action 1", "Action 2", "Action 3" };
    int ids[3] = {}, caps[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        ids[i] = c.add(UIWidgetType::Button, row, kNames[i]);
        c.at(ids[i]).sizeX = 104.0f;
        c.radius(ids[i], UIThemeSize::Small);
        c.role(ids[i], "Normal Color", UIThemeRole::Background);
        c.role(ids[i], "Hovered Color", UIThemeRole::Border);
        c.role(ids[i], "Pressed Color", UIThemeRole::Accent);
        caps[i] = c.caption(ids[i], "Caption", kNames[i]);
    }
    for (int i = 0; i < 3; ++i)
    {
        const std::string n = std::string("Action ") + static_cast<char>('1' + i);
        c.param((n + " Label").c_str(), caps[i], "Text", "What this button says.");
        c.param((n + " Shown").c_str(), ids[i], "Visible",
                "Whether this button is there. A toolbar with two actions "
                "switches the third off rather than leaving a gap.");
    }
    c.bake();
    return c;
}

// The strip along the bottom: a message on the left, a detail on the right.
Comp statusBar()
{
    Comp c(900.0f, 26.0f);
    const int bar = c.add(UIWidgetType::Panel, 0, "Bar");
    c.fill(bar);
    c.role(bar, "Color", UIThemeRole::Surface);

    const int rule = c.add(UIWidgetType::Panel, bar, "Rule");
    c.strip(rule, 0.0f, 1.0f);
    c.role(rule, "Color", UIThemeRole::Border);

    const int msg = c.add(UIWidgetType::Text, bar, "Message");
    c.fill(msg, 10.0f, 0.0f, 220.0f, 0.0f);
    c.str(msg, "Text", "Ready");
    c.whole(msg, "Align V", 1);
    c.role(msg, "Color", UIThemeRole::MutedText);
    c.textLevel(msg, UIThemeTextLevel::Small);

    const int detail = c.add(UIWidgetType::Text, bar, "Detail");
    { UIElement& e = c.at(detail);
      uiSetAnchorPreset(e, 14);
      uiSetAnchorInsetsY(e, 0.0f, 0.0f);
      e.pivotX = 1.0f; e.posX = -10.0f; e.sizeX = 200.0f; }
    c.str(detail, "Text", "");
    c.whole(detail, "Align H", 2); c.whole(detail, "Align V", 1);
    c.role(detail, "Color", UIThemeRole::MutedText);
    c.textLevel(detail, UIThemeTextLevel::Small);

    c.param("Message", msg, "Text", "What the application is doing, in a few words.");
    c.param("Detail", detail, "Text",
            "The right-hand corner: a count, a position, a connection state.");
    c.bake();
    return c;
}

// A pill-shaped field with room for an icon. The icon is a slot rather than a
// shipped picture: the engine has no icon set, and an empty Image that a project
// points at its own file is honest where a hard-coded glyph would not be.
Comp searchField()
{
    Comp c(320.0f, 34.0f);
    const int frame = c.add(UIWidgetType::Panel, 0, "Frame");
    c.fill(frame);
    c.role(frame, "Color", UIThemeRole::Surface);
    // Half the height, so it is a pill at the authored size and stays one for
    // anything shorter. Not bound to a theme step: this radius is a SHAPE, and a
    // theme that made its corners 4 would turn the pill into a box.
    c.at(frame).cornerRadius = glm::vec4(17.0f);
    c.at(frame).borderWidth = 1.0f;
    c.role(frame, "Border Color", UIThemeRole::Border);
    // The focus ring goes round the PILL, not round the text field inset inside
    // it. The field is what takes the keyboard; the frame is what a person sees
    // as the control, and a ring around the field alone is a small rectangle
    // floating inside a pill (UIElement::focusFrame).
    c.at(frame).focusFrame = true;

    const int icon = c.add(UIWidgetType::Image, frame, "Icon");
    { UIElement& e = c.at(icon);
      uiSetAnchorPreset(e, 4);             // middle-left point
      e.pivotX = 0.0f; e.pivotY = 0.5f;
      e.posX = 11.0f; e.posY = 0.0f; e.sizeX = e.sizeY = 16.0f;
      e.visible = false; e.hitTestable = false; }
    c.role(icon, "Tint", UIThemeRole::MutedText);

    const int field = c.add(UIWidgetType::TextInput, frame, "Field");
    c.fill(field, 14.0f, 2.0f, 12.0f, 2.0f);
    c.str(field, "Placeholder", "Search");
    // Transparent: the frame around it is what is being seen, and a second
    // filled rectangle inside a pill shows its own square corners.
    c.prop(field, "Back Color", UIPropValue::ofColor(glm::vec4(0.0f)));
    c.role(field, "Text Color", UIThemeRole::Text);
    c.role(field, "Selection Color", UIThemeRole::Accent);
    c.textLevel(field, UIThemeTextLevel::Body);

    c.param("Placeholder", field, "Placeholder", "The hint shown while it is empty.");
    c.param("Text", field, "Text", "What it starts out holding.");
    c.param("Icon", icon, "Texture",
            "A picture to put on the left. Switch Show Icon on as well, and move "
            "the field's left inset if yours is a different size.");
    c.param("Show Icon", icon, "Visible", "Whether the icon takes up its place.");
    c.bake();
    return c;
}

// What a list shows when it has nothing to show. The single most-forgotten
// screen in an application, which is exactly why it ships.
Comp emptyState()
{
    Comp c(400.0f, 180.0f);
    const int root = c.add(UIWidgetType::VerticalBox, 0, "Empty");
    c.fill(root);
    c.num(root, "Padding", 24.0f);
    c.num(root, "Spacing", 8.0f);

    const int head = c.add(UIWidgetType::Text, root, "Heading");
    c.at(head).sizeY = 30.0f;
    c.str(head, "Text", "Nothing here yet");
    c.whole(head, "Align H", 1);
    c.role(head, "Color", UIThemeRole::Text);
    c.textLevel(head, UIThemeTextLevel::Heading);

    const int msg = c.add(UIWidgetType::Text, root, "Message");
    c.at(msg).slotFill = 1.0f;
    c.str(msg, "Text", "What you add will show up here.");
    c.flag(msg, "WordWrap", true);
    c.whole(msg, "Align H", 1);
    c.role(msg, "Color", UIThemeRole::MutedText);
    c.textLevel(msg, UIThemeTextLevel::Body);

    c.param("Heading", head, "Text", "The one line that says what is missing.");
    c.param("Message", msg, "Text",
            "What to do about it. An empty state that only says \"no items\" tells "
            "somebody they are stuck.");
    c.bake();
    return c;
}

// ── The dialog, and the one component with logic of its own ──────────────────
// Shown with Create Widget → Show Modal, so the page that opens it already holds
// a reference to it. That is what makes its buttons useful without any new
// mechanism: the dialog EMITS "Confirmed" or "Cancelled", and the page binds to
// those events on the reference it already has.
//
// It hides itself as well as emitting. A dialog that stayed on screen after OK
// would leave every page that uses it responsible for closing something it did
// not open.
HorizonCode::Graph dialogGraph(int okButton, int cancelButton)
{
    HorizonCode::Graph g;

    // Pins live in ONE flat index space (exec-in, exec-out, data-in, data-out),
    // computed from the signature rather than written as literals — the numbers
    // depend on how many pins each type happens to have, and a literal would be
    // silently wrong the day one of them gains an input.
    auto execOut = [](const HorizonCode::Node& n, int k)
    {
        const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
        return static_cast<int>(s.execIns.size()) + k;
    };

    // Laid out in two rows. Node positions are not decoration: everything
    // defaults to (0, 0), and a graph written without them opens as one pile in
    // which only the last node drawn is visible.
    auto chain = [&](int button, const char* event, float y)
    {
        HorizonCode::Node ev;
        ev.id = g.nextId++; ev.type = HorizonCode::NodeType::Event;
        ev.s = "OnClicked"; ev.elem = button;
        ev.x = 0.0f; ev.y = y;
        g.nodes.push_back(ev);

        HorizonCode::Node emit;
        emit.id = g.nextId++; emit.type = HorizonCode::NodeType::EmitEvent;
        emit.s = event;
        emit.x = 260.0f; emit.y = y;
        g.nodes.push_back(emit);

        HorizonCode::Node hide;
        hide.id = g.nextId++; hide.type = HorizonCode::NodeType::HideSelf;
        hide.x = 520.0f; hide.y = y;
        g.nodes.push_back(hide);

        // connect() validates, so a refused link is a bug in this function and
        // says so here instead of shipping a dialog whose buttons do nothing.
        const bool wired =
            g.connect(ev.id,   execOut(ev, 0),   emit.id, 0) &&
            g.connect(emit.id, execOut(emit, 0), hide.id, 0);
        if (!wired)
            std::fprintf(stderr, "  DialogFrame: could not wire the %s chain\n", event);
        return wired;
    };

    chain(okButton,     "Confirmed", 0.0f);
    chain(cancelButton, "Cancelled", 200.0f);
    return g;
}

Comp dialogFrame(HorizonCode::Graph& graphOut)
{
    Comp c(460.0f, 210.0f);
    const int cardId = c.add(UIWidgetType::Panel, 0, "Card");
    c.fill(cardId);
    c.role(cardId, "Color", UIThemeRole::Surface);
    c.radius(cardId, UIThemeSize::Large);
    c.at(cardId).borderWidth = 1.0f;
    c.role(cardId, "Border Color", UIThemeRole::Border);
    c.at(cardId).shadow = true;
    c.at(cardId).shadowBlur = 18.0f;
    c.at(cardId).shadowOffsetY = 6.0f;

    const int title = c.add(UIWidgetType::Text, cardId, "Title");
    c.strip(title, 18.0f, 28.0f, 20.0f, 20.0f);
    c.str(title, "Text", "Are you sure?");
    c.role(title, "Color", UIThemeRole::Text);
    c.textLevel(title, UIThemeTextLevel::Heading);

    const int msg = c.add(UIWidgetType::Text, cardId, "Message");
    c.fill(msg, 20.0f, 56.0f, 20.0f, 72.0f);
    c.str(msg, "Text", "This cannot be undone.");
    c.flag(msg, "WordWrap", true);
    c.role(msg, "Color", UIThemeRole::MutedText);
    c.textLevel(msg, UIThemeTextLevel::Body);

    const int buttons = c.add(UIWidgetType::HorizontalBox, cardId, "Buttons");
    { UIElement& e = c.at(buttons);
      uiSetAnchorPreset(e, 11);            // whole bottom side
      uiSetAnchorInsetsX(e, 20.0f, 20.0f);
      e.pivotY = 1.0f; e.posY = -18.0f; e.sizeY = 36.0f; }
    c.num(buttons, "Padding", 0.0f);
    c.num(buttons, "Spacing", 10.0f);

    // A spacer that eats the leftover pushes both buttons to the right, which is
    // where a dialog's buttons live. Its own element rather than an anchored
    // box, so adding a third button costs nothing.
    const int gap = c.add(UIWidgetType::Spacer, buttons, "Gap");
    c.at(gap).slotFill = 1.0f;

    const int cancel = c.add(UIWidgetType::Button, buttons, "Cancel");
    c.at(cancel).sizeX = 110.0f;
    c.radius(cancel, UIThemeSize::Small);
    c.role(cancel, "Normal Color", UIThemeRole::Background);
    c.role(cancel, "Hovered Color", UIThemeRole::Border);
    c.role(cancel, "Pressed Color", UIThemeRole::Border);
    const int cancelCap = c.caption(cancel, "Caption", "Cancel");

    const int ok = c.add(UIWidgetType::Button, buttons, "OK");
    c.at(ok).sizeX = 110.0f;
    c.radius(ok, UIThemeSize::Small);
    c.role(ok, "Normal Color", UIThemeRole::Accent);
    c.role(ok, "Hovered Color", UIThemeRole::Accent);
    c.role(ok, "Pressed Color", UIThemeRole::Accent);
    const int okCap = c.caption(ok, "Caption", "OK");

    c.param("Title", title, "Text", "The question, in one line.");
    c.param("Message", msg, "Text", "The consequence, under it. Wraps on its own.");
    c.param("Confirm Label", okCap, "Text",
            "What the accepting button says. \"Delete\" beats \"OK\" — a button that "
            "names its action can be read without the title.");
    c.param("Cancel Label", cancelCap, "Text", "What the refusing button says.");
    c.param("Show Cancel", cancel, "Visible",
            "Off turns this into a message with one button, which is what an "
            "error dialog is.");

    graphOut = dialogGraph(ok, cancel);
    c.bake();
    return c;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: widget_gen <output-dir>\n");
        return 2;
    }
    const std::string outDir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    ContentManager cm(outDir);

    HorizonCode::Graph dialogLogic;
    struct Entry { const char* name; Comp comp; const HorizonCode::Graph* graph; };
    std::vector<Entry> entries;
    entries.push_back({ "FormRowText",   formRowText(),   nullptr });
    entries.push_back({ "FormRowToggle", formRowToggle(), nullptr });
    entries.push_back({ "FormRowChoice", formRowChoice(), nullptr });
    entries.push_back({ "SectionHeader", sectionHeader(), nullptr });
    entries.push_back({ "Card",          card(),          nullptr });
    entries.push_back({ "ListRow",       listRow(),       nullptr });
    entries.push_back({ "TitleBar",      titleBar(),      nullptr });
    entries.push_back({ "Toolbar",       toolbar(),       nullptr });
    entries.push_back({ "StatusBar",     statusBar(),     nullptr });
    entries.push_back({ "SearchField",   searchField(),   nullptr });
    entries.push_back({ "EmptyState",    emptyState(),    nullptr });
    entries.push_back({ "DialogFrame",   dialogFrame(dialogLogic), &dialogLogic });

    int ok = 0, index = 0;
    for (const Entry& e : entries)
    {
        UIWidgetAsset a;
        a.type      = HE::AssetType::Widget;
        a.name      = e.name;
        a.path      = std::string(e.name) + ".hasset";
        a.id        = HE::UUID{ kWidgetBaseHi + static_cast<uint64_t>(index),
                                0x0000000000000001ULL };
        a.treeJson  = uiWidgetTreeToJson(e.comp.t);
        a.graphJson = e.graph ? HorizonCode::toJson(*e.graph) : std::string();

        // A component with no parameters is a picture. Said here rather than
        // trusted, because the whole point of the library is that these can be
        // told what to say, and a forgotten param() call is invisible otherwise.
        if (e.comp.t.params.empty())
            std::fprintf(stderr, "  %-14s WARNING: declares no parameters\n", e.name);

        if (cm.saveAsset(a))
        {
            std::printf("  %-14s %2zu elements  %2zu parameters%s\n",
                        e.name, e.comp.t.elements.size(), e.comp.t.params.size(),
                        e.graph ? "  + logic" : "");
            ++ok;
        }
        else
            std::fprintf(stderr, "  FAILED to write %s.hasset\n", e.name);
        ++index;
    }

    std::printf("widget_gen: wrote %d/%zu components to %s\n",
                ok, entries.size(), outDir.c_str());
    return ok == static_cast<int>(entries.size()) ? 0 : 1;
}
