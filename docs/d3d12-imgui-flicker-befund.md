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

## Verifikation auf echter Hardware (Schritt 4)

Stand: 2026-09-30, NN-WS03 (Ryzen 9 9950X, RTX 4070, Anzeige 50 Hz), Zweig auf `a1411f3e`,
Release-Bau in eigenem Baum (`-B C:/hw97 -DDEPLOY_DIR=C:/hw97/deploy`), eigene `APPDATA`,
Kopie eines Testprojekts (Szene mit Terrain, Himmel, Baum).

### Vollbau

`cmake --build C:/hw97 -j8` über alle Ziele: 0 Fehler. Gebaut sind `RendererD3D11`,
`RendererD3D12`, `RendererVulkan` (dazu OpenGL/Software), `HorizonEditor`, `he_tests`.
Die Prüfung der eingebetteten Shader meldet: „All 93 embedded shaders compile.“ In den geänderten
Dateien gibt es keine Compiler-Warnung.

### Messverfahren

Einem Menschen beim Hinsehen zuzuschauen geht in einer Agenten-Sitzung nicht. Die echte Maus
zu bewegen (SendInput) kam nicht in Frage, weil der Mensch zur selben Zeit an der Konsole
arbeitete (Leerlauf < 1 s). Also wurde gezählt statt geschaut. Die Instrumentierung war
**vorübergehend** und steckt nicht im Code des Zweigs. Zum Wiederholen liegt sie als
`docs/d3d12-imgui-flicker-messung.patch` bei (`git apply`, nur lokal, nicht mergen):

- `HE_T97_NFIF=<n>` überschreibt `dx12Info.NumFramesInFlight`, und nur diesen Wert. Der
  Renderer bleibt bei 3. So entsteht der Altzustand wie oben beschrieben.
- **Zähler:** Im Overlay-Callback wird vor `ImGui_ImplDX12_RenderDrawData` ein eigener Fence
  mit dem Wert `k` auf die Queue signalisiert (Frame `k`). Diese Signalisierung steht in der
  Queue vor dem `ExecuteCommandLists` von Frame `k`. Hat der Fence also mindestens den Wert `k`
  erreicht, sind die Frames `0..k-1` fertig. ImGui schreibt in Slot `k % N`, den zuletzt Frame
  `k-N` benutzt hat. Gilt `completed < k-N+1`, ist das eine **Überschreibung während des Flugs**.
  Ist dabei zusätzlich der FNV-Hash über VB+IB anders als der von Frame `k-N`, ist sie
  **gefährlich**: Die GPU kann andere Bytes lesen, als ihre Draw-Befehle erwarten. Nur solche
  Überschreibungen können Flackern erzeugen.
- **Reiz:** `HE_T97_SWEEP=1` speist vor `ImGui::NewFrame()` jeden Frame per `io.AddMousePosEvent`
  eine Mausposition ein. Sie springt 97 px weiter und läuft zeilenweise über das ganze
  Editorfenster (Toolbar, Slider, Outliner, Content Browser). ImGui kann das nicht von einer
  echten Maus unterscheiden. Dass Hover wirklich greift, zeigt eine Burst-Aufnahme: Die Zeile
  „Prefabs“ im Content Browser ist in einem Bild hervorgehoben und im nächsten nicht mehr.
  Gepostete `WM_MOUSEMOVE` reichen dagegen **nicht**. SDL3 registriert `TME_LEAVE`, und weil
  der echte Cursor außerhalb liegt, kommt sofort ein `WM_MOUSELEAVE` hinterher.

### Ergebnis: Vorher / Nachher (D3D12, Hauptfenster)

| NFIF | Vsync | Reiz | Frames | Überschr. im Flug | davon gefährlich |
|---|---|---|---|---|---|
| 1 (Negativkontrolle) | an | Sweep | 1 800 | 1 799 (100 %) | 246 |
| 1 (Negativkontrolle) | aus | Sweep | 17 100 | 17 099 (100 %) | 3 022 |
| **2 (Stand vor dem Fix)** | an | Sweep | 1 800 | 1 176 (65 %) | **246** |
| 2 | an | keiner | 1 800 | 1 218 (68 %) | 44 |
| **2 (Stand vor dem Fix)** | aus | Sweep | 24 900 | 21 506 (86 %) | **5 722** |
| **3 (Fix)** | an | Sweep | 1 800 | **0** | **0** |
| **3 (Fix)** | aus | Sweep | 25 500 | **0** | **0** |
| **3 (Fix, ohne Override)** + Debug-Layer | an | Sweep, 3,2 min | 9 600 | **0** | **0** |

- Die Reihenfolge 1 > 2 > 3 = 0 ist die, die H1 vorhersagt. Der Wettlauf ist echt, und der Fix
  schließt ihn.
- In beiden Sweep-Läufen ändert sich der Inhalt ähnlich oft (28 bis 54 von 300 Frames mit
  anderem VB/IB-Hash). Der Fix verdeckt also nicht den Reiz, er beseitigt die Ursache.
- Der Sweep verfünffacht die gefährlichen Überschreibungen gegenüber der ruhenden UI (246 gegen
  44). Das passt zu „besonders bei schneller Mausbewegung“. Ohne Reiz ändern sich nur FPS-Text
  und Ähnliches.
- Der **D3D12-Debug-Layer** (`HE_GPU_DEBUG=1`) meldet auch bei NFIF=1 mit 3 022 gefährlichen
  Überschreibungen nichts dazu. Er zeigt nur den bekannten Performance-Hinweis
  `ClearRenderTargetView: clear values do not match`. Die Vorhersage oben stimmt also: Ein
  stiller Debug-Layer widerlegt H1 nicht.
- **H1a** (Pufferwachstum) wurde im Test nicht ausgelöst. Nach der Startphase blieb die Vertexzahl
  zwischen 6 048 und 6 198 und damit unter der Puffergröße (TotalVtx + 5 000 Reserve). Gewachsen
  ist der Puffer nur beim Aufbau der UI in den ersten Frames.

### Was nicht gelang: das Flackern selbst im Bild einfangen

80 PrintWindow-Aufnahmen (etwa 12 pro Sekunde bei rund 600 fps, NFIF=1, Vsync aus) zeigen
keine kaputte Geometrie, nur saubere Hover-Paare. Das Verfahren ist dafür zu grob. DWM liefert
den zuletzt komponierten Frame, ein falscher Frame steht bei 600 fps rund 1,7 ms. Die meisten
gefährlichen Überschreibungen ändern nur Farben (Hover), nicht das Layout. Das Ergebnis sagt
also nichts gegen den Fix, belegt aber auch nicht das sichtbare Flackern. **Offen ist deshalb
der längere Blick eines Menschen auf den D3D12-Editor** mit echter Maus. Der gemessene
Mechanismus ist behoben.

### Andere Backends und Tests

- Der saubere Deploy (Instrumentierung zurückgenommen, `git status` sauber, im Exe steht kein
  `T97` mehr) startet auf **D3D12, D3D11, Vulkan und OpenGL** mit Projekt. Jedes Backend
  initialisiert ImGui für sich, läuft 15 s ohne Absturz, und der Screenshot zeigt die normale UI.
  Vulkan hat nur Validierungsmeldungen: die bekannten zu Barrier im Subpass, `vkCmdUpdateBuffer`
  im Renderpass und nicht deklarierten Deskriptoren, dazu einmal in Frame 3
  `vkFreeDescriptorSets … currently in use`. Der Zweig ändert keinen Vulkan-Code (Diff
  `152659ff..a1411f3e`: nur D3D12-Renderer, der D3D12-Zweig in `EditorApplication.cpp` und
  Doku), die Meldung kommt also nicht vom Fix. Vom Muster her ähnelt sie H2 (Deskriptor
  freigegeben, solange er noch benutzt wird).
- `he_tests.exe` (Release, eigene `APPDATA`): **3 742 / 3 742 Testfälle, 506 140 Assertions,
  0 Fehler.**
- `ctest --test-dir C:/hw97 -j8` (eigene `APPDATA`): **100 % von 198 bestanden.**
  `runtime_size_app_advanced` und `runtime_size_app_basic` melden sich selbst als „Skipped“.

### Stolperfalle im Testaufbau, kein Produktfehler

`HE_DUMP_RHI=D3D12` **ohne** Dump-Modus stürzt beim ersten Frame ab (0xC0000005, Sprung auf
Adresse 0). Das gilt für D3D11 und D3D12, mit und ohne Fix. `EditorApplication.cpp` setzt am Ende
von `OnInit` (`m_backend = m_globalState->getSelectedRHI()`) das Backend auf den Wert aus der
Config zurück, im Test OpenGL. Der Overlay-Callback ruft dann `ImGui_ImplOpenGL3_RenderDrawData`
ohne GL-Kontext auf. Belegt ist das per Minidump: Die Rücksprungadresse zeigt auf
`glGetIntegerv(GL_ACTIVE_TEXTURE)` bzw. `glActiveTexture(GL_TEXTURE0)`. Wer das Backend von
außen erzwingen will, schreibt es als `"RHI": 3` in die `config.json` (0 GL, 1 Vulkan, 2 D3D11,
3 D3D12). Benutzer, die D3D12 in der Config gewählt haben, betrifft das nicht.

## Flackern im Bild bestätigt (Schritt 5)

Stand: 2026-09-30, NN-WS03 (RTX 4070, Anzeige 50 Hz), gleicher Release-Baum `C:/hw97`,
gleiche Szene (Testie) und `APPDATA` wie in Schritt 4.

**Ergebnis: Vor dem Fix zeigt der Editor unter D3D12 bei schneller Mausbewegung einzelne Frames
mit kaputter ImGui-Geometrie, die im nächsten Frame wieder weg sind. Mit dem Fix kein einziger
solcher Frame.** Das offene „Flackern selbst im Bild einfangen“ aus Schritt 4 ist damit erledigt.

### Verfahren: jeder Frame gegen eine rennfreie Referenz

PrintWindow war zu langsam (Schritt 4). Deshalb wird jetzt **jeder** Frame auf der GPU selbst
aufgenommen, im Overlay-Callback und in derselben Command-List:

1. Der Backbuffer wird unmittelbar vor ImGui in eine eigene Textur kopiert (Szene, Viewport-Bild
   und Clear, also alles unter ImGui).
2. ImGui zeichnet wie immer in den Backbuffer, aus seinem eigenen Ring (dem mit dem Wettlauf).
   Diese Zeichnung bleibt unverändert.
3. Dieselbe `ImDrawData` wird ein zweites Mal in die Kopie gezeichnet, aus einem privaten Ring mit
   16 Puffern, den nie ein laufender Frame benutzt. `vd->FrameIndex` bleibt dabei unberührt, der
   Wettlauf wird also nicht verschoben. Ergebnis: die **Referenz**, also so, wie der Frame aussehen
   müsste.
4. Beide Bilder gehen in einen Readback-Ring. Sobald der Frame laut eigenem Fence fertig ist,
   werden sie pixelgenau verglichen. Bei einer Abweichung werden Hauptbild, Referenz, Diff-Maske
   sowie Vorgänger- und Nachfolgerbild gespeichert (die ersten 6 je Lauf).

Gleiche GPU, gleiche PSO, gleiche Bytes: Die beiden Bilder müssen bitgleich sein. Weicht ein
Pixel ab, hat die GPU für diesen Frame andere Vertex- oder Indexdaten gelesen, als ImGui
aufgezeichnet hatte. Das ist genau das, was der Mensch als Flackern sieht, denn der Backbuffer
wird so präsentiert. Reiz wie in Schritt 4: `HE_T97_SWEEP=1`, jeden Frame 97 px weiter über das
ganze Fenster.

**Kontrollen, bevor irgendeine 0 zählt:**

- *Nullprobe:* Im Fix-Stand müssen Hauptbild und Referenz bitgleich sein. Ergebnis: 2 994 von 2 994
  Frames ohne Positivkontrolle sind bitgleich.
- *Positivkontrolle:* `HE_T97_SCRIBBLE=500` verschiebt in jedem 500. Frame die ersten 600 Vertices
  des echten Draws nach dem Upload um 40 px. Ergebnis: **genau diese 6 Frames** (250, 750, …, 2 750)
  werden gemeldet (`scribbleHit=6`, `scribbleMiss=0`), sonst keiner. Der Detektor sieht also
  verfälschte ImGui-Geometrie und meldet nichts, was nicht da ist.
- *Gegenprobe zu Schritt 4:* Jede Abweichung wird mit dem Fence-/Hash-Zähler aus Schritt 4
  abgeglichen („Opfer“ = Frame, dessen Puffer-Slot ein späterer Frame mit anderen Bytes überschrieb,
  solange er noch lief). Bei NFIF=2 sind **alle** abweichenden Frames solche Opfer (55/55, 670/670,
  190/190, 635/635). Das Bild und die Zählung zeigen also denselben Mechanismus.

### Ergebnis (D3D12, Hauptfenster, Sweep)

„Vor dem Fix“ ist hier **der echte Commit `7daffeb5`** (Elter von `47c273c8`), neu gebaut mit der
Instrumentierung obendrauf. Gegenüber `7daffeb5` kommen in den drei Fix-Dateien nur Zeilen hinzu,
es fehlt keine: `k_frameCount = 3` im Renderer und `NumFramesInFlight = 2` im Editor stehen dort
wie im Original. Das Log bestätigt `T97 ImGui NumFramesInFlight=2 (pre-fix build 7daffeb5)`. Zum
Vergleich laufen auch die Overrides aus Schritt 4 auf dem Fix-Stand (`HE_T97_NFIF`, nur ImGui,
Renderer bleibt bei 3).

| Stand | Vsync | Frames | sichtbar kaputte Frames | davon Race-Opfer | ≥ 10 000 px (ab Frame 300) |
|---|---|---|---|---|---|
| **`7daffeb5` (vor dem Fix)** | an | 3 000 | **190** (6,3 %) | 190 | 38 |
| **`7daffeb5` (vor dem Fix)** | aus | 15 000 | **635** (4,2 %) | 635 | 148 |
| Fix-Stand, NFIF=2 (Override) | an | 3 000 | 55 (1,8 %) | 55 | 13 |
| Fix-Stand, NFIF=2 (Override) | aus | 15 000 | 670 (4,5 %) | 670 | 149 |
| Fix-Stand, NFIF=1 (Negativkontrolle) | an | 3 000 | 529 (18 %) | 463¹ | 99 |
| **Fix (`a1411f3e`+, ohne Override)** | an | 2 994² | **0** | – | 0 |
| **Fix (`a1411f3e`+, ohne Override)** | aus | 15 300 | **0** | – | 0 |

¹ Bei NFIF=1 kann auch ein Frame zwei Schritte später denselben Slot treffen; der Opfer-Ring
merkt sich nur den direkten Vorgänger. ² 3 000 Frames, davon 6 absichtlich verfälscht
(Positivkontrolle), alle 6 gefunden.

- Die Reihenfolge NFIF 1 > 2 > 3 = 0 gilt jetzt auch im Bild. Wie hoch die Rate vor dem Fix ist,
  schwankt von Lauf zu Lauf: 55 bzw. 190 bei Vsync an, obwohl die gezählten gefährlichen
  Überschreibungen fast gleich waren (674 bzw. 684). Vermutlich entscheidet das Timing, wann die
  GPU den Draw tatsächlich ausführt, ob eine Überschreibung schon im Bild landet; bewiesen ist das
  nicht. Die Rate liegt aber in jedem Lauf deutlich über 0. Der Fix gibt in 18 294 Frames 0.
- **So sieht es aus** (Vsync an, 50 Hz, vor dem Fix, unter Capture-Last): ab Frame 300, also in
  rund 54 s Sweep, alle 1,4 s (`7daffeb5`, 38 Fälle) bzw. alle 4 s (Override, 13 Fälle) ein grober
  Fehlframe mit mindestens 10 000 falschen Pixeln, dazwischen kleinere. Ohne Capture ist das
  wahrscheinlich seltener (siehe Grenzen). Median der Abweichung: 2 600 bis
  4 500 px pro kaputtem Frame. Meist steht der Fehler genau einen Frame: Nur 22 von 190 kaputten
  Frames haben einen kaputten Nachfolger. Das ist das gemeldete „sporadische Flackern“.
- **Beispiel `f000480`** (NFIF=2, Vsync an, mitten im Sweep): Die Ordner-Icons im Content Browser
  sind durch Schnipsel des Font-Atlas ersetzt. Ein gestreckter Keil zieht sich durch die Tab-Leiste,
  Beschriftungen sind abgeschnitten („Viewpo“, „He“, „Rea y“, „Engin“), und im 3D-Viewport steht
  ein einzelner Strich. Referenz und Frame 481 sind sauber.
- **Beispiel `f000059`** (`7daffeb5`, Vsync aus, Startphase): Der ganze Font-Atlas ist über den
  3D-Viewport gemalt, die Panels sind voller Dreiecke. Bei 864 640 falschen Pixeln ist fast das
  ganze Bild betroffen.
- Die Bilder liegen **nicht im Repo** (Screenshots werden nicht committet, siehe `.gitignore`),
  sondern auf NN-WS03 unter `C:/hw97/shots/s5_<lauf>/` als `fNNNNNN_{main,ref,diff,prev_main,next_main}.png`,
  daneben das `HorizonEngine.log` des Laufs.

### Grenzen der Aussage

- **Die Instrumentierung verstärkt den Wettlauf.** Drei zusätzliche Vollbild-Kopien und ein
  zweiter ImGui-Draw pro Frame lassen die GPU weiter hinterherlaufen: bei NFIF=2 und Vsync an
  99,5 % Überschreibungen im Flug gegenüber 65 % ohne Capture (Schritt 4). Die Raten vor dem Fix
  sind deshalb eher Obergrenzen. Für die Aussage über den Fix spielt das keine Rolle: Mit
  NFIF=3 gibt es auch unter dieser Last 0 Überschreibungen und 0 abweichende Pixel.
- Aufgenommen wird der Backbuffer unmittelbar vor `Present`. Was DWM daraus macht, ist nicht
  aufgenommen. Bei Vsync an wird jeder Frame gezeigt; bei Vsync aus fallen einzelne weg.
- Die Maus ist synthetisch (`io.AddMousePosEvent`, siehe Schritt 4). Für ImGui ist das dasselbe
  wie die echte Maus: Hover, Tooltip-Logik und Geometrie entstehen gleich. Das OS-Cursorbild
  gehört nicht zur ImGui-Geometrie.
- Nur das Hauptfenster. ImGui-Fenster, die als eigene OS-Fenster laufen (Multi-Viewport), haben
  eigene Fences in `imgui_impl_dx12.cpp` (siehe Befund) und sind hier nicht aufgenommen.
- **H2** (Viewport-SRV beim Ziehen eines Dock-Splitters) testet der Sweep nicht, denn er zieht nichts.

### Was ein Mensch am Bildschirm noch prüfen kann

Für den Mechanismus ist nichts mehr offen. Als Abnahme im Sinne des Themas reichen ein, zwei
Minuten im D3D12-Editor mit echter Maus: schnell über Toolbar, Quick-Settings-Slider, Outliner und
Content Browser fahren, einmal mit Vsync an und einmal ohne. Dabei sollte nichts aufblitzen. Wer
den Unterschied sehen will: Vor dem Fix blitzen dabei im Content Browser, in der Tab- und in der
Menüleiste Font-Schnipsel und Dreiecke auf. Unter Capture-Last war das ein grober Fehler alle 1,4
bis 4 s, ohne Capture ist es wahrscheinlich seltener. Wer den Vor-Fix-Stand also nur ein paar
Sekunden lang nicht flackern sieht, hat ihn damit noch nicht widerlegt. Zusätzlich lohnt ein Dock-Splitter-Zug
über den Viewport (H2, gehört nicht zu diesem Fix).

### Wiederholen

`docs/d3d12-imgui-flicker-bildvergleich.patch` (nur lokal, nicht mergen) enthält die komplette
Instrumentierung: Schritt 4 (Sweep, NFIF-/Vsync-Override, Fence-Zähler) plus Referenz-Draw,
Readback-Vergleich, Positivkontrolle und den vorübergehenden Getter
`D3D12Renderer::T97GetBackBuffer()`. Das Patch lässt sich auf den Zweigstand anwenden. Für den
Vor-Fix-Bau die drei Fix-Dateien aus `7daffeb5` holen, das Patch mit `-C1 --reject` anwenden und den
einen abgelehnten Hunk (NFIF-Override hinter `NumFramesInFlight = 2`) von Hand setzen.
`docs/d3d12-imgui-flicker-bildvergleich.ps1` fährt einen Lauf und wertet ihn aus: Er setzt
`"RHI": 3` in der Scratch-Config, setzt alle `HE_*` zurück, startet den Editor, bricht nach N
verglichenen Frames ab und beendet nur den eigenen Deploy-Prozess. Beispiel:
`-Name prefix -Frames 3000` bzw. `-Name fix -Scribble 500`. Nach den Läufen wurden die Quellen
zurückgesetzt und `C:/hw97` sauber neu gebaut. Im Deploy steckt also keine Instrumentierung mehr.

## Echte OS-Maus statt synthetischer ImGui-Events (Schritt 8)

Stand: 2026-09-30, NN-WS03, gleicher Release-Baum `C:/hw97`, gleiche Szene (Testie).

### Werkzeug (fertig, auf NN-WS03 geprüft)

Ziel ist derselbe Frame-Diff wie in Schritt 5, aber ohne `HE_T97_SWEEP`. Der Reiz ist diesmal
der echte Cursor: `SendInput` → `WM_MOUSEMOVE` → SDL3 → ImGui.

- **`docs/d3d12-imgui-flicker-osmaus.cpp`** (`t97mouse.exe`, mit `cl /O2 /EHsc /std:c++17 /utf-8`)
  bewegt den echten Cursor per `SendInput` (`MOUSEEVENTF_ABSOLUTE|VIRTUALDESK`). Das Programm
  ist PerMonitorV2-DPI-aware und bewegt nur, es klickt nie. Etwa alle 1 ms geht es 7 px weiter,
  also 4 000 bis 7 000 px/s, ein schneller Wisch. Es fährt im Zickzack (23 px Zeilenabstand)
  nacheinander über:
  - den Toolbar-Streifen oben im Scene-Fenster,
  - Quick Settings,
  - World Outliner,
  - Content Browser (Baum und Raster),
  - Menü- und Tab-Leiste,
  - zuletzt den ganzen Client-Bereich.

  Die Rechtecke schreibt der Editor selbst ins Log (`T97WIN`). Sie werden über
  `GetClientRect`/`ClientToScreen` auf Bildschirmkoordinaten umgerechnet.
- **Sicherungen**, weil das der Cursor des Menschen ist:
  - Ein `WH_MOUSE_LL`/`WH_KEYBOARD_LL`-Hook bricht bei jedem Ereignis ohne `LLMHF_INJECTED`
    bzw. `LLKHF_INJECTED` sofort ab.
  - Vor jedem Schritt prüft `WindowFromPoint`, ob das Fenster unter dem Cursor zum Editorprozess
    gehört. Wenn nicht, wird der Schritt ausgelassen, nach 20 Auslassungen bricht das Programm ab.
  - `--minidle=S` verweigert den Start, wenn in den letzten S Sekunden jemand Maus oder Tastatur
    benutzt hat.
  - Am Ende steht der Cursor wieder an der Ausgangsposition.
- **`docs/d3d12-imgui-flicker-osmaus.patch`** ist die Instrumentierung aus Schritt 5 plus
  `HE_T97_MOUSELOG`. Sie loggt alle 100 Frames `io.MousePos`, das gehoverte Fenster und die Zahl
  der Hover-Wechsel. So ist belegt, dass der echte Cursor bei ImGui ankommt, statt es nur
  anzunehmen.
- **`docs/d3d12-imgui-flicker-osmaus.ps1`** entspricht dem Lauf-Skript aus Schritt 5, setzt aber
  `HE_T97_SWEEP` bewusst **nicht**. Sonst würde `AddMousePosEvent` jeden Frame die echte Maus
  überschreiben, und man mäße wieder den alten Reiz. `-Dry` startet den Editor und berechnet die
  Ziele, bewegt aber nichts.

Dry-Run (keine Bewegung): Editor-Client 1600×900 an (121,50). ImGui meldet dieselbe Fläche
(Skalierung 1,000), und alle sechs Ziele werden gefunden. Die Nullprobe ohne Reiz ergibt
300/300 Frames bitgleich, NFIF=3, 0 Überschreibungen im Flug.

### Ergebnis: nicht gefahren, das Kriterium „echte OS-Maus“ bleibt OFFEN

**Es gab keinen einzigen SendInput-Lauf.** Technisch ginge es, es fehlt aber ein freier Platz an
der Konsole:

- `SendInput` bewegt den Cursor des Menschen, und das Editorfenster muss dafür vorne liegen. Der
  Mensch arbeitete die ganze Zeit an NN-WS03. Gemessen über rund 25 min lag die Leerlaufzeit
  (`GetLastInputInfo`) meist bei 0 bis 20 s, höchstens einmal bei 189 s.
- Eine Freigabe habe ich im Thema erfragt (Beitrag #750), bis zum Schluss kam keine Antwort.
- Ohne Freigabe habe ich den Cursor nicht übernommen. Selbst mit Freigabe hätte der Abbruch-Hook
  jeden Lauf bei der nächsten echten Eingabe beendet.

Automatisierbar ist der Test also, aber nur, wenn niemand am Rechner sitzt: `-MinIdle 600`
wartet darauf, dass seit 10 min keine Eingabe kam. Die Bewegungs- und Abbruchpfade von
`t97mouse.exe` (Hook, `WindowFromPoint`-Prüfung) sind deshalb **ungetestet**. Geprüft ist nur der
Dry-Run.

Unverändert gilt: Schritt 5 misst mit `AddMousePosEvent` 0 kaputte Frames in 18 294. Offen ist
nur, ob der Weg über die echte OS-Maus (WM_MOUSEMOVE → SDL3) etwas anderes auslöst.

### Manuelle Probe mit Messung (1–2 min, für den Menschen)

Für die Probe liegt der instrumentierte Editor als eigene Kopie in
`C:/hw97/deploy_t97instr/Editor`. `C:/hw97/deploy` ist wieder sauber gebaut. Die Kopie wurde aus
dem neuen Pfad gestartet und läuft: 9 Ziele erkannt, Capture aktiv, kein Firewall-Ereignis 2097.

So läuft die Probe:

- Aus dem Worktree starten:

  ```
  .\docs\d3d12-imgui-flicker-osmaus.ps1 -Name mensch_vs1 -Manual -Exe C:\hw97\deploy_t97instr\Editor\HorizonEditor.exe
  .\docs\d3d12-imgui-flicker-osmaus.ps1 -Name mensch_vs0 -Manual -Vsync 0 -Exe C:\hw97\deploy_t97instr\Editor\HorizonEditor.exe
  ```

- Jeweils 1–2 min die Maus schnell über den Toolbar-Streifen oben im Scene-Fenster, Quick
  Settings samt Slidern, World Outliner und Content Browser bewegen. Dann den Editor schließen.
- Das Skript gibt danach aus:
  - `T97MOUSE … hoverChanges=… mouseMoveFrames=…`: Belegt, dass die echte Maus bei ImGui ankam.
  - `T97CAP compared=… mismatched=…`: Die Zahl der Frames, die vom rennfreien Referenzbild
    abweichen. Mit dem Fix muss sie 0 sein.
  - Im Fehlerfall zusätzlich die ersten 6 Abweichungen als PNG unter
    `C:/hw97/shots/s8_<Name>/`.

So misst die Probe objektiv, statt sich nur auf das Auge zu verlassen. Die Negativkontrolle
(`-Nfif 2`, dann erwartet `mismatched > 0`) geht mit demselben Befehl.
