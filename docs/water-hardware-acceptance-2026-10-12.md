# D3D11, D3D12, Vulkan: Hardware-Abnahme See/Pinsel/Ufer/Undo (Thema 174 Schritt 10)

Stand 2026-10-12, NN-WS03. Ziel: den Lake-Werkzeug-, Pinsel-, Ufer-Beschneidungs- und
Undo/Redo-Pfad von Thema 174 auf echter Hardware auf D3D11, D3D12 und Vulkan prüfen —
nicht nur strukturell (Schritt 9), sondern mit echtem Zeichnen, Umformen, Speichern/Laden
und Fenster-Resize.

## 0. Hardware, Treiber, Backend

| | |
|---|---|
| Rechner | NN-WS03, Windows 11 Pro (10.0 build 26200) |
| CPU | AMD Ryzen 9 9950X, 32 Hardware-Threads |
| GPU | NVIDIA GeForce RTX 4070, Treiber 610.88 |
| Vulkan | 1.4.341 (SDK gefunden), GI-Raytracing verfügbar (`VK_KHR_ray_query`) |
| D3D12 | DXC gefunden (DXR-GI), GPU-Debug-Layer + DRED verfügbar |
| D3D11 | kein Debug-Layer-Codepfad im Renderer (bestätigt, siehe §4) |
| Build | Release, Ninja/MSVC (vcvars64), inkrementell auf `C:/hebuild/pws9` (derselbe Baum wie Schritt 9), `-DDEPLOY_DIR` isoliert |
| Commit | 8d1ca365 + diese Schritt-10-Änderung (HE_DUMP_LAKEEDIT um `undo`/`redo` erweitert) |

## 1. Code-Änderung dieses Schritts

Eine kleine Erweiterung der bestehenden `HE_DUMP_LAKEEDIT`-Zeugnis-Szene
(`src/HE_Editor/EditorApplication.cpp`), zwei neue Modi `undo` und `redo`: sie spiegeln
genau das Protokoll, das der echte "Create Lake"-Knopf im Spline-Panel benutzt
(`ctx.undoSys->snapshotNow("Create Lake")` unmittelbar vor `L::create`, `SplineTool.cpp:333`)
und danach `EditorUndo::undo()`/`redo()` — dieselbe Klasse, die Ctrl+Z im Editor aufruft.
Kein Shortcut über das Modell. Kein anderer Code geändert.

`cmake --build . -j8`: rc 0. `he_tests.exe` gefiltert auf water/spline/lake: **190/190
Testfälle, 358138/358138 Assertions, 0 fehlgeschlagen** (identisch zu Schritt 9 — die
Änderung betrifft nur den env-var-gesteuerten Dump-Pfad, den kein Test aufruft).

## 2. Matrix: 5 Szenen × 4 Backends, Zeugnis-Dumps

Jede Szene läuft durch denselben Code wie das echte Werkzeug (siehe Kommentar über
`HE_DUMP_LAKEEDIT`/`HE_DUMP_WATERLAKE` in `EditorApplication.cpp`), `HE_DUMP_QUIT=1`,
1280×720, Kamera `CAMY=375 CAMZ=48 PITCH=-60 TOD=0.4`, frisches `HE_CONFIG_DIR` je Lauf.

- **reshaped** — See zeichnen, mit dem Pinsel erweitern (Kanal + Insel), dann Punkte
  umformen (Ostlappen raus, Südbucht rein).
- **extended** — See zeichnen, mit dem Pinsel erweitern UND löschen (Dig-Strich + Radierer
  schneidet eine Insel aus der Mitte).
- **undo** — See zeichnen, dann Ctrl+Z (`EditorUndo::undo()`) — neu in diesem Schritt.
- **redo** — wie `undo`, dann Ctrl+Shift+Z (`EditorUndo::redo()`) — neu in diesem Schritt.
- **slope** — See über einen Hang (0,12 m/m Neigung + Wellung), Ufer-Beschneidung an.

Ergebnis: **20/20 Läufe EXIT=0, BMP geschrieben, 0 [ERROR] über alle 20 Logs** (volles
Grep über jede `enginelog.txt`, nicht nur die GPU-Debug-Zeilen). Die Zahlen in jeder
Zeugnis-Log-Zeile sind auf allen vier Backends **exakt identisch**:

| Szene | D3D11 | D3D12 | Vulkan | GL |
|---|---|---|---|---|
| extended | wet cells 2771, triangles 318 | ident. | ident. | ident. |
| reshaped | 2771→2865 cells, triangles 322, channel/island korrekt | ident. | ident. | ident. |
| undo | 2564→0 cells, surfaces 0 | ident. | ident. | ident. |
| redo | 2564→0→2564 cells, surfaces 1, triangles 221 | ident. | ident. | ident. |
| slope | triangles 325, ground over sheet ≤0,050 m | ident. | ident. | ident. |

Screenshots (gleiche Szene, alle vier Backends — Seefläche mit Wellenstruktur/Trübung/
Ufer-Schattensaum, Kanal und ausradierte Insel bzw. Hang-Kontur je nach Szene, Spline-
Punkt-Handles als Overlay). Mittelwert-RGB über das ganze Bild bzw. nur die untere
Bildhälfte (Wasserband), `scripts`-Stil wie Schritt 9: D3D11/D3D12/Vulkan liegen je Kanal
innerhalb von ~2 Einheiten zueinander (`extended`: RGB≈160,172,182 voll / 159,170,181
unten; `slope`: RGB≈121,147,163 voll / 108,141,158 unten). OpenGL weicht etwas mehr ab
(`extended` unten: 173 statt 159 — ca. 14 Einheiten, vorbekannte Cross-Backend-Varianz,
kein neuer Befund):

| Backend | extended | slope |
|---|---|---|
| D3D11 | [d3d11-extended.png](water-hardware-acceptance-2026-10-12/d3d11-extended.png) | [d3d11-slope.png](water-hardware-acceptance-2026-10-12/d3d11-slope.png) |
| D3D12 | [d3d12-extended.png](water-hardware-acceptance-2026-10-12/d3d12-extended.png) | [d3d12-slope.png](water-hardware-acceptance-2026-10-12/d3d12-slope.png) |
| Vulkan | [vulkan-extended.png](water-hardware-acceptance-2026-10-12/vulkan-extended.png) | [vulkan-slope.png](water-hardware-acceptance-2026-10-12/vulkan-slope.png) |
| OpenGL | [gl-extended.png](water-hardware-acceptance-2026-10-12/gl-extended.png) | [gl-slope.png](water-hardware-acceptance-2026-10-12/gl-slope.png) |

**Metal**: auf diesem Rechner nicht verfügbar (kein Mac). Per `hive_request` beim Menschen/
der Queen angefragt (nicht blockierend); zwei Nahaufnahmen vom Wassermaterial mit
Spline-Overlay wurden in den Thread gepostet (vermutlich Metal, andere Kameraführung als
oben, daher kein Pixel-Vergleich, aber dieselbe Transparenz/Reflexions-/Trübungscharakteristik
wie in den vier Bildern oben). Kein strikter Metal-Vergleich in dieser Tabelle.

## 3. GPU-Debug-/Validation-Layer

D3D12 (`HE_GPU_DEBUG=1`, GPU-Debug-Layer + DRED) und Vulkan (Validation Layer) auf allen
fünf Szenen (reshaped/extended/undo/redo/slope): **0 Fehler** laut Session-Summary, auf
jedem Backend, in jedem Lauf. D3D12 zeigt in jedem Lauf **3 Warnungen** — dieselbe
vorbekannte, benigne `ID3D12CommandList::ClearRenderTargetView`-Meldung aus Schritt 9 §8.3
und `docs/water-shader-plan.md` §8.3 ("the clear values do not match … will still clear to
the desired value"), nicht wasserbezogen. Vulkan zeigt 0 Warnungen.

Das ist zugleich die Positiv-Kontrolle für die "0 Fehler"-Aussage auf D3D12: dieselbe
Session-Summary-Zählung, die bei den Fehlern 0 meldet, meldet bei den Warnungen
zuverlässig eine echte 3 — der Kanal ist also nachweislich nicht stumm. Für Vulkan ergab
sich eine schärfere Positiv-Kontrolle ungeplant beim Resize-/Schliessen-Test (§5): dort
zählte dieselbe Vulkan-Validation-Pipeline 8 echte `[ERROR]`-Zeilen — der Kanal liefert
also auch Fehler-Schwere zuverlässig, nicht nur Warnungen; die Vulkan-Validation-Messenger-
Schwellen sind im Quelltext zudem auf WARNING_BIT|ERROR_BIT gesetzt
(`VulkanRenderer.cpp:1215`), INFO/VERBOSE werden absichtlich nicht geloggt.

**D3D11**: kein Debug-Layer-Codepfad im Renderer (im Quelltext nachgeschlagen, nicht nur
Schritt 9 geglaubt) — "0 Fehler" ist hier NICHT geprüft, sondern schlicht nicht prüfbar.
Als offene Lücke vermerkt, nicht als "bestanden".

## 4. Speichern/Laden (MCP, live laufender Editor)

Nicht per Zeugnis-Dump (der hat kein Projekt/keine Szene auf Platte), sondern über die
ECHTE MCP-Bridge des laufenden Editors (`HE_MCP=1`, private `APPDATA`, private Projekt-
kopie aus `Downloads/Test`): `entity_create` (Terrain + geschlossene Spline) →
`terrain_lake` (action `create`, derselbe Gateway-Pfad wie der "Create Lake"-Knopf) →
`entity_get` → `scene_save` → `scene_open` → `entity_get` erneut. Pro Backend
(RHI aus `Types/Enums.h`: D3D11=2, D3D12=3, Vulkan=1):

| Backend | See erzeugt | Wasserdaten nach Speichern/Laden identisch |
|---|---|---|
| D3D11 | 10278 nasse Zellen, 4269 Bodenvertices gegraben | **ja** |
| D3D12 | ident. | **ja** |
| Vulkan | ident. | **ja** |

`scene_screenshot` (MCP) ist laut Quelltext (`McpToolsScreenshot.cpp:452,568`) NUR auf
Metal implementiert ("unsupported" auf D3D11/D3D12/Vulkan/GL) — deshalb kein Bild dieses
Laufs; der Render-Pfad nach dem Laden ist aber derselbe `TerrainSystem::updateTerrains` /
`WaterSurface::update` / `InvalidateMesh`, den jeder Dump-Lauf in §2 schon zeigt.

## 5. Fenster-Resize (interaktiv, nicht Dump-Modus)

Echter, sichtbarer Editor (kein `HE_HIDDEN_WINDOW`), DPI-bewusstes PowerShell-Skript
(`SetProcessDpiAwarenessContext(-4)`, NN-WS03 läuft bei 125 %), `HE_GPU_DEBUG=1`, Szene mit
See aus §4 geladen. Fünf Größen inkl. 1×1 (entartete Swapchain, der klassische Absturzfall)
plus Minimize/Restore, Prozess-Lebendigkeit nach jedem Schritt geprüft:

| Backend | 1600×900 | 400×300 | 2200×1300 | 1×1 | 1920×1080 | Minimize | Restore | Absturz |
|---|---|---|---|---|---|---|---|---|
| D3D11 | ok | ok | ok | ok | ok | ok | ok | **nein** |
| D3D12 | ok | ok | ok | ok | ok | ok | ok | **nein** |
| Vulkan | ok | ok | ok | ok | ok | ok | ok | **nein** |

Kein neuer Eintrag unter `%TEMP%\he_crash_*` (letzter vorhandener Eintrag ist vom
2026-10-07, fünf Tage alt — nicht von diesem Lauf).

**Nebenbefund beim normalen Schliessen (nicht beim Resize selbst):** Vulkan-Validation
meldet beim gewöhnlichen Fenster-Schliessen (WM_CLOSE, nicht `HE_DUMP_QUIT`) 8 `[ERROR]`
— `vkDestroyBuffer`/`vkFreeDescriptorSets`/`vkDestroySampler`/`vkDestroyPipeline`
"can't be called on X that is currently in use by VkCommandBuffer". **Nicht
wasserspezifisch**: reproduziert identisch (gleiche 8 Meldungen, gleiche Objekttypen) auf
dem unveränderten `Downloads/Test`-Projekt ganz ohne Wasser. Ein allgemeiner
Shutdown-Synchronisationsfehler im Editor/VulkanRenderer, der bisher niemandem auffiel,
weil jede bisherige Wasser-Verifikation (Schritt 9, §2 hier) über `HE_DUMP_QUIT` sofort
nach einem Frame beendet — vor dem normalen Shutdown-Pfad. **Nicht in diesem Schritt
behoben** (außerhalb des Rahmens); als Befund in Thema 174 gepostet (`hive_post`,
2026-10-12), eigenes Thema vorgeschlagen. Logs: `C:\hebuild\pws9\s10\resize_vulkan_control\
enginelog.txt` (Kontrolle ohne Wasser) und `resize_vulkanb\enginelog.txt` (mit See).

## 6. Pinsel-Cursor-Ring

Weiterhin nicht im Bild bestätigt (bräuchte eine `io.AddMousePosEvent`-Hover-Simulation,
siehe Schritt 9 §4 und die Lehre „Interactive editor UI tests on Windows"). Die
ImGui-Pipeline, über die `TerrainTools.cpp` ihn zeichnet, ist strukturell auf D3D11, D3D12
und Vulkan initialisiert (unverändert seit Schritt 9).

## 7. Nicht in diesem Schritt

- D3D11-Debug-Layer (es gibt keinen) — vorbestehende Lücke, nicht wasserspezifisch.
- Der Vulkan-Shutdown-Fehler aus §5 — gemeldet, nicht behoben, nicht wasserspezifisch.
- Pinsel-Cursor-Ring im Bild (§6).
- AMD/Intel-GPUs, das exportierte Spiel.
- Menschliches Feedback zu Aushub-Tiefe/Ufer-Abschneiden und "Lakes als Actor" (im
  Themen-Thread 2026-10-12 nach Beginn dieses Schritts eingegangen) — ein Design-/
  UX-Thema, von der Queen bereits in eigene Schritte aufgeteilt, nicht Teil dieser
  Hardware-Abnahme.

## 8. Ergebnis

D3D11, D3D12 und Vulkan zeichnen See, Pinsel-Erweiterung/-Löschung, Umformen, Ufer-
Beschneidung am Hang und Undo/Redo untereinander mittelwert-RGB-gleich (≤2 Einheiten je
Kanal) und nahe an OpenGL (≤14 Einheiten, vorbekannte Cross-Backend-Varianz), mit denselben Zahlen
(Zellen, Dreiecke, Bodenvertices) in jedem Fall. Speichern/Laden ist auf allen drei
Backends bit-gleich. Kein Absturz bei fünf Fenstergrößen inkl. 1×1 plus Minimize/Restore.
D3D12 und Vulkan zeigen 0 Rendering-Validation-Fehler bei aktivem Debug-/Validation-Layer
(Positiv-Kontrolle bestätigt den Kanal als funktionsfähig); D3D11 hat keinen Debug-Layer-
Pfad und bleibt ungeprüft. Ein Vulkan-Shutdown-Validation-Fehler wurde gefunden, ist aber
nicht wasserspezifisch und als eigener Befund gemeldet statt hier behoben.
