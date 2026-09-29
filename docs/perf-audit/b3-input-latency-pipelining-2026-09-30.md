# Perf B3: Input-zu-Present-Latenz, Drawable vor der Eingabe holen (2026-09-30)

Thema 104, Schritt 1, Zweig `claude/perf-input-zu-present-latenz-pipelining-b3`.
Grundlage: `docs/perf-audit/step4-input-audit-2026-09-27.md`, Abschnitt 3 und Vorschlag 1.

## Was geändert ist

Vorher lag das Warten auf ein freies Drawable (`[CAMetalLayer nextDrawable]`) zwischen der Eingabeabfrage
und dem Commit:

```
vorher:  PollEvents → OnRender → Encode → nextDrawable (wartet) → Present-Pass → commit
nachher: nextDrawable (wartet) → PollEvents → OnRender → Encode → Present-Pass → commit
```

- `IRenderer::WaitForFrame()` (neu, Default leer): „blockiere, bis das Hauptfenster einen Frame annehmen
  kann". Muss idempotent sein.
- `Application::Run` ruft ihn direkt nach `profiler.beginFrame` und **vor** `PollEvents` auf, im Scope
  `WaitForFrame`, aber nur, solange das Hauptfenster nicht `IsInBackground()` ist (versteckt, minimiert,
  verdeckt). Eine unsichtbare Layer gibt kein Drawable heraus, `nextDrawable` liefe dann in jedem Durchlauf
  in seinen 1-s-Timeout, auch in eventgesteuerten Frames, die gar nicht präsentieren (App-Modus des
  Spiels). Dort bleibt die alte Reihenfolge. In keinem Messlauf war das Fenster im Hintergrund (kein
  „Window in background"-Log), ein Kontroll-Lauf mit dem Gate ergab Poll→Commit p50 2,27 / p90 2,71 ms
  bei 41,3 FPS.
- `MetalRenderer::WaitForFrame` gleicht erst `drawableSize` an die Fenstergröße an (wie `EncodeFrame`), holt
  dann `[layer nextDrawable]` unter dem unveränderten Scope `Metal::NextDrawable` und hält es retained
  (`m_heldDrawable`).
- `EncodeFrame` nimmt im Swapchain-Pass das gehaltene Drawable. Passt seine Größe nicht mehr (Resize
  zwischen Holen und Zeichnen), wird es verworfen und frisch geholt, das ist der alte Pfad. Ein
  capture-only-Frame (`RenderSceneImage`) fasst es nicht an. Headless-Dumps, die `Render()` direkt rufen,
  nehmen ohne gehaltenes Drawable ebenfalls den alten Pfad.
- Im eventgesteuerten Modus kann ein Frame nach dem Holen ohne Present enden, dann wandert das Drawable in
  den nächsten Frame (der Haken kehrt sofort zurück, wenn schon eins gehalten wird).
- `Shutdown` gibt das gehaltene Drawable vor dem Layer frei.
- **Schalter:** `HE_MTL_EARLY_DRAWABLE=0` stellt die alte Reihenfolge wieder her (einmal gelesen, das Log
  meldet beim Start `Metal: drawable acquired before the input poll (early)` bzw. `after the encode`).
  Standard ist an.

Es gibt weiterhin keine Frames-in-flight-Semaphore. `nextDrawable` bleibt der einzige Gegendruck, er sitzt
nur an einer anderen Stelle. Der CPU-Vorlauf vor der GPU wird dadurch eher kleiner (die CPU schreibt die
Per-Frame-Daten erst nach dem Warten), nicht größer.

## Messung (gemessen)

Release, Metal, Editor Edit-Modus, Landscape-Szene (`scenes/landscape.hescene`, `--cam 0,25,90,0,-0.25`),
300 Warmup + 600 Frames, Kopie des Testprojekts unter `/tmp/b3proj`. Ein Build, A/B nur über
`HE_MTL_EARLY_DRAWABLE`. Zwei Durchgänge, der zweite in umgekehrter Reihenfolge (spät zuerst).

Bedingungen: Bildschirm **gesperrt** (`CGSSessionScreenIsLocked=Yes`), die FPS sind also nicht die am
Bildschirm. Stromsparmodus aus. Keine fremde Compile-Last während der Läufe (Build des Wolken-Arbeiters
abgewartet, Load 5,5 → 2,0 fallend, oberster Fremdprozess < 20 % CPU).

„Poll→Commit" ist pro Frame `PollEvents + OnRender + Render` (Render endet mit dem Commit), dasselbe Maß
wie Zeile (b) im Input-Audit, aber pro Frame summiert und dann das Perzentil genommen.

| Lauf | Modus | vsync | FPS | GPU p50 | Frame p50 | NextDrawable p50 / p90 | OnRender p50 | Render p50 / p90 | Poll→Commit p50 / p90 |
|---|---|---|---|---|---|---|---|---|---|
| run1 | spät | keep | 39,5 | 15,33 | 11,76 | 9,87 / 62,94 | 1,06 | 10,89 / 64,12 | **11,78 / 65,25** |
| run1 | früh | keep | 40,9 | 15,85 | 11,71 | 9,76 / 62,39 | 1,00 | 1,05 / 1,26 | **2,11 / 2,67** |
| run2 | spät | keep | 40,9 | 14,00 | 10,91 | 9,08 / 63,45 | 1,03 | 10,06 / 64,49 | **10,90 / 65,39** |
| run2 | früh | keep | 38,9 | 14,43 | 10,48 | 9,06 / 63,53 | 1,06 | 1,08 / 1,27 | **2,22 / 2,73** |
| run1 | spät | off | 40,1 | 14,16 | 12,12 | 10,29 / 63,04 | 1,02 | 11,39 / 64,18 | **12,13 / 65,07** |
| run1 | früh | off | 43,6 | 14,45 | 12,19 | 10,38 / 62,38 | 0,56 | 0,60 / 0,82 | **1,22 / 1,72** |
| run2 | spät | off | 41,1 | 15,82 | 11,15 | 9,55 / 63,03 | 0,98 | 10,34 / 63,93 | **11,61 / 64,79** |
| run2 | früh | off | 41,5 | 15,08 | 11,72 | 9,54 / 62,60 | 1,07 | 1,08 / 1,29 | **2,24 / 2,74** |

(alle Zeiten in ms)

**Ergebnis:**

1. **Das Eingabealter beim Commit sinkt um das ganze Warten, pro Frame gezählt:** Poll→Commit p50 von
   10,9–12,1 ms auf 1,2–2,2 ms, **p90 von ~65 ms auf unter 3 ms**. Das Warten selbst ist unverändert
   (NextDrawable p50 9–10 ms, p90 ~63 ms in beiden Modi), es liegt jetzt nur vor der Eingabe.
   *Einschränkung aus Schritt 2 (unten):* Pro Event gezählt, also „wie lange wartet eine beliebige
   Eingabe, bis ein Frame sie committet", sind es bei diesem Takt nur **~2–3 ms**. Das Maß pro Frame
   überträgt sich hier nicht auf das Eingabealter pro Event.
2. **Kein Durchsatzverlust:** FPS (38,9–43,6 früh gegen 39,5–41,1 spät), GPU p50 und Frame p50 liegen
   in beiden Modi in derselben Streuung. Die ~2 ms, die das Drawable jetzt länger gehalten wird, schluckt
   die Drei-Drawable-Warteschlange.
3. **Positivkontrolle:** Jedes Log meldet den gewählten Modus. Im frühen Modus ist die Summe von
   `Metal::NextDrawable` gleich `WaitForFrame`, das späte Fallback-Holen in `EncodeFrame` hat also nicht
   gefeuert, und `Render` enthält kein Warten mehr (p90 1,3 ms statt 64 ms).

**Hergeleitet, nicht gemessen:** Die Photonen-Latenz. Mit gesperrtem Bildschirm gibt es kein
belastbares `presentedTime`. Nach der Zerlegung des Audits (a + b + c + d) fällt Anteil (b) um
das Warten, also um die gemessenen **~9–10 ms im Median und ~60 ms im p90** in dieser Lage. Die
Audit-Schätzung (8–19 ms p50) passt dazu. Am entsperrten Bildschirm ohne Fremdlast wäre ein
`addPresentedHandler`-Haken der nächste Messschritt. (Schritt 2 unten zeigt, dass diese Rechnung pro
Frame gilt. Pro Event gemessen sind es bei gesperrtem Bildschirm nur 2–3 ms, bei gleichmäßigem Takt
wären ~W zu erwarten, siehe dort.)

**Nicht geprüft:** Das Bild im Fenster (Bildschirm gesperrt, und ein Headless-Screenshot liest das
Offscreen-Target, nicht das Drawable). Der kodierte Frame ist Befehl für Befehl derselbe, nur das
Drawable kommt früher. Live-Resize am Bildschirm und der Play-Modus sind nicht eigens angefahren, der
Größenabgleich in `EncodeFrame` deckt den Resize-Fall ab.

## Schritt 2: Vollbau, Tests, Eingabealter pro Event

Zweig wie oben, Stand `c6dbf123` (Schritt 1 + Messhaken).

**Vollbau und Tests (gemessen):** `ninja -j8` im Release-Baum, alle 497 Ziele, rc 0, zweiter Lauf „no work
to do". Danach der Messhaken und inkrementell neu gebaut (rc 0). Volle Suite wie in CI
(`ctest --output-on-failure -j4`) auf diesem Stand: **220/220 bestanden**, davon die zwei
`runtime_size_app_*` wie in CI absichtlich übersprungen, 209 s.

### Messhaken `HE_PERF_INPUT_LATENCY_HZ=N`

Der Injektor aus dem Input-Audit (`HE_PERF_INPUT_EVENTS`) schiebt seine Events direkt vor `PollEvents` ein.
Sie sind beim Abholen nie alt, er misst Verarbeitungskosten, kein Eingabealter. Deshalb ein zweiter Haken
in `Application.cpp`: Ein eigener Thread schiebt N Events pro Sekunde (hier 1000, wie eine 1-kHz-Maus) mit
SDLs Zeitstempel (`SDL_GetTicksNS`) in die Queue. Sie landen also mitten im Drawable-Warten oder im Encode
und warten dort wie echte Eingaben. Ein eigener registrierter Event-Typ, der im Event-Callback abgefangen
wird, ImGui und `Input` sehen ihn nie. Pro Event wird verbucht: Ankunft → Abholung in `PollEvents`
(**Event→Poll**), Abholung → Commit des Frames nach `Render`/`SwapBuffers` (**Poll→Commit**) und die
Summe (**Event→Commit**). Gezählt ab `HE_PROFILE_WARMUP`, Zusammenfassung im Log alle 600 Frames und beim
Beenden. Ohne die Variable aus, mit `HE_PERF_INPUT_EVENTS` zusammen abgeschaltet (Warnung im Log).

Positivkontrolle: Jedes Log hat die Startzeile, die Modus-Zeile von Schritt 1 und die Abschlusszeile mit
~15 000 Events über 602 Frames bei **996–1000 Hz** erreichter Rate.

### Messung (gemessen)

Gleiche Lage wie Schritt 1: Release, Metal, Editor Edit-Modus, Landscape, 300 Warmup + 600 Frames, ein
Build, A/B nur über `HE_MTL_EARLY_DRAWABLE`, zweiter Durchgang umgekehrt. Bildschirm **gesperrt**,
Stromsparmodus aus. Gemessen erst, nachdem Build und `he_shot`-Serie des Wolken-Arbeiters 60 s geruht
hatten, Load 1,2–2,3 fallend, kein Fremdprozess über 5 % CPU. Zwischen `lat-early1-vsyncoff-run1` und
`lat-early0-vsyncoff-run1` sah die Stichprobe einmal einen Fremdprozess (vermutlich ein `he_shot` des
Wolken-Arbeiters an der Laufgrenze), beide Läufe liegen aber in der Streuung der anderen.

| Lauf | Events | Event→Commit p50 / p90 / p99 | Event→Poll p50 / p90 | Poll→Commit p50 / p90 | FPS |
|---|---|---|---|---|---|
| spät, keep, run1 | 15333 | 26,54 / 59,23 / 112,02 | 18,71 / 53,77 | 2,11 / 2,39 | 39,8 |
| spät, keep, run2 | 15701 | 26,42 / 57,95 / 68,54 | 19,80 / 52,95 | 2,12 / 2,45 | 38,5 |
| früh, keep, run1 | 15359 | **20,87** / 56,79 / 70,76 | 18,66 / 54,52 | 2,18 / 2,37 | 39,7 |
| früh, keep, run2 | 15593 | **26,13** / 55,01 / 71,91 | 24,08 / 52,76 | 2,12 / 2,31 | 39,2 |
| spät, off, run1 | 14842 | 24,72 / 54,32 / 74,93 | 18,90 / 49,52 | 2,21 / 2,67 | 41,0 |
| spät, off, run2 | 14470 | 25,43 / 56,59 / 70,50 | 18,23 / 51,07 | 2,27 / 3,78 | 42,2 |
| früh, off, run1 | 14907 | **22,63** / 53,47 / 67,30 | 20,65 / 51,32 | 2,12 / 2,33 | 41,0 |
| früh, off, run2 | 14310 | **23,13** / 53,83 / 88,26 | 20,84 / 51,75 | 2,13 / 2,34 | 42,5 |

(ms, pro Event; FPS aus dem Profiler)

**Ergebnis pro Event:** Event→Commit p50 früh 20,9–26,1 gegen spät 26,4–26,5 (vsync keep) und früh
22,6–23,1 gegen spät 24,7–25,4 (vsync off), also **~2–3 ms weniger**. p90 früh 53,5–56,8 gegen spät
54,3–59,2, ebenfalls **~2–3 ms**. Die Streuung ist so groß wie der Effekt: Die zwei frühen keep-Läufe
liegen 5 ms auseinander. Das p99 von 112 ms in einem späten Lauf ist ein Einzelausreißer, darauf stützt
sich nichts. FPS und Durchsatz wie in Schritt 1 unverändert.

### Warum pro Event so viel weniger als pro Frame

Auffällig ist **Poll→Commit pro Event ~2,1–2,3 ms in beiden Modi**, obwohl `Render` im späten Modus im
Mittel 23–24 ms dauert und das ganze Warten enthält. Die Einzelframes (`lat_analysis.py` auf den
`*.profile.json`) erklären das:

- Die Drawable-Wartezeit W wechselt lang/kurz ab: **Lag-1-Korrelation −0,69 bis −0,78**. Nach einem
  Warten über 30 ms wartet der Folgeframe im Median **0,0 ms**.
- **48 %** der Frames beginnen weniger als 8 ms nach dem vorigen, 23–29 % erst nach über 50 ms. Bei einem
  angezeigten Fenster mit einem Drawable pro 60-Hz-Refresh ginge das nicht. Der gesperrte WindowServer
  gibt die Drawables schubweise frei.
- Ein Event wird von dem Frame abgeholt, der nach seiner Ankunft beginnt. Die Events stauen sich während
  des langen Wartens an und gehen in den Folgeframe, der kaum wartet. Die Frames mit langem Warten tragen
  dagegen fast keine Events (sie folgen einem kurzen Frame). Pro Event spart das frühe Holen also das
  Warten **des abholenden Frames**, und das ist meist ~0.
- **Gegenprobe:** Aus dem Frame-Verlauf der späten Läufe vorhergesagt (Event im Intervall vor Frame i
  spart W[i], gewichtet mit der Intervalllänge) ergibt sich eine mittlere Ersparnis von **2,5–3,1 ms pro
  Event**. Gemessen sind ~2–3 ms. Haken und Modell stimmen überein, der Haken misst also, was er soll.

**Was daraus folgt:**

1. Schritt 1s Satz „das Eingabealter sinkt um das ganze Warten" gilt **pro Frame** (was jedes angezeigte
   Bild an Eingabe enthält). Für das Eingabealter pro Event gilt er bei diesem Takt nicht, dort sind es
   2–3 ms.
2. **Hergeleitet, nicht gemessen:** Bei gleichmäßigem Takt (jeder Frame wartet ungefähr gleich lang, wie
   bei einem angezeigten Fenster mit vsync) laufen beide Maße zusammen, die Ersparnis pro Event wäre dann
   ≈ W, also die 8–10 ms p50 aus dem Audit. Das lässt sich nur am **entsperrten Bildschirm** zeigen. Dort
   bräuchte es denselben Haken plus `addPresentedHandler` (`presentedTime`).
3. Die Änderung bleibt richtig: kein Durchsatzverlust, pro Frame deutlich frischere Eingabe, pro Event
   auch bei ungünstigem Takt ein kleiner Gewinn. p50 und p90 sind in keinem frühen Lauf schlechter
   als in einem späten derselben vsync-Einstellung. Das p99 streut in beide Richtungen (früh bis 88,
   spät bis 112 ms) und trägt keine Aussage.

Nachmessen:

```sh
R --label lat-early1-vsynckeep-run1 --vsync keep --env HE_MTL_EARLY_DRAWABLE=1 --env HE_PERF_INPUT_LATENCY_HZ=1000
R --label lat-early0-vsynckeep-run1 --vsync keep --env HE_MTL_EARLY_DRAWABLE=0 --env HE_PERF_INPUT_LATENCY_HZ=1000
grep "HE_PERF_INPUT_LATENCY (final)" docs/perf-audit/raw-b3/lat-*.log
python3 docs/perf-audit/raw-b3/lat_analysis.py   # Tabelle + Takt-Auswertung (braucht die *.profile.json)
```

Rohdaten: `docs/perf-audit/raw-b3/lat-*.log` (mit `git add -f` eingecheckt, `*.log` ist global ignoriert)
und `lat-*.summary.json`. Die `lat-*.profile.json` (je ~5 MB) sind wie in Schritt 1 nicht eingecheckt,
ohne sie druckt `lat_analysis.py` nur die Tabelle. Die Logs von Schritt 1 (`early*.log`) sind wegen
derselben Regel nie im Repo gelandet, nur deren `summary.json`.

## Übertragbarkeit auf die anderen Backends (Befund, nicht umgesetzt)

| Backend | Wo es heute wartet | Was der Haken dort täte | Aufwand |
|---|---|---|---|
| **Vulkan** | `VulkanRenderer::Render` beginnt mit `vkWaitForFences(m_frameFence[fi])` + `vkAcquireNextImageKHR` + Warten auf `m_imagesInFlight[img]` (VulkanRenderer.cpp:436–453), also nach PollEvents/OnRender | Die drei Schritte in `WaitForFrame` ziehen, `imageIndex` bis `Render` halten. Falle: `VK_ERROR_OUT_OF_DATE_KHR` → `recreateSwapchain` muss dann im Haken passieren, und ein gehaltenes Image darf einen Resize nicht überleben. Vulkan wartet zwar schon vor dem Kodieren, aber immer noch nach PollEvents/OnRender, der Gewinn ist also wieder das ganze Warten (auf Hardware nicht gemessen). | klein bis mittel |
| **D3D12** | `waitForFrame(frameIndex)` am Anfang von `Render` (D3D12Renderer.cpp:10324), aber das eigentliche vsync-Warten steckt in `Present()` (Flip-Discard, 3 Buffer, ohne Waitable Object) am Frame-Ende | Nur `waitForFrame` vorzuziehen hilft wenig. Richtig ist `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` + `SetMaximumFrameLatency` und `WaitForSingleObjectEx(GetFrameLatencyWaitableObject())` im Haken, das ist genau das Muster, für das DXGI den Waitable gebaut hat. Swapchain-Flags müssen bei `ResizeBuffers` gleich bleiben. | mittel |
| **D3D11** | `Present()` blockiert (D3D11Renderer.cpp:6766). Swapchain im alten Blt-Modell: `BufferCount 1`, `DXGI_SWAP_EFFECT_DISCARD` (Z. 5362/5372) | Braucht erst den Wechsel auf Flip-Modell (`FLIP_DISCARD`, ≥ 2 Buffer), dann wie D3D12 den Waitable. Der Flip-Wechsel berührt RTV-Handling und Resize. | mittel bis groß |
| **OpenGL** | `SDL_GL_SwapWindow` in `Window::SwapBuffers` nach `Render`, der Treiber blockiert dort | Kein Drawable/Image zum Vorholen. Näherung: Fence nach dem Swap, im Haken `glClientWaitSync` auf die Fence des Vorframes. Auf macOS-GL (4.1, veraltet) wenig lohnend, unter Windows/Linux treiberabhängig. | groß, unsicherer Gewinn |
| Software | CPU-Blit, kein Warten | nichts | – |

Empfehlung für einen Folgeschritt: Vulkan zuerst (mechanisch, Haken ist schon da), dann D3D12 mit
Waitable Object. D3D11 und GL nur, wenn die Latenz dort gemessen stört.

## Nachmessen

```sh
# Build: cmake-build-release, ninja -j8 HorizonEditor
R() { python3 scripts/he_perf_capture.py --project /tmp/b3proj/Test/Test.heproj --out docs/perf-audit/raw-b3 \
        --cfgdir /tmp/b3_cfg --cam 0,25,90,0,-0.25 --scene docs/perf-audit/scenes/landscape.hescene "$@"; }
R --label early1-vsyncoff-run1 --env HE_MTL_EARLY_DRAWABLE=1
R --label early0-vsyncoff-run1 --env HE_MTL_EARLY_DRAWABLE=0
```

`--scene` überschreibt die Startszene des Projekts, deshalb eine Kopie nehmen. Rohdaten:
`docs/perf-audit/raw-b3/*.summary.json` und `*.log` (die `*.profile.json` mit den Einzelframes sind
38 MB und nicht eingecheckt, Poll→Commit wurde aus ihnen berechnet).
