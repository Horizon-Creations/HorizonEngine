# Physik (Jolt): Constraints/Joints, Collision-Layers, Shape-Casts, Kraefte-Rest

Stand 07.09.2026, Branch `claude/physics-jolt-rest`. Schritt 1 des Themas:
Kartierung des vorhandenen `PhysicsWorld`/Jolt-Stands, Festlegung des
Layer-Modells, des Constraint-Modells und der fehlenden Query-Formen.
**Kein Feature-Code in diesem Schritt.**

Bezug: `docs/gap-audit-2026-08-25.md` (§ Physik), `docs/game-readiness-audit-2026-08-27.md`
(Tabelle „Physik-Schreibseite"), `src/HE_Scene/include/HorizonScene/PhysicsWorld.h`,
`src/HE_Scene/include/HorizonScene/EngineApi.h` (`namespace physics`).

Jolt-Version: **v5.5.0**, per `FetchContent` (`CMakeLists.txt:99-116`).

---

## 0. Korrektur an der Aufgabenbeschreibung

Die Themenbeschreibung stammt aus dem Roadmap-Text und ist an zwei Punkten
veraltet. Was tatsaechlich offen ist, steht in der rechten Spalte.

| Punkt aus der Beschreibung | Wirklicher Stand |
| --- | --- |
| Impulse/Forces-API | **Fertig.** `addForce`/`addImpulse`/`addTorque` in `PhysicsWorld.h:226-228`, Impl `PhysicsWorld.cpp:1991/2007/2023`, `HE::api::physics` in `EngineApi.h:369-372`, Registry `EngineApi.cpp:4577-4582`, Tests `tests/test_physics.cpp:325-377`. Offen bleibt nur der Rest daneben: Kraft/Impuls **an einem Punkt** und Winkelgeschwindigkeit. |
| Shape-Casts/Overlap-Queries | **Halb fertig.** `sphereCast` (`PhysicsWorld.cpp:1898`) und `overlapSphere` (`:1965`) gibt es samt Registry-Eintraegen. Es fehlen Box- und Kapsel-Cast, Box- und Kapsel-Overlap, ein Mehrfachtreffer-Raycast und jede Layer-Filterung. |
| Constraints/Joints | **Nichts vorhanden.** Kein Treffer im Baum. |
| Collision-Layers | **Nichts vorhanden.** Es gibt genau zwei feste Object-Layer (`MOVING`/`NON_MOVING`, `PhysicsWorld.cpp:58-110`), die allein aus dem Motion-Type folgen (`:972-1002`). |

Auch der `Remaining:`-Satz in `Website/HorizonEngine/roadmap.json` („impulses and
forces … spawning bodies at runtime … mesh/convex colliders are all still to be
exposed") ist veraltet — die drei sind auf main. Der Text gehoert am Ende des
Themas angeglichen, nicht in diesem Schritt.

**Folge fuer den Schnitt der Schritte 2–5:** siehe § 7. Der Punkt
„Impulse/Forces" traegt keinen eigenen Schritt mehr, „Collision-Layers" traegt
mehr als einen.

---

## 1. Was schon da ist

### 1.1 Die Klasse

`PhysicsWorld` ist eine PIMPL-Huelle um `JPH::PhysicsSystem`
(`PhysicsWorld.cpp:290-349`). Kein Jolt-Header verlaesst die `.cpp`. Das ist die
Randbedingung, an der jede Entwurfsentscheidung unten haengt: **alles, was neu in
die oeffentliche API kommt, muss ohne Jolt-Typ auskommen** — also eigene Enums,
`glm`-Vektoren, `uint32_t`-Entity-Ids.

Im `Impl`:

```cpp
BPLayerInterfaceImpl      bpLayerInterface;    // 2 Broadphase-Layer
ObjectVsBPLayerFilterImpl ovbpFilter;
ObjectLayerPairFilterImpl ooFilter;
JPH::PhysicsSystem        physicsSystem;
HEContactListener         contactListener;
std::unordered_map<uint32_t, JPH::BodyID>                        entityToBody;
std::unordered_map<uint32_t, std::unique_ptr<JPH::CharacterVirtual>> entityToCharacter;
static constexpr uint32_t kMaxBodies = 1024;
```

Die drei Filter-Objekte werden **einmal im Konstruktor** an
`physicsSystem.Init()` uebergeben (`:338-346`). Jolt haelt Referenzen darauf,
nicht Kopien — sie duerfen also spaeter mutiert werden, aber nicht ersetzt. Das
ist die Tuer, durch die die Layer-Matrix hereinkommt (§ 3.3).

### 1.2 Der Lebenszyklus eines Koerpers

| Weg hinein | Funktion |
| --- | --- |
| Szenenstart, alles auf einmal | `initialize()` `:1172` (ruft `clear()` zuerst) |
| Laufzeit, eine Entity | `addEntity()` `:1225` — **idempotent, reisst vorher ab** |
| Laufzeit, ein Teilbaum | `addEntityTree()` |
| Terrain ohne eigenen RigidBody | `buildTerrainBodyFor()` |

| Weg hinaus | Funktion |
| --- | --- |
| alles | `clear()` `:2250` |
| eine Entity | `removeEntity()` → `destroyBodyFor()` `:1351` |
| Nachlese pro Frame | `step()` `:1527` raeumt Bodies ungueltiger Entities ab |

`destroyBodyFor` ist die **einzige** Stelle, die einen Body zerstoert; alle drei
Wege oben laufen durch sie. Das ist der Haken, an dem die Constraint-Aufraeumung
haengen muss (§ 4.5).

### 1.3 Layer, so wie sie heute sind

```cpp
namespace HELayers   { NON_MOVING = 0; MOVING = 1; NUM_LAYERS = 2; }   // :58-62
namespace HEBPLayers { NON_MOVING(0); MOVING(1); NUM_LAYERS = 2; }     // :64-68
```

`ObjectLayerPairFilterImpl::ShouldCollide` (`:103-112`): `NON_MOVING` kollidiert
nur mit `MOVING`, `MOVING` mit allem. Der Layer eines Bodies faellt beim Bauen
direkt aus `RigidBodyComponent::type` (`:972-1002`) und wird bei einem
Downgrade auf Static (Mesh/HeightField als Dynamic) nochmal ueberschrieben
(`:993-1001`).

`CharacterFilters` (`:905-913`) verdrahtet den CharacterVirtual fest auf
`HELayers::MOVING`, an drei Stellen benutzt (`:1403`, `:1481`, `:1639`).

Die Queries geben in den Layer-Filter-Slots `{}, {}` an, also „alles":
`raycast` `:1869`, `sphereCast` `:1937`, `overlapSphere` `:1988`. Gefiltert wird
nur ueber `HEQueryFilter` (`:1749-1765`), einen `JPH::BodyFilter`, der genau zwei
Dinge kann: eine Entity ausblenden und Sensoren ueberspringen.

`JPH_OBJECT_LAYER_BITS` ist **nicht** gesetzt, Jolt bleibt also beim Default 16
(`ObjectLayer.h:12-14` im gefetchten Baum). `ObjectLayer` ist damit ein
`uint16` — die `…Mask`-Varianten der Jolt-Filter (Gruppe+Maske in den Layer
gepackt) brauchen 32 Bit und scheiden ohne globale Definition aus. Siehe § 3.2.

### 1.4 Wie eine API-Funktion bis in alle vier Frontends kommt

Eine einzige Registry-Zeile in `EngineApi.cpp` bedient alle Frontends: Lua
(`ScriptContext.cpp:834`), Python (`PyScriptBackend.cpp:775`), HorizonCode-Graph
(`HcGraphHost.cpp:1207`, `HcNodeReference.cpp:183`) und den Codegen. Sie
iterieren alle `HE::api::registry()`.

Das ist billig — aber nicht kostenlos, siehe § 6.

---

## 2. Was noch fehlt, in einem Satz je Stueck

1. **Collision-Layers** — pro Koerper waehlbarer Layer, N×N-Matrix, Layer-Filter in allen Queries, Layer als Feld in `RaycastHit`.
2. **Constraints/Joints** — Modell, Komponente, Serialisierung, Laufzeit-API, Aufraeumung.
3. **Shape-Casts** — Box- und Kapsel-Cast, Box- und Kapsel-Overlap, Mehrfachtreffer-Raycast.
4. **Kraefte-Rest** — `addForceAtPosition`, `addImpulseAtPosition`, `set/getAngularVelocity`.

Reihenfolge: **Layers zuerst.** Casts und Constraints bekommen beide einen
Layer-Parameter; wenn der Layer nachkommt, aendert sich die Signatur von
Funktionen, die schon in HorizonCode-Graphen stehen.

---

## 3. Collision-Layers

### 3.1 Was ein Layer hier bedeutet

Ein Layer ist ein **benannter Kollisionskanal**, projektweit, nummeriert
0…N-1. Zwei Koerper kollidieren, wenn die Matrix fuer ihr Layer-Paar `true`
sagt. Das ist das Unity-Modell und es ist das, was Leute erwarten: „der
Spieler-Trigger soll nur Pickups sehen", „Geschosse gehen durch Gegner-Ragdolls".

**N = 16.** Nicht 32. Begruendung: die Matrix wird in der Projekt-UI als Dreieck
gezeichnet (16×17/2 = 136 Kaestchen, 32 waeren 528 und unbedienbar), und der
Object-Layer bleibt bei 16 Bit (§ 3.2). Die ersten vier sind vorbelegt und
umbenennbar, aber nicht loeschbar:

| Index | Name | Wer landet dort ohne Zutun |
| --- | --- | --- |
| 0 | `Default` | jeder Body ohne eigene Wahl, jede Static-Geometrie |
| 1 | `Player` | nichts automatisch — der Name existiert, damit die Vorbelegung nicht leer ist |
| 2 | `Trigger` | nichts automatisch, siehe unten |
| 3 | `Character` | Default-Wert von `CharacterControllerComponent::collisionLayer` |
| 4 | `Terrain` | der implizite Landschafts-HeightField, fest |

**Nachgezogen in Schritt 2 (`92338f9e`), die Tabelle oben ist die korrigierte:**

- Es sind **fuenf** Vorbelegungen, nicht vier. Der Terrain-Layer ist die
  Entscheidung des Menschen (Brett, chefchen): so kann eine Abfrage „nur der
  Boden" oder „alles ausser dem Boden" ueberhaupt formuliert werden.
- **`Trigger` ist kein Automatismus.** Die urspruengliche Begruendung („niemand
  hat einen Component, in den er etwas anderes schreiben koennte", § 3.4) gilt
  fuer Sensoren nicht: ein Sensor IST ein Body aus `RigidBodyComponent`, und die
  traegt das Feld jetzt. Ihn zwangsweise nach 2 zu schieben wuerde also eine
  explizit getroffene Wahl ueberstimmen — genau das, was der naechste Absatz
  verbietet. Der Name steht bereit fuer den, der ihn waehlt.
- `RigidBodyComponent::collisionLayer` ist **0**,
  `CharacterControllerComponent::collisionLayer` ist **3**. Auf einer
  PlayerCharacter-Entity, die beides hat, beantworten die zwei Felder zwei
  verschiedene Fragen: das des Characters, wovon der Spieler **aufgehalten**
  wird, das des Koerpers, wofuer ihn alle anderen **halten**.

Die Vorbelegung ist **kein** Automatismus, der die Wahl ueberstimmt: sie ist nur
der Default-Wert des neuen Feldes fuer eine Entity, die nie einen Layer gewaehlt
hat. Explizit gewaehlt schlaegt Vorbelegung.

Die Matrix ist per Default **komplett `true`**. Alles andere bricht jedes
bestehende Projekt beim Oeffnen: heute kollidiert alles mit allem ausser
Static↔Static.

### 3.2 Kodierung: Layer × Beweglichkeit

Der Broadphase muss bei **zwei** Layern bleiben. Das ist die Perf-Dimension
(Jolt baut je Broadphase-Layer einen Quadtree, und die Trennung „bewegt sich /
bewegt sich nicht" ist die, die etwas bringt); 16 Broadphase-Layer waeren 16
Baeume und ein Rueckschritt.

Also:

```
ObjectLayer = (userLayer << 1) | isMoving        // 5 Bit belegt von 16
BroadPhaseLayer = isMoving ? MOVING : NON_MOVING
```

`NUM_OBJECT_LAYERS` = 32. Die Filter werden dann:

```cpp
ObjectVsBPLayerFilterImpl::ShouldCollide(obj, bp):
    (obj & 1) ? true                    // MOVING trifft beide Baeume
              : bp == BP::MOVING        // NON_MOVING nur den bewegten

ObjectLayerPairFilterImpl::ShouldCollide(a, b):
    if (!(a & 1) && !(b & 1)) return false;       // Static↔Static wie bisher
    return matrix[a >> 1][b >> 1];
```

Die erste Zeile des Paar-Filters erhaelt genau das Verhalten von heute; die
zweite ist alles Neue.

**Gegen `ObjectLayerPairFilterTable` und die `…Mask`-Varianten aus Jolt
entschieden:** Die Table-Variante kann die Matrix, aber ihre `NUM_OBJECT_LAYERS`
sind fix zur Konstruktionszeit, und der Perf-Sinn der Static/Moving-Trennung
muesste als Konvention obendrauf. Die Mask-Variante braucht
`JPH_OBJECT_LAYER_BITS 32` — eine globale Definition, die in jeder
Uebersetzungseinheit gleich gesetzt sein muss, die Jolt einbindet, sonst ist es
eine stille ODR-Verletzung mit einem `uint16`- und einem `uint32`-`ObjectLayer`
im selben Binary. Die eigenen Filterklassen existieren bereits und sind je
sieben Zeilen; das ist der billigere und der besser lesbare Weg.

### 3.3 Wo die Matrix wohnt

Muster gibt es im Baum: eine projektweite Einstellung wandert
`ProjectData` (`.heproj`, `HE_Tools`) → `ExportSettings` → `ProjectConfig`
(`project.hcfg`, `HE_Core`) → Runtime. Siehe `allowFiles` als lebendes Beispiel
(`ProjectManager.h:288`, `ProjectExporter.h:83`, `ProjectConfig.h:72`).

Aber: **`HorizonScene` linkt `HE_Tools` nicht** (`src/HE_Scene/CMakeLists.txt:57-65`,
nur `HorizonCore`, `HorizonNet`, Jolt, Recast, json). `PhysicsWorld` darf
`ProjectData` also nicht sehen.

Der Traeger gehoert damit nach **`HE_Core`**, wo beide Seiten hinsehen duerfen:

```
src/HE_Core/include/Physics/CollisionLayers.h
    struct HE_API CollisionLayerConfig {
        static constexpr int kCount = 16;
        std::string names[kCount];        // leer = "Layer 7"
        bool        matrix[kCount][kCount] = { alles true };  // symmetrisch gehalten
        // toJson/fromJson, damit .heproj und .hcfg denselben Block schreiben
    };
```

Und ein Setter auf der Physik:

```cpp
void PhysicsWorld::setCollisionLayers(const CollisionLayerConfig&);
```

Den ruft der Editor beim Projektwechsel und `GameApplication` beim Start. Ohne
Aufruf gilt die Default-Konstruktion: 16 namenlose Layer, alles kollidiert mit
allem — also exakt das heutige Verhalten. **`HE_Core` bekommt `HE_API`, `HE_Scene`
nie** (Windows-CI-Regel, `docs/coding-conventions.md`).

Die Matrix darf **zur Laufzeit geaendert** werden: die Filter sind Member des
`Impl` und Jolt haelt nur Referenzen (§ 1.1). Aenderung wirkt ab dem naechsten
Broadphase-Update; bereits bestehende Kontakte loesen sich beim naechsten Step
auf. Das ist gut genug und muss nicht dokumentiert werden als „sofort".

### 3.4 Wo der Layer eines Koerpers steht

**Neues Feld auf `RigidBodyComponent`**, nicht auf `ColliderComponent`:

```cpp
uint8_t collisionLayer = 0;   // Index in CollisionLayerConfig
```

Begruendung, die in den Header gehoert: ein Body existiert genau dann, wenn
`RigidBodyComponent` da ist — `ColliderComponent` ist optional (ohne ihn faellt
die Physik auf eine Box aus der Skalierung zurueck, `PhysicsWorld.h:75-79`).
Ein Layer-Feld auf einem optionalen Component waere ein Feld, das an einer
Entity mit Body manchmal existiert und manchmal nicht. Ausserdem sitzt der Layer
in Jolt auf `BodyCreationSettings`, also da, wo `RigidBodyComponent` schon
`mass`/`friction`/`restitution` hinliefert.

Dazu ein zweites Feld auf `CharacterControllerComponent` — der Character hat
keinen `RigidBodyComponent`-Zwang und braucht seinen eigenen (§ 3.5).

Terrain-Implizitkoerper: `Terrain`, fest, weil niemand einen Component hat, in
den er etwas anderes schreiben koennte. Sensoren bekommen **nichts** aufgezwungen
— Begruendung oben in § 3.1.

Nachtrag aus Schritt 2: `setCollisionLayers` muss nach dem Schreiben der Matrix
**alle Bodies aufwecken**. Ein schlafender Body ist nicht in Jolts Active Set,
die Broadphase fragt also nie wieder nach ihm — der Boden unter einer bereits
liegengebliebenen Kiste wegzuschalten liesse sie in der Luft haengen. Genau der
Fall, den ein Autor trifft, der die Matrix waehrend des Spielens aendert; ein
Test deckt ihn ab (`PhysicsWorld: the matrix may be changed while the simulation
runs`, mit vier Sekunden Vorlauf, damit die Kiste wirklich schlaeft).

### 3.5 Character-Controller

`CharacterFilters` (`:905-913`) baut heute
`DefaultObjectLayerFilter(ooFilter, HELayers::MOVING)`. Neu: der Layer des
Characters wird neben dem `CharacterVirtual` gemerkt (dritte Map, oder ein
kleines Struct statt des nackten `unique_ptr` in `entityToCharacter`) und die
Filter werden je Update daraus gebaut. Die Filter sind billige Wertobjekte, die
heute schon dreimal lokal konstruiert werden — das aendert sich nicht.

### 3.6 Layer in den Queries

Der Layer-Filter gehoert in den **`ObjectLayerFilter`-Slot** (den zweiten `{}`
in den drei Query-Aufrufen), nicht in `HEQueryFilter`. Der Grund ist
Broadphase-Bewusstsein: ein `BodyFilter` wird erst in der Narrow-Phase befragt,
ein `ObjectLayerFilter` schon davor.

Neue Filterklasse neben `HEQueryFilter`:

```cpp
class HELayerMaskFilter final : public JPH::ObjectLayerFilter {
    uint32_t m_mask;   // Bit i = Layer i erlaubt; 0xFFFFFFFF = alles
    bool ShouldCollide(JPH::ObjectLayer l) const override {
        return (m_mask >> (l >> 1)) & 1u;
    }
};
```

Die Maske ist ein `uint32` mit 16 belegten Bits, kein Layer-Index: eine Query
will „Welt und Gegner, keine Trigger", nicht einen Kanal.

`RaycastHit` bekommt ein Feld `uint8_t layer` — sonst muss der Aufrufer die
getroffene Entity nachschlagen, nur um zu erfahren, was er getroffen hat.
Das Feld ist **additiv** ans Ende; `HE::api::physics::RaycastHit` spiegelt es,
und die Registry-Ausgaenge von `physics.raycast`/`physics.sphereCast` bekommen
eine fuenfte Ausgabe. Zusaetzliche **Ausgaenge** sind fuer bestehende Graphen
harmlos (ein Node zeichnet die Ausgaenge, die der Registry-Eintrag nennt); die
Frage ist nur bei zusaetzlichen **Eingaengen** heikel, siehe § 6.2.

### 3.7 Editor

Zwei Dinge, beide in Schritt „Layers, UI":

- **Projekt-Einstellungen**: eine Seite in `EditorSettingsPanel` mit 16
  Namensfeldern und der Dreiecksmatrix aus Checkboxen. Nachbarschaft: die
  bestehenden Projekt-Bloecke dort (`EditorSettingsPanel.cpp:1194ff`), die
  schon die Regel „gehoert dem PROJEKT, wird in die `.heproj` geschrieben"
  tragen — inklusive der Falle in der Datei selbst (`:1285`): die **Datei** wird
  am Ende einer Bearbeitung geschrieben, nicht pro Tastendruck.
- **Details-Panel**: eine Combo `Collision Layer` in der `RigidBodyComponent`-
  und der `CharacterControllerComponent`-Zeile (`InspectorPanel.cpp:1428ff` als
  Nachbar). Die Combo listet die Namen aus der Config, nicht Zahlen.

Beide brauchen einen F1-Tooltip-Eintrag — die Deckung ist ein ctest.

---

## 4. Constraints/Joints

### 4.1 Welche Typen, in welcher Reihenfolge

Jolt v5.5 liefert Fixed, Point, Hinge, Slider, Distance, Cone, SwingTwist,
SixDOF, Path, Pulley, Gear, RackAndPinion.

**Erste Runde — fuenf:**

| Typ | Jolt | Wofuer |
| --- | --- | --- |
| `Fixed` | `FixedConstraint` | zwei Teile zusammenschweissen, zerbrechliche Objekte |
| `Point` | `PointConstraint` | Kugelgelenk; die Grundlage jeder Kette |
| `Hinge` | `HingeConstraint` | Tuer, Deckel, Rad — mit Limits und Motor |
| `Slider` | `SliderConstraint` | Schublade, Aufzug, Kolben — mit Limits und Motor |
| `Distance` | `DistanceConstraint` | Seil, Grapple, Federaufhaengung |

**Zweite Runde, nicht in diesem Thema:** `SwingTwist` und `SixDOF`. Das sind die
Ragdoll-Typen und sie ziehen `JPH::Ragdoll` samt Skelett-Mapping nach sich — das
ist ein eigenes Thema neben der Skeletal-Animation, nicht ein sechster Eintrag in
dieser Liste. Die Roadmap darf „Basis fuer Ragdolls" sagen, weil diese fuenf sie
sind; „Ragdolls" darf sie nicht sagen.

### 4.2 Beide Wege hinein, ein Bauweg

Wie bei Koerpern (`buildBodyFor` bedient `initialize()` **und** `addEntity()`,
`PhysicsWorld.h:333-341`): eine private `buildJointFor(world, entityId)`, die
beide Wege benutzen.

- **Autoring**: `JointComponent` auf Entity A, die B nennt. Das ist der Weg, der
  serialisiert, in Prefabs steckt und im Details-Panel sichtbar ist.
- **Laufzeit**: `physics.addJoint(...)` / `physics.removeJoint(...)`, fuer das
  Seil, das erst beim Abschuss des Grapples entsteht.

Der Laufzeitweg **schreibt die Komponente** und ruft dann `buildJointFor`. Sonst
gibt es zwei Quellen der Wahrheit und ein zur Laufzeit erzeugtes Gelenk
verschwindet beim Speichern.

### 4.3 Die Komponente

```cpp
struct JointComponent {
    JointType   type   = JointType::Fixed;
    std::string targetUuid;          // die ANDERE Entity, per UUID
    glm::vec3   anchorA{0}, anchorB{0};   // LOKAL zur jeweiligen Entity
    glm::vec3   axis{0,1,0};              // Hinge-Achse / Slider-Richtung, lokal zu A
    float       minLimit = 0.0f, maxLimit = 0.0f;  // Winkel (rad) oder Strecke (m); min>=max = frei
    float       motorTarget = 0.0f;                 // Zielgeschwindigkeit; 0 = Motor aus
    float       motorMaxForce = 0.0f;
    float       breakForce = 0.0f;                  // 0 = unzerbrechlich
    bool        collideConnected = false;           // duerfen A und B einander treffen
};
```

**Nur eine Komponente, nicht fuenf.** Ein `HingeJointComponent` neben einem
`SliderJointComponent` waere sauberer getrennt, kostet aber fuenfmal
Serializer, fuenfmal Details-Panel, fuenfmal Tooltip-Deckung und macht den
Typwechsel im Editor zu „loeschen und neu anlegen". Das Muster im Baum ist
`ColliderComponent`: **ein** Struct, dessen Felder je nach `shape` gelesen
werden oder nicht, mit genau diesem Kommentar im Header
(`ColliderComponent.h:7-24`). Der Kommentar dort ist die Vorlage fuer den hier.

**Zielangabe per UUID, nicht per `entt::entity`.** Ein roher Handle ueberlebt
weder Speichern noch Prefab-Instanziierung noch die Kollaborationssitzung. Die
UUIDs gibt es seit CP-A (`EntityIdComponent`), und `SceneSerializer` schreibt
schon UUID-Referenzen.

**Ein Gelenk pro Entity.** Eine Kette wird als Kette von Entities gebaut, jede
mit einem `JointComponent`, das die vorherige nennt. Mehrere Gelenke an einer
Entity braeuchten einen Vektor in der Komponente, und der braucht eine
Listen-UI im Details-Panel — beides spaeter nachruestbar, ohne dass sich das
Dateiformat aendert (ein Array mit einem Element).

**Ankerraum.** `anchorA`/`anchorB` sind **lokal zur jeweiligen Entity** — das
ist die Hausregel („PhysicsWorld spricht WELT, die Skript-Transform-API LOKAL,
umgerechnet wird an der Grenze"), und lokal ist auch das einzig Sinnvolle: ein
Weltanker in einem Prefab waere beim Platzieren falsch. Umgerechnet wird beim
**Bauen**, ueber `HE::worldMatrixOf` / `HE::worldPositionOf` aus
`TransformHierarchy` — **niemals** ueber `TransformComponent::worldMatrix`. Das
Feld schreibt nur `propagateTransforms`, und in `SceneSystems::tickWorld` laeuft
das nicht; fuer eine gerade gespawnte Entity ist es die Identitaet. Der Fehler
ist in diesem Baum viermal aufgetreten (Physik, ParticleSystem, LODSystem,
Kamerapos) und waere hier besonders unauffaellig, weil ein Gelenk mit falschem
Anker nicht abstuerzt, sondern das Objekt langsam wegzieht.

### 4.4 Laufzeit-API

Auf `PhysicsWorld`, alle jolt-frei:

```cpp
bool addJoint(HorizonWorld&, uint32_t entityA, uint32_t entityB, const JointDesc&);
bool removeJoint(uint32_t entityA);
bool hasJoint(uint32_t entityA) const;
bool setJointMotor(uint32_t entityA, float targetSpeed, float maxForce);
```

`JointDesc` ist derselbe Satz Felder wie die Komponente, ohne `targetUuid`
(die Entity steht im Parameter). Auf `HE::api::physics` gespiegelt und in die
Registry — dort mit Entity-Ids statt UUIDs, wie jede andere Zeile.

`setJointMotor` ist das, was eine Tuer aufgehen laesst und ein Aufzug braucht;
ohne sie ist der Motor eine reine Autoring-Einstellung.

### 4.5 Die drei Lebenszyklus-Fallen

Die kosten mehr Sorgfalt als alles andere in diesem Thema.

**(a) Zerstoerung.** Jolt asserted, wenn ein Body zerstoert wird, an dem noch ein
Constraint haengt. `destroyBodyFor` (`:1351`) muss also **zuerst** alle
Constraints entfernen, die diesen Body auf einer der beiden Seiten nennen — und
zwar auch die, die eine **andere** Entity haelt (die Kette, deren mittleres Glied
stirbt). Es braucht daher eine Rueckwaerts-Zuordnung Body → Constraints, nicht
nur Entity-A → Constraint. `clear()` (`:2250`) muss alle Constraints vor allen
Bodies abraeumen, und die Nachlese in `step()` (`:1527`) laeuft ueber
`destroyBodyFor`, ist also automatisch mit versorgt — solange sie es bleibt.

**(b) Wiederaufbau.** `addEntity` ist dokumentiert idempotent: „ein vorhandenes
Abbild wird zuerst abgerissen" (`PhysicsWorld.h:311-315`). Der Abriss geht durch
`destroyBodyFor`, also faellt nach (a) das Gelenk mit — und muss danach neu
angelegt werden. Betroffen ist nicht nur die Entity selbst, sondern jede, deren
Gelenk auf sie zeigt. Ohne das verliert ein Collider-Wechsel im laufenden Spiel
still die halbe Kette.

**(c) Reihenfolge.** `initialize()` muss **zweiphasig** werden: erst alle Bodies,
dann alle Gelenke. Ein Gelenk kann nicht gebaut werden, bevor beide Bodies
existieren, und die Entity-Iteration hat keine Ordnung, die das garantiert.
Fuer den Laufzeit-Spawn (`addEntity`) reicht das nicht: eine Entity kann ein
Gelenk auf eine Entity tragen, die es noch nicht gibt. Also eine **Warteliste**
unbefriedigter Gelenke, die `addEntity` nach jedem erfolgreichen Body-Bau
abarbeitet. `addEntityTree` erledigt den haeufigen Fall (Prefab mit Kette) damit
von selbst.

Ein Gelenk, dessen Partner nach dem Szenenstart immer noch fehlt, **loggt einmal
und wird verworfen** — nicht ewig in der Warteliste gehalten. Sonst wird die
Liste zum Leck.

**(d) Partner ohne Body.** Eine Entity mit CharacterController und ohne
RigidBody hat keinen Jolt-Body und kann kein Constraint-Partner sein
(`CharacterVirtual` ist kein Body). Das muss **ablehnen und loggen**, nach der
Regel der Schreibseite (`PhysicsWorld.h:216-224`): ein stilles No-Op liest sich
fuer den Autor als „Physik ist kaputt". Der uebliche Fall — der PlayerCharacter
— hat beides und funktioniert.

### 4.6 Bruch

`breakForce > 0`: nach dem Step die `GetTotalLambdaPosition`-Groesse der
Constraints pruefen und ueberschrittene entfernen. Das erzeugt ein Ereignis, das
Spiellogik hoeren will — dieselbe Form wie die Kollisionsereignisse:
`pollJointBroken()` liefert die Entity-Paare seit dem letzten Aufruf. Die
Ereignis-Infrastruktur (`HEContactListener` puffert, `poll*` leert) steht schon
und wird nur nachgeahmt.

Der Bruch ist **optionaler Teil des letzten Constraint-Schritts**. Faellt er
weg, faellt `breakForce` mit weg; ein serialisiertes Feld, das nichts tut, ist
schlimmer als keins.

### 4.7 Debug-Zeichnung

Der Collider-Debug-Draw des Editors liegt in
`LevelScriptPanel.cpp:1689-1801` und zeichnet aus den **Komponenten**, nicht aus
Jolt. Gelenke gehoeren genauso gezeichnet: eine Linie A-Anker → B-Anker plus ein
typabhaengiges Symbol (Achse beim Hinge, Strecke beim Slider). Jolts eigenes
`DrawConstraints` scheidet aus — es setzt `JPH_DEBUG_RENDERER` und einen
`DebugRenderer` voraus, den die Engine nicht hat, und es zoege einen
Jolt-Typ ueber die PIMPL-Grenze.

---

## 5. Shape-Casts und Overlap-Queries

### 5.1 Was fehlt

| Neu | Was es kann, was `sphereCast` nicht kann |
| --- | --- |
| `boxCast` | ein Fahrzeug, ein Aufzug, alles Kastenfoermige; orientiert |
| `capsuleCast` | die Bewegung einer Spielfigur vorausschauen („passe ich da durch") |
| `overlapBox` | Raumbereiche ohne Trigger-Volumen; Auswahlrechtecke |
| `overlapCapsule` | „steht jemand in meiner Figur drin" beim Respawn |
| `raycastAll` | Durchschuss durch mehrere Ziele; Sicht durch Glas |

### 5.2 Umbau statt Copy-Paste

`sphereCast` (`:1898-1962`) und `overlapSphere` (`:1965-1989`) sind je ~60
Zeilen, von denen genau eine die Form nennt (`new JPH::SphereShape(radius)`).
Erster Schritt ist deshalb ein Refactoring **ohne** Verhaltensaenderung:

```cpp
// privat, in der .cpp
RaycastHit            castShapeImpl (const JPH::Shape&, origin, dir, maxDist, ignore, layerMask) const;
std::vector<uint32_t> overlapShapeImpl(const JPH::Shape&, const JPH::RMat44&, ignore, layerMask) const;
```

`sphereCast`/`overlapSphere` werden dann Dreizeiler, die neuen ebenso. Die
Feinheiten, die dabei erhalten bleiben muessen und die im Kommentar dort schon
begruendet stehen:

- `mBackFaceModeTriangles = IgnoreBackFaces` (`:1930`) — sonst meldet eine
  beruehrte Flaeche einen Treffer bei Fraktion 0.
- Die Distanz kommt aus der **Fraktion**, nicht aus dem Kontaktpunkt (`:1944`) —
  sonst steht die Kamera in der Wand.
- Die Normale ist die **negierte** `mPenetrationAxis` (`:1952-1954`).
- `RShapeCast::sFromWorldTransform` mit `origin` als Basisoffset (`:1922`) und
  `CollideShape` mit `at` als Basisoffset (`:1985`) — Praezision fern vom
  Weltursprung.
- **Sensorregel, unveraendert:** ein *Cast* ueberspringt Sensoren
  (`skipSensors=true`), ein *Overlap* meldet sie (`false`). Ein Cast fragt „was
  blockiert mich", ein Overlap fragt „was ist hier".

Box und Kapsel brauchen eine **Orientierung**. `boxCast(origin, halfExtents,
rotation, dir, maxDist, …)`; fuer die Registry (die keinen Quaternion-Typ hat)
ein Euler-`Vec3` in Grad, wie es die uebrige flache Oberflaeche macht.

`raycastAll` braucht einen `AllHitCollisionCollector` statt des
`ClosestHitCollisionCollector` und muss **nach Distanz sortiert** liefern —
Jolt garantiert die Reihenfolge nicht, und „das erste Ding, das die Kugel
trifft" ist die Frage, die jeder Aufrufer stellt.

---

## 6. Querschnitt: was jede neue API-Funktion kostet

### 6.1 Die Checkliste je Registry-Eintrag

1. Methode auf `PhysicsWorld` (Header-Kommentar in der Ausfuehrlichkeit der Nachbarn).
2. Duenne Weiterleitung in `HE::api::physics` (`EngineApi.h` + `.cpp`).
3. Registry-Zeile in `EngineApi.cpp` (~4553-4600er Block).
4. Anzeigename in der Tabelle `EngineApi.cpp:5755-5775` — **mit Suffix, wenn der
   Name mit Transform/Movement kollidiert** („Set Position (Physics)" ist der
   Praezedenzfall; das Add-Menue listet die Registry flach).
5. Beschreibung in `HcNodeDocs.cpp`. **Pflicht**, sonst ist der Build rot:
   `tests/test_hc_node_docs.cpp:24-46` prueft jeden Registry-Eintrag auf eine
   Beschreibung ≥ 20 Zeichen ohne Doppelleerzeichen, und `:95-107` prueft, dass
   jeder Eintrag im Node-Reference-Handbuch auftaucht.
6. Testfall in `tests/test_engine_api.cpp` (Registry-Rundlauf) und in
   `tests/test_physics.cpp` (Verhalten).

### 6.2 Die Arity-Falle

Ein bestehender HorizonCode-Graph zeichnet die Eingaenge, die der
Registry-Eintrag **heute** nennt. Kommt ein Parameter dazu, hat ein
gespeicherter Node einen Eingang zu wenig. Der Codegen faengt das ab —
`HcCodegen.cpp:2481-2487` schreibt `i < n.params.size() ? input(...) :
zeroLit(...)`, ein fehlender Eingang wird also zu einer Null. Fuer einen
Layer-Parameter waere „Null" aber „kein Layer erlaubt", also das Gegenteil des
gewuenschten Defaults.

**Entscheidung: die Layer-Maske kommt nicht an die bestehenden Signaturen.**
`physics.raycast`, `physics.sphereCast` und `physics.overlapSphere` behalten ihre
Parameter. Die Layer-Filterung bekommt eigene Eintraege
(`physics.raycastLayers`, `physics.sphereCastLayers`, `physics.overlapSphereLayers`),
und die **neuen** Formen (Box/Kapsel/`raycastAll`) tragen die Maske von Anfang an
als letzten Parameter. Die C++-Ebene (`PhysicsWorld`, `HE::api::physics`) bekommt
dagegen einen Default-Parameter — dort gibt es keine gespeicherten Aufrufer.

Vor der Umsetzung von Schritt „Layers, Queries" ist noch zu pruefen, ob der
**Interpreter** (`HcGraphHost.cpp:1207ff`) sich genauso verhaelt wie der Codegen;
der Codegen ist belegt, der Interpreter nicht.

### 6.3 Komponentenfelder

Je neues Feld auf `RigidBodyComponent` / `CharacterControllerComponent` /
`JointComponent`:

- `SceneSerializer.cpp` Schreiben **und** Lesen (`:361-380` schreibt, `:961-980`
  liest) — mit tolerantem Default, sonst verlieren bestehende Szenen beim ersten
  Speichern etwas.
- `InspectorPanel.cpp` Zeile + Undo-Snapshot (`:1428ff` als Muster).
- F1-Tooltip-Eintrag; die Deckung ist ein ctest.
- `JointComponent` zusaetzlich: Eintrag in der „Add Component"-Liste
  (`InspectorPanel.cpp:2042`) und in `AssetRefScan` (das `targetUuid` ist eine
  Entity-Referenz, kein Asset — pruefen, ob der Scan das ueberhaupt betrifft).

### 6.4 Weiteres

- `HE_Scene` bekommt **nie** `HE_API`; `HE_Core` braucht es (Windows-CI-Regel).
- Der Renderer wird nicht angefasst. Einzige UI-Beruehrung: Editor-Panels
  (Einstellungen, Details, Debug-Draw).
- `kMaxBodies = 1024` (`:333`) bleibt. Gelenke haben ihr eigenes Limit
  (`max contact constraints` in `Init()`, heute 1024) — beim Bau der
  Constraint-Verwaltung pruefen, ob Jolt ein separates Limit fuer
  `TwoBodyConstraint` fuehrt oder ob das mitgezaehlt wird.

---

## 7. Vorschlag fuer den Schnitt der Schritte 2–5

Der Schnitt vom Brett geht von vier gleich grossen Bloecken aus. Nach § 0 ist
einer davon fertig und einer doppelt so gross wie gedacht. Vorschlag:

| Schritt | Inhalt | Abhaengig von |
| --- | --- | --- |
| **2** | **Layers, Kern.** `CollisionLayerConfig` in `HE_Core`, Layer-Kodierung + Filter in `PhysicsWorld`, Feld auf `RigidBodyComponent` + `CharacterControllerComponent`, Serializer, `setCollisionLayers`, Character-Filter. Tests: Matrix trennt zwei Koerper, Static↔Static bleibt aus, Default-Config = heutiges Verhalten. | — |
| **3** | **Layers, Queries + UI.** `HELayerMaskFilter`, `layer` in `RaycastHit`, die drei `…Layers`-Registry-Eintraege, Projekt-Einstellungsseite mit Matrix, Details-Combo, `.heproj`/`.hcfg`-Durchreichung. | 2 |
| **4** | **Shape-Casts.** Refactoring auf `castShapeImpl`/`overlapShapeImpl`, dann `boxCast`, `capsuleCast`, `overlapBox`, `overlapCapsule`, `raycastAll` — alle mit Layer-Maske. Dazu der Kraefte-Rest (`addForceAtPosition`, `addImpulseAtPosition`, `set/getAngularVelocity`), weil er in dieselben Dateien faellt und zusammen eine halbe Stunde kostet. | 3 |
| **5** | **Constraints, Kern.** `JointType`, `JointComponent`, `buildJointFor`, die zweiphasige `initialize`, die Warteliste, die Aufraeumung in `destroyBodyFor`/`clear`, Serializer, Laufzeit-API + Registry. Fuenf Typen. | 2 (Layer beim Body-Bau) |
| **6** | **Constraints, Rest.** `collideConnected`, Motor-API, Bruch + `pollJointBroken`, Details-Panel, Debug-Zeichnung, Handbuch/Tooltips. | 5 |

Also **fuenf statt vier** verbleibende Schritte. Schritt 5 ist der groesste und
laesst sich nicht sinnvoll weiter teilen: die drei Lebenszyklus-Fallen aus § 4.5
muessen zusammen mit dem ersten Constraint kommen, sonst ist der Zwischenstand
einer, der beim Loeschen einer Entity abstuerzt.

---

## 8. Offene Punkte fuer den naechsten Schritt

1. ~~**Interpreter-Arity**~~ — **beantwortet in Schritt 3, und schlimmer als
   gedacht.** Der Interpreter (`HorizonCode.cpp:3157` fuer den Exec-Fall,
   `:3779` fuer den puren) baut `args` aus `n.params.size()`, also aus der am
   Node GESPEICHERTEN Parameterliste, und die Lese-Helfer in `EngineApi.cpp`
   (`aF`/`aI`/`aV3`, `:4434-4457`) antworten fuer einen fehlenden Index die Null
   des Typs. Ein Node, der einen Parameter zu wenig hat, uebergibt also eine
   Null — genau wie `HcCodegen`s `zeroLit`, nur ohne dass irgendwo ein Literal
   sichtbar wuerde. **Fuer AUSGAENGE ist beides harmlos:** beide Pfade lesen ein
   Ergebnis ueber einen bereichsgepruefen Index, ein ANGEHAENGTER Ausgang ist
   fuer einen alten Node unsichtbar. Deshalb hat `raycast`/`sphereCast` jetzt
   `layer` als sechsten Ausgang, waehrend die Maske unter drei neuen Namen kommt.
2. ~~**Constraint-Limit**~~ — **beantwortet in Schritt 5: es gibt keins.**
   `ConstraintManager::mConstraints` ist ein dynamisches `JPH::Array`
   (`ConstraintManager.h:96`), das `max contact constraints` aus
   `PhysicsSystem::Init` zaehlt ausschliesslich Kontakte. Gelenke brauchen also
   weder ein Limit noch eine Zaehlung noch eine Warnung wie `kMaxBodies`. Die
   alte Frage steht darunter, damit man sieht, was gefragt war:
   ~~fuehrt `PhysicsSystem::Init`s `max contact constraints`
   auch die `TwoBodyConstraint`s, oder gibt es kein eigenes Limit (§ 6.4)?~~
3. ~~**`.heproj`-Durchreichung**~~ — **beantwortet in Schritt 3: jedes Feld
   braucht seine eigene Zeile**, an fuenf Stellen. `ProjectData`
   (`ProjectManager.h`) + Lesen/Schreiben (`ProjectManager.cpp:1525/1631`),
   `ExportSettings` (`ProjectExporter.h`), die Uebergabe im Export-Dialog
   (`ExportDialogPanel.cpp:1291`), die Kopie im Exporter
   (`ProjectExporter.cpp:1072`) und `ProjectConfig` + Reader/Writer. Die `.hcfg`
   ist BINAER mit versionierten Schwaenzen: die Matrix ist **v6**, ein String mit
   demselben JSON, das auch in der `.heproj` steht. Geschrieben nur, wenn
   `!isDefault()` — ein Projekt, das die Matrix nie angefasst hat, emittiert
   weiter v2, sonst weigert sich ein aelteres Runtime-Bundle daneben und bootet
   ohne sein Pak. Dieselbe Sparsamkeit in der `.heproj`: kein Schluessel, solange
   nichts geaendert wurde.
4. ~~**Terrain-Layer**~~ — vom Brett entschieden und in Schritt 2 gebaut: eigener
   vorbelegter Kanal `Terrain` (4).

---

## 9. Was Schritt 3 gebaut hat (Layers, Queries + UI)

- `HELayerMaskFilter` in `PhysicsWorld.cpp`, im **ObjectLayerFilter-Slot** der
  drei Queries (der zweite `{}`), nicht in `HEQueryFilter` — Broadphase statt
  Narrow-Phase. `PhysicsWorld::kAllLayers` neben `kNoEntity`; `layerMask` als
  LETZTER, defaultierter Parameter von `raycast`/`sphereCast`/`overlapSphere`.
- `RaycastHit::layer` (`uint8_t`), gefuellt aus `body.GetObjectLayer()` in beiden
  Lock-Bloecken; gespiegelt als `int layer` auf `HE::api::physics::RaycastHit`
  und als sechster Registry-Ausgang von `physics.raycast`/`physics.sphereCast`.
- Drei neue Registry-Eintraege `physics.raycastLayers` /
  `physics.sphereCastLayers` / `physics.overlapSphereLayers`, samt Anzeigenamen
  („Raycast (Layers)" …) und `HcNodeDocs`-Beschreibungen. Die Maske ist ein
  BITFELD, kein Kanal-Index; die Beschreibungen sagen das, weil es in diesem
  Schritt keinen Hilfs-Node dafuer gibt.
- **Der Unterschied, den ein Test festnagelt:** die Maske sagt, was eine Query
  SEHEN darf; die Matrix sagt, was die Simulation AUFLOEST. Ein Strahl auf einem
  Kanal, der mit nichts kollidiert, findet trotzdem, was seine Maske nennt.
- Projekt-Einstellungsseite **Project ▸ Collision Layers**: 16 Namensfelder
  (Platzhalter zeigt, wie ein leerer Name zurueckliest) und die Matrix als
  DREIECK — die obere Haelfte waere dieselbe Antwort ein zweites Mal, und
  `setCollides` schreibt ohnehin beide Zellen. Datei wird am Ende einer
  Bearbeitung geschrieben, nicht pro Tastendruck. Knopf „Everything Collides"
  als Rueckweg.
- `AppContext::applyCollisionLayers` (Callback, kein `PhysicsWorld*`: die Welt
  entsteht beim Play-Start und stirbt beim Stopp) — eine Matrix-Aenderung
  waehrend des Spielens landet sofort in der laufenden Simulation.
- Details-Combo `Collision Layer` auf `RigidBodyComponent` und
  `CharacterControllerComponent`, Namen aus der Projekt-Config, geklemmt NUR
  fuer das Widget (Praezedenzfall: `Collider/Shape`).
- `setCollisionLayers` wird jetzt tatsaechlich gerufen: `GameApplication` aus
  `m_config.collisionLayers`, `EditorApplication` beim Play-Start — beide **vor**
  `initialize()`, weil dort die Kanaele in die Bodies wandern.

---

## 10. Was Schritt 4 gebaut hat (Shape-Casts + Kraefte-Rest)

- `castShapeImpl` / `overlapShapeImpl` als **freie Funktionen im anonymen
  Namensraum** von `PhysicsWorld.cpp`, nicht als private Member: ihre Argumente
  sind Jolt-Typen, und kein Jolt-Typ darf in `PhysicsWorld.h`. Die Formulierung
  in § 5.2 war insoweit nicht umsetzbar. `sphereCast`/`overlapSphere` sind
  Dreizeiler daneben, mit unveraendertem Verhalten (die vorhandenen Tests waren
  der Beleg dafuer, bevor irgendeine neue Form dazukam).
- Fuenf neue Abfragen: `boxCast`, `capsuleCast`, `overlapBox`, `overlapCapsule`,
  `raycastAll` — alle mit `layerMask` als letztem, defaultiertem Parameter.
- **Die Drehung ist ein Euler-Vec3 in GRAD, auf beiden Ebenen**, und geht durch
  `joltRotationOf`, das denselben Ausdruck benutzt wie `TransformHierarchy`
  (`glm::quat(glm::radians(euler))`). Eine zweite Konvention waere ein Fehler,
  den niemand sehen koennte; ein Test nagelt es fest, indem die ausgerichtete
  Platte weiter fliegt als die verdrehte.
- **Entartete Formen sind eine leere Frage, kein Assert:** Jolt behauptet
  `halfHeight > 0` und `radius > 0` fuer die Kapsel, ein Debug-Build stirbt
  daran. Halbausdehnung/Radius ≤ 0 antwortet „nichts". Eine Kapsel, die nur aus
  Kappen besteht (`height ≤ 2·radius`), behaelt einen Millimeter Zylinder und
  ist damit die Kugel, die der Aufrufer gemeint hat.
- `raycastAll` sortiert selbst (`AllHitCollisionCollector::Sort()`; Jolt
  verspricht keine Reihenfolge) und meldet **jede Entity einmal, die naechste
  Beruehrung**: ein Netz meldet Vorder- und Rueckwand. `RayCastSettings` bleibt
  auf den Vorgaben, weil genau die der Ein-Treffer-`CastRay` implizit benutzt —
  `hits[0]` ist damit derselbe Treffer, den `raycast` gemeldet haette.
- Kraefte-Rest: `addForceAtPosition`, `addImpulseAtPosition`,
  `set/getAngularVelocity`.
- **Die Ausnahme von der Lokal-Regel:** der Angriffspunkt der beiden ersten ist
  eine WELT-Position, obwohl `EngineApi.h` als Regel fuehrt, dass eine Position
  neben einer Entity lokal ist. Begruendung im Header an beiden Stellen: es ist
  keine Pose, sondern der Ort, an dem der Stoss landet, und jede Quelle dafuer
  (Raycast-Treffer, Explosionszentrum) ist bereits Welt.
- **Winkelgeschwindigkeit ist in RADIANT pro Sekunde**, die einzige Stelle
  dieser Flaeche, die nicht in Grad rechnet — es ist eine Rate, keine Lage.
  Steht im Header UND in der Node-Beschreibung. Keine Character-Weiche wie bei
  `setVelocity`: ein `CharacterVirtual` hat keine Drehung.
- Neun Registry-Zeilen, alle mit Anzeigename und `HcNodeDocs`-Beschreibung.
  **Die neuen Formen brauchen keine `…Layers`-Zwillinge** (§ 6.2), weil sie die
  Maske von Anfang an tragen. `physics.raycastAll` liefert **fuenf parallele
  Arrays** (entities/points/normals/distances/layers) — ein Graph-Wert ist eine
  Liste EINES Typs, ein Array von Strukturen gibt es nicht. Vec3-Arrays hatten
  keinen Praezedenzfall in der Registry; Lua-, Python- und ForEach-Pfad lesen
  den Elementtyp generisch vom Array, ein Test haelt das fest.
- 122/122 ctest-Ziele gruen (3 uebersprungene `runtime_size`-Ziele wie immer),
  22 neue Testfaelle/Bloecke in `test_physics.cpp` und `test_engine_api.cpp`.

---

## 11. Was Schritt 5 gebaut hat (Constraints, Kern)

`HE::JointType` (fuenf Werte, append-only wie `ColliderShape`), `JointComponent`,
`PhysicsWorld::buildJointFor` samt zweiphasiger `initialize`, Warteliste,
Aufraeumung in `destroyBodyFor`/`clear`, Serializer, Laufzeit-API und drei
Registry-Zeilen.

### 11.1 Vier Abweichungen vom Entwurf in § 4.3/§ 4.4

- **Limits sind GRAD, nicht Radiant.** § 4.3 sagte Radiant. `PhysicsWorld.h:401`
  sagt aber ausdruecklich, die Winkelgeschwindigkeit sei „the one place in this
  API that is not in degrees, because it is a rate rather than a pose" — ein
  Hinge-Limit ist eine Pose. `glm::radians()` an der Jolt-Grenze.
- **`target` ist `HE::UUID`, nicht `std::string`.** Das Haus-Muster fuer eine
  Entity-Referenz (`RopeComponent::attachStart`, `CameraRigComponent::target`),
  aufgeloest ueber `HorizonWorld::findByEntityId`.
- **Anker pro Typ, nicht zwei Anker fuer alle.** Zwei getrennte Weltanker sagen
  Jolt „mache diese beiden Punkte zu einem" und reissen ein Point- oder
  Hinge-Gelenk im ersten Schritt zusammen. Die Tabelle steht im Header von
  `JointComponent`, im Stil von `ColliderComponent.h:12-24`:
  Fixed liest nichts (`mAutoDetectPoint`), Point und Hinge nehmen `anchorA` als
  EINEN gemeinsamen Drehpunkt, Slider nimmt `axis` (plus `mAutoDetectPoint`),
  und Distance ist der einzige Typ, der beide Anker liest.
- **Motor, Bruch und `collideConnected` kommen nicht mit** — weder als
  Komponentenfeld noch als Parameter. Sie gehoeren nach § 7 zu Schritt 6, und
  § 4.6 sagt selbst, dass ein serialisiertes Feld, das nichts tut, schlimmer ist
  als keins. Die Arity-Falle trifft sie nicht: `setJointMotor` und
  `pollJointBroken` sind ohnehin eigene Registry-Zeilen.

### 11.2 Die drei Lebenszyklus-Fallen, wie sie geloest sind

- **(a) Zerstoerung.** `destroyJointsInvolving(entityId, requeue)` laeuft als
  ERSTES in `destroyBodyFor`, vor jedem `RemoveBody`. Beide Richtungen: das
  Gelenk, das die Entity selbst geschrieben hat, UND jedes, das auf sie zeigt
  (das mittlere Kettenglied). Ein Jolt-Constraint haelt rohe `Body*`, und dafuer
  gibt es keine Assertion — ein zurueckgelassenes Constraint liest im naechsten
  Step freigegebenen Speicher. `clear()` raeumt alle Constraints vor allen
  Bodies ab.
- **(b) Wiederaufbau.** `destroyBodyFor` setzt die betroffenen Besitzer mit
  `requeue=true` zurueck auf die Warteliste; `addEntity` arbeitet sie nach jedem
  erfolgreichen Body-Bau ab. Ein Collider-Wechsel im laufenden Spiel verliert
  also weder das eigene Gelenk noch die halbe Kette.
- **(c) Reihenfolge.** `initialize()` ist zweiphasig: erst alle Bodies, dann in
  einem zweiten Durchlauf alle Gelenke. Ein Test baut dieselbe Szene in beiden
  Erzeugungsreihenfolgen — der Fehler waere in genau einer davon unsichtbar.
  Die Warteliste (`Impl::pendingJoints`) traegt einen Zaehler, der bei JEDEM
  Durchlauf mit Fortschritt auf null geht: eine Kette loest sich pro Durchlauf
  um ein Glied auf, und die hinteren Eintraege sind geduldig, nicht kaputt. Erst
  acht folgenlose Durchlaeufe geben auf, mit einer Meldung.
- **(d) Partner ohne Body.** Warnt und bleibt in der Warteliste (ein Body kann
  spaeter kommen); die Aufgabe-Meldung hat das letzte Wort. Zwei statische
  Bodies, ein Selbstbezug, eine Achse ohne Laenge und ein leeres `target` sind
  dagegen harte Absagen mit `HE_LOG_ERROR` und fliegen sofort von der Liste.

### 11.3 Was noch nicht geht — eine Luecke, die aelter ist als dieser Schritt

**Prefab-Instanziierung remappt KEINE Entity-Referenzen.** `applyPrefabJson`
(`SceneSerializer.cpp:1745ff`) praegt frische UUIDs und ruft `applyComponents`
mit den Rohdaten auf; ein `target`, ein `RopeComponent::attachStart` und ein
`CameraRigComponent::target` zeigen danach auf die Entity im QUELL-Baum (oder
ins Leere), nicht auf das Geschwister in derselben Prefab-Instanz. Das trifft
also alle drei Komponenten gleichermassen und ist nicht durch die Gelenke
entstanden. § 4.5 (c) hoffte, `addEntityTree` erledige „Prefab mit Kette" von
selbst — das stimmt fuer die BODIES, nicht fuer die Referenzen. Ein
`idMap`-Durchlauf ueber die drei Felder in `applyPrefabJson` waere die
Reparatur; sie gehoert nicht in dieses Thema.

### 11.4 Restliches

- `HorizonWorld::reserveComponentStorage()` reserviert den `JointComponent`-Pool
  mit: `physics.addJoint` SCHREIBT die Komponente, ein hot-geladenes
  Game-Logic-dylib waere sonst der erste, der den Pool anfasst.
- Der Serializer behandelt einen unbekannten `type` wie `jsonToColliderShape`
  einen unbekannten Shape: Fixed, und eine Warnung, statt eines stillen Casts
  auf 0. `"joint"` steht in `isKnownComponentKey` (sonst warnt der Loader, eine
  Komponente werde VERWORFEN, waehrend sie einwandfrei laedt).
- Drei Registry-Zeilen: `physics.addJoint` (acht Parameter, alle von Anfang an —
  ein gespeicherter Node kann keinen Eingang nachwachsen lassen),
  `physics.removeJoint`, `physics.hasJoint`. Anzeigenamen und
  `HcNodeDocs`-Beschreibungen dazu.
- 122/122 ctest-Ziele gruen, 19 neue Testfaelle in `test_physics.cpp`,
  `test_scene_serializer.cpp` und `test_engine_api.cpp` — darunter je einer pro
  Gelenktyp mit einer messbaren physikalischen Aussage (das Pendel behaelt
  seinen Radius, der Slider traegt das Gewicht, das Seil faengt den Fall) und
  einer, der einen Anker an einer 100 m entfernten Elternkette prueft.

---

## 12. Was Schritt 6 gebaut hat (Constraints, Rest)

`collideConnected`, Motor, Bruch samt `pollJointBroken`, das Details-Panel, die
Debug-Zeichnung und Handbuch/Tooltips. Damit ist das Thema durch.

### 12.1 Die eine Entscheidung, die aelter ist als dieser Schritt: das Vorzeichen

**Positiv heisst ab jetzt: DIESE Entity bewegt sich entlang IHRER Achse.** Jolt
misst andersherum — ein `HingeConstraint` und ein `SliderConstraint` messen
Winkel und Weg als Bewegung von **body2 relativ zu body1**, und body1 ist hier
die Entity, die die Komponente traegt. Mit der Achse, wie sie dasteht, haette
„oeffne bis 90°" also den Rahmen gedreht statt die Tuer.

Gemessen, nicht hergeleitet: eine Sonde mit Achse (0,1,0) und Arm auf +X ergab
`Motor +1 → z = +0.33`, also eine Drehung gegen die Rechte-Hand-Regel, und
Limits `0..45` liessen genau dieselbe Richtung zu. Jolt ist also in sich
schluessig — Limit und Motor teilen die Konvention. Deshalb wird **die Achse
einmal negiert**, an der Jolt-Grenze in `buildJointFor`, und nicht Limit und
Motor je fuer sich: ein Motor, der von seinem eigenen Limit wegdreht, ist der
Fehler, in dem sich das sonst zeigen wuerde. Ein Test pinnt beide Richtungen.

Kein bestehender Test kippt dadurch: die aus Schritt 5 benutzen symmetrische
Bereiche (−5..5, −0.5..0.5), in denen das Vorzeichen nicht sichtbar ist.

### 12.2 collideConnected liegt im Kontakt-Listener, nicht in Jolt

Jolt hat kein Flag „diese beiden duerfen sich nicht beruehren". Das, was es
anbietet — Kollisions-GRUPPEN am Koerper — vergibt eine Gruppe pro Koerper und
kann „A ignoriert B, B ignoriert C, A trifft C weiter" nicht ausdruecken; genau
das ist aber die Form einer Kette. Also `HEContactListener::OnContactValidate`
mit einer Menge von Body-Paar-Schluesseln (derselbe `bodyPairKey`, also mit
Sequenznummer — ein recycelter Body-Slot erbt keine alte Ausnahme).

**Die Falle dabei:** `PhysicsSystem::ProcessBodyPair` benutzt einen
Body-Pair-Cache und ruft `OnContactValidate` **nicht erneut**, solange keiner der
beiden Koerper seinen Cache fuer ungueltig erklaert hat. Ohne
`BodyInterface::InvalidateContactCache` auf beide blieb ein zur Laufzeit
umgeschaltetes `collideConnected` — und ein durchtrenntes Gelenk — wirkungslos.
Beides laeuft jetzt durch `Impl::setJointedPairCollides`.

Default ist **false**: aufeinanderfolgende Kettenglieder ueberlappen bauartbedingt
und wuerden sonst gegen das Gelenk arbeiten, das sie haelt. Das ist auch, was
Unity und Unreal voreinstellen. Kein Test aus Schritt 5 haengt daran.

### 12.3 Motor: die KRAFT ist der Schalter

`motorMaxForce <= 0` heisst aus. Damit faengt kein altes Projekt an, sich zu
bewegen (0 ist der Default), und „Ziel 0 **mit** Kraft" bleibt als BREMSE
benutzbar, die eine Tuer zuhaelt — genau der Fall, den ein Ziel-als-Schalter
unerreichbar gemacht haette. Nur Hinge und Slider; die anderen drei bekommen
eine Absage mit Log, kein stilles No-Op. `motorTarget` ist eine RATE, also
rad/s (Hinge) bzw. m/s (Slider); der Satz in `PhysicsWorld.h:402` ist von „die
eine Ausnahme" auf die Regel umgeschrieben (Pose = Grad, Rate = Radiant).

Angewandt wird der Motor in **`buildJointFor`**, aus der Komponente — sonst
verlaere ein per Skript gestarteter Aufzug seinen Antrieb beim naechsten
Collider-Wechsel. `setJointMotor` schreibt erst die Komponente, dann das
Constraint, und weckt beide Koerper (Jolt loest kein Constraint zwischen
schlafenden Koerpern).

### 12.4 Bruch

In `step()` direkt nach `Update()`, bevor irgendetwas anderes Jolts Solver-Stand
anfasst: pro Gelenk `GetTotalLambdaPosition` (Vec3 bei Fixed/Point/Hinge,
`Vector<2>` beim Slider, ein Float beim Distance — die Rotations-Lambdas
absichtlich nicht, es bricht der Zug und nicht die Verdrehung), geteilt durch
`dt`, gegen `breakForce`.

**Ein gebrochenes Gelenk nimmt seine KOMPONENTE mit.** Sonst kaeme die Tuer beim
naechsten Rebuild ihres Bodys — oder beim naechsten Szenenladen — wieder in die
Angeln. `pollJointBroken()` liefert die Paare einmal und leert dabei, wie die
Kontakt-Warteschlangen. Ein ZERSTOERTES Gelenk erscheint dort nicht: dieselbe
Linie, die `pollCollisionExit` zieht.

### 12.5 Restliches

- Vier neue Registry-Zeilen statt vier weiterer Parameter an `physics.addJoint`
  (Arity-Falle, § 8.1): `setJointMotor`, `setJointBreakForce`,
  `setJointCollideConnected`, `pollJointBroken` (zwei parallele Int-Arrays wie
  `raycastAll`, exec — Lesen leert).
- Details-Panel: eigener Block „Joint", Typ-Combo mit derselben Klemmung wie die
  Shape-Combo des Colliders, Ziel-Picker wie bei Rope/Camera Rig (NICHT auf
  Entities mit RigidBody gefiltert — Hinweise statt versteckter Eintraege),
  Felder pro Typ, Motor-Zeilen nur bei Hinge/Slider, „Joint" im
  Komponenten-Menue.
- Debug-Zeichnung im Viewport (`EditorApplication.cpp`), aus den KOMPONENTEN und
  fuer JEDES Gelenk der Szene, nicht nur die Auswahl: ein Gelenk ist eine
  Beziehung zwischen zwei Entities, und ob die Linie hingeht, wo der Autor
  denkt, sieht er an einem Ende allein nicht. Weltmatrizen ueber
  `HE::worldMatrixOf`. Jolts `DrawConstraints` scheidet aus (§ 4.7).
- Elf Handbuch-Eintraege unter `Joint/…`; `editor_help_audit` bleibt bei 727/727.
