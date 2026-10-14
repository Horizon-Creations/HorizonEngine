# Landscape: falsche GI-Schatten an steilen Kanten, Ursache (Thema 204, Schritt 1)

Stand: 14.10.2026, NN-WS03 (RTX 4070), Kopie des Deploys
`HorizonEngineBuild\Editor` vom 14.10. 22:28 (Basis `2500a502`, main) in
`C:\hw204\deploy`. Gemessen auf **D3D12, Vulkan, D3D11, OpenGL** (headless,
`HE_DUMP_*`, je frisches APPDATA). Metal nur am Quelltext. Noch nichts am
Renderer geändert.

Vom Menschen gibt es nur einen Screenshot (der Queen beschrieben, hier nicht
vorhanden): Gebirgsterrain, dunkle, scharf begrenzte Bänder an Kämmen und
steilen Hangkanten, die der Kontur des Hangs folgen; dunkle Ränder an einer
Schneekuppe. Backend und GI-Einstellung sind unbekannt. Die Editor-Config des
Menschen auf NN-WS03 sagt: **Vulkan, RenderPath 1 (deferred), GI an**,
`GIIndirectIntensity` 1.0, `GILightRadius` 0.5.

## 1. Kurzfassung

Es sind **zwei** voneinander unabhängige Fehler, beide nur mit GI an. Keiner
davon ist eine Lightmap-Naht (es gibt keine Lightmaps) und keiner eine
Normalen-Diskontinuität an Chunk-Grenzen (die Normalen sind dort identisch,
§4).

| | Fehler | Backends | Bild | Wo |
|---|---|---|---|---|
| **A** | DDGI: im Terrain **vergrabene Sonden** werden nicht verworfen. Der Chebyshev-Sichttest läuft am **rohen** Oberflächenpunkt, ohne den in DDGI üblichen Surface-Bias (P + N·b + V·b). Wo der Strahl einer vergrabenen Sonde genau am Schattierungspunkt aus dem Gelände austritt, ist mittlere Tiefe ≈ Abstand: der Test besteht und die dunkle Sonde bekommt volles Gewicht. Das ergibt dunkle, harte Linien und Bänder **entlang der Höhenlinie, an der eine Sondenschicht das Terrain schneidet**. Steile Hänge drehen N zur Sonde hin (Backface-Gewicht steigt). | **alle** (Sampler in 7 Kopien, §3.4) | Linie/Stufe auf Kontur, nur im indirekten Licht | `sampleDDGIIrradiance` in `scene.frag`, D3D11/D3D12/Metal/GL-Renderer, `MaterialShaderLibrary.cpp` |
| **B** | Der GI-G-Buffer (liefert den Startpunkt der Sonnen-Schattenstrahlen) hat auf D3D11/D3D12 einen **`D16_UNORM`**-Tiefenpuffer. Bei near 0.1 / far 5000 ist eine Tiefenstufe z²/6554 m: 0,1 m bei 25 m, 1,5 m bei 100 m, 6 m bei 200 m. Die **Skirts** (senkrechte Wände unter jeder Chunk-Kante, Tiefe `max(Zelle/2, Höhenspanne/4)`) gewinnen dann den Tiefentest gegen die Nachbar-Oberfläche. Der gespeicherte Strahlstart liegt **unter** dem Nachbar-Chunk, der Sonnenstrahl trifft Gelände: dunkle Striche **genau auf den Chunk-Nähten**. Steile Chunks haben tiefere Skirts, also breitere Striche. | **nur D3D11, D3D12** (Vulkan/Metal D32, GL D24) | gestrichelte Linie auf Chunk-Naht, im Sonnenschatten-Maskenterm | `D3D11Renderer.cpp:3346`, `D3D12Renderer.cpp:6956` + `:7157`, Skirt `TerrainMeshGenerator.cpp:332-372` |

**Korrektur nach dem Test am echten Terrain (§3.5): Auf dem Terrain des
Menschen ist weder A noch B das, was man sieht.** Mit seiner Konfiguration
(Vulkan, deferred, Auto-Material, echte Texturen, TOD 0.4) liegen die
dunklen, hart begrenzten Flächen an den steilen Flanken **mit GI aus genauso**
an derselben Stelle wie mit GI an; CSM und RT-Maske stimmen überein. Es sind
echte Sonnenschatten bzw. sonnenabgewandte Flanken. GI macht sie heller
(≈ 44 → ≈ 59 Luma bei Albedo ≈ 129, beleuchtet ≈ 190), nicht dunkler. Was
„falsch“ wirkt, ist, wie **dunkel und hart** sie sind (Schatten/Licht ≈
0,23 ohne GI, ≈ 0,3 mit GI), nicht wo sie liegen. A tritt dort nicht auf:
das gemessene Gitter (15×4×15, 171,25 m) hat Schichten bei y ≈ −52 und 119,
keine schneidet das Terrain (−1…68 m). B kann auf Vulkan nicht auftreten
(§3.2).

A und B sind trotzdem echte Fehler (gemessen an den Zeugen) und gehören in
Schritt 2. Die Zuordnung „zwischen Landscape-Parts“ im Titel ist sehr
wahrscheinlich eine Fehldeutung: auf Vulkan liegt nichts davon auf
Chunk-Grenzen. Bevor Schritt 2 „die“ Ursache repariert, braucht es den
Original-Screenshot oder Kamera/TOD/Backend des Menschen (§6).

## 2. Zeugen und Werkzeug

`scripts/landscape-gi-edge-repro/cap204.ps1` (Aufnahme, ein frisches APPDATA
pro Bild, `-Cfg` schreibt `config.json`-Schlüssel, `-RenderPath`, `-Project`)
und `ana204.py` (`cols` = Spaltenmittel, `seams` = dünne dunkle Zeilen).

- **AUTOLAND** (`HE_DUMP_AUTOLAND`, Thema 158): 128 m Terrain bei y=300, 2×2
  Chunks (Nähte x=0, z=0), Smoothstep-Rampe x −24…8 auf 40 m (bis ~62°),
  konstant in Z. Draufsicht CAM (0,400,0), Pitch −89, TOD 0.4: die Rampe liegt
  im Eigenschatten, dort dominiert das indirekte Licht. `-Mode builtin`
  (Standard-Terrainmaterial) und `-Mode 1` (ausgeliefertes Auto-Material).
  Sondengitter laut Log **11×6×11, Abstand 16,46 m**.
- **MOUNTAINTEST** (`HE_DUMP_MOUNTAINTEST=after`): 240 m Terrain, 4×4 Chunks
  à 60 m (Nähte bei x,z = −60, 0, 60), Berg aus `TerrainGenerate::mountain`
  (40 m). Sondengitter laut Log 12×5×12, Abstand 26,8 m.
- A/B-Schalter am selben Binary: `HE_DUMP_GI` 0/1, `GIIndirectIntensity=0`
  (nur Sonnen-RT-Maske, kein DDGI), `HE_GI_REFERENCE=1` (256 Strahlen, kein
  Filter).

## 3. Messungen

### 3.1 A: DDGI-Linie auf der Kontur

AUTOLAND Draufsicht, Spaltenmittel (Zeilen 100..600), D3D12 forward builtin:

| Bild | px 580 | **590** | 600 |
|---|---|---|---|
| GI aus | 75 | 74 | 73 |
| GI an | 93 | **89** | 92 |
| GI an, `GIIndirectIntensity=0` | 69 | 68 | 68 |
| GI an, Referenz (256 Strahlen) | 93 | **89** | 92 |

Die Linie verschwindet ohne indirektes Licht und ist mit der Referenz-Maske
bitgleich, also **DDGI, nicht die Sonnen-Schattenmaske**. Vulkan forward:
gleiche Werte. **Vulkan deferred** (Konfiguration des Menschen): builtin
93/**89**/92, ohne indirekt 68/67/66; mit dem **Auto-Material** statt der
schmalen Linie eine **harte Stufe**: ~150 links von px 596, ~140 rechts davon,
ohne indirekt flach 97–98.

Lage: px 590 = x ≈ −8 m in Höhe der Ebene y=300; die Rampe liegt dort bei
y≈320, also 80 statt 100 m vor der Kamera, korrigiert **x ≈ −6,4 m, Höhe
≈ 323 m**. Das aus dem Log rekonstruierte Gitter (Box x,z ±64, y ≈ 290…340
mit Skirt, Mitte 315; Ursprung y = 315 − 2,5·16,46 = 273,8) hat eine
**Sondenschicht bei y = 323,2**. Die x-Ebenen liegen bei −16,5 und 0, also
**nicht** an der Linie. Die Sonde (0 | 323,2) steckt 10,6 m tief in der Rampe;
ihr waagrechter Strahl Richtung P tritt genau bei x ≈ −6,4 aus dem Gelände.
(Gitter-Ursprung nicht geloggt, nur Zahl und Abstand; Ursprung aus
`FitGIProbeGrid` und der Box nachgerechnet.)

TOD-Reihe (0.3/0.4/0.6/0.7): die Linie gibt es nur, wo der Hang im
Sonnenschatten liegt (indirekt dominiert). Wo die Sonne die Rampe trifft,
stimmen RT-Maske und CSM auf ±3 überein, keine Linie.

### 3.2 B: Striche auf Chunk-Nähten nur auf D3D

MOUNTAINTEST, CAM (0,350,−140), Yaw 180, Pitch −15, nur Sonnenmaske
(`GIIndirectIntensity=0`). Zeilen mit dünnen dunklen Strichen
(`ana204.py seams`): Zeile 402–407 = Ebene bei z=0, Zeile 346/347 = z=60
(vorhergesagt aus Kamera und ~60° FOV: 402 und 344).

| Backend (G-Buffer-Tiefe) | Striche (Pixel) |
|---|---|
| GI aus (CSM) | 0 Zeilen |
| D3D12 (D16) | Zeilen 402–407 |
| D3D11 (D16) | Zeilen 346/347 (~300 px), 402/403 |
| Vulkan (D32) | 0 Zeilen |
| OpenGL (D24) | 0 Zeilen |

Gegenprobe Abstand (z²-Gesetz der D16-Präzision), Naht z=0 abseits des Bergs,
CAM x=−90:

| | aus 25 m | aus 200 m |
|---|---|---|
| D3D12 | 0 | Zeilen 368–371 (Naht z=0) und 416–419 (Naht z=−60) |
| Vulkan | 0 | 0 auf den Nähten |

Nah keine Striche, fern auf genau den vorhergesagten Nahtzeilen (368, 417),
nur mit D16. Eine Präzisionsursache, kein Unterschied der Shader (D3D11 und
D3D12 teilen das HLSL, Vulkan nicht; der Abstandstest trennt das).

Nebenbefund, **alle** RT-Backends: Zeilen 532–535 im Fernbild sind der
**äußere Terrainrand** (z=−120). Der Rand-Skirt ist dort eine sichtbare Wand
mit kopierter Oberflächennormale; mit GI wird sie vom eigenen Gelände
verschattet, ohne GI nicht. Nur von außerhalb des Terrains zu sehen.

### 3.3 Das Projekt des Menschen (Schätzung, gemessen in §3.5)

`C:\Users\conno\Downloads\HEProject`, `StartupScene.hescene`: **ein**
Terrain, 2000 × 2000 m, Auflösung 1025 (16×16 Chunks à 125 m, Zelle 1,95 m),
gemalte Höhen −1,1…67,9 m, Gebirgsblock bei x −500…125, z 125…750, nur 1,4 %
der Fläche steiler als 30°. **Kein** Material am Terrain (Standard).

Geschätzt mit `FitGIProbeGrid` über die Terrain-Box: **171 m Abstand,
15×4×15**, Schichten bei y ≈ −52, 119. Im Live-Lauf (§3.5) **gemessen**:
15×4×15, 171,25 m. Keine Schicht schneidet das Gelände, die Kontur-Variante
von A kann hier also nicht entstehen. Eine Schicht liegt ganz **unter** dem
Gelände; ob diese vergrabenen Sonden die Füllung sichtbar verfälschen, ist
nicht getrennt gemessen (§3.5 zeigt nur: GI an hellt die Schatten auf).

### 3.5 Das echte Terrain, live (Vulkan, deferred)

`scripts/landscape-gi-edge-repro/live204.py`: startet die Deploy-Kopie mit
eigenem APPDATA (Projekt-**Kopie** in `C:\hw204\proj`, `RHI` 1, RenderPath 1,
`HE_MCP=1`, `HE_COLLAB_OFFLINE=1`, Fenster minimiert ohne Fokus), kopiert den
EngineContentCache des Menschen hinein (echte Texturen) und holt drei Bilder
über `scene_screenshot` (`south`, `oblique`, `close` auf den Bergblock). In
der Kopie ist `timeOfDay` 0.0 → 0.4 gesetzt (die Szene steht auf Mitternacht,
`autoAdvance` aus; der Screenshot des Menschen war Tag). Log: 256 Chunks
gebaut, **GI probe grid 15x4x15 (900 probes), spacing 171.251556**.

Albedo-Ansicht (`LIVE204_VIEWMODE=basecolor`): die steilen Flanken sind
hellbraunes Gestein, nicht dunkel. Normalen-Ansicht: glatt, nach außen. Licht:
Fels-Pixel (Albedo ≈ 129), die im GI-aus-Bild im Schatten liegen
(Luma < 60), Mittelwert:

| Ansicht | GI an | GI an, indirekt 0 | GI aus | beleuchteter Fels |
|---|---|---|---|---|
| close | 58,6 | 34,5 | 45,4 | ≈ 190 |
| oblique | 56,2 | 33,5 | 42,9 | ≈ 190 |
| south | 64,6 | 36,5 | 43,6 | ≈ 190 |

Lage und Kanten der dunklen Flächen sind in allen drei Varianten gleich
(Augenschein, `grid_close.png`): Rinne hinter einem Grat, sonnenabgewandte
Flanken. Keine Linien auf Chunk-Nähten (125 m). Bilder liegen nur lokal in
`C:\hw204\live2` (keine Screenshots nach git).

### 3.4 Der Sampler (identisch in allen Kopien)

```
weight  = trilinear;
weight *= max(0.05, dot(N, dirToProbe) * 0.5 + 0.5);      // Backface, Boden 0.05
dist    = length(probePos - P);                            // P = roher Oberflächenpunkt
chebyshev = (dist > mean) ? (var / (var + (dist-mean)^2))^3 : 1;
weight *= max(chebyshev, 0.05);                            // Boden 0.05
```

Kein Surface-Bias, keine Sondenklassifikation (Sonde in Geometrie → aus),
keine Relokation, Sichtkarte 8×8 Oktaeder pro Sonde. Kopien:
`shaders/scene.frag` (Vulkan), `D3D11Renderer.cpp`, `D3D12Renderer.cpp:535`,
`MetalRenderer.mm`, `OpenGLRenderer.cpp` (2×), `MaterialShaderLibrary.cpp`
(Graph-Material-Präambel, „line-for-line“). Deferred zeigt dasselbe Bild
(gemessen, §3.1); welche Kopie der deferred-Pfad je Backend durchläuft, ist
nicht einzeln nachverfolgt.

## 4. Ausgeschlossen bzw. nicht die Ursache

- **Normalen an Chunk-Grenzen**: `sampleFieldNormal` rechnet aus dem globalen
  Höhenfeld mit Quellzellen-Abstand, beide Chunks liefern an der gemeinsamen
  Kante dieselbe Normale. LOD0 gilt bis 6 Chunkbreiten (750 m beim Menschen),
  Nachbarn sind in der Nähe also gleiches LOD mit identischen Kantenvertices.
- **LOD-/Tessellations-Risse**: erst ab 6 Chunkbreiten bzw. an der
  50-m-Tessellationsgrenze (beim Menschen aus, Faktor 1).
- **Sonnen-RT-Maske selbst** (Terminator auf groben Dreiecken): in AUTOLAND und
  MOUNTAINTEST (≈ 1 m Zellen) stimmen Maske (`GIIndirectIntensity=0`) und CSM
  in den sonnenbeschienenen Bereichen auf ±3 Luma überein, Referenzmaske
  bitgleich. Am echten Terrain (1,95 m Zellen, §3.5) liegen die Schatten mit
  RT-Maske und mit CSM an derselben Stelle (Augenschein, keine Pixel-Metrik).
- **BLAS-Staleness**: Cache nach Mesh-UUID, `InvalidateMesh` leert ihn.
- Rand-Normale: `sampleFieldNormal` klemmt u,v auf [0,1], am **äußeren**
  Terrainrand ist die Steigung halbiert. Wirkt nur am Außenrand, nicht innen.

## 5. Fallen

- Der Headless-Dump (`HE_DUMP_PATH`) rendert vor dem ersten `tickWorld`. Für
  ein per `LastProjectPath` geladenes Projekt baut er **keine
  Terrain-Chunks** (Bild ohne Gelände). Die Zeugen rufen
  `TerrainSystem::updateTerrains` selbst. Für das echte Projekt braucht es
  den Live-Weg: `live204.py` (MCP `scene_screenshot`, Kamera per
  `position`/`look_at`, Ansicht per `HE_DUMP_VIEWMODE`, das das Viewport
  auch live jeden Frame übernimmt). `scene_screenshot`-Namen dürfen keinen
  Punkt enthalten.
- Die Szene des Menschen steht auf `timeOfDay` 0.0 (Nacht); ohne Anpassung
  der Kopie misst man Mondlicht.
- Aufnahmen müssen den **RenderPath des Menschen** treffen (deferred). Mit
  dem Auto-Material ist A dort eine harte Stufe statt einer dünnen Linie.
- Pixel→Welt bei Draufsicht: Höhenunterschiede verschieben den Maßstab
  (Rampe 80 m statt 100 m vor der Kamera), sonst landet man auf der falschen
  Sondenebene.
- D16-Effekte verschwinden aus der Nähe: wer B prüft, muss aus ≥ 100 m
  aufnehmen.

## 6. Für Schritt 2

- **Zuerst klären, was der Mensch genau meint** (Queen → Mensch): Original-
  Screenshot oder Backend, GI an/aus, Tageszeit, Kamera. Auf seinem Terrain
  mit Vulkan sind die harten dunklen Flächen echte Schatten, die mit und ohne
  GI gleich liegen (§3.5). Wenn er „zu dunkel/zu hart“ meint, ist das ein
  Füll-/Ambient-Thema (Schatten/Licht ≈ 0,23 ohne GI, 0,3 mit GI; laut
  `cap158auto.ps1` bekommen Graph-Materialien auf D3D11/D3D12/Vulkan kein
  Sky-IBL, `fog.z = 0`), kein falscher Schattenwurf. A/B am echten Terrain
  mit `live204.py` wiederholen, bevor ein Fix als „die“ Lösung gilt.
- **A (alle Backends)**: DDGI-Surface-Bias für den Sichttest einführen
  (P + N·normalBias + V·viewBias, skaliert mit dem Sondenabstand), und
  vergrabene Sonden deaktivieren (Klassifikation: Anteil Rückseiten-Treffer
  der Sondenstrahlen > ~25 % → Gewicht 0) statt sie mit Boden 0.05 mitzählen.
  Alle 7 Sampler-Kopien gleich ändern. Prüfen: AUTOLAND px 590 bzw. Stufe
  px 596 (Vulkan deferred, `-Mode 1`) verschwinden, GI-aus-Bild bleibt
  bitgleich. Grob-Gitter des Menschen (171 m) im Blick behalten.
- **B (D3D11/D3D12)**: GI-G-Buffer-Tiefe auf `D32_FLOAT` (D3D11 `:3346`,
  D3D12 PSO `:6956` + Textur `:7157`). Gegenprobe: MOUNTAINTEST fern
  (CAM −90/340/−200, Pitch −10) Zeilen 368–371/416–419 leer, nah unverändert.
- Außenrand-Skirt (alle RT-Backends) ist ein eigener, kleiner Befund.
