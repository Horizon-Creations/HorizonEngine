# Editor Asset-Editing Plan — dedizierte Tabs & Asset-Inspector

Ziel: Für **jeden** Asset-Typ eine klare Bearbeitungs-Story im Editor. Aktuell haben nur
**Scene** und **Script** einen eigenen Tab, **Material** wird inline im Details-Panel editiert,
alles andere ist Import-/Create-only.

Dieses Dokument ist der Umsetzungsplan: pro Typ eine Empfehlung (eigener Tab vs.
Asset-Inspector vs. Import-Dialog), plus konkrete Dateien/Strukturen/Widgets/Save-Load,
plus phasierte Reihenfolge.

---

## 1. Ist-Zustand (verifiziert)

| Asset (`AssetType`) | Daten-Struct | Bearbeitung heute | Evidenz |
|---|---|---|---|
| StaticMesh | `StaticMeshAsset` (Assets.h:23) | Import-only, Referenz-Slot | EditorUI.cpp:4143 |
| SkeletalMesh | `SkeletalMeshAsset` (Assets.h:52) | Import-only | EditorUI.cpp:4922 |
| Texture | `TextureAsset` (Assets.h:124) | Import-only, **keine** Import-Settings-UI | EditorUI.cpp:2073 |
| **Material** | `MaterialAsset` (Assets.h:65) | **Inline im Details-Panel** (PBR-Scalars, Textur-Slots, Save) | EditorUI.cpp:5305-5395 |
| **Scene** | `SceneAsset` (Assets.h:90) | **Eigener Tab** (Viewport/Outliner/Inspector) | EditorUI.cpp:4139 |
| **Script** | `ScriptAsset` (Assets.h:96) | **Eigener Tab** (Syntax-Highlight-Editor) | ScriptEditorPanel.h |
| Audio | `AudioAsset` (Assets.h:105) | Import-only | EditorUI.cpp:2075 |
| Font | `FontAsset` (Assets.h:112) | Create-Stub, **kein Importer** | EditorUI.cpp:4480 |
| Shader | `ShaderAsset` (Assets.h:142) | Create-Stub, **kein Editor** (Quelle in `CHUNK_SRC`) | EditorUI.cpp:4478 |
| Prefab | `PrefabAsset` (Assets.h:151) | „Save as Prefab" im Outliner, **kein Edit-Modus** | EditorUI.cpp:3627 |
| AnimationClip | `AnimationClipAsset` (Assets.h:175) | Import-only, Playback am Component | EditorUI.cpp:5008 |
| PropertyAnimClip | `PropertyAnimClipAsset` (Assets.h:202) | **komplett ungenutzt** in der UI | — |

Terrain/Landscape ist **kein** Asset-Typ, sondern ein Component-System mit eigenem
Landscape-Modus (Sculpting) — bleibt außerhalb dieses Plans.

---

## 2. Tab-Infrastruktur (Extension-Point)

- `EditorTab { std::string label; std::string assetPath; bool closable; bool open; }`
  (EditorApplication.h:192). `assetPath.empty()` == Scene-Tab.
- Tab-Gating: EditorUI.cpp:2490-2506 — bei nicht-leerem `assetPath` wird statt
  Viewport/Panels der `ScriptEditorPanel::render(...)` gezeichnet + **early return**.
- Double-Click-Dispatch: EditorUI.cpp:4136-4163 — `.hescene` → Szene öffnen;
  `isScriptAsset` → Tab anlegen/finden + `s_tabSelectRequest`; sonst No-op.
- `ScriptEditorPanel`-Muster: `g_states` (map keyed by `assetPath`), lazy `loadFromDisk`
  über `HAsset::Reader`/`CHUNK_SRC`, `saveToDisk` erhält alle anderen Chunks,
  `isDirty` via Undo-Index. **Dieses Muster ist die Blaupause für jeden neuen Tab.**

**Schwachstelle:** Dispatch + Gating sind hartcodierte `if (isScriptAsset)`-Ketten.
Vor dem dritten Tab lohnt eine kleine Registry (siehe Phase 0).

---

## 3. Empfehlung pro Asset-Typ

Entscheidungsregel: **eigener Tab**, wenn großer Canvas / eigenes Werkzeug (Code, 3D-Preview,
Timeline, Sub-Szene). **Asset-Inspector** (Details-Panel bei Einfach-Klick auf ein Asset),
wenn es nur wenige Properties / Import-Settings / kleine Preview sind.

| Asset | Empfehlung | Begründung | Aufwand |
|---|---|---|---|
| **Shader** | **ENTFÄLLT** (User-Entscheidung 2026-07-05): Shader sind kein Asset-Typ mehr — sie werden vom Node-Editor im **Material** generiert (siehe `material-system-design.md` §1b). `ShaderAsset` wird ausgemustert, kein Shader-Editor-Tab | — | — |
| **Material** | **eigener Track** → siehe `material-system-design.md` (Node-Editor wie Unreal + Cross-Backend-Codegen). Material-Editor-Tab = dessen UI-Frontend ab M2/M3 | meistgenutztes Asset; wird zur Shader-Abstraktion, nicht bloß PBR-Panel | **XL** |
| **Texture** | **Viewer-Tab** (Bild-Canvas, Zoom/Pan, Kanal-Toggle, Mip-Slider) + **Asset-Inspector** für Import-Settings | Doppelklick = Bild groß ansehen ist natürlich | **M** |
| **StaticMesh / SkeletalMesh** | **Viewer-Tab** (3D-Orbit-Preview, Stats, Material-Slots, LOD, Skelett) | Ansehen/Prüfen; Geometrie kommt aus DCC | **M–L** |
| **Audio** | **Asset-Inspector** (Waveform + Play + Import-Info) | zu leicht für einen Tab | **S** |
| **Font** | **Asset-Inspector** (Glyph-Preview bei Größe) + **TTF-Importer nachrüsten** | leicht; schließt fehlende Importer-Lücke | **S** |
| **AnimationClip** | **Animation-Editor-Tab** (Timeline/Dopesheet) | braucht echten Timeline-Canvas | **XL** |
| **PropertyAnimClip** | **derselbe** Animation-Editor-Tab (Property-Track-Modus) + Create surface | zusammenlegen | (mit oben) |
| **Prefab** | **Prefab-Editor-Tab** (isolierte Mini-Szene, reused Outliner/Viewport/Inspector) | echtes verschachteltes Editieren | **L** |
| Scene / Script | — | bereits erledigt | — |

---

## 4. Gemeinsame Infrastruktur (zuerst — macht den Rest billig)

### 4.1 Tab-Dispatch-Registry
Die `isScriptAsset`-Kette an **zwei** Stellen (Double-Click 4136-4163, Gating 2490-2506)
durch eine Tabelle ersetzen:

```cpp
struct AssetEditorKind {
    bool (*matches)(const std::string& path);                 // isScriptAsset, isShaderAsset, ...
    void (*render)(AppContext&, const std::string&, ImVec2, ImVec2);
    bool (*isDirty)(const std::string&);
};
static const AssetEditorKind kAssetEditors[] = { … };
```

- `EditorTab` um `HE::AssetType type` erweitern, damit das Gating den Typ nicht jedes
  Frame neu vom Datenträger schnüffelt (heute ruft `isScriptAsset` pro Frame `HAsset::Reader`).
- Double-Click: erste passende `matches`-Regel → Tab öffnen (bestehende Find/Create/
  `s_tabSelectRequest`-Logik generisch machen).

### 4.2 Asset-Inspector (Rückgrat für die „leichten" Typen)
Einfach-Klick auf ein Asset im Content-Browser → **Details-Panel zeigt die Asset-eigenen
Properties** (statt/zusätzlich zur selektierten Entity). Unity/Unreal-Konvention.
Deckt in einem Aufwasch ab: Texture-Import-Settings, Audio, Font, Mesh-Info, Material-Quick-Edit.

- Neuer Zustand in `AppContext`: `std::string selectedAssetPath;` (leert Entity-Selektion-
  Anzeige nicht, sondern schaltet den Details-Header um).
- `AssetInspector::render(ctx)` — schaltet per `HAsset`-Typ auf die passende Sektion.
- Save-Pfad identisch zum ScriptEditorPanel (Reader→Writer, betroffenen Chunk ersetzen).

### 4.3 Offscreen-Asset-Preview-Renderer
Helper, der **ein** Asset (Mesh, oder Material auf Standard-Kugel) in ein kleines
Offscreen-Color-Target rendert und eine `ImTextureID` für `ImGui::Image` liefert — den
per-Backend-Binding-Pfad gibt es bereits (`GetViewportTexture()` → `ImGui::Image`,
EditorUI.cpp:2671). Einmal bauen (Metal + GL), dann teilen Material/Mesh/Texture-Preview.

- Neue Renderer-API: `void* RenderAssetPreview(const PreviewRequest&)` (eigenes RT,
  Orbit-Kamera, ein Key-Light). Metal zuerst, GL-Parität.

---

## 5. Neue Tabs — konkrete Umsetzung (ScriptEditorPanel-Blaupause)

Jeder Tab = neues Dateipaar `src/HE_Editor/<Name>Panel.{h,cpp}` mit
`namespace <Name>Panel { render(ctx, assetPath, pos, size); isDirty(path); is<Type>Asset(path); forget(path); }`,
`g_states`-Map, HAsset-Load/Save. Registrierung in der Dispatch-Registry (4.1).

### 5.1 ShaderEditorPanel (XS)
- Praktisch eine Kopie von `ScriptEditorPanel`. `SetLanguageDefinition(Glsl)` bzw. `Hlsl`
  (heuristisch nach Endung/Backend; Default GLSL). Load/Save über `CHUNK_SRC` — identisch.
- Optional: „Compile"-Button, der den Shader durch `glslangValidator`/`xcrun metal`-
  Validierung schickt und Fehler unter dem Editor listet (später).
- **Alternativ**: `ScriptEditorPanel` generalisieren zu `TextAssetEditorPanel` und Script +
  Shader teilen sich einen Editor (empfohlen — weniger Duplikat).

### 5.2 MaterialEditorPanel (M)
- Links: Preview-Kugel (4.3). Rechts: Shader-Pfad (Drag-Drop), BaseColor (`ColorEdit3`),
  Metallic/Roughness/Opacity (`SliderFloat`), DoubleSided (`Checkbox`), Textur-Slots mit
  **Thumbnails** (Drag-Drop + Clear) — dieselben Felder wie heute inline (EditorUI.cpp:5335-5379),
  nur mit Preview und mehr Platz.
- Load/Save: `MaterialAsset`-Felder liegen in eigenen Chunks (nicht `CHUNK_SRC`) — Writer
  ersetzt die Material-Chunks, erhält Rest. Live-Apply wie heute (EditorUI.cpp:5391).
- Inline-Editing im Component-Details **bleibt** als Quick-Edit; Doppelklick öffnet den Tab.

### 5.3 TextureViewerPanel (M)
- Bild-Canvas: `ImGui::Image(GetOrUploadPreview(path))`, Zoom (Mausrad) + Pan (MMB),
  Kanal-Toggle (R/G/B/A als Shader-Uniform oder 4 separate Uploads), Mip-Level-Slider,
  Alpha-Schachbrett-Hintergrund.
- Import-Settings (sRGB, Kompression ASTC/RGBA8, Mip-Generierung) → in den **Asset-Inspector**
  (4.2), da sie beim (Re-)Cook greifen, nicht am geladenen Pixelbuffer.

### 5.4 MeshViewerPanel (M–L)
- 3D-Orbit-Preview (4.3), Wireframe-Toggle, Stats (Tris/Verts/Submeshes/Bounds), Material-
  Slot-Liste (Drag-Drop), LOD-Auswahl, bei SkeletalMesh Skelett-Overlay + Bone-Count.
- Read-only bzgl. Geometrie (kein Mesh-Editing) — reiner Viewer/Inspector.

### 5.5 PrefabEditorPanel (L)
- Doppelklick lädt `PrefabAsset` (CBOR-Subtree) in eine **temporäre `HorizonWorld`** und
  rendert einen vollwertigen Mini-Szenen-Editor (Outliner + Viewport + Inspector der
  bestehenden `EditorUI`-Panels, aber gegen die Prefab-Welt). „Save" serialisiert zurück.
- Erfordert: die Panels gegen eine übergebene Welt parametrisierbar machen (heute implizit
  gegen `m_editorWorld`). Größter Umbau, aber maximaler Reuse.

### 5.6 AnimationEditorPanel (XL)
- Timeline/Dopesheet: Zeitleiste, Keyframe-Marker pro Channel (`AnimationClipAsset.channels`
  = per-Joint Translation/Rotation/Scale; `PropertyAnimClipAsset` = 15 Scalar-Targets),
  Keyframe hinzufügen/verschieben/löschen, optional Kurven-Editor (Tangenten).
- Scrub-Preview gegen ein Test-Mesh (AnimationClip) bzw. Live-Property-Anwendung
  (PropertyAnimClip). Höchster Aufwand — eigenes ImGui-Custom-Drawing.
- Zieht `PropertyAnimClip` erstmals in die UI (Create-Menü + Editor).

---

## 6. Phasierung (jede Phase eigenständig auslieferbar)

**Phase 0 — Infra:** Dispatch-Registry (4.1) + `EditorTab.type` + Asset-Inspector-Skelett (4.2).
→ danach kostet jeder neue Tab nur noch das Panel selbst.

**Phase 1 — ENTFÄLLT** (war: Shader-Editor). Shader sind kein Asset-Typ mehr; Shader-Autoring
läuft ausschließlich über den Material-Node-Editor (material-system-design.md M3).

**Phase 2 — Asset-Inspector füllen (S–M):** Texture-Import-Settings, Audio (Waveform+Play),
Font (Glyph-Preview **+ TTF-Importer nachrüsten**), Mesh-Info, Material-Quick-Edit hierher.

**Phase 3 — Preview-Renderer + Preview-Tabs (M–L):** Offscreen-Renderer (4.3) → Material-
Editor-Tab (5.2) + Texture-Viewer (5.3) + Mesh-Viewer (5.4).

**Phase 4 — Prefab-Editor (L):** Panels welt-parametrisierbar machen → Prefab-Mini-Szene (5.5).

**Phase 5 — Animation-Editor (XL):** Timeline/Dopesheet für AnimationClip + PropertyAnimClip (5.6).

---

## 7. Offene Verifikationen vor Umsetzung
- Material-Chunk-Layout in `HAsset` prüfen (welche Chunk-IDs `MaterialAsset` schreibt) für
  den Writer-Erhalt.
- Font-Import: TTF/OTF-Loader-Bibliothek wählen (stb_truetype ist wahrscheinlich schon via
  ImGui vorhanden).
- Preview-Renderer: prüfen, ob `MetalRenderer`/`RendererOpenGL` ein zweites, kleines RT
  neben dem Haupt-Viewport erlauben (Command-Buffer/FBO-Isolation).
- Prefab-Panels: Aufwand, `EditorUI`-Panels von `m_editorWorld` zu entkoppeln.
