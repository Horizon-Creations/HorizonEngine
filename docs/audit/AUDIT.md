# Gesamtauswertung – Performance- und Simplification-Audit Horizon Engine

Stand: 2026-10-16, Branch `release/0.7.0`. Rein lesendes Audit (nur `docs/audit/` wurde geschrieben). Grundlage: 11 Modulberichte `modul_*.md` (je mit vollständigen Funden im geforderten Format) und `00_INVENTAR.md`.

**Wie belastbar ist das?** Statisch gelesen, nichts gebaut, nichts profiliert. Alle Laufzeit-/Speicherangaben in den Modulberichten sind Schätzungen und dort so markiert. Stichprobe durch mich im Quelltext (nicht nur im Teilbericht) bestätigt: `gpuDebug = true` (VulkanRenderer.cpp:1286), `entityByUuid` mit `snprintf` pro Entity (EditorCommands.cpp:419-429), `resolveShaders` im Draw-Loop von D3D11/D3D12/Vulkan (Zeilen 7009/10974/8254), `onProjectWriteRefused` unbedingt benutzt, aber nur unter `HE_HAVE_LIBSSH2` definiert (EditorApplication.cpp:536 vs. 2412-2670). Die übrigen Funde habe ich nicht einzeln nachgeprüft; jeder Fund trägt Datei:Zeilen und Code-Auszug, damit das in der Umsetzung geschieht.

## 1. Zusammenfassung

### Funde pro Modul, Kategorie und Schweregrad
(Zählung maschinell über die Fundblöcke der Modulberichte; "Verf." = Vereinfachung, "Perf." = Performance)

| Modul | Funde | Hoch | Mittel | Niedrig | Verf. | Perf. | Beides | A | B | C | D | E | F |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Core | 157 | 17 | 66 | 74 | 45 | 94 | 18 | 29 | 8 | 37 | 53 | 13 | 17 |
| Net | 41 | 5 | 13 | 23 | 20 | 19 | 2 | 9 | 1 | 12 | 5 | 6 | 8 |
| SourceControl | 24 | 3 | 10 | 11 | 8 | 16 | 0 | 2 | 0 | 5 | 8 | 5 | 4 |
| ContentSync | 16 | 2 | 7 | 7 | 9 | 6 | 1 | 3 | 0 | 2 | 3 | 2 | 6 |
| SelfUpdate | 20 | 0 | 6 | 14 | 17 | 2 | 1 | 7 | 0 | 2 | 2 | 2 | 7 |
| Python | 14 | 1 | 6 | 7 | 8 | 6 | 0 | 5 | 2 | 3 | 1 | 0 | 3 |
| Scene | 153 | 20 | 71 | 62 | 21 | 114 | 18 | 12 | 4 | 60 | 59 | 7 | 11 |
| Rendering | 204 | 33 | 108 | 63 | 61 | 132 | 11 | 44 | 12 | 43 | 74 | 14 | 17 |
| Tools | 27 | 3 | 11 | 13 | 9 | 14 | 4 | 7 | 0 | 6 | 8 | 4 | 2 |
| Game | 13 | 0 | 5 | 8 | 4 | 7 | 2 | 2 | 2 | 2 | 4 | 1 | 2 |
| Editor | 362 | 35 | 157 | 170 | 116 | 210 | 36 | 94 | 5 | 101 | 100 | 22 | 40 |
| **Summe** | **1031** | **119** | **460** | **452** | 318 | 620 | 93 | 214 | 34 | 273 | 317 | 76 | 117 |

Kategorien: A Logik · B Kontrollfluss · C Speicher · D Algorithmen/Datenstrukturen · E Nebenläufigkeit/I/O · F Sonstiges.

Lesart: Rund 60 % der Funde sind reine Performance, 31 % reine Vereinfachung. Der Schwerpunkt liegt bei **C (Speicher/Kopien/Allokationen pro Frame)** und **D (fehlende Indizes, fehlende Dirty-Flags)**, nicht bei exotischen Algorithmen: Die Engine ist an vielen Stellen *korrekt, aber jeden Frame alles neu*. Der Editor stellt mit 362 Funden die Hälfte der Hoch-Funde, aber dort sind es fast ausschließlich Draw()-Pfade, die pro Frame Verzeichnisse scannen, Listen kopieren oder Zeilen nicht clippen.

### Die 10 wichtigsten Maßnahmen engine-weit
Sortiert nach (Wirkung × Verbreitung) / Aufwand. Details und Code-Skizzen in den genannten Funden.

1. **Graph-Material-Draw: Shader-String nicht pro Draw kopieren/hashen** (Rendering: REN-101, 201, 403, 602, 801, 1001; 5 Backends). `MaterialShaderLibrary::resolveShaders/resolveFragment` kopiert den GLSL-Text und hasht ihn pro Draw und Frame. Lösung: Hash und Zeiger am Material cachen (Revision), im Draw-Loop nur Pointer-Lookup. Ein Eingriff in `src/material/MaterialShaderLibrary.cpp` wirkt in allen Backends.
2. **DrawCall nicht tief kopieren** (REN-901, 902, 1002). `CommandBuffer::recordDraw`, die Section-Expansion in `RenderPass.cpp:102` und der Split-Pfad kopieren `instanceTransforms` (bis 65 536 mat4) und Bone-Matrizen mehrfach. Lösung: Move-Overloads, Instanzdaten als Range in flachem Puffer.
3. **Cull+Sort pro Shadow-Layer und mehrfach pro Frame** (REN-203, 407, 605, 805; auf allen Backends). Der Sort-Schlüssel ist kameraabhängig und für alle Layer gleich; statische Layer werden jeden Frame neu gerendert. Lösung: einmal sortieren, pro Layer nur filtern; statische Layer cachen.
4. **Content-Baum-Scans aus dem Draw-Pfad nehmen** (Editor: EDT-901, 902, 1001, 1002, 1101, 1702, 1801; Core: CORE-202). `HcEditorUtil::listAssets/listClasses` läuft rekursiv mit Header-Read pro `.hasset` und wird in mindestens sieben Panels pro Frame aufgerufen. Lösung: ein revisionsbasierter Asset-Index im `ContentManager` (invalidiert bei Dateiänderung), Listen erst bei geöffneter Combo holen; `discoverAssets` liest nur den Typ-Header.
5. **Lineare Suchen im UI-/Widget-Baum durch Index ersetzen** (Core: CORE-601, 701, 702, 703; Editor: EDT-1802, 1803). `UIWidgetTree::find`/`childrenOf`/`uiChildIndexOf` sind linear und werden pro Element, pro Vorfahr, pro Frame aufgerufen (O(n²·Tiefe)). Lösung: `id→index`-Tabelle und Kinder-Index im Baum, ein Top-down-Layoutpass pro Frame. Wirkt in Game und Editor.
6. **Skript-/HorizonCode-Dispatch: kein `Context`/`Ctx` mit 35–60 `std::function` pro Aufruf** (Core: CORE-401, 402, 301, 302; Scene: SCN-201, 703; Python: PYT-101, 102; Game: GAM-106; Editor: EDT-406). `Value` ist 280 Byte und wird per Wert kopiert. Lösung: `Context` pro Instanz/Level einmal bauen, Event-/Funktions-Index pro Klasse, `coerce` per `const&`, Argumente per `span`. Größter Hebel für Gameplay-Skripte, aber mit Messung zu begleiten (siehe Abschnitt 5).
7. **Per-Frame-Systeme ohne Dirty-Gate** (Scene: SCN-102, 301, 401/501, 402, 601, 604, 801, 802; Editor: EDT-402, 1601, 201, 1401, 2001). Animatoren, Replikation, LOD, Physik-Write-back, Seile, Terrain und Collab-Strukturdiff laufen jeden Frame über alles und setzen teils `dirty = true` auch im Stillstand (löst dann Transform-Propagation und Extraktion aus). Lösung: Revisionszähler/`settled`-Flags, `on_construct/on_destroy`-Signale statt Vollvergleich.
8. **Echte Fehler und Release-Fallen (zuerst, billig)**: `gpuDebug = true` in Release (REN-804, Kommentar im Code nennt es "temporarily"); Link-Fehler ohne libssh2 (EDT-401); Use-after-return beim Download-Abbruch (CORE-203); unbegrenzte Rekursion bei handeditierten Struct-Defaults (CORE-604); Integer-Overflow in der Snapshot-Chunk-Prüfung und `resize` vor Längenprüfung (NET-105/106); Datenrennen auf Debug-Line-Buffer bei 3 Frames in Flight (REN-204); beendete Stimmen werden nie abgeräumt (SCN-112); Use-after-erase in `BoneMaskPanel::jointRow` (EDT-2007); fehlende Timeouts (CORE-501 Windows-Drain, CSY-102 libssh2).
9. **Gemeinsame Quellen statt handgepflegter Parallellisten** (Core: CORE-201, 603, 511; Editor: EDT-801, 1208/1209, 703/704; Rendering: REN-701, 1013). Die Listen driften bereits sichtbar: Pool-Listen in `ContentManager` (Struct/Enum/Theme/BoneMask lassen sich nicht entladen), 8 parallele Panel-Listen in `EditorUI.cpp` (Cinematic/SkeletalMesh fehlen jeweils in einer), Settings-Katalog vs. Panel mit widersprüchlichen Bereichen, drei Uniform-Location-Sätze. Lösung: je eine Tabelle (`forEachPool`, `AssetPanelOps`, `UIPropTable`, `ScenePassLocs`).
10. **GPU-/I/O-Synchronisation auf dem Render-/UI-Thread** (Vulkan: REN-802/803 `vkQueueWaitIdle` pro Upload, host-visible Meshes; Metal: REN-302, 402, 406; D3D12: REN-202 Meshes im UPLOAD-Heap; SourceControl: SCM-101/102 git-Prozesse pro Frame im UI-Thread; Editor: EDT-1501). Lösung: Staging-/Upload-Ring wiederverwenden (existiert in Metal bereits), Device-Local-Memory mit Sub-Allocator, git-Ergebnisse pro Namensänderung cachen.

## 2. Muster-Übersicht (projektweit per Regel/Refactoring behebbar)

| # | Muster | Fundstellen (Auswahl) | Regel / Refactoring |
|---|---|---|---|
| M1 | **Pro-Frame-Neuaufbau statt Dirty-Flag/Revision**: Vollserialisierung zum Änderungsvergleich, Vollkopie von Stores, Neuaufbau von Sets | EDT-201, 1401, 1402, 2001, 402, 1601; SCN-301, 401, 501, 604, 801; GAM-102 | Regel: "Wer Änderung erkennen will, bekommt einen Revisionszähler am Besitzer". Gemeinsame `Revision`-Hilfe in Core. |
| M2 | **Kein Index → lineare Suche pro Element** (O(n²)) | CORE-601/701/702/703, 406; EDT-202, 701, 1802, 1803, 1804; REN-404; SCN-704 | `id→index`-Tabellen an der Datenstruktur; `std::vector`+`lower_bound` oder `unordered_map` mit stabilen Schlüsseln. |
| M3 | **String als Schlüssel im Hot Path** (`snprintf`/`to_string`/Konkatenation, FNV über Text, Textur-/Shader-Keys) | EDT-1201, 1602, 503; REN-101, 102, 906; CORE-101, 104; PYT-101; Hash-Duplikate in ~14 Dateien (Editor_1) | Typisierte IDs/POD-Keys; `entityByUuid` auf `HorizonWorld::findByEntityId`; ein gemeinsamer `Fnv1a`-Header. |
| M4 | **`std::function`-Bündel/By-Value-Contexts pro Aufruf** (35–60 Funktionsobjekte) | CORE-401, SCN-201/703, EDT-406, GAM-106, EDT-903, 1102 | Kontext einmal pro Instanz; Interface/Vtable oder Zeiger-Struct statt Funktionsobjekte; Callbacks nur an Konfigurationsstellen. |
| M5 | **Whole-File-Read und Zweifachparsing**: `istreambuf_iterator`, volle `Reader::open` nur für den Typ, JSON zweimal parsen | CORE-102, 202; TLS-102, 103, 108; EDT-204; SCN-702 | Header-only-Leser (`readAssetTypeFromFile` existiert), `std::ifstream`+`read` in vorreservierten Puffer, Parse-Ergebnis weiterreichen. |
| M6 | **Kopie statt Move/Referenz** (große Structs by value, Vektoren in Callbacks) | REN-901/902/1002; SCN-701; CORE-302, 108; EDT-1402, 906, 908 | Move-Overloads, `const&`/`span`, `std::move` beim letzten Verbrauch; clang-tidy `performance-*` mit Schwerpunkt Hot Paths. |
| M7 | **Handgepflegte Parallellisten driften** | CORE-201, 511, 603; EDT-801, 1208, 703/704; REN-701; Pin-Typ-Tabelle in 5 Kopien (EDT-1008) | Eine Tabelle pro Konzept + Test, der Vollständigkeit prüft. |
| M8 | **Copy-Paste über Backends** (5 Renderer): SSAO-Kernel, Sky-IBL-Bake, Cluster-Fill, TAA-Velocity-`unordered_map`, UI-Quads, GI-Kernel (7 Kopien), HLSL D3D11/D3D12 | REN-104, 111–113, 401, 903, 908, 911, 1013 | Backendneutrale Frontend-Funktionen in `HE_Rendering/src` (Sky-Bake, SSAO-Kernel einmalig, Cluster-Build ohne Heap); Shader-Quelle einmal, Spiegelung generieren statt von Hand pflegen. |
| M9 | **Monolith-Funktionen** (>1000 Zeilen) | `ContentBrowserPanel::render` 4 400, `dumpFrameHeadless` 4 750, `renderEditor` ~3 000, `renderForImpl` 2 900, `GraphEditor::draw` 1 300, `EncodeFrame` ~1 000, `OnInit/OnRender` (Game) | Extract-Method ohne Verhaltensänderung; Witness-Szenen in eigene Datei. Reine Vereinfachung, aber Voraussetzung, um M1/M2 sauber zu fixen. |
| M10 | **Prozess-/Datei-/Syscall-Aufrufe im Draw-Pfad** | SCM-101/102, EDT-1501, 1410, 103, 305, 403 | Regel: Im Draw-Pfad nur lesen; Ergebnisse asynchron in Worker, pro Eingabeänderung cachen. |
| M11 | **Fehlende Timeouts/Drain bei Prozessen und Netz** | CORE-501, CSY-102, EDT-1205 | Einheitliche Timeout-Policy in `Process.h`/`SftpClient`. |
| M12 | **Row-Clipping fehlt, obwohl Zeilenhöhe konstant** (CLAUDE.md fordert es) | EDT-301, 302, 1503, 1802, 909, 910, 1404 | `ImGuiListClipper` für alle Listen/Grids mit konstanter Zeilenhöhe; im UI-Test (`test_*_ui`) Frame-Zeit-Budget prüfen. |

## 3. Cross-Modul-Funde (Schnittstellen)

* **Rendering ⇄ Scene/Extract**: Cull/Sort laufen bis zu 5× pro Frame, weil jede Passfunktion (Decal-Depth, DrawScene, GI, SSAO, Schatten) `RenderExtractor`/`RenderSorter` selbst aufruft (REN-805, 304, 407). Besitzer sollte der Frontend-`RenderGraph` sein; Backends bekommen fertig sortierte Listen.
* **Rendering ⇄ Core (ContentManager/Material)**: `resolveShaders` ruft pro Draw `ContentManager::getMaterial` plus Textkopie (REN-101/1001). Schnittstellenänderung: Material liefert unveränderlichen `ShaderRef` mit Hash/Revision.
* **Scene ⇄ Core (Script-API)**: `HE::api::Ctx` (≈25–35 `std::function`) wird pro Skriptaufruf gebaut (SCN-203, 703; CORE-401; GAM-106). Cross-Module-Verträge (`EngineApi.h`) zwingen `Value`-Vektoren (280 B) durch jeden Aufruf. `EngineApi.h`/`HorizonWorld.h` ziehen zudem ≈3 900 Zeilen HE_Core in 55–190 Übersetzungseinheiten (EDT-1208, CORE-801: WidgetManager.h ⇒ HorizonCode/UIWidget-Header überall).
* **Editor ⇄ Scene**: `entityByUuid` (linear, String-Format) vs. vorhandenes `HorizonWorld::findByEntityId` O(1) (EDT-1201, 1602, 503); `EditorSelection::roots()` pro Frame O(S²·Tiefe) (EDT-701, 1805); Collab-Strukturdiff statt ECS-Signalen (EDT-402/1601).
* **Editor ⇄ Core (UIWidget)**: UI-Designer rechnet pro Frame Hierarchie/Zeichenliste mit linearem `UIWidgetTree::find` – dieselbe Wurzel wie CORE-701 (EDT-1802/1803).
* **Net ⇄ Editor (Collab)**: Host serialisiert Assets pro Peer neu (NET-103) und das Relay-Fan-out bleibt ungenutzt; Editor liest Dateien mit `istreambuf_iterator` auf dem UI-Thread (EDT-204). `TcpTransport/SecureTransport` mit `erase(begin)` (NET-202).
* **SourceControl ⇄ Net**: `ScClient`-Verhalten bestimmt SCM-106/107/109 (serielle PUTs, Teilmengen-Löschungen bei Abbruch).
* **ContentSync ⇄ SelfUpdate**: gleicher `path.string() + ".tmp"`-Fehler gegen die UTF-8-Pfadregel in beiden Modulen (SUP-102, SftpClient.cpp:337-341); doppelte Atomic-Write-Implementierung.
* **Core ⇄ Tools**: `Importer::writeAsset` baut pro Asset einen ganzen `ContentManager` (TLS-101, inkl. Default-Assets und Icon-Raster) – Core sollte einen leichten "Writer ohne Katalog" anbieten; `hpak_packer` ohne `IncrementalCache` (TLS-203), obwohl der Editor-Export ihn nutzt.
* **Core ⇄ Rendering (Sky)**: Sky-IBL-Bake (98 k Integrale, ~60 ms laut Kommentar) und `BuildSSAOKernel` pro Frame in allen 5 Backends – gehört in ein gemeinsames Frontend-Modul (REN-401, 908).

## 4. Empfohlene Reihenfolge der Umsetzung

Sortiert nach Impact/Aufwand und Modulabhängigkeit (Basismodule zuerst, weil Ergebnisse nach oben durchschlagen). Jeder Punkt: separate PR, bestehende Tests (`tests/`) als Netz, vorher/nachher messen wo "Messung" angegeben.

**Stufe 0 – Korrektheit und Release-Fallen (Stunden, geringes Risiko)**
- [ ] REN-804 `gpuDebug` auf `_DEBUG || getenv("HE_GPU_DEBUG")` zurückstellen (Kommentar im Code sieht das vor)
- [ ] EDT-401 `onProjectWriteRefused` auch ohne libssh2 definieren; Build ohne libssh2 in CI aufnehmen
- [ ] CORE-203 Use-after-return in `EngineDeps::resolve` (`shared_ptr`-Zustand); CORE-604 Zyklenschutz in `fieldDefault`; NET-105/106 Längenprüfungen vor `resize`; EDT-2007 Use-after-erase; REN-204 Debug-Buffer pro Frame in Flight; SCN-112 Stimmen abräumen; CORE-501/CSY-102 Timeouts
- [ ] Sichtbare Defekte: EDT-604 (`##wx` in Referenztiteln), EDT-1718 (Gedankenstrich doppelt kodiert), EDT-307 (Collab-Reload nie wirksam)

**Stufe 1 – Core-Fundamente (beeinflussen alles darüber)**
- [x] CORE-201 `forEachPool`; CORE-202 Typ-Header statt Volllesen; CORE-204 `m_pathMtime` nach Save (`docs/audit-core-fundamente-2026-10-17.md`)
- [ ] UIWidgetTree: `id→index` + Kinder-Index (M2), danach CORE-601/602/701–703 (ein Layoutpass)
- [ ] CORE-302 `coerce(const Value&)` (Einzeiler), CORE-401/402/303 Context-/Index-/Klassen-Cache (mit Messung)
- [ ] CORE-403 Hash: wortweise FNV mit Versions-Bump und Reader für alte Version (HAsset.h-Regel beachten)

**Stufe 2 – Rendering-Frontend (wirkt in 5 Backends)**
- [ ] REN-101/1001: Shader-Ref am Material (M3) ⇒ danach pro Backend nur Pointer-Lookup
- [ ] REN-901/902/1002: DrawCall-Moves und Instanz-Range
- [ ] REN-805/407: Cull+Sort einmal, pro Layer filtern; statische Shadow-Layer cachen (Messung)
- [ ] REN-903/908/401: Cluster-Build ohne Heap, SSAO-Kernel einmalig, Sky-Bake gemeinsam und gedrosselt

**Stufe 3 – Backend-spezifisch (je Backend eigene PRs, unabhängig parallelisierbar)**
- [ ] Vulkan: REN-802/803 Upload-Pfad (Staging-Ring, kein `vkQueueWaitIdle`, Device-Local + Sub-Allocator), `VkPipelineCache`
- [ ] Metal: REN-302/402 `FrameUploadRing` benutzen, REN-405/404 TLAS/BLAS-Dirty, REN-406 asynchroner BLAS-Build
- [ ] D3D12: REN-202 Meshes in DEFAULT-Heap; D3D11/GL: Material-Draw-Zustand (REN-103/601), UI-Batching (REN-603)
- [ ] Metal-Gottklasse REN-301 auf Pimpl wie D3D (reine Vereinfachung, danach)

**Stufe 4 – Scene-Systeme**
- [ ] SCN-102/101 Animator-Gate und Vektor-Wiederverwendung; SCN-103 `lower_bound`
- [ ] SCN-301/401/501/402/601: Revisions-/Settled-Flags; SCN-801/802 Terrain-Gates (vor Tessellation/Material-Sync den Early-out)
- [ ] SCN-302 geteilter Asset-Cache statt Kopie pro Entity; SCN-701/702 Move statt Kopie, Prefab-Sync nur bei Änderung

**Stufe 5 – Netz, Tools, Randmodule**
- [ ] NET-101 Byte-aligned Fast Path im BitStream (Wire-Format unverändert, Test!), NET-102/103 Frame einmal bauen, NET-201/202 POD-Adresse und Lesekopf
- [ ] TLS-101/102 (leichter Asset-Writer, MREF-Chunk-only), TLS-201 glslang einmal initialisieren, TLS-203 Cache für hpak_packer
- [ ] SCM-101–103 git-Aufrufe reduzieren; CSY-101 SFTP-Session wiederverwenden; SUP-101 Takeover vom UI-Thread

**Stufe 6 – Editor (viele kleine, voneinander unabhängige PRs)**
- [x] Zentraler Asset-Index (Maßnahme 4) ⇒ dann die sieben Panel-Fixes in einem Zug (`docs/audit-editor-2026-10-18.md`)
- [ ] `entityByUuid` → `findByEntityId`; `EditorSelection` mit Set (beides erledigt, `docs/audit-editor-2026-10-18.md` Step 2); Row-Clipping-Pass (M12) offen
- [ ] Collab: EDT-402/1601/201/202 Signale+Revisionen
- [ ] Tabellen statt Parallellisten (EDT-801, 703/704), danach Extract-Method an den Monolithen (M9)

## 5. Nicht beurteilbar / Messung nötig

| Bereich | Warum unsicher | Vorschlag: was messen |
|---|---|---|
| HorizonCode-Interpreter und `Context`-Bau (CORE-301/401/402/302) | Auswirkung hängt von Graphgröße und Tick-Rate ab | Mikrobenchmark: 1000 Entities mit Tick-Graph, ns/Knoten, Allokationen/Frame (heaptrack / Tracy-Zonen) |
| UI-Layout/`topmostHit`/`applyAutoSize` (CORE-601/602/701–704) | Szenen mit realistisch vielen Widgets fehlen | Frame-Zeit `WidgetManager::extract` bei 100/500/2000 Elementen; gibt es Frame-Gates im Aufrufer (GameApplication.cpp:2347)? |
| Shadow-Pass-Cache (REN-203/407/605) | GPU- vs. CPU-Bound je Backend unklar | CPU-Zeit Cull+Sort pro Layer vs. GPU-Zeit Shadow-Pass (RenderDoc/PIX/Xcode); Anteil statischer Caster |
| `resolveShaders` pro Draw (REN-101 u. a.) | Gewinn hängt von Draws mit Graph-Material ab | Zeit in `resolveShaders` pro Frame bei 1000 Draws, Allokationen |
| Sky-Bake (REN-401) | Code-Kommentar nennt ~60 ms seriell, nie gemessen im Tag-Nacht-Zyklus | Häufigkeit des Rebake bei Sonnenbewegung, CPU-ms/Frame; Alternativen 32² / GPU-Bake |
| Vulkan Upload-Pfad (REN-802/803) | Ladezeit-Anteil unbekannt | Ladezeit großer Szene, Zeit in `vkQueueWaitIdle` |
| Physik-Write-back (SCN-601), Contact-Mutex (SCN-602) | Jolt-Worker-Last | Jolt-Profiler, Lock-Contention bei 1000 Kontakten/Step |
| Transform-Propagate-Scan (Scene, "Unsicher") | Autoren haben Trade-off dokumentiert und gemessen | Nur neu messen, wenn Entity-Zahl >50 k |
| Editor-Draw (Panels, EDT-*) | Vieles vermutlich <0,05–0,5 ms | ImGui-Frame-Profil mit großem Projekt (≥10 k Assets, ≥5 k Entities): Top-10 Panels nach ms |
| Asset-Typ-Cache-Invalidierung (EDT-504) | Dateisystem- und Cachegrößenabhängig | Zeit des ersten Typfilter-Frames nach `invalidateAll()` |
| Startzeit Editor/Game (GAM-104, EDT-607 EditorHelp-Index, REN-110 Shader-Compile 5×) | Nie gemessen | Cold-Start-Trace; Anteil Shader-Compile, `project.hcfg`-Doppelladen |
| Hpak-Export (CORE-409–411) | Serielles Cook; Parallelitätsgewinn hängt von Texturanteil ab | Exportzeit eines Referenzprojekts, CPU-Auslastung |
| `Value`-Verschlankung (CORE-302) | Strukturänderung mit weitem Radius | Erst nach `coerce`-Fix und Profil entscheiden |

## 6. Abdeckung und Vorbehalte

* **Nicht auditiert**: `tests/` (≈229 k LOC, dient als Verifikationsnetz), vendorte Bibliotheken (`glm`, `glad`, `vendor/`, EditorDeps, FetchContent-Pakete), Build-Skripte (nur punktuell über die Modul-CMakeLists im Editor-Slice 19).
* **Teilweise gelesen** (Details in den Modulberichten): `gi_probe_hw.hlsl`/`gi_shadow_hw.hlsl` (Rendering), Datentabellen in `EditorHelp.cpp` (Z. 1960–9372) und `HcNodeDocs.cpp` (Z. 120–2080) nur per Skript, `EditorSelfUpdate.h` Z. 1–60 überflogen, Witness-Szenen in `EditorApplication.cpp` nur strukturell.
* **Editor Slice 1** wurde nach einem Fehlstart (falscher Umfang) neu gelesen; der verworfene Bericht liegt als Ergänzung in `_parts/supplement/` und ist nicht in den Zahlen oben.
* **Ablauf**: Module wurden in der vorgegebenen Reihenfolge begonnen; kleine Module (2–6, 9–10) liefen aus Zeitgründen parallel. Innerhalb jedes Moduls haben Subagents je einen Slice vollständig gelesen, die Modulberichte habe ich maschinell aus den Teilberichten zusammengeführt (`_merge.py`), nicht jeden Fund einzeln gegengelesen.
* **Plattformen**: Windows-/macOS-Backends wurden nicht gebaut; Aussagen zu D3D11/D3D12/Metal sind reine Codelektüre. Vorschläge sind so formuliert, dass sie nicht plattformspezifisch sind; Verifikation pro Backend bleibt nötig.
