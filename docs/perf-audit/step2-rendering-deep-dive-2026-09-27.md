# Performance-Audit Schritt 2: NextDrawable und die teuersten GPU-Pässe (Befund)

Thema 99, Schritt 2. Stand 27.09.2026, Zweig `claude/performance-audit-editor-runtime-auf-m5-macbook-landscape-hi`.
Grundlage sind die Rohdaten aus Schritt 1 (`docs/perf-audit/baseline-2026-09-27.md`) und eigene Kontrollmessungen
von heute Nachmittag (`docs/perf-audit/raw-step2/`). **Nur Befund, kein Fix.**

> **Vorbehalt, wie in Schritt 1:** Der verwaiste Test-Editor pid 72986 lief auch während *aller* Messungen dieses
> Schritts (Start Do 24.09. 19:15, 49 % CPU). Die GPU stand ohne meine Läufe bei 89–94 % „Device Utilization“.
> Dieser Schritt misst seinen Anteil jetzt direkt pro Prozess (Abschnitt 1.3) und zeigt, dass er die
> NextDrawable-Befunde der Baseline weitgehend erklärt.

## Kurzfassung

1. **NextDrawable ist keine Engine-Arbeit, sondern Gegendruck.** Ein minimaler Metal-Presenter *ohne Engine*
   (gleiche Layer-Einstellungen, gleiche Fenstergröße, 6 ms synthetische GPU-Last) zeigt unter denselben
   Bedingungen dieselben Werte wie der Editor: 44–60 FPS, NextDrawable p50 16,5–20,7 ms. Die Layer-Konfiguration
   der Engine ist Standard und nicht die Ursache.
2. **Die GPU wird geteilt.** Während der Editor die Landscape-Szene rendert, teilt sich die GPU (per-Prozess-GPU-Zeit
   aus dem IORegistry) in **Editor 46 %, verwaister Editor 35 %, WindowServer 18 %** auf. Im Leerlauf nimmt sich der
   Orphan allein 54 %. Zieht er mehr, verpasst das Fenster Refreshes, und es entsteht das Muster aus der
   Baseline (NextDrawable abwechselnd ~0 und 30–60 ms, 44–51 FPS).
3. **Wenn die GPU reicht, ist NextDrawable reine vsync-Wartezeit.** Heute Nachmittag schaffte derselbe Editor mit
   derselben Landscape-Szene unter derselben Fremdlast **59,5–59,6 FPS**. NextDrawable lag dabei gleichmäßig bei ~15 ms:
   Die CPU wartet auf den nächsten 60-Hz-Takt des Compositors, sie verliert dabei nichts.
   „NextDrawable p50 17 ms“ ist also für sich kein Kostenposten.
4. **Echter Engine-Befund dahinter:** Ein Editor mit verstecktem Fenster drosselt nicht. pid 72986 hat in
   2 Tagen 19 Stunden **93 217 s GPU-Zeit** verbraucht (im Mittel ~39 % der GPU) und nimmt jedem anderen
   Renderer auf dem Gerät ein Drittel bis die Hälfte der GPU weg. `Application::Run` kennt keinen Zustand für
   verstecktes, verdecktes oder minimiertes Fenster (Abschnitt 1.5).
5. **Die GPU-Arbeit der Szene selbst ist klein:** Die Summe der Pass-Minima liegt bei ~4,4–6 ms für 1718×884 Pixel,
   ein Viertel bis ein Drittel des 60-Hz-Budgets. Die teuersten Posten (Minima, Nachmittags-/Mittagsserie):
   **Wolken-Dome-Raymarch 1,4/2,2 ms** (32 Schritte, 224–544 3D-Fetches pro Himmelspixel auf eine 64-MiB-Textur
   ohne Mips; die Coverage-Schranke, die das sparen soll, greift bei `coverage 0.5` nie), **Himmel ohne Wolken ~1,2 ms** (Single-Scattering mit ~216 `exp` pro Pixel und Frame bei stehender
   Sonne), **Wolkenschatten-Map 0,6/1,0 ms** (512², rechnet das 3D-Wolkenfeld für 12 × 12 km, das Terrain nutzt davon
   ~4×4 Texel), Bloom 0,35/0,62 ms (11 Encoder, größte Streuung unter Last). Details in Abschnitt 2.

## 1. Hypothese zuerst: Warum blockiert `Metal::NextDrawable`?

### 1.1 Layer-Konfiguration (Code-Befund)

`MetalRenderer::CreateTarget` (`src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm:10168`):

| Eigenschaft | Wert in der Engine | Bewertung |
|---|---|---|
| `maximumDrawableCount` | nicht gesetzt → Standard **3** | normal |
| `displaySyncEnabled` | = vsync-Einstellung | normal. Im Fenstermodus komponiert der WindowServer trotzdem im 60-Hz-Takt; „vsync aus“ hebt die Taktung nur teilweise auf (siehe 1.2) |
| `presentsWithTransaction` | nicht gesetzt → NO | normal |
| `framebufferOnly` | **NO** (bewusst, für den Backdrop-Blit, Kommentar Z. 10183) | einzige Abweichung vom Standard; Effekt unter dieser Last nicht messbar (1.4) |
| `opaque` | YES | günstig für den Compositor |
| `drawableSize` | Fensterpixel **2840×1528**, jeden Frame abgeglichen (Z. 15361) | Das ganze Editorfenster ist das Drawable; die Szene selbst rendert offscreen in 1718×884 |

Frame-Ablauf (`EncodeFrame`, Z. 15268–16175): **ein** Command-Buffer pro Frame. Alle Offscreen-Pässe werden encodiert,
**erst danach** kommt `[layer nextDrawable]` (Z. 16034), dann Present-Pass (Clear, ImGui), `presentDrawable`, `commit`.
Es gibt keinen Frames-in-flight-Semaphor. Die CPU läuft also so weit vor, bis alle drei Drawables vergeben sind.
Genau an dieser Stelle wartet sie, und das ist das von Apple empfohlene Muster. `Metal::NextDrawable` ist damit
**der einzige Punkt, an dem Gegendruck von GPU oder Compositor auf der CPU sichtbar wird**: Jede Ursache, die ein
Drawable länger festhält (GPU noch nicht fertig, Compositor hat noch nicht übernommen, vsync), landet in diesem Scope.

### 1.2 Was die Baseline-Frames zeigen (Nachauswertung der vollständigen Dumps)

Frame für Frame (`L1-landscape-vsyncoff-run1`, Frames 100–163, Format delta/NextDrawable/GPU-Spanne in ms):

```
100   2.4/28.2/28.9   30.5/ 0.0/28.9    2.7/58.0/36.2   60.4/ 0.0/36.2    2.5/24.3/32.2   26.3/ 0.0/32.2 ...
```

- NextDrawable wechselt Frame für Frame zwischen **~0 ms** (244 von 600 Frames < 1 ms) und **20–60 ms**
  (286 Frames ≥ 20 ms). Drawables kommen also **paarweise** zurück, wenn der Compositor einen Refresh verpasst hat.
- Auch mit „vsync aus“ liegen die Frame-Abstände auf Vielfachen von 16,67 ms (L6 vsync aus: 274× ≈ 0, 133× ≈ 1,
  185× ≈ 2 Intervalle). Im Fenstermodus takten Compositor-Refreshes das Freiwerden der Drawables.
- Die **„GPU-Spanne“ aus Tabelle A (33–45 ms p50) sind keine GPU-Kosten.** Die Spanne GPUStartTime→GPUEndTime eines
  Command-Buffers ist länger als der Frame-Abstand (21 ms). Aufeinanderfolgende Command-Buffer überlappen also, und
  die Spanne enthält die Zeit, in der die GPU fremde Arbeit ausführt.
- **Die Wartezeit skaliert nicht mit der eigenen GPU-Arbeit.** Ohne Wolken (Detailed L5: GPU-Summe p50 3,3 ms, eng
  am Minimum) bleibt NextDrawable im Normallauf L6 bei p50 10,0 ms (65 FPS). Mit halber Auflösung (Detailed L8:
  Scene 1,1 ms min) liegt er im Normallauf bei p50 8,4 / p90 52 ms (51 FPS).

### 1.3 Kontrollmessung 1: minimaler Presenter ohne Engine

`scripts/perf/metal_present_probe.m` (neu, ~250 Zeilen ObjC): gleiches Drawable (2840×1528, BGRA8, `opaque=YES`,
`framebufferOnly=NO`, 3 Drawables), ein Command-Buffer pro Frame mit Offscreen-Pass in RGBA16F 1718×884,
spätes `nextDrawable`, Fullscreen-Pass ins Drawable, `presentDrawable`, `commit`. Die synthetische Last ist
eine Fragment-Schleife. `--load 140` kostet **~6 ms GPU-Zeit ohne Konkurrenz** (Kalibrierung mit
`--calibrate`, min über 120 Frames: load 150 → 6,47 ms). Das entspricht der Summe der Pass-Minima des Editors.
Unter der Fremdlast bekommt die Probe etwa die Hälfte der GPU (GPU-Spanne p50 ≈ 2× min).

Je 300 Frames Warmup und 600 Frames Aufnahme, Display per `caffeinate -u` wach, gleiche Fremdlast. Alle Läufe in
`docs/perf-audit/raw-step2/probe-*.json` (mit Frame-Arrays).

| Lauf | Last | vsync | fbOnly | Drawables | FPS | delta p50 / p95 | NextDrawable p50 / p90 | ND < 1 ms | GPU-Spanne min / p50 |
|---|---|---|---|---|---|---|---|---|---|
| P1 | 0 | aus | NO | 3 | 116.4 | 7.9 / 17.9 | 7.8 / 16.6 | 141 | 0.1 / 2.7 |
| P2 | 0 | an | NO | 3 | **50.4** | 16.7 / 33.7 | 16.5 / 33.1 | 6 | 0.2 / 3.8 |
| P2b | 0 | an | NO | 3 | 60.0 | 16.7 / 17.1 | 16.4 / 16.6 | 20 | 0.1 / 0.5 |
| P2c | 0 | an | NO | 3 | 53.9 | 16.7 / 46.5 | 16.5 / 33.0 | 63 | 0.1 / 0.4 |
| P3 | 140 | aus | NO | 3 | **44.3** | 21.1 / 44.2 | **20.7** / 34.6 | 26 | 9.0 / 35.9 |
| P3b | 140 | aus | NO | 3 | 56.9 | 16.7 / 17.8 | 16.5 / 17.0 | 13 | 4.3 / 16.3 |
| P3c | 140 | aus | NO | 3 | 59.9 | 16.6 / 17.4 | 16.5 / 16.8 | 3 | 8.2 / 16.7 |
| P4 | 140 | an | NO | 3 | 45.0 | 17.4 / 49.4 | 16.9 / 33.7 | 119 | 8.7 / 23.0 |
| P5 | 140 | aus | YES | 3 | 55.5 | 16.7 / 17.9 | 16.6 / 17.0 | 3 | 4.7 / 16.3 |
| P5b | 140 | aus | YES | 3 | 55.2 | 13.7 / 51.6 | 13.4 / 36.7 | 250 | 4.2 / 8.6 |
| P5c | 140 | aus | YES | 3 | 60.0 | 16.7 / 17.1 | 16.5 / 16.7 | 2 | 4.4 / 16.7 |
| P6 | 140 | aus | NO | 2 | 84.4 | 13.1 / 21.8 | 12.9 / 19.7 | 21 | 5.4 / 13.3 |
| P6b | 140 | aus | NO | 2 | 36.6 | 29.0 / 40.5 | 28.8 / 34.6 | 4 | 4.2 / 5.0 |
| P6c | 140 | aus | NO | 2 | 72.5 | 13.8 / 25.4 | 13.6 / 22.6 | 9 | 4.3 / 11.9 |

Deutung:
- **Die Probe ohne Engine reproduziert das Editor-Bild** (P3: 44 FPS, NextDrawable p50 20,7 ms; Baseline L1:
  46 FPS, p50 17 ms). Sogar reines Clear mit vsync schafft in zwei von drei Läufen keine 60 FPS (P2, P2c).
- Dieselbe Konfiguration streut zwischen 44 und 60 FPS (P3/P3b/P3c). Die Streuung kommt von außen, nicht aus der
  Konfiguration. `framebufferOnly` (P5*) und `maximumDrawableCount=2` (P6*) lassen sich unter dieser Last **nicht**
  von der Streuung trennen, eine Aussage dazu wäre geraten.

### 1.4 Kontrollmessung 2: Editor und Probe abwechselnd, GPU-Zeit pro Prozess

Editor-Landscape (Harness aus Schritt 1, vsync aus) und Probe (`--load 140`) direkt hintereinander:

| Lauf | FPS | delta p50 / p95 | NextDrawable Ø | Render Ø (inkl. ND) |
|---|---|---|---|---|
| X1 Editor Landscape | **59.6** | 16.67 / 17.21 | 15.15 | 15.93 |
| X2 Probe load 140 | 58.1 | 16.7 / 23.4 | 16.5 (p50) | – |
| X3 Editor Landscape | **59.5** | 16.67 / 17.18 | 14.82 | 15.84 |
| X4 Probe load 140 | 60.0 | 16.7 / 17.0 | 16.5 (p50) | – |
| X5 Editor Landscape (2400 Frames, GPU-Anteile gemessen) | 58.6 | 16.66 / 17.74 | 15.54 | 16.33 |

Derselbe Editor, der mittags 44–47 FPS lieferte (L1/L2), liefert jetzt 59,5 FPS. NextDrawable ist dabei 15 ms im Mittel,
**eng verteilt** (X1: 581 von 600 Frames zwischen 8 und 20 ms), also vsync-Takt. Die CPU-Arbeit ohne NextDrawable
ist unverändert ~1,7–2,4 ms.

**GPU-Zeit pro Prozess** (`scripts/perf/gpu_time_by_process.py`, neu: Differenz von `AppUsage.accumulatedGPUTime`
des AGX-Treibers im IORegistry über 10–15 s, ohne sudo; Summen > 100 % sind möglich, weil Vertex/Fragment/Compute
verschiedener Clients gleichzeitig laufen):

| Zustand | pid 72986 (verwaister Editor) | messender Prozess | WindowServer | Rohdaten |
|---|---|---|---|---|
| Leerlauf (kein Lauf von mir) | **53.8 %** | – | < 0.1 % | `gpu-by-process-idle.json` |
| Editor Landscape X5, 58,6 FPS | 35.2 % | Editor **46.1 %** | 18.0 % | `gpu-by-process-editor-landscape.json` |
| Probe load 140, 60,2 FPS (W1) | 35.7 % | Probe 40.2 % | 23.3 % | `gpu-by-process-W1-load140-fbonly0.json` |
| Probe load 140, 53,9 FPS (W2) | 42.6 % | Probe 33.3 % | 0.1 % | `gpu-by-process-W2-load140-fbonly0.json` |
| Probe Clear, vsync, 42,2 FPS (W1) | 49.3 % | Probe 4.4 % | 3.1 % | `gpu-by-process-W1-clear-fbonly1.json` |

- Der verwaiste Editor nimmt sich **35–54 %** der GPU. Holt er sich mehr (W1-clear: 49 %, W2: 43 %), fallen die
  FPS des Vordergrund-Renderers, und zwar auch bei einer Probe, die fast nichts rendert.
- Die Summen um ~99 % (X5, W1-load) sprechen für eine volle GPU, beweisen sie aber nicht: Anteile verschiedener
  Clients können sich überlappen. Der tragende Beleg ist ein anderer, nämlich dass die Probe ohne Engine dieselben
  Einbrüche zeigt und diese mit dem Anteil des Orphans schwanken.
- 46 % GPU-Anteil bei 58,6 FPS heißt: Der Editor braucht unter Konkurrenz ~7,9 ms GPU-Zeit pro Frame (Minimum ohne
  Konkurrenz aus der Detailed-Capture ~6 ms). Das passt in 16,7 ms, **wenn** der Rest der GPU frei ist.
- Der WindowServer-Anteil springt zwischen 0,1 % und 44 %, ohne erkennbaren Zusammenhang mit `framebufferOnly`
  (W1/W2 mit fbOnly YES/NO, alle Paare in `raw-step2/`). Wahrscheinlich bucht der Treiber Compositor-Arbeit
  schubweise. Ein Beleg, dass `framebufferOnly=NO` den Compositor merklich belastet, ergibt sich daraus nicht.

### 1.5 Engine-Befund: Ein verstecktes Editorfenster rendert ungebremst

- pid 72986 läuft mit `HE_HIDDEN_WINDOW=1` (Fenster mit `SDL_WINDOW_HIDDEN`, `src/HE_Core/src/Window/Window.cpp:125`),
  ohne Interaktion. IORegistry: `accumulatedGPUTime = 93 217 086 179 750 ns` ≈ **93 217 s GPU-Zeit** in
  2 Tagen 19 Stunden (~241 000 s Laufzeit) → **im Mittel ~39 % der GPU**, dazu 49 % CPU.
- `Application::Run` (`src/HE_Core/src/Application/Application.cpp:511–577`) rendert und präsentiert jeden Frame.
  Überspringen kann er nur im **event-getriebenen Modus** (`m_eventDriven`), und den schaltet nur `GameApplication`
  für HE-Apps ein (`src/HE_Game/src/GameApplication.cpp:653`). Einen Zweig für versteckte, verdeckte oder
  minimierte Fenster (`SDL_WINDOW_HIDDEN/OCCLUDED/MINIMIZED`, `SDL_EVENT_WINDOW_OCCLUDED`) gibt es außerhalb von
  `vendor/` nirgends.
  Ein Frame-Limit greift nur bei vsync aus und `MaxFps > 0` (Standard 0, Z. 641–651).
- **Gemessen mit der Probe** (`--hidden`: Fenster nie gezeigt, wie `SDL_WINDOW_HIDDEN`; `probe-H*.json`):
  Ein verstecktes Fenster bekommt **mit vsync an 72–77 FPS** (H1 Clear 72,1; H2 6-ms-Last 73,0; H3 vsync aus 76,6).
  Sichtbar schafft dieselbe Probe maximal 60. NextDrawable liefert dabei nie nil (0 Timeouts). Ein verstecktes Fenster
  ist also **nicht an den Displaytakt gebunden**. Unbegrenzt ist es hier nicht, die Obergrenze setzt vermutlich die
  GPU-Konkurrenz mit pid 72986. Der verwaiste Editor rendert so jedes Bild voll, samt Wolken (`HE_SKY_TIME=30`),
  und zwar öfter als 60-mal pro Sekunde.
- Das betrifft nicht nur verwaiste Testläufe. Es betrifft jede Situation, in der ein Engine-Fenster im Hintergrund
  läuft (Editor minimiert während eines Spiel-Exports, zweiter Editor im Collab-Test, Spiel hinter dem Editor).
  Für den Menschen am M5 heißt das: **Ein zweiter HorizonEditor/HorizonGame-Prozess im Hintergrund kann dem
  Vordergrund-Fenster ein Drittel bis die Hälfte der GPU nehmen** (gemessen am Orphan: 35–54 %).

### 1.6 Folgerungen für NextDrawable (priorisierte Verdächtige)

| # | Verdächtiger | Beleg | Stärke |
|---|---|---|---|
| 1 | Fremdlast auf der GPU durch den verwaisten Editor (und allgemein: ungedrosselte Hintergrund-Engine-Fenster) | 35–54 % GPU-Anteil pro Prozess; Probe ohne Engine zeigt dieselben FPS-Einbrüche; Editor erreicht 59,5 FPS, sobald die Fremdlast gerade weniger zieht | **stark** |
| 2 | Compositor-Takt im Fenstermodus (Drawables werden erst mit dem nächsten Refresh frei) | Frame-Abstände auf Vielfachen von 16,67 ms auch bei vsync aus; paarweises Freiwerden | stark, aber **erwartetes macOS-Verhalten**, kein Fehler |
| 3 | `framebufferOnly = NO` (mögliche Zusatzkopie im Compositor bei 2840×1528) | P5*/W*: kein trennbarer Effekt unter der Streuung | offen, erst ohne Fremdlast messbar |
| 4 | Drawable-Anzahl / fehlender Frames-in-flight-Semaphor | Standardwerte; P6* streut zu stark | schwach |
| 5 | Stage-Boundary-Counter-Sampling in *allen* „normalen“ Baseline-Läufen | Jeder Aufnahme-Frame legt einen neuen `MTLCounterSampleBuffer` an (Z. 15452) und sampelt an Encoder-Grenzen (599/600 Frames `gpuMode: counter`). Auf TBDR kann das Überlappung zwischen Encodern verhindern. Ein Schalter zum Abstellen fehlt; der Effekt ist nicht gemessen | **Messvorbehalt** für Tabelle A |

**Die Baseline-FPS von 44–51 sind damit kein belastbares Maß für die Engine auf diesem Gerät.** Eine saubere Messung
braucht: pid 72986 beendet, Stromsparmodus-Zustand notiert, dann `he_perf_capture.py` (Editor) und
`metal_present_probe` (Kontrolle) im Wechsel. Befehle in Abschnitt 4.

## 2. Die teuersten GPU-Pässe (Scene/Wolken, Shadow/Wolkenschatten, Bloom)

### 2.1 Methode

- **Minima der Detailed-Capture** (exklusive Pass-Zeit, ein Command-Buffer pro Pass, 600 Frames). Unter Fremdlast sind
  p50/Mittel um das 3- bis 15-Fache aufgebläht; das Minimum kommt der unbelasteten Zeit am nächsten.
- **Szenenvarianten** statt Pass-Grenzen, weil Himmel, Wolken und Terrain in einem Encoder liegen und die GPU keine
  Draw-Boundary-Counter kann. Neu in diesem Schritt: `cloudQuality` 0/2, `lowResClouds` an, `cloudShadows` aus
  (Läufe Y0–Y5, heute Nachmittag direkt hintereinander, `docs/perf-audit/raw-step2/Y*.summary.json`).
- **Fetch- und Schrittzahlen aus dem Shader-Code gezählt** (`kSkyMSL`/`kSkyFuncMSL` in `MetalRenderer.mm`).
  ISA-Instruktionszahlen und Occupancy ließen sich **nicht** messen: `xcrun metal` scheitert mit
  „missing Metal Toolchain“ (separater Download, ~688 MB, `xcodebuild -downloadComponent MetalToolchain`, nicht
  ohne Rückfrage installiert), und Occupancy/Limiter gibt es nur in einer Xcode-GPU-Aufnahme (GUI).

### 2.2 Messwerte: exklusive GPU-Zeit pro Pass, ms, min / Mittel (Landscape-Kamera, 1718×884)

| Lauf | Shadow | SSAO | Scene | Bloom | Tonemap | Present | gpuMs p50 |
|---|---|---|---|---|---|---|---|
| Y5 ohne Wolken | 0.16 / 1.37 | 0.21 / 1.61 | **1.20** / 2.07 | 0.34 / 2.75 | 0.12 / 0.73 | 0.24 / 0.98 | 9.45 |
| Y1 `cloudQuality 0` | 0.74 / 2.86 | 0.21 / 1.23 | **1.95** / 2.54 | 0.35 / 2.94 | 0.12 / 0.54 | 0.24 / 0.68 | 10.86 |
| Y0 Szene wie gespeichert (`cloudQuality 1`) | **0.73** / 2.72 | 0.00¹ / 2.50 | **2.57** / 5.25 | 0.35 / 4.51 | 0.12 / 1.39 | 0.24 / 1.39 | 17.16 |
| Y2 `cloudQuality 2` | 0.79 / 3.25 | 0.22 / 3.51 | **4.42** / 8.62 | 0.36 / 5.93 | 0.13 / 1.89 | 0.25 / 1.39 | 25.69 |
| Y3 `lowResClouds` an | 0.74 / 3.22 | 0.21 / 1.42 | **1.95** / 3.23 | 0.34 / 3.41 | 0.12 / 0.80 | 0.24 / 0.81 | 12.52 |
| Y4 `cloudShadows` aus | **0.16** / 1.48 | 0.21 / 1.64 | 2.66 / 3.65 | 0.35 / 3.11 | 0.12 / 0.66 | 0.24 / 0.79 | 11.40 |

¹ Einzelner Ausreißer-Frame mit 0; die übrigen Läufe zeigen 0,21 ms.

Die Minima liegen heute Nachmittag niedriger als mittags in der Baseline (Scene 2,57 statt 3,41–3,55 ms, Bloom 0,35
statt 0,62 ms). Auch das Minimum ist also noch lastabhängig. Die **Differenzen innerhalb einer Serie** sind belastbarer als die
absoluten Werte.

Daraus, pro Posten (Y-Serie / Baseline-Serie L3−L5):

| Rang | Posten | GPU-Zeit (min) | Anteil an ~4,4 ms Summe (Y0) |
|---|---|---|---|
| 1 | **Wolken, Dome-Raymarch** im Scene-Pass (`applyClouds`, `cloudQuality 1`) | **1,37 ms** / 2,21 ms | ~31 % |
| 2 | **Himmel ohne Wolken** (Atmosphäre `atmoScatter` + Rest von `skyFragment`) | **~1,1–1,3 ms** (Scene ohne Wolken 1,20; nur Himmel ohne Wolken 1,33) | ~27 % |
| 3 | **Wolkenschatten-Map** (`cloudShadowFragment`, 512²) | **0,57 ms** / 1,04 ms | ~13 % |
| 4 | Bloom (Bright + 10 Blur-Pässe) | 0,35 ms / 0,62 ms, **Mittel 2,8–5,9 ms** | ~8 % (min) |
| 5 | SSAO | 0,21 ms / 0,33 ms | ~5 % |
| – | Terrain-CSM 0,16, Tonemap+FXAA 0,12, Present (Drawable-Clear + ImGui, 2840×1528) 0,24 | | |

**Zusammen ~4,4–6 ms für ein 1718×884-Bild.** Selbst bei 60 Hz ist das nur ein Viertel bis ein Drittel des
Frame-Budgets. Diese Szene ist auf einer unbelasteten M5-GPU nicht GPU-limitiert. Das deckt sich mit Abschnitt 1:
Der Editor braucht unter Konkurrenz 46 % der GPU-Zeit.

### 2.3 Verdächtiger 1: Wolken-Raymarch (Dome-Pfad), Code-Befund

Die Szene hat `cloudMode 0`. `skyFragment` nimmt deshalb den Zweig `applyClouds` (Dome), nicht das 3D-Raymarching
(`MetalRenderer.mm:5470–5473`, Kopf von `applyClouds` ab Z. 3995).

- **Schrittzahl:** `N = clamp(qBaseN / dir.y, qBaseN, qMaxN)`, bei Qualität 1 `qBaseN = 12`, `qMaxN = 32`.
  Für jede Blickrichtung unter **dir.y < 0,375 (≈ 22° über dem Horizont)** ist N = **32**. Mit der Landscape-Kamera
  (Neigung −0,25 rad) gilt das für praktisch den ganzen sichtbaren Himmel. Die Schrittzahl ist gerade dort am höchsten,
  wo die Wolken ohnehin in den Horizontdunst ausgeblendet werden (`horizon = smoothstep(0.03, 0.22, dir.y)`).
  Unter dir.y < 0,02 bricht der Pfad ab.
- **Fetches pro Schritt** (alle auf die 3D-Rauschtextur, trilinear):
  - 4 für die Coverage-Schranke (`starFbm3`, 4 Oktaven),
  - +3, wenn die Schranke eine Wolke zulässt (`worleyFbm`),
  - +2 × 5 = 10, wenn Dichte > 0,001 (Licht-March `qShadow = 2` × `cloudShadowDensity` mit 3 + 2 Fetches).
  Die Slab-Prüfung (`hgrad <= 0`) spart im Dome-Pfad fast nichts: Der Slab liegt per Konstruktion ganz in `[s0, s1]`.
- **Die „exakte Coverage-Schranke“ greift bei dieser Szene nie.** `starFbm3` liegt in [0; 0,9375], also
  `perlin·0,5 + 0,55 ≥ 0,55`. Der Schwellwert ist `lo = mix(0.70, 0.22, coverage)` = **0,46** bei `coverage 0.5`.
  Die Bedingung `perlin*0.5 + 0.55 < lo` ist erst für `coverage < 0,3125` überhaupt erfüllbar. Der Kommentar dort
  schreibt der Schranke zu, dass sie jede Qualitätsstufe billiger macht. Bei der Standardbedeckung 0,5 spart sie
  nichts, jeder Schritt zahlt mindestens 7 Fetches.
- **Pro Himmelspixel damit 224 (klarer Himmel) bis 544 (dichte Wolke) 3D-Fetches** bei Qualität 1;
  bei Qualität 0: 126–216, bei Qualität 2: 448–1408. Die Minima stehen im Verhältnis 1 : 1,8 : 3,6. Der gemessene
  Anstieg über „ohne Wolken“ (Q0 → Q1 → Q2: +0,75 → +1,37 → +3,22 ms) steht im Verhältnis 1 : 1,8 : 4,3 und folgt
  damit der Schritt- und Fetch-Zahl. Früher Abbruch (`T < 0.02`) greift bei `coverage 0.5` selten.
- **Wolkenpixel, aus der Kamera gerechnet** (vertikales FOV 60°, `EditorCameraOverride::fovDegrees`, Neigung
  −0,25 rad, 1718×884): **390 550 Pixel = 25,7 %** des Viewports haben dir.y ≥ 0,02 (Zeilen 0–229, Horizont ~Zeile 247).
  **Alle** davon haben N = 32. Das gelbliche Dunstband im Screenshot liegt schon unter dem mathematischen Horizont
  (geklemmte Atmosphäre, keine Wolken). Macht **≥ 87 Mio. trilineare 3D-Fetches pro Frame** schon bei klarem Himmel
  (390 550 × 32 × 7).
- **Plausibilität:** 87 Mio. Fetches in 1,37 ms wären ~64 Mrd. trilineare 3D-Fetches pro Sekunde. Das liegt in der
  Größenordnung einer voll ausgelasteten Textureinheit einer 10-Kern-GPU (die M5-Rate ist nicht dokumentiert, geschätzt
  eher darunter). Entweder läuft der Pass am Texturlimit, oder der Compiler spart Fetches ein (z. B. gleiche
  Koordinaten bei `worleyNoise3`/`starNoise3` innerhalb eines Schritts). Klären kann das nur eine GPU-Aufnahme.
  Die Zahl ist gezählt, nicht gemessen.
- **Die Rauschtextur:** 256³ `RG16Unorm` = **64 MiB**, **ohne Mipmaps**, `MTLStorageModeShared` (Z. 6760–6775).
  Die hohen Oktaven (Faktor bis ~8,4 bei `starFbm3`, 4,06 bei Worley) tasten ohne Mip-Stufen weit auseinanderliegende
  Texel ab. Das ist schlecht für den Textur-Cache. `Shared` statt `Private` schließt auf Apple-GPUs verlustfreie
  Kompression bzw. optimales Layout aus. Ob Cache-Misses oder ALU der Limiter ist, kann nur eine Xcode-GPU-Aufnahme
  zeigen. Das ist ein Verdacht, kein Messwert.
- **Auflösung:** volle Viewport-Auflösung. `lowResClouds` (Pre-Pass auf `sceneW/2 × sceneH/2`, Z. 15840) ist in der
  Szene aus. Eingeschaltet spart es gemessen **0,62 ms** (Y3: Scene 1,95 statt 2,57). Das ist weniger als die
  erwarteten ¾, weil Upsampling, Composite und der Pre-Pass als eigener Render-Pass mitkosten.

### 2.4 Verdächtiger 2: Himmel ohne Wolken (Single-Scattering pro Pixel)

- `skyColor` ruft für **jedes** Himmelspixel `atmoScatter` auf (`MetalRenderer.mm:5524–5580`): **12 Blickschritte ×
  (3 `exp` + 5 Sonnenschritte × 3 `exp`)** ≈ 216 `exp`, dazu 24 Strahl-Kugel-Schnitte mit `sqrt`. Das ist reine
  ALU-Arbeit, keine Textur.
- Der Himmel läuft auf **allen Nicht-Terrain-Pixeln**, auch unter dem Horizont (dort mit geklemmter Richtung).
  Im Landscape-Bild sind das geschätzt ~⅔ des Viewports (≈ 1 Mio. Pixel; das Terrain-Trapez deckt grob ein Drittel).
  Gemessen: Scene ohne Wolken 1,20 ms (mit Terrain) bzw. 1,33 ms (nur Himmel, alle 1,52 Mio. Pixel).
- **Unnötige Arbeit:** Das Ergebnis hängt nur von Blickrichtung und Sonnenrichtung ab. Die Sonne steht im
  Edit-Modus still (`timeOfDay 0.5`, kein Tageszyklus). Die Engine backt denselben Himmel für die IBL-Cubemap
  (128², nur bei Sonnenbewegung, `UpdateSkyEnvCube` Z. 12535) bereits auf der CPU. Für das sichtbare Himmelsbild
  rechnet sie ihn trotzdem jeden Frame pro Pixel neu.
- Nachtelemente (Sterne, Nebel, Aurora) sind bei Tag korrekt übersprungen (`nightF`-Zweig, kohärent).
  Zirren, Kondensstreifen und God-Rays haben in der Szene Menge 0; ob ihre Funktionen dann früh aussteigen, ist
  nicht einzeln gemessen.

### 2.5 Verdächtiger 3: Wolkenschatten-Map, Arbeit ohne Empfänger

- `EncodeCloudShadow` (`MetalRenderer.mm:10799`) rendert **jeden Frame** eine **512×512**-Map
  (`kCloudShadowMapSize`, Z. 10773). Das sind 262 144 Texel, M = **6** Schritte entlang der Sonne pro Texel.
- Die Map rechnet das **„realistische“ 3D-Wolkenfeld** (`cloudFieldDensity` mit `cloudStyle 1`, Szene und Standard).
  Pro Schritt sind das 9 Fetches bis zur Präsenzprüfung (2 Domain-Warp, 4 `cloudCoverFbm`, 1 Makro, 1 Worley,
  1 Formation), bei Präsenz +8 (4 Body, 3 Billow, 1 Carve). Macht **54–102 3D-Fetches pro Texel, 14–27 Mio. pro Frame**.
  In `cloudCoverFbm(…, farW = 0)` werden zwei Oktaven mit 0 multipliziert. Ob der Compiler diese Fetches entfernt,
  ist ungeprüft (Fast-Math würde es erlauben).
- **Die sichtbaren Wolken dieser Szene kommen aus dem Dome-Pfad (`cloudMode 0`), die Schatten aus dem 3D-Feld.**
  Das ist ein anderes, teureres Dichtefeld als das sichtbare. Optisch passen Schatten und Wolken damit ohnehin nicht
  zusammen, bezahlt wird trotzdem das teure Feld.
- **Abdeckung:** ±30 Wolkenhöhen um den Projektionspunkt, bei `cloudHeight 200` also 12 km × 12 km, ein Texel
  ≈ **23 m**. Das 100×100-m-Terrain belegt davon etwa **4×4 Texel**. Von 262 144 berechneten Texeln empfangen
  ~20 überhaupt einen Schatten.
- Gemessen: Shadow-Pass 0,73 → 0,16 ms ohne Wolkenschatten (Y4), also **0,57 ms**. In der Baseline 1,04 ms.
  Qualitätsunabhängig (Y1/Y0/Y2: 0,74/0,73/0,79).

### 2.6 Verdächtiger 4: Bloom, viele kleine Pässe

- `EncodeBloom` (`MetalRenderer.mm:10909`): Bright-Pass + **10 Blur-Pässe** (5× horizontal, 5× vertikal) in halber
  Auflösung (859×442, RGBA16F, 8 B/Pixel). Jeder Pass ist ein **eigener Render-Encoder** mit Store, 11 insgesamt.
  Der Blur (`blurFragment`, Z. 1448) liest **9 Taps auf ganzen Texel-Offsets**, nutzt also den Bilinear-Trick nicht
  (5 Taps würden dasselbe liefern). Macht ~90 Fetches pro Halbauflösungspixel, **~34 Mio. Fetches** und
  **~33 MB geschriebene Render-Targets pro Frame**.
- Wiederholtes Gauß-Filtern wächst nur mit √n: Die Gewichte entsprechen etwa σ ≈ 1,9 Texel, nach 5 Iterationen
  σ_eff ≈ √5 · 1,9 ≈ 4 Halbauflösungs-Texel (~8 Viewport-Pixel). Eine Mip-Kette (Downsample/Upsample) erreicht
  mehr Radius mit weniger Pässen.
- Gemessen: nur 0,35–0,62 ms Minimum, aber das **größte Verhältnis Mittel/Minimum aller Pässe** unter Last
  (Y0: 4,51 / 0,35 ms, Baseline L3: p50 6,95 / min 0,62). Jede der 11 Encoder-Grenzen ist ein Punkt, an dem die GPU
  auf einen anderen Prozess umschalten kann. Das ist eine Vermutung, erklärt aber, warum Bloom (und die ähnlich
  gebauten SSAO-Blurs) unter Konkurrenz überproportional wachsen. Ohne Fremdlast prüfen.

### 2.7 Explizit geprüft und für diese Szene ausgeschlossen

| Hypothese aus dem Auftrag | Befund |
|---|---|
| DDGI-Rückprojektion ohne bewegte Geometrie | GI ist in den Editor-Voreinstellungen aus. GIAccel/GIShadow/GIProbes = 0 in allen Läufen |
| TAA / SSR | AA-Modus 1 = FXAA (Tonemap-Bucket 0,12 ms inkl. FXAA); SSR aus |
| 3D-Wolken-Raymarching (`applyClouds3D`/`applyClouds3DReal`, Worley-Nebel) | läuft nicht, `cloudMode 0`. **Aber** der Wolkenschatten nutzt das 3D-Feld (2.5) |
| GPU-Foliage | keine Foliage in der Szene; `Foliage`-Scope 0,0007 ms |
| Terrain-LOD-Übergänge | 4 Chunks, 34 816 Dreiecke; Terrain-CSM 0,16 ms, Terrain im Scene-Pass billiger als der Himmel, den es verdeckt (1,20 vs. 1,33 ms) |
| IBL-Cubemap-Neubau jeden Frame | nein, nur bei Sonnenbewegung (`UpdateSkyEnvCube`, Distanz-Schwelle 1e-4) |
| Render-Scale | skaliert linear mit den Pixeln (Baseline: Scene 1,11 / 3,41 / 14,06 ms bei 0,5 / 1,0 / 2,0). Der Scene-Pass ist fragmentgebunden |

## 3. Was dieser Schritt nicht zeigt

- **Unbelastete Zahlen**: Alle Messungen liefen neben pid 72986. Die FPS-Werte (Abschnitt 1) schwanken deshalb
  zwischen 44 und 60, und auch die Pass-Minima (Abschnitt 2) sind noch lastabhängig.
- **ISA-Instruktionszahl, Register, Occupancy, Limiter (ALU vs. Textur vs. Bandbreite)**: nicht messbar ohne
  Metal-Toolchain bzw. Xcode-GPU-Aufnahme. Die Fetch-Zahlen oben sind aus dem Code gezählt, nicht gemessen.
- **Effekt von `framebufferOnly = NO`** und der Drawable-Anzahl: unter der Streuung nicht trennbar.
- **Effekt des Counter-Samplings** auf die „normalen“ Baseline-Läufe: kein Schalter vorhanden, nicht gemessen.
- **Runtime (`HorizonGame`), Play-Modus, Stromsparmodus aus**: wie in Schritt 1 nicht gemessen.
- Der Wolkenpixel-Anteil (25,7 %) ist aus FOV und Neigung gerechnet (unter der Annahme, dass die Editor-Kamera das
  Standard-FOV 60° hat); der Nicht-Terrain-Anteil (~⅔) ist aus dem Screenshot geschätzt.
- Die Hidden-Window-Messung (72–77 FPS) lief unter GPU-Konkurrenz; ohne Orphan wäre die Obergrenze vermutlich höher.

## 4. Nachmessen

```sh
# 0) Vorher: kein anderer Engine-Prozess auf der GPU
python3 scripts/perf/gpu_time_by_process.py 10          # darf nur WindowServer & Co. zeigen
# 1) Kontroll-Probe (ohne Engine)
clang -fobjc-arc -O2 -framework AppKit -framework Metal -framework QuartzCore \
      scripts/perf/metal_present_probe.m -o /tmp/metal_present_probe
/tmp/metal_present_probe --calibrate --warmup 20 --frames 120 --load 140   # gpu_min ≈ 6 ms?
caffeinate -u -t 60 & /tmp/metal_present_probe --load 140 --vsync 0 --label P3 > probe-P3.json
# 2) Editor im Wechsel mit der Probe (Harness aus Schritt 1)
R() { python3 scripts/he_perf_capture.py --project /tmp/pa1/proj/Test/Test.heproj \
        --out docs/perf-audit/raw-step2 --cam 0,25,90,0,-0.25 "$@"; }
R --label X1-editor-landscape-vsyncoff --scene docs/perf-audit/scenes/landscape.hescene
# 3) Pass-Kosten: Szenenvarianten (cloudQuality/lowResClouds/cloudShadows im environment-Block der .hescene)
R --label Y0-landscape-detailed --scene docs/perf-audit/scenes/landscape.hescene --detailed
```

Rohdaten dieses Schritts: `docs/perf-audit/raw-step2/` (Probe-JSONs mit Frame-Arrays, GPU-Anteile pro Prozess,
Editor-Zusammenfassungen und -Logs). Die vollständigen Editor-Dumps liegen gzip-komprimiert außerhalb von git unter
`/Users/connorjansen/VSCode/HorizonEngine/out/perf-audit/2026-09-27-step2/`. Die Szenenvarianten Y1–Y4 entstehen aus
`landscape.hescene` durch Ändern je eines Schlüssels (`cloudQuality` 0/2, `lowResClouds` true, `cloudShadows` false).
