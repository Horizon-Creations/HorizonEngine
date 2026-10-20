# Modulbericht Core – HorizonCore (75 k LOC)

Pfad: `src/HE_Core` · Teilberichte: 8 (`_parts/Core_*.md`)

## Modulzusammenfassung

**157 Funde**: Hoch 17, Mittel 66, Niedrig 74 · Typ: Vereinfachung 45, Performance 94, Beides 18 · Kategorien: A 29, B 8, C 37, D 53, E 13, F 17

| Teilbericht | Hoch | Mittel | Niedrig |
|---|---|---|---|
| Core_1.md | 0 | 9 | 15 |
| Core_2.md | 4 | 9 | 7 |
| Core_3.md | 3 | 11 | 6 |
| Core_4.md | 3 | 12 | 11 |
| Core_5.md | 1 | 11 | 10 |
| Core_6.md | 2 | 5 | 11 |
| Core_7.md | 4 | 6 | 9 |
| Core_8.md | 0 | 3 | 5 |

### Hoch-Funde (Index)

- **CORE-201** – Pool-Listen in `unloadAsset`, `forgetProjectContent`, `retargetAssetReferencesInMemory` sind unvollständig
- **CORE-202** – `discoverAssets` liest für jede `.hasset` den **gesamten** Dateiinhalt, nur um den Typ zu prüfen
- **CORE-203** – Use-after-return in `EngineDeps::resolve` beim Abbruch eines Downloads
- **CORE-204** – Hot-Reload: Self-Reload nach jedem `saveAsset`, endlose Falsch-Änderungen bei nicht entladbaren Typen
- **CORE-301** – Interpreter: lineare Suchen und Pin-Signatur-Neuaufbau bei jedem Knoten und jeder Eingabe
- **CORE-302** – `Value` ist 280 Byte groß und wird überall per Wert kopiert
- **CORE-303** – `resolveClass` parst und verschmilzt bei jedem Spawn die ganze Klassenkette neu (auch für kompilierte Klassen)
- **CORE-401** – Pro Event/Funktionsaufruf wird ein kompletter `Context` (37 `std::function`) neu gebaut
- **CORE-402** – Lineare Knotensuche pro Event/Call (`levelHandlingEvent`, `levelWithFunction`, `functionSignatureOf`)
- **CORE-403** – Byteweiser FNV-1a als Content-Hash im Streaming-Lesepfad (664 MB/s)
- **CORE-501** – Windows `Proc::run`: Drain-Phase ohne Timeout, ein überlebender Enkelprozess hängt den Worker für immer
- **CORE-601** – `hidesChild` ist O(N) und wird je Vorfahr rekursiv gefragt (quadratisch pro Frame)
- **CORE-602** – `UIText::applyAutoSize` misst jedes Label jeden Frame neu (Default `autoSize = true`)
- **CORE-701** – Layout ist zustandslos: jede Rect-Abfrage läuft Parent-Kette + lineare Suche + Geschwister-Scans
- **CORE-702** – `topmostHit` macht pro Frame für jedes Element die teuerste Arbeit vor dem billigen Punkt-Test
- **CORE-703** – `extract` ermittelt vererbte Zustände pro Element mit separaten Parent-Wanderungen
- **CORE-704** – `graftChildRef` wendet Theme, Text-Katalog und Asset-Auflösung auf den GANZEN Baum an (pro List-Zeile)

## Zusammenfassungen der Teilberichte

**Core_1.md**: Der Slice ist insgesamt sauber, gut kommentiert und überwiegend nicht heiß: der einzige echte Frame-Loop-Code ist `Application::Run` samt `Input`/`InputMapping`/`GameLoop`. Dort gibt es keine groben Verstöße; die relevanten Funde sind (a) das string-basierte Input-API, das pro Frame, Aktion und Spieler Heap-Strings und mehrfache Hash-Lookups erzwingt (Verbraucher in `PlayerHost.cpp`), (b) eine ~700 Zeilen lange `Run()`-Funktion mit eingebettetem Diagnose-Harness, (c) die Asset-Referenz-Werkzeuge (`AssetRefRetarget`/`AssetRefScan`), die viel dupliziert und an mehreren Stellen unnötig kopieren/neu suchen, sowie (d) `GameLogicLoader::load`, das die DLL auch im ausgelieferten Spiel immer kopiert. Die Audio-Envelope (`AudioEnvelope::apply`) läuft auf dem Audio-Thread und ist bereits gut gebaut (Rampen-Raster, templatisierte Schleife); es bleibt nur ein kleiner Doppelaufwand. Alle Funde sind ohne Messdaten geschätzt und entsprechend markiert.

Funde: Hoch 0 · Mittel 9 · Niedrig 15.

---

**Core_2.md**: Der Slice (ContentManager/HAsset/EngineDependencies/TextureArrayBuild, Sha256/X25519, DebugDraw, CrashHandler/DevStatsTile/EngineProfiler/GlobalState/Log) ist insgesamt sorgfältig geschrieben: Async-Streaming nutzt Sink-`shared_ptr`, Cancel-Tokens, Zeit-Budget; Log und Profiler haben saubere Hot-Path-Guards. Die größten Schwächen sind (1) von Hand gepflegte Listen aller 26 `SlotMap`-Pools, die an vier Stellen auseinanderlaufen (echte Bugs), (2) Verzeichnis-Walks, die komplette Asset-Dateien lesen bzw. pro Datei `fs::relative` (= `weakly_canonical`) aufrufen, (3) ein Use-after-return in `EngineDeps::resolve` beim Abbruch, (4) ein 500-Zeilen-`parseAndRegisterAsset` mit mehrfachen Payload-Kopien und Material-Codegen im Ladepfad, (5) synchrones `fflush` + Konsole + Sinks unter globalem Log-Mutex. Funde: 4 Hoch, 9 Mittel, 7 Niedrig. Keine Messdaten vorhanden; Schätzungen sind als solche markiert.

---

**Core_3.md**: Der Slice ist insgesamt sehr sorgfältig kommentiert und fachlich konsistent (Semantik-Parität Interpreter/Codegen ist dokumentiert und bewusst). Die relevanten Probleme liegen im HorizonCode-Interpreter (`HorizonCode.cpp`, `HorizonCode.h`) und in der Klassen-Auflösung: Der Interpreter benutzt für jeden Knoten lineare Suchen über `links`/`nodes` und baut dabei pro Zugriff die komplette Pin-Signatur neu auf; `Value` ist 280 Byte groß und wird überall per Wert kopiert (gemessen mit g++ auf Linux: `sizeof(Value)=280`, `sizeof(Node)=336`, `sizeof(Variable)=528`, `sizeof(Context)=1128`); `resolveClass` parst und verschmilzt bei JEDEM Spawn die komplette Klassenkette neu (kein Cache, auch dann, wenn danach die kompilierte Klasse genommen wird). Diagnostics/Events/GraphCommon sind überwiegend unkritisch (Editor-/Ladepfade), mit einigen kleinen Fallstricken (Log-Makros `HE_LOG_ONCE`/`THROTTLE`, `EventBus::publish`-Snapshot, nicht-werfende Loader, die trotzdem werfen können).
Anzahl Funde: 3 Hoch, 11 Mittel, 6 Niedrig (20 gesamt). Alle Messwerte ohne Laufzeit-Profiling; Größenangaben sind echte `sizeof`-Messungen, Zeitgewinne sind Schätzungen.

---

**Core_4.md**: Der Slice ist insgesamt sorgfältig und gut kommentiert; die Parallelitätslogik im JobSystem (CAS-Claim, Cancel, Dependencies) ist korrekt durchdacht und enthält keinen offensichtlichen Fehler. Die relevanten Befunde liegen an drei Stellen. (1) Der HorizonCode-Interpreter baut pro Event und pro Funktionsaufruf einen kompletten `Context` (37 `std::function`-Member) und sucht den behandelnden Level linear durch alle Knoten. Das läuft für `Tick` jeder Widget-, Entity- und Player-Instanz in jedem Frame. (2) Der Pak-Lesepfad hasht jeden gelesenen Eintrag mit byteweisem FNV-1a. Gemessen sind das 664 MB/s und damit der langsamste Schritt, noch vor Entschlüsseln und LZ4. (3) Der Pak-Writer/Exporter arbeitet strikt sequenziell, hält das gesamte Projekt im RAM und kopiert jedes Asset mehrfach. Das ist Editor/Export-Zeit, kein Frame-Pfad. Dazu kommen Duplikate und Vereinfachungspotenzial (MTRL-Layout dreifach, Dispatch-Signaturprüfung doppelt, Öffentlich-Variable-Prüfung fünffach).

Funde: **3 Hoch, 12 Mittel, 11 Niedrig** (26 gesamt). Verifiziert wurde nur durch Lesen. Einzige Messung ist der FNV-Durchsatz (g++ -O2, 4 Kerne, 256 MiB: 664 MB/s). Alle anderen Aufwandszahlen sind Schätzungen und entsprechend markiert.

---

**Core_5.md**: Der Slice (33 Dateien, ca. 11 000 Zeilen) ist insgesamt sauber, gut kommentiert und defensiv geschrieben. Die meisten Strukturen sind bereits sinnvoll geteilt (GraphCommon, X-Makro für die Grafikfelder, ein gemeinsamer Notify-Helfer). Die relevanten Funde liegen an drei Stellen. Erstens das UI-Text-Pipeline in `UIFont.cpp`, das pro Label und Frame viele Heap-Allokationen und quadratische Messungen macht und 224 Byte pro Glyph erzeugt. Zweitens `Process.cpp` auf Windows, das den Vertrag aus `Process.h` bricht (Timeout, Callback-Thread). Drittens die Material-/Partikel-Auswertung ohne Memoisierung. Zusätzlich ein belegter Logikfehler in `isPathWithin` (Wurzel mit abschließendem Separator). Gefunden: 1 Hoch, 11 Mittel, 10 Niedrig. Messdaten liegen keine vor; alle Performance-Aussagen sind Schätzungen aus dem Code und als solche gekennzeichnet.

---

**Core_6.md**: Der Slice (20 Dateien, ~11 000 Zeilen) ist insgesamt sauber und sehr gut kommentiert; die Property-Tabellen (`uiprop::slot`), die „eine Rechnung für Zeichnen und Hit-Test"-Helfer und die kleinen Parser (Shortcut, Ease, Binding) sind einfach und korrekt. Die relevanten Probleme liegen an drei Stellen: (1) Frame-Loop-Kosten im UI-Baum (`hidesChild` ist O(N) und wird über die Vorfahrenkette rekursiv gefragt → quadratisch; `UIText::applyAutoSize` misst jedes Label jeden Frame neu), (2) die doppelt/dreifach geführte Liste der Basis-Properties samt String-If-Kette auf dem Animationspfad, (3) Robustheit und Kopien in den JSON-Pfaden (TypeRegistry, Theme, Sequence, UIElement::readJson). Zusätzlich ein echter Logikfehler (Zyklusschutz in `fieldDefault` wird für Struct-Arrays umgangen). Funde: 2 Hoch, 5 Mittel, 11 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten und als solche gekennzeichnet.

---

**Core_7.md**: Der Slice ist funktional sauber und sehr gut kommentiert; `UIWindowFrame.*` und `UIWidgetBinding.h` sind unauffällig (reine Geometrie bzw. drei Deklarationen, keine Funde). Das Hauptproblem ist architektonisch: Layout und alle vererbten Zustände (Rect, Sichtbarkeit, Enabled, Opacity, Rotation, Clip, Tiefe/Sortier-Key) sind bewusst *zustandslos* und werden pro Element über Parent-Ketten mit linearem `tree.find()` neu berechnet. Jede Frame-Schleife (`extract`, `topmostHit`, `uiApplyAutoSize`, `processPointer`) ruft das für jedes Element auf, was grob O(n²·Tiefe) pro Frame ergibt. Dazu kommen List-Zeilen-Grafting (`graftChildRef`), das pro Zeile den ganzen Baum neu themed/übersetzt/Assets auflöst, und mehrere pro-Frame-Vollscans mit `dynamic_cast`. Funde: 4 Hoch, 6 Mittel, 9 Niedrig. Alle Aussagen sind statisch gelesen, ohne Messdaten; wo Größenordnungen geschätzt sind, ist das markiert.

**Core_8.md**: Der Slice besteht aus dem Header der Widget-Verwaltung (1441 Zeilen, fast nur Deklarationen und Kommentare), der Window-Implementierung (521 Zeilen) und dem Window-Header. Der Code ist sauber und gut begründet kommentiert; die Window-Datei enthält keinen Hot-Path-Code außer `PollEvents`, `IsInBackground` und dem Hit-Test-Callback, alle drei sind unkritisch. Die relevanten Befunde betreffen (1) die Header-Abhängigkeiten von `WidgetManager.h` und `Window.h` (Compile-Zeit), (2) die Datenhaltung `std::vector<Instance>` mit linearer Suche und (3) duplizierte Env-Parsing-Logik in `Window.cpp`. Gesamt: 0 Hoch, 3 Mittel, 5 Niedrig. Alle Performance-Aussagen sind ohne Messdaten Schätzungen. Die Implementierung `WidgetManager.cpp` liegt nicht in diesem Slice; Aussagen dazu sind nur Stichproben (grep) und als solche markiert.

## Cross-Modul-Hinweise

**Core_1.md**

- `src/HE_Scene/src/PlayerHost.cpp:388,409,410`: Event-Namen werden pro Frame, Aktion und Spieler neu gebaut (siehe CORE-101); `PlayerHost.cpp:40-44` ruft drei JSON-Parses je Aktion (CORE-115).
- `src/HE_Scene/src/AudioEngine.cpp:820-821`: `std::make_shared<const HE::AudioEnvelope>(*envelope)` kopiert pro Stimmenstart den Punkte-Vektor (Heap). Gehört zu Audio-Slice; ggf. `shared_ptr` direkt aus dem Asset teilen statt kopieren.
- `src/HE_Rendering/src/RenderExtractor.cpp:263`: `meshSectionsOf` per Wert im Extraktor (CORE-108).
- `src/HE_Core/src/ContentManager/ContentManager.cpp:2928`: UI-thread-Aufruf von `retargetTree` (CORE-102).
- `src/HE_Net/src/PortMapper.cpp:56`: dritte `xmlEscape`-Kopie (CORE-117).
- `Application.cpp:788` ruft `m_loop.tick` jedes Frames, auch ohne Spiellogik-Modul (billig, nur Beobachtung); Window-Event-Callback (`Application.cpp:290-326`) ist ein `std::function` pro Event, bei Maus-Events mit drei aufeinanderfolgenden `switch` (`ProcessMouseEvent`, `ProcessGamepadEvent`, `ProcessEvent`): nicht relevant, nur zur Information.

**Core_2.md**

- `src/HE_Scene/src/PlayerHost.cpp:37,57,80` ruft `ContentManager::discoverAssets` dreimal beim Spielstart (siehe CORE-202); `src/HE_Core/src/Types/TypeRegistry.cpp:704-706` zweimal. Eine gemeinsame, gecachte Typ-Liste würde die Wiederholung vermeiden.
- `src/HE_Editor/EditorApplication.cpp:3430-3500`: Reaktion auf `pollHotReload`: bei Textur-Änderung `InvalidateMaterial` für **jedes** Material – bei Self-Reload nach Save (CORE-204) unnötig teuer.
- `src/HE_Core/include/ContentManager/AssetRefRetarget.h` (nicht im Slice) wird mit denselben Pool-Listen konsistent sein müssen, falls dort ähnliche Aufzählungen existieren.
- `Hpak/HpakWriter.cpp` MTRL-Branch hängt am Feldlayout in `encodeAssetChunks` (Kommentar `ContentManager.cpp:1992-1997`); Änderungen an CORE-208(b) betreffen ihn.
- `Diagnostics/ProfilerStats.h` (`aggregateScopes`, `findHitches`, `computePercentiles`) wird in `dumpNow` für den gesamten Capture auf dem Frame-Thread berechnet – Teil des Stalls aus CORE-211, nicht gelesen.
- `Hpak/HpakReader` (`openShared`, `readEntry`) bestimmt die Wirkung von CORE-216; liegt außerhalb dieses Slice.

**Core_3.md**

- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:76-77` (Slice 4): `Inst::levels` ist `std::vector<Graph>` je Instanz (tiefe Kopie, `HorizonCodeRuntime.h:651`) – mit CORE-303 auf geteilte, immutable Graphen umstellen.
- `src/HE_Scene/src/EntityHost.cpp:110-114`, `src/HE_Scene/src/PlayerHost.cpp:91`, `src/HE_Game/src/GameApplication.cpp:1190,1237`: `resolveClassAsset` pro Spawn auch dann, wenn danach `compiledClasses().create()` gewinnt.
- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1565,1633,1694,2087,2114`: `Runner` + `makeContext(...)` pro Event/Fire (CORE-308); Makro-erzeugte `fireOn*`-Funktionen (`~1712-1730`) übergeben den Eventnamen als `const char*` an `runEventOnLevel(const std::string&)` → temporäres `std::string` pro Aufruf.
- `src/HE_Core/include/Application/InputAssets.h:15-17`: `inputEventPressed/Released/Axis` geben `std::string` per Wert zurück und werden in `inputActionEventNames` pro Fire gebaut (CORE-307).
- `src/HE_Tools/hc_codegen` (Codegen): Move-Semantik für `hc::arrAdd/setAdd/mapSet` (CORE-309) und typisierte `collectRefs` je Slot (CORE-310) wären dort zu erzeugen.
- `UIWidgetBinding.cpp` (`uiHcValueToProp`): dritte Kopie der Coerce-Regel neben Interpreter und `hc::coerce*` – bewusst dokumentiert, aber bei jeder Typregel-Änderung dreifach zu pflegen. Eine gemeinsame, `constexpr`-fähige Konvertierungsmatrix (`scalarConvert(srcType, dstType)`) in HE_Core würde die drei Stellen verbinden.

**Core_4.md**

- `src/HE_Core/include/HorizonCode/HorizonCode.h:1463` – `Runner(const Graph&, Context ctx)` nimmt den Context per Wert; Voraussetzung für CORE-401 (`const Context&`/`shared_ptr`). `Context` hat ca. 37 `std::function`-Felder (`:1213`).
- `src/HE_Core/src/UIWidget/WidgetManager.cpp:2935`, `:3425` – ruft `eventBindingsOf` pro Element der Treffer-Kette (siehe CORE-405). Nach Einführung des Caches ggf. `listensTo(...)` nutzen.
- `src/HE_Scene/src/Net/PropertyReplicator.cpp:104,138,164,203,216` – `replicatedVariablesOf` pro Entity und Tick, teils nur zur Mitgliedschaftsprüfung (CORE-405).
- `src/HE_Core/include/ContentManager/HAsset.h:437-477` – `Reader::openData` kopiert alle Chunk-Payloads (CORE-411). Eine nicht kopierende `peekChunk`/`peekAssetType`-API wäre auch für `ContentManager` nützlich (jeder Typ-Sniff).
- `istreambuf_iterator` als Datei-Leser steht in mindestens 22 Dateien (u. a. `ContentManager.cpp`, `AssetRefScan.cpp`, `AssetRefRetarget.cpp`, `SceneSerializer.cpp`, `EngineApi.cpp`, `GameApplication.cpp`, `ExportDialogPanel.cpp`). Ein gemeinsamer `readFileBytes` in HE_Core wäre eine Querschnittsmaßnahme.
- `src/HE_Core/include/Hpak/HpakFormat.h` bindet `<functional>`, `<string>`, `<vector>` ein, weil `PackSettings` dort lebt. `HpakReader.h` braucht davon nur `EntryDesc`/`hash64`; `PackSettings` in `HpakWriter.h` auszulagern entlastet den Spiel-Runtime-Build (Runtime-Reader-Include-Kette).
- `src/HE_Game/src/GameApplication.cpp:794` – `IntegrityProbe::start` ohne Priorität (CORE-424).

**Core_5.md**

- `src/HE_Rendering/src/RenderExtractor.cpp:466, 614, 636, 798`: `AABB::transformed` pro Objekt und Frame (CORE-512); dort außerdem prüfen, ob für statische Objekte (Transform unverändert) die Weltbox gecacht werden kann (Dirty-Flag am Transform) – das wäre der größere Hebel als die Methode selbst.
- `src/HE_Core/src/UIWidget/WidgetManager.cpp` / `src/HE_Scene` (UISystem::extract): Aufrufer von `emitUITextGlyphs`/`measureUIText` (43 Stellen). Klären, ob Text-Quads pro Frame neu gebaut oder bei unverändertem Text/Rect gecacht werden; davon hängt die Gewichtung von CORE-504 bis 506 ab.
- `src/HE_Scene/src/PhysicsWorld.cpp:153`: `PhysicsWorld` cacht `CollisionLayerConfig::collides` bereits in `m_matrix`; die Funktion in `CollisionLayers.cpp` ist daher nicht im Jolt-Filterpfad (kein Fund).
- `src/HE_Scene` `ScriptContext`: „Jede Instanz hört jeden Timer/Net-Event/Cheat-Report“ (Kommentar in `IScriptBackend.h:99-123`) heißt O(Instanzen) Aufrufe pro Ereignis über die virtuelle Schnittstelle; bei vielen Instanzen ohne passenden Handler wäre ein „Hat-Handler“-Bitset pro Skript (einmal bei `loadScript` ermittelt) ein Gewinn. Dort prüfen.
- `src/HE_Rendering/*Renderer*`: Die `Set…Settings`-Push-Aufrufe pro Frame (CORE-518) führen in den Backends zu Vergleichen/Uploads; prüfen, dass Änderungen (z. B. Schattenauflösung) nur bei echtem Delta Ressourcen neu anlegen.
- `src/HE_Editor/EditorApplication.cpp` (7 Stellen `generateFragment`): prüfen, ob die Shader-Generierung beim Ziehen eines Parameter-Sliders neu läuft (Parameteränderungen sollen laut Header nie neu kompilieren).
- `src/HE_Tools` / `HE_Editor` Aufrufer von `Proc::run` (GitService): Verhalten bei Windows-Timeout (CORE-501, 502) prüfen.

**Core_6.md**

- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1501-1518, 1522-1531` (`uiChildIndexOf`, `uiChildCountOf`, `uiElementEffectiveVisible`): Ursache von CORE-601; Cache/Index gehört in `UIWidgetTree` (Slice mit UIWidgetTree).
- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1239-1300` (`uiApplyAutoSize`): läuft pro Frame mit `dynamic_cast` je Element, `uiElementUnitScale` + `uiElementRect` je Element, TabBox-Labelaufbau O(TabBoxen·N); Kandidat für Dirty-Flags (nur bei geänderten Elementen/Canvas) – Zusammenhang zu CORE-602/618.
- `src/HE_Core/src/UIWidget/WidgetManager.cpp:2382-2392, 2565` (`applyClipAt`): löst jede Animations-Property pro Frame zweimal per Namen auf (vgl. CORE-603); Slot-Zeiger beim Start cachen.
- `src/HE_Core/src/UIWidget/WidgetManager.cpp:1323` (`menuItemForShortcut`): siehe CORE-611.
- `src/HE_Editor/UIEditorPanel.cpp:3398, 6365, 6687` und `McpToolsWidget.cpp:361,464,660,855`: rufen `allProperties()` (~50 `UIPropDesc` mit Strings) teils pro Frame bzw. je Element; nach CORE-603 als statische Tabelle ohne Kopie lieferbar.
- `src/HE_Editor/HcGraphHost.cpp:1249,1300,1694` und `HcEditorUtil.cpp:554-627`: `TypeRegistry::structs()/enums()` (Kopie + Sort) in Editor-Pfaden, evtl. pro Frame – siehe CORE-606.
- `src/HE_Core/src/HorizonCode/HorizonCode.cpp:1316-1344` und `HorizonCodeRuntime.cpp:817-824`: `getEnum/getStruct/makeDefaultValue` kopieren Definitionen bei Wert-Erzeugung; profitieren von CORE-606.
- `src/HE_Editor/UIEditorPanel.cpp:410`, `McpToolsType.cpp:216`, `TypeAssetPanel.cpp:411-413`: rufen `uiThemeFromJson`/`structFromJson` ohne erkennbaren try/catch – siehe CORE-605.

**Core_7.md**

- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1281-1330` (`eventBindingsOf`): allokiert pro Aufruf; Bedingung für CORE-707. Außerdem unsicher, ob `fireTick(scriptId)` (WidgetManager.cpp:2766-2774) für Instanzen ohne Tick-Event früh zurückkehrt – andernfalls teuer für jede Embed-Instanz (Listenzeilen) pro Frame.
- `src/HE_Core/include/GraphCommon/GraphModel.h:45-71` (`findNodeById`): linear; dieselbe Struktur nutzen Node-Graphen. Ein optionaler `id→index`-Index wäre dort wiederverwendbar.
- `src/HE_Rendering/src/RenderExtractor.cpp:1589`: `world.widgets().extract(...)` läuft pro Fenster/Frame; ob `extract` bei `!consumeVisualDirty()` übersprungen wird, liegt außerhalb dieses Slices (Event-getriebene Schleife in `EditorApplication`/`GameApplication`) – bei fehlendem Gate wären CORE-701/703 doppelt kritisch.
- `src/HE_Editor/EditorApplication.cpp:4188` und `src/HE_Game/src/GameApplication.cpp:2347`: rufen `processPointer` ohne Gate auf (relevant für CORE-702).

**Core_8.md**

- src/HE_Core/src/UIWidget/WidgetManager.cpp:32-47, 948-958 – lineare `find()`/`findByScript()` in `topGrabOf`/`takesInput`, die in jedem Input-Eintrittspunkt laufen (siehe CORE-802); `const_cast<WidgetManager*>(this)->find(...)` an Z. 954 und in `takesInput` (Z. 963) wegen fehlender Verwendung der const-Überladung (Z. 37 existiert bereits – die `const_cast` sind überflüssig).
- src/HE_Core/src/Application/Application.cpp:885-910 – Hintergrund-Throttle fragt `IsInBackground()` (SDL_GetWindowFlags) für Haupt- und alle Sekundärfenster jeden Frame; billig, aber `backgroundFrameInterval()` ist statisch gecacht, also korrekt. Schleife über `m_secondaryWindows` ruft `win->IsInBackground()` weiter, auch wenn `unseen` schon false ist (`unseen && ...` kurzschließt nur den Aufruf, nicht die Schleife) – vernachlässigbar.
- src/HE_Scene/include/HorizonScene/HorizonWorld.h:11 – bindet `WidgetManager.h` ein (siehe CORE-801).
- src/HE_Game/src/GameApplication.cpp:716 – Hit-Test-Lambda (CORE-808).

## Unsicher / Messung nötig

**Core_1.md**

- CORE-104/105: Durchsatz des Referenz-Scans auf realen Projekten (GB-Größe) vor/nach; der Gewinn hängt vom Anteil Binärdaten ab.
- CORE-102: tatsächliche UI-Blockierdauer beim Verschieben in einem großen Projekt (Schätzung "Sekunden" nicht gemessen).
- CORE-109: Ob STDF/ENDF/SGTP-Chunks Asset-Pfade enthalten, die ein Move umschreiben müsste (Verhaltensfrage, nicht in diesem Slice verifiziert).
- CORE-120: Kosten der SDL-Gamepad-Abfragen mit 1–4 Pads (SDL-Internas nicht gelesen).
- CORE-119: Startzeit-Anteil des Splash-Logo-Scans (Release vs. Debug).
- `MaterialAsset`/`MaterialShaderVariant` (`Assets.h:15-44,243-386`): `syncMaterialInstance` kopiert laut Kommentar (Z. 357-362) die generierten GLSL-Strings des Parents in jede Instanz; bei vielen Instanzen sind das Duplikate im RAM (Hash-gleiche Quellen). Ob `shared_ptr<const std::string>` sinnvoll wäre, hängt von Instanzanzahl ab; messen (Speicherprofil einer Szene mit vielen Material-Instanzen).
- `StaticMeshAsset`/`SkeletalMeshAsset` (`Assets.h:121-181`): lose Assets halten `vertices/normals/uvs` als SoA, gecookte nur `interleaved`; `boneIDs` als `uint32_t` (4/Vertex). Speicher/Upload-Kosten (Interleaving beim Upload, 4× breitere Joint-Indizes als nötig) nur mit konkreten Messungen bewertbar; Umbau würde das Chunk-Layout berühren (siehe `HAsset.h` Versionsregeln) und ist daher hier nur als Beobachtung vermerkt.

**Core_2.md**

- CORE-205/206: Anzahl Syscalls und Dauer von `pollHotReload` / `scanContentDirectory` bei realen Projekten (5k+ Assets); Windows-AV kann Open-Kosten dominieren.
- CORE-207: tatsächlicher Anteil der Kopien an der Main-Thread-Zeit in `pollAsyncResults` (Profiler-Scope `ContentManager::load`).
- CORE-208: Anteil von `regenerateMaterialFromGraph` an der Editor-Ladezeit; wie oft alte Codegen-Stände wirklich neu erzeugt werden müssen.
- CORE-209: Log-Volumen pro Sekunde in Ladeszenen; Wirkung von `fflush` pro Zeile auf Windows/NTFS mit Echtzeit-Scanner.
- CORE-210/211: Auswirkung von `maxFrames` und Capture-Länge auf Frame-Zeit bzw. Stop-Stall.
- CORE-213: Größe des echten EngineContent-Katalogs (Einträge pro Ordner).
- CORE-216: Kosten eines `open()` je Pak-Job.
- `setMaterialParam` (CORE-215) und `ProfileScope` (CORE-218) nur ändern, wenn Profiling sie zeigt.
- Nicht verifiziert: ob `isEngineDefaultPath`/`toContentRelativePath` im Editor pro Frame aufgerufen werden (Aufrufstellen in `src/HE_Editor/` nicht gelesen).

**Core_3.md**

- Tatsächlicher Anteil der Signatur-Neubauten (CORE-301) an der Interpreter-Laufzeit: Profil mit `HE_PROFILE_SCOPE` um `evalInput` / Instruments / perf.
- Gewinn von `Value`-Verschlankung (CORE-302 Stufe 2) vs. Migrationsaufwand: Mikro-Benchmark der `Add`-/`Branch`-Kette, 1 Mio. Läufe.
- Allokationen pro `makeContext`-Aufruf (CORE-308) hängen von der SBO-Größe der Standardbibliothek ab (libstdc++ 16 Byte, MSVC 16, libc++ 24).
- Ladezeit/Speicher großer Profiler-Captures und ob der Editor `LoadedCapture::load` im UI-Thread aufruft (CORE-314).
- Kosten von `Log::enabled` über die DLL-Grenze unter Windows (CORE-319).
- Ob `EventBus` in der Praxis in Frame-Schleifen publiziert wird (CORE-311) – im Repo nur Tests.

**Core_4.md**

- CORE-401: tatsächliche Kosten von `makeContext` pro Aufruf (Heap-Anteil hängt von der STL ab: libstdc++ SBO 16 B, libc++ 24 B, MSVC größer). Mikrobenchmark 1000 Instanzen × `fireTick`.
- CORE-404: Anzahl Bindings/Watches in realen Projekten; Kosten pro Frame heute.
- CORE-408: ob Aufrufer den `Graph` kopieren oder verschieben; Größe eines typischen Graphen; ob irgendwo `leaf()` mutiert wird.
- CORE-403: Verhältnis Hash-Zeit zu Gesamtzeit eines Streaming-Jobs auf der Zielhardware (der Wert 664 MB/s stammt von dieser 4-Kern-Testmaschine).
- CORE-409: Thread-Sicherheit der Editor-Callbacks `compileShaderVariants`/`compileParticleShaderVariants` und von `HAsset::Reader`/`Writer` (statische Zustände?).
- CORE-414: GPU-Kosten des AutoLandscape-Materials je Backend (Pass-Timer), erst danach Maßnahmen.
- `ParallelForJob`-Allokation pro `parallel_for` (`JobSystem.h:356`: `make_shared` mit Mutex + CV + `exception_ptr`, ca. 120 B pro Aufruf; zusätzlich `copies` Kopien eines `std::function` unter dem Queue-Lock, `JobSystem.cpp:573-577`) und 64 × `yield` in `parallel_for_wait` (`:715`): bei etwa 10-30 Aufrufen pro Frame vermutlich unter 10 µs, deshalb kein eigener Fund. Nur ändern, wenn der Profiler dort etwas zeigt.
- `JobState` ist schwer (Mutex, CV, drei `std::function`, zwei Vektoren, ca. 300 B). Für grobkörnige Streaming-Jobs unproblematisch, für feinkörnige `schedule`-Nutzung messen.
- Die `HpakWriter.cpp`-Annahme, dass einzelne Assets > 2 GiB nie vorkommen: `origSize`/`dataSize` sind `uint32_t` (`:579`, `:1236`), `LZ4_compressBound(int)` (`:593`). Der Reader lehnt >2 GiB ab (`HpakReader.cpp:182`), der Writer prüft es nicht. Wäre ein klarer Fehler statt stillem Abschneiden wert; Auftreten in der Praxis unbekannt.
- `HpakReader::open` lässt bei einem Fehler nach erfolgreichem `m_file.open` (Magic/Version/TOC) den Handle offen (`HpakReader.cpp:62-107`); harmlos bis zum nächsten `open` oder Destruktor, nicht geprüft, ob Aufrufer das ausnutzen.

**Core_5.md**

- Wie oft pro Frame `emitUITextGlyphs`, `measureUIText` und `uiLayoutRichText` tatsächlich laufen (CORE-504 bis 507): Frame-Profil mit einem textreichen Widget.
- Reale Kosten des 224-Byte-`UIRenderObject` im Sort/Batching (CORE-506): `perf`/Instruments auf `UISystem::extract` und Backend-UI-Pass.
- Reproduktion des Windows-Hängers (CORE-501) mit `adb start-server` oder einem eigenen Testprogramm, das einen Enkel mit geerbtem stdout startet.
- Laufzeit von `matGraphApproxSurface` bei Diamant-Graphen (CORE-503) – ob ein vorhandener Content-Graph die Fälle schon erreicht (Content-Scan über alle `.hasset`-Material-Graphen).
- Spawn-Kosten pro Lua-Instanz (CORE-509): Benchmark 5000 `createInstance` mit realistischem Skript; Kompatibilität mit `pairs(self)` in Projekt-Skripten.
- Ob `UIFontCache::keyFor` jemals von Nicht-Main-Threads erreicht wird (CORE-517).
- Gewinn von Arvo-`AABB::transformed` (CORE-512): Mikrobenchmark plus Extract-Zeit bei 10 000 Objekten.
- Kompilierzeitgewinn durch schlankeren `IRenderer.h` (CORE-518): `-ftime-trace`.

**Core_6.md**

- CORE-601: tatsächlicher Anteil an der Frame-Zeit; Benchmark mit synthetischem Baum (z.B. 2000 Elemente unter Splitter+TabBox), Aufrufzahlen von `uiElementEffectiveVisible` pro Frame zählen.
- CORE-602: Kosten von `measureUIText` je Label (Profiler über `uiApplyAutoSize` in einer realen Widget-Szene); Verhältnis Labels/Frame.
- CORE-603: ob Animationen in realen Projekten viele Tracks pro Frame ausführen; sonst nur Vereinfachungsnutzen.
- CORE-606: ob `TypeRegistry::structs()/enums()` aus Panels pro Frame aufgerufen werden (Aufruferliste prüfen) und Größe typischer Projekte.
- CORE-605: welche Aufrufer die Loader in try/catch hüllen (ContentManager-Ladepfad für `CHUNK_STDF/ENDF`, `CHUNK_THEM` wurde nicht verfolgt).
- CORE-613: ob UUID-Hash-Kollisionen bei strukturierten UUIDs auftreten; Compile-Zeit-Effekt von `<random>` messen (`-ftime-trace`).
- CORE-617: reale Größe/Länge von Property-Spuren in Sequences.

**Core_7.md**

- Reale Größenordnung von n (Elemente pro Widget-Seite) in den ausgelieferten Apps/Templates: davon hängt ab, ob CORE-701/702/703 Millisekunden oder Mikrosekunden kosten. Profil mit einer Seite mit 500+ Elementen und einer Tabelle mit 30 sichtbaren Zeilen.
- Kosten von `measureUIText` / `sharedUIFont` (CORE-711): Cache vorhanden?
- Ob alle `applyAutoSize`-Überschreibungen bei `autoSizedAxes()==0` wirkungslos sind (UIElements.h:187, 364, 840, 1993) – Voraussetzung für den Vorfilter in CORE-710.
- Ob `Runtime::fireTick` für Instanzen ohne Tick-Binding schnell zurückkehrt.
- Ob `processPointer`/`extract` in Editor und Spiel tatsächlich jeden Frame laufen oder durch `consumeVisualDirty` gegated werden (siehe Cross-Modul).
- Ob das Re-Theming des ganzen Baums bei Row-Graft (CORE-704) ein gewollter Effekt ist (z. B. um Skript-gesetzte Werte zurückzusetzen).

**Core_8.md**

- Build-Zeit-Anteil von `WidgetManager.h` und `Window.h` (`-ftime-trace`, CORE-801/804).
- Tatsächliche Instanzzahlen und Aufrufhäufigkeit von `find()`/`topGrabOf` pro Frame in einem realistischen UI-Szenario (CORE-802); `WidgetManager.cpp` ist nicht Teil dieses Slice.
- Wie oft `liveIds()` (Outliner, `WidgetManager.h:333`) pro Frame gerufen wird und ob die Allokation relevant ist.
- Ob `Instance`-Zeiger heute über `createWidget`-Aufrufe aus Skript-Callbacks gehalten werden (Relevanz für CORE-802).
- Hit-Test-Overhead auf Borderless-Fenstern (CORE-808).


---
# Funde (vollständig, nach Teilbericht)

## Core Slice 1 – AnimatorStateMachine, Application, Audio, BlendSpace, BoneMask, ContentManager (AssetRefRetarget/AssetRefScan/Assets.h)

## Zusammenfassung

Der Slice ist insgesamt sauber, gut kommentiert und überwiegend nicht heiß: der einzige echte Frame-Loop-Code ist `Application::Run` samt `Input`/`InputMapping`/`GameLoop`. Dort gibt es keine groben Verstöße; die relevanten Funde sind (a) das string-basierte Input-API, das pro Frame, Aktion und Spieler Heap-Strings und mehrfache Hash-Lookups erzwingt (Verbraucher in `PlayerHost.cpp`), (b) eine ~700 Zeilen lange `Run()`-Funktion mit eingebettetem Diagnose-Harness, (c) die Asset-Referenz-Werkzeuge (`AssetRefRetarget`/`AssetRefScan`), die viel dupliziert und an mehreren Stellen unnötig kopieren/neu suchen, sowie (d) `GameLogicLoader::load`, das die DLL auch im ausgelieferten Spiel immer kopiert. Die Audio-Envelope (`AudioEnvelope::apply`) läuft auf dem Audio-Thread und ist bereits gut gebaut (Rampen-Raster, templatisierte Schleife); es bleibt nur ein kleiner Doppelaufwand. Alle Funde sind ohne Messdaten geschätzt und entsprechend markiert.

Funde: Hoch 0 · Mittel 9 · Niedrig 15.

---

### CORE-101 – Input-API ist namensbasiert: pro Frame/Aktion/Spieler Heap-Strings und 1–3 Hash-Lookups
- **ID**: CORE-101
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Application/InputAssets.cpp:12-15`, `src/HE_Core/src/Application/InputMapping.cpp:179-220`; Verbraucher `src/HE_Scene/src/PlayerHost.cpp:388,409,410` (außerhalb des Slice)
- **Problem**: `inputEventPressed/Released/Axis/Axis2D` bauen bei jedem Aufruf `"Input." + a + ".Pressed"` neu auf. `PlayerHost` ruft sie im Per-Frame-Pump je Aktion und je Spieler (`fireInputEvent(i, HE::inputEventAxis(a.name), ...)`). Ab einem Aktionsnamen von 5 Zeichen übersteigt der String die SSO-Grenze (16 Byte inkl. "Input." + ".Axis") und allokiert. Zusätzlich löst `PlayerHost` für jede Button-Aktion `justPressed`, `justReleased`, `isPressed` einzeln auf (`getAction(name)` = je ein `unordered_map<std::string,...>::find` mit Hashing des Namens), für Achsen `axisValue`. Das Muster skaliert mit `Aktionen × Spieler × Frames`.
- **Code-Auszug**:
```cpp
std::string inputEventAxis(const std::string& a) { return "Input." + a + ".Axis"; }
...
const InputActionState* InputMapping::getAction(const std::string& name) const
{ auto it = m_actions.find(name); return it != m_actions.end() ? &it->second.state : nullptr; }
```
- **Lösung**: (1) Event-Namen einmal beim Aufbau der Aktionsliste (`PlayerHost.cpp:40-44`, dort wird pro Aktion ohnehin einmal geparst) in die Aktionsbeschreibung legen (`pressedEvent`, `releasedEvent`, `axisEvent`) und nur noch als `const std::string&` weiterreichen. (2) In `InputMapping` ein `const InputActionState* getAction(name)` einmal pro Aktion und Frame nutzen und `justPressed/justReleased/isPressed` aus demselben Zeiger lesen (`const InputActionState* s = m.getAction(a.name); pressed = s && s->justPressed; ...`). Öffentliche API bleibt unverändert, nur die Aufrufer ändern sich.
- **Erwarteter Effekt**: Schätzung: ca. 1–3 Heap-Allokationen und 2–3 Hash-Lookups weniger pro Aktion/Spieler/Frame; bei ~30 Aktionen im Bereich einiger µs/Frame, kein FPS-relevanter Gewinn, aber Wegfall von malloc-Druck im Main-Thread. Messen (PlayerHost-Tick im Profiler).
- **Risiko / Verifikation**: Gering; Event-Namen müssen byte-identisch bleiben (Editor-Katalog `HorizonCode.cpp:1095-1098` nutzt dieselben Funktionen, daher die Funktionen selbst nicht ändern). Bestehende Input-Tests (`tests/` zu InputMapping/PlayerHost) laufen lassen.

### CORE-102 – `retargetTree` blockiert den Editor-Frame: gesamter Content-Baum wird synchron gelesen
- **ID**: CORE-102
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/AssetRefRetarget.cpp:251-315` (Kommentar Zeile 261-262: "this runs inside an editor frame"); Aufrufer `ContentManager.cpp:2928`
- **Problem**: Verschieben/Umbenennen eines Assets läuft `retargetTree` auf dem UI-Thread: rekursiver Walk über alle `.hasset`, jede Datei wird per `fileMentions` komplett gestreamt (256-KB-Blöcke), bei Treffer ganz gelesen, neu geschrieben und per Rename getauscht. Bei einem Projekt mit mehreren GB Meshes/Texturen ist das ein Hitch im Sekundenbereich (Schätzung, nicht gemessen). Das Gegenstück `AssetRefScan::findReferrers` ist ausdrücklich worker-tauglich (`AssetRefScan.h:113-116`) und hat Abbruch-Callback; `retargetTree` hat beides nicht.
- **Code-Auszug**:
```cpp
for (; !ec && it != end; it.increment(ec))
{ ...
  if (!fileMentions(it->path(), rules)) continue;   // liest ggf. die ganze Datei
```
- **Lösung**: Entweder (a) auf einen Worker/Job verlagern und das Ergebnis marshallen (analog `findReferrers`, plus `isCancelled`), oder (b) die Kandidatenmenge vorab über denselben Streaming-Gate-Pass wie `findReferrers` einschränken und nur Treffer umschreiben. Mindestens: Fortschritt/Busy-Zustand anzeigen, damit der Hitch erklärt ist. Verhalten (Temp+Rename-Atomarität) beibehalten.
- **Erwarteter Effekt**: Schätzung: UI-Hitch entfällt; Gesamtzeit unverändert (I/O-gebunden). Messen mit großem Content-Ordner.
- **Risiko / Verifikation**: Mittel – Thread-Sicherheit von `ContentManager`-Folgeschritten (`retargetAssetReferencesInMemory`, Manifest-Update) beachten; Reihenfolge Disk-Rewrite → In-Memory-Update muss bleiben. Bestehende Tests zu `AssetRefs` (`tests/` nach `retarget` durchsuchen) erweitern.

### CORE-103 – `retargetBlob`/`retargetTree` kopieren jeden Chunk mehrfach, auch unveränderte; Datei wird zeichenweise gelesen
- **ID**: CORE-103
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `AssetRefRetarget.cpp:222-249` (insb. 231, 244), `:273-278`; analog `AssetRefScan.cpp:474-480`
- **Problem**: Für jede Datei, die einen Pfad erwähnt (z. B. jedes Mesh mit MREF auf das verschobene Material), liegen gleichzeitig vor: der Datei-`blob`, die vom `Reader` kopierten Chunk-Payloads, die Kopie `std::vector<uint8_t> data = chunk.data;` pro Chunk (auch für Vertex-/Pixel-Chunks, die nie geändert werden), die Kopie im `Writer::addChunk` und das Ergebnis von `toBytes`. Das sind 4–5× die Dateigröße. Zudem liest `blob.assign(std::istreambuf_iterator<char>(f), ...)` (und `readWhole` in `AssetRefScan.cpp:478`) Byte für Byte über den Streambuf; das ist deutlich langsamer als ein `read` in einen vorab dimensionierten Puffer.
- **Code-Auszug**:
```cpp
for (const auto& chunk : reader.chunks())
{
    std::vector<uint8_t> data = chunk.data;      // Kopie, auch wenn nichts geändert wird
    ...
    writer.addChunk(chunk.id, data.data(), data.size());
}
...
blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
```
- **Lösung**: Chunk nur bei Treffer kopieren (`const std::vector<uint8_t>* src = &chunk.data; std::vector<uint8_t> changedCopy; ... writer.addChunk(id, src->data(), src->size())`). Datei per `seekg(0,end)/tellg/read` in `blob.resize(size)` lesen (gemeinsamer Helfer `readFileBytes` für Retarget und Scan, siehe CORE-109).
- **Erwarteter Effekt**: Schätzung: Spitzen-RAM für große Assets um ~50 % kleiner, Lesen mehrfach schneller; nur bei Move/Delete-Dialog relevant.
- **Risiko / Verifikation**: Gering; Ausgabe muss byte-identisch bleiben (Test: Blob mit Mesh+MREF vor/nach vergleichen, außer dem geänderten Chunk).

### CORE-104 – Digit-Run-Gate erzeugt für jeden Ziffernlauf in Binärdaten einen `std::string` und hasht ihn
- **ID**: CORE-104
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/AssetRefScan.cpp:167-191` (`digitRunMatches`, `blockMentions`), ebenso `nextHit` `:412-421`
- **Problem**: Das Gate scannt jede Projektdatei (bis GB) byte-weise; bei UUID-Zielen wird für jeden Lauf aufeinanderfolgender Bytes `'0'..'9'` ein `std::string(hay.substr(...))` gebaut und per `unordered_set::count` gesucht. In Binärdaten (Vertexpuffer, Pixel) sind ~4 % der Bytes Ziffern, also startet etwa jedes 25. Byte einen Lauf mit Konstruktion plus Hash. Der Kommentar (Z. 167-170) nennt selbst, dass ein `hi` 19–20 Ziffern lang ist; Läufe anderer Länge können nie treffen.
- **Code-Auszug**:
```cpp
const std::size_t len = i - start;
if (len == 0) return false;
return q.uuidDigits.count(std::string(hay.substr(start, len))) > 0;
```
- **Lösung**: In `compile()` `minDigits/maxDigits` über `uuidDigits` merken und in `digitRunMatches` sofort `return false`, wenn `len < minDigits || len > maxDigits`; Lookup nur danach (mit `std::string_view`-Heterogenous-Lookup oder C++20 `unordered_set::contains` mit transparentem Hash, sonst ein auf dem Stack gehaltener Puffer). Ergebnisse bleiben identisch, weil kürzere/längere Läufe nie in der Menge sind.
- **Erwarteter Effekt**: Schätzung: Gate-Durchsatz auf Binärdaten spürbar höher (Hashing/Konstruktion entfällt für ~alle Läufe); Größenordnung 2–5× auf Mesh/Textur-Daten, zu messen (z. B. 1 GB Content, Delete-Dialog eines Ordners).
- **Risiko / Verifikation**: Gering; `tests/` zu `findReferrers` (UUID-Treffer in Scene/ASMG/PTGR/PFAB) müssen unverändert grün sein.

### CORE-105 – `nextHit` sucht bei jedem Treffer alle Needles ab `start` neu (potenziell quadratisch)
- **ID**: CORE-105
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `AssetRefRetarget.cpp:108-119` (genutzt in `:130-166`), `AssetRefScan.cpp:403-423` (genutzt in `:434-466`)
- **Problem**: Jeder Aufruf führt für jede Regel/jeden Needle `hay.find(needle, start)` aus. Hat eine Regel keinen oder nur einen weit entfernten Treffer, wird der Rest des Chunks bei jedem Treffer der anderen Regel erneut durchsucht: O(Treffer × Chunkgröße). `moveRules` erzeugt systematisch zwei Regeln, deren zweite (`Content/<rel>`) die erste als Teilstring enthält; jede Fundstelle der zweiten ist auch eine der ersten (Offset +8), die Suchen verdoppeln sich also ohne Not. In `AssetRefScan::nextHit` kommt der Ziffernscan bis `best` hinzu, der pro Aufruf wieder von `start` läuft.
- **Code-Auszug**:
```cpp
for (const Rule& r : rules)
{
    if (r.from.empty()) continue;
    const size_t p = hay.find(r.from, start);
    if (p < best) best = p;
}
```
- **Lösung**: Pro Regel die zuletzt gefundene Position cachen (`std::vector<size_t> next`) und nur neu suchen, wenn `next[i] < start`. Bei Scan zusätzlich die Position des nächsten UUID-Treffers cachen. Ergebnis (kleinster Offset) bleibt gleich.
- **Erwarteter Effekt**: Schätzung: bei Chunks mit vielen Treffern von O(n·k) zu O(n); in der Praxis selten extrem, daher nur Mittel. Messen mit einem MTRL-Chunk mit großem Node-Graph-JSON und vielen Pfaden.
- **Risiko / Verifikation**: Gering; Test mit Chunks, die Treffer beider Regeln mischen.

### CORE-106 – `Application::Run` ist ~700 Zeilen lang; Diagnose-Harness steckt inline im Frame-Loop
- **ID**: CORE-106
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/Application.cpp:231-934`; Harness-Blöcke `:448-473` (HE_EXIT_AFTER_FRAMES), `:475-548` (HE_PROFILE_CAPTURE), `:550-594` (HE_CAPTURE_FRAME), `:625-680` und `:686-690` (HE_PERF_INPUT_EVENTS), `:47-172` (InputLatencyProbe)
- **Problem**: Der Haupt-Loop mischt Fensterverwaltung, Event-Driven-Scheduling, Profiler, Hitch-Detektor, drei env-gesteuerte Test-/Perf-Hilfen (je mit eigenem `static const` Lambda-Parse und eigenem Zustand), GPU-Stats-Abholung und Frame-Cap/Background-Throttle. Das sind ~250 Zeilen reiner Diagnosecode im Hot-Path-Rumpf, die mit jedem Frame durchlaufen werden (zwar billig: Static-Guard-Check, Vergleich), aber den eigentlichen Kontrollfluss (`runThisFrame`/`present`) schwer lesbar machen. Dazu eine 125-Zeilen-`InputLatencyProbe`-Klasse im selben File.
- **Code-Auszug**:
```cpp
{
    static const unsigned long long kExitAfter = []() -> unsigned long long { ... getenv ... }();
    if (kExitAfter != 0 && m_frameIndex >= kExitAfter) { ...; m_running = false; }
}
{
    struct AutoCapture { ... };
    static const AutoCapture kAuto = [] { ... }();
    if (kAuto.frames != 0) { ... }
}
```
- **Lösung**: Neue interne Klasse (z. B. `Application/FrameDiagnostics.{h,cpp}`) mit `FrameDiagnostics::fromEnvironment()` (liest alle `HE_*` einmal im Konstruktor) und Methoden `beginFrame(Application&, frameIndex)`, `injectSyntheticInput(Window&)`, `onCommitted()`. `InputLatencyProbe` dorthin verschieben. `Run()` zerfällt in `initWindowAndRenderer()`, `runFrame()` (Entscheidung ob/was gezeichnet wird), `collectProfilerStats()`, `throttle()`. Verhalten unverändert; nur Extraktion.
- **Erwarteter Effekt**: Reine Lesbarkeit/Wartbarkeit; Laufzeit unverändert (minus ein paar Branch-Checks pro Frame, nicht messbar).
- **Risiko / Verifikation**: Gering-mittel (Reihenfolge der Blöcke im Frame ist semantisch: Capture VOR dem Zeichnen, Profiler `beginFrame` nach Hitch-Check, `WaitForFrame` vor `PollEvents`). Verifikation: `HE_EXIT_AFTER_FRAMES`, `HE_PROFILE_CAPTURE`, `HE_CAPTURE_FRAME` Läufe vor/nach vergleichen (Exit-Code, Dump, PPM).

### CORE-107 – `GameLogicLoader::load` kopiert die DLL immer, auch im ausgelieferten Spiel
- **ID**: CORE-107
- **Kategorie**: E / A
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Application/GameLogicLoader.cpp:24-43`, Header-Kommentar `GameLogicLoader.h:13-15`
- **Problem**: Der Header sagt "In packaged game: load once, never reload". `load()` legt trotzdem bei jedem Start eine `GameLogic.hot-0001.<ext>`-Kopie neben der Original-DLL an (`copy_file`, `overwrite_existing`). Im Spiel ist das unnötiger Start-I/O und in nur lesbaren Installationsverzeichnissen ein stiller Fehlschlag (`if (!ec) loadPath = copy;` – Fallback greift, aber jedes Mal ein fehlgeschlagener Versuch); bei hartem Prozessende bleibt die Kopie liegen (Entfernung nur in `unload`). Auf macOS/Windows ist die Kopie im Editor nötig (dyld-Cache, Dateilock), im Spiel nicht.
- **Code-Auszug**:
```cpp
static unsigned s_loadCounter = 0;
std::filesystem::path loadPath = dllPath;
{
    ++s_loadCounter;
    ...
    std::filesystem::copy_file(dllPath, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) loadPath = copy;
}
```
- **Lösung**: Ladeoption (`enum class LoadMode { Direct, HotCopy }`, Default `Direct`) einführen und nur `reload`/`reloadAndStart` sowie den Editor-Erstladeaufruf mit `HotCopy` rufen. `s_loadCounter` dabei als `std::atomic<unsigned>` oder in die Instanz verlegen (statische, nicht thread-sichere Variable, falls je mehrere Loader).
- **Erwarteter Effekt**: Schätzung: Spielstart spart eine Dateikopie (Größe der Game-DLL, typ. wenige MB); keine Restdateien nach Crash.
- **Risiko / Verifikation**: Mittel – Editor muss weiter kopieren (Windows-Lock). Aufrufer von `load/loadAndStart` im Editor (`grep loadAndStart src/HE_Editor`) prüfen; Hot-Reload-Test im Editor manuell.

### CORE-108 – `meshSectionsOf` gibt die Section-Tabelle per Wert zurück (Heap-Kopie pro Aufruf)
- **ID**: CORE-108
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/ContentManager/Assets.h:191-201`; Hot-Path-Aufrufer `src/HE_Rendering/src/RenderExtractor.cpp:263` (`lod0SectionsOf`)
- **Problem**: Die Funktion kopiert `mesh.sections` (Vektor mit je einem `std::string materialPath`) bzw. baut für leere Tabellen ein temporäres Ein-Element-Vektor-Objekt (Heap). Im Extractor läuft sie in `extractMeshes` für Entitäten, deren gezeichnetes LOD nicht LOD0 ist, also potenziell pro Entität und Frame; der Kommentar dort sagt "A copy" bewusst (Zeigerstabilität), die Kopie geht aber über die Strings.
- **Code-Auszug**:
```cpp
template<typename Mesh>
std::vector<MeshSection> meshSectionsOf(const Mesh& mesh)
{
    if (!mesh.sections.empty()) return mesh.sections;
    MeshSection whole; ...
    return { whole };
}
```
- **Lösung**: Für den Extractor ist nur `materialId`/`materialPath` pro Slot nötig (`lod0Material(slot)`): LOD0-Material-Ids je LOD0-Mesh einmal pro Frame cachen (Map `lod0 meshId → vector<UUID>`), oder eine Variante `forEachMeshSection(mesh, fn)` anbieten, die die Tabelle ohne Kopie durchläuft. Die by-value-Variante für Inspector bleibt.
- **Erwarteter Effekt**: Schätzung: pro betroffener Entität/Frame 1–N Allokationen weniger; Gewinn nur bei vielen Entitäten mit nicht-LOD0-Darstellung. Messen mit LOD-Stresszene.
- **Risiko / Verifikation**: Gering-mittel; Slot-Mapping (`lodSlotMap`) darf sich nicht ändern. RenderExtractor-/LOD-Tests ausführen.

### CORE-109 – Retarget und Scan duplizieren Streaming-Gate, Chunk-Klassifikation und Binär-Walker; Chunk-Listen sind bereits auseinandergelaufen
- **ID**: CORE-109
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `AssetRefRetarget.cpp:32-37, 39, 61-76, 81-105, 108-119` vs. `AssetRefScan.cpp:29-36, 75, 204-248, 373-401, 403-423`
- **Problem**: `fileMentions` ≙ `gateFile`, `isJsonChunk` ≙ `isJsonPayloadChunk`, `readU32`, `enclosingJsonAt` und `nextHit` existieren zweimal mit leichten Abweichungen (Scan hat Lookback-Cap und `gaveUpAtCap`). Die Chunk-Listen divergieren bereits: `isJsonPayloadChunk` führt `CHUNK_STDF/ENDF/SGTP`, `isJsonChunk` nicht (der Scan-Kommentar `AssetRefScan.cpp:24-28` nennt das selbst). Wird ein Asset verschoben, das in einem dieser Chunk-Typen steht, behandelt der Retarget-Pfad es als Binärchunk; ob dort Pfade stehen, ist hier nicht verifiziert (siehe "Unsicher"). Jede künftige Chunk-Änderung muss an zwei Stellen nachgezogen werden.
- **Code-Auszug**:
```cpp
// Retarget
return id == HAsset::CHUNK_MGRF || id == HAsset::CHUNK_UIWT || ... || id == HAsset::CHUNK_SEQU;
// Scan
return id == HAsset::CHUNK_MGRF || ... || id == HAsset::CHUNK_STDF || id == HAsset::CHUNK_ENDF || id == HAsset::CHUNK_SGTP || id == HAsset::CHUNK_SEQU;
```
- **Lösung**: Internen Header `ContentManager/AssetRefChunks.h` (nicht `HE_API`) mit `isJsonPayloadChunk`, `readU32`, `enclosingJsonAt(…, maxLookback)` und einer gemeinsamen `streamContains(file, needles)`-Funktion; beide Dateien nutzen sie. Entscheidung treffen, ob STDF/ENDF/SGTP im Retarget als JSON zu behandeln sind (Test!). Oder, wenn bewusst anders: im gemeinsamen Header dokumentieren.
- **Erwarteter Effekt**: ~120 Zeilen weniger, ein Ort für Chunk-Klassifikation; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Mittel, wenn die Klassifikation vereinheitlicht wird (Verhaltensänderung für STDF/ENDF/SGTP). Test: Struct-/Enum-/SaveGame-Asset mit Pfadreferenz verschieben und Re-Scan.

### CORE-110 – `GameLoop::tick`: redundantes `if (logic)`
- **ID**: CORE-110
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/GameLoop.cpp:21,26`
- **Problem**: Nach dem Early-Return `if (!logic) { ...; return m_running; }` (Z. 21) ist `logic` garantiert nicht null; die Prüfung in der Schleife (Z. 26) ist tot.
- **Code-Auszug**:
```cpp
if (!logic) { m_accumulator = 0.0f; return m_running; }
...
if (logic) logic->onUpdate(world, m_config.fixedTimestep);
```
- **Lösung**: Zeile 26 zu `logic->onUpdate(world, m_config.fixedTimestep);`.
- **Erwarteter Effekt**: Nur Lesbarkeit.
- **Risiko / Verifikation**: Keines; `tests/` zu GameLoop.

### CORE-111 – Profiler-Capture: pro Frame drei Vektor-Allokationen/-Kopien der GPU-Pass-Liste
- **ID**: CORE-111
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `Application.cpp:809-830` (`gs = m_renderer->GetFrameGpuStats()`, `std::vector<ProfGpuPass> passes`), `EngineProfiler.cpp:458-465` (`m_current.gpuPasses = passes`)
- **Problem**: Während einer Aufnahme (oder bei offenem Live-HUD) wird pro Frame `FrameGpuStats` samt `passes`-Vektor zurückgegeben, dann in einen zweiten `ProfGpuPass`-Vektor (mit `reserve`) umkopiert und beim Setzen nochmals kopiert. Das stört genau die Messung, für die es gedacht ist (kleine, konstante Allokationslast im Capture).
- **Code-Auszug**:
```cpp
std::vector<ProfGpuPass> passes;
passes.reserve(gs.passes.size());
for (const auto& p : gs.passes) passes.push_back({ p.name, p.ms, p.approx });
profiler.setGpuTimes(gs.gpuFrameMs, passes, gs.gpuTimingMode);
```
- **Lösung**: Den `passes`-Vektor als Member von `Application` wiederverwenden (`m_profPassScratch.clear(); ...`) und `setGpuTimes` per `std::vector<ProfGpuPass>&&`/Span übernehmen lassen, oder `GpuPassTime` und `ProfGpuPass` auf einen Typ vereinheitlichen.
- **Erwarteter Effekt**: Schätzung: 2 Allokationen/Frame weniger nur während Capture; im µs-Bereich.
- **Risiko / Verifikation**: Gering; Profiler-Dump-Test.

### CORE-112 – `AudioEnvelope`: jeder Raster-Gain-Punkt wird pro Block doppelt ausgewertet
- **ID**: CORE-112
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Audio/AudioEdit.cpp:209-235` (Aufrufer auf dem Audio-Thread: `HE_Scene/src/AudioEngine.cpp:287` `curve->apply`)
- **Problem**: `rampBlock(env, block, sr)` wertet `g0 = evalGain(block*128)` und `g1 = evalGain((block+1)*128)` aus (je ein `upper_bound`, bei Exponential zusätzlich 2× `log10` + `pow`). Der nächste Block berechnet sein `g0` identisch zum vorigen `g1` neu. Die Schleife in `forEachRampedGain` kann `g1` mitnehmen. Pro 128 Frames ist das ein Binärsuch-/ggf. Transzendental-Aufruf zu viel; der Gain-Pfad pro Frame (Multiplikation, Konversion) ist deutlich teurer, also Niedrig. (Die Division durch `kRampFrames` ist eine Zweierpotenz-Konstante und wird vom Compiler exakt zur Multiplikation, daher kein Fund.)
- **Code-Auszug**:
```cpp
const double g0 = env.evalGain(static_cast<double>(f0) / sampleRate);
const double g1 = env.evalGain(static_cast<double>(f0 + AudioEnvelope::kRampFrames) / sampleRate);
return RampBlock{ g0, g1 - g0 };
```
- **Lösung**: In `forEachRampedGain` `g1` des vorigen Blocks als `g0` des nächsten wiederverwenden (innerhalb eines Aufrufs; über Aufrufe hinweg bleibt die Funktion rein, wie dokumentiert). Das Ergebnis ist bit-identisch, solange `evalGain` deterministisch ist.
- **Erwarteter Effekt**: Schätzung: ~50 % der `evalGain`-Aufrufe pro `apply`; im Verhältnis zur Frame-Schleife wenige Prozent. Messen auf Audio-Thread nur bei vielen Stimmen mit Kurve.
- **Risiko / Verifikation**: Gering; `tests/test_audio_edit.cpp` (bit-gleiche Gains für `rampedGain` vs. `apply` bleiben Pflicht – `rampedGain` selbst weiter auf `rampBlock` stützen).

### CORE-113 – `retargetJsonText` baut für jedes JSON-String-Literal eine neue `std::string`-Kopie
- **ID**: CORE-113
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AssetRefRetarget.cpp:189-220`
- **Problem**: Der Scanner kopiert Zeichen für Zeichen jeden String-Body in `raw` (Heap ab 16 Zeichen), um ihn mit den Regeln zu vergleichen, und hängt ihn danach unverändert wieder an `out`. Bei MB-großen Node-Graph-JSONs entstehen so Zehntausende Allokationen, obwohl nur ein Bruchteil trifft.
- **Code-Auszug**:
```cpp
std::string raw;
...
raw += text[j++];
...
if (retargetValue(raw, rules)) changed = true;
out += '"'; out += raw; out += '"';
```
- **Lösung**: Body als `std::string_view` (Anfang/Ende) ermitteln, `retargetValue` auf `string_view` anbieten (Vergleich + Rückgabe von `optional<string>` nur bei Treffer) und unveränderte Bereiche per `out.append(text, i, j - i + 1)` blockweise kopieren.
- **Erwarteter Effekt**: Schätzung: wenige Allokationen statt einer pro String; nur beim Move relevant.
- **Risiko / Verifikation**: Gering; Ausgabe muss byte-identisch bleiben (inkl. Escapes).

### CORE-114 – `value(key, json::array())` kopiert das ganze Array bei jedem Laden
- **ID**: CORE-114
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AnimatorStateMachineGraph.cpp:86,93`; `BlendSpace.cpp:50`; `BoneMask.cpp:30`; Zusatz `AnimatorStateMachineGraph.cpp:65-69`
- **Problem**: `nlohmann::json::value(key, default)` liefert für den Typ `json` eine Kopie des gesamten Teilbaums; die Range-for iteriert dann über die Kopie. Beim Laden jedes Assets wird das States-/Transitions-/Samples-Array einmal tief kopiert. Beim Schreiben wird `j["states"]` per `push_back` aufgebaut und bei leerem Array nachträglich auf `array()` gesetzt.
- **Code-Auszug**:
```cpp
for (const auto& sj : j.value("states", nlohmann::json::array()))
...
for (const auto& s : g.states) j["states"].push_back(stateToJsonObj(s));
if (g.states.empty()) j["states"] = nlohmann::json::array();
```
- **Lösung**: `if (auto it = j.find("states"); it != j.end() && it->is_array()) for (const auto& sj : *it) ...`. Beim Schreiben `j["states"] = nlohmann::json::array();` vorab setzen und dann pushen (entfällt der `empty()`-Fix).
- **Erwarteter Effekt**: Eine tiefe Kopie weniger pro Asset-Load; Lade-/Hot-Reload-Pfad, nicht pro Frame.
- **Risiko / Verifikation**: Gering; beachten, dass `value()` bei falschem Typ (z. B. `"states": 3`) eine Exception wirft, `find+is_array` überspringt still – bewusst entscheiden (gutmütiger). Round-trip-Tests für alle drei Formate.

### CORE-115 – `inputActionIsAxis`, `inputActionIsAxis2D`, `inputActionRunsWhilePaused` parsen dasselbe JSON dreimal
- **ID**: CORE-115
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `InputAssets.cpp:19-39`; Aufrufmuster `PlayerHost.cpp:40-44`, `McpToolsInput.cpp:537-543,565-571`
- **Problem**: Die Aufrufer stellen immer dieselbe Dreier-Frage (Axis2D? Axis? runWhilePaused?) zu einem Payload; jeder Aufruf parst das JSON neu.
- **Code-Auszug**:
```cpp
const ActionKind k = HE::inputActionIsAxis2D(a->json) ? ActionKind::Axis2D
                   : HE::inputActionIsAxis(a->json)   ? ActionKind::Axis : ...
... HE::inputActionRunsWhilePaused(a->json)
```
- **Lösung**: `struct InputActionInfo { std::string valueType; bool runWhilePaused; }; InputActionInfo parseInputActionInfo(const std::string&)` einmal parsen; die drei bestehenden Funktionen bleiben als dünne Wrapper (API-kompatibel).
- **Erwarteter Effekt**: Zwei Parses weniger pro Aktion bei Sessionstart; vernachlässigbar, aber einfacher.
- **Risiko / Verifikation**: Gering; bestehende Tests der drei Funktionen.

### CORE-116 – `applyInputMappingContext`: redundante Vorab-Bedingung und wiederholte `contains`/`operator[]`-Lookups
- **ID**: CORE-116
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/InputAssets.cpp:188-231`, `:164-176,287`
- **Problem**: Die äußere Bedingung (`keys || gamepadButtons || mouseButtons` ist Array) ist überflüssig: die drei inneren `if`s prüfen dasselbe erneut, und bei leerem `binds` passiert ohnehin nichts. Jedes Feld wird zweimal nachgeschlagen (`contains` + `operator[]`), `j["entries"]` dreimal; `readAxes` wird je Entry als Lambda neu erzeugt.
- **Code-Auszug**:
```cpp
if ((e.contains("keys") && e["keys"].is_array()) ||
    (e.contains("gamepadButtons") && e["gamepadButtons"].is_array()) ||
    (e.contains("mouseButtons") && e["mouseButtons"].is_array()))
{ std::vector<ActionBinding> binds; if (e.contains("keys") && e["keys"].is_array()) ...
```
- **Lösung**: Hilfsfunktion `const json* arrayOf(const json& e, const char* key)` (find + is_array), `readAxes` und neue `readButtonBindings` als freie Funktionen; Vorab-Bedingung entfernen. Funktionsverhalten identisch.
- **Erwarteter Effekt**: ~25 Zeilen weniger, Funktion unter 100 Zeilen.
- **Risiko / Verifikation**: Gering; Mapping-Context-Tests inkl. Axis2D und Replace/Union.

### CORE-117 – Doppelte Hilfsfunktionen: `sameBinding` (2×), `xmlEscape` (3×), PNG-Dekodierung (2×)
- **ID**: CORE-117
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `InputMapping.cpp:31-35` vs. `InputRebind.cpp:29-33`; `Autostart.cpp:25-39` vs. `DocumentTypes.cpp:8-22` (dritte Kopie `HE_Net/src/PortMapper.cpp:56`); `AppIcon.cpp:281-294` vs. `296-312`
- **Problem**: `ActionBinding`-Vergleich existiert privat und exportiert (`HE::sameBinding`); `xmlEscape` ist wortgleich in zwei Dateien dieses Slice; `heSetWindowIcon` wiederholt `exists`+`stbi_load` aus `heLoadPngRGBA`.
- **Code-Auszug**:
```cpp
bool sameBinding(const ActionBinding& a, const ActionBinding& b)
{ return a.key == b.key && a.gamepadButton == b.gamepadButton && a.mouseButton == b.mouseButton; }
```
- **Lösung**: `operator==` für `ActionBinding` ist laut Kommentar bewusst vermieden (Aggregat); stattdessen `sameBinding(ActionBinding, ActionBinding)` aus `InputRebind.h` auch in `InputMapping.cpp` verwenden (oder in `InputMapping.h` deklarieren). `xmlEscape` in eine interne `Util/XmlEscape.h` (inline) legen. `heSetWindowIcon` per `heLoadPngRGBA` + `SDL_CreateSurfaceFrom` auf dem Vektor (ein zusätzliches memcpy, vernachlässigbar bei Icon-Größe).
- **Erwarteter Effekt**: ~40 Zeilen weniger, eine Stelle für Escape-Regeln.
- **Risiko / Verifikation**: Gering; Tests für `heAutostartText`, `heInfoPlistDocumentTypes`, `sameBinding`.

### CORE-118 – `BindingCapture` scannt pro Frame alle 512 Scancodes dreifach; `bindingConflicts` kopiert und sortiert alle Namen
- **ID**: CORE-118
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `InputRebind.cpp:217-224,270-272` (Listening/Arming), `InputRebind.cpp:55-94`
- **Problem**: Während "Listening" läuft pro Frame `keyEdge` über alle `SDL_SCANCODE_COUNT` (~512) Codes plus `snapshot` (nochmals 512 + 26 Calls mit Cast und Geräte-Gate). Nur in einer Rebind-Session aktiv, also selten. `bindingConflicts` holt `actionNames()` und `axisNames()` (kopiert alle Strings, sortiert), schlägt dann jeden Namen erneut in der Map nach und sortiert/uniquet das Ergebnis nochmals; die Funktion läuft einmal pro Capture, nicht pro Frame.
- **Code-Auszug**:
```cpp
for (int sc = 1; sc < SDL_SCANCODE_COUNT && what == Result::None; ++sc)
    if (sc != SDL_SCANCODE_ESCAPE && keyEdge(sc)) ...
...
for (const std::string& name : mapping.actionNames()) { ... mapping.actionBindings(name) ...
```
- **Lösung**: Optional: eine Const-Iteration über die Binding-Maps als `forEachAction(fn)` in `InputMapping` anbieten und danach nur das (kleine) Ergebnis sortieren. Scancode-Scan lässt sich über ein `Input::keysDownBitset()` auf das Delta zum Snapshot reduzieren. Beides nur nötig, wenn Rebind-UI messbar auffällt.
- **Erwarteter Effekt**: Vernachlässigbar (µs, nur in Rebind-Dialog).
- **Risiko / Verifikation**: Gering; deterministische Reihenfolge (sortiert) beibehalten.

### CORE-119 – Splash: synchroner Alpha-BBox-Scan über 1024² Pixel und doppeltes UTF-8-Dekodieren je Textzeile
- **ID**: CORE-119
- **Kategorie**: D / F
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SplashScreen.cpp:163-198` (BBox), `:242-279` (`textWidth` + `text`), `:289-407` (`draw`)
- **Problem**: Das Logo (1024² laut Kommentar Z. 158) wird beim Start vollständig dekodiert und zweimal verarbeitet (BBox-Scan mit 1 M Pixel-Zugriffen, dann `crop` per memcpy); `draw()` ruft `SDL_PumpEvents` und rendert 64+48+32 Rechtecke bei jedem `setStatus`. `text()` ruft `textWidth` auf (Dekodierung) und dekodiert danach nochmals. Kritisch ist nur, dass das alles auf dem kritischen Startpfad VOR dem Fenster liegt (Kommentar `Application.cpp:277-279`); in einem Debug-Build ist der BBox-Scan spürbar. Messen.
- **Code-Auszug**:
```cpp
for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
        if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > 8) { x0 = std::min(x0, x); ... }
```
- **Lösung**: BBox mit Zeilen-Early-Exit (obere/untere Kante von außen nach innen suchen, danach Spalten) oder den Crop beim Asset-Build vorab erledigen (HC_Logo.png schon beschnitten ablegen). `text()` den Rückgabewert aus der Zeichenschleife berechnen statt `textWidth` vorzuschalten (die Breite braucht nur der Aufrufer `draw`, der ohnehin `textWidth` für das Zentrieren ruft).
- **Erwarteter Effekt**: Schätzung: wenige ms Startzeit (Release), mehr in Debug. Unsicher, messen.
- **Risiko / Verifikation**: Gering; `HE_SPLASH_DUMP` Frames vor/nach vergleichen.

### CORE-120 – `Input::PollGamepads`: pro Pad und Frame ~32 einzelne SDL-Abfragen (jeweils mit Joystick-Lock)
- **ID**: CORE-120
- **Kategorie**: D / E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Application/Input.cpp:223-246`
- **Problem**: Pro offenem Pad und Frame werden 6 Achsen und ~26 Buttons einzeln über `SDL_GetGamepadAxis/Button` gelesen (SDL3 nimmt dabei intern den Joystick-Lock je Aufruf), danach `remerge()` über alle 8 Slots. Ohne Pad greift das frühe `anyOpenPad()`-Return, der Normalfall ist also billig; mit 1–4 Pads ca. 130 Locks/Frame (Schätzung, SDL-Internas nicht verifiziert).
- **Code-Auszug**:
```cpp
for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
    one.axes[a] = static_cast<float>(SDL_GetGamepadAxis(s.pad, static_cast<SDL_GamepadAxis>(a))) / 32767.0f;
for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    one.buttons[b] = SDL_GetGamepadButton(s.pad, static_cast<SDL_GamepadButton>(b));
```
- **Lösung**: Nur messen; falls auffällig, Zustand aus `SDL_EVENT_GAMEPAD_AXIS_MOTION/BUTTON_*` ableiten (Events kommen ohnehin durch `PollEvents`), und `remerge()` nur bei Änderung. Das ändert aber die Semantik "Snapshot per Frame" und sollte nur mit Messbeleg angegangen werden.
- **Erwarteter Effekt**: Unsicher; vermutlich < 20 µs/Frame.
- **Risiko / Verifikation**: Mittel bei Umbau (Verhalten bei Hot-Plug und Fokusverlust); daher nicht ohne Messung.

### CORE-121 – Schwere Header: `Application.h` und `Assets.h` ziehen SDL, UIFont, Renderer, ContentManager, AudioEdit mit
- **ID**: CORE-121
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `Application.h:3-13` (u. a. `SplashScreen.h` → `Renderer/UIFont.h`, `Input.h` → `<SDL3/SDL.h>`), `Input.h:4` (`<unordered_map>` ungenutzt), `InputMapping.h:8,10` (`<algorithm>` im Header ungenutzt, `class Input;` redundant nach `#include Input.h`), `Assets.h:9-10` (`Scripting/ScriptTypes.h`, `Audio/AudioEdit.h` mit `<functional>`), `AudioBusConfig.h:3` (volles `AudioEdit.h` nur für `AudioEq`)
- **Problem**: `Assets.h` ist der Umbrella-Header aller Asset-Typen und wird breit eingebunden; `AudioEdit.h` wird nur für das by-value-Member `AudioAsset::edit` gebraucht. `SplashScreen.h` hält `BakedUIFont` by value und zwingt so `UIFont.h` in jede Datei, die `Application.h` einbindet.
- **Code-Auszug**:
```cpp
#include "Scripting/ScriptTypes.h"
#include "Audio/AudioEdit.h"        // Assets.h
```
- **Lösung**: Ungenutzte Includes entfernen (`<unordered_map>` in `Input.h`, `<algorithm>` und `class Input;` in `InputMapping.h`); `SplashScreen` per Pimpl/`unique_ptr<Impl>` kapseln, damit `Application.h` kein `UIFont.h` braucht; `AudioEq` in `AudioEq.h` abspalten (`AudioBusConfig.h` braucht nur das). Erst mit Build-Zeitmessung (z. B. `-ftime-trace`) priorisieren.
- **Erwarteter Effekt**: Kürzere Inkrementell-Builds; Größe unbekannt, messen.
- **Risiko / Verifikation**: Gering (Compilerfehler zeigen fehlende Includes); alle Targets bauen.

### CORE-122 – `AudioEq::responseDb` wird nur von Tests benutzt und rechnet pro Aufruf alle Bänder neu aus
- **ID**: CORE-122
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `AudioEdit.cpp:400-410`, Header `AudioEdit.h:239-240`
- **Problem**: Die Funktion ruft pro Frequenzpunkt `biquadCoefficients` (cos, sin, pow) pro Band neu auf. Im Editor macht `AudioMixView.cpp:199` es richtig (Koeffizienten einmal, dann `magnitudeDb` je Punkt); `responseDb` selbst hat im Produktivcode keinen Aufrufer (nur `tests/test_audio_bus_eq.cpp`, `test_audio_edit.cpp`, `test_ui_shot.cpp`).
- **Code-Auszug**:
```cpp
for (const AudioEqBand& b : bands)
{
    const BiquadCoeffs c = biquadCoefficients(b, sampleRate);
    if (!c.isIdentity()) db += c.magnitudeDb(freqHz, sampleRate);
}
```
- **Lösung**: Entweder als Test-Hilfsfunktion belassen und kommentieren, oder eine `responseCurveDb(const AudioEq&, rate, span<freq>, span<out>)`-Variante anbieten, die Koeffizienten einmal berechnet, und `AudioMixView` darauf umstellen.
- **Erwarteter Effekt**: Kein Produktivgewinn; verhindert, dass jemand `responseDb` in eine Zeichenschleife steckt.
- **Risiko / Verifikation**: Gering.

### CORE-123 – `AppIcon`: pro Pixel `sqrt` für die Plate-Coverage, zusätzliche Kopie im PNG-Encoder
- **ID**: CORE-123
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AppIcon.cpp:28-41,88-101` (1024²-Rendering), `:52-60` (`encodePng` kopiert `stbi_write_png_to_mem`-Puffer in einen Vektor)
- **Problem**: Der Exportpfad (`ProjectExporter.cpp:1340-1384`, einmal je Export) rendert 7 Größen bis 1024² und berechnet für jedes Pixel `std::sqrt` auch im Inneren der Plate, wo die Coverage 1 ist. Außerdem wird das PNG-Ergebnis einmal von stb in den Vektor kopiert.
- **Code-Auszug**:
```cpp
const float d  = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
return std::clamp(r + 0.5f - d, 0.0f, 1.0f);
```
- **Lösung**: In `plateCoverage` zuerst auf quadratischen Abstand prüfen: `d2 <= (r-0.5)²` → 1.0 und `d2 >= (r+0.5)²` → 0.0 ohne `sqrt`. Encoder-Kopie ist bei Icon-Größe vernachlässigbar; belassen.
- **Erwarteter Effekt**: Schätzung: Icon-Rendering ~2× schneller; Exportdauer praktisch unverändert (Sekundenbruchteile).
- **Risiko / Verifikation**: Gering; Pixelvergleich-Test des bestehenden AppIcon-Tests (muss bit-gleich bleiben, die Randbereiche rechnen weiter wie bisher).

### CORE-124 – `Input::SetGamepadFrame(slot, frame)`: verschachtelter Ternary ersetzt einfache Zustandslogik
- **ID**: CORE-124
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/Input.cpp:248-261`
- **Problem**: Drei Zustände in einem verschachtelten `?:` plus nachgelagertes `if (!frame.connected && !s.pad) s.frame = ...`. Schwer zu lesen; die Zustandsänderung ist "connected → Connected; sonst Free bleibt Free, alles andere Reserved".
- **Code-Auszug**:
```cpp
s.state = frame.connected ? GamepadSlotState::Connected
        : s.state == GamepadSlotState::Free ? GamepadSlotState::Free
                                             : GamepadSlotState::Reserved;
```
- **Lösung**: `if (frame.connected) s.state = Connected; else if (s.state != Free) s.state = Reserved;` und bei `!connected` direkt `s.frame = GamepadFrame{}` im selben Zweig.
- **Erwarteter Effekt**: Lesbarkeit.
- **Risiko / Verifikation**: Keines; bestehende Pad-Slot-Tests (`tests/` zu SetGamepadFrame).

---

## Gelesen

Alle 35 Dateien des Slice 1 vollständig gelesen: ja (`Application.cpp` in einem Block bis Zeile 1093, alle übrigen Dateien ganz); zusätzlich gezielte Greps/Auszüge in `PlayerHost.cpp`, `RenderExtractor.cpp`, `AudioEngine.cpp`, `ContentManager.cpp`, `InputAssetPanel.cpp`, `EngineProfiler.cpp` zur Verifikation von Aufrufern.

## Core Slice 2 – ContentManager, Crypto, DebugDraw, Diagnostics

## Zusammenfassung

Der Slice (ContentManager/HAsset/EngineDependencies/TextureArrayBuild, Sha256/X25519, DebugDraw, CrashHandler/DevStatsTile/EngineProfiler/GlobalState/Log) ist insgesamt sorgfältig geschrieben: Async-Streaming nutzt Sink-`shared_ptr`, Cancel-Tokens, Zeit-Budget; Log und Profiler haben saubere Hot-Path-Guards. Die größten Schwächen sind (1) von Hand gepflegte Listen aller 26 `SlotMap`-Pools, die an vier Stellen auseinanderlaufen (echte Bugs), (2) Verzeichnis-Walks, die komplette Asset-Dateien lesen bzw. pro Datei `fs::relative` (= `weakly_canonical`) aufrufen, (3) ein Use-after-return in `EngineDeps::resolve` beim Abbruch, (4) ein 500-Zeilen-`parseAndRegisterAsset` mit mehrfachen Payload-Kopien und Material-Codegen im Ladepfad, (5) synchrones `fflush` + Konsole + Sinks unter globalem Log-Mutex. Funde: 4 Hoch, 9 Mittel, 7 Niedrig. Keine Messdaten vorhanden; Schätzungen sind als solche markiert.

---

### CORE-201 – Pool-Listen in `unloadAsset`, `forgetProjectContent`, `retargetAssetReferencesInMemory` sind unvollständig
- **ID**: CORE-201
- **Kategorie**: A (handgepflegte Redundanz; führt zu Fehlverhalten, auch C: Leak)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (behebt Bugs)
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2732-2743`, `3281-3294`, `3030-3044`; Pools deklariert in `include/ContentManager/ContentManager.h:789-814`
- **Problem**: Die Klasse hat 26 `SlotMap`-Member; drei Funktionen zählen sie von Hand auf (der Kommentar bei 2727 sagt selbst „MUST list every SlotMap member“). Abgleich gegen die Member:
  - `unloadAsset` kennt **nicht**: `m_structTypeAssets`, `m_enumTypeAssets`, `m_saveTemplateAssets`, `m_themeAssets`. Folge: `unloadAsset()` liefert für diese Typen `false`; `pollHotReload` kann sie nie neu laden (siehe CORE-204: `loadAsset` gibt dann die alte UUID zurück, der mtime wird nicht aktualisiert, jede Poll-Runde meldet die Datei erneut als „geändert“).
  - `forgetProjectContent` leert **nicht**: `m_themeAssets`, `m_boneMaskAssets`, `m_blendSpaceAssets`. Nach Projektwechsel bleiben die Daten des alten Projekts im Pool liegen (Leak, die Handle-Maps sind aber geleert, die Objekte sind unerreichbar).
  - `retargetAssetReferencesInMemory` (`rekeyAssetPaths`) kennt **nicht**: struct, enum, saveTemplate, theme, boneMask, blendSpace. Ein umbenanntes, geladenes Asset dieser Typen behält den alten `path`; `saveAsset` schriebe an den verlassenen Ort (genau das Szenario, das der Kommentar bei 3003-3007 verhindern will).
- **Code-Auszug**:
```cpp
const bool removed =
    tryRemove(m_staticMeshAssets)   || ... || tryRemove(m_blendSpaceAssets)   || tryRemove(m_sequenceAssets);
// fehlt: m_structTypeAssets, m_enumTypeAssets, m_saveTemplateAssets, m_themeAssets
```
- **Lösung**: Eine einzige Aufzählung der Pools (z. B. `template<class F> void forEachPool(F&& f)` mit `f(m_staticMeshAssets); f(...)`), und alle drei Stellen (plus `getXxx`/`register` falls gewünscht) darüber implementieren. `unloadAsset` kann zusätzlich den Pool über `m_assetTypeIndex[id]` per `switch` direkt wählen (statt 22 sequenziellen `get()`-Versuchen). Neue Pools müssen dann nur an einer Stelle eingetragen werden; ein `static_assert` auf die Poolanzahl verhindert Drift.
- **Erwarteter Effekt**: Korrektheit (Hot-Reload/Unload/Rename/Projektwechsel für 3-6 Typen); `unloadAsset` O(1) Pool-Wahl statt bis zu 22 Versuchen (vernachlässigbar). Kein Performance-Gewinn zu erwarten.
- **Risiko / Verifikation**: Verhalten ändert sich bewusst nur dort, wo es bisher falsch war. Tests ergänzen: Struct/Enum/Theme laden → `unloadAsset` ist `true`; `setContentRoot` auf anderes Projekt → `m_themeAssets` leer; `retargetAssetReferencesInMemory` aktualisiert `path` eines geladenen BoneMask/BlendSpace. Vorhandene `tests/` für ContentManager (Retarget/Hot-Reload) laufen lassen.

---

### CORE-202 – `discoverAssets` liest für jede `.hasset` den **gesamten** Dateiinhalt, nur um den Typ zu prüfen
- **ID**: CORE-202
- **Kategorie**: D (+ E: blockierende I/O)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2821-2833`; `include/ContentManager/HAsset.h:351-357, 380-434`; Aufrufer `src/HE_Scene/src/PlayerHost.cpp:37,57,80`, `src/HE_Core/src/Types/TypeRegistry.cpp:704-706`
- **Problem**: Der Loose-Content-Walk öffnet jede Datei mit `HAsset::Reader::open()`, das **alle Chunk-Payloads** (Meshes, Texturen, Audio, hunderte MB) in Vektoren liest, und verwirft sie, falls der Typ nicht passt. Der Kommentar in `HAsset.h:351-356` warnt genau davor und bietet `readAssetTypeFromFile` an – das hier nicht benutzt wird. `PlayerHost` ruft `discoverAssets` dreimal beim Start, `TypeRegistry::refreshFromContent` zweimal: bis zu 5 komplette Lesedurchläufe über den ganzen Content-Baum. Zusätzlich `std::filesystem::relative` pro Treffer (siehe CORE-205). Das Ergebnis ist außerdem mit `m_diskRegistry` (Kap. `scanContentDirectory`, enthält UUID → Pfad) bzw. `m_assetTypeIndex` großteils bereits bekannt.
- **Code-Auszug**:
```cpp
HAsset::Reader r;
if (!r.open(it->path().string()) ||
    r.assetType() != static_cast<uint16_t>(type)) continue;
```
- **Lösung**: (a) Minimal: `HAsset::readAssetTypeFromFile(path, t)` verwenden (32 Byte lesen), nur bei Treffer `loadAsset(rel)`. (b) Besser: den Typ beim `scanDirInto` mitspeichern (der Header wird dort ohnehin gelesen: `hdr.asset_type`) – `m_diskRegistry` bekommt `{path, type}`, `discoverAssets` iteriert die Registry statt das Dateisystem. Bereits geladene Assets werden über `loadAsset` → `isLoaded(path)` ohnehin kurzgeschlossen.
```cpp
uint16_t t; if (!HAsset::readAssetTypeFromFile(it->path().string(), t) || t != uint16_t(type)) continue;
```
- **Erwarteter Effekt (Schätzung)**: Start-/Play-Stall und Struct/Enum-Refresh von „Content-Größe lesen × 5“ auf „Dateizahl × 32 Byte × 5“; bei Projekten mit hunderten MB Content Sekunden → Millisekunden. Messen: Zeit von `PlayerHost`-Discovery mit großem Projekt.
- **Risiko / Verifikation**: (a) verhaltensgleich. (b) ändert die Reihenfolge nicht, wenn nach Pfad sortiert wird; Test: `discoverAssets` vor/nach liefert dieselbe UUID-Menge (loses Content, Engine-Content-Override, Pak-Fall). Registry-Variante deckt Dateien ab, die nach dem letzten Scan hinzukamen, nicht ab → entweder (a) wählen oder vorher `scanContentDirectory()`.

---

### CORE-203 – Use-after-return in `EngineDeps::resolve` beim Abbruch eines Downloads
- **ID**: CORE-203
- **Kategorie**: E (Nebenläufigkeit/Lifetime)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (behebt Bug)
- **Ort**: `src/HE_Core/src/ContentManager/EngineDependencies.cpp:291-325`
- **Problem**: Die Fetch-Callbacks fangen `m`, `cv`, `finished`, `ok` (Stack-Variablen des `if`-Blocks) per `[&, i]` ein. Bei `options.cancelled()` wird mit `return w.result;` ausgestiegen, während „die bereits angestoßenen Downloads weiterlaufen“ (Kommentar 320). Wenn ein Download später fertig wird, schreibt sein Callback in `ok[i]`, `finished` und lockt/benachrichtigt `m`/`cv` – alles bereits zerstört (Stack/Heap freigegeben). Stack-Korruption bzw. UB auf dem SFTP-Worker.
- **Code-Auszug**:
```cpp
options.fetch(toFetch[i].substr(kEnginePrefixLen), [&, i](bool success)
{
    std::lock_guard<std::mutex> lock(m);
    ok[i] = success ? 1 : 0; ++finished; cv.notify_all();
});
...
if (options.cancelled && options.cancelled()) { w.result.cancelled = true; return w.result; }
```
- **Lösung**: Gemeinsamen Zustand in ein `struct State { std::mutex m; std::condition_variable cv; size_t finished = 0; std::vector<char> ok; };` legen, per `std::make_shared` allokieren und in jeder Lambda **by value** (shared_ptr) fangen. Außerdem die 100-ms-Poll-Schleife auf `cv.wait_for(lock, 100ms, pred)` mit Prädikat umstellen (entfernt das umständliche unlock/lock-Muster 314-316).
- **Erwarteter Effekt**: Beseitigt UB; kein Performance-Effekt.
- **Risiko / Verifikation**: Gering. Test mit Fake-`fetch`, der nach dem Abbruch (verzögert auf anderem Thread) die Completion aufruft – unter ASan/TSan laufen lassen (vorhandene EngineDeps-Tests erweitern).

---

### CORE-204 – Hot-Reload: Self-Reload nach jedem `saveAsset`, endlose Falsch-Änderungen bei nicht entladbaren Typen
- **ID**: CORE-204
- **Kategorie**: D (wiederholte Arbeit) / E
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2290-2321` (saveAsset), `2845-2881` (pollHotReload), Aufruf `src/HE_Editor/EditorApplication.cpp:3430-3435`
- **Problem**: `saveAsset` schreibt die Datei, aktualisiert aber `m_pathMtime[path]` **nicht**. Der nächste `pollHotReload` (alle 1,5 s) sieht eine neuere Datei-mtime, entlädt das gerade gespeicherte Asset und lädt es erneut – jeder Save löst also einen vollen Reload aus (inkl. Re-Parse, bei Materialien Codegen, `InvalidateMaterial`, bei Texturen `InvalidateMaterial` für **alle** Materialien in `EditorApplication`). Bei den in CORE-201 genannten Typen scheitert `unloadAsset`, `loadAsset(relPath)` gibt wegen `isLoaded(path)` sofort die alte UUID zurück und `m_pathMtime` bleibt alt → die Datei wird **bei jedem Poll** erneut als „geändert“ gemeldet.
- **Code-Auszug**:
```cpp
unloadAsset(pathIt->second);                 // Ergebnis ignoriert
const HE::UUID newId = loadAsset(relPath);   // isLoaded(path) → alte UUID
if (!(newId == HE::UUID{})) changed.push_back(newId);
```
- **Lösung**: (1) Nach erfolgreichem `writeAssetFile` in `saveAsset` die mtime der geschriebenen Datei in `m_pathMtime[asset.path]` eintragen (nur wenn der Pfad bereits geladen/registriert ist). (2) In `pollHotReload` den Rückgabewert von `unloadAsset` prüfen und bei `false` (gepinnt/unbekannt) `m_pathMtime[relPath] = mtime` setzen und `continue` – kein falsches `changed`.
- **Erwarteter Effekt (Schätzung)**: Entfällt ein kompletter Reload pro Save (bei Materialien mit Graph: JSON-Parse + Codegen + Renderer-Invalidierung), und endloses 1,5-s-Rauschen bei 4 Typen.
- **Risiko / Verifikation**: Mittel: Prüfen, ob Editor-Panels sich auf den Self-Reload verlassen (z. B. um Instanzen via `syncMaterialInstancesOf` neu abzuleiten – laut Kommentar in `EditorApplication.cpp` leitet der Material Editor Instanzen bei eigenen Edits selbst ab). Test: speichern → `pollHotReload()` liefert leere Liste; externe Änderung → liefert UUID genau einmal; Theme/Struct extern ändern → nicht in Dauerschleife.

---

### CORE-205 – `pollHotReload` macht alle 1,5 s O(n) Syscalls und Allokationen auf dem Main-Thread
- **ID**: CORE-205
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2845-2881`, `983-1004` (`resolveAbsolutePath`)
- **Problem**: Pro Poll: Kopie aller Pfad-Strings aus `m_pathMtime` in einen Vektor; pro Pfad `resolveAbsolutePath` (String-Konkatenation; für `Engine/…` bis zu drei `std::filesystem::exists`-Syscalls) und dazu `last_write_time`. Bei einigen tausend geladenen Assets sind das 10-20k Syscalls alle 1,5 s im UI-Thread (Hitch). Engine-Default-Dateien ändern sich im Betrieb praktisch nie.
- **Code-Auszug**:
```cpp
std::vector<std::string> paths; paths.reserve(m_pathMtime.size());
for (const auto& [p, _] : m_pathMtime) paths.push_back(p);
for (const auto& relPath : paths) {
    const std::string fullPath = resolveAbsolutePath(relPath);
    const auto mtime = fs::last_write_time(fullPath, ec);
```
- **Lösung**: In `m_pathMtime` neben der mtime den aufgelösten Absolutpfad cachen (`struct { file_time_type mtime; std::string full; }`), so entfallen Kopie und `exists`-Kaskade; `Engine/`-Defaults (ohne Override) überspringen; oder Poll auf Round-Robin-Scheiben (z. B. 1/8 der Einträge pro Tick) bzw. Dateisystem-Watcher.
- **Erwarteter Effekt (Schätzung)**: Faktor 2-4 weniger Syscalls (Pfadauflösung entfällt), mit Round-Robin gleichmäßig verteilte Last. Messen: Dauer von `pollHotReload` bei 5k Assets.
- **Risiko / Verifikation**: Gecachter Pfad wird ungültig, wenn ein Override unter `Content/Engine/…` neu entsteht → bei `noteContentChanged()` (`m_contentEpoch`) Cache verwerfen. Latenz der Erkennung steigt bei Round-Robin (Dokumentieren). Test: Override anlegen/löschen wird erkannt.

---

### CORE-206 – `std::filesystem::relative` (= `weakly_canonical`) pro Datei im Scan und in Pfad-Helfern
- **ID**: CORE-206
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:3260` (`scanDirInto`), `1019`, `1028` (`isEngineDefaultPath`/`isEngineOverridePath`), `1038`, `1054`, `1060` (`toContentRelativePath`), `2829` (`discoverAssets`)
- **Problem**: `fs::relative(p, base)` ruft `weakly_canonical` auf beide Argumente (libstdc++/MSVC), das pro Pfadkomponente `status`-Syscalls macht. `scanDirInto` läuft für drei Wurzeln (Content, Engine, Cache) über jede `.hasset`; `isEngine*Path`/`toContentRelativePath` werden vom Editor (Content Browser, Drag/Drop) aufgerufen, ggf. pro Item/Frame (Aufrufstellen außerhalb dieses Slice – unsicher). Außerdem ruft `scanDirInto` in der Schleife zwei Mal `p.path().extension()` und öffnet jede Datei einzeln per `ifstream`.
- **Code-Auszug**:
```cpp
const auto rel = std::filesystem::relative(p.path(), root, ec);
if (!ec) m_diskRegistry[id] = pathPrefix + rel.generic_string();
```
- **Lösung**: Im rekursiven Iterator ist der Pfad bereits unterhalb `root`; `p.path().lexically_relative(root)` genügt (reine String-Operation). Für `toContentRelativePath`/`isEngine*` ebenfalls lexikalisch, nach einmaliger Normalisierung der Roots (`weakly_canonical` im Setter, nicht im Aufruf).
- **Erwarteter Effekt (Schätzung)**: `scanContentDirectory` bei tausenden Dateien um einen großen Teil schneller (Syscalls pro Datei: ~5-10 → 2 für open/read). Messen mit `strace -c`/Profiler-Scope um `scanContentDirectory`.
- **Risiko / Verifikation**: Unterschied bei Symlinks/`..`-Segmenten: `weakly_canonical` löst sie auf, `lexically_relative` nicht. Wurzeln beim Setzen kanonisieren; Test mit Symlink-Content-Ordner und Windows-Laufwerksbuchstaben-Groß/Kleinschreibung (Fall `isEngineOverridePath`).

---

### CORE-207 – `parseAndRegisterAsset`: 500-Zeilen-Switch mit mehrfach kopiertem Payload
- **ID**: CORE-207
- **Kategorie**: A + C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `ContentManager.cpp:171-696` (u. a. `289`, `405`, `514-516`, `531`, `548`); `HAsset.h:437-477` (`openData`); `ContentManager.cpp:3048-3067, 3373-3376`
- **Problem**: (A) Eine Funktion decodiert 26 Assettypen inline; zehn Typen wiederholen wortgleich `a.xxx.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size())`. (C) Payload wird auf dem Ladepfad mehrfach kopiert: Pak-Eintrag (`readEntry` Vektor) → `Reader::openData` kopiert jeden Chunk in einen eigenen Vektor → `parseAndRegisterAsset` kopiert erneut ins Asset (`a.data = c->data` für Textur-Pixel, Audio, Font, Prefab, HC-Komponenten; `readVec` für Mesh-Arrays). Für große Texturen/Meshes sind das 2-3 Vollkopien, der letzte Schritt auf dem Main-Thread (`pollAsyncResults`). Die Kopie im Worker (`splitChunks`) ist unkritisch, die im Main-Thread nicht.
- **Code-Auszug**:
```cpp
if (const auto* c = reader.findChunk(HAsset::CHUNK_PIXL)) a.data = c->data;   // Kopie
if (const auto* c = reader.findChunk(HAsset::CHUNK_OGGD)) { a.audioData = c->data; ... }
```
- **Lösung**: (1) `HAsset::Reader::takeChunk(id)` (liefert `std::vector<uint8_t>&&`, der Reader ist ein lokal besessenes Wegwerf-Objekt) und in Blob-Typen (`PIXL`, `OGGD`/`PCMD`, `FNTD`, `PFAB`, `HCCP`, `PSHD`-Eingang) verschieben statt kopieren. (2) Pro Typ eine `static bool parseXxx(const Reader&/Reader&, XxxAsset&)`-Funktion, die Text-Typen über `chunkToString(reader, id, out)` füllen; `parseAndRegisterAsset` wird zur Tabelle `type → (pool, parseFn)`. (3) Optional `openData(std::vector<uint8_t>&&)` mit Offset-Views statt Chunk-Kopien.
- **Erwarteter Effekt (Schätzung)**: Main-Thread-Zeit beim Registrieren großer Texturen/Audio um die Kopierkosten (memcpy-Bandbreite, ~1-2 GB/s effektiv inkl. Allokation) kleiner, d. h. bei 64-MB-Textur grob 30-60 ms weniger pro Asset; Peak-Speicher −1× Assetgröße. Codegröße −150 Zeilen.
- **Risiko / Verifikation**: `findChunk` ist `const`; Reader wird nach dem Parse nicht mehr benutzt (außer `readMeshSections`, nach den `take`s aufrufen oder Reihenfolge beachten). Round-Trip-Tests je Typ (Save → Load → Save, Bytes identisch) und `test_hpak`/Streaming-Tests ausführen. Chunk-Layout bleibt unverändert.

---

### CORE-208 – Material-Ladepfad: Graph wird bis zu dreimal geparst, Codegen läuft auf dem Main-Thread
- **ID**: CORE-208
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:673-691`, `788-819`, `1723-1733`, `1735-1776` (insb. `1760-1766`)
- **Problem**: Jedes registrierte Basis-Material mit `nodeGraphJson` (ohne `precompiledShaders`, also Editor/Dev) durchläuft `regenerateMaterialFromGraph`: JSON-Parse (`materialGraphFromJson`), `generateFragment`, Param-Neuaufbau. Sind Remote-Assets aktiv (`m_remoteByPath` nicht leer), kommen `graphFunctionsReady` (Parse #2) und `prefetchRemoteReferences` (Parse #3, nur für Funktionsnamen) hinzu. Das geschieht synchron in `pollAsyncResults` (Main-Thread; das Zeitbudget greift erst *nach* dem Asset) und bei jedem Hot-Reload. Der Zweck („Codegen-Upgrade ohne Resave“) bräuchte die Regeneration nur, wenn der Codegen sich seit dem Speichern geändert hat.
- **Code-Auszug**:
```cpp
else if (m && !m->nodeGraphJson.empty() && m->precompiledShaders.empty())
{
    if (graphFunctionsReady(m->nodeGraphJson))   // parse #2 (remote only)
        regenerateMaterialFromGraph(id);          // parse #1 + generateFragment
}
```
- **Lösung**: (a) Graph nur einmal parsen und das `MaterialGraph` an `graphFunctionsReady`, `prefetch…` und `regenerate…` durchreichen (oder FunctionCall-Pfade direkt aus dem bereits geparsten Graph lesen). (b) Generierte GLSL mit `hash(nodeGraphJson, kCodegenVersion)` markieren (im Asset im Speicher, optional später im MTRL-Tail) und die Regeneration überspringen, wenn die gespeicherte `customShaderFragGlsl` nachweislich aus derselben Codegen-Version stammt.
- **Erwarteter Effekt**: (a) sicher −2 JSON-Parses bei Remote-Setups; (b) potenziell der größte Teil der Material-Ladezeit im Editor. Zahlen **unsicher / messen** (Profiler-Scope um `regenerateMaterialFromGraph`, Projekt mit vielen Graph-Materialien).
- **Risiko / Verifikation**: (b) ändert das Tail-Format → laut `HAsset.h`-Kommentar nur Append erlaubt, und `HpakWriter` (MTRL-Branch) muss mitziehen; deshalb zuerst nur in-memory-Cache. Test: Material mit Graph laden → GLSL identisch zum bisherigen Ergebnis (Golden), Funktion nachladen → Regeneration läuft weiterhin.

---

### CORE-209 – Logging: `fflush` pro Record, Datei-, Konsolen- und Sink-Ausgabe unter dem globalen Mutex
- **ID**: CORE-209
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/Log.cpp:259-285` (`emitLine`), `663-681` (`writeV`)
- **Problem**: Jeder Log-Record, der den Filter passiert, hält `mutex()` während `fwrite` + `fputc` + **`fflush`** (ein `write`-Syscall je Zeile), `fprintf` auf stdout/stderr (unter Windows-Konsole teuer) und aller registrierten Sinks. Alle Threads (Job-Worker, Streaming, Main) serialisieren darauf; beim Laden (INFO/DEBUG je Asset, Shader-Compile usw.) blockiert I/O den Frame-Thread. Sinks laufen unter dem Lock – ein Sink, der selbst loggt, deadlockt (std::mutex nicht rekursiv).
- **Code-Auszug**:
```cpp
std::fwrite(line, 1, static_cast<size_t>(length), s_file);
std::fputc('\n', s_file);
std::fflush(s_file);          // pro Record
...
for (const SinkSlot& s : sinks()) if (s.fn) s.fn(rec, s.user);
```
- **Lösung**: `fflush` nur bei `Level >= Warning` (und in `flush()`/Crash-Handler, der den Ring ohnehin schreibt); für Debug/Info reicht gepuffertes Schreiben plus periodischer Flush (z. B. beim `setFrameNumber`-Tick oder 250-ms-Timer). Optional: Sinks aus einer Kopie der Sink-Liste außerhalb des Locks aufrufen (Snapshot unter Lock, dann Aufruf), Datei-I/O auf Hintergrund-Thread mit Ring (Ring existiert bereits).
- **Erwarteter Effekt (Schätzung)**: Pro Info-Zeile ein Syscall weniger (≈ 5-30 µs auf SSD, mehr unter AV-Scannern/Windows); bei Ladeszenen mit tausenden Zeilen im Bereich 10-100 ms. **Messen**: Zähler `messageCount` × Zeit im `emitLine`.
- **Risiko / Verifikation**: Absturz-Sicherheit: letzte Info-Zeilen können bei hartem Kill (SIGKILL/Stromausfall) fehlen; Crash-Handler gibt den Ring ohnehin aus (`CrashHandler.cpp:38-44`). Tests: `test_log*` (falls vorhanden) und manuelles Crash-Szenario prüfen. Das Verhalten „Warnung/Fehler sofort auf Platte“ bleibt erhalten.

---

### CORE-210 – `EngineProfiler`: Ring über `vector::erase(begin())`, Live-Puffer ebenso, pro Frame frische Allokation
- **ID**: CORE-210
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/EngineProfiler.cpp:292-299`, `315-317`, `321-325`, `271`; `include/Diagnostics/EngineProfiler.h:210`
- **Problem**: Mit gesetztem `maxFrames` wird bei jedem Frame der älteste `ProfFrameRecord` per `m_frames.erase(m_frames.begin())` entfernt – O(N) Move-Assignments von Records, die je zwei `std::vector` enthalten (nicht trivially copyable). Dasselbe für `m_frameMarks` und – auch ohne Aufnahme – `pushLive` (240 Elemente pro Frame, solange der Live-HUD/`DevStatsTile` an ist). Der Profiler soll messen, verfälscht aber die Messung proportional zu `maxFrames`. Zusätzlich `m_current = ProfFrameRecord{}` pro Frame: `scopes`/`gpuPasses` werden jedes Mal neu aufgebaut (Wachstum 0 → N Reallokationen), weil `m_current` beim Push in `m_frames` leergezogen wird. `liveSnapshot()` kopiert den Puffer by value.
- **Code-Auszug**:
```cpp
if (m_maxFrames != 0 && m_frames.size() >= m_maxFrames)
    m_frames.erase(m_frames.begin());   // ring: drop oldest
```
- **Lösung**: `std::deque` (oder fester Ringpuffer mit Index) für `m_frames`, `m_frameMarks`, `m_live`; `snapshot()` kopiert dann einmalig linearisiert. Für `m_current.scopes` vor jedem Frame `reserve(letzte Größe)`; bei `m_singleFrame`/`m_frames` Vektorkapazität wiederverwenden.
- **Erwarteter Effekt (Schätzung)**: pro Frame während der Aufnahme von O(maxFrames) auf O(1); für `maxFrames` ≈ 5000 grob 20-80 µs/Frame weniger Messrauschen. **Messen** mit `maxFrames` groß/klein.
- **Risiko / Verifikation**: `lastFrame()` gibt `&m_frames.back()` – bei `deque` weiterhin gültig. Tests für Profiler-Dump/Stats (`HE::Prof::*` aus `ProfilerStats.h`) laufen lassen; Ringverhalten (`frameCount <= maxFrames`) prüfen.

---

### CORE-211 – Profiler-Dump: ganze Ausgabe als `nlohmann::json` im Speicher, mehrfach kopiert, synchron auf dem Main-Thread
- **ID**: CORE-211
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `EngineProfiler.cpp:229-233` (Stop im `beginFrame`), `603`, `760`, `794`, `804`, `835`, `727-735`, `847`
- **Problem**: `doStopDump` läuft im `beginFrame` des Frame-Threads und baut für bis zu 120 000 Timeline-Spans plus jeden Frame (mit je allen Scopes) ein `json`-Objekt (bei nlohmann pro Objekt ein `std::map`, je Span 4 Schlüssel). Mehrere Zuweisungen **kopieren** statt zu verschieben: `j["summary"] = summary;`, `j["frames"] = frames;`, `jf["cpu"] = scopes;`, `jf["gpu"] = gp;`, `summary["hitches"] = jh;`. Danach `j.dump(1, '\t')` als zusätzlicher String im Speicher und `ofstream`. Beim Stoppen steht der Frame-Thread dadurch Sekunden still, der Peak-Speicher ist ein Vielfaches der Rohdaten.
- **Code-Auszug**:
```cpp
j["summary"] = summary;       // Kopie des gesamten Summary-Baums
...
jf["cpu"] = scopes;           // pro Frame Kopie
...
j["frames"] = frames;         // Kopie des größten Teils
f << j.dump(1, '\t');
```
- **Lösung**: `std::move` an allen genannten Stellen; Dump auf einen Hintergrund-Thread verlagern (Daten per `std::move` übergeben: `m_frames`, `lanes` sind nach Stop ohnehin nicht mehr nötig – `dumpNow()` für den „Dump“-Button muss aber kopieren); Frames/Spans streamend schreiben (`nlohmann::json::dump` in Chunks oder ein einfacher Writer), statt den gesamten Baum zu halten.
- **Erwarteter Effekt (Schätzung)**: Peak-Speicher und Stop-Stall bei langen Captures grob halbiert allein durch Moves; mit Thread zusätzlich kein Frame-Stall. **Messen** bei 5000-Frame-Capture.
- **Risiko / Verifikation**: Ausgabeformat darf sich nicht ändern (Tools/Skripte, die `profile_*.json` lesen) → Golden-Vergleich mit einem kurzen Capture; Thread-Variante: `m_justDumped`/`m_lastDumpPath` müssen thread-sicher werden. Niedrigster Aufwand: nur die Moves.

---

### CORE-212 – Mbed-TLS-Pfad: `ensurePsaInit` ist nicht thread-sicher und meldet fälschlich `false`
- **ID**: CORE-212
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (behebt Bug)
- **Ort**: `src/HE_Core/src/Crypto/X25519.cpp:88-97`
- **Problem**: Zwei nicht-atomare `static bool`s: `initialized = true` wird **vor** `psa_crypto_init()` gesetzt. Ruft ein zweiter Thread (Handshake mehrerer Verbindungen) währenddessen `ensurePsaInit` auf, sieht er `initialized == true`, aber `ok == false` und der Handshake scheitert (oder Datenrennen auf `ok`). Betrifft nur Builds ohne OpenSSL.
- **Code-Auszug**:
```cpp
static bool initialized = false; static bool ok = false;
if (!initialized) { initialized = true; ok = (psa_crypto_init() == PSA_SUCCESS); }
return ok;
```
- **Lösung**: Function-local static mit Initialisierer: `static const bool ok = (psa_crypto_init() == PSA_SUCCESS); return ok;` (C++11 garantiert thread-sichere Einmal-Initialisierung; kürzer).
- **Erwarteter Effekt**: Korrektheit, −5 Zeilen.
- **Risiko / Verifikation**: Gering. KATs `tests/test_crypto_x25519.cpp` mit Mbed-Backend laufen lassen; optional TSan-Test mit zwei Threads.

---

### CORE-213 – `GlobalState`: Merge-Funktionen sind O(n²) und stat-en jede Remote-Datei bei jedem Refresh
- **ID**: CORE-213
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/GlobalState.cpp:672-673`, `690-691`, `730-732`, `786-796`, `829`
- **Problem**: `mergeOverrideInto` und `mergeManifestInto` suchen jeden Eintrag linear per String-Vergleich in `subfolders`/`files` (`f->name == leafName`). Bei einem Engine-Katalog mit tausenden Einträgen in wenigen Ordnern ist das O(n·m) String-Vergleiche pro Refresh. Zusätzlich `fs::is_regular_file(cachePath)` (Syscall) für jedes Manifest-Element, das lokal nicht existiert, bei **jedem** `refreshEngineFolder` (laut Header-Kommentar standardmäßig alle 60 s und bei jedem Create/Rename), plus Kopie des gesamten `m_engineRemoteAssets`-Vektors (829). Läuft laut Kommentar abseits des Frame-Threads (SFTP-Worker/Refresh) → Mittel statt Hoch.
- **Code-Auszug**:
```cpp
for (HE::File* f : parent->files)
    if (f->name == leafName) { existsLocally = true; break; }
...
const bool cached = fs::is_regular_file(cachePath, ec);
```
- **Lösung**: Pro Ordner beim ersten Zugriff ein `unordered_map<std::string_view, Folder*/File*>` (oder Sortierung + `lower_bound`) aufbauen; Cache-Existenz einmal pro Refresh über einen einzigen Verzeichnis-Walk der Cache-Wurzel in ein `unordered_set` von relativen Pfaden statt N einzelner `stat`s.
- **Erwarteter Effekt (Schätzung)**: Bei ~5000 Manifest-Einträgen von Millionen String-Vergleichen und 5000 Syscalls auf ~ein paar zehn Tausend Operationen. **Messen**: Dauer `refreshEngineFolder` mit großem Manifest.
- **Risiko / Verifikation**: Reihenfolge der Knoten ändert sich nicht (Sortierung am Ende, `sortFolderTree`). Tests: bestehende Content-Folder-Tests (`remoteOnly`/`isLocalCacheCopy`-Fälle).

---

### CORE-214 – `ContentManager`-Getter/Register: ca. 75 handgeschriebene Einzeiler-Wrapper
- **ID**: CORE-214
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ContentManager.cpp:2541-2585`, `ContentManager.h:129-187`, `2658-2685`
- **Problem**: Jeder Assettyp hat `getX`, `getXMutable`, `registerX`, je eine Zeile mit denselben zwei Argumenten (Handle-Map + Pool). Dazu kommt, dass `lookupAsset` zwei Hash-/Slot-Zugriffe macht und das `m_handleToUUID`-Handle über Pools hinweg nur durch die Nach-Prüfung `a->id == id` unterschieden wird. Die Boilerplate ist der Grund für die Drift in CORE-201.
- **Lösung**: Ein Trait `template<class T> SlotMap<T>& pool()` (Spezialisierung per Member-Zeiger-Tabelle) und `template<class T> const T* get(HE::UUID) const`; die benannten Methoden bleiben als dünne Inline-Wrapper oder per Makro-freier Liste. Mittel- bis langfristig Pool-Lookup über `m_assetTypeIndex` statt Nach-Prüfung.
- **Erwarteter Effekt**: −80..100 Zeilen, weniger Drift. Keine Laufzeitänderung erwartet.
- **Risiko / Verifikation**: API bleibt gleich (Header-ABI, `HE_API`-Klasse) – nur additiv. Compile-Test aller Aufrufer; keine Verhaltensänderung.

---

### CORE-215 – `unloadAsset`/`pathForId`/`setMaterialParam`: lineare Suchen in Maps und Param-Namen
- **ID**: CORE-215
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:2747-2748` (`unloadAsset`), `ContentManager.h:73-80` (`pathForId`), `ContentManager.cpp:2587-2616` (`setMaterialParam`/`getMaterialParam`)
- **Problem**: `unloadAsset` durchläuft `m_pathToUUID` komplett, um den Pfad zur UUID zu finden (O(n) pro Unload; beim Projektwechsel/Massenunload O(n²)); `pathForId` ebenso (Kommentar verlangt Caching beim Aufrufer). `setMaterialParam` (Scripting-API, potentiell pro Frame und Material) sucht den Parameternamen linear per String-Vergleich.
- **Code-Auszug**:
```cpp
for (auto pit = m_pathToUUID.begin(); pit != m_pathToUUID.end(); ++pit)
    if (pit->second == id) { m_pathMtime.erase(pit->first); m_pathToUUID.erase(pit); break; }
```
- **Lösung**: Der Pfad steht im Asset (`asset.path`, vor dem Entfernen lesen) → `m_pathToUUID.erase(path)` / `m_pathMtime.erase(path)` direkt (O(1)); Achtung `registerRuntimeAsset` mit leerem Pfad. `setMaterialParam`: Slot-Index vom Skript cachen lassen (Name → Index einmalig) oder bei ≤ 16 Parametern belassen (16×String-Compare ist klein) – nur ändern, wenn Profiler es zeigt.
- **Erwarteter Effekt**: Unload O(1); `setMaterialParam` **unsicher / messen**.
- **Risiko / Verifikation**: `m_pathToUUID` kann mehrere Pfade auf dieselbe UUID abbilden (`loadAsset` setzt `m_pathToUUID[relativePath] = rid` für Pak-Aliase, Zeile 1080-1085); die Schleife entfernt aktuell nur den ersten Treffer. Verhalten dort angleichen oder dokumentieren; Test: Alias-Pfad nach Unload.

---

### CORE-216 – Pak-Streaming: pro Asset-Job ein eigener Datei-Open und Pfad-Kopie
- **ID**: CORE-216
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:1625-1693` (`launchPakLoad`), `1785-1812` (`streamMountedAssets`)
- **Problem**: Jeder `AssetLoadPak`-Job erzeugt einen neuen `HpakReader`, öffnet die `.hpak`-Datei (`openShared`) und liest einen Eintrag. `streamMountedAssets` startet einen Job je Pak-Eintrag, d. h. N Datei-Opens und N Kopien von `path`, `key`, `std::shared_ptr<Toc>`. Der Kommentar zeigt, dass der quadratische TOC-Teil schon beseitigt wurde; Datei-Open pro Job bleibt.
- **Lösung**: Thread-lokalen Reader-Cache (`thread_local std::unique_ptr<HpakReader>` je Pfad+TOC-Zeiger) im Worker, der bei Pfadwechsel neu öffnet. Verhalten identisch.
- **Erwarteter Effekt**: **unsicher / messen** (Open-Kosten hängen vom Betriebssystem/AV ab; HpakReader selbst liegt außerhalb dieses Slice).
- **Risiko / Verifikation**: Lifetime des Readers vs. ausgetauschte Pak-Datei (die `reader.open(path)`-Fallback-Logik bleibt nötig). Messen mit `strace -c`/Process Monitor beim Start eines Spiels mit großem Pak.

---

### CORE-217 – `CrashHandler`: POSIX ohne Alternate-Stack und mit nicht signalsicheren Aufrufen; irreführender Kommentar unter Windows
- **ID**: CORE-217
- **Kategorie**: F / E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Diagnostics/CrashHandler.cpp:72-121`, `348-354`, `430-440`
- **Problem**: (1) Der POSIX-Handler läuft auf dem Stack des abstürzenden Threads (`sigaltstack` fehlt): bei Stack-Overflow-SIGSEGV kann er nichts ausgeben, obwohl der Windows-Zweig genau diesen Fall adressiert. (2) Er ruft `fopen`, `fprintf`, `localtime`, `ctime`, `backtrace_symbols` (malloc) auf – nicht async-signal-safe; bei Heap-Korruption (häufige Absturzursache) kann er selbst hängen. Der Kommentar bei 98-100 räumt das teilweise ein. (3) Windows-Kommentar sagt „the wait below is bounded rather than infinite“, der Code ist `for (;;) ::Sleep(1000);`.
- **Code-Auszug**:
```cpp
// A crash INSIDE the report thread lands here too, so the wait below is bounded rather than infinite.
if (::InterlockedCompareExchange(&s_reporting, 1, 0) != 0) { for (;;) ::Sleep(1000); }
```
- **Lösung**: `sigaltstack` mit statischem 64-KB-Puffer + `SA_ONSTACK`; Report-Pfad vorab bauen (`buildReportPath` im `install`, nicht im Handler), `open`/`write` statt `fopen`/`fprintf`, `backtrace_symbols_fd` statt `backtrace_symbols`. Kommentar anpassen oder tatsächlich begrenzen (z. B. `WaitForSingleObject(..., 60000)` Schleife).
- **Erwarteter Effekt**: Robustere Crash-Reports (Stack-Overflow, Heap-Korruption). Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Gering; Test mit Absicht ausgelöstem SIGSEGV/Stack-Overflow auf Linux/macOS, Ausgabeformat bleibt (Header-Kommentare, Report-Zeilen) – von Bug-Reporter ggf. geparst (`Log::recentProblemLines`-Konsumenten prüfen).

---

### CORE-218 – `ProfileScope` ruft pro Konstruktion die exportierte `instance()`; „Zero cost“ stimmt nur annähernd
- **ID**: CORE-218
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `include/Diagnostics/EngineProfiler.h:334-349`, `EngineProfiler.cpp:154-158`
- **Problem**: Der Header verspricht „one bool check per scope“. Tatsächlich ruft jede `ProfileScope`-Konstruktion `EngineProfiler::instance()` (nicht inline, `HE_API`-exportiert, Function-Static-Guard) und dann `isRecording()` (atomares Acquire-Load) – auch im Normalbetrieb ohne Aufnahme. Der JobSystem-Pool umhüllt jeden Task mit einem solchen Scope, der Main-Loop hat viele (hunderte bis tausende pro Frame).
- **Code-Auszug**:
```cpp
explicit ProfileScope(const char* name) : m_opened(EngineProfiler::instance().isRecording())
```
- **Lösung**: Ein inline zugängliches `extern std::atomic<bool>`-Flag (`EngineProfiler::s_recordingFlag`, im .cpp definiert, im Header als `HE_API extern`) für den Schnellpfad; `instance()` nur noch bei `true`.
- **Erwarteter Effekt (Schätzung)**: wenige ns pro Scope; bei ~2000 Scopes/Frame ≈ 5-20 µs. Nur ändern, wenn Profiling es relevant zeigt (**messen**).
- **Risiko / Verifikation**: Gering; `m_recording` bleibt Single Source (Flag = Referenz auf dieselbe Variable).

---

### CORE-219 – `Sha256`: reine Softwareimplementierung für Startup-Integritätsprüfung
- **ID**: CORE-219
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Crypto/Sha256.cpp:39-67`, `134-152`; Aufrufer `src/HE_Core/src/Integrity/IntegrityProbe.cpp:103,128`
- **Problem**: Die Integritätsprobe hasht beim Start Executable und alle Engine-Bibliotheken („tens of megabytes each“, laut Header) mit einer skalaren Implementierung (~150-300 MB/s je Kern, ohne SHA-NI/ARMv8-SHA). Der Header begründet die Eigenimplementierung mit Abhängigkeitsfreiheit; die Krypto-Backends (OpenSSL/mbedTLS) stehen im selben Modul zur Verfügung (siehe `X25519.cpp`) und beschleunigen SHA-256 hardwarebasiert.
- **Lösung**: Bei vorhandenem Backend `EVP_Digest*` bzw. `psa_hash_*` verwenden und die Eigenimplementierung als Fallback behalten; alternativ Hashing in einem Hintergrund-Thread parallel zur restlichen Initialisierung. `finish()` allokiert nichts; `~Sha256() = default;` kann inline bleiben.
- **Erwarteter Effekt (Schätzung)**: bei 100-200 MB Binaries ~0,3-0,7 s → ~0,05-0,15 s Startzeit. **Messen**: `IntegrityProbe`-Dauer im Startlog.
- **Risiko / Verifikation**: Digest bleibt identisch (KATs in `test_hpak`/`test_net_secure`, siehe Header). Zwei Codepfade → beide testen.

---

### CORE-220 – Kleinkram: toter Code, Duplikate, Kommentare, schwerer Header
- **ID**: CORE-220
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: mehrere (siehe Liste)
- **Problem / Lösung** (je 1-5 Zeilen Änderung, rein lesbarkeitsbedingt):
  1. `EngineProfiler.h:191,295` / `EngineProfiler.cpp:247,308`: `m_forceDetailed` wird nirgends auf `true` gesetzt (Grep im gesamten `src/`), `detailedGpuCapture()` ist damit nur `m_detailedGpu`. Flag und die zugehörigen Kommentare entfernen.
  2. `ContentManager.cpp:3549-3551`: `((x % kCell) <= 1 && (x % kCell) >= 0)` – `>= 0` ist für `int` ≥ 0 immer wahr; `(x % kCell + y % kCell < 2)` allein deckt die Ecke ab.
  3. `ContentManager.cpp:3114-3181`: `mountPak` parst zwei gleich aufgebaute JSON-Indizes (Pfad-Index, Typ-Index) mit duplizierter `"hi:lo"`-Zerlegung inkl. `try/catch` um `stoull`; eine Hilfsfunktion `parseUuidPair(const std::string&, UUID&)` (mit `std::from_chars`, kein Exception-Pfad) genügt.
  4. `Log.cpp:908-929` (`systemInfoBlock`) und `946-956` (`logStartupBanner`) formatieren Build/Platform/CPU/Memory doppelt und rufen `osVersionString()`/`cpuBrandString()` (liest `/proc/cpuinfo`, `/etc/os-release`) jeweils neu; `logStartupBanner` könnte `systemInfoBlock()` zeilenweise loggen.
  5. `Log.cpp:97-108`: `verbosities()` hat zwei Function-Statics (Array + `seeded`) und wird in jedem `enabled()` – also in jedem `HE_LOG_*`-Aufruf, auch bei deaktiviertem Level – aufgerufen. Ein Namespace-Scope-Array mit `constexpr`-Konstruktor (Startwert `Info`) spart zwei Guard-Prüfungen im Hot Path.
  6. `GlobalState.h:11-13`: Der Header zieht `<nlohmann/json.hpp>` (schwer) und definiert globales `using json = nlohmann::json;`; nur 17 Includer, aber der Alias leakt in deren Namespace. JSON-Zugriff hinter `.cpp` verstecken (`m_customConfig` als `std::unique_ptr<json>`/Pimpl, `setCustomConfigEntry(const std::string&, const json&)` → Overloads für bool/int/float/string).
  7. `TextureArrayBuild.cpp:88-98`: `prev` kopiert Level 0 jeder Schicht zusätzlich zu `data.insert`, und `halve` alloziert pro Mip einen neuen Vektor; direktes Schreiben in `data` (Ziel-Offset per `textureArrayOffset`) vermeidet 1+mips Allokationen/Kopien pro Slice. Editor/Bake-Pfad, nur bei großen Arrays relevant.
  8. `DebugDraw.h:46-66`: `sphere`/`capsule` berechnen `cos/sin` pro Segment doppelt (a1 von Iteration i ist a0 von i+1); die Werte der Vorgängeriteration weiterreichen. Editor-only, `vector::push_back` ohne `reserve` – bei Zeichnung vieler Collider addiert sich das (zu messen).
- **Erwarteter Effekt**: −Code, kleine Einsparungen; keine funktionalen Änderungen.
- **Risiko / Verifikation**: Gering; 2, 3, 5 mit bestehenden Tests (`test_hpak`, Log-Tests) absichern; 6 per Compile aller 17 Includer.

---

## Gelesen

Ja, alle 23 Dateien des Slice 2 vollständig gelesen (`ContentManager.cpp` 1-3687 in Blöcken, `ContentManager.h`, `DefaultAssets.h`, `EngineDependencies.{h,cpp}`, `HAsset.h`, `TextureArrayBuild.{h,cpp}`, `Sha256.{h,cpp}`, `X25519.{h,cpp}`, `DebugDraw.h`, `CrashHandler.{h,cpp}`, `DevStatsTile.{h,cpp}`, `DiagnosticsStructs.h`, `EngineProfiler.{h,cpp}`, `GlobalState.{h,cpp}`, `Log.cpp`); außerhalb des Slice nur gezielte Greps (Log.h-Makros, `pollHotReload`/`discoverAssets`-Aufrufer, `IntegrityProbe`-Aufrufe).

## Core Slice 3 – Diagnostics, Events, GraphCommon, HorizonCode (Interpreter, Klassen-Auflösung, Compiled/GenSupport)

## Zusammenfassung

Der Slice ist insgesamt sehr sorgfältig kommentiert und fachlich konsistent (Semantik-Parität Interpreter/Codegen ist dokumentiert und bewusst). Die relevanten Probleme liegen im HorizonCode-Interpreter (`HorizonCode.cpp`, `HorizonCode.h`) und in der Klassen-Auflösung: Der Interpreter benutzt für jeden Knoten lineare Suchen über `links`/`nodes` und baut dabei pro Zugriff die komplette Pin-Signatur neu auf; `Value` ist 280 Byte groß und wird überall per Wert kopiert (gemessen mit g++ auf Linux: `sizeof(Value)=280`, `sizeof(Node)=336`, `sizeof(Variable)=528`, `sizeof(Context)=1128`); `resolveClass` parst und verschmilzt bei JEDEM Spawn die komplette Klassenkette neu (kein Cache, auch dann, wenn danach die kompilierte Klasse genommen wird). Diagnostics/Events/GraphCommon sind überwiegend unkritisch (Editor-/Ladepfade), mit einigen kleinen Fallstricken (Log-Makros `HE_LOG_ONCE`/`THROTTLE`, `EventBus::publish`-Snapshot, nicht-werfende Loader, die trotzdem werfen können).
Anzahl Funde: 3 Hoch, 11 Mittel, 6 Niedrig (20 gesamt). Alle Messwerte ohne Laufzeit-Profiling; Größenangaben sind echte `sizeof`-Messungen, Zeitgewinne sind Schätzungen.

---

### CORE-301 – Interpreter: lineare Suchen und Pin-Signatur-Neuaufbau bei jedem Knoten und jeder Eingabe
- **ID**: CORE-301
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3822-3827` (`execLinkFrom`), `3934-3955` (`runExecChain`), `4430-4474` (`evalInput`, `inputLinked`), `4232-4234` (FunctionCall-Entry-Suche), `603-643` (`sigScratch`, `signatureCountsOf`, `dataPinDescOf`), `1198-1224` (`pinRanges`, `dataPinType`); `src/HE_Core/include/GraphCommon/GraphModel.h:44-57` (`findNodeById`)
- **Problem**: Pro ausgeführtem Exec-Knoten und pro gelesenem Daten-Eingang laufen mehrere O(Links)- bzw. O(Nodes)-Scans (`execLinkFrom`, `m_graph.findNode`, Schleife über alle `links` in `evalInput`/`inputLinked`) und mehrfach `signatureInto` (ein ~510 Zeilen langer `switch`, der vier `std::vector<PinDesc>` per Initializer-Liste neu beschreibt). Ein einziger `evalInput` ruft `pinRanges(n)`, `pinRanges(*src)` und `dataPinType(n,…)` auf = 3 komplette Signatur-Builds, bei ungebundenem Pin mit Container-Check 3 weitere. Der Kommentar in `GraphModel.h:41` („Graphs are small (tens of nodes)") gilt für den Editor, nicht für Event-Handler, die pro Frame (`Tick`) und pro Instanz laufen. Die Kosten wachsen quadratisch mit der Graphgröße: jeder Knoten × alle Links × (Signatur-Builds pro Treffer).
- **Code-Auszug**:
```cpp
Value Runner::evalInput(const Node& n, int dataInIndex, int depth)
{
    const PinRanges r = pinRanges(n);                 // Signatur-Build #1
    const int pin = r.dataIn0 + dataInIndex;
    for (const auto& l : m_graph.links)               // O(Links)
    {
        if (l.dstNode != n.id || l.dstPin != pin) continue;
        const Node* src = m_graph.findNode(l.srcNode); // O(Nodes)
        ...
        return coerce(evalData(*src, l.srcPin - pinRanges(*src).dataOut0, depth), // Build #2
                      dataPinType(n, true, dataInIndex));                           // Build #3
```
- **Lösung**:
  1. Pro Graph einmal (lazy, z. B. beim ersten `Runner`-Lauf oder beim Laden) einen unveränderlichen `RunIndex` bauen: `std::vector<const Node*> byId` (Ids sind kleine, nie wiederverwendete ints, `nextId` aufwärts → dichter Vektor oder `unordered_map<int,uint32_t>`), pro Node vorberechnete `PinRanges`, pro Node ein Array der Pin-Typen (`dataInTypes`, `dataOutTypes`, `isArray`), pro `(dstNode,dstPin)` den speisenden `Link*` und pro `(srcNode,execOutPin)` den Ziel-Link (z. B. sortierter `std::vector<Link>`-Index oder `unordered_map<uint64_t,int>`).
  2. `Runner` liest nur noch aus dem Index. Der Index lebt neben dem `Graph` (z. B. `std::shared_ptr<const GraphRunIndex>` in `Graph`, bei jeder Mutation `reset()`; der Editor mutiert über `addNode/removeNode/connect`, diese setzen ihn zurück).
  3. `signatureInto` bleibt für Editor/Codegen unverändert.
- **Erwarteter Effekt**: (Schätzung) pro Knoten-Ausführung von O(N+L) mit großer Konstante auf O(1); bei Graphen mit 50-200 Knoten pro Event ein Faktor 5-20 weniger CPU im Interpreter. Messen: `test_horizoncode_runtime.cpp` mit synthetischem 200-Knoten-Tick-Graph, 10 000 Instanzen.
- **Risiko / Verifikation**: Cache-Invalidierung ist die Gefahr (Graph wird in Editor, Collab-Sync `nodeFromJson`, `syncTypeSignatures` etc. mutiert; Runtime hält `levels` je Instanz). Daher Index nur im Runtime-Pfad bauen (nicht für editierbare Graphen) oder strikt über `Graph`-Mutatoren invalidieren. Verhalten bleibt bitgleich; absichern mit den vorhandenen Paritäts-/Trace-Tests (`test_horizoncode_runtime.cpp`, `test_hc_exec_trace.cpp`, `test_horizoncode_codegen.cpp`, `test_hc_breakpoint_play_path.cpp`) plus einem neuen Test „mehrere Links auf einen Eingang / Doppel-Ids nach Merge“.

---

### CORE-302 – `Value` ist 280 Byte groß und wird überall per Wert kopiert
- **ID**: CORE-302
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCode.h:93-130` (Struct), `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3572-3575` (`coerce(Value v, …)`), `4442-4447`, `4489-4492`, `4895-4921`; `HorizonCode.h:1215-1266` (`std::function<Value(…)>` Rückgaben)
- **Problem**: `Value` enthält alle Varianten gleichzeitig (`f`, `d`, `b`, `i`, `v2`, `col`, `v3`, `v4`, `ref`, `tpos/trot/tscl`) plus drei `std::string` (`s`, `keyTypeName`, `typeName`) und zwei `std::vector<Value>` (`items`, `keys`). Gemessen: `sizeof(Value)==280`. Jede Skalar-Operation (`Value::ofFloat(…)`, `coerce`) initialisiert/kopiert/zerstört diese 280 Byte inklusive dreier Strings und zweier Vektoren. `coerce` nimmt `Value` per Wert: bei Lvalue-Aufrufen (`coerce(it->second, …)`, `coerce(m_eventArg, …)`, `coerce(v.items[i], …)`) wird tief kopiert, auch wenn `v.type == want` (dann wird die Kopie zurückgegeben). Ein `Add`-Knoten erzeugt drei Values (2× `evalInput`, 1× `ofFloat`) = ~840 Byte Initialisierung für eine Addition.
- **Code-Auszug**:
```cpp
Value coerce(Value v, PinType want)
{
    if (v.isArray) return v;
    if (v.type == want) return v;
    Value r; r.type = want;
```
- **Lösung**: Stufenweise, ohne Formatänderung:
  1. `coerce` auf `const Value& v` + Ausgabe umstellen (`Value coerce(const Value&, PinType)` und `Value coerce(Value&&, PinType)`-Überladung, die bei Typgleichheit verschiebt). Sofort-Gewinn bei Lvalue-Aufrufern, kein Semantikunterschied.
  2. Mittelfristig `Value` auf ein Tag + `union`/`std::variant`-artiges Layout für Skalare umbauen (float/double/bool/int/ref/vec2/vec3/vec4/transform teilen Speicher) und die selten genutzten Container-/Typnamen-Felder in einen lazy allozierten `std::unique_ptr<ContainerPayload>` auslagern. Das ändert die öffentliche Struktur (`v.f`, `v.col` …) und damit viele Aufrufer (HE_Scene `EngineApi`, UIWidgetBinding, Replikation) – nur mit Accessor-Migration machbar.
  3. Kurzfristig reicht oft: `evalInput`-Fast-Path für `type == want`, der `Value` direkt weiterreicht (siehe CORE-301).
- **Erwarteter Effekt**: (Schätzung) Schritt 1: weniger Kopien um 30-50 % in Container-/Struct-lastigen Graphen; Schritt 2: `sizeof(Value)` ~280 → ~48 Byte, Interpreter-Skalarpfad 2-4× schneller. Messen mit Mikro-Benchmark (1 Mio. `Add`-Auswertungen).
- **Risiko / Verifikation**: Schritt 1 niedrig (rein mechanisch). Schritt 2 hoch: `Value` wird in Replikation, Savegame, Editor-Watch, Codegen-Parität (`hc::raw/toValue`) und `UIWidgetBinding::uiHcValueToProp` feldweise gelesen; siehe `test_horizoncode_containers.cpp`, `test_horizoncode_types.cpp`, `test_horizoncode_compiled.cpp`, `test_hc_watch*.cpp`, Replikationstests. Als eigenes Teilprojekt behandeln.

---

### CORE-303 – `resolveClass` parst und verschmilzt bei jedem Spawn die ganze Klassenkette neu (auch für kompilierte Klassen)
- **ID**: CORE-303
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HcClassResolve.cpp:146-193` (`walk`), `196-224` (`resolveClass`), `306-316` (`contentLoader`); Aufrufer `src/HE_Scene/src/EntityHost.cpp:110-114`, `src/HE_Scene/src/PlayerHost.cpp:91`, `src/HE_Game/src/GameApplication.cpp:1190,1237`
- **Problem**: Jeder Spawn einer Klasse ruft `resolveClassAsset` auf. Das lädt pro Vorfahre den JSON-String (`json = a->graphJson;` Kopie), parst ihn per `fromJson` (inkl. aller Nachbearbeitungs-Pässe, siehe CORE-305/306), kopiert die Graphen mehrfach (`out.levels = a.graphsRootFirst;` Kopie, `out.graph = a.graphsRootFirst.front();` Kopie, `mergeDerivedInto(out.graph, a.graphsRootFirst[i])` nimmt `Graph derived` per Wert = weitere Kopie) und verschmilzt die Kette zu `out.graph`, das die Spawn-Aufrufer gar nicht benutzen (`addLevels(std::move(rc.levels), …)`; `graph` ist laut Header nur für Editor/Codegen). Bei kompilierten Klassen (`compiledClasses().create(assetPath)` → `addCompiled`) wird das gesamte Ergebnis außer `engineBase` und `chain` verworfen. Bei 100 gespawnten Gegnern derselben Klasse = 100 JSON-Parses + 100 Merges. Zusätzlich hält danach jede `Runtime::Inst` ihre eigene Kopie `std::vector<Graph> levels` (`HorizonCodeRuntime.h:651`).
- **Code-Auszug**:
```cpp
    out.levels = a.graphsRootFirst;                 // Kopie
    out.graph = a.graphsRootFirst.front();          // Kopie
    for (size_t i = 1; i < a.graphsRootFirst.size(); ++i)
        mergeDerivedInto(out.graph, a.graphsRootFirst[i]);  // Kopie (by value)
```
- **Lösung**:
  1. Cache `ContentManager`-seitig (oder in einem `ClassResolveCache` in HE_Core): Schlüssel Klassenpfad (+ Content-Version/Reload-Zähler der beteiligten Assets), Wert `std::shared_ptr<const ResolvedClass>`; Invalidierung bei `HorizonCodeClassAsset`-Reload/Änderung (im Editor bei jeder Graph-Bearbeitung/Play-Start).
  2. `Runtime::Inst::levels` auf `std::shared_ptr<const Graph>` (oder `shared_ptr<const std::vector<Graph>>`) umstellen, damit Instanzen nicht jeweils tief kopieren (Cross-Modul, Runtime ist Slice 4).
  3. Kurzfristig und lokal: `merged`-Graph lazy/optional machen (`resolveClass(key, load, bool wantMerged=false)` oder getrennte Funktion `resolveLevels`), `out.levels` erst nach dem Merge verschieben (`mergeDerivedInto(out.graph, std::move(...))` bzw. Parameter `const Graph&`), und in `EntityHost::bindClass` zuerst `compiledClasses().find(assetPath)` prüfen und `chain/engineBase` aus `CompiledInstance::classChain()/baseClassKey()` nehmen statt zu parsen.
- **Erwarteter Effekt**: (Schätzung) Spawn-Kosten pro Instanz von „JSON-Parse + Merge + 3 Graph-Kopien“ auf einen `shared_ptr`-Copy; bei Spawn-Wellen (Projektile, Gegner) sichtbare Frame-Spikes weg. Messen: Spawn von 1000 Instanzen einer abgeleiteten Klasse.
- **Risiko / Verifikation**: Cache-Invalidierung bei Hot-Reload/Editor-Änderung; geteilte `const Graph` darf nie mutiert werden (Runtime schreibt heute in `leaf()` – `HorizonCodeRuntime.h:656` hat nicht-const `leaf()`, prüfen, wer das nutzt). Tests: `test_hc_inheritance.cpp`, `test_horizoncode_runtime.cpp`, `test_horizoncode_compiled.cpp`; neuer Test „zwei Spawns derselben Klasse liefern gleiche Levels, Änderung des Assets invalidiert“. Der Wechsel „Chain aus Compiled statt aus Asset“ nur nach Paritätstest beider Quellen (die Doku nennt Chain als Teil der Kompilat-Metadaten).

---

### CORE-304 – Container-Knoten kopieren den ganzen Container für einen Lesezugriff; ForEach alloziert pro Iteration
- **ID**: CORE-304
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:4610-4619` (`ArrayGet`), `4665-4680` (`ArrayContains/IndexOf`), `4711-4716`, `4794-4809` (Set/Map-Lesen), `4326-4376` (ForEach*), `4132`, `4019-4049` (`unwindFrame`)
- **Problem**: Lesende Knoten holen den Container als `const Value arr = evalInput(…)`. Bei einem `GetVariable`-gespeisten Array bedeutet das: `m_ctx.getVariable(n.s)` liefert bereits eine Kopie (`std::function<Value(const std::string&)>`), `coerce(v, …)` dann ggf. eine zweite (Lvalue). Ein `ArrayGet` in einer Schleife über N Elemente kopiert also N-mal das ganze Array: O(N²) Value-Kopien à 280 Byte. `ForEach` kopiert das Array einmal (`const Value arr = evalInput`) und weist dann pro Iteration `m_execOutputs[n.id] = { arr.items[i], Value::ofInt((int)i) }` zu: Hash-Lookup + `std::vector<Value>`-Allokation mit zwei Values (Element wird erneut kopiert, ggf. Strings/verschachtelte Vektoren) pro Iteration.
- **Code-Auszug**:
```cpp
case T::ArrayGet:
{
    const Value arr = evalInput(n, 0, depth + 1);   // tiefe Kopie
    const int idx = evalInput(n, 1, depth + 1).i;
    if (idx >= 0 && idx < (int)arr.items.size()) return arr.items[idx];
...
for (size_t i = 0; i < arr.items.size(); ++i)
{
    m_execOutputs[n.id] = { arr.items[i], Value::ofInt((int)i) };  // Heap pro Iteration
```
- **Lösung**:
  1. Für `GetVariable`-Quellen einen Referenz-Pfad ergänzen: `const Value* Runner::peekInput(n, idx)` (liefert Zeiger auf Instanz-/Frame-Variable ohne Kopie, sonst `nullptr`), den `ArrayGet/Length/Contains/IndexOf`, `Set*/Map*`-Lesezugriffe zuerst probieren (die Context-Hooks brauchen dafür eine `const Value* getVariableRef(const std::string&)`-Variante).
  2. `ForEach*`: Element/Index in einen wiederverwendeten Slot schreiben (`std::vector<Value>& slot = m_execOutputs[n.id]; slot.resize(2); slot[0] = arr.items[i]; slot[1].i = i;` einmalig außerhalb der Schleife anlegen) – Allokation entfällt, Element-Kopie bleibt (Semantik: Body darf `arr` nicht beeinflussen).
- **Erwarteter Effekt**: (Schätzung) `ArrayGet`-Schleifen O(N²) → O(N); `ForEach` ohne Heap pro Iteration, ~2-3× schneller bei kleinen Elementen.
- **Risiko / Verifikation**: Aliasing: Zeiger auf Variable kann während der Auswertung ungültig werden (Subgraph schreibt Variable) – Zeiger nur direkt vor dem Lesen verwenden, nie über einen weiteren `evalInput` hinweg halten (`ArrayGet` liest Index zuerst). Tests: `test_horizoncode_containers.cpp` (Container-Semantik/Reihenfolge), Paritätstests gegen Codegen.

---

### CORE-305 – `fromJson`/`nodeFromJsonObj`: tiefe JSON-Kopien durch `value()`, `findNode` je Link, lineare Knotentyp-Suche
- **ID**: CORE-305
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:2243-2310` (`fromJson`), `1775-1782` (`nodeTypeFromStoredName`), `1934-2007` (`nodeFromJsonObj`), `2011-2115` (`variableFromJsonObj`)
- **Problem**: (a) `for (const auto& e : j.value("nodes", nlohmann::json::array()))` – `json::value(key, default)` liefert **per Wert**: das gesamte `nodes`-Array (und `links`, `variables`, `events`, `comments`) wird einmal tief kopiert, bevor iteriert wird. Dasselbe in `nodeFromJsonObj` (`e.value("params", json::array())`, `e.value("pos", …)`, `e.value("f", …)`, `e.value("xform", …)`, `e.value("pinDefaults", …)`: Kopie je Knoten). (b) Pro Link zwei `findNode` (O(L·N)) plus eine zweite Schleife für die CreateObject-Migration mit weiteren `findNode`. (c) `nodeTypeFromStoredName` iteriert pro Knoten über alle ~150 `NodeType`s, ruft jeweils den `nodeDisplayName`-`switch` und vergleicht `std::string == const char*`.
- **Code-Auszug**:
```cpp
for (const auto& e : j.value("nodes", nlohmann::json::array()))   // tiefe Kopie
...
bool nodeTypeFromStoredName(const std::string& stored, NodeType& out)
{
    for (NodeType t : nodeRegistry())
        if (stored == nodeDisplayName(t)) { out = t; return true; }
```
- **Lösung**: `if (auto it = j.find("nodes"); it != j.end() && it->is_array()) for (const auto& e : *it)` (kein Kopieren; Zugriffsmuster ist schon in `commentFromJsonObj` mit `e.find("pos")` vorhanden). `static const std::unordered_map<std::string_view, NodeType>` aus `nodeRegistry()` + `kLegacyNodeNames` einmalig aufbauen. Für Links einmal eine `unordered_set<int>` der vorhandenen Node-Ids bzw. ein sortierter Vektor erzeugen statt `findNode` je Link.
- **Erwarteter Effekt**: (Schätzung) Ladezeit eines 500-Knoten-Graphen ~2-3× kürzer, Allokationen deutlich weniger. Wirkt sich verstärkt aus, solange CORE-303 (Parse bei jedem Spawn) nicht gelöst ist.
- **Risiko / Verifikation**: Verhalten identisch (nur Lesepfad). Absichern mit Roundtrip-Tests (`test_horizoncode_types.cpp`, `test_horizoncode_reroute.cpp`, `test_hc_inheritance.cpp`) und einem Test, der ein Asset mit unbekanntem Knotentyp und mit altem Namen „Array Add“ lädt.

---

### CORE-306 – `fromJson`-Nachbearbeitung: mehrere quadratische/kubische Pässe über Nodes × Links
- **ID**: CORE-306
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3242-3296` (`inferUserTypeNames`), `3416-3469` (`traceBody`/`assignSubgraphs`), `1547-1570` (`propagateRerouteTypes`), `2594-2608` (`syncFunctionSignatures`), `2701-2764` (`remapLinksFromSnapshot`)
- **Problem**: `inferUserTypeNames` iteriert bis zum Fixpunkt über alle Nodes, baut je Node `signatureOf(n)` (vier Vektor-Allokationen) und scannt für jeden Node alle Links, davon je Treffer `findNode` + `pinRanges(*peer)` + `dataPinDescOf` → O(Pässe·N·(L·N)). `assignSubgraphs` ruft für jeden Event- und Function-Knoten `traceBody` auf, das pro Stack-Element alle Links scannt und `pinRanges` aufbaut (O(E·N·L)). `propagateRerouteTypes` läuft bis zu `nodes.size()+1` Pässe über alle Links, je Link `findNode`×2 + `dataPinDescOf`. `syncFunctionSignatures` sucht für jeden Call/Return linear die Entry (O(N²)) und kopiert `params/results` auch wenn unverändert. All das läuft bei jedem `fromJson` (und, bis CORE-303 gelöst ist, bei jedem Spawn).
- **Code-Auszug**:
```cpp
for (Node& n : g.nodes)
{
    if (!n.typeName.empty()) continue;
    const NodeSig  mine = signatureOf(n);
    ...
    for (const Link& l : g.links)
    {
        ...
        const Node* peer = g.findNode(weAreSrc ? l.dstNode : l.srcNode);
```
- **Lösung**: Gemeinsamen, einmalig aufgebauten Hilfsindex innerhalb von `fromJson` verwenden (`id→Node*`, `Node-id→eingehende/ausgehende Link-Listen`, Funktionsname→Entry `unordered_map`). Frühausstieg: `inferUserTypeNames` nur für Nodes mit Enum/Struct-Pins (Typ-Whitelist) anstatt für jeden; `propagateRerouteTypes` komplett überspringen, wenn kein Reroute-Node existiert (`any_of`); `syncFunctionSignatures` Entry-Map statt Scan und Vergleich vor Zuweisung.
- **Erwarteter Effekt**: (Schätzung) für typische Graphen (<100 Nodes) 10-40 % der Ladezeit; für große Graphen (500+ Nodes) Faktor >5. Messen mit großem Asset.
- **Risiko / Verifikation**: Reihenfolge der Pässe und die Link-Remap-Semantik exakt bewahren (das Kommentarwerk beschreibt subtile Wechselwirkungen, z. B. „snapshot sonst doppelt remappen“). Tests: `test_horizoncode_types.cpp`, `test_horizoncode_reroute.cpp`, `test_hc_function_rename.cpp`, `test_hc_rename_scope.cpp`.

---

### CORE-307 – `Runner::fireEvent` scannt bei jedem Feuern alle Nodes; InputAction-Knoten allozieren Strings
- **ID**: CORE-307
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3829-3861` (`fireEvent`), `1092-1107` (`inputActionEventNames`, `inputActionChainFor`); `HorizonCode.h:1468`
- **Problem**: `fireEvent(const std::string&, …)` läuft bei jedem Aufruf (z. B. jeden Frame `Tick`, jede Pointer-Bewegung) über alle Nodes. Für jeden `InputAction`-Knoten ruft es `inputActionChainFor` → `inputActionEventNames`, das bis zu zwei `std::string`s per `HE::inputEventPressed/Released(n.s)` zusammensetzt und einen `std::vector<std::string>` zurückgibt (mehrere Heap-Allokationen pro Knoten und Fire), nur um `names[i] == eventName` zu prüfen. Zusätzlich vergleicht jeder Event-Knoten `n.s != eventName` als String. Es gibt keinen Index „Eventname → Einstiegsknoten“, obwohl das Runtime-Layer Events sonst bereits interniert (`EventId`).
- **Code-Auszug**:
```cpp
for (const auto& n : m_graph.nodes)
{
    if (n.type == T::InputAction)
    {
        const int chain = inputActionChainFor(n, eventName);   // baut vector<string>
```
- **Lösung**: Einmal pro Graph (zusammen mit dem Index aus CORE-301) eine Tabelle `eventName/EventId → [(nodeId, execOutPin, elem)]` aufbauen, inkl. vorab berechneter Input-Action-Eventnamen. `fireEvent` schlägt nur noch nach. Wenn das Runtime-Layer schon `EventId` hat, `fireEvent(EventId, …)` ergänzen (Namensversion als Wrapper).
- **Erwarteter Effekt**: (Schätzung) `Tick` auf einer Instanz ohne passenden Handler wird O(1) statt O(N) mit Stringvergleichen; bei Input-Action-Graphen entfallen ~2 Heap-Allokationen pro Knoten und Fire.
- **Risiko / Verifikation**: Reihenfolge mehrerer gleichnamiger Handler und die Mehrfach-Entry-Semantik (`deliverSuspension` je Entry) müssen erhalten bleiben (Tabelle in Knoten-Reihenfolge). Tests: `test_hc_exec_trace.cpp`, `test_hc_breakpoint_play_path.cpp`, `test_horizoncode_shortcuts.cpp`.

---

### CORE-308 – `Context` (1128 Byte, ~35 `std::function`) wird pro Fire gebaut und per Wert weitergereicht
- **ID**: CORE-308
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCode.h:1213-1373` (`Context`), `1463` (`Runner(const Graph&, Context ctx)`), `HorizonCodeCompiled.h:343` (`bindContext(Context ctx)`); Aufrufer `HorizonCodeRuntime.cpp:1565,1633,1694,2087,2114` (Slice 4)
- **Problem**: `sizeof(Context)==1128` (g++/libstdc++, Linux; MSVC/libc++ ähnlich oder größer). Die Runtime erzeugt für jeden Fire/Resume/Inherited-Call per `makeContext(id, level)` einen frischen `Context` (jeder Eintrag ist ein Lambda mit `this`+`id`+`level`, meist >16 Byte → `std::function` allokiert auf dem Heap) und `Runner` nimmt ihn per Wert. Das ist pro Event pro Instanz Dutzende Heap-Allokationen, obwohl der Inhalt bis auf `traceInstance/traceKey/traceLevel` instanzkonstant ist.
- **Code-Auszug**:
```cpp
Runner::Runner(const Graph& graph, Context ctx) : m_graph(graph), m_ctx(std::move(ctx)) {}
...
Runner r(i->levels[lv], makeContext(id, (size_t)lv));   // HorizonCodeRuntime.cpp:1565
```
- **Lösung**: `Runner` hält eine `const Context&` (Runtime besitzt pro Instanz/Level einen langlebigen, einmal gebauten `Context`; die Lifetimes sind klar: der Runner lebt nur für die Dauer eines Aufrufs). Alternativ `Context` in eine kleine Schnittstelle (`struct IHost { virtual … }` mit einem Zeiger + `instanceId`) umbauen und die 35 `std::function` durch virtuelle Aufrufe ersetzen – spart Heap und Speicher, ändert aber die API (Tests/Codegen/GenSupport benutzen `c.getProperty(...)` etc.).
- **Erwarteter Effekt**: (Schätzung) pro Event 20-40 Heap-Allokationen weniger; relevante Entlastung bei vielen Instanzen mit Tick/Overlap-Events. Messen: Allokationszähler in `fireTick` über 10 000 Instanzen.
- **Risiko / Verifikation**: Lifetime der Lambdas (`this`, Instanz-Referenz) bei Instanz-Zerstörung während des Laufs; `Runner` kann `destroyObject` aufrufen → Context darf nicht mit dem Inst-Container verschwinden. Tests: `test_horizoncode_runtime.cpp`, `test_hc_on_changed.cpp`, `test_hc_bind_to.cpp`. Unsicher / messen: tatsächliche Zahl der Allokationen hängt von der Standardbibliothek-SBO ab.

---

### CORE-309 – GenSupport: Array-/Set-/Map-Operationen nehmen den Container per Wert (O(n) Kopie je Operation)
- **ID**: CORE-309
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCodeGenSupport.h:323-335` (`arrAdd/arrSet/arrInsert/arrRemove`), `399-410` (`setAdd/setRemove`), `457-473` (`mapSet/mapRemove`), `415` (`setToArray`), `488-499` (`mapKeys/mapValues`)
- **Problem**: Jede Operation nimmt den Container per Wert und gibt ihn per Wert zurück (Copy-Semantik des Interpreters, bewusst). Die generierte Schleife `for … arr = hc::arrAdd(arr, x)` kopiert dadurch pro Iteration den gesamten Vektor, wenn der Codegen kein `std::move` emittiert. Das macht aus einem Append-in-Schleife O(n²) – in genau dem Code, der laut Doku die Performance-Variante des Interpreters sein soll. Ob der Codegen schon `std::move` setzt, ist in diesem Slice nicht sichtbar (Codegen ist `HE_Tools/hc_codegen`).
- **Code-Auszug**:
```cpp
template <typename T> inline Array<T> arrAdd(Array<T> a, const Same<T>& v)
{ a.push_back(v); return a; }
```
- **Lösung**: Die Signaturen sind bereits „sink“-Parameter und damit korrekt für `hc::arrAdd(std::move(arr), x)`. Der Codegen sollte den letzten Lesezugriff auf die lokale Variable/Zwischenvariable erkennen und `std::move` emittieren (bzw. den Wert direkt in den temporären Ausdruck einsetzen). Hier im Header nichts ändern; optional `static_assert`/Kommentar „callers must move“.
- **Erwarteter Effekt**: (Schätzung) Append-/Set-Schleifen von O(n²) auf O(n) in generiertem Code.
- **Risiko / Verifikation**: Move von Variablen ändert die Semantik, wenn dieselbe Variable später noch gelesen wird (Interpreter „Copy-Semantik“): nur bei bewiesener letzter Verwendung. Absichern über die Paritätstests `test_horizoncode_codegen.cpp` / `test_horizoncode_compiled.cpp`.

---

### CORE-310 – GenSupport: namensbasierte Zugriffe konstruieren `std::string` und suchen linear
- **ID**: CORE-310
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCodeGenSupport.h:648-664` (`getProperty/setProperty/getVariableCtx/setVariableCtx`), `686-693` (`getExternal/setExternal/bindEvent/emitEvent`), `709-713` (`callExternal/callApi`), `859-899` (`findSlot/getVar/setVar/collectVarRefs`); `HorizonCode.h:1215-1266` (Context-Signaturen `const std::string&`)
- **Problem**: Alle `Context`-Callbacks verlangen `const std::string&`, die generierten Aufrufer reichen `const char*` (String-Literal). Jeder Aufruf konstruiert also ein temporäres `std::string` (Heap-Allokation bei Namen > SSO-Grenze, z. B. 15 Zeichen unter libstdc++/MSVC; Property-Namen wie `"backgroundColor"` liegen genau an der Grenze). `findSlot` vergleicht linear `name == s.name` für jeden `GetExternal/SetExternal`, jeden `getVariable`-Aufruf der Runtime und jeden GC-Scan. `collectVarRefs` ruft `s.get(self)` und baut dadurch für jede Ref-/Map-Variable eine vollständige `Value`-Kopie (inkl. kompletter Arrays), nur um Refs herauszulesen.
- **Code-Auszug**:
```cpp
inline Value getVariableCtx(const Context& c, const char* name)
{ return c.getVariable ? c.getVariable(name) : Value{}; }   // std::string-Temporary
...
inline const VarSlot* findSlot(const VarSlots& slots, const std::string& name)
{
    for (const VarSlot& s : slots) if (name == s.name) return &s;
```
- **Lösung**: In `Context` zusätzlich `std::string_view`-Varianten (oder interned `NameId`) anbieten; `GenSupport` ruft zuerst diese. `findSlot`: einmalig beim Aufbau (`concatSlots`) einen `unordered_map<std::string_view, uint32_t>` bzw. sortierten Vektor mit `string_view`-Suche halten. `collectVarRefs`: pro Slot einen typed `collectRefs`-Callback (`void(*)(const CompiledInstance*, std::vector<uint32_t>&)`) analog zu `get/set`, damit keine `Value`-Kopie nötig ist.
- **Erwarteter Effekt**: (Schätzung) pro Property-/Variablenzugriff in generiertem Code 1 Heap-Allokation weniger bei langen Namen; GC-Scan ohne Array-Kopien. Messen: Allokationszähler im Tick einer generierten Klasse.
- **Risiko / Verifikation**: API-Erweiterung additiv (alte Signaturen bleiben als Fallback, wie bei `emitEventId`). Tests: `test_horizoncode_compiled.cpp`, `test_horizoncode_codegen.cpp`.

---

### CORE-311 – `EventBus::publish` kopiert bei jedem Publish die Handlerliste (inkl. `std::function`s)
- **ID**: CORE-311
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Events/EventBus.h:81-92`, `61-78`
- **Problem**: `publish` macht `auto snapshot = it->second;` – eine Kopie des `std::vector<Entry>` samt jeder `std::function` (Heap-Allokation für Vektor und für jeden Handler mit Capture > SBO) bei jedem Publish, nur damit Handler während des Dispatch subscriben/unsubscriben dürfen. Das ist die öffentliche Gameplay-API („typed pub/sub bus for game code“), also potenziell pro Frame und pro Ereignis. Die Hash-Suche über `std::type_index` kommt dazu.
- **Code-Auszug**:
```cpp
// Copy list so handlers can safely subscribe/unsubscribe during dispatch.
auto snapshot = it->second;
for (auto& entry : snapshot)
    entry.fn(&event);
```
- **Lösung**: Per Index und Größe iterieren, Mutation während des Dispatch über einen `m_dispatchDepth`-Zähler abfangen: `subscribe` hängt hinten an (Größe am Start merken → neue Handler feuern erst beim nächsten Publish, wie heute), `unsubscribe` markiert den Eintrag (`id = 0`) und kompaktiert erst bei Tiefe 0. Handler als `std::shared_ptr<const std::function>` oder per `unique_ptr` stabil halten, wenn Reallokation während des Dispatch möglich bleibt. Beobachtbares Verhalten (neue Handler in diesem Publish nicht aufgerufen; zwischenzeitlich entfernte Handler werden – wie heute – noch aufgerufen, falls im Snapshot) beibehalten oder bewusst anpassen und dokumentieren.
- **Erwarteter Effekt**: (Schätzung) pro Publish 1 + #Handler Heap-Allokationen weniger.
- **Risiko / Verifikation**: Subtile Semantik „entfernter Handler im laufenden Dispatch wird noch gerufen“ (Snapshot) ändert sich bei der Markierungs-Variante. Der Header sagt, nur die Tests nutzen den Bus – dort (`tests/` EventBus-Test; per `grep EventBus tests` zu finden) Fälle „unsubscribe im Handler“, „subscribe im Handler“ prüfen.

---

### CORE-312 – Log-Wiederholungs-Makros: Zustand wird vor dem Filter verbraucht / Uhr wird vor dem Filter gelesen
- **ID**: CORE-312
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Core/include/Diagnostics/Log.h:272-277` (`HE_LOG_ONCE`), `280-287` (`HE_LOG_EVERY_N`), `290-300` (`HE_LOG_THROTTLE`); Kopfkommentar `25-29`
- **Problem**: Der Kopfkommentar verspricht, dass bei ausgefiltertem Level nur „one predictable branch“ anfällt. Bei den Varianten stimmt das nicht: `HE_LOG_ONCE` setzt das Flag (`exchange(true)`) **vor** dem `enabled`-Test – wird die Kategorie später per `configureFromString`/Konsole hochgedreht, erscheint die Meldung nie mehr (Funktionsfehler). `HE_LOG_THROTTLE` ruft `monotonicMillis()` (exportierte Funktion, `steady_clock::now()`) bei jedem Aufruf, auch wenn die Kategorie ausgeschaltet ist; `EVERY_N` macht einen atomaren `fetch_add` pro Aufruf. Im Repo gibt es ~90 Verwendungen von `THROTTLE/EVERY_N`, viele vermutlich in Frame-Schleifen.
- **Code-Auszug**:
```cpp
#define HE_LOG_ONCE(cat, level, ...)
	do {
		static ::std::atomic<bool> heLogOnceFired_{false};
		if (!heLogOnceFired_.exchange(true, ::std::memory_order_relaxed))
			HE_LOG(cat, level, __VA_ARGS__);
	} while (0)
...
		const int64_t heNow_ = ::HE::Log::detail::monotonicMillis();
```
- **Lösung**: Reihenfolge umkehren: erst `if (::HE::Log::enabled(...))`, dann Once/Counter/Zeit prüfen (bei ONCE: `if (enabled && !exchange(true))`). Beobachtbares Verhalten bleibt für aktivierte Kategorien gleich; für deaktivierte entfällt Uhr/Atomic, und ONCE geht nicht mehr verloren.
- **Erwarteter Effekt**: (Schätzung) pro ausgefiltertem THROTTLE-Aufruf ~20-30 ns Uhr + Cross-DLL-Call gespart; Korrektheit von ONCE.
- **Risiko / Verifikation**: EVERY_N zählt künftig nur noch aktivierte Treffer (Trefferzahl in der Meldung ändert sich bei Zwischenzeitlich-Aktivierung) – akzeptabel, dokumentieren. `tests/test_logging.cpp` ergänzen: „ONCE bei Off, dann aktivieren, dann erneut“.

---

### CORE-313 – „Nicht-werfende“ Loader werfen bei Typfehlern doch (`value()`/`get<T>()`)
- **ID**: CORE-313
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/GraphCommon/GraphJson.h:69-75` (`linkFromArray`), `19-28` (Kommentar „must not throw“); `src/HE_Core/src/HorizonCode/HorizonCode.cpp:1934-2007`, `2011-2115`, `1724-1755`; `src/HE_Core/src/Diagnostics/ProfilerCaptureFile.cpp:101-167`
- **Problem**: `parseGraphObject` parst ohne Exceptions und der Kommentar begründet das mit einer früheren Crash-Schleife. Danach greifen die Lader aber mit `e.value("id", 0)`, `j[0].get<int>()`, `p[0].get<float>()` zu: nlohmann wirft `type_error.302`, wenn der Schlüssel existiert, aber einen anderen Typ hat (z. B. `"id": "abc"` oder ein String statt Zahl in einem Link-Array). Eine beschädigte oder handeditierte `.hasset` kann so aus `fromJson` herauswerfen – genau der Fall, den die Header-Doku ausschließen will. `LoadedCapture::load` fängt nur das `operator>>` (Zeilen 63-72); alle `jf.value(…)`-Zugriffe danach sind ungeschützt.
- **Code-Auszug**:
```cpp
inline bool linkFromArray(const nlohmann::json& j, int& sn, int& sp, int& dn, int& dp)
{
    if (!j.is_array() || j.size() < 4) return false;
    sn = j[0].get<int>(); sp = j[1].get<int>();   // wirft bei String/null
```
- **Lösung**: Entweder (a) den gesamten Dokument-Decode in `try { … } catch (const nlohmann::json::exception&) { return false; }` fassen (eine Stelle je Loader: `fromJson`, `nodeFromJson`, `variableFromJson`, `LoadedCapture::load`), oder (b) kleine Helfer `getOr<T>(j, key, def)` mit `is_number`-Check einführen. (a) ist die einfachste Lösung und lässt alle Lesepfade unverändert.
- **Erwarteter Effekt**: Robustheit; keine Laufzeitänderung auf dem Gutpfad (try-Block kostenlos auf modernen ABIs).
- **Risiko / Verifikation**: Teil-geladenes Ergebnis darf nicht in `out` landen (heute wird in lokale `Graph g` gebaut und erst am Ende verschoben – bleibt so). Test: Graph-JSON mit `"id":"x"` und Link-Eintrag `["a",1,2,3]` darf nicht werfen, Rückgabe `false`/Eintrag übersprungen.

---

### CORE-314 – Capture-Laden: DOM + drei Sortierungen derselben Daten + String-Schlüssel je Sample
- **ID**: CORE-314
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/ProfilerCaptureFile.cpp:60-67`, `110-125`, `203-218`, `16-24`; `src/HE_Core/include/Diagnostics/ProfilerStats.h:94-103`, `143-155`, `206-234`, `238-260`
- **Problem**: (a) `in >> j` baut das komplette DOM der Capture-Datei im Speicher auf und die Strukturen werden danach ein zweites Mal gefüllt (Spitzenverbrauch ≈ 2× Dateigröße, mal nlohmann-Overhead). (b) Aus `deltas` werden `computePercentiles(deltas)`, `computeFpsLows(deltas)` und `findHitches(frames)` unabhängig berechnet – jedes kopiert und sortiert dieselben Frame-Zeiten neu. (c) `accumulateFrameScopes` konstruiert pro Scope-Sample `const std::string key(s.name ? s.name : "?");` (Heap bei Namen > 15 Zeichen) und schlägt danach zweimal in `unordered_map<std::string,…>` nach (`agg[key]`, `samples[key]`), plus `intern(js.value("n", std::string("?")))` je Sample (temporäres Default-`std::string`, Hash-Lookup). Bei großen Captures (zehntausende Frames × hunderte Scopes) sind das Millionen Allokationen und Hashes. Ob das Laden im UI-Thread blockiert, ist aus diesem Slice nicht ersichtlich (Aufrufer im Editor).
- **Code-Auszug**:
```cpp
const std::string key(s.name ? s.name : "?");
ScopeAggregate& a = agg[key];
...
samples[key].push_back(s.ms);
```
- **Lösung**: (b) `deltas` einmal sortieren und an `percentileOfSorted`/`FpsLows`/Median weiterreichen (Überladungen `...FromSorted`). (c) Schlüssel `std::string_view` auf `s.name` (statische bzw. internierte Strings, siehe Lifetime-Kommentar in `ProfilerCaptureFile.h`) verwenden, ein einziges Map-Lookup auf `struct { ScopeAggregate agg; std::vector<double> samples; }`. In `intern` `std::unordered_set<std::string>` statt `deque`+`map` (Knoten-stabil, kein doppeltes Halten der Strings). (a) Für sehr große Dateien SAX-Parser / `json::parse` mit Callback oder Warnung ab Größe X; nur nötig, wenn Messung Probleme zeigt.
- **Erwarteter Effekt**: (Schätzung) Ladezeit großer Captures 2-4× kürzer, Speicherspitze −30 %.
- **Risiko / Verifikation**: Ergebnisse müssen bitgleich bleiben (Sortier-Stabilität/Tie-Breaker `name` im Aggregat-Sort bleibt). `tests/test_profiler_stats.cpp`, `tests/test_profiler.cpp` (Dump→Load-Roundtrip). Unsicher / messen: Dateigrößen realer Captures.

---

### CORE-315 – `CompiledInstance`-Standard-Hooks bauen pro Aufruf `std::string` + 280-Byte-`Value` und gehen über `fireEvent` per Namen
- **ID**: CORE-315
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCodeCompiled.h:166-169`, `172-305`, `319-320`
- **Problem**: Die Default-Implementierungen aller Hooks lauten z. B. `virtual void onTick(float dt) { fireEvent("Tick", 0, Value::ofFloat(dt)); }`. Für Klassen, die nur *einen Teil* der Hooks überschreiben (laut Kommentar der Normalfall), erzeugt jeder nicht überschriebene Hook pro Aufruf ein `std::string` aus dem Literal und ein `Value` (280 Byte), ruft die virtuelle `fireEvent` – deren Standard ist ein No-Op (Zeile 319-320). Bei `onTick` ist das pro Frame und Instanz. Ob die Runtime bei bekanntem Fehlen eines Handlers den Hook gar nicht erst aufruft, liegt außerhalb des Slices.
- **Code-Auszug**:
```cpp
virtual void onTick(float dt) { fireEvent("Tick", 0, Value::ofFloat(dt)); }
virtual void fireEvent(const std::string& name, int elem, const Value& arg)
{ (void)name; (void)elem; (void)arg; }
```
- **Lösung**: Eine Methode `virtual bool handles(EventId)`/Bitmaske `handledHooks()` (vom Codegen generiert, ist bereits aus `eventInfos()` ableitbar) anbieten; die Runtime ruft `onTick` nur, wenn der Hook belegt ist. Alternativ `fireEvent(const char*, …)` mit Literal ohne `std::string`.
- **Erwarteter Effekt**: (Schätzung) pro Frame und Instanz ohne Tick-Handler ~50-100 ns gespart; bei 10 000 Instanzen ~0.5-1 ms.
- **Risiko / Verifikation**: Handgeschriebene `CompiledInstance`s (Tests) die nur `fireEvent` überschreiben dürfen weiter funktionieren → Default muss „alles handled“ sein. `test_horizoncode_compiled.cpp`.

---

### CORE-316 – Zu lange Funktionen und wiederholte Muster im Interpreter
- **ID**: CORE-316
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:89-601` (`signatureInto`, ~510 Zeilen), `4095-4428` (`execNode`, ~330), `4476-5078` (`evalData`, ~600); Wiederholungen: `4224-4226`, `4243-4245`, `4270-4272`, `4283-4284`, `4320-4322`, `4981-4983` (Argument-Auswertung), `3892-3902` vs. `4279-4287` (Frame-Aufbau), `4326-4376` + `4019-4049` (Loop-Varianten), `4692-4780` (`isArray=true; container=…; type=…`-Re-Stamping 8×), `4810-4842` (`MapKeys/MapValues/MapBreak`)
- **Problem**: Drei Riesen-Switches, die jeweils mehrere Verantwortlichkeiten mischen; sechs Kopien der Schleife „Parameter auswerten und coercen“; `callFunction` und `FunctionCall` bauen den `CallFrame` getrennt (nur in der Reihenfolge von Zeilen verschieden, Gefahr des Auseinanderlaufens, wie der Kommentar in `execNode` selbst andeutet); `ForEach/ForEachSet/ForEachMap` und `unwindFrame` enthalten dieselbe Schleife dreimal; die kleinen Makros `HC_SET_PIN/HC_MAP_PIN/HC_KEY_PIN` (Zeilen 334-387) sind lokal und korrekt `#undef`et, ließen sich aber als `auto`-Lambdas/`constexpr`-Funktionen ausdrücken.
- **Code-Auszug**:
```cpp
std::vector<Value> args(n.params.size());
for (size_t i = 0; i < n.params.size(); ++i)
    args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);   // 6× im File
```
- **Lösung**: Hilfsfunktionen `evalArgs(const Node&, int firstPin, const std::vector<FuncParam>&, int depth)`, `makeFrame(const Node& entry, std::vector<Value> args)`, `runLoop(n, items/keys, r, depth)` und `restamp(Value&, ContainerKind, PinType)`; `execNode`/`evalData` in Gruppen (Variablen, Container, Funktionen, Vektoren) mit je eigener Funktion aufteilen. Keine Verhaltensänderung.
- **Erwarteter Effekt**: Lesbarkeit/Wartbarkeit; weniger Divergenzrisiko zum Codegen. Keine Performance-Änderung zu erwarten.
- **Risiko / Verifikation**: Rein mechanisch; Auswertungsreihenfolge der Eingänge (Seiteneffekte, Trace) muss gleich bleiben – Tests: `test_hc_exec_trace.cpp`, `test_horizoncode_runtime.cpp`, Paritätstests.

---

### CORE-317 – `Logger::setSink`: Race bei zwei parallelen Installationen, komplizierter Handle-Tanz
- **ID**: CORE-317
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Diagnostics/Logger.cpp:25-77`
- **Problem**: `setSink` entscheidet unter dem Lock, ob ein Add nötig ist (`handleToAdd = 1`), führt `addSink` aber erst außerhalb des Locks aus und schreibt `g_legacyHandle` danach. Rufen zwei Threads gleichzeitig `setSink(non-null)` auf, sehen beide `g_legacyHandle == 0` und installieren zwei Bridges (doppelte Zustellung, ein Handle geht verloren). `handleToAdd` ist ein Flag mit dem Wert `1` statt `bool`. Der Aufwand (Mutex + zwei Handle-Variablen) steht in keinem Verhältnis zu dem, was nötig wäre.
- **Code-Auszug**:
```cpp
else if (sink && !g_legacyHandle)
{
    handleToAdd = 1;
}
...
const int h = HE::Log::addSink(&legacyBridge, nullptr);
std::lock_guard<std::mutex> lk(g_legacySinkMutex);
g_legacyHandle = h;
```
- **Lösung**: Die Bridge einmal dauerhaft registrieren (`static const int h = HE::Log::addSink(&legacyBridge, nullptr);` in `setSink`) und `g_legacySink`/`g_legacySinkUser` als ein `std::atomic<const Slot*>` bzw. unter dem bestehenden Mutex halten; `nullptr` setzt nur den Zeiger zurück. Keine `removeSink`-Aufrufe, keine Handle-Variablen. Die Bridge ist billig, wenn kein Sink gesetzt ist (ein atomarer Load).
- **Erwarteter Effekt**: Entfernt ein Race und ~15 Zeilen.
- **Risiko / Verifikation**: Bridge läuft dauerhaft mit (ein zusätzlicher Funktionsaufruf pro Log-Record unter dem Log-Mutex). Test: `tests/test_logging.cpp` – Sink setzen/entfernen/neu setzen, doppelte Zustellung ausschließen.

---

### CORE-318 – `Subscription` hält einen rohen `EventBus*` (Dangling, wenn der Bus zuerst stirbt)
- **ID**: CORE-318
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/Events/EventBus.h:22-47`, `141-152`
- **Problem**: `~Subscription()` ruft `release()` → `m_bus->unsubscribe(...)` auf einen eventuell bereits zerstörten Bus (Reihenfolge-Abhängigkeit bei Mitgliedern/Statics, z. B. Bus als Member des Spiel-Objekts, Subscription in Systemen, die länger leben). Das Verhalten ist in der Header-Doku nicht erwähnt. Die Move-Zuweisung ist selbstzuweisungs-unsicher (`x = std::move(x)` ruft `release()` und kopiert dann Nullzustand – harmlos, aber unbeabsichtigt).
- **Code-Auszug**:
```cpp
inline void Subscription::release()
{
    if (m_bus) { m_bus->unsubscribe(m_type, m_id); m_bus = nullptr; }
}
```
- **Lösung**: Entweder dokumentieren („Subscription darf den Bus nicht überleben“) oder `EventBus` mit einem `std::shared_ptr<bool> m_alive` ausstatten, den die Subscription als `weak_ptr` hält. Move-Zuweisung mit `if (this != &o)` absichern.
- **Erwarteter Effekt**: Vermeidet use-after-free in Abbau-Reihenfolgen; sonst keiner.
- **Risiko / Verifikation**: Gering. Test: Bus vor Subscription zerstören (mit ASan).

---

### CORE-319 – `HE::Log::enabled` ist ein exportierter Nicht-Inline-Aufruf pro Log-Stelle
- **ID**: CORE-319
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Diagnostics/Log.h:107`, `256-261`; `src/HE_Core/src/Diagnostics/Log.cpp:382-390`
- **Problem**: Jede `HE_LOG_*`-Stelle (≈ 114 TRACE/DEBUG-Stellen im Repo, dazu alle anderen) ruft `Log::enabled` als echten Funktionsaufruf über die DLL-Grenze (Windows: Import-Thunk) mit Bereichsprüfung, Funktions-Static-Guard (`verbosities()`) und zwei atomaren Loads. Der Header-Kommentar verspricht „one predictable branch“. Für ausgefiltertes Trace/Debug in Hot Paths ist ein Call + 2 Loads vermeidbar.
- **Code-Auszug**:
```cpp
bool enabled(Cat cat, Level level)
{
    const int i = static_cast<int>(cat);
    if (i < 0 || i >= kCatCount) return false;
    ...
```
- **Lösung**: Nur messen, wenn überhaupt. Bei Bedarf: eine einzige `HE_API const std::atomic<uint8_t>* effectiveThresholds()`-Adresse einmal pro TU cachen (`static const auto* p = ...;`) und im Makro inline `lv >= p[cat]` prüfen, wobei der effektive Schwellwert (`max(catVerbosity, globalFloor)`) bei jeder Änderung in `setVerbosity` vorberechnet wird. Der Kommentar „kein exportiertes Datum“ bleibt gewahrt, weil nur ein Zeiger geholt wird.
- **Erwarteter Effekt**: (Schätzung) wenige ns pro ausgefilterter Stelle – nur relevant bei dichten Trace-Logs in Schleifen; ohne Messung nicht umsetzen.
- **Risiko / Verifikation**: Niedrig. Unsicher / messen: Mikrobenchmark 100 Mio. ausgefilterte `HE_LOG_TRACE`.

---

### CORE-320 – Kleinkram: veraltete/verirrte Kommentare, ignorierte Rückgaben, kleine O(n²)-Stellen, Header-Gewicht
- **ID**: CORE-320
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: siehe Liste
- **Problem**: 
  1. `HorizonCode.h:962-976`: zwei Doku-Blöcke (zu `syncTypeSignatures` und `inferUserTypeNames`) stehen verwaist vor `engineEvents`, die zugehörigen Deklarationen sind erst in Zeilen 1116 und 1162.
  2. `HorizonCodeGenSupport.h:249` („variableDefaultValue :702“) und `676` („logs the fail like :1019“) verweisen auf Zeilennummern, die längst nicht mehr stimmen; Zeilennummern in Kommentaren verrotten.
  3. `HcClassResolve.cpp:176`: `if (!json.empty()) fromJson(json, g);` ignoriert den Rückgabewert; ein defektes Asset wird still zur leeren Klasse (kein Log).
  4. `HcClassResolve.cpp:94-99`: pro abgeleitetem Entry-Knoten Schleife über alle Basis-Knoten + je Treffer `removeMember` mit vollen `erase/remove_if`-Pässen über `nodes` und `links` (O(E·(B+N+L))); `overridableMembers/inheritedVariables/inheritedFunctions` prüfen Duplikate per `any_of` über `out` (O(n²)). Nur Lade-/Editor-Zeit, Größen klein.
  5. `ProfilerCaptureFile.cpp:183-197`: `std::any_of` über alle `threads` pro `summary.threads`-Eintrag (O(T²), T klein).
  6. `ProfilerCaptureFile.cpp:226-228`, `HcCompiledLoader.cpp:20-22,28-31,41-44,51-52`, `HorizonCodeGenSupport.cpp:29-31,41-43`: Muster `HE_LOG_*(cat, "%s", (a + b + c).c_str())` baut die Meldung per String-Konkatenation, obwohl das Log-System printf-Formatierung hat (`"… %s …", x.c_str()`); funktional gleich, aber unnötige Temporaries. Nur an Fehlerpfaden → Lesbarkeit.
  7. `HorizonCode.h:4`: `#include <glm/glm.hpp>` (der gesamte glm-Kern) in einem Header, den jede generierte Klasse über `HorizonCodeGenSupport.h` einbindet; `Value` braucht nur `vec2/3/4` (`glm/vec2.hpp`, `vec3.hpp`, `vec4.hpp`) → kürzere Compile-Zeiten.
  8. `Profiler.h:46-49`: unter `TRACY_ENABLE` expandieren `HE_PROFILE_SCOPE()`/`_N`/`_DYN` zu zwei Anweisungen ohne Klammerung (`ZoneScoped; HE_PROF_SCOPE_IMPL(...)`) – in `if (x) HE_PROFILE_SCOPE();` ohne Klammern wirkt nur die erste. Und `HE_PROFILE_FRAME()` ist ohne Tracy leer, aber ohne Semikolon-Absicherung (`((void)0)`).
  9. `GraphModel.h:36-71`: vier `findNodeById`-Überladungen (value/unique_ptr × const/non-const) plus vier `nodeAddr`-Überladungen; ließe sich mit einer einzigen Template-Funktion über `decltype(nodeAddr(*nodes.begin()))` als Rückgabetyp und `auto&&`-Parameter halbieren. Nur Vereinfachung, geringer Nutzen.
- **Code-Auszug**:
```cpp
if (!json.empty()) fromJson(json, g);   // Ergebnis ignoriert
```
- **Lösung**: Je Punkt wie beschrieben; Punkt 3 mit `HE_LOG_WARN(HorizonCode, "class '%s': graph JSON invalid", cur.c_str())`, 7 Includes verschlanken, 8 die Makros als `do {} while` geht nicht (Scope-Objekt) → Kommentar „nur als eigenständige Anweisung verwenden“.
- **Erwarteter Effekt**: Wartbarkeit, minimal kürzere Compile-Zeit (Punkt 7).
- **Risiko / Verifikation**: Gering; Punkt 3 ändert nur Logging; Punkt 7 per Build aller Targets prüfen (Aufrufer verlassen sich evtl. transitiv auf `glm.hpp`).

---

## Gelesen

Dateien vollständig gelesen: ja – alle 21 Dateien des Slice (Log.h, Logger.cpp/.h, Profiler.h, ProfilerCaptureFile.cpp/.h, ProfilerStats.h, EventBus.cpp/.h, GraphJson.h, GraphModel.h, HcClassResolve.cpp/.h, HcCompiledLoader.cpp/.h, HorizonCode.cpp (1-5080), HorizonCode.h (1-1551), HorizonCodeCompiled.cpp/.h, HorizonCodeGenSupport.cpp/.h); zusätzlich punktuell `Log.cpp:382-390,696-700`, `HorizonCodeRuntime.cpp` (Aufrufer), `EntityHost.cpp:98-130` und `sizeof`-Messung per g++ zur Verifikation.

## Core Slice 4 – HorizonCodeRuntime, GameServices, Hpak (Aes/Reader/Writer/Config/Exporter), IGameLogic, IntegrityProbe, JobSystem, AutoLandscapeMaterial

## Zusammenfassung

Der Slice ist insgesamt sorgfältig und gut kommentiert; die Parallelitätslogik im JobSystem (CAS-Claim, Cancel, Dependencies) ist korrekt durchdacht und enthält keinen offensichtlichen Fehler. Die relevanten Befunde liegen an drei Stellen. (1) Der HorizonCode-Interpreter baut pro Event und pro Funktionsaufruf einen kompletten `Context` (37 `std::function`-Member) und sucht den behandelnden Level linear durch alle Knoten. Das läuft für `Tick` jeder Widget-, Entity- und Player-Instanz in jedem Frame. (2) Der Pak-Lesepfad hasht jeden gelesenen Eintrag mit byteweisem FNV-1a. Gemessen sind das 664 MB/s und damit der langsamste Schritt, noch vor Entschlüsseln und LZ4. (3) Der Pak-Writer/Exporter arbeitet strikt sequenziell, hält das gesamte Projekt im RAM und kopiert jedes Asset mehrfach. Das ist Editor/Export-Zeit, kein Frame-Pfad. Dazu kommen Duplikate und Vereinfachungspotenzial (MTRL-Layout dreifach, Dispatch-Signaturprüfung doppelt, Öffentlich-Variable-Prüfung fünffach).

Funde: **3 Hoch, 12 Mittel, 11 Niedrig** (26 gesamt). Verifiziert wurde nur durch Lesen. Einzige Messung ist der FNV-Durchsatz (g++ -O2, 4 Kerne, 256 MiB: 664 MB/s). Alle anderen Aufwandszahlen sind Schätzungen und entsprechend markiert.

---

## Funde

### CORE-401 – Pro Event/Funktionsaufruf wird ein kompletter `Context` (37 `std::function`) neu gebaut
- **ID**: CORE-401
- **Kategorie**: B / C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1338-1586` (`makeContext`), `:2082-2089` (`runEventOnLevel`), `:1322-1336` (`classKeyAtLevel`), `:1565`/`:1633`/`:1694`/`:2114` (Aufrufer)
- **Problem**: Jeder interpretierte Aufruf (`fireTick`, `fireOnClicked`, `callFunction`, `callOwn`, Delay-Resume, `resumeRun`) erzeugt über `makeContext` ein neues `Context` und übergibt es dem `Runner` per Wert (`HorizonCode.h:1463: Runner(const Graph&, Context)`). Der `Context` enthält rund 37 `std::function`-Member, jeweils mit eigener Lambda-Konstruktion. Drei davon (`scheduleResume`, `getNodeState`, `setNodeState`) erfassen `[this,id,level]` = 24 Byte und landen je nach STL im Heap (libstdc++: ja, libc++/MSVC: wahrscheinlich im SBO). Zusätzlich liefert `classKeyAtLevel` einen `std::string` per Wert (Asset-Pfad, meist länger als SSO), der in `ctx.traceKey` kopiert wird. Der Context ist dabei zustandslos: der Kommentar `:1467-1472` sagt ausdrücklich, dass Services zur Aufrufzeit durchgereicht und nie kopiert werden. Der kompilierte Pfad macht es bereits richtig und bindet seinen Context einmal (`:150`, `bindContext(makeContext(id))`). Aufrufer pro Frame: `WidgetManager.cpp:2769,2773`, `PlayerHost.cpp:324`, `EntityHost.cpp:263`.
- **Code-Auszug**:
```cpp
void Runtime::runEventOnLevel(Inst& i, InstanceId id, const std::string& event, int elem, const Value& arg)
{
    const int lv = levelHandlingEvent(i, event, elem);
    if (lv < 0) return;
    Runner r(i.levels[lv], makeContext(id, (size_t)lv));   // 37 std::function + string-Kopie, pro Event
    r.fireEvent(event, elem, arg);
}
```
- **Lösung**: (1) Pro `Inst` und Level einen lazy erzeugten, gecachten `Context` halten (`std::vector<std::unique_ptr<Context>> ctxByLevel` in `Inst`). (2) `Runner` auf `const Context&` bzw. `std::shared_ptr<const Context>` umstellen. Das ist eine API-Änderung in `HorizonCode.h`, außerhalb dieses Slice. (3) `traceKey` als `const std::string*` auf `inst.cls.key` bzw. `chain[i]` statt als Kopie führen. Alternativ, falls `Runner` nicht anfassbar ist, `makeContext` in zwei Teile trennen: einen statischen Teil, der einmal pro Instanz/Level gebaut wird, und einen dünnen per-Call-Teil.
- **Erwarteter Effekt** (Schätzung, messen): ca. 0,2-0,5 µs und 3 Heap-Allokationen weniger pro Event auf libstdc++. Bei einigen tausend tickenden Instanzen sind das 0,5-2 ms pro Frame auf dem Spielthread.
- **Risiko / Verifikation**: Rekursion. Ein gecachter Context wird von verschachtelten Runnern derselben Instanz gleichzeitig benutzt; er muss deshalb unveränderlich sein (alle Lambdas lesen nur `this`/`id`/`level`, Zustand liegt in `Runtime`). Beim Entfernen einer Instanz (`m_doomed`) muss der Cache mit der `Inst` leben (tut er, solange er im `Inst` hängt). Tests: `tests/test_horizoncode_runtime.cpp`, `test_hc_bind_to.cpp`, `test_hc_pull_on_construct.cpp`, Codegen-Parity-Harness. Zusätzlich Mikrobenchmark 1000 Instanzen × `fireTick`.

---

### CORE-402 – Lineare Knotensuche pro Event/Call (`levelHandlingEvent`, `levelWithFunction`, `functionSignatureOf`)
- **ID**: CORE-402
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:2054-2068` (`levelHandlingEvent`), `:2070-2080` (`levelWithFunction`), `:1548-1555` (`callOwn`), `:1263-1277` (`functionSignatureOf`), `:2112`
- **Problem**: Jeder `fireTick`/`fireOn*`-Aufruf und jeder Funktionsaufruf durchläuft alle Knoten aller Level des Graphen leaf-first und vergleicht `n.s == event` bzw. `n.s == fn` (String-Vergleich). Bei `Tick` auf Instanzen, die gar keinen Tick-Handler haben, ist das die gesamte Arbeit eines Frames, denn `runEventOnLevel` kehrt danach zurück. Zusätzlich wird für `InputAction`-Knoten `inputActionChainFor(n, event)` aufgerufen. `callOwn` scannt nach `levelWithFunction` ein zweites Mal alle Knoten des gefundenen Levels, um den `FunctionEntry` für Run-On zu finden. Der Graph ist nach der Registrierung unveränderlich (`levels` liegt by value in `Inst`, `Runner` nimmt `const Graph&`).
- **Code-Auszug**:
```cpp
for (size_t lv = i.levels.size(); lv-- > 0; )
    for (const Node& n : i.levels[lv].nodes)
    {
        if (n.type == NodeType::Event && n.s == event && (n.elem == 0 || n.elem == elem))
            return (int)lv;
        if (n.type == NodeType::InputAction && inputActionChainFor(n, event) >= 0)
            return (int)lv;
    }
```
- **Lösung**: Pro `Inst` (besser pro Klasse, wenn Graphen geteilt werden, siehe CORE-408) zwei Indizes lazy aufbauen: `unordered_map<EventId, small_vector<{level, elem}>>` (leaf-first sortiert, InputAction-Ereignisse über `inputActionEventNames` eingetragen) und `unordered_map<std::string, {level, const Node*, access}>` für Funktionen. `levelHandlingEvent` wird dann zu einem Lookup plus einem Filter über `elem`. Die Semantik "leaf-most gewinnt, bei `elem` Filter 0 oder gleich" bleibt erhalten. Die öffentlichen/privaten Funktionen getrennt führen (`publicOnly`).
- **Erwarteter Effekt** (Schätzung): O(Knoten·Level) → O(1) pro Event. Bei Graphen mit 200+ Knoten und Tick pro Frame mehrere µs pro Instanz gespart. Zusammen mit CORE-401 der größte Hebel am Interpreter-Hot-Path.
- **Risiko / Verifikation**: Reihenfolge-Semantik (Override, Elem-Filter, InputAction-Ketten) muss exakt bleiben. Ein Differenztest "Index vs. alte lineare Funktion" über alle Graphen aus `tests/test_horizoncode_runtime.cpp` und den Inheritance-Tests ist günstig und aussagekräftig.

---

### CORE-403 – Byteweiser FNV-1a als Content-Hash im Streaming-Lesepfad (664 MB/s)
- **ID**: CORE-403
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Hpak/HpakFormat.h:110-119` (`hash64`), `src/HE_Core/src/Hpak/HpakReader.cpp:235` (Prüfung bei jedem Lesen), `:103` (TOC); Schreibseite `HpakWriter.cpp:653,1083,1113,1154`
- **Problem**: `readStoredBytes` hasht jeden gelesenen Eintrag vollständig und single-threaded, bevor entschlüsselt oder dekomprimiert wird. FNV-1a hat eine Abhängigkeitskette aus XOR + Multiplikation pro Byte (ca. 4 Zyklen/Byte). Gemessen (g++ -O2, 256 MiB): **664 MB/s**. AES-GCM mit AES-NI und LZ4-Decode liegen typischerweise im Bereich mehrerer GB/s (Schätzung, nicht gemessen). Der Hash ist damit der langsamste Schritt je Streaming-Job auf einem Pool-Thread, und ein 500-MB-Eintrag blockiert einen Worker ca. 0,75 s nur für die Prüfsumme. Auf der Schreibseite läuft derselbe Hash dreimal pro Asset (`inHash`, `srcHash`, `contentHash`) plus für Parent-Materialien.
- **Code-Auszug**:
```cpp
inline uint64_t hash64(const uint8_t* data, size_t len) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) { h ^= data[i]; h *= 1099511628211ULL; }
    return h;
}
```
- **Lösung**: Option A (format-erhaltend, kleiner Gewinn): Für verschlüsselte Einträge die Prüfung nach dem GCM-Tag überspringen; der Tag authentifiziert bereits, und `contentHash` bleibt im Format. Option B (voller Gewinn): Wortweiser/mehrspuriger Hash (z. B. XXH3-64 oder 4 parallele FNV-Lanes über 8-Byte-Wörter, 5-10 GB/s). Das ändert den gespeicherten Wert und braucht deshalb `k_version = 3` mit Reader für v2 und v3 (v2-Hash weiter byteweise). `kPackPipelineVersion` (`ProjectExporter.cpp:316`) anheben, damit alte Manifeste ungültig werden. Option C: blockweises Hashen in den bereits vorhandenen 4-MiB-Leseblöcken (`HpakReader.cpp:203-224`), solange die Daten im Cache liegen.
- **Erwarteter Effekt** (Schätzung): Option B reduziert den Hash-Anteil um etwa 8-15×. Das Lesen eines großen Eintrags wird dann I/O-/Decompress-bound statt Hash-bound. Export: ca. 3 Hash-Durchläufe pro Asset fallen praktisch weg (bei 10 GB Projekt ca. 45 s Single-Thread-Zeit).
- **Risiko / Verifikation**: Formatänderung. Test, der ein v2-Pak aus dem Repository weiter lädt (CLAUDE.md verlangt das für Chunk-Layouts), plus Roundtrip v3. Tests: `tests/test_hpak.cpp`, `tests/test_project_exporter.cpp`. Der `HE_Tools`-Packer ist nur ein Redirect (`HE_Tools/src/Packer/HpakWriter.h`), also keine zweite Implementierung.

---

### CORE-404 – `exchangeState()`: pro Frame Wert-Kopien, String-Lookups und lineare Suche je Binding/Watch
- **ID**: CORE-404
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:412-497` (`exchangeState`), `:581-621` (`reportChanges`), `:164-196` (`readPullSource`), `:365-410` (`resolveBindSource`), `:534-539` (`findWatched`), `:1078-1086` (`getVariable`). Aufrufer pro Frame: `GameApplication.cpp:3620`, `EditorApplication.cpp:5136`
- **Problem**: Pro Frame und pro Binding laufen: `find(owner)` (2 Hashes, siehe CORE-406), `resolveBindSource` mit `std::string`-Vergleichen gegen `kPullFrom*`, `find(src)`, die Sichtbarkeitsprüfung per `findVarInfo`/`findVarInLevels` (lineare Suche mit String-Vergleich), `getVariable(src, var)` (**Wertkopie**, bei Strings/Arrays/Structs tief mit Heap) und `valuesEqual` gegen den Shadow. Bei kompilierten Instanzen läuft `findVarInfo` doppelt (`getVariable` `:1082` prüft, dann sucht `compiled->getVariable` erneut). `reportChanges` kopiert ebenfalls jede beobachtete Variable (`Value now = getVariable(...)`, `:591`) in jedem Frame, auch wenn nichts passiert. Außerdem geht jede Runde mit mindestens einer Änderung in eine zweite komplette Runde über alle Bindings (`:436-491`, `changedAny`).
- **Code-Auszug**:
```cpp
for (size_t i = 0; i < m_watched.size(); ++i) {
    const InstanceId owner = m_watched[i].owner;
    if (!find(owner)) continue;
    Value now = getVariable(owner, m_watched[i].name);   // Kopie pro Variable pro Frame
    ...
    if (valuesEqual(now, w.shadow)) continue;
```
- **Lösung**: (1) Lesezugriff ohne Kopie: `const Value* peekVariable(id, name)` (interpretiert: Zeiger in `vars`; kompiliert: ggf. Reflection-Referenz, sonst Fallback auf Kopie). `valuesEqual` direkt dagegen, Kopie nur bei Änderung. (2) Quelle und Variablen-Deklaration beim ersten Auflösen in `Binding` cachen (Zeiger auf `Variable`/`CompiledVarInfo`-Index, `boundTo`-Instanz). Neu auflösen nur, wenn sich die Quell-Instanz ändert. (3) Für interpretierte Instanzen ein Versionszähler pro `Inst`, den `setVariable` erhöht; unveränderte Quelle überspringt den Vergleich. Kompilierte Member werden direkt geschrieben, dort bleibt der Vergleich. (4) Weitere Runden nur über Bindings, deren Quelle in der letzten Runde geschrieben wurde. `findWatched` über `unordered_map<(owner,name)>`.
- **Erwarteter Effekt** (Schätzung, messen): Bei ca. 200 Bindings/Watches grob 30-80 µs pro Frame heute, mit Peek und Cache ein Faktor 3-10 weniger. Bei wenigen Bindings vernachlässigbar, daher Mittel.
- **Risiko / Verifikation**: Semantik "nur eine Bewegung schreibt" und die Reihenfolge der Handler (Notify on Change) müssen bleiben. `tests/test_hc_bind_to.cpp` deckt das ab; ein Test mit großem Array als Quelle ergänzen. Der Peek-Zeiger darf nicht über einen Handler-Aufruf gehalten werden (Rehash in `vars`).

---

### CORE-405 – Abgeleitete, unveränderliche Listen werden pro Aufruf neu berechnet (`eventBindingsOf`, `replicatedVariablesOf`)
- **ID**: CORE-405
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:1281-1317` (`eventBindingsOf`), `:1167-1197` (`replicatedVariablesOf`), `:1199-1227` (`savedVariablesOf`). Aufrufer außerhalb: `WidgetManager.cpp:2935` und `:3425` (Pointer-Hit-Test, je Element der Treffer-Kette), `PropertyReplicator.cpp:104,138,164,203,216` (pro Entity und Replikations-Tick)
- **Problem**: `eventBindingsOf` durchläuft alle Knoten aller Level, kopiert jeden Event-Namen in einen neuen `std::string` und dedupliziert mit `std::any_of` über das Ergebnis (O(n²)). `inputActionEventNames(n)` alloziert zusätzlich pro InputAction-Knoten. Der Hit-Test ruft das für jedes Element der Treffer-Kette bei Mausbewegung bzw. Klick auf. `replicatedVariablesOf` baut pro Aufruf einen `vector<ReplicatedVar>` mit Namenskopien (ebenfalls quadratisches `report`), `PropertyReplicator` ruft es sogar nur zur Mitgliedschaftsprüfung (`rv.name == name`, `:203-204`, `:216-217`). Die Ergebnisse hängen nur von den (unveränderlichen) Graphen/`varInfos` ab.
- **Code-Auszug**:
```cpp
for (auto lv = i->levels.rbegin(); lv != i->levels.rend(); ++lv)
    for (const auto& n : lv->nodes) {
        auto report = [&](const std::string& name, int elem) {
            const bool have = std::any_of(out.begin(), out.end(), ...);   // O(n^2)
            if (!have) out.push_back({ name, elem });
        };
```
- **Lösung**: Beide Listen (und `savedVariablesOf`) lazy in `Inst` cachen und als `const std::vector<...>&` zurückgeben. Die Aufrufer iterieren nur. Zusätzlich `bool isReplicated(id, name)` für die reine Mitgliedschaftsprüfung. Ein Event-Name-Set für den Hit-Test (`bool listensTo(id, name, elem)`) würde die Schleife in `WidgetManager` auf einen Lookup reduzieren.
- **Erwarteter Effekt** (Schätzung): pro Aufruf O(Knoten) mit Allokationen → O(1) ohne Allokation. Relevant bei vielen Widgets (Hit-Test) und vielen replizierten Entities.
- **Risiko / Verifikation**: Rückgabetyp ändert sich (Wert → const-Referenz), Aufrufer in `HE_Scene` und `HE_Core` anpassen. Der Cache lebt mit `Inst`; bei Re-Registrierung entsteht ein neuer. `PropertyReplicator`-Tests und `test_horizoncode_runtime.cpp`.

---

### CORE-406 – `remove()` ist O(Listener+Bindings+Watches+Pending); `find()` hasht zweimal
- **ID**: CORE-406
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:744-772` (`remove`), `:1011-1058` (`retainOnlyReachableFrom`), `:46-57` (`find`)
- **Problem**: Jedes `remove(id)` scannt alle `m_listeners` (äußere und innere Maps, `vec.erase(std::remove...)`), `m_pending`, `m_suspended`, `m_bindings`, `m_watched`. Der Level-GC (`retainOnlyReachableFrom`) ruft `destroy → remove` für jede nicht erreichbare Instanz auf: O(N·(L+B+W)). Eine Szene mit 5000 Scene-Scope-Objekten, die ein Levelwechsel verwirft, wird quadratisch. `find()` macht außerdem pro Aufruf zwei Hash-Lookups (`m_doomed.count(id)` und `m_insts.find(id)`) und wird von praktisch jeder Context-Lambda (z. B. `:1361,1396,1514,1575`) aufgerufen.
- **Code-Auszug**:
```cpp
Runtime::Inst* Runtime::find(InstanceId id)
{
    if (m_doomed.count(id)) return nullptr;
    auto it = m_insts.find(id);
    return it != m_insts.end() ? &it->second : nullptr;
}
```
- **Lösung**: (1) `bool doomed` direkt im `Inst`; `find` = ein Lookup + Flag-Prüfung. `m_doomed` bleibt als Liste für `purgeDoomed`. (2) Batch-Variante `removeMany(const unordered_set<InstanceId>&)` für den GC: ein Durchlauf über Listener/Bindings/Watches mit Set-Lookup, danach je Instanz `destroy` ohne erneutes Scannen. Alternativ Reverse-Index `listener → [(owner, ev)]` und `owner → Bindings/Watches` (multimap).
- **Erwarteter Effekt** (Schätzung): `find` ca. halbe Kosten. Level-GC von O(N²) auf O(N), erst ab einigen hundert Instanzen spürbar.
- **Risiko / Verifikation**: `destroy` ruft Handler, die weitere Instanzen entfernen können (Reentranz). Der Batch muss die Reihenfolge "Destruct vor Remove" je Instanz beibehalten und darf keine Iteratoren über Handler-Aufrufe halten. Tests: `test_horizoncode_runtime.cpp` (GC, Destroy-in-Destruct).

---

### CORE-407 – Registrierung pro Instanz baut Strings für Pull/Watch, obwohl meist nichts zu tun ist
- **ID**: CORE-407
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:198-231` (`pullOnConstruct`), `:508-532` (`watchDeclared`), `:121-156`, `:71-119`
- **Problem**: Jede neue Instanz (Create Widget/Object, Listenzeilen, Projektile) baut in `pullOnConstruct` für **jede** Instanzvariable ein `PullSpec` mit 7 `std::string` (beim kompilierten Pfad mit `str()` jeweils eine String-Konstruktion pro Feld), sucht per Namen linear nach einem bestehenden Eintrag (`report`, O(n²)) und verwirft danach per `remove_if` fast alles (`src.empty() || var.empty()`). `watchDeclared` baut zusätzlich einen `vector<string>`. `widgetCreatorsWithoutShow(g)` läuft für jede Instanz ohne Klassen-Key. Das Ergebnis hängt ausschließlich von der Klasse ab, nicht von der Instanz.
- **Code-Auszug**:
```cpp
for (const auto& vi : inst->compiled->varInfos())
    report({ str(vi.name), str(vi.pullSource), str(vi.pullVar), str(vi.pullMember),
             str(vi.pullClass), vi.bindTo, str(vi.pullRef) });   // 6 Strings pro Variable pro Instanz
```
- **Lösung**: (1) Zwei-Phasen-Auswertung ohne Kopien: Zeiger/`string_view` der leaf-most Deklaration pro Name sammeln, Strings erst für Variablen mit `pullSource` und `pullVar` kopieren. (2) Besser: pro Klassen-Key die Spec-Liste und die `Watched`-Namen einmalig berechnen und cachen (`unordered_map<std::string classKey, ClassRegInfo>`; Instanzen ohne Key weiterhin direkt). Die dann leere Pull-Liste erlaubt einen frühen Rückkehrpfad.
- **Erwarteter Effekt** (Schätzung): bei Klassen mit 20 Variablen ca. 100+ kurzlebige String-Konstruktionen (teils Heap) weniger pro Spawn.
- **Risiko / Verifikation**: Semantik "leaf-most Deklaration entscheidet; Re-Deklaration ohne Pull nimmt den Pull heraus" erhalten. `tests/test_hc_pull_on_construct.cpp`, `test_hc_pull_editor.cpp`.

---

### CORE-408 – Jede Instanz hält eine tiefe Kopie ihrer Graphen (`std::vector<Graph>` by value) – unsicher
- **ID**: CORE-408
- **Kategorie**: C
- **Schweregrad**: Mittel (unsicher / messen)
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.h:651` (`std::vector<Graph> levels`), `:95-99` (`add(Graph graph, ...)`, `addLevels(std::vector<Graph>, ...)`), `HorizonCodeRuntime.cpp:59-69,71-112`
- **Problem**: `Inst::levels` besitzt die Graphen. Jeder `add`/`addLevels`-Aufruf überträgt (ggf. nach einer Kopie beim Aufrufer) einen kompletten `Graph` mit Knoten, Strings und Variablen in die Instanz. Zwei Instanzen derselben Klasse (z. B. 200 Listenzeilen-Widgets oder Gegner) halten damit zwei vollständige Kopien. Der `Runner` nimmt `const Graph&` (`HorizonCode.h:1463`), die Graphen werden also zur Laufzeit nicht verändert. Ob die Aufrufer (`WidgetManager`, `EntityHost`, `PlayerHost`, `HE_Scene`) kopieren oder schon verschieben, wurde nicht geprüft.
- **Lösung**: `std::vector<std::shared_ptr<const Graph>> levels`, vom Aufrufer pro Klasse geteilt (Cache nach Klassen-Key). Das ermöglicht zugleich die klassenweisen Indizes aus CORE-402/407. Der nicht-const Zugriff `Inst::leaf()` (`:656`) muss auf Mutationsstellen geprüft werden.
- **Erwarteter Effekt** (Schätzung): Speicher und Spawn-Zeit pro Instanz sinken um die Graphgröße (Knoten × Strings). Größe pro Graph ist nicht gemessen.
- **Risiko / Verifikation**: Prüfen, ob irgendwo in `Runtime` oder den Hosts Graph-Daten pro Instanz verändert werden (Hot-Reload, Editor-Debug). `add(Graph)` als Kompatibilitäts-Overload behalten. Messen: `sizeof`-Summe eines typischen Widget-Graphen × Spawn-Anzahl.

---

### CORE-409 – `HpakWriter` verarbeitet Pass 2 strikt sequenziell (Cook, ASTC/BC7, zstd-19, Verschlüsselung)
- **ID**: CORE-409
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Hpak/HpakWriter.cpp:1110-1199` (Schleife), `:574-656` (`addEntry`, zstd Level 19 `:614`), `:703-815` (`cookTexture`), `:42-68` (`encodeAstc4x4`), `:101-127` (`encodeBc7`)
- **Problem**: Alle teuren Schritte laufen assetweise hintereinander auf einem Thread: Ref-Rewrite, Mip-Kette, ASTC/BC7-Block-Encoding, zstd-19, AES-GCM, Hashes. Das Dateikommentar `HpakWriter.h:22-26` nennt genau diese Teile als "die Minuten". Die Assets sind voneinander unabhängig; die Encoder sind bereits für Multi-Threading vorbereitet (`std::call_once` für `bc7enc`, `:107-108`). `astcenc_context_alloc(&cfg, 1, &ctx)` pro Mip-Ebene erzeugt je einen Ein-Thread-Kontext. Das JobSystem (`parallel_for`, `schedule`) liegt im selben Modul.
- **Code-Auszug**:
```cpp
for (auto& pe : pending)
{
    ...
    std::vector<uint8_t> blob = rewriteRefsForPack(pe.bytes, pathToUuid, settings, &blobByPath, &deps);
    if (settings.cook) blob = cookForPack(blob, ...);   // Mips + ASTC/BC7
    ...
    if (!reused) addEntry(pe.id, blob, settings);        // zstd -19 + AES-GCM
```
- **Lösung**: Schleife in zwei Teile trennen. (a) Parallel: pro Asset ein Ergebnisobjekt `{PendingEntry or reused StoredEntry, srcHash, CachedEntry, warnings}`, berechnet über `parallel_for(pending.size(), …, "PackAsset", /*minGrain=*/1)` (nur ein Level parallelisieren, Nesting fällt per Design auf den Aufrufer zurück). (b) Seriell in `pending`-Reihenfolge: `m_entries`/`m_srcHashes`/`m_entryInfos` füllen, `warn()`, `onAsset`, `progress` aufrufen. Damit bleibt der Thread-Vertrag der Callbacks ("same thread as the pack call") und ein deterministisches Manifest. `cache->previousPak->readStoredEntry` nutzt einen geteilten `ifstream` (nicht thread-safe, `HpakReader.h:12`); dafür pro Job `openShared` (`HpakReader.h:33`) verwenden.
- **Erwarteter Effekt** (Schätzung): nahezu linear mit Kernen. zstd-19 liegt grob bei wenigen MB/s pro Kern, ein 1-GB-Projekt braucht single-threaded Minuten und auf 8 Kernen grob ein Achtel.
- **Risiko / Verifikation**: `compileShaderVariants`/`compileParticleShaderVariants` sind Editor-Callbacks (Shader-Cross-Compiler), deren Thread-Sicherheit unbekannt ist. Entweder serialisieren (Mutex) oder vorab prüfen. Ausgabe muss byte-identisch bleiben: Vergleichstest "Pak seriell vs. parallel" in `tests/test_hpak.cpp`/`test_project_exporter.cpp` (write() sortiert nach UUID, die Reihenfolge von `m_entries` ist unkritisch).

---

### CORE-410 – Pass 1 liest jedes `.hasset` komplett (zeichenweise) und hält das ganze Projekt im RAM
- **ID**: CORE-410
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HpakWriter.cpp:1001-1005` (Einlesen), `:941-944` (`pending`), `:1039`, `:1064-1070` (`blobByPath`), `:1204-1271` (`write`); ähnlich `ProjectConfig.cpp:121-123`, `ProjectExporter.cpp:159-165`
- **Problem**: (1) `std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>())` liest Zeichen für Zeichen über `sgetc/snextc`, typisch einige 100 MB/s statt GB/s, und wächst per Verdopplung (Spitzen-RAM bis 2× Dateigröße). (2) Der Scan braucht je Datei nur UUID, Pfad und Typ aus dem META-Chunk, behält aber alle Bytes in `pending`. Das gesamte Projekt liegt gleichzeitig im Speicher, danach zusätzlich Cook-Kopien und `m_entries` mit allen gespeicherten Daten bis `write()`. Bei mehreren GB Content ist das der Spitzenverbrauch des Editors beim Export.
- **Code-Auszug**:
```cpp
std::ifstream f(p.path(), std::ios::binary);
std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
...
pending.push_back({std::move(bytes), id, std::move(rel), std::move(path)});
```
- **Lösung**: (a) Sofort und ohne Verhaltensänderung: Größe per `seekg/tellg` bzw. `std::filesystem::file_size`, `resize`, ein `f.read`. (b) Strukturell: Pass 1 liest nur Header + META (Chunk-Header, `seekg` über Payloads) und speichert `{path, size, uuid, relPath, metaPath, type}`. Pass 2 lädt die Bytes erst, wenn der Eintrag dran ist (bei parallelem Pass 2 pro Job); `blobByPath` für Material-Parents durch einen On-Demand-Loader mit kleinem LRU ersetzen. `write()` kann die Nutzdaten statt aus `m_entries` aus einer temporären Datei streamen.
- **Erwarteter Effekt** (Schätzung): (a) Lesen 3-10× schneller. (b) Spitzen-RAM von Σ(Projekt) auf wenige Assets pro Thread. Das hat große Wirkung auf Rechnern mit wenig RAM.
- **Risiko / Verifikation**: (b) berührt die Level-1-Reuse-Logik (`inHash` braucht den Dateiinhalt; der Hash kann dabei beim Lesen mitlaufen). `test_hpak.cpp`/`test_project_exporter.cpp` (inkrementeller Export, Override/Collision-Warnungen). Sehr große Projekte als manueller Test.

---

### CORE-411 – Mehrfaches Voll-Kopieren jedes Assets durch `HAsset::Reader::openData` und `return blob`
- **ID**: CORE-411
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HpakWriter.cpp:165-180` (`metaFromHasset`), `:290-309` (`rewriteRefsForPack`), `:823-834` (`cookForPack`), `:1145-1151`; Ursache `src/HE_Core/include/ContentManager/HAsset.h:437-477` (`openData` kopiert jede Chunk-Payload in einen eigenen Vektor)
- **Problem**: Pro Asset läuft: `metaFromHasset` (kopiert **alle** Chunks, nur um META zu lesen), danach `rewriteRefsForPack`: neuer Reader (Vollkopie), und für Texturen, Sounds und alle anderen Typen am Ende `return blob;` als **Kopie** (Parameter ist `const&`), dann `cookForPack`: wieder neuer Reader (Vollkopie) und `return blob;` (Kopie). Der Typ steht aber im Dateiheader (`stripImportSourceForPack` macht das bereits so, `:201-205`). Für eine 64-MB-Textur oder 200-MB-Mesh sind das 4-5 transiente Vollkopien, bevor auch nur ein Byte komprimiert wird.
- **Code-Auszug**:
```cpp
HAsset::Reader r;
if (!r.openData(blob)) return blob;                 // Vollkopie aller Chunks
...
if (!isMesh && !isMat && !isScene && !isParticles) return blob;   // und noch eine Kopie als Rückgabewert
```
- **Lösung**: (1) `HAsset::peekAssetType(const vector&)` bzw. `peekChunk(data, id)` (nicht kopierend, gibt `span` zurück). (2) `rewriteRefsForPack`/`cookForPack` nehmen `std::vector<uint8_t>&& blob` bzw. arbeiten in-place mit `bool changed`-Rückgabe. Frühaustritt vor dem Reader, wenn der Typ nichts zu tun hat. (3) `metaFromHasset` über `peekChunk(CHUNK_META)`.
- **Erwarteter Effekt** (Schätzung): 3-4 Vollkopien pro durchgereichtem Asset weniger (Zeit und Spitzen-RAM), bei texturlastigen Projekten deutlich.
- **Risiko / Verifikation**: Ausgabebytes müssen identisch bleiben (Goldtest: Pak vor/nach der Änderung binär vergleichen, `test_hpak.cpp`). `HAsset.h` liegt in einem anderen Slice (ContentManager), dort nur eine additive API.

---

### CORE-412 – Material-Layout (MTRL) dreifach nachgebaut; `rewriteRefsForPack` ist eine 280-Zeilen-Funktion
- **ID**: CORE-412
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `HpakWriter.cpp:248-275` (`mtrlGraphAndParent`), `:354-542` (MTRL-Zweig, ca. 190 Zeilen, Offsets `tailStart`, `graphTexOffset`, `afterTypes`, `afterGBuf`), `:514` (Magic-Größen 92/40/32), `:290-572` (gesamte Funktion)
- **Problem**: Die Feldreihenfolge des MTRL-Chunks wird an zwei Stellen in diesem File von Hand nachgelesen (`mtrlGraphAndParent` und der Zweig in `rewriteRefsForPack`), eine dritte Kopie liegt im Writer in `ContentManager::saveAsset`. Der Code warnt selbst (`:356-361`), dass ein Feld, das dort ergänzt wird und hier fehlt, ohne Fehler zu falschen Offsets in gepackten Materialien führt. Die Fall-Unterscheidung über die Größe des Approx-Blocks (`rem >= 92 ? 92 : ... 40 ... 32`) kodiert historische Revisionen als Magic Numbers. Die Funktion mischt vier Aufgaben (Mesh-MREF/MSEC, Material, Partikel, Szene) mit tiefer Verschachtelung.
- **Lösung**: Einen Skimmer `HAsset::skimMtrl(const std::vector<uint8_t>&) -> MtrlLayout{ offsets, shaderPath, texturePaths, graphJson, parentPath, switchNames, switchValues, ... }` neben dem MTRL-Writer in `HAsset`/`ContentManager` ablegen und von beiden Funktionen nutzen. Die Approx-Blockgrößen als benannte Konstanten. Danach `rewriteRefsForPack` in `rewriteMeshRefs`, `rewriteMaterial`, `rewriteParticles`, `rewriteScene` aufteilen.
- **Erwarteter Effekt**: Weniger Drift-Risiko, kürzere Funktionen. Reine Wartbarkeit, keine Laufzeitwirkung.
- **Risiko / Verifikation**: Hoch, weil Ausgabebytes exakt gleich bleiben müssen. Absichern mit (a) Goldtest über ein Pak mit Material, Material-Instanz (mit Parent), Switch-Overrides und alten Tail-Revisionen (92/40/32/0 Byte), (b) einem Roundtrip-Test "Writer schreibt MTRL, Skimmer liest dieselben Felder". `test_hpak.cpp`, `test_material_graph.cpp`.

---

### CORE-413 – Runtime-Binaries: pro Datei `std::system("strip …")`/`codesign`, sequenziell, jedes Mal neu
- **ID**: CORE-413
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Hpak/ProjectExporter.cpp:200-203` (`stripLocalSymbols`), `:210-217` (`resignMachO`), `:944-1027` (Kopierschleife), `:982-984`, `:992-1022`, `:1454`
- **Problem**: Für jede reguläre Datei der Runtime (auch `config.json` u. Ä.; die Magic-Prüfung `looksMachO` betrifft nur den Apple-Re-Sign) wird eine Shell mit `/usr/bin/strip -x` gestartet. Das schreibt jede Bibliothek komplett neu. Auf macOS folgt ein `codesign` pro Mach-O, und `signAppBundle` signiert danach `--deep` das ganze Bundle noch einmal. Alles seriell, bei jedem Export, auch wenn die Runtime unverändert ist (zuvor wird jede Zieldatei gelöscht und neu kopiert, `:982-984`).
- **Code-Auszug**:
```cpp
return std::system(("/usr/bin/strip -x " + shellQuote(p) + " 2>/dev/null").c_str()) == 0;
...
std::filesystem::remove(dst, ec); ec.clear();
std::filesystem::copy_file(dit->path(), dst, overwrite_existing, ec);
```
- **Lösung**: (1) Nur ELF/Mach-O strippen (Magic prüfen, bevor ein Prozess gestartet wird). (2) Ein einmal gestrippter Cache der Runtime (Schlüssel: Pfad + Größe + mtime) im Build-Cache, der Export kopiert nur noch. Das spart auch das Re-Signieren. (3) Die Dateien über `parallel_for` bearbeiten. (4) `posix_spawn` statt `std::system` (keine Shell, kein Quoting-Thema).
- **Erwarteter Effekt** (Schätzung): Der Export-Schritt "binaries" wird von Sekunden bis zu einer halben Minute (große Runtime, macOS-Signatur) auf nahe null bei Cache-Treffer.
- **Risiko / Verifikation**: Der Cache muss bei Runtime-Wechsel zuverlässig ungültig werden (Hash über Datei). Die Key-Patch-Reihenfolge (Patch nach Strip/Sign) bleibt. `tests/test_project_exporter.cpp`, auf macOS manuell: gestarteter Export läuft auf Apple Silicon.

---

### CORE-414 – AutoLandscape-Shader wertet alle Schichten und Bombing-Taps unbedingt aus – unsicher
- **ID**: CORE-414
- **Kategorie**: D
- **Schweregrad**: Mittel (unsicher / GPU-Messung nötig)
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp:109-142` (4 Schichten × 3 Maps, je `TextureArrayBombSample`), `:154-158`, `:179-212` (Blend-Kette), `:268-273` (drei `layerBlend`, die wieder alle Schichten referenzieren)
- **Problem**: Der Graph liest für Grass, Dirt, Rock und Snow je Albedo, Normal und Mask. Mit aktivem `Texture Bombing` (Standard) ist jede dieser 12 Lesungen ein Hex-Tiling mit je 3 Taps (laut Kommentar im Header), also rund 36 Array-Samples pro Pixel, dazu zwei `Fbm` und die Weather-Funktionen. Codegen kennt keine Verzweigung nach Masken: Schnee in der Ebene und Fels auf flachem Gelände werden trotzdem gesampelt, da Lerp-Zweige keine dynamische Verzweigung bilden. Die gemalte Variante (`layerBlend`) liest alle Schichten ein zweites Mal als Blend-Eingang, teilt aber die bereits gelesenen Knoten. Aufwand ist also nicht verdoppelt, aber hoch.
- **Lösung** (nur nach Messung): (a) Entfernungsabhängiger Schalter: Bombing nur nahe der Kamera, fern plain (der `StaticSwitch` existiert bereits als Permutation, Material-Instanz-Override). (b) Wenn `MaterialGraph`/Codegen eine `Branch`/`if`-Node mit Uniform-Control-Flow unterstützt, Snow-Lesungen hinter `snowAny > 0`. (c) Die `Mask`-Arrays für Fernbereiche aus einem niedrigeren Mip lesen.
- **Erwarteter Effekt**: Unbekannt; auf Mittelklasse-GPUs kann ein bildfüllendes Terrain mit 36+ Taps bandbreitenlimitiert sein. Ohne GPU-Zeitmessung (Pass-Timer des Editors) nur Vermutung.
- **Risiko / Verifikation**: Optik ändert sich (a)/(c). Die Witness-/Mask-Tests lesen nur die Masken, nicht die Samples. GPU-Timings pro Backend mit dem Stats-Card-Pass-Timer messen, bevor etwas geändert wird.

---

### CORE-415 – `dispatchChanged` und `dispatchDestroyed` sind zu großen Teilen Duplikate
- **ID**: CORE-415
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `HorizonCodeRuntime.cpp:623-703` (`dispatchChanged`) und `:883-966` (`dispatchDestroyed`)
- **Problem**: Beide Funktionen enthalten dieselben drei Blöcke: Listener kopieren und Tiefenwache, das Dispatch-Budget (`++m_dispatchFires > kMaxDispatchFires` mit einmaliger Warnung), und die Ermittlung der Handler-Signatur (`handles/wantsArg/argType/argTypeName` getrennt für kompilierte `eventInfos()` und interpretierte `Event`-Knoten). Dazu kommt das Verschicken (`compiled->fireEvent` vs. `runEventOnLevel`). Kleiner Nebeneffekt: `kOnDestroyed == std::string(e.name)` (`:916`) konstruiert pro Event-Info einen temporären String, die Schwesterfunktion nutzt `evName == e.name`.
- **Lösung**: `struct HandlerSig { bool handles, wantsArg; PinType argType; std::string argTypeName; }; HandlerSig handlerSignature(const Inst&, const std::string& ev) const;` und `template<class MakeArg> void forEachBoundListener(owner, EventId, const std::string& name, MakeArg)` mit Tiefe, Budget und Versand. Die beiden Funktionen reduzieren sich auf die jeweilige Argumentprüfung (Struct-Typ bzw. numerische Kompatibilität). Dieselbe Signatursuche sollte auch `levelHandlingEvent` (CORE-402) nutzen.
- **Erwarteter Effekt**: ca. 70 Zeilen weniger, eine Stelle für Budget-/Signatur-Regeln.
- **Risiko / Verifikation**: Unterschiede sind absichtlich (`dispatchDestroyed` ohne trailing Listener-Pass; Struct-Prüfung statt `pullShapesCompatible`). Tests: `test_hc_bind_to.cpp` (Notify on Change), Extract-on-Destruct-Tests in `test_horizoncode_runtime.cpp`.

---

### CORE-416 – "Ist öffentliche Instanzvariable?" und "über alle Variablen beider Backends laufen" mehrfach implementiert
- **ID**: CORE-416
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: Öffentlichkeitsprüfung: `HorizonCodeRuntime.cpp:169-182` (`readPullSource`), `:389-402` (`resolveBindSource`, Ref-Variante), `:1098-1113` (`setPublicVariable`), `:1115-1126` (`isPublicVariable`), `:1432-1448` (`ctx.getExternal`). Variablen-Iteration (leaf-most wins): `:213-225`, `:514-523`, `:1185-1195`, `:1213-1222`
- **Problem**: Der Satz "compiled: `findVarInfo` und `access == 0`, interpretiert: `findVarInLevels` und `access == 0 && scope == 0`" steht fünfmal. `getExternal` dupliziert `isPublicVariable` + `getVariable` samt zusätzlichen Warnungen. Die vier Iterationen "kompiliert über `varInfos()`, interpretiert über `levels[].variables` mit `scope == 0`, mit Ersetzen-nach-Namen" sind strukturell gleich, jede mit eigener linearer `report`-Suche (O(n²)).
- **Lösung**: `struct VarDecl { bool found, isPublic, isRef, isArray; PinType type; }; VarDecl declOf(const Inst&, const std::string&) const;` und `template<class F> void forEachInstanceVar(const Inst&, F f)` (liefert eine kleine View mit `name,type,access,replicated,saveGame,pull*`). Die genannten Funktionen sind dann Einzeiler.
- **Erwarteter Effekt**: ca. 60 Zeilen weniger; Verhalten identisch; leichter, CORE-404/405 umzusetzen.
- **Risiko / Verifikation**: Gering. Bestehende Tests (`test_horizoncode_runtime.cpp`, `test_scripting_binding.cpp`) decken Zugriffsregeln ab.

---

### CORE-417 – JobSystem: Kleinigkeiten (Header-Gewicht, 1-ms-Polling in `wait()`, Statistik-Uhr pro Task)
- **ID**: CORE-417
- **Kategorie**: E / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `JobSystem.h:7-14`; `JobSystem.cpp:410-413`, `:461-464`, `:475-517`
- **Problem**:
  1. `JobSystem.h` (in 19 Dateien eingebunden) zieht `<queue>` ("no longer used here", Zeile 12) und `<future>` mit. Nur `ContentManager.h` nutzt `std::queue`, und es inkludiert `<queue>` selbst (`ContentManager.h:15`; eine Repo-weite Suche in `src/` und `tests/` fand keinen weiteren Nutzer).
  2. `JobHandle::wait()` auf einem `Waiting`-Job, dessen Upstream auf anderen Threads läuft, schläft in einer 1-ms-Schleife (`wait_for(1ms)`), statt auf eine echte Benachrichtigung zu warten.
  3. Der Worker-Loop wertet `core->pick(prio)` nach dem Wait-Prädikat ein zweites Mal aus (`:461-464`) und liest für **jeden** laufenden Task zweimal `steady_clock::now()` plus drei bis vier Relaxed-Atomics, obwohl der Kommentar `:55-56` sagt, die Diagnose solle nicht zusätzliche Kosten pro Task verursachen. Bei µs-kleinen `parallel_for`-Helfern ist das ein messbarer Anteil.
- **Code-Auszug**:
```cpp
std::unique_lock<std::mutex> lock(job->m);
job->cv.wait_for(lock, std::chrono::milliseconds(1), [&] { return job->status.load(...) != JobStatus::Waiting; });
continue;
```
- **Lösung**: (1) `<queue>` entfernen; `<future>` hinter ein `JobSystem/JobSubmit.h` oder in den Aufrufer von `submit` verschieben (nur wenn der Build-Zeitgewinn gemessen wird). (2) Poolweiten Fortschrittszähler + `cv` (`core.progress`), den `finishJobs` hochzählt; `wait()` wartet auf Änderung dieses Zählers statt zu pollen. (3) Task-Zeit nur erfassen, wenn Stats/Profiler aktiv sind (`std::atomic<bool> statsOn`), `pick` im Prädikat in einen Merker schreiben.
- **Erwarteter Effekt** (Schätzung): (1) kürzere Compile-Zeit. (2) Keine 1000 Wakeups/s mehr bei blockierenden Dependency-Waits (selten, daher Niedrig). (3) ca. 40-60 ns pro Task.
- **Risiko / Verifikation**: Lost-Wakeup bei (2) gründlich prüfen (Zähler vor dem Test lesen). `tests/test_jobsystem.cpp` (Cancel/Dependency/Stats-Tests) deckt das ab.

---

### CORE-418 – `addPackedEntry` kopiert gespeicherte Daten; `cookTexture` legt mehrere Vollkopien an
- **ID**: CORE-418
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `HpakWriter.cpp:658-669` (`e.data = stored.data;`), Aufrufer `:1131`, `:1190`; `:743-752` (`rgbaLevels = px->data; prev = px->data;`)
- **Problem**: `addPackedEntry` bekommt `const StoredEntry&` und kopiert den ganzen Datenpuffer, obwohl die Aufrufer ihr lokales `se` danach nicht mehr benutzen. Bei vollständigem Reuse (häufigster inkrementeller Export) entspricht das einer Kopie des gesamten Paks im Speicher. In `cookTexture` werden Level 0 doppelt kopiert (`rgbaLevels` und `prev`), die Kette wächst ohne `reserve` (Faktor 4/3).
- **Lösung**: Overload `addPackedEntry(const HE::UUID&, HpakReader::StoredEntry&&)` mit `e.data = std::move(stored.data)`. In `cookTexture` `rgbaLevels.reserve(px->data.size() * 4 / 3 + 64)`, `prev` als `const uint8_t*` auf die zuletzt angehängte Ebene.
- **Erwarteter Effekt**: eine Pak-große Kopie weniger beim Reuse; bei 4K-Texturen zwei bis drei Kopien à 64 MB weniger.
- **Risiko / Verifikation**: Gering; Ausgabebytes gleich. `test_hpak.cpp`.

---

### CORE-419 – Pak, `project.hcfg` und Manifest werden nicht atomar geschrieben
- **ID**: CORE-419
- **Kategorie**: E / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `HpakWriter.cpp:1204-1277` (`write`, `trunc` direkt auf Zielpfad), `ProjectConfig.cpp:24-26`, `ProjectExporter.cpp:416` (Manifest). Vorbild im selben File: `patchEmbeddedPakKey` `:253-274`
- **Problem**: `write()` öffnet das Ziel mit `trunc`. Bricht der Export mittendrin ab (Absturz, Kill, volle Platte), sind das alte Pak **und** das passende Manifest verloren, und das Ziel ist ein halbes Pak. Der Exporter hat die Technik (Temp + `rename`) schon für die Executable, wendet sie aber auf die übrigen Dateien nicht an.
- **Lösung**: Ein Helfer `writeFileAtomic(path, writerFn)` (Temp-Datei im selben Verzeichnis, `flush/close`, `rename`) für Pak, hcfg und Manifest; `patchEmbeddedPakKey` nutzt ihn ebenfalls.
- **Erwarteter Effekt**: Robustheit; kein Performance-Effekt.
- **Risiko / Verifikation**: Windows: `rename` über eine geöffnete Datei schlägt fehl, der Exporter schließt `prevPak` bereits vorher (`ProjectExporter.cpp:842`). Test: Schreibfehler simulieren (read-only Verzeichnis) und prüfen, dass das alte Pak bleibt.

---

### CORE-420 – Schlüsselpatch liest die komplette Executable (zeichenweise) und sucht naiv
- **ID**: CORE-420
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `ProjectExporter.cpp:159-165` (`readWholeFile`), `:220-251` (`patchEmbeddedPakKey`), `:289-301` (`readEmbeddedPakKey`), Aufrufer `:646-649`
- **Problem**: Für das Auffinden eines 24-Byte-Magic in einem 20-50-MB-Executable wird die Datei per `istreambuf_iterator` komplett eingelesen (siehe CORE-410) und mit `std::search` durchsucht. `readEmbeddedPakKey` macht das bei jedem inkrementellen verschlüsselten Export auf der alten Executable. Das Patchen schreibt zusätzlich die komplette Datei neu (Temp + `rename` ist beabsichtigt und richtig).
- **Lösung**: Gemeinsamer `readFileBytes(path, out)` mit `file_size` + `read`. Für das reine Lesen des Schlüssels in 1-MiB-Fenstern mit 23 Byte Überlappung suchen (`std::boyer_moore_horspool_searcher`), ohne die Datei zu halten.
- **Erwarteter Effekt** (Schätzung): einige hundert ms pro Export und Executable.
- **Risiko / Verifikation**: Gering. `test_project_exporter.cpp` (Embedded-Key-Tests).

---

### CORE-421 – ProjectExporter: Duplikate, uneinheitliche Doku, unescapte Plist-Werte
- **ID**: CORE-421
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ProjectExporter.cpp:1354-1364` und `:1406-1416` (identischer `#if`-Block), `:183-188` (`shellQuote`) vs. `:501-504` (Inline-Kopie), `:914-919` (`n.compare(...)` ohne Klammern um `&&`/`||`) vs. `:966-969` (`endsWith`), `:461-476` (`ident`/`ver` unescaped), `ProjectExporter.h:208-214` (`onStage`-Doku), Phasenkommentare `:1191/1314`, `:1255/1392` ("Phase 7b/7c" jeweils doppelt), `:886-1108` (220 Zeilen, bis zu 6 Verschachtelungsebenen)
- **Problem**: (1) Das Auflösen von `ExportPlatform::Host` steht zweimal wortgleich. (2) `signAppBundle` baut sein Shell-Quoting selbst, obwohl `shellQuote` existiert. (3) Dateiendungen werden an drei Stellen (hier zweimal, plus `IntegrityProbe.cpp:35-38`) mit eigenen Lambdas verglichen. Die unklammerte Mischung `A && (B) || C && (D)` ist korrekt, löst aber `-Wparentheses` aus. (4) `writeInfoPlist` escaped nur den Anzeigenamen; `bundleId` und `appVersion` gehen roh in das XML. Ein `&` oder `<` in der Version erzeugt ein ungültiges Plist. (5) `ExportSettings::onStage` listet sechs Stage-Namen, der Code meldet auch `"icon"`. (6) `copyRuntimeBinaries` ist die längste Funktion der Datei.
- **Lösung**: `ExportPlatform resolveHost(ExportPlatform)`, `shellQuote` überall, `hasSuffix(std::string_view, std::string_view)` in einem gemeinsamen Util, `xmlEscape` für alle Plist-Strings, `copyRuntimeFile(...)` und `shipSubdir(...)` aus `copyRuntimeBinaries` herauslösen, Kommentare angleichen.
- **Erwarteter Effekt**: Lesbarkeit, ein latenter Plist-Fehler weniger.
- **Risiko / Verifikation**: Gering. Plist-Ausgabe mit Sonderzeichen in einem Test prüfen (`test_project_exporter.cpp`).

---

### CORE-422 – Krypto: mbedTLS-`randomBytes` seedet pro Aufruf (pro Pak-Eintrag), duplizierte Konstanten, veralteter Kommentar
- **ID**: CORE-422
- **Kategorie**: D / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `Aes256Gcm.cpp:110-137` (mbedTLS-`randomBytes`), `HpakWriter.cpp:635` (Aufruf pro Eintrag), `Aes256Gcm.cpp:10-11` und `:105-106` (doppelte Konstanten), `KeyDerivation.h:8,31-32` und `KeyDerivation.cpp:28` (`hmacSha256` mit Vektor-Allokation)
- **Problem**: (1) Im mbedTLS-Fallback wird für jede 12-Byte-Nonce ein frischer Entropie-Kontext plus CTR-DRBG initialisiert und geseedet. Der Kommentar `:115-116` ("not in a hot loop") stimmt nicht, denn `addEntry` ruft es pro Asset. Bei 100.000 Einträgen sind das 100.000 Seeds. (2) `kTagLen`/`kNonceLen` stehen in beiden Backend-Blöcken identisch. (3) `KeyDerivation.h` behauptet "SHA-256 is implemented inline", die Implementierung liegt aber in `Crypto/Sha256.cpp` (`KeyDerivation.cpp:7-12`). (4) `hmacSha256` kopiert die Nachricht in einen neuen `std::vector` (`:28`); für den Handshake einmal pro Verbindung vernachlässigbar.
- **Lösung**: Pro Thread einen DRBG halten (`thread_local`, lazy) oder alle Nonces eines Packs in einem Aufruf anfordern. Konstanten vor die `#if`-Kette ziehen. Kommentare korrigieren. Falls die `Sha256`-Klasse einen Streaming-Modus hat, diesen für HMAC nutzen.
- **Erwarteter Effekt**: Nur mbedTLS-Builds und große Pakete; sonst Wartbarkeit.
- **Risiko / Verifikation**: DRBG-Zustand nicht zwischen Threads teilen. Bestehende AES-/HMAC-Vektor-Tests in `tests/test_hpak.cpp` bzw. Netz-Tests.

---

### CORE-423 – `HorizonGameServices.h`: `std::string`-Parameter, `strlen` trotz bekannter Länge, Makros statt `constexpr`
- **ID**: CORE-423
- **Kategorie**: F / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `HorizonGameServices.h:734-735` (`keyDown(const std::string&)`), `:759-762` (`gamepadButton`/`gamepadAxis`), `:526` (`std::string(small)`), `:38-54` (ABI-Konstanten als `#define`), `:58-66` vs. `IGameLogic.h:16-22` (zweimal `HE_GAME_API`)
- **Problem**: Die Wrapper nehmen `const std::string&`. Spielcode ruft `he::input::keyDown("W")` im Per-Frame-`onUpdate`, jede Aufrufstelle konstruiert einen String (SSO, bei Namen über 15 Zeichen Heap, z. B. `"leftshoulder"` ist 12, `"righttrigger"` 12, `"Escape"`, knapp). `fetchString` kennt nach dem Aufruf `need`, baut den Ergebnis-String aber mit `std::string(small)` (erneutes `strlen`). Die ABI-Versionen sind Makros, obwohl der Header ohnehin C++17-kompatibel bleiben muss (`inline constexpr` ist dort erlaubt). `HE_GAME_API` ist an zwei Stellen definiert (einmal mit `#ifndef`, einmal ohne), was bei identischem Text zufällig funktioniert.
- **Lösung**: Zusätzliche Overloads mit `const char*` (die `std::string`-Varianten rufen sie auf). `std::string(small, (size_t)need)`. `HE_*_ABI_VERSION` als `inline constexpr uint32_t` (die Makros nur dort behalten, wo sie im Präprozessor gebraucht werden). `HE_GAME_API` aus einer gemeinsamen kleinen Datei `HeGameApi.h`.
- **Erwarteter Effekt**: Spielcode ohne temporäre Strings; marginal.
- **Risiko / Verifikation**: ABI unverändert (nur inline Wrapper). Gilt für das C++17-Scaffold (`CppScaffold::cmakeLists`): kompilieren mit `-std=c++17`. Test der Wrapper existiert wahrscheinlich in `test_scripting_binding.cpp`/Game-Logic-Tests (nicht geprüft).

---

### CORE-424 – IntegrityProbe: Log kopiert das Manifest, Hash-Job mit Normal-Priorität, kleine Quadratik
- **ID**: CORE-424
- **Kategorie**: C / E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `IntegrityProbe.cpp:264` (`manifest().entries.size()`), `:224-226` (`submit` ohne Priorität), `:124-126` (`any_of` pro Datei), `:148-150` (`compare` kopiert und sortiert beide Listen)
- **Problem**: `publish` ruft für die Log-Zeile `manifest()` auf (Mutex + **Kopie** des gesamten Manifests), obwohl die Größe vor dem `std::move` bekannt ist. Der Start-Job hasht Executable und alle Bibliotheken (bis einige hundert MB SHA-256) als `Normal`-Job und konkurriert damit beim Spielstart mit dem Asset-Streaming. `JobPriority::Low` existiert (`submit` hat den Parameter) und bleibt hinter dem Frame-Pfad. Die weiteren Punkte sind klein (n < 100).
- **Lösung**: `const size_t n = m.entries.size();` vor dem Move loggen. `globalPool().submit(..., "IntegrityProbe", HE::JobPriority::Low)`. Optional: Bibliotheken parallel hashen, falls Startzeit zählt.
- **Erwarteter Effekt**: Vernachlässigbar bis klein; Low-Priorität nimmt dem Streaming beim Start einen Worker weg.
- **Risiko / Verifikation**: Gering; `join` wartet weiter über `Running`-Zustand. `tests/test_integrity.cpp`.

---

### CORE-425 – `ProjectConfig`: verschachtelte Versions-Ternary, doppelte Kommentare, tote Felder, verstreute Flag-Bits
- **ID**: CORE-425
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ProjectConfig.cpp:31-49` (Kommentar doppelt, 5-stufige Ternary), `:61-85` vs. `:143-154` (Flag-Bits an zwei Stellen), `:51-52`/`:127` (`reserved`), `ProjectConfig.h:15` und `ProjectConfig.cpp:57,138-140` (`projectUuidBytes` "legacy (unused)")
- **Problem**: Die Versionswahl ist eine fünfstufige Verkettung; der Kommentar dazu steht doppelt (`:31-37`). Die Flag-Bits (1, 2, 4, 8, 16, 32, 64, … 4096) stehen als Zahlenliterale in `save` und `load`. Bit 4096 ist in `load` vor Bit 2 platziert. `projectUuidBytes` wird geschrieben und gelesen, aber laut Header nirgends benutzt (16 tote Byte im Format, nicht entfernbar ohne Formatänderung). Die einzelne Feldreihenfolge wird in `save`/`load` über `if (version >= N)` gespiegelt.
- **Lösung**: `enum HcfgFlag : uint32_t { ModSupport = 1, Encrypted = 2, … }` und `static uint16_t requiredVersion(const ProjectConfig&)` mit `std::max({ ... })`. Kommentar zusammenführen. `projectUuidBytes` als "reserviert, wird weiter geschrieben" kennzeichnen.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Dateiformat muss byte-identisch bleiben (Golden-Test: bekannte hcfg-Dateien v2..v7 laden und wieder schreiben). Test in `test_project_exporter.cpp`.

---

### CORE-426 – HorizonCode-`fire*`-Einstiegspunkte: fünf Makro-Familien und String-Temporäre
- **ID**: CORE-426
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `HorizonCodeRuntime.cpp:1711-1733`, `:1745-1761`, `:1765-1776`, `:1779-1850`, `:1855-1890`, handgeschrieben `:1807-1815`, `:1892-1990`; `:2002`, `:2019-2020`
- **Problem**: Rund 60 Einstiegspunkte entstehen aus fünf Makrofamilien plus 10 handgeschriebenen Varianten, die dieselbe Struktur wiederholen (`find`, kompiliert/interpretiert, `static const EventId`, `dispatchToListeners`). Der Namensparameter ist ein String-Literal, das für `runEventOnLevel(const std::string&)` und `dispatchToListeners(const std::string&)` jedes Mal einen `std::string` erzeugt. Namen mit mehr als 15 Zeichen (`"OnSelectionChanged"`, `"OnWindowFocusChanged"`, `"OnAnimationNotifyBegin"`, `"OnAnimationFinished"`) landen dabei bei jedem Event im Heap. `fireEvent` → `dispatchToListeners(id, event, arg)` hasht den Namen per `eventId(event)` bei jedem Aufruf neu.
- **Lösung**: Eine Funktionsvorlage `template<class Hook, class MakeArg> void fireBuiltin(InstanceId id, const BuiltinEvent& ev, int elem, Hook hook, MakeArg arg)` mit `static const BuiltinEvent ev{ "OnClicked", eventId("OnClicked") }`, wobei `BuiltinEvent` den `std::string` einmal hält. Damit entfallen die temporären Strings und die Makros. `Runtime`-Methoden bleiben Einzeiler.
- **Erwarteter Effekt**: Weniger Code (ca. 150 Zeilen), ein bis zwei Heap-Allokationen weniger pro längerem Event-Namen.
- **Risiko / Verifikation**: Gering; Semantik (Listener-Pass nur für bestimmte Familien, `HE_HC_LIFECYCLE_EVENT` ohne) per Parameter erhalten. `test_horizoncode_runtime.cpp`, UI-Widget-Tests.

---

## Gelesen

Alle 23 Dateien des Slice 4 vollständig gelesen: ja (`HorizonCodeRuntime.cpp` 1-2121, `HorizonCodeRuntime.h`, `HorizonGameServices.h`, `Aes256Gcm.cpp/.h`, `HpakFormat.h`, `HpakReader.cpp/.h`, `HpakWriter.cpp` 1-1277 / `.h`, `KeyDerivation.cpp/.h`, `ProjectConfig.cpp/.h`, `ProjectExporter.cpp` 1-1512 / `.h`, `IGameLogic.h`, `IntegrityProbe.cpp/.h`, `JobSystem.cpp/.h`, `AutoLandscapeMaterial.cpp/.h`). Außerhalb des Slice nur gezielt gegrept: `HAsset.h:437-477`, `HorizonCode.h:1463`, die Aufrufer von `exchangeState/eventBindingsOf/replicatedVariablesOf/fireTick`, `std::queue`-Nutzer.

## Core Slice 5 – MaterialGraph, Math, ParticleGraph, Physics (CollisionLayers/WaterWaves), Platform, Project, Renderer (IRenderer/UIFont/…), Scripting

## Zusammenfassung

Der Slice (33 Dateien, ca. 11 000 Zeilen) ist insgesamt sauber, gut kommentiert und defensiv geschrieben. Die meisten Strukturen sind bereits sinnvoll geteilt (GraphCommon, X-Makro für die Grafikfelder, ein gemeinsamer Notify-Helfer). Die relevanten Funde liegen an drei Stellen. Erstens das UI-Text-Pipeline in `UIFont.cpp`, das pro Label und Frame viele Heap-Allokationen und quadratische Messungen macht und 224 Byte pro Glyph erzeugt. Zweitens `Process.cpp` auf Windows, das den Vertrag aus `Process.h` bricht (Timeout, Callback-Thread). Drittens die Material-/Partikel-Auswertung ohne Memoisierung. Zusätzlich ein belegter Logikfehler in `isPathWithin` (Wurzel mit abschließendem Separator). Gefunden: 1 Hoch, 11 Mittel, 10 Niedrig. Messdaten liegen keine vor; alle Performance-Aussagen sind Schätzungen aus dem Code und als solche gekennzeichnet.

---

### CORE-501 – Windows `Proc::run`: Drain-Phase ohne Timeout, ein überlebender Enkelprozess hängt den Worker für immer
- **ID**: CORE-501
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (Robustheit/Korrektheit)
- **Ort**: `src/HE_Core/src/Platform/Process.cpp:340-371` (Windows-Zweig), Vergleich POSIX `:553-616`
- **Problem**: Der Timeout gilt nur für `WaitForSingleObject(pi.hProcess, waitMs)`. Endet der Kindprozess normal, kehrt das Wait mit `WAIT_OBJECT_0` zurück und danach folgt unbedingt `outThread.join()` / `errThread.join()`. Hat das Kind einen Daemon/Enkel gestartet, der die vererbten Schreib-Handles der Pipes hält (klassisch: `adb start-server`, `git fsmonitor`, Build-Server), bekommt `ReadFile` nie EOF und der Aufrufer blockiert unbegrenzt, obwohl `timeoutMs` gesetzt war. Der Job-Object-Kill (`TerminateJobObject`) wird nur im `WAIT_TIMEOUT`-Pfad ausgeführt, und das Job wird erst nach den `join()`s geschlossen. Die POSIX-Variante hat dieses Problem nicht, weil ihre Poll-Schleife die Deadline auch nach dem Kindende noch prüft (und die Prozessgruppe killt). Der Kommentar in `makePipe` (POSIX) beschreibt genau diesen Fehlerfall als real aufgetreten.
- **Code-Auszug**:
```cpp
const DWORD waitMs = o.timeoutMs ? o.timeoutMs : INFINITE;
if (::WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT) { ... TerminateJobObject ... }
outThread.join();
errThread.join();
```
- **Lösung**: Nach dem Prozessende die Reader nur noch bis zur Restdeadline abwarten. Variante ohne zweites Primitive: Reader mit `OVERLAPPED`/`PeekNamedPipe` statt blockierendem `ReadFile`, oder nach `WAIT_OBJECT_0` ein zweites `WaitForSingleObject` auf ein Event, das die Reader beim EOF setzen, mit Restdeadline; bei Ablauf `TerminateJobObject(job, 1)` (schließt die Pipe-Enden der Enkel) und dann `join()`. Skizze:
```cpp
const ULONGLONG deadline = o.timeoutMs ? GetTickCount64() + o.timeoutMs : 0;
// ... nach dem Prozess-Wait:
if (deadline && WaitForMultipleObjects(2, readerDone, TRUE, remaining(deadline)) == WAIT_TIMEOUT)
{ r.timedOut = true; TerminateJobObject(job, 1); }
outThread.join(); errThread.join();
```
- **Erwarteter Effekt**: Kein dauerhaft hängender Worker (Git/Source-Control, Codegen-Aufrufe). Kein Performance-Effekt im Normalfall.
- **Risiko / Verifikation**: Unsicher/messen: auf Windows mit einem Testprogramm reproduzieren, das einen Enkel mit geerbtem stdout startet und selbst beendet. `tests/test_process_runner.cpp` hat vermutlich nur POSIX-/Kurzläufer-Fälle; ein Fall „Kind endet, Enkel hält Pipe, Timeout 500 ms → `timedOut == true`“ ergänzen. `timedOut` bei sauberem Ende darf nicht fälschlich gesetzt werden.

---

### CORE-502 – Windows: Zeilen-Callbacks laufen auf den Drain-Threads, nicht auf dem Aufrufer-Thread, und unter einem Mutex
- **ID**: CORE-502
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit der Schnittstelle)
- **Ort**: `src/HE_Core/include/Platform/Process.h:31-33`, `src/HE_Core/src/Platform/Process.cpp:261-272, 343-344`
- **Problem**: `Process.h` verspricht: „The line callbacks are invoked on the calling thread, so they may touch the caller's state without locking“. Auf POSIX stimmt das (eine Poll-Schleife). Auf Windows ruft `drainPipe` → `splitter.feed` → `m_sink` den Callback aber auf `outThread`/`errThread` auf, serialisiert durch `mx`. Aufrufer, die sich auf das Versprechen verlassen (UI-Zustand ohne Lock beschreiben), haben auf Windows eine Datenrasse. Zudem hält der Mutex den jeweils anderen Strom an, solange ein Callback blockiert.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lock(mx);
into.append(buf, got);
splitter.feed(buf, got);   // → o.onStdoutLine(...) auf dem Reader-Thread
```
- **Lösung**: Entweder den Vertrag in `Process.h` ehrlich machen („auf unspezifiziertem Thread, serialisiert“) oder auf Windows die Zeilen über eine Queue an den Aufrufer-Thread übergeben: Reader füllen eine `std::deque<std::pair<bool,std::string>>`, der Aufrufer-Thread wartet in einer Schleife auf `{Prozess beendet, Queue nicht leer}` (Event) und ruft die Callbacks selbst auf. Das passt gut zur Lösung von CORE-501 (gleiche Wartestelle).
- **Erwarteter Effekt**: Plattformgleiches Verhalten; keine versteckten Races in Aufrufern wie `GitService`.
- **Risiko / Verifikation**: Reihenfolge stdout/stderr bleibt wie bisher nicht garantiert. Aufrufer der Callbacks (`rg "onStdoutLine"`) prüfen, ob sie bereits eigene Locks haben. Test mit Callback, der einen nicht-atomaren Zähler hochzählt und `std::this_thread::get_id()` mit dem Aufrufer vergleicht.

---

### CORE-503 – `approxFoldNode` ohne Memoisierung: exponentielle Laufzeit bei Diamanten im Graphen
- **ID**: CORE-503
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp:2177-2186, 2233-2369`
- **Problem**: Der CPU-Fold (`matGraphApproxSurface`, aufgerufen beim Materialladen in `ContentManager.cpp:749`, im `HpakWriter` und in den MCP-Tools) wertet jede Eingabe rekursiv neu aus. Wird ein Knoten mehrfach genutzt (z. B. `Multiply(x, x)` mit beiden Pins aus derselben Quelle, ein typisches „Quadrat“), wird sein Teilgraph zweimal gefaltet. Eine Kette von n solchen Knoten kostet 2^n Besuche; die einzige Bremse ist `depth > 32`, also bis 2^32. Der Codegen (`emitNode`) memoisiert dagegen korrekt (`c.outVar`), der Fold nicht. Zusätzlich sucht jeder Besuch den Link linear (`approxFindLink`).
- **Code-Auszug**:
```cpp
case MatNodeType::Add: case MatNodeType::Subtract: case MatNodeType::Multiply: case MatNodeType::Divide:
    if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
        !approxFoldInput(g, *n, 1, depth + 1, b, sw)) return false;
```
- **Lösung**: Pro `matGraphApproxSurface`-Aufruf einen Cache `std::unordered_map<int, std::optional<std::array<float,4>>>` (Knoten-id → Ergebnis, `nullopt` = nicht faltbar) mitführen und in `approxFoldNode` vor dem `switch` prüfen. Das Ergebnis ist pro Knoten deterministisch (Output-Pin spielt bei der Faltung keine Rolle), das Verhalten bleibt also identisch. Dazu optional einen `dstNode*64+dstPin → Link`-Index (siehe CORE-513).
```cpp
struct FoldCtx { const MaterialGraph& g; const std::map<std::string,bool>* sw;
                 std::unordered_map<int, std::optional<std::array<float,4>>> memo; };
```
- **Erwarteter Effekt**: Von exponentiell auf linear in Knoten+Links. Bei normalen Graphen (< 50 Knoten) vermutlich nicht messbar, bei Diamant-Ketten der Unterschied zwischen Millisekunden und Hängern (Schätzung).
- **Risiko / Verifikation**: Gering. Test in `tests/test_material_graph.cpp`: Kette aus 28 `Multiply(x,x)` gegen ein Zeitlimit; bestehende Fold-Tests müssen bit-gleich bleiben (der Cache ändert keine Werte). Zyklen: weiterhin über `depth`-Guard abfangen (Cache erst nach erfolgreichem Fold schreiben).

---

### CORE-504 – Text-Emission: pro Label ein `vector<string>` mit zeichenweiser Kopie, Messung und Zeichnung layouten doppelt
- **ID**: CORE-504
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:459-488` (`layoutUITextLines`), `:1113-1125` (`measureUIText`), `:1133-1194` (`emitUITextGlyphs`)
- **Problem**: `emitUITextGlyphs` (43 Aufrufstellen in Core/Scene/Editor) ruft `layoutUITextLines`, das auch für einen einzeiligen Text ohne Umbruch den ganzen String Zeichen für Zeichen in `cur` kopiert (`cur.push_back(c)`), dann in `lines` kopiert und zurückgibt. Danach ruft `emitUITextGlyphs` `lineWidth` pro Zeile (zweiter UTF-8-Durchlauf) und dann die Glyphenschleife (dritter Durchlauf). Auto-sizende Elemente rufen zusätzlich `measureUIText`, das dasselbe Layout noch einmal berechnet. Wird der Text pro Frame neu extrahiert, entstehen pro Label mindestens zwei Heap-Strings plus ein Vektor pro Frame.
- **Code-Auszug**:
```cpp
std::string cur;
for (char c : text)
{
    if (c == '\n') { lines.push_back(cur); cur.clear(); }
    else if (c != '\r') cur.push_back(c);
}
lines.push_back(cur);
```
- **Lösung**: Schnellpfad: enthält der Text weder `'\n'` noch `'\r'` und ist `!wrap` (häufigster Fall), `std::string_view`/Byte-Ranges verwenden, wie es `uiTextLineRanges` schon tut, statt Strings zu erzeugen. Intern auf `std::vector<std::string_view>` (oder Ranges) umstellen und `lineWidth` für `string_view` anbieten; nur die Wrap-Pfade, die wirklich Text verändern (Trimmen), brauchen eigene Strings. Die öffentliche Funktion `layoutUITextLines` kann für Aufrufer erhalten bleiben und die interne Variante nutzen.
- **Erwarteter Effekt**: Schätzung: 2–3 Allokationen weniger pro Label und Frame; bei Hunderten Labels im HUD spürbar im UI-Extract. Messen: Frame-Profil von `WidgetManager::extract`/`UISystem::extract` mit einem Text-lastigen Widget.
- **Risiko / Verifikation**: Ausgabe muss byte-identisch bleiben (CRLF-Behandlung, entfernte Schluss-Leerzeile, UTF-8). Tests: `tests/test_ui_font_weight.cpp` und Widget-Tests (`tests/test_ui_widgets.cpp`) laufen lassen; neuen Test für `"a\r\nb\n"`, `""`, `"a\n\n"` gegen die alte Implementierung (Golden-Vergleich).

---

### CORE-505 – Wrap-Algorithmen sind quadratisch und erzeugen pro Wort Kopien
- **ID**: CORE-505
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:498-541` (`layoutUITextLines`), `:1029-1030, 1054, 1069-1075` (`uiTextWrapRanges`)
- **Problem**: Beim Umbruch wird für jedes Wort die gesamte bisherige Zeile neu vermessen (`lineWidth(font, trimmed(acc + word), scale)`: ein Temporärstring plus eine Kopie in `trimmed`, dann O(Zeilenlänge)). Bei `uiTextWrapRanges` erzeugt `widthOf` pro Aufruf ein `substr` und misst von `segStart` neu; das Hard-Break-Innere (`while (k < wEnd) { widthOf(segStart, k) ...}`) ist O(k²) mit k Allokationen. Ein einzelnes langes Wort (URL, Pfad im Eingabefeld) kostet damit Millisekunden. `uiTextWrapRanges` läuft für editierbare Textfelder bei jeder Änderung bzw. Zeichnung.
- **Code-Auszug**:
```cpp
auto widthOf = [&](std::size_t from, std::size_t to)
{ return lineWidth(font, text.substr(from, to - from), scale); };
...
if (widthOf(segStart, k) > wrapWidth) break;   // pro Zeichen neu vom Zeilenanfang
```
- **Lösung**: Breite inkrementell mitführen: `float rowW` (Breite bis `visEnd`) und Wortbreite `wordW` einmal per Durchlauf über `[cur, wEnd)` berechnen; Entscheidung `rowW + gapW + wordW > wrapWidth` ohne Neumessung. Für den Hard-Break das Prefix zeichenweise akkumulieren (`acc += glyph->xadvance * scale`), bis die Grenze überschritten ist. `lineWidth` bekommt eine `string_view`-/Range-Überladung (kein `substr`). Ergebnis (Zeilengrenzen) bleibt identisch, weil dieselben Advance-Summen verwendet werden (Gleitkomma-Summation in gleicher Reihenfolge beibehalten, sonst Randfälle bei exakt passender Breite).
- **Erwarteter Effekt**: Von O(n²) auf O(n) pro Zeile, keine Allokationen pro Wort. Schätzung: bei Absätzen mit > 1000 Zeichen Faktor 10+; bei kurzen Labels vernachlässigbar.
- **Risiko / Verifikation**: Gleitkommareihenfolge: Summe muss genauso gebildet werden wie `lineWidth` (links nach rechts). Differenztest alt/neu mit zufälligen Texten (ASCII, Umlaute, lange Wörter, Leerzeichenfolgen) und mehreren `wrapWidth`; vorhandene Tests zu Textfeldern (`tests/test_ui_*`).

---

### CORE-506 – `UIRenderObject` ist 224 Byte groß und wird pro Glyph erzeugt; `push_back` ohne `reserve`
- **ID**: CORE-506
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Renderer/UIRenderObject.h:7-100`, `src/HE_Core/src/Renderer/UIFont.cpp:958-967, 1181-1190`
- **Problem**: Jeder Glyph-Quad (`type == 2`) trägt alle Felder eines Rechtecks: zwei `UUID` (32 B), Border, Gradient, Radii, Innenschatten, `clipRect`, Rotation, `uiState` usw. `sizeof(UIRenderObject)` ist 224 Byte (nachgemessen mit g++/glm). Ein Bildschirm mit 5000 Zeichen erzeugt pro Frame über 1 MB Quad-Daten, die danach (Sortierung nach `uiSortKey`, Batching nach Clip/Atlas, Übergabe an `RenderWidgetThumbnail`/Backends) mehrfach kopiert bzw. verschoben werden. Dazu wächst `out` bei jedem Label durch `push_back` ohne `reserve` mehrfach.
- **Code-Auszug**:
```cpp
UIRenderObject ro;            // 224 B, alle Felder default-initialisiert
ro.position = ...; ro.type = 2; ...
out.push_back(std::move(ro));
```
- **Lösung**: Zwei kleine Schritte ohne Formatwechsel: (1) in den Emit-Funktionen `out.reserve(out.size() + text.size())` (obere Schranke Glyphen = Zeichen); (2) mittelfristig Glyphen als eigene kompakte Struktur (`position, size, uvMin, uvMax, color, layer, atlasKey, clipRect` ≈ 72 B) führen oder rare Felder (`borderColor`, `gradient*`, `innerShadow*`, `uiState`, beide UUIDs) in ein optionales, selten genutztes Sub-Struct auslagern. Sortieren per Index-Array statt Objekte zu verschieben.
- **Erwarteter Effekt**: Schätzung: 2–3× weniger Speichertraffic im UI-Pfad bei textlastigen Szenen; `reserve` allein spart die Realloc-Kaskade. Messen: `sizeof` × Anzahl Quads im UI-Extract, Zeit für `std::stable_sort` der UI-Objekte.
- **Risiko / Verifikation**: Teil (2) ändert das Backend-Interface (6 Backends, `MetalRenderer.mm` etc.) und ist nur nach Messung sinnvoll. `reserve` ist risikolos. Backend-Bildvergleich (`HE_UI_DUMP_DIR`) nach Umbau.

---

### CORE-507 – `uiLayoutRichText`: pro Zeichen vier Mal `advanceAt` (inkl. `resolveFace`-Hash-Lookup), tote Variable
- **ID**: CORE-507
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:768-791, 813-843, 857-876, 899-905`
- **Problem**: `advanceAt(b)` löst bei jedem Aufruf `runAt` (Cursor), `faceAt` → `resolveFace` und `uiUtf8Decode` + `glyph()` auf. Für Nicht-Base-Faces (`<b>`, Icons) bedeutet `resolveFace` jedes Mal `uiEngineFaceKey` (statische Guard) und `UIFontCache::find` (unordered_map-Lookup). Die Funktion wird pro Zeichen im Umbruch-Scan, in der Zeilenhöhen-Schleife (zweimal: Summe `w` und getrimmte Breite), und in der Piece-Schleife aufgerufen. Die Summe `w` der Höhenschleife wird nie benutzt (`(void)w;`), ist also tote Arbeit. `runAt` bewegt den Cursor außerdem rückwärts (`while (runCursor > 0 && b < ...) --runCursor`), was bei den wiederholten Zeilendurchläufen jedes Mal zurücklaufen muss.
- **Code-Auszug**:
```cpp
float mx = 0.0f, w = 0.0f;
...
w += advanceAt(b);
...
(void)w;
```
- **Lösung**: Einmal pro Zeichen die Advance-Werte in ein `std::vector<float> adv(text.size())` (am Zeichenanfang-Byte) füllen und überall nachlesen; `faceAt` je Run vorab in `std::vector<ResolvedFace>` auflösen. Die tote Summe `w` entfernen.
- **Erwarteter Effekt**: Schätzung: 3–4× weniger Arbeit im Rich-Text-Layout; relevant nur, wenn das Layout pro Frame läuft (Cache in `UIElement.cpp:1395` gilt für das Parsen, nicht nachgewiesen für das Layout – messen).
- **Risiko / Verifikation**: Gleiche Werte, gleiche Summationsreihenfolge → bit-gleich. Es gibt laut Kommentar einen Test, der „plain == rich“ pinnt; zusätzlich Test mit `<b>`/`<icon=…>` und Mischgrößen.

---

### CORE-508 – `isPathWithin` weist jedes Kind einer Wurzel mit abschließendem Separator ab (toter `"."`-Zweig)
- **ID**: CORE-508
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: `src/HE_Core/include/Platform/PathSafety.h:29-49`
- **Problem**: Der Kommentar behauptet, `lexically_normal` hinterlasse bei „a/“ ein abschließendes `"."`. Tatsächlich liefert `lexically_normal` für `"/proj/Content/"` und `"/proj/Content/."` einen leeren letzten Pfadbestandteil (`""`), kein `"."`. Der Zweig `if (*ri == ".")` wird dadurch nie erreicht; der leere Bestandteil wird mit dem Kandidatenelement verglichen und `isPathWithin("/proj/Content/", "/proj/Content/a.txt")` ergibt `false`. Nachgemessen (g++ -std=c++20):
```
/proj/Content   | /proj/Content/a.txt -> 1
/proj/Content/  | /proj/Content/a.txt -> 0     (falsch)
/proj/Content/. | /proj/Content/a.txt -> 0     (falsch)
/proj/Content/  | /proj/Content       -> 0     (falsch)
```
  Aktuell fail-closed (kein Sicherheitsloch), aber ein Aufrufer, der einen Pfad aus Nutzereingabe/Dialog mit Slash am Ende übergibt, lehnt dann alles ab. Aufrufer heute: `EditorApplication.cpp:10165` (baut über `/ "Source"`, daher unkritisch).
- **Code-Auszug**:
```cpp
if (*ri == ".") { ++ri; break; }
if (ci == c.end()) return false;
if (*ci != *ri)    return false;
```
- **Lösung**: Einfacher und korrekt über `lexically_relative` oder durch Entfernen des leeren Schlussbestandteils:
```cpp
std::filesystem::path r = root.lexically_normal(), c = candidate.lexically_normal();
if (!r.has_filename()) r = r.parent_path();     // "/proj/Content/" -> "/proj/Content"
if (!c.has_filename()) c = c.parent_path();
auto rel = c.lexically_relative(r);
return !rel.empty() && *rel.begin() != "..";
```
  (`lexically_relative` liefert `"."` für gleiche Pfade, das ist erlaubt; ein leeres Ergebnis bedeutet „nicht darstellbar“ → false, z. B. verschiedene Laufwerke.) Den toten Zweig samt falschem Kommentar entfernen.
- **Erwarteter Effekt**: Korrekte Semantik, weniger Code.
- **Risiko / Verifikation**: Sicherheitsrelevant: alle bestehenden Fälle aus `tests/test_collab_controller.cpp:1776-1785` müssen weiter bestehen (Geschwisterverzeichnis `Content-backup`, `..`-Ausbruch), neu: Wurzel mit `/` am Ende, `"."` am Ende, Windows-Laufwerksbuchstaben (`C:/a` vs `D:/a`), Groß-/Kleinschreibung unter Windows bleibt wie bisher (case-sensitiver Vergleich, ggf. separat entscheiden).

---

### CORE-509 – Lua: jede Instanz kopiert alle Modul-Schlüssel; Hot-Reload muss alle Instanzen patchen
- **ID**: CORE-509
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Scripting/ScriptEngine.cpp:154-172` (`createInstance`), `:591-613` (`hotReloadScript`)
- **Problem**: `createInstance` legt pro Entity eine Tabelle an und kopiert per `lua_next`/`lua_rawset` jeden Schlüssel des Modulstabelle (alle Funktionen plus `properties` usw.) hinein. Bei 1000 Entities mit einem 15-Funktionen-Skript sind das 15 000 Tabelleneinträge und Hash-Inserts beim Spawn, plus Speicher pro Instanz. `hotReloadScript` muss dafür für jede Instanz die ganze Modultabelle erneut durchlaufen (O(Instanzen × Schlüssel)). Das übliche Lua-Muster (Instanztabelle mit Metatabelle `__index = module`) vermeidet beides.
- **Code-Auszug**:
```cpp
while (lua_next(m_L, -3) != 0)
{
    lua_pushvalue(m_L, -2);
    lua_insert(m_L, -2);
    lua_rawset(m_L, -4);   // instance_table[key] = value
}
```
- **Lösung**: Pro Skript eine Metatabelle `{ __index = moduleTable }` einmal in der Registry halten (zweite Ref in `ScriptRef`), `createInstance` = `lua_newtable` + `lua_rawgeti(meta)` + `lua_setmetatable`. Hot-Reload ersetzt dann nur den Inhalt der Modultabelle (oder tauscht `__index`), Instanzdaten bleiben unberührt. `pushInstanceMethod` funktioniert unverändert, da `lua_getfield` den `__index` auswertet.
- **Erwarteter Effekt**: Schätzung: Spawnkosten pro Instanz von O(Funktionen) auf O(1), weniger Speicher; Hot-Reload O(Schlüssel) statt O(Instanzen × Schlüssel). Messen: Zeit für 5000 `createInstance`.
- **Risiko / Verifikation**: Verhaltensunterschiede für Skripte, die `pairs(self)`, `rawget(self, "onUpdate")` oder `next(self)` verwenden, und für Skripte, die Modulfunktionen nach dem Spawn ändern (`M.foo = ...` würde jetzt live wirken, vorher nicht). `ScriptContext` (HE_Scene) und HE_Python prüfen (`rg "lua_next|rawget" src/HE_Scene`). Tests: `tests/test_script_*` (Lua), insbesondere Hot-Reload mit erhaltenen Datenfeldern und `injectProperties`. Als Entscheidung dokumentieren; nicht still einführen.

---

### CORE-510 – ~16 fast identische `callOn…`-Hooks in Interface und Lua-Backend
- **ID**: CORE-510
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/Scripting/IScriptBackend.h:52-146`, `src/HE_Core/src/Scripting/ScriptEngine.cpp:217-407`
- **Problem**: Jeder Hook wiederholt den gleichen Rumpf: Instanz suchen, Fehlermeldung setzen, `pushInstanceMethod`, Argumente pushen, `pcall(n, 0)`. Es gibt bereits drei Abkürzungen (`callNotifyMethod`, `callInstanceMethod` mit `std::function`, und die Enum-Variante für UI/Net), aber `callOnStart/Update/CollisionEnter/Exit/Overlap×2/Timer/CheatDetected/InputAxis/InputAxis2D` sind weiter handkopiert. Im Interface kommen pro neuem Event zwei Stellen (Interface + Python-Plugin + Lua) dazu; die Autoren haben das für UI/Net selbst als Problem erkannt („six near-identical virtuals would only spread it out“), aber nicht für die übrigen Paare.
- **Code-Auszug**:
```cpp
bool ScriptEngine::callOnTimer(InstanceId id, int handle)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) { m_lastError = "Invalid instance id"; return false; }
    if (!pushInstanceMethod(m_L, it->second.luaRef, "onTimer")) return true;
    lua_pushinteger(m_L, static_cast<lua_Integer>(handle));
    return pcall(2, 0);
}
```
- **Lösung**: Einen privaten Template-Helfer einführen (kein `std::function`, kein Heap):
```cpp
template <class Push>
bool ScriptEngine::callHook(InstanceId id, const char* fn, int nargs, Push&& push)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) { m_lastError = "Invalid instance id"; return false; }
    if (!pushInstanceMethod(m_L, it->second.luaRef, fn)) return true;
    push(m_L);
    return pcall(1 + nargs, 0);
}
bool ScriptEngine::callOnTimer(InstanceId id, int h)
{ return callHook(id, "onTimer", 1, [&](lua_State* L){ lua_pushinteger(L, h); }); }
```
  Damit schrumpfen ca. 150 Zeilen auf ca. 60. Das öffentliche Interface (ABI zum Python-Plugin) bleibt unverändert; eine Zusammenfassung der Hooks zu einem `callEvent(id, Kind, payload)` wäre ein ABI-Bruch und nur bei ohnehin geplantem Plugin-Update sinnvoll.
- **Erwarteter Effekt**: Weniger Code, ein Ort für Fehlermeldung und Stack-Disziplin. Laufzeit unverändert.
- **Risiko / Verifikation**: Gering, rein mechanisch. Bestehende Lua-Skript-Tests laufen lassen. Fehlermeldung „Invalid instance id“ und das Nicht-Setzen von `m_lastError` bei fehlendem Handler beibehalten.

---

### CORE-511 – `ProjectSettings`: dieselbe Feldliste in vier handgepflegten Funktionen
- **ID**: CORE-511
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Project/ProjectSettings.cpp:164-224` (`operator==`), `:226-334` (`clamp`), `:336-435` (`toJson`), `:437-567` (`fromJson`); Vorbild: `ProjectSettings.h:213-252` (`HE_PROJECT_GRAPHICS_FIELDS`)
- **Problem**: Für `graphics` ist die Feldliste ein einziges X-Makro (laut Header „a field is added in exactly one place“). Für alle anderen Abschnitte (shadows, physics, anticheat, multiplayer, …, ca. 60 Felder) steht jedes Feld in `operator==`, `toJson`, `fromJson` und teilweise `clamp` einzeln. Ein neues Feld braucht vier Edits; vergisst man `operator==`, erkennt `isDefault()`/der Panel-Dirty-Vergleich die Änderung nicht, vergisst man `toJson`, wird sie nicht gespeichert – beides still. Zusätzlich wird in `operator==` für die Float-Felder `nearlyEqual` von Hand gesetzt, während der Rest `==` benutzt (Inkonsistenz-Risiko).
- **Code-Auszug**:
```cpp
&& nearlyEqual(shadows.distance, o.shadows.distance)
&& shadows.cascadeCount == o.shadows.cascadeCount
&& shadows.resolution == o.shadows.resolution
```
- **Lösung**: Pro Abschnitt ein kleines X-Makro oder (C++20) eine `fields()`-Tupel-Funktion `auto fields() { return std::tie(distance, cascadeCount, ...); }` je Struct; `operator==` iteriert über beide Tupel mit der vorhandenen `fieldEqual`-Überladung, `fromJson` über `readField`. Schlüsselnamen bleiben als Textliste im Makro, damit das Dateiformat byte-gleich bleibt. Nur die Clamp-Regeln (Wertebereiche, die teils vom Feld abhängen) bleiben handgeschrieben.
- **Erwarteter Effekt**: Ein Edit pro Feld, keine stillen „vergessen“-Fehler. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: JSON-Ausgabe muss byte-gleich bleiben (Reihenfolge der Schlüssel in `toJson`, verschachtelte Objekte `score`/`policy`/`prediction`/`discovery`, `sourceControl.mode` als String). `tests/test_project_settings.cpp`: Golden-Vergleich der Ausgabe vor/nach dem Umbau; Test, der per Reflection prüft, dass jedes Feld im Roundtrip und im `==` vorkommt.

---

### CORE-512 – `AABB::transformed`: 8 volle Matrix-Vektor-Multiplikationen statt Arvo-Methode
- **ID**: CORE-512
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Math/AABB.h:44-58`; Aufrufer im Frame-Pfad: `src/HE_Rendering/src/RenderExtractor.cpp:466, 614, 636, 798`, `FoliageExtract.cpp:158`, `RenderObject.h:145`
- **Problem**: Für jedes sichtbare Objekt und jeden Frame werden 8 Ecken mit `m * vec4(corner, 1)` (je 16 Mul + 12 Add) transformiert und per `min/max` zusammengeführt. Für affine Matrizen genügt die Center/Extent-Methode (Arvo): ca. 18 Mul, ohne Verzweigung auf `isValid` in der Schleife.
- **Code-Auszug**:
```cpp
for (int i = 0; i < 8; ++i) {
    const glm::vec3 corner{ (i & 1) ? max.x : min.x, ... };
    out.expand(glm::vec3(m * glm::vec4(corner, 1.0f)));
}
```
- **Lösung**:
```cpp
const glm::vec3 c = center(), e = extents();
const glm::vec3 nc = glm::vec3(m[3]) + glm::vec3(m[0]) * c.x + glm::vec3(m[1]) * c.y + glm::vec3(m[2]) * c.z;
const glm::vec3 ne = glm::abs(glm::vec3(m[0])) * e.x + glm::abs(glm::vec3(m[1])) * e.y + glm::abs(glm::vec3(m[2])) * e.z;
out.min = nc - ne; out.max = nc + ne;
```
  Ergebnis ist mathematisch identisch (für Matrizen mit letzter Zeile (0,0,0,1); die bestehende Funktion nimmt das ebenfalls an, `w` wird nicht dividiert), kann sich aber in den letzten Float-Bits unterscheiden.
- **Erwarteter Effekt**: Schätzung: 3–4× weniger Arithmetik pro Aufruf; bei 10 000 Objekten im Extract im Bereich 0,1 ms. Nur bei einem Frame-Profil relevant, daher vor Umsetzung messen.
- **Risiko / Verifikation**: Leicht andere letzte Bits können Tests mit exaktem Vergleich brechen (`rg "transformed" tests/`) und das Culling um ein ULP verschieben. Test: zufällige affine Matrizen, Vergleich mit der 8-Ecken-Version mit Toleranz 1e-5; Nicht-affine Matrizen (Projektion) dürfen nie hier landen.

---

### CORE-513 – MaterialGraph: lineare Registry- und Link-Suchen im Codegen und beim JSON-Laden
- **ID**: CORE-513
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp:327-339` (`matNodeDesc`, `matNodeDescByName`), `:1430, 1479, 1511` (Aufrufer im Codegen), `:1487-1494, 1521-1526, 2155-2160` (Link-Suche), `:1253, 1470-1475, 1505` (Funktionsschnittstelle)
- **Problem**: (a) `matNodeDesc(type)` ist eine lineare Suche über ca. 95 Registry-Einträge und wird pro Knoten mehrfach aufgerufen (`emitNode`, `outputPinType`, `inputExpr`, JSON-Schreiben); `matNodeDescByName` vergleicht pro geladenem Knoten mit bis zu 95 Namen (`std::string == const char*`). (b) `inputExpr`, `hasInput` und `uvInput` scannen die komplette Linkliste pro Pin → O(Knoten × Links); `uvInput` scannt zweimal. (c) `fnInterfaceNodes` (alloziert + sortiert) wird für jeden `FnInput`-Knoten und für jeden Pin eines `FunctionCall` neu gebaut, `outputPinType` baut dazu `matFunctionPins` (zwei Vektoren) pro Link. Alles läuft bei jeder Shader-Generierung (Editor ruft `generateFragment` an ca. 7 Stellen), nicht pro Frame; bei dem 151-Knoten-Landscape-Graph vernachlässigbar.
- **Code-Auszug**:
```cpp
const MatNodeDesc& matNodeDesc(MatNodeType type)
{
    for (const auto& d : registry())
        if (d.type == type) return d;
    return registry().front();
}
```
- **Lösung**: (a) Beim ersten Zugriff ein `std::array<const MatNodeDesc*, 256>` nach `uint8_t`-Typ und eine `unordered_map<string_view,const MatNodeDesc*>` für Namen aufbauen. (b) Pro `Scope` einen Index `unordered_map<uint64_t(dstNode<<32|dstPin), const MatGraphLink*>` beim ersten Zugriff füllen. (c) Funktionsschnittstellen pro Aufruf von `generateFragment` im `EmitCtx` cachen (Schlüssel = Funktionsgraph-Zeiger).
- **Erwarteter Effekt**: Nur bei sehr großen Graphen messbar (Schätzung: Codegen-Zeit O(N·L) → O(N+L)). Niedrige Priorität, erst messen (Editor-Latenz beim Ziehen eines Parameterknotens in einem großen Graphen).
- **Risiko / Verifikation**: Gering. Erzeugter GLSL-Text muss byte-gleich bleiben (Pipeline-Cache-Schlüssel!). `tests/test_material_graph.cpp` enthält Codegen-Vergleiche; mit Golden-Text für den Landscape-Graph prüfen.

---

### CORE-514 – MaterialGraph: StaticSwitch-Auflösung dreifach dupliziert, kleine Codegen-Reste
- **ID**: CORE-514
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MaterialGraph.cpp:1228-1236` (Codegen), `:2267-2272` (`approxFoldNode`), `:2386-2392` (`approxSource`); `:1584` und `:1599` (doppelte `uiDomain`-Berechnung); `:1948-1962` (leerer Zweig); `:1914-1926` (große String-Kopien)
- **Problem**: Der Code für „Name des Schalters, Override aus der Map, Node-Default“ steht dreimal wörtlich da (`swName = n.s.empty() ? "switch_" + id : n.s`). Weicht eine Kopie ab, folgen Codegen und GI-Fold verschiedenen Zweigen. In `generateFragment` wird `uiDomain` zweimal mit demselben Ausdruck berechnet (ein Kommentar erklärt die Reihenfolge, aber `c.uiDomain` ließe sich einfach später wiederverwenden), `if (uiDomain) { /* no G-buffer tail */ }` ist ein leerer Zweig nur zur Kettenbildung, und `common` (der gesamte Shader-Prolog) wird in `src = withArrayMask(header) + common` und `withArrayMask(headerGB) + common` zweimal kopiert, obwohl Header kurz und `common` groß ist.
- **Code-Auszug**:
```cpp
const std::string swName = n->s.empty() ? ("switch_" + std::to_string(n->id)) : n->s;
bool on = n->p[0] > 0.5f;
if (sw) if (auto it = sw->find(swName); it != sw->end()) on = it->second;
```
- **Lösung**: Freie Funktion `bool resolveSwitch(const MatGraphNode&, const std::map<std::string,bool>*, std::string* nameOut)` und an den drei Stellen verwenden; `uiDomain` einmal berechnen; Tail-Aufbau als `if (!uiDomain) { if (lit) ... else ...; gb += ... }`. Die Kopie von `common` lässt sich vermeiden, indem man `src`/`gb` direkt mit `reserve(header.size()+common.size()+tail)` bildet; Gewinn gering.
- **Erwarteter Effekt**: Weniger Divergenzrisiko, ca. 20 Zeilen weniger. Kein messbarer Laufzeiteffekt.
- **Risiko / Verifikation**: Rein mechanisch; Codegen-Text byte-gleich (Golden-Test), insbesondere Switch-Vergleich mit Override und doppeltem Schalternamen (der erste gewinnt, `c.switches`).

---

### CORE-515 – Partikel-Graph: Auswertung ohne Memo, `RandomRange` würfelt pro Verbraucher, UB bei min > max
- **ID**: CORE-515
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/ParticleGraph/ParticleGraph.cpp:242-313` (insbesondere `:277-283`), Header `ParticleGraph.h:13-17`
- **Problem**: (a) Wie in CORE-503 wird ein mehrfach genutzter Teilgraph bei jedem Verbraucher neu ausgewertet (bis `depth <= 32`, also 2^32 bei einer Diamantkette). Die Knotenzahl der Partikelgraphen ist klein, praktisch kaum relevant. (b) `RandomRange` ruft bei jeder Auswertung `dist(rng)`: speist ein Knoten zwei Pins (Start- und Endgröße), erhält jeder Pin einen anderen Zufallswert. Der Header sagt „Resolves once per evaluate() call“, was man als „ein Wert pro Knoten“ liest; der Code liefert „ein Wert pro Verbraucher“. (c) `std::uniform_real_distribution<float>(p0, p1)` ist undefiniert, wenn `p0 > p1` oder nicht endlich (aus Datei/Eingabe möglich).
- **Code-Auszug**:
```cpp
case ParticleNodeType::RandomRange:
{
    std::uniform_real_distribution<float> dist(n->p[0], n->p[1]);
```
- **Lösung**: `lo = std::min(p0,p1)`, `hi = std::max(p0,p1)` und bei Nicht-Endlichkeit auf `lo` zurückfallen. Für (a)/(b) pro `evaluateParticleGraph` eine Memo-Map `nodeId → vec3` führen; das ändert für fan-out-genutzte `RandomRange` das Ergebnis (gleicher Wert an beiden Pins) und ist damit eine bewusste Verhaltensentscheidung, die man mit dem Autor des Headers klären muss. Nur (c) ist risikolos.
- **Erwarteter Effekt**: (c) Robustheit; (a)/(b) kein messbarer Gewinn, aber dokumentierte Semantik.
- **Risiko / Verifikation**: `tests/test_particle_graph.cpp`: Fall `RandomRange` mit zwei Verbrauchern (aktuelles Verhalten festschreiben oder ändern), Fall min > max. Seed-abhängige Ergebnisse bei Reihenfolgeänderungen der rng-Aufrufe prüfen.

---

### CORE-516 – `sampleWaves`: Richtungs-Trigonometrie und `k` pro Aufruf statt pro Wellenzug
- **ID**: CORE-516
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Physics/WaterWaves.h:78-98` (Zeilen 86-88: `cos(rad)`, `sin(rad)`, `k`)
- **Problem**: Für jeden Auftriebs-/Query-Punkt und jede Physik-Teilschritt werden pro Wellenzug `cos(rad)`, `sin(rad)` und `k` neu berechnet, obwohl sie nur von `WaveTrain` abhängen. Pro Punkt kommen dann `sin(ph)` und `cos(ph)` hinzu. Bei zwei Trains also 4 überflüssige Trig-Aufrufe pro Punkt (von 8).
- **Code-Auszug**:
```cpp
const double rad = static_cast<double>(w.directionDeg) * (kWaveTwoPi / 360.0);
const double dx  = std::cos(rad), dz = std::sin(rad);
const double k   = kWaveTwoPi / static_cast<double>(w.length);
```
- **Lösung**: Eine `PreparedTrain { double dx, dz, k, speed, steep; }`-Struktur einmal pro Wasserkörper und Schritt aufbauen (z. B. in `WaterQuery`, HE_Scene), `sampleWaves(const PreparedTrain*, int, x, z, t)` als Überladung; `sin`/`cos(ph)` über `sincos` (GCC/Clang) bzw. gemeinsamen Aufruf. Die bisherige Funktion bleibt als Wrapper und bleibt für die Material-Generatoren gültig.
- **Erwarteter Effekt**: Schätzung: ca. 40–50 % weniger Trig-Aufrufe in der Wasserabfrage. Wegen `double` und weniger Punkten pro Frame kaum messbar; erst mit `test_water_buoyancy`/Profil mit vielen Schwimmkörpern prüfen.
- **Risiko / Verifikation**: Rundung identisch, solange `dx,dz,k` mit denselben Ausdrücken berechnet werden (Bit-gleich). `tests/test_water_query.cpp`, `test_water_buoyancy.cpp`.

---

### CORE-517 – `UIFontCache::keyFor`: fehlgeschlagener Bake wird nicht gemerkt, 32-Bit-Hash ohne Kollisionsprüfung, globale Zustände ohne Synchronisation
- **ID**: CORE-517
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:221-224, 366-387`; Aufrufer `UIWidget/WidgetManager.cpp:218`, `HE_Editor/AssetThumbnailCache.cpp:298`
- **Problem**: (a) Schlägt `bakeInto` fehl, `return 0` ohne Eintrag im Cache; jeder weitere Aufruf mit derselben defekten TTF backt erneut (bis zu drei Pack-Versuche mit Atlasverdopplung auf 4096², also Dutzende MB Allokation). (b) `key = (uint32_t)(h ^ (h >> 32))` und danach `c.find(key) == c.end()`: zwei verschiedene Fonts mit gleichem 32-Bit-Key teilen sich stillschweigend das Atlas des ersten. (c) `g_scripts`, `g_weightBold`, `g_sharedBaked` und die `cache()`-Map sind nicht atomar/gelockt, obwohl `keyFor` aus Editor-Hintergrund-Pfaden (Thumbnails) erreichbar sein kann – unsicher, ob jemals von mehreren Threads genutzt.
- **Code-Auszug**:
```cpp
if (c.find(key) == c.end())
{
    BakedUIFont f; ...
    bakeInto(ttf.data(), f, uiFontScripts());
    if (!f.ok) return 0;      // nichts gemerkt
    c.emplace(key, std::move(f));
}
```
- **Lösung**: Fehlschlag als `BakedUIFont` mit `ok=false` cachen (die Anrufer prüfen bereits `ok`); Key als 64 Bit halten und intern auf `uint64` indizieren, nach außen nur den (zufällig kollisionsarmen) 32-Bit-Wert, oder bei Kollision hochzählen (`while (c.count(key) && c[key].stableId != id) ++key`). Für (c) einen `std::mutex` um `cache()`-Schreibzugriffe oder in der Dokumentation „nur Main-Thread“ festschreiben.
- **Erwarteter Effekt**: Vermeidet Wiederholtes Backen defekter Fonts; Kollisionssicherheit.
- **Risiko / Verifikation**: Gering. Test: kaputte TTF zweimal, zählt Bakes; zwei Font-IDs mit erzwungenem Hash-Konflikt. `tests/test_ui_font_weight.cpp`.

---

### CORE-518 – `IRenderer.h`: schwerer Header und 12 Einzel-Setter, drei handgepflegte Abbildungen der Umgebungseinstellungen
- **ID**: CORE-518
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/Renderer/IRenderer.h:1-14, 376-613` ; `EnvironmentSettings.h:16-26`
- **Problem**: (a) Der Header zieht `glm/gtc/matrix_transform.hpp` (nur für `worldPreviewProjection`), `UIRenderObject.h` (224-Byte-Struktur nur für `RenderWidgetThumbnail`), `DebugDraw`, `<functional>`, und wird von `GpuPassAccumulator.h`, `RendererFactory.h` u. a. transitiv eingebunden (Kompilierzeit). (b) Zwölf `Set…Settings`-Virtuals (Bloom, AA, SSAO, Shadow, GI, SSR, Occlusion, DoF, MotionBlur, GIRefl, …) mit jeweils leerem Default; die Kommentare nennen sie „pushed every frame“. Das sind ca. 12 virtuelle Aufrufe pro Frame (vernachlässigbar), aber jedes Backend muss jedes Setter-Paar einzeln auf Änderung vergleichen. (c) `EnvironmentSettings` liegt im globalen Namespace (Konvention: `namespace HE`) und muss laut Kommentar an drei Stellen von Hand synchron gehalten werden (`EnvironmentPush.cpp`, `SkyFrameParams.cpp`, OpenGL-Sky-Block); ein neues Feld, das in einer fehlt, wirkt auf einem Backend stillschweigend nicht.
- **Code-Auszug**:
```cpp
// KEEP IN SYNC — a new field needs BOTH of these, and on OpenGL a third:
```
- **Lösung**: (a) `worldPreviewProjection`/`worldPreviewVerticalFov` in einen eigenen Header (`Renderer/WorldPreview.h`) auslagern, `UIRenderObject.h` per Vorwärtsdeklaration (`struct UIRenderObject;`) einbinden – Signaturen mit `const std::vector<UIRenderObject>&` benötigen nur die Deklaration. (b) Ein `struct RenderSettings { BloomSettings bloom; AntiAliasingSettings aa; ... }` plus ein einziges `virtual void SetRenderSettings(const RenderSettings&)` mit `operator==` zur Änderungserkennung; die ABI-Reihenfolge der Vtable darf nicht verändert werden (Kommentar im Header beachten), daher nur anfügen. (c) Test, der per Reflection/`sizeof`-Check meldet, wenn `EnvironmentSettings` wächst, ohne dass die Abbildungstabelle aktualisiert wurde.
- **Erwarteter Effekt**: Kürzere Kompilierzeit (schwer zu schätzen, Messung mit `-ftime-trace`), weniger Wartungsfehler.
- **Risiko / Verifikation**: Mittel für (b) wegen Vtable-ABI und sechs Backends – nur umsetzen, wenn ohnehin Backend-Umbau ansteht. (a) risikoarm: Build aller Ziele.

---

### CORE-519 – `GpuPassAccumulator`: drei Kopien pro abgeschlossenem Frame, `std::map`, GC-Scan pro Report
- **ID**: CORE-519
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Renderer/GpuPassAccumulator.h:75-125` (Zeilen 93-110)
- **Problem**: Beim Abschluss eines Frames wird der Pass-Vektor dreimal kopiert (`fs.passes = a.passes`, `m_last = fs`, `outDone = fs`), die in-flight-Frames liegen in einem `std::map`, und jeder `report()` scannt die komplette Map nach alten Frames. Das läuft nur im Profiler-Detail-Capture, im Completion-Handler von Metal-Command-Buffern (Hintergrund-Thread, unter Mutex). Der Aufwand ist klein (≈ 20–40 Pässe), der Mutex wird aber vom Hauptthread in `latest()` geteilt. Zudem bindet der Header `IRenderer.h` nur wegen zweier kleiner Structs ein (siehe CORE-518).
- **Code-Auszug**:
```cpp
fs.passes = a.passes;
...
m_last = fs;
...
outDone = fs;
```
- **Lösung**: `fs.passes = std::move(a.passes)`, danach `m_last = fs;` (eine Kopie) und `outDone = std::move(fs)`; `m_inflight` als kleiner `std::vector<std::pair<uint64_t,Accum>>`; GC nur, wenn `frame > m_newestSeen`. `GpuPassTime`/`FrameGpuStats` in einen kleinen Header `Renderer/GpuStats.h` verschieben.
- **Erwarteter Effekt**: Vernachlässigbar außerhalb des Profilers; nur Aufräumen.
- **Risiko / Verifikation**: Gering; es gibt laut Header einen direkten Unit-Test des Akkumulators (Reihenfolge, Straggler).

---

### CORE-520 – Rich-Text-Parser und Glyphen-Emission: unnötige Kopien, doppelte `stof`, doppelter Emit-Code
- **ID**: CORE-520
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:602-606, 647, 711` (Parser), `:958-967` vs. `:1181-1190` (Glyphen-Quads), Umbruch in drei Varianten (`:459-543`, `:813-843`, `:1022-1092`)
- **Problem**: (a) `readTag` prüft `size=` mit `std::stof` in einem `try/catch`, danach parst `uiParseRichText` denselben Wert noch einmal (`std::stof(value)`). Ungültige Eingaben („size=abc“) kosten eine Exception; die `std::stof`-Konvertierung ist außerdem locale-abhängig (Komma-Dezimaltrenner nach `setlocale`), `std::from_chars` wäre locale-frei und werfend-frei. (b) `attrs()` liefert `RichScope` per Wert (zwei `std::string`-Kopien) und wird in `flush` und an mehreren Stellen aufgerufen. (c) Der Block, der aus `BakedGlyph` ein `UIRenderObject` baut, steht in `uiEmitRichText` und `emitUITextGlyphs` je einmal vollständig; änderte jemand ein Quad-Feld, müsste er beide anfassen. (d) Es gibt drei Greedy-Umbruch-Implementierungen (Label-Strings, Byte-Ranges für Eingabefelder, Rich-Layout) mit absichtlich unterschiedlichen Randregeln (dokumentiert); jede neue Regel (z. B. Tab-Behandlung: nur `uiTextWrapRanges` kennt `\t`) muss dreifach gepflegt werden.
- **Code-Auszug**:
```cpp
try { const float v = std::stof(outValue); return v > 0.0f && v < 100.0f; }
catch (...) { return false; }
...
else if (name == "size") a.sizeScale = std::stof(value);
```
- **Lösung**: `readTag` gibt den geparsten `float` mit heraus (`outSize`), `std::from_chars` statt `stof`; `attrs()` als `const RichScope&` mit statischer Leer-Instanz; ein `static UIRenderObject makeGlyphQuad(const BakedGlyph&, float scale, ...)` für beide Emitter. Die drei Umbruchregeln nur zusammenführen, wenn die Unterschiede (Trimmen, `\r`, Tab) bewusst vereinheitlicht werden – sonst nur dokumentieren.
- **Erwarteter Effekt**: Weniger Code, kein Exception-Pfad; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: `from_chars` für `float` ist bei libc++ erst ab neueren Versionen vorhanden (macOS-Toolchain prüfen), sonst `strtof` mit C-Locale. Parser-Regeln laut Header sind „pinned by tests“ → diese Tests müssen unverändert bestehen.

---

### CORE-521 – `Proc::run` / `augmentToolPath`: kleine Reste (POSIX-`cwd`, Zeilensplitter, Wartepolling, `setenv`)
- **ID**: CORE-521
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Platform/Process.cpp:33-41` (Splitter), `:107` (`setenv`), `:489-494` (`cwd`), `:627-634` (Kill-Wartezeit)
- **Problem**: (a) Ist `posix_spawn_file_actions_addchdir_np` nicht verfügbar (Bedingung `__APPLE__ || _POSIX_C_SOURCE >= 200809L`), wird `Options::cwd` stillschweigend ignoriert und der Prozess läuft im falschen Verzeichnis. Die Bedingung prüft ein POSIX-Makro, aber `_np` ist eine glibc/macOS-Erweiterung (glibc ≥ 2.29). (b) `LineSplitter::feed` hängt jedes Zeichen einzeln per `push_back` an; `memchr` auf `'\n'` und Blockanhang wäre kürzer und schneller (relevant bei Megabyte-Ausgaben wie `git status`/`cmake`). (c) Nach `SIGTERM` pollt die Schleife mit `usleep(10000)` bis zu 2 s. (d) `augmentToolPath` ruft `setenv` in einem Thread, während andere Threads `getenv` ausführen können (nicht thread-sicher); einmalig und früh, daher nur ein Risiko, wenn der erste Aufruf aus einem Worker kommt.
- **Code-Auszug**:
```cpp
#if defined(__APPLE__) || (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L)
    ::posix_spawn_file_actions_addchdir_np(&actions, o.cwd.c_str());
#endif
```
- **Lösung**: Mit `#else` bei nicht vorhandener Funktion `r.launchFailed = true; return r;` (oder `chdir`-Wrapper über `/usr/bin/env -C`), statt still weiterzulaufen; `feed` mit `memchr`; `augmentToolPath()` in `Application`-Start (Main-Thread) vorab aufrufen.
- **Erwarteter Effekt**: Klare Fehler statt falschem Verzeichnis; schnelleres Splitten großer Ausgaben.
- **Risiko / Verifikation**: Gering. `tests/test_process_runner.cpp`: Fall mit `cwd` (Ausgabe von `pwd`) und mehrzeilige Ausgabe mit `\r\n`, Zeile ohne Abschluss.

---

### CORE-522 – Lua-Fehlerbehandlung: `lua_pcall` ohne Message-Handler (kein Traceback), `atoi` ohne Bereichsprüfung
- **ID**: CORE-522
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Scripting/ScriptEngine.cpp:454-464` (`pcall`), `:70-72` (`parseScriptErrorLocation`)
- **Problem**: `lua_pcall(m_L, nargs, nresults, 0)` liefert nur die nackte Fehlermeldung `name:7: message`; der Aufrufstapel geht verloren, was bei verschachtelten Skriptfunktionen die Fehlersuche erschwert (Konsole kann nur die innerste Stelle öffnen). `std::atoi` in `parseScriptErrorLocation` hat bei Zeilennummern über `INT_MAX` undefiniertes Verhalten (Eingabe stammt aus einer Fehlermeldung, also nicht vertrauenswürdig, z. B. aus Skripttext `error("x:99999999999:")`).
- **Code-Auszug**:
```cpp
if (lua_pcall(m_L, nargs, nresults, 0) != LUA_OK)
...
out.line = std::atoi(message.c_str() + colon + 1);
```
- **Lösung**: Einen C-Message-Handler (`luaL_traceback(L, L, msg, 1)`) einmal in der Registry halten, per `lua_pushcfunction`/`lua_insert` unter die Funktion legen und als `msgh` übergeben, die Fehlerposition weiterhin aus der ersten Zeile lesen (`parseScriptErrorLocation` findet das erste `<name>:<zahl>:`, bleibt also stabil). `atoi` durch `std::from_chars` mit Bereichsprüfung ersetzen.
- **Erwarteter Effekt**: Bessere Fehlerdiagnose; geringe Mehrkosten nur im Fehlerfall (Handler läuft nur bei Fehler). Der Handler-Slot auf dem Stack kostet je Aufruf zwei Stack-Operationen im Hot Path `callOnUpdate` – vor Einführung messen oder den Handler nur im Editor-/Debug-Build aktivieren.
- **Risiko / Verifikation**: `lastError()`-Text ändert sich (länger, mehrzeilig); Tests, die `lastError()` exakt vergleichen, und die Konsole (`parseScriptErrorLocation`-Tests) anpassen. `ScriptContext` loggt die Meldung 1:1.

---

## Gelesen

Ja, alle 33 Dateien des Slice 5 vollständig gelesen (MaterialGraph.cpp/.h, WeatherMaterialFunctions.cpp/.h, AABB.h, Math.h, ParticleGraph.cpp/.h, CollisionLayers.cpp/.h, WaterWaves.h, DynLib.cpp/.h, PathSafety.h, Process.cpp/.h, ProjectWriteGate.h, ProjectSettings.cpp/.h, EnvironmentSettings.h, GpuPassAccumulator.h, IRenderer.cpp/.h, RendererFactory.h, UIFont.cpp/.h, UIRenderObject.h, WaterRipples.h, IScriptBackend.h, PythonPluginAbi.h, ScriptEngine.cpp/.h, ScriptTypes.h); keine Datei ausgelassen. `isPathWithin` (CORE-508) und `sizeof(UIRenderObject)` (CORE-506) wurden zusätzlich mit g++ nachgemessen; alle übrigen Angaben stammen aus dem Quelltext.

## Core Slice 6 – Sequence/SequenceJson, SlotMap, Types/*, UIWidget/{AppMenu, UIElement, UIElements, UIShortcut, UITheme, UIWidgetAnim, UIWidgetBinding}

## Zusammenfassung

Der Slice (20 Dateien, ~11 000 Zeilen) ist insgesamt sauber und sehr gut kommentiert; die Property-Tabellen (`uiprop::slot`), die „eine Rechnung für Zeichnen und Hit-Test"-Helfer und die kleinen Parser (Shortcut, Ease, Binding) sind einfach und korrekt. Die relevanten Probleme liegen an drei Stellen: (1) Frame-Loop-Kosten im UI-Baum (`hidesChild` ist O(N) und wird über die Vorfahrenkette rekursiv gefragt → quadratisch; `UIText::applyAutoSize` misst jedes Label jeden Frame neu), (2) die doppelt/dreifach geführte Liste der Basis-Properties samt String-If-Kette auf dem Animationspfad, (3) Robustheit und Kopien in den JSON-Pfaden (TypeRegistry, Theme, Sequence, UIElement::readJson). Zusätzlich ein echter Logikfehler (Zyklusschutz in `fieldDefault` wird für Struct-Arrays umgangen). Funde: 2 Hoch, 5 Mittel, 11 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten und als solche gekennzeichnet.

---

### CORE-601 – `hidesChild` ist O(N) und wird je Vorfahr rekursiv gefragt (quadratisch pro Frame)
- **ID**: CORE-601
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIElement.cpp:3022-3031` (TabBox), `:3148-3154` (Splitter), `:3296-3303` (Accordion); Helfer `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1501-1518`; Aufrufer `UIWidgetTree.cpp:1522-1531` (`uiElementEffectiveVisible`)
- **Problem**: `uiElementEffectiveVisible` ruft für jeden Vorfahren `p->hidesChild(tree, e)` auf und rekurriert dann zum nächsten Vorfahren. Bei TabBox/Splitter/Accordion löst `hidesChild` über `uiChildIndexOf` (lineare Suche über *alle* `tree.elements`) bzw. zusätzlich `uiChildCountOf` (nochmal alles) den Kindindex auf. Für jedes Element unterhalb eines solchen Containers kostet „ist sichtbar?" also O(Tiefe_unter_Container × N); über alle Elemente O(N²). `uiElementEffectiveVisible` hat 16 Aufrufstellen (Extract, Hit-Test, Designer). Typische App-Shells (Splitter + TabBox an der Wurzel) liegen in genau dieser Situation.
- **Code-Auszug**:
```cpp
bool UITabBox::hidesChild(const UIWidgetTree& tree, const UIElement& child) const
{
    const int idx = uiChildIndexOf(tree, id, child.id);          // O(N)
    if (idx < 0) return false;
    const int active = activeTab >= 0 && activeTab < uiChildCountOf(tree, id) ? activeTab : 0; // O(N)
    return idx != active;
}
// UIWidgetTree.cpp
int uiChildIndexOf(const UIWidgetTree& tree, int parentId, int childId)
{ int i = 0; for (const auto& ep : tree.elements) { if (!ep || ep->parentId != parentId) continue; if (ep->id == childId) return i; ++i; } return -1; }
```
- **Lösung**: Kindindex einmal pro Frame/Änderung berechnen statt pro Abfrage. Einfachste Variante: `UIWidgetTree` hält einen lazy aufgebauten Cache `std::unordered_map<int,int> childIndex` (childId → Index unter dem Elternteil) und `std::unordered_map<int,int> childCount`, invalidiert über einen `structureVersion`-Zähler, der in jedem add/remove/reparent/reorder erhöht wird (die Tree-Mutatoren sind bereits zentral). `uiChildIndexOf/CountOf` lesen dann aus dem Cache; `hidesChild`-Signatur bleibt unverändert. Alternative ohne Cache: `uiElementEffectiveVisible` iterativ aufwärts laufen lassen und den Index des Kindes beim Wandern berechnen, dies bleibt aber O(N) pro Schritt.
- **Erwarteter Effekt** (Schätzung): O(N²) → O(N·Tiefe) für Bäume mit Tab/Splitter/Accordion-Vorfahren; bei ~1000 Elementen unter einem TabBox grob 10⁶ Elementvergleiche pro Abfragedurchgang entfallen. Unsicher/messen, ob es bei realen Widgets sichtbar ist.
- **Risiko / Verifikation**: Cache muss bei jeder strukturellen Änderung (auch Graft von Embeds, NamedSlot-Umhängen, Designer-Drag) invalidiert werden, sonst „Klick auf unsichtbaren Button" (der im Header ausdrücklich verhinderte Fehler). Absichern mit `tests/test_ui_widgets.cpp` (TabBox/Splitter/Accordion-Hit-Test-Fälle) plus einem neuen Test, der nach Reparent/Reorder die Sichtbarkeit prüft; vorher Benchmark mit 2000-Elemente-Baum.

### CORE-602 – `UIText::applyAutoSize` misst jedes Label jeden Frame neu (Default `autoSize = true`)
- **ID**: CORE-602
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIElement.cpp:1428-1466`; Default `src/HE_Core/include/UIWidget/UIElements.h:118`; Aufrufer `WidgetManager.cpp:6157` → `UIWidgetTree.cpp:1239-1300` (läuft pro Frame über *alle* Elemente)
- **Problem**: Jedes neu angelegte Text-Element hat `autoSize = true`; `uiApplyAutoSize` ruft pro Frame für jedes Element `applyAutoSize`, bei Text also `HE::measureUIText` (bzw. `uiLayoutRichText` mit Markup-Parse-Cache-Prüfung `m_parsedFrom != text`, `UIElement.cpp:1393`) – obwohl sich Text, Schriftgröße, Wrap-Breite und Font fast nie ändern. Dazu kommen pro Frame `uiElementUnitScale` + `uiElementRect` je Element. Ergebnis wird danach nur in `sizeX/sizeY` geschrieben.
- **Code-Auszug**:
```cpp
void UIText::applyAutoSize(float resolvedWidth, float fontScale)
{
    if (!autoSize) return;
    ...
    else
        m = f ? HE::measureUIText(*f, text, fs, wrapW, opts)
              : HE::measureUIText(text, fs, wrapW, opts);   // jeden Frame
    if (!wordWrap && !stretchX) sizeX = m.x + fs * 0.25f;
    if (!stretchY)              sizeY = m.y + fs * 0.35f;
}
```
- **Lösung**: Gemessenes `glm::vec2 m` zusammen mit seinem Schlüssel in `mutable`-Membern der UIText ablegen: `{text-Hash oder Kopie, fs, wrapW, alignH/alignV, wordWrap, richText, fontAtlasKey, BakedUIFont*-Zeiger/ok-Flag}`. Bei Treffer nur `m` wiederverwenden und die `sizeX/sizeY`-Zuweisung wie bisher ausführen (die Zuweisung muss bleiben, weil Skripte/Stretch `sizeX` dazwischen ändern können). Font-Zeiger im Schlüssel, damit ein später fertig gebackener Font neu misst. Gleiche Technik für CheckBox/RadioButton/ComboBox (`:1739`, `:2366`, `:3963`), dort nur bei aktivem `autoSize`.
- **Erwarteter Effekt** (Schätzung): Pro statischem Label entfällt die komplette Glyphen-Messung pro Frame; bei UI-Szenen mit hunderten Labels vermutlich spürbar (Größenordnung: ein Teil der UI-Layout-Kosten). Unsicher/messen mit Profiler über `uiApplyAutoSize`.
- **Risiko / Verifikation**: Schlüssel muss *alle* Eingaben der Messung enthalten (Font, Scale, Wrap), sonst veraltete Größen. Tests: `tests/test_ui_widgets.cpp` (Auto-Size-Fälle, fontScale 1.5, Wrap-Text, Rich-Text); neuer Test „Text ändern → Größe folgt im selben Frame".

### CORE-603 – Basis-Properties sind dreifach/vierfach geführt (String-If-Ketten) und liegen auf dem Animationspfad
- **ID**: CORE-603
- **Kategorie**: A (und D)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `UIElement.cpp:878-947` (`getBaseProp`), `:951-1013` (`uiBaseProperties`), `:1017-1098` (`setBaseProp`), `:1101-1181` (`allProperties`); Hot-Path-Aufrufer `WidgetManager.cpp:2382-2392` (`applyClipAt`, pro Tick und laufendem Clip, `:2565`)
- **Problem**: Die ~55 Basis-Properties stehen als 4 unabhängige Listen (Getter-If-Kette, Setter-If-Kette, Deskriptorliste, nochmal Deskriptorliste in `allProperties`). Die Listen driften bereits: `allProperties()` enthält `Accepts Drop`, `Draggable`, `Drag Payload` nicht (fehlen in `:1104-1136`), obwohl `getBaseProp/setBaseProp/uiBaseProperties` sie führen; `WidgetManager.cpp:2321` benutzt `allProperties()` als „kennt die Property?"-Test für `rememberOriginal`. Laufzeit: `getPropAny` + `setPropAny` laufen pro Sample und Frame (`applyClipAt`), jeweils bis zu ~55 `std::string == const char*`-Vergleiche (jeder mit strlen) bevor die typeigene Tabelle (ebenfalls linear) drankommt; Rückgabe jeweils als fette `UIPropValue` (String + Vector).
- **Code-Auszug**:
```cpp
bool getBaseProp(const UIElement& e, const std::string& n, UIPropValue& out)
{
    if (n == "Visible")      { out = UIPropValue::ofBool(e.visible);     return true; }
    if (n == "Hit Testable") { out = UIPropValue::ofBool(e.hitTestable); return true; }
    ... // ~55 weitere
```
- **Lösung**: Basis-Properties als `static const UIPropTable kBaseTable` mit denselben `uiprop::slot<&UIElement::visible>(...)`/`uiprop::custom(...)`-Zeilen deklarieren (wie die Typtabellen; Clamping in den `custom`-Settern beibehalten). Daraus `uiBaseProperties()`, `getBaseProp`, `setBaseProp` ableiten (eine Quelle). `allProperties()` = Typtabelle + gefilterte Basistabelle (Filter `hasSurfaceStyle()/hasMaterialSlot()/...` über ein Flag-Feld je Zeile statt Hand-Liste). Für den Hot Path zusätzlich ein Lookup `unordered_map<string_view, const UIPropSlot*>` je Tabelle (einmalig `static`), oder – billiger – `UIAnimTrack` löst den Slot einmal beim Start auf (`const UIPropSlot*`) und `applyClipAt` ruft `slot->set` direkt.
- **Erwarteter Effekt** (Schätzung): ~170 Zeilen weniger, eine Quelle der Wahrheit; pro animiertem Sample entfallen ~100 String-Vergleiche + 2 temporäre `UIPropValue`. Bei wenigen Clips nicht spürbar, bei Listen-/Menü-Animationen mit vielen Tracks messbar (unsicher).
- **Risiko / Verifikation**: Die Namen sind On-Disk-Format; `allProperties()` um die drei fehlenden Properties zu erweitern ändert die MCP-/Editor-Ausgabe (bewusste Verhaltensänderung, ggf. getrennt machen). Reihenfolge muss erhalten bleiben (Editor-Anzeige). Absicherung: bestehender Test, der `uiBaseProperties` gegen `getBaseProp` pinnt (`tests/test_ui_widgets.cpp`), plus Round-Trip-Test je Basis-Property.

### CORE-604 – Zyklusschutz in `fieldDefault` wird für Struct-Arrays umgangen (unbegrenzte Rekursion)
- **ID**: CORE-604
- **Kategorie**: A (Korrektheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Types/TypeRegistry.cpp:328-331` (Array-Zweig), Eingang über `:670-672` (`structFromJson`)
- **Problem**: Die Kopfkommentare (`TypeRegistry.h:137-141`) versprechen, dass jeder rekursive Konsument einen Visited-Guard trägt. Im Array-Zweig ruft `fieldDefault` für Struct-Elemente `reg.makeDefaultValue(f.typeName)` auf, das ein *neues* `visiting`-Set anlegt – der Guard des laufenden Aufrufs geht verloren. `structFromJson` legt für ein `isArray`-Feld Slots aus `"default"` an, auch für `PinType::Struct` (`defaultFromJson` ignoriert den Wert, der Slot entsteht trotzdem). Eine handeditierte Definition `A { arr: Array<A>, "default": [0] }` rekursiert damit `structDefault(A) → fieldDefault → makeDefaultValue(A) → …` bis zum Stack-Overflow (Editor/Spiel-Absturz beim ersten `makeDefaultValue`). `structToJson` schreibt solche Slots nie (`:596`), der Fall entsteht nur durch Handarbeit/MCP.
- **Code-Auszug**:
```cpp
else if (f.type == PinType::Struct)
{
    it = reg.makeDefaultValue(f.typeName);   // elements use their own defaults   <- neues visiting-Set
}
```
- **Lösung**: `visiting` durchreichen: `it = structDefault(reg, f.typeName, visiting);` (dafür steht `structDefault` schon vor `fieldDefault`). Zusätzlich in `structFromJson` Struct-Slots nicht anlegen (nur zählen, damit `items.size()` konsistent bleibt oder ganz verwerfen, passend zu „Struct elements carry no slot payload").
- **Erwarteter Effekt**: Kein Absturz mehr bei zyklischen Handdaten; Verhalten für nicht-zyklische Definitionen identisch (jedes Element bekommt weiter seinen eigenen Default).
- **Risiko / Verifikation**: Test in der TypeRegistry-Testdatei (siehe `tests/CMakeLists.txt`): Selbstreferenz mit Array-Default, direkter Zyklus A→B→A; erwartet Terminierung und leeres/abgeschnittenes Struct.

### CORE-605 – Inkonsistente Fehlertoleranz der JSON-Lader (Wurf aus `value()`/`get<float>()` in handeditierten Dateien)
- **ID**: CORE-605
- **Kategorie**: A/F (Robustheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `UITheme.cpp:30-35, 443, 474, 478-479`; `TypeRegistry.cpp:633-643, 457-482`; `SequenceJson.cpp:175-176, 221-231, 239-243, 297-304`; `UIElement.cpp:270-307, 2429-2435`
- **Problem**: Die Loader behaupten „fehlerhafte Einträge werden verworfen, nicht der ganze Asset", verhalten sich aber verschieden: `sequenceFromJson` fängt `json::exception` und lehnt dann das *ganze* Dokument ab (ein String in `"start"` macht die komplette Sequence unlesbar, `dropped` bleibt ungesetzt). `uiThemeFromJson` hat gar kein try/catch: `readColor` ruft `j[0].get<float>()` ohne Zahlenprüfung (`:33`), `j.value("name", …)` (`:443`) und `s->value("blur", …)` (`:474`) werfen bei falschem Typ. `TypeRegistry::structFromJson/enumFromJson` parsen mit `allow_exceptions=false`, rufen danach aber `e.value("type", 1)` (`:634`) und `j[0].get<float>()` (`:458`) ungeschützt. `uiPropValueFromJson` (`:286-288`) und `colFrom` (`:2432-2433`) casten Array-Elemente ungeprüft. Ob die Aufrufer fangen, ist je Aufrufer verschieden (z.B. `UIEditorPanel.cpp:410`, `McpToolsType.cpp:216` zeigen keinen try/catch) → unsicher/prüfen.
- **Code-Auszug**:
```cpp
bool readColor(const nlohmann::json& j, glm::vec4& out)
{
    if (!j.is_array() || j.size() != 4) return false;
    out = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };  // wirft bei "x"
    return true;
}
```
- **Lösung**: Ein gemeinsamer kleiner Helfer (z.B. in `GraphCommon/GraphJson.h`, das `sequenceFromJson`/Theme schon einbinden): `template<class T> T jget(const json& j, const char* key, T def)` = `find` + `is_number()/is_string()`-Prüfung statt `value()`, und `bool jfloat4(const json&, glm::vec4&)` mit `is_number` je Element. Dann entfällt der Sonder-try/catch in `sequenceFromJson` und das Verwerfen geschieht pro Eintrag wie dokumentiert. Alternativ pragmatisch: an jedem öffentlichen `*FromJson`-Eintritt ein einheitliches `try { … } catch (const json::exception&) { return false; }`.
- **Erwarteter Effekt**: Keine Abstürze/Totalverluste bei handeditierten Assets; weniger Duplikat-Absicherung. Kein Laufzeitgewinn.
- **Risiko / Verifikation**: Verhalten bei *gültigen* Dateien unverändert; bei kaputten ändert sich „ganzes Dokument abgelehnt" → „Eintrag verworfen" (Doku in `SequenceJson.h:31-37` verspricht Letzteres). Tests: je Loader ein Fuzz-artiger Test mit falschen Typen (`"duration":"2"`, `"colors":{"Text":{"Light":["a",1,2,3]}}`).

### CORE-606 – `TypeRegistry` liefert tiefe Kopien: `getStruct/getEnum` je Verschachtelungsebene und je Array-Element, `structs()/enums()` kopieren + sortieren pro Aufruf
- **ID**: CORE-606
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `TypeRegistry.cpp:160-178` (Kopie unter Lock), `:194-220` (Kopie + `sort`), `:292-293` (`structDefault`), `:318-324`, `:344-347` (EnumDef-Kopie pro Element), `:403-407`
- **Problem**: `getEnum` kopiert die komplette `EnumDef` (Entries + `formerNames`-Vektoren), `getStruct` die komplette `StructDef` inkl. aller `StructField`-`HorizonCode::Value`s. `structDefault` macht das für jedes Struct auf jeder Ebene, `fieldDefault` für Enum-Arrays/Map-Keys sogar *pro Element* (die Schleifen `for (Value& it : v.items)` / `for (Value& k : v.keys)` rufen `reg.getEnum(...)` im Schleifenkörper). `enums()/structs()` kopieren alles und sortieren bei jedem Aufruf; Aufrufer sind u.a. Editor-Dropdowns (`HcGraphHost.cpp:1249,1300,1694`, `HcEditorUtil.cpp:554-627`), potenziell pro Frame in offenen Panels (unsicher).
- **Code-Auszug**:
```cpp
for (Value& it : v.items)
{
    ...
    if (f.type == PinType::Enum)
    {
        EnumDef ed;                          // pro Element
        ...
        if (reg.getEnum(f.typeName, ed)) { ... }
```
- **Lösung**: (a) Kurzfristig `getEnum` aus der Schleife herausziehen (einmal vor der Schleife laden). (b) Strukturell: `Impl` speichert `std::shared_ptr<const EnumDef/StructDef>`; Getter geben `shared_ptr<const T>` zurück (Kopie = Refcount) und `enums()/structs()` können einen gecachten, bei `register*/remove/clear` invalidierten sortierten Snapshot (`shared_ptr<const vector<…>>`) liefern. Die Signatur `bool getX(path, T& out)` kann als dünner Wrapper bleiben.
- **Erwarteter Effekt** (Schätzung): Kopien pro Default-Wert-Erzeugung von O(Tiefe·Felder) auf O(1) pro Zugriff; für UI-Dropdowns pro Frame Wegfall von Kopie+Sort (Größenordnung: Projekte mit dutzenden Typen). Unsicher/messen, ob außerhalb des Editors relevant.
- **Risiko / Verifikation**: API-Änderung berührt ~30 Aufrufer (Editor, Python, Scripting, Codegen); den Kopier-Wrapper beibehalten, dann nur interne Änderung. Tests: bestehende TypeRegistry-/HorizonCode-Struct-Tests.

### CORE-607 – `TreeView`: `visibleRows()`, `parseIntList()` und Text-Vergleich bei jedem Aufruf (mehrfach pro Frame)
- **ID**: CORE-607
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `UIElements.h:2086, 2102-2123`; `UIElement.cpp:4063-4067` (`nodes()`), `:4112-4117` (`isCollapsed`), `:4133-4155` (`visibleRows`), `:4229-4230, 4260` (`render`)
- **Problem**: `nodes()` vergleicht bei *jedem* Aufruf `m_parsedFor == items` (O(Textlänge)). `visibleRows()` parst `collapsed` neu (`parseIntList`, Heap), legt `hidden` und `rows` an und läuft O(Knoten). Es wird aufgerufen von `measuredExtent()` → `maxScroll()` → `maxScrollAmount()` und `scrollBar()` (Aufrufer: `uiUpdateScrollExtents` pro Frame `UIWidgetTree.cpp:1556`, `uiScrollThumbRect` im Render und bei Drag), von `rowAt()` (jede Mausbewegung), `scrollToRow` und `render`. `render` ruft zusätzlich `parseIntList(collapsed)` und dann pro Pfeil ein lineares `std::find` (O(Zeilen·Folds)). Pro Frame also ≥3× volle Neuberechnung mit mehreren Heap-Allokationen; bei einem Baum mit tausenden Knoten (Dateibrowser, Outliner – genau der beworbene Einsatz) spürbar.
- **Code-Auszug**:
```cpp
float measuredExtent() const
{ return static_cast<float>(visibleRows().size()) * rowHeight; }   // allokiert + O(n)
int rowAt(float localY) const { ... return i < static_cast<int>(visibleRows().size()) ? i : -1; }
```
- **Lösung**: Fold-Zustand und sichtbare Zeilen mitcachen, analog zu `m_nodes`: `mutable std::string m_collapsedFor; mutable std::vector<int> m_rows; mutable std::vector<char> m_folded;` Neuaufbau nur, wenn sich `items` oder `collapsed` geändert haben (Stringvergleich genügt, oder Versionszähler in `setCollapsed`/Property-Setter, aber beachte, dass `collapsed` ein öffentliches Feld ist → Vergleich behalten). `visibleRows()` gibt `const std::vector<int>&` zurück, `isCollapsed` nutzt `m_folded[node]` (O(1)). Gleiches Muster für die Container-Render-Allokationen: `UIAccordion::render` baut pro Frame zwei Vektoren (`:3342-3348`), `contentHeight/headerAt` je eine `std::vector<Slot>` (`:3278, 3289`).
- **Erwarteter Effekt** (Schätzung): Pro Frame und TreeView von mehreren O(n)-Neuaufbauten + Allokationen auf einen Stringvergleich; größter Gewinn bei >1000 Knoten.
- **Risiko / Verifikation**: Cache-Invalidierung bei direkter Schreibweise auf `collapsed`/`items` (Skript, Property) – über den Stringvergleich robust. Tests in `tests/test_ui_widgets.cpp` (TreeView Fold/Selection/Scroll) + neuer Test: Fold ändern über `setProp("Collapsed")` und Zeilenliste aktualisiert sich.

### CORE-608 – JSON-Lesepfade kopieren Teilbäume (`j.value(key, json())`, `t.value("sections", kEmpty)`)
- **ID**: CORE-608
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIElement.cpp` (56 Stellen, z.B. `:2438`, `:2809-2811`, `:3139-3143`), Helfer `:2429-2435`; `SequenceJson.cpp:221, 235, 254, 265`, `:173`
- **Problem**: `nlohmann::json::value(key, default)` liefert den Wert *by value*. `colFrom(j.value("color", nlohmann::json()), color)` kopiert das 4-elementige Array (Heap) und konstruiert zudem ein temporäres `json()`; in `parseSequence` kopiert `t.value("sections", kEmpty)` bzw. `"cuts"/"events"` die komplette Section-Liste jedes Tracks. Läuft beim Laden jedes Widgets (viele Farben pro Element) bzw. jeder Sequence; kein Frame-Pfad, aber unnötige Allokationen auf dem Ladepfad.
- **Code-Auszug**:
```cpp
void UIPanel::readJson(const nlohmann::json& j)  { color = colFrom(j.value("color", nlohmann::json()), color); }
...
for (const auto& s : t.value("sections", kEmpty))   // Kopie der Section-Liste
```
- **Lösung**: `colFrom(const json& parent, const char* key, const glm::vec4& def)` mit `find` + `is_array()`; in `parseSequence` `const json* arr = find(t, "sections")` / `if (it != end && it->is_array())` iterieren. Zusammen mit CORE-605 umsetzen (gemeinsamer Helfer).
- **Erwarteter Effekt** (Schätzung): Weniger Kleinallokationen beim Laden; nur bei sehr großen Widgets/Sequences messbar.
- **Risiko / Verifikation**: Rein mechanisch; bestehende Lade-/Round-Trip-Tests in `tests/test_ui_widgets.cpp` genügen.

### CORE-609 – `UIElement::copyBaseTo` ist toter Code (und ein Drift-Risiko)
- **ID**: CORE-609
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/UIWidget/UIElement.h:979-1025`
- **Problem**: Der 45-Zeilen-Block kopiert von Hand ~60 Felder („used by clone()"), aber alle `clone()`-Implementierungen in `UIElements.h` nutzen den Kopierkonstruktor (`std::make_unique<UIPanel>(*this)`); `grep -rn copyBaseTo src` findet nur die Definition. Bei neuen Basisfeldern würde der Block veralten (z.B. enthält er schon nicht `shadow…` identisch zur Reihenfolge, aber jedes neue Feld fehlt) und der Kommentar lügt.
- **Code-Auszug**:
```cpp
protected:
    // Copy the shared base fields into a freshly-constructed subclass (used by clone()).
    void copyBaseTo(UIElement& dst) const { dst.id = id; dst.parentId = parentId; ... }
```
- **Lösung**: Block und Kommentar löschen.
- **Erwarteter Effekt**: −47 Zeilen, ein Drift-Risiko weniger.
- **Risiko / Verifikation**: Keine Referenz → Build genügt; zusätzlich `grep -rn copyBaseTo` über `src/` und `tests/` (Tests wurden nicht durchsucht, vorher prüfen).

### CORE-610 – Caret-/Selektions-Hit-Test ist O(n²) (Substring + Messung je Zeichen), `selectRows` misst jeden Frame
- **ID**: CORE-610
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIElement.cpp:1611-1625` (`byteAtRowX`), `:1677-1718` (`UIText::caretAtPoint`), `:2352-2360` (`UITextInput::caretAtPoint`), `:1627-1675` + `:1493` (`selectRows` je Frame bei `selectable`)
- **Problem**: Für jedes Zeichen werden zwei Teilstrings gebildet und je komplett gemessen (`widthTo(i)`, `widthTo(nx)`); Gesamt O(L²) pro Klick/Drag-Event (und pro Pfeiltaste in `byteAtRowX`). Bei einer langen Einzelzeile (z.B. 10 000 Zeichen, `wrapText` aus) ~10⁸ Glyphschritte pro Mausbewegung beim Ziehen. Außerdem misst `UIText::render` für `selectable`-Labels pro Frame jede Zeile (`selectRows`), obwohl sich Text/Breite nicht ändern. Nur Interaktions-/Opt-in-Pfade → niedrig.
- **Code-Auszug**:
```cpp
for (size_t i = lo; i < hi; )
{
    const size_t next = uiUtf8Next(text, i);
    const float a = runWidth(text.substr(lo, i - lo));
    const float b = runWidth(text.substr(lo, next - lo));
    if (localX < (a + b) * 0.5f) return i;
```
- **Lösung**: Einmal pro Zeile die kumulativen Caret-Positionen berechnen (eine Messung der Zeile liefert i.d.R. per-Glyph-Advance; falls `measureUIText` nur Summen liefert: Binärsuche über Byte-Offsets mit O(log L) Messungen) und das vorherige `a` als `b` wiederverwenden (halbiert die Messungen sofort). `selectRows` mit `(text, sizePx, px.w, alignH)`-Schlüssel cachen.
- **Erwarteter Effekt** (Schätzung): O(L²) → O(L log L) pro Klick; Frame-Messung von Selectable-Labels entfällt.
- **Risiko / Verifikation**: Mittelpunktsregel („linke Hälfte → davor") und UTF-8-Grenzen müssen identisch bleiben; Tests `tests/test_ui_widgets.cpp` (Caret-Positionierung, fontScale 1.5, Multiline-Wrap).

### CORE-611 – Menü-Shortcuts werden bei jedem Tastendruck für jede Zeile neu geparst
- **ID**: CORE-611
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIShortcut.cpp:141-165` (`uiShortcutMatches`, `uiShortcutMatchesText`), Aufrufer `WidgetManager.cpp:1323`; `include/UIWidget/AppMenu.h:42` (`std::string shortcut`)
- **Problem**: `menuItemForShortcut` ruft pro Tastendruck für jeden Menüeintrag `uiShortcutMatchesText` auf: `squeeze` + `lower` + Token-Zerlegung + `canonicalKey` (mehrere `std::string`-Allokationen), dann `squeeze(lower(...))` auf beiden Seiten des Matches. Nur auf Tastendruck, aber O(Einträge) Parses; die Chord-Struktur `UIShortcut` existiert bereits.
- **Code-Auszug**:
```cpp
bool uiShortcutMatchesText(const std::string& text, const std::string& keyName, bool ctrl, bool shift, bool alt)
{
    UIShortcut s;
    if (!uiParseShortcut(text, s)) return false;      // bei jedem Tastendruck
    return uiShortcutMatches(s, keyName, ctrl, shift, alt);
}
```
- **Lösung**: `AppMenuItem` bekommt ein `mutable UIShortcut parsed; mutable bool parsedValid`, gefüllt beim Aufbau/`setMenuBar` (der Text bleibt führend). `uiShortcutMatches` vergleicht die kanonische `key` mit dem normalisierten `keyName` ohne Zwischenstrings (`keyName` einmal pro Tastendruck normalisieren, nicht pro Eintrag).
- **Erwarteter Effekt**: Pro Tastendruck O(Einträge) Allokationen → O(Einträge) Vergleiche; absolut klein (Schätzung).
- **Risiko / Verifikation**: Nur Cache-Invalidierung bei Änderung von `shortcut`; Tests `tests/test_ui_widgets.cpp` / Shortcut-Tests (Ctrl+S vs. Ctrl+Shift+S, „Page Up" vs. „PageUp", keypad Enter).

### CORE-612 – `SlotMap`: `clear()` verliert alle Slot-Indizes, kein Invalid-Handle, doppelte Generationserhöhung
- **ID**: CORE-612
- **Kategorie**: C/F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/include/SlotMap.h:14-32, 6-9, 80-85`
- **Problem**: (1) `clear()` leert `m_freeList` und erhöht nur die Generationen; `m_slots` bleibt behalten, aber keiner der Slots ist je wieder in der Freiliste, `insert` hängt also immer neue Slots an → bei wiederholtem `clear()`/Neuaufbau (Projektwechsel, Asset-Reload) wächst `m_slots` unbegrenzt (8 Byte/Slot). (2) `remove` erhöht `generation`, `insert` erhöht sie nochmal → pro Zyklus +2 (harmlos, aber Wrap-Risiko doppelt so früh; Generationsüberlauf wird nirgends behandelt). (3) `SlotHandle{}` hat keine Default-Initialisierer – ein unbelegtes Handle ist unbestimmt, es gibt (anders als `RenderHandle::invalid()`) keinen Sentinel (Generation 0 wird nie vergeben, wäre aber möglich). (4) Globaler Namespace, widerspricht `docs/coding-conventions.md` (Legacy).
- **Code-Auszug**:
```cpp
void clear() {
    m_data.clear(); m_erase.clear(); m_freeList.clear();
    for (auto& s : m_slots) s.generation++;     // Slots nie mehr in der Freiliste
}
```
- **Lösung**: `clear()`: `m_freeList` mit allen Slot-Indizes (absteigend) füllen; in `insert` die zweite `generation++` entfernen (Generation wird beim `remove` bereits erhöht; Initialwert 1 bleibt gültig). `SlotHandle` Default `index = 0, generation = 0` (nie gültig).
- **Erwarteter Effekt**: Kein Speicherwachstum bei Reload-Zyklen; sonst neutral.
- **Risiko / Verifikation**: Handles aus der Zeit vor `clear()` müssen weiter ungültig sein (Generationen steigen weiter) – Test: insert, clear, insert → altes Handle `get()==nullptr`. `ContentManager` ist der einzige große Nutzer (`ContentManager.h:789-792`).

### CORE-613 – `UUID.h` zieht `<random>`/`<functional>` in 89 Dateien und trägt einen 2,5 KB-Zustand pro Thread
- **ID**: CORE-613
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/include/Types/UUID.h:2-24`
- **Problem**: `HE::UUID` (reiner POD mit `hi/lo`) ist einer der meistinkludierten Header (89 Dateien in `src/` laut `grep`), enthält aber `static UUID generate()` inline mit `std::mt19937_64` (`thread_local`, ~2,5 KB je Thread, seed per `random_device`). `<random>` ist ein schwerer Header. Der Hash `h1 ^ (h2 << 1)` ist für Zufalls-UUIDs ok, für deterministisch erzeugte/abgeleitete UUIDs mit gleichem `hi` (falls es welche gibt – unsicher) schwach.
- **Code-Auszug**:
```cpp
static UUID generate()
{
    static thread_local std::mt19937_64 rng{ std::random_device{}() };   // 64-Bit-Seed für 312×64-Bit-State
```
- **Lösung**: `generate()` nur deklarieren, in `UUID.cpp` (`HE_API`) mit denselben Bits implementieren (oder xoshiro/`splitmix` – dann ändert sich die Verteilung nicht beobachtbar); `<random>` raus. Hash auf `hi ^ (lo * 0x9E3779B97F4A7C15)` o.ä. – nur falls Kollisionen gemessen werden.
- **Erwarteter Effekt** (Schätzung): Kürzere Compile-Zeit in ~89 Übersetzungseinheiten; Laufzeit unverändert.
- **Risiko / Verifikation**: Linkabhängigkeit: UUID wird teils in Tools/Tests ohne HorizonCore verwendet? (unsicher) → vorher `grep` der Nutzer. Messen: Build-Zeit vorher/nachher.

### CORE-614 – `uiHcValueToProp`: verschachtelte Ternaries und eine dritte Kopie der Coercion-Regel
- **ID**: CORE-614
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetBinding.cpp:42-76` (Zeilen 60-65 die Ternaries)
- **Problem**: Die Float/Int/Bool-Zweige sind je 2 Zeilen dreifach verschachtelter `?:`; der Kommentar gibt selbst zu, dass es die „dritte Implementierung" der HorizonCode-Coercion-Regel ist (neben `HorizonCode.cpp` und `HorizonCodeGenSupport.h`). Schwer zu lesen, und ein neuer `PinType` (z.B. `Vec3`→Float) müsste an drei Stellen nachgezogen werden.
- **Code-Auszug**:
```cpp
case UIPropType::Float:  out.f = v.isArray ? v.f : (v.type == P::Int ? (float)v.i : (v.type == P::Bool ? (v.b ? 1.0f : 0.0f)
                                                 : (v.type == P::Double ? (float)v.d : v.f))); break;
```
- **Lösung**: Lesbar, ohne Verhaltensänderung: kleiner Helfer `double asNumber(const Value&)` (switch über `v.type`, `isArray → 0`-Sonderfall getrennt behandeln) bzw. pro Zielwert eine kurze `switch`-Funktion:
```cpp
static float toFloat(const Value& v) {
    if (v.isArray) return v.f;
    switch (v.type) { case P::Int: return (float)v.i; case P::Bool: return v.b ? 1.f : 0.f;
                      case P::Double: return (float)v.d; default: return v.f; }
}
```
  Nebenbei prüfen, ob die geteilte Regel aus `HorizonCode` (Header-only) direkt nutzbar ist.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitänderung.
- **Risiko / Verifikation**: Reihenfolge der Bool/Int/Double-Fälle exakt erhalten; Test: bestehender Binding-Test in `tests/test_ui_widgets.cpp` (Array-Passthrough, Int→Float) plus Tabelle aller Kombinationen.

### CORE-615 – `uiAnimEvaluate` / `UIAnimSample`: Heap-Kopien pro Track und Frame
- **ID**: CORE-615
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIWidgetAnim.cpp:200-235`; `UIWidgetAnim.h:121-126`; Aufrufer `WidgetManager.cpp:2382-2392`
- **Problem**: Je Track und Frame entsteht ein `UIAnimSample` mit kopiertem `std::string prop` (Property-Namen wie `"Inner Shadow Color"`, `"Hovered Color"` überschreiten SSO → Heap) und einer kopierten `UIPropValue` (String + `vector<string>` Member). `out` wird nicht `reserve`d. Zusammen mit CORE-603 (Namensauflösung pro Frame) wird jedes Sample zweimal per Namen aufgelöst.
- **Code-Auszug**:
```cpp
UIAnimSample s;
s.element = tr.element;
s.prop    = tr.prop;                 // String-Kopie je Track und Frame
...
out.push_back(std::move(s));
```
- **Lösung**: `UIAnimSample` speichert statt Kopie `const UIAnimTrack* track` (oder Trackindex) und `UIPropValue value`; `out.reserve(out.size() + clip.tracks.size())`. Konsumenten (`applyClipAt`, Designer-Overlay) lesen `track->prop`.
- **Erwarteter Effekt** (Schätzung): Pro Frame einige Heap-Allokationen weniger pro laufendem Clip; gering.
- **Risiko / Verifikation**: Lebensdauer des Zeigers (Clip darf während Evaluate nicht verändert werden – im Designer Scrub-Pfad prüfen). Tests: `tests/test_ui_widgets.cpp` (Evaluate-Randfälle: vor erster Key, zwei Keys gleiche Zeit, Typmix).

### CORE-616 – Theme: Amber-Palette mit duplizierten Literalen, lineare String-Suche in Styles, Substring-Allokationen in `tagsFor`
- **ID**: CORE-616
- **Kategorie**: A/F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `UITheme.cpp:210-248` (Rollen und danach dieselben 12 Farbpaare nochmal als Locals), `UITheme.h:154-170, 231-264`; `UITheme.cpp:129-142`
- **Problem**: In `uiAmberTheme()` stehen die Rollenfarben (`:210-221`) und direkt danach dieselben Werte als `surfaceL/D … accentL/D` (`:243-248`) – derselbe Zahlenwert zweimal, eine Änderung der Palette muss an zwei Stellen erfolgen. `UIThemeStyle::find/set` und `UITheme::styleFor/styleMut` suchen linear mit `std::string ==` (geordneter Vektor aus Absicht, `UITheme.h:149-152`); `tagsFor` zerlegt jeden Key mit `uiThemeSelectorTag`/`Type` in zwei neue `std::string`s (`:258-262`). Auflösung läuft nur bei Theme-/Moduswechsel und Widget-Erzeugung (nicht pro Frame, unsicher bei `uiThemeStylesFor` je Element), daher niedrig.
- **Code-Auszug**:
```cpp
set(UIThemeRole::Surface,    { 1.00f, 0.99f, 0.96f, 1.0f }, { 0.14f, 0.13f, 0.11f, 1.0f });
...
const glm::vec4 surfaceL{ 1.00f, 0.99f, 0.96f, 1.0f }, surfaceD{ 0.14f, 0.13f, 0.11f, 1.0f };
```
- **Lösung**: Locals aus `d.color[role][mode]` lesen (`const glm::vec4 surfaceL = d.color[(int)UIThemeRole::Surface][0]` …) oder zuerst Locals definieren und `set(...)` damit füttern. Für `tagsFor`: `std::string_view` für Typ/Tag (`find('.')` ohne Substring). Linearer Style-Lookup bei Bedarf später per `unordered_map` neben dem geordneten Vektor – nur wenn gemessen.
- **Erwarteter Effekt**: −12 duplizierte Zeilen, keine Drift der Palette; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Ergebniswerte identisch halten; vorhandener `test_ui_theme.cpp` (Rollen pinnen, Kontrast) deckt es ab.

### CORE-617 – Kleinigkeiten SequenceJson: fragile Enum-Iteration, Pretty-Print großer Float-Arrays
- **ID**: CORE-617
- **Kategorie**: A/F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SequenceJson.cpp:31-39, 112-115, 164`
- **Problem**: `kindFromName` iteriert `0..static_cast<int>(SequenceTrackKind::Audio)` – ein neuer Track-Typ nach `Audio` wird beim Lesen stillschweigend verworfen, solange diese Konstante nicht mitwandert (der `switch` in `kindName` warnt dagegen per `-Wswitch`). `j.dump(2)` schreibt die `times`/`values`-Arrays einer Property-Spur mit je einer Zeile pro Float (nlohmann pretty-print); eine Spur mit tausenden Keys erzeugt entsprechend große Chunk-Texte (und `MCP`-Antworten).
- **Code-Auszug**:
```cpp
for (int i = 0; i <= static_cast<int>(SequenceTrackKind::Audio); ++i)   // „letzter Enumerator" von Hand
```
- **Lösung**: `kindFromName` über eine `constexpr`-Tabelle `{kind, name}` führen (dann `kindName` daraus ableiten, `static_assert` auf die Größe), Float-Arrays kompakt schreiben (z.B. eigenes Dump mit Arrays in einer Zeile oder `dump(2)` nur für Struktur). Letzteres ist optisch/Größen-Optimierung – messen, ob Sequenzen überhaupt groß werden.
- **Erwarteter Effekt**: Kein stilles Verwerfen neuer Track-Typen; kleinere Dateien (Schätzung, nur bei langen Spuren).
- **Risiko / Verifikation**: Datei-Byte-Identität für bestehende Sequences ändert sich bei kompaktem Dump (Diff-Rauschen in Git, Collab-Sync-Diffs); Test: Round-Trip `sequenceToJson → sequenceFromJson` bleibt identisch (vorhandene Sequence-Tests).

### CORE-618 – TabBox-Labels werden pro Frame neu aufgebaut und pro Render/Klick neu gemessen
- **ID**: CORE-618
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIElement.cpp:3033-3073, 3107-3108`; Aufbau in `UIWidgetTree.cpp:1239-1257` (`uiApplyAutoSize`)
- **Problem**: `uiApplyAutoSize` ersetzt pro Frame für jede TabBox `tabLabels` (`clear` + Schleife über *alle* Elemente → O(TabBoxen·N) mit `std::string`-Kopien). `tabLayout` misst in `render()` (und in `tabAtPoint` bei jedem Pointer-Event) alle Labels neu mit `measureUIText`. Bei einer TabBox mit 10 Tabs sind das pro Frame 10 Messungen + 10 Stringkopien.
- **Code-Auszug**:
```cpp
for (const std::string& label : labels)
{
    const float textW = label.empty() ? 0.0f : (f ? HE::measureUIText(*f, label, sizePx, 0.0f, opts) : ...).x;
```
- **Lösung**: Labels nur ersetzen, wenn sich Kindnamen geändert haben (Vergleich statt Neuaufbau) und die Breiten mit `(labels, sizePx, fontAtlasKey)`-Schlüssel im Element cachen (`mutable std::vector<float> m_tabW`). Zusammen mit dem Kindindex-Cache aus CORE-601 kann die O(N)-Kindschleife entfallen.
- **Erwarteter Effekt** (Schätzung): Gering; relevant nur bei vielen TabBoxen/großen Bäumen.
- **Risiko / Verifikation**: Cache-Schlüssel muss `fontScale` (über `sizePx`) und Font-Wechsel abdecken; Tests `tests/test_ui_widgets.cpp` (Tab-Klick auf zugeschnittene Labels).

---

## Gelesen

Alle 20 Dateien des Slice wurden vollständig gelesen (Sequence/SequenceJson.{h,cpp}, SlotMap.h, Types/{Defines,Enums,Handle,TypeRegistry,UUID}.{h,cpp}, UIWidget/{AppMenu.h, UIElement.{h,cpp} in Blöcken bis Zeile 4348, UIElements.h bis Zeile 2215, UIShortcut.{h,cpp}, UITheme.{h,cpp}, UIWidgetAnim.{h,cpp}, UIWidgetBinding.cpp}); Aufrufer in `UIWidgetTree.cpp`/`WidgetManager.cpp` nur gezielt (grep + ~40 Zeilen) zur Belegung der Funde gelesen.

## Core Slice 7 – UIWidget (UIWidgetBinding.h, UIWidgetTree.{h,cpp}, UIWindowFrame.{h,cpp}, WidgetManager.cpp)

## Zusammenfassung

Der Slice ist funktional sauber und sehr gut kommentiert; `UIWindowFrame.*` und `UIWidgetBinding.h` sind unauffällig (reine Geometrie bzw. drei Deklarationen, keine Funde). Das Hauptproblem ist architektonisch: Layout und alle vererbten Zustände (Rect, Sichtbarkeit, Enabled, Opacity, Rotation, Clip, Tiefe/Sortier-Key) sind bewusst *zustandslos* und werden pro Element über Parent-Ketten mit linearem `tree.find()` neu berechnet. Jede Frame-Schleife (`extract`, `topmostHit`, `uiApplyAutoSize`, `processPointer`) ruft das für jedes Element auf, was grob O(n²·Tiefe) pro Frame ergibt. Dazu kommen List-Zeilen-Grafting (`graftChildRef`), das pro Zeile den ganzen Baum neu themed/übersetzt/Assets auflöst, und mehrere pro-Frame-Vollscans mit `dynamic_cast`. Funde: 4 Hoch, 6 Mittel, 9 Niedrig. Alle Aussagen sind statisch gelesen, ohne Messdaten; wo Größenordnungen geschätzt sind, ist das markiert.

## Funde

### CORE-701 – Layout ist zustandslos: jede Rect-Abfrage läuft Parent-Kette + lineare Suche + Geschwister-Scans
- **ID**: CORE-701
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:16-19` (find), `:230-259` (parentRectOf), `:464-527` (boxSlotRect), `:793-818` (gridSlotRect/solveGrid `:566-711`), `:831-896` (wrapSlotRect), `:1012-1063` (uiElementRect/Unbounded); Verbraucher u. a. `WidgetManager.cpp:6165-6171`, `:2969-3006`, `UIWidgetTree.cpp:1277-1285`
- **Problem**: `uiElementRect(e)` ruft für Kind-eines-Containers `boxSlotRect`, das `uiElementRect(box)` (rekursiv bis zur Wurzel) aufruft und zweimal über *alle* `tree.elements` läuft; `tree.find` ist ein linearer Scan (`GraphModel.h:findNodeById`). Pro Rect-Abfrage also O(Tiefe·n); `GridSolved` wird sogar pro Kind komplett neu gelöst (`gridSlotRect` → `solveGrid`, enthält `cellOf`-O(rows·cols) je Auto-Track-Kind). Jeder Frame fragt das Rect für jedes Element ab (extract, topmostHit, AutoSize, Scroll-Thumb, Wheel) → O(n²·Tiefe), bei Grids/Wraps/Listen zusätzlich O(n²) pro Container. Kommentar im Code ("no cached slot table to invalidate") nennt die Entscheidung, aber ohne Messung.
- **Code-Auszug**:
```cpp
// UIWidgetTree.cpp:491-499 (boxSlotRect), 511-525 – zwei volle Durchläufe pro Kind
for (const auto& sp : tree.elements)
{
    if (!sp || sp->parentId != box.id || !sp->visible) continue;
    ++count; ...
}
// :1028 und :237
if (const UIElement* p = tree.find(e.parentId); p && p->laysOutChildren())
```
- **Lösung**: Pro Frame (oder pro „Tree-Version“) ein einziger Top-down-Layoutpass, der für alle Elemente `UIWidgetRect` (inkl. Bounds) in ein flaches `std::vector<UIWidgetRect>` (Index = Elementindex) schreibt: (1) `id→index`-Tabelle und `children`-Listen (CSR-Array parent→Kinder, in Vektorreihenfolge) beim Strukturwechsel bauen; (2) Wurzeln auflösen, dann je Container die Slots seiner Kinder in *einem* Durchlauf verteilen (Box: Summen einmal, Grid: `solveGrid` einmal pro Grid, Wrap: Linien einmal). `uiElementRect(tree,e,canvas)` bleibt als dünner Wrapper (Cache-Treffer oder Fallback auf alten Pfad), damit Editor/Designer-Aufrufer unverändert funktionieren. Cache-Invalidierung über einen `tree.revision`-Zähler, der von allen Mutatoren (setPropAny, add/remove/move, AutoSize/Scroll-Änderungen) erhöht wird – oder simpler: Cache nur innerhalb eines Aufrufs von `extract`/`topmostHit` halten (Scope-Objekt `UILayoutScope`), dann entfällt jede Invalidierungslogik.
- **Erwarteter Effekt**: Schätzung: von O(n²·d) auf O(n) pro Frame; bei Seiten mit einigen hundert Elementen vermutlich ein Faktor 10-100 weniger CPU-Zeit im UI-Pfad (messen).
- **Risiko / Verifikation**: Hohes Verhaltensrisiko, weil Layout exakt gleich bleiben muss (Einheiten-Skalierung in WidgetRefs, Min/Max-Bounds, DesignSlots, Scroll-Offsets). Absicherung: Differential-Test, der für alle Beispiel-/Asset-Bäume (`tests/test_ui_widgets.cpp`, nur mit Offset/Limit lesen) Rect aus Alt- und Neupfad für jedes Element vergleicht (Toleranz 0), inkl. Grid/Wrap/List/Tab/Splitter/Accordion/WidgetRef-Verschachtelung. Zunächst nur den Cache einführen, Algorithmus unverändert lassen.

### CORE-702 – `topmostHit` macht pro Frame für jedes Element die teuerste Arbeit vor dem billigen Punkt-Test
- **ID**: CORE-702
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/WidgetManager.cpp:2951-3056` (Hauptschleife `:2969-3006`); zweiter Vollscan in `windowHitAt` `:3090-3123`; Aufrufer `HE_Game/src/GameApplication.cpp:2347`, `HE_Editor/EditorApplication.cpp:4188`
- **Problem**: `processPointer` wird laut Kommentar jeden Frame aufgerufen („An application calls this every frame whether the mouse stirred or not“), auch wenn sich Zeiger und Baum nicht geändert haben. Darin läuft `topmostHit` über alle Elemente aller Widgets und ruft *vor* dem Punkt-in-Rect-Test nacheinander `uiElementEffectiveVisible` (rekursiv), `uiElementEffectiveEnabled`, `uiElementEffectiveOpacity`, `uiElementRect`, `uiElementRotation` (bis zu 3 Kettenwanderungen) und `uiElementClipRect` auf – jeweils mit linearem `find`. Die Mehrheit der Elemente liegt gar nicht unter dem Zeiger. `windowHitAt` (aus dem OS-Hit-Test-Callback, potentiell pro Mausbewegung) wiederholt dieselbe Struktur und ruft zusätzlich vorher `topmostHit`.
- **Code-Auszug**:
```cpp
if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
if (!e.hitTestable) continue;
if (!HE::uiElementEffectiveEnabled(w.tree, e)) continue;
if (HE::uiElementEffectiveOpacity(w.tree, e) <= 0.001f) continue;
HE::UIWidgetRect r = HE::uiElementRect(w.tree, e, &canvas);
...
if (HE::uiElementClipRect(w.tree, e, clip, &canvas)) { ... }
```
- **Lösung**: (1) Early-out im Aufrufer: wenn `valid`, Zeigerposition, Viewport, Baum-Revision (siehe CORE-701) und Grab-Stack gegenüber dem letzten Frame unverändert sind, das letzte `PointerHit` wiederverwenden. (2) Reihenfolge ändern: `hitTestable` zuerst (billig), dann Rect (aus Cache), dann den groben Punkt-Test gegen das Rect *erweitert um die Rotation*: nur wenn `rot`-Kette leer ist (Flag im Layout-Cache), sofort ablehnen; erst danach Visible/Enabled/Opacity/Clip. Da alle Filter reine Konjunktionen sind, bleibt das Ergebnis identisch. (3) Den Clip-Schnitt (identischer Block in `:2994-3002` und `:3105-3117`) in eine Hilfsfunktion auslagern.
- **Erwarteter Effekt**: Schätzung: pro Frame von O(n²·d) auf ~O(n) bzw. bei ruhendem Zeiger O(1); in UI-lastigen Szenen (Menüs, Listen) spürbar, vor allem im Editor-Viewport und im Spiel mit eingeblendetem HUD (messen).
- **Risiko / Verifikation**: Mittel. Zeiger-Cache darf Hover/Cursor nicht „einfrieren“, wenn sich Elemente ohne Revisionszähler ändern (Skript-Writes, Animationen, Scroll) → Zähler muss alle Pfade abdecken (siehe CORE-705). Tests: bestehende Pointer-Tests in `tests/test_ui_widgets.cpp`; zusätzlich Test „gleiche Eingabe, geänderter Baum (Visible/Enabled/Opacity per Skript) ⇒ neuer Hit“.

### CORE-703 – `extract` ermittelt vererbte Zustände pro Element mit separaten Parent-Wanderungen
- **ID**: CORE-703
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: `WidgetManager.cpp:19-29` (elementSortKey), `:6163-6173`, `:6229`, `:6243`, `:6358-6360`, `:6390`; `UIWidgetTree.cpp:1372-1421` (ClipRect), `:1521-1533`, `:1624-1639`, `:1641-1706`, `:1717-1730`
- **Problem**: Für jedes sichtbare Element läuft `extract` mindestens: `uiElementEffectiveVisible` (rekursiv), `elementSortKey` (Tiefen-Walk), `uiElementRect`, `uiElementClipRect` (Walk + Rect pro Clip-Ahn), `uiElementUnitScale` (Walk, ruft ggf. `uiElementRect(ref)` + `uiResolveCanvasFor`), `uiElementEffectiveOpacity`, `uiElementEffectiveEnabled`, `uiElementRotation` (2-3 Walks + Rect pro rotierendem Ahn). Das sind ~8 Parent-Ketten mit je linearem `find` pro Schritt, also O(n·d·n) pro Widget und Frame. Zusätzlich wird `items` (mit je `UIWidgetRect`) pro Widget pro Frame neu allokiert und stable-sortiert, `out` ohne `reserve`.
- **Code-Auszug**:
```cpp
for (const auto& ep : w.tree.elements)
{
    if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
    items.push_back({ &e, elementSortKey(w.tree, e), HE::uiElementRect(w.tree, e, &canvas) });
}
...
const float alpha = HE::uiElementEffectiveOpacity(w.tree, e) * ...;
const bool  usable = HE::uiElementEffectiveEnabled(w.tree, e);
```
- **Lösung**: Ein einziger Top-down-Pass (derselbe wie in CORE-701, Struktur `UIResolvedState {rect, unitScaleX/Y, depth, visible, enabled, opacity, clip(optional), rotation(optional)}` pro Element), jeweils aus dem Zustand des Parents abgeleitet (O(1) pro Element). `extract` und `topmostHit` lesen nur noch aus dieser Tabelle; die öffentlichen `uiElementEffective*`-Funktionen bleiben für Editor/Tests als Wrapper bestehen. `items`/`out` mit `reserve` bzw. als Member-Scratch wiederverwenden (CORE-C: Heap pro Frame).
- **Erwarteter Effekt**: Schätzung: wie CORE-701; zusätzlich entfallen pro Frame und Widget 1 Vektor-Allokation sowie das Sortieren neu berechneter Keys.
- **Risiko / Verifikation**: Gleiche Semantik der Kette (Cycle-Guards, fehlende Parents ⇒ `break`, Tab-Box-`hidesChild`). Differentialtest wie in CORE-701 für jede der Funktionen gegen den alten Pfad; ein Test mit zyklischem/ungültigem `parentId`.

### CORE-704 – `graftChildRef` wendet Theme, Text-Katalog und Asset-Auflösung auf den GANZEN Baum an (pro List-Zeile)
- **ID**: CORE-704
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:750-838` (Kern `:818-824`), `:223-401` (embedWidgetRefs, `:244-247`, `:278-283`, `:356-362`, `:395-398`), `:1913-1927` (Zeilen-Schleife in `syncLists`)
- **Problem**: Jede neu realisierte Zeile (Scrollen erzeugt laufend neue, bis zu `kMaxRealizedRows = 256`) führt aus: `embedWidgetRefs` (scannt alle Elemente nach WidgetRefs, parst die Zeilen-JSON komplett neu mit `uiWidgetTreeFromJson`, scannt noch zweimal alle Elemente für Slots/`given`), danach `uiApplyTheme(w.tree, …)` (alle Elemente × alle Properties), `uiApplyTextCatalog(w.tree, …)` und `refreshElementAssets` für *alle* Elemente (je drei `loadAsset(path)`-Auflösungen: Material, Textur, Font). Kosten pro Zeile also O(n_gesamt), pro Füllung einer Liste O(rows·n). Außerdem überschreibt das Re-Theming bestehende Elemente erneut (kann Skript-gesetzte Farben zurücksetzen – evtl. unbeabsichtigter Nebeneffekt).
- **Code-Auszug**:
```cpp
HE::uiApplyTheme(w.tree, themeFor(w), themeMode());
HE::uiApplyTextCatalog(w.tree, m_catalog, m_language);
for (const auto& e : w.tree.elements)
    if (e && e->id > 0) refreshElementAssets(w, *e);
```
- **Lösung**: (1) Alle drei Schritte nur auf die neu eingefügten Elemente (Id-Bereich `(offset, em.idMax]` plus das Ref-Element selbst) anwenden; Hilfsfunktionen mit Bereichsparameter bzw. Elementliste. (2) Den geparsten Teilbaum pro Asset-Pfad cachen (analog `m_columnInfo`) und klonen statt JSON neu zu parsen; Cache beim Reload/`createWidget` invalidieren. (3) `embedWidgetRefs` beim Graft nur die neuen Elemente nach Refs durchsuchen statt `w.tree.elements` komplett.
- **Erwarteter Effekt**: Schätzung: Zeilen-Graft von O(n) auf O(Größe der Zeilen-Vorlage); Scrollen durch große Listen/Tabellen ruckelfrei statt linear in der Seitengröße (messen: Frame-Zeit beim Scrollen einer 10 000-Zeilen-Liste mit 30 sichtbaren Zeilen).
- **Risiko / Verifikation**: Mittel. Verhaltensänderung nur beim Überschreiben vorhandener, skript-gesetzter Theme-/Textwerte (de facto Bugfix, aber beobachtbar). Tests: List-Test, der eine Zeile realisiert, vorher ein Skript-Property auf einem Nicht-Zeilen-Element setzt und prüft, dass es erhalten bleibt (bzw. bewusst dokumentiert); Test auf identisches Ergebnis des Zeilen-Teilbaums vs. bisher.

### CORE-705 – `setProperty`/`setPropertyOn` setzen `m_visualDirty` bedingungslos
- **ID**: CORE-705
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:121-138` (`:132`), `:151-162` (`:159`)
- **Problem**: Jeder Skript-Schreibzugriff markiert das Bild als verändert, auch wenn der Wert identisch ist (typisch: ein Tick-Graph schreibt jeden Frame denselben Text/Position). Die event-getriebene Schleife (A2) kommt dadurch nie zur Ruhe, und `extract` + `topmostHit` laufen jeden Frame (siehe CORE-701/702/703). Der Code liest den alten Wert ohnehin schon (`e->getPropAny(prop).type`) und besitzt `samePropValue` (`:583`).
- **Code-Auszug**:
```cpp
e->setPropAny(prop, HE::uiHcValueToProp(v, e->getPropAny(prop).type));
m_visualDirty = true;
```
- **Lösung**: Alten Wert einmal holen (`const auto cur = e->getPropAny(prop)`), neuen Wert konvertieren, nur bei `!samePropValue(cur, next)` schreiben und `m_visualDirty` setzen. `afterScriptWrite` und `refreshElementAssets` unverändert für den Änderungsfall; für den Gleichheitsfall nur das Dirty-Flag weglassen (Wert wurde ohnehin nicht geändert). Spart nebenbei den zweiten String-Lookup und die zwei `std::string`-Vergleiche `prop == "Material"` nur wenn geändert.
- **Erwarteter Effekt**: Schätzung: bei Skripten mit Tick-Writes gleicher Werte kein Redraw mehr → Idle-CPU/GPU-Last in Apps sinkt deutlich; sonst neutral.
- **Risiko / Verifikation**: Niedrig, solange Properties mit Seiteneffekt beim Setzen (Grid-Tracks parsen neu, `setPropAny` mit Normalisierung) beim Vergleich den *normalisierten* Zustand sehen – daher Vergleich nach dem Schreiben (alter Wert vorher kopieren, `samePropValue(alt, getPropAny(prop))`). Test: Skript schreibt zweimal denselben Wert, `consumeVisualDirty()` ist nach dem zweiten false.

### CORE-706 – `syncLists()` läuft in jedem Pointer-/Extract-/Wheel-/Drop-Aufruf und hat O(rows·n)-Teile
- **ID**: CORE-706
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:1732-1736`, `:1816-2016` (Spalten-Schleife `:1963-1991`), Aufrufer `:3300`, `:4255`, `:4278`, `:5629`, `:6023`, `:6106`
- **Problem**: Pro Aufruf und Widget: Scan aller Elemente nach `ListView` (virtueller `type()`-Aufruf je Element), pro Liste 2-3× `listRowsOf` (`dynamic_cast` über alle Elemente), und – bei sichtbarem Header – pro *Zeile* zwei komplette Baumdurchläufe, um Row-Root und Zellen zu finden. `rows` (bis 256) × `n` ergibt O(rows·n) pro Sync, und der Sync läuft jeden Frame (`processPointer`, `extract`), auch wenn sich weder Scroll noch Itemanzahl noch Viewport geändert haben.
- **Code-Auszug**:
```cpp
for (HE::UIWidgetRef* r : rows)
{
    const HE::UIElement* rowRoot = nullptr;
    for (const auto& ep : w.tree.elements)
        if (ep && ep->parentId == r->id) { rowRoot = ep.get(); break; }
    ...
    for (const auto& ep : w.tree.elements) { if (!ep || ep->parentId != rowRoot->id) continue; ... }
}
```
- **Lösung**: (1) Dirty-Gate pro Liste: Sync nur, wenn sich `(itemCount, scrollOffset, innerHeight, rowWidget, Viewport)` oder die Elementstruktur (Revision) seit dem letzten Sync geändert haben, sonst früh zurück. (2) Wo gesynct wird, einmal pro Aufruf eine `parentId → Kinder`-Map aufbauen und für Row-Root/Zellen nutzen (statt zwei Scans pro Zeile). (3) Liste der ListView-Ids pro Widget cachen (Strukturrevision).
- **Erwarteter Effekt**: Schätzung: Leerlauf-Sync von O(n) auf O(#Listen); Spaltenpflege von O(rows·n) auf O(n).
- **Risiko / Verifikation**: Gate muss alle Eingänge kennen (auch Skript-Änderungen an `rowWidget`, `showHeader`, Spaltenanteilen); ein vergessener Eingang ließe Listen veralten. Test: `setListCount`, Scrollen, Resize, Spaltenbreite ziehen, Template-Wechsel jeweils → Zeilen korrekt (vorhandene List-Tests).

### CORE-707 – `isInteractive` alloziert pro Aufruf `std::vector<EventBinding>` mit `std::string`-Namen
- **ID**: CORE-707
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:2921-2945`, genutzt in `:3012`, `:3327`, `:5451` (isFocusable); Quelle `HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1281-1330` (`eventBindingsOf`)
- **Problem**: `rt().eventBindingsOf(id)` baut jedes Mal einen neuen Vektor mit Namens-Strings (compiled: Kopie aller `eventInfos`; interpretiert: Durchlauf aller Nodes aller Klassenlevel mit O(k²)-Duplikatprüfung per `std::any_of`). Anschließend wird linear mit sieben `std::string`-Vergleichen gesucht. Aufgerufen für jeden Treffer-Kandidaten im Pointer-Hit-Test (jeden Frame), beim Bubbling nach oben und für jedes Element in `navigate`/`focusNext` (`isFocusable`).
- **Code-Auszug**:
```cpp
for (const auto& b : rt().eventBindingsOf(target.scriptId))
    if (b.elem == 0 || b.elem == target.elem)
    {
        const std::string& n = b.name;
        if (n == "OnClicked" || n == "OnPressed" || n == "OnReleased" || ...
```
- **Lösung**: In `Runtime` pro Instanz eine vorberechnete Menge „Pointer-Bindungen“ pflegen (z. B. sortiertes `std::vector<int>` der `elem`-Ids plus Flag „any element“), beim `add`/Graph-Änderung/`addCompiled` aufgebaut; API `bool hasPointerBinding(InstanceId, int elem) const` ohne Allokation. WidgetManager ruft nur noch diese.
- **Erwarteter Effekt**: Schätzung: pro Hit-Kandidat von mehreren Heap-Allokationen + String-Kopien auf einen binären Suchlauf; entfällt komplett als Allokationsquelle im Frame.
- **Risiko / Verifikation**: Niedrig-Mittel (Cross-Modul, Runtime-Klassenhierarchie „leaf-most wins“ muss gleich bleiben). Test: bestehende Interaktionstests + neuer Test, dass ein vererbtes Event (Basis-Klasse) weiterhin `interactive` macht.

### CORE-708 – Skript-Property-Zugriffe laufen über lineare Suchen (Owner, Embed-Offset, Element, Name)
- **ID**: CORE-708
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:32-41` (find), `:49-70` (scriptTargetFor/resolveScriptOwner), `:80-100` (elementOfScript), `:109-162` (Bindings), `:2934` (isInteractive)
- **Problem**: Jedes `getProperty`/`setProperty` aus einem HorizonCode-Graph: `resolveScriptOwner` durchläuft alle Instanzen *und* deren komplette `embeds` (bei Listen bis ~256 Einträge pro Widget), danach `tree.find(elem+off)` linear über alle Elemente. `getPropertyOn/setPropertyOn` (per Name) scannen zusätzlich alle Elemente mit `std::string`-Vergleich (`elementOfScript`). `scriptTargetFor` läuft rückwärts linear durch alle Embeds und wird bei jedem Pointer-Event, jeder Aktivierung und in `isInteractive` pro Kandidat aufgerufen. Bei einer Liste mit vielen Zeilen-Graphen, die in `OnRowBind`/`Tick` Properties schreiben, wird jeder Zugriff O(Embeds + Elemente).
- **Code-Auszug**:
```cpp
for (auto& w : m_instances)
{
    if (w.scriptId == scriptId) return &w;
    for (const Instance::Embed& em : w.embeds)
        if (em.scriptId == scriptId) { idOffset = em.idOffset; return &w; }
}
```
- **Lösung**: `unordered_map<InstanceId, {widgetIndex, embedIndex}>` (oder sortierter Vektor) beim Registrieren/Entfernen pflegen; Embeds sind nach `idOffset` aufsteigend – `scriptTargetFor` kann per `std::upper_bound` suchen. Elementzugriff über die `id→index`-Tabelle aus CORE-701; für `…On` pro Instanz `name → elemId`-Cache (Invalidierung bei Struktur-/Namensänderung).
- **Erwarteter Effekt**: Schätzung: konstante statt lineare Kosten je Skript-Zugriff; wirkt vor allem bei vielen Embeds/Zeilen (messen).
- **Risiko / Verifikation**: Niedrig, aber Invalidierung nach `removeChild`/`graftChildRef`/`destroyWidget` (Vektor-Reallokation von `m_instances` – Zeiger/Indizes!). Test: Zeilen entfernen/hinzufügen und danach Property-Zugriffe der verbliebenen Zeilen.

### CORE-709 – Pro-Frame-Vollscans mit `dynamic_cast` über alle Elemente
- **ID**: CORE-709
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:2705-2717` (Spinner-Scan in `tick`), `:3547-3597` (drei Scans in `processPointer`), `UIWidgetTree.cpp:1248-1258`, `:1299-1303`, `:1537-1606` (`uiUpdateScrollExtents`: bis zu 4 `dynamic_cast` je Element), `:1832-1833`
- **Problem**: Jeder Frame (tick, processPointer, extract) läuft mehrfach über alle Elemente, um nur wenige Typen (ProgressBar-indeterminate, ListView, TreeView, DatePicker, TabBox, Accordion, ScrollBox, Box mit sizeToContent) zu finden – per RTTI. Bei n Elementen und ~8 Scans sind das 8·n `dynamic_cast` pro Frame und Widget.
- **Code-Auszug**:
```cpp
for (auto& ep : w.tree.elements)
{
    auto* lv = dynamic_cast<HE::UIListView*>(ep.get());
    if (!lv) continue;
```
- **Lösung**: Beim Strukturwechsel (add/remove/move/graft/clear) einmal Typlisten pro Widget aufbauen (`std::vector<UIListView*>`, `…TreeView*`, `…DatePicker*`, `…TabBox*`, `…Accordion*`, `…ScrollBox*`, `…ProgressBar*`) und per Revision invalidieren; die Scans iterieren dann nur diese Listen. Alternativ `UIElement::type()` (virtuell, bereits vorhanden) statt `dynamic_cast` zum billigen Vorfiltern verwenden.
- **Erwarteter Effekt**: Schätzung: pro Frame ~8·n RTTI-Casts weniger; absolut klein pro Scan, summiert sich aber mit CORE-701/702/703 auf (messen).
- **Risiko / Verifikation**: Niedrig; gefährlich sind nur gespeicherte Rohzeiger über Vektor-Realloc/`erase` – daher Zeiger nie über eine Frame-Grenze halten (Revision prüfen).

### CORE-710 – `uiApplyAutoSize` rechnet für jedes Element ein Rect und baut Label-Strings jeden Frame neu
- **ID**: CORE-710
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1239-1370` (`:1248-1258` Tab-Labels, `:1277-1294` Elementschleife, `:1299-1315` Tiefenberechnung+Sort), `:1563-1573` (Accordion-Labels)
- **Problem**: (a) Für *jedes* Element wird `uiElementUnitScale` und `uiElementRect(...).w` berechnet, bevor `applyAutoSize` aufgerufen wird; die Basisimplementierung ist ein No-op (`UIElement.h:948`), `autoSizedAxes()` ist für die meisten Typen 0. (b) Für jede TabBox werden `tabLabels` jeden Frame geleert und neu per `push_back(std::string)` gefüllt, ebenso Accordion `sectionLabels`/`sectionBodies` – O(n) je Tab/Accordion und Heap-Allokationen pro Frame, auch wenn sich nichts geändert hat. (c) Die Tiefe der selbstmessenden Boxen wird per Parent-Walk mit linearem `find` bestimmt und sortiert, jeden Frame.
- **Code-Auszug**:
```cpp
e->applyAutoSize(uiElementRect(tree, *e, canvas).w / std::max(1e-4f, us), fontScale);
```
- **Lösung**: (a) Nur Elemente behandeln, die tatsächlich messen: Vorfilter `e->autoSizedAxes() != 0` – unsicher, ob alle `applyAutoSize`-Überschreibungen auch bei `autoSizedAxes()==0` Arbeit leisten (z. B. UIElements.h:187/364/840/1993) → vor Umsetzung die vier Overrides prüfen oder ein `virtual bool needsAutoSize()` einführen. (b) Labels nur schreiben, wenn sich Namen/Anzahl der Kinder ändern (in-place vergleichen statt `clear()` + neu bauen; Strukturrevision). (c) Tiefe aus dem Top-down-Pass (CORE-701/703) beziehen.
- **Erwarteter Effekt**: Schätzung: spart pro Frame ein Rect (O(d·n) wegen find) je nicht-messendem Element sowie Strings/Allokationen der Tab/Accordion-Leisten.
- **Risiko / Verifikation**: Mittel wegen Override-Semantik (siehe oben). Test: Text-Auto-Size mit/ohne Wrap, SizeToContent-Boxen verschachtelt, Tab-Beschriftung nach Umbenennen einer Seite.

### CORE-711 – Menüleiste: Breiten werden bei jeder Abfrage neu gemessen (O(k²) Text-Messungen)
- **ID**: CORE-711
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:1384-1394` (menuTitleRect), `:1396-1427` (menuPopupRect), `:1429-1461` (menuTitleAt/menuItemAt), `:6590-6611` (drawMenuBar)
- **Problem**: `menuTitleRect(i)` summiert `menuTextWidth` aller vorherigen Titel; `menuTitleAt` ruft es in einer Schleife (O(k²) `measureUIText`-Aufrufe), `drawMenuBar` ruft es erneut pro Titel und dann `menuPopupRect`, das jede Zeile misst und per `uiParseShortcut`/`uiFormatShortcut` (String-Allokationen) formatiert; `menuItemAt` ruft `menuPopupRect` pro Zeigerframe, solange ein Menü offen ist. Unsicher/messen: Kosten von `measureUIText` (Font-Cache?).
- **Code-Auszug**:
```cpp
for (std::size_t k = 0; k < i; ++k)
    pen += menuTextWidth(m_menuBar[k].label, mm.fontPx) + 2.0f * mm.titlePad;
```
- **Lösung**: Layout-Struct `MenuLayout {vector<float> titleX, titleW; popup rect; vector<string> chordText}` pro (`m_menuBar`-Version, `m_fontScale`, `m_menuOpen`) cachen und in `setMenuBar`/`setFontScale`/`openMenuAt` invalidieren; alle vier Funktionen lesen daraus.
- **Erwarteter Effekt**: Schätzung: ein paar µs pro Frame, bei kleiner Menüzahl kaum messbar; vor allem Vereinfachung (eine Quelle für Bild und Hit-Test, wie im Kommentar gefordert).
- **Risiko / Verifikation**: Niedrig. Vorhandene Menü-Tests; zusätzlich Test mit Font-Scale-Wechsel bei offenem Menü.

### CORE-712 – Theme-Anwendung: Stilkette, Property-Namen und `getPropAny`-Kopien pro Property
- **ID**: CORE-712
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1089-1129` (uiThemeStylesFor/DecidedProps), `:1131-1207` (uiThemeValueFor), `:1209-1237` (uiApplyTheme)
- **Problem**: `uiApplyTheme(Element)` ruft `uiThemeDecidedProps` (baut `std::vector<const Style*>` + `typeName()`-String + `styleFor`-Lookups + `vector<string>` mit O(p²)-Duplikatprüfung), und danach pro Property `uiThemeValueFor`, das `getPropAny(prop)` (liefert `UIPropValue` mit String/Liste *by value*) und *erneut* `uiThemeStylesFor` aufruft. Das läuft bei `setTheme`, Modus-Wechsel, `registerInstance` und – siehe CORE-704 – bei jedem Zeilen-Graft über alle Elemente.
- **Code-Auszug**:
```cpp
for (const std::string& prop : uiThemeDecidedProps(e, theme))
    if (e.themeRoleFor(prop).empty()) write(prop);   // write → uiThemeValueFor → uiThemeStylesFor
```
- **Lösung**: Stilkette einmal pro Element bestimmen und an eine interne `uiThemeValueFor(e, styles, mode, prop, out)` weiterreichen; Property-Namen als `std::string_view`/Pointer in den Style-Daten statt Kopien; Dedup über kleines `flat_set` o. ä.
- **Erwarteter Effekt**: Schätzung: 2-4× weniger Allokationen pro Element beim Theme-Wechsel; wirkt hauptsächlich in Kombination mit CORE-704 (dort Hauptgewinn durch Bereichsbeschränkung).
- **Risiko / Verifikation**: Niedrig. Bestehende Theme-Tests (Rollen-Bindung, `kUIThemeLiteral`, Kaskade); Ausgabe muss bytegleich bleiben.

### CORE-713 – `uiElementToJsonObj` baut pro Element ein Prototyp-Element
- **ID**: CORE-713
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1820-1826`; außerdem `:1754`/`:1758` (zweimal `uiAnchorLegacyPointOf`)
- **Problem**: Für jede Serialisierung (Speichern, Undo-Snapshots des Designers, Collab pro Element, Clipboard) wird `makeUIElement(e.type())` heap-allokiert und konstruiert, nur um `cornerRadius` zu vergleichen. `uiAnchorLegacyPointOf` (9-Punkt-Schleife) wird doppelt ausgewertet.
- **Code-Auszug**:
```cpp
if (const std::unique_ptr<UIElement> proto = makeUIElement(e.type());
    !proto || e.cornerRadius != proto->cornerRadius)
```
- **Lösung**: Statisches Array `const glm::vec4 kDefaultRadius[UIWidgetType::COUNT]` lazily (function-local static) befüllen, oder einen virtuellen `defaultCornerRadius()`-Getter am Element; Ergebnis von `uiAnchorLegacyPointOf` in lokaler Variable halten.
- **Erwarteter Effekt**: Schätzung: eine Allokation + Konstruktion pro Element und Save eingespart; Editor-Pfad, daher niedrig.
- **Risiko / Verifikation**: Niedrig; die vorhandenen Round-Trip-Tests (byte-identisches Speichern) decken es ab.

### CORE-714 – `uiElementRotation`: drei fast identische Parent-Walks für die Früh-Ausstiegsprüfung
- **ID**: CORE-714
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1641-1706` (Walks `:1647-1656`, `:1661-1672`, `:1685-1702`)
- **Problem**: Die Prüfung „gibt es überhaupt eine Rotation in der Kette“ benötigt zwei separate Wanderungen (Summe der Winkel, dann nochmal „irgendein Eltern-Winkel > 1e-4“), danach eine dritte für die eigentliche Berechnung. Die Funktion wird für jedes Element in `extract`/`topmostHit` aufgerufen (Common Case: false) und kostet damit zwei Ketten mit linearem `find`.
- **Code-Auszug**:
```cpp
if (std::fabs(total) < 1e-4f && std::fabs(e.rotation) < 1e-4f)
{
    bool any = false;
    int g2 = 0;
    const UIElement* c2 = &e;
    while (c2->parentId != 0 && g2++ < ...) { ... if (std::fabs(p->rotation) > 1e-4f) { any = true; break; } ... }
    if (!any) return false;
}
```
- **Lösung**: Eine einzige Schleife, die `total` *und* `any` gleichzeitig berechnet; Rückgabe `false` genau dann wenn `|total|<1e-4 && |e.rotation|<1e-4 && !any` (identische Semantik). Mit CORE-703 entfällt die Funktion im Common Case ganz (Flag „Kette rotiert“ aus dem Top-down-Pass).
- **Erwarteter Effekt**: Schätzung: halbiert die Kettenwanderungen im Common Case; kleiner Lesbarkeitsgewinn.
- **Risiko / Verifikation**: Niedrig; Test mit Kette aus sich aufhebenden Rotationen (+30°/−30°) und mit vielen sehr kleinen Winkeln (Sonderfall `total ≥ 1e-4` ohne einzelnen Winkel > 1e-4).

### CORE-715 – `processPointer` (~1070 Zeilen) und `WidgetManager.cpp` (6810 Zeilen) bündeln unabhängige Zuständigkeiten
- **ID**: CORE-715
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `WidgetManager.cpp:3127-4198` (processPointer), gesamte Datei
- **Problem**: `processPointer` enthält Menüleiste, Dropdown-Grab, Hover, Listen-/Tree-/Datepicker-Hover, Press-Behandlung pro Widget-Typ (Slider, Splitter, TextInput, DatePicker, ColorPicker, TabBox, Accordion, Tree, Liste) mit bis zu 8 Einrückungsebenen, plus fünf Drag-Zweige; `resolveCanvas(w.tree, …)` wird ~12× inline neu berechnet, identische Rect→Pixel-Rect-Umrechnung (`pxr{ r.x*scaleX … }`) steht ~8× kopiert. Die Datei mischt zudem Instanzverwaltung, Menüleiste, Listen, Animation, Textbearbeitung, Drag&Drop, Fokus/Navigation und Zeichnen.
- **Code-Auszug**:
```cpp
const HE::UIWidgetRect pxr{ r.x * canvas.scaleX, r.y * canvas.scaleY,
                            r.w * canvas.scaleX, r.h * canvas.scaleY };
```
- **Lösung**: Hilfsfunktion `pixelRectOf(tree, e, canvas)`; `processPointer` in Teilfunktionen je Phase (`handleMenuBarPointer`, `handleDropdownPointer`, `updateHover`, `handlePress(w, hot, …)` mit je einer Funktion pro Widget-Typ, `handleDrags`) zerlegen; Datei in `WidgetManager_{Menu,Lists,Anim,Text,Drag,Focus,Draw}.cpp` aufteilen. Reine Verschiebung, keine Logikänderung.
- **Erwarteter Effekt**: Keine Laufzeitänderung; deutlich bessere Les-/Testbarkeit und Compile-Zeit (Teilneubauten).
- **Risiko / Verifikation**: Niedrig bei mechanischer Zerlegung; Absicherung durch die bestehende Test-Suite (`tests/test_ui_widgets.cpp`, Pointer-/Drag-Szenarien) vor und nach jedem Teilschritt. Nur nach CORE-701-703 sinnvoll, sonst Merge-Konflikte.

### CORE-716 – Editor-/Hilfsoperationen am Baum sind O(n²) (removeSubtree, Clipboard, Selection)
- **ID**: CORE-716
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIWidgetTree.cpp:21-26` (childrenOf allokiert), `:42-52` (removeSubtree), `:2563-2584` (uiSelectionRoots), `:2603-2621` (uiElementsToClipboard), `:2623-2671` (uiElementsFromClipboard `:2655-2661`)
- **Problem**: `removeSubtree` ruft pro Knoten `childrenOf` (voller Scan + neuer Vektor) und sucht danach pro Element linear in `doomed`; `uiElementsToClipboard` macht `std::find` in `take` pro Element; `uiSelectionRoots` O(k·Tiefe·n); `uiElementsFromClipboard` sucht in `remap` linear und ruft `tree.add`/`tree.find` (linear). Nur Editor, aber bei großen Seiten (Löschen/Duplizieren großer Teilbäume) O(n²).
- **Code-Auszug**:
```cpp
return std::find(doomed.begin(), doomed.end(), e->id) != doomed.end();
```
- **Lösung**: Einmal `parent → children`-Index (oder `unordered_set<int>` für `doomed`/`take`/`selected`) bauen; `remap` als `unordered_map<int,int>`.
- **Erwarteter Effekt**: Schätzung: O(n²) → O(n); spürbar erst bei >1000 Elementen.
- **Risiko / Verifikation**: Niedrig; Reihenfolge der Ergebnisse (Tree-Vektorreihenfolge) bleibt identisch. Vorhandene Designer-/Clipboard-Tests.

### CORE-717 – `UITextCatalog::find` sucht linear nach Sprache und Schlüssel
- **ID**: CORE-717
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIWidgetTree.cpp:2287-2315`, `:2373-2398` (`uiApplyTextCatalog`)
- **Problem**: `find` läuft pro gebundener Property durch alle Sprachen und alle Einträge der Sprache (String-Vergleich pro Eintrag); beim Sprachwechsel und (siehe CORE-704) pro Zeilen-Graft wird das für alle gebundenen Elemente wiederholt: O(Elemente·Katalogeinträge). Die Reihenfolge in den Vektoren muss für die Datei erhalten bleiben (Kommentar), aber das schließt einen zusätzlichen Index nicht aus.
- **Code-Auszug**:
```cpp
for (const auto& [k, text] : entries) if (k == key) return &text;
```
- **Lösung**: Pro Sprache zusätzlich `unordered_map<string,size_t>` (Index in `entries`) lazy aufbauen und bei `set()` pflegen; `languages` bleibt der Datei-Reihenfolge-Träger.
- **Erwarteter Effekt**: Schätzung: O(1) statt O(Katalog) je Lookup; relevant nur bei großen Katalogen (≥ tausend Keys).
- **Risiko / Verifikation**: Niedrig; Round-Trip-Test der Katalog-JSON (Reihenfolge) bleibt maßgeblich.

### CORE-718 – `focusNext`: rekursive `std::function`-Wanderung mit O(n)-`childrenOf` pro Knoten
- **ID**: CORE-718
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `WidgetManager.cpp:5923-5933` (und `#include <functional>` `:10`), `:5945-5951`
- **Problem**: Die Tab-Reihenfolge wird über eine rekursive `std::function` aufgebaut, die pro Knoten `tree.childrenOf` (voller Scan + Vektor-Allokation) und danach `find` ruft – O(n²) und n Allokationen pro Tab-Druck. Außerdem ruft jedes Kandidatenelement `isFocusable` → `isInteractive` (siehe CORE-707) und mehrere Kettenwanderungen.
- **Code-Auszug**:
```cpp
const std::function<void(int)> walk = [&](int parent)
{
    for (int child : w->tree.childrenOf(parent))
```
- **Lösung**: Mit dem `parent → Kinder`-Index (CORE-701) iterativ (expliziter Stack) durchlaufen; `std::function`-Include entfällt. Nutzereingabe-getrieben, daher niedrige Priorität.
- **Erwarteter Effekt**: Gering (Tastendruck), vor allem Vereinfachung.
- **Risiko / Verifikation**: Niedrig; Reihenfolge Depth-first, Eltern vor Kindern, muss identisch sein (vorhandene Tab-Order-Tests).

### CORE-719 – Unnötige Kopien in Konstruktions-/Design-Pfaden
- **ID**: CORE-719
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:254` (`const std::vector<std::string> chain = queue[qi].chain;`), `:501-502` (`HorizonCode::Graph graph` by value), `:608-625` (`w.tree = tree` + zusätzlicher `before`-Vollklon + Vergleich aller Properties per `getPropAny`), `:526` (`const std::string json = ta->json;`)
- **Problem**: `embedWidgetRefs` kopiert pro Ref die Kette (`vector<string>`); `runDesignTimePreConstruct` kopiert die ganze Baumstruktur zweimal (tiefe `clone()` je Element) und vergleicht anschließend jede Property aller Elemente über string-basierte `getPropAny`; `registerInstance` nimmt den Graph by value (Aufrufer mit `const&` kopieren ihn). Die Kopien an manchen Stellen (`:526`, `:254`) sind aus Reallokations-Sicherheitsgründen kommentiert und teils nötig; nur die Designer-Variante ist wirklich vermeidbar.
- **Code-Auszug**:
```cpp
w.tree = tree;
registerInstance(content, w, graph);
...
const HE::UIWidgetTree before = m_instances.back().tree;
```
- **Lösung**: `chain` als Index/`shared_ptr<const Chain>` bzw. verkettete Liste (Eltern-Zeiger) statt Kopie je Queue-Eintrag; im Designer-Lauf `before` als kompakte Liste der Property-Werte statt Vollklon speichern oder die Änderungen per Dirty-Markierung am Setzen erfassen.
- **Erwarteter Effekt**: Klein (Designer-Preview, Widget-Erzeugung); nur bei sehr großen Widgets relevant.
- **Risiko / Verifikation**: Niedrig; Designer-Preview-Tests (`runDesignTimePreConstruct`-Schreibliste muss identisch bleiben).

## Gelesen

Ja, alle sechs Dateien vollständig gelesen: `UIWidgetBinding.h` (16 Z.), `UIWidgetTree.h` (675), `UIWidgetTree.cpp` (2788), `UIWindowFrame.h` (93), `UIWindowFrame.cpp` (156), `WidgetManager.cpp` (6810, in Blöcken). Zusätzlich gezielt (nur per grep/kurzem Ausschnitt) `GraphModel.h`, `UIElement.h`, `UIElements.h`, `WidgetManager.h`, `HorizonCodeRuntime.cpp` (`eventBindingsOf`) und die Aufrufstellen in `GameApplication.cpp`/`EditorApplication.cpp`. `UIWindowFrame.*` und `UIWidgetBinding.h` ohne Befund.

## Core Slice 8 – UIWidget/WidgetManager.h, Window/Window.cpp, Window/Window.h

## Zusammenfassung
Der Slice besteht aus dem Header der Widget-Verwaltung (1441 Zeilen, fast nur Deklarationen und Kommentare), der Window-Implementierung (521 Zeilen) und dem Window-Header. Der Code ist sauber und gut begründet kommentiert; die Window-Datei enthält keinen Hot-Path-Code außer `PollEvents`, `IsInBackground` und dem Hit-Test-Callback, alle drei sind unkritisch. Die relevanten Befunde betreffen (1) die Header-Abhängigkeiten von `WidgetManager.h` und `Window.h` (Compile-Zeit), (2) die Datenhaltung `std::vector<Instance>` mit linearer Suche und (3) duplizierte Env-Parsing-Logik in `Window.cpp`. Gesamt: 0 Hoch, 3 Mittel, 5 Niedrig. Alle Performance-Aussagen sind ohne Messdaten Schätzungen. Die Implementierung `WidgetManager.cpp` liegt nicht in diesem Slice; Aussagen dazu sind nur Stichproben (grep) und als solche markiert.

## Funde

### CORE-801 – `WidgetManager.h` zieht die komplette Widget-/HorizonCode-Welt in jeden Includer
- **ID**: CORE-801
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Performance (Build-Zeit) / Vereinfachung
- **Ort**: src/HE_Core/include/UIWidget/WidgetManager.h:2-16; Includer u.a. src/HE_Scene/include/HorizonScene/HorizonWorld.h:11, HorizonScene.h:57
- **Problem**: Der Header inkludiert sieben schwere Projekt-Header (`UIWidgetTree`, `UIElements`, `UIWidgetBinding`, `AppMenu`, `UIWindowFrame`, `HorizonCode`, `HorizonCodeRuntime`, `UIRenderObject`) und `<unordered_map>`. Er wird von `HorizonWorld.h` und `HorizonScene.h` eingebunden, also transitiv von praktisch jeder Datei in HE_Scene/Editor/Game. Die Klasse braucht die vollständigen Typen nur, weil `Instance`, `StateSnapshot`, `m_menuBar`, `m_theme`, `m_ownRuntime` (Wert-Member `HorizonCode::Runtime`, Z. 1317) und `m_catalog` inline im Header stehen. Das Kommentar-Muster aus `Window.h:13-17` („Declared, not included“) zeigt, dass das Problem im Haus bekannt ist.
- **Code-Auszug**:
```cpp
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElements.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
...
HorizonCode::Runtime  m_ownRuntime;        // fallback when none is injected
```
- **Lösung**: Pimpl-Light: `struct Instance`, `m_grabs`, `m_columnInfo`, Drag/Tooltip-Zustand und `m_ownRuntime` in `struct WidgetManager::Impl` (nur in der .cpp) verlagern; im Header nur öffentliche Typen behalten (`StateSnapshot`, `DesignTimeRun`, `TextEdit`, `NavDir`). `StateSnapshot` braucht `HorizonCode::Value` und `UIPropValue`; diese zwei Header bleiben, der Rest entfällt. Alternativ erst die einfache Stufe: in `HorizonWorld.h` nur `class WidgetManager;` vorwärts deklarieren und `std::unique_ptr<WidgetManager>` halten (nur sinnvoll, wenn `HorizonWorld` den Member nicht inline braucht; prüfen).
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Inkremental-Builds bei Änderungen an `UIWidgetTree.h`/`HorizonCode*.h` (derzeit rebuilden alle Scene/Editor-TUs). Konkrete Zahl nur per `-ftime-trace` / `ClangBuildAnalyzer` messbar.
- **Risiko / Verifikation**: Mittel – `HorizonWorld` und Tests verwenden `WidgetManager` per Wert; Pimpl ändert Move-/Copy-Verhalten (Kommentar Z. 1027 „stays movable“ beachten, d.h. Move-Ctor/Assign explizit in der .cpp definieren). Verifikation: voller Build aller Targets (Editor, Game, `he_tests`), `test_ui_widgets`, `test_horizoncode_*`. Vorher mit `-ftime-trace` messen, ob sich der Aufwand lohnt.

### CORE-802 – Widget-Instanzen als `std::vector<Instance>` mit Wert-Semantik und linearer ID-Suche
- **ID**: CORE-802
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Core/include/UIWidget/WidgetManager.h:863-1021 (`Instance`), 1023-1025, 1312; Gegenstück src/HE_Core/src/UIWidget/WidgetManager.cpp:32-47, 948-958 (Stichprobe, nicht Teil des Slice)
- **Problem**: `Instance` ist ein sehr großer Wert-Typ (eigener `UIWidgetTree`, vier `unordered_map`, drei `vector`, mehrere `std::string`) und liegt direkt in `std::vector<Instance> m_instances`. Jeder `push_back` mit Reallokation und jedes `erase` (destroyWidget, destroyWidgetsOfWindow) verschiebt/kopiert ganze Bäume und macht alle `Instance*` ungültig – der Header muss dafür an mehreren Stellen Latch-Konstrukte begründen (`m_syncingLists`, Z. 1120-1132, Use-after-free-Szenario). `find(int id)` und `findByScript()` sind lineare Scans; `grep` zeigt 169 `find(`-Treffer in `WidgetManager.cpp`, darunter `takesInput` → `topGrabOf` → `find` pro Grab pro Eingabe-Eintrittspunkt (`topGrabOf` liest `find()` per `const_cast` im Schleifenkörper). Bei den realistischen Instanzzahlen (einige bis ~dutzend; List-Rows sind aber eigene Embeds/Skript-Instanzen, keine Instanzen) ist der Scan billig – das ist ein Robustheits- und Skalierungsproblem, kein bewiesener Engpass.
- **Code-Auszug**:
```cpp
std::vector<Instance> m_instances;          // Z. 1312
Instance* find(int id);                     // Z. 1023, linear in .cpp:34
```
- **Lösung**: `std::vector<std::unique_ptr<Instance>>` (oder `std::deque<Instance>`): stabile Adressen, `erase` verschiebt nur Zeiger, die Re-Entrancy-Gefahr bei `createWidget` aus einem Graph-Callback sinkt. `find` bleibt linear (Instanzen sind sortiert nach Erzeugung und `id` wird monoton vergeben – dann reicht `std::lower_bound` auf `id`, falls IDs nie wiederverwendet werden; verifizieren). Zusätzlich `const_cast` in `topGrabOf` durch eine const-Überladung/`findConst` ersetzen (Vereinfachung).
- **Erwarteter Effekt**: Schätzung: geringe Frame-Zeit-Änderung; Hauptgewinn ist Wegfall von Dangling-Pointer-Klassen (Instance* über Skript-Callbacks hinweg). Ohne Messung keine Zeitangabe.
- **Risiko / Verifikation**: Mittel. Code, der `Instance*` über mögliche `push_back`s hält, kann sich bisher implizit auf „kein Reallokieren“ verlassen oder wird heute schon durch Latches geschützt; Umstellung ändert Reihenfolge nicht. Absicherung: bestehende `test_ui_widgets` (nur gezielt lesen), ASAN-Lauf mit List-/Modal-Tests, Profiling mit 50+ Widgets vor dem Umbau.

### CORE-803 – Env-Auswertung in `Window.cpp` ist fünffach dupliziert, inklusive nachträglicher „Ursachen-Rekonstruktion“ für das Log
- **ID**: CORE-803
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Core/src/Window/Window.cpp:50-145 (bes. 59-61, 78-80, 95-101, 130-134)
- **Problem**: Die Regel „Zahl ≠ 0“ (`*s && std::strtoull(s,nullptr,10) != 0`) steht an fünf Stellen. Die beiden Lambdas in `hiddenWindowRequested()` und `backgroundFrameInterval()` rechnen dieselbe Entscheidung für den Log-Text ein zweites Mal aus (verschachtelte `?:`, Z. 78-80 und 130-134). Entscheidungsfunktion und Log-Begründung können auseinanderlaufen, sobald eine Bedingung ergänzt wird. Zudem ist `hiddenWindowFromEnv` mit drei positionellen `const char*`-Parametern gleichen Typs fehleranfällig (Vertauschung kompiliert).
- **Code-Auszug**:
```cpp
(hw && *hw) ? "HE_HIDDEN_WINDOW"
: (ex && *ex && std::strtoull(ex, nullptr, 10) != 0)
    ? "HE_EXIT_AFTER_FRAMES" : "HE_DUMP_PATH");
```
- **Lösung**: Kleine anonyme Hilfe `bool nonZeroNumber(const char*)` und die Entscheidungsfunktionen mit einem optionalen `const char** why`-Out-Parameter bzw. einem Rückgabe-Struct `{bool value; const char* reason;}` versehen; die Lambdas loggen nur noch `result.reason`. Die öffentlichen Signaturen (`HE_API`, im Test verwendet) bleiben als dünne Wrapper unverändert.
- **Erwarteter Effekt**: Weniger Code (~15-20 Zeilen), keine doppelte Wahrheit. Laufzeit irrelevant (einmaliger Start).
- **Risiko / Verifikation**: Niedrig. Verhalten identisch; Tests, die `hiddenWindowFromEnv`/`backgroundFrameIntervalFromEnv` aufrufen (grep in `tests/`), müssen unverändert grün bleiben.

### CORE-804 – `Window.h` zieht `<SDL3/SDL_events.h>` und `<functional>` in jeden Application-Includer
- **ID**: CORE-804
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Performance (Build-Zeit)
- **Ort**: src/HE_Core/include/Window/Window.h:6-7, 191; Includer: Application/Application.h:8 (13 Dateien nutzen `Window/Window.h` direkt, Application.h verbreitet es weiter)
- **Problem**: `SDL_events.h` wird nur für den `EventCallback`-Typ `std::function<void(const SDL_Event&)>` benötigt. Eine Vorwärtsdeklaration reicht (`union SDL_Event;`), `std::function` mit unvollständigem Parametertyp als Referenz ist zulässig; die Definition des Aufrufs steht in der .cpp. Das Fenster-Header-Kommentar zu `UIWindowHit` (Z. 13-17) wendet genau diese Technik bereits an.
- **Code-Auszug**:
```cpp
#include <SDL3/SDL_events.h>
...
using EventCallback = std::function<void(const SDL_Event&)>;
```
- **Lösung**: `union SDL_Event;` vorwärts deklarieren, Include in `Window.cpp` verschieben; Aufrufer, die `SDL_Event` selbst benutzen (Application.cpp, Editor), inkludieren SDL selbst. `SetEventCallback` ist inline mit `std::move` – braucht `<utility>`.
- **Erwarteter Effekt**: Schätzung: kleiner, nur per Build-Trace messbar.
- **Risiko / Verifikation**: Niedrig, aber Folgefehler durch transitiv genutzte SDL-Typen in Aufrufern möglich (Compile-Fehler, kein Laufzeitrisiko). Voller Build prüft das.

### CORE-805 – `PollEvents` ruft pro Event bis zu dreimal `SDL_GetWindowID(m_window)` auf
- **ID**: CORE-805
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance / Vereinfachung
- **Ort**: src/HE_Core/src/Window/Window.cpp:345-380
- **Problem**: In der Event-Schleife (Hot Path Input, einmal pro Frame, bei Mausbewegung viele Events) wird die Fenster-ID bis zu zweimal pro Event neu abgefragt, und die beiden Blöcke prüfen getrennt `event.type`. Die ID ist über die Lebenszeit des Fensters konstant. Zusätzlich sind die zwei `if`-Ketten für QUIT/CLOSE_REQUESTED und die Größen-Events eine einzige `switch (event.type)`-Struktur.
- **Code-Auszug**:
```cpp
if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
    event.window.windowID == SDL_GetWindowID(m_window))
...
event.window.windowID == SDL_GetWindowID(m_window) &&
```
- **Lösung**: `const SDL_WindowID myId = SDL_GetWindowID(m_window);` einmal vor der Schleife (oder als Member in `Init`), dann `switch (event.type)` mit drei Fällen.
- **Erwarteter Effekt**: Schätzung: unmessbar klein (SDL_GetWindowID ist ein Zeiger-Lookup mit Parameter-Check); Nutzen ist Lesbarkeit.
- **Risiko / Verifikation**: Niedrig. Verhalten identisch.

### CORE-806 – Doppelter `SDL_GetDisplayForWindow`/`SDL_GetDisplayName`-Aufruf und mehrfache Primary-Display-Abfragen in `Init`
- **ID**: CORE-806
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Core/src/Window/Window.cpp:236-251, 296-297
- **Problem**: `SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window))` wird zweimal in einem Ausdruck ausgewertet (Prüfung und Wert); `SDL_GetPrimaryDisplay()` wird zweimal aufgerufen. Einmaliger Start, daher nur Lesbarkeit.
- **Code-Auszug**:
```cpp
SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window))
    ? SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window)) : "?");
```
- **Lösung**: `const char* dn = SDL_GetDisplayName(...); ... dn ? dn : "?"`; `const SDL_DisplayID primary = SDL_GetPrimaryDisplay();`.
- **Erwarteter Effekt**: Lesbarkeit; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Niedrig.

### CORE-807 – `WidgetManager.h`: Die Kommentarmasse überwiegt den Code und enthält Verwaiste/falsch platzierte Kommentare
- **ID**: CORE-807
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Core/include/UIWidget/WidgetManager.h:1266-1275, 1325-1330, 1137-1141
- **Problem**: Mehrere Kommentare stehen an der falschen Deklaration: der Tooltip-Block (Z. 1266-1274) beschreibt `m_tooltip*`, steht aber über `drawOpenDropdown`; „Starts true: the first frame…“ (Z. 1325-1327) steht über `m_theme`, gehört zu `m_visualDirty` (Z. 1345); der Tooltip-Kommentar nennt die Member erst 30 Zeilen später. Das ist reines Wartungsrisiko (der Code-Stil „erkläre das Warum“ ist ansonsten gut), der Header ist 1441 Zeilen lang und damit kaum zu überblicken. `liveIds()` (Z. 333) liefert eine neue `std::vector<int>` pro Aufruf; der Outliner ruft sie laut Kommentar pro Frame – siehe Messhinweis unten.
- **Code-Auszug**:
```cpp
// Starts true: the first frame after anything is created has never been
// drawn, so "nothing changed since the last draw" is false by construction.
// What roles resolve to. A copy rather than a pointer: ...
HE::UITheme           m_theme = HE::uiDefaultTheme();
```
- **Lösung**: Kommentare zu den zugehörigen Membern verschieben; den Header entlang der vorhandenen Abschnitte (Menu-Bar, Drag, Tooltips, Listen) in private Teil-Header oder in die Impl (CORE-801) aufteilen.
- **Erwarteter Effekt**: Wartbarkeit.
- **Risiko / Verifikation**: Keines (nur Kommentare).

### CORE-808 – `std::function` als Hit-Test-Callback, der laut Vertrag pro Mausbewegung im Event-Pump läuft
- **ID**: CORE-808
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Core/include/Window/Window.h:149-159; src/HE_Core/src/Window/Window.cpp:488-501; Verwendung src/HE_Game/src/GameApplication.cpp:716
- **Problem**: Der Hit-Test wird von der OS pro Mausbewegung aufgerufen; Pfad: C-Lambda → `std::function` → Lambda `[this]` → `frameHitAt`. Der Vertrag („must not allocate, log, or take a lock“) ist nur Kommentar. Der Mehraufwand eines `std::function`-Aufrufs ist im Vergleich zum Hit-Test selbst klein; das Ganze betrifft nur Borderless-Fenster. Der `(SDL_HitTestResult)(int)`-Cast ist durch die `static_assert`s (Z. 477-486) abgesichert. Kein echtes Problem, nur „unsicher / messen“.
- **Code-Auszug**:
```cpp
using HitTestCallback = std::function<UIWindowHit(int x, int y)>;
return (SDL_HitTestResult)(int)self->m_hitTest(area->x, area->y);
```
- **Lösung**: Nur falls ein Profil es zeigt: Funktionszeiger + `void* user` (wie SDL selbst) statt `std::function`. Sonst belassen.
- **Erwarteter Effekt**: Schätzung: nicht messbar.
- **Risiko / Verifikation**: Niedrig; ändert öffentliche API (`GameApplication.cpp:716` und evtl. Editor). Nicht empfohlen ohne Messdaten.

## Gelesen
Alle drei Dateien vollständig gelesen: ja (WidgetManager.h 1441 Zeilen, Window.cpp 521, Window.h 211); `WidgetManager.cpp` und `Application.cpp` nur per gezieltem Grep/Ausschnitt.
