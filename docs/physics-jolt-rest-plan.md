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
| 0 | `Default` | jeder Body ohne eigene Wahl, das Terrain, jede Static-Geometrie |
| 1 | `Player` | nichts automatisch — der Name existiert, damit die Vorbelegung nicht leer ist |
| 2 | `Trigger` | jeder Body mit `ColliderComponent::isTrigger` |
| 3 | `Character` | der Kinematic-Proxy eines `CharacterControllerComponent` |

Die Vorbelegung von 2 und 3 ist **kein** Automatismus, der die Wahl ueberstimmt:
sie ist nur der Default-Wert des neuen Feldes fuer eine Entity, die nie einen
Layer gewaehlt hat. Explizit gewaehlt schlaegt Vorbelegung.

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

Terrain-Implizitkoerper und Sensoren: `Default` bzw. `Trigger`, fest, weil
niemand einen Component hat, in den er etwas anderes schreiben koennte.

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

1. **Interpreter-Arity** — verhaelt sich `HcGraphHost` bei einem Node mit zu
   wenigen Eingaengen wie der Codegen (§ 6.2)? Belegt ist nur der Codegen.
2. **Constraint-Limit** — fuehrt `PhysicsSystem::Init`s `max contact constraints`
   auch die `TwoBodyConstraint`s, oder gibt es kein eigenes Limit (§ 6.4)?
3. **`.heproj`-Durchreichung** — schreibt `ProjectExporter` einen unbekannten
   Block der `.heproj` automatisch in die `.hcfg` weiter, oder braucht jedes Feld
   seine eigene Zeile? (`allowFiles` hat je eine — vermutlich Letzteres.)
4. **Terrain-Layer** — das implizite HeightField bekommt `Default`. Ob es
   stattdessen einen eigenen vorbelegten Layer verdient, ist eine Frage an den
   Menschen, keine technische.
