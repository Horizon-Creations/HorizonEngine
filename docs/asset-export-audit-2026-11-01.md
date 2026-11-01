# Export-Audit: was packt der Export, und wo steckt die Größe? (Thema 228, Schritt 1585)

Wiederholung des fehlgeschlagenen Audits (Schritt 1144). Lokal auf dem MacBook gemacht, ohne Hive
(Thema gehört der Queen auf NN-RP01, Deadline abgelaufen). Nur gelesen und gemessen; Quelltext
unverändert. Der ältere Plan `docs/asset-optimization-plan.md` bleibt gültig (Cook-Stand dort).

## 1. Kernbefund: Texturen sind die Größe, nicht Shader

Gemessen an einem echten Export, `~/HorizonEngineProjects/Catania/Export/` (Pak-TOC per Python gelesen):

| Pak | Gespeichert | Entpackt | Einträge |
|---|---|---|---|
| Development (Codec Store, 27.10.) | 360,8 MB | 360,8 MB | 73 |
| Shipping (zstd + AES, 30.10.) | 128,3 MB | 367,6 MB | 106 |

Im Shipping-Pak machen die drei Auto-Landscape-Texturarrays (UUID `{0x40f,1}`, `{0x410,1}`, `{0x411,1}`,
je 111,85 MB roh) **120,7 MB von 128,3 MB aus (94 %)**: 52,7 + 39,6 + 28,3 MB nach zstd. Das
Projekt selbst (Content) wiegt 15 MB, davon 7 MB Szene.

Warum die Arrays unkomprimiert bleiben: `PackSettings::cookTextureArrays` (`HpakFormat.h:226`) ist
`false`, und nichts außerhalb von `HpakWriter.cpp` setzt es (grep). Der Grund steht im Header-Kommentar:
`giAutoLandscapeSliceMeans` (`GiAutoLandscape.cpp:71`, aufgerufen `EditorApplication.cpp:8064`) liest die
RGBA8-Pixel der Albedo-Slices und kann BC7/ASTC nicht lesen.

Material/Shader sind im Vergleich klein: In der Development-Pak haben die 4 Materialien zusammen 1,33 MB
(PSHD 0,89 MB, MTRL 0,45 MB), also 0,4 % des Paks.

## 2. Was der Export packt (Erreichbarkeit)

- `HpakWriter::addDirectories` (`HpakWriter.cpp:1025`) läuft über jede `.hasset` in den Wurzeln
  (Projekt-`Content`, EngineContent, EngineContent-Cache; `ProjectExporter.cpp:781-790`) und packt
  alles außer `excludePatterns`. **Es gibt keinen Erreichbarkeitsfilter.** Das gilt für Assets
  unabhängig davon, ob eine Szene sie braucht.
- Folge im Catania-Shipping-Pak: 106 Einträge, ein Python-Prototyp (`reach2.py`, Pfad- und
  UUID-Suche ab der Startszene) findet davon nur ca. 16 von 101 Quell-Assets erreichbar. Nicht
  erreichbar sind u. a. 5 Wetter-Audios (1,08 MB), 48 Engine-Meshes, 12 Widgets, 6 Materialien.
  **Bytes**: nur ca. 2,5 MB, weil die großen Texturen erreichbar sind. Für Catania spart der Filter
  also ~2 %; der Hebel ist die Texturkompression.
- Der Prototyp ist nicht belastbar genug zum Einbauen: Er kennt keine Laufzeit-Abhängigkeiten der
  Engine (Wetter-Audio per Konstante `0x500`, Trail-Clipmap `0x41F`, GL-Defaults) und HorizonCode-Klassen,
  die über Komponenten-UUIDs referenziert werden (`Scripts/asd.hasset` wurde als unerreichbar gemeldet).
  `EngineDependencies.h` (Referenzschluss über "Engine/…") und `AssetRefScan.h` (Pfad + UUID, auch
  Prefab-CBOR) sind die Bausteine.
- Testprojekt `foliage-test-project`: alle 60 Assets erreichbar (Prototyp). Der Filter bringt dort nichts.
- Startszene wird doppelt gepackt: Im Development-Pak zwei byte-gleiche Einträge zu je 7,02 MB
  (Startszene-UUID und Pfad-UUID, `addSceneEntries`, `ProjectExporter.cpp:628-653`). Mit zstd
  ist das fast gratis (Szene komprimiert 7,04 MB → 53 KB), ohne Kompression kostet es 7 MB.

## 3. Größe pro Asset-Typ (Quelle, roh)

| Quelle | Texture | StaticMesh | Material | Audio | Rest |
|---|---|---|---|---|---|
| foliage-test-project (Content, 616 MB) | 604 MB (36) | 11,7 MB (12) | 0,06 MB (12) | | |
| Catania Content (15 MB) | 8,4 MB (2) | | 0,33 MB (3) | | Szene 7 MB |
| EngineContent im Repo (4,3 MB) | | 0,56 MB (51) | 0,80 MB (6) | 1,08 MB (5, PCM) | Widgets 1,5 MB (davon `HE_Logo.png` 1,48 MB) |
| EngineContentCache (369 MB .hasset) | 369 MB (5) | | | | 6 `.wav` 447 MB (nicht gepackt) |

Textur-Faustzahlen (zlib6): foliage-test-project 604 MB → 182 MB. Alle Texturen sind roh RGBA8 im Editor,
der Cook macht daraus BC7/ASTC/BC3 mit Mips (Plan 0.). Die Cook-Ersparnis dort ist nicht gemessen (siehe 6).

## 4. Shader und Material

- Pro Material werden im Pak behalten: MTRL mit `nodeGraphJson` und `customShaderFragGlsl`
  (byte-verbatim, `HpakWriter.cpp:~520`) **und** PSHD je gewähltem Backend (`HpakWriter.cpp:594`).
  Beispiel Catania: Material `{0x600,1}` MTRL 326 KB + PSHD 370 KB; `{0x412,1}` MTRL 111 KB + PSHD 315 KB.
- Laufzeit nutzt den Graphen nur ohne PSHD (`ContentManager.cpp:749`) und für Instanzen mit Switches
  (`ContentManager.cpp:928`). Ob bei fertigem PSHD und ohne Switch-Instanzen Graph und GLSL aus dem
  MTRL fallen dürfen, ist **nicht geprüft** (Fundstellen 857, 953, 975, 1865 müssen gelesen werden).
- Backend-Auswahl ist ein manuelles Häkchenfeld (`ExportDialogPanel.cpp:88,1203`), Standard
  Metal|OpenGL, **nicht an die Zielplattform gekoppelt** (die Texturkompression ist es, `:1700`).
  Ein Windows-Export mit dem Standard bekommt Metal/GL-Varianten, aber keine D3D/Vulkan.
- Ein gemeinsamer Dictionary-Pass ist nicht möglich: `kArchiveHasDict` ist reserviert und nie gesetzt
  (`HpakFormat.h`). Messung auf `EngineContent/Materials`: roh 983 KB, je Datei zlib 180 KB, alle
  zusammen lzma 52 KB. Der Gewinn über Datei-Grenzen ist real, der Aufwand liegt im Pak-Format.
- `ShaderAsset.sourceCode`: Nicht neu geprüft. Fonts sind inzwischen **nicht** mehr tot
  (Thema 226 bringt Font-Assets in UI-Text), die Aussage im alten Plan ist überholt.

## 5. Weitere Typen (Befund, nicht gemessen am echten Export)

- Audio: Wetter-Audio liegt im Editor und Pak als PCM16 (`PCMD`). Vorbis/MP3/FLAC kennt der Mixer
  (`AudioEncoding`, `Assets.h:558`), der Export wandelt aber nichts um. Quellen im Cache: 447 MB `.wav`
  (nicht im Pak).
- Static-Mesh-Cook ist da (interleaved + AABB). Offen: uint16-Indizes, Quantisierung, Skeletal-Cook.
- Scenes: 7 MB JSON, davon 5,6 MB `sculptHeightsB64` und 1,4 MB `layerWeightsB64` (Base64 von meist Nullen).
  Gespeichert wird nach zstd klein (53 KB), im Store-Modus aber 7 MB und die Ladezeit der JSON-Auflösung.
- Texturen mit `HE_Logo.png` (1,48 MB): lose Datei im Widgets-Ordner, kein Asset; wird vom Export nicht gepackt.

## 6. Priorisierte Liste

1. **Auto-Landscape-Arrays komprimieren** (Schätzung, nicht gemessen: 335 MB → ~84 MB roh bei 4:1, Shipping ca. 121 MB → ca. 60-70 MB, VRAM -75 %).
   Voraussetzung: Slice-Mittelwerte beim Pack berechnen und als Tail-Chunk ablegen, damit
   `giAutoLandscapeSliceMeans` nicht mehr lesen muss; dann `cookTextureArrays` im Export setzen.
2. **Erreichbarkeitsfilter** (Schritt 1586), mit Engine-Wurzeln (Wetter `0x500`, Clipmap `0x41F`, Defaults)
   und "Immer mitpacken"-Muster. Spart bei Catania nur ~2,5 MB, bei Projekten mit viel Altbestand mehr.
3. Startszene nicht doppelt packen (7 MB im Store-Modus).
4. Shader-Backends an die Zielplattform koppeln (Standard aus Ziel + Renderer).
5. Graph-JSON/GLSL aus MTRL streichen, wenn PSHD vorhanden und kein Laufzeit-Konsument (erst klären, s. 4).
6. Normal/Mask-Arrays mit BC5/BC4 statt BC7 (nur wenn sie Kanäle sparen), Audio als Vorbis, Mesh uint16.

## Handoff

- **Topic / step:** 228, Schritt 1585 (Audit), lokal ohne Hive; danach 1586 (Filter)
- **Branch / commit:** claude/landscape-3-0-multi-group-paint, dieses Dokument (pushed: siehe Commit)
- **Done:** Pak-Aufbau am echten Export gemessen, Quelle der Größe gefunden (3 Arrays = 94 % des Shipping-Paks), Fehlen des Erreichbarkeitsfilters belegt.
- **Files touched:** nur diese Datei.
- **Verified:** Pak-TOC-Parser (Python) auf Catania Development + Shipping; Größen je Typ per HAsset-Header (`sizes.py`); Fundstellen oben gelesen.
- **Not verified:** Wirkung des Cooks auf Größe (kein Build des Packers aktuell, `build/src/HE_Tools/hpak_packer` ist vom 11.10. und ohne Cook-Optionen); Laufzeitverbrauch von nodeGraphJson/GLSL; der Erreichbarkeits-Prototyp ist nur ein Python-Skript.
- **Decisions taken:** Audit lokal ausgeführt, weil Hive-Dispatch gesperrt war.
- **Decisions suggested, not taken:** Reihenfolge der Liste in 6.
- **Pending:** Schritt 1586; Hive-Schritt 1144 (failed) auf skipped setzen, wenn das Thema zumachen soll.
- **Traps:** `grep --include` in zsh bricht ab, `/usr/bin/grep` oder das Grep-Tool nehmen; Slice-Means-Leser (`GiAutoLandscape.cpp:71`) liest RGBA8.
- **Next steps:** 1. Slice-Means beim Pack berechnen (`cookTexture`, HpakWriter.cpp:769) und `giAutoLandscapeSliceMeans` darauf umstellen. 2. `cookTextureArrays` im Export setzen und mit Catania neu messen. 3. Filter in `addDirectories` einbauen.
