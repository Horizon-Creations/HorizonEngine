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
2. **Die GPU ist voll.** Während der Editor die Landscape-Szene rendert, teilt sich die GPU (per-Prozess-GPU-Zeit
   aus dem IORegistry) in **Editor 46 %, verwaister Editor 35 %, WindowServer 18 %** auf, zusammen ~99 %.
   Sobald die Summe über die Kapazität geht, verpasst das Fenster Refreshes, und es entsteht das Muster aus der
   Baseline (NextDrawable abwechselnd ~0 und 30–60 ms, 44–51 FPS).
3. **Wenn die GPU reicht, ist NextDrawable reine vsync-Wartezeit.** Heute Nachmittag schaffte derselbe Editor mit
   derselben Landscape-Szene unter derselben Fremdlast **59,5–59,6 FPS**. NextDrawable lag dabei gleichmäßig bei ~15 ms:
   Die CPU wartet auf den nächsten 60-Hz-Takt des Compositors, sie verliert dabei nichts.
   „NextDrawable p50 17 ms“ ist also für sich kein Kostenposten.
4. **Echter Engine-Befund dahinter:** Ein Editor mit verstecktem Fenster drosselt nicht. pid 72986 hat in
   2 Tagen 19 Stunden **93 217 s GPU-Zeit** verbraucht (im Mittel ~39 % der GPU) und nimmt jedem anderen
   Renderer auf dem Gerät ein Drittel bis die Hälfte der GPU weg. `Application::Run` kennt keinen Zustand für
   verstecktes, verdecktes oder minimiertes Fenster (Abschnitt 1.5).
5. **Die GPU-Arbeit der Szene selbst ist klein:** Die Summe der Pass-Minima liegt bei ~6 ms für 1718×884 Pixel.
   Die teuersten Posten: Wolken im Scene-Pass (~2,2 ms), Wolkenschatten-Map im Shadow-Pass (~1,0 ms), Bloom
   (~0,6 ms, aber mit der größten Streuung unter Last). Details in Abschnitt 2.

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
- Bei einem versteckten Fenster nimmt der Compositor die Drawables nicht im Displaytakt ab. Der Editor rendert deshalb
  so schnell, wie die GPU ihn lässt, und zwar das volle Szenenbild samt Wolken (`HE_SKY_TIME=30`).
- Das betrifft nicht nur verwaiste Testläufe. Es betrifft jede Situation, in der ein Engine-Fenster im Hintergrund
  läuft (Editor minimiert während eines Spiel-Exports, zweiter Editor im Collab-Test, Spiel hinter dem Editor).
  Für den Menschen am M5 heißt das: **Jeder zweite HorizonEditor/HorizonGame-Prozess im Hintergrund halbiert die GPU
  des Vordergrund-Fensters.**

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

*(folgt)*

## 3. Was dieser Schritt nicht zeigt

*(folgt)*

## 4. Nachmessen

*(folgt)*
