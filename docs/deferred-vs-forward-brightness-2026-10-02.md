# Deferred vs Forward: Helligkeitsunterschied im MANYLIGHTS-Zeugen (Thema 125, Schritt 1)

Stand 02.10.2026, Metal, Apple Silicon. Herkunft: Nebenbefund aus Thema 117 Schritt 2
(`docs/clustered-lighting-forward-plan-2026-10-01.md` §5, PR #75) und A8 in
`docs/pr-followup-review-2026-10-02.md`.

## Ergebnis in einem Satz

Nicht Deferred ist zu hell, sondern **Forward verliert das Richtungslicht**: der
Extraktor reihte Sonne/Mond hinter die Punktlichter, und alle Forward-Shader
schattieren nur die ersten 8 Lichter von `RenderWorld::lights`. Mit 16 Punktlichtern
fiel der Mond aus dem Fenster. Der Deferred-Resolve (Clustered) schreibt sein Fenster
auf „nur Richtungslichter" um und behielt ihn, deshalb war er der richtige Pfad.

## Messungen (he_shot, Release-Deploy von PR #75, `HE_SKY_TIME=10`, privates `HE_CONFIG_DIR`)

Grundaufruf: `MANYLIGHTS=16 TOD=0 COVERAGE=0 CLOUDMODE=0 AA=0 CAMY=207 CAMZ=2 PITCH=-38 RENDERPATH=0|1`.
Pixel als sRGB, Bodenoberseite (640,520) / Seitenfläche der Platte (640,587).

| Lauf | Forward | Deferred |
|---|---|---|
| Graph-Boden, 16 Lichter, Mitternacht | (78,85,110) / (79,86,111) | (182,192,211) / (144,154,181) |
| eingebauter Boden (`16builtin`) | (60,66,87) / gleich | (158,168,192) / (117,128,156) |
| Graph-Boden, TOD=0.5 | (114,135,170) | (246,246,247) |
| `SSAO=0 GI=0` | unverändert | unverändert |
| `SKYTEST=1` | unverändert | unverändert |
| **Forward, `MANYLIGHTS=7`** | **(182,192,211) / (144,155,181)** | |
| Forward, `MANYLIGHTS=8` / `=9` | (78,85,110) flach | |
| Kontrolle DOFTEST (Bodenhöhe, 0 Punktlichter, TOD=0) | (158,169,193) | (158,169,193) |

Was die Zahlen sagen:

- Forward: Oberseite = Seitenfläche → nur noch das flache Ambient, kein N·L-Term.
  Die Punktlichtpools sind dabei kräftig, also funktionieren die Punktlichter.
- Deferred: Oberseite heller als Seite → ein Richtungslicht (Mond bzw. Sonne) wirkt.
- G-Buffer-Ansichten (`GBUFFER=1..4`) sind sauber: Grundfarbe 0.8, Normale nach oben,
  Emissive 0. Codegen und G-Buffer sind nicht die Ursache.
- Der Kipp-Punkt liegt exakt zwischen 7 und 8 Punktlichtern, mit 7 ist Forward
  bitgleich zum Deferred-Wert. Damit ist das 8er-Lichtfenster die Ursache.
- Auch der **eingebaute** Forward-Shader (`fragmentMain`) ist betroffen, nicht nur
  Graph-Materialien (heLitP). Es ist keine Clustered-Regression aus PR #75, sondern
  besteht auf main genauso.

Ausgeschlossene Spuren: Sky-Env-Cubemap (beide Pfade befüllen sie mit derselben
Sonnenrichtung), fehlende Sampler, SSAO, GI, Nebel/Höhe, Schattenparameter.

## Ursache im Code

`src/HE_Rendering/src/RenderExtractor.cpp`:

1. `extractLights` reiht Lichter in ECS-Reihenfolge ein.
2. `applyDayNight` hängt eine synthetische Sonne und einen Mond **hinten** an, wenn die
   Welt keine eingebauten hat (so im Dump-Zeugen).
3. Alle Forward-Konsumenten nehmen die ersten `kMaxLightWindow = 8` Einträge:
   `FillMaterialLightWindow` (heLitP), die SceneUniforms-Packung der eingebauten
   Shader, `BuildMaskedLocalLights`, `assignLocalShadowLayers`.

Deferred-Clustered (`MetalRenderer::EncodeClusterData`) legt Punkt/Spot in die
Cluster-Listen und lässt im Fenster nur Richtungslichter, darum blieb dort der Mond.

## Fix (dieser Schritt)

`orderLightWindow` in `RenderExtractor.cpp`, aufgerufen nach `applyDayNight` und vor
den Schattenphasen: stabile Dreiteilung

1. Richtungslichter mit Intensität > 0,
2. Punkt/Spot,
3. Richtungslichter mit Intensität 0 (Mond bei Tag, Sonne bei Nacht).

Die dritte Gruppe kostet so keinem Punktlicht seinen Platz. Weil die Sortierung im
Extraktor sitzt, gilt sie für alle fünf Backends und für eingebaute wie Graph-Shader
gleichzeitig; die Packing-Funktionen und Shader bleiben unverändert (sie lesen
weiterhin „die ersten 8 in Extraktor-Reihenfolge", der Kommentar in
`LightPacking.h` nennt jetzt die Reihenfolge).

Test: `tests/test_light_window_order.cpp` (16 Punktlichter + Tag/Nacht um Mitternacht
→ Mond auf Platz 0, ausgeschaltete Sonne hinten, 7 Punktlichter im Fenster; eine
nach den Punktlichtern erzeugte Sonne landet vorne, `FillMaterialLightWindow` trägt sie
in Slot 0).

## Folgen und offene Punkte

- Szenen mit mehr als 8 Lokallichtern **und** einem Richtungslicht sehen im Forward-Pfad
  jetzt ein Punktlicht weniger (vorher fehlte stattdessen die Sonne). Das 8er-Fenster
  selbst bleibt die Grenze der eingebauten Forward-Shader (Thema 123).
- Die Kanalzuordnung der GI-Lokalmasken folgt automatisch, weil Shader und
  `BuildMaskedLocalLights` dieselbe Reihenfolge zählen.

## Verifikation

Release-Build dieses Zweigs (main + Fix). Der MANYLIGHTS-Zeuge liegt nur auf PR #75; sein
Block in `EditorApplication.cpp` wurde für die Aufnahmen **vorübergehend und uncommittet**
eingespielt (`git diff 0513f8d4 origin/claude/clustered-lighting-forward-parity --
src/HE_Editor/EditorApplication.cpp`) und danach zurückgesetzt, damit dieser Zweig nicht
mit PR #75 kollidiert. Gleicher Aufruf wie oben:

| Lauf | Forward | Deferred |
|---|---|---|
| Graph-Boden, `MANYLIGHTS=16` | (182,192,211) / (144,155,181) | (182,192,211) / (144,154,181) |
| eingebauter Boden, `16builtin` | (157,168,192) / (117,128,156) | (158,168,192) / (117,128,156) |

Forward und Deferred liegen jetzt bis auf 1/255 beieinander, beide Shader-Arten.

Tests: `test_light_window_order.cpp` (neu), `test_world_preview_grid`, `test_editor_icons`,
`test_culling`, `test_viewport_pick`, `test_particles`, `test_rope_trail`,
`test_skeletalmeshcomponent` grün. Negativkontrolle: ohne den `orderLightWindow`-Aufruf
scheitert der Mitternacht-Fall an vier Fenster-Checks. Der zweite Fall (eine nach den
Punktlichtern angelegte Sonne) ist auch ohne Fix grün, weil entt die zuletzt angelegte
Entität zuerst liefert; er sichert nur die Reihenfolge ab und beweist den Fix nicht.

Nicht geprüft: OpenGL/D3D/Vulkan zur Laufzeit (der Fix liegt backendneutral im
Extraktor, die Konsumenten lesen dort dieselbe Reihenfolge).
