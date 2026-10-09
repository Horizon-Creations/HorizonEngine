# Spline-Werkzeug im Editor (Thema 174, Schritt 2)

Stand 09.10.2026, Mac, Debug-Build. Schritt 1 lieferte die Komponente
(`SplineComponent`) und die Kurve (`HE::spline::Curve`); dieser Schritt ist das
Werkzeug im Scene-Viewport. Wasser und Terrain-Aushub sind bewusst nicht
angefasst (Schritte 3 bis 8).

## Bedienung

Ein dritter Modus neben View und Landscape: die Zelle **Spline** in der
Viewport-Leiste. Im Modus ist das Move/Rotate/Scale-Gizmo der Entities aus, die
Auswahl per Klick ebenso; Klicks gehören dem Werkzeug.

| Geste | Wirkung |
|---|---|
| Klick auf den Boden, nichts ausgewählt | neue Entity "Spline" an der Stelle, ein Punkt |
| Klick auf den Boden | Punkt am Ende anhängen |
| Klick auf die Linie | Punkt zwischen den Nachbarn einfügen (auf der Kurve, nicht auf der Sehne) |
| Klick auf ein Handle | Punkt auswählen, Move-Gizmo erscheint daran |
| Gizmo ziehen | Punkt verschieben (Snap und Oberflächen-/Vertex-Snap der Leiste gelten) |
| Entf | gewählten Punkt löschen, der Nachbar ist danach gewählt |
| Esc | Punkt loslassen; ein zweites Esc ist das normale Auswahl-aufheben |
| Closed (Panel, Details) | schließt die Linie zur Fläche; gilt ab drei Punkten |
| New Spline (Panel) | Auswahl lösen, der nächste Klick beginnt eine neue Linie |
| Klick auf eine andere Spline | wechselt das Ziel (ändert nichts) |

"Boden" ist die Szenenoberfläche unter dem Strahl (derselbe dreiecksgenaue Probe
wie beim Snap to Ground, Terrain eingeschlossen). Trifft der Strahl nichts, gilt
die waagerechte Ebene durch den letzten Punkt (ohne Punkt: y = 0).

Ein Klick zählt erst beim Loslassen und nur ohne Ziehen: ein Alt+LMB-Orbit, ein
RMB-Flug oder ein Zug über die Schwelle setzt keinen Punkt.

Das Details-Panel hat zur selben Komponente einen Abschnitt (Closed, eine Zeile
pro Punkt, Remove, + Point). Add Component bietet "Spline", der Outliner-Filter
kennt den Typ.

## Undo

Jede Geste ist genau ein Eintrag im bestehenden `EditorUndo`:
"Add Spline", "Add Spline Point", "Insert Spline Point", "Delete Spline Point",
"Close Spline" / "Open Spline", "Move Spline Point". Ein Zug des Gizmos ist ein
Eintrag, egal wie viele Frames er dauert; ein Griff, der nichts bewegt, hinterlässt
keinen. Eine Geste, die nichts ändert (Index außerhalb, schon geschlossen),
schreibt keinen Eintrag.

Undo baut die Welt neu auf und leert die Auswahl. Das Werkzeug hält deshalb nie
einen `Entity`-Handle über Frames, nur Indizes und die UUID. In dem Frame, in dem
die Auswahl leer wird und sich die Historie bewegt hat, wählt es seine Spline per
UUID wieder aus (`SplineEdit::Tool::sync`); nahm der Schritt die Spline ganz weg
(Undo des ersten Klicks), wartet es auf das Redo. Wer die Auswahl selbst aufhebt
(Esc, Outliner), bei dem lässt das Werkzeug los.

Entf und Esc laufen in `EditorUI.cpp` vor dem Scene-Fenster. Das Werkzeug bekommt
sie dort zuerst angeboten (`SplineTool::deleteKey/escapeKey`): mit gewähltem Punkt
löscht Entf den Punkt, nicht die Entity. Esc nimmt den bestehenden Vorframe-Stempel
(`escFree`) mit: ImGui verbraucht Esc schon in `NewFrame`.

## Aufbau

| Datei | Inhalt |
|---|---|
| `src/HE_Editor/SplineEdit.h/.cpp` | ohne ImGui: Picking (Handle, Kurve, Bildschirm-Abstand), Ebenen-Strahl, die Edits, `Tool` (Zustand aus Indizes + UUID) |
| `src/HE_Editor/SplineTool.h/.cpp` | ImGui: Maus-Scharfstellung, Gizmo, Hinweiszeile, Quick-Settings-Panel, Tasten |
| `src/HE_Editor/ViewportOverlays.*` | `appendSplineGuides`: die Linie und die Handles als Debug-Linien |
| `src/HE_Editor/EditorTransformGizmo.*` | `manipulatePoint`: dieselbe Gizmo-Einrichtung (Latch, Snap) für einen bloßen Punkt |
| `ViewportPanel.cpp`, `ViewportToolbar.cpp`, `EditorUI.cpp`, `EditorConfig.h` | Modus `Spline`, Gizmo/Pick nur in View |
| `InspectorPanel.cpp`, `EditorHelp.cpp`, `OutlinerFilter.cpp` | Details-Abschnitt, Hilfe, Typfilter |
| `EditorApplication.cpp` | Debug-Linien im Frame, Zeuge `HE_DUMP_SPLINETEST` |

Die Linie wird in lokalen Koordinaten abgetastet und erst die STÜTZPUNKTE der
Abtastung werden in den Weltraum gebracht (zentripetales Catmull-Rom ist nicht
invariant gegen ungleichmäßige Skalierung, siehe Schritt 1). Gezeichnet und
getroffen wird dieselbe Abtastung (`kSamplesPerSpan = 16`), damit "auf der Linie"
auf dem Bildschirm und für den Klick dasselbe heißt.

Handles sind Kästchen in Debug-Linien mit bildschirmkonstanter Größe
(`kSplineHandleScale` mal Abstand zur Kamera): erster Punkt violett, gewählter
orange, überfahrener rosa, der Rest blau; die Kurve grün, ungewählte Splines
gedimmt. Die Farben sind satt, weil Linien ein Pixel breit sind: blasse Töne
verschwanden im Zeugen auf einem sonnigen Boden (Weiß auf Weiß beim Hover).

## Verifikation

Tests (Debug, `env -u HE_CONFIG_DIR HOME=<scratch>`):

- `test_spline_edit` (23 Fälle, 228 Assertions): Picking (Handle vor Kurve, Schluss-
  Span einer geschlossenen Spline hängt an), Welt gegen Lokal unter verschobener
  und skalierter Entity, jede Geste genau ein Undo-Eintrag und rückwärts wie
  vorwärts durchgelaufen, abgelehnte Edits ohne Eintrag, der Klickablauf mit
  Ctrl+Z wie im Editor (Auswahl geleert, Werkzeug findet die Spline per UUID),
  Oberflächen-Probe gegen Ebene, Entf/Esc/Closed, ein Zug = ein Eintrag, ein
  Griff ohne Bewegung = keiner, Hover/Guides, Zeilenzahlen und Welt-Lage der
  Overlay-Linien, bildschirmkonstante Handle-Größe.
- `test_spline_tool_ui` (9 Fälle, 94 Assertions): das echte `SplineTool` in einem
  headless ImGui mit echtem `AppContext`: Klick, Zug, Alt+Klick, RMB, View-Modus,
  Entf/Esc, Einfügen/Auswählen, Gizmo-Zug als ein Undo-Eintrag, Panel (Closed,
  Delete Point, New Spline), Hinweiszeile.
- `test_inspector_ui`, neuer Fall: der Details-Abschnitt schließt, fügt Punkte
  hinzu und entfernt sie, je ein Undo-Schritt.
- Negativkontrollen: Insert ohne Snapshot und das Wiederfinden nach Undo
  herausgenommen (`test_spline_edit` rot), Zug-Erkennung herausgenommen
  (`test_spline_tool_ui` rot); danach wiederhergestellt.
- Voller `ctest` ohne `test_material_graph` (Debug, in der Datei unberührt, braucht
  allein über 260 s): 252 Tests grün, 3 übersprungen (`runtime_size*`).
- `scripts/editor_help_audit.py --check`: 1120 von 1120 Steuerelementen gedeckt.

Zeuge `HE_DUMP_SPLINETEST` (Wörter: `closed`, `undo`, `redo`, `hover`,
`hoverpoint`): sieben Klicks mit der echten Kamera durch `SplineEdit::Tool::click`
auf einen Boden, dann dieselben Overlay-Linien wie im Editor. Aufruf:

```
HE_CONFIG_DIR=<frisch> HE_COLLAB_OFFLINE=1 HE_SKY_TIME=1 HE_DUMP_PATH=out.bmp \
HE_DUMP_QUIT=1 HE_DUMP_SKYTEST=1 HE_DUMP_RHI=Metal HE_DUMP_TOD=0.5 \
HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0 HE_DUMP_CAMX=0 HE_DUMP_CAMY=5 \
HE_DUMP_CAMZ=6 HE_DUMP_PITCH=-30 HE_DUMP_SPLINETEST="closed hoverpoint" \
out/deploy/Editor/HorizonEditor
```

Logzeilen (Zahlen ohne Bild zu lesen):

| Wörter | Punkte | Linien | Undo/Redo-Tiefe |
|---|---|---|---|
| (offen) | 7 | 180 = 6 Spans x 16 + 7 Handles x 12 | 7 / 0 |
| `closed hoverpoint` | 7 | 196 = 7 x 16 + 84 | 8 / 0 |
| `closed hover` | 7 | 199 = 196 + Einfüge-Kreuz (3) | 8 / 0 |
| `undo` | 7 -> 5 | 124 = 4 x 16 + 5 x 12 | 5 / 2 |
| `undo redo` | 7 -> 6 | 152 = 5 x 16 + 6 x 12 | 6 / 1 |

Bilder in `docs/spline-tool/`:

- `metal-open.png`, `metal-closed-hover-handle.png`, `metal-closed-hover-line.png`,
  `metal-after-undo-2.png`, `metal-after-undo-2-redo-1.png` (Metal)
- `opengl-closed-hover-handle.png` (OpenGL, dieselbe Szene, dasselbe Bild)
- `panel-quick-settings.png`, `details-spline-section.png`,
  `viewport-hint-and-gizmo.png` (ImGui, Software-Raster)

## Bekannte Grenzen und Offenes

- Nur Mac verifiziert (Metal, OpenGL, headless). D3D11, D3D12 und Vulkan zeichnen
  Debug-Linien wie bisher; ein Ausfall träfe jede Debug-Linie, nicht die Spline
  (Abnahme in den Schritten 9 und 10).
- Die echte Maus im laufenden Editor ist nicht von Hand bedient worden (kein
  Editor-MCP in diesem Lauf). `ViewportPanel` reicht `IsItemClicked` an der Stelle
  weiter, an der es auch das Auswählen tut; das Werkzeug selbst läuft im
  UI-Test mit demselben Aufruf.
- Nicht gebaut: Menüeintrag "Window > Spline Tools" (Mac-Menüleiste), Outliner-
  Preset zum Anlegen, Punkt-Zeile im Details-Panel, die den gewählten Punkt
  markiert, Handbuch-Eintrag (Schritt 11), Punktauswahl per Rahmen.
- Beim Ziehen eines Griffs auf Terrain folgt der Punkt der Oberfläche nur mit
  eingeschaltetem Oberflächen-Snap der Leiste (sonst die Ebene des Gizmos).
