# Was sich am exportierten Spiel noch sparen lässt (Größe, Startzeit, RAM/VRAM)

Stand 2026-11-02, Thema 228. Grundlage: ein echter Export des Foliage-Testprojekts (Metal, Store-Codec, nach Erreichbarkeitsfilter) und vier unabhängige Code-Untersuchungen
(Meshes, Texturen, Startzeit, Pak-Container und übrige Dateien). **Gemessen** ist nur, was ausdrücklich so steht; der Rest sind Schätzungen aus dem Code. Schon erledigt und
hier nicht noch einmal vorgeschlagen: Texturarrays komprimieren, Erreichbarkeitsfilter, Startszene einmal, vorkompilierte Shader (PSHD v4), Mesh-Textur-Bake.

## 1. Gemessen

| Was | Wert |
|---|---|
| Pak (Foliage, 136 Einträge, ungepackt) | 216,1 MB: Texturen **93,1 %** (36 Stück), Mesh-Vertices 3,8 %, Mesh-Indizes 1,9 %, Audio 0,5 %, Shader (PSHD) 0,5 %, Rest < 0,3 % |
| Alle 36 Texturen | 2048 × 2048, ASTC 4×4 (8 Bit/Texel), 12 Mips, je 5,59 MB; 24 lineare Datenkarten, 12 sRGB-Albedos |
| Laufzeitpaket neben dem Pak | 50 MB: `libphonon` (Steam Audio) 22, `libHorizonEngine` 21, `libcrypto` 4,3, SDL 2,1, zstd 0,6, lz4 0,15 |
| Start Metal, warm (2 Läufe) | OnInit 55 bis 58 ms, **erstes Bild nach rund 350 ms** (davon 120 ms Frame) |
| Start Metal, kalt (1. Lauf) | OnInit 1594 ms, davon **1,46 s im Öffnen des Audiogeräts** (`ma_engine_init`); nicht reproduziert |
| Start OpenGL (Hidden) | Fenster+SDL 351 ms, Renderer 152 ms, OnInit 98 ms, erstes Bild 941 ms, davon rund 270 ms Programm-Kompilierung |
| Speicher beim Lauf (Metal) | größter RSS 731 MB, Spitze des Footprints 1,36 GB, bei 201 MB Texturen im Pak |

## 2. Rangliste

**A. Größe und Speicher (Disk, RAM, VRAM)**

| # | Maßnahme | Ersparnis | Aufwand | Risiko |
|---|---|---|---|---|
| 1 | **Größenkappe für Texturen / oberste Mip weglassen** beim Export (`PackSettings::maxTextureSize`; `halveRGBA8` vor der Mip-Kette in `cookTexture`, HpakWriter.cpp ~764/911), je Rolle (Datenkarten härter als Albedo) | 75 % je gekappter Textur, bei 36 × 2k bis zu rund 150 MB auf Disk, im RAM und im VRAM zugleich (geschätzt) | S | weichere Nahansicht |
| 2 | **Größere ASTC-Blöcke** (6×6 = 3,56 Bit, 8×8 = 2 Bit): neue Enum-Werte, Blockmathematik, Pixelformat in 5 Backends (Assets.h ~652, HpakWriter.cpp:55) | 6×6 rund 56 % (112 MB), 8×8 rund 75 % | M bis L | Qualität bei Normalen und Albedo |
| 3 | **CPU-Kopie der Pixel nach dem Upload freigeben** (`TextureAsset::data` wird nie geleert, `uploadMetalTexture` kopiert nur; verifiziert per grep). Nötig nur für Terrain-Gewichte, Trail-Clipmap, Widgets, Thumbnails | bis rund 200 MB RAM bei diesem Projekt (geschätzt, passt zum gemessenen RSS) | M | Zeiger, Re-Upload nach Geräteverlust |
| 4 | **`libphonon` nicht mitliefern, wenn Steam Audio aus ist** (kein Filter im Exporter, verifiziert; `audioBuses.spatialBackend` ist schon da, `dlopen` nur bei Bedarf) | 22 MB im Paket (gemessen) | S | Laufzeit-Wechsel des Backends per Skript (ungeprüft) |
| 5 | Mesh: **Vertex-Cache/Overdraw-Reorder** (meshoptimizer, im Repo nicht vorhanden) in `cookForPack` | GPU-Zeit, bessere Kompression; Größe geschätzt | S bis M | niedrig |
| 6 | **Skelett-Meshes werden gar nicht gekocht** (64 B/Vertex roh); Bone-Ids uint8, Gewichte unorm8 | 75 % der Bone-Daten | M | nur Disk-Format |
| 7 | Mesh-Vertex auf Disk quantisieren (Oktaeder-Normale, Half-UV), beim Laden aufweiten | 8,2 auf rund 5,1 MB vor zstd; **kein** RAM/VRAM-Gewinn | M | niedrig |
| 8 | Mesh-Indizes auf Disk als uint16 (breit aufweiten beim Laden) | rund 2 MB (vor zstd) | S | niedrig; die volle Variante (GPU) ist L und riskant |
| 9 | CPU-Kopie der Meshes nach dem Upload freigeben (Physik, Nav, Occlusion, GI-BVH, Picking lesen sie) | rund 12 MB je 63 Meshes | M | mittel, braucht Flag "braucht CPU" |
| 10 | Pro-Rolle-Formate: BC1 für deckendes Albedo, BC4/BC5 für Datenkarten (Encoder sind da, der Dialog wählt sie nie) | rund 33 MB bei den 12 Albedos, **nur D3D/Vulkan** (auf Apple kein Gewinn) | M | gering mit Deckkraft-Test |
| 11 | Texturen nach Inhalt **deduplizieren** (`addAlias` nur für Szenen) und einfarbige auf 4×4 | unbekannt, bis einige 5,59 MB | S | keines |
| 12 | Audio: pro Clip Mono-Downmix (`AudioImporter::convert` existiert, der Import nutzt es nicht); Vorbis-Encoder wäre L | 0,3 bis 0,8 MB | S / L | niedrig |
| 13 | PSHD per zstd-Wörterbuch (Format reserviert Felder), tote Teile in MTRL | 0,2 bis 0,4 MB / 0,1 bis 0,25 MB | M bis L | Formatänderung |

**B. Startzeit**

| # | Maßnahme | Ersparnis | Aufwand |
|---|---|---|---|
| 1 | **GL: die richtigen Varianten vorwärmen.** `SetRenderPath` läuft (GameApplication.cpp:3583) nach `WarmupMaterials` (3228); deshalb werden die Deferred- und Instanz-Programme erst im ersten Frame gebaut | rund 55 ms (geschätzt) | S |
| 2 | **GL: die 270 ms Programmbau aus dem ersten Frame** in die Splash-Phase oder einen Worker-Kontext | 150 bis 250 ms am ersten Bild | M |
| 3 | **Audiogerät auf einem Worker öffnen**, vor `playOnStart` zusammenführen (`AudioEngine::init`, eager in GameApplication.cpp:1427) | 85 ms (GL, gemessen); der kalte Metal-Lauf zeigte einmal 1,46 s, nicht reproduziert | S |
| 4 | **Sky-Noise 256³ (33 MB) asynchron oder lazy** (`BuildSkyNoise3D`); Metal nutzt auch im Debug 256 statt 64 | 50 bis 120 ms (geschätzt), im Debug Minuten | S bis M |
| 5 | SDL- und Fenster-Init (rund 245 ms) mit Pak-Mount und Pool überlappen | bis 245 ms | M bis L |
| 6 | Metal-Shader-Quelltext schon im Export zu einer `.metallib` kompilieren (heute `newLibraryWithSource` beim Laden) | unbekannt | L |

Nichts Großes mehr bei: Pak-Hash und Schlüsselableitung (billig), Laden (schon lazy und gestreamt, 4 ms/Frame Registrierung), Python (nur bei Python-Projekten dabei).

**C. Container**

- zstd Stufe 19 wird immer behalten, auch wenn das Ergebnis nicht kleiner ist; ASTC schrumpft nur um 3 bis 12 %. Ein Schutz "unter 3 % gespart, dann roh speichern" spart Entpackzeit und eine Heap-Kopie je Eintrag beim Laden (und viel Exportzeit), nicht Größe. **S**.
- Lesen: `seekg` + `read` in einen frischen Vektor, kein mmap; Entpacken braucht einen zweiten Puffer. mmap spart je Lesen eine Kopie. M.

## 3. Kostet es Bildqualität? (Korrektur nach Rückfrage des Nutzers)

Die Rangliste oben mischte Maßnahmen, die nichts kosten, mit solchen, die Qualität gegen Platz tauschen. Getrennt:

**Ohne Qualitätsverlust** (bit- oder bildgleich): A3 CPU-Pixel freigeben (nur RAM), A4 `libphonon`, A5 meshopt-Reorder, A8 uint16-Indizes auf Disk, A9 CPU-Kopie der Meshes,
A11 Duplikate zusammenlegen, A12 Mono-Downmix nur für Clips, die ohnehin räumlich als Mono gespielt werden, alle Punkte unter B (Startzeit) und C (Container).

**Praktisch ohne Verlust, aber mit Rundung:** A6 Bone-Ids als uint8 (verlustfrei) und Gewichte als unorm8 (Rundung unter 1/255), A7 Vertex-Quantisierung (Oktaeder-Normale 16 Bit ist
nicht sichtbar; **Half-Float-UV nicht** bei gekachelten UVs, dort unorm16 nur für Werte in 0..1).

**Tauschen Qualität gegen Platz:** A1 Texturgröße kappen, A2 größere ASTC-Blöcke, A10 BC1 statt BC7/ASTC 4×4 (4 statt 8 Bit je Texel). Das sind keine Standardeinstellungen, sondern Wahlmöglichkeiten.
Wenn überhaupt, dann so: Voreinstellung bleibt die heutige Qualität; ein Export-Voreinsatz "Kompakt" (oder je Textur ein Häkchen) schaltet sie ein; und die Spieler bekommen zur Laufzeit eine
Grafikoption "Texturqualität", die beim Laden die oberste Mip überspringt (spart RAM und VRAM, **nicht** Platz auf der Platte, der Export bleibt voll).

## 4. Reihenfolge, die ich vorschlage (Stand nach der Umsetzung siehe 4a)

1. **CPU-Pixel freigeben (A3):** erledigt für Metal und OpenGL, offen für D3D11/D3D12/Vulkan.
2. **`libphonon` weglassen (A4)**, **GL-Vorwärmen (B1)**, **Audiogerät und Sky-Noise (B3, B4)**: erledigt.
3. Offen, noch nicht gemessen: Duplikate (A11), Zstd-Schutz gegen unkomprimierbare Einträge (C), Skelett-Meshes (A6, braucht ein Beispielprojekt).
4. Verworfen nach Messung: meshopt-Reorder, uint16-Indizes, Vertex-Quantisierung und Remap (A5, A7, A8, siehe 4a).
5. Nur auf ausdrücklichen Wunsch: die Qualitätsstufen aus Abschnitt 3 (A1, A2, A10).

Ehrlich dazu: Auf der Platte ist bei diesem Projekt ohne Qualitätsverlust kaum noch etwas zu holen. 93 % sind 2k-ASTC-Texturen, und ASTC lässt sich mit zstd nur um 3 bis 12 % verkleinern.
Die verlustfreien Gewinne liegen im RAM (A3), im Laufzeitpaket (A4) und in der Startzeit.

## 4a. Umgesetzt und gemessen (2026-11-02, nach "verlustfrei zuerst")

| Punkt | Commit | Messung |
|---|---|---|
| A3 CPU-Pixelkopie nach dem Upload freigeben (`ContentManager::releaseCpuPixels` / `restoreCpuPixels`, Metal an allen Upload-Stellen, das Spiel schaltet es ein, `HE_KEEP_CPU_PIXELS=1` schaltet es ab) | 99fb6682 | Foliage-Export, Metal, 300 Frames: größter RSS **843 → 694 MB (−149 MB)**, Spitze des Footprints **1171 → 1013 MB (−159 MB)**; Bild unverändert (die Läufe mit Behalten sind untereinander nicht bitgleich, wegen asynchroner Pipelines; die drei Läufe mit Freigabe sind es, und ihr Bild ist byte-gleich zu einem der Behalten-Bilder). Frei nur: block-komprimiert, aus einem Pak, kein Array ohne gebackene Mittelwerte. **OpenGL (BC3-Export, macOS GL 4.1, 4 Läufe)**: RSS **1116 → 946 MB (−170 MB)**, Footprint **1476 → 1306 MB (−170 MB)**, alle vier Bilder byte-gleich (md5), 0 Fehler; die Texturen sind im Bild zu sehen. Gleicher Helfer an allen 6 Upload-Stellen in `OpenGLRenderer.cpp` (`uploadGlTextureReleasing`). Nicht umgesetzt: D3D11/12, Vulkan (behalten die Kopie wie bisher) |
| A4 `libphonon` nur mitliefern, wenn die Audio-Busse Steam Audio wählen | d91bc526 | Test im Exporter (Teilfälle aus/an); spart 22 MB im Paket. Das Backend wird nur über `AudioBusConfig::spatialBackend` gewählt (kein Skript-API), und genau das packt der Export |
| B1 GL: Render-Pfad schon am Ende von `OnInit` setzen, damit das Vorwärmen die G-Buffer-Programme mitbaut | (dieser Commit) | Vorwärmen baut jetzt 4 statt 2 Programme; erstes Bild OpenGL **rund 450 → 433 ms** (3 Läufe je Seite, ohne den kalten ersten Lauf), also nahe am Rauschen. Die Schätzung von 55 ms war zu hoch: das Vorwärmen läuft weiter auf dem Hauptthread vor dem ersten Bild, die Kompilierzeit wandert nur dorthin. Ein echter Gewinn bräuchte B2 (Programmbau parallel oder in der Splash-Phase) |

### Start (nach "mach beides", gleiche Messung: je 4 Läufe, Foliage, hidden window)

| Punkt | Commit | Messung |
|---|---|---|
| B3 Audiogerät in einem Hintergrund-Thread öffnen, schon im `GameApplication`-Konstruktor (`m_audioInit`, `OnInit` sammelt das Ergebnis) | 6b773917 | `OnInit` **57 → 4 ms**; erstes Bild Metal **374 → 327 ms**, OpenGL **508 → 464 ms**; Audio meldet sich weiter, 0 Fehler |
| B4 Sky-Noise (256³, **61 ms** auf dem Job-Pool, gemessen, vorher nirgends geloggt) parallel zum SDL-Start backen (`PrewarmSkyNoise3D` im Konstruktor, `BuildSkyNoise3D` übernimmt das fertige Volumen, dieselben Bytes: `test_culling` pinnt sie, 78/78) | 3e1e63f8 | Renderer-Phase Metal **110 → 43 ms**, OpenGL **135 → 71 ms**; erstes Bild Metal rund 327 → **299 ms**, OpenGL rund 464 → **450 ms** (der Bake läuft jetzt im Hintergrund 65 bis 78 ms, weil er sich mit dem SDL-Start die Kerne teilt). Metal bäckt im Debug jetzt 64³ wie die anderen Backends |
| B2 GL-Programmbau aus dem ersten Frame | nicht gebaut | auf macOS-GL gibt es einen Kontext; ein Worker-Kontext für parallelen Build ist dort nicht sicher belegt, und der Gewinn läge nur bei den rund 120 ms der G-Buffer-Programme |

### Gemessen und verworfen: Mesh-Formate (Punkte A5, A7, A8 der Liste oben)

Die 63 Foliage-Meshes sind roh 12,2 MB, **nach zstd Stufe 19 (dem Export-Codec, je Eintrag) nur 6,3 MB**, also rund 3 % des Paks. Ich habe die Varianten an den echten Daten durchgerechnet
(Wegwerf-Skript `meshstat.py`, komprimiert mit der zstd-CLI):

| Variante | MVBO + INDX komprimiert |
|---|---|
| heute | 6,306 MB |
| uint16-Indizes (Vertexzahl ≤ 65535) | 6,369 MB (**schlechter**) |
| Vertices nach erster Verwendung ordnen (Dreiecksreihenfolge bleibt) | 6,265 MB (−0,04 MB, 0,6 %) |
| beides | 6,352 MB |

Die rohen Gewinne verschwinden in der Kompression. Deshalb sind uint16-Indizes auf der Platte, Vertex-Quantisierung (derselbe Mechanismus: zstd findet die Redundanz selbst) und der Vertex-Remap **nicht umgesetzt**.
Der Dreiecks-Reorder für den Vertex-Cache (meshoptimizer) ist außerdem nicht bildgleich garantiert: Bei tiefengleichen, sich überlappenden Dreiecken (Z-Fighting) und bei Transparenz hängt das Bild von der Zeichenreihenfolge ab,
und auf einer Apple-GPU (Tile-Renderer) ist der Gewinn ungemessen. Die GPU-Seite (uint16 im VRAM, 2 MB je 63 Meshes) wäre gegen 700 MB RAM vernachlässigbar.
**Skelett-Meshes** (Bone-Daten packen, A6) bleiben offen: Auf dieser Maschine gibt es keine einzige Skelett-Mesh-Datei, ich kann den komprimierten Gewinn nicht messen, und nach dem Ergebnis oben ist er unsicher.
Das Ziel für den Aufwand wäre ein Projekt mit Figuren; dort lässt sich dieselbe Messung in einer Viertelstunde wiederholen.

## 5. Nicht geprüft

Nichts von A und B wurde umgesetzt oder an einem zweiten Projekt gemessen. Das Foliage-Projekt ist texturlastig (alle Texturen 2k) und hat 63 Ein-Slot-Meshes; ein Spiel mit vielen Meshes, Audio oder Skeletten gewichtet anders. Die Startwerte stammen von einem Rechner (Apple M5), zwei Läufen (Metal) und einem (GL); es gibt keinen Timer pro Phase im Renderer-Init. Die Speicher-Schätzungen von A3 und A9 sind aus dem Code gelesen, nicht gemessen; der gemessene RSS (731 MB) ist nur damit verträglich.
Ob der Linux- oder Windows-Start dieselben Posten hat, ist offen.

## Handoff

- **Topic / step:** 228, neue Untersuchung (kein Code).
- **Done:** diese Auswertung; Wegwerf-Skript `pakstat.py` (Scratchpad, nicht im Repo) schlüsselt ein Store-Pak nach Typ und Chunk auf.
- **Verified:** Pak- und Bündelgrößen, Startzeiten, `libphonon` ohne Filter, kein Freigeben von `TextureAsset::data`, zstd Stufe 19.
- **Not verified:** siehe Abschnitt 5.
- **Decisions taken:** Qualitätsverlust ist keine Standardeinstellung (Nutzer: die Texturkappe kostet Spielqualität); die Reihenfolge in Abschnitt 4 ist ein Vorschlag.
- **Next steps:** Auswahl durch den Nutzer; danach je Punkt ein eigener Schritt im Thema.
