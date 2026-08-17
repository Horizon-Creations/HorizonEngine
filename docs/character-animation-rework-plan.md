# Character- und Animations-Umbau

Umbau der Animations-Anbindung und der Klassen-Editor-Panels, Richtung Unreals
Blueprint-Pawn. Betrifft MASTERPLAN-Block **E5**.

---

## Stand (16.08.2026)

| CP | Inhalt | Status |
|---|---|---|
| **CP0** | Tick-Split, Animation nach dem Gameplay | ✅ `55459e8b` |
| **CP1** | `animator.*`-Registry + Anlege-Weg mit SkeletalMesh-Gate | ✅ `b0a08fdf` |
| **CP2a** | Sync-Graph im Asset + `AnimatorHost` | ✅ `162256b6` |
| **CP2b** | Autoren-Oberfläche + Palette-Erlaubnisliste | ✅ `2f20c561` |
| **CP2c** | Codegen-Sammlung + Parity-Fixture | ✅ `dba7505b` |
| — | Variablen **sind** die Parameter, Update geseedet, Palette enger | ✅ `1ed4a177` |
| — | `MovementComponent` + `movement.*`/`locomotion.*` | ✅ `dd683474` |
| — | Knotentitel, `entity.instance`, `Get Owning Object`, Details-Spalte | ✅ `3ab4c19d`, `3490268a`, `d402a49d` |
| **CP3** | Klassen-Editor: Code/Viewport-Trennung, Hierarchie-Baum | ✅ `0146b328` |
| **CP4** | Vorschau: Mesh + Collider + Kamera-Arm | ✅ (siehe unten) |

**CP4 ist anders gebaut als geplant.** Der Plan sah eine neue
`IRenderer`-Methode vor, die eine beliebige Welt in ein eigenes Target
extrahiert — echte Arbeit in jedem Backend, von denen hier nur zwei baubar
sind, und beurteilt allein danach, ob das Bild stimmt: genau das, was ohne
Bildschirm nicht prüfbar ist.

Stattdessen rendert die Vorschau das Mesh der Wurzel über den vorhandenen
Per-Asset-Pfad und legt Collider und Kamera-Arm als projizierte Linien darüber
(`ImDrawList`). Das kostet kein Backend- nichts, funktioniert auch auf D3D und
Vulkan, und zeichnet die Gizmos DURCH das Mesh — was man bei einem Collider
ohnehin will.

Die Matrix dafür kommt aus dem Renderer (`RenderSkeletalPreview` hat einen
optionalen `outViewProj` bekommen) statt im Panel nachgebaut zu werden: die
Rahmung hängt an GPU-seitigen Bounds, die nur der Renderer hat, und eine Kopie
dieser Regel würde driften — sichtbar als falscher Collider, nicht als falsche
Kamera.

Was damit weiterhin fehlt: echte Verdeckung der Gizmos und ein zweites Mesh im
Bild (nur die Wurzel wird gezeichnet). Beides wäre der ursprüngliche CP4b, und
er bleibt möglich, falls das Overlay sich als zu wenig erweist.

### CP4b ist nachgezogen — für GL und Metal (`IRenderer::RenderWorldPreview`)

Der Haken, der eine **beliebige Welt in ein beliebiges Target** zeichnet, ist
gebaut. Signatur wie die Geschwister-Previews, nur nimmt sie eine
`HorizonWorld&` statt einer Asset-UUID:

```cpp
void* RenderWorldPreview(ContentManager&, HorizonWorld&,
                         uint32_t width, uint32_t height,
                         float yaw, float pitch, float dist,
                         const glm::vec3& pivot = glm::vec3(0.0f),
                         glm::mat4* outViewProj = nullptr);
```

Bewusste Abweichungen von den Per-Asset-Previews, alle im Header begründet:

* **Opaker grauer Hintergrund mit Boden und Grid**, nicht transparent — das ist
  eine Szenen-Ansicht im Sinne von Unreals Character-Viewport. Boden, Grid und
  die Markierung des **eigenen Origins** liegen in
  `HorizonRendering/WorldPreviewGrid.h`, damit GL und Metal nicht auseinander
  laufen (getestet in `tests/test_world_preview_grid.cpp`).
* **Kein Auto-Fit** auf die Bounds: `dist` ist eine echte Weltdistanz um
  `pivot`. Der Extractor lässt Bounds für noch nicht residente Meshes
  absichtlich ungültig — ein Auto-Fit würde beim Nachladen springen, und das
  sähe aus, als bewege sich der Charakter.
* **Eigener `RenderExtractor`** statt `m_extractor`: auf dem hängt der
  Tag/Nacht-Zustand der Hauptszene, und eine Vorschau, die den Sonnenuntergang
  der Welt erbt, ist eine Überraschung ohne Nutzen.
* **Kein Deferred-Pfad**, sondern die vorhandenen kleinen Preview-Shader
  (statisch + skinned). Damit gibt es auch keine „mein Charakter ist schwarz"-
  Falle, wenn der Klassen-Blob gar kein Licht enthält.
* **Ein einziges Target pro Backend.** Der Aufrufer garantiert die Exklusivität:
  Asset-Tabs sind exklusiv und ImGui führt den Inhalt eines inaktiven Tabs nicht
  aus, also fragt pro Frame nur der aktive Tab an.

Nebenbei aufgeräumt: die Pipeline-/Programm-Erzeugung der Mesh- und
Skinning-Preview lag inline in ihren Zeichenfunktionen und wurde in
`EnsureMeshPreviewProgram`/`EnsureSkelPreviewPrograms` (GL) bzw.
`EnsureMeshPreviewPipeline`/`EnsureSkelPreviewPipeline` (Metal) gezogen — sonst
wäre der Shader ein zweites Mal kopiert worden.

**Offen (Paritäts-Arbeit, bewusst später):**

| Backend | Stand |
|---|---|
| OpenGL | ✅ gebaut, **blind** — kein Display in dieser Umgebung |
| Metal | ✅ gebaut, real-HW-Optik steht aus |
| D3D11 | ❌ offen |
| D3D12 | ❌ offen |
| Vulkan | ❌ offen |

Die drei offenen Backends erben die Default-Implementierung (`nullptr`), das
Panel fällt dort auf das Per-Asset-Bild + Gizmo-Overlay zurück. Vorlage für den
Nachzug ist jeweils der eigene `RenderSkeletalPreview` des Backends plus die
beiden Ensure-Helfer; der Rest — Kamera, Extraktion, Backdrop-Geometrie — ist
backend-unabhängig und steht schon.

### CP5 — Der Viewport ist jetzt ein Viewport

Damit hängt das Panel am neuen Haken statt am Per-Asset-Bild, und der linke Baum
zeigt, woraus die Klasse besteht:

* **Jede Komponente ist eine eigene Zeile** unter ihrer Entity, einzeln
  auswählbar; die Details-Spalte zeigt dann genau diese eine Komponente. Neue
  Komponenten hängt man per Rechtsklick auf die Entity-Zeile an, entfernen geht
  per Rechtsklick auf die Komponenten-Zeile.
* Die Zeilen kommen aus **`InspectorPanel::listComponents`**, das denselben
  Funktionskörper wie das Details-Panel in einem Sammel-Modus durchläuft. Eine
  zweite, handgepflegte Liste von Komponentennamen wäre beim nächsten neuen
  Komponententyp sofort veraltet. Aus demselben Grund gehen „Add" und „Remove"
  über `addComponentMenu`/`removeComponent` durch genau die vorhandenen Pfade —
  das Entfernen benutzt die `if (removed) registry.remove<T>()`-Zeile, die schon
  am Header hing, statt eine Label→Typ-Tabelle als dritte Liste einzuführen.
* `previewDist` bedeutet jetzt **Meter um den Origin**, nicht mehr ein Vielfaches
  der Mesh-Bounds; der Zoom ist multiplikativ, sonst fühlt er sich auf 20 m tot
  an.
* Der Origin ist der **des Wurzel-Entities**, nicht die Welt-Null: der Blob
  speichert das Transform der Wurzel und der Spawner beachtet es, also würde eine
  Klasse mit versetzter Wurzel sonst gegen ein leeres Stück Grid gerahmt — was
  wie ein kaputter Origin-Marker aussieht und nicht wie eine verschobene Wurzel.
  Boden, Grid und Marker liegen deshalb alle um diesen Punkt.
* `renderFor` und `addComponentMenu` melden jetzt, ob eine Komponente dazukam
  oder wegfiel. Von außen ist beides unsichtbar — das Menü lebt in einem Popup
  (eigenes ImGui-Fenster), und ein Entfernen hinterlässt kein aktives Item —,
  also hätte die „war hier gerade etwas aktiv"-Heuristik des Tabs die Änderung
  nicht als ungespeichert gezählt: Komponente anlegen, Tab schließen, weg.

**Nie live verifiziert** — wie alles in diesem Umbau.

**Nie live verifiziert:** nichts davon ist im laufenden Editor gelaufen. Optik,
Maus-Gefühl, Kollisions-Popping und der Sync-Graph in PIE stehen aus.

Kleinere Reste, die zum Umbau gehören, aber keinen eigenen CP haben:

* Der Animator-Editor hat **keinen Compile-Button** wie der Klassen-Editor —
  ein Sync-Graph, der nicht kompiliert, fällt erst beim Export auf.
* `AssetThumbnailCache::thumbnailKindOf` kennt `HorizonCodeClass` nicht, obwohl
  der `componentBlob` Prefab-Format ist und `prefabPrimaryMesh` direkt passt.
* MASTERPLAN-E5-Reste, unberührt: Root Motion, Layer/Bone-Masks, Blend-Trees,
  Animation-Notifies, IK. Die FSM kann weiterhin **eine** Bedingung pro
  Transition, ohne Exit-Time und „Any State".

---

## Ausgangslage

Die State Machine ist **autorierbar, aber nicht ansteuerbar** — und das steht so
nirgends. Der Masterplan listet unter E5, was zur Action-Game-Reife fehlt (Root
Motion, Layer, Blend-Trees, Notifies, IK) und liest sich damit als „Basis da,
Ausbau fehlt". Tatsächlich fehlt die Basis:

| Teil | Zustand |
|---|---|
| Laufzeit (`AnimationStateMachineSystem`) | funktioniert — States, Transitions, echter TRS-Crossfade, Parse-Cache, Laufzeitzustand pro Entity |
| Editor (`AnimatorStateMachineEditorPanel`, 421 Z.) | funktioniert — Canvas, Transitions ziehen, Clip-Slots, Default-Params, Collab-Sync |
| Tests | 8 Runtime- + 1 Graph-Roundtrip-Test |
| **Parameter setzen** | **existiert nicht.** Kein `animator.*` in der Registry, keine Lua-/Python-Bindung, kein HorizonCode-Knoten, kein Inspector-Feld |
| **Komponente anlegen** | **existiert nicht.** Nicht im Add-Component-Menü, nicht in `EntityHost::defaultComponents` |

Ein Setup „speed > 0.5 → Walk" wird also jeden Frame korrekt ausgewertet, und
`speed` bleibt für immer auf dem Default. Der Kommentar im System selbst
(`AnimationStateMachineSystem.cpp:78`) spricht von „a param a script already
tweaked at runtime" — er beschreibt eine API, die es nicht gibt.

**Nicht mehr aktuell:** Der Masterplan (Forts. 70) notiert, dass
`SkeletalMeshComponent`, `AnimatorComponent` und Nachbarn keine
Serializer-Anbindung haben und beim Speichern still verschwinden. Geprüft:
alle sieben speichern und laden heute. Das ist kein Blocker mehr.

---

## Das Modell

Connors Vorgabe, auf Unreal abgebildet:

| Unreal | Hier | Rolle |
|---|---|---|
| Animation Blueprint **EventGraph** | **Sync-Graph** | läuft pro Frame, liest die Figur, schreibt Parameter |
| Animation Blueprint **AnimGraph** | **State Machine** | liest Parameter, wählt Clip, blendet |
| Blueprint **Viewport / Components / Details** | neue Tabs im Klassen-Editor | siehe CP3/CP4 |

Zwei Regeln daraus:

1. **Der Sync-Graph ist der einzige Schreiber.** Die State Machine liest
   Parameter und schreibt sie nie — das ist heute schon so
   (`evalTransition` vergleicht nur) und bleibt eine Invariante durch
   Konstruktion, kein Laufzeit-Guard.
2. **Der Animations-Pass läuft nach dem Code-Pass.** Sonst liest die State
   Machine Werte von gestern.

---

## CP0 — Animation nach dem Code-Pass

**Das ist heute in PIE verletzt, und zwar genau andersherum als im Spiel.**

```
Spiel  (GameApplication::OnRender)    Editor (EditorApplication::OnRender)
  :937  updateScripts                   :1971  SceneSystems::tick   ← Animation ZUERST
  :944  Physik                          :1981  Physik
  :968  Kamera                          :1994  Kamera
  :983  playerHost.tick                 :2019  Skripte
  :985  entityHost.tick                 :2048  playerHost.tick
 :1009  SceneSystems::tick  ← Animation :2051  entityHost.tick
```

In PIE läuft der Animations-Pass also **vor allem anderen** — ein voller Frame
Nachlauf, und die umgekehrte Reihenfolge zur ausgelieferten Version. Ein
Preview, das eine andere Reihenfolge fährt als das Spiel, ist kein Preview.

**Umbau:** `SceneSystems::tick` in zwei Phasen teilen.

* `tickWorld` — Terrain, Navigation, Weather, GPU-Partikel, LOD. Läuft im Editor
  weiter **immer** (auch im Edit-Modus, wie heute).
* `tickAnimation` — `AnimationSystem`, `AnimationBlendSystem`,
  `AnimationStateMachineSystem`, `PropertyAnimationSystem`
  (`SceneSystems.cpp:95-98`, schon zusammenhängend).

Zielreihenfolge, in **beiden** Apps identisch:

```
Skripte → Physik → Kamera → Hosts → tickWorld → tickAnimation → Extraktion
```

Zwei Punkte, die dabei nicht untergehen dürfen:

* **`tickAnimation` muss vor der Extraktion bleiben.** Der Extractor konsumiert
  `boneMatrices` und `SkeletalMeshComponent::dirty`; nach hinten verschoben
  tauscht man einen Nachlauf gegen einen anderen.
* **`NavigationSystem` bewegt Transforms**, ist also Gameplay und gehört vor die
  Animation — in `tickWorld` ist es damit richtig einsortiert, aber es ist eine
  Entscheidung und keine Selbstverständlichkeit.

Testbar ohne Grafik: eine Welt mit FSM + Skript, das einen Parameter setzt, in
einer Runde ticken und prüfen, dass der Zustandswechsel **im selben** Frame
ankommt statt im nächsten.

---

## CP1 — Parameter setzen, und die Komponente anlegen können

Zwei kleine Teile, die zusammen das vorhandene Feature erst benutzbar machen.

**a) Registry-Zeilen** nach dem Muster von `material.setParam`
(`EngineApi.cpp:1479`):

```
animator.setParam(entity, name, value)
animator.getParam(entity, name) -> float
animator.getState(entity)       -> string   (Diagnose, read-only)
```

Gruppe `"animator"`, dazu in `isScriptGroup` — damit sind Lua, Python,
HorizonCode und der C++-Codegen auf einen Schlag bedient.

Diese Zeilen werden vom Sync-Graph (CP2) **nicht ersetzt**: ein Lua- oder
Python-Projekt hat keinen Sync-Graph und braucht trotzdem einen Weg an die FSM.
Der Sync-Graph ist der HorizonCode-native Pfad, die Registry der
sprachunabhängige.

**b) Die Komponente anlegbar machen.** Sie steht heute bewusst nicht im
Add-Component-Menü, mit Verweis auf einen „owning asset workflow" — den es nicht
gibt. Außerhalb von Serializer und Tests existiert im ganzen `src/` kein
`emplace<AnimatorStateMachineComponent>`.

**Die Regel ist die Abhängigkeit, nicht die Klasse:** eine State Machine
animiert ein Skelett, also gehört sie dorthin, wo eines ist. Der Menüeintrag
erscheint für Entities mit `SkeletalMeshComponent` und sonst nicht. Damit ist
sie auch an einer normalen Szenen-Entity verfügbar, ohne dass man sie „lose an
irgendwas" hängen kann.

---

## CP2 — Der Sync-Graph

Das Animator-Asset bekommt einen zweiten Graphen. Vorbild ist
`HorizonCodeClassAsset` (`Assets.h:296`), das schon `graphJson` + `baseClass` +
`componentBlob` trägt — hier also `graphJson` (die FSM) + `syncGraphJson`.

**Funktionsumfang bewusst eng.** Der Graph hat genau ein Event (`Update`, mit
`dt`), erreicht seinen Besitzer, und schreibt Parameter. Er soll keine Szenen
wechseln, nichts spawnen, nichts speichern.

Durchgesetzt über zwei vorhandene Mechanismen:

* die Knoten-Kategorien pro Host (`LevelScriptPanel.cpp:259` und
  `UIEditorPanel.cpp:1139` führen je eigene Listen) — für den Sync-Host also
  `Flow, Literals, Math, Logic, Variables, Debug`.
* eine Erlaubnisliste für Engine-Calls. `HcEditorUtil::drawEngineApiMenu`
  (`HcEditorUtil.cpp:844`) surft heute die **ganze** Registry ungefiltert; ein
  optionaler Gruppenfilter ist die kleinste Erweiterung. Erlaubt:
  `player`, `entity`, `transform`, `physics` (nur Lesezugriffe), `math`,
  `time`, `animator`.

**Wo die Parameter leben.** Sie bleiben auf der **Komponente**
(`AnimatorStateMachineComponent::params`, `map<string,float>`, pro Entity) —
nicht als Graph-Variablen, die pro Runtime-Instanz liegen. Der Sync-Graph
schreibt sie über dieselben `animator.setParam`-Zeilen aus CP1, nur mit seinem
Besitzer als Entity. Damit gibt es **einen** autoritativen Speicher und keine
zweite Wahrheit, die synchron gehalten werden müsste.

**Nicht vergessen — der Grund, warum E0 wehgetan hat:** der Sync-Graph muss
durch dieselbe Interpreter/Compiled-Dualität wie jeder andere Graph. Er
registriert sich bei der `Runtime` wie eine Klasse, wird vom Export
mitgesammelt, und läuft im gepackten Spiel kompiliert. Ein Graph-Asset, das
nicht ausgeliefert und verdrahtet wird, feuert dort nichts —
`GameInstance.hcode` hat das einmal vorgemacht.

---

## CP3 — Klassen-Editor: Code und Viewport trennen

### Was heute da ist

Der Klassen-Editor (`LevelScriptPanel.cpp:1654-1829`, die Datei beherbergt
Level Script, Game Instance und Klassen in einem) hat **zwei Modi**, umgeschaltet
über zwei Toolbar-Zellen — keine Tab-Leiste, keine Dock-Struktur:

* **Graph** → `drawGraphBody` (`:1125`): 220-px-Spalte mit Variablen, Funktionen
  und Details, daneben die Node-Canvas.
* **Components** → `drawComponentsBody` (`:1517`): 220-px-**flache Liste** aller
  benannten Entities, daneben `InspectorPanel::renderFor` gegen eine echte
  `HorizonWorld`.

Diese `compWorld` ist der wichtigste Bestandteil für den Umbau. Sie wird von
`ensureComponentWorld` (`:1462`) aus dem Prefab-Blob der Klasse instanziiert —
mit Vererbung: eigener `componentBlob` → der des nächsten Vorfahren →
`EntityHost::defaultComponents`. **Der Details-Bereich ist damit schon der echte
Inspector auf einer echten Welt.** Was fehlt, ist das Bild und ein Baum.

### Zielbild

Aus „Graph / Components" wird **„Code / Viewport"**. Der Code-Modus bleibt, wie
er ist; der Components-Modus wird zum Viewport-Modus und bekommt eine Spalte
dazu:

```
Viewport                                      Code
┌──────────┬──────────────────┬──────────┐   ┌──────────┬──────────────────────┐
│Components│  3D-Vorschau     │ Details  │   │Variablen │                      │
│          │                  │          │   │Funktionen│   Node-Canvas        │
│ Player   │   Mesh, Collider │ z.B. die │   │Details   │                      │
│ ├ Capsule│   Kamera-Arm     │ State    │   │          │                      │
│ ├SkelMesh│                  │ Machine  │   │          │                      │
│ └ Camera │                  │          │   │          │                      │
└──────────┴──────────────────┴──────────┘   └──────────┴──────────────────────┘
```

Damit schließt sich der Kreis zu CP1: weil die State Machine an
`SkeletalMeshComponent` hängt, zeigt das Anwählen der Mesh-Komponente im Baum
rechts direkt den Asset-Slot „welche State Machine" — genau die Stelle, an der
Unreal die „Anim Class" führt.

### Arbeit

1. **Die zwei Toolbar-Zellen umbenennen** und `st.showComponents` in einen
   Modus-Enum überführen (`:1714-1798`). Klein.
2. **Flache Liste → echter Hierarchie-Baum.** `drawComponentsBody:1525` listet
   heute alle `NameComponent`-Entities nebeneinander; die Klasse hat seit der
   Kamera aber eine Hierarchie (`Player → Camera`). `OutlinerPanel` hat den
   Baum-Code samt Drag&Drop bereits — Muster übernehmen, nicht neu erfinden.
3. **Die Vorschau-Spalte einhängen** (CP4).

Layout-Vorlage ist `MaterialEditorPanel.cpp` (`:2034-2368`): feste linke Spalte
mit Vorschau oben (`##matPreview`, Höhe 240) und Eigenschaften darunter, rechts
die Canvas. Strukturell ist das fast schon das Blueprint-Layout.

**Kein verschachteltes DockSpace.** Asset-Tabs sind heute `NoDocking`-Fenster auf
einem berechneten Rechteck (`EditorUI.cpp:2388-2420`); dockbare Sub-Panels
innerhalb eines Klassen-Tabs wären ein Bruch mit dem Muster aller anderen
Editoren und bräuchten eine eigene ini-Sektion. Handgebaute Spalten bleiben.

**Nicht vergessen:** `HorizonCodeClassPanel::collabDocs` (`:1615`) und
`reloadFromDisk` (`:1625`) hängen am Panel-State.

---

## CP4 — Die Vorschau

Hier liegt die Unbekannte, und sie hat einen klaren Preis.

**Die vorhandene Vorschau-Infrastruktur ist per Asset, nicht per Welt.**
`IRenderer` bietet `RenderSkeletalPreview`, `RenderMaterialPreview` (optional mit
`meshId`-Override), `RenderParticlePreview` und die Thumbnail-Pfade — jeder
rendert **genau ein Asset** in ein eigenes Offscreen-Target. Keiner nimmt eine
`HorizonWorld`. (`RenderExtractor` hat entgegen der Vermutung *keine*
Preview-Pfade; er ist nur der ECS→`RenderWorld`-Snapshot.)

Deshalb in zwei Stufen:

### CP4a — Vorschau der ausgewählten Komponente

Zeigt das, was die angewählte Komponente ist: `SkeletalMeshComponent` →
`RenderSkeletalPreview` (Bind-Pose oder ein Clip), `MeshComponent` →
`RenderMaterialPreview` mit `meshId`-Override. Vorlage ist
`SkeletalMeshEditorPanel.cpp:220-267` — rund 30 Zeilen inklusive Orbit.

**Keine Backend-Arbeit.** Ehrliche Grenze: das ist die Vorschau *einer*
Komponente, nicht des Zusammenbaus. Collider und Kamera-Arm fehlen.

### CP4b — Vorschau des Zusammenbaus

Das, was du eigentlich willst: Mesh **und** Collider **und** Kamera-Arm in einem
Bild, mit Klick-Auswahl.

Dafür braucht es eine neue `IRenderer`-Methode in der Art
`RenderWorldPreview(world, root, size, yaw, pitch, dist)`, die eine beliebige
Welt extrahiert und in ein eigenes FBO rendert. Die Bausteine liegen bereit —
`RenderExtractor` ist public und exportiert, jedes Backend hat schon ein
`m_extractor`-Member, und `RenderSkeletalPreview` (`OpenGLRenderer.cpp:7700ff`)
ist die exakte Vorlage für „eigenes FBO, lazy resize, kleines eigenes Shaderset".

**Aber es ist Arbeit pro Backend** — GL, Metal, Vulkan, D3D11, D3D12. Auf dieser
Maschine sind nur GL und Metal baubar; die anderen drei wären wieder blind
geschrieben. Das ist der teuerste Posten des ganzen Umbaus, und er ist der
einzige, den man auch später nachziehen kann: CP4a liefert vorher schon ein Bild.

Collider-Umrisse und der Kamera-Arm kommen als Debug-Linien, denselben Weg wie
im Haupt-Viewport.

---

## Nebenbefund: Klassen haben kein Thumbnail

`AssetThumbnailCache::thumbnailKindOf` (`:88-107`) kennt `HorizonCodeClass`
nicht, obwohl der `componentBlob` exakt das Prefab-Format hat und der
Prefab-Pfad (`prefabPrimaryMesh`, `:575-591`) unverändert anwendbar wäre — er
zeichnet ein Prefab als „das erste Mesh darin". Ein paar Zeilen, und
PlayerCharacter-Assets im Content Browser haben ein Bild statt eines Icons.
Gehört nicht zum Umbau, ist aber der billigste sichtbare Gewinn hier.

---

## Reihenfolge

| CP | Inhalt | Größe |
|---|---|---|
| **CP0** | Tick-Split, Animation nach dem Code-Pass | klein |
| **CP1** | `animator.*`-Registry + Anlege-Weg mit SkeletalMesh-Gate | klein |
| **CP2** | Sync-Graph im Animator-Asset, Palette begrenzt, Codegen | mittel |
| **CP3** | Code/Viewport-Trennung, Hierarchie-Baum | mittel |
| **CP4a** | Vorschau der ausgewählten Komponente | klein |
| **CP4b** | Vorschau des Zusammenbaus, neue Renderer-Methode | groß, pro Backend |

CP0 und CP1 sind unabhängig voneinander und schalten alles Weitere frei — nach
CP1 ist die State Machine zum ersten Mal aus Gameplay-Code bedienbar. CP2 ist der
Architektur-Schritt. CP3 + CP4a zusammen ergeben bereits einen Editor, der sich
wie ein Blueprint-Pawn anfühlt; CP4b macht das Bild vollständig.
