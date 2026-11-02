# Export-Audit: was packt der Export, und wo steckt die Größe? (Thema 228, Schritt 1585)

> **Stand 2026-11-01 (später):** Punkt 1 der Liste (Texturarrays komprimieren) ist umgesetzt, siehe
> Abschnitt 7. Der Erreichbarkeitsfilter (Punkt 2) ist ebenfalls umgesetzt, siehe Abschnitt 8.

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
3. Startszene nicht doppelt packen (7 MB im Store-Modus). ✅ umgesetzt, Abschnitt 9.
4. Shader-Backends an die Zielplattform koppeln (Standard aus Ziel + Renderer). ✅ umgesetzt, Abschnitt 10.
5. Graph-JSON/GLSL aus MTRL streichen, wenn PSHD vorhanden und kein Laufzeit-Konsument (erst klären, s. 4). Geklärt, **nicht umgesetzt**: Abschnitt 11.
6. Normal/Mask-Arrays mit BC5/BC4 statt BC7 (nur wenn sie Kanäle sparen), Audio als Vorbis, Mesh uint16.

## 7. Umgesetzt: Texturarrays werden gecookt (Punkt 1)

Der Export komprimiert jetzt auch Texture Arrays (`ExportSettings::cookTextureArrays`, Standard an,
Schalter "Compress texture arrays" im Export-Dialog und im Profil, MCP-Argument `compressTextureArrays`).
Das Hindernis war `giAutoLandscapeSliceMeans`: Es liest die Pixel, kann BC7/ASTC aber nicht dekodieren.
Jetzt backt der Cook die Mittelwerte jeder Slice aus dem RGBA8 der Quelle in einen neuen optionalen
Chunk `TXSM` (`HAsset.h`, `TextureAsset::sliceMeans`); die GI nimmt sie, wenn vorhanden, sonst liest sie
wie bisher die Pixel (RGBA8, BC1/3/4/5). Gemeinsame Rechnung: `HE::textureArraySliceMean` /
`bakeTextureArraySliceMeans` in `TextureBlockDecode.cpp`, gebackener und gelesener Wert sind identisch.

**Gemessen** (`test_bench_landscape_arrays`, echte Arrays aus dem EngineContentCache, V=1, 5 Slices, Qualität 0):

| Array | RGBA8 | ASTC 4x4 / BC7 | Encode ASTC / BC7 |
|---|---|---|---|
| Albedo | 111,85 MB | 27,96 MB | 4,1 s / 4,1 s |
| Normal | 111,85 MB | 27,96 MB | 17,3 s / 4,5 s |
| Mask | 111,85 MB | 27,96 MB | 1,8 s / 4,3 s |

Drei Arrays: 335,5 MB → 83,9 MB (Development, Store: -251,6 MB). Shipping (zstd): die BC7-Arrays
komprimieren mit `zstd -19` auf 20,9 + 15,9 + 17,8 = 54,6 MB, gegen 120,7 MB heute: etwa -66 MB, das
Catania-Pak liegt bei 61,7 MB statt 128,2 MB (gemessen, siehe unten).
VRAM der Arrays -75 %. Bildabweichung (frühere Messung, Thema 180): 0,6 (ASTC) / 0,8 (BC7) mittlere Kanalstufen.

**Gemessen am Projekt (Catania, 2026-11-01):** `hpak_packer` mit den Export-Einstellungen von Shipping
(zstd, Cook, ASTC wie auf Metal, Qualität Fast; Wurzeln Projekt-Content + EngineContent + Cache), einmal
ohne und einmal mit `--cook-arrays` (die Flags `--cook`, `--texture-format`, `--texture-quality`,
`--cook-arrays`, `--engine-content-cache` sind neu, vorher packte das Tool nur roh):

| Pak | Größe | Zeit |
|---|---|---|
| Arrays aus (bisheriges Verhalten) | 128,2 MB | 69 s |
| Arrays an | **61,7 MB** (-52 %) | 33 s |

Die Zeile "aus" trifft den Export vom 30.10. (128,3 MB) fast genau, die Messung ist also repräsentativ.
Nicht enthalten sind die vorkompilierten Material-Shader (PSHD, ca. 0,9 MB; der Compiler gehört dem Editor)
und die Szenen. In einem Store-Pak mit Arrays an liegen die drei Arrays als ASTC (Format 1, 12 Mips,
5 Slices, je 27,96 MB) mit `TXSM` (15 Floats); die gebackenen Mittelwerte stimmen mit einer unabhängigen
Python-Rechnung aus den Quellpixeln überein (Albedo-Slice 0: 0,159 / 0,120 / 0,049).

**Getestet:** `test_texture_bcn.cpp` (Cook trägt die Means, exakt gleich den Pixel-Means; BC7/ASTC-Array ohne
lesbare Pixel nimmt die gebackenen Werte; falsche Anzahl wird verworfen; Chunk überlebt Speichern/Laden;
Textur ohne Chunk lädt wie vorher), `test_culling.cpp` (alter Pixelpfad unverändert). Ganze Suite: 6043 von 6044
grün; der eine Fehler (`Disk registry: scanContentDirectory`, 7 statt 2) kommt vom echten EngineContentCache
dieses Rechners (5 Assets zusätzlich) und hat mit der Änderung nichts zu tun.

**Nicht verifiziert:** Sichtprüfung eines kompletten Exports auf der GPU; D3D11/D3D12/Vulkan (Array-Upload
komprimierter Formate ist dort wie bei 2D-Texturen "blind", keine Windows/Linux-Maschine); ein Voll-Export aus dem Editor (die Paketmessung oben kommt vom `hpak_packer`). Normal-Arrays bleiben BC7/ASTC (BC5 würde Z verlieren).

## 8. Umgesetzt: Export packt nur Erreichbares (Punkt 2, Schritt 1586)

**Regel:** Erreichbarkeit ist transitiv ab echten Wurzeln, nicht "hat irgendeinen Verweiser". Ein Mesh, das ein
Material nennt, hält weder Mesh noch Material, wenn nichts Erreichbares das Mesh nennt; ein Material, das ein
erreichtes und ein nicht erreichtes Mesh nennen, bleibt (das erreichte braucht es).

**Wurzeln** (`src/HE_Core/src/Hpak/ReachFilter.cpp`, `src/HE_Core/src/Hpak/ProjectExporter.cpp` `packContent`):
- die Startszene (als CBOR-Blob), `GameInstance.hcode`, das `.heproj`, lose `.lua/.py/.hcode` unter Content, `Config/`;
- jede andere Szene ist ein Knoten und bleibt nur, wenn etwas Erreichbares ihren Pfad nennt (die Startszene
  behält auch ihren Pfad-Eintrag, damit `scene.load` auf sie geht); ohne gepackte Startszene bleiben alle;
- Engine-implizit (`engineImplicitRoots()`): Assets, die die Laufzeit **nach Typ** findet (`PlayerHost.cpp:37-80`
  InputAction, InputMappingContext, HorizonCodeClass; `TypeRegistry::refreshFromContent` Struct/Enum; dazu
  SaveGameTemplate, Theme), die Wetter-Sounds `Engine/Audio/Weather/`, `Engine/MaterialFunctions/`,
  `Engine/Meshes/` (LayerMeshPresets baut diese Namen aus der Material-Gruppe), `Engine/Materials/Water.hasset`,
  die Trail-Textur `0x41F`;
- "Immer mitpacken"-Globs im Profil (`alwaysIncludePatterns`) für Pfade, die ein Skript zur Laufzeit baut.

**Kanten:** Pfad (Suffix-Treffer auf `.hasset`/`.hescene`, mit/ohne `Content/`, auch hinter einem Längenbyte), UUID als
16 Rohbytes (little- und big-endian), als CBOR-Array `82 <uint> <uint>` in kürzester Form (**wichtig**: die Engine-IDs
wie `{0x412,1}` sind `82 19 04 12 01`, ein 16-Byte-Muster findet sie nicht; ohne diese Form flogen bei der ersten
echten Messung die drei Landscape-Arrays, 344 MB, aus dem Pak) und als JSON-Dezimalpaar/-Objekt. Texturen, Audio,
Fonts werden nie gelesen, bei Meshes nur die Strukturchunks (kein VERT/INDX/…); im Zweifel wird behalten.

**Einbau:** `HpakWriter::addDirectories`, Pass 1b zwischen Einlesen und Umschreiben; ausgelassene Assets verschwinden
auch aus `packedPaths`/`packedTypes` und aus `pathToUuid` (ein von einem behaltenen Asset doch genannter Pfad wird so zur
bekannten Warnung "in no packed root" statt zu einer UUID, die das Pak nicht hält). Fingerprint-Bit 7. Der Export-Log
bekommt eine Zeile (`onNotice`: Anzahl, Größe, die fünf größten, nicht geladene Szenen). Schalter: Profilfeld
`onlyReferenced` (Standard an) + `alwaysIncludePatterns`, Dialog-Checkbox "Pack only referenced assets", MCP-Argumente,
`hpak_packer --only-referenced --startup-scene <f> [--always-include <glob>]`. `ExportSettings::onlyReferenced` ist
auf Bibliotheksebene aus (wer lose Assets ohne Szene übergibt, will alle); der Editor reicht das Profil durch.

**Gemessen an Catania** (zwei echte Exporte über `ProjectExporter::exportProject`, `tests/test_bench_export_reach.cpp`,
ASTC, Arrays komprimiert, ohne Zstd):

| | Assets | Pak |
|---|---|---|
| alles | 99 | 108,94 MB |
| nur Erreichbares | 78 | 102,90 MB |

21 Assets raus (nur 6 MB, weil die großen Arrays erreichbar sind): die Alt-Textur `Grass_Albedo` (16,4 MB roh, 5,0 MB
gepackt), `Grass_Normal`, die drei Gruppen-Materialien Desert/Grassland/Forest, 14 Editor-Widgets, ein unbenutztes
Projekt-Material. Mit zstd (hpak_packer): 61,7 MB → 56,7 MB. Das Foliage-Testprojekt: 0 von 60 raus (alles erreichbar).

**Beweis, dass nichts fehlt:** beide Exporte als lauffähiges Spiel (`HEProject`, derselbe Runtime-Ordner) headless mit
`HE_SKY_TIME=30` auf Frame 2100 aufgenommen. Kontrolle zuerst: zwei Läufe des Voll-Exports sind byte-gleich. Gefiltert
gegen voll: einmal byte-gleich, einmal 16 Bytes um höchstens 1 verschieden an der Bergkante (derselbe gefilterte Export,
zweiter Lauf, ist wieder byte-gleich zum vollen: Lauf-zu-Lauf-Rauschen, nicht der Filter). Logs: gleiche 4 Warnungen,
0 Fehler, "loaded packed startup scene". Ohne gepinnte Sky-Zeit unterscheidet sich nur der animierte Himmel.
Die Aufnahme ist erst nach rund 3000 Frames die Szene (davor der Intro-Splash).

**Tests:** `tests/test_reach_filter.cpp` (Insel, Spielarten der Referenz inkl. CBOR-Kurzform, Szenen als Knoten,
Typ-/Engine-/Glob-Wurzeln, Fingerprint, Exporter-Ende-zu-Ende), `tests/test_export_profiles.cpp` (Profilfelder).
Ganze Suite: nur `Disk registry: scanContentDirectory` (EngineContentCache dieses Rechners) und einmal
`test_team_ui` (Netz-Timing unter Last, allein grün) fehlen.

**Nicht verifiziert:** Export aus dem laufenden Editor (Dialog, Log-Zeile; die drei Editor-Dateien nur per
`-fsyntax-only` und Hilfe-Audit); Shader-Vorkompilierung (PSHD) im Filterlauf; Windows/Linux; ein Projekt mit Skripten, die
Pfade zur Laufzeit bauen (dafür die Globs); mehrere Szenen mit Level-Wechsel im echten Spiel.

## 9. Umgesetzt: Startszene nur einmal im Pak (Punkt 3)

Der Editor übergibt die Startszene zweimal: als `sceneBinary` (UUID im `project.hcfg`) und als Eintrag der Szenenliste
(Pfad-UUID für `scene.load("<Pfad>")`). `addSceneEntries` packte beide Male die Bytes.

**Änderung:** `HpakWriter::addAlias(id, target)` legt eine zweite TOC-Zeile auf dieselben gespeicherten Bytes; der Writer
schreibt sie einmal und vergibt zuerst alle Offsets der Besitzer (der Alias kann vor seinem Besitzer sortieren). Der Reader
braucht nichts: er sucht pro Zeile `dataOffset`/`dataSize` und prüft den `contentHash`, und sonst liest niemand
`dataOffset` (grep). Verschlüsselt geht es auch, Nonce und Tag gehören zu den Bytes und stehen in beiden Zeilen. Der Exporter
nimmt jede Szene, deren Bytes schon gepackt sind, als Alias (Startszene oder zwei gleiche Levels).
Kein Formatwechsel: ältere Reader lesen so ein Pak, weil die TOC-Zeilen gewöhnlich aussehen.

**Gemessen (Catania, zwei echte Exporte über `exportProject`, Store, ASTC-Arrays):**

| | vorher | nachher |
|---|---|---|
| alles packen | 108,94 MB | 101,91 MB |
| nur Erreichbares | 102,90 MB | 95,87 MB |

Das sind 7,03 MB, genau eine Szene. Mit zstd bringt es fast nichts (Szene 7,04 MB → 53 KB), dort ist es nur sauberer.

**Verifiziert:** Spiel aus dem alten und dem neuen Pak, gleiche Flags (`HE_SKY_TIME=30`, Frame 2100): die Aufnahmen sind
byte-gleich, "loaded packed startup scene". Tests: `addAlias` (Alias vor und hinter dem Besitzer, Alias eines Alias, zstd-Besitzer,
verschlüsselt) in `tests/test_hpak.cpp`, Exporter-Ende-zu-Ende in `tests/test_project_exporter.cpp` (drei Namen, zwei Szenen
im Pak, Startszene, Pfad-UUID und die andere Szene lesen richtig). Der bestehende Szenenindex-Test (drei gleiche Szenen) läuft
jetzt über Aliase und ist grün.

**Nicht verifiziert:** `scene.load` der Startszene im laufenden Spiel (nur der Pak-Eintrag unter der Pfad-UUID gelesen);
Export aus dem Editor; Windows/Linux.

## 10. Umgesetzt: Shader-Backends folgen dem Backend des Spiels (Punkt 4)

Vorher ein Häkchenfeld, Standard Metal|OpenGL, unabhängig vom Ziel (Abschnitt 4): ein Windows-Export bekam Metal- und
GL-Varianten, ein D3D12-Spiel gar keine, und jeder Export trug die Varianten von zwei Backends, von denen das Spiel eines läuft.

**Regel (`GameBackendRules.h`, `shaderBackendMaskFor`):** die PSHD-Varianten des einen Backends, auf dem das fertige Spiel startet.
Das ist das Backend der Render Defaults des Projekts (`effectiveExportBackend`), sonst der Plattform-Standard der Laufzeit
(`defaultBackendNameFor`: macOS Metal, sonst OpenGL, wie `RendererFactory::Default`). Eine App ohne Advanced Shader Effects
(Software) bekommt 0, es gibt nichts zu backen.

**Profil:** neues Feld `autoShaderBackends` (Standard an, Dialog-Checkbox "Match the game's backend", Hilfe-Eintrag). Aus: die
Häkchen gelten wie bisher. Ein Profil ohne das Feld bleibt manuell, wenn sein Satz nicht der alte Standard Metal|OpenGL ist
(den hat niemand bewusst gewählt); der alte Standard wird Auto. Der Auto-Satz geht in den Fingerprint (`PackManifest`), ein
Wechsel packt die Materialien neu.

**Gefahr:** ein Spiel, das zur Laufzeit auf einem anderen Backend landet (Config nennt eines, das die Laufzeit nicht hat),
übersetzt beim Laden, wie ein Pak ohne Varianten. Kein Fehler, nur die alte Wartezeit.

**Größe:** nicht am echten Export gemessen: die Übersetzung (`CompileMaterialShaderVariants`) hängt am Editor und läuft nicht in
`he_tests`. Shader und Materialien sind bei Catania ca. 0,4 % des Paks (Abschnitt 1), die Ersparnis ist also unter 1 MB. Der Gewinn
ist die Richtigkeit: Windows/Linux-Exporte tragen jetzt die Varianten ihres Backends.

**Verifiziert:** Tests `shader precompile follows the backend the shipped game runs` (`test_export_backend.cpp`), Profil-Runde und
Migration (`test_export_profiles.cpp`); die vier Exporter/Hpak/Profil-Testdateien grün bis auf den bekannten
`scanContentDirectory`-Fehler dieses Rechners; `editor_help_audit.py --check` grün; `ExportDialogPanel.cpp` und `EditorHelp.cpp`
per compdb `-fsyntax-only` ohne Fehler.
**Nicht verifiziert:** der Dialog selbst (nicht gebaut, nicht gesehen); ein echter Export aus dem Editor mit gebackenen
Varianten; Windows/Linux. Partikel-Shader teilen sich dieselbe Maske, bei D3D/Vulkan backen sie weiterhin nichts (siehe
`CompileParticleShaderVariants`), das ist unverändert.

## 11. Geklärt: Graph und GLSL im MTRL, und was das Spiel aus Quelltext kompiliert (Punkt 5)

Gelesen am Code (`ContentManager.cpp`, `MaterialShaderLibrary.*`, die fünf Renderer) und gemessen am echten Catania-Development-Pak
vom 27.10. (4 Materialien, Store, mit PSHD für Metal und OpenGL). Nichts im Spiel gestartet, nichts geändert.

**GLSL im MTRL (`customShaderFragGlsl`, `customShaderGBufGlsl`, `customShaderVertGlsl`): bleibt.** Es ist nicht nur Quelle für die
Übersetzung. `MaterialShaderLibrary::resolveShaders`/`resolveGBufferShaders` leiten daraus den Pipeline-Schlüssel ab (Hash der Quelle),
ein leeres Fragment heißt "eingebautes PBR", und den G-Buffer-Fall (unten) kompiliert das Spiel aus genau diesem Text.

**Graph-JSON (`nodeGraphJson`): zur Laufzeit nur an drei Stellen gelesen**, alle in `ContentManager.cpp`:
1. Basismaterial beim Laden (`:758`): neu erzeugen, aber nur wenn **kein** PSHD da ist. Mit PSHD ist der Graph für ein Basismaterial tot.
2. Instanz mit Switch-Überschreibungen (`syncMaterialInstance`, `:937`): erzeugt GLSL aus dem Graphen des **Elternmaterials** neu und
   verwirft dabei das PSHD der Instanz (`:978`). Fehlt der Graph beim Elternteil, fällt der Code in den Zweig "reine Parameter" und
   die Switches werden still ignoriert, das Bild wäre falsch.
3. `prefetchRemoteReferences` (Funktionsaufrufe, nur im Editor mit Remote-Assets).
Sonst liest ihn kein Laufzeitcode (Szene, Renderer, Partikel haben ihren eigenen). Streichen wäre also nur für Materialien mit PSHD und
ohne Switch-Instanz im Elternbaum sicher, und das ist ein Pack-Zeit-Walk über Elternketten.

**Gemessen:** vier Materialien, MTRL 445 KB roh, davon Graph höchstens ca. 320 KB (die Schätzung schließt GLSL-Klammern ein), zlib
zusammen ca. 82 KB für alle vier MTRL. PSHD dagegen 886 KB roh. Streichen spart im Shipping-Pak (zstd) ein paar zehn KB, bei
Catania unter 0,1 %. **Empfehlung: nicht machen.** Aufwand und Risiko (falsches Bild bei Switch-Instanzen) stehen in keinem
Verhältnis. Die Gelegenheit ist eher, Materialfunktionen und Graph erst bei einem Projekt mit hunderten Materialien anzusehen.

**Wird im Spiel aus Quelltext kompiliert? Ja, in diesen Fällen:**
- **Immer, auf Metal, D3D11/12 und OpenGL: das "vorkompilierte" PSHD ist Quelltext.** Metal bekommt MSL und ruft
  `newLibraryWithSource` auf, D3D ruft `D3DCompile` auf HLSL, OpenGL gibt GLSL an den Treiber. Das PSHD erspart nur die Übersetzung
  durch glslang/SPIRV-Cross, nicht den nativen Compiler. Nur Vulkan bekommt SPIR-V (einen echten Binärblock).
- **Deferred-Pfad:** Das Pak bäckt keine G-Buffer-Variante (`HpakWriter.cpp:597` bäckt nur Forward, UI-Vertex und die geclusterte
  Zwilling-Variante). Läuft das Spiel im Deferred-Pfad (`RenderPath` = 1 in `config.json`, kommt aus dem Editor und steht standardmäßig
  auf 0, Forward), übersetzt es `customShaderGBufGlsl` beim Laden, in allen Backends (`pre = nullptr`, `gbuffer = true`).
  Das ist der einzige Fall, in dem ein Spiel, das exakt nach Vorschrift exportiert wurde, noch glslang braucht.
- **Instanzierte Draws** (Thema 182): Die gebackene Variante hat keinen instanzierten Vertex. Vulkan und D3D geben bei
  `instanced && precompiled` null zurück, der Aufrufer zeichnet dann in einer Schleife statt instanziert (kein Compile, aber auch keine
  Instanzierung für gebackene Materialien).
- **Instanz mit Switch-Überschreibungen:** neu aus dem Graphen des Elternmaterials erzeugt und danach vom Treiber kompiliert, das
  PSHD der Instanz wird verworfen. Das Pak trägt es trotzdem (verschwendete Bytes).
- **Backend ohne gebackene Variante** (Spiel startet auf einem anderen als dem gebackenen, oder der Bake eines Materials schlug fehl):
  Übersetzung beim Laden, wie ohne PSHD.
- **Eingebaute Shader der Engine** (PBR ohne Graph, Himmel, Post-Processing, UI): sind Quelltext im Binär und werden beim Start kompiliert.
- Partikel: nur OpenGL und Metal werden gebacken, D3D/Vulkan setzen die Vorlagen beim Laden zusammen.

**Nicht verifiziert:** nichts davon durch einen Lauf mit Zähler oder Log bestätigt (z. B. ob ein Deferred-Spiel beim Start wirklich
Übersetzungen loggt); die Aussage zu `RenderPath` beruht auf dem Standardwert und der Export-Konfiguration, nicht auf einem Export mit 1.

**Mögliche Arbeit daraus, nicht begonnen:** (a) G-Buffer-Variante mitbacken (nur sinnvoll für Spiele mit Deferred); (b) PSHD der
Switch-Instanzen weglassen oder statt der Neuerzeugung verwenden (Letzteres ändert Laufzeitverhalten, braucht eine Prüfung gegen den
Editor-Sync); (c) instanzierten Vertex mitbacken.

## 12. Umgesetzt: G-Buffer-Variante, instanzierter Vertex, kein PSHD für Switch-Instanzen (Abschnitt 11 a, b, c)

**Wichtig vorweg (Berichtigung zu Abschnitt 11):** Metal baut den instanzierten Zwilling bei einem *nicht* gebackenen Material beim Laden
(`cShBaked` verbot ihn für gebackene), GL gibt bei `instanced && precompiled` 0 zurück. Beide zeichneten gebackene Materialien also
per Instanz in einer Schleife, nicht instanziert. Vulkan und D3D genauso (laut ihren Kommentaren, nicht gelesen).

**a) G-Buffer-Fragment gebacken.** `MaterialShaderVariant::fragmentGBuffer` (Material-G-Buffer-Tail aus `customShaderGBufGlsl`, gleicher
Übersetzer wie das Forward-Fragment, gepaart mit dem normalen Vertex). **c) Instanzierter Vertex:** `vertexInstanced` und, nur GL,
`vertexInstancedClustered` (GLSL 4.30 für das Cluster-Paar). Gebacken wird nur für OpenGL und Metal, die Backends, deren Renderer es
liest. PSHD-Version 4 (`ContentManager.cpp`): drei Strings pro Eintrag als Schwanz nach dem Cluster-Schwanz; ein v3-Leser hört davor auf, ein
v4-Leser nimmt v1 bis v3 wie bisher (Test mit handgeschriebenem v3-Blob, abgeschnittener Schwanz bleibt heil bis dorthin).
`kPackPipelineVersion` 3 → 4, damit inkrementelle Exporte die Materialien neu backen. Der Editor-Export reicht den G-Buffer-Text über den
Rückruf (`compileShaderVariants` hat jetzt vier Argumente), `HpakWriter` liest ihn aus dem MTRL.
**Renderer:** GL (`GetOrBuildMaterialProgram`) und Metal (`GetOrBuildMaterialPipeline`) nehmen die gebackene G-Buffer- und Instanz-Variante,
sonst wie vorher (G-Buffer: Übersetzung beim Laden; Instanz: Schleife). Metal fragt für die Instanz-Pipelines die Variante frisch ab
(`bakedMetalVariant`), weil ContentManager-Zeiger beim nächsten Laden ungültig werden. Das Verbot `!cShBaked` für gebackene Batches ist
weg, **gebackene Materialien zeichnen auf Metal jetzt instanziert**. Das Log sagt, was aus dem Pak kam ("PRECOMPILED variant, G-buffer",
"..., G-buffer, instanced", "..., clustered").

**b) Switch-Instanzen:** `HpakWriter` bäckt für eine Instanz mit Switch-Überschreibungen kein PSHD mehr (das Spiel verwirft es beim Laden,
`ContentManager.cpp:978`). Das Elternmaterial behält Graph und PSHD. Test `Pack hands the G-buffer tail ... and skips switch instances`.

**Gemessen:** Catania (Reached): 95,87 MB ohne Shader, 96,38 MB mit gebackenen Metal-Shadern (+0,5 MB, vier Materialien). Foliage-Testprojekt:
215,10 gegen 216,14 MB (+1,0 MB).

**Verifiziert (Metal, Deferred, `RenderPath` = 1, `HE_SKY_TIME=30`, Frame 2100, je zwei echte Exporte, Kontrolle ohne PSHD gegen mit PSHD):**
Catania: Frames byte-gleich, 0 Fehler; das Log der gebackenen Variante zeigt "G-buffer" und zweimal "clustered" aus dem Pak, die Kontrolle nichts.
Foliage-Testprojekt: Frames byte-gleich, 0 Fehler; zweimal "G-buffer", zweimal "G-buffer, instanced", zweimal "clustered" aus dem Pak.
Tests: `PSHD v4` und `Pack hands the G-buffer tail` in `test_hpak.cpp`, der Torn-Tail-Test in `test_material_graph.cpp` angepasst (der v4-Schwanz
kommt jetzt nach dem Cluster-Schwanz). Die Test-Dateien zu Hpak, Exporter, Profil, Material-Graph, Engine-Materialien und Landscape-Gruppen sind
grün bis auf den bekannten `scanContentDirectory`-Fehler dieses Rechners.
Wiederholbar: `HE_REACH_SHADERS=16` (Bit `1u << Metal`) im Bench `tests/test_bench_export_reach.cpp` bäckt wie der Editor-Export.

**Nicht verifiziert:**
- **OpenGL:** inzwischen im Lauf geprüft, siehe Abschnitt 13.
- **Metal forward instanziert (gebacken):** Der Foliage-Lauf war Deferred, der Forward-Instanz-Pfad mit gebackenem Vertex lief nicht.
- **Vulkan und D3D11/12:** Das Pak bäckt für sie weiter keine Zwillinge, ihre Renderer lesen sie nicht (G-Buffer wird dort weiter beim Laden übersetzt, ein
  Batch gebackener Materialien zeichnet in der Schleife). Auf dem Mac nicht prüfbar.
- **Editor-Export und Dialog** (die Bake-Funktion im Editor ist per `-fsyntax-only` geprüft, nicht gebaut), Windows und Linux.
- Kein Zeitvergleich der Ladezeit oder der Draw-Calls mit und ohne Instanzierung.

## 13. Deferred als Standard, OpenGL mit gebackenem Pak im Lauf

**Deferred als Standard:** `EditorConfig::RenderPath` (`src/HE_Editor/EditorConfig.h`), `ProjectSettings` (`renderPath`, Vorgabe 1) und der
Rückfall in `GameApplication.cpp` (`getCustomConfigInt("RenderPath", 1)`) stehen auf 1. Ein Backend ohne `supportsDeferredRendering` bleibt bei
Forward. Gespeicherte Projekte behalten ihren Wert. Das Golden `tests/fixtures/game_build_config/HEGameBuildConfig.h` ist neu erzeugt
(`"renderPath": 1`, sonst nichts).

**OpenGL mit gebackenem Pak (macOS, GL 4.1, Foliage-Testprojekt, Deferred, `HE_SKY_TIME=30`, 300 Frames, Capture bei Frame 250):**
Zwei echte Exporte (Kontrolle ohne PSHD, `HE_REACH_SHADERS=1` mit PSHD), beide mit `GameBackend` OpenGL. Die Frames sind byte-gleich
(md5 gleich), 0 Fehler, `glGetError=0` im ersten Frame. Das Log der gebackenen Variante zeigt "PRECOMPILED variant, plain", "G-buffer" und
"G-buffer, instanced" aus dem Pak; die Kontrolle nur "canonical GLSL via he::shaderc". Pak 215,10 gegen 215,98 MB (+0,9 MB).
Die geclusterten Paare (GLSL 4.30) kann macOS nicht benutzen, sie sind hier also nicht gelaufen.

**Falle:** Ein verstecktes Fenster (`HE_HIDDEN_WINDOW`) mit OpenGL und eingeschaltetem VSync blieb nach Frame 1 stehen (0 % CPU, Hauptthread in
einer SDL-Bedingungsvariable, `SDL_GL_SwapWindow`). Das betraf Kontrolle und gebackene Variante gleich, hat also nichts mit dem Pak zu tun.
Behoben in `Window.cpp`: Ein Fenster, das nie sichtbar wird (`hiddenWindowRequested()`), bekommt Swap-Intervall 0, beim Anlegen und in
`Window::SetVSync`. Geprüft mit dem Export aus diesem Abschnitt, `GameVSync` true, neu gebaute Bibliothek: 300 Frames, 0 Fehler, Frame 250
byte-gleich zum Lauf mit VSync aus. Nicht geprüft: Metal/Vulkan/D3D (dort wird nichts am Swap geändert) und ein sichtbares Fenster.

**Nicht verifiziert:** Der erste Frame der gebackenen Variante zeichnete 19 Draw-Calls, die Kontrolle 18 (der gespeicherte Frame 250 ist
gleich); den Grund habe ich nicht verfolgt. Kein Lauf mit Forward auf GL. Windows und Linux.

## Handoff

- **Topic / step:** 228, Schritt 1585 (Audit, fertig) und Punkt 1 der Liste (Arrays, umgesetzt); Schritt 1586 (Filter) umgesetzt, Abschnitt 8
- **Branch / commit:** release/0.8.0 (pushed: siehe Commit)
- **Done:** Audit-Bericht; Texturarrays werden beim Export komprimiert, GI-Mittelwerte kommen aus dem Pak (TXSM).
- **Files touched:** `src/HE_Core/src/Hpak/HpakWriter.cpp` (cookTexture: Means backen), `src/HE_Core/src/ContentManager/TextureBlockDecode.cpp` (Mittelwert-Helfer), `src/HE_Core/src/ContentManager/ContentManager.cpp` (TXSM laden/speichern), `src/HE_Rendering/src/GiAutoLandscape.cpp` (baked bevorzugt), `src/HE_Core/include/Hpak/ProjectExporter.h` + `ProjectExporter.cpp` (Schalter), `src/HE_Tools/src/FileOps/ProjectManager.*` (Profilfeld), `src/HE_Editor/ExportDialogPanel.cpp`, `EditorHelp.cpp`, `McpToolsBuild.cpp`, `tests/test_texture_bcn.cpp`.
- **Verified:** he_tests gebaut und gelaufen (6043/6044, der Fehler ist umgebungsbedingt, s. o.); Bench auf den echten Arrays; `editor_help_audit.py --check` grün; die drei Editor-Dateien per compdb `-fsyntax-only` ohne Fehler (HorizonEditor selbst nicht gebaut).
- **Not verified:** siehe Abschnitt 7.
- **Decisions taken:** Schalter standardmäßig an, weil die Arrays 94 % des Shipping-Paks sind. Means immer backen, auch bei BC3 (genauer und ohne Dekodieren).
- **Decisions suggested, not taken:** Kein `kPackPipelineVersion`-Bump (das Flag `cookTextureArrays` steckt schon im Fingerprint, vorher gecookte Arrays gab es nur in Tests).
- **Pending:** Export aus dem Editor einmal von Hand ansehen (Dialog mit der neuen Checkbox, Log, gebackene Varianten). Punkt 5 geklärt (Abschnitt 11), die drei Arbeiten daraus umgesetzt (Abschnitt 12, Metal geprüft, GL nur kompiliert). GL gebacken im Lauf geprüft (Abschnitt 13), Deferred ist Standard. Offen: Vulkan/D3D-Zwillinge (nicht auf dem Mac prüfbar), Metal forward instanziert mit gebackenem Vertex im Lauf ansehen, verstecktes GL-Fenster mit VSync hängt. Startszene nur einmal: erledigt, Abschnitt 9 (`HpakWriter::addAlias`).
- **Traps:** `grep --include` und zsh-Globs: `/usr/bin/grep -r --include='*.cpp'` mit Anführungszeichen; Ziel `src/HE_Editor/CMakeFiles/...o` baut den EngineSDK-Deploy mit (stört), für einzelne Dateien compdb nutzen; ein leeres `libHorizonImporters.a` nach abgebrochenem Build löschen.
- **Next steps:** 1. Catania aus dem Editor exportieren (Profil Shipping) und auf Metal ansehen (die Paketgröße ist schon per `hpak_packer` gemessen). 2. Hängen des versteckten GL-Fensters mit VSync ansehen (`Window::SwapBuffers`, `SDL_GL_SetSwapInterval`), Unterschied 18/19 Draw-Calls im ersten GL-Frame erklären.
