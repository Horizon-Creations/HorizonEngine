# Horizon Engine — Roadmap

Stand: Juni 2026. Basis: Code-Audit nach dem Metal-Backend-Merge.
Leitidee: **jeder Meilenstein endet mit etwas Sichtbarem im Editor** — kein
monatelanges Infrastruktur-Graben ohne Feedback.

---

## Ist-Zustand (Kurzfassung)

| Bereich | Status |
|---|---|
| Core (Window, App-Loop, Input, Logger, GlobalState, ContentManager) | ✅ funktioniert |
| Backends (GL 4.1/4.6, Metal, Vulkan*, D3D11/12*) | 🟡 Clear + ImGui-Overlay, keine Draw-Calls |
| Rendering-Mittelschicht (RenderGraph, RenderWorld, Extractor, ResourceManager, GPUAllocator, CommandBuffer) | 🔴 Header + Design-Doc vorhanden, alle .cpp leer |
| Editor (Hub, Docking, Outliner, Content Browser, Quick Settings) | 🟡 UI steht; kein Viewport-Bild, kein Inspector, kein Picking/Gizmo/Undo |
| Asset-Pipeline (.hasset Reader/Writer, ContentManager) | 🟡 Format + Laden ok; **alle Importer sind Stubs**, asset_compiler leer, Packer leer |
| SceneSerializer | 🔴 nur Name + Hierarchie — Transform/Mesh/Light/Camera werden nicht gespeichert |
| Memory (Ref.h, PoolAllocator.h, Allocator.h) | 🔴 leere Dateien |
| Scripting / Physics / Audio | 🔴 nur Komponenten-Structs, keine Systeme |
| Tests | 🔴 keine |

\* Vulkan nur mit SDK, D3D nur Windows.

---

## Meilenstein 1 — Erstes gerendertes Mesh („Triangle to Cube")

**Ziel:** Ein MeshComponent aus der ECS-Welt erscheint im Fenster. Erst
hardcodiert, dann aus dem ContentManager. Das ist der kritische Pfad — alles
andere hängt daran.

1. **Shader-Strategie festlegen (zuerst entscheiden, dann bauen).**
   Empfehlung: GLSL 410 als einzige Quellsprache, Laufzeit-Kompilierung pro
   Backend wie bisher. Für Metal kurzfristig handgeschriebene MSL-Pendants
   neben der GLSL-Datei (`shader.vert.glsl` + `shader.metal`); mittelfristig
   `glslc → SPIR-V → SPIRV-Cross → MSL` in den shader_compiler einbauen.
   Entry-Point-Konvention `vertexMain`/`fragmentMain` ist im
   MetalShaderManager bereits angelegt.
2. **CommandBuffer + RenderWorld + RenderObject implementieren** (reine
   Datencontainer, Header existieren — wenige Stunden Arbeit).
3. **Backend-Draw-Pfad:** `OpenGLRenderer` und `MetalRenderer` bekommen
   `submit(const CommandBuffer&)`: VAO/VBO bzw. MTLBuffer + PipelineState,
   ein Unlit-Shader mit MVP-Uniform. Depth-Buffer im Metal-Backend ergänzen
   (GL hat ihn schon).
4. **Kamera:** CameraComponent → CameraData (View/Projection aus
   TransformComponent), Default-Editor-Kamera wenn keine in der Szene.
5. **RenderExtractor:** EnTT-View über (Transform, Mesh) → RenderWorld füllen.
   Dirty-Flags gibt es in MeshComponent schon.
6. **RenderResourceManager minimal:** UUID → RenderHandle, Upload beim ersten
   Sehen (Daten kommen aus ContentManager::StaticMeshAsset), SlotMap intern.

**Definition of Done:** Würfel mit Transform aus der Szene dreht sich im
Fenster, auf OpenGL **und** Metal.

---

## Meilenstein 2 — Asset-Pipeline end-to-end

**Ziel:** `.obj`/`.gltf`/`.png` rein → `.hasset` raus → erscheint im Content
Browser → liegt in der Szene.

1. **TextureImporter** mit stb_image (liegt schon im Editor-vendor) → PIXL/TXMI-Chunks.
2. **MeshImporter** — Empfehlung: **cgltf oder tinyobjloader** vendoren
   (header-only, passt zum bisherigen Vendor-Ansatz; assimp ist mächtiger,
   aber ein schwerer Build-Klotz). Schreibt VERT/INDX/NORM/TEXC + MREF.
3. **MaterialImporter:** JSON-Beschreibung → MTRL-Chunk (Shader-Pfad + Textur-Refs).
4. **asset_compiler verdrahten:** Verzeichnis-Walk, Endung → Importer,
   Inkrementalität über mtime-Vergleich.
5. **Editor-Import:** „Import"-Button + Drag&Drop ins Content-Browser-Panel,
   ruft die Importer-Lib direkt auf (nicht das CLI).
6. **UUID-Stabilität klären:** ContentManager generiert UUIDs derzeit beim
   Laden — sie müssen in die META-Chunk geschrieben und wiederverwendet
   werden, sonst brechen Szenen-Referenzen bei jedem Neustart. **Das ist ein
   Bug, der vor Meilenstein 3 gefixt sein muss.**

**Definition of Done:** Ein heruntergeladenes glTF-Modell mit Textur wird
importiert und rendert im Editor.

---

## Meilenstein 3 — Editor wird Werkzeug

**Ziel:** Man kann eine Szene bauen, speichern, laden — ohne Code anzufassen.

1. **Szenen-Viewport:** Offscreen-Rendertarget (FBO / MTLTexture) statt
   Direkt-ins-Fenster; als `ImGui::Image` ins Viewport-Tab. Resize-Handling.
   Renderer-API: `SetRenderTarget(w, h)` + `GetViewportTexture()`.
2. **SceneSerializer vervollständigen:** alle 10 Komponenten (Transform,
   Mesh, Material, Light, Camera, RigidBody, Script, Transform2D) +
   Binary-Pfad. Versionierung beibehalten.
3. **Inspector/Details-Panel:** Komponenten des selektierten Entities
   anzeigen/editieren, Add/Remove-Component-Menü.
4. **Outliner ausbauen:** Entity anlegen/löschen/umbenennen, Reparenting per
   Drag&Drop, Selektion mit Inspector koppeln.
5. **Picking:** ID-Buffer-Pass (Entity-ID in RenderObject existiert schon
   genau dafür) — Klick im Viewport selektiert.
6. **Gizmos:** ImGuizmo vendoren (Translate/Rotate/Scale, Snap).
7. **Undo/Redo:** Command-Pattern auf Komponenten-Ebene; Toolbar-Icons
   existieren bereits.
8. **Play-in-Editor:** Welt-Snapshot (Serializer in Memory-Buffer) bei Play,
   Restore bei Stop — Buttons existieren bereits.

**Definition of Done:** Szene aus importierten Assets zusammenklicken,
speichern, Editor neu starten, weitermachen.

---

## Meilenstein 4 — Rendering-Features

Reihenfolge nach Sichtbarkeit-pro-Aufwand:

1. **FrustumCuller + RenderSorter** (Header existieren; AABB.h liegt in
   Core/Math) — Culling vor Sorting vor Submit.
2. **Beleuchtung:** Blinn-Phong zuerst, dann PBR (metallic/roughness aus
   glTF kommt in M2 gratis mit). LightData-Array als Uniform-Block.
3. **RenderGraph + Pass-System aktivieren:** GeometryPass → PostProcessPass;
   ShadowPass (Directional, eine Cascade) danach.
4. **GPUMemoryAllocator:** Budget + LRU-Eviction, Hooks in
   RenderResourceManager (`onHandleUsed` beim Draw).
5. **HDR + Tonemapping** als erster echter PostProcess-Pass.
6. **Instancing** (instanceCount im DrawCall existiert schon).

---

## Meilenstein 5 — Engine-Systeme

Parallelisierbar, jeweils eigenes Modul:

- **Scripting:** Lua zuerst (sol2, klein, embedded-freundlich) — die
  ScriptComponent/ScriptLanguage-Enums sind vorbereitet. Python/C# später
  oder nie.
- **Physics:** Jolt Physics (CMake-freundlich, modern) gegen
  RigidBodyComponent; fixedUpdate-Hook im GameLoop existiert.
- **Audio:** miniaudio (header-only) + AudioImporter (PCMD-Chunks sind im
  Format definiert).
- **Packaging:** HpakWriter/KeyDerivation implementieren, asset_compiler →
  hpak_packer-Kette, GameApplication lädt aus .hpak statt losen Dateien
  (SerializeFormat::Binary-Pfad).

---

## Querschnitt (nebenher, nicht als eigene Phase)

- **Memory:** `Ref<T>` (intrusive refcount) implementieren und im
  ContentManager nutzen — Voraussetzung für Asset-Unloading
  (KeepCPUAssets-Flag existiert schon, tut aber nichts). PoolAllocator erst
  bei nachgewiesenem Bedarf.
- **Tests:** Catch2 oder doctest; zuerst SlotMap, HAsset round-trip,
  SceneSerializer round-trip, ContentManager. CTest-Gerüst existiert im
  Build-Verzeichnis bereits.
- **CI:** GitHub Actions Matrix (macOS + Windows) sobald das Repo ein
  Git-Repo ist (aktuell nicht initialisiert!).
- **Aufräumen:** D3D11/D3D12-ShaderManager-Cleanup-TODOs; doppelte
  glm-Kopie (vendored in HE_Rendering/glm **und** FetchContent) —
  eine Quelle wählen.

## Empfohlene Reihenfolge der nächsten 5 Arbeitsschritte

1. UUID-Persistenz im META-Chunk fixen (klein, blockiert später alles).
2. CommandBuffer/RenderWorld/RenderObject implementieren.
3. OpenGL-Submit-Pfad + Unlit-Shader → hardcodiertes Dreieck.
4. Metal-Submit-Pfad nachziehen (Parität halten, solange der Code klein ist).
5. RenderExtractor + Kamera → ECS-Würfel sichtbar, dann Meilenstein 2 starten.
