# Kamera-Rig: First-Person und Third-Person

Umbauplan für MASTERPLAN-Block **E3 („Es gibt keine Spielkamera")**.

Ziel: First-Person- und Third-Person-Kameras, die einer Ziel-Entity folgen —
inklusive der Option, die Ziel-Entity mit der Kamera mitzudrehen.

---

## Stand

**Umgesetzt** (`2e2505fc`, `ff528aa4`, `8dc841a4`, `21d7cac9`, 1610 Tests grün):

* §2.4 Physik-Stomp gefixt — Rigid-Body-Rückschreiben überspringt Character-Entities
* §2.3 `HE::propagateTransforms` als geteilter Helper
* §3 `CameraRigComponent` + `CameraRigController`, beide Modi, Rotationskopplung
* §2.2 Tick-Reihenfolge: Kamera nach der Physik, in Spiel und PIE
* §4 Inspector, Serializer (JSON + CBOR), Registry-Zeilen für alle vier Frontends

**Nicht verifiziert:** kein Durchlauf im echten Editor/Spiel — bisher nur durch
Tests abgedeckt. Optik, Maus-Gefühl und PIE-Capture stehen noch aus.

**Offen:** alles unter §5, plus der Nebenfund in §6.

---

## 1. Ausgangslage

| Teil | Ort | Zustand |
|---|---|---|
| `CameraComponent` | `src/HE_Scene/include/HorizonScene/Components/CameraComponent.h` | 5 Felder: `fovDegrees`, `nearPlane`, `farPlane`, `isMain`, `orthographic` |
| Auflösung der aktiven Kamera | `RenderExtractor.cpp:89` (`extractCamera`) | Editor-Override → `isMain` → erste Kamera → fixer Fallback |
| Einzige Gameplay-Kamera | `FlyCameraController.cpp` | Freiflug; statisch, kein State, schreibt direkt in den Transform der Hauptkamera |
| Editor-Kamera | `EditorCamera.cpp` | Orbit/Fly/Pan — editor-privat, keine Entity, nicht wiederverwendbar |
| Follow / Spring-Arm / Third-Person | — | existiert nicht |

Die Regel „nur eine `isMain`" ist reine Konvention: die Inspector-Checkbox löscht
das Flag bei anderen Kameras nicht, die Auflösung nimmt die erste gefundene.

---

## 2. Vorbedingungen

### 2.1 Maus-Delta — erledigt

`Input` trägt seit `36023a2c` einen **ereignisbasierten** Maus-Stream
(`MouseFrame`, `Input::ProcessMouseEvent`, `Input::mouse()`), der pro Frame
genau einmal am Frame-Ende geleert wird und nicht beim Lesen drainiert.
`HE::api::input::pushSdlSnapshot(dx, dy)` reicht ihn an die Skript-Frontends
weiter, `input.mouseDelta` liefert also echte Werte.

**Konsequenz für das Rig:** es liest `Input::mouse()`, niemals
`SDL_GetRelativeMouseState`. Damit gibt es keinen Ownership-Konflikt mit
`FlyCameraController`, der weiter den SDL-Akkumulator drainiert — die beiden
Ströme sind unabhängig.

### 2.2 Tick-Reihenfolge — muss geändert werden

`updateCameraController` läuft heute **vor** dem Physik-Step:

```
GameApplication::OnRender
  :869  input::pushSdlSnapshot(...)
  :898  updateCameraController(dt)     ← Kamera
  :902  updateScripts(dt)
  :907  Physik-Step (schreibt Transforms zurück)
  :969  SceneSystems::tick(..., camPos, ...)
```

Bei einer Freiflugkamera ist das egal. Eine Follow-Kamera, die vor der Physik
läuft, folgt der Position des *letzten* Frames — sichtbarer Nachlauf.

**Das Rig muss nach dem Physik-Step und vor `SceneSystems::tick` laufen**
(im Editor analog nach dem `m_isPlaying`-Physik-Block). `SceneSystems::tick`
bekommt dann eine aktuelle `camPos` für LOD und Niederschlag.

### 2.3 Weltmatrizen — geteilter Helper

`worldMatrix` wird ausschließlich in `RenderExtractor.cpp:52` (`propagateFrom`)
propagiert, also *nach* dem Rig-Tick. Das Rig würde eine Frame-alte Weltmatrix
des Ziels lesen.

`propagateFrom` in einen geteilten Helper ziehen und direkt vor dem Rig-Tick
aufrufen. Für Root-Entities ist das billig, und die Kamera sitzt exakt richtig
statt einen Frame hinterher.

### 2.4 Physik stomped die Rotation — blockiert die Kopplung

`PhysicsWorld::initialize` legt für **jede** Entity mit
`TransformComponent` + `RigidBodyComponent` einen Jolt-Body an — ohne Entities
mit `CharacterControllerComponent` auszunehmen (`PhysicsWorld.cpp:303`).

`EntityHost::defaultComponents` gibt jedem `PlayerCharacter` genau diese
Kombination: `CharacterControllerComponent` **plus** einen kinematischen
`RigidBodyComponent`. Ein Standard-Spieler hat also beides — einen
`CharacterVirtual` *und* einen kinematischen Body.

In `PhysicsWorld::step` folgt daraus:

1. Rigid-Body-Rückschreibschleife (`:490`–`:528`): kinematisch ≠ statisch, also
   schreibt sie Position **und Rotation** des Bodys in den Transform. Der Body
   wird nie bewegt — nach `initialize()` gibt es keinen Transform→Body-Push —,
   also schreibt er jeden Step seine **Spawn-Pose** zurück.
2. Character-Schleife (`:537`–`:576`): schreibt **nur die Position** zurück,
   danach. Die Position überlebt deshalb, die Rotation nicht.

Was das heute schon kaputt macht: **Lua/Python können einen Standard-`PlayerCharacter`
nicht drehen.** `updateScripts` läuft vor der Physik (`:902` vs. `:907`), der
Schreibvorgang wird im selben Frame überschrieben.

Das Rig würde nach der Physik laufen und die Rotation damit jeden Frame
„selbst heilen" — sichtbar wäre nichts. Sich darauf zu verlassen, einen Stomp
zu überholen, bricht aber in dem Moment, in dem jemand die Tick-Reihenfolge
anfasst. Der Fix ist deshalb Voraussetzung, nicht Kosmetik:

> In der Rigid-Body-Rückschreibschleife Entities überspringen, die einen
> `CharacterControllerComponent` haben. Deren Transform gehört der
> Character-Schleife.

**Verwandter Befund, nicht Teil dieses Plans:** der kinematische Body bleibt an
der Spawn-Pose stehen, weil ihn nie jemand nachführt. Andere Körper kollidieren
also mit einer Geister-Kapsel am Startpunkt. Fix wäre ein Transform→
`MoveKinematic`-Push pro Step — eigene Änderung.

---

## 3. `CameraRigComponent`

Eine Komponente für beide Modi — passt zum flachen Komponentenstil der Engine
und hält die Mathematik in einem Codepfad.

```cpp
struct CameraRigComponent {
    enum class Mode      { FirstPerson, ThirdPerson };
    enum class TargetYaw { Free, Follow };

    Mode      mode        = Mode::ThirdPerson;
    HE::UUID  target;                        // verfolgte Entity (EntityIdComponent)

    glm::vec3 pivotOffset = {0.0f, 1.6f, 0.0f};   // Augen-/Schulterhöhe über Target-Origin
    glm::vec3 armOffset   = {0.4f, 0.0f, 0.0f};   // Schulter-Versatz, Kameraraum (nur TPS)
    float     armLength   = 4.0f;                 // Boom-Länge (nur TPS)

    float     yaw = 0.0f, pitch = -10.0f;    // Laufzeit-Zustand des Rigs
    float     sensitivity = 0.12f;           // °/px
    float     pitchMin = -80.0f, pitchMax = 75.0f;

    TargetYaw targetYaw = TargetYaw::Free;   // ← Rotationskopplung
    bool      collision = false;             // Spring-Arm-Kollision (siehe 5.)
    bool      hideTargetMesh = true;         // FPS: eigenes Modell ausblenden
};
```

### Mathematik

Identisch für beide Modi:

```
pivot   = targetWorldPosition + pivotOffset      // UNROTIERT, Weltachsen
rot     = quat(radians(pitch, yaw, 0))
camPos  = pivot + rot * (armOffset - forward * armLength)
```

First-Person ist `armLength == 0` (und `armOffset == 0`). Kein zweiter Zweig.

`pivotOffset` wird bewusst **nicht** mit der Ziel-Rotation gedreht: bei
aktivierter Kopplung entstünde sonst eine Rückkopplung Rig → Ziel-Yaw → Pivot →
Rig. Weltachsen halten den Pfad azyklisch.

### Rotationskopplung (`targetYaw`)

`Free` — das Rig fasst die Rotation des Ziels nie an. Die Kamera orbitiert, die
Figur dreht sich unabhängig (typisch Action-/Adventure: Figur dreht sich in
Laufrichtung). Das ist der TPS-Standard.

`Follow` — das Rig schreibt `target.rotation.y = yaw`. Die Figur schaut immer
dorthin, wo die Kamera schaut; Strafen und Rückwärtslaufen werden dadurch
möglich (Shooter-TPS, und der Normalfall für First-Person).

Defaults: `FirstPerson` → `Follow`, `ThirdPerson` → `Free`.

Drei Regeln, die dabei nicht verhandelbar sind:

* **Nur Yaw.** `rotation.x` und `rotation.z` des Ziels bleiben unangetastet —
  byte-identisch, nicht über einen Quaternion-Roundtrip neu berechnet.
  `glm::eulerAngles` liefert nahe ±90° Pitch eine andere, gleichwertige
  Darstellung; ein Roundtrip würde eine Figur bei steilem Blick still
  umklappen. Geschrieben wird genau ein Float.
* **Das Ziel braucht 2.4.** Ohne den Fix gewinnt der Stomp, sobald jemand die
  Tick-Reihenfolge ändert.
* **Kopplung allein reicht nicht.** Wer die Figur mitdreht, muss die Bewegung
  kamera-relativ machen, sonst läuft sie seitwärts weiter. Dafür bekommt die
  Registry eine Zeile `camera.getRigYaw`, die die Character-Klasse liest.

### Ziel-Referenz

Als `HE::UUID` (`EntityIdComponent`), nicht als `entt::entity` — Handles
überleben keinen Save/Load-Roundtrip, genau dafür existiert die UUID.

Fallback wenn leer: die vom `PlayerHost` besessene `PlayerCharacter`-Entity über
`EntityHost::entityOf`. Damit braucht ein Standardprojekt gar keine Zuweisung.

### Verhältnis zur Fly-Cam

Gültiges Rig mit auflösbarem Ziel → das Rig treibt die Hauptkamera.
Sonst → `FlyCameraController` wie bisher. Bestehende Projekte ändern sich nicht.

---

## 4. Umsetzung

Vier Scheiben, jede für sich commit-fähig:

1. **Rigid-Body-Rückschreiben überspringt Character-Entities** (`PhysicsWorld.cpp`).
   Eigenständiger Bugfix, unabhängig vom Rest. Test: Entity mit
   `CharacterControllerComponent` + kinematischem `RigidBodyComponent`, Rotation
   vor `step()` setzen, danach prüfen dass sie überlebt.
2. **`propagateFrom` in einen geteilten Helper** ziehen (aus `RenderExtractor.cpp`).
3. **`CameraRigController`** in `HE_Scene`, neben `FlyCameraController`, gleiches
   statisches Muster. Aufrufstellen umhängen: `GameApplication.cpp:898` und
   `EditorApplication.cpp:1965` → **nach** den jeweiligen Physik-Block.
4. **Komponente verdrahten:** Inspector-Block + Add-Menü (`InspectorPanel.cpp:905`
   als Muster), Serializer save/load (Binär ist CBOR derselben Struktur — kein
   zweiter Pfad), Registry-Zeilen in `EngineApi.cpp`.

Registry-Zeilen (je eine `t.push_back`, damit automatisch in Lua, Python,
HorizonCode und C++-Codegen):
`camera.setRigMode`, `camera.setRigTarget`, `camera.setArmLength`,
`camera.setTargetYawMode`, `camera.getRigYaw`, `camera.addYawPitch`.

Aufwand: rund 2–3 Personentage, deckt sich mit der MASTERPLAN-Schätzung für E3.

---

## 5. Bewusst nicht in v1

* **Spring-Arm-Kollision.** `PhysicsWorld::raycast` hat keine Ignore-Liste und
  keinen Layer-Filter (`PhysicsWorld.h:44`) und liefert nur den nächsten
  Treffer — ein Ray vom Pivot nach hinten trifft zuerst die Figur selbst.
  Empfehlung: einen `ignoreEntity`-Parameter mitnehmen (klein, passt in
  MASTERPLAN E2). Ohne Kollision fährt die Kamera durch jede Wand.
* **Weiches Nachdrehen** (`FollowSmoothed` + `turnRate` °/s) — sinnvolle
  Erweiterung, aber Tuning-Fläche; erst wenn die harte Kopplung steht.
* **Positions-Lag / Kamera-Glättung**, Blending zwischen Rigs, Shake, FOV-Kick.
* **Gamepad** (MASTERPLAN E4) — es gibt keinen SDL-Gamepad-Code in der Engine.
* **Shape-Casts** statt Rays für den Boom.

---

## 6. Nebenfund

`GameApplication.cpp:959` — die `camPos`, die an `SceneSystems::tick` geht, nimmt
die **erste** Kamera per `break`, ohne `isMain` zu prüfen. Bei mehreren Kameras
folgen LOD und Niederschlag einer anderen Kamera als das Bild. Eigener Fix.
