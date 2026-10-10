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

---

## 6. Schritt 2: Frame-Zustand im RenderWorld des Aufrufers, ein Walk pro Frame (10.10.2026)

Stand: Zweig auf `origin/release/0.7.0` (67a1064a) gemergt, dann Schritt 2. **Weg A** (Auftrag der
Königin), klein am Extractor gehalten: der Reuse-Mechanismus aus Thema 153 bleibt, wo er ist; er
bekommt den Struktur-Zähler aus Thema 164 in den Schlüssel, er hört auf zu kopieren, und Vulkan
bekommt den Scope, den Metal seit Thema 153 hat. Die Pässe behalten ihren eigenen `extract()`-Aufruf.

### 6.1 Warum die Pässe ihren Aufruf behalten (und Weg B trotzdem nicht fehlt)

Der Aufruf ist nach dem ersten im Frame ein Schlüsselvergleich, kein Walk und keine Kopie mehr
(gemessen unten). Ihn aus den Pässen zu entfernen und oben einmal zu extrahieren hätte jeden
Ausstieg nachbauen müssen, bei dem ein Pass ihn nicht braucht: Metals SSAO aus dem G-Buffer extrahiert
gar nicht (`EncodeSSAO`, `fromGBuffer`), der Wolken-Prepass ist opt-in, Vulkans `EncodeDecalDepth`
kehrt vor seinem Extract zurück, wenn der Schatten-Extract keine Decals hinterließ. Jeder Pass
braucht ein gültiges `m_renderWorld` **mit seinem Aspekt**, egal welche Pässe vor ihm liefen; der
Aufruf ist genau diese Absicherung. D3D11/D3D12/OpenGL sind schon bei "ein Aufruf je Frame" und
bleiben unangetastet.

### 6.2 Was gebaut ist

| Stelle | Änderung | Wirkung |
|---|---|---|
| `RenderExtractor::FrameKey` (`.h`, `makeFrameKey`/`sameFrameKey` in `.cpp`) | `+ structureEpoch`, gelesen aus `HorizonWorld::structureEpoch()` (Thema 164, Plan 164 §7.2; nicht neu gebaut) | Ein Zellwechsel, Spawn, Destroy oder Reparent zwischen zwei Extrakten eines Frames führt zu einem neuen Walk statt zur Antwort aus der Alt-Welt. Der Vertrag aus 164 §7.1 ist damit geprüft statt nur beschrieben. |
| Frame-Zustand (`extract()`, `beginFrame`/`endFrame`) | `m_frameCopy` (eine zweite `RenderWorld`) entfällt. Der Walk bleibt in dem `RenderWorld`, in das er geschrieben wurde (`m_frameOut`); jeder weitere Aufruf mit gleichem Schlüssel **und gleichem `out`** kehrt sofort zurück. Ein anderes `RenderWorld` ist ein anderer Verbraucher und bekommt einen eigenen Walk. | Vorher: eine tiefe Kopie nach dem ersten Walk (Snapshot), eine je Wiederverwendung, ein `clear()` im `endFrame`. Jetzt keine. |
| `FrameShape` (`m_frameShape`) | Objekt-Array-Zeiger und Größen von `objects`, `skinnedObjects`, `instanceBlocks`, `lights`, `decals`, `particleBatches`, `ribbonBatches`, `landscapes`, nach dem Walk gemerkt und bei jeder Wiederverwendung verglichen | Wer das Ergebnis leert, verkleinert oder neu zuweist, bekommt einen neuen Walk statt einer leeren Welt. |
| `fitDirectionalShadow` (`RenderExtractor.cpp`) | nimmt die Szenenbox (Vereinigung der Caster-/Empfänger-Grenzen) herein und füllt sie beim ersten Aufruf des Walks (`m_frameShadowBox`) | Der aspektabhängige Schwanz (Projektion, Kaskaden-Fit, lokale Schattenlayer) läuft bei anderem Aspekt **an Ort und Stelle** und landet bitgleich beim vollen Walk, obwohl die Backends die Grenzen inzwischen verfeinert haben. Ein Refit aus den verfeinerten Grenzen hätte andere Kaskaden ergeben als die, mit denen der Schattenpass zeichnete. |
| `extractCameraOnly` | verwirft den Frame-Zustand, wenn es in das `RenderWorld` des Frames schreibt | Eine Kamera, die nicht zum gemerkten Aspekt passt, wird nie als Walk ausgegeben. Nur Editor, dort gibt es keinen Scope. |
| `VulkanRenderer::Render()` und `RenderSceneImage()` | `RenderExtractor::FrameScope` vor dem ersten Pass | Bis zu **fünf volle Walks je Frame** (Kaskaden, Decal-Tiefe, GI, SSAO, Szene) werden zu einem. |
| `tests/test_world_scale.cpp` | 5 neue Fälle (Struktur-Epoche je Änderungsart, In-Place-Zustand, fremdes `RenderWorld`, Größen-/Leer-Prüfung), 2 bestehende auf die Renderer-Praxis umgestellt (alle Pässe in dasselbe `RenderWorld`, Vergleich mit einem frischen Walk), Bench `Extraction bench: the frame copy against the walk it saves` (skip) | siehe 6.5 |
| `tests/test_culling.cpp` | Quelltext-Pin "Vulkan and Metal record a frame inside one extractor FrameScope": Vulkan genau zwei Scopes (`Render()`, `RenderSceneImage()`), beide vor dem ersten Pass; Metal genau einer vor `EncodeShadowMap` | Der lokale Zeuge für Vulkan, das auf diesem Mac nicht kompiliert wird. Ein dritter, verschachtelter Scope in `DrawViewportFrame` würde den äußeren früh schließen (`endFrame`). |

### 6.3 Was der Schlüssel erzwingt, was nicht

| Änderung zwischen zwei Extrakten eines Frames | Wirkung |
|---|---|
| `createEntity`, `destroyEntity`, `reparentEntity`, `moveChild`/`placeNextTo`/`sortChildrenByName`, ein Zell-Laden oder -Entladen, `noteStructureChanged` | neuer Walk (Test "every kind of structure change invalidates the frame copy") |
| anderer Editor-Kamera-Zustand, Tageszeit, Schatten-Einstellungen, ContentManager oder dessen Epoche, andere Welt | neuer Walk (schon vorher) |
| anderer Aspekt | Schwanz an Ort und Stelle, kein Walk |
| Komponentenwerte (bewegter Transform, anderes Material), Mesh-Komponente an lebender Entity hinzugefügt | **nicht gesehen**: der Vertrag "die Welt wird im Scope nicht angefasst" gilt weiter. Genau das nennt 164 §7.2 als Grenze des Zählers. |

### 6.4 Was die Pässe dem Zustand antun dürfen (Prüfung der Mutationsstellen)

Ohne die Kopie sieht der nächste Pass, was der vorige im `RenderWorld` hinterließ. Alle nicht-const
Zugriffe der Backends auf das Ergebnis wurden durchgesehen (`RenderWorld&` ohne const in
`HE_Rendering`, Schleifen `for (RenderObject& ...)`, direkte Feldzuweisungen):

- `RenderObject::refineWorldBounds` (Metal 5 Stellen, Vulkan 5, D3D11/D3D12/GL je 1 bis 2): ersetzt die
  Grenze durch `meshLocalBounds.transformed(transform)`, hängt nur an Mesh-Grenze und Transform und
  schreibt jedes Mal denselben Wert. Jeder Pass verfeinert vor dem Gebrauch ohnehin selbst.
- `HE::resolveWorldMaterialScalars` (Vulkan 3 Stellen, D3D11/D3D12 je 1): schreibt die Skalare des
  Materials, lässt den Wert unverändert, wenn es nicht auflöst. Gleiche Eingabe, gleicher Wert.
- `extractUI` löscht `uiObjects` selbst, bevor es füllt; Metals `swap(uiObjects)` stellt wieder her.
- Nichts sortiert, löscht oder hängt an `objects`, `lights`, `decals`. `RenderSorter::sort`,
  `FrustumCuller::cull` und `GIProbeSceneSignature` nehmen `const RenderWorld&` bzw. nur
  Entity- und Mesh-Id (nicht die Grenzen).

Wer künftig in einem Pass sortiert oder entfernt, wird über `FrameShape` aufgefangen, wenn sich Größe
oder Array-Zeiger ändern, und zahlt dann einen Walk. Ein In-Place-Umsortieren bei gleicher Größe fängt
nichts ab: so etwas gehört in einen eigenen Index (wie `m_sortedIndices`), nicht in `objects`.

### 6.5 Messung und Belege

**Kosten der Kopie, die entfällt** (Bench, Release, M5, Last 2,4; 100 Entities je Gruppe, Würfel-Meshes,
ohne ContentManager, deshalb ist der Walk billiger als im Spiel; Median über 7 Läufe):

| Objekte | ein Walk | tiefe Kopie des Ergebnisses | Frame aus Walk + 1 Wiederverwendung (alt) |
|---|---|---|---|
| 1 000 | 0,25 ms | 0,04 ms | 0,33 ms |
| 10 000 | 2,48 ms | 0,39 ms | 3,30 ms |
| 50 000 | 15,8 ms | 2,18 ms | 20,4 ms |
| 100 000 | 35,1 ms | 4,74 ms | 48,1 ms |

Eine Kopie kostet also rund 14 % eines Walks. Metal zahlte davon im Forward-Frame drei (Snapshot nach dem
Walk, SSAO, Szene) plus das `clear()`, Vulkan nach dem Scope fünf. Bei 50k sind das rechnerisch
**6,6 ms (Metal) bzw. 11 ms (Vulkan) je Frame**, bei 100k das Doppelte. Das ist aus dem Bench
abgeleitet, nicht im echten Loop gemessen: die Baseline-Binary wurde für den Vollbau überschrieben, und
der Vorher/Nachher-Lauf der Leiter ist Schritt 5.

**Der echte Editor-Loop, neue Build** (`scripts/he_perf_capture.py`, Metal, 50 568 Entities aus
`gen_reference_world.py --count 50000`, `--warmup 240 --frames 120 --no-counters --cam 0,25,90,0,-0.25`,
Last 2 bis 3, Bildschirm entsperrt):

- Jeder der 120 Frames hat **genau 3 `RenderExtractor::extract`-Aufrufe und 2 `RenderExtractor::reuse`**
  (1 Walk, SSAO, Szene). Der Struktur-Zähler bricht die Wiederverwendung im Betrieb also nicht.
- Der volle Walk: p50 17,0 ms, p90 29,3 ms je Frame. Die Wiederverwendung: p50 0,000 ms, max 0,0004 ms
  (statt einer Kopie von 2 bis 3 ms).
- CPU/Frame p50 38,2 ms, `Render` p50 30,9 ms, `EncodeShadowMap` p50 20,8 ms (davon 17,0 der Walk),
  `EncodeSSAO` p50 5,2 ms, `FrustumCuller::cull` 5 Aufrufe je Frame, zusammen p50 2,1 ms.

**Tests** (alle lokal, Release, shaderc ON):

- Gegen den unveränderten Code liefen die zwei Struktur-Tests und der Vulkan-Pin **rot** (der Walk
  "Late" fehlte: `c.objects.size() == 41` statt 42), danach grün. Die später ergänzten Fälle
  (In-Place, fremdes `RenderWorld`, Größenprüfung) wurden nur gegen den neuen Code gefahren.
- `RenderExtractor: *`, die Pins `Vulkan extracts with this frame's sun ...`, `GI instances get their
  material colour ...` und der neue Pin: 22 Fälle, 985 Assertions, grün.
- ctest voll (`-j4 --timeout 1500`, Release, shaderc ON, `HE_CONFIG_DIR` ungesetzt, frisches `HOME`):
  257 Tests, **255 grün, 0 rot, 2 übersprungen** (`runtime_size_app_advanced`/`_basic`: für diese
  Plattform "reported and skipped" per Konstruktion, `scripts/runtime_size.py`), 300 s. Vorher ein
  Vollbau des gemergten Stands ohne eigene Änderung (rc 0, 1637 Schritte, 32 min) und danach der
  Vollbau mit der Änderung (rc 0).

**Metal-Pixel-A/B, erste Runde** (`scripts/he_shot.py`, 15 Shots; `HE_SKY_TIME=30`, `AA=0`, privates
`HE_CONFIG_DIR`): Kontrolle zuerst, dieselbe Baseline-Binary zweimal. **12 von 15 Shots sind byte-identisch
zwischen zwei Läufen, und alle 12 sind byte-identisch zwischen Baseline und neuer Build.** Die drei
übrigen (GI Forward, GI Deferred, Low-Res-Wolken) rauschen schon in der Baseline selbst; neu gegen
Baseline liegt im gleichen Rauschen wie neu gegen neu (GI Forward: mittlere Abweichung 0,0025/255, max 2,
bei Baseline gegen Baseline 0,0006/255, max 1, bei neu gegen neu 0,0004 bis 0,0029/255, max 2; GI Deferred:
0,0000/255, max 1; Low-Res-Wolken 0,05 bis 0,11/255, max 8, bei neu gegen neu 0,02 bis 0,06/255, max 8).
**Grenzen dieser Runde**: unter den 12 sind nur 9 verschiedene Bilder (`SSAO=1` ändert gegenüber dem
Standard nichts, Forward und Deferred gleichen sich im Lokalschatten-Shot), `LOCALSHADOW=1` ist kein
gültiger Modus (`point|spot`, das Bild ist der Nachthimmel), und die Decal- und Foliage-Shots zeigen den
Boden ohne ihr Motiv. Belegt sind damit Forward und Deferred mit Kaskadenschatten, Würfel-/Instancing-
/Occlusion-Szenen, Nachtlicht, Himmel und GI im Rauschen; **nicht** belegt sind Decals, Lokalschatten und
Foliage im Bild. Die Zahlen dafür liefert der Extractor-Test (Lokalschatten-Schicht und -Matrizen werden
bitgleich gegen einen frischen Walk verglichen). Eine zweite Runde mit richtig gerahmten Witnesses folgt
weiter unten, falls sie in diesem Schritt noch gefahren wurde.

### 6.6 Was nur die CI belegt

- **Vulkan**: `VulkanRenderer.cpp` wird auf diesem Mac nicht kompiliert (kein SDK). Lokal belegt sind
  nur `clang -fsyntax-only` gegen die MoltenVK-Header (rc 0; Negativkontrolle mit vertipptem
  `FrameScope`-Typ scheitert wie erwartet) und der Quelltext-Pin. Dass es unter MSVC baut, belegt der
  Windows-Job; dass die fünf Pässe aus **einem** Walk dasselbe Bild liefern, höchstens der Job
  `vulkan-lavapipe` (Bild-A/B mit Validation). Mit Hardware ist nichts davon gelaufen.
- **D3D11/D3D12**: keine Quelländerung, aber `RenderExtractor.h` (private Elemente) wird dort mitkompiliert:
  Windows-Job.
- **Linux/OpenGL**: Linux-Job und `he_tests` (der Extractor wird in `he_tests` direkt mitgebaut).

### 6.7 Abgeschnitten, und was für Schritt 3 bis 5 daraus folgt

- **Cross-Frame-Zustand: nicht gebaut.** Der Bauplan nannte "Frame- auf Cross-Frame-Gültigkeit heben".
  Dafür fehlt die Voraussetzung: Komponentenwerte werden von Skripten, Inspector, Gizmo, Physik und
  Animation direkt über Referenzen geschrieben, es gibt keinen Schreibpfad, an dem ein Zähler hängen
  könnte (`TransformComponent::dirty` wird laut Kommentar nicht zuverlässig gesetzt), und `structureEpoch`
  deckt Werte ausdrücklich nicht ab (164 §7.2). Dazu ändert sich in jedem Frame mit Spielinhalt etwas
  (Partikel, Trails, Skinning, Tageszeit im Schlüssel), ein Frame-Überspringen würde selten greifen.
  Der Hebel ist der Walk selbst (Schritt 3), nicht seine Gültigkeit.
- **Editor-`s_extractor`: nicht angefasst.** Er schreibt in einen eigenen Snapshot mit eigener Kamera und
  eigenem Aspekt, nur bei Bedarf (Klick, Rahmen, Drop, Snap-Drag, zweites Scene-Fenster). Ihn mit dem
  Renderer zu teilen hieße, das `RenderWorld` des Backends über `IRenderer` herauszureichen: ein
  API-Eingriff in alle fünf Backends, den dieser Schritt nicht rechtfertigt. Die Frage aus 2.4 ist damit
  beantwortet: außen vor.
- **Schritt 3 (Dirty-Flag)**: Ein Vergleich von `parentWorld` mit einem gemerkten Wert spart die
  Multiplikation je Entity, **nicht den Abstieg**: `propagateFrom` liest jede `HierarchyComponent` und
  jede `TransformComponent` in jedem Frame, und das ist Speicherverkehr, der bei 50k den Walk dominiert.
  Ein echtes Überspringen eines Teilbaums braucht ein Signal je Teilbaum (von den Schreibern gesetzt oder
  aus einer Prüfsumme je Gruppe), und das ist eine eigene Entscheidung. Der Anteil von
  `propagateTransforms` am Walk ist im Bench (Abschnitt 6.5) noch nicht ausgewiesen.
- **Schritt 4 (Schattenpass)**: Jeder Metal-Pass verfeinert in einer eigenen Schleife alle Objekte
  (`ResolveMesh` je Objekt, fünf Schleifen je Frame). Die Verfeinerung ist idempotent und das Ergebnis
  bleibt jetzt im Zustand: ein Verfeinern je Walk ist möglich, und `FrustumCuller::cull` läuft fünfmal
  je Frame (2,1 ms bei 50k). Beides ist Kandidat für Schritt 4 oder 5; dieser Schritt hat es nicht
  angefasst, weil es das Verhalten der Pässe ändert (eine Mesh-Auflösung mitten im Frame würde dann erst
  im nächsten Walk sichtbar).
- **Schritt 5**: Die Leiter fährt gegen diesen Stand; der Zähler-Zeuge ist `RenderExtractor::extract`
  gegen `RenderExtractor::reuse` je Frame (`scripts/perf/dump_scope_p50.py`), erwartet (3, 2) auf Metal
  im Forward-Frame. Die Bench `Extraction bench: the frame copy against the walk it saves` lässt sich
  mit `--no-skip` wiederholen.
