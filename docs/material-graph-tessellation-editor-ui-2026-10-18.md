# Material-Graph: Displacement-Pin im Material-Editor (Schritt 3)

Thema 207, Schritt 3 von 4. Vorher: `docs/material-graph-tessellation-umsetzung-2026-10-17.md`
(Pin 9, Codegen, GL-Verdrahtung). Dieser Schritt macht den Pin im Editor bedienbar und
die Verformung in der Vorschau sichtbar, wo das Backend tesselliert.

## Was steht

- **Pin sichtbar/verbindbar:** war es schon. `matOutputPins` liefert alle Registry-Pins, Pin 9
  „Displacement" steht in jedem Blend-Mode auf dem Output-Knoten (kein Code nötig).
- **Fähigkeit:** `IRenderer::SupportsTessellation()` (Standard `false`), `OpenGLRenderer`
  gibt `m_tessSupported` zurück (GL 4.0+, `HE_TESSELLATION=0` schaltet ab). Schritt 5 setzt
  den Override für Vulkan/D3D11/D3D12, dann stimmt der Editor ohne weitere Änderung.
- **Leiste (Material-Bar, `MaterialEditorPanel.cpp`, hinter dem Blend-Popup):** Zelle
  „Tessellation", nur solange Pin 9 verbunden ist. Popup mit Max Factor, Fade Near, Fade Far,
  Max Displacement. Übernommen wird über `matSanitizeTess`, der Knoten hält also, was der
  Shader backt. Bearbeiten setzt `headerEdit`, also neu generieren + Undo wie beim Blend-Mode.
  Die Zelle wird **gelb** mit Hinweis, wenn (a) das Backend nicht tesselliert („Not tessellated
  on Metal: the pin has no effect here …") oder (b) Domain = User Interface. Speist ein Vektor
  den Float-Pin, steht im Popup eine graue Notiz (nur x / Rot wird gelesen; Registry-Typ,
  Reroutes werden verfolgt, FunctionCall zählt als Float). Das ist die Link-Warnung aus
  Entwurf §2.1, bewusst nur grau: die Registry typt jeden Math-Knoten als Vec3, also träfe
  eine echte Warnung auch Höhe × Skalierung, den Normalfall.
- **Vorschau:** auf GL tesselliert `RenderMaterialPreview` schon seit Schritt 2. Neu: auf einem
  Backend ohne Tessellation steht unten auf dem Vorschaubild „Displacement not shown on
  <Backend>". `usesTime` fragt auch `customShaderDispGlsl` (nur wo tesselliert wird), sonst
  stünde eine zeitgetriebene Höhe still.
- **Inline-Compile-Check:** kompiliert zusätzlich die Domain-Stufe (`tessEval(…, SpirV)`,
  reines glslang, läuft auch im Metal-Editor). Fehler kommen mit dem Präfix „Displacement:"
  ins Banner.
- **Hilfe:** `EditorHelp.cpp`, „Material Settings/Max Factor|Fade Near|Fade Far|Max Displacement".

## Prüfung

| Was | Ergebnis |
|---|---|
| Build `HorizonEditor` + `he_tests` (Release, Worktree) | grün |
| `editor_help_audit.py --check` | 1202/1202 |
| `he_tests --source-file='*test_material_tessellation.cpp'` | 12 Fälle grün (neu: Time-Höhe steckt im Disp-Body, nicht im Fragment) |
| `*test_mcp_tools_material.cpp` | grün |
| Vorschau-Dump GL (`MATERIALTEST=displace PREVIEW=1`, `HE_PREVIEW_DUMP`), Pin an vs. aus | 230 853 px Unterschied, Silhouette verformt (`docs/images/material-tessellation-preview-gl-h015.png` bei Höhe 0.15 gegen `…-preview-gl-unconnected.png`) |
| derselbe Dump auf Metal, Pin an vs. aus | **0 px**, bitgleich (Fallback-Vertrag) |
| voller `he_tests`-Lauf (Release) | 5347/5348; rot nur `test_hpak.cpp:1186` (`6 == 2`) = 4 Assets im lokalen EngineContentCache + 2, geräteabhängig, nicht dieser Zweig. Nach der letzten Hilfetext-Änderung `test_editor_help` + Tess-Tests erneut grün |

**Nicht gesehen:** die Leistenzelle, das Popup und der Vorschauhinweis selbst. `MaterialEditorPanel`
ist nicht in `he_tests`, und kein MCP-Werkzeug öffnet einen Material-Tab im laufenden Editor.
Geprüft sind nur Kompilat, Hilfe-Audit und die Bedingungen, an denen die Texte hängen.

## Handoff

- **Topic / step:** 207, Schritt 3 von 4 (Editor-UI)
- **Branch / commit:** `claude/material-graph-shader-tessellation-displacement-als-pin-am-o` @ d66518b8 + dieser Doku-Commit (gepusht)
- **Done:** Tessellation-Zelle + Popup in der Material-Leiste, Hinweise (Backend, UI-Domain, Vektor→Float), Vorschauhinweis, `usesTime` über den Disp-Body, Domain-Stufe im Inline-Check, `IRenderer::SupportsTessellation()`.
- **Files touched:** `IRenderer.h` (nach `RenderMaterialPreview`), `OpenGLRenderer.h:57`, `MaterialEditorPanel.cpp` (`displacementLink`/`displacementSourceType`/`backendLabel` vor `matNodePins`, Leiste hinter `##blendPopup`, Vorschau `usesTime` + Hinweis nach `ImGui::Image`, Inline-Check am Ende von `regenerateMaterial`), `EditorHelp.cpp` (hinter „Material Settings/Clip"), `tests/test_material_tessellation.cpp`.
- **Verified:** Tabelle oben.
- **Not verified:** UI-Bild der Zelle/des Popups (kein Weg, den Tab headless zu öffnen); Undo-Schritt eines Tess-Edits nur gelesen (läuft über `headerEdit → paramEdit` wie der Blend-Mode); `tessHere`/`usesTime` und die Domain-Stufe im Inline-Check nur per Build + Test der Bedingung, nicht im laufenden Editor; Windows/Linux nur per CI.
- **Decisions taken:** Einstellungen in der Leiste statt auf dem Knoten (Output-Knotenhöhe bleibt, Präzedenz Blend-Mode); Pin 9 bleibt in der UI-Domain sichtbar (WPO auch, `matOutputPins` kennt die Domain nicht), stattdessen gelber Hinweis — Abweichung von Entwurf §2.1; Vektor→Float nur als Hinweis, Verbinden wird nicht verhindert; keine WPO-Ersatzvorschau auf Metal (Entwurf §6 D verworfen), nur der Hinweis.
- **Pending:** Schritt 4 (Abnahme, Beispielmaterial), Schritt 5 (Vulkan/D3D-Verdrahtung, dort `SupportsTessellation()` überschreiben).
- **Traps:** Ein Knoten-Preview („Preview This Node") trennt nur Pins 0–4, Displacement und WPO bleiben also aktiv, die Kugel ist dann verformt + unbeleuchtet (gewollt wie bei WPO). Höhe 0.4 im `displace`-Zeugen lässt die Vorschaukugel das Bild füllen; für Bilder `HE_DUMP_DISPHEIGHT=0.15`.
- **Next steps:** 1. Schritt 4: Beispielmaterial mit Höhenkarte (Texture → Displacement) anlegen, GL-Dump + Editor-Vorschau. 2. Schritt 5: `SupportsTessellation()` in Vulkan/D3D11/D3D12 mit der Verdrahtung zusammen setzen.
