# Befund: ImGui flackert unter D3D12 bei schneller Mausbewegung (Thema 97, Schritt 2)

Stand: 2026-09-27, Zweig `claude/d3d12-imgui-flicker-fix` auf Basis `152659ff`.

**Nur Code-Lektüre.** Auf dem Mac lässt sich D3D12 weder bauen noch ausführen. Nichts
hier ist reproduziert. Die Hypothesen sind so formuliert, dass sie sich auf einem
Windows-Rechner mit einem einzigen Zahlenwechsel bestätigen oder verwerfen lassen
(siehe „Unterscheidungstest“).

Gelesen:

- `src/HE_Rendering/src/Backends/D3D12/D3D12Renderer.cpp` (Frame-Takt, Fences, `Render()`, Swapchain)
- `src/HE_Editor/EditorApplication.cpp` (ImGui-DX12-Init, SRV-Heap-Allocator, Overlay-Callback, Viewport-SRV)
- `src/HE_Editor/EditorUI.cpp` (`ImGui::Render`, Multi-Viewport)
- `src/HE_Editor/vendor/imgui/backends/imgui_impl_dx12.cpp` (ImGui 1.92.9 WIP)
- zum Vergleich `imgui_impl_dx11.cpp`, `imgui_impl_vulkan.cpp`, `VulkanRenderer.cpp`

## Hauptursache (H1): Renderer hat 3 Frames in flight, ImGui wurde 2 gesagt

| Stelle | Wert |
|---|---|
| `D3D12Renderer.cpp:72` | `static constexpr UINT k_frameCount = 3;` gilt für Swapchain-Puffer **und** Allocators/Fences |
| `D3D12Renderer.cpp:7924` | `scd.BufferCount = k_frameCount;` |
| `EditorApplication.cpp:953` | `dx12Info.NumFramesInFlight = 2;` |

Ablauf in Frame N:

1. `D3D12Renderer::Render()` wartet in `D3D12Renderer.cpp:9396` mit
   `waitForFrame(p.frameIndex)` nur auf den Fence dieses Swapchain-Slots, also auf
   **Frame N-3** (`fenceValues[]` pro Slot, gesetzt in `:9528–9530`, Slotwechsel über
   `GetCurrentBackBufferIndex()` in `:9531`). Frame N-2 und N-1 dürfen danach noch auf der GPU laufen.
2. Der Overlay-Callback (`D3D12Renderer.cpp:9460`, einzige Aufrufstelle) ruft
   `ImGui_ImplDX12_RenderDrawData` (`EditorApplication.cpp:1066`).
3. ImGui wählt seine Vertex-/Indexpuffer mit
   `FrameRenderBuffers[FrameIndex % numFramesInFlight]` (`imgui_impl_dx12.cpp:319–320`),
   also Puffer `N % 2`. Denselben Puffer hat **Frame N-2** benutzt.
4. Die Puffer liegen im Upload-Heap (`:328`, `:349`). ImGui mappt sie und schreibt
   per `memcpy` hinein (`:370–390`). Für das Hauptfenster hat ImGui **keinen eigenen
   Fence**. Es verlässt sich darauf, dass der Aufrufer höchstens `NumFramesInFlight`
   Frames in flight hat (vgl. Kommentar `:316` „We are assuming …“).

Folge: Die CPU überschreibt Vertex- und Indexdaten, die die GPU für Frame N-2 womöglich
noch liest. Frame N-2 zeichnet dann teils mit den Daten von Frame N: falsche Dreiecke,
falsche Farben/UVs, Lücken. Das zeigt sich als kurzes Flackern einzelner Bereiche.

### Warum gerade bei schneller Mausbewegung

- Steht die UI still, schreibt ImGui Frame für Frame **dieselben Bytes** in den Puffer.
  Das Überschreiben eines laufenden Frames ist dann unsichtbar.
- Hover-Hervorhebungen, Tooltips, Slider-Griffe und Cursorwechsel ändern die Geometrie
  **jeden Frame**. Erst dann unterscheiden sich die Daten von N und N-2, und der
  Wettlauf wird sichtbar. Je mehr Steuerelemente die Maus pro Sekunde überstreicht,
  desto öfter.
- Vsync schützt nicht. `Present(1, 0)` (`:9481`) auf einer FLIP_DISCARD-Swapchain ohne
  `SetMaximumFrameLatency`/Waitable Object (im Renderer nicht vorhanden) lässt DXGI bis
  zur Standard-Latenz von 3 Frames vorauslaufen. Mit Vsync aus (`:9483`, `Present(0, …)`)
  läuft die CPU typischerweise weiter voraus. Das sollte das Flackern **verstärken**,
  ist aber keine Voraussetzung.

### Zwei Ausprägungen derselben Ursache

- **H1a, Pufferwachstum = Freigabe eines laufenden Puffers.** Übersteigt `TotalVtxCount`
  oder `TotalIdxCount` die Puffergröße (etwa wenn ein großer Tooltip aufgeht), gibt ImGui
  den alten Puffer mit `SafeRelease` frei (`imgui_impl_dx12.cpp:325`, `:346`), obwohl
  Frame N-2 ihn noch liest. Das Freigeben einer Ressource, die die GPU noch benutzt, ist
  in D3D12 undefiniertes Verhalten: von Müll-Geometrie bis Device-Removed. Gerade Tooltips
  beim Überfahren vieler Steuerelemente lösen Wachstum aus.
- **H1b, Texturen aus dem dynamischen Font-Atlas.** `ImGui_ImplDX12_UpdateTexture` zerstört
  eine Textur, sobald `UnusedFrames >= numFramesInFlight` (`imgui_impl_dx12.cpp:646`),
  also nach 2 Frames. Mit 3 Frames in flight kann Frame N-2 sie noch samplen. Die Textur
  wird freigegeben und ihr SRV-Slot geht über `SrvDescriptorFreeFn` sofort zurück in die
  LIFO-Freiliste (`EditorApplication.cpp:174–179`). Das seltenere Bild, zum Beispiel wenn
  der Atlas nach neuen Glyphen/Größen wächst.

### Gegenprobe: warum D3D11 und Vulkan sauber sind

- **D3D11:** ImGui mappt VB/IB mit `D3D11_MAP_WRITE_DISCARD` (`imgui_impl_dx11.cpp:223`,
  `:225`). Der Treiber benennt den Puffer um, die CPU schreibt nie in Speicher, den die
  GPU noch liest. Das Problem kann dort nicht entstehen.
- **Vulkan:** Der Renderer hat `k_maxFramesInFlight = 2` (`VulkanRenderer.cpp:28`). ImGui
  hält **so viele Puffersätze wie Swapchain-Bilder** (`imgui_impl_vulkan.cpp:600`,
  `wrb->Count = v->ImageCount`, gesetzt aus `vk->GetImageCount()` in
  `EditorApplication.cpp:1000`). ImGui hat dort nie weniger Puffer, als Frames in flight sind.
- **D3D12** ist das einzige Backend, in dem die Zahl von Hand (2) und die Zahl im Renderer
  (3) auseinanderlaufen. Herkunft laut `git log -S`: `k_frameCount` war im Initial-Commit
  `14d1c7d2` noch `2` und wurde in `f8be1e15` („windows parity …“, 2026-06-21) auf `3`
  angehoben, gegen Ruckeln bei Vsync aus (Kommentar `D3D12Renderer.cpp:68–71`).
  `NumFramesInFlight = 2` in `EditorApplication.cpp:953` stammt unverändert aus
  `14d1c7d2` und wurde nicht mitgezogen. Seit `f8be1e15` besteht also das Missverhältnis.

## Nebenhypothese (H2): Viewport-SRV wird im selben Frame freigegeben und neu belegt

`EditorApplication.cpp:3989–4011`: Hat der Renderer das Offscreen-Viewport-RT neu angelegt
(`HasViewportResourceChanged()`), gibt der Editor den alten SRV-Slot frei (`:3993`) und
holt sofort einen neuen (`:4001`). Die Freiliste ist LIFO (`EditorApplication.cpp:170`,
`:177`), also kommt **derselbe Slot** zurück. `CreateShaderResourceView` (`:4011`)
überschreibt einen Deskriptor im shader-sichtbaren Heap, den der vorige, womöglich noch
laufende Frame im Overlay-Draw referenziert.

`createViewportRT` wartet zwar mit `waitForAllFrames()` (`D3D12Renderer.cpp:1547`), aber
**davor**. Der Frame, der danach noch mit dem alten Handle aufgezeichnet und abgeschickt
wird, ist beim Überschreiben des Slots im nächsten Editor-Frame nicht abgesichert.

Auslöser ist ein Viewport-Resize, also das Ziehen eines Dock-Splitters mit der Maus. Das
Bild wäre ein kurz falscher oder leerer Viewport-Inhalt während des Ziehens, nicht
flackernde Buttons. Deshalb nachrangig. Der Renderer hat für seine eigenen Heaps schon das
richtige Muster: verzögerte Slot-Rückgabe über `m_freeSlotPending` (`D3D12Renderer.cpp:9383–9387`).
Die ImGui-Freiliste im Editor hat keines.

## Geprüft und für das Flackern ausgeschlossen

| Verdacht | Befund | Zeilen |
|---|---|---|
| Barriers PRESENT ↔ RENDER_TARGET | vollständig und in der richtigen Reihenfolge um Clear, Szene, UI und Overlay | `D3D12Renderer.cpp:9424–9430`, `:9462–9464` |
| RTV-Format ImGui-PSO vs. Swapchain | beide `DXGI_FORMAT_R8G8B8A8_UNORM` | `EditorApplication.cpp:954`, `D3D12Renderer.cpp:7928` |
| SRV-Heap vor ImGui-Draws gebunden | ja, eigener Heap, dazu Viewport und Scissor | `EditorApplication.cpp:1050–1064` |
| Overlay mehrfach pro Frame (ImGui-`FrameIndex` außer Takt) | einzige Aufrufstelle, genau einmal pro `Render()` | `D3D12Renderer.cpp:9460` |
| Command-Allocator-Wiederverwendung | `Reset()` erst nach `waitForFrame` desselben Slots, eindeutige monotone Fence-Werte | `:9396`, `:9410–9411`, `:9525–9530` |
| Multi-Viewport-Fenster (Tooltips/Popups als OS-Fenster, `ViewportsEnable` an: `EditorApplication.cpp:778`) | ImGui synchronisiert diese Fenster selbst: eigener Fence pro FrameCtx, eigene Swapchain | `imgui_impl_dx12.cpp:1215–1231`, `:1284–1324` |
| Font-Upload (`UpdateTexture`) | eigene Command-List, Signal und blockierender Wait | `imgui_impl_dx12.cpp:632–641` |
| Idle-Drosselung, die erst bei Mausbewegung Frames dicht macht | trifft nicht zu, `setEventDriven(true)` setzt nur `GameApplication` | `GameApplication.cpp:653` |

### Nebenfund, nicht Ursache des Flackerns

Der D3D12-Renderer hat **kein Resize der Haupt-Swapchain**. `ResizeBuffers` kommt in
`D3D12Renderer.cpp` nicht vor, `width`/`height` werden nur bei der Initialisierung gesetzt
(`:7839–7840`). Nach einer Fenstergrößenänderung bleibt die Swapchain auf Startgröße und DXGI
streckt das Bild. ImGui setzt Viewport und Scissor dagegen auf die aktuelle `DisplaySize`
(`EditorApplication.cpp:1055–1064`). Das ergibt unscharfe oder abgeschnittene UI nach Resize,
aber kein sporadisches Flackern bei Mausbewegung. Gehört in ein eigenes Thema.

## Unterscheidungstest für den Windows-Rechner

Ein Zahlenwechsel in `EditorApplication.cpp:953`, sonst nichts:

| `NumFramesInFlight` | Erwartung, falls H1 stimmt |
|---|---|
| `1` | Flackern fast dauerhaft bei jeder Mausbewegung, eventuell Device-Removed beim Pufferwachstum |
| `2` (heute) | sporadisches Flackern wie gemeldet |
| `3` | Flackern verschwindet |

Hinweis für die Reproduktion: Debug-Layer und GPU-Based Validation melden H1 **nicht**.
Ein CPU-Schreibzugriff in einen gemappten Upload-Puffer ist kein API-Fehler. H1a (Release
eines laufenden Puffers) kann unter Debug-Layer als Fehler oder Device-Removed auftauchen,
muss aber nicht. Ein leerer Debug-Layer-Log widerlegt H1 also nicht.

## Richtung für den Fix (Schritt 3, hier nicht umgesetzt)

- Die Frame-Zahl nicht zweimal von Hand pflegen. `k_frameCount` ist ein `static constexpr`
  in der .cpp. Der Renderer sollte sie herausgeben (z. B. `GetFramesInFlight()`), und
  `EditorApplication.cpp:953` übergibt diesen Wert an ImGui. Dann können die beiden Zahlen
  nicht wieder auseinanderlaufen. Das behebt H1, H1a und H1b zugleich, ohne D3D11 oder
  Vulkan anzufassen und ohne die ImGui-Version zu ändern.
- H2 separat: den freigegebenen Viewport-SRV-Slot erst nach `k_frameCount` Frames zurück in
  die Freiliste geben (Muster wie `m_freeSlotPending`), oder den Slot behalten und nur den
  Deskriptor neu schreiben, nachdem alle Frames fertig sind.

## Umsetzung (Schritt 3)

H1 behoben, H2 bewusst nicht angefasst (nachrangig, eigenes Bild beim Splitter-Ziehen).

- `D3D12Renderer.h`: öffentliche Konstante `D3D12Renderer::kFramesInFlight = 3`, mit
  Kommentar zur Race-Condition.
- `D3D12Renderer.cpp`: `k_frameCount = D3D12Renderer::kFramesInFlight`, kein eigenes Literal mehr.
- `EditorApplication.cpp`: `dx12Info.NumFramesInFlight = D3D12Renderer::kFramesInFlight`
  statt der festen `2`, ebenfalls mit Kommentar.

Beide Zahlen haben jetzt eine einzige Quelle. D3D11, Vulkan und die ImGui-Version sind unverändert.

**Nicht gebaut und nicht verifiziert.** Alle geänderten Zeilen liegen hinter `_WIN32` bzw.
`HE_BACKEND_D3D12` und werden auf dem Mac nicht übersetzt. Ein macOS-Bau sagt über diesen
Fix also nichts aus. Geprüft ist nur: Der Header und die beiden Ausdrücke gehen durch
`clang++ -std=c++20 -fsyntax-only` (mit Negativkontrolle per `static_assert`). Offen und nur
auf Windows machbar: Bau mit D3D12, danach der längere manuelle Test (schnelle Mausbewegung
über viele Steuerelemente, Tooltips) aus dem Unterscheidungstest oben.

Für den Unterscheidungstest nach dem Fix: Den Altzustand stellt man her, indem man in
`EditorApplication.cpp` (ImGui-DX12-Init, `dx12Info.NumFramesInFlight = …`) den Ausdruck
`D3D12Renderer::kFramesInFlight` vorübergehend durch `2` bzw. `1` ersetzt. **Nicht** die
Konstante im Header ändern, sonst zieht der Renderer mit und der Vergleich ist wertlos.
