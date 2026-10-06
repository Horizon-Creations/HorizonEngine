# Vulkan: DDGI-Probe-Feld ohne Farbbounce, Ursache (Thema 154, Schritt 1)

Stand: 06.10.2026, NN-WS03 (RTX 4070, Vulkan 1.4.341), Release-Build auf `9b00bfb1` + Diagnose-Log,
privater Deploy `C:\hw154\deploy`. Symptom und Zeuge: `docs/ddgi-graph-material-hardware-acceptance-2026-10-06.md`
§3 (Zweig `claude/ddgi-graph-material-pfad-visuelle-hardware-abnahme-auf-d3d11`).

## Ursache

`VulkanRenderer::runGi` extrahiert `m_renderWorld` selbst neu (`m_extractor.extract`, `VulkanRenderer.cpp`
in `runGi`) und ruft danach `updateGiAccel()` auf, **ohne** `HE::resolveWorldMaterialScalars`. Der Extraktor
lässt `RenderObject::baseColor` auf (1, 1, 1) (`RenderObject.h`: „The extractor leaves the defaults;
D3D11/D3D12/Vulkan fill them through HE::resolveWorldMaterialScalars“). Auf Vulkan passiert dieser Resolve
nur in `DrawScene` und im Decal-Vorpass, beide **nach** `runGi`, auf einer eigenen Extraktion.

D3D11 (`D3D11Renderer.cpp`, `resolveWorldMaterialScalars` direkt vor `p.updateGiAccel`) und D3D12
(ebenso) lösen vorher auf. Darum bekommt dort die GI-Instanz die Materialfarbe, auf Vulkan dagegen jede
Instanz Weiß. Roter und grauer Boden ergeben dasselbe Feld, also Bleed exakt 0,00, mit HW- und mit SW-Strahlen.

Das CPU-seitige Füllen in `updateGiAccel` ist auf allen drei Backends derselbe Code
(`inst.baseColor = obj.baseColor`). Der Unterschied liegt nur darin, ob `obj.baseColor` zu diesem Zeitpunkt
schon aufgelöst ist.

## Laufzeitbeleg

`HE_GI_LOG_INSTANCES=N` (neu, Vulkan + D3D11) loggt beim N-ten `updateGiAccel` jede GI-Instanz mit
Position, Material und `baseColor`. Aufnahme: `cap148.ps1 -Scene bleed -Bleed 3|4 -Extra @{HE_GI_LOG_INSTANCES='30'}`.

| Backend | Boden rot (b3) | Boden grau (b4) | Graph-Kugel |
|---|---|---|---|
| Vulkan (main) | (1, 1, 1) | (1, 1, 1) | (1, 1, 1) |
| D3D11 | (1, 0.05, 0.05) | (0.5, 0.5, 0.5) | (0.2, 0.8, 0.3) |
| Vulkan + Resolve in `runGi` (Gegenprobe, nicht committet) | (1, 0.05, 0.05) | (0.5, 0.5, 0.5) | (0.2, 0.8, 0.3) |

## Gegenprobe (nicht committet)

Eine Zeile `HE::resolveWorldMaterialScalars(m_renderWorld, m_contentManager);` vor `updateGiAccel()` in
`runGi`, inkrementell gebaut. Gemessen wurde mit derselben Metrik wie `ana148.py`: (R−G)[rot] − (R−G)[grau]
auf der unteren Kugelhälfte.

| | Graph-Kugel (b3/b4) | eingebaute Kugel (b1/b2) |
|---|---|---|
| Vulkan (main) | 0,00 | 0,00 (Thema 148) |
| Vulkan + Resolve | **19,87** | **69,56** |
| D3D11 | 19,87 | 69,56 |

Vulkan trifft D3D11 danach bitgenau. Einen zweiten Defekt (Kernel, Atlas, Dispatch) gibt es nicht.

## Für Schritt 2 (offen, nicht entschieden)

- **Form des Fixes:**
  - (a) wie D3D: Resolve in `runGi` vor `updateGiAccel`. Belegt durch die Gegenprobe.
  - (b) alle drei Backends auf `HE::giInstanceSurface` umstellen, wie GL und Metal.
    `GiInstanceSurface.h` behauptet „Every backend routes through THIS function“, D3D11/D3D12/Vulkan tun
    es aber nicht. Unterschied: Bei Graph-Materialien liefert der Resolve nur den Skalar
    `MaterialAsset::baseColor` (hier (0.2, 0.8, 0.3)), `giInstanceSurface` dagegen die Approx-Faltung der
    BaseColor-Pins (Param-Slots, Landscape-Layer).
  - (b) verschiebt also auch D3D11/D3D12 (Terrain, Graph-Kugel) und ändert deren Abnahmewerte aus
    Thema 148.
- **Geschwister-Pässe:** Weitere Vulkan-Vorpässe extrahieren `m_renderWorld` selbst (SSAO, Schatten). Zu
  prüfen ist, ob sie `opacity`/`baseColor` unaufgelöst lesen. Der Decal-Vorpass löst bereits auf.
- **Abnahme:** die Tabelle oben, dazu `ana148.py` gegen GL.

## Schritt 2: Fix und Abnahme

**Fix, Form (a) wie D3D:** `VulkanRenderer::runGi` ruft nach dem eigenen Extract und vor `updateGiAccel()`
`HE::resolveWorldMaterialScalars(m_renderWorld, m_contentManager)` auf. Die Graph-Materialien bekommen damit
denselben Skalar `MaterialAsset::baseColor` wie auf D3D11/D3D12. Form (b), also `giInstanceSurface` auf allen
drei Backends, bleibt offen. Sie würde die Abnahmewerte von D3D11/D3D12 aus Thema 148 mitverschieben und ist
ein eigenes Thema.

**Geschwister-Vorpässe:** Schatten (`m_extractor.extract` im CSM/Local-Pass) und SSAO/Reflexions-MRT lesen
weder `baseColor` noch `opacity`, `metallic` oder `roughness`. Sie lesen nur Transformation, `castsShadow` und
`contributesAO`. `RenderCuller`/`RenderSorter` lesen ebenfalls keinen Skalar. Dort ist kein Resolve nötig. Der
Decal-Vorpass löst schon auf.

**Pin:** `tests/test_culling.cpp`, „GI instances get their material colour before the acceleration update
(Thema 154)“. Er prüft im Quelltext die Reihenfolge extract → resolve → `updateGiAccel` für Vulkan `runGi`
und resolve → `p.updateGiAccel` für D3D11/D3D12. Negativkontrolle: Ohne die Zeile in `runGi` fällt der Test
mit „runGi no longer resolves material scalars“.

**Messung:** Release `C:\hw154`, Deploy `C:\hw154\deploy` mit Fix, RTX 4070, Aufnahmen in `C:\hw154\cap2` mit
der `ana148.py`-Namensgebung (`post_<rhi>_b<n>_gi<g>`). Aufgenommen per `cap148.ps1 -Deploy C:\hw154\deploy
-Out C:\hw154\cap2`. Tabelle A von `ana148.py`, (R−G)[rot] − (R−G)[grau], untere Kugelhälfte:

| Backend | Graph-Kugel GI an | Graph-Kugel GI aus | eingebaute Kugel GI an | eingebaute Kugel GI aus |
|---|---|---|---|---|
| OpenGL | 20,19 | 0,00 | 46,77 | 0,00 |
| D3D11 | 19,87 | 0,00 | 69,56 | 0,00 |
| D3D12 | 19,87 | 0,00 | 69,56 | 0,00 |
| Vulkan vor Fix (Schritt 1, `C:\hw154\cap\Vulkan_b3/b4`) | 0,00 | – | 0,00 (Thema 148) | – |
| **Vulkan mit Fix, HW-Ray-Query** | **19,87** | 0,00 | **69,56** | 0,00 |
| **Vulkan mit Fix, `HE_GI_FORCE_SW=1`** | **19,87** | – | **69,56** | – |

- Vulkan b3 gegen D3D11 b3 (Graph-Maske): mittleres |d| 0,0001, max. 1 Stufe. Gegen GL (Tabelle C): 0,15 wie
  D3D11/D3D12.
- Rauschen (b3 GI an, zweite Aufnahme): 0,00 auf allen vier Backends.
- HW gegen SW auf Vulkan: |d| 0,00. Belegt ist nur, dass die HW-Kernel gebaut sind („GI HW ray-query kernels
  built“), bzw. dass der SW-Pfad erzwungen ist („HE_GI_FORCE_SW set“). Eine Gegenprobe pro Dispatch (z. B.
  `gi_probe_hw.spv` tauschen) wurde nicht gemacht. Der Fix sitzt CPU-seitig in `giInsts`, die beide Pfade lesen.
- Der Abstand GL ↔ D3D bei der eingebauten Kugel (46,77 gegen 69,56) bestand schon vor Thema 154. Vulkan
  stimmt jetzt mit D3D überein.
- Vulkan-Validation (HE_GPU_DEBUG an): vor und nach dem Fix dieselbe Art, `VUID-vkCmdUpdateBuffer-renderpass`
  (bei 10 gesättigt, vorbestehend), keine neue.
