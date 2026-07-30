# Codebase-Audit für den Rework (Stand 30.07.2026)

Vollständiges Review aller 6 Module (~410k Zeilen First-Party-Code) auf Benennung,
Kommentare, Redundanz und Vereinfachungspotenzial. Gesamteindruck: Die Codebase ist
für ihre Größe ungewöhnlich gut kommentiert (viele „Warum"-Kommentare mit
Bugfix-Historie, kaum toter auskommentierter Code, keine TODO-Leichen). Die
Hauptprobleme sind (a) einige **echte Funktionsbugs**, die beim Review aufgefallen
sind, (b) **systematische Duplikation** — vor allem zwischen den 5 Renderer-Backends,
zwischen Game-/EditorApplication und zwischen den 5 Graph-Systemen — und (c) ein paar
**Monolith-Dateien** (EditorUI.cpp, RenderExtractor::extract, exportProject).

---

## 0. Echte Bugs / Funktionsdefekte (zuerst fixen, unabhängig vom Rework)

1. **Hot-Reload für 4 Asset-Typen still defekt** — `ContentManager::unloadAsset`
   (ContentManager.cpp:1465–1473) kennt die SlotMaps `m_inputActionAssets`,
   `m_inputMappingAssets`, `m_particleGraphAssets`, `m_animatorStateMachineAssets`
   nicht → `unloadAsset` liefert `false`, `pollHotReload` lädt nie neu.
2. **Collision-Exit-Events feuern nie** — `HEContactListener::OnContactRemoved`
   (PhysicsWorld.cpp:125–131) ist ein leerer Stub; `pollCollisionExit()` liefert immer
   `{}`. Der komplette `on_collision_exit`-Pfad in allen 3 Script-Backends ist
   unerreichbar. Implementieren oder ehrlich als „not implemented" markieren.
3. **Datenverlust-Risiko im Editor** — Particle- und AnimatorSM-Panel exportieren kein
   `isDirty()`; die Unsaved-Changes-Prüfung beim Tab-Schließen/Editor-Exit übergeht
   sie → ungespeicherte Graph-Edits gehen kommentarlos verloren. Außerdem fehlt
   `forget()` für Particle/ASM/SkeletalMesh in `forgetTabState`
   (EditorUI.cpp:3723–3739) → State-Leak pro geöffnetem Asset.
4. **UI-Doppel-Emission bei >1 Canvas** — `UISystem::extract` (UISystem.cpp:96–182)
   iteriert pro Canvas ALLE UIElemente ohne Membership-Check → bei 2 aktiven Canvases
   wird jedes Element doppelt gezeichnet.
5. **AudioEngine ignoriert `sampleRate`** — `play()`/`playSpatial()` validieren den
   Parameter, setzen ihn aber nie in die `ma_audio_buffer_config` → 44,1-kHz-Assets
   spielen ~9 % zu schnell (falls der Importer nicht alles auf 48 kHz resampled).
6. **`Window::GetWidth/GetHeight` veralten nach User-Resize** — `PollEvents`
   behandelt `SDL_EVENT_WINDOW_RESIZED` nicht (Window.cpp:82–95); die Getter bleiben
   beim Startwert. Backends retten sich per `SDL_GetWindowSizeInPixels`, aber die API
   lügt.
7. **Wahrscheinlicher Transpose-Bug im SkeletalMeshImporter** —
   `SkeletalMeshImporter.cpp:91–93` transponiert glTF-Node-Matrizen, der
   MeshImporter (MeshImporter.cpp:217–218) nicht. `cgltf_node_transform_world` liefert
   column-major, was `glm::make_mat4` direkt erwartet → die Transpose ist vermutlich
   falsch (wirkt nur auf nicht-geskinnte Nodes in Skelett-Dateien).
8. **Integer-Overflow-Lücken im .hasset-Parsing** — `HAsset.h:363` (`readVec` POD:
   `count * sizeof(T)` kann wrappen) und `Reader::open()` (HAsset.h:252–280, kein
   Size-Check vor `resize(ch.size)` — die String-Variante und `openData()` haben die
   Guards, die POD-/Disk-Varianten nicht).
9. **`getCurrentOS()`-Default falsch** — `readConfig` setzt auf allen Plattformen
   `OS::Windows` und persistiert das (GlobalState.cpp:72/97/118); Funktion hat zudem
   null Aufrufer.
10. **Editor-Typ-Cache nie invalidiert** — die `s_typeCache`-Statics der Panels
    (z.B. ParticleGraphEditorPanel.cpp:76, MaterialEditorPanel.cpp:1312) werden nie
    geleert; Asset löschen + Pfad mit anderem Typ neu belegen → Doppelklick öffnet den
    falschen Editor.
11. **Content-Folder-Baum nicht exception-sicher** — `Folder`/`File` besitzen rohe
    Kind-Zeiger ohne Destruktor; `populateFolder` (GlobalState.cpp:236–264) nutzt den
    werfenden `fs::directory_iterator` → Permission-Error leakt den halben Baum und
    crasht ggf. den Editor. Fix: `directory_iterator(path, ec)` + Ownership.
12. **Uncommitted Diff: `Content/Scenes` doppelt** — ProjectManager.cpp:584 + :589
    erzeugen den Ordner zweimal; neue Zeilen haben falsche Doppel-Tab-Einrückung.
    Vor dem Commit bereinigen.

---

## 1. Die großen Redundanz-Hotspots (Rework-Kern)

### 1a. Fünf Renderer-Backends (~43k Zeilen)

Cascade-Fitting, Licht-Extraktion, Culling, Sortierung liegen bereits in der
gemeinsamen Schicht — die verbleibende Redundanz ist CPU-seitige
Parameter-Aufbereitung + eingebettete Shader-Quellen:

- **D3D11 ↔ D3D12: ~850 identische HLSL-Zeilen** eingebettet (Sky, Scene-VS/PS, HBAO,
  UI, GI). → gemeinsamer `D3D_Shared/HlslSources.h`. Größter Einzelgewinn, rein
  mechanisch.
- **5× byte-identische C++-Helfer**, je einmal pro Backend:
  - SSAO-Kernel/Noise (`BuildSSAOKernel/Noise`, identische LCG-Konstanten) — GL:4442,
    Metal:4027, Vulkan:149, D3D11:1694, D3D12:1673 → `HE_Rendering/src/SsaoKernel.{h,cpp}`
  - 3D-Wolken-Noise `BuildSkyNoise3D` (GL:34, Metal:154, Vulkan:39, D3D11:47, D3D12:71)
  - Dominant-Light-Auswahl (4 Kopien mit 4 Namen: `glDominantDirectionalLight`,
    `dominantDirectionalLight`, `vkDominant…`, `d3d12Dominant…`) → `RenderWorld`-Methode
  - 8-Licht-Uniform-Packing (GL:5119, Vulkan:6486, D3D12:5625, Metal:5459) →
    gemeinsames `PackedLightArray`, einmal pro Frame im Extractor gebaut
  - Wind-Vektor `sin/cos(dir)*speed*0.025` an 6 Stellen inkl. kopiertem Kommentar →
    `HE::CloudWindVector(EnvironmentSettings)`
  - GPU-Wetter-Partikel-Seeding (GL:8130, Metal:9400, nur Lane-Reihenfolge differiert)
- **Sky-Uniform-Befüllung**: jedes Backend übersetzt dieselben ~25 Env-Felder in seine
  Sky-CB → backend-agnostisches `SkyFrameParams`-POD (std140), Backends kopieren 1:1.
  Erschlägt zugleich die Metal-interne Doppelung: `SkyParams` wird 2× befüllt
  (EncodeCloudPrepass :6835 vs. EncodeSky :7744) mit „MUST stay identical"-Kommentar —
  und ist bereits divergiert (`star2.zw`).
- **OpenGL intern**: der 8-Licht-Block inkl. CSM-Binding ist 3× copy-pasted in
  `DrawScene` (unlit :7096, instanced :7164, skinned :7593) mit sichtbarem
  Einrückungs-Drift → Helper `bindSceneLighting(...)`.
- **Weitere**: GPU-Timer-Ringpuffer 4× (Template-Kandidat), Transparenz-Partition +
  Back-to-Front-Sort 5× in zwei Idiomen (→ RenderSorter), `kD3DClipFix` 2× wörtlich
  (→ `ClipSpace.h`), Sky-IBL-Cube-Bake GL/Metal doppelt.
- **Tote Pfade**: Metal `EnsureShadercDemoPipeline`/`EnsureShadercTestMesh` (~350
  Zeilen env-var-gated PoC, produktiv abgelöst) → löschen; GL
  `m_lastInvViewProj`/`m_lastSunDir` geschrieben, nie gelesen → löschen.
- **Shader-Drift GL↔Vulkan**: `shaders/sky.frag`/`scene.frag` sind reduzierte
  Zweitfassungen der GL-embedded-Shader; Vulkan hinkt hinterher (keine Aurora, kein
  hgPhase). Mindestens dokumentieren, besser aus einer Quelle einbetten.
- **Benennung**: Casing-Split PascalCase (GL/Metal) vs. camelCase (Vulkan/D3D12);
  „Gi" vs. „GI"; gleiche Konzepte verschieden benannt (`EnsureCloudFBO` vs.
  `EnsureCloudTarget`, `DispatchGiProbeUpdate` vs. `EncodeGIProbeUpdate`). Konvention
  festlegen (PascalCase, „GI").
- **Stale Kommentar**: `RenderWorld.h:30–35` behauptet, GL sei noch Single-Shadow-Map —
  GL hat längst CSM; nur D3D11/D3D12/Vulkan sind Single-Map.

### 1b. GameApplication ↔ EditorApplication

- `pushEngineInputSnapshot()` **byte-identisch** (GameApplication.cpp:837 vs.
  EditorApplication.cpp:974) → einmal neben `HE::api::input`.
- **EnvironmentSettings-Push-Block** (~45 Zeilen inkl. Day/Night-Auto-Advance) doppelt
  (GameApplication.cpp:946 vs. EditorApplication.cpp:2820) — Feldreihenfolge divergiert
  bereits → `makeEnvironmentSettings(EnvironmentComponent&, float dt)` in HE_Scene.
  Höchste Drift-Gefahr im ganzen Audit.
- **Free-Fly-Kamera** doppelt (GameApplication.cpp:721 vs. EditorApplication.cpp:2464)
  → gemeinsamer `FlyCameraController`.
- **Script-Start-Schleife dreifach** (GameApplication.cpp:596 + :625,
  EditorApplication.cpp:2628) → Helper an `ScriptContext`.

### 1c. Fünf Graph-Systeme (HorizonCode / Material / Particle / AnimatorSM / UIWidgetTree)

- **Graph-Container-Muster 5× implementiert** (`findNode`/`addNode`/`connect`/
  `disconnectInput`/`removeNode`: HorizonCode.cpp:530–677, MaterialGraph.cpp:360–417,
  ParticleGraph.cpp:95–139, UIWidgetTree.cpp:9–66) → gemeinsames
  `HE::GraphModel<NodeT, LinkT>`-Template; spart ~150 Zeilen und garantiert identische
  Link-Semantik.
- **JSON-Serialisierungs-Gerüst 5×** (parse-Guard, `nextId`-Reparatur, Link-Format) —
  dabei inkonsistent: pretty vs. kompakt, Link als Array vs. Objekt → gemeinsame
  Helfer; Formatdivergenz mindestens dokumentieren.
- **Serialisierung über Display-Namen** (HorizonCode, `"Array Add"`-Legacy-Alias zeigt
  das Problem) → stabile `serialName`s oder datengetriebene Alias-Map; Kommentar an
  `nodeDisplayName`, dass die Strings On-Disk-Format sind.
- **Koerzierungs-Regeln 3×** (HorizonCode.cpp:1015, HorizonCodeGenSupport.h:113
  [bewusster Parity-Contract, ok], UIWidgetBinding.cpp:42 [sollte auf HC-Koerzierung
  aufsetzen — Array-Passthrough fehlt dort schon]).

### 1d. Editor: HorizonCode-Canvas-Host doppelt (~600+ Zeilen)

`LevelScriptPanel.cpp` und `UIEditorPanel.cpp` duplizieren den kompletten
GraphEditor-Host nahezu 1:1 (per Diff verifiziert): `loadClassGraph`/
`resolveClassGraph` unterscheiden sich nur in Variablennamen; `drawCanvas` (~515 Z.)
vs. `drawGraphCanvas` (~575 Z.) sind parallel, dazu `uniqueFunctionName`,
`uniqueVarName`, `pinTypeName`, `addNode`, `drawVariables`, `drawNodeDetails` je
doppelt. Bereits leicht divergiert. → gemeinsamer `HcGraphHost` mit kleiner
Hook-Struktur für die echten Unterschiede. (Die Adapter der Material-/Particle-/
ASM-Panels sind dagegen wirklich dünn — die GraphEditor-Abstraktion funktioniert.)

### 1e. Wiederholte ImGui-Muster im Editor

- **`HE_ASSET_PATH`-Drop-Slot ~20× kopiert** (EditorUI.cpp 13 Stellen + 5 Panels) →
  `EditorWidgets::assetDropSlot(ctx, label, UUID& target, AssetType want)`; spart
  300–400 Zeilen und vereinheitlicht Undo-Snapshots (fehlen z.T. in den Graph-Panels).
- **Panel-State-Boilerplate 9×** (`g_states` + `stateFor` + Typ-Cache + `forget`) →
  Template `AssetPanelState<T>` + zentraler invalidierbarer `assetTypeOf(path)`-Cache
  (fixt zugleich Bug 10).
- `pushWidgetScale`/`popWidgetScale` 3× wortgleich → nach `GraphEditor.h`.

---

## 2. Monolithen aufteilen

- **EditorUI.cpp (8148 Z.)**: `RenderEditor` läuft von Z. 2066–6589, `RenderInspector`
  bis 8148. Die Sektionen sind durch Banner bereits markiert und kommunizieren fast nur
  über `AppContext&` — natürlicher Split: `ExportDialogPanel.cpp` (~1400 Z. inkl.
  Worker-Thread, klarster Kandidat), `ViewportPanel.cpp`, `TerrainTools.cpp`,
  `OutlinerPanel.cpp`, `ContentBrowserPanel.cpp` (~1200 Z.), `InspectorPanel.cpp`,
  `ProjectHubPanel.cpp`; die statischen `Draw*Window`-Funktionen sind trivial
  verschiebbar. Konvention wie bestehende Panels: `namespace XPanel { void render(AppContext&); }`.
- **RenderExtractor::extract() (640 Z., RenderExtractor.cpp:59–703)**: 8 klar trennbare
  Phasen → private Helfer (`extractCamera`, `extractMeshes`, `fitCascades`,
  `assignLocalShadowLayers`, …); die CSM-Sphere-Fit-Mathematik als freie, testbare
  Funktion. Dazu zwei Formatierungsunfälle (nicht eingerückter else-Block Z. 90–113,
  Klammer auf Spalte 0 in Z. 702).
- **ProjectExporter::exportProject (~400 Z.)**: Phasen sind schon per Kommentar-Banner
  markiert → als Funktionen ausformen.
- **ProjectManager.cpp**: ~350 Zeilen C++-Scaffold-Template-Strings (Z. 164–509) in
  eigene Datei `CppScaffoldTemplates.cpp` auslagern.
- **SceneSerializer.cpp**: jedes Component-Feld doppelt getippt (serialize Z. 151–563
  ↔ apply Z. 617–1170); für Environment (~50 Felder × 2) existiert bereits die
  X-Macro-Liste `HE_ENV_FIELDS_*` in EngineApi.h:150–206 — wiederverwenden. Außerdem
  Hierarchie-Rebuild 3× copy-paste (Z. 1218/1290/1413) → `rebuildHierarchy(...)`-Helper.

---

## 3. Toter Code (verifiziert: repo-weit keine Aufrufer)

Löschkandidaten:

- `HE_Rendering/include/ViewportUI/UIManager.{h,cpp}` — nie benutzt, nie implementiert
  (Methoden deklariert ohne Definition); + CMakeLists.txt:258.
- `HE_Rendering/include/HorizonRendering/IRenderDevice.h` — kein Backend implementiert
  es; nur der Umbrella-Header referenziert es.
- `HE_Rendering/include/IRenderer.h` + `src/IRenderer.cpp` — Kompat-Shims, niemand
  inkludiert sie mehr; + CMakeLists.txt:276.
- Eingecheckte `.spv`-Binaries in `shaders/` (5 Stück, nachweislich älter als ihre
  Quellen; Build kompiliert selbst) → löschen + `.gitignore`.
- `HE::ScriptLanguage {Lua, Python, CSharp}` in Enums.h:135 — toter Doppel-Enum, der
  echte ist `::ScriptLanguage` in ScriptTypes.h; Bug-Magnet.
- `Input`-Callback-Maschinerie (Input.h:13–55, `OnKeyDown` etc., Event-Structs,
  `IsMouseButtonDown`, `GetMousePosition`) — nur `ProcessEvent` + `IsKeyDown` werden
  genutzt.
- `SceneGraph::setParent/detach/propagateTransforms` — nirgends aufgerufen; dupliziert
  `HorizonWorld::reparentEntity` ohne dessen Cycle-Guards (gefährlich falls je
  benutzt); HierarchyComponent.h:7-Kommentar behauptet fälschlich, es liefe jeden Frame.
- `MaterialShaderLibrary::standardVertexGlsl()` — toter Code mit falschem
  Header-Kommentar.
- `shader_compiler`-CLI (HE_Tools/ShaderCompiler/main.cpp) — kein Aufrufer, shellt
  unsicher per `std::system` zu glslc; Engine kompiliert längst in-process.
- `AnimationClipImporter` — null Aufrufer (nicht mal Tests); entweder in
  `asset_compiler` verdrahten (der kennt nur 4 von 7 Importern — .gltf mit Skin wird
  kommentarlos als statisches Mesh importiert!) oder entfernen.
- `WindowState`-Enum + `WindowProps::state`, `OS`-Enum + `getCurrentOS`,
  `EngineStatus::isRunning`, `Logger::logfile`, `GameLoop::vsync/running()/requestStop`,
  `EngineProfiler::requestToggle/lastDeltaMs`, `UIRenderObject::text/fontSize`
  (Legacy-Typ-1), `HcCodegen Result::ok` (immer true; Fehlerzweig im CLI unerreichbar),
  `ApiFn::cppCall` (~300 handgepflegte Strings, nur ein Test prüft non-null; Doku
  behauptet fälschlich Codegen-Nutzung — dazu showZone/hideZone-Signatur-Falle),
  leere Redirect-TUs `Packer/HpakWriter.cpp`/`KeyDerivation.cpp`.
- Grenzfälle (nur Tests als Nutzer, entscheiden: behalten als Zukunfts-API + Kommentar,
  oder weg): `EventBus`, `Ref<T>/RefCounted`, `RenderResourceManager` +
  `GPUMemoryAllocator` (inkl. immer-invalider `meshHandle`/`materialHandle`-Felder in
  RenderObject/DrawCall), `ContentManager::loadPak` (Legacy neben `mountPak`),
  8 leere Platzhalter-Header + 6 Ein-Zeilen-.cpp-Stubs (Platform/Memory/Math/Types).

---

## 4. Irreführende / veraltete Kommentare (aktiv gefährlich)

- **HpakFormat.h:33/56/116 + HpakWriter.h:79**: behaupten „XOR-Verschlüsselung,
  AES geplant" — AES-256-GCM ist längst aktiv. Gleicher Fehler in
  Packer/main.cpp:4–6 (nennt auch die falsche API `loadPak` statt `mountPak`).
  Krypto-Kommentare, die den falschen Algorithmus nennen → sofort korrigieren.
- **MaterialGraph.h:220–225**: nennt die v1-Pin-Indizes des Output-Nodes — seit dem
  Specular/AO-Insert falsch → auf die `kMatOutput*Pin`-Konstanten verweisen.
- **MaterialShaderLibrary.h:27**: „Lighting = four vec4 = 64 bytes" — real weit über
  2 KB (localShadowVP allein 1 KiB).
- **DefaultAssets.h:27–33**: Grid-Textur- und Terrain-Material-Beschreibung stimmen
  beide nicht mehr mit `initDefaultAssets` überein.
- **EditorUI.cpp:205**: „compile HorizonCode → C++ (not implemented yet)" — ist
  vollständig implementiert (Checkbox Z. 2568, Worker-Build Z. 2986).
- **CrashHandler.h:6–10**: verspricht Reports „next to the log" — Default ist
  `$TMPDIR`.
- **RenderObject.h:533 / RenderExtractor.h:604**: „Fallback-Cube-Bounds" — Extractor
  lässt Bounds heute bewusst invalid.
- **EnvironmentComponent.h:4 / WeatherComponent.h Kopf**: beschreiben das alte
  Root-Entity-Modell (vor Forts. 8).
- **ParticleSystem.cpp:184 / SceneSystems.cpp:99**: „Phase 2 camera-following volume" —
  wurde stattdessen im WeatherSystem implementiert; `(void)cameraPos` bereinigen.
- **EngineApi.cpp:507**: „zone hiding and setVisible share this" — tun sie nicht
  (Sichtbarkeits-Flip doppelt implementiert, Z. 66–88 vs. 507–516).
- **HAsset.h:34, ProjectConfig.h:15, KeyDerivation.h:8, IRenderer.h:59,
  RenderPass.cpp:9, ScriptEngine.cpp:98** („Denk-laut"-Protokoll): kleinere
  Stale-Kommentare, Liste in den Einzelbefunden.

---

## 5. Benennungs-Inkonsistenzen (Konventionen festlegen, dann mechanisch)

- **Namespace**: `HE` vs. global wild gemischt — riskant global: `Input`, `File`,
  `Folder`, `Logger`, `EventBus`, `ThreadPool`, `ScriptLanguage`. Auch die exportierte
  Tools-API (`toString`, `cppIdentifier` in ProjectManager.h) liegt global. → Neue
  Typen konsequent in `HE`, Bestand schrittweise.
- **Methoden-Casing**: PascalCase (`Window::SetTitle`, `Application::Run`, GL/Metal-
  Backends) vs. camelCase (`InputMapping::mapAction`, Vulkan/D3D12, EngineProfiler) —
  teils in derselben Klasse (`Application`, `EditorUI.h`: `render()` neben
  `RenderEditor`).
- **Member-Präfix**: `m_` vs. Suffix-`_` vs. nichts — teils in derselben Klasse
  (`HorizonWorld`: `registry_` neben `m_hierarchyDirty`; `RenderResourceManager`:
  `allocator_` neben `m_nextIndex`). → auf `m_` vereinheitlichen.
- **Editor-Statics**: `s_` vs. `g_` gemischt (MaterialEditorPanel.cpp:241/244 direkt
  nebeneinander).
- **Konkrete Umbenennungen**: `HcClassList.{h,cpp}` → `HcEditorUtil.{h,cpp}` (Inhalt
  ist längst der Util-Namespace); `getAssetType(path)` → `sniffAssetTypeFromFile`
  (+ fehlendes `&` am by-value-String); `Node::hasArg` (auf EngineCall zu „isExec"
  umgedeutet) → neutrales Feld oder lokale `isExecCall`-Aliase;
  `ShowWidget`/`ShowWidgetId`-Enum über Kreuz zu ihren Anzeigenamen → `ShowSelf`/
  `ShowWidget` (gefahrlos, Ints werden nicht serialisiert); „Quick Settings"-Fenster →
  „Landscape"; `KeepCPUAssetsInfoAcknoleged`-Tippfehler (persistiert → Migrations-Read);
  `RendererBackend`/`GraphicsAPI`/`RendererFactory::Backend` = drei Namen für ein Enum
  → auf einen konsolidieren (Application.cpp:54 zeigt die Verwirrung: No-op-Cast mit
  falschem Kommentar).

---

## 6. Weitere Vereinfachungen (Mittel/Niedrig, je System)

**ContentManager/Hpak**
- 10× `getXxxMutable`-Boilerplate → `lookupAssetMutable`-Template (const-Pendant
  existiert schon).
- `MatFunctionLoader`-Lambda + Param-Slot-Loop doppelt (regenerate ↔ syncInstance).
- `encode/decodeMaterialShaderVariants` ↔ `…ParticleShaderVariants` zeilenidentisch
  → Template; Block liegt zudem mitten in der Getter-Sektion.
- `readStoredEntry`/`readEntry` duplizieren find+read (HpakReader.cpp:84–139).
- Handgerollter JSON-Escaper (ProjectExporter.cpp:526) neben nlohmann-Nutzung.
- `"__scene_index__"` als Magic-String an 3 Stellen → Konstante neben
  `kAssetPathIndexEntry`.
- zstd-Dictionary: halbtotes Format-Feature — Reader ignoriert `kFlagUsesDict` still
  → als RESERVED ablehnen oder implementieren.
- MTRL-Rewrite in `rewriteRefsForPack` (HpakWriter.cpp:196–259) muss mit `saveAsset`
  feldsynchron bleiben — beidseitige Querverweis-Kommentare fehlen.
- `TextureAsset::width/height/channels` als `size_t` im Binärformat → auf `uint32_t`
  festnageln.
- ScriptEngine: 5× kopiertes Lua-Call-Boilerplate → `pushInstanceMethod`-Helper.

**HE_Scene**
- `HE::api::find()` = Linear-Scan über ~300 Einträge pro Script-Call →
  `unordered_map`-Index.
- kGroups-Whitelist doppelt (ScriptContext.cpp:453 ↔ PyScriptBackend.cpp:395).
- Geplante Inversion ScriptApi→HE::api umsetzen → macht ~500 Zeilen Hand-Shims in
  ScriptContext/PyScriptBackend obsolet.
- `play`/`playSpatial` ~55 Zeilen Copy-Paste; Sort-Key-Formel `layer*256+depth` 3×;
  `createWidget` dupliziert `refreshElementAssets`; Animation-Systeme teilen
  Zeitfortschalt-Muster; `int cap = isSnow ? 20000 : 20000;` (SceneSystems.cpp:57).
- HcCodegen: exec-Emission CallExternal↔EngineCall identisch; `unique`-Suffix-Loop
  doppelt; `runStreaming` doppelt deklariert/dokumentiert.

**HorizonCode/Interpreter-Performance**
- `signatureOf` materialisiert im Hot-Path pro Aufruf 4 Heap-Vektoren (bis zu 6× pro
  connect, je Step im Runner) → Zähler-Variante oder per-Runner-Cache.
- `traceBody`-Lambda dupliziert `pinRanges`.
- ParticleGraph: hartcodierte Pin-Indizes 0–16 → benannte Konstanten (Vorbild
  `kMatOutput*Pin`).
- AnimatorSM: Transitions referenzieren States per Name statt id (erzwingt
  Rename-Fixups im Panel); `TransitionOp`-Cast ohne Range-Check.
- MaterialGraph: Textur-Slot-Auflösung doppelt (TextureSample↔NormalMapSample);
  FnInterface-Sammeln 3×; Param-Slots >16 erst nach Emit gekappt (out-of-bounds im
  UBO-Array-Zugriff im generierten Shader).
- UIWidget: 9× getProp/setProp/properties-Dreifachpflege → Deskriptor-Tabelle;
  `uiWidgetTypeName` alloziert komplette Elemente zur Namensauflösung → constexpr-Tabelle.

**HE_Tools/HE_Game**
- MeshImporter ↔ SkeletalMeshImporter: Vertex-Schleife + Base-Color-Import komplett
  doppelt → `ImporterCommon`; SkeletalMeshImporter missbraucht `materialPath` für den
  Textur-Pfad (semantisch anders als MeshImporter).
- Scene-Request-Dispatch mit Magic-Ints `r.kind == 0..5` → `enum class RequestKind`.
- GameApplication: Default-Kamera 2×, Streaming-Seed 4×, inkonsistente
  `renderer()`-Nullchecks (Z. 870 geprüft, 925/937 nicht), VSync-Hotkey `V` hart in
  Shipped Games, `auto m_backend = …` als Lokalname.
- hpak_packer: Argument-Parsing schluckt Fehler/unbekannte Flags still.
- `isUpToDate` prüft Sidecar-Outputs nicht (gelöschte `_basecolor.hasset` wird ohne
  `--force` nie regeneriert).

**Rendering shared**
- Shadow-Map-Auflösung 2048 als Magic Number an 3+ Stellen → `kShadowMapResolution`.
- `buildCustomVertex` dupliziert die beiden Standard-Vertex-Shader-Strings → aus einem
  Assembler erzeugen.
- GLSL↔HLSL-HW-RT-Kernel (4 Dateien) + `kLightingPreamble` (5 bewusste Kopien der
  Backend-Shader-Logik): Drift nur durch Disziplin verhindert → Sync-Kommentarblöcke +
  optional String-Vergleichs-Test der kritischen Konstanten.
- ShaderManager.h: deutscher Kommentar-Fremdkörper, `namespace fs = std::filesystem`
  global im public Header, ungenutzte Includes.
- Wetter-`ParticleInput`: `matId` immer leer (16 tote Bytes × zehntausende Partikel),
  Emitter-Konstanten pro Partikel kopiert → Range-Struktur.
- `EnvironmentSettings` (~60 Felder) aus IRenderer.h in eigenen Header ausgliedern;
  IRenderer mittelfristig in kleinere Interfaces trennen (Preview-Familie).

---

## 7. Empfohlene Rework-Reihenfolge

1. **Bug-Runde** (Abschnitt 0) — klein, unabhängig, sofort wertstiftend.
2. **Kommentar-/Doku-Korrekturen** (Abschnitt 4) — eine Session, kein Risiko.
3. **Toten Code löschen** (Abschnitt 3) — reduziert die Masse vor allen Refactorings.
4. **Editor: HC-Canvas-Host dedupen (1d) + Drop-Slot/Panel-State-Helper (1e)** — dann
5. **EditorUI.cpp-Split (2)** — mechanisch, aber groß; profitiert von 4.
6. **App-Schicht dedupen (1b)** + **Graph-Basis (1c)**.
7. **Backend-Dedup (1a)**: erst D3D-Shared-HLSL, dann SkyFrameParams + die kleinen
   Helper-Umzüge (SSAO/Noise/DominantLight/PackedLightArray).
8. **Namenskonventionen (5)** zum Schluss mechanisch durchziehen, wenn die Struktur
   steht.
