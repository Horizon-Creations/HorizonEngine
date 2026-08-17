# Anti-Aliasing — Methodenübersicht und Bewertung für HorizonEngine

> Stand: 2026-08-17 · Untersuchung, noch kein Umsetzungsbeschluss. Zwei Fragen in einem Dokument:
> **(a)** Was hat die Engine heute? **(b)** Welche Verfahren gibt es aktuell (Stand 2026) und
> welche davon sind in unseren fünf Backends realistisch?

---

## 0. Warum überhaupt

Aliasing entsteht an drei verschiedenen Stellen, und die Verfahren unten lösen nicht dieselbe
Sache. Das vorab zu trennen erspart die typische Fehldiskussion („TAA ist an, warum flimmert das
Metalldach noch?"):

| Quelle | Was flimmert | Was hilft |
|---|---|---|
| **Geometrie-Kanten** | Treppchen an Silhouetten | MSAA, FXAA/SMAA, TAA, ML-Upscaler |
| **Shading/Specular** | kriechende Glanzlichter, Normalmap-Sparkles | Roughness-Regularisierung (Toksvig/LEAN), Mip-Bias, TAA (nur dämpfend) |
| **Sub-Pixel-Detail & Transparenz** | Zäune, Laub, Alpha-Test-Kanten, Haare | Alpha-to-Coverage, TAA/Upscaler, SSAA |

MSAA rührt Punkt 2 und 3 gar nicht an — es supersampled nur die Rasterisierung, nicht den
Fragment-Shader. Genau deshalb ist die Branche bei temporalen Verfahren gelandet: sie sind die
einzigen, die alle drei Achsen gleichzeitig bedienen.

---

## 1. Ausgangslage im Code (verifiziert)

| Baustein | Ort | Zustand |
|---|---|---|
| **FXAA (Lottes, klassische 3×3-Kantenmischung)** | GL `OpenGLRenderer.cpp:3100-3160, 9674`, Metal `MetalRenderer.mm:839-935, 10095`, D3D11 `D3D11Renderer.cpp:650-…, 4033`, D3D12 `D3D12Renderer.cpp:696-…, 1417`, Vulkan `VulkanRenderer.h:368,379` | in **allen fünf** Backends vorhanden, läuft nach dem Tonemap auf dem LDR-Zwischenbild |
| **An/Aus-Schalter** | — | **keiner.** D3D11/D3D12 haben ein `fxaaEnabled = true`, das nirgends geschrieben wird; GL/Metal haben nicht einmal das. Kein Settings-Eintrag, keine Editor-UI, keine `IRenderer`-Methode (vgl. `SetBloomSettings`, das es gibt) |
| **MSAA** | — | nirgends. Kein `sampleCount > 1` auf Swapchain, Viewport-Target oder G-Buffer (die Metal-Treffer für `sampleCount` gehören zum Profiler-Counter-Buffer) |
| **Motion-Vector-Target** | `docs/deferred-renderer-plan.md:118-129` | bewusst **nicht** in G-Buffer v1 (GB0/GB1/GB2 + Depth, 24 B/px) |
| **Temporale Infrastruktur** | GI/SSR: `m_giPrevViewProj`, `m_ssrPrevViewProj`, `m_giShadowHistory[2]`, `m_giReflPrevViewProj`, Frame-Seed-Jitter (Metal/GL/Vulkan) | vorhanden, aber **nur Kamera-Reprojektion** — bewegte Objekte werden nicht reprojiziert („mild ghosting under motion accepted", `MetalRenderer.h:293-295`) |
| **Projektions-Jitter der Hauptkamera** | — | nicht vorhanden (der vorhandene Jitter ist Ray-/Sample-Jitter in GI/SSR, keine Sub-Pixel-Verschiebung der Kamera) |
| **Resolution Scale / Upscaling** | — | nicht vorhanden; gerendert wird immer in Viewport-Auflösung |
| **UI-Reihenfolge** | GL `OpenGLRenderer.h:698`, Metal `MetalRenderer.mm:13413` | In-Game-UI wird **nach** FXAA gezeichnet → bleibt scharf. Das ist die richtige Reihenfolge und muss bei jedem Nachfolger erhalten bleiben |

**Kurzfassung:** Wir stehen auf der Stufe „FXAA, fest verdrahtet, ohne Regler" — dem Stand von
ca. 2011. Die Voraussetzungen für alles Temporale (History-Puffer, `prevViewProj`, Reprojektion)
existieren bereits im GI-/SSR-Code, aber der eine fehlende Baustein, **per-Objekt-Velocity**,
fehlt in allen Backends.

---

## 2. Die Methodenfamilien (Stand 2026)

### 2.1 Supersampling — SSAA / DSR / VSR / „Resolution Scale > 100 %"

Szene in n-facher Auflösung rendern, dann herunterfiltern. Löst alle drei Aliasing-Quellen aus
Abschnitt 0, korrekt und ohne Artefakte. Kosten skalieren linear mit der Pixelzahl (2×2 = ×4).
Bleibt relevant als **Referenzbild** („so soll es aussehen") und als Screenshot-/Cinematic-Modus,
nicht als Default. Für uns günstig zu bauen, weil es nur ein größeres Render-Target plus
Downsample-Pass ist — und der Regler ist derselbe, den man später für Upscaling < 100 % braucht.

### 2.2 Hardware-MSAA (2×/4×/8×) und Alpha-to-Coverage

Der Rasterizer testet Deckung an n Subsample-Positionen, der Fragment-Shader läuft aber nur
einmal pro Pixel. Perfekte Geometriekanten bei moderaten Kosten — *im Forward-Pfad*.

Warum das bei uns praktisch tot ist: MSAA verlangt, dass **jeder** Buffer multisampled ist. Bei
Deferred hieße das ein 24-B/px-G-Buffer × 4 Samples und ein Resolve, der pro Sample shaden muss
(oder eine Kanten-Maske mit Per-Sample-Shading — der klassische „MSAA-Deferred"-Trick, teuer und
in fünf Backends fünfmal zu bauen). Zusätzlich kollidiert es direkt mit unserem
Apple-Silicon-Tile-Pfad (memoryless G-Buffer + Framebuffer-Fetch, `deferred-renderer-plan.md`
P6) und mit jedem Screenspace-Effekt, der Depth/Normalen wieder liest (SSAO, SSR, GI). Die
Branche hat aus genau diesen Gründen abgeschaltet.

Realistischer Rest-Nutzen für uns: **Editor-Viewport-Gizmos/Grid** und ein reiner Forward-Pfad
für einfache Projekte. Alpha-to-Coverage (billiges, ordnungsfreies Alpha-Test-AA für Laub) hängt
ebenfalls an MSAA und fiele damit weg.

### 2.3 Morphologische Post-Filter — FXAA, SMAA, CMAA2

Ein Fullscreen-Pass sucht Kanten im fertigen LDR-Bild und mischt entlang der Kante.

* **FXAA** (haben wir): billigst (~0,2–0,4 ms @1080p), backend-neutral, aber weichzeichnend und
  ohne jede Sub-Pixel-Information → Zäune, Drähte und dünne Geometrie flimmern weiterhin.
* **SMAA 1x**: Kantenerkennung + Musterklassifikation gegen eine vorberechnete Area-Lookup-Tex.
  Deutlich schärfer als FXAA bei fast identischen Kosten (~0,3–0,8 ms), drei Passes statt einem,
  zwei kleine Hilfstexturen (Area/Search). **Läuft überall, auch auf GL 4.1 ohne Compute** — der
  offensichtlichste Sofort-Upgrade-Pfad für uns.
* **SMAA T2x / S2x**: SMAA plus temporale bzw. MSAA-Komponente; T2x braucht schon
  Jitter + History, also dieselben Voraussetzungen wie TAA.
* **CMAA2** (Intel): Qualität zwischen FXAA und SMAA bei sehr geringen Kosten, aber
  **Compute-basiert** → auf macOS-GL (4.1, kein Compute) nicht lauffähig.

Gemeinsame Grenze der ganzen Familie: kein Sub-Pixel-Sampling → keine echte Rekonstruktion, nur
Kantenglättung. Deshalb ist keins davon heute noch „das" AA eines AAA-Titels.

### 2.4 Temporale Verfahren — TAA, TAA-Upsampling/TSR, DLAA

Die Kamera-Projektion wird pro Frame sub-pixelweise verschoben (Halton 2,3 / 8–16 Positionen),
das Ergebnis wird mit dem reprojizierten Vorframe akkumuliert. Über die Zeit entsteht echtes
Supersampling — die einzige nicht-ML-Familie, die alle drei Aliasing-Quellen bedient, zu
2–5 % Frame-Kosten.

Voraussetzungen (das ist der eigentliche Bauaufwand):
1. **Jitter** in der Projektionsmatrix, konsistent für alle Passes,
2. **Velocity-Buffer** (per-Objekt, inkl. Skinning und Landscape — sonst geistern bewegte Dinge),
3. **History-Target** + Reprojektion,
4. **Neighbourhood-Clamping** (YCoCg-AABB, Varianz-Clipping) gegen Ghosting,
5. Nachgeschärfter Output, weil TAA prinzipbedingt weichzeichnet.

Bekannte Nachteile: Ghosting/Smearing bei schneller Bewegung, Disocclusion-Löcher, Unschärfe.
Der öffentliche „TAA sieht matschig aus"-Ärger der letzten Jahre trifft vor allem Titel, die
zusätzlich Effekte in Viertelauflösung durch die TAA-History rekonstruieren.

* **TAAU / TSR** (Unreal): dieselbe Pipeline, aber die History läuft in Zielauflösung und der
  aktuelle Frame kommt niedriger aufgelöst herein → AA und Upscaling in einem Pass, ohne
  Hersteller-SDK. TSR ist Epics eigene, deutlich robustere Variante davon.
* **DLAA / „Native AA"**: ML-Upscaler bei Skalierungsfaktor 1,0 — beste Kantenqualität ohne
  Auflösungsverlust, ~10–15 % Kosten. Setzt exakt dieselben Eingaben voraus wie TAA.

**Für uns entscheidend:** Punkte 1–4 sind zu ~60 % schon da (History-Ping-Pong, `prevViewProj`,
Reprojektions-Shader in GI/SSR). Was fehlt, ist der Velocity-Buffer — und **denselben** braucht
auch SSR/GI, um bewegte Objekte korrekt zu reprojizieren (`ssr-plan.md`, „parallax is NOT
reprojected"). Einmal bauen, zwei Systeme freischalten.

### 2.5 ML-/Hersteller-Upscaler — DLSS 4, FSR 4, XeSS 2, MetalFX

Alle vier sind im Kern „TAA mit gelerntem Rekonstruktionsfilter" und verlangen dieselben Eingaben
(Farbe, Tiefe, Motion Vectors, Jitter-Offset, Exposure).

| Tech | Modell / HW | Reichweite | Kern |
|---|---|---|---|
| **DLSS 4** (NVIDIA) | Transformer-Modell (statt CNN in DLSS 3) | RTX only | aktuell Qualitätsführer, insbesondere Temporalstabilität an Haaren/Zäunen/Laub |
| **FSR 4** (AMD) | ML, RDNA-4 | RX 9000 aufwärts (FSR 3.1 bleibt der breite Fallback) | großer Sprung ggü. FSR 3.1, über DLSS-CNN-Niveau |
| **XeSS 2** (Intel) | XMX (Arc) + DP4a-Fallback für alle | breiteste Modus-Palette inkl. „Native AA" | ~50 Titel Anfang 2026 |
| **MetalFX** (Apple) | `MTLFXTemporalScaler`, Metal 4 / macOS 26 | Apple Silicon | Temporal-AA + Upscaling als OS-API, kein eigenes Modell nötig |

In Summe schlagen DLSS 4 / FSR 4 / XeSS 2 in vielen Titeln natives TAA — vor allem bei
Bildstabilität und Feindetail.

Bewertung für HorizonEngine: **MetalFX ist der mit Abstand günstigste Einstieg**, weil es eine
System-API ist (kein SDK-Vendoring, keine Signatur-/Lizenzfragen) und unser Metal-Pfad ohnehin
der führende ist. Voraussetzung ist wieder derselbe Velocity-Buffer. DLSS/FSR/XeSS sind je ein
eigenes SDK plus DLL/Redistributable pro Plattform und lohnen erst, wenn der TAA-Unterbau steht —
denn dann sind sie fast nur noch ein Austausch des Resolve-Passes.

### 2.6 Nicht-AA-Verfahren, die trotzdem gegen Flimmern helfen

Billig, unabhängig vom AA-Modus, und in unserer Engine teils noch offen:

* **Specular-/Normalmap-AA** (Toksvig, LEAN/CLEAN, „normal variance → roughness"): dämpft
  kriechende Glanzlichter an der Quelle. Für einen PBR-Deferred-Renderer der beste
  Qualität-pro-Aufwand-Hebel neben SMAA.
* **Mip-Bias und Anisotropie**: bei aktiviertem Upscaling muss der Mip-Bias um
  `log2(renderScale)` verschoben werden, sonst wird das Bild trotz Rekonstruktion matschig.
* **Alpha-Hashing / Dithered Transparency** statt harter Alpha-Tests bei Laub.
* **Geometrie-LOD-Hysterese**: LOD-Popping liest sich wie Aliasing, ist aber keins.

---

## 3. Vergleich auf einen Blick

| Verfahren | Kosten @1080p | Kantenqualität | Sub-Pixel/Shading | Voraussetzungen | Machbarkeit in unseren 5 Backends |
|---|---|---|---|---|---|
| Kein AA | 0 | — | — | — | — |
| **FXAA** (Ist-Zustand) | ~0,2–0,4 ms | mittel, weich | nein | keine | ✅ überall, gebaut |
| **SMAA 1x** | ~0,3–0,8 ms | gut, scharf | nein | 2 Lookup-Texturen | ✅ überall, auch GL 4.1 |
| CMAA2 | ~0,3 ms | gut | nein | Compute | ⚠️ nicht auf macOS-GL |
| MSAA 4× (Forward) | 20–40 % | sehr gut | nein | Forward-Pfad | ⚠️ nur Forward/Editor-Gizmos |
| MSAA 4× (Deferred) | prohibitiv | sehr gut | nein | MSAA-G-Buffer + Per-Sample-Resolve | ❌ kollidiert mit Tile-/Memoryless-Pfad |
| SSAA 2×2 | ×4 | exzellent | ja | größeres Target | ✅ trivial, nur als Referenz-/Foto-Modus |
| **TAA** | 2–5 % | sehr gut | ja | Jitter + Velocity + History + Clamping | ✅ baubar, Velocity fehlt |
| TAAU / TSR | ~TAA, spart durch Upscaling | sehr gut | ja | wie TAA + Mip-Bias | ✅ Ausbaustufe von TAA |
| DLAA / Native-AA-Modi | 10–15 % | exzellent | ja | wie TAA + Vendor-SDK | ⚠️ HW-gebunden |
| MetalFX Temporal | ~TAA | sehr gut | ja | wie TAA, Apple Silicon | ✅ Metal, System-API |
| DLSS 4 / FSR 4 / XeSS 2 | spart Frames | exzellent | ja | wie TAA + SDK pro Vendor | ⚠️ je ein SDK + Redistributable |

---

## 4. Engine-spezifische Randbedingungen

1. **Fünf Backends** (Metal, GL, D3D11, D3D12, Vulkan) — jede Entscheidung ist fünfmal zu bauen
   oder muss einen definierten Fallback haben. Bewährtes Muster: Metal zuerst, Rest zieht nach
   (wie GI/SSR/CSM).
2. **macOS-GL ist 4.1, kein Compute** → alles Compute-basierte braucht dort einen
   Fragment-Shader-Fallback oder entfällt.
3. **Deferred + memoryless Tile-Pfad** → MSAA ist faktisch ausgeschlossen; ein zusätzliches
   Velocity-Target macht den G-Buffer 4–8 B/px breiter und zwingt den Tile-Pfad an dieser Stelle
   zu `stored` (dieselbe Kröte, die SSR bereits geschluckt hat, `MetalRenderer.h:226`).
4. **Editor-Viewport rendert in eine Textur**, die ImGui sampelt — TAA-History muss an
   Viewport-Resize sauber invalidieren, sonst Ghosting beim Ziehen von Panels.
5. **In-Game-UI nach dem AA-Pass** — gilt für jeden Nachfolger, sonst wird die UI mitgeglättet.
6. **`he_shot.py`-Headless-Dumps** sind unser Verifikationsweg; temporale Verfahren brauchen dort
   n Aufwärm-Frames, sonst dumpt man Frame 0 ohne History.

---

## 5. Empfehlung — Stufenplan

| Stufe | Inhalt | Aufwand | Nutzen |
|---|---|---|---|
| **A0** | AA-Modus als echte Einstellung: `IRenderer::SetAntiAliasingSettings`, Editor-UI + Projekt-Settings, Modi `Off / FXAA`; die bestehenden `fxaaEnabled`-Attrappen verdrahten | klein | schließt die peinlichste Lücke (kein Regler), schafft den Steckplatz für alles Weitere |
| **A1** | **SMAA 1x** in allen fünf Backends | mittel | sichtbar schärfer als FXAA bei gleichen Kosten, keine neuen Pipeline-Abhängigkeiten |
| **A2** | **Velocity-Buffer + Kamera-Jitter** (G-Buffer-Erweiterung, Skinning + Landscape + Instancing korrekt) | groß | Voraussetzung für A3–A5 **und** Qualitätssprung für SSR/GI-Reprojektion |
| **A3** | **TAA** (Reprojektion, YCoCg-Neighbourhood-Clamp, Disocclusion, Sharpen) — Metal zuerst | groß | Industriestandard-Qualität, deckt Specular- und Sub-Pixel-Aliasing mit ab |
| **A4** | **Resolution Scale** (< 100 % Upscaling, > 100 % SSAA) + Mip-Bias, TAA als Upsampler (TAAU) | mittel, nach A3 | Performance-Regler „gratis" obendrauf |
| **A5** | **MetalFX Temporal** als Modus auf Apple Silicon; DLSS/FSR/XeSS optional später | mittel | Vendor-Qualität ohne eigenes Modell; nach A2/A3 fast nur ein Resolve-Austausch |
| **A6** | Specular-/Normalmap-AA (Roughness-Regularisierung) im Material-System | klein–mittel | orthogonal, hilft in **jedem** AA-Modus |

Sinnvoller Schnitt für eine erste Auslieferung: **A0 + A1 + A6** (klein, sofort sichtbar, kein
Umbau der Pipeline). **A2 + A3** ist das eigentliche Projekt und sollte zusammen mit der offenen
SSR-/GI-Reprojektionsschwäche geplant werden, damit der Velocity-Buffer nur einmal entsteht.

> **Entschieden (17.08.2026): A2/A3 laufen deferred-only, Metal zuerst, GL danach.** Velocity
> wird ein G-Buffer-Attachment; im Forward-Pfad gibt es kein TAA. Das ist exakt das Muster von
> SSR und GI und erspart es, Velocity per MRT durch *jede* Forward-Pipeline zu fädeln (built-in,
> skinned, instanced, Graph-Material, Transparenz — mal fünf Backends, drei davon blind).
> **Konsequenz, die man kennen muss:** der Standard-Render-Pfad ist Forward, dort bleibt es
> also bei SMAA. `supportsTemporalAA` bleibt dort false → der Combo-Eintrag ist ausgegraut und
> `ResolveAAMethod` fällt auf SMAA zurück, mit genau dem Hinweistext, den A0 schon zeigt.
>
> Zwei Architektur-Festlegungen dazu, damit sie nicht später teuer werden:
> * **Jitter lebt NUR in der Rasterisierungs-Projektion.** Velocity, TAA-Reprojektion und die
>   vorhandene GI/SSR-`prevViewProj`-Reprojektion rechnen mit den *ungejitterten* Matrizen —
>   sonst wandert das Sub-Pixel-Zittern in die GI/SSR-Akkumulation und die Velocity trägt den
>   Jitter-Delta mit. Beide Matrizen werden ab Tag eins getrennt gehalten. Der Sky-Prepass
>   (`m_prepassViewProj`) und die Shadow-Matrizen bleiben ungejittert.
> * **TAA akkumuliert auf dem getonemappten LDR-Bild**, im AA-Resolve-Slot, den A0/A1 schon
>   gebaut haben — nicht auf HDR vor dem Tonemap. Das fügt sich in die bestehende Pass-Struktur
>   ein statt sie umzubauen, und dämpft nebenbei Fireflies.
> * Headless: temporale Modi brauchen Aufwärm-Frames — `HE_DUMP_FRAMES` (Default 3) existiert
>   bereits und geht bis 240.

---

## 5b. Umsetzungsstand

| Stufe | Stand | Anmerkung |
|---|---|---|
| **A0** | ✅ auf dem Branch | `HE::AAMethod` + `IRenderer::AntiAliasingSettings` + `ResolveAAMethod`, Editor-Preferences-Zeile, Projekt-Config, Push aus Editor **und** gepacktem Spiel, `HE_DUMP_AA` / `HE_DUMP_RENDERSCALE` / `HE_DUMP_SPECAA`. „Off" tauscht den Shader gegen einen Passthrough — der Pass läuft weiter, weil er das Ausgabetarget füllt. Metal headless verifiziert |
| **A2+A3** | ✅ Metal (deferred) auf dem Branch | Halton(2,3)-Jitter nur in der Rasterisierungsmatrix, **eigener Velocity-Pass** statt fünftem G-Buffer-Attachment (siehe unten), TAA auf dem getonemappten Bild mit Neighbourhood-Clamp + Sharpen im vorhandenen Resolve-Slot. `supportsTemporalAA` = „Render Path ist Deferred". GL/D3D/Vulkan: offen |
| **A6** | ✅ auf dem Branch | Roughness-Verbreiterung aus der Normal-Varianz im Pixel (Kaplanyan/Filament), in Forward **und** G-Buffer, nie im Resolve. Schalter + Stärke in den Preferences, `HE_DUMP_SPECAA` überschreibt beides headless. Wirkungsnachweis auf gekrümmter Geometrie offen (siehe unten) |
| **A1** | ✅ auf dem Branch | SMAA in allen fünf Backends, aber **einpassig**: Kantenerkennung, Span-Suche, analytische Coverage und Blend stecken in dem Pass, der vorher FXAA war. Kein Edges-/Weights-Target, also keine neuen Render-Targets, Descriptor-Slots oder Render-Passes in fünf Backends. Verzicht: keine AreaTex → keine Diagonalen, keine Corner-Rounding; Suchreichweite 32 Texel (8 Einzel-, dann Doppelschritte). Qualität = MLAA-Niveau, sichtbar über FXAA. Nächster Ausbau wäre die AreaTex zur Laufzeit zu erzeugen — nicht Binaries zu vendorn |

Zwei Messungen, die den A1-Shader geformt haben (Referenzimplementierung in Python gegen
synthetische Treppen, damit die Mathematik nicht über 2-Minuten-Renders debuggt werden muss):

* **Reichweite entscheidet, ob flache Kanten überhaupt geglättet werden.** Mit Suchlimit 12
  bekommt eine Kante mit 33-Texel-Spans in 23 von 60 Spalten einen Zwischenwert, mit 32
  Texeln in 57 von 60. Ein Span, dessen Enden beide außer Reichweite liegen, hat kein
  Muster und damit Gewicht 0 — er bleibt eine Treppe.
* **Ein Sample in der Pixelmitte verliert genau die Ecktexel.** Bei einem Ein-Texel-Span liegt
  die Mitte exakt auf dem Nulldurchgang der revektorisierten Linie. Zwei-Punkt-Quadratur über
  die Pixelbreite: 46 → 56 von 60 Spalten.

**A6 (Specular-AA)** ist ebenfalls drin: `heSpecAARoughness` in der geteilten Preamble (greift
für alle Graph-Materialien auf jedem Backend), plus je eine Kopie im GL-Scene-Shader, im
GL-G-Buffer-Shader und im Metal-Scene-MSL (das den Built-in-Forward **und** den G-Buffer trägt).
Zwei Dinge daran sind nicht verhandelbar und im Code kommentiert:

* Die Verbreiterung gehört in **Geometrie-Pässe** (Forward-Shading + G-Buffer), nie in den
  Deferred-Resolve. Dort ist die „Normale" ein G-Buffer-Texel, dessen Ableitung an jeder
  Silhouette springt — das würde Halos zeichnen statt zu glätten. Deshalb trägt
  `heLight.specAA.y` ein Flag „dieser Pass besitzt seine eigene Normale"; Resolve und
  SSR-Composite löschen es direkt nach dem gemeinsamen Fill.
* Der Zwischenwert darf nicht `kernel` heißen — das ist in MSL ein reservierter
  Funktions-Qualifier, und der Metal-Shader ist mit genau diesem Namen nicht mehr
  kompiliert (`expected unqualified-id`). Steht jetzt in allen Kopien als `kernelRough` da.

**Warum A2 ein eigener Pass wurde statt eines fünften G-Buffer-Attachments:** ein weiteres
Attachment hätte **vier** Pipeline-Deskriptoren (Built-in-G-Buffer, zwei Graph-Material-Varianten,
Decals) und die Node-Graph-Codegen dazu bringen müssen, Velocity mitzuschreiben — und ein
Material, das es vergisst, liefert undefinierte Bewegung und geistert. Der separate Pass ist
material-agnostisch: er liest nur Positionen, tiefen-testet gegen die Tiefe, die der G-Buffer
schon geschrieben hat (LessEqual, kein Write), und kostet einen zusätzlichen Geometrie-Durchlauf
— nur wenn TAA an ist.

Gemessen (Metal, deferred, Würfel-auf-Boden): Spalten der Silhouette mit echtem Zwischenwert
**135 ohne AA → 584 mit TAA** (FXAA: 680, glättet aber per Filter statt per Sub-Pixel-Sample).
Zwischen 16 und 48 Aufwärm-Frames ändern sich noch 3723 Pixel — die History-Gewichtung 0.9
behält bewusst 10 % des letzten (gejitterten) Frames, das Bild „atmet" also minimal, statt
einzufrieren.

**Reichweite von A6, exakt:** Graph-Materialien bekommen es über die Preamble auf **jedem**
Backend, sobald dieses `heLight.specAA` füllt — heute füllen es GL und Metal. Die
**Built-in**-Shader von D3D11/D3D12/Vulkan haben es nicht (eigene Cbuffer, kein Deferred-Pfad;
dieselbe Parität-Lücke wie bei SSAO/GI, siehe `docs/backend-parity-plan.md`). Der
**GL-Skinned-Pfad** ebenfalls nicht: er schiebt schon heute weder Wetness noch Snow in seinen
Shader, das ist eine ältere Lücke dieses Pfades und kein A6-Thema — Metal-Skinned ist dagegen
abgedeckt, weil es die SceneUniforms des Aufrufers weiterreicht.

**Verifikationslücke A6:** headless nachweisbar ist bisher nur, dass es dort **nichts** tut, wo
es nichts tun darf — auf den flachen Flächen der Testszene ist das Bild bei Stärke 400
byte-identisch zu „aus". Eine Headless-Szene mit gekrümmter oder normal-gemappter Geometrie im
Bild gibt es nicht (`HE_DUMP_MATERIALTEST` rendert schwarz), also ist der positive Effekt nicht
gemessen. Abgesichert ist stattdessen die Formel selbst (Unit-Test: „aus" ist bit-exakt,
flache Fläche unveränderlich, drehende Normale verbreitert monoton, verschärft nie) plus ein
Drift-Test über alle vier Shader-Kopien.

Am Würfel-auf-Boden-Testbild (`HE_DUMP_SSRTEST`, Metal) glättet SMAA die Silhouette
vergleichbar zu FXAA (572 vs. 602 von 1280 Spalten mit Zwischenwert), fasst dabei aber
deutlich weniger Bild an (1674 vs. 2286 geänderte Pixel) — genau der Unterschied, um den es
geht: Kante glätten statt Bild weichzeichnen.

---

## 6. Fallen, absehbar

* **Velocity ist mehr als `prevViewProj`**: Skinning, Instancing, Landscape-Chunks und
  vertex-animierte Materialien (Wind) brauchen jeweils ihre Vorframe-Transformation. Wer nur die
  Kamera reprojiziert, baut TAA mit Geisterspuren an genau den Objekten, die sich bewegen.
* **Jitter muss überall gleich sein**: Shadow-Pass, GI, SSR und Sky-Prepass lesen dieselbe
  Projektion. Ein Pass ohne Jitter (oder mit dem Jitter des Vorframes) erzeugt Flimmern statt es
  zu entfernen. Unser Sky-Cloud-Prepass hat bereits eine eigene `m_prepassViewProj` — Kandidat
  Nummer eins für diesen Fehler.
* **History-Invalidierung**: Resize, Kamerasprung/Teleport, Szenenwechsel, `timeScale`-Sprünge,
  Editor↔Play-Wechsel.
* **Transparenz und TAA**: unjitterte oder nicht-velocity-schreibende Transparenz smeart.
* **Debug-Views** (G-Buffer-Dumps, Cascade-Tint, Wireframe) müssen den AA-Pass umgehen können.
* **Determinismus der Tests**: temporale Verfahren machen Pixel-Vergleiche instabil — Headless-
  Dumps brauchen feste Frame-Anzahl und festen Jitter-Startindex.

---

## 7. Quellen

* PCGamingWiki — [Glossary: Anti-aliasing (AA)](https://www.pcgamingwiki.com/wiki/Glossary:Anti-aliasing_(AA)),
  [Glossary: High-fidelity upscaling](https://www.pcgamingwiki.com/wiki/Glossary:High-fidelity_upscaling)
* Apple Developer — [Applying temporal antialiasing and upscaling using MetalFX](https://developer.apple.com/documentation/MetalFX/applying-temporal-antialiasing-and-upscaling-using-metalfx),
  WWDC22 [Boost performance with MetalFX Upscaling](https://developer.apple.com/videos/play/wwdc2022/10103/)
* [Anti-Aliasing in Unreal Engine 5: TAA vs TSR](https://medium.com/@GroundZer0/understanding-anti-aliasing-in-unreal-engine-5-4d993140177f)
  (Kosten/Verhalten im Deferred-Renderer)
* Vergleichsübersichten DLSS 4 / FSR 4 / XeSS 2 (2026): Transformer-Modell in DLSS 4,
  FSR 4 auf RX 9000, XeSS 2 „Native AA", jeweils über nativem TAA in vielen Titeln —
  [PC Game Check](https://pcgamecheck.com/blog/dlss-4-vs-fsr-4-vs-xess-2026),
  [Switchblade Gaming](https://www.switchbladegaming.com/game-settings/dlss-vs-fsr-vs-xess-2026/),
  [Notebookcheck: FSR 4 vs FSR 3 vs DLSS](https://www.notebookcheck.net/FSR-4-vs-FSR-3-vs-DLSS-FSR-4-shows-remarkable-improvement-over-FSR-3-and-approaches-latest-DLSS-version-in-quality.973137.0.html)
* Engine-intern: `docs/deferred-renderer-plan.md` (G-Buffer v1), `docs/ssr-plan.md`
  (Reprojektion/Jitter), `docs/backend-parity-plan.md`
