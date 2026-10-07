# UI-Bild-Quads auf Vulkan weiß: Ursache (Thema 157, Schritt 1)

Stand: 2026-10-07, NN-WS03 (RTX 4070), Release-Build des Zweigs auf `origin/main` 6866923d,
Baum `C:/hw157`, `DEPLOY_DIR=C:/hw157/deploy`.

## Ergebnis

Vulkans UI-Pass hat **keinen Textur-Pfad**. Die Bindung ist nicht kaputt und es greift kein Fallback auf eine
weiße Textur: `VulkanRenderer::runUIPass` liest `UIRenderObject::textureAssetId` überhaupt nicht. Ein
Image-Element wird deshalb als Vollton-Quad in seiner Tint-Farbe gezeichnet. Die Tint-Farbe steht per Vorgabe
auf Weiß (`UIImage::tint`), daher der weiße Quad. Mit einem anderen Tint wäre der Quad einfarbig in dieser Farbe.

Das ist keine Regression. In der Git-Historie kommt `textureAssetId` in `VulkanRenderer.cpp`,
`D3D11Renderer.cpp` und `D3D12Renderer.cpp` nie vor. Den Pfad haben nur OpenGL (`ResolveUITexture`, Modus 2),
Metal und der Software-Rasterizer. Thema 133 hat die Lücke schon benannt
(`docs/widgets-d3d-vulkan-analysis-2026-10-02.md`, „Was weiter fehlt“, Punkt 1), aber nicht gemessen, weil
der Zeuge keine Bild-Kachel hatte.

Im Widget-Designer ist das Bild trotzdem zu sehen, weil der Designer nicht durch den UI-Pass des Renderers
zeichnet. Er malt Image-Elemente mit ImGui (`dl->AddImage`, `src/HE_Editor/UIEditorPanel.cpp`, Fall
`UIWidgetType::Image`) und benutzt dafür das Textur-Handle aus `CreateImGuiTexture`. Das ist ein eigener Weg,
den jedes Backend hat.

## Was im Vulkan-Pfad fehlt, verglichen mit GL

| Stelle | OpenGL (Referenz) | Vulkan heute |
|---|---|---|
| Schleife im UI-Pass | `RenderUIPass`: bei `obj.type == 0 && obj.textureAssetId != 0` wird `ResolveUITexture` aufgerufen, die Textur auf Unit 0 gebunden und `uUIMode = 2` gesetzt | `runUIPass` (`VulkanRenderer.cpp`, Schleife über `m_renderWorld.uiObjects`): nur `params.x = type == 2 ? 1 : 0`, gebunden ist immer der Font-Atlas |
| Shader | `kUIFS` Modus 2: Textur × Tint, dieselbe SDF für runde Ecken | `shaders/ui.frag`: Modus 0 = Vollton plus Stil, Modus 1 = Glyphe (R8-Atlas). Es gibt keinen Texturzweig und nur den einen Sampler `uFontAtlas` (set 0, binding 0) |
| Asset → GPU-Textur | `ResolveUITexture`: eigener Cache `m_uiTexCache`, Upload **ohne** sRGB-Dekodierung (Thema 107: UI-Farben sind sRGB-Zahlen, sonst wird das Bild zu dunkel) | nichts. Es gibt nur `uiFontAtlasSet(key)` für R8-Atlanten. Der UI-Pool `m_uiAtlasDescPool` ist für Schriften bemessen (32 Sets) |
| Descriptor/Sampler | Textur und Sampler pro Draw an Unit 0 | Das Set-Layout `m_uiAtlasDSLayout` hat einen Combined-Sampler mit Immutable-Sampler für den Atlas. Ein RGBA8-Bild bräuchte ein eigenes Set pro Textur (gleiches Layout reicht, wenn der Sampler passt) |

Für Schritt 2 (Fix) heißt das, drei Teile sind neu:
1. ein Cache Asset → VkImage/VkImageView/Descriptor-Set für UI-Bilder, UNORM (nicht SRGB);
2. in `runUIPass` der Zweig `type == 0 && textureAssetId != 0`, der das Set bindet und Modus 2 schiebt;
3. in `ui.frag` ein Texturzweig (Textur × Tint, danach dieselbe Form/SDF wie Modus 0, wie `kUIFS`).
Der Pool muss mitwachsen oder einen eigenen bekommen. Die Push-Constants sind schon bei genau 128 B
(`static_assert` in `runUIPass`); der Modus passt aber in das vorhandene `params.x`.

## Reproduktion auf Hardware

Zeuge: `HE_DUMP_UITEST=image` (neu, `src/HE_Editor/EditorApplication.cpp`). Er zeigt die 12 Stil-Kacheln aus
Thema 133 und zusätzlich Kachel 12: ein `UIImage` bei x 60–300, y 590–700 mit einem erzeugten 64×64-Bild,
oben links rot, oben rechts grün, unten links blau, unten rechts gelb, Tint weiß. Die Textur wird unter dem
virtuellen Pfad `__uiImageWitness.hasset` registriert, weil das Widget sein Bild nur per Pfad kennt und
`WidgetManager` es beim Instanziieren neu auflöst. `HE_DUMP_UITEST=1` bleibt unverändert; die Messbasis von
Thema 133 gilt also weiter.

```powershell
& scripts\ui-image-repro\cap157.ps1 -Deploy C:\hw157\deploy -Shots C:\hw157\shots -Tag img
python scripts\ui-image-repro\ana157.py C:\hw157\shots img       # Exit = Zahl der Backends ohne Bild
```

| Backend | Kachel 0 (Kontrolle) | TL | TR | BL | BR | Urteil |
|---|---|---|---|---|---|---|
| OpenGL | (66,76,97) | (230,30,30) | (30,200,60) | (30,80,230) | (240,220,40) | **Bild** |
| Vulkan | (66,76,97) | (255,255,255) | (255,255,255) | (255,255,255) | (255,255,255) | **weiß** |
| D3D11 | (66,76,97) | (255,255,255) | (255,255,255) | (255,255,255) | (255,255,255) | **weiß** |
| D3D12 | (66,76,97) | (255,255,255) | (255,255,255) | (255,255,255) | (255,255,255) | **weiß** |

![Bild-Kachel auf den vier Backends](img/ui-image-quads-2026-10-07/bild-kachel-4-backends.png)

- GL ist die Positivkontrolle: Der Zeuge selbst funktioniert, die Farben kommen bytegenau an.
- Gegenprobe `HE_DUMP_UITEST=1` (ohne Bild-Kachel, Tag `plain`): An derselben Stelle steht der Hintergrund,
  GL (0,0,0), Vulkan (4,4,4). Das Skript meldet dort „missing“. Weiß heißt also wirklich „gezeichnet, aber
  ohne Textur“ und nicht „fehlt“.
- Vulkan lief auf der RTX 4070 mit aktivem Validation-Layer (`VulkanRenderer: validation layer ENABLED`, Khronos
  1.4.341). Der Debug-Callback schreibt Layer-Meldungen als `[ WARN]`/`[ERROR]` ins Log. Die
  Session-Zusammenfassung sagt „1 warning(s), 0 error(s)“, und die eine Warnung ist „No config file …“ (frisches
  APPDATA). Es gibt also **keine** Validation-Meldung. Das passt zu „nie gebaut“: Der Shader liest nichts außer
  dem gebundenen Atlas. Gegen eine falsche Bindung spricht das ebenfalls. Bei D3D11/D3D12 ist es genauso
  (D3D12 ohne `HE_GPU_DEBUG`, also ohne Debug-Layer).
- Die 12 Stil-Kacheln bleiben mit dem neuen Zeugen auf Vulkan, D3D11 und D3D12 bei 0 Fehlschlägen
  (`docs/widgets-d3d-vulkan-tiles.py`, Tags `plain` und `img`).

## D3D11/D3D12 (Nebenbefund für Schritt 3)

Beide zeigen denselben weißen Quad, gemessen im selben Lauf. Die Ursache ist dieselbe: `renderUIPass`
(D3D11) und `renderUIPass12` (D3D12) setzen `mode = type == 2 ? 1 : 0` und lesen `textureAssetId` nicht.
Thema 133 hat abgeschätzt, was dort jeweils fehlt: Auf D3D11 ist es klein (`graphTexCache`/`createAlbedoSRV`
gibt es schon). Auf D3D12 gehören die SRVs in den shader-sichtbaren UI-Heap (heute `k_maxUIFontAtlases`
Slots), dazu kommen Upload und Barrieren vor dem Pass.

## Auch betroffen (nicht gemessen)

`quad(..., textureAssetId)` ist nicht auf UIImage beschränkt. Auch Panel, Border, Button und weitere Elemente
mit gesetzter Textur geben `textureAssetId` mit (`UIElement.cpp`, Aufrufe von `quad`). Auf Vulkan, D3D11 und
D3D12 sind deren Texturen aus demselben Grund unsichtbar. Der Fix in Schritt 2 deckt sie mit ab, wenn er im
UI-Pass auf `type == 0 && textureAssetId != 0` prüft wie GL.

## Nicht geprüft

- Das exportierte Spiel (Swapchain-Pfad) ist nicht aufgenommen. `runUIPass` ist dieselbe Funktion für den
  Swapchain- und den Viewport-Pipeline-Pfad (`m_uiPipeline` / `m_uiViewportPipeline`), deshalb ist dort
  dasselbe zu erwarten. Gemessen ist es nicht.
- Metal ist nicht gemessen. Laut Code hat es `ResolveUITexture`.
