# Landscape im Weltmaßstab: Arbeitsauftrag für Windows (Stand 02.11.2026)

**Repo / Branch:** HorizonEngine, `claude/landscape-weltmassstab-100km` (PR 214 gegen `release/0.8.0`). **Hive:** Thema 326, Schritte 1520 und 1525 offen. Lies zuerst `landscape-world-scale-gpu-2026-11-01.md` (Vertrag, Entscheidungen, §5a Material, §5b Foliage/Wasser, §5c Messungen).
**Was auf dem Mac fertig und geprüft ist (OpenGL, Metal):** Landschaft zeichnen (CDLOD, Atlas, Morph), Boden in Welt-UVs aus dem Albedo-, Normal- und Mask-Array einer Texturgruppe (Ebenen 0-3), Foliage und Wasser als versteckte Entities (`LandscapeWorldProxies`, laufen auf jedem Backend), Physik-Boden und Navmesh in Spiel und Editor (Play-Modus), Component-Felder gespeichert, Messmodus. **Texture-Arrays** für Forest, Grassland, Desert (Sommer, 2048) liegen auf dem Content-Server.

## 1. Renderer-Backends (Schritt 1520)

Für **D3D11, D3D12 und Vulkan** (es gibt eine zweite Session `d3d-landscape` im Hive für D3D; erst mit ihr abstimmen, nichts doppelt bauen):
1. Vorlage: `OpenGLLandscape.{h,cpp}` (kürzeste), `MetalLandscape.{h,mm}`. Eingang: `LandscapeRenderFrame` über `IRenderer::SetLandscapeProvider`; Haken im Szenen-Pass wie `OpenGLRenderer::DrawLandscape`.
2. **Shader-Vertrag, seit 02.11. erweitert:** der Fragment-Shader liest `uTex` (x = 1/Meter, y = Albedo-Array da, zw = fract(origin.xz * x)), `uTexMaps` (x = Normal-Array da, y = Mask-Array da), `uAlbedoArray`, `uNormalArray`, `uMaskArray` (Texture-2D-Arrays, wiederholend, linear). Pfade: `LandscapeRenderStatic::albedoArrayPath / normalArrayPath / maskArrayPath`, `textureMeters`. Der Renderer löst sie wie ein Material-Array auf; das **weiße Array heißt "nicht da" = flach**. D3D und Vulkan nehmen das GLSL über SPIRV-Cross wie der Himmel (`kLandscapeVulkanPrelude`: Block statt loser Uniforms, `LsGlobals` im Metal-Quelltext ist die Vorlage).
3. Clip-Konvention: Vulkan `kVulkanClipFix` (y gedreht), D3D 0..1-Tiefe; der Provider-Aufruf bekommt `zeroToOne = true`. Metal-Fund: der Metal-Szenenpass nutzt die GL-Matrix ohne Korrektur; prüfe, was der D3D-/Vulkan-Szenenpass tut.
4. `scripts/validate_embedded_shaders.py` um die HLSL-/SPIR-V-Pfade ergänzen; `tests/test_landscape_shader.cpp` zählt die Uniforms (FS: 20).
5. `landscape_gpu_check`: Backend-Dateien für D3D11/D3D12/Vulkan neben `GlBackend.cpp` und `MetalBackend.mm` (Szenen Rampe, Hügel, Welt-UV, `--measure`). Abnahme wie dort: keine Löcher, Tiefe und Farbe im Toleranzbereich, Welt-UV-Szene (Boden aus dem Array, gleiches Bild bei anderem Floating Origin, Normal/AO-Vorzeichen), Vergleich gegen OpenGL.
6. Schritt 1520 erst schließen, wenn alle drei fertig sind.

## 2. Messungen auf NN-WS03 (Plan §4, 4070, 1440p, Release)

* `landscape_store_bench --full` (Schritt 0), `landscape_gpu_check --measure` für jedes Backend. Mac-Zahlen zum Vergleich in §5c des GPU-Dokuments (Metal CPU 0,09 ms, GL CPU 1,1 ms, GPU-Pass 4,9 ms auf dem Integrierten, nicht vergleichbar).
* Abnahme: Terrain-CPU < 0,5 ms, GPU < 1,5 ms, keine Risse, kein Popping über 1 px. **OpenGL-CPU ist auf dem Mac über dem Ziel** (ein Draw und ein Uniform-Satz je Patch); falls das auf Windows auch so ist, ist ein Uniform-Puffer je Frame oder Multi-Draw die Aufgabe.
* Zusätzlich: GPU-Speicher im Betrieb, Textur-Pfad in der Zeit, Foliage und Wasser auf der GPU (Zeit je Bild), TAA in Bewegung.

## 3. Foliage und Wasser auf einer GPU ansehen

Sie sind als Entities und Daten geprüft, **nie als Bild gesehen**. Mit einer Szene (Pack mit See, Pflanzenebenen) in Editor und Spiel auf D3D11/D3D12/Vulkan ansehen: stehen die Pflanzen auf dem Boden, liegt das Wasser am Ufer, folgt beides dem Floating Origin (`LandscapeWorldProxies`).
Offen davon: tessellierte Wasserfläche (die Region trägt `table.tessellation`, gezeichnet wird das einfache Wasser), Wellen für Auftrieb und die Unterwasserkamera (lesen ein `water::Field` eines `TerrainComponent`; die Landschaft hat keins, `LandscapeRuntime::waterSurfaceAt` antwortet), Wasser außerhalb der Region (Fernmeer), Dichtemasken je Kachel.

## 4. Material, was fehlt

* **Path und Water mit festen Namen** (Auftrag des Menschen): `Path` und `WaterPebbles` sind feste Rollen jeder Gruppe; **beim Malen von Wasser liegt das Wasser-Material darunter**. Dafür fehlt ein Platz: das Array hat fünf Slices (Grass, Dirt, Rock, Snow, WetGround; WetGround liest das Material nicht). Entscheidung nötig: Array auf sechs/sieben Slices (alle Arrays der Gruppen und Mountains neu bauen und neu hochladen) oder WetGround als Wasser-Kies und Path in einen Kanal 4-11 mit eigenem Array. Die gewählten Texturen stehen in `~/Downloads` (Poly Haven, `*_4k.blend.zip`: `ganges_river_pebbles`, `dry_river_pebbles`, `grass_path_3`, `sandy_gravel_02`, `rocky_trail`, `forest_ground_04`) und sind noch **nicht** gebaut.
* **Jahreszeiten** (eigenes Thema, Hive 1554-1560): Season-Wert, `MF_Season`, Textur je Jahreszeit je Layer und Gruppe. Die Auswahl des Menschen für Herbst, Winter, Frühling liegt vor (Forest: `forest_leaves_04`, `forest_ground_04`, `forest_leaves_02` für ForestFloor; Grassland Meadow Herbst `forest_leaves_02`, Winter `brown_mud_03`, Frühling `forrest_ground_01`; Mountains Grass/Dirt Herbst/Winter/Frühling) und ist nicht gebaut.
* **Schnee und Sand prozedural** (Auswahl: Schnee `color=#f2f6fb grain=0.3 drift=0.5 sparkle=0.5 depth=0.6 wetmelt=0.2`, Sand `color=#d6b57a grain=0.4 ripple=0.5 wind=30 sparkle=0.2 wet=0.5`): im Landscape-Boden nicht umgesetzt; Ebene 3 liest den Slice 3, nicht die Funktion.
* **Kanäle 4-11** bleiben flach; die Zuordnung Kanal zu Rolle ist nicht entschieden. **Rauheit und Höhe** der Maske ungenutzt (kein Glanz), Höhenblend, Anti-Kachel-Muster, anisotrope Filterung fehlen.
* **Mountains:** drei Fels-Rollen (Bergwand, Hang, Hügel) gewählt, die Engine kennt eine Rock-Rolle; Texturen von Mountains Sommer bleiben die bestehenden.
* Texturgruppe wirkt erst beim Beginn der Landschaft (Wechsel zur Laufzeit nicht); Inspector und Editor-Import (Schritt 6) setzen sie.

## 5. Anbindung, was noch fehlt

* Physik-Boden hängt im Spiel und im Play-Modus des Editors, aber **die Regions-Haltung** (`PhysicsWorld::setRegionHold` mit `LandscapeRuntime::groundHolds`, damit ein Körper über ungebautem Boden nicht durchfällt) ist **nicht gesetzt**, auch nicht zusammen mit der der Zellen; und `updateLandscape` läuft in `GameApplication` **nach** dem Physik-Schritt (die Doku verlangt davor). Anker (`setPhysicsAnchors`: Spieler, Fahrzeuge) werden nicht gesetzt, nur die Kamera.
* Navigation: Der Play-Modus und das Spiel setzen die Konfiguration; die Agenten kommen aus `NavAgentComponent`; die Szene muss eine `NavMeshComponent` haben, in die die Laufzeit ihr Netz einhängt. Nicht ausprobiert.

## 6. Schritte 4 bis 7 (Hive 1522-1525)

GI, Schatten und Reflexionen auf der Höhen-Clipmap (4); ganze Regionen entladen, Entity-Zellen, HLOD-Backen (5); Editor-Werkzeuge: Import, Sculpt/Paint auf Tiles, Migration, Streaming-Ansicht, Masken je Kachel, Seenwerkzeug (6); Gesamtverifikation auf allen fünf Backends und Abschlussbericht (7).

## 7. Fallen

* `ninja` zweimal im selben Build-Ordner scheitert; jeder Push bricht die laufende CI ab (erst alles committen, dann einmal pushen).
* "Publish Engine Content" im Editor aus einem Checkout **ohne** die Landschaftstexturen schreibt ein Manifest ohne sie: stattdessen "Rebuild Manifest from Server". Die neun Gruppen-Arrays stehen im Manifest (96 Einträge, Stand 02.11.).
* Windows kann keine Datei löschen, die ein Leser offen hält (Tests setzen `pack.reset()` vor `remove`).
* GL braucht 32 kombinierte Texturen-Einheiten für die Landschaft (sonst aus, mit Warnung).
* Das Prüfwerkzeug `--arrays <EngineContent-Wurzel>` zeichnet mit den echten Arrays; die Wurzel braucht `Textures/Landscape/T_Landscape_<Gruppe>_{Albedo,Normal,Mask}_Array.hasset`.
