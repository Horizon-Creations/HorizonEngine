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

---

# Schritt 3: Restunterschied zu GL benannt und behoben

Stand: 2026-10-02, NN-WS03, Release-Build aus dem Zweig in `C:/hw133`. Code-Commit `966c4614`.

## Was genau anders war, nach den Kategorien der Aufgabe

| Kategorie | Befund vor Schritt 3 | Beleg |
|---|---|---|
| Farbe | **gleich.** Flache Quads haben auf allen vier Backends dieselben Werte. | Spiel: Rot (229,25,25), Grün (25,204,51), Blau (25,76,229) auf allen vier. Editor: Kachel 0 (66,76,97) |
| Schriftglättung | **gleich, bitgenau.** | Spiel-Capture, Ausschnitt der „WIDGET“-Box (440×141 px, 2438 Textpixel): max. Abweichung D3D11/D3D12/Vulkan zu GL = **0** |
| Form/Anti-Aliasing/Verlauf/Schatten | **das war der Unterschied.** `kUIFS` (GL) hat `heRoundedBoxSDF`, Rahmen, linearen/radialen Verlauf, Blur-Drop-Shadow und Inner Shadow. `kUIHLSL`, `kUIHLSL12` und `ui.frag` konnten nur `return uColor`. | pro Kachel, Tabelle unten |
| Alpha/Blending | **D3D11/D3D12 schrieben Quad-Alpha ins Ziel.** Der Blend-State war `SrcBlendAlpha=ONE, DestBlendAlpha=ZERO`. Ein Drop-Shadow-Quad (Alpha 0,45) setzte das Viewport-RT dort auf Alpha 115. Der Editor zeigt das RT per `ImGui::Image` **mit** Blending (`ViewportPanel.cpp:910`), also war der Schatten in PIE durchsichtig. In den RGB-Dumps war das nicht zu sehen. Vulkan (`ONE/ONE_MINUS_SRC_ALPHA`) blieb bei 255. | Dumps sind 32 bit: D3D11/D3D12 hatten 24480 Pixel mit Alpha < 255, Vulkan 0 |
| Quad-Limit (Nebenbefund) | D3D12 brach den UI-Pass nach `k_maxUIQuads = 256` ab. Jeder Glyph ist ein Quad, ein Menü mit etwas Text verlor ab dem 257. Quad still den Rest. GL, D3D11 und Vulkan haben kein Limit. | Code (`D3D12Renderer.cpp`, `renderUIPass12`). Mit > 256 Quads **nicht** gemessen, der Witness hat 14 |

## Was geändert ist (`966c4614`)

- **D3D11** (`kUIHLSL`) und **D3D12** (`kUIHLSL12`): Der Pixelshader ist Zeile für Zeile `kUIFS`. `UICB` wächst von 80 auf
  176 B (+ cornerRadius, borderColor, gradientColor, innerColor, style0, style1). D3D11 bekommt einen eigenen lokalen
  0..1-Interpolanten (`TEXCOORD1`), D3D12 hatte ihn schon. `static_assert` auf die Größe.
- **Vulkan** (`shaders/ui.vert`, `shaders/ui.frag`): derselbe Shader in GLSL. Die Push-Constants wachsen von 80 auf genau
  **128 B**, das garantierte Minimum von `maxPushConstantsSize`, ohne UBO. Dafür sind die drei Zusatzfarben als
  `unorm8x4` gepackt, Rahmenbreite und Verlaufswinkel liegen in bisher freien Komponenten. Das Ziel ist 8 bit, es fällt also nichts weg.
  Farben außerhalb 0..1 werden geklemmt.
- **D3D11/D3D12-Alpha-Blend**: `DestBlendAlpha = INV_SRC_ALPHA` wie bei Vulkan. Ein deckendes Ziel bleibt deckend.
- **D3D12**: `k_maxUIQuads` von 256 auf 4096 (1 MB Upload-Puffer pro Frame-Slot).

Nicht geändert: GL und Metal.

## Messung

`docs/widgets-d3d-vulkan-tiles.py` vergleicht die 12 Witness-Kacheln (`HE_DUMP_UITEST`) einzeln mit GL. Gemessen werden
die RGB-Abweichung (> 8 Stufen) und die Zahl der Pixel mit Alpha < 255. Exit-Code = Zahl der fehlgeschlagenen
(Kachel, Backend)-Paare.

```powershell
& docs\widgets-d3d-vulkan-editor-capture.ps1 -Tag sdf          # Editor/PIE-Captures
python docs\widgets-d3d-vulkan-tiles.py C:/hw133/shots sdf    # nach Schritt 3: 0
python docs\widgets-d3d-vulkan-tiles.py C:/hw133/shots fix    # Stand Schritt 2: 32
```

| Kachel | vorher (`fix`), D3D11 = D3D12 = Vulkan | nachher (`sdf`), alle drei |
|---|---|---|
| 0 plain | 0,00 % | 0,00 % |
| 1 round 24 / 2 tab / 3 leaf | 1,07 / 0,73 / 0,84 % | 0,00 % |
| 4 border | 5,66 % | 0,00 % |
| 5 grad lin / 6 grad rad / 7 grad 90 | 49,13 / 49,76 / 47,91 % | 0,00 % |
| 8 drop shadow | 0,49 %, D3D: 13896 px Alpha 115 | 0,00 %, Alpha überall 255 |
| 9 inner shadow | 13,00 % | 0,00 % |
| 10 shadow+border | 3,47 %, D3D: 10584 px Alpha 115 | 0,00 %, Alpha überall 255 |
| 11 capsule rad+inner | 50,43 % | 0,00 % |
| **alle Kacheln** | **18,54 %**, 32 Fehlschläge | **0,00 %**, 0 Fehlschläge |

- Ganzes Bild GL gegen D3D11: vorher 12,31 %, jetzt **0,00 %** (`diffbmp.py`, > 8 Stufen). Innerhalb der Kacheln bleiben
  Abweichungen von höchstens 4 Stufen. Ursachen: die Kanten mischen über dem Szenenhintergrund, und der ist auf GL (0,0,0),
  auf D3D/Vulkan (4,4,4). Auf Vulkan weichen zusätzlich 41k Pixel um 1 Stufe ab, vermutlich die 8-Bit-Quantisierung der
  gepackten Verlaufsfarbe. Das ist nicht einzeln nachgewiesen.
- Die Folge des Alpha-Fehlers im Editor-Fenster (durchsichtiger Schatten in PIE) ist aus dem Code abgeleitet:
  `ImGui::Image`, ImGui-Backends blenden mit `SRC_ALPHA`. Am Editor-Bildschirm selbst ist sie **nicht** gemessen. Gemessen ist der
  Alphakanal des Viewport-RT in den Dumps.
- GL ist bitgleich mit dem Stand von Schritt 2 (`sdf_OpenGL` gegen `fix_OpenGL`: 0,00 %).
- Spielpfad (Swapchain): `widgets-d3d-vulkan-verify.ps1 -EditorTag sdf -GameTag gsdf` gibt **0 Fehlschläge**, und der
  „WIDGET“-Text ist weiter bitgleich mit GL. Der Lauf nutzte `C:/hw133/out_sdf` = `out_fix` (MCP-Export aus Schritt 2), in dem
  genau die geänderten Runtime-Dateien aus `deploy/Editor/Game` ersetzt sind: `HorizonRendering.dll`, `HorizonGame.exe`
  (neu gelinkt) und `Shaders/ui.{vert,frag}.spv`. Ein Hash-Vergleich zeigte, dass sich sonst nichts unterschied. Es war **kein** frischer MCP-Export.
- Shader: `scripts/validate_embedded_shaders.py`, alle 105 eingebetteten Shader kompilieren, `kUIHLSL`/`kUIHLSL12` VS+PS
  eingeschlossen. Die `.spv` baut glslangValidator im Build.
- Deploy-Konsistenz: `HorizonRendering.dll` 118F1259… ist identisch in `build`, `deploy/Editor`, `deploy/Game` und
  `deploy/Editor/Game`. `ui.frag.spv` A2CD80A5… und `ui.vert.spv` D5DD0ADD… sind identisch in `build/Shaders`, `deploy/Editor/Shaders`,
  `deploy/Game/Shaders` und `deploy/Editor/Game/Shaders`. Achtung: Nach dem ersten Build hatten `deploy/Game` und
  `deploy/Editor/Game` noch die alte DLL und die alten `.spv`, weil `HorizonGame` nicht neu gelinkt hatte. Erst das Löschen
  beider Exe im Build-Baum und ein neues Linken hat das behoben.

## Was weiter fehlt (Umfang für ein Folgethema)

1. **UIImage mit Textur (`textureAssetId`)**: Auf D3D11/D3D12/Vulkan erscheint weiter eine einfarbige Tint-Fläche. GL nutzt
   Modus 2 (Textur × Tint, mit gerundeten Ecken über dieselbe SDF). Der Shader-Teil ist klein. Pro Backend fehlt aber die
   Auflösung Asset → GPU-Textur im UI-Pass:
   - D3D11: Das ist klein, `graphTexCache`/`createAlbedoSRV` gibt es schon.
   - D3D12: Die SRVs gehören in den shader-sichtbaren UI-Heap, der heute nur `k_maxUIFontAtlases` Slots hat. Dazu kommen Upload und Barrieren vor dem Pass.
   - Vulkan: ein Descriptor-Set pro Textur im UI-Pool. Der hat heute 32 Sets, die für Schriften gedacht sind.

   Der Witness enthält keine Textur-Kachel. Ein Port braucht deshalb zuerst eine, sonst lässt er sich nicht messen.
2. **UI-Materialien (`materialAssetId`, `uiState`, Backdrop-Blur)**: Das ist groß. GL und Metal haben einen eigenen UI-Materialpfad
   (`GetOrBuildUIMaterialProgram`, `MaterialShaderLibrary::uiVertex`, `HeUI`-Block, Backdrop-Snapshot mit Mip-Kette für
   „Frosted Glass“). D3D11/D3D12/Vulkan haben den Material-Graph-Pfad nur für Meshes. Nötig wären eine UI-Vertex-Variante
   in HLSL und SPIR-V, der `HeUI`-Block und eine Backdrop-Kopie mit Mips je Backend.
3. **GL-Alpha** (nur Hinweis, nicht geändert): GL mischt mit `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` auch den
   Alphakanal. Unter einem Drop-Shadow bleibt das Viewport-RT deshalb bei a² + (1 − a), also mindestens 0,75. Die Tabelle
   oben zählt 12292 bzw. 10624 solche Pixel. Im Editor ist der Schatten auf GL also leicht durchsichtig. Ein
   `glBlendFuncSeparate(…, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` im UI-Pass würde das wie bei D3D/Vulkan beheben.
