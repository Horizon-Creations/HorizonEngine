# Himmel, Wetter & indirekte Beleuchtung — Performance-Recherche & Umsetzungsplan

> Stand: 2026-06-24 · Ziel: glaubwürdiger, dynamischer Himmel + Wetter für **Catania** (Open-World-Mittelalter), der bei leerer Welt **niemals unter 200 FPS** (Frame-Budget 5 ms) drückt. Der Himmel ist Hintergrund — er darf nur einen Bruchteil des Budgets kosten.

Diese Recherche vergleicht Unreal Engine 5, Unity HDRP und die Marketplace-Assets *Ultra Dynamic Sky / Weather* (UDS/UDW) und leitet daraus einen priorisierten Umbauplan für HorizonEngine ab. Alle externen Zahlen sind mit Primärquellen belegt (Hillaire EGSR 2020, Schneider/Nubis SIGGRAPH 2015/17, Lagarde „Water drop", GPU Gems 2, Epic-/Unity-Docs).

---

## 0. Performance-Budget (das Maß aller Dinge)

200 FPS = **5,0 ms Gesamt-Frame**. Realistische Zielaufteilung für den reinen Hintergrund:

| Teilsystem | Zielbudget | Referenz aus der Industrie |
|---|---|---|
| Atmosphäre/Himmelfarbe | **≤ 0,2 ms** | UE Sky-Atmosphere komplett ~0,2 ms auf GTX 1080 |
| Volumetrische Wolken | **≤ 1,0 ms** | Horizon Zero Dawn: ~2 ms auf PS4 (deutlich schwächere HW) |
| Niederschlag (Partikel) | **≤ 0,5 ms** | Lagarde Regen-Kern ~2,8 ms auf PS3 (alt) |
| Himmel-IBL/Ambient | **≤ 0,2 ms** | UE SkyLight time-sliced ~0,2 ms/Frame |
| **Summe Hintergrund** | **≈ 1,9 ms** | bleibt unter 2 ms → > 200 FPS möglich |

Kernprinzip aller großen Engines: **die teure Berechnung von der Pixel-/Frame-Rate entkoppeln**. Himmel und Beleuchtung ändern sich langsam → in LUTs vorrechnen, niedrig auflösen, zeitlich amortisieren.

---

## 1. Ist-Zustand HorizonEngine (Code-Map)

| Bereich | Implementierung | Datei(en) |
|---|---|---|
| Himmel/Atmosphäre | Prozedural (Preetham-artig), **voll per-Pixel jeden Frame**, kein LUT, kein Half-Res, keine Temporal-Wiederverwendung | `src/HE_Rendering/shaders/sky.frag` (~450 Z.), `sky.vert` |
| Sonne/Mond/Sterne/Nebula | Analytische Disks + Hash-Sterne + FBM-Nebula, alles in `sky.frag`; Sonnenbogen aus `timeOfDay` in `RenderExtractor.cpp` | `sky.frag`, `RenderExtractor.cpp:320–406` |
| Wolken | 3D-Noise-Raymarch **inline in `sky.frag`**, fixe **16 Schritte** + 3 Schatten-Schritte, 256³-Noise-Volume (R=Value, G=Worley), HG-Phase g=0.6, **keine Temporal-Reprojection, kein Low-Res-Buffer** | `sky.frag:175–300` |
| Wetter | `WeatherComponent`/`WeatherSystem` blendet Presets → `EnvironmentComponent` (cloudCoverage/fog/wind/precip), 7×7-Boden-Grid via Raycasts | `WeatherComponent.h`, `WeatherSystem.{h,cpp}` |
| GPU-Niederschlag | GL: Transform-Feedback (`kParticleSimVS`/`kParticleDrawVS`, GLSL 4.1); Metal: Compute-Kernel `particleSim`; **1 Mio.-Pool**, kamerafolgende Spawn-Box (16 u Halbweite, 24 u über Kamera) | `OpenGLRenderer.cpp`, `MetalRenderer.mm:3404–3588` |
| Indirekte Beleuchtung | **Sky-basiertes Ambient-IBL**: `scene.frag` ruft `skyColor()` für Diffus (Normal-Up) und Spekular (Reflexionsrichtung) auf — **kein SH, keine Probes, keine Cubemap**. Bei >0,5 Wolken fadet Direktlicht aus → Energie in Ambient | `scene.frag:130–140`, `RenderExtractor.cpp:340–408` |
| Tick/Loop | `GameApplication::OnRender` → `SceneSystems::tick` (Weather/Particles/…) → `renderer->Render()` → ShadowPass → GeometryPass (Sky-Fullscreen → Meshes → Billboards) → PostFX | `GameApplication.cpp:83–124`, `SceneSystems.cpp:59–81` |
| Backends | Sky/Wolken in allen Backends (GL/Metal/D3D11/D3D12/Vulkan); GPU-Partikel nur GL+Metal | — |

**Zentrale Erkenntnis:** Der Himmel ist gleichzeitig der **größte Kostenpunkt** *und* die **einzige Quelle indirekter Beleuchtung**. `scene.frag` ruft `skyColor()` **mehrfach pro Lichtpixel** auf (Diffus + Spekular). Jede `skyColor()`-Verteuerung trifft also nicht nur den Hintergrund, sondern jedes beleuchtete Objekt. Das ist die wichtigste Optimierungs-Hebelstelle.

---

## 2. Was die großen Engines tun (belegte Erkenntnisse)

### 2.1 Atmosphäre — UE5 Sky Atmosphere (Hillaire EGSR 2020)
- **Kein Bruneton-4D-LUT**, sondern Raymarch für Single-Scattering + 4 kleine LUTs + nicht-iterative Multi-Scatter-Näherung (geometrische Reihe, Gl. 9).
- **LUT-Auflösungen & Kosten (GTX 1080):** Transmittance 256×64 (40 Schritte, ~0,01 ms), Sky-View 200×100 (30 Schritte, ~0,05 ms), Aerial-Perspective 32³ Froxel über 32 km (~0,04 ms), Multi-Scatter 32² (~0,07 ms). **Gesamt ≈ 0,2 ms.**
- **Der Trick für „billiger Hintergrund":** Himmelspixel lesen **eine einzige Sky-View-LUT-Stelle** statt per-Pixel zu marchen. Cinematic-Modus (`r.SkyAtmosphere.FastSkyLUT 0`) fällt auf vollen Raymarch zurück.
- **Sonne = Atmosphäre + Directional Light** (Disk via `SkyAtmosphereLightDiskLuminance`). **Mond = zweites Atmosphärenlicht (Index 1).** **Sterne/Milchstraße sind NICHT Teil der Atmosphäre** → separate Sky-Dome-Mesh mit Stern-Cubemap.
- Quellen: Hillaire EGSR 2020 (`sebh.github.io/publications/egsr2020.pdf`), UE Sky-Atmosphere-Docs, Referenz-D3D11 `github.com/sebh/UnrealEngineSkyAtmosphere`.

### 2.2 Volumetrische Wolken — Nubis/HZD + UE5/HDRP
- **Raymarch (HZD-Kanon):** **64–128 View-Schritte** durch die Wolkenschicht, **6 Licht-Samples pro Schritt in einem Cone** (weiche Selbstschattierung). Adaptiv: billiger Vortest „Wolke ja/nein", große Schritte bis Treffer → kleine Schritte im Volumen.
- **Noise (exakte HZD-Spezifikation):** Basis-Shape **128³ RGBA** (R=Perlin-Worley, GBA=Worley steigender Frequenz), Detail/Erosion **32³ RGB** (Worley), Weather-Map **512² 2D**, ~5 Oktaven Perlin (Coverage/Type/Precip in Kanälen).
- **Temporal (der größte Gewinn):** Wolken in **Viertel-Auflösung** rendern, **nur 1/16 Pixel pro Frame** marchen, die restlichen 15/16 per **Reprojection** rekonstruieren → voller Frame über **16 Frames** (4×4-Bayer-Offset + 8er-Halton-Jitter, ~75 % Neu-Anteil gegen Ghosting).
- **Wolkenschatten:** separate **512² Opacity-Shadow-Map** auf den Boden.
- **Kosten:** **~2 ms GPU auf PS4** als Zielbudget für das ganze Wolkensystem.
- UE5/HDRP exponieren das als Slider/CVAR statt feste Defaults (UE: `r.VolumetricCloud.*SampleMaxCount`, `DistanceToSampleMaxCount=15 km`; HDRP: Num Primary Steps [linear], Num Light Steps [**exponentiell teuer**], Temporal Accumulation Factor).
- Quellen: Schneider SIGGRAPH 2015/2017 (advances.realtimerendering.com), jpgrenier.org/clouds.html, bitsquid-Blog, UE/HDRP-Docs.

### 2.3 Wetter — UDS/UDW, Niagara, VFX Graph, Lagarde
- **Niederschlag = GPU-Partikel in kamera-lokalem Volumen** mit **festen Bounds** (Pflicht bei GPU-Sim — die CPU kennt Partikelpositionen nicht und kann nicht cullen). UDW: „nur im Kamera-Raum berechnet, sehr effizient". GPU lohnt erst **ab ~1000 Partikeln**.
- **Overdraw ist der echte Kostentreiber**, nicht die Simulation. Große, überlappende transparente Streifen/Flocken kosten Füllrate. Mitigation: weniger/kleinere Nah-Partikel, ggf. **4 texturierte Shells statt Millionen Einzeltropfen** (ATI/Lagarde).
- **Regen = gestreckte Billboards**, **Schnee = langsame, weiche Partikel**. (Beides hat HorizonEngine bereits.)
- **Nässe (Lagarde „Water drop 3b", PBR-Standard, getrieben von einem globalen `WetLevel` 0–1):**
  - Albedo abdunkeln: `Diffuse *= lerp(1.0, factor, WetLevel)` (factor ~0,2–1,0, abhängig von Porosität); Sättigung leicht erhöhen.
  - Rauheit senken / Glanz erhöhen: `Gloss = lerp(1, Gloss, lerp(1, factor, 0.5*WetLevel))`.
  - Metalle schützen: `factor = lerp(1, 0.2, (1-Metalness)*Porosity)`.
  - Pfützen: höhenbasiert `saturate((FloodLevel - height)/0.4)`, Normale → flach.
  - **Regen-Occlusion-Maske** = Tiefenkarte aus Regenrichtung (Shadow-Map-Stil), damit überdachte Flächen trocken bleiben.
- **Schnee:** Up-Facing-Maske via `dot(worldNormal, up) > SnowLevel`, optional Vertex-Extrusion für Tiefe; Material trägt Color/Smoothness/Normal/Emissive (Funkeln).
- **UDW-Architektur (übernehmenswert):** globale Nässe/Schnee über **eine geteilte Material-Parameter-Collection**; spielerfolgende Pfützen/Schnee über **rezentrierende Render-Targets**; per-Actor „Interaction"-Komponente für Fußspuren/Ripples. **Half-Rate-Tick** (Logik nur jeden 2. Frame über 45 FPS).
- Quellen: Epic Niagara-Docs, Unity VFX-Graph/Shader-Graph-Weather-Sample, Lagarde „Water drop 2a/2b/3b", ATI SIGGRAPH 2006, UDS/UDW-Docs (ultradynamicsky.com V6–V9).

### 2.4 Indirekte Beleuchtung — der billige Pfad
- **Lumen ist hier das falsche Werkzeug:** selbst „High" zielt auf **~4 ms** (60 FPS); bei 5 ms Gesamt-Budget unbezahlbar. Lumen-Cache konvergiert pro Probe → schnelle Tageszeit erzwingt Re-Konvergenz → noch teurer.
- **Der bewährte billige Rezept (genau was UE SkyLight & Unity Ambient-Probe intern tun):**
  - **Diffus = Himmel-Irradianz als Order-2-SH (9 Koeffizienten/Kanal).** Order-2 ist für Diffus beweisbar ausreichend (Ramamoorthi/Hanrahan 2001). Projektion einer kleinen Sky-Cubemap (32²/64²) → 9 SH-Koeffizienten ist **< 0,1 ms** (GPU Gems 2, Kap. 10). UE misst den Diffus-Schritt mit **0,015 ms**.
  - **Spekular = vorgefiltertes Sky-Cubemap mit Rauheits-Mips.** Das ist der teure Teil (UE: 0,80 ms Convolution). **Amortisieren:** nur alle N Frames / eine Cube-Face oder ein Mip pro Frame → UE „Time Slicing" über **9 Frames** → Peak **~0,2 ms/Frame**.
- **Statische Sky-Occlusion ohne Laufzeitkosten (Unity APV):** pro Probe einen statischen Sky-Sichtbarkeits-Skalar backen, zur Laufzeit mit der **dynamischen** SH-Himmelfarbe multiplizieren → Tag-Nacht ohne Re-Bake. Optional für später (Innenräume/Höhlen in Catania).
- Quellen: UE SkyLight-Docs (ms-Breakdown), UE Lumen-Performance-Guide, Unity APV-Sky-Occlusion-Docs, GPU Gems 2 Kap. 10, Wright et al. Lumen SIGGRAPH 2022.

---

## 3. Gap-Analyse — wo HorizonEngine Performance verschenkt

| # | Lücke | Heutige Kosten | Industrie-Standard | Hebel |
|---|---|---|---|---|
| **G1** | `skyColor()` wird per-Pixel **mehrfach** ausgewertet (Himmel + Diffus-Ambient + Spekular-Ambient in `scene.frag`) | Voller prozeduraler Sky-Eval × (Sky-Pixel + 2× pro Lit-Pixel) | Sky-View-LUT + SH-Ambient: ein Tex-Lookup | **Sehr hoch** |
| **G2** | Wolken **voll per-Pixel, jeden Frame, 16 Schritte**, kein Temporal, kein Low-Res | Dominanter Sky-Pass-Kostenpunkt | Viertel-Res + 1/16-Temporal über 16 Frames | **Sehr hoch** |
| **G3** | Kein vorgefiltertes Spekular-IBL → Spekular-Ambient ist nur ein einzelner `skyColor()`-Sample (raue Reflexion sieht falsch aus *und* kostet vollen Eval) | Qualität *und* Speed | Amortisiertes vorgefiltertes Sky-Cubemap | **Hoch** |
| **G4** | 1-Mio.-Partikel-Pool, fixe Spawn-Box — keine Niederschlags-Skalierung nach `precip`-Intensität, kein Overdraw-Limit | Potenziell hohe Füllrate bei Sturm | Partikelzahl ∝ Intensität, Nah-Partikel begrenzen | Mittel |
| **G5** | Nässe/Schnee-Response existiert im Lit-Shader, aber **kein globaler Wetness-Uniform-Pfad** + keine Regen-Occlusion-Maske (alles wird nass, auch überdacht) | Glaubwürdigkeit | Globaler `WetLevel` + Occlusion-Tiefenkarte | Mittel |
| **G6** | Noise-Volume R=Value/G=Worley statt **Perlin-Worley 128³ + Erosion 32³** | Wolken-Look | HZD-Noise-Layout | Niedrig (Qualität) |
| **G7** | GL 4.1 (macOS) hat **kein Compute** → Wolken-Temporal/SH-Projektion muss als Fragment-Pass laufen; Metal/D3D/Vulkan können Compute | Backend-Parität | Fragment-Fallback-Pfad | Constraint, kein Gap |

---

## 4. Umsetzungsplan (priorisiert)

Reihenfolge nach **Performance-Hebel pro Aufwand**. Jede Stufe ist eigenständig nützlich und testbar. Backend-Politik aus dem Projektgedächtnis beachten: **Core-Shader von Hand pro Backend; Compute-Features Metal/D3D/Vulkan mit GL-4.1-Fragment-Fallback.**

### Stufe 1 — Sky-View-LUT + SH-Ambient (höchster Hebel, G1) ⭐
**Ziel:** die teure prozedurale `skyColor()`-Auswertung von der Pixel-/Lit-Pixel-Rate entkoppeln.

**Wichtig — der Himmels-Pass darf NICHT komplett aus der LUT gelesen werden.** Kompositing in `sky.frag:351–375` ist:
```
364:  col  = skyColor(dir, sunDir)                         // niederfrequente Atmosphäre → LUT-fähig
368–371: col += starField + nebula + aurora + moonDisk    // hochfrequent → MUSS per-Pixel full-res bleiben
373:  col  = applyClouds(col, …)                           // Wolken (separat → Stufe 2)
```
Eine grobe Sky-View-LUT (z. B. 192×108) würde genau die hochfrequenten Features zerstören, die der Nutzer explizit nennt — Punkt-Sterne, Twinkle, Sonnen-/Mond-Disk-Kanten, Nebula. **Genau wie UE** (Atmosphäre-LUT + separate Stern-Dome) wird der Pass gesplittet:
1. **Sky-View-LUT** (192×108, RGBA16F), 1×/Frame aus **nur** `skyColor(dir,sunDir)` (Zeile 364) befüllt (Fragment-Pass; Compute auf Metal/D3D/Vulkan). Der Himmels-Pass liest die Basis-Atmosphäre aus der LUT.
2. **`starField`/`nebula`/`aurora`/`moonDisk` bleiben per-Pixel full-res** und werden über die LUT-Basis komponiert (Zeilen 368–371 unverändert).
3. **Diffus-Ambient = Order-2-SH:** 9 Koeffizienten/Kanal aus einer 32²-Sky-Cubemap projizieren (1×/Frame oder über wenige Frames amortisiert), als UBO an `scene.frag`. Ersetzt den `skyColor(Nup,…)`-Aufruf durch eine billige SH-Auswertung.
   - **Achtung Wolken-Dimming:** Die heutige Logik fadet bei >0,5 Coverage das Direktlicht aus und verstärkt Ambient. Die SH **muss aus einer Cubemap projiziert werden, die den Coverage-Term enthält** (d. h. `applyClouds` mit einbezieht), sonst verlieren bedeckte Szenen ihre Ambient-Abdunklung.
- **Ehrliche Erwartung:** Der Sky-*Pixel*-Gewinn ist durch den Split kleiner (Sterne/Nebula/Mond kosten weiter voll). Der eigentliche G1-Hebel war ohnehin der **per-Lit-Pixel-Ambient** (SH statt voller `skyColor`-Eval × 2 pro beleuchtetem Pixel) — der bleibt voll erhalten.
- **Aufwand:** mittel. **Risiko:** niedrig (additive Pässe, alter Pfad als Fallback).
- **Akzeptanz:** identischer Look (Byte-Vergleich-Toleranz wie bei der Wolken-Parität), messbar weniger Sky-Eval-ALU pro Lit-Pixel.

### Stufe 2a — Wolken in Low-Res (G2, einfacher Großteil des Gewinns) ⭐
**Ziel:** den dominanten Sky-Pass-Kostenpunkt ~4× senken, ohne Temporal-Risiko.
1. Wolken-Raymarch (`applyClouds`) aus `sky.frag` in **eigenen Quarter-Res-Pass** auslagern (RGBA: Streufarbe + Transmittance).
2. Adaptive Schritte: großer Schritt bis Dichte > 0, dann feine Schritte; Schritt-Cap je nach Distanz.
3. Bilinear upsample + Komposit über die Atmosphäre/LUT-Basis (vor den Sternen? — Wolken liegen vor allem; Reihenfolge wie heute Zeile 373).
- **Warum zuerst:** In der leeren Welt sitzen Wolken am Far-Plane mit nichts davor → Low-Res + bilinear ist nahezu artefaktfrei und braucht **keine History-Buffer, keine Reprojection, kein Compute** → trivial auch auf GL 4.1.
- **Aufwand:** mittel. **Risiko:** niedrig. **Erwartung:** der Großteil des Wolken-Gewinns.

### Stufe 2b — Temporal-Reprojection (G2, optional, **gated auf Messung**)
Nur falls Stufe 2a + Mess-Baseline zeigen, dass die Wolken weiterhin das Budget sprengen.
1. Pro Frame nur ein Teil der Pixel marchen (Bayer-Offset), Rest aus History reprojizieren; History-Blend ~75 %. Reprojection nutzt Kamera-Matrix des Vorframes.
2. **Frame-Fenster retunen:** Das HZD-„1/16 über 16 Frames" ist eine 30–60-FPS-Konsolenzahl — bei 200 FPS wären 16 Frames **80 ms** Rekonstruktions-Latenz (sichtbares Nachziehen bei Wind/Kameraschwenk). Bei 200 FPS eher **4–8 Frames** wählen.
- **Backend:** Metal/D3D/Vulkan Compute; **GL 4.1 Fragment-Fallback** mit Ping-Pong-History-Targets.
- **Aufwand:** hoch. **Risiko:** hoch (Ghosting, History über 5 Backends inkl. GL-ohne-Compute → Halton-Jitter + Neighborhood-Clamp). **Das risikoreichste Item des Plans — bewusst hinter Messung gestellt.**

### Stufe 3 — Vorgefiltertes Spekular-Sky-IBL, amortisiert (G3)
1. Kleine Sky-Cubemap (64², RGBA16F), **eine Face oder ein Mip pro Frame** neu rendern → voller Refresh über ~6–12 Frames.
2. Roughness-Mips vorfiltern (GGX-Importance-Sampling), in `scene.frag` Spekular-Ambient daraus statt aus `skyColor(reflDir)`.
- **Aufwand:** mittel. **Nutzen:** bessere Spekular-Qualität *und* billiger. Synergie mit Stufe 1 (gleiche Capture-Infrastruktur).

### Stufe 4 — Niederschlags-Skalierung + Overdraw-Budget (G4)
1. Aktive Partikelzahl ∝ `curPrecip`-Intensität (statt fix 1 Mio.); getrennte Caps Regen/Schnee.
2. Nah-Partikel-Größe/Anzahl begrenzen (Overdraw ist der Kostentreiber, nicht Sim).
3. Spawn-Box-Halbweite an Wettertyp koppeln.
- **Aufwand:** niedrig. **Risiko:** niedrig. Reine Skalierungs-/Tuning-Arbeit am bestehenden GL/Metal-Pfad.

### Stufe 5 — Glaubwürdige Nässe/Schnee (G5)
1. Globaler **`uWetLevel` (0–1)** als Renderer-Uniform aus `WeatherSystem` (analog UDW-MPC).
2. Lit-Shader: Lagarde-Nässe-Modell (Albedo-Dunkel, Glanz hoch, Metall-Schutz, Porosität aus Roughness ableiten).
3. **Regen-Occlusion-Tiefenkarte** (orthografisch aus Regenrichtung, z. B. 256² über ~20 m) → überdachte Flächen bleiben trocken. Dieselbe Karte später für Pfützen/Splash-Spawns.
4. Schnee: Up-Facing-`dot`-Maske + globaler `uSnowLevel`.
- **Aufwand:** mittel. **Nutzen:** großer Glaubwürdigkeitsgewinn fürs Mittelalter-Setting bei kleinem Frame-Kosten.

### Stufe 6 — Wolken-Noise auf Perlin-Worley umstellen (G6, Qualität)
- 128³ RGBA (Perlin-Worley + 3× Worley) + 32³ RGB Erosion + 512² Weather-Map. Rein optische Verbesserung; nach Stufe 2a einbauen, da der Low-Res-Pfad den Mehraufwand an Tex-Fetches abfedert.

### Später / Kür
- **Statische Sky-Occlusion-Probes (Unity-APV-Stil)** für Innenräume/Höhlen — dynamische SH-Himmelfarbe × gebackener Sichtbarkeits-Skalar. Erst relevant, wenn Catania-Interieurs Thema werden.
- D3D/Vulkan GPU-Partikel-Parität (offene Lücke aus dem Projektgedächtnis).

---

## 5. Backend-Constraints (aus dem Projektgedächtnis)
- **macOS GL = 4.1:** kein Compute, aber Transform-Feedback. Wolken-Temporal & SH-Projektion müssen einen **Fragment-Pass-Fallback** haben (Ping-Pong-Render-Targets). Offline-Validierung mit `glslangValidator` **ohne** `-G` (Desktop-GLSL).
- **Sandbox hat kein Display** → GL nicht lauffähig, keine Screenshots; Verifikation per Offline-Shader-Validierung + Byte-Vergleich gegen Referenz-Backend (wie bei der D3D-Wolken-Parität bewährt).
- **Compute-Policy:** echte Compute-Features Metal-first mit GL-Fallback; Core-Shader von Hand pro Backend pflegen.

---

## 6. Empfohlene erste Schritte
1. **Mess-Baseline ZUERST** (vor jedem Umbau). Der ganze Plan ruht auf der **unbewiesenen** Annahme, dass der aktuelle Himmel die 200 FPS reißt — die „~1–2 ms"-Zahlen in §1 sind Schätzungen, keine Messungen. **Metal läuft auf dem Mac** (nur GL/Sandbox hat kein Display), also ist eine isolierte Sky-Pass-Messung (leere Welt, Sky/Wolken/Ambient getrennt) jetzt machbar. Falls die Wolken schon ~1 ms kosten, ist die schwerste Maschinerie (Stufe 2b) evtl. unnötig.
2. **Stufe 1** — höchster Hebel, niedrigstes Risiko, schafft die Capture-Infrastruktur (Sky-View-LUT + Sky-Cubemap), auf der Stufe 3 aufsetzt.
3. **Stufe 2a** (Low-Res-Wolken) — der einfache Großteil des Wolken-Gewinns. **Stufe 2b nur**, wenn die Messung nach 2a es rechtfertigt.
4. Stufen 4 & 5 sind kleine, unabhängige Verbesserungen, die parallel/zwischendurch laufen können.

---

### Quellenverzeichnis (Auswahl)
- Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique*, EGSR 2020 — sebh.github.io/publications/egsr2020.pdf · Referenz-Impl. github.com/sebh/UnrealEngineSkyAtmosphere
- Schneider, *Real-Time Volumetric Cloudscapes of Horizon Zero Dawn*, SIGGRAPH 2015 · *Nubis*, SIGGRAPH 2017 — advances.realtimerendering.com
- jpgrenier.org/clouds.html · bitsquid.blogspot.com/2016/07/volumetric-clouds.html (konkrete Temporal-Zahlen)
- Lagarde, *Water drop 2a/2b/3b* — seblagarde.wordpress.com (Regen, Ripples, PBR-Nässe)
- ATI, *Artist-Directable Real-Time Rain Rendering in City Environments*, SIGGRAPH 2006
- GPU Gems 2, Kap. 10 — *Real-Time Computation of Dynamic Irradiance Environment Maps* (SH-Projektion)
- UE-Docs: Sky-Atmosphere, Volumetric-Cloud, Sky-Lights (Real-Time Capture, ms-Breakdown), Lumen-Performance-Guide
- Unity-Docs: HDRP Volumetric Clouds, APV Sky-Occlusion, Realtime-GI/Enlighten
- UDS/UDW-Docs: ultradynamicsky.com (V6–V9) — Sky-Modi, Material-Quality, DLWE-Nässe, Half-Rate-Tick
