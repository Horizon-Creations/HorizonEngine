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

## 3. Reihenfolge, die ich vorschlage

1. **Texturgröße kappen (A1)** und **CPU-Pixel freigeben (A3)**: zusammen der Großteil von Platte, RAM und VRAM; der Export bleibt einstellbar.
2. **`libphonon` weglassen (A4)** und **GL-Vorwärmen (B1)**: je S, risikoarm, sofort messbar.
3. **Audiogerät und Sky-Noise nach hinten (B3, B4)**, dann **GL-Programmbau aus dem ersten Frame (B2)**.
4. ASTC 6×6 (A2) als Qualitätsstufe; meshopt (A5); Skelett-Meshes kochen (A6).

## 4. Nicht geprüft

Nichts von A und B wurde umgesetzt oder an einem zweiten Projekt gemessen. Das Foliage-Projekt ist texturlastig (alle Texturen 2k) und hat 63 Ein-Slot-Meshes; ein Spiel mit vielen Meshes, Audio oder Skeletten gewichtet anders. Die Startwerte stammen von einem Rechner (Apple M5), zwei Läufen (Metal) und einem (GL); es gibt keinen Timer pro Phase im Renderer-Init. Die Speicher-Schätzungen von A3 und A9 sind aus dem Code gelesen, nicht gemessen; der gemessene RSS (731 MB) ist nur damit verträglich.
Ob der Linux- oder Windows-Start dieselben Posten hat, ist offen.

## Handoff

- **Topic / step:** 228, neue Untersuchung (kein Code).
- **Done:** diese Auswertung; Wegwerf-Skript `pakstat.py` (Scratchpad, nicht im Repo) schlüsselt ein Store-Pak nach Typ und Chunk auf.
- **Verified:** Pak- und Bündelgrößen, Startzeiten, `libphonon` ohne Filter, kein Freigeben von `TextureAsset::data`, zstd Stufe 19.
- **Not verified:** siehe Abschnitt 4.
- **Decisions taken:** keine; die Reihenfolge in Abschnitt 3 ist ein Vorschlag.
- **Next steps:** Auswahl durch den Nutzer; danach je Punkt ein eigener Schritt im Thema.
