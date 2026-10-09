# RenderExtractor und Schatten-Pass: ein Extract pro Frame statt mehrfach voll

Stand 09.10.2026, Branch `claude/render-extractor-und-schatten-pass-einmal-pro-frame-statt-me`,
HEAD `21370cd8`. Schritt 1 des Themas 162: Bestandsaufnahme aller Aufrufstellen von
`RenderExtractor::extract`, Messbasis aus `docs/world-streaming-baseline-2026-10-06.md` §11.3
übernommen, Bauplan für Schritt 2–5. **Kein Feature-/Perf-Code in diesem Schritt.**

Ziel (Thema 153 §11.3/§5, Fortsetzung — der dortige Schlusssatz "Der nächste Hebel wäre der
Lauf des Extractors selbst" ist genau dieses Thema): ein persistenter `RenderWorld`-Zustand,
ein Extract pro Frame, Pässe lesen daraus, Transforms nur für geänderte Teilbäume neu propagieren.

Bezug: `docs/world-streaming-baseline-2026-10-06.md` (Messbasis), `src/HE_Rendering/include/HorizonRendering/RenderExtractor.h`,
`src/HE_Rendering/src/RenderExtractor.cpp`, `src/HE_Scene/src/TransformHierarchy.cpp`.

---

## 1. Messbasis (übernommen aus §11.3, Tabelle `s6end-table.md`)

| Entities | CPU/Frame p50 `s5end`→`s6end` | OnRender `s5end`→`s6end` | `RenderExtractor::extract` `s5end`→`s6end` |
|---|---|---|---|
| 1 084 | 12,3 → 10,1 ms | 1,7 → 0,7 ms | 1,0 → 0,7 ms |
| 10 174 | 31,2 → 14,1 ms | 8,2 → 2,1 ms | 8,9 → 6,4 ms |
| 50 574 | 98,6 → 62,4 ms | 42,2 → 7,7 ms | 59,7 → 33,4 ms |
| 101 074 | 195,5 → 145,4 ms | 85,2 → 17,6 ms | 111,6 → 76,6 ms |
| 202 074 | 501,2 → 286,8 ms | 221,4 → 34,5 ms | 265,5 → 146,1 ms |

Wörtlich aus §11.3: *"Wohin die 62 ms bei 50k jetzt gehen: `Render` 56 ms. Davon entfallen 33 ms
auf den einen vollen Extract im Schattenpass samt Wiederverwendung, 35 ms auf `EncodeShadowMap`
(den Extract eingerechnet) und je 10 ms auf Szene und SSAO."* Das ist die Zielgröße: der eine
verbleibende volle Lauf von `RenderExtractor::extract` (inkl. `propagateTransforms` ohne
Dirty-Flag) ist der größte Posten im Frame — größer als alle Encode-Pässe zusammen.

Messmethodik für Schritt 5 (gleiches Werkzeug, gleiche Leiter): `scripts/perf/world_streaming_ladder.sh`,
`scripts/he_perf_capture.py`, `scripts/perf/ladder_table.py`, Stufen 1k/10k/50k/100k/200k,
Rohdaten nach `docs/perf-audit/raw-streaming/`.

## 2. Aufrufstellen von `RenderExtractor::extract` (alle fünf Backends + Editor)

Die ursprüngliche Vermutung des Themas ("läuft mehrfach pro Frame: Schatten, SSAO, Szene,
GBuffer, GI-Accel") trifft **nicht gleich stark auf alle Backends zu**. Metal und Vulkan haben
das fragmentierte Design; D3D11, D3D12 und OpenGL haben es nie gehabt.

### 2.1 Metal — 6 Aufrufstellen, **bereits** mit Cache (Thema 153)

| Pass | Funktion | Extract-Zeile |
|---|---|---|
| Schatten | `EncodeShadowMap` | `MetalRenderer.mm:7634` (Aufruf `:7649`) |
| GI-Accel | `EncodeGIAccelBuild` | `:8051` (Aufruf `:8096`) |
| SSAO | `EncodeSSAO` | `:11959` (Aufruf `:11979`) |
| Szene | `EncodeScene` | `:13585` (Aufruf `:13599`) |
| GBuffer | `EncodeGBuffer` | `:15673` (Aufruf `:15686`) |
| Wolken-Prepass (opt-in, low-res clouds) | inline in `EncodeFrame` | `:16647` |

Aufrufreihenfolge innerhalb von `EncodeFrame` (`:16047`): Shadow (`:16287`) → GIAccelBuild
(`:16322`) → SSAO (`:16472`) → GBuffer (`:16529`) → [Wolken-Prepass, opt-in] → Scene (`:16663`).
Die ganze Funktion steht in einem `RenderExtractor::FrameScope` (`:16055`,
`beginFrame()`/`endFrame()` aus `RenderExtractor.h:150-163`): der **erste** Aufruf (Shadow) läuft
voll, jeder weitere mit gleichem `FrameKey` (Welt, Editor-Kamera, Day-Night, Shadow-Settings,
ContentManager-Epoche) antwortet aus der Kopie — nur wenn sich die `aspectRatio` unterscheidet
(SSAO bei halber Auflösung) wird der aspekt-abhängige Schwanz (Projektion, Kaskaden-Fit,
lokale Schatten-Layer) neu gerechnet, der Registry-Walk nicht (`RenderExtractor.cpp:1292-1309`).

**Das ist Schritt 2 des Plans für Metal schon zur Hälfte gelöst** — aber nur *innerhalb* eines
Frames, nicht über Frames hinweg, und über einen Cache/Vergleich statt einer Architektur, in der
Pässe grundsätzlich keinen eigenen Extract-Aufruf hätten.

### 2.2 Vulkan — 5 Aufrufstellen, **kein** Reuse

| Pass | Funktion | Extract-Zeile |
|---|---|---|
| Decal-Depth | `EncodeDecalDepth` | `VulkanRenderer.cpp:3631` (Aufruf `:3646`) |
| Schatten | `EncodeShadowMap` | `:5879` (Aufruf `:5937`) |
| Szene | `DrawScene` | `:6770` (Aufruf `:6782`) |
| GI-Accel | `runGi` | `:10146` (Aufruf `:10160`) |
| SSAO | `runSSAO` | `:11310` (Aufruf `:11326`) |

Kein `FrameScope`, kein `beginFrame`/`endFrame` irgendwo in `VulkanRenderer.cpp` — jeder dieser
fünf Aufrufe ist ein **voller** Walk, jeden Frame. Das ist der Zustand, den der Thema-153-Fix nur
für Metal behoben hat; Vulkan ist unverändert im alten Zustand (eher noch 5 statt 4 Aufrufe).
Bereits heute von `tests/test_culling.cpp:3297-3341` dokumentiert ("Vulkan's runGi() extracts on
its own") — ein Struktur-Pin, kein Perf-Test, aber ein Beleg dafür, dass dieser Zustand bekannt
und beabsichtigt (nicht versehentlich) ist.

### 2.3 D3D11 / D3D12 / OpenGL — je **1** Aufrufstelle, geteilter Zustand

| Backend | Funktion | Extract-Zeile |
|---|---|---|
| D3D11 | `DrawScene` | `D3D11Renderer.cpp:5886` (Aufruf `:5917`) |
| D3D12 | `DrawScene` | `D3D12Renderer.cpp:9519` (Aufruf `:9555`) |
| OpenGL | `DrawScene` | `OpenGLRenderer.cpp:10508` (Aufruf `:10593`) |

Diese drei Backends rufen `extract()` nur **einmal** pro Frame auf. Schatten (`ShadowPass` im
Render-Graph, `RenderPass.cpp:188`), SSAO (`runSSAO`/`createSSAOPipeline` als Methoden auf
derselben `p.m_renderWorld`) und GI-Accel-Refresh laufen als Hilfsfunktionen/Render-Graph-Pässe,
die alle dasselbe, einmal extrahierte `p.m_renderWorld` lesen — kein zweiter Extract-Aufruf. Das
ist bereits exakt die Zielarchitektur von Schritt 2 ("alle Pässe lesen aus diesem Zustand statt
eigenem extract-Aufruf"). Kein `FrameScope` nötig, weil es nur einen Aufruf gibt.

OpenGL hat zusätzlich eine **unabhängige** `RenderExtractor`-Instanz `previewExtractor`
(`OpenGLRenderer.cpp:9377`) für Asset-Thumbnail-/Material-Vorschauen — läuft nicht pro
Viewport-Frame, sondern pro Vorschau-Anfrage; aus der Perf-Betrachtung ausgeschlossen.

### 2.4 Editor — `ViewportPanel`, eigene Instanz `s_extractor`

`s_extractor` (`ViewportPanel.cpp:653`) ist von der Renderer-eigenen Extractor-Instanz und ihrem
`FrameScope` komplett unabhängig:

- `s_extractor.extract(...)` (`:678`, in `sceneSnapshot(ctx)`) — **voller** Lauf, aber nur bei
  Bedarf (Klick, Rahmen, Drop, Snap-Drag, zweites Scene-Fenster mit Auswahl), nicht jeden Frame im
  Leerlauf. Bereits von Thema 153 §9.4/§11.2 so umgestellt (§11.5: *"Voller Extract bei Bedarf:
  Grenze, bleibt so. Braucht etwas die Objekte, wird weiter voll extrahiert"*).
- `s_extractor.extractCameraOnly(...)` (`:1237`) — **jeden** Frame, für Kamera/Gizmo; überspringt
  `extractTransforms` nur, wenn eine aktive Editor-Kamera-Override vorliegt
  (`RenderExtractor.cpp:1355`).

Ein Snap-Drag oder ein offenes zweites Scene-Fenster mit Auswahl erzwingt also weiterhin jeden
Frame einen vollen `sceneSnapshot`-Extract, zusätzlich zum einen vollen Extract des aktiven
Renderer-Backends — zwei unabhängige volle Walks pro Frame in diesem Fall. Ob Schritt 2/3 das
mit abdecken (gemeinsamer, über Editor und Renderer geteilter Zustand) oder bewusst außen vor
lassen (wie §11.5 es für den aktuellen Zustand tut), ist eine offene Frage für Schritt 2.

## 3. Was aus Thema 153 schon behoben ist

- **Lokale-Matrix-Cache** (`TransformComponent.h:12-25`, `TransformHierarchy.cpp:14-37`):
  `sin`/`cos` aus den Euler-Winkeln wird nur neu gerechnet, wenn Position/Rotation/Skala vom
  zuletzt gecachten Wert abweichen (Wertvergleich, nicht über `dirty` — siehe Kommentar
  `TransformComponent.h:18-20`: Inspector/Gizmo setzen `dirty` nicht zuverlässig).
- **`RenderExtractor::beginFrame`/`endFrame` + `FrameScope`** (`RenderExtractor.h:131-163`,
  `.cpp:1290-1309`): innerhalb *eines* Frames antwortet jeder Aufruf nach dem ersten (mit
  gleichem `FrameKey`) aus der Kopie. Bisher nur von Metal genutzt (§2.1).
- **`extractCameraOnly`** (`RenderExtractor.h:120-129`, `.cpp:1350-1357`) für den
  Editor-Pick-Snapshot statt eines vollen Extracts jeden Frame (§2.4).
- Ergebnis laut §11.3: CPU/Frame bei 50k sinkt von 98,6 auf 62,4 ms — aber
  `RenderExtractor::extract` bleibt mit 33,4 ms der größte Einzelposten, weil **ein** voller Lauf
  pro Frame übrig bleibt, egal was reduziert wurde.

## 4. Was offen bleibt — der eine verbleibende Extract selbst

`propagateFrom` (`TransformHierarchy.cpp:39-51`) steigt bei **jedem** Aufruf unbedingt durch die
komplette `HierarchyComponent`-Kette ab, für jedes Kind eine Matrixmultiplikation — unabhängig
davon, ob sich irgendetwas am Teilbaum geändert hat. `TransformComponent::dirty` existiert
(`TransformComponent.h:8`), wird von `propagateFrom` nur **geschrieben** (`:46`,
`t->dirty = false`), nirgends **gelesen**, um Arbeit zu überspringen. Der Lokale-Matrix-Cache
(§3) erspart nur die `sin`/`cos`-Rechnung pro Entity, nicht den Abstieg und die
Welt-Matrix-Multiplikation selbst — das ist exakt der Rest, der bei 50k noch 33 ms kostet.

Zusätzlich, aus §2.2: Vulkan hat die Thema-153-Optimierung (Reuse innerhalb eines Frames) nie
bekommen — dort bleibt der Zustand *schlechter* als der, den §11.3 für Metal vermessen hat
(5 volle Walks statt 1).

## 5. Bauplan für Schritt 2–5 (Vorschlag — noch nicht umgesetzt, Entscheidung für Schritt 2 offen)

### Schritt 2 — Persistenter `RenderWorld`-Zustand

Zwei Wege, keiner in diesem Schritt entschieden:

- **A. Metal-Cache ausweiten:** `FrameKey`/Reuse-Mechanismus aus Frame- auf
  Cross-Frame-Gültigkeit heben (ein Änderungs-Zähler auf der Registry oder ein globales
  Dirty-Bit, das jede Transform-/Hierarchie-/Mesh-Ref-Mutation setzt), dazu Vulkan denselben
  `FrameScope` spendieren, den Metal schon hat. Kleinerer Eingriff, behält die Pass-pro-Aufruf-
  Struktur beider Backends.
- **B. Auf das D3D11/D3D12/OpenGL-Muster vereinheitlichen:** alle fünf Backends auf "ein
  Extract-Aufruf pro Frame, Pässe lesen `m_renderWorld`" umstellen. Größerer Eingriff (Metal
  müsste `EncodeShadowMap`/`EncodeGIAccelBuild`/`EncodeSSAO`/`EncodeGBuffer`/`EncodeScene` von
  eigenen Extract-Aufrufen auf gemeinsames Lesen umstellen, ebenso Vulkans fünf Stellen aus §2.2),
  dafür eine Architektur statt eines Caches — und D3D11/D3D12/OpenGL bleiben unangetastet, weil
  sie das schon sind.

Beide Wege müssen den aspekt-abhängigen Sonderfall (SSAO bei halber Auflösung,
`RenderExtractor.cpp:1297-1306`) erhalten.

### Schritt 3 — Dirty-Flag für `propagateTransforms`

Offene Design-Frage, nicht Teil dieses Schritts: `propagateFrom` müsste einen Teilbaum
überspringen (Rekursion **und** Matrixmultiplikation), wenn weder sein eigenes Lokal noch das
übergebene `parentWorld` sich seit dem letzten Lauf geändert haben — und dabei korrekt bleiben,
wenn ein Vorfahre sich bewegt (dann ändert sich `parentWorld` für den ganzen Teilbaum, auch ohne
dass ein einzelnes Kind editiert wurde) oder ein Kind neu eingehängt wird (`HierarchyComponent`
ändert sich). Ein reiner Wertvergleich von `parentWorld` gegen einen gecachten Wert (ähnlich dem
Lokale-Matrix-Cache) deckt den Vorfahren-Fall ab, ohne dass jede Mutation einen Teilbaum-weiten
Schreibzugriff bräuchte.

### Schritt 4 — Schattenpass auf das neue Modell umstellen

`EncodeShadowMap` ist der Pass, der aktuell den einen vollen Walk zahlt (läuft als erster Pass in
Metals `EncodeFrame`-Reihenfolge, §2.1). Sonderfälle zu prüfen: Day/Night (`setDayNight`,
`RenderExtractor.h:194-213`) ändert die Sonnenrichtung jeden Frame, wenn `dayNightCycle` an ist —
das invalidiert Licht-/Schattendaten unabhängig vom Entity-Dirty-Zustand und darf nicht durch
Schritt 3 verdeckt werden. Kaskaden-Fit bleibt aspekt-abhängig (siehe Schritt 2).

### Schritt 5 — Verifikation

Leiter 1k/10k/50k/100k/200k wie §11.3 (gleiches Werkzeug, §1), Vollbau, `ctest`. Bekannte
Struktur-Pins, die bei einer Umstellung mitgehen müssen:

- `tests/test_culling.cpp:3270-3294` (Day-Night-Aufrufreihenfolge/-zahl, Pass-Reihenfolge
  `EncodeShadowMap`/`EncodeDecalDepth`/`DrawScene`),
- `tests/test_culling.cpp:3297-3341` (Vulkan-`runGi`-Kommentar "extracts on its own" — bei
  Vereinheitlichung nach Weg B nicht mehr zutreffend, Test + Kommentar müssten umgeschrieben
  werden).
