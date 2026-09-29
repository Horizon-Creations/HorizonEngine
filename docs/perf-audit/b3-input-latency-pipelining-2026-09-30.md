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
  `WaitForFrame`.
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

1. **Das Eingabealter beim Commit sinkt um das ganze Warten:** Poll→Commit p50 von 10,9–12,1 ms auf
   1,2–2,2 ms, **p90 von ~65 ms auf unter 3 ms**. Das Warten selbst ist unverändert (NextDrawable p50
   9–10 ms, p90 ~63 ms in beiden Modi), es liegt jetzt nur vor der Eingabe.
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
`addPresentedHandler`-Haken der nächste Messschritt.

**Nicht geprüft:** Das Bild im Fenster (Bildschirm gesperrt, und ein Headless-Screenshot liest das
Offscreen-Target, nicht das Drawable). Der kodierte Frame ist Befehl für Befehl derselbe, nur das
Drawable kommt früher. Live-Resize am Bildschirm und der Play-Modus sind nicht eigens angefahren, der
Größenabgleich in `EncodeFrame` deckt den Resize-Fall ab.

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
