# Horizon Engine — Masterplan zur vollwertigen Engine

Stand: 12. Juni 2026. Ersetzt die Meilenstein-Sicht der ROADMAP.md durch einen
vollständigen Plan bis zur „modernen Engine auf Augenhöhe" (Referenzrahmen:
Unity/Godot-Featureset, nicht Unreal-AAA).

---

## Ist-Zustand (was schon fertig ist)

| Bereich | Status |
|---|---|
| Core: Window, App-Loop, Input, Logger, ContentManager, .hasset-Format | ✅ |
| UUID-Persistenz im META-Chunk (v2) | ✅ |
| Erster Render-Pfad: ECS-Welt → sichtbares Mesh auf GL **und** Metal (CommandBuffer, RenderWorld, RenderExtractor, Kamera) | ✅ |
| Editor-Shell: Hub, Docking, Outliner, Content Browser | ✅ |
| Backend-Gerüste GL/Metal/Vulkan/D3D11/D3D12 | 🟡 GL+Metal zeichnen (getestet); D3D11/D3D12/Vulkan haben jetzt denselben Szenen-Draw-Pfad, aber **unverifiziert** (nicht auf macOS baubar) |
| Asset-Importer (Texture/Mesh/Material/Audio), asset_compiler, Packer | 🔴 Stubs |
| SceneSerializer | 🔴 nur Name + Hierarchie |
| RenderGraph, RenderPass, RenderResourceManager, GPUMemoryAllocator | 🔴 leer |
| Memory (Ref\<T\>, Allocatoren) | 🔴 leer |
| Physik, Scripting, Audio, Animation, Partikel, Navigation, In-Game-UI | 🔴 fehlen komplett |
| Tests, CI, Profiling | 🔴 fehlen |

---

## Abhängigkeitsgraph (Phasen)

```mermaid
graph TD
    P0[Phase 0: Fundament<br/>Tests, CI, Memory, Job-System]
    P1[Phase 1: Asset-Pipeline<br/>Importer, asset_compiler]
    P2[Phase 2: Editor als Werkzeug<br/>Viewport, Inspector, Serializer,<br/>Picking, Gizmos, Undo, Play-Mode]
    P3[Phase 3: Modernes Rendering<br/>PBR, Schatten, RenderGraph,<br/>PostFX, Culling]
    P4a[Phase 4a: Physik - Jolt]
    P4b[Phase 4b: Scripting - Lua]
    P4c[Phase 4c: Audio - miniaudio]
    P4d[Phase 4d: Animation/Skinning]
    P5[Phase 5: Gameplay-Schicht<br/>Partikel, In-Game-UI, Navigation,<br/>Prefabs, Input-Mapping]
    P6[Phase 6: Shipping<br/>hpak, Build-Pipeline,<br/>Vulkan/D3D-Parität, Streaming]
    P7[Phase 7: Kür<br/>GI, TAA, Terrain, Networking]

    P0 -.läuft parallel zu allem.-> P1
    P1 --> P2
    P1 --> P3
    P2 --> P4a
    P2 --> P4b
    P2 --> P4c
    P1 --> P4d
    P3 --> P4d
    P4a --> P5
    P4b --> P5
    P2 --> P5
    P5 --> P6
    P3 --> P6
    P6 --> P7
    P3 --> P7
```

Kernaussage: **Asset-Pipeline (P1) ist der Flaschenhals** — Editor-Ausbau,
Rendering-Features und Animation hängen alle daran. Die vier 4er-Blöcke sind
untereinander unabhängig und parallelisierbar.

---

## Phase 0 — Fundament (Querschnitt, sofort startbar, läuft nebenher)

Keine Abhängigkeiten; jede Woche ein bisschen davon.

| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 0.1 | **Test-Gerüst** (doctest oder Catch2) | — | Zuerst: SlotMap, HAsset-Roundtrip, SceneSerializer-Roundtrip, ContentManager |
| 0.2 | **CI** GitHub-Actions-Matrix (macOS + Windows, später Linux) | 0.1 | Build + Tests pro PR; verhindert Backend-Drift |
| 0.3 | **`Ref<T>`** (intrusiver Refcount) + Einsatz im ContentManager | — | Voraussetzung für Asset-Unloading (6.4) und GPU-Eviction (3.7) |
| 0.4 | **Job-System** (Thread-Pool, parallel_for, Abhängigkeits-Handles) | — | Voraussetzung für parallele Extraction (3.8), Async-Loading (6.4), Physik-Threading |
| 0.5 | **Profiling-Hooks**: Tracy vendoren, Frame-/Zone-Marker | — | Früh einbauen ist billig, nachrüsten teuer |
| 0.6 | **Aufräumen**: doppelte glm-Kopie (vendored + FetchContent) auf eine Quelle | — | klein |
| 0.7 | **Debug-Draw-API** (Linien, Wireframe-AABBs, Text im Viewport) | Render-Pfad ✅ | Hilft jeder späteren Phase (Physik-Collider, Frustum, NavMesh sichtbar machen) |

---

## Phase 1 — Asset-Pipeline end-to-end (kritischer Pfad)

**Ziel:** glTF/PNG rein → .hasset raus → Content Browser → Szene.
Blockiert P2 (man braucht Assets zum Editieren), P3 (PBR braucht Texturen/Materialien)
und P4d (Skelette kommen aus dem Mesh-Import).

| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 1.1 | **TextureImporter** | — | stb_image (liegt schon im vendor) → PIXL/TXMI-Chunks; Mipmap-Generierung gleich mitmachen |
| 1.2 | **MeshImporter** | — | cgltf vendoren (header-only); VERT/INDX/NORM/TEXC + MREF; Tangenten gleich mitberechnen (PBR braucht sie) |
| 1.3 | **MaterialImporter** | 1.1 | JSON → MTRL-Chunk; glTF-Materialien (metallic/roughness) aus 1.2 übernehmen |
| 1.4 | **asset_compiler verdrahten** | 1.1–1.3 | Verzeichnis-Walk, Endung → Importer, Inkrementalität per mtime |
| 1.5 | **Editor-Import**: Button + Drag&Drop in den Content Browser | 1.4 | ruft die Importer-Lib direkt auf, nicht das CLI |
| 1.6 | **Textur-Kompression** (BCn/ASTC, z. B. via bc7enc o. ä.) | 1.1 | kann nach hinten rutschen, aber vor Shipping (P6) nötig |
| 1.7 | **AudioImporter** (PCMD-Chunks, WAV/OGG via dr_libs/stb_vorbis) | — | unabhängig; wird erst in P4c konsumiert |

**DoD:** Heruntergeladenes glTF-Modell mit Texturen importieren und im Editor gerendert sehen.

---

## Phase 2 — Editor wird Werkzeug

**Ziel:** Szene bauen, speichern, laden, abspielen — ohne Code anzufassen.
Braucht P1 (Assets zum Platzieren); 2.1–2.2 können sofort parallel zu P1 starten.

| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 2.1 | **Szenen-Viewport offscreen** (FBO/MTLTexture → `ImGui::Image`) | Render-Pfad ✅ | Renderer-API `SetRenderTarget(w,h)` + `GetViewportTexture()`; Resize-Handling |
| 2.2 | **SceneSerializer vervollständigen** | — | alle Komponenten (Transform, Mesh, Material, Light, Camera, RigidBody, Script, Transform2D) + Binary-Pfad; Versionierung beibehalten |
| 2.3 | **Inspector-Panel** | 2.2 sinnvoll | Komponenten anzeigen/editieren, Add/Remove-Menü; Reflection-Mini-Makro lohnt sich hier schon |
| 2.4 | **Outliner-Ausbau** | — | Entity anlegen/löschen/umbenennen, Reparenting per Drag&Drop, Selektion ↔ Inspector |
| 2.5 | **Picking** (ID-Buffer-Pass; Entity-ID im RenderObject existiert) | 2.1 | Klick im Viewport selektiert |
| 2.6 | **Gizmos** (ImGuizmo vendoren) | 2.1, 2.5 | Translate/Rotate/Scale + Snap |
| 2.7 | **Undo/Redo** (Command-Pattern auf Komponentenebene) | 2.3 | Toolbar-Icons existieren schon |
| 2.8 | **Play-in-Editor** | 2.2 | Welt-Snapshot in Memory-Buffer bei Play, Restore bei Stop |
| 2.9 | **Editor-Kamera-Komfort** | 2.1 | Orbit/Fly-Modus, Focus-on-Selection (F), Grid im Viewport |

**DoD:** Szene zusammenklicken, speichern, Editor neu starten, weitermachen, Play drücken.

---

## Phase 3 — Modernes Rendering

Reihenfolge nach Sichtbarkeit pro Aufwand. Braucht P1 für Materialien/Texturen;
3.1 und 3.3 gehen sofort.

| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 3.1 | **FrustumCuller + RenderSorter** | Render-Pfad ✅ | Header existieren, AABB.h liegt in Core/Math; Culling → Sorting → Submit |
| 3.2 | **Shader-Cross-Compile** ausbauen | — | `glslc → SPIR-V → SPIRV-Cross → MSL/HLSL` im shader_compiler; beendet handgeschriebene MSL-Duplikate und ist Voraussetzung für Vulkan/D3D-Parität (6.2) |
| 3.3 | **Beleuchtung**: Blinn-Phong → **PBR** (metallic/roughness) | 1.2/1.3 für echte Materialien | LightData als Uniform-Block; Punkt-, Spot-, Directional-Lights |
| 3.4 | **RenderGraph + Pass-System aktivieren** | 3.1 | GeometryPass → PostProcessPass als erste Knoten; .cpp sind leer, Header + Design-Doc existieren |
| 3.5 | **Schatten**: Directional mit einer Cascade → CSM | 3.4 | danach Punkt-/Spotlicht-Schatten |
| 3.6 | **HDR + Tonemapping** als erster PostProcess-Pass | 3.4 | danach Bloom |
| 3.7 | **RenderResourceManager + GPUMemoryAllocator** | 0.3 | Budget + LRU-Eviction, `onHandleUsed`-Hook beim Draw |
| 3.8 | **Instancing + parallele Extraction** | 3.1, 0.4 | instanceCount im DrawCall existiert schon |
| 3.9 | **Skybox + IBL** (Environment-Map, Irradiance/Prefilter) | 3.3 | macht PBR erst „modern aussehend" |
| 3.10 | **Transparenz-Pass** (sortiertes Alpha-Blending) | 3.1, 3.4 | OIT ist Kür (P7) |
| 3.11 | **Anti-Aliasing**: FXAA zuerst | 3.6 | TAA ist Kür (P7) |
| 3.12 | **SSAO** | 3.4 | optionaler, aber sichtbarer Gewinn |

**DoD:** PBR-Szene mit Schatten, HDR, Skybox bei stabilen Frametimes; Frustum-Culling messbar via Tracy.

---

## Phase 4 — Engine-Systeme (vier unabhängige, parallelisierbare Blöcke)

Alle brauchen P2.2 (Serializer, damit Komponenten persistiert werden) und
profitieren von P2.8 (Play-Mode zum Testen).

### 4a — Physik
| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 4a.1 | **Jolt Physics** integrieren (FetchContent) | 2.2, 2.8 | gegen RigidBodyComponent; fixedUpdate-Hook im GameLoop existiert |
| 4a.2 | Collider-Komponenten (Box/Sphere/Capsule/Mesh) + Debug-Draw | 4a.1, 0.7 | |
| 4a.3 | Raycasts/Queries als Engine-API | 4a.1 | braucht Scripting (4b) später als Konsument |
| 4a.4 | Character-Controller | 4a.1 | |
| 4a.5 | 2D-Physik (Box2D) — optional, wenn Catania es braucht | 2.2 | Transform2D existiert schon |

### 4b — Scripting
| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 4b.1 | **Lua via sol2**, ScriptComponent-Lifecycle (onStart/onUpdate) | 2.2, 2.8 | Enums sind vorbereitet |
| 4b.2 | Engine-API-Binding (Entity, Transform, Input, Spawn/Destroy) | 4b.1 | |
| 4b.3 | Hot-Reload von Scripts im Play-Mode | 4b.1 | |
| 4b.4 | Script-Properties im Inspector (exportierte Variablen) | 4b.1, 2.3 | |
| 4b.5 | C#/.NET-Hosting — später oder nie | 4b.2 | erst evaluieren, wenn Lua nicht reicht |

### 4c — Audio
| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 4c.1 | **miniaudio** + AudioSource/AudioListener-Komponenten | 1.7, 2.2 | |
| 4c.2 | 3D-Spatialization, Attenuation | 4c.1 | |
| 4c.3 | Mixer/Bus-System (Music/SFX-Gruppen, Lautstärke) | 4c.1 | |

### 4d — Animation
| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 4d.1 | Skelett + Skinning-Daten im MeshImporter (glTF-Skins) | 1.2 | neue Chunks im .hasset-Format |
| 4d.2 | GPU-Skinning (Bone-Matrizen als Uniform/Storage-Buffer) | 4d.1, 3.3 | |
| 4d.3 | AnimationClip-Playback + AnimatorComponent | 4d.1, 2.2 | |
| 4d.4 | Blending + State-Machine (einfacher Animator-Graph) | 4d.3 | |
| 4d.5 | Property-Animation (Transform/Material animieren, für Cutscenes/UI) | 4d.3 | |

---

## Phase 5 — Gameplay-Schicht

Macht aus „Renderer + Systeme" eine Engine, in der man ein Spiel *baut*.

| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 5.1 | **Prefabs** (Entity-Hierarchie als Asset, Instanzen + Overrides) | 2.2 | enormer Workflow-Gewinn, früh in P5 machen |
| 5.2 | **Input-Mapping** (Actions/Axes statt Roh-Keys, Gamepad) | — | Scripting (4b.2) konsumiert es |
| 5.3 | **Partikelsystem** (CPU-Sim zuerst, instanziertes Rendering) | 3.8 | GPU-Sim ist Kür |
| 5.4 | **In-Game-UI-Runtime** (Canvas, Text via MSDF/stb_truetype, Buttons, Anchoring) | Render-Pfad ✅ | nicht ImGui — das ist Editor-only |
| 5.5 | **Navigation**: Recast/Detour-NavMesh-Baking + Agenten | 4a.1 | |
| 5.6 | **Szenen-Streaming/Additive-Load** (mehrere Szenen gleichzeitig) | 2.2 | |
| 5.7 | **Event-/Messaging-System** für Gameplay-Code | 4b.2 | |

---

## Phase 6 — Shipping & Plattform-Reife

| # | Aufgabe | Hängt ab von | Details |
|---|---|---|---|
| 6.1 | **hpak-Packaging**: HpakWriter + KeyDerivation implementieren, asset_compiler → Packer-Kette, GameApplication lädt aus .hpak | 1.4 | SerializeFormat::Binary-Pfad |
| 6.2 | **Vulkan-Backend auf Parität** (Draw-Pfad, danach D3D12; D3D11 ggf. streichen) | 3.2, 3.4 | Linux-Support hängt hieran |
| 6.3 | **„Build Game"-Pipeline im Editor**: Standalone-Export (Executable + .hpak) pro Plattform | 6.1 | |
| 6.4 | **Async-Asset-Streaming** (Lade-Jobs, Platzhalter-Assets, Unloading via Ref\<T\>) | 0.3, 0.4 | |
| 6.5 | **Crash-Reporting scharf schalten** (CrashHandler existiert), Logging in Datei | — | |
| 6.6 | **Linux-Window/Input-Pfad** testen + CI-Leg | 0.2, 6.2 | |
| 6.7 | **Doku**: Getting-Started, Script-API-Referenz | 4b.2 | spätestens wenn jemand Zweites die Engine benutzt |

**DoD:** Ein Knopf im Editor erzeugt ein lauffähiges, ausliefbares Spiel-Binary mit gepackten, komprimierten Assets — auf macOS und Windows.

---

## Phase 7 — Kür (nach Bedarf, von Catania getrieben)

Kein fester Plan — einzeln ziehen, wenn das Spiel es verlangt:

- **TAA** und/oder **OIT** (Order-Independent Transparency)
- **Global Illumination** (Probes/DDGI-light) und **SSR**
- **LOD-System** + Impostors
- **Terrain** + Vegetation/Foliage
- **GPU-Partikel**
- **Networking** (Replikation) — nur falls Catania Multiplayer wird
- **Virtual Texturing / Bindless** — nur bei nachgewiesenem Bedarf

---

## Empfohlene Reihenfolge der nächsten 5 Arbeitsschritte

> **Status 12.06.2026:** Alle 5 Schritte sind umgesetzt. ✅

1. ✅ **TextureImporter + MeshImporter** (1.1–1.5) — stb_image + cgltf, dazu Material-
   und Audio-Importer (dr_wav), asset_compiler-CLI mit mtime-Inkrementalität und
   UUID-Stabilität bei Re-Imports. Editor: „Import"-Kontextmenü im Content
   Browser + „Add to Scene" für Mesh-Assets. **Dazu:** GL- und Metal-Backend
   lösen `meshAssetId` jetzt wirklich auf (Upload on first sight, Basecolor-
   Textur über Material-Kette); der hartkodierte Würfel ist nur noch Fallback.
2. ✅ **Offscreen-Viewport** (2.1) — `SetViewportSize`/`GetViewportTexture` in der
   Renderer-API, GL-FBO + Metal-Offscreen-Pass, andockbares „Scene"-Fenster
   mit HiDPI-Handling und Resize.
3. ✅ **SceneSerializer vervollständigt** (2.2) — alle 8 Komponenten, JSON- und
   Binary-Pfad (CBOR derselben Struktur), Version 1.1, abwärtskompatibel.
4. ✅ **Test-Gerüst + CI** (0.1, 0.2) — doctest mit 14 Test-Cases (SlotMap,
   HAsset-Roundtrip, ContentManager-UUID-Persistenz, Serializer-Roundtrips),
   GitHub-Actions-Matrix macOS + Windows in `.github/workflows/ci.yml`.
5. ✅ **Inspector + Outliner-CRUD** (2.3, 2.4) — Details-Panel mit allen
   Komponenten-Editoren + Add/Remove-Component; Outliner mit Selektion,
   Create/Rename/Delete und Drag&Drop-Reparenting (zyklensicher via
   `HorizonWorld::reparentEntity`, rekursives `destroyEntity`).

> **Bugfix 13.06.2026:** Editor-Crash beim Viewport-Resize behoben (Use-after-free:
> `EnsureViewportTarget` gab die alte MTLTexture frei, während die ImGui-Drawlist
> desselben Frames sie noch referenzierte → SIGSEGV in `setFragmentTexture:`).
> Fix: Retired-Texture-Graveyard, Freigabe erst 3 Frames später (GL + Metal).
> Regression-Hook: `HE_VIEWPORT_RESIZE_STRESS=1` ändert die Viewport-Größe
> jeden Frame — damit verifiziert.

> **Status 13.06.2026:** Auch die zweite Top-5-Runde ist umgesetzt. ✅

1. ✅ **Picking** (2.5) — als CPU-Ray-AABB-Test statt ID-Buffer (bewusste
   Abweichung: backend-unabhängig, und die AABB-Infrastruktur in
   `Core/Math/AABB.h` braucht das Culling sowieso). Klick im Scene-Viewport
   selektiert das nächstgelegene Objekt, Klick ins Leere deselektiert.
   ID-Buffer-Picking kann später für Pixel-Präzision nachgerüstet werden.
2. ✅ **Gizmos** (2.6) — ImGuizmo v1.92.5 vendored
   (`src/HE_Editor/vendor/imguizmo/`). Translate/Rotate/Scale per W/E/R,
   World→Local-Rückrechnung über die Parent-Matrix.
3. ✅ **Play-in-Editor** (2.8) — Play/Stop-Button verdrahtet:
   CBOR-Snapshot bei Play, `HorizonWorld::clear()` + Restore bei Stop.
4. ✅ **FrustumCuller + RenderSorter** (3.1) und **Blinn-Phong** (3.3) —
   Gribb/Hartmann-Frustum gegen Welt-AABBs (Backends verfeinern mit echten
   Mesh-Bounds), Sortierung Mesh-gruppiert + front-to-back; bis zu 8 Lichter
   (Directional/Point/Spot mit Range-Attenuation und Spot-Kegel) auf GL und
   Metal, Fallback-Headlight für Szenen ohne Lichter.
5. ✅ **Undo/Redo** (2.7) — Snapshot-basiert (`EditorUndo`, CBOR-Weltzustand,
   max. 64 Einträge) statt feingranularer Commands: deckt alle Operationen
   einheitlich ab (Create/Delete/Reparent/Rename/Komponenten-Edits/Gizmo).
   Cmd/Ctrl+Z, Shift+Cmd+Z bzw. Ctrl+Y, Footer-Buttons mit Disabled-State.
   Bekannte Einschränkung: Selektion geht bei Undo verloren (Entity-Handles
   werden remapped).

Stand der Tests: 23 doctest-Cases (zusätzlich: AABB/Frustum/Sorter, EditorUndo,
Play-Mode-Zyklus), alle grün.

> **Status 13.06.2026 (Forts.):** Editor-Kamera (2.9) umgesetzt. ✅

**Editor-Kamera (2.9)** — Vollwertige Scene-View-Kamera (`EditorCamera`,
`src/HE_Editor/EditorCamera.{h,cpp}`) im Unity-Stil:
- **Alt+LMB** orbit um den Pivot, **MMB** pan, **Mausrad** dolly, **RMB**
  Fly-Look mit **WASDQE** (Shift = schneller), **F** = Focus-on-Selection.
- Architektur: `EditorCameraOverride` (view + Position + fov/near/far) liegt in
  `Renderer/IRenderer.h` (Core). Der Editor schiebt sie pro Frame über
  `IRenderer::SetEditorCamera`; der `RenderExtractor` nutzt sie statt der
  Szenen-`CameraComponent` und baut die Projektion mit dem Backend-Aspect, sodass
  Bild, Gizmo und Picking-Strahl exakt übereinstimmen (GL + Metal verdrahtet).
- Im Play-Mode wird der Override deaktiviert → die Spiel-Kamera der Szene zählt.
- **Grid** über `ImGuizmo::DrawGrid` auf der Welt-XZ-Ebene, Toggle „Show Grid"
  in den Quick Settings (persistiert). Gizmo/Picking sind während Navigation
  bzw. bei gedrücktem Alt unterdrückt.
- Tests: 4 neue doctest-Cases (`tests/test_editorcamera.cpp`) für Default-
  Framing, Dolly, Orbit-Radius-Erhalt und Focus → jetzt **27 Cases, alle grün**.

> **Status 13.06.2026 (Forts.):** Szene speichern/laden im Editor umgesetzt. ✅

**Szene speichern/laden im Editor** — Save/Load auf den komplettierten
SceneSerializer (JSON) gelegt:
- File-Menü: **New Scene**, **Open Scene…**, **Save Scene** (Cmd/Ctrl+S),
  **Save Scene As…** (Shift+Cmd/Ctrl+S); SDL-Datei-Dialoge (`.hescene`-Filter,
  Start im `Content`-Ordner). Tastatur-Shortcuts global im Editor.
- Szenenwechsel per **Doppelklick** auf eine `.hescene` im Content Browser.
- Der gemeinsame async-Datei-Slot (`pendingFileReady/Result`) wird über eine
  `PendingFileOp`-Intent-Enum für Projekt-Öffnen / Szene-Öffnen / Szene-Speichern
  disambiguiert; bei „Save As" wird die `.hescene`-Endung erzwungen.
- `EditorApplication` trackt `m_currentScenePath` + `m_savedRevision`; der
  Fenstertitel zeigt „Projekt — Szene [*]" (Dirty-Marker). Dirty-Erkennung über
  einen Revisions-Zähler in `EditorUndo` (bumpt bei push/undo/redo). Beim Öffnen
  einer Szene wird der Play-Mode verlassen, Undo-History geleert, Selektion
  zurückgesetzt; `New Scene` leert die Welt auf den Root.

> **Status 13.06.2026 (Forts.):** RenderGraph-Grundlage (3.4) aktiviert. ✅

**RenderGraph aktivieren (3.4)** — Das Pass-System ist scharf geschaltet und
beide Backends submitten jetzt darüber (technische Grundlage zuerst, Features
bauen darauf auf):
- `CommandBuffer`/`RenderGraph`/`RenderPass` (vorher leere Stubs) implementiert.
  `DrawCall` trägt jetzt `meshAssetId`/`entityId`/`lod` (+ die künftigen
  RenderHandle-Felder), sodass das Backend ohne RenderWorld-Zugriff replayen kann.
- **GeometryPass** wandelt die gecullten + sortierten sichtbaren Objekte in
  DrawCalls. GL und Metal bauen pro Frame `m_renderGraph.execute(world, sorted,
  m_cmds)` und replayen `m_cmds.drawCalls()` statt direkt über `sortedIndices`
  zu iterieren — Mesh-Auflösung per UUID bleibt im Backend, das die Rolle von
  `IRenderDevice::submit` übernimmt (kein voller RHI-Umbau nötig).
- **ShadowPass**/**PostProcessPass** sind deklariert, aber bewusst inert: sie
  brauchen Render-Target-Plumbing (Depth-Target aus Licht-POV für 3.5,
  Offscreen-HDR-Target + Fullscreen-Pass für 3.6), das der reine CPU-seitige
  CommandBuffer noch nicht modelliert → Folgeschritt.
- Tests: 4 neue doctest-Cases (`tests/test_rendergraph.cpp`) für DrawCall-
  Reihenfolge/Payload, Out-of-range-Skip, Buffer-Reset pro Frame und inerte
  Passes → jetzt **31 Cases, alle grün**.

> **Status 13.06.2026 (Forts.):** D3D11/D3D12/Vulkan auf Szenen-Draw-Parität
> gebracht (Option „erst Parität, dann Targets"). ✅ **Achtung: unverifiziert** —
> keines der drei baut auf dieser macOS-Maschine (D3D = `if(WIN32)`, Vulkan =
> kein SDK), daher sorgfältig-aber-blind und auf Windows / mit Vulkan-SDK zu
> validieren. GL+Metal + 31 Tests unverändert grün.

**D3D11/D3D12/Vulkan Szenen-Draw (P6-Parität vorgezogen):** Alle drei nutzen jetzt
denselben `extractor→cull→sort→RenderGraph→GeometryPass→DrawCall`-Pfad wie GL/Metal,
zeichnen also beleuchtete Geometrie statt nur Clear. Gemeinsam: interleaved
pos3+normal3+uv2, Mesh-Upload on first sight aus dem ContentManager, Cube-Fallback,
Blinn-Phong (8 Lichter), Editor-Kamera-Override, Tiefenpuffer.
- **D3D11** (`d3dcompiler`, HLSL zur Laufzeit): inkl. Basecolor-Texturen.
- **D3D12** (Root-CBVs + PSO, Upload-Heaps statt Staging): Flat-Color, Texturen
  als TODO (DEFAULT-Heap-Upload + Descriptor-Tables zu fehleranfällig blind).
- **Vulkan** (Push-Constants + per-frame-UBO, GLSL→SPIR-V via glslc, Clip-Fix für
  Y/Tiefe): Flat-Color, Texturen TODO; `.spv` müssen nach `<exe>/Shaders/` deployen.

> **Status 13.06.2026 (Forts.):** Render-Target-Abstraktion im RenderGraph
> (Seam) steht. ✅

**Render-Target-Abstraktion (Fundament):** `RenderTarget.h` definiert
backend-agnostisch `RenderTargetDesc`/`RenderPassIO` (Format RGBA8/RGBA16F/Depth,
Größe Viewport/Fixed, Ein-/Ausgabe-Targets, `kBackbufferTarget = 0`).
`RenderPass::describe()` deklariert das Ziel eines Passes (GeometryPass →
Backbuffer; ShadowPass → 2048²-Depth; PostProcessPass → Backbuffer + SceneColor-
Input). `RenderGraph::execute(world, sorted, PassSink)` ruft pro Pass den Backend-
Sink `(pass, io, cmds)` — der bindet das Ziel und replayt. **Alle fünf Backends**
nutzen jetzt den Sink (GL+Metal verifiziert, D3D/Vulkan blind/mechanisch),
verhaltensneutral: heute rendert nur GeometryPass in den Backbuffer. Die
eigentliche Offscreen-Target-Allokation (FBO/MTLTexture/RTV-DSV/VkImage-Pool)
landet mit dem ersten Feature, das sie braucht (ShadowPass/HDR), wo auch die
Sampling-Shader entstehen. 2 neue Tests (Per-Pass-Dispatch + `describe()`) →
**33 Cases, alle grün**.

> **Status 13.06.2026 (Forts.):** ShadowPass (3.5) auf **OpenGL** umgesetzt +
> kompilier-verifiziert (Commit `e7779e0`); Metal/D3D/Vulkan offen. ✅(GL)

**ShadowPass (3.5) — OpenGL:** Directional-Schatten via 2048²-Depth-Map. Shared:
`RenderWorld.shadow` (lightVP/dir/enabled), Extractor fittet ein Ortho-Frustum um
die Szene; `ShadowPass` zeichnet die sichtbare Geometrie depth-only, GeometryPass
sampelt die Map (Slope-Bias). GL rendert ShadowPass → Map → GeometryPass über den
Sink. **Metal/D3D/Vulkan offen** — konventionssensibel (NDC/Tiefe z 0..1, Y-Flip
des lightVP) und hier nicht render-testbar; Metal braucht zudem einen eigenen
Depth-Encoder vor dem Szenen-Encoder.

**Nächste Schritte (neue Top 5):**

1. **ShadowPass auf Metal/D3D/Vulkan** — lightVP pro NDC anpassen (clipFix z→0..1,
   Y-Flip); Metal: separater Depth-Encoder. Auf Mac (Metal) zumindest baubar.
2. **HDR + Tonemapping** (3.6) — SceneColor als RGBA16F-Target, PostProcessPass
   als Fullscreen-Tonemap auf den Backbuffer (nutzt dieselbe Target-Infra).
3. **Material-Inspector** — Material-Zuweisung per Drag&Drop aufs
   MaterialComponent, Shader-/Textur-Slots editierbar.
4. **Save-Prompt bei ungesicherten Änderungen** — „Speichern?"-Dialog vor
   Szenenwechsel/Projektschließen/Quit, wenn der Dirty-Marker aktiv ist.
5. **D3D/Vulkan-Validierung** — auf Windows / mit Vulkan-SDK bauen, die
   blind geschriebenen Szenen-Draw-Pfade verifizieren und korrigieren.

Faustregel für die Parallelisierung danach: eine Person/ein Strang auf dem
kritischen Pfad P1 → P2 → P5 → P6, Rendering (P3) und je ein P4-Block laufen
daneben her.
