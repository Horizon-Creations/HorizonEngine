# Material-Graph: Displacement-Pin, Abnahme (Schritt 5)

Thema 207, Schritt 5. Vorgänger: `docs/material-graph-tessellation-backends-2026-10-18.md`
(Schritt 4, Vulkan/D3D11/D3D12-Verdrahtung), `…-editor-ui-2026-10-18.md` (Schritt 3),
`…-umsetzung-2026-10-17.md` (Schritt 2), Entwurf `…-displacement-entwurf-2026-10-15.md`.

## Was neu ist

- **Beispielmaterial `Engine/Materials/DisplacementDemo.hasset`** (`mat_gen`, Eintrag 1, UUID
  `0x601/1`, angehängt; bis 2026-10-28 stand die Datei noch auf `0x401/1`, der UUID von T_Landscape_Grass_Normal): gesteppte Kissen, `|sin(π·4·u)·sin(π·4·v)| · Height` am Displacement-Pin,
  dieselbe Kissenform als Farbe (Fugen dunkel) und Rauheit. Knöpfe `Height` (0,15 m, Regler bis
  0,5 = Max Displacement), `TopColor`, `GrooveColor`. Tess: maxFactor 16, Fade 8–40 m.
  **Nur Sinus, kein Rauschen:** Wertrauschen hasht auf D3D12/Vulkan anders als auf D3D11/GL
  (Befund Schritt 4), als Höhe wäre das ein Riss. Die Höhe ist auf jeder Zellgrenze 0, auch bei
  u, v = 0/1: Naht und Pole der UV-Kugel und Würfelkanten reißen nicht. Zellzahl fest (ganzzahlig).
- **Zeuge `HE_DUMP_MATTESTASSET=<Inhaltspfad>`** (`EditorApplication.cpp`, vor `registerMaterial`):
  die Testkugel trägt die Material-DATEI statt des Inline-Graphen; `HE_DUMP_DISPPIN=0` leert ihren
  Displacement-Body (= Pin unverbunden). `cap207.ps1 -Asset …` (Bilder `demo-<rhi>-<v>`, neue
  Varianten `p0`/`p30` = `Height` zur Laufzeit 0 / 0,3), `ana207.py <dir> <rhis> demo-`.
- **UI-Test `tests/test_material_editor_ui.cpp`:** `MaterialEditorPanel.cpp` ist jetzt in `he_tests`
  (per-file `HE_IMGUI_ENABLED`, keine Stubs nötig). Der echte Tab auf einer Kopie des
  ausgelieferten Demos, Stand-in-Renderer (`SupportsTessellation()` + Scheibe als Vorschau). Die
  Zelle wird per Zeiger gesucht (ID `##tess` im Fenster, Hover-Lauf) und **geklickt**; das Popup
  muss vom Klick aufgehen. Prüft Bernstein-Pixel (kWarn) in Zelle, Popup und Vorschau.
- `test_engine_materials.cpp`: Demo lädt, Pin 9 verdrahtet, Tess saniert, Body aus `sin(`, kein
  `heHash`/Noise, keine Textur, exakt drei Knöpfe, und die DATEI trägt Fragment + Disp-Body.

## Prüfung

NN-WS03, RTX 4070, Release-Baum `C:/hw207` (eigenes Deploy), 19.10.2026, frische APPDATA je Lauf.

| Was | Ergebnis |
|---|---|
| voller `he_tests` (Release) | 5360/5362; rot nur `inspector ui: …copies…` (2 Fälle, Zwischenablage) — auch allein rot, `Get-Clipboard` scheitert auf dem Gerät gleichzeitig: Umgebung, bekannt |
| `test_engine_materials` + `test_material_tessellation` + `test_material_graph` + `test_mcp_tools_material` | 191 Fälle, 18 395 Assertions grün (inkl. neuer Demo-Fall) |
| `test_material_editor_ui` | 3 Fälle grün: D3D12 ohne Bernstein, Metal mit (Zelle > 20, Popup > 50, Vorschauhinweis > 50 px), ohne Draht keine Zelle (Kontrolle: Ordner-Zelle wird gefunden) |
| `editor_help_audit.py --check` | 1202/1202 |
| Demo, Pin an gegen unverbunden | D3D11 118 718, D3D12 118 718, Vulkan 118 682, GL 118 736 px: Kissen und Silhouette verformt |
| `HE_TESSELLATION=0` gegen unverbunden (Fallback-Vertrag) | **0 px** auf D3D11, D3D12, Vulkan, GL |
| ref zweimal (Rauschboden) | 0 px auf allen vier |
| `Height` zur Laufzeit 0 gegen unverbunden | ≤ 574 px, max 1/255 auf allen vier: der Knopf erreicht die Domain-Stufe |
| `Height` 0,3 gegen Standard 0,15 | ~129 300 px auf allen vier |
| Backends untereinander (Demo, Pin an) | D3D12↔D3D11 8 px max 1; Vulkan max 4; GL max 7; **0 px > 8**: kein Band wie beim Rauschzeugen (stützt die `heHash21`-Vermutung, bestätigt sie nicht) |
| Logs | 0 `[ERROR]`; „built a tessellated …" auf D3D11/D3D12/Vulkan; D3D12-Debug-Layer nur ClearRenderTargetView (bekannt) |
| Software-Backend | zeichnet **nur UI** („user interface only"): Bild leer, 0 px ist kein Beweis. Belegt: Demo lädt, Lauf ohne Fehler/Absturz, `SupportsTessellation()` = false → Editor zeigt den gelben Hinweis |

Bilder: `docs/images/material-tessellation-demo-backends.png` (ref, D3D11, D3D12, Vulkan, GL);
`material-tessellation-ui-metal-bar.png` (gelbe Zelle + „Displacement not shown on Metal"),
`…-ui-metal-popup.png`, `…-ui-d3d12-popup.png` (graue Zeile „Tessellated on D3D12").

**Metal:** auf diesem Gerät nicht prüfbar. Beleg aus Schritt 2/3 (Mac, 18.10.): Vorschau-Dump
Pin an/aus 0 px. **Auf dem Endstand nicht erneut gefahren.**

## Handoff

- **Topic / step:** 207, Schritt 5 (Build, Tests, Abnahme, Beispielmaterial, UI-Bild, Bericht)
- **Branch / commit:** `claude/material-graph-shader-tessellation-displacement-als-pin-am-o` @ 19540a54 (+ Korrektur-Commit dieses Berichts); CI 39217259679
- **Done:** DisplacementDemo (mat_gen + .hasset), Demo-Test, `HE_DUMP_MATTESTASSET`, UI-Test mit
  Bildern, Hardware-A/B auf vier Backends + Software, voller Testlauf, Hilfe-Audit.
- **Verified:** Tabelle oben.
- **Not verified:** Metal auf dem Endstand (nur Schritt-2/3-Beleg); Software zeichnet kein 3D,
  also keine Pixel-Aussage; andere GPUs (AMD/Intel); Linux/macOS nur per CI; der Material-Editor
  im laufenden Editor (Test nutzt den echten Tab, aber Stand-in-Renderer und -Vorschau).
- **Decisions taken:** Demo als Engine-Material (Content Browser + Export), texturfrei wie Water
  (mat_gen-Regel) und ohne Rauschen; Zellzahl als Konstante, nicht als Knopf (Naht);
  `Water.hasset` NICHT neu committet — `mat_gen` auf HEAD schreibt es 2 Byte länger, Unterschiede
  ab Byte ~3568 im Fragmenttext. **Ursache nicht untersucht**; dieser Zweig hat die Codegen in
  Schritt 2 geändert (6a36dd61), kann also nicht ausgeschlossen werden. Die eingecheckte Datei
  bleibt, die Water-Tests sind mit ihr grün (der Ladepfad regeneriert ohnehin).
- **Traps:** `mat_gen` erzeugt immer alle Einträge → nach dem Lauf `git checkout` auf Water, falls
  nicht beabsichtigt. Bash-Heredocs schlucken `\` in PowerShell-Pfaden (Edit-Tool nehmen).
  `he_ui::registerTexture` liefert 64-Bit-IDs (nicht auf `uint32_t` kürzen → weißes Bild).
  Kommentar-Rahmen des Canvas (222,183,97) liegen nah an kWarn: Bernstein-Test eng halten.
- **Next steps:** 1. CI 39217259679 abwarten (Hive meldet; 39213586011 kann vom Concurrency-
  Group abgebrochen erscheinen, das ist kein Rot). 2. Optional Mac: `cap207`-Äquivalent mit
  `HE_DUMP_MATTESTASSET` auf Metal (erwartet 0 px) und GL; oder das Demo in den Bake-Test am Ende
  von `test_engine_materials.cpp` aufnehmen (MSL/GL/FXC-Kompilat, heute nur Water). 3. `Water.hasset`-
  Drift klären (s. o.). 4. Handbuch
  (`EditorDeps/Docs/he-docs.json`) um das Demo ergänzen; Merge-Fallen aus Schritt 4 gelten weiter
  (Thema 217 `bindMatRunState`, 214 Resolve-Cache, 215 `VkPipelineCache`).
