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
