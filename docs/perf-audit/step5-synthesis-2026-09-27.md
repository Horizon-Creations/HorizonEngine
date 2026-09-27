# Performance-Audit Schritt 5: Synthese, priorisierter Befund und Optimierungsvorschläge

Thema 99, Schritt 5. Stand 27.09.2026, Zweig `claude/performance-audit-editor-runtime-auf-m5-macbook-landscape-hi`.
Grundlage sind ausschließlich die Berichte der Schritte 1 bis 4:

- S1 = `baseline-2026-09-27.md`
- S2 = `step2-rendering-deep-dive-2026-09-27.md`
- S3 = `step3-cpu-memory-deep-dive-2026-09-27.md`
- S4 = `step4-input-audit-2026-09-27.md`

Verweise wie „S2 2.5“ meinen Bericht und Abschnitt. **Nichts davon ist umgesetzt.** Dieses Dokument ist die
Entscheidungsgrundlage dafür, welche Optimierungen als eigene Themen folgen.

## Kurzurteil

1. **Die Szene selbst ist klein.** Unbelastet braucht sie auf dem M5 etwa **4,4–6 ms GPU** (Summe der Pass-Minima, S2 2.2)
   und **2,5 ms CPU** auf dem Hauptthread (p50, p99 3,4 ms, S3 1.2 Lauf N2) für ein 1718×884-Bild. Beides passt mehrfach
   in die 16,7 ms eines 60-Hz-Frames. Dazu kommt, dass alle CPU-Zeiten auf Effizienzkernen gemessen sind (S3 3.1).
2. **Die gemessenen < 60 FPS hatten Ursachen außerhalb der Szene**, und die Engine verstärkt sie:
   - Ein **verwaister zweiter Editor** nahm dem Vordergrund-Fenster 35–54 % der GPU (S2 1.4). Er lief mit, weil die
     Engine ein verstecktes Fenster ungebremst weiterrendern lässt (S2 1.5). Derselbe Editor mit derselben Szene lief
     44–47 FPS, sobald der Orphan viel GPU zog, und 59,5 FPS, sobald er weniger zog (S2 1.4, X1/X3).
   - **Rechnerlast** (parallele Builds) macht den Hauptthread über das Job-System zur Geisel: Die Engine-Arbeit stieg von
     p50 2,5 auf 14,5 ms, ohne dass die Szene sich änderte (S3 2.1, N1/N2).
   - **Stromsparmodus**: `pmset lowpowermode 1` ist auf diesem M5 dauerhaft gesetzt, Haupt- und Worker-Threads liefen
     zu 100 % auf E-Cores (S3 3.1).
3. **Was dieses Urteil nicht trägt:** Der Audit hat **keine einzige Messung am entsperrten Bildschirm ohne Fremdlast**.
   Schritt 1 und 2 liefen neben dem Orphan. Ab Schritt 3 war der Bildschirm gesperrt (auch jetzt, 18:02, wieder geprüft:
   `CGSSessionScreenIsLocked=Yes`). Außerdem liefert Lauf N2 (GPU frei, CPU 2,5 ms) unter gesperrtem Schirm nur 40 FPS
   mit NextDrawable p90 61 ms, und das ist **nicht erklärt** (S3 1.2). „Die Engine ist in Ordnung“ lässt sich daraus
   also nicht schließen. Was sich schließen lässt: Weder CPU- noch GPU-Arbeit der Szene erklären < 60 FPS.
4. **Der verwaiste Editor lief vom 24.09. 19:15 bis zum 27.09. nachmittags** (S1, S2 1.5). Hat der Mensch seine
   Beobachtung in diesem Zeitraum gemacht, war sie von ihm betroffen.

## So sind die Gewinne zu lesen (ms → FPS)

- **Am freien Gerät mit vsync (60 Hz) bringt keine GPU- oder CPU-Einsparung in dieser Szene mehr FPS.** Der Frame ist
  schon kürzer als 16,7 ms, und die FPS kleben am Displaytakt. Einsparungen werden dort zu **Reserve**: weniger Wärme
  und Akku auf dem lüfterlosen MacBook Air, später Drosselung, mehr Platz für aufwendigere Szenen.
- **Unter Konkurrenz** (zweiter Engine-Prozess, Build, Browser) oder in größeren Szenen hebt jede eingesparte
  Millisekunde den Boden. Die Spalte „FPS unter Last“ rechnet linear von einem 20-ms-Frame (50 FPS, typischer Wert aus
  S1 L1/L2): −1 ms ≈ **+2,6 FPS**. Mit vsync werden Frame-Abstände auf Vielfache von 16,7 ms gerundet (S2 1.2). Der
  Gewinn zeigt sich dann als weniger verpasste Refreshes und nicht linear. Die Spalte ist deshalb eine
  **Größenordnung, keine Vorhersage**. CPU- und GPU-Einsparungen addieren sich nicht: Eine CPU-Einsparung zählt nur,
  wenn der Hauptthread der Engpass ist (Rechnerlast), eine GPU-Einsparung nur, wenn es die GPU ist (zweiter
  Engine-Prozess, größere Szene).
- „gemessen“ = die Einsparung ist als Differenz zweier Läufe belegt (z. B. Szenenvariante mit/ohne).
  „geschätzt“ = aus Code und Zählung hergeleitet, ohne A/B.
- **Aufwand:** S = bis ein Tag, M = wenige Tage, L = eine Woche oder mehr bzw. mehrere Backends.
  **Risiko** meint Verhaltens- oder Optikänderung und Regressionsgefahr.
- **Backends:** Die Engine hat fünf Render-Backends. Was in `MetalRenderer.mm` liegt, betrifft nur macOS. Was in
  `Application::Run` oder im Job-System liegt, wirkt überall, braucht aber je nach Vorschlag einen Haken in jedem Backend.

---

## Tabelle 0: Umgebung, kein Umbau (Prüfschritte für den Menschen)

Diese Punkte kosten nichts und klären, ob die Beobachtung überhaupt noch besteht.

| # | Punkt | Beleg | Wirkung | Prüfen (30 s) |
|---|---|---|---|---|
| U1 | **Kein zweiter HorizonEditor/HorizonGame im Hintergrund** (auch versteckte Test-Editoren aus Hive-Läufen) | S2 1.4/1.5: Orphan 35–54 % GPU, 93 217 s GPU-Zeit in 2 Tagen 19 h | gemessen: 44–47 → 59,5 FPS, sobald er weniger zieht | `python3 scripts/perf/gpu_time_by_process.py 10`: darf nur WindowServer zeigen |
| U2 | **Stromsparmodus aus** (Systemeinstellungen → Batterie) | S3 3.1: 0 von 1 150 Hauptthread-Proben auf P-Cores | nicht gemessen. Alle CPU-Zahlen des Audits sind E-Core-Zahlen | `pmset -g \| grep lowpowermode` |
| U3 | **Keine parallelen Builds** beim Beurteilen der FPS | S3 2.1: Load 10,5 → Engine-Arbeit p50 14,5 ms statt 2,5 ms | gemessen (N1/N2) | `uptime`: Load-Average < 3 |
| U4 | **Die eine fehlende Messung nachholen:** L1/L2 am entsperrten Bildschirm, U1–U3 erfüllt | S3 8, S4 Kurzfassung | liefert erstmals die echte On-Screen-Zahl | Befehl unten, ~30 s je Lauf |
| U5 | **Deckt die Testszene die Szene des Menschen ab?** Gemessen wurde die Startszene von `~/HorizonEngineProjects/Test`: `cloudMode 0` (Dome-Wolken), `cloudQuality 1`, `coverage 0.5`, GI/TAA/SSR aus, keine Foliage, 100×100-m-Terrain | S1 3 | Mit GI, TAA, 3D-Wolken, Foliage oder größerem Terrain gelten die GPU-Zahlen hier nicht | eigene Szene mit `--scene` messen, Befehl unten |

```sh
# U4/U5: vorher entsperren, U1-U3 pruefen
/usr/sbin/ioreg -n Root -d1 | grep -o '"CGSSessionScreenIsLocked"=[A-Za-z]*'   # muss No sein
R() { python3 scripts/he_perf_capture.py --project /tmp/pa1/proj/Test/Test.heproj \
        --out docs/perf-audit/raw-step5 --cam 0,25,90,0,-0.25 "$@"; }
caffeinate -u -t 600 &
R --label U4-landscape-vsyncoff --scene docs/perf-audit/scenes/landscape.hescene
R --label U4-landscape-vsynckeep --scene docs/perf-audit/scenes/landscape.hescene --vsync keep
# eigene Szene: --project <eigenes .heproj> --scene <eigene .hescene>
```

---

## Tabelle A: In dieser Szene unnötige Arbeit

Arbeit, die für Terrain + Himmel so nicht nötig ist oder bei diesen Einstellungen nichts beiträgt. Kosten sind
GPU-Minima der Y-Serie (S2 2.2), die beste Schätzung der unbelasteten Kosten (siehe „Korrekturen“).

| # | Befund | Beleg | Gewinn GPU | FPS unter Last | Aufwand | Risiko | Art | Gilt für |
|---|---|---|---|---|---|---|---|---|
| A1 | **Wolkenschatten-Map rechnet 12 × 12 km für ein 100-m-Terrain, und zwar aus dem falschen Dichtefeld.** 512² Texel, 6 Schritte, 54–102 3D-Fetches pro Texel, jeden Frame. Davon empfangen ~20 Texel überhaupt Schatten. Die sichtbaren Wolken kommen aus dem Dome-Pfad (`cloudMode 0`), der Schatten aus dem teureren 3D-Feld. Schatten und Wolken passen optisch nicht zusammen. Das ist ein **Qualitätsbefund**, nicht nur ein Kostenbefund | S2 2.5, `MetalRenderer.mm:10773`, `:10799` | **0,57 ms gemessen** (Y4 vs Y0, kompletter Wegfall). Mit Zuschnitt oder Rate geschätzt 0,3–0,5 ms | +0,8 bis +1,5 | S–M | niedrig (Zuschnitt), mittel (Feldwechsel ändert die Optik) | Quick-Win (Zuschnitt), Umbau (gleiches Feld) | beide, nur Metal-Code geprüft |
| A2 | **Die Coverage-Schranke im Wolken-Raymarch greift bei `coverage 0.5` nie.** `starFbm3·0,5 + 0,55 ≥ 0,55`, der Schwellwert liegt bei 0,46. Bei der Standardbedeckung zahlt jeder Schritt mindestens 7 Fetches. Der Kommentar verspricht das Gegenteil | S2 2.3 | geschätzt 0–0,7 ms (Anteil der 1,37 ms, der auf wolkenfreie Schritte fällt; nicht gemessen) | 0 bis +1,8 | S | niedrig, wenn die Schranke exakt bleibt. Optischen A/B mit `he_shot.py` Pflicht | Quick-Win (Formelfehler) | beide |
| A3 | **Wolken-Schrittzahl am Horizont maximal, wo die Wolken ausgeblendet werden.** Unter 22° Blickhöhe sind es immer 32 Schritte, `horizon = smoothstep(0.03, 0.22, dir.y)` blendet dort gleichzeitig aus. Mit der Landscape-Kamera trifft das jedes Wolkenpixel (25,7 % des Viewports, ≥ 87 Mio. Fetches/Frame). Außerdem steht `lowResClouds` aus | S2 2.3, Y3 | **`lowResClouds` an: 0,62 ms gemessen.** Schrittzahl an die Ausblendung koppeln: geschätzt 0,3–0,6 ms | +0,8 bis +1,6 | S | niedrig bis mittel (halbe Auflösung: Kanten; Schrittzahl: Banding am Horizont) | Quick-Win | beide |
| A4 | **Himmel wird jeden Frame pro Pixel neu gestreut, obwohl die Sonne steht.** `atmoScatter`: ~216 `exp` pro Pixel auf ~⅔ des Viewports. Das Ergebnis hängt nur von Blick- und Sonnenrichtung ab. Die IBL-Cubemap backt denselben Himmel bereits, nur bei Sonnenbewegung | S2 2.4, `UpdateSkyEnvCube` | geschätzt **~1 ms** (Himmel ohne Wolken 1,1–1,3 ms gemessen, Rest nach LUT ~0,1–0,2 ms) | +2,6 | M (Sky-View-LUT, nur bei Sonnenänderung neu). L, wenn GL/D3D/Vulkan mitziehen | mittel (Auflösung am Horizont; Sonnenscheibe analytisch lassen) | Umbau | beide |
| A5 | **Editor zeichnet dauerhaft**, auch ohne Eingabe und bei stehender Szene. Nur der App-Modus des Spiels ist ereignisgesteuert. `MaxFps` greift nur bei vsync aus | S4 4, S2 1.5, `Application.cpp:708` | 0 ms pro Frame, aber Dauerlast auf dem lüfterlosen Air (Wärme, Akku, Drosselung) | – | S (Limit bei unfokussiertem Fenster, wie „Use less CPU in background“ in UE) | niedrig | Quick-Win | Editor |
| A6 | Kleinposten, einzeln < 0,3 ms: SSAO 0,21 ms, Terrain-CSM 0,16 ms mit 3 Kaskaden-Culls + eigenem Extract (CPU 0,27 ms), FXAA 0,05 ms | S1 C, S2 2.2 | zusammen ~0,4 ms GPU + ~0,4 ms CPU | +1 | – | – | nur als Einstellung, kein Thema | – |

**Summe A1–A4, geschätzt:** ~1,5–3 ms GPU (A2 und A3 überlappen teilweise), also bis zur Hälfte der Szenen-GPU-Arbeit. Alles davon liegt im Himmel- und
Wolkencode. Terrain, LOD, Streaming, IBL, Input, DDGI, TAA, SSR und Foliage kosten in dieser Szene nichts oder laufen nicht
(S2 2.7, S3 5, S4).

---

## Tabelle B: Grundsätzlich ineffizient (kostet auch in anderen Szenen)

Engine-Probleme, die mit Szenengröße, Rechnerlast oder Prozessanzahl wachsen.

| # | Befund | Beleg | Gewinn | FPS unter Last | Aufwand | Risiko | Art | Gilt für |
|---|---|---|---|---|---|---|---|---|
| **B1** | **Verstecktes, verdecktes oder minimiertes Fenster rendert ungebremst.** `Application::Run` kennt `SDL_WINDOW_HIDDEN/OCCLUDED/MINIMIZED` nicht. Ein verstecktes Fenster ist nicht an den Displaytakt gebunden (72–77 FPS). Das ist der Mechanismus, mit dem der Orphan dem Vordergrund-Fenster 35–54 % der GPU genommen hat. Betrifft jeden Hintergrund-Editor, jedes Spiel hinter dem Editor und jeden Collab-Test | S2 1.5, `Application.cpp` Hauptloop | **pro Hintergrundprozess 35–54 % GPU für den Vordergrund zurück (gemessen am Orphan)** | gemessen: 44–47 → 59,5 FPS | S | niedrig. Opt-out-Variable für Test-Harnesse nötig, die mit verstecktem Fenster rendern müssen (`HE_HIDDEN_WINDOW`-Läufe, Screenshots) | **Quick-Win** | beide, alle Backends |
| **B2** | **Job-System ohne Mindest-Körnung, und der Aufrufer schläft in `fut.get()`.** `parallel_for` verteilt 4 Objekte auf 4 Blöcke, 3 davon an den Pool. Der Aufrufer rechnet seinen Block, holt sich die übrigen aber nicht aus der Queue. Pro Frame 27 Jobs à 0,7 µs. Eine FIFO für Frame- und Asset-Jobs, keine Worker-QoS | S3 2, `JobSystem.h:70-98` | ruhig: −0,25 ms CPU. **Unter Last (Load ~10): −6 bis −12 ms p50 CPU** (N1 14,5 → N2 2,5 ms; die Stalls liegen fast nur in extract/cull). −50 Allokationen/Frame | unter Last der größte Einzelposten: bei 36,8 FPS (N1) war die Engine-Arbeit selbst über 16,7 ms | S (Körnung + Aufrufer arbeitet die Queue ab). M (eigene Frame-Queue/Priorität, QoS) | niedrig (Körnung), mittel (Mitarbeiten, Nebenläufigkeit) | **Quick-Win** + Umbau | beide, alle Backends |
| B3 | **Latenz: Die Eingabe ist schon alt, bevor die GPU anfängt.** Poll am Frame-Anfang, dann Encode, dann Warten in `nextDrawable`, erst danach Commit. Die ganze Szene hängt in einem Command-Buffer hinter dem Drawable | S4 3, `MetalRenderer.mm:16034` | **−8 bis −19 ms Eingabealter p50** (p90 31–51 ms). Geschätzte Photonen-Latenz heute 50–90 ms. FPS unverändert | – | M (Warten vor `PollEvents` per Frames-in-flight-Semaphore oder `CAMetalDisplayLink`; Szene vor dem Drawable committen). Backend-Haken in `Application::Run` für alle Backends | mittel (Pipelining, vsync-Verhalten) | Umbau | beide, alle Backends |
| B4 | **Zu viele Render-Encoder und Pass-Deskriptoren pro Frame.** Geschätzt 20–25 Encoder, Bloom allein 11 (Bright + 10 Blur à 9 Taps auf ganzen Texeln). Encoder-Anlage = 21 % der Hauptthread-CPU. Bloom hat unter Konkurrenz das größte Verhältnis Mittel/Minimum (~13×, Y0 4,51 / 0,35 ms). Jede Encoder-Grenze ist eine Umschaltstelle für die GPU | S2 2.6, S3 3.2, 4.2 | CPU: ~0,6 ms/Frame Encoder gesamt (E-Core), Bloom-Anteil geschätzt ~0,3 ms. GPU: Bloom 0,35 ms min, mit Mip-Kette geschätzt −0,15 ms. Unter Last Mittel 2,8–5,9 ms (**Vermutung**, dass weniger Encoder hier am meisten bringen) | +0,4 bis +0,8 (geschätzt) | S (5 bilineare statt 9 Taps, Ergebnis identisch; Deskriptoren wiederverwenden). M (Bloom als Mip-Kette, Optik neu abstimmen) | niedrig (Taps), mittel (Bloom-Optik) | Quick-Win + Umbau | beide, Metal (andere Backends je eigen) |
| B5 | **Vier volle Extracts pro Frame**, jeder mit `propagateTransforms` ohne Dirty-Prüfung. Dazu ein 280-B-AoS-`RenderObject`, von dem der Cull 24 B liest, 5× pro Frame. Foliage-Instanzen werden in jedem Extract einzeln zu `RenderObject`s und pro Frame per `newBufferWithBytes` hochgeladen | S3 5 | hier ~0,15 ms (4 Objekte). **Mit Foliage hochgerechnet ~11 MB AoS-Schreibzugriffe und 40 000 Cull-Objekte pro Frame bei 10 000 Instanzen (nicht gemessen)** | hier 0 | L (einmal extrahieren, `RenderWorld` für Schatten/SSAO/Scene teilen; Bounds als SoA; Foliage als Instanzblock) | mittel (alle Backends lesen `RenderWorld`) | Umbau, **vor einer Foliage-Szene messen** | beide, alle Backends |
| B6 | **Pro Frame neu angelegte `MTLBuffer`** (Instanz-Matrizen, Debug-Linien), 413 Heap-Allokationen und 228 KiB pro Frame auf dem Hauptthread | S3 3.2, 4 | ~0,2 ms CPU (3,8 % + 2,7 % der Hauptthread-CPU). Kein Leck (RSS flach) | +0,5 | S–M (Ring-Buffer pro Frame-Slot) | niedrig | Quick-Win | beide, Metal |
| B7 | **3D-Rauschtextur 64 MiB ohne Mips, `StorageModeShared`.** Die hohen Oktaven tasten weit auseinanderliegende Texel ab (Textur-Cache). `Shared` schließt auf Apple-GPUs die verlustfreie Kompression aus. 64 MiB sind ein Fünftel des gesamten RSS (~320 MB) | S2 2.3 | **unbekannt**, Verdacht auf Cache-Limit im Wolken-Raymarch. Braucht eine Xcode-GPU-Aufnahme | ? | S (`Private` + Blit-Upload), M (Mips) | niedrig | Messung zuerst | beide, Metal |
| B8 | **Editor: Hilfe-Tooltip-Suche linear bei jedem Frame.** `Help::findKey` durchsucht ~1 335 Einträge mit `strlen` pro Vergleich, zweimal pro Toolbar-Zelle, auch ohne Hover | S3 3.2, `EditorToolbar.cpp:55/69`, `EditorHelp.cpp:7105` | ~0,35 ms CPU/Frame (12,4 % der Hauptthread-CPU, E-Core) | +0,9 | S (Hash-Map oder Cache pro Aufrufstelle, Suche erst bei Hover) | niedrig | **Quick-Win** | Editor |
| B9 | Editor-Kleinkram: Bodengitter jeden Frame neu (144 KiB/Frame), Debug-Linien kopiert (39 KiB/Frame), `MacMenuBar::setItemTitle` 3× pro Frame, `isFavorite` mit String-Verkettung | S3 4.2 | −46 Allokationen und ~180 KiB pro Frame, CPU < 0,1 ms | – | S | niedrig | Quick-Win | Editor |
| B10 | **Messwerkzeug: Die Profiler-Aufnahme legt jeden Frame einen `MTLCounterSampleBuffer` an und sampelt an allen Encoder-Grenzen.** Kann auf TBDR die Überlappung der Encoder verhindern. Das betrifft **alle** „normalen“ FPS-Läufe dieses Audits | S2 1.6 #5, S3 3.2 (2,4 % CPU) | Wirkung auf die gemessenen FPS unbekannt, eher pessimistisch | – | S (Schalter „Counter-Sampling aus“, Puffer wiederverwenden) | keins (nur Messung) | Voraussetzung für belastbare Nachmessungen | Messung |

---

## Top 5 als nächste Themen

Reihenfolge nach Verhältnis von belegter Wirkung zu Aufwand. Vor Thema 1 lohnt sich U4 (30 s), sonst fehlt allen
Folgethemen die Vergleichszahl.

1. **Hintergrund-Fenster drosseln (B1, dazu A5).** Verstecktes, verdecktes oder minimiertes Fenster: nicht rendern oder
   selten rendern. Unfokussierter Editor: Frame-Limit. Größte gemessene Wirkung des ganzen Audits, klein, wirkt in allen
   Backends und in Editor und Spiel. Braucht eine Opt-out-Variable für Test-Harnesse.
2. **Job-System: Mindest-Körnung und Aufrufer arbeitet die Queue ab (B2).** Der einzige Posten, der schon in dieser
   Szene zweistellige Millisekunden kostet (unter Rechnerlast). Kern ist klein und risikoarm. Eigene Frame-Queue und
   Worker-QoS als zweite Stufe.
3. **Wolken-Paket (A1 + A2 + A3, danach B7).** Coverage-Schranke korrigieren, Schrittzahl an die Horizont-Ausblendung
   koppeln, Wolkenschatten auf die Empfänger zuschneiden und auf dasselbe Dichtefeld wie die sichtbaren Wolken umstellen.
   Geschätzt 1–2 ms GPU, dazu ein Qualitätsgewinn (Schatten passen zu den Wolken). Jede Änderung mit optischem A/B
   (`he_shot.py`, feste `HE_SKY_TIME`). Anschließend eine Xcode-GPU-Aufnahme, um B7 zu entscheiden.
4. **Latenz-Pipelining (B3).** Warten vor die Eingabeabfrage, Szene vor dem Drawable committen. Keine FPS, aber −8 bis
   −19 ms Eingabealter und ein ruhigeres Kamera-Gefühl im Viewport. Mittlerer Aufwand, weil `Application::Run` einen
   Haken für alle Backends braucht.
5. **Sky-View-LUT (A4).** Himmel nur neu streuen, wenn die Sonne sich bewegt. Geschätzt ~1 ms GPU. Mittlerer Aufwand pro
   Backend, optisches Risiko am Horizont.

**Als Sammel-Quick-Win nebenher** (je unter einem Tag, niedriges Risiko): B8 Hilfe-Suche, B9 Editor-Kleinkram,
B4-Teil „5 bilineare Taps + Deskriptoren wiederverwenden“, B6 Ring-Buffer, B10 Schalter fürs Counter-Sampling.

**Bewusst zurückgestellt:** B5 (einmal extrahieren, SoA) ist der richtige Umbau für Szenen mit Foliage oder vielen
Meshes. In dieser Szene bringt er nichts messbar. Erst eine Foliage-Szene messen (die Hochrechnung in S3 5 ist nicht
gemessen), dann entscheiden.

---

## Korrekturen und Präzisierungen zu Schritt 1–4

- **`parallel_for` (S3 2.2):** Dort steht „Aufrufer hilft nicht mit“. Genauer: Der Aufrufer rechnet den **ersten Block
  selbst** (`JobSystem.h:92`), wartet danach aber in `fut.get()` (`:97`), ohne noch nicht gestartete Blöcke aus der Queue
  zu holen. Der Fehler sind die fehlende Mindest-Körnung und das Schlafen statt Mitarbeiten, nicht ein untätiger Aufrufer.
  Die Messung aus S3 bleibt davon unberührt.
- **Welches Scene-Minimum gilt?** Mit Wolken gibt es drei Werte: L3 3,41 ms (S1, Orphan), Y0 2,57 ms (S2, Orphan),
  C3 4,34 ms (S3, GPU frei, Bildschirm gesperrt). Diese Synthese nimmt die **Y-Serie**, wie S3 1.3 begründet: Ohne Wolken
  sind die Minima in allen drei Serien gleich (1,20 ms). Die Abweichung mit Wolken folgt also dem Zustand der GPU (Takt),
  nicht der Szene. Das niedrigste Minimum kommt der Arbeit bei vollem Takt am nächsten.
- **„GPU-Spanne“ in S1 Tabelle A (33–45 ms) ist keine GPU-Arbeit** (S2 1.2). Die Spannen überlappen und enthalten fremde
  Arbeit. Für Kosten gelten nur die Detailed-Minima.
- **Die Baseline-FPS 44–51 (S1) sind kein Maß für die Engine auf diesem Gerät** (S2 1.6). Sie bleiben als Beleg für den
  Effekt von B1 stehen.

## Offen und Messvorbehalte

1. **On-Screen-FPS ohne Fremdlast.** Nie gemessen, siehe U4. Um 18:02 erneut geprüft: Bildschirm gesperrt, deshalb auch
   in diesem Schritt nicht nachgeholt.
2. **Anomalie N2** (S3 1.2): GPU frei, Engine-Arbeit 2,5 ms, trotzdem 40 FPS und NextDrawable p90 61 ms. Eine halbe
   Stunde vorher (C-Serie) waren es 69–71 FPS. Unter gesperrtem Schirm nicht repräsentativ, aber ungeklärt. Wiederholt
   sich das am entsperrten Schirm, ist es der wichtigste offene Befund, und keiner der Vorschläge oben adressiert ihn.
3. **Counter-Sampling in allen Normal-Läufen** (B10): Die gemessenen FPS sind womöglich pessimistisch.
4. **Stromsparmodus aus / P-Cores:** nicht gemessen (U2).
5. **Limiter der Wolken- und Himmels-Shader** (ALU, Textur, Bandbreite), ISA und Occupancy: Die Metal-Toolchain fehlt
   (`xcodebuild -downloadComponent MetalToolchain`, ~688 MB), eine Xcode-GPU-Aufnahme braucht die GUI. Die Fetch-Zahlen
   sind gezählt, nicht gemessen.
6. **`framebufferOnly = NO`** und die Drawable-Anzahl: unter der Streuung nicht trennbar (S2 1.3).
7. **Runtime (`HorizonGame`), Play-Modus, andere Backends:** nicht gemessen. Die Einstufung „gilt für beide“ beruht auf
   Code-Lesen: Job-System, Extract, Cull, Himmel und Wolken gehören zur Engine.
8. **Szenen mit GI, TAA, SSR, 3D-Wolken, Foliage:** nicht gemessen (U5). Die Hypothesen aus dem Auftrag (DDGI,
   TAA/SSR, Foliage, Worley-Nebel) sind **für diese Szene ausgeschlossen**, weil sie nicht laufen (S2 2.7). Sie sind
   **nicht allgemein widerlegt**.

## Rohdaten und Werkzeuge

Dieser Schritt hat nichts neu gemessen und keinen Code geändert. Alle Zahlen stammen aus den Berichten S1–S4 und deren
Rohdaten (`docs/perf-audit/raw*/`). Messwerkzeuge: `scripts/he_perf_capture.py`, `scripts/he_perf_tables.py`,
`scripts/perf/` (Probe, GPU-Zeit pro Prozess, Alloc-Probe, Frame-Auswertung, Time-Profiler-Export) sowie die Env-Haken
`HE_PROFILE_*` und `HE_PERF_INPUT_EVENTS` in `Application::Run`.
