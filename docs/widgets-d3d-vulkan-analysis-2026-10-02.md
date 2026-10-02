# Widgets unsichtbar auf D3D11/D3D12/Vulkan: Repro und Ursache (Thema 133, Schritt 1)

Stand: 2026-10-02, NN-WS03 (RTX 4070), Release-Build aus `9ed6f816` (main) in `C:/hw133`.
Kein Engine-Code geändert. Dieser Schritt liefert nur Repro, Ursache und Belege.

## Ergebnis in einem Satz

Auf **D3D11, D3D12 und Vulkan** wird `m_renderWorld.uiObjects` nie befüllt:
`RenderExtractor::extractUI` (UISystem + WidgetManager) wird nur von OpenGL und Metal
aufgerufen. Jeder UI-Pass auf den drei anderen Backends kehrt deshalb sofort bei
`uiObjects.empty()` zurück. Das betrifft PIE, den Editor-Viewport und das exportierte Spiel.
Es ist **keine Regression**: Seit dem Port der UI-Passes in `d12cc7ab` (2026-06-20)
gab es den Aufruf dort nie. Wird der Aufruf ergänzt, kommen auf Vulkan zwei weitere
Fehler zum Vorschein.

## Betroffen

| Backend | Editor/PIE (Viewport-RT) | exportiertes Spiel (Swapchain) |
|---|---|---|
| OpenGL | sichtbar (Referenz) | sichtbar (Referenz) |
| D3D11  | **unsichtbar** | **unsichtbar** |
| D3D12  | **unsichtbar** | **unsichtbar** |
| Vulkan | **unsichtbar** | **unsichtbar**. Ohne manuell nachkopierte `Shaders/` zeichnet das Spiel überhaupt nichts, siehe Befund 4 |

Metal habe ich nicht auf NN-WS03 gemessen. Laut Code ruft Metal `extractUI` auf (`MetalRenderer.mm:13096`, `:17129`).

PIE: Play-in-Editor speichert einen Snapshot von `m_editorWorld` und spielt in derselben Welt
weiter. Gerendert wird mit demselben Renderer über `DrawViewportFrame`, dieselbe Stelle wie
beim `HE_DUMP_UITEST`-Capture. Der Dump ist deshalb der Beleg für den PIE-Pfad.

## Befund 1 (Hauptursache): extractUI fehlt auf D3D11/D3D12/Vulkan

- `src/HE_Rendering/src/RenderExtractor.cpp:1333`: `extractUI` füllt `out.uiObjects` aus
  `UISystem::extract` (Entity-UI) und `world.widgets().extract` (WidgetManager-Widgets).
- Aufrufer sind nur `OpenGLRenderer.cpp:10086` (direkt nach `m_extractor.extract`) und Metal.
- D3D11 `DrawScene` (`D3D11Renderer.cpp:5557`), D3D12 (`D3D12Renderer.cpp:9108`) und Vulkan
  `DrawScene` (`VulkanRenderer.cpp:6210`) rufen nur `extract`. Dieses löscht über
  `RenderWorld::clear()` auch `uiObjects`.
- Die Abbruchstellen: `D3D11Renderer.cpp:5233`, `D3D12Renderer.cpp:4311` und
  `VulkanRenderer.cpp:506/798/854`.
- `git log -S extractUI -- src/HE_Rendering` liefert nur GL/Metal-Commits (`2977ffe4`,
  `f83dbe5b`, `8ebd6f76`). `d12cc7ab` ("in-game 2D UI canvas on D3D11, D3D12, Vulkan")
  portierte nur die Draw-Passes. Spätere Pflege dieser Passes (Clipping `2e553d03`,
  Rotation `4310ed80`) wurde offenbar nur auf GL/Metal angesehen. PR #72 (PreConstruct)
  und die Postprocessing/GI-Themen 117/120 sind **nicht** die Ursache.

### Messung Editor/PIE-Pfad (`HE_DUMP_UITEST=1`, 1280x720, ohne SKYTEST)

Der Witness erzeugt ein Widget-Asset mit 12 Kacheln über WidgetManager → extractUI →
Backend-UI-Shader. Gezählt werden helle Pixel (max. Kanal > 30, jedes 4. Pixel) und die
Kachel 0 (x 70–290, y 90–180, Sollfarbe 0.26/0.30/0.38).

| Lauf | GL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| main (`base_*`) | 77861 / Kachel 0 19800/19800, (66,76,97) | 0 / 0, (4,4,4) | 0 / 0, (4,4,4) | 0 / 0, (4,4,4) |
| + extractUI-Patch (`ctl_*`) | 77861 / 19800, (66,76,97) | 79200 / 19800, (66,76,97) | 79200 / 19800, (66,76,97) | 79200 / **0**: Kacheln gespiegelt |

Auf allen Backends meldet das Log `Created widget '__uiStyleWitness.hasset' (id 1, 12 element(s))`
und keinen UI-Shader- oder PSO-Fehler. Das Widget existiert also, es wird nur nicht gezeichnet.

### Messung Spielpfad (exportiertes Spiel, Swapchain, PrintWindow 1600x900 bei 125 % DPI)

Szene: `docs/widgets-d3d-vulkan-witness-scene.ps1`, ein Canvas mit Rot oben links, Grün unten
rechts und Blau mittig mit dem Text "WIDGET". Export: `docs/widgets-d3d-vulkan-export.py`.
Lauf: `docs/widgets-d3d-vulkan-game-capture.ps1`. Gemessen wurden Pixel an den Soll- und an
den gespiegelten Positionen.

| Lauf | GL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| main | Rot/Grün/Blau an der Sollposition | nur Himmel/Boden | nur Himmel/Boden | nur Himmel/Boden (mit nachkopierten Shaders/), ohne: schwarz |
| + extractUI-Patch | wie oben | an den Messpunkten gleich GL (229,25,25)/(25,204,51)/(25,76,229) | an den Messpunkten gleich GL | Rot **unten** links, Grün **oben** rechts, Text auf dem Kopf |

Der Kontroll-Patch liegt bei `docs/widgets-d3d-vulkan-extractui-positive-control.patch`.
Es sind drei Zeilen, jeweils `extractUI(*m_world, width, height, renderWorld)` direkt nach dem
Haupt-`extract` in `DrawScene`. Er ist **nicht** committet, der Fix gehört zu Schritt 2.

Was der Patch repariert, ist die **Zuführung**. Flache Quads und Text stimmen auf D3D11/D3D12
in Farbe und Position mit GL überein. Die Stil-Features fehlen weiterhin (Befund 3). Deshalb
sind mit dem Patch 79200 statt 77861 Pixel hell: Die runden, gestylten Kacheln werden als volle
Rechtecke gezeichnet. Vulkan wird sichtbar, aber gespiegelt (Befund 2). Drei Zeilen reichen
also nicht für „sieht aus wie auf GL“.

Reihenfolge: `runGi` (`VulkanRenderer.cpp:9455`) und `runSSAO` (`:10591`) extrahieren ebenfalls
und leeren dabei `uiObjects`. `extractUI` muss deshalb nach dem letzten `extract` vor dem UI-Pass
laufen. Der Aufruf in `DrawScene` erfüllt das, und das ist **gemessen**. Lauf `ctlgi_*`:
`HE_DUMP_GI=1 HE_DUMP_SSAO=1 HE_DUMP_SHADOWINSTTEST=1 HE_DUMP_SKYTEST=1` mit dem Patch, und alle
drei Backends loggen `GI probe grid 11x4x11 (484 probes)`. Kachel 0 hat danach die exakte
Kachelfarbe (66,76,97): auf D3D11/D3D12 an der Sollposition (y 90–180), auf Vulkan an der
gespiegelten (y 540–630). An der jeweils anderen Stelle liegt Szene.

## Befund 2: Vulkan zeichnet die UI vertikal gespiegelt

`src/HE_Rendering/shaders/ui.vert` rechnet `gl_Position.y = 1.0 - sp.y / H * 2.0`, also in
GL-Konvention mit y nach oben. Vulkan-NDC hat y nach unten, und beide UI-Pipelines setzen einen
Viewport mit positiver Höhe (`VulkanRenderer.cpp:508ff`, `:812ff`). Dadurch landet die
Canvas-Oberkante unten. Gemessen auf beiden Pfaden (Viewport-RT und Swapchain): Positionen und
Glyphen stehen auf dem Kopf. Der Fehler besteht seit `d12cc7ab`. Er fiel nie auf, weil die
Quads wegen Befund 1 nie gezeichnet wurden.

## Befund 3: Die UI-Shader von D3D11/D3D12/Vulkan können nur flache Quads, Glyphen, Clip und Rotation

Feld-Nutzung von `UIRenderObject` je Backend (gezählt mit grep):

| Feld | GL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| cornerRadius, borderWidth/Color, gradient*, blur, innerShadow* | ja | – | – | – |
| textureAssetId, materialAssetId, uiState | ja | – | – | – |
| clipRect, rotation, fontAtlasKey | ja | ja | ja | ja |

Auch nach der Reparatur von Befund 1 erscheinen Widgets deshalb ohne runde Ecken, Rahmen,
Verläufe und Schatten. Drop-Shadow-Quads (vergrößert, in Schattenfarbe) erscheinen als harte
dunkle Rechtecke, sichtbar in `ctl_D3D11`/`ctl_Vulkan`. Bilder (UIImage mit Textur) und
Material-Widgets erscheinen als einfarbige Tint-Fläche. "Widgets sind sichtbar" ist damit
noch nicht dasselbe wie "Widgets sehen aus wie auf GL". Das gehört in Schritt 2 oder in ein
Folgethema.

## Befund 4 (Nebenbefund, Vulkan-Spiel allgemein): Der Export lässt `Shaders/` weg

`ProjectExporter.cpp:891–896` kopiert aus dem Game-Runtime-Ordner nur **reguläre Dateien der
obersten Ebene**. `deploy/Editor/Game/Shaders/` (42 Dateien: Vulkan-`.spv` und D3D12-DXR-`.cso`)
landet nie im Export. Die Folgen im exportierten Spiel:

- Vulkan: `shader not found — <out>/Shaders/scene.vert.spv`, dann `scene shaders missing — scene will not draw`.
  Das Bild ist komplett schwarz, nicht nur die UI fehlt.
- D3D12: Die DXR-GI-Kernel `gi_*_hw.cso` fehlen. Laut Code gibt es dafür einen SW-Fallback;
  nicht separat gemessen.

Für die Messungen oben habe ich `Shaders/` von Hand in die Exporte kopiert. Das ist ein
eigener Fehler, unabhängig von der UI, aber Teil von "Vulkan im Spiel zeigt nichts".

## Was nicht gemessen ist

- Metal (läuft nicht auf Windows) und GL < 4.3.
- Ein per Game-Logic (`Create Widget`) erzeugtes Widget im exportierten Spiel. Der Spiel-Witness
  nutzt Entity-UI. Beide Wege laufen über dasselbe `extractUI` (`RenderExtractor.cpp:1338/1341`),
  und der Editor-Witness belegt den WidgetManager-Weg.
- Ein interaktiver PIE-Klick. Der PIE-Pfad ist über Code plus den Viewport-Dump belegt (siehe oben).

## Stand der Messbäume auf NN-WS03 (für Schritt 2)

- `C:/hw133/deploy` = **Kontroll-Stand** (Editor-`HorizonRendering.dll` 624BCB6C… mit Patch). Das ist NICHT main.
- `C:/hw133/pre` = main-Baseline (`9ed6f816`, `HorizonRendering.dll` A69E46C8…).
- `C:/hw133/out_pre`, `C:/hw133/out_ctl` = exportiertes UiWit-Spiel, Baseline bzw. mit Kontroll-DLL. In beide ist `Shaders/` von Hand nachkopiert (Befund 4).
- Der Quellbaum ist zurückgesetzt. Im Zweig ist nur Doku committet.

## Reproduktion

```powershell
# Build (privater Deploy, nie das HorizonEngineBuild des Menschen)
cmake --preset x64-release -B C:/hw133/build -DDEPLOY_DIR=C:/hw133/deploy
cmake --build C:/hw133/build -j8 --target HorizonEditor HorizonGame
# Editor/PIE-Pfad je Backend: frisches APPDATA, HE_DUMP_PATH/QUIT/RHI/FRAMES=16, HE_DUMP_UITEST=1
# Spielpfad: Szene + Export + Capture mit den drei docs/widgets-d3d-vulkan-*-Skripten
# Achtung: deploy/Editor/Game/HorizonRendering.dll ist nach einer reinen DLL-Änderung
# veraltet (relinkt nur mit HorizonGame). Für den Kontroll-Export die DLL aus
# build/src/HE_Rendering nehmen.
```

---

# Schritt 2: Fix und Vorher/Nachher-Vergleich

Stand: 2026-10-02, NN-WS03, Release-Build aus dem Zweig in `C:/hw133` (`DEPLOY_DIR=C:/hw133/deploy`).

## Was geändert ist

| Commit | Befund | Änderung |
|---|---|---|
| `1155f63d` | 1 | `extractUI` in `DrawScene` von D3D11, D3D12 und Vulkan, direkt nach dem Haupt-`extract` wie bei GL. Danach extrahiert niemand mehr, also auch nicht `runGi`/`runSSAO`. |
| `69c43bbd` | 2 | `shaders/ui.vert`: `y = sp.y / H * 2 - 1` (Vulkan-NDC, y nach unten). Die Datei wird nur von Vulkan benutzt. Der Clip-Scissor passt damit auch wieder zur Geometrie. |
| `04de39bd` | 4 | `ProjectExporter`: `Shaders/` aus dem Game-Runtime-Ordner wird rekursiv mitgeliefert und dabei komplett ersetzt, wie `lib-dynload`. Der Test `ProjectExporter ships the runtime Shaders/ subdirectory and replaces a stale one` deckt das ab. |

**Nicht** geändert ist Befund 3. D3D11/D3D12/Vulkan zeichnen Widgets jetzt, aber weiter ohne runde Ecken, Rahmen,
Verläufe, Blur/Drop-Shadow, Inner Shadow, Texturen (UIImage) und UI-Materialien. Ein Drop-Shadow-Quad, das vorher
gar nicht zu sehen war, erscheint jetzt als **hartes dunkles Rechteck** (vergrößert, in Schattenfarbe). Das ist eine
sichtbare Folge dieses Fixes. Ein Port braucht auf Vulkan mehr als die 128 B garantierten Push-Constants (heute 80 B),
also einen UBO. Das ist ein eigenes Thema.

## Messung

Skripte, alle in `docs/`:

- `widgets-d3d-vulkan-editor-capture.ps1`: Editor/PIE-Pfad (`HE_DUMP_UITEST`, Viewport-RT)
- `widgets-d3d-vulkan-witness-scene.ps1` + `widgets-d3d-vulkan-export.py` + `widgets-d3d-vulkan-game-capture.ps1`: exportiertes Spiel (Swapchain)
- `widgets-d3d-vulkan-verify.ps1`: PASS/FAIL mit festen Messpunkten. Exit-Code = Zahl der Fehlschläge.

```powershell
& docs\widgets-d3d-vulkan-verify.ps1 -EditorTag fix -GameTag gfix   # nach dem Fix
& docs\widgets-d3d-vulkan-verify.ps1 -EditorTag base -GameTag gpre  # main
```

(Mit `&` aufrufen. `powershell -File ... -Rhis A,B` reicht die Liste als *einen* String durch.)

| Stand | Editor/PIE | Spiel | Fehlschläge |
|---|---|---|---|
| main (`base`/`gpre`) | D3D11/D3D12/Vulkan: keine Kachel (4,4,4) | D3D11/D3D12: nur Szene. Vulkan: schwarz (Export ohne `Shaders/`) | **12** |
| nur extractUI (`ctl`/`gctl`, Schritt-1-Kontrolle) | Vulkan: Kachel an den gespiegelten Zeilen, Bild = D3D11 gespiegelt (47,92 % Abweichung, 0,00 % zum Spiegelbild) | Vulkan: Rot unten, Grün oben | **6** |
| Fix (`fix`/`gfix`) | alle vier: Kachel 0 (66,76,97) an y 90–180. Vulkan = D3D11 auf 0,00 % der Pixel | alle vier: Rot (229,25,25) oben links, Grün (25,204,51) unten rechts, Blau (25,76,229) mittig, Text aufrecht | **0** |

Weitere Belege zum Nachher-Stand:

- Export `C:/hw133/out_fix` aus dem gefixten Editor über MCP, `Shaders/` **nicht** von Hand kopiert: 42 Dateien,
  dieselben wie in `deploy/Editor/Game/Shaders`. Vulkan-Log ohne `shader not found`.
- Deploy-Konsistenz geprüft: `HorizonRendering.dll` (60a9635c…) ist in `build`, `deploy/Editor`, `deploy/Game`,
  `deploy/Editor/Game` und `out_fix` identisch. `HorizonCore.dll` (6d5a899e…) ist identisch in `build`,
  `deploy/Editor`, `deploy/Editor/Game`, `out_fix` und `build/tests`. `ui.vert.spv` war vorher überall 19fb653e…,
  jetzt überall 1944830b….
- GL ist vorher wie nachher bitgleich an den Messpunkten. GL gegen D3D11 weicht auf 12,31 % der Pixel ab, vor und nach
  dem Fix gleich. Das sind die Stil-Features aus Befund 3.
- Die Szene (Himmel, Boden) sieht auf GL und D3D/Vulkan unterschiedlich aus. Das ist Thema 130
  (Spielpfad-Postprocessing) und hat mit der UI nichts zu tun.
- `he_tests` (Release, Scratch-APPDATA), Teilmenge Exporter/UI/Widget, 412 Fälle: viermal grün, einmal 1 Fehlschlag,
  der sich nicht reproduzieren ließ (siehe Ergebnis im Thema).

Nicht abgedeckt: ein Doctest für das Rendering selbst. Die UI-Passes laufen nur gegen echte Geräte, deshalb ist der
Nachweis das Skriptpaar oben. Metal ist weiterhin nicht gemessen und nicht geändert.
