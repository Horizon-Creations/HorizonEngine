# HorizonCode: widget elements as objects, drag-off menu (2026-11-25)

Goal: as in Blueprint, an element of a widget can be held as a reference. Its properties
are read and written through that reference with Get/Set. A HorizonCode class's public
variables are reached the same way.

## Handoff

- **Commits (release/0.8.0):**
  - ca5d802c2: drag-off menu lists own functions for data wires, and Set Variable off
    inputs.
  - a9e13804f: element references.
  - Then: guide text plus bundle.
- **What was built:**
  - `NodeType::GetElement` (`HorizonCode.h`, at the end of the enum). Fields: `elem` =
    element id, `s` = name (the pin label), `className` = `ui:<Type>`. It is pure and
    outputs a Ref.
  - Runtime: `Runtime::elementRef(owner, elem)` and `isElementRef`
    (`HorizonCodeRuntime.h/.cpp`).
    - Handles are `0x80000000 | slot`, kept separate from instance ids. They are stable
      per (instance, element) and resolve to null once the owner is gone.
    - `ctx.getExternal`/`setExternal` resolve element handles to
      `host.getProperty/setProperty(owner, elem, prop)`, which is the same binding
      Get Property uses. `ctx.isValid` knows them too.
  - Codegen: `hc::elementRef(m_ctx, elem)` (`HcCodegen.cpp`, `HorizonCodeGenSupport.h`).
    GetExternal on a control class takes no fast path and goes through the runtime seam,
    so both modes share one implementation.
  - Control classes: `HE::uiClassKey`, `uiClassFromKey` and `uiClassProperties`
    (`UIElement.h/.cpp`). `HcRename::classOfRefSource` returns `className` for
    GetElement.
  - Drag-off menu (`HcGraphHost.cpp`, Ref-output section): for a control class it lists
    each property as Get/Set, i.e. GetExternal/SetExternal with `s` = property and
    `className` = key.
  - Widget editor (`UIEditorPanel.cpp`):
    - add menu gets an "Elements" section (`addElementRef`);
    - element drop: "Reference" first, and Get/Set create a reference + Get/Set (Ref)
      pair;
    - `Host::dragOffers` offers element references (no longer `Get Label1.Text`
      directly);
    - title "Get <Elem>" → the canvas draws a pill.
  - Type picker: a "Controls" section (`HcEditorUtil::drawTypePicker`).
  - GetElement is in addExcluded/dragExcluded of all three hosts (LevelScript, UIEditor,
    Animator).
- **Verified:**
  - Parity fixture `widget_props` extended: Get/Set via element ref, interpreted ==
    compiled.
  - Runtime test for the handle rules.
  - `test_hc_drag_menu`: all built-ins, all engine calls, own functions, property nodes.
  - Headless menu shots `drag-menu-{float-out,float-in,element-ref,button-var}`.
  - Full ctest 385/385, help audit.
- **Not verified:**
  - Clicking through it in the live editor.
  - Running a real widget in Play with an element ref.
  - The HorizonCode class variables via reference (existed before) with a real class
    asset, not headless.
- **Known / open:**
  - A Cast to a control class does not exist (the runtime's `isA` does not know element
    types).
  - A String output dragged in a widget graph no longer lists "Set Label1.Text"; the way
    there is the reference.
  - `m_elementSlots` never shrinks (`shortcut:` comment).
  - Old Get/Set Property nodes stay and work.
  - The website guide (horizoncode.html) is not committed in the website checkout and
    not deployed.
- **Next steps:**
  1. Click it in the editor: element drop ▸ Get ▸ Text, then Play.
  2. Optionally "Set Text (Text)" entries for data drags, with a Target to fill.
  3. Optionally Cast to control classes.
