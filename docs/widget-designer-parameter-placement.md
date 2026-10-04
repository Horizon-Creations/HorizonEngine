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
