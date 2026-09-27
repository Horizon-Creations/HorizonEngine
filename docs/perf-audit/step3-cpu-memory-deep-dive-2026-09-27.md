# Performance-Audit Schritt 3: CPU und Speicher (Befund) + Wiederholung der Baseline

Thema 99, Schritt 3. Stand 27.09.2026, Zweig `claude/performance-audit-editor-runtime-auf-m5-macbook-landscape-hi`.
Grundlage: Schritt 1 (`baseline-2026-09-27.md`), Schritt 2 (`step2-rendering-deep-dive-2026-09-27.md`) und eigene
Messungen von 15:35 bis 16:02 (`docs/perf-audit/raw-step3/`). **Nur Befund und Messinfrastruktur, kein Umbau.**

## Kurzfassung

**Vorweg: Die CPU ist auf einem ruhigen Rechner nicht der Grund, warum die Szene keine 60 FPS schafft.** Die Engine-Arbeit
auf dem Hauptthread (CPU-Frame ohne `Metal::NextDrawable`) liegt bei niedriger Systemlast bei **p50 2,5 ms, p99 3,4 ms**
(Lauf N2). Davon ist der Hauptthread nur ~2,8 ms pro Frame tatsächlich auf der CPU (Time Profiler). Was dieser Schritt findet,
sind Ineffizienzen, die mit Szenengröße und Rechnerlast wachsen. Eine davon kostet schon in dieser Szene zweistellige
Millisekunden, sobald der Rechner beschäftigt ist:

1. **Das Job-System macht den Hauptthread zur Geisel der Rechnerlast** (Engine, gilt auch für die Runtime). Pro Frame
   verteilen `RenderExtractor::extract` (4×) und `FrustumCuller::cull` (5×) je 3 Mini-Jobs auf 10 Worker und warten darauf,
   insgesamt 27 Jobs für 4 Terrain-Chunks. Die Worker rechnen darin zusammen **0,05–0,07 ms pro Frame**. Derselbe Build mit
   derselben Szene, wenige Minuten auseinander:
   - bei Load-Average ~2 wartet der Hauptthread p50 **0,31 ms** pro Frame darauf, 2 von 600 Frames haben > 5 ms Engine-Arbeit;
   - bei Load-Average ~10,5 (andere Instanzen bauen) sind es p50 **8,7 ms**, p90 25,7 ms, und **381 von 600 Frames** haben > 5 ms.

   Die CPU-Arbeit ohne NextDrawable steigt dabei von 2,5 auf 14,5 ms (p50). Der Sampling-Profiler bestätigt es: 38 % der
   Hauptthread-Proben stehen in `std::future::get()` → `condition_variable::wait` unter extract/cull.
2. **Hauptthread und Worker liefen zu 100 % auf Effizienzkernen.** Von 1 150 Time-Profiler-Proben des Hauptthreads lag
   keine einzige auf einem P-Core, von den Worker-Threads 673 von 674. Der Stromsparmodus war an und der Bildschirm gesperrt.
   Das ist eine Messbedingung, betrifft aber wahrscheinlich auch den Menschen: `pmset lowpowermode 1` ist auf diesem M5
   dauerhaft gesetzt.
3. **Die größten echten CPU-Posten** (Anteil an der On-CPU-Zeit des Hauptthreads, Time Profiler):
   - **Metal-Encoder-Erzeugung 21 %.** Geschätzt 20–25 Render-Encoder pro Frame, allein Bloom baut 11.
   - **Hilfe-Tooltip-Suche `Help::findKey` 12,4 %** (nur Editor). Jede Toolbar-Zelle durchsucht pro Frame zweimal linear
     ~1 335 Katalogeinträge, mit einem `strlen` pro Vergleich. Das passiert auch dann, wenn die Maus nicht darüber steht.
   - Pro Frame neu angelegte `MTLBuffer` 3,8 %.
4. **Heap: 413 Allokationen und 228 KiB pro Frame auf dem Hauptthread** in einer stehenden Szene, dazu 34 auf anderen
   Threads. Die Quellen sind Metal-Pass-Deskriptoren und -Encoder, Job-Dispatch (`packaged_task`, `future`),
   das Debug-Bodengitter (144 KiB/Frame), Menü-Titel und Editor-Panels. RSS bleibt über 2 400 Frames flach (326 → 329 MB),
   ein Leck gibt es also nicht. **0 Datei-Operationen pro Frame**: Filesystem-Polling pro Frame ist ausgeschlossen.
5. **Datenlayout und doppelte Arbeit:** 4 vollständige Extracts pro Frame, jeder mit komplettem `propagateTransforms`.
   Dazu kommt ein 280-Byte-AoS-`RenderObject`, von dem der Cull 24 Byte liest. In dieser Szene (4 Objekte) kostet das
   fast nichts. Mit Foliage skaliert es aber als 4 × Instanzen × 280 B pro Frame (Hochrechnung, nicht gemessen).
6. **Terrain, LOD, Streaming bei stehender Kamera:** Es wird nichts neu gebaut und nichts hochgeladen. Die Scopes liegen bei
   Terrain 0,007 ms und LOD 0,006 ms, `streamingInFlight` ist 0. Erledigt.

Zur **Wiederholung der Baseline** (Abschnitt 1): Die GPU war frei, aber der Bildschirm war während aller Läufe gesperrt.
Die FPS (37–71) entsprechen deshalb nicht dem, was man am offenen Bildschirm sieht, und die GPU-Minima sind **nicht**
niedriger als in Schritt 2.

---

## 1. Wiederholung der Baseline (Chefchen-Auftrag), ohne fremden Engine-Prozess

### 1.1 Bedingungen

| Punkt | Wert |
|---|---|
| Fremdlast GPU | keine. `gpu_time_by_process.py` im Leerlauf: nur WindowServer 0,69 % (`raw-step3/gpu-by-process-idle-clean.txt`). pid 72986 ist beendet |
| **Bildschirm** | **gesperrt seit 15:19:18** (`ioreg … CGSSessionScreenIsLocked=Yes`), also vor allen Läufen. Vor jedem weiteren Lauf erneut geprüft, blieb gesperrt |
| Folge | Das Editorfenster wird nicht komponiert und ist nicht an den 60-Hz-Takt gebunden (wie das Hidden-Fenster in Schritt 2, 1.5). Auch mit vsync an liefern die Läufe bis zu 71 FPS |
| Energie | Stromsparmodus **an** (`pmset lowpowermode 1`), Netzteil |
| CPU-Last anderer Prozesse | stark schwankend (andere Hive-Instanzen bauen): Load-Average 1 min 3,0 (15:34), 10,5–11,0 (15:46–15:56), 2,0 (16:01) |
| Build | Release, derselbe Stand wie Schritt 1/2 für C*/S1; ab N1 mit dem Messhaken aus Abschnitt 6 (ohne Probe wirkungslos, N1 vs. N2 zeigt dasselbe Bild wie C*) |
| Harness | `scripts/he_perf_capture.py`, 300 Frames Warmup, 600 Frames Aufnahme (C7: 2 400, S1/T1: 3 000), Landscape-Kamera wie Schritt 1 |

### 1.2 Frame-Takt und CPU-Arbeit

„CPU ohne ND“ = CPU-Frame minus `Metal::NextDrawable` = Engine-Arbeit auf dem Hauptthread. „extract+cull“ = Hauptthread-Zeit
in `RenderExtractor::extract` + `FrustumCuller::cull` pro Frame. „Worker-Arbeit“ = Summe aller Job-Spans der 10 Worker in
diesem Frame. Ausgewertet mit `scripts/perf/step3_frames.py` aus den vollständigen Dumps.

| Lauf | vsync | FPS Ø | 1%-Low | delta p50/p95 ms | NextDrawable p50/p90 | CPU ohne ND p50/p90/p99 | Frames CPU ohne ND > 5 ms | extract+cull p50/p90 | Worker-Arbeit p50 | RSS max MB |
|---|---|---|---|---|---|---|---|---|---|---|
| C1-landscape-vsyncoff-run1 | aus | 69.6 | 25.2 | 10.9/35.2 | 7.85/30.5 | 2.73/3.62/13.0 | 31/600 | 0.33/0.76 | 0.055 | 326 |
| C1-landscape-vsyncoff-run2 | aus | 69.9 | 26.6 | 10.9/35.2 | 8.69/31.1 | 2.49/2.95/4.0 | 2/600 | 0.31/0.39 | 0.054 | 326 |
| C2-landscape-vsynckeep-run1 | an | 69.5 | 26.6 | 10.4/34.9 | 8.22/31.1 | 2.51/2.99/3.9 | 3/600 | 0.32/0.39 | 0.056 | 326 |
| C2-landscape-vsynckeep-run2 | an | 68.5 | 25.4 | 10.2/35.1 | 7.83/31.5 | 2.58/3.10/4.1 | 2/600 | 0.32/0.41 | 0.056 | 326 |
| C4-skyonly-vsyncoff | aus | 69.3 | 26.3 | 10.4/34.7 | 8.44/31.8 | 1.91/2.40/3.6 | 1/600 | – (0 Objekte) | – | 317 |
| C6-landscape-noclouds-vsynckeep | an | 71.3 | 25.7 | 10.5/35.0 | 8.20/31.4 | 2.56/3.13/5.0 | 6/600 | 0.31/0.41 | 0.054 | 326 |
| C7-landscape-vsyncoff-long (2 400 Fr.) | aus | 55.2 | 14.8 | 16.9/38.8 | 0.08/29.4 | 3.14/30.67/49.3 | 639/2400 | 0.38/24.5 | 0.051 | 329 |
| S1 (mit `sample`, gestört) | aus | 38.2 | 12.1 | 24.1/65.3 | 0.07/56.7 | 3.58/29.29/46.8 | 1207/3000 | 0.47/24.6 | 0.062 | 245 |
| **N1** (Load ~10,5) | aus | 36.8 | 12.0 | 25.4/65.9 | 0.08/52.3 | **14.46**/31.56/52.8 | **381/600** | **8.72**/25.7 | 0.073 | 305 |
| A1 (Alloc-Probe, Load ~10,5) | aus | 37.2 | 12.5 | 23.6/65.3 | 0.08/53.0 | 11.56/33.32/56.0 | 420/600 | 4.90/23.2 | 0.064 | 316 |
| T1 (Time Profiler, Load 11 → 5) | aus | 41.2 | 14.0 | 11.8/66.2 | 7.67/60.6 | 2.57/3.11/14.8 | 92/3000 | 0.33/0.43 | 0.056 | 429 ² |
| **N2** (Load ~2) | aus | 40.1 | 14.1 | 10.9/66.6 | 9.45/61.5 | **2.48**/2.88/3.4 | **2/600** | **0.31**/0.38 | 0.055 | 326 |

² T1 lief mit angehängtem `xctrace` (Time Profiler). Der höhere RSS ist nicht untersucht; alle Läufe ohne Werkzeug am
Prozess liegen bei 305–329 MB.

Beobachtungen:
- **Die FPS hängen unter gesperrtem Schirm nicht an der CPU.** N2 (CPU ohne ND p50 2,5 ms, GPU frei) schafft nur 40 FPS,
  die C-Serie eine halbe Stunde vorher 69–71 FPS mit derselben CPU-Arbeit. Den Unterschied macht allein NextDrawable
  (p90 31 → 61 ms), also die Präsentationsseite. Ursache nicht untersucht (Präsentation ist Schritt 2, Takt der GPU unter
  Sperre und Stromsparmodus ist hier nicht messbar).
- Die Frame-Abstände wechseln auch ohne Fremdlast zwischen ~1 ms und 30–35 ms (delta p50 10 ms, p95 35 ms), das Muster aus
  Schritt 2, 1.2. Hier nicht weiter verfolgt.
- **Die CPU-Arbeit ohne ND hängt an der Rechnerlast, und zwar über extract+cull** (N1 vs. N2, Abschnitt 2).

### 1.3 GPU-Pässe: Minimum / p50 in ms (Detailed-Capture), drei Serien nebeneinander

| Lauf | Bedingung | Shadow | SSAO | Scene | Bloom | Tonemap | Present | gpuMs p50 |
|---|---|---|---|---|---|---|---|---|
| L3-landscape-detailed-run1 | Schritt 1, Orphan läuft | 1.20 / 2.02 | 0.33 / 5.12 | 3.41 / 6.27 | 0.62 / 6.95 | 0.18 / 0.82 | 0.32 / 0.67 | 31.06 |
| Y0-landscape-detailed | Schritt 2, Orphan läuft | 0.73 / 1.25 | 0.00 / 0.70 | **2.57** / 3.96 | 0.35 / 2.99 | 0.12 / 0.43 | 0.24 / 0.42 | 17.17 |
| **C3-landscape-detailed** | GPU frei, Bildschirm gesperrt | 1.21 / 2.74 | 0.33 / 0.73 | **4.34** / 10.33 | 0.52 / 1.37 | 0.18 / 0.35 | 0.38 / 0.82 | 17.56 |
| L5-landscape-noclouds-detailed | Schritt 1 | 0.16 / 0.34 | 0.21 / 0.27 | 1.20 / 1.34 | 0.34 / 0.43 | 0.12 / 0.14 | 0.24 / 0.39 | 3.31 |
| Y5-landscape-noclouds-detailed | Schritt 2 | 0.16 / 0.30 | 0.21 / 0.25 | 1.20 / 1.24 | 0.34 / 0.38 | 0.12 / 0.14 | 0.24 / 0.31 | 9.49 |
| **C5-landscape-noclouds-detailed** | GPU frei, Bildschirm gesperrt | 0.18 / 0.52 | 0.22 / 0.56 | 1.20 / 3.13 | 0.35 / 0.92 | 0.12 / 0.28 | 0.25 / 0.75 | 7.05 |

- **Ohne Wolken sind die Minima in allen drei Serien gleich** (Scene 1,20 ms). Sie sind damit belastbar.
- **Mit Wolken ist die „saubere“ Messung nicht besser.** C3 liegt über Y0 (Scene 4,34 statt 2,57 ms). Das Minimum hängt also
  auch ohne fremde GPU-Arbeit am Zustand der GPU. Die naheliegende Erklärung sind niedrige GPU-Takte (Detailed wartet
  nach jedem Pass, die GPU läuft zwischendurch leer, Stromsparmodus, kein Display). Das ist eine Vermutung, gemessen ist sie
  nicht (Takt ohne sudo nicht lesbar). **Für die Synthese bleiben die Y-Minima aus Schritt 2 die beste Schätzung** der
  GPU-Kosten (Summe ~4,4 ms).
- Auch die Probe-Kontrolle aus Schritt 2 ist hier nicht wiederholt: Unter gesperrtem Schirm ist sie so wenig
  aussagekräftig wie der Editor.

**Was offen bleibt:** On-Screen-FPS ohne Fremdlast. Das braucht einen entsperrten Mac: `R --label L1… --scene landscape`
und `R … --vsync keep` aus Schritt 1, Abschnitt 6, dauern je ~30 s.

---

## 2. Hauptthread-Stalls außerhalb von NextDrawable: das Job-System

### 2.1 Messung

**Profiler-Dumps, Frame für Frame.** In allen Läufen außerhalb von NextDrawable hat keine Zeit gefehlt: Die Lücke zwischen
Frame-Zeit und Top-Level-Scopes liegt bei p50 0,005 ms, max. 3 ms. Die Stall-Frames (Engine-Arbeit > 5 ms) wachsen fast
ausschließlich in `RenderExtractor::extract`, `FrustumCuller::cull` und ihren Eltern-Scopes (`Metal::EncodeShadowMap` enthält
3 Culls + 1 Extract, `OnRender` enthält den Viewport-Extract). Beispiele aus C1: Frame 59 mit extract +43,9 ms,
Frame 53 mit EncodeShadowMap +13,8 und extract +12,5 ms (Liste in `raw-step3/frames-analysis.txt`).

**Aufgeschlüsselt pro Aufruf** (Worker-Spans innerhalb jedes extract/cull-Aufrufs, `threads[]` des Dumps):

| Lauf | Aufrufe | Aufrufdauer p50 / p90 / p99 | davon bis zum Start des letzten Jobs (p99) | Job-Arbeit im Aufruf | Hauptthread gesamt in extract+cull | Worker-Arbeit darin gesamt |
|---|---|---|---|---|---|---|
| C1 (Load niedrig) | 5 400 | 0,032 / 0,068 / 1,12 ms | 0,95 ms | ~0,005 ms | 433 ms | 33 ms |
| C7 (Load hoch) | 16 875 | 0,035 / 2,04 / 5,59 ms | 2,21 ms | ~0,004–0,01 ms | **7 872 ms** | **102 ms** |

In C7 verbringt der Hauptthread 7,9 s in diesen beiden Funktionen, während die Worker darin 0,1 s rechnen. Bei langen
Aufrufen vergeht im Mittel die Hälfte der Zeit, bevor der letzte Job überhaupt startet: Das ist die Aufwachlatenz eines
Workers. Die andere Hälfte geht auf das Rück-Aufwachen des Hauptthreads nach `fut.get()`.

**Sampling-Profiler** (`sample`, 10 s, `raw-step3/S1-sample-10s.txt`): Von 183 Hauptthread-Proben stehen 90 in `nextDrawable`
und **70 in `std::future<void>::get()` → `condition_variable::wait`**. Davon liegen 23 im Cull der Schatten-Kaskaden,
12 im Extract des Schatten-Pass, 12 im Viewport-Extract, 9 im Scene-Extract, 7 im SSAO-Extract und 7 in weiteren Culls.
Vorbehalt: `sample` hält den Prozess für jede Probe an und kam unter Last nur auf 183 statt ~10 000 Proben. Die Zeiten von
S1 sind dadurch gestört, die Anteile bleiben aber brauchbar.

**Kontrollpaar N1/N2** (Tabelle 1.2): gleicher Build, gleiche Szene, 10 Minuten Abstand. Unterschied ist nur die
Systemlast. extract+cull p50 8,72 → 0,31 ms, Stall-Frames 381 → 2 von 600.

### 2.2 Ursache im Code

- `parallel_for` (`src/HE_Core/include/JobSystem/JobSystem.h:70-98`):
  - Die einzige Abkürzung ist `count == 1` (Z. 74). Eine Mindest-Körnung gibt es nicht, deshalb werden 4 Objekte zu
    `chunks = min(4, 10+1) = 4` Blöcken, und 3 davon gehen per `submit` an den Pool (Z. 78-91).
  - Danach blockiert der Aufrufer in `fut.get()` (Z. 96-97). Er hilft nicht mit und stiehlt keine Arbeit.
- `ThreadPool::submit` (`JobSystem.h:26-37`) legt jedes Mal einen `std::make_shared<std::packaged_task>` an, samt
  Shared State und `std::function`, und ruft dann `notify_one`.
- Der Pool (`src/HE_Core/src/JobSystem/JobSystem.cpp:9-59, 77-80`) hat **eine** FIFO-Queue unter einem Mutex, mit
  `hardware_concurrency()` = 10 Workern. Die Worker bekommen keine explizite QoS-Klasse: Weder
  `pthread_set_qos_class_self_np` noch ein anderer Aufruf dieser Art kommt außerhalb von `vendor/` vor. Welche Klasse sie
  tatsächlich erben, ist nicht gemessen.
- **Derselbe Pool bedient auch asynchrone Arbeit außerhalb des Frames** (Code-Befund, nicht gemessen):
  - `ContentManager.cpp:1092`, `:1334`
  - `ContentBrowserPanel.cpp:207`, `:2599`
  - `EngineContentSync.cpp:208`
  - `IntegrityProbe.cpp:224`

  Ein langer Asset-Job vorne in der FIFO lässt die Frame-Jobs dahinter warten.
- Aufrufstellen pro Frame:
  - **Extract (4×):**
    - `ViewportPanel.cpp:1115` (Editor-Viewport, in `OnRender`)
    - `MetalRenderer.mm:7064` (Schatten)
    - `:11270` (SSAO)
    - `:12724` (Scene)
    - Darin jeweils `parallel_for` in `RenderExtractor.cpp:407` („ExtractMeshes“).
  - **Cull (5×):**
    - `MetalRenderer.mm:7095` (einmal je Kaskade, 3×)
    - `:16581` (`CullCameraObjects` für SSAO und Scene)
    - Darin `parallel_for` in `FrustumCuller.cpp:61`.

**Folgerung:** Das Verteilen kostet mehr, als die verteilte Arbeit einbringt. Der Hauptthread verbringt in extract+cull
bei niedriger Last das ~6-Fache der Worker-Arbeit (0,31 vs. 0,055 ms pro Frame, N2) und unter Last das ~120-Fache
(8,72 vs. 0,073 ms, N1). Ein einzelner Job rechnet p50 0,7 µs. Solange alle Kerne frei sind, fällt das kaum auf (0,3 ms). Sobald der Rechner etwas anderes tut (Browser, Build, Spotlight, zweiter Engine-Prozess), wartet der
Hauptthread auf Worker, die das System hinter andere Arbeit einreiht. Die Worker laufen dabei ohnehin auf E-Cores
(Abschnitt 3.1). **Das trifft Editor und Runtime gleich**: `RenderExtractor`, `FrustumCuller` und
`JobSystem` gehören zur Engine, nicht zum Editor. Nur der Viewport-Extract ist editor-eigen.

---

## 3. Wohin die CPU-Zeit des Hauptthreads geht (Time Profiler)

`xcrun xctrace record --template 'Time Profiler' --attach <pid> --time-limit 10s` während Lauf T1, ausgewertet mit
`scripts/perf/xctrace_timeprofile.py`. Der Time Profiler sampelt per kperf, ohne den Prozess anzuhalten, und zählt nur
Zeit auf der CPU (1 ms pro Probe).

### 3.1 Threads und Kerntypen

| Thread | Proben (ms CPU in 10 s) | P-Core | E-Core |
|---|---|---|---|
| Main Thread | 1 150 | **0** | 1 150 |
| unbenannte Threads (Metal-Submit, Job-Worker, Completion) | 674 | **0** | 673 |
| Audio-IO | 1 | 0 | 1 |

- **Kein einziger Sample auf einem Performance-Kern.** Mit Stromsparmodus an (und gesperrtem Schirm) plant macOS den
  Editor komplett auf Effizienzkerne. Ob der Hauptthread ohne Stromsparmodus auf P-Cores liefe, ist **nicht gemessen**
  (sudo bzw. der Mensch nötig). Alle CPU-Zeiten dieses Audits sind E-Core-Zeiten.
- 1 150 Proben in 10 s bei 41 FPS ergeben **~2,8 ms On-CPU pro Frame**. Der Rest der Frame-Zeit ist Warten.
- Die Nicht-Haupt-Threads verbringen 54 % in Metals eigener Command-Buffer-Übergabe
  (`-[_MTLCommandQueue _submitAvailableCommandBuffers]` → `IOGPUCommandQueueSubmitCommandBuffers`) und 15 % in
  Completion-Handlern. Die Job-Worker (`std::thread`) kommen nur auf 88 Proben, überwiegend Condvar-Warten und -Wecken.

### 3.2 Hauptthread, inklusive Anteile

| Anteil | Funktion | Art |
|---|---|---|
| 52,8 % | `MetalRenderer::EncodeFrame` | Engine |
| **21,0 %** | `-[AGXG17GFamilyCommandBuffer renderCommandEncoderWithDescriptor:]` (Encoder anlegen, davon 18 % `initWithCommandBuffer:…`) | Engine |
| 10,5 % | `EncodeBloom`, 11 Encoder | Engine |
| 10,1 % | `EncodeShadowMap` (3 Kaskaden-Encoder + Culls) | Engine |
| 6,3 % | `EncodeSSAO` | Engine |
| 3,8 % | `IOGPUMetalBuffer init…`: neue `MTLBuffer` pro Frame | Engine |
| 3,0 % | `condition_variable::notify_one` (Job-`submit`) | Engine |
| 2,7 % | `EncodeDebugLines` (neuer Buffer jeden Frame) | Engine / Editor-Gitter |
| 2,4 % | `-[AGXMTLCounterSampleBuffer initWithDevice:…]` | **Messartefakt** (Profiler-Aufnahme legt jeden Frame einen an, `MetalRenderer.mm:15452`) |
| 42,9 % | `EditorApplication::OnRender`, davon `EditorUI::renderEditor` 34,2 % | Editor |
| **12,4 %** | `HE::Ed::Help::findKey`, dazu `_platform_strlen` 8,5 % als Blatt | Editor |
| 9,9 % | `ViewportToolbar::render` → `EditorToolbar::cellImpl` 9,6 % | Editor |
| 7,4 % | `EditorSettingsPanel::DrawEngineSettings` | Editor |
| 5,1 % / 3,5 % | `ContentBrowserPanel::render` / `OutlinerPanel::render` | Editor |
| 3,7 % | `HE::Window::PollEvents` (Input) | Engine |

**Help-Suche (Editor, 12,4 %):**
- `EditorToolbar.cpp:55` prüft `hasHelp = … Help::findKey(helpKey) != nullptr`, und `:69` ruft
  `EditorWidgets::helpForKey(helpKey)` (`EditorRowWidgets.cpp:404-407`) und damit `findKey` **ein zweites Mal**, bevor
  `queueIfHovered` schaut, ob die Maus überhaupt darüber steht.
- `findKey` (`EditorHelp.cpp:7105-7111`) vergleicht `key == e.key` linear über den `constexpr`-Katalog. `e.key` ist ein
  `const char*`, der Vergleich baut also für jeden der ~1 335 Einträge (`EditorHelp.cpp:24` ff.) ein `string_view` samt
  `strlen`.
- `Help::find` (`:7113-7128`) setzt zusätzlich zwei `std::string` für den Scope zusammen und ruft `findKey` bis zu
  viermal.
- Aufrufer laut Profil: Viewport-Toolbar 66 %, Outliner 20 %, Content Browser 14 %.

**Encoder-Erzeugung (Engine, 21 %):** 242 Proben in 10 s bei 41 FPS sind ~0,6 ms pro Frame. Bei den aus der
Pass-Struktur geschätzten 20–25 Encodern kostet einer auf dem E-Core also ~25 µs. Gezählt, nicht gemessen, ist die Anzahl
pro Frame:
- **Bloom: 11 Encoder.** Das ist ein Bright-Pass plus 10 Blur-Pässe, siehe Schritt 2, 2.6.
- **SSAO: mehrere Encoder.**
- **Schatten: je Kaskade einer, dazu der Wolkenschatten.**
- **Je einer für Scene, Tonemap, FXAA und Present.**

Dazu kommt pro Pass ein frischer `[MTLRenderPassDescriptor renderPassDescriptor]`, zum Beispiel `MetalRenderer.mm:7099`
(je Kaskade), `:15785`, `:15972`, `:15997` und `:16038`.

---

## 4. Heap-Allokationen pro Frame

### 4.1 Mengen (Lauf A1, 599 Frames, `scripts/perf/alloc_probe.c` per `DYLD_INSERT_LIBRARIES`)

| Größe | p50 / p90 / max pro Frame | Mittel |
|---|---|---|
| Allokationen Hauptthread | **411 / 415 / 566** | 412,9 |
| Bytes Hauptthread | **228 845 / 229 189 / 1 277 453** | 233 KB |
| Freigaben Hauptthread | 310 / 317 / 445 | 313,2 |
| Allokationen andere Threads | 34 / 43 / 70 | 34,4 |
| Datei-Ops Hauptthread (`stat`/`lstat`/`open`/`fopen`/`access`) | **0 / 0 / 1** | 0,0 |
| Datei-Ops andere Threads | 0 / 0 / 0 | 0,0 |

- Gezählt wird Frame-Anfang bis nächster Frame-Anfang, also die ganze Schleife.
- Der Probe erfasst `malloc`/`calloc`/`realloc`, die `malloc_type_*`-Varianten, `malloc_zone_*` und `operator new`, auch
  Aufrufe aus Metal, Foundation und libobjc. Der Selbsttest (`scripts/perf/alloc_probe_selftest.mm`) zählt
  1000 × `malloc`, `new[]`, `vector`, `string` und `[NSObject new]` jeweils **exakt 1000**.
- Rund 100 Hauptthread-Allokationen pro Frame werden auf anderen Threads freigegeben (Metal-Completion-Handler,
  Job-State). RSS bleibt dabei flach: 326 MB (600 Frames) und 329 MB (2 400 Frames, C7). Ein Hinweis auf ein Leck ergibt
  sich daraus nicht.
- Die eine Datei-Op in einzelnen Frames ist `pollHotReload` (alle 1,5 s, `EditorApplication.cpp:2734-2741`): 5 Ops in
  300 Frames.

### 4.2 Wer allokiert (Lauf A2, 300 Frames, Stacks aller Hauptthread-Allokationen)

Nach der ersten Engine-Funktion im Stack gruppiert (`scripts/perf/alloc_probe_report.py`, vollständig in
`raw-step3/A2-alloc-by-owner.txt`).

| pro Frame | KiB/Frame | Verursacher | Art |
|---|---|---|---|
| 67 | 8,7 | `EncodeBloom` (11 Pässe: Deskriptor, Encoder, Handler) | Engine |
| 60 | 7,4 | `EncodeFrame` (Command-Buffer, Present, Tonemap, FXAA, Completion-Blöcke) | Engine |
| 50 | 3,7 | `FrustumCuller::cull` (5×: `futures`-Vektor + 3 × `packaged_task` je Aufruf) | Engine |
| 44 | 6,4 | `RenderExtractor::extract` (4 × 11: `items`, Jobs, `sections`) | Engine |
| 20 + 18 + 6 | 5,6 | `EncodeSSAO`, Schatten-Kaskaden, `EncodeCloudShadow` | Engine |
| 8 | 0,0 | `HE::worldMatrixOf` (`chain`-Vektor pro LOD-Entity, `LODSystem.cpp:33` → `TransformHierarchy.cpp:38-62`) | Engine |
| 27 | 0,8 | `EditorSettingsPanel::isFavorite` (`EditorSettingsPanel.cpp:71-75`: zwei String-Verkettungen pro Zeile) | Editor |
| 26 | 1,0 | `Help::find` aus `EditorWidgets::Row::*` (Scope-Strings) | Editor |
| 19 | 0,9 | `MacMenuBar::setItemTitle` (`MacMenuBar.mm:521-537`, 3 × pro Frame aus `EditorUI.cpp:1539-1545`: `NSString` + Menü-Durchlauf) | Editor |
| 12 | **144** | `ViewportPanel::appendGroundGrid` (Bodengitter jeden Frame neu) | Editor |
| 2 | **38,9** | `EditorApplication::OnRender` (Debug-Linien-Kopien `dbg.lines()`, `EditorApplication.cpp:3948-3964`) | Editor |
| 7 | 0,3 | `AssetThumbnailCache::cacheDirForProject` aus dem Content Browser | Editor |
| 7 | 3,0 | `EngineProfiler::endScope` (Scope-Vektor wächst pro Frame neu) | **Messartefakt** |

Zusammen ~413/Frame, 228 KiB/Frame. Grob die Hälfte der Anzahl kommt aus Metal-Pass-Objekten und Job-Dispatch (Engine),
der Großteil der Bytes aus dem Editor-Gitter und den Debug-Linien.

---

## 5. Datenlayout, doppelte Arbeit, Terrain/LOD/Streaming bei stehender Kamera

- **4 volle Extracts pro Frame** in dieselbe Welt mit derselben Kamera:
  - `ViewportPanel.cpp:1115`
  - `MetalRenderer.mm:7064`, `:11270`, `:12724`

  Jeder davon führt aus:
  - `out.clear()`
  - `HE::propagateTransforms` (`RenderExtractor.cpp:63`, alle Weltmatrizen neu, ohne Dirty-Prüfung)
  - `extractMeshes` samt `parallel_for`
  - Lichter, Icons und Foliage

  Nach jedem Extract läuft im Metal-Backend noch eine Bounds-Verfeinerung über alle Objekte (Code-Karte, nicht einzeln
  gemessen). Gemessen: `RenderExtractor::extract` 0,19 ms pro Frame über 4 Aufrufe bei niedriger Last (C1, p50), also
  ~0,05 ms pro Aufruf für 4 Objekte. Das meiste davon ist Job-Dispatch (Abschnitt 2).
- **`RenderObject` = 280 Byte, AoS** (per `sizeof` mit den echten Build-Flags gemessen). `worldBounds` liegt bei Offset
  136 und ist 24 B groß. `FrustumCuller::cull` (`FrustumCuller.cpp:54-65`) liest pro Objekt nur diese 24 B, und das
  5× pro Frame. Bei 128-Byte-Cache-Zeilen belegt jedes Objekt gut 2 Zeilen, von denen der Cull eine anfasst.
- **Foliage (Hochrechnung, nicht gemessen, die Szene hat keine):** Jede Instanz aus `FoliageComponent::cachedInstances`
  (64-B-`mat4`) wird in **jedem** der 4 Extracts zu einem eigenen 280-B-`RenderObject`. Später wird sie wieder in
  `DrawCall::instanceTransforms` gebündelt und pro Frame per `newBufferWithBytes` hochgeladen (`MetalRenderer.mm:13366-13380`).
  Bei 10 000 Instanzen wären das rechnerisch ~11 MB AoS-Schreibzugriffe und 40 000 Objekte, die gecullt und sortiert werden,
  pro Frame. Die hier gemessenen Kosten sagen darüber nichts aus.
- **Terrain bei stehender Kamera:** Der Neubau ist hinter `if (!tc.dirty && !tc.regionDirty) continue;` gesperrt
  (`TerrainSystem.cpp:291`). Der Scope „Terrain“ liegt bei 0,007 ms p50. Pro Frame bleiben ein Material-Abgleich und
  1 Allokation (`TerrainSystem::updateTerrains`, Tabelle 4.2 „1/Frame“).
- **LOD bei stehender Kamera:** 0,006 ms. Es wird nichts umgeschaltet und nichts hochgeladen, übrig bleiben 8 kleine
  Allokationen pro Frame in `worldMatrixOf`.
- **Streaming:** `streamingInFlight = 0` in allen Frames. Es gibt keine Datei-Ops pro Frame (4.1).
- **Input:** `PollEvents` 0,09 ms p50 (C1), 3,7 % der Hauptthread-CPU. Unauffällig.

---

## 6. Neue Messinfrastruktur (ohne Verhaltensänderung)

| Datei | Zweck |
|---|---|
| `scripts/perf/alloc_probe.c` | Interposer per `DYLD_INSERT_LIBRARIES`. Zählt Heap-Allokationen und Datei-Ops, getrennt nach Hauptthread und anderen Threads. Mit `HE_ALLOC_PROBE_STACKS=1` zeichnet er die Stacks aller Hauptthread-Allokationen auf, Ausgabe nach `HE_ALLOC_PROBE_OUT`. Nur für Messläufe, nichts linkt dagegen |
| `scripts/perf/alloc_probe_selftest.mm` | Abdeckungstest (C++, ObjC, Metal, `stat`) |
| `scripts/perf/alloc_probe_report.py` | Gruppiert Stacks nach der ersten Engine-Funktion, demangelt |
| `src/HE_Core/src/Diagnostics/EngineProfiler.cpp` | Messhaken: sucht die drei `he_alloc_probe_*`-Funktionen per `dlsym(RTLD_DEFAULT, …)` (nur `__APPLE__`). Ohne injizierten Probe sind alle Zeiger null und nichts passiert. Mit Probe stehen `allocsMain`, `allocBytesMain`, `freesMain`, `allocsOther`, `fileOpsMain` und `fileOpsOther` pro Frame in `frames[].stats` des Dumps. Kein Header geändert |
| `scripts/perf/step3_frames.py` | Pro-Frame-Auswertung: CPU ohne NextDrawable, Stall-Frames mit gewachsenen Scopes, Job-Fan-out (Jobs, Worker-Arbeit, Hauptthread in extract/cull) und Alloc-Zähler |
| `scripts/perf/xctrace_timeprofile.py` | Time-Profiler-Export: Proben pro Thread, P-/E-Core-Anteil, inklusive und Self-Zeit pro Funktion |

---

## 7. Priorisierte Vorschläge (nicht umgesetzt)

| # | Vorschlag | Beleg | Erwartung | Gilt für |
|---|---|---|---|---|
| 1 | **`parallel_for` mit Mindest-Körnung.** Unter einer Schwelle (grob einige Hundert Elemente bzw. Mikrosekunden) inline laufen lassen. Darüber soll der Aufrufer mitarbeiten, bis die Queue leer ist, statt in `fut.get()` zu schlafen | 2.1: Worker-Arbeit 0,05 ms vs. Hauptthread-Warten bis 8,7 ms p50 | Stalls unter Last fallen weg (N1 → N2-Niveau, −5 bis −12 ms p50 pro Frame bei Load ~10). Bei Ruhe −0,25 ms. Dazu −50 Allokationen/Frame | Engine + Runtime |
| 2 | Frame-Jobs **nicht in dieselbe FIFO wie Asset-Loads** (eigene Queue oder Priorität). Worker mit passender QoS starten | 2.2 (Code-Befund) | verhindert, dass ein Import oder Load den Frame blockiert. Nicht gemessen | Engine + Runtime |
| 3 | **Einmal pro Frame extrahieren** und den `RenderWorld` für Schatten, SSAO und Scene teilen, nur die Kamera-Matrizen tauschen. `propagateTransforms` mit Dirty-Flag | 5, 4.2 | 3 von 4 Extracts entfallen. Wichtig, sobald Foliage oder viele Meshes da sind | Engine + Runtime |
| 4 | **Hilfe-Katalog per Hash-Map** (oder Ergebnis pro Aufrufstelle cachen) und Lookup erst bei Hover | 3.2: 12,4 % der Hauptthread-CPU | ~0,3 ms/Frame auf E-Core | Editor |
| 5 | Weniger Render-Encoder: Bloom als Mip-Kette, Blur-Pässe zusammenlegen. `MTLRenderPassDescriptor` einmal anlegen und wiederverwenden | 3.2: Encoder-Anlage 21 %, 4.2: Bloom 67 Allokationen/Frame | CPU und GPU (Schritt 2, 2.6) | Engine + Runtime |
| 6 | Pro Frame neu angelegte `MTLBuffer` (Instanz-Matrizen `MetalRenderer.mm:7156`, `:13378`, Debug-Linien) durch einen Ring-Buffer ersetzen. Das Bodengitter nur bei Kamerawechsel neu bauen | 3.2: 3,8 % + 2,7 %, 4.2: 144 KiB/Frame | weniger IOGPU-Kernelaufrufe und Allokationen | Engine / Editor |
| 7 | `MacMenuBar::setItemTitle` nur bei Zustandswechsel. `isFavorite` ohne String-Verkettung | 4.2 | −46 Allokationen/Frame | Editor |
| 8 | Stromsparmodus beim Nachmessen am Gerät des Menschen ausschalten bzw. notieren | 3.1: 100 % E-Core | alle CPU-Zahlen hier sind E-Core-Zahlen | Messbedingung |

---

## 8. Was dieser Schritt nicht zeigt

- **On-Screen-FPS ohne Fremdlast.** Der Bildschirm war während aller Läufe gesperrt (1.1). Das braucht einen entsperrten Mac.
- **Besser belastbare GPU-Minima.** Die saubere Wiederholung liefert mit Wolken höhere Minima als Schritt 2 (1.3).
- **Verhalten auf P-Cores bzw. ohne Stromsparmodus**: nicht umschaltbar ohne sudo.
- **Runtime (`HorizonGame`)**: wie in Schritt 1/2 nicht gemessen. Die Befunde zu Job-System, Extract, Cull und Metal-Encodern
  beruhen auf Code, der zur Engine gehört und in beiden Programmen läuft. Übertragen sind sie per Code-Lesen, nicht per Messung.
- **Wirkung der Vorschläge**: kein Umbau, also kein A/B. Die Erwartungen in Abschnitt 7 sind aus den Messungen abgeleitet.
- **Asset-Jobs vor Frame-Jobs in der Queue**: nur Code-Befund. In den Läufen lag kein Asset-Job in der Queue.
- **Profiler-Artefakte**: Die Aufnahme selbst legt pro Frame einen `MTLCounterSampleBuffer` an (2,4 % CPU) und allokiert
  ~7 Mal/Frame für ihre Scope-Liste. Beides ist in den Zahlen oben enthalten und gekennzeichnet.
- `sample` (S1) hält den Prozess pro Probe an. Die Zeiten von S1 sind gestört und nur als Anteile verwendet.

## 9. Nachmessen

```sh
# Vorher: Bildschirm entsperrt? GPU frei? Last?
ioreg -n Root -d1 | grep -o '"CGSSessionScreenIsLocked"=[A-Za-z]*'
python3 scripts/perf/gpu_time_by_process.py 10; uptime
R() { python3 scripts/he_perf_capture.py --project /tmp/pa1/proj/Test/Test.heproj \
        --out docs/perf-audit/raw-step3 --cam 0,25,90,0,-0.25 \
        --scene docs/perf-audit/scenes/landscape.hescene "$@"; }
R --label N2-landscape-vsyncoff-lowload                       # Stalls: step3_frames.py auf den Dump
# Allokationen zählen (Timing gültig) bzw. Stacks (Timing ungültig)
clang -O2 -dynamiclib scripts/perf/alloc_probe.c -lc++ -o /tmp/alloc_probe.dylib
R --label A1 --env DYLD_INSERT_LIBRARIES=/tmp/alloc_probe.dylib
R --label A2 --frames 300 --env DYLD_INSERT_LIBRARIES=/tmp/alloc_probe.dylib \
  --env HE_ALLOC_PROBE_STACKS=1 --env HE_ALLOC_PROBE_OUT=/tmp/A2.txt
python3 scripts/perf/alloc_probe_report.py /tmp/A2.txt
# Time Profiler (während eines Laufs mit --frames 3000)
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun xctrace record \
  --template 'Time Profiler' --attach $(pgrep -n HorizonEditor) --time-limit 10s --output /tmp/t.trace
DEVELOPER_DIR=… xcrun xctrace export --input /tmp/t.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > /tmp/t.xml
python3 scripts/perf/xctrace_timeprofile.py /tmp/t.xml
```

Die vollständigen Dumps (gzip) und die rohe Stack-Datei liegen außerhalb von git unter
`/Users/connorjansen/VSCode/HorizonEngine/out/perf-audit/2026-09-27-step3/`.
