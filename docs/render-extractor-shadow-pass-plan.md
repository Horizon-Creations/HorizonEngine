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

### Schritt 3 — Dirty-Flag für `propagateTransforms` (umgesetzt, siehe Abschnitt 7)

Die Design-Frage, wie sie in Schritt 1 stand (Abschnitt 7 beantwortet sie mit einem Scan statt eines
`parentWorld`-Vergleichs): `propagateFrom` müsste einen Teilbaum
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

### Schritt 5 — Verifikation (durchgeführt, siehe Abschnitt 9)

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
| `scripts/perf/frame_pixel_ab.sh` | Metal-Pixel-A/B: 16 richtig gerahmte Witness-Szenen (Lokalschatten, Decals, Foliage, Kaskadenschatten) einschließlich Render-Scale 0,51, bei dem der aspektabhängige Schwanz wirklich läuft; druckt je Shot die md5 | siehe 6.5; für Schritt 3 bis 5 wiederverwendbar |

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
**Grenzen der ersten Runde**: unter den 12 sind nur 9 verschiedene Bilder (`SSAO=1` ändert gegenüber dem
Standard nichts, Forward und Deferred gleichen sich im Lokalschatten-Shot), `LOCALSHADOW=1` ist kein
gültiger Modus (`point|spot`, das Bild ist der Nachthimmel), die Decal- und Foliage-Shots zeigen den
Boden ohne ihr Motiv, und alle Shots rendern bei 1280×720: SSAO läuft bei 640×360, dasselbe Verhältnis,
der aspektabhängige Schwanz des Extractors läuft in dieser Runde (und im 50k-Capture, 2840×1528 zu
1420×764) nie. Deshalb die zweite.

**Metal-Pixel-A/B, zweite Runde: richtig gerahmt, echter Vorher/Nachher** (`scripts/perf/frame_pixel_ab.sh`,
16 Shots). Die Baseline-Binary war für den Vollbau überschrieben; sie wurde deshalb nachgebaut (nur die
drei Produktdateien per `git checkout 25e8915d -- ...` auf den Stand vor Schritt 2, inkrementell), ihr
Deploy-Ordner und der der neuen Build in je ein Verzeichnis kopiert und mit
`scripts/perf/selfcontain_deploy_copy.sh` selbstständig gemacht (eine bloße Kopie lädt weiter die dylibs
des Build-Baums). Beleg, dass die Kopien verschiedene Builds halten: das Symbol `frameShapeOf` steht nur
in der neuen `libHorizonRendering.dylib`. Danach wurden die drei Dateien zurückgesetzt und alles neu
gebaut (rc 0, `git status` sauber, Extractor-Tests erneut grün, das Skript im Repo reproduziert die md5s
der neuen Build).

| Witness | Bild |
|---|---|
| `ls_point_*`, `ls_spot_*` (Forward und Deferred) | Würfel wirft einen Schatten des Punkt- bzw. Spotlichts auf die Platte |
| `decal_def`, `decal_fwd` | roter Decal-Fleck auf der Platte (Deferred projiziert, Forward ignoriert ihn in v1) |
| `foliage_fwd`, `foliage_def` | Feld aus 2000 Foliage-Instanzen |
| `shadinst_*` | Reihe aus sieben instanzierten Würfeln mit Kaskadenschatten |
| `odd_*` (Render-Scale 0,51) | dasselbe bei Szene 653×367 und SSAO 326×183 (laut Engine-Log "scene render size 653x367"): **der Schwanz läuft zwischen Kaskaden- und Szenenpass**, auch mit Lokalschatten und mit Kaskadenschatten aus der Instanzreihe |

Ergebnis: **16 von 16 Shots byte-identisch zwischen der Baseline-Binary und der neuen Build.** Kontrolle:
jede der beiden Builds zweimal gefahren, beide Läufe je Build byte-gleich (16 von 16). Die md5s der
neuen Build (Referenz für Schritt 3 bis 5, nur auf diesem Gerät und dieser macOS-Version gültig):

```
ls_point_fwd 83fe304229de8752d0cf0658caf5237b      shadinst_fwd 28daf946b3530cb82ae14a4ec921cd80
ls_point_def c8f89f3ce3a707cc57abe74a7101c2dc      shadinst_def 5afc47ec1419ac39e5405ebf50d4b707
ls_spot_fwd  049fc80e11c94c3af30ac21f7e1b221e      odd_shadinst_fwd eb65ffc9233511981e05fab88118585d
ls_spot_def  1ebed478b610ab9ccf407d73449f339b      odd_shadinst_def 527517a083b2142f1f6d71eb5c99e56f
decal_def    f49e23b192c0bb28dd8791de79bd70f4      odd_dof_fwd e261707ae4a2e19a4fefddf81630521f
decal_fwd    26d302e97b016b60b3d8d202b815061e      odd_ls_point_fwd d2f62530c03eda9f8d3b855dd6a540e4
foliage_fwd  19f138ba17824037c143c2507927be80      odd_ls_spot_fwd 74bcd815f76b621a8021e8583e34a59e
foliage_def  07f8ad963cf4db5b0127a7e99e82a21b      odd_shadinst_nossao 84d4c5a9e6f754fa0e230a4793258156
```

Damit ist der aspektabhängige Schwanz nicht nur durch den Unit-Test ("a reused extract at another aspect
equals a full walk at it", Grenzen absichtlich auf ±1000 zerschossen, bitgleich gegen einen frischen Walk),
sondern auch im Bild belegt. Nicht abgedeckt bleiben GI, Low-Res-Wolken und der Wolken-Prepass (nicht
bitgenau), D3D11/D3D12/Vulkan (kein Gerät) und echte Spielinhalte (Skinned, Partikel, Trails).

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
- **Schritt 3 (Dirty-Flag), gebaut in Abschnitt 7**: Ein Vergleich von `parentWorld` mit einem gemerkten Wert spart die
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
  im nächsten Walk sichtbar). Ein Kommentar ist durch den In-Place-Zustand veraltet und wurde bewusst
  nicht angefasst (Metal blieb unberührt): `MetalRenderer.mm`, `EnsureGIProbeGrid` ("m_renderWorld was
  re-extracted by EncodeGIAccelBuild's extract() … creates BRAND NEW RenderObjects whose worldBounds are
  whatever the extractor could produce"): die Grenzen sind dort schon vom Schattenpass verfeinert. Das
  Verfeinern ist idempotent, nichts bricht; der Text gehört mit den fünf Schleifen zusammen in Schritt 4.
- **Schritt 5**: Die Leiter fährt gegen diesen Stand; der Zähler-Zeuge ist `RenderExtractor::extract`
  gegen `RenderExtractor::reuse` je Frame (`scripts/perf/dump_scope_p50.py`), erwartet (3, 2) auf Metal
  im Forward-Frame (**seit Schritt 4: (4, 3)**, siehe 8.5: der Frame nimmt den Walk, der Schattenpass
  behält seinen Aufruf als Schlüsselvergleich). Die Bench `Extraction bench: the frame copy against the
  walk it saves` lässt sich mit `--no-skip` wiederholen.

---

## 7. Schritt 3: Dirty-Flag für `propagateTransforms` (11.10.2026)

Stand: Zweig auf `53fc31cb` (Schritt 2). Die CI auf `9d664207` (Schritt 2) ist grün: im Lauf `38088649573`
Windows, macOS, Linux und Linux Vulkan (lavapipe), im Lauf `38088642698` ("Runtime flavours") macOS, Linux und
Windows. Der lavapipe-Job, der einzige Zeuge für Vulkan aus **einem** Walk (6.6), ist grün; an Schritt 2
war nichts zu beheben.

### 7.1 Die Entscheidung

§6.7 hatte offen gelassen, woher das Signal je Teilbaum kommt: "von den Schreibern gesetzt oder aus einer
Prüfsumme". Von den Schreibern geht es nicht. `TransformComponent::dirty` setzen Skripte, Physik, Animation,
Sequencer und Replikation, nicht aber der Inspector und der Gizmo, und an
`position`/`rotation`/`scale` wird über Referenzen aus Hunderten Stellen geschrieben; es gibt keinen Pfad, an
dem ein Zähler hängen könnte. Ein Vergleich von `parentWorld` mit einem gemerkten Wert spart die Multiplikation,
nicht den Abstieg.

Gebaut ist deshalb das Signal aus dem Vergleich, den es schon gab: der Lokal-Cache (`localCache` mit
`localCachePosition/Rotation/Scale`) hält die Werte, aus denen die lokale Matrix gebaut wurde. **Ein Scan über
den dichten `TransformComponent`-Speicher findet jede Entity, deren Werte nicht mehr dazu passen.** Das ist ein
sequentielles Lesen je Entity, ohne Baumabstieg, ohne `try_get` und ohne Matrixprodukt. Danach wird nur
gerechnet, was sich geändert hat, samt allem darunter.

Verworfen: Dirty von den Schreibern (nicht verlässlich, nicht prüfbar); `parentWorld`-Vergleich (spart den
Abstieg nicht); eine Prüfsumme je Gruppe (müsste jede Entity der Gruppe lesen, also dasselbe wie der Scan, nur
ungenauer); ein Hash der Werte (eine Kollision ließe eine Bewegung still ausfallen, das verträgt sich nicht mit
"bitgleich"); die Hot-Felder an den Anfang der Struktur ziehen (im Mikro-Bench kein stabiler Gewinn).

### 7.2 Was eine Weltmatrix bewegt, und wer es erkennt

Eine Weltmatrix ist `Eltern-Welt * Lokal`. Sie ändert sich aus fünf Gründen, jeder hat einen Detektor, und
jeder hat eine Negativkontrolle (7.6):

| Ursache | Detektor | Folge |
|---|---|---|
| Position/Rotation/Skala der Entity oder eines Vorfahren | Scan: 36 Byte Werte gegen die 36 Byte des Cache-Schlüssels. Ein gesetztes `dirty` zählt als Hinweis, die Werte entscheiden auch ohne | Entity plus Teilbaum neu |
| Eltern: Reparent, Zellen laden/entladen, Szene laden, Spawn, Destroy, Geschwister umordnen | `HorizonWorld::structureEpoch()` (Thema 164, nur gelesen, nicht erweitert) gegen die Epoche des letzten Durchlaufs | voller Walk |
| Links von Hand umverdrahtet | `HorizonWorld::noteStructureChanged()` (der Vertrag aus 164, die Lader rufen es) | voller Walk |
| Weltmatrix von Hand geschrieben | `HE::invalidateWorldMatrices(world)`. Das tut nur `shiftWorldOrigin`: es zieht `worldMatrix[3] -= shift` an **jeder** Entity ab, damit ein Leser vor dem nächsten Durchlauf die neue Lage sieht, und das ist nicht das Produkt, das `propagateTransforms` bildet (Rundung; und eine Entity ohne `HierarchyComponent` oder unter einem Knoten ohne Transform behält eine verschobene Matrix, zu der ihre Position nie passte) | voller Walk, einmal je Verschiebung |
| `TransformComponent` von einem Knoten genommen, der noch lebende Kinder hat | `on_destroy`-Hörer im Zustand: Kinder nehmen dann die Matrix des Großelternteils | voller Walk |
| Ganze Komponente zurückgeschrieben (`*tc = saved`, so macht es `CinematicPreview`) | `TransformCacheFlag`: `localCacheValid` überlebt eine **Kopie** nicht, die Kopie gilt als ungültig und der Scan findet sie, obwohl ihre Matrix von irgendwo anders stammt | Entity plus Teilbaum neu |

Eine neu angelegte `TransformComponent` hat einen ungültigen Cache und `dirty = true`: der Scan findet sie
ohne weiteres Zutun. Darum braucht ein Spawn keinen eigenen Detektor.

**Zellwechsel** (Auftrag der Königin: Floating Origin, Reparent und Zellwechsel müssen richtig ungültig werden).
Floating Origin und Reparent haben je eine Negativkontrolle, die rot wird (`ignoreInvalidation`,
`ignoreStructureEpoch`, 7.6). Für den Zellwechsel gibt es keine, und das ist das Argument, keine Lücke: jede
Komponente einer geladenen Zelle ist neu (ungültiger Cache, `dirty = true`), der Scan findet also den ganzen
Teilbaum auch ohne Epoche; die späteren Slices unter `attachTo` klettern auf die gespeicherte Matrix der
Zellwurzel; ein Entladen braucht keine Rechnung. Die Epoche ist dort ein zweiter Gurt (`applyAdditiveJson` ruft
`ensureEnvironmentLights` und räumt die Wurzel-Kinder auf, das kann auch bestehende Links berühren) und macht
aus dem Laden einen vollen Walk. Belegt wird der Zellfall nicht durch eine rote Kontrolle, sondern durch den
Gleichheitstest: die Eingriffsart `CellLoad` im Zufallsvergleich (Zelle additiv laden, Wurzel auf `-origin`) und der
Fall "a cell loaded and unloaded between two passes" (Laden, ruhige Welt, Floating Origin, Entladen), beide
bitgleich zum vollen Walk.

### 7.3 Der Ablauf

Der Zustand (`structureEpoch` des letzten Durchlaufs, `forceFull`, `preferFull`, die letzten Zahlen, ein
Scratch-Vektor) liegt je Registry in deren Kontext (`registry.ctx()`), nicht in `HorizonWorld`: kein Eingriff in
den Kopf, den alles einbindet, und jede Welt (Tests bauen viele, der Editor hat die Spielkopie) hat ihren eigenen.

1. **Voller Walk**, wenn es keinen Durchlauf gibt, auf dem man aufbauen kann (erster Aufruf), die
   `structureEpoch` eine andere ist, `forceFull` gesetzt ist oder `HE_PROPAGATE_FULL=1`.
2. Sonst der **Scan**: jede Entity mit `dirty` oder abweichendem Cache-Schlüssel bekommt `dirty = true` und
   kommt in eine Liste. Leer: fertig, nichts wurde geschrieben.
3. Mehr als die Hälfte der Transforms in der Liste: **voller Walk** (ein Aufstieg je Entity wäre teurer), und
   `preferFull` wird gesetzt. Solange das gilt, läuft der nächste Aufruf **ohne Scan** direkt den Walk; ein Walk,
   der höchstens ein Viertel der Lokalmatrizen neu bauen musste, nimmt das zurück. So kostet eine Welt, in der
   ohnehin fast alles läuft, nicht mehr als vorher (die Zeile "jedes Blatt" in 7.7 zeigt gleiche Zeiten).
4. Sonst je Eintrag der Liste: Eintrag ohne `dirty` ist schon von einem Vorfahren miterledigt (überspringen).
   Dann den Elternpfad hinauf bis zur Welt-Wurzel: ist ein Vorfahr `dirty`, deckt dessen Durchlauf diesen Eintrag
   ab (überspringen); endet der Pfad nicht an der Wurzel, erreicht auch der volle Walk die Entity nicht
   (überspringen, genau wie dort). Sonst ist die Welt-Matrix des nächsten Vorfahren mit Transform
   (gespeichert, aktuell, denn nichts darüber ist `dirty`) die Ausgangsmatrix, und `propagateFrom` rechnet die
   Entity und alles darunter. Entities ohne `HierarchyComponent` bekommen wie im vollen Walk `Lokal = Welt`.

### 7.4 Warum das bitgleich ist

Es gibt **eine** Stelle, die eine Weltmatrix bildet: `propagateFrom` (`parentWorld * refreshLocal(*t)`). Der volle
Walk und der Teilbaum-Durchlauf rufen dieselbe Funktion, und sie ist `noinline`, damit das Produkt nicht in einem
Aufrufer anders geplant oder zu FMA zusammengezogen wird als im anderen. Der Teilbaum bekommt als `parentWorld`
die gespeicherte Matrix des Vorfahren, und das sind dieselben Bits, die der volle Walk dem Kind als lokale Variable
weitergäbe. Der Lokal-Cache (`refreshLocal`) bleibt der alleinige Schiedsrichter, ob eine Lokalmatrix neu gebaut
wird; der Scan vergleicht Bytes statt Werte und ist damit nur an einer Stelle strenger (+0 gegen -0: eine
Entity mehr im Durchlauf, dieselbe Matrix) und an einer Stelle milder (NaN: Wert ungleich, Bytes gleich; der volle
Walk baut sie jedes Mal neu und bekommt jedes Mal dieselben Bits).

Die Annahme dahinter: jede Entity steht in den `children` genau ihres `parent` und nirgends sonst (das hält
`HorizonWorld`; der additive Lader hat deshalb den Eintrag unter der Wurzel entfernt, siehe Kommentar in
`applyAdditiveJson`), und niemand außer `propagateTransforms` und `shiftWorldOrigin` schreibt `worldMatrix`.

### 7.5 Was gebaut ist

| Datei | Änderung |
|---|---|
| `src/HE_Scene/src/TransformHierarchy.cpp` | Scan, Teilbaum-Durchlauf, Zustand im Registry-Kontext, `propagateTransformsFull` (der alte Walk, wörtlich, als Referenz und Rückfall), `invalidateWorldMatrices`, `lastPropagateStats`, `HE_PROPAGATE_FULL`, `HE_PROPAGATE_VERIFY`, Profiler-Scopes `Transforms::propagate/scan/subtrees/full` |
| `src/HE_Scene/include/HorizonScene/TransformHierarchy.h` | die neuen Aufrufe, `PropagateStats`, `PropagateTestSwitches`, der Vertrag (was erkannt wird, was angenommen wird) |
| `src/HE_Scene/include/HorizonScene/Components/TransformComponent.h` | `localCacheValid` ist ein `TransformCacheFlag`: liest sich wie ein `bool`, überlebt aber keine Kopie. Bleibt ein Aggregat (die Tests initialisieren mit `.position = ...`) und gleich groß (208 Byte) |
| `src/HE_Scene/src/FloatingOrigin.cpp` | `shiftWorldOrigin` ruft `invalidateWorldMatrices` |
| `src/HE_Scene/include/HorizonScene/Components/HierarchyComponent.h` | der Kopfkommentar nannte noch den Extractor als den, der läuft |
| `tests/test_transform_propagation.cpp` (neu), `tests/CMakeLists.txt` | 14 Fälle, siehe 7.6 |

`HorizonWorld` ist **nicht** angefasst: `structureEpoch` und `noteStructureChanged` reichen. Das heißt aber auch,
dass jede Strukturänderung (auch ein Spawn oder Destroy) den nächsten Durchlauf zu einem vollen Walk macht, siehe 7.8.

Zwei Schalter für Diagnose und Messung, beide aus der Umgebung gelesen:

- `HE_PROPAGATE_FULL=1`: jeder Aufruf läuft den ganzen Walk, wie vor diesem Schritt. Ein A/B im **selben
  Binary** (so ist 7.7 gemessen), und die Antwort auf "ist dieses falsche Transform der inkrementelle Pfad?".
- `HE_PROPAGATE_VERIFY=1`: nach jedem inkrementellen Durchlauf zusätzlich ein voller Walk und ein Bitvergleich
  aller Weltmatrizen; eine Abweichung wird geloggt und bricht ab. Langsam, für Läufe, die das Überspringen auf
  echtem Inhalt belegen sollen.

### 7.6 Tests und Negativkontrollen

`tests/test_transform_propagation.cpp`, 14 Fälle, 1132 Assertions, grün (auch mit `HE_PROPAGATE_VERIFY=1`).

- **Vergleich über zufällige Änderungsfolgen**: zwei Welten, gleich gebaut und gleich geändert (16 Seeds, je 300
  Schritte, je Schritt 1 bis 3 Eingriffe); die eine mit `propagateTransforms`, die andere mit dem vollen Walk
  nachgezogen, danach jede Weltmatrix beider **Bit für Bit** verglichen (`memcmp`). Eingriffe: Werte schreiben
  mit und ohne `dirty`, nur `dirty`, Reparent, Spawn (mit und ohne Transform), Destroy eines Teilbaums,
  Transform abnehmen und aufsetzen, Floating Origin, ganze Komponente zurückschreiben (mit `dirty = false`),
  Geschwister umordnen, Entity ohne Hierarchie, Entity ohne Verbindung zur Wurzel (die der volle Walk nie
  erreicht), eine Zelle additiv laden (mit dem Wurzel-Transform auf `-origin` wie der Streamer), großer Anteil der
  Welt auf einmal (über der Schwelle), Leerlauf. Die Welt enthält Knoten ohne Transform mitten in Ketten. Der Test
  prüft, dass alle drei Wege (still, Teilbäume, voller Walk) und alle 16 Eingriffsarten vorkamen. Der Zufall ist
  ein eigener LCG mit je einer Ziehung je Anweisung, auf jedem Compiler dieselbe Folge.
- **Arbeit, nicht nur Ergebnis** (über `lastPropagateStats`): eine Welt, in der nichts geschah, wird gescannt
  und nichts wird geschrieben; ein Blatt ist eine Matrix; eine Gruppe samt einem Blatt darin und einem Blatt
  einer anderen Gruppe sind 51 + 1 Matrizen (das Blatt in der Gruppe wird nicht zweimal gemacht, auch wenn die
  Gruppe nach ihren Kindern angelegt wurde); `dirty` ohne Wertänderung ist eine Matrix und wird gelöscht; eine
  Welt, in der drei Viertel läuft, fällt auf den Walk zurück und scannt wieder, sobald sie ruhig ist.
- **Jeder Detektor, einzeln abgeschaltet** (`PropagateTestSwitches`, bleiben im Code, damit die Kontrolle wahr
  bleibt): `ignoreStructureEpoch` (Reparent), `ignoreInvalidation` (Floating Origin), `ignoreValueCompare`
  (Werte ohne `dirty`), `ignoreTransformRemoval` (Transform vom Elternknoten genommen). Je Detektor ein
  gezielter Fall und dazu: dieselben Zufallsfolgen divergieren, und alle Schalter aus divergieren nicht.
  Dazu die zwei Verträge als Fälle: Links von Hand umverdrahtet **ohne** `noteStructureChanged` bleiben alt
  (das ist der Vertrag, kein Fehler), **mit** stimmen sie; eine zurückgeschriebene Komponente wird gefunden, und
  ein `localCacheValid`, das die Kopie überlebt hätte, würde sie verstecken.
- **Mutationen am Produktcode** (je ein Build, nur diese Datei rot, Quelle danach zurück):

  | Mutation | rot |
  |---|---|
  | Prüfung "Vorfahr ist `dirty`" entfernt | "a moved group carries its children, once" (Ergebnis bleibt richtig, Arbeit verdoppelt sich) |
  | Prüfung "schon von einem Vorfahren erledigt" entfernt | derselbe Fall, aber erst mit der Gruppe, die nach ihren Kindern angelegt ist (davor blieb sie grün: die Lücke war im Test, nicht im Code) |
  | Ausgangsmatrix des Vorfahren nicht genommen | 5 Fälle |
  | Entity ohne Hierarchie schreibt nichts | die Zufallsfolgen (2 Fälle) |
  | Keine Schwelle zum vollen Walk | "most of the world moving ..." |

- **Echter Inhalt**: der ganze ctest (258 Tests) mit `HE_PROPAGATE_VERIFY=1`: jeder `propagateTransforms`-Aufruf
  aller Tests (Kamera-Rig, Sequencer, Physik, Floating Origin, Zellen, Extractor, 100k-Entity-Fälle) wurde gegen
  den vollen Walk geprüft, kein Abbruch. Der Editor-Loop mit der 50k-Welt, 360 Frames mit
  `HE_PROPAGATE_VERIFY=1`: kein Abbruch.

### 7.7 Messung

**Mikro (Bench `Transform propagation bench`, Release, M5, `--no-skip`)**, Median von 7 Läufen, ms, voller Walk
gegen `propagateTransforms`, Welt in Gruppen zu 100 (die Bench misst auch eine flache Welt mit allen Entities
direkt unter der Wurzel, mit ähnlichen Zahlen):

| Entities | nichts bewegt | ein Blatt | 1 % der Blätter | eine Gruppe (101) | 10 % der Gruppen | jedes Blatt |
|---|---|---|---|---|---|---|
| 1 000 | 0,10 → 0,015 | 0,10 → 0,015 | 0,10 → 0,018 | 0,10 → 0,024 | 0,10 → 0,024 | 0,22 → 0,22 |
| 10 000 | 1,0 → 0,17 | 1,0 → 0,16 | 1,0 → 0,19 | 1,0 → 0,17 | 1,0 → 0,25 | 2,2 → 2,2 |
| 50 000 | 5,0 → 1,06 | 5,0 → 0,94 | 5,0 → 0,7 bis 1,1 | 5,0 → 0,86 | 5,0 → 1,5 | 10,9 → 10,9 |
| 100 000 | 10,0 → 2,4 | 10,0 → 2,1 | 10,1 → 3,1 | 10 → 2,1 | 10,0 → 3,3 | 21,6 → 21,8 |
| 200 000 | 19 → 5,0 | 20 → 5,0 | 20 → 6,8 | 20 → 4,8 | 20 → 7,3 | 43,4 → 43,3 |

Der Gewinn bei ruhiger Welt ist rund das Vierfache (der Scan kostet etwa 25 ns je Entity bei 200k, etwa 20 ns bei
50k), je mehr sich ändert desto weniger, und wenn fast alles läuft (letzte Spalte, über der Schwelle) kostet es
dasselbe wie vorher. Einzelne Ausreißer der Walk-Spalte (1,8 ms bei 50k/1 %, 5,3 bei 100k/Gruppe) sind der
Wechsel zwischen schnellen und langsamen Kernen im Median über 7 Läufe; die Bench ist ein Verhältnis, keine
Absolutzahl.

**Echter Editor-Loop** (`scripts/he_perf_capture.py`, Metal, `/tmp/he162_scenes/ref_50000.hescene` = 50 568
Entities, `--warmup 240 --frames 120 --no-counters --cam 0,25,90,0,-0.25`; **Bildschirm gesperrt, Akkubetrieb,
Last 2 bis 3**, also nicht die Bedingungen von 6.5, nur zwischen den Zeilen vergleichbar), abwechselnd im
**selben Binary** mit `HE_PROPAGATE_FULL=1` (= vorher) und ohne (= nachher), p50 je Frame:

| Lauf | `Transforms::propagate` | `RenderExtractor::extract` (ein Walk) | CPU je Frame |
|---|---|---|---|
| vorher 1 | 14,73 ms | 31,5 ms | 64,9 ms |
| nachher 1 | 1,35 ms | 18,5 ms | 52,1 ms |
| vorher 2 | 15,00 ms | 31,8 ms | 64,5 ms |
| nachher 2 | 1,39 ms | 19,1 ms | 54,1 ms |

`propagateTransforms` ist im echten Loop knapp ein Elftel (15,0 → 1,4 ms), mehr als im Mikro-Bench: dort liegen
die Entities dicht und frisch angelegt, hier hat jede Entity viele andere Komponenten und ihre `children`-Vektoren
liegen verstreut, was den Baumabstieg teuer macht, den linearen Scan aber nicht. Der Walk im Extract sinkt
von 31,6 auf 18,8 ms, die CPU je Frame von 64,7 auf 53,1 ms (minus 11,6 ms, minus 18 %). Die Vorher-Zeile
sagt auch, wohin der Rest des Extracts geht: **47 % des Walks waren `propagateTransforms`**, das ist der Anteil, der in
6.7 noch offen war. Was bleibt (18,8 ms im Walk) ist `extractMeshes` und Zubehör, nicht Transforms.

### 7.8 Grenzen

- **Jede Strukturänderung macht einen vollen Walk.** `structureEpoch` bewegt sich bei `createEntity`,
  `destroyEntity`, `reparentEntity`, Geschwister umordnen, `clear`, jedem Lader und jedem Zellwechsel. Ein
  Spiel, das jeden Frame spawnt, läuft damit wie vorher (kein Rückschritt, kein Gewinn). Feiner ginge es mit
  einem zweiten Zähler, der nur das bewegt, was **bestehende** Entities umhängt (Reparent, Lader, Floating
  Origin, `clear`) und Spawn/Destroy auslässt: ein neuer Eintrag wird vom Scan ohnehin gefunden, ein
  entfernter braucht nichts. Das ist ein Eingriff in `HorizonWorld.h` (Neubau von allem) und ein Fall für
  einen Folgeschritt, wenn die Messung zeigt, dass Spawns im Frame vorkommen.
- **Der Scan ist O(N).** Etwa 25 ns je Entity bei 200k (DRAM-gebunden). Auf mehrere Kerne verteilt
  (`HE::parallel_for` gibt es) wäre er ein Drittel davon; nicht gebaut: bei 50k sind es 1,3 ms von 52.
- **Mehr als die Hälfte der Welt in Bewegung** läuft den Walk, und der kostet wie vor diesem Schritt.
- **Mehrere Aufrufe je Frame** (Kamera-Rig bis zu dreimal, Navigation, Extractor) zahlen je einen Scan. Sie
  waren vorher je ein voller Walk.
- **Nicht getestet auf dieser Maschine**: Windows und Linux (Compiler: MSVC, GCC; die Bitgleichheit gilt dort,
  wenn derselbe Quelltext dieselben Bits bildet, und genau das prüft der Vergleichstest in der CI), D3D11,
  D3D12 und Vulkan (kein Backend-Code angefasst).
- Wer **`worldMatrix` selbst schreibt** (außer `shiftWorldOrigin`) oder **Links von Hand umhängt** ohne
  `noteStructureChanged`, bekommt eine alte Matrix. `HE_PROPAGATE_VERIFY=1` findet es.
- **Vorbestehend, nicht angefasst, aber im Test sichtbar geworden**: `shiftWorldOrigin` verschiebt nur die
  Position von Wurzelkindern **mit** `TransformComponent`. Ein Wurzelkind ohne Transform (ein Ordner aus
  `createEntity`) bleibt, und seine Kinder erfasst die Verschiebung nur in der Weltmatrix, die der nächste Durchlauf
  aus den unverschobenen Lokalwerten neu bildet: sie stehen danach wieder an ihrer alten lokalen Stelle, in
  absoluten Koordinaten also um `shift` versetzt. Der alte volle Walk tat dasselbe; der neue ist bitgleich dazu und
  erbt es. Wer Ordner unter der Wurzel mit Floating Origin nutzt, gibt ihnen ein Transform.
- Zwei Kleinigkeiten ohne Folgen für das Ergebnis: `propagateTransformsFull` setzt `preferFull` nicht zurück (nach
  einer Strukturänderung kostet das höchstens einen überzähligen Walk, der sich selbst korrigiert), und eine
  Hierarchie, die an nichts hängt (Orphan), bleibt `dirty` und zählt in jedem Scan in `flagged`, ohne dass etwas
  gerechnet wird (nur in beschädigten Szenen).

### 7.9 Was für Schritt 4 und 5 daraus folgt

- Schritt 5 kann **dieselbe Binary** vorher und nachher messen: `HE_PROPAGATE_FULL=1` ist der Zustand von
  vor diesem Schritt (alle anderen Schritte bleiben an). Die Scopes `Transforms::*` weisen den Anteil aus.
- Der Walk im Extract hat noch 18,8 ms bei 50k (6.5: 17,0 ms bei anderer Last); die nächsten Hebel liegen in
  `extractMeshes` (RenderObject je Entity, 280 Byte), im fünffachen `FrustumCuller::cull` und in den fünf
  Verfeinern-Schleifen der Metal-Pässe (6.7), nicht mehr in den Transforms.
- Im Schattenpass (Schritt 4) ändert sich nichts durch diesen Schritt: die Sonnenrichtung bei Tag/Nacht hängt
  nicht an den Transforms.
- CI zu diesem Schritt: siehe die Hive-Meldung des Themas (Lauf auf dem Commit dieses Schritts).

---

## 8. Schritt 4: Der Schattenpass liest den Frame-Zustand (11.10.2026)

Stand: Zweig auf `7cb9a18d` (Merge von `origin/release/0.7.0` in `9a890ca6`, Schritt 3; der Merge brachte nur
Editor-Code, `.heproj`-Registrierung). Die CI auf `9a890ca6` ist grün (Läufe `38095347474`, `38088649573`,
`38088642698`).

### 8.1 Was der Schattenpass vorher tat

Auf Metal und Vulkan war `EncodeShadowMap` der erste Pass eines Frames und rief selbst `setDayNight`,
`setContentManager` (Vulkan zusätzlich `setShadowSettings`) und `extract()` auf. Er zahlte deshalb den einen vollen
Walk des Frames, und der Profiler buchte ihn unter `Metal::EncodeShadowMap`: die "35 ms `EncodeShadowMap` (den
Extract eingerechnet)" aus 153 §11.3 sind im Kern der Walk, der Pass selbst ist der Rest (6.5: 3,8 ms bei 50k,
unten 8.5: 25,0 ms mit Walk und Verfeinern, 2,7 ms ohne beides). Danach lief jeder Pass seine eigene Schleife über
alle Objekte, die deren Grenzen aus dem echten Mesh verfeinert (`RenderObject::refineWorldBounds`): drei auf Metal
(Schatten, SSAO, Szene), bis zu fünf auf Vulkan (Decal-Tiefe, Kaskaden, Szene, GI, SSAO). Das sind die "fünf
Verfeinern-Schleifen", die 6.7 für diesen Schritt vorgemerkt hatte, und sie sind der größte Posten außerhalb des
Walks: **3,5 ms je Lauf bei 50k Entities, 10,6 ms je Frame, ein Fünftel der CPU-Zeit** (gemessen mit eigenem Scope,
8.5).

D3D11, D3D12 und OpenGL extrahieren einmal je Frame in `DrawScene` und lassen ihren Schattenpass `m_renderWorld`
lesen (2.3): für den Schattenpass gab es dort nichts umzustellen. Sie verfeinern weiter je Pass (ein bis zwei
Stellen), also nicht mehrfach je Frame, und sind unverändert.

### 8.2 Was gebaut ist

Zwei Änderungen, beide auf Metal und Vulkan:

| Stelle | Änderung | Wirkung |
|---|---|---|
| `MetalRenderer::ExtractFrame(aspect)` (neu), `EncodeFrame` | Der Frame nimmt den Walk, bevor der erste Pass läuft: Sonne (`GetEnvironment()`), ContentManager, `extract()`. Eigener Profiler-Scope `Metal::ExtractFrame`. | Der Walk gehört dem Frame, nicht dem Pass; das Profil trennt beides |
| `VulkanRenderer::extractFrame(aspect)` (neu) | Dasselbe an den zwei Stellen, die einen Frame aufzeichnen (`Render()` ohne Viewport, `DrawViewportFrame`): Schatten-Einstellungen, ContentManager, `extract()`. Die Sonne bleibt einmal oben in `Render()`/`RenderSceneImage()` (Pin: genau zwei `setDayNight`). | Textpatch, nur die CI belegt mehr (8.6) |
| `MetalRenderer::RefineObjectBounds()`, `VulkanRenderer::refineObjectBounds()` (neu) | Die Verfeinern-Schleife läuft **einmal je Walk**: der Helfer merkt sich `RenderExtractor::fullExtractCount()` sowie Objektfeld und Größe und kehrt zurück, wenn beides passt. Der Frame ruft ihn direkt nach dem Walk, die Pass-Schleifen sind Aufrufe desselben Helfers (Metal 5, Vulkan 5). | Metal: zwei von drei Läufen entfallen; Vulkan: vier von fünf |
| `EditorApplication.cpp` | `HE_DUMP_DAYNIGHT=0` schaltet im Headless-Dump den Zyklus aus (Standard an, wie immer) | Zeuge für die Zyklus-aus-Seite |
| `scripts/perf/frame_pixel_ab.sh` | dritter Parameter `frame\|shadow\|all`, neuer Satz `shadow` mit 26 Zeugen | 8.5 |
| `scripts/perf/ladder_table.py` | Spalten `Metal::ExtractFrame` und `Metal::RefineBounds` | Schritt 5 liest den Walk dort |
| `scripts/he_shot.py` | `DAYNIGHT` und `SHADOW` im Kopf dokumentiert | |
| Kommentare | `EnsureGIProbeGrid` (der in 6.7 gemerkte Kommentar, der von einem Re-Extract sprach), Wolken-Schatten und Sky-View-LUT in `EncodeFrame` | |

### 8.3 Warum der Aufruf im Pass bleibt, und warum es eine Funktion ist

Wie in 6.1: jeder Pass sichert sich selbst ab, dass `m_renderWorld` den Walk mit **seinem Aspekt** hält, egal was
vor ihm lief. Der `extract()`-Aufruf im Schattenpass ist deshalb nicht weg, sondern ein Schlüsselvergleich (Metal
`ExtractFrame(aspect)` im Pass, Vulkan `extractFrame(aspect)`), und `RefineObjectBounds()` im Pass findet die Grenzen
fertig. Auch der Schattenpass kehrt weiter früh zurück, wenn es nichts zu zeichnen gibt (keine Sonne und kein
Lokalschatten): der Frame hat den Walk dann trotzdem genommen, wie es vorher der nächste Pass getan hätte.

Das hat eine Falle, die kein Bildtest sieht: der zweite Aufruf bleibt nur dann ein Schlüsselvergleich, wenn er genau
das pusht, was der erste gepusht hat. Ein Setter, der abweicht (Schatten-Einstellungen, ContentManager, Sonne), macht
den Pass zu einem **zweiten vollen Walk**: dasselbe Bild, gut 18 ms mehr bei 50k, weder Pixel-A/B noch lavapipe noch
ein Test des Extractors sehen es. Darum gehen Frame und Pass durch dieselbe Funktion, der Pass pusht nichts selbst,
und ein Quelltext-Pin hält das fest (8.5). Der Zähler-Zeuge im Betrieb ist `RenderExtractor::extract` gegen
`RenderExtractor::reuse` je Frame: ein Walk je Frame heißt (4, 3), jeder zusätzliche Walk wäre (5, 3) mit einem
zweiten großen Eintrag.

### 8.4 Die Sonderfälle

**Day/Night.** Mit dem Zyklus an ändert sich die Sonnenrichtung jeden Frame, und das ist unabhängig davon, was an
den Entities dirty ist. Sonne, Mond, Farben, Stärken, Bewölkung und `dayNight` selbst sind Teil des
`FrameKey` (`timeOfDay` und die anderen), ein Walk mit einer anderen Sonne kann also nie als Antwort auf einen
anderen dienen. Metal schiebt `GetEnvironment()` im Helfer vor jedem Walk, Vulkan einmal oben in `Render()` und
`RenderSceneImage()`, vor dem ersten Extract. Schritt 3 berührt weder Lichter noch Sonne (7.9), und das ist hier
belegt statt nur behauptet:

- Einheitstest `a frame as the backends record it is one walk with that frame's sun, night to night`: sieben
  Tageszeiten (Nacht, Dämmerung, Morgen, Mittag, Abend, Nacht, bei denen das Schattenlicht vom Mond zur Sonne und
  zurück wechselt) in der Aufrufreihenfolge eines Frames (Walk, Schattenpass, SSAO bei halber Auflösung, Szene):
  je Frame genau ein Walk (`full == Frames`, `reuse == 3 × Frames`), und der Zustand nach dem Frame ist bei beiden
  Aspekten bitgleich zu einem frischen Walk mit derselben Tageszeit. Positivkontrolle: die Sonne und die erste
  Kaskade bewegen sich von Frame zu Frame wirklich.
- Zyklus aus: die Sonne bleibt auf ihrer festen Richtung, die Tageszeit wird ignoriert (Test, Bild `dn_off_*`).
- Volle Bewölkung: Sonne und Mond gehen auf Intensität 0, `shadow.enabled` ist falsch, der Schattenpass kehrt
  vor dem Verfeinern zurück, der Lokalatlas hat seine Schichten weiter (Test, Bild `dn_overcast_fwd`).
- Kein Himmel: ohne Sonnenlicht in der Welt gibt es keine Kaskaden (Bild `nosky_fwd`).

**Kaskaden.** Der aspektabhängige Schwanz (Projektion, Kaskaden-Fit, lokale Schichten) läuft bei SSAO an Ort und
Stelle aus der Szenenbox des Walks (6.2) und ist in Schritt 2 für drei Kaskaden bitgleich belegt. Hier zusätzlich:

- Einheitstest `the cascade settings reach every pass of the frame, and a pass that pushes others pays a walk`:
  fünf Einstellungen (1, 2, 3 Kaskaden, Distanz 30 bis 250 m, Auflösung 512 bis 4096), je Frame ein Walk und der
  Zustand bei beiden Aspekten bitgleich zu einem frischen Walk; jede Einstellung ist ein anderer Fit. Und die Falle
  aus 8.3 als Test: ändert ein Pass zwischen Walk und Schlüsselvergleich die Einstellungen, kostet das genau einen
  zusätzlichen Walk (`fullExtractCount() + 1`) und liefert die neuen Kaskaden, nicht die alten.
- Bilder: 1, 2 und 3 Kaskaden, Auflösung 512, Lambda 1, Kamera nah, fern und mit der Zeile über der ersten Grenze
  (3 Kaskaden über 60 m trennen bei etwa 10, 24 und 60 m), Sonne und Lokallicht zugleich in einem Schattenpass,
  zwei Kaskaden beim ungeraden Render-Scale 0,51 (dort läuft der Schwanz zwischen Schatten- und Szenenpass).
- Die Schatten-Einstellungen (`SetShadowSettings`): Distanz, Kaskadenzahl und Lambda gehen beim Setzen in den
  Extractor, ein Auflösungswechsel wird am Anfang von `EncodeFrame` übernommen, vor dem ersten Extract. Alle vier
  stehen im Schlüssel: ein Wechsel mitten im Frame wäre ein neuer Walk, nie die Kaskaden des alten.

**Verfeinern einmal je Walk: die eine Grenze.** Ein Mesh, das beim Lauf der Schleife noch nicht auflösbar war, behält
bis zum Ende des Walks die Grenze, die der Walk ihm gab, im Zweifel ungültig. Wird es zwischen dem Lauf im Frame und
einem späteren Pass desselben Frames auflösbar (Laden im Hintergrund), hätte der alte Code es im späteren Pass
verfeinert, der neue erst im nächsten Frame. Ein Objekt mit ungültiger Grenze wird nie gecullt, und die Zeichen-
Schleifen lösen das Mesh selbst auf: das ist die vorsichtige Seite, ein Frame mit etwas mehr Draws am Rand, keine
verlorenen Objekte. Das gilt nur im Zeitfenster eines einzelnen Frames.

### 8.5 Belege

**Bild, Metal** (`scripts/perf/frame_pixel_ab.sh`, privates `HE_CONFIG_DIR`, `HE_SKY_TIME=30`, AA aus, Render-
Pfad explizit). Der Vorher-Stand wurde **vor dem ersten Rebuild** als selbstständige Deploy-Kopie gesichert
(`selfcontain_deploy_copy.sh`; Beleg, welche Build eine Kopie hält: das Symbol `MetalRenderer::ExtractFrame` steht
nur in der neuen `libHorizonRendering.dylib`). Reihenfolge der Kontrollen:

1. Der Vorher-Stand reproduziert die 16 md5s aus 6.5 (Schritt 2) Byte für Byte: Schritt 3, der Merge und macOS
   haben nichts verschoben, die Tabelle ist eine gültige Referenz.
2. Rauschboden: die erste neue Build zweimal gefahren, beide Läufe in beiden Sätzen byte-gleich (16 von 16 und
   26 von 26). Der Vorher-Stand ist zusätzlich durch die Tabelle aus Schritt 2 belegt (anderer Lauf, andere
   Sitzung), und alle drei neuen Stände stimmen untereinander überein.
3. Sensitivität: die 24 vergleichbaren Zeugen des neuen Satzes `shadow` sind 24 **verschiedene** Bilder (24 md5s),
   und sie zeigen ihr Motiv (angesehen: Kaskadenschatten über mehrere Tiefen, lange Morgenschatten, bei voller
   Bewölkung gar keine).
4. Ergebnis, für alle drei Stände (nur Frame-Walk, plus Verfeinern einmal je Walk in der ersten Pass-Schleife, plus
   Verfeinern im Frame-Schritt = der Stand dieses Commits): **16 von 16 byte-identisch zur Referenz, 24 von 24
   vergleichbaren Zeugen des Satzes `shadow` byte-identisch zur alten Binary.** Die zwei Zeugen mit ausgeschaltetem
   Zyklus (`dn_off_*`) gehen nicht gegen die alte Exe (sie kennt den Schalter nicht): dort steht der alte Renderer
   in der **neuen Exe** gegen den neuen Renderer in derselben Exe, byte-identisch, mit Kontrollen (Zyklus aus bei
   TOD 0,5 gleich TOD 0,9, Zyklus an bei TOD 0,9 ein anderes Bild).

Die md5s des Satzes `shadow` (Referenz für Schritt 5, nur auf diesem Gerät und dieser macOS-Version gültig; die 16
des Satzes `frame` stehen in 6.5):

```
dn_0_fwd           c3f894f1a9a86dc71a48f1ba0f520072    casc1_fwd         70dd8a52a51d552704ff02984a933cce
dn_20_fwd          a8f787f0216b25c2ec76d8a4472aa836    casc2_fwd         213e4a78fe48a2b894b467a16724564f
dn_25_fwd          08c787b3c1011b961f9795d504805e83    casc3_fwd         f09feee1d02db976fe160fcd5ff9895f
dn_30_fwd          435272e08df94aa62f326cace95ecaff    casc3_def         cbd90d0851f6a2cee8663b67fcf599de
dn_50_fwd          a4d726a63ac6d18f3528f71862daedd6    casc3_res512_fwd  a20ef0d3caeaaaa1b177e85a8d4e07c3
dn_70_fwd          bde03197cb89e5aea56f151789a15312    casc3_lambda1_fwd 0417e4b606cd782115d89fc0a344978c
dn_75_fwd          fbb540c1b9eed22b91f6392732eed041    casc3_near_fwd    4ab0dc96bd3ee458ae0941dd4ee37b1c
dn_90_fwd          a918b0e6239d4133d4fb6cbb2ddeca3f    casc3_far_fwd     c3182f81cf4b2aa14f2767320c1c56b2
dn_30_def          ef201f9c4c7e24a0feb63a45975448da    casc3_split_fwd   4ff07829f0bed3138d5218b2fd9e8ed3
dn_70_def          5cd138c546e80f3d94577c733f3ff85a    ls_point_day_fwd  385de35fb6e7c4e14e68f09befd3969f
dn_off_fwd         a11a70f2502112f5a42f167dbf23931b    ls_spot_day_def   9d1b83e8cb3224ce73d4f9a828eaa84a
dn_off_def         2d91dff673508ddc70e6b43d326e1052    odd_casc2_fwd     4c3d0cd5318f843879366fd34d2aded5
dn_overcast_fwd    73b9fd6f017b060c45f7525cff365b8c    nosky_fwd         be9d5dc81f95503f8c6b0cb6aec001bf
```

**Zähler und Profil, Metal, 50 568 Entities** (`scripts/he_perf_capture.py`, `ref_50000.hescene`, `--warmup 240
--frames 120 --no-counters --cam 0,25,90,0,-0.25`, ein **Scratch-Projekt** kopiert aus dem Test-Projekt, damit
kein Projekt des Menschen angefasst wird; Fenster **versteckt**). Bedingungen, ehrlich: **Bildschirm gesperrt,
Akkubetrieb, Last 2 bis 4, andere Hive-Instanzen am Rechner**. Die absoluten Zahlen sind damit nicht mit 6.5 und 7.7
vergleichbar (anderes Fenster, anderer Zustand), nur alt gegen neu **in dieser Sitzung**, abwechselnd gefahren.
p50 je Frame in ms, in Klammern die Aufrufe je Frame:

| Lauf | Stand | CPU/Frame | `extract` | `reuse` | `ExtractFrame` | `EncodeShadowMap` | `RefineBounds` | `EncodeSSAO` | `EncodeScene` | `Render` |
|---|---|---|---|---|---|---|---|---|---|---|
| old1, old2 | vor Schritt 4 | 51,40; 50,15 | 19,10; 19,07 (3×) | (2×) | | 25,74; 25,34 | | 8,02; 7,90 | 7,78; 7,80 | 41,94; 41,79 |
| old3, old4, old5 | vor Schritt 4 | 49,46; 49,58; 49,47 | 18,55; 18,64; 18,79 (3×) | (2×) | | 24,86; 24,81; 25,00 | | 7,76; 7,72; 7,75 | 7,58; 7,43; 7,53 | 40,99; 40,59; 40,97 |
| new1, new2 | nur Frame-Walk | 49,77; 49,90 | 18,79; 18,73 (4×) | (3×) | 18,79; 18,73 | 6,33; 6,52 | | 7,95; 8,07 | 7,71; 7,78 | 41,32; 41,52 |
| meas1 | + Scope um die Schleife | 49,39 | 18,48 (4×) | (3×) | 18,48 | 6,23 | **10,56 (3×)** | 7,68 | 7,54 | 40,82 |
| n2a, n2b | Verfeinern einmal, im ersten Pass | 41,74; 42,36 | 18,37; 18,66 (4×) | (3×) | 18,37; 18,66 | 6,24; 6,21 | **3,48 (1×)** | 4,05; 4,09 | 4,07; 4,04 | 33,45; 34,04 |
| n3a, n3b | **dieser Commit** | 43,15; 41,66 | 18,87; 18,56 (4×) | (3×) | 22,25; 22,06 | 2,85; 2,69 | 3,48; 3,54 (1×) | 4,11; 4,06 | 4,21; 4,10 | 34,32; 33,45 |

Was die Tabelle sagt:

- **Ein Walk je Frame, wie vorher.** `extract` (3 → 4) und `reuse` (2 → 3) je Frame wachsen um genau den
  Schlüsselvergleich des Schattenpasses; die Walk-Zeit (`extract` p50, 18,4 bis 19,1 ms) bleibt, es gibt keinen
  zweiten Walk. Die Erwartung für Schritt 5 ist damit (4, 3), nicht (3, 2) (6.7 ist angepasst).
- **Das Profil trennt Walk und Pass.** `ExtractFrame` (18,8) plus `EncodeShadowMap` (6,3) ist der alte
  `EncodeShadowMap` (25,3 bis 25,7). In diesem Commit steht das einzige Verfeinern (3,5) im Frame-Schritt:
  `ExtractFrame` 22,1 bis 22,3 (Walk plus Verfeinern), `EncodeShadowMap` nur noch 2,7 bis 2,9. **Wer in Schritt 5
  `Metal::ExtractFrame` mit dem alten `RenderExtractor::extract` vergleicht, sieht den Walk gewachsen: es ist das
  Verfeinern.** `EncodeShadowMap` zwischen vorher und nachher nur als Summe mit `ExtractFrame` vergleichen.
- **Verfeinern einmal je Walk spart bei 50k rund 6 bis 8 ms je Frame** (CPU/Frame 49,5 auf 41,7 bis 43,2, `Render`
  41,0 auf 33,5 bis 34,3): SSAO und Szene sinken um je 3,5 bis 3,9 ms, ungefähr um ihre Schleife (`RefineBounds`
  10,56 ms in drei Läufen auf 3,48 ms in einem). Die alte Binary als Gegenprobe zwischen den neuen Läufen (old5)
  liegt wieder bei 49,5, die Verschiebung ist also kein Drift der Sitzung.
- Die Streuung der neuen Läufe (41,7 bis 43,2) ist größer als die der alten (49,5 bis 49,6); eine Genauigkeit über
  1 ms hinaus nehme ich daraus nicht. Die saubere Leiter bei entsperrtem Bildschirm und am Netz ist Schritt 5.

**Tests, lokal** (Release, shaderc ON, frisches `HOME`, `HE_CONFIG_DIR` ungesetzt):

- Neu in `tests/test_world_scale.cpp`: die drei Fälle aus 8.4 (Tageszeiten, Kaskaden-Einstellungen, Zyklus aus und
  volle Bewölkung) und `the bounds a pass refined stay for the rest of the frame, the next walk starts unrefined`
  (der Zähler `fullExtractCount()` bewegt sich bei einer Wiederverwendung nicht und bei jedem neuen Walk, und was
  ein Pass in den Objekten schrieb, bleibt für den Rest des Frames: das ist die Annahme hinter "einmal je Walk").
- Neu in `tests/test_culling.cpp`: der Quelltext-Pin `The frame is walked before the shadow pass, through the
  function the pass itself calls`. Metal: der Walk (`ExtractFrame`) und das Verfeinern stehen in `EncodeFrame` nach
  dem `FrameScope` und vor dem Schattenpass; der Helfer pusht Sonne, ContentManager und `extract` in dieser
  Reihenfolge; `EncodeShadowMap` hat keinen eigenen `extract`/`setDayNight`/`setContentManager`/
  `setShadowSettings`. Vulkan: dasselbe an den zwei Stellen, die einen Frame aufzeichnen, und der Helfer enthält
  kein `setDayNight`. D3D11, D3D12, OpenGL: genau ein `m_extractor.extract(` je Datei (die Vorschau-Instanz in
  OpenGL ist ein anderer Extractor).
- `tests/test_foliage_cluster.cpp` (`Every backend refreshes bounds through RenderObject::refineWorldBounds`):
  Metal und Vulkan haben genau eine Schleife (im Helfer) und mindestens fünf Aufrufe; die anderen drei unverändert.
- **Negativkontrollen**: drei Mutationen am Quelltext, der Pin wurde jedes Mal rot, danach der Originalstand per
  `cp -p` zurück (Byte-Vergleich und Zeitstempel, damit ninja nicht neu baut): ein eigener `setContentManager` im
  Metal-Schattenpass, ein fehlendes `extractFrame` im Vulkan-Frame, ein zweiter `extract()` in D3D11.
- ctest voll (`-j4 --timeout 1500`): **259 Tests, 259 grün, 0 rot, 2 übersprungen** (`runtime_size_app_advanced`
  und `_basic`, für diese Plattform "reported and skipped" per Konstruktion), 297 s. Der Stand ist der Commit
  dieses Schritts nach einem Vollbau (rc 0).

### 8.6 Was nur die CI belegt

- **Vulkan**: `VulkanRenderer.cpp` wird auf diesem Mac nicht kompiliert. Lokal belegt sind `clang -fsyntax-only`
  gegen die MoltenVK-Header (Homebrew `molten-vk` 1.4.1, Flags aus dem Compile-Befehl des OpenGL-Objekts), **rc 0**,
  mit zwei Negativkontrollen (ein vertippter Aufruf scheitert an allen fünf Stellen, ohne den MoltenVK-Include
  scheitert es an `vulkan/vulkan.h`), und der Quelltext-Pin. Dass es unter MSVC baut, belegt der Windows-Job; ob
  die fünf Pässe aus einem Walk und mit einmal verfeinerten Grenzen dasselbe Bild liefern, höchstens der Job
  `vulkan-lavapipe` (Bild-A/B mit Validation). **Auf Hardware ist nichts davon gelaufen, und der Gewinn vom
  Verfeinern (6 bis 8 ms bei 50k) ist für Metal gemessen; für Vulkan ist es dieselbe Schleife, aber ungemessen.**
- **D3D11, D3D12**: keine Quelländerung. Beide verfeinern weiter je Pass (eine Stelle), nicht mehrfach je Frame. Der
  Pin liest ihren Quelltext, geprüft wird nichts davon zur Laufzeit.
- **Linux/OpenGL**: Linux-Job und `he_tests`; `RenderExtractor.h` ist in diesem Schritt **nicht** geändert.
- **Metal** ist lokal belegt (Bild, Zähler, Profil, Tests). **Nicht im Bild abgedeckt** bleiben GI, die Wolken mit
  halber Auflösung und der Wolken-Prepass (nicht bitgenau), Skinned, Partikel, Trails und echte Spielinhalte. Der
  Fall "Mesh wird mitten im Frame auflösbar" (8.4) ist begründet, nicht gemessen.

### 8.7 Offen, und was für Schritt 5 folgt

- **Zähler-Erwartung (4, 3)** auf Metal im Forward-Frame, wie 8.5; `scripts/perf/ladder_table.py` hat die zwei neuen
  Spalten. Die Leiter 1k/10k/50k/100k/200k vorher/nachher ist Schritt 5; `HE_PROPAGATE_FULL=1` (7.9) gibt weiter
  den Stand von vor Schritt 3 im selben Binary, für Schritt 4 gibt es keinen Schalter (die alte Binary liegt als
  Kopie, wer sie braucht, baut `9a890ca6`).
- **Der Rest des Frames bei 50k** (Metal, p50): Walk 18,6 (davon `Transforms::propagate` 1,3, der Rest ist
  `extractMeshes` und Zubehör), Verfeinern 3,5, `FrustumCuller::cull` 3,2 (fünf Läufe, drei davon die Kaskaden),
  SSAO und Szene je 4,1, der Schattenpass selbst 2,7. Nächste Hebel, nicht gebaut: das Verfeinern selbst (70 ns
  je Objekt, ein Hash-Lookup je Objekt; ein Merker "gleiches Mesh wie das Objekt davor" ließe die meisten
  entfallen), die Culls aller Kaskaden in einem Lauf über die Objekte, und der Walk (`extractMeshes`, 280 Byte je
  `RenderObject`).
- **D3D11, D3D12, OpenGL** verfeinern weiter je Pass; da sie nur einmal je Frame extrahieren, ist das ein Lauf je Frame
  und kein Fund.
- Vorbestehend, nicht angefasst: der Kommentar in Vulkans `runGi` ("this extraction throws away DrawScene's resolve")
  beschreibt den Zustand vor Schritt 2; die Auflösung der Materialwerte ist idempotent, es bricht nichts.

---

## 9. Schritt 5: Verifikation, Messleiter vorher/nachher (11.10.2026)

Kein Produktcode geändert. Gemessen ist **Metal**; Vulkan, D3D11, D3D12 und OpenGL sind nur über die CI belegt (9.6).
Rohdaten, Tabellen und Bedingungen: `docs/perf-audit/raw-t162s5/` (Zusammenfassungen, `conditions.txt`,
`ab-table.md`, `ladder-table.md`; die Profiler-Dumps selbst bleiben lokal, `.gitignore`).

### 9.1 Was verglichen wurde

| | Stand | Bau |
|---|---|---|
| **old** | `25e8915d`, der letzte Merge vor Schritt 2 (Thema 153 komplett, kein Thema 162) | `src/HE_Rendering` und `src/HE_Scene` aus diesem Commit in den Baum gelegt, `HorizonEditor` gebaut, Deploy als selbstständige Kopie gesichert (`selfcontain_deploy_copy.sh`), danach `git checkout HEAD -- src/HE_Rendering src/HE_Scene` (Arbeitsbaum wieder sauber) |
| **new** | `9606ecac`, Ende Schritt 4 | derselbe Baum, Deploy ebenfalls als Kopie |
| **full** | `9606ecac` mit `HE_PROPAGATE_FULL=1` | dieselbe Kopie wie **new**, nur der Schalter aus Schritt 3: trennt den Anteil von Schritt 3 vom Rest |

Dass die Kopien wirklich zwei Stände sind, belegt das Symbol: `Metal ExtractFrame` steht nur in der `libHorizonRendering.dylib`
von **new** (`nm`, 0 gegen 1), der Text `HE_PROPAGATE_FULL` nur in der `libHorizonScene.dylib` von **new**. Zwischen den beiden
Ständen liegt in `src/HE_Rendering` und `src/HE_Scene` nichts als die Dateien der Schritte 2 bis 4
(`git diff --stat 25e8915d HEAD` auf diese Ordner: 12 Dateien, alle von Thema 162; der Merge `7cb9a18d` brachte dort nichts. Der Rest des Baums,
`src/HE_Editor` mit dem `.heproj`-Code aus dem Merge, ist in beiden Ständen derselbe).

Werkzeug wie in Thema 153: `scripts/perf/world_streaming_ladder.sh` (`FRAMES=120`, `WARMUP=0`, `--no-counters`, Kamera
`0,25,90,0,-0.25`, Referenzwelt aus `gen_reference_world.py` mit 64 Lichtern) über ein **Scratch-Projekt**, kopiert aus
`~/HorizonEngineProjects/Test` nach `/tmp` (kein Projekt des Menschen angefasst). Aufruf je Binary:
`EDITOR=<Kopie>/HorizonEditor FRAMES=120 scripts/perf/world_streaming_ladder.sh <Scratch>/Test.heproj <out> <Präfix> 1000 10000 50000 100000 200000`.

Reihenfolge, damit Drift sichtbar wird: Runde 1 alt, neu; Runde 2 neu, alt; Runde 3 full, neu, **alt zuletzt**; Runde 4 nur 50k, neu, alt.
Auswertung: `scripts/perf/ladder_ab_table.py <raw> old=r1old,r2old,r3old new=r1new,r2new,r3new full=r3full`.

**Bedingungen, ehrlich** (je Lauf in `conditions.txt`): Bildschirm **gesperrt** (`CGSSessionScreenIsLocked=Yes`), **Akkubetrieb** (57 %,
`lowpowermode 0`, keine Thermik-Warnung), Last 1,9 bis 3,8, Fremdlast (die drei größten Prozesse vor jedem Lauf): CLion 8 bis 15 %,
das Hive-Dashboard (Python) 8 bis 23 %, einmal die Hive-App mit 44 %, Dock 14 bis 16 %, die Claude-Sitzung 8 bis 17 %; GPU-Auslastung 19 bis 53 % (jeweils
unmittelbar nach dem vorigen Lauf gelesen, also zum Teil der Editor selbst), keine zweite Bee auf dem Mac. Entsperrt und am Netz war von hier aus nicht herzustellen
(dieselben Bedingungen wie Schritt 2 bis 4, der Queen vorab gemeldet). Das Fenster ist **nicht** versteckt (Standard der Leiter;
2840 x 1528), die Vergleiche mit der Zähler-Tabelle aus 8.5 (versteckt) sind darum nur Größenordnungen. **Absolute Werte sind nicht
mit 1 (s6end) vergleichbar**: der Vorher-Stand ist hier `25e8915d` und nicht `s6end`, und die Sitzung ist eine andere (bei 50k
70,7 ms hier gegen 62,4 ms dort, die 8 ms sind nicht untersucht). Belastbar ist nur **alt gegen neu in dieser Sitzung**.

### 9.2 CPU je Frame, p50 (ms)

Mittel über drei Läufe je Stand (kleinster bis größter Lauf), Frames ab 2. Rohwerte je Lauf in `ladder-table.md`.

| Entities | old | new | Δ new | full (= new ohne Schritt 3) |
|---|---|---|---|---|
| 1 084 | 12,9 (11,2 bis 14,7) | 13,3 (13,0 bis 13,8) | +0,4 ms, **im Rauschen** | 14,4 |
| 10 174 | 14,5 (14,2 bis 14,8) | 9,6 (9,4 bis 9,9) | −4,9 ms (−34 %) | 12,2 |
| 50 574 | 70,7 (70,5 bis 70,9) | 43,5 (43,1 bis 43,8) | **−27,3 ms (−39 %)** | 56,1 |
| 101 074 | 142,5 (140,7 bis 143,9) | 89,2 (88,2 bis 90,5) | −53,2 ms (−37 %) | 111,8 |
| 202 074 | 291,6 (289,0 bis 295,4) | 182,5 (178,5 bis 187,1) | −109,1 ms (−37 %) | 233,4 |

- **Streuung und Drift**: bei 50k vier Läufe je Stand, old 70,5 / 70,1 / 70,9 / 71,2 (die Läufe 3 und 4 sind die jeweils letzten ihrer
  Runde), new 43,4 / 43,1 / 43,8 / 43,1. Der Abstand zwischen den Ständen (27 ms) ist mehr als das Zwanzigfache der Streuung innerhalb eines
  Standes (bis 1,1 ms über vier Läufe); die alte Binary am Ende der Runden liegt wieder bei 70,9 und 71,2, es gibt keinen Drift der Sitzung.
- **Zerlegung bei 50k**: 70,7 → 56,1 (**−14,6 ms**, Schritt 2 und 4: keine Kopien des `RenderWorld` mehr, Verfeinern einmal je Walk,
  Schlüsselvergleich im Schattenpass) → 43,5 (**−12,6 ms**, Schritt 3: `Transforms::propagate` 14,2 → 1,4 ms). `full` ist nur die
  Näherung "Schritt 3 aus": der Schalter lässt den alten Walk im neuen Binary laufen, nicht den alten Quelltext.
- **FPS bei 50k** (Leiter, VSync aus, gesperrter Bildschirm): 11,5 → 16,3. **GPU-Zeit je Frame unverändert** (p50 10,6 bis 11,2 ms alt,
  10,1 bis 10,6 ms neu; die Pässe kodieren dieselben Draws): der Gewinn ist reine CPU-Zeit, und bei 50k und mehr ist die CPU die Grenze.
- **p99 (ab Frame 2)** bei 50k: old 85,8 bis 98,3, new 69,4 bis 86,0 ms. Die Hänger des gesperrten Bildschirms bleiben (jeweils
  Einzelframes über dem Doppelten des Medians: 1 bis 2 je Lauf). Bei 1k sind es 46 bis 51 solche Frames in beiden Ständen, bei 10k 2 bis 5 (old) und
  9 bis 10 (new): der Median ist dort kleiner (9,6 gegen 14,5 ms), die Hänger des gesperrten Bildschirms nicht.
- **Laden unverändert** (Parse, Aufbau, Laden): 50k `loadMs` 1035 / 1049 (old) gegen 1065 / 1016 (new); RSS max bei 50k 757 bis 764 MB
  gegen 743 bis 755 MB. Schritt 5 prüft nur den Frame, die Ladezeit hat kein Schritt dieses Themas angefasst.

### 9.3 Wohin die Zeit ging (p50 je Frame, ms, Mittel der Läufe)

Beachte 8.5: der Walk steht jetzt in `Metal::ExtractFrame`, vorher im `Metal::EncodeShadowMap`. Verglichen werden darum nur Summen:
"Schatten samt Walk" ist beim alten Stand `EncodeShadowMap`, beim neuen `ExtractFrame` + `EncodeShadowMap`.

| Entities | `extract` (Summe je Frame) old → new | Schatten samt Walk old → new | `EncodeSSAO` old → new | `EncodeScene` old → new | `FrustumCuller::cull` old → new | `OnRender` old → new |
|---|---|---|---|---|---|---|
| 1 084 | 0,7 → 0,4 | 0,9 → 0,7 | 0,3 → 0,2 | 0,4 → 0,3 | 0,2 → 0,2 | 0,9 → 0,9 |
| 10 174 | 6,5 → 3,2 | 7,1 → 4,6 | 1,9 → 0,7 | 1,9 → 0,8 | 0,6 → 0,6 | 2,6 → 2,6 |
| 50 574 | 37,7 → 18,7 | 39,9 → 25,1 | 10,2 → 4,3 | 10,1 → 4,3 | 3,3 → 3,2 | 8,3 → 8,8 |
| 101 074 | 75,1 → 37,2 | 79,2 → 50,0 | 22,4 → 10,5 | 22,8 → 11,0 | 7,9 → 7,4 | 17,3 → 16,7 |
| 202 074 | 147,5 → 73,1 | 155,5 → 99,0 | 48,7 → 24,3 | 51,4 → 27,4 | 19,4 → 18,5 | 35,1 → 33,5 |

Aufrufe je Frame (Median über die Frames, Metal-Forward): `RenderExtractor::extract` / `reuse` **old (3, 2), new (4, 3)**, in allen fünf
Größen. Das ist die Erwartung aus 8.5/8.7: genau ein voller Walk je Frame, die drei weiteren Aufrufe (Schatten, SSAO, Szene) sind
Schlüsselvergleiche; ein zweiter Walk wäre (5, 3). `Metal::ExtractFrame` und `Metal::RefineBounds` je 1 (old: nicht vorhanden),
`FrustumCuller::cull` 5 in beiden Ständen (fünf Läufe, drei davon die Kaskaden, 8.7).

Verschachtelung (aus den Tiefen des Profils): `ExtractFrame` = `RenderExtractor::extract` (der Walk) + `RefineBounds`; `Transforms::scan` steckt in
`Transforms::propagate`, und das im Walk; `FrustumCuller::cull` steckt in den Pässen (Schatten, SSAO, Szene), die Spalte ist also keine
weitere Summe, sondern ein Teil der Pass-Spalten.

Der Rest des Frames bei 50k (new): Walk 18,7 (darin `Transforms::propagate` 1,4, darin der Scan 1,4), Verfeinern 3,5, SSAO 4,3, Szene 4,3,
Schattenpass selbst 2,8 (die Culls, zusammen 3,2, stecken darin). Gegen old: der Walk wird nicht mehr mit zwei Kopien des `RenderWorld` bezahlt
(alte `extract`-Summe 37,7), die Verfeinern-Schleife läuft einmal statt dreimal (SSAO und Szene sinken um je 5,9 ms), und `propagate` liest
nur noch die geänderten Teilbäume.

### 9.4 Ziel erreicht oder verfehlt

Das Ziel des Themas: *ein persistenter RenderWorld-Zustand, ein Extract pro Frame, Pässe lesen daraus, Transforms nur für geänderte
Teilbäume neu propagieren*; Anlass waren bei 50k ~33 ms `extract` und ~35 ms `EncodeShadowMap`, "der größte Posten im Frame".

- **Ein Extract pro Frame, Pässe lesen daraus: erreicht, auf Metal gemessen** ((4, 3) im Betrieb, Schattenpass ist ein Schlüsselvergleich,
  Pin gegen den zweiten Walk in `test_culling.cpp`). Ehrlich dazu: einen vollen Walk je Frame gab es auf Metal schon nach Thema 153
  ((3, 2) im alten Stand). Der Gewinn von Schritt 2 und 4 liegt in den **Kopien**, die weg sind, und im **dreifachen Verfeinern**,
  nicht in einem Walk weniger.
- **Transforms nur für geänderte Teilbäume: erreicht, aber nur für eine ruhende Welt gemessen.** `Transforms::propagate` bei 50k 14,2 → 1,4 ms
  (der Scan über 50 574 Entities kostet 1,4 ms, bei 200k 5,1). Die Referenzwelt bewegt nichts: das ist der günstigste Fall. Mit bewegten
  Entities wächst die Zeit mit der Zahl der geänderten Teilbäume (Messung in 7.7), und jede Strukturänderung im Frame (Spawn, Destroy,
  Reparent, Zellwechsel) kostet weiter einen vollen Walk (7.8).
- **Persistenter Zustand über Frames hinweg: nicht gebaut.** Der Walk (`extractMeshes`, 280 Byte je `RenderObject`) läuft jeden Frame
  voll und kostet bei 50k **18,7 ms, bei 200k 73,1 ms**. Er ist weiter der größte Einzelposten des Frames, und, ehrlich, **weiter größer als
  alle Encode-Pässe zusammen**: SSAO 4,3 + Szene 4,3 + Schattenpass 2,8 = 11,4 ms, mit dem Verfeinern (3,5) 14,9 ms. Das war der Anlass des
  Themas ("größer als alle Encode-Pässe zusammen"); der Abstand ist kleiner geworden (vorher 37,7 gegen rund 22 für SSAO 10,2, Szene 10,1 und den
  Schattenpass ohne Walk, jetzt 18,7 gegen 11,4), die Aussage gilt noch.
- **Zahlen gegen den Anlass**: `extract` 37,7 → 18,7 ms (in 1, `s6end`, unter anderen Bedingungen 33,4), Schatten samt Walk 39,9 → 25,1 ms,
  CPU je Frame **−39 %** bei 50k, −37 % bei 100k und 200k, −34 % bei 10k, nichts messbar bei 1k. **Halbiert, nicht beseitigt.**

### 9.5 Vollbau, Tests

- **Vollbau** (Release, shaderc ON, `ninja -j8`, alle Ziele): rc 0. Der Aufruf lief als Hintergrundbefehl und wurde nach 600 s vom
  Werkzeug-Zeitlimit beendet (Schritt 228 von 305, "interrupted by user"); ich habe ihn mit längerem Limit neu gestartet, ninja setzte fort,
  `BUILD_RC=0` gelesen, danach kein Schritt offen. Der Deploy (`out/deploy/Editor`) ist der Build-Baum (`cmp` auf `HorizonEditor` und
  `libHorizonRendering.dylib` byte-gleich, `ExtractFrame` im Symbol); `git diff HEAD -- src` leer.
- **ctest voll** im Vordergrund auf `9606ecac`: `ctest -j4 --timeout 1500 --output-on-failure`, frisches `HOME`, `HE_CONFIG_DIR` ungesetzt,
  04:08:29 bis 04:13:25: **259 von 259 grün, rc 0, 296,7 s**, 2 übersprungen (`runtime_size_app_advanced`, `runtime_size_app_basic`,
  für diese Plattform per Konstruktion). `docs/perf-audit/raw-t162s5/ctest-summary.txt`.
- **CI** auf `9606ecac` (Schritt 4), workflow_dispatch-Lauf
  [38101027036](https://github.com/Horizon-Creations/HorizonEngine/actions/runs/38101027036): **Linux, Windows, macOS und
  Linux · Vulkan (lavapipe) alle grün**. Nichts von Schritt 4 war rot, es gab nichts zu beheben.

### 9.6 Was nur die CI belegt, was ungemessen ist

- **Vulkan**: übersetzt unter MSVC und GCC (Windows- und Linux-Job), der Bild-A/B läuft über lavapipe (grün). **Die CPU-Zeit ist auf Vulkan
  nicht gemessen** und auf Hardware lief Vulkan nie; der Gewinn (Kopien, Verfeinern, `propagate`) ist derselbe Quelltext, die Größe
  ist für Vulkan eine Annahme.
- **D3D11, D3D12, OpenGL**: Schritt 2 bis 4 haben dort keinen Pass umgebaut (ein `extract` je Frame von Anfang an). Der Gewinn aus Schritt 3
  (`propagateTransforms`) gilt für alle Backends, weil er im gemeinsamen `HE_Scene` steckt; **gemessen ist er nur auf Metal**.
- **Nicht in der Leiter**: bewegte Entities, GI, Wolken mit halber Auflösung, Skinned, Partikel, Trails, echte Spielinhalte, Play-Modus,
  ein offenes zweites Scene-Fenster (`sceneSnapshot` extrahiert dort weiter jeden Frame voll, 2.4).
- **Entsperrter Bildschirm, Netzbetrieb**: nicht gemessen (9.1). Die Differenz alt gegen neu ist unter diesen Bedingungen belegt; ob
  sie unter anderen genauso groß ist, ist eine Annahme (die CPU-Scopes sind von Sperre und Akku weniger abhängig als FPS und GPU).

### 9.7 Offen, nächste Hebel (nicht gebaut)

Gemessen bei 50k (new): der Walk 18,7 ms ist die Grenze nach unten, solange er jeden Frame voll läuft. Hebel in der Reihenfolge ihres
Gewichts: (1) der Walk selbst, nur geänderte Entities neu schreiben (braucht einen Änderungszähler je Entity, den der Scan aus Schritt 3
schon fast liefert), (2) das Verfeinern, 3,5 ms (70 ns je Objekt, ein Hash-Lookup; ein Merker "gleiches Mesh wie das Vorherige"),
(3) die Culls aller Kaskaden in einem Lauf (3,2 ms). Alle drei wachsen linear mit der Entity-Zahl (200k: 73 / 14 / 18 ms).

### 9.8 Änderungen an den Werkzeugen dieses Schritts

- `scripts/perf/ladder_ab_table.py` (neu): die Tabellen aus 9.2 und 9.3 und die Aufrufzähler aus mehreren Läufen je Stand.
- `scripts/perf/ladder_table.py`: ein Scope, den kein Frame des Laufs enthält, druckt `-` statt `0.0` (die Scopes aus Thema 162 gibt es im alten
  Stand nicht); die Spalte `FrustumCull` hieß falsch und war in jeder alten Tabelle 0,0, sie heißt jetzt `FrustumCuller::cull` (3,2 ms bei 50k,
  die alten `s5end`/`s6end`-Tabellen haben dort einen Artefaktwert); `Transforms::scan`/`::propagate` neu; `entities` kommt vom letzten Frame
  (mit `WARMUP=0` zeigen die ersten Frames noch die leere Szene mit 1 Entity).
