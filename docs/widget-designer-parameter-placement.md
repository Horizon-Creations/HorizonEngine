# Widget-Designer: wo die Widget-Parameter wohnen (Thema 139, Schritt 1)

Stand 04.10.2026, Basis `6193e26d`. Nur Ist-Zustand und Entscheidung, noch kein Umbau.

## Ist-Zustand

- Der Designer (`src/HE_Editor/UIEditorPanel.cpp`, `render`, ab Zeile 6819) ist **kein Dock-Layout**,
  sondern drei feste `BeginChild`-Spalten:
  - links `##uiw_left`, 230 px, halbiert in `##uiw_palette` und `##uiw_tree` (Hierarchie),
  - Mitte `##uiw_mid` mit `##uiw_canvas` und der Timeline darunter,
  - rechts `##uiw_details`, 300 px (Zeile 7028): `drawDetails`, bei Auswahl danach `drawDetailsEvents`.
- `drawDetails` (Zeile 2000) hat zwei Zweige. **Nichts ausgewählt:** Abschnitte Canvas, Theme,
  **Parameters** (Zeile 2099, ruft `drawParameterDeclarations`), Preview. **Element ausgewählt:**
  nur dessen Eigenschaften. Die Parameter-Deklarationen sind also nur erreichbar, wenn man die
  Auswahl aufhebt, belegt ist dafür die Zeile „Canvas" oben in der Hierarchie (Zeile 6973). Diesen
  Weg findet niemand, und die Auswahl ist danach weg.
- `drawParameterDeclarations` (Zeile 1644) zeichnet Name, Element, Property und Help je Parameter
  und den Knopf „Add Parameter", der den neuen Parameter auf `st.selected` richtet. Der Kommentar
  darüber sagt schon, dass die Parameter zum Widget als Ganzes gehören. Hilfe-Scope ist `Canvas`
  (`EditorHelp.cpp` 5332 bis 5367: Parameter Name, Element, Property, Help, Add Parameter).
- Ein **zweites** „Parameters" gibt es schon: Ist ein eingebettetes Widget (WidgetRef) ausgewählt,
  heißt dessen Inhaltsabschnitt „Parameters" (`drawContentSection`, Zeile 1837, `isRef`) und zeigt
  über `drawParamValues` (Zeile 1512, Hilfe `UI Widget/Parameters`) die **Werte**, die diese Kopie
  setzt. Das ist die andere Seite derselben Sache und bleibt, wo sie ist.
- Die Graph-Ansicht (`viewMode == 1`, `drawGraphVariables`) ist nicht betroffen.

## Entscheidung: Reiterleiste oben in der rechten Spalte

Die rechte Spalte bekommt eine Reiterleiste mit zwei Reitern: **Details** (wie heute) und
**Widget Parameters** (der Inhalt von `drawParameterDeclarations`). Die Reiterköpfe stehen immer
sichtbar oben in der Spalte, bei jeder Auswahl. Das ist die „feste, immer sichtbare Stelle" aus der
Themenbeschreibung: sichtbar ist der Einstieg, ein Klick genügt, und die Auswahl bleibt dabei stehen.

Verworfen:

- **Sprungknopf im Details-Kopf, der die Auswahl aufhebt:** verletzt die Fertig-Bedingung „ohne die
  Auswahl zu verlieren" direkt.
- **Eigenes Dock-Fenster:** der Designer hat keinen DockSpace, das hieße das Layout umbauen.
- **Dritter Bereich in der linken Spalte:** die ist mit 230 px schon zwischen Palette und Hierarchie
  halbiert, die Parameter-Zeilen haben zwei Combos und ein Textfeld je Eintrag.
- **Immer angehängter Abschnitt unter den Element-Details:** wandert bei Elementen mit vielen
  Eigenschaften weit nach unten, also wieder ein Umweg.

## Hinweise für Schritt 2

1. **Der Reiter folgt der Auswahl nicht.** Wer auf „Widget Parameters" steht und ein Element auf der
   Canvas oder in der Hierarchie anklickt, bleibt auf „Widget Parameters". Zurückspringen auf Details
   wäre genau der Fehler, um den es geht. Dafür wird „Add Parameter" nützlicher: Element anklicken,
   Parameter anlegen, er zeigt schon auf das Element.
2. **Nur noch ein Ort.** Der Abschnitt „Parameters" im Nichts-ausgewählt-Zweig (Zeile 2099) fällt weg.
   Danach den Test „nothing selected: the canvas settings" in `tests/test_widget_designer_ui.cpp`
   (ab Zeile 272, `inkedPixels(...) > 10000`) gegenprüfen, weniger Inhalt heißt weniger Tinte.
3. **ID-Stapel.** `BeginTabItem` legt die Tab-ID auf den ID-Stapel (`imgui_widgets.cpp`,
   `PushOverrideID(tab->ID)`). Steht der Details-Inhalt innerhalb des Tab-Items, ändern sich die IDs
   aller CollapsingHeader, und `openSection()` im Test (Zeile ~208, Seed = Fenster-ID von
   `##uiw_details`) findet sie nicht mehr. Deshalb: Tab-Items leer lassen, den gewählten Reiter in
   einem `State`-Feld merken (pro Asset, wie `viewMode`) und den Inhalt **nach** `EndTabBar` zeichnen.
   Dann bleiben alle bestehenden Test-Seeds gültig. Für das Setzen per Code (z. B. aus einem Test)
   `ImGuiTabItemFlags_SetSelected` verwenden.
4. **Name.** Der Reiter heißt „Widget Parameters", nicht „Parameters", damit er nicht neben dem
   gleichnamigen Abschnitt eines ausgewählten WidgetRef steht und dasselbe zu meinen scheint.
   Jeder neue Literal-String an `helpForLabel` braucht einen Eintrag in `EditorHelp.cpp` (der
   Abdeckungs-Scan liest Literale). Heute hat auch „Canvas/Parameters" keinen eigenen Eintrag,
   nur den Abschnitts-Fallback „Canvas/".
5. **Hilfe-Scope** der Zeilen bleibt `Canvas`, damit die fünf vorhandenen Einträge weiter greifen.
6. **Undo** läuft wie bisher über `commitEdit` in `drawParameterDeclarations`. Die Funktion selbst
   muss dafür nicht angefasst werden, nur ihr Aufrufort zieht um.
7. Nur die Designer-Ansicht (`viewMode == 0`). `drawGraphVariables`, Expose on Spawn (Thema 119) und
   das Datenmodell der Parameter bleiben unberührt.

## Schritt 3: Tests, Tooltips, Handbuch (04.10.2026)

**Tooltips.** Die beiden Reiterköpfe schlagen unter einem eigenen Scope `UI Details` nach
(`UIEditorPanel.cpp`, `render`, um die Reiterleiste; der Scope endet vor dem Inhalt, damit die
Abschnitte darunter nicht auf ihn zurückfallen). Einträge `UI Details/Details` und
`UI Details/Widget Parameters` in `EditorHelp.cpp`, Kapitelzeile
`{ "UI Details/", "editor-ui", "UI Designer", "The details column" }` in `kAreas`. Die
Parameter-Zeilen bleiben bei `Canvas` (Entscheidung 5 oben); `Canvas/Add Parameter` sagt jetzt,
dass die Auswahl beim Reiter bleibt. `helpForLabel` steht direkt nach `BeginTabItem` und vor dem
`if`, weil der Reiterkopf nur dort sicher das letzte Element ist, offen oder nicht.
`editor_help_audit.py` liest `BeginTabItem` nicht; den Wächter macht der Laufzeit-Lookup in
`tests/test_editor_help.cpp` (fünf neue Paare: die zwei Reiter, `Open Widget Parameters`,
`Add Parameter`, `Parameter Name`). Audit unverändert 1073/1073.

**Tests** (`tests/test_widget_designer_ui.cpp`, Testfall „the Details panel as it is"):

- *the tab heads carry their help*: Zeiger eine Sekunde ruhig auf jedem Reiterkopf, das
  Tooltip-Fenster `##Tooltip_00` ist aktiv. Gegenprobe ohne die beiden `helpForLabel`: rot.
- *an element selected: rename, reselect, remove, undo — all on the tab*: Logo auswählen,
  Reiter öffnen, Parameter anlegen (zeigt aufs Logo), umbenennen über das Namensfeld + Enter,
  Cmd/Ctrl+Z nimmt nur das Umbenennen zurück; dann „Title" in der Hierarchie anklicken: der
  Reiter bleibt offen, ein neuer Parameter zeigt auf Title; den ersten per × löschen, Title ist
  weiter ausgewählt; zwei Undos holen die gelöschte Zeile zurück. Gegenprobe (Reiter springt bei
  Auswahlwechsel auf Details): rot an der Stelle nach dem Klick auf Title.

Zusammen mit den zwei Subcases aus Schritt 2 ist die Fertig-Bedingung damit belegt: anlegen,
umbenennen und löschen bei ausgewähltem Element, ohne die Auswahl zu verlieren, jeweils mit Undo.
Bilder: `HE_UI_DUMP_DIR=… he_tests -tc="ui shot: widget designer — the Details panel*"` schreibt
`widget-designer-widget-parameters-edited` und `widget-designer-widget-parameters-tooltip`.

**Befund, nicht behoben (Datenmodell, laut Thema ausgeschlossen).** `uiWidgetTreeFromJson`
(`UIWidgetTree.cpp`, Commit `00f1a56f`) verwirft eine Parameter-Deklaration ohne Property
(„names nothing"). Der Undo-Snapshot des Designers (`makeSnapshot`/`restoreSnapshot`) läuft durch
genau dieses JSON. Ein frisch angelegter Parameter hat noch keine Property, also geht er bei jedem
Undo verloren, das einen Snapshot mit ihm zurückholt: *anlegen, umbenennen, Cmd/Ctrl+Z* lässt
null Parameter übrig statt einem. Das ist älter als dieses Thema. Der Schritt-2-Test
(anlegen, Undo, leer) war davon nicht betroffen, weil er ohnehin auf dem Snapshot ohne Parameter
landet. Der neue Test geht den Weg eines Autors und wählt direkt nach dem Anlegen eine Property
(über `UIEditorPanel::markEdited`, den Commit-Pfad des MCP). Abhilfe wäre, im Editor-Snapshot
auch unvollständige Deklarationen mitzunehmen oder „Add Parameter" gleich eine Property
vorzubelegen; beides eigenes Thema.

**Handbuch.** Die Editor-Referenz im In-Engine-Handbuch wird aus `EditorHelp.cpp` gebaut
(`EditorReference.cpp`), die neuen Einträge stehen dort im Kapitel „UI Designer" unter
„The details column". Die Website-Handbuchseite (`Website/HorizonEngineDocs/ui.html`, Quelle des
Bündels `EditorDeps/Docs/he-docs.json`) erwähnt Widget-Parameter gar nicht; dort gab es nichts
anzugleichen, und eine neue Seite wäre ein Commit im Website-Repository plus Deploy mit
Bestätigung, also nicht Teil dieses Schritts.

## Schritt 4: Verifikation (04.10.2026)

Stand `d7472b78`, macOS, Debug, `HE_ENABLE_SHADERC=OFF`.

- **Vollbau** `cmake --build . -j8`: RC 0, keine Warnung, kein Fehler im Log. Alle Objektdateien
  von `UIEditorPanel.cpp`, `EditorHelp.cpp` und den beiden Testdateien sind neuer als ihre Quellen
  (in `HorizonEditor` wie in `he_tests`), der Build ist also nicht stale.
- **ctest**, 9 Tests, alle grün: `test_widget_designer_ui` (181 s), `test_editor_help`,
  `editor_help_audit`, `test_ui_widgets`, `test_widget_pre_construct`, `test_widget_design_time`,
  `test_widget_expose_on_spawn`, `test_mcp_tools_widget`, `test_editor_row_widgets`.
- **doctest direkt:** `test_widget_designer_ui.cpp` 11 Testfälle, 946/946 Zusicherungen; davon
  der Fall „ui shot: widget designer — the Details panel as it is (Thema 92)" mit den vier neuen
  Subcases 90/90. `test_editor_help.cpp` 17 Testfälle, 77137/77137.
- **Tooltips** (`scripts/editor_help_audit.py`, gegen einen main-Checkout verglichen):
  main 1072/1072 Bedienelemente gedeckt, 1632 Hilfe-Einträge; Zweig 1073/1073, 1636 Einträge.
  Bereich `ui` 132 → 133, offen bleibt 0. Die Abdeckung fällt also nicht.
- **Handbuch:** `test_editor_help` verlangt für jeden der 1636 Einträge einen Bereich (`kAreas`)
  und einen eigenen, auflösbaren Abschnitt in der Editor-Referenz; grün heißt, kein Eintrag steht
  ohne Handbuch-Abschnitt.

Offen bleibt nur der Undo-Befund aus Schritt 3 (frischer Parameter ohne Property überlebt kein
Undo), der laut Thema außerhalb liegt. Den vollen ctest-Lauf hat dieser Schritt nicht gemacht;
mit `HE_ENABLE_SHADERC=OFF` wäre `test_app_todo` dort ohnehin rot, ohne Bezug zu diesem Thema.
