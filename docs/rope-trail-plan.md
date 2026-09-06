# Rope & Trail Rendering: Spline-Tube/Ribbon-Geometrie

Stand 06.09.2026, Branch `claude/rope-trail-rendering`. Schritt 1 des Themas:
Kartierung der vorhandenen Bausteine, Festlegung des Geometrie-Ansatzes, Entwurf
von `RopeComponent`/`TrailComponent` und der Renderintegration. **Kein
Feature-Code in diesem Schritt.**

Ziel des Features: Seile, Ketten, Grapple-Lines, Kabel und Waffen-/Fahrzeug-Trails
aus einer Spline. Es hängt an nichts als dem fertigen PBR-Pfad und überschneidet
sich mit keinem anderen laufenden Zweig.

Bezug: `docs/deferred-renderer-plan.md`, `docs/backend-parity-plan.md`,
`docs/gap-audit-2026-08-25.md`.

---

## 1. Was schon da ist

Zwei fertige Muster im Baum decken zusammen alles ab, was dieses Feature braucht.
Der Plan besteht im Wesentlichen daraus, das jeweils richtige zu wählen.

### 1.1 Muster A — prozedurales Mesh als Runtime-Asset (Terrain)

`TerrainSystem::buildChunk` (`src/HE_Scene/src/TerrainSystem.cpp:114-169`) baut
Chunk-Meshes auf der CPU und schleust sie so in den Renderer:

```cpp
StaticMeshAsset m = generateTerrainChunkMesh(...);
if (haveLevels && cm.getStaticMesh(lod->levels[k].meshId) != nullptr) {
    id = lod->levels[k].meshId;          // UUID behalten
    cm.replaceStaticMesh(id, std::move(m));
    if (renderer) renderer->InvalidateMesh(id);
} else {
    id = cm.registerStaticMesh(std::move(m));
}
```

API: `ContentManager::registerStaticMesh` (`ContentManager.h:136`),
`replaceStaticMesh` (`:176`), `IRenderer::InvalidateMesh`
(`src/HE_Core/include/Renderer/IRenderer.h:648`).

**Das Entscheidende: `InvalidateMesh` ist in allen fünf Backends überschrieben**
(Metal, GL, Vulkan, D3D11, D3D12 — je in deren Header). Ein prozedurales Mesh ist
damit ohne eine Zeile Backend-Code cross-backend, und es läuft durch den
kompletten normalen Pfad: PBR, Material-Node-Graph, Schatten, SSAO, GI, Sortierung.

### 1.2 Muster B — CPU-Geometrie pro Frame ohne Asset (Debug-Linien)

`DebugDrawBuffer` (`src/HE_Core/include/DebugDraw/DebugDraw.h`) sammelt Linien auf
der CPU, `IRenderer::SetDebugLines` (`IRenderer.h:686`) reicht sie durch, jedes
Backend lädt sie pro Frame in einen eigenen dynamischen Vertexbuffer und zeichnet
sie mit einer eigenen Mini-Pipeline (GL: `OpenGLRenderer.cpp:9400` plus
`CreateDebugLinePipeline`). Auch dieser Weg ist in allen fünf Backends vorhanden.

Kein Asset, keine UUID, keine Cache-Invalidierung, kein GI. Dafür: eigener Shader,
eigene Pipeline, kein Material-System.

### 1.3 Was es nicht gibt

* Keine Spline-, Kurven- oder Ribbon-Hilfsmittel im Baum. `glm/gtx/spline.hpp`
  liegt zwar vendort, ist aber ohne `GLM_ENABLE_EXPERIMENTAL` nicht benutzbar und
  `Math/Math.h` setzt das nicht — die zehn Zeilen Catmull-Rom schreiben wir selbst,
  statt ein Experimental-Define durch die halbe Engine zu ziehen.
* Kein Vertex-Farb-Attribut (siehe §3.2).
* Keine Seil-Physik. Dieses Thema baut Geometrie und Rendering, nicht Simulation
  (siehe §9).

---

## 2. Der Befund, der den Plan bestimmt: zwei Datenraten, zwei Wege

Seil und Trail sehen nach demselben Problem aus (Spline rein, Bandgeometrie raus),
unterscheiden sich aber in genau einer Eigenschaft, und die entscheidet alles:

| | Rope | Trail |
|---|---|---|
| Geometrie ändert sich | wenn jemand sie ändert (Editor, Anhängepunkt bewegt sich) | **jeden Frame** |
| Lebensdauer | so lange die Entity lebt | Ringpuffer, Punkte verfallen |
| Beleuchtung | voll, wirft Schatten | meist selbstleuchtend, wirft keinen Schatten |
| Größenordnung | ein paar Dutzend im Level | ein paar Dutzend gleichzeitig, kurz |

Muster A für einen Trail zu nehmen wäre falsch, und zwar nicht „ein bisschen
teuer", sondern kaputt. `MetalRenderer::InvalidateMesh`
(`MetalRenderer.mm:7822`) stellt die UUID in eine Liste, die der Renderloop
abarbeitet (`MetalRenderer.mm:13786-13807`), und dort passiert das:

```cpp
if (it->second.blas) CFBridgingRelease(it->second.blas);
m_meshCache.erase(it);
...
if (m_giSwBlasCache.count(id)) {
    m_giSwBlasCache.clear();  m_giSwNodesCpu.clear();
    m_giSwTrisCpu.clear();    m_giSwBlasDirty = true;
}
```

Also: HW-BLAS freigeben und neu bauen, und — weil die SW-RT-BVH-Ranges in
konkatenierten Arrays liegen und ein einzelnes Mesh nicht herausgeschnitten werden
kann — **der komplette Software-BVH-Cache wird verworfen**. Der GL-Port hält es
laut Kommentar genauso. Ein Trail, der jeden Frame `replaceStaticMesh` +
`InvalidateMesh` ruft, reißt bei aktivem DDGI jeden Frame den GI-Beschleuniger der
ganzen Szene ein. Das ist keine Mikrooptimierung, das ist der Unterschied zwischen
lauffähig und nicht.

**Entscheidung.**

* **Rope → Muster A.** Runtime-`StaticMeshAsset`, `InvalidateMesh` nur bei echter
  Änderung. Volle PBR-Beleuchtung, Schatten, GI, Materialgraph. Null Backend-Arbeit.
* **Trail → Muster B, erweitert.** Neue `RibbonBatch`-Liste in der `RenderWorld`
  mit fertigen CPU-Vertices, pro Frame vom Backend in einen dynamischen
  Vertexbuffer geschoben. Kein Asset, keine UUID, kein BLAS, keine
  Cache-Invalidierung.

Damit ist die Gabelung, die sonst in Schritt 3/4 aufschlagen würde, hier
aufgelöst: Schritt 3 und 4 sind **nicht** „nichts zu tun". Der Rope-Teil ist
nichts zu tun, der Trail-Teil ist pro Backend eine Pipeline in der Größenordnung
der Debug-Linien-Pipeline.

---

## 3. Der Geometrie-Kern

Neues Modul ohne Renderer-Abhängigkeit, damit es testbar bleibt und der Editor es
für die Vorschau mitbenutzen kann:

```
src/HE_Scene/include/HorizonScene/SplineGeometry.h
src/HE_Scene/src/SplineGeometry.cpp        (namespace HE::spline)
```

### 3.1 Die vier Stufen

1. **Kurve.** Zentripetale Catmull-Rom durch die Kontrollpunkte (α = 0.5). Die
   zentripetale Parametrisierung, nicht die uniforme: uniform erzeugt bei eng
   stehenden Punkten Schleifen und Überschwinger, und genau das passiert an einem
   Seil, das jemand im Editor zusammenschiebt. Endpunkte werden gespiegelt
   verdoppelt (Phantom-Punkte), damit das erste und letzte Segment definiert sind.
2. **Bogenlängen-Resampling.** Erst dicht abtasten, Bogenlängentabelle bauen,
   dann auf `N` gleich lange Stücke umtasten. Ohne das sitzen die Ringe dort dicht,
   wo die Kurve krümmt, und die UV-Kachelung läuft sichtbar aus dem Takt.
3. **Frames.** Rotationsminimierende Frames (Parallel Transport, Doppelreflexion
   nach Wang et al.) — **niemals Frenet**. Der Frenet-Frame ist auf einem geraden
   Abschnitt undefiniert (Krümmung 0) und kippt am Wendepunkt um 180°; ein Seil,
   das mittendrin einmal in sich verdreht ist, ist genau dieser Fehler. Startframe:
   die Up-Achse der Entity, orthogonalisiert gegen die Tangente; ist sie parallel,
   wird auf die Weltachse mit dem kleinsten |dot| ausgewichen.
4. **Auffädeln.**
   * **Tube**: pro Sample ein Ring aus `radialSegments` Punkten im Frame,
     benachbarte Ringe zu einem Quadstreifen verbunden. Normale = radial nach
     außen. Optional Kappen an den Enden.
   * **Ribbon**: pro Sample zwei Punkte, `± width/2` entlang der Breitenachse.
     Breitenachse entweder aus dem RMF (`alignment = Frame`, für ein flaches Band
     mit fester Lage) oder `normalize(cross(tangent, cameraPos - p))`
     (`alignment = Camera`, das Übliche für Waffen-Trails). `SceneSystems::tickWorld`
     reicht `cameraPos` schon durch, die Kameraausrichtung kostet also nichts extra.

### 3.2 Der Zwang, der das Datenmodell formt: das Vertex-Layout ist fix

`StaticMeshAsset` (`src/HE_Core/include/ContentManager/Assets.h:77-95`) kennt
`vertices`/`normals`/`uvs` als SoA plus die gekochte Interleaved-Form
`pos3 + norm3 + uv2`. **Es gibt kein Vertex-Farb- und kein Vertex-Alpha-Attribut**,
und eines einzuführen hieße, jedes Backend-Vertexformat, den Packer und das
Cooked-Format anzufassen. Das steht in keinem Verhältnis.

**Festlegung: das Alter läuft über die UV-Koordinate.**

* `uv.u` = Position **quer** zum Band (0…1 über den Ringumfang bzw. über die
  Ribbon-Breite).
* `uv.v` = Position **längs** — bei einem Seil die Bogenlänge geteilt durch
  `uvTileLength` (kachelt, damit eine Seiltextur mitläuft), bei einem Trail das
  normierte Alter 0 (Spitze, jüngster Punkt) … 1 (Schwanzende, gleich tot).

Ein Material-Graph macht daraus Deckkraft, Farbverlauf oder Ausblenden. Das ist
die Stelle, an der Trails über das Materialsystem konfiguriert werden statt über
Component-Felder — konsistent mit dem Partikel-Weg, wo die Farbe-über-Leben
ebenfalls im Graph sitzt und nicht im Component.

Konsequenz fürs Datenmodell: `TrailComponent` braucht **kein** Start-/Endfarbfeld.
Es braucht ein Material, dessen Graph `uv.v` liest.

### 3.3 Rückseiten

Die Szenenpässe zeichnen ohne Backface-Culling: D3D11 setzt
`D3D11_CULL_NONE` mit dem Kommentar „meshes aren't guaranteed a consistent
winding" (`D3D11Renderer.cpp:2623`), D3D12 ebenso, GL aktiviert `GL_CULL_FACE`
ausschließlich im Decal-Projektor-Pass (`OpenGLRenderer.cpp:11078`) und Metal
setzt `MTLCullModeFront` nur im Schattenpass. `MaterialAsset::doubleSided`
(`Assets.h:141`) existiert, wird aber von keinem Backend gelesen.

Also: **ein Ribbon ist von hinten sichtbar, ohne dass irgendetwas dafür getan
werden muss.** Was von hinten *nicht* stimmt, ist die Normale — sie zeigt weg, die
Rückseite schattiert dunkel. Zwei Auswege, beide ohne Shader-Änderung:

* Trails sind in aller Regel selbstleuchtend/emissiv. Dann ist die Normale egal.
  **Das ist der Standardweg** und braucht gar nichts.
* Für ein beleuchtetes Band (Fahne, Gurt): Flag `twoSidedGeometry` am Component,
  das die Bandfläche ein zweites Mal mit umgekehrter Wicklung und gespiegelter
  Normale erzeugt. Doppelte Dreiecke, funktioniert überall, keine Pipeline-Arbeit.

Ein Tube ist eine geschlossene Fläche — dort stellt sich die Frage nicht.

---

## 4. Die Components

Beide neu unter `src/HE_Scene/include/HorizonScene/Components/`. Einrückung nach
der Nachbardatei im selben Verzeichnis (`DecalComponent.h` Tabs,
`ParticleSystemComponent.h` Leerzeichen — es gibt dazu keine Regel in
`docs/coding-conventions.md`, also nicht vereinheitlichen, nur nicht mischen).

### 4.1 `RopeComponent`

```cpp
enum class RopeShape : uint8_t { Tube = 0, Ribbon = 1 };

struct RopeComponent
{
    bool      visible = true;

    // Autorenseite ------------------------------------------------------
    // Kontrollpunkte im LOKALEN Raum der Entity. Mindestens zwei; darunter
    // wird nichts erzeugt.
    std::vector<glm::vec3> controlPoints { {0,0,0}, {0,-1,0} };
    // Optionale Anhängepunkte: ist eine gesetzt, ersetzt die WELT-Position
    // dieser Entity (nach lokal zurückgerechnet) den ersten bzw. letzten
    // Kontrollpunkt. So hängt ein Grapple zwischen zwei bewegten Objekten.
    HE::UUID  attachStart, attachEnd;   // EntityIdComponent-UUID, {} = keiner
    // Durchhang in Metern, senkrecht nach unten auf die Sehne addiert
    // (Näherung einer Kettenlinie, kein Solver). 0 = straff.
    float     sag = 0.0f;

    RopeShape shape          = RopeShape::Tube;
    float     radius         = 0.05f;  // Tube: Radius. Ribbon: halbe Breite.
    int       radialSegments = 8;      // Tube; Ribbon ignoriert es
    int       samplesPerSpan = 8;      // Auflösung längs, pro Kontrollpunktpaar
    float     uvTileLength   = 1.0f;   // Meter pro UV-V-Kachel
    bool      twoSidedGeometry = false; // §3.3
    bool      castsShadow      = true;

    HE::UUID  materialAssetId;

    // Laufzeit — NICHT serialisiert ------------------------------------
    // Das erzeugte Runtime-Mesh. Wird beim ersten Tick registriert und
    // danach nur noch ersetzt; siehe §6.1, warum das kein Detail ist.
    HE::UUID  runtimeMeshId;
    // Hash über alles, was die Geometrie bestimmt, inklusive der aktuellen
    // Weltpositionen der Anhängepunkte. Ungleich → neu bauen.
    uint64_t  builtHash = 0;
};
```

### 4.2 `TrailComponent`

```cpp
enum class TrailAlignment : uint8_t { Camera = 0, Frame = 1 };

struct TrailComponent
{
    bool  visible  = true;
    bool  emitting = true;   // false = nichts Neues, Bestehendes läuft aus

    float lifetime          = 0.5f;   // Sekunden bis ein Punkt verfällt
    float minVertexDistance = 0.05f;  // Meter, bevor ein neuer Punkt fällt
    int   maxPoints         = 64;     // Ringpuffergröße
    float startWidth        = 0.2f;   // an der Spitze (jüngster Punkt)
    float endWidth          = 0.0f;   // am Schwanzende
    TrailAlignment alignment = TrailAlignment::Camera;

    HE::UUID materialAssetId;         // Graph liest uv.v = Alter (§3.2)

    // Laufzeit — NICHT serialisiert ------------------------------------
    struct Point { glm::vec3 worldPos; float age; };
    std::vector<Point> points;        // Ringpuffer, Spitze zuletzt
    glm::vec3 lastEmitPos {};
    bool      hasLastEmit = false;
};
```

Der Trail hält **Weltpositionen**. Das ist der ganze Sinn eines Trails: er bleibt
liegen, wo er war, während sich die Entity weiterbewegt.

---

## 5. Das System

```
src/HE_Scene/include/HorizonScene/RopeTrailSystem.h
src/HE_Scene/src/RopeTrailSystem.cpp
```

```cpp
namespace RopeTrailSystem {
    void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                const glm::vec3& cameraPos, float dt);
}
```

Einhängepunkt: `SceneSystems::tickWorld`, nach `ParticleSystem`
(`src/HE_Scene/src/SceneSystems.cpp:132`) und vor `LOD` (`:134`), mit eigenem
`HE_PROFILE_SCOPE_N("RopeTrail")` wie die Nachbarn. Nach Particle, weil beides
Effektgeometrie ist und die Reihenfolge untereinander egal ist; vor LOD, weil LOD
Meshes tauscht und nichts von uns danach kommen sollte.

Zwei Dinge sind vor Schritt 2 zu prüfen, nicht anzunehmen:

* **Läuft `tickWorld` im Editor auch ohne Play?** Ein Seil muss im Viewport
  stehen, während man es baut. `TerrainSystem` hat dasselbe Problem und löst es;
  dessen Editor-/Play-Behandlung ist die Vorlage.
* **Ruft `GameApplication` `tickWorld` überhaupt?** Beim Wetter-System war genau
  das die Lücke. Nachsehen, bevor jemand meldet, dass im gepackten Build nichts
  wackelt.

---

## 6. Renderintegration

### 6.1 Rope — Runtime-Mesh, drei Regeln, die keine Kür sind

**Regel 1: einmal registrieren, danach nur ersetzen.** `registerStaticMesh` hängt
an einen dichten Vektor an; die nächste Registrierung/Ladung macht **alle**
`ContentManager`-Zeiger ungültig, samt der Strings, die sie besitzen. Jeder Frame
eine neue Registrierung ist also nicht nur ein Leck, sondern ein Zeiger-Minenfeld
für alles, was in derselben Schleife noch einen `getStaticMesh`-Zeiger hält.
`runtimeMeshId` leer → registrieren; sonst `replaceStaticMesh` + `InvalidateMesh`,
genau wie `TerrainSystem::buildChunk`. Beim Zerstören der Entity die
Mesh-Registrierung wieder loswerden.

**Regel 2: `runtimeMeshId` wird nie serialisiert und gehört nicht in
`collectAssetRefs`.** Die Mesh-UUID ist Laufzeitzustand einer prozeduralen
Geometrie; in `SceneSystems::collectAssetRefs`
(`src/HE_Scene/src/SceneSystems.cpp:201`) eingetragen würde der Packer nach einer
Datei suchen, die es nicht gibt. `materialAssetId` **muss** dagegen dort hinein,
sonst fehlt im gepackten Build das Material.

**Regel 3: kein `MeshComponent`, kein `LODComponent`.** Das Seil bekommt einen
eigenen Extraktor `extractRopes` in `RenderExtractor.cpp`, gebaut wie
`extractDecals` (`:447`, Aufruf `:756`), der ein `RenderObject` schiebt:
`meshAssetId = runtimeMeshId`, `materialAssetId` aus dem Component,
`transform` = Weltmatrix der Entity, `worldBounds` aus den erzeugten Bounds,
`castsShadow` aus dem Component. Über `MeshComponent` zu gehen hieße, sich mit
LOD-Swaps, Materialauflösung und Editor-UI um dieselbe Entity zu streiten.

### 6.2 Trail — `RibbonBatch`

Neu in `RenderWorld.h`, in der Nachbarschaft von `ParticleBatch` und `DecalData`:

```cpp
// Ein Trail, fertig trianguliert in WELTKOORDINATEN. Anders als ein Rope
// existiert das hier bewusst NICHT als Asset: die Geometrie ändert sich jeden
// Frame, und ein Runtime-Mesh würde pro Frame den BLAS- und den SW-BVH-Cache
// der Szene wegwerfen (siehe MetalRenderer.mm:13786). Die Backends laden es
// wie die Debug-Linien in einen dynamischen Vertexbuffer.
struct RibbonBatch {
    HE::UUID           materialAssetId;
    std::vector<float> vertices;   // vertexCount * 8: pos3 + norm3 + uv2
    std::vector<uint32_t> indices;
    uint32_t           entityId = 0;
    HE::AABB           worldBounds;
};
std::vector<RibbonBatch> ribbonBatches;
```

Vertices in Weltkoordinaten, das Modell ist also die Identität. Trails setzen
`castsShadow = false` und `contributesAO = false` sinngemäß — sie erscheinen
weder im Schattenpass noch im SSAO-Vorpass noch in einer
Beschleunigungsstruktur, aus demselben Grund wie die Niederschlags-Billboards.

Pro Backend zu bauen (Schritte 3 und 4): eine Pipeline, ein dynamischer
Vertex-/Indexbuffer mit Ring-/Wachstumsstrategie, ein Draw pro Batch im
Transparenzabschnitt nach den Opaken. Die Debug-Linien-Implementierung desselben
Backends ist jeweils die nächstliegende Vorlage; für den Shader ist es der
gewöhnliche Materialpfad, das Vertexformat ist absichtlich identisch zum
Mesh-Format, damit derselbe Vertexshader benutzbar bleibt.

### 6.3 Frischezwang: `worldMatrix` ist einen Frame alt

`tc.worldMatrix` schreibt **nur** `propagateTransforms`, und in
`SceneSystems::tickWorld` läuft das nicht. Für alles, was in diesem Frame
entstanden oder bewegt worden ist, steht dort die Identität. Das hat in der
Physik, im ParticleSystem, im LODSystem und in der GameApplication je einmal
zugeschlagen.

Für dieses Feature betrifft es zwei Stellen, und beide sind zentral:

* Die Weltposition der **Anhängepunkte** eines Seils.
* Die Emissionsposition eines **Trails** — die Entity, an der der Trail hängt,
  ist per Definition die, die sich gerade bewegt.

Beide gehen über `HE::worldPositionOf` / `worldMatrixOf`, nicht über
`tc.worldMatrix`. `extractRopes` darf `t.worldMatrix` lesen (der Extraktor läuft
nach der Propagation, alle anderen Extraktoren tun es auch); das *System* darf es
nicht.

Für die Anhängepunkte ist außerdem zu prüfen, wie andere Components auf **fremde
Entities** verweisen (`CameraRigComponent`, `NavAgentComponent`) — dieselbe
Konvention übernehmen, keine neue erfinden.

---

## 7. Die Schritte 2 bis 5

### Schritt 2 — Geometriekern + Components + Tests

* `SplineGeometry.h/.cpp`, `RopeComponent.h`, `TrailComponent.h`,
  `RopeTrailSystem.h/.cpp`, Einhängung in `SceneSystems::tickWorld`.
* `SceneSerializer`: Speichern (`SceneSerializer.cpp:302` als Muster), Laden
  (`:846`), **und der Liste bekannter Component-Schlüssel** (`:1748`) — die wird
  gern vergessen. `runtimeMeshId`/`points` bleiben draußen (§6.1).
* `collectAssetRefs` (`SceneSystems.cpp:201`): nur die Material-UUIDs.
* Tests (`tests/test_rope_trail.cpp`, in `tests/CMakeLists.txt` eintragen):
  * RMF kippt nicht: über eine S-Kurve **und** über eine gerade Strecke ist
    `dot(frame[i], frame[i+1]) > 0` für alle i — das ist der Test, der den
    Frenet-Fehler fängt.
  * Bogenlängen-Resampling: Abstände aufeinanderfolgender Samples gleich bis auf
    Toleranz, Parameter monoton.
  * Vertex-/Indexanzahl gegen die Formel für Tube und Ribbon; Bounds enthalten
    alle Kontrollpunkte.
  * `uv.v` läuft am Trail monoton von 0 nach 1 und trifft die Enden.
  * `twoSidedGeometry` verdoppelt die Dreiecke und die zweite Lage hat die
    gespiegelte Normale.
  * Trail: `minVertexDistance` unterdrückt Punkte, `lifetime` entfernt sie,
    `maxPoints` deckelt.
  * **Ein-Registrierungs-Test**: nach N Updates mit geänderten Parametern ist
    `runtimeMeshId` unverändert und die Zahl der Runtime-Meshes im ContentManager
    genau um eins gewachsen. Das ist der Test für Regel 1 aus §6.1.

### Schritt 3 — Metal + GL

* `extractRopes` (Rope, ohne Backend-Arbeit — vorhandener Mesh-Pfad),
  `extractTrails` (füllt `ribbonBatches`), beide in `RenderExtractor.cpp` neben
  `extractDecals`, Aufrufe bei `:752-758`.
* `RibbonBatch` in `RenderWorld.h`, `clear()` erweitern.
* Metal: Pipeline + dynamischer Buffer + Draw. GL: dasselbe.
* **Abnahme:** `scripts/he_shot.py` auf Metal mit einem Tube und einem Ribbon im
  Bild, PNG selbst ansehen. Das ist der Weg, wie die Optik hier headless geprüft
  wird; GL ist in dieser Sandbox nicht lauffähig (kein Display) und geht wie beim
  Decal-Port blind mit Offline-Shader-Validierung.

### Schritt 4 — Vulkan, D3D11, D3D12

Nur der Trail-Teil; das Seil läuft dort schon, sobald Schritt 2 steht. Reihenfolge
und Zuschnitt wie beim Decal-Port: Vulkan zuerst, D3D11 danach, D3D12 erbt dessen
Vertrag.

### Schritt 5 — Editor

* `InspectorPanel.cpp`: zwei Abschnitte nach dem Muster des Decal-Blocks
  (`:1204`), zwei `addItem`-Zeilen im Add-Component-Menü (`:1848`). Für die
  Kontrollpunktliste die vorhandenen Zeilen-Widgets benutzen — und beim
  Umbenennen/Editieren pro Punkt an den Scratch-Buffer denken, sonst sieht
  `IsItemDeactivatedAfterEdit` nie den alten Wert.
* `EditorApplication.cpp:4303` als Muster für einen „Rope erzeugen"-Menüpunkt.
* Viewport-Vorschau der Kontrollpunkte und der Spline über `DebugDrawBuffer`.
* **Pflicht, vom Brett nicht genannt: jedes neue Bedienelement braucht einen
  Handbucheintrag in `EditorHelp.cpp`.** Die Deckung ist ein ctest (600/600), der
  sonst rot wird. Ein Eintrag ohne offenen Scope ist tot — die zwei Gesetze aus
  dem bestehenden Handbuchsystem gelten unverändert.
* Später, eigener Posten: `HE::api`-Registry-Einträge (`setControlPoint`,
  `setRopeAttachment`, `setTrailEmitting`), damit Lua/Python/HorizonCode
  drankommen. Nicht Teil dieser fünf Schritte.

---

## 8. Bewusste Grenzen von v1

* **Keine Seil-Simulation.** Kontrollpunkte kommen aus dem Editor, aus
  Anhängepunkten oder aus Skript. Ein Verlet-Solver mit Constraints ist ein
  eigenes Thema und gehört zu `PhysicsWorld`, nicht hierher. `sag` ist eine
  geometrische Näherung, kein Solver.
* **Kein LOD.** `samplesPerSpan` und `radialSegments` sind fest. Eine
  entfernungsabhängige Absenkung ist bei Muster A trivial nachrüstbar (neu bauen
  bei Stufenwechsel), aber jede Änderung kostet dort eine Cache-Invalidierung —
  daher erst mit Messung.
* **Keine Kollision.** Ein Seil ist reine Optik.
* **Trails ohne Sortierung untereinander.** Sie zeichnen nach den Opaken in
  Extraktionsreihenfolge. Zwei sich durchdringende transparente Trails können
  falsch überlagern; das ist der übliche Kompromiss und für additive/emissive
  Materialien ohnehin egal.
* **`MaterialAsset::doubleSided` bleibt ungelesen.** Dieses Feature repariert das
  nicht, es umgeht es (§3.3).
