# Root Motion + Animation-Notifies

Plan für den ersten Zuschnitt aus dem Rest der Skeletal-Animation-Roadmap
(Stand 78 %: offen sind Root Motion, Layer/Bone-Masks, Blend-Trees, Notifies,
IK). Root Motion und Notifies zusammen, weil beide reine Gameplay-Animations-
schicht sind: kein Backend-Port, kein Shader, kein Renderer. Layer/Masks und
Blend-Trees fassen dagegen die Pose-Auswertung selbst an und kommen später.

Branch: `claude/skeletal-animation-root-motion`. Kein Merge nach main aus
diesem Zweig heraus.

---

## 1. Was heute steht (Kartierung)

### 1.1 Die drei Pose-Treiber und der geteilte Kern

Alles, was ein Skelett stellt, endet in `SkeletalMeshComponent::boneMatrices`
(eine `glm::mat4` pro Joint) und setzt `dirty`. Drei Systeme tun das:

| System | Komponente | Datei |
|---|---|---|
| `AnimationSystem` | `AnimatorComponent` (ein Clip) | `src/HE_Scene/src/AnimationSystem.cpp` |
| `AnimationBlendSystem` | `AnimatorBlendComponent` (zwei Clips, `blendAlpha`) | `src/HE_Scene/src/AnimationBlendSystem.cpp` |
| `AnimationStateMachineSystem` | `AnimatorStateMachineComponent` (FSM + Crossfade) | `src/HE_Scene/src/AnimationStateMachineSystem.cpp` |

Der gemeinsame Kern liegt in `src/HE_Scene/src/AnimationEval.h/.cpp`, einem
internen Header (nicht im Public-Include-Pfad):

* `advancePlayback(t, playing, speed, looping, duration, dt)` — Playhead, wrap
  bei Loop (mit Korrektur des negativen `fmod` bei Rückwärtslauf), sonst Clamp
  und `playing = false`.
* `sampleClip(clip, t, localTRS)` — schreibt pro Joint ein `JointTRS`
  (translation/rotation/scale), lineare Interpolation, Slerp bei Rotation.
* `composeBoneMatrices(mesh, localTRS, boneMatrices)` — Forward-Kinematik über
  `SkeletonJoint::parent` (Eltern vor Kindern angenommen), danach `* IBM`.
* `blendTRS(a, b, alpha, out)` — Lerp/Slerp pro Joint.

`AnimationPreview::evaluateClipPose` (public) ist die Editor-Klammer um
dasselbe, für Aufrufer ohne Entity (Skeletal-Mesh-Editor-Scrub).

**Konsequenz für diesen Umbau:** Root Motion und Notifies gehören in
`AnimationEval.h` als geteilte Helfer, die alle drei Systeme aufrufen. Genau so
ist `advancePlayback` entstanden, nachdem drei Kopien des Wrap-Codes
auseinandergelaufen waren.

### 1.2 Die Zustandsmaschine im Detail

`AnimatorStateMachineComponent` hält `resolvedGraph` (aus dem
`AnimatorStateMachineAsset` geparst, gecacht über `configDirty` /
`resolvedFromAssetId`), die Live-`params`, `currentStateName`, `clipTime` und
den laufenden Crossfade (`inTransition`, `transitionTarget`,
`transitionElapsed`, `transitionDuration`).

Wichtig für beide Features: **während eines Crossfades gibt es ZWEI Playheads.**
Der ausgehende Clip läuft auf `clipTime`, der eingehende auf
`transitionElapsed` (auf die Clip-Dauer gewrappt bzw. geclampt). Beim Abschluss
gilt `sm.clipTime = sm.transitionElapsed`, der eingehende Playhead wird also
stetig fortgesetzt. Jede Delta- oder Fenster-Rechnung braucht darum **pro
Playhead** einen „Zeitpunkt der letzten Auswertung", nicht einen pro Entity.

Der Sync-Graph (`AnimatorHost`) feuert innerhalb dieses Systems, direkt vor der
Transitions-Auswertung; `sync` ist `nullptr` außerhalb einer Play-Session.

### 1.3 Wo die Phase im Frame liegt

`SceneSystems::tickAnimation(world, cm, dt, sync)` ruft die vier Systeme in
fester Reihenfolge (Clip, Blend, FSM, Property). Der Frame in
`GameApplication.cpp` (und gleichlautend in `EditorApplication` für PIE):

```
executeSceneRequests → Debug-Linien → Streaming → updateScripts(gameDt)
  → PhysicsWorld::step (fixed, evtl. mehrfach)   ← liest CharacterVirtual-Velocity
  → CollisionSystem::dispatch                     ← EIN Drain für beide Frontends
  → updateCameraController
  → SceneSystems::tickWorld   (Terrain, Movement, Nav, Weather, Particles, LOD)
  → SceneSystems::tickAnimation
  → Extraktion (propagateTransforms, boneMatrices → Renderer)
```

`tickAnimation` läuft im Editor **ungegatet**, damit ein authorter Clip beim
Bearbeiten animiert. Nur der Sync-Graph ist auf Play beschränkt, und zwar über
den Parameter, nicht über eine Prüfung im System.

### 1.4 Wo Gameplay-Events heute langlaufen

`CollisionSystem::dispatch` ist die Vorlage: eine Drain-Stelle, die beide
Frontends bedient (Lua/Python über `ScriptContext`, HorizonCode über
`Runtime::fireOn*` + `EntityHost::instances()`), mit einem `registry().valid()`-
Test gegen tote Entities.

Der Weg für **ein** neues Entity-Event ist vollständig ablesbar an `OnHitEnd`:

| Datei | Was |
|---|---|
| `src/HE_Core/src/HorizonCode/HorizonCode.cpp:2324` | Zeile in `engineEvents()` (`{name, hook, PinType, elem}`) |
| `src/HE_Core/src/HorizonCode/HorizonCode.cpp:2346` | Eintrag in der `Entity`-Zeile von `engineClasses()` |
| `src/HE_Core/include/HorizonCode/HorizonCodeCompiled.h:202` | `virtual void onX(...)`-Hook |
| `src/HE_Core/include/HorizonCode/HorizonCodeRuntime.h:293` | `void fireOnX(...)`-Deklaration |
| `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:767` | Makro-Zeile (`HE_HC_ENTITY_EVENT` bzw. `HE_HC_VALUE_EVENT`) |
| `src/HE_Core/include/Scripting/IScriptBackend.h:62` | `virtual bool callOnX(...)` **mit Default-No-Op-Body** |
| `src/HE_Core/src/Scripting/ScriptEngine.cpp:224` | Lua-Implementierung |
| `src/HE_Python/src/PyScriptBackend.cpp:1156` | Python-Implementierung |
| `src/HE_Scene/src/ScriptContext.cpp:1495` | Weiterleitung über `HE_SCRIPT_CALL` |
| `tests/test_horizoncode_runtime.cpp:1387` | Event steht in der `Entity`-Klasse |

`HcCodegen.cpp` und der Knoten-Doku-Generator lesen `engineEvents()` generisch
— dort ist **nichts** von Hand nachzutragen. Das ist der Grund, warum ein
Event überhaupt so billig ist.

### 1.5 Assets: Clip-Format und Chunk-Verträglichkeit

`AnimationClipAsset { float duration; std::vector<AnimationChannel> channels; }`
liegt im `.hasset`-Chunk `CHUNK_ANIM` (`ContentManager.cpp:487` lesend,
`:1616` schreibend, beides `case HE::AssetType::AnimationClip`). `HAsset::Reader` liest alle Chunks generisch nach
`id → bytes` und `findChunk` liefert `nullptr` für einen fehlenden. **Ein neuer
Chunk ist damit in beide Richtungen verträglich:** eine alte Datei hat keine
Notifies, ein alter Build ignoriert den unbekannten Chunk. Nichts an
`CHUNK_ANIM` selbst anfassen.

### 1.6 Charakter und Transform

* `TransformComponent` ist **lokal** (Position, Euler-Grad, Scale) plus ein
  abgeleitetes `worldMatrix`, das ausschließlich `propagateTransforms` schreibt
  — und das läuft in `tickAnimation` **nicht**. In der Animationsphase ist also
  `HE::worldMatrixOf` / `HE::worldPositionOf` zu benutzen, nie `tc.worldMatrix`.
  (Der Fehler ist in diesem Repo viermal aufgetreten.)
* `MovementSystem` (in `tickWorld`, also **vor** `tickAnimation`) rechnet
  `MovementComponent::moveInput` in eine planare Geschwindigkeit um und ruft
  `PhysicsWorld::setCharacterVelocity(e, {planar.x, cc.velocity.y, planar.z})`.
  Das Y kommt vom Controller (Gravitation, Sprung) und wird bewusst erhalten.
* `PhysicsWorld::setCharacterVelocity` setzt **nur** `SetLinearVelocity` am
  `CharacterVirtual`. Bewegt wird erst im nächsten `step()`.

Das letzte Detail entscheidet das Root-Motion-Design (siehe 2.3).

---

## 2. Root Motion

### 2.1 Modell

Root Motion heißt: die Bewegung, die der Künstler in die Wurzel-Bone gelegt
hat, wird aus der Pose **herausgenommen** und stattdessen auf die Entity
angewandt. Ohne das Herausnehmen bewegt sich die Figur doppelt (die Pose
schiebt das Mesh, und die Entity fährt zusätzlich).

Drei Teile, in dieser Reihenfolge pro Frame und pro Playhead:

1. **Extraktion.** `delta = rootTRS(t) − rootTRS(tPrev)`, im Mesh-Raum.
2. **Root-Lock.** Die Wurzel-TRS in `localTRS` wird auf einen festen Wert
   gesetzt, bevor `composeBoneMatrices` läuft.
3. **Anwendung.** Das Delta landet auf der Entity (Transform oder Character
   Controller, siehe 2.3).

### 2.2 Entscheidungen

**Welcher Joint ist die Wurzel?**
`RootMotionComponent::rootJointName` (leer = Standard). Standard ist der erste
Joint mit `parent < 0`. glTF-Skelette können mehrere Wurzeln haben, deshalb
gibt es den Namen überhaupt; ohne Treffer wird einmal pro Entity gewarnt
(`HE_LOG_THROTTLE`) und Root Motion für die Entity ausgesetzt, nicht still
deaktiviert.

**Wie wird das Delta über einen Loop gerechnet?**
`advancePlayback` wrappt bereits, deshalb muss der Delta-Helfer die Wrap-Kante
selbst erkennen. Regel (Vorwärtslauf, `t < tPrev` heißt gewrappt):
`delta = (rootTRS(duration) − rootTRS(tPrev)) + (rootTRS(t) − rootTRS(0))`.
Rückwärts (`speed < 0`) spiegelverkehrt. Nicht-loopender Clip am Ende:
`tPrev == t`, das Delta ist null, es passiert nichts. Das ist richtig so.

**Was ist „tPrev"?**
Ein `lastSampledTime` **pro Playhead**, nicht pro Entity:
`AnimatorComponent`, `AnimatorBlendComponent` je eines; die
Zustandsmaschine **zwei** (`clipTime` und `transitionElapsed`). Beim Abschluss
des Crossfades wandert der eingehende Wert auf den ausgehenden mit, analog zu
`sm.clipTime = sm.transitionElapsed`, sonst springt das Delta im Abschlussframe.

**Crossfade und Zwei-Clip-Blend:** die beiden Deltas werden mit demselben
`alpha` gemischt, mit dem auch die Pose gemischt wird (`blendTRS`). Ein Delta
ist eine Translation und ein Yaw, das mischt sich linear bzw. per Slerp genau
wie die Pose. Alles andere führte dazu, dass die Figur im Übergang
beschleunigt oder stehen bleibt.

**Root-Lock-Varianten** (`RootMotionComponent::lock`, Unreals Namen):
* `Zero` (Standard): Wurzel-Translation auf 0, Yaw auf 0.
* `FirstFrame`: auf `rootTRS(0)` — für Clips, deren Wurzel nicht im Ursprung
  beginnt.
* `TranslationOnly`: nur die Translation wird gesperrt, die Rotation bleibt in
  der Pose (für Clips, deren Wurzel dreht, ohne dass sich die Figur drehen soll).

Gesperrt wird immer nur, was auch extrahiert wird: die Achsen aus
`extractTranslation` / `extractRotation` (siehe unten).

**Was wird extrahiert?**
`RootMotionComponent` trägt `bool extractTranslationXZ = true`,
`bool extractTranslationY = false`, `bool extractYaw = true`. Y ist per
Vorgabe aus, weil ein Character Controller seine Vertikale selbst besitzt
(Gravitation, Sprung, `cc.velocity.y`) und ein Root-Motion-Y damit sofort
streitet. Pitch/Roll werden nicht extrahiert: eine Figur, die kippt, ist keine
Root Motion, sondern eine Pose.

**Per-Clip oder per-Entity?**
Beides, und das ist kein Widerspruch:
* `AnimationClipAsset::hasRootMotion` (neu, im Notify-Chunk mitgeschrieben,
  siehe 3.2) sagt, ob **dieser Clip** Root Motion trägt. Ein Idle mit
  gesperrter Wurzel soll nicht dieselbe Behandlung bekommen wie ein Roll.
* `RootMotionComponent` sagt, ob **diese Entity** sie anwendet und wie.

Ohne Komponente passiert nichts, auch bei einem Clip mit `hasRootMotion`. Das
ist die kompatible Richtung: kein bestehendes Projekt ändert sein Verhalten.

### 2.3 Anwendung: zwei Modi, und warum kein Puffer nötig ist

`RootMotionComponent::mode`:

* **`Off`** (Standard) — nichts.
* **`Transform`** — das Delta wird auf `TransformComponent` addiert:
  Yaw auf `t.rotation.y`, die Translation **gedreht in den Weltrahmen der
  Entity und dann über `HE::localPositionForWorld` in den lokalen Rahmen
  zurückgerechnet**, `t.dirty = true`. Für Requisiten, Kamerafahrten,
  nicht-physikalische Figuren. Kein Lag: `propagateTransforms` läuft in der
  Extraktion danach.
* **`CharacterController`** — `PhysicsWorld::setCharacterVelocity(e,
  {rm.x/dt, cc.velocity.y, rm.z/dt})`, plus Yaw auf `t.rotation.y`.

Zum letzten Modus die Ordnungs-Überlegung, weil sie beim ersten Hinsehen
falsch aussieht: `setCharacterVelocity` **bewegt nichts**, es hinterlegt eine
Geschwindigkeit für den nächsten `step()`. `MovementSystem` ruft dieselbe
Funktion in `tickWorld`, also ebenfalls **nach** dem Step dieses Frames. Ein
Aufruf aus `tickAnimation` hat damit exakt dieselbe Latenz wie der aus
`MovementSystem` (ein Frame, wie die Eingabe-Bewegung heute auch) und ist der
**letzte** Schreiber vor dem Step, gewinnt also gegen die Eingabe, solange
Root Motion aktiv ist. Genau diese Semantik will man.

Daraus folgt: **kein `pendingDelta`-Puffer, keine Änderung an
`MovementSystem`.** Das Verbot in der `movement`-Doku („nichts, was eine Figur
bewegt, gehört in die Animationsphase") betrifft **Transform-Schreibvorgänge
nach dem Step**; eine Geschwindigkeit für den kommenden Step ist keiner. Der
Yaw-Schreiber ist ein Transform-Schreiber, aber der Character Controller
schreibt seine Rotation ohnehin nie zurück (`PhysicsWorld` schreibt bei
`CharacterVirtual` nur die Position), es gibt also keinen zweiten Besitzer.

**Edit-Mode-Gate.** `tickAnimation` läuft im Editor ungegatet. Root Motion, die
dort mitliefe, würde die authorte Entity durch die Szene wandern lassen, und
das würde **gespeichert**. Root Motion bekommt deshalb dasselbe Gate wie der
Sync-Graph: einen Parameter an `tickAnimation`, keine Play-Prüfung in den
Systemen.

```cpp
void tickAnimation(HorizonWorld&, ContentManager&, float dt,
                   AnimatorHost* sync = nullptr,
                   bool applyRootMotion = false,      // NEU
                   NotifyQueue* notifies = nullptr);  // NEU (Abschnitt 3)
```

Es gibt genau eine Aufrufstelle je App: `GameApplication.cpp:2636` (dort immer
`true`, ein gepackter Build hat keinen Edit-Modus) und
`EditorApplication.cpp:2869` (dort an die laufende PIE-Session gebunden, in
derselben Weise wie `m_animatorHost` es schon ist).

Der Editor bekommt zusätzlich eine Vorschau (Schritt 4): Root Motion **wird**
extrahiert und die Wurzel **wird** gesperrt, aber statt die Entity zu bewegen
wird der aufsummierte Pfad als Debug-Linie gezeichnet. Das ist die Ansicht, die
ein Künstler braucht, und sie verändert die Szene nicht.

### 2.4 Aufteilung in der Datei

Neu in `AnimationEval.h` (intern, alle drei Systeme rufen es):

```cpp
struct RootMotionDelta { glm::vec3 translation{0}; float yawDegrees = 0.0f; };

// Wurzel-Joint des Skeletts (Name leer = erster mit parent < 0). -1 wenn keiner passt.
int  findRootJoint(const SkeletalMeshAsset& mesh, const std::string& name);

// Delta zwischen zwei Playhead-Ständen, wrap- und rückwärtsfest.
RootMotionDelta extractRootMotion(const AnimationClipAsset& clip, int rootJoint,
                                  float tPrev, float t, bool looping,
                                  const RootMotionOptions& opt);

// Wurzel-TRS in localTRS neutralisieren, passend zu opt/lock.
void lockRootJoint(std::vector<JointTRS>& localTRS, int rootJoint,
                   const AnimationClipAsset& clip, const RootMotionOptions& opt);
```

`RootMotionOptions` ist der Wertebereich aus `RootMotionComponent`, ohne
ECS-Abhängigkeit, damit `AnimationPreview` dieselben Helfer benutzen kann.

Die Anwendung auf die Entity (Transform vs. Character Controller) ist **nicht**
in `AnimationEval`, weil sie `HorizonWorld` und `PhysicsWorld` braucht. Sie
bekommt ein eigenes `RootMotionSystem`-artiges freies Paar in
`AnimationEval`-Nachbarschaft oder — besser, weil es die drei Systeme ohnehin
teilen — eine `applyRootMotion(world, physics, e, delta)`-Funktion in einem
neuen internen Header `src/HE_Scene/src/RootMotionApply.h`.

> Offene Frage für Schritt 2: `tickAnimation` bekommt heute keinen
> `PhysicsWorld`. Für den `CharacterController`-Modus muss er durchgereicht
> werden (beide Apps haben ihn zur Hand). Alternative wäre, das Delta auf
> `CharacterControllerComponent::velocity` zu schreiben und `PhysicsWorld::step`
> es lesen zu lassen — das ist aber genau die Umkehrung des heutigen Vertrags
> (`velocity` ist ein Ausgabefeld). Empfehlung: `PhysicsWorld*` durchreichen.

---

## 3. Notifies

### 3.1 Modell

Ein Notify ist ein benanntes Ereignis mit Zeitstempel auf der Clip-Timeline.
Zwei Formen, wie in Unreal:

* **Notify** (`duration == 0`): feuert einmal, wenn der Playhead die Zeit
  überstreicht. Fußtritt, Trefferfenster-Start, Sound, Partikel.
* **Notify State** (`duration > 0`): feuert `Begin` beim Eintritt und `End`
  beim Austritt des Fensters. Trefferfenster, Unverwundbarkeit, Spurzeichnung.

```cpp
struct AnimationNotify
{
    std::string name;              // frei, das ist die ganze Nutzlast
    float       time     = 0.0f;   // Sekunden auf der Clip-Timeline
    float       duration = 0.0f;   // > 0 = Notify State
};
// in AnimationClipAsset:
std::vector<AnimationNotify> notifies;
bool                         hasRootMotion = false;   // siehe 2.2
```

### 3.2 Speicherung

Neuer Chunk `CHUNK_ANOT` (`makeChunkId('A','N','O','T')`) in
`src/HE_Core/include/ContentManager/HAsset.h`, gelesen und geschrieben neben
`CHUNK_ANIM` in `ContentManager.cpp`. Inhalt: `hasRootMotion` (uint8),
Notify-Anzahl (uint32), dann pro Notify `appendString(name)`,
`appendPOD(time)`, `appendPOD(duration)`.

**Nicht** in `CHUNK_ANIM` hinein: ein alter Build würde die angehängten Bytes
als weitere Kanäle zu lesen versuchen. Getrennter Chunk ist der einzige
verträgliche Weg, und `findChunk` liefert für alte Dateien sauber `nullptr`.

Zu prüfen in Schritt 3: der Pack-Zeit-Pfad, der Assets reserialisiert
(Pfad→UUID-Rewrite). Er läuft über `Reader::chunks()` und ist damit generisch,
aber das ist zu verifizieren, bevor ein Notify im gepackten Build verschwindet.

### 3.3 Feuerregel

Pro Playhead und Frame, halboffenes Intervall `(tPrev, t]`:

* **Notify** feuert, wenn `time` im Intervall liegt.
* **Notify State** feuert `Begin`, wenn `time` im Intervall liegt, und `End`,
  wenn `time + duration` darin liegt.
* **Loop-Wrap:** das Intervall zerfällt in `(tPrev, duration]` und `(0, t]`,
  beide werden geprüft. Ein Clip, der in einem Frame mehr als einmal durchläuft
  (winzige Dauer, großer `dt`), feuert jedes Notify genau einmal — das ist
  bewusst: hundert Fußtritte in einem Frame sind kein Feature.
* **Rückwärtslauf** (`speed < 0`): dasselbe Intervall, umgekehrt orientiert;
  `Begin`/`End` bleiben an ihre Zeitstempel gebunden und tauschen die
  Reihenfolge nicht. Ein rückwärts abgespielter Angriff meldet erst das
  Fenster-Ende. Alles andere wäre eine zweite Semantik für dieselben Daten.
* **Nicht-loopender Clip am Ende:** `tPrev == t`, es feuert nichts mehr. Ein
  Notify exakt auf `duration` feuert damit genau einmal, im Frame des
  Erreichens (weil das Intervall rechts geschlossen ist).
* **Crossfade:** nur der Playhead mit dem höheren Gewicht feuert
  (`alpha < 0.5` → ausgehender, sonst eingehender). Beide feuern zu lassen gibt
  während jedes Übergangs doppelte Fußtritte, und das ist die Beschwerde, die
  dieses Feature auslösen würde. Der Schwellwert steht als benannte Konstante
  im Code, nicht als Zahl im Vergleich.

### 3.4 Sammlung und Zustellung

Die Systeme kennen keinen Play-Modus, also sammeln sie nur:

```cpp
struct AnimationNotifyEvent
{
    uint32_t    entity;
    std::string name;
    enum class Kind : uint8_t { Fire, Begin, End } kind;
};
using NotifyQueue = std::vector<AnimationNotifyEvent>;
```

`tickAnimation` bekommt ein `NotifyQueue* out = nullptr`. `nullptr` heißt: gar
nicht erst auswerten. Damit wächst im Editor nichts unbegrenzt, und die Systeme
bleiben ahnungslos darüber, ob gespielt wird — dieselbe Bauart wie `sync`.

**Eine Drain-Stelle, beide Frontends**, exakt nach dem Muster von
`CollisionSystem::dispatch`: ein neues `AnimationNotifySystem::dispatch(queue,
world, scripts, instances, runtime, hcInstances, animatorHost)` in einem
Header `src/HE_Scene/include/HorizonScene/AnimationNotifySystem.h`. Es prüft
`registry().valid()` gegen tote Entities, wie dort.

**Wo gedraint wird:** unmittelbar **nach** `tickAnimation`, in beiden Apps
(`GameApplication.cpp` bei `SceneSystems::tickAnimation`,
`EditorApplication.cpp` im PIE-Zweig). Nicht an der Kollisions-Drain-Stelle
mitgenommen: die liegt im Frame **vor** `tickAnimation` und würde jedes Notify
einen Frame kosten. Ein Handler, der dort Bewegungsabsicht setzt, wird im
nächsten Frame verbraucht — nicht schlechter als heute.

**Empfänger** (in dieser Reihenfolge, damit die Regel eine ist):
1. das Lua/Python-Skript auf der Entity (`ScriptContext`),
2. die HorizonCode-`Entity`-Klasse auf der Entity (`EntityHost::instances()`),
3. der Sync-Graph derselben Entity (`AnimatorHost::instanceOf`).

Der dritte Punkt ist eine bewusste Entscheidung und keine Vollständigkeits-
Geste: ein Sync-Graph ist die Stelle, an der eine Zustandsmaschine „das
Trefferfenster ist auf" in einen Parameter übersetzen würde, und ohne ihn
müsste dieser Weg über die Entity-Klasse und zurück laufen.

### 3.5 Events und Skript-API

Ein Event, drei Ausprägungen, mit **String**-Nutzlast (dem Namen) und ohne
`elem` — die Form von `fireOnClipFinished`, nicht die von `fireOnHit`:

| HorizonCode | Lua/Python |
|---|---|
| `OnAnimationNotify` (String) | `onAnimationNotify(name)` |
| `OnAnimationNotifyBegin` (String) | `onAnimationNotifyBegin(name)` |
| `OnAnimationNotifyEnd` (String) | `onAnimationNotifyEnd(name)` |

Drei Events statt eines mit Kind-Argument, weil ein Graph sonst jedes Mal einen
Vergleich vor die Logik setzen müsste und ein Fußtritt-Handler auch
Fenster-Enden bekäme. Alle drei tragen den **Namen** und nicht mehr: das ist
die ganze Nutzlast eines Notifies, und eine Entity-Id gibt es hier nicht
(anders als bei einem Kontakt).

Die vollständige Touch-Point-Liste pro Event steht in 1.4. Für die drei Events
ist das dieselbe Liste dreimal, plus je eine Zeile in der `Entity`-Klasse von
`engineClasses()`.

**Registry-Zeilen** in `HE::api::animator` (eine Zeile bedient Lua, Python und
HorizonCode gleichzeitig, siehe `EngineApi.h`):

* `animator.notifiesOf(clipId) → String[]` — welche Namen ein Clip trägt.
  Für eine Debug-Ansicht und damit ein Graph nicht raten muss.

Mehr nicht. Notifies werden **empfangen**, nicht gerufen; eine `fireNotify`-
Zeile wäre eine zweite Quelle für dieselben Ereignisse.

---

## 4. Schnitt der Folgeschritte

### Schritt 2 — Root Motion

Dateien: `AnimationEval.h/.cpp` (Helfer), neuer
`Components/RootMotionComponent.h`, die drei Animationssysteme,
`SceneSystems.h/.cpp` (Signatur + Durchreichen von `PhysicsWorld*` und
`applyRootMotion`), `SceneSerializer` (Komponente speichern/laden),
`GameApplication.cpp` + `EditorApplication.cpp` (Aufrufstelle),
`ContentManager.cpp` + `HAsset.h` (`hasRootMotion` im neuen Chunk),
`InspectorPanel.cpp` (Komponente anlegen und bearbeiten).

Tests (`tests/test_animationsystem.cpp` oder neu
`tests/test_root_motion.cpp`), alle physikfrei außer dem letzten:

1. `findRootJoint`: leerer Name findet den ersten `parent < 0`; benannter Joint
   wird gefunden; unbekannter Name gibt -1.
2. Delta über einen einfachen Translations-Clip: halbe Dauer = halbe Strecke.
3. Delta über die Loop-Kante: Summe der beiden Teilintervalle, kein Sprung
   zurück auf null.
4. Delta bei `speed < 0` ist das negierte Vorwärts-Delta.
5. Root-Lock: `boneMatrices[root]` ist nach dem Lock identisch zum Bindpose-
   Ergebnis, obwohl der Clip die Wurzel bewegt.
6. `Transform`-Modus: Entity mit Elternteil steht nach N Frames an der
   erwarteten **Welt**-Position (das ist der Test, der die Welt/Lokal-Grenze
   festnagelt).
7. `applyRootMotion = false`: die Entity bewegt sich nicht, die Pose ist aber
   dieselbe wie mit Root Motion (Lock passiert trotzdem).
8. Crossfade: Delta bei `alpha = 0.5` ist das Mittel der beiden Clip-Deltas.
9. `CharacterController`-Modus gegen eine headless `PhysicsWorld`, wie es die
   bestehenden Physik-Tests tun: nach einem Step steht die Figur weiter vorn,
   und ihr Y ist von der Gravitation bestimmt, nicht vom Clip.

### Schritt 3 — Notifies

Dateien: `Assets.h`, `HAsset.h`, `ContentManager.cpp`, `AnimationEval.h/.cpp`,
die drei Animationssysteme, `SceneSystems.h/.cpp`, neues
`AnimationNotifySystem.h`, die Event-Kette aus 1.4 dreimal,
`EngineApi.h/.cpp` (eine Registry-Zeile), beide Apps.

Tests:

1. Chunk-Rundlauf: Clip mit drei Notifies speichern, laden, identisch.
   Und: eine Datei **ohne** `CHUNK_ANOT` lädt mit leerer Notify-Liste.
2. Feuern im Intervall `(tPrev, t]`: genau einmal, nicht null- und nicht
   zweimal bei zwei aufeinanderfolgenden Frames um den Zeitstempel herum.
3. Loop-Wrap: ein Notify bei `t = 0.05` feuert im Frame, der über die Kante
   läuft.
4. Notify State: `Begin` und `End` in getrennten Frames, in dieser Reihenfolge.
5. Nicht-loopender Clip: das Notify auf `duration` feuert genau einmal, danach
   nie wieder, egal wie viele Frames folgen.
6. Crossfade: bei `alpha = 0.2` feuert nur der ausgehende Clip.
7. `NotifyQueue* == nullptr`: nichts wird gesammelt (und nichts kostet).
8. Zustellung: Lua-Skript, HorizonCode-Entity-Klasse und Sync-Graph bekommen
   dasselbe Notify, und ein zerstörtes Entity bekommt keins.
9. `test_horizoncode_runtime.cpp`: die drei Events stehen in der
   `Entity`-Klasse.
10. Codegen-Parität: ein kompilierter Graph bekommt das Notify wie der
    interpretierte (der bestehende Parity-Harness deckt das ab, sobald das
    Event in `engineEvents()` steht).

### Schritt 4 — Editor und Doku

* **Wo die Notify-Timeline lebt:** Empfehlung ist der bestehende
  `SkeletalMeshEditorPanel`, im vorhandenen Clip-Scrub-Bereich, als Leiste
  unter dem Zeitregler. Begründung: dort wird ein Clip heute schon gegen ein
  Skelett abgespielt, und ein Notify ohne sichtbare Pose zu setzen ist raten.
  Der Clip ist zwar ein eigenes Asset, aber ein eigener Clip-Editor müsste
  Mesh-Auswahl, Kamera und Vorschau von dort abschreiben. Gespeichert wird
  natürlich in das **Clip**-Asset, nicht ins Mesh.
* **Root-Motion-Vorschau:** aufsummierter Pfad als Debug-Linie im Viewport,
  Entity bleibt stehen (siehe 2.3).
* **Handbuch:** die Deckung ist ein `ctest` (600/600 Bedienelemente). Jedes
  neue Bedienelement (Inspector-Felder der `RootMotionComponent`, die
  Timeline-Leiste, ihr Kontextmenü) braucht einen Eintrag, sonst wird CI rot.
  Zwei Gesetze aus dem bestehenden System: ein Eintrag ohne offenen Scope ist
  tot, und ein geteilter Helfer scopet sich selbst.
* **Tooltips erst am Frameende zeichnen**, sonst stirbt das Undo im
  Details-Panel.
* Website-Roadmap `Skeletal Animation` (78 %) nachziehen, plus Devlog.

---

## 5. Risiken und offene Punkte

1. **`PhysicsWorld*` in `tickAnimation`.** Die Signatur wächst um drei
   Parameter (`applyRootMotion`, `NotifyQueue*`, `PhysicsWorld*`). Das ist
   viel für eine Funktion, die heute vier hat. Falls es unhandlich wird: ein
   `AnimationTickArgs`-Struct, aber erst wenn es tatsächlich stört, nicht
   vorsorglich.
2. **Mehrere Pose-Treiber auf einer Entity.** `tickAnimation` erlaubt heute,
   dass eine Entity `AnimatorComponent` **und** eine Zustandsmaschine trägt;
   der letzte gewinnt bei den Bone-Matrizen. Bei Root Motion würde sich das
   Delta hingegen **addieren**, und das ist schlimmer als eine überschriebene
   Pose. Schritt 2 muss das entscheiden — Empfehlung: pro Entity und Frame
   höchstens ein Delta anwenden, und beim zweiten Treiber warnen.
3. **Root Motion im Netzwerk.** Die Replikationsschicht existiert; eine
   root-motion-getriebene Figur bewegt sich clientseitig aus der Animation
   heraus. Das ist bewusst außerhalb dieses Zuschnitts, gehört aber notiert,
   bevor jemand es für gelöst hält.
4. **`hasRootMotion` wird beim Import nicht gesetzt.** Der glTF-Importer weiß
   nicht, ob ein Clip Root Motion trägt. Vorschlag: beim Import setzen, wenn
   der Wurzel-Joint einen Translations-Kanal mit mehr als einem verschiedenen
   Wert hat, und im Editor überschreibbar lassen. Das ist eine Heuristik und
   sie muss als solche im Inspector stehen.
5. **Notify-Namen sind Strings ohne Registry.** Ein Tippfehler feuert nichts
   und meldet nichts. Ein späterer Schritt könnte die im Projekt vergebenen
   Namen sammeln und im Timeline-Editor als Vorschläge anbieten; für jetzt ist
   `animator.notifiesOf` die Debug-Antwort.
