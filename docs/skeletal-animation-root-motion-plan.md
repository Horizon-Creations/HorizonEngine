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

1. **Extraktion.** Das Delta ist ein **relativer Transform**, keine
   Subtraktion:
   `Δ = inverse(rootTransform(tPrev)) * rootTransform(t)`,
   also `Δ.translation = inverse(q(tPrev)) * (p(t) − p(tPrev))` und
   `Δ.yaw = yaw(q(t) * inverse(q(tPrev)))`.
   Eine Subtraktion wäre nur richtig, solange sich die Wurzel nicht dreht. Sobald
   `extractYaw` an ist und der Clip eine Kurve läuft (jeder Turn, jede Rolle),
   dreht die Entity mit dem extrahierten Yaw mit, und eine im Mesh-Raum
   subtrahierte Translation läge dann im falschen Rahmen: die Figur driftet
   seitwärts aus der Kurve heraus. Unreal rechnet an derselben Stelle
   `GetRelativeTransform`.
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
Der Helfer bekommt die **ungewrappte** Spanne, nicht zwei gewrappte Stände:
`tPrev` und `tPrev + dt * speed`, gerechnet **bevor** `advancePlayback` (bzw.
das inline-`fmod` der Zustandsmaschine) wrappt. Der Helfer zerlegt selbst in
Runden und komponiert die Teil-Deltas:
`Δ = Δ(tPrev → duration) * Δ(0 → duration)^n * Δ(0 → t)`.

Das ist der wichtige Teil der Entscheidung. Aus zwei gewrappten Ständen lässt
sich weder die Laufrichtung noch die Rundenzahl zurückgewinnen: `t < tPrev`
kann „vorwärts über die Kante" oder „rückwärts" heißen, und ein Clip, der in
einem Frame anderthalb Runden läuft, sieht aus wie ein halber Frame. Mit der
ungewrappten Spanne sind Rückwärtslauf und Mehrfachrunden gratis, und der
Helfer braucht kein `looping`-Flag, um zu raten.

Nicht-loopender Clip am Ende: `advancePlayback` klemmt, die Spanne ist leer,
das Delta ist die Identität. Für den Charakter-Modus reicht das aber **nicht**,
siehe die Dauerschreib-Regel in 2.3.

**Was ist „tPrev"?**
Ein `lastSampledTime` **pro Playhead**, nicht pro Entity:
`AnimatorComponent`, `AnimatorBlendComponent` je eines; die
Zustandsmaschine **zwei** (`clipTime` und `transitionElapsed`). Beim Abschluss
des Crossfades wandert der eingehende Wert auf den ausgehenden mit, analog zu
`sm.clipTime = sm.transitionElapsed`, sonst springt das Delta im Abschlussframe.

Drei Regeln dazu, die sonst je einen eigenen Fehler ergeben:

* `lastSampledTime` wird **jeden Frame für jeden laufenden Playhead**
  fortgeschrieben, unabhängig davon, ob dieser Playhead gerade etwas feuert
  oder anwendet. Sonst sammelt der gerade nicht gewichtete Playhead eine
  Spanne an und schüttet sie in dem Frame aus, in dem er das Gewicht bekommt.
* Beim **Start** einer Transition wird der eingehende Playhead auf 0 gesetzt,
  und sein `lastSampledTime` mit ihm.
* Beim **Eintritt** eines Playheads (erste Auswertung, neu angelegte
  Komponente) gilt `lastSampledTime = -ε`, damit ein Notify bei `time == 0`
  nicht verlorengeht (siehe 3.3).

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

**Dauerschreib-Regel, und warum sie kein Detail ist.**
`setCharacterVelocity` **hält** die Geschwindigkeit in Jolt, bis sie jemand
überschreibt. `MovementSystem` überschreibt sie nur für Entities mit
`MovementComponent` — ein NPC mit bloßem Character Controller hat keine. Und
`AnimationSystem` überspringt eine Entity mit `!playing`, also hört Root Motion
in dem Moment auf zu schreiben, in dem ein nicht-loopender Clip klemmt. Beides
zusammen ergibt eine Figur, die für immer weitergleitet.

Regel: solange auf einer Entity im `CharacterController`-Modus ein
root-motion-tragender Clip aktiv ist, wird **jeden Frame** geschrieben, auch
bei Delta null. In dem Frame, in dem der Clip endet (oder Root Motion abgewählt
wird), wird die Horizontale einmal auf 0 geschrieben. Das Y bleibt in beiden
Fällen `cc.velocity.y`.

**Wer besitzt den Yaw?** `MovementSystem` schreibt `t.rotation.y` bereits an
zwei Stellen (`lookYaw` und `orientToMovement`). Root Motion mit `extractYaw`
ist ein dritter Schreiber. Entscheidung: Root Motion gewinnt, solange sie
aktiv ist, und `orientToMovement` wird für diese Entity ausgesetzt, mit einer
gedrosselten Warnung, wenn beides angeschaltet ist. Zwei Besitzer eines Yaw
sind ein Zittern, das hinterher niemand lokalisiert.

Daraus folgt: **kein `pendingDelta`-Puffer und keine Umbauten an
`MovementSystem`** (bis auf die eine Yaw-Ausnahme oben). Das Verbot in der `movement`-Doku („nichts, was eine Figur
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

// Delta über eine UNGEWRAPPTE Spanne (tPrev, tEnd]. Der Helfer zerlegt Runden
// und Richtung selbst — deshalb kein `looping`-Flag und kein Vorzeichen-Raten.
RootMotionDelta extractRootMotion(const AnimationClipAsset& clip, int rootJoint,
                                  float tPrev, float tEnd,
                                  const RootMotionOptions& opt);

// Dieselbe Spanne, dieselbe Zerlegung, andere Ausbeute.
void collectNotifies(const AnimationClipAsset& clip, uint32_t entity,
                     float tPrev, float tEnd, NotifyQueue& out);

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

Pro Playhead und Frame, über **dieselbe ungewrappte Spanne**, mit der auch das
Root-Motion-Delta gerechnet wird (2.2): `(tPrev, tPrev + dt * speed]`,
halboffen links, geschlossen rechts.

* **Notify** feuert, wenn `time` in der Spanne liegt.
* **Notify State** feuert `Begin`, wenn `time` darin liegt, und `End`, wenn
  `time + duration` darin liegt.
* **Loop-Wrap:** der Helfer zerlegt die Spanne in Runden, genau wie beim
  Delta. Ein Clip, der in einem Frame anderthalb Runden läuft, feuert die
  Notifies der vollen Runde einmal und die der halben einmal. Kein Rundenzähler
  wird geraten, weil die Spanne ungewrappt hereinkommt.
* **Die Naht bei 0 und `duration`.** In einem Loop sind das derselbe Augenblick,
  und ein Notify dort darf pro Runde genau einmal feuern. Konvention: die
  rechte Kante gewinnt, also gehört `duration` zur ablaufenden Runde und `0`
  zur neuen. Da die Spanne links offen ist, feuert ein Notify bei `time == 0`
  in der ersten Runde sonst **nie** — deshalb startet ein Playhead mit
  `lastSampledTime = -ε` (2.2). Ein Fußtritt auf Frame 0 ist der normale
  Autorenfall, nicht die Ausnahme.
* **Rückwärtslauf** (`speed < 0`): dieselbe Spanne, negativ orientiert.
  `Begin`/`End` bleiben an ihre Zeitstempel gebunden und tauschen die
  Reihenfolge nicht. Ein rückwärts abgespielter Angriff meldet erst das
  Fenster-Ende. Alles andere wäre eine zweite Semantik für dieselben Daten.
* **Nicht-loopender Clip am Ende:** die Spanne ist leer, es feuert nichts mehr.
  Ein Notify exakt auf `duration` feuert damit genau einmal, im Frame des
  Erreichens (weil die Spanne rechts geschlossen ist).
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
4. Delta bei `speed < 0` ist das inverse Vorwärts-Delta.
4b. **Kurve:** Clip, dessen Wurzel 90° dreht und dabei vorwärts läuft. Nach
   voller Abspieldauer steht die Entity dort, wo der Künstler sie hingelegt
   hat. Das ist der Test, der die Subtraktion vom relativen Transform
   unterscheidet, und ohne ihn fällt der Fehler erst im Spiel auf.
5. Root-Lock: `boneMatrices[root]` ist nach dem Lock identisch zu
   `composeBoneMatrices` derselben, von Hand gesperrten TRS. **Nicht** gegen
   „Bindpose" prüfen: die Bind-Lokale der Wurzel ist bei Blender-Exporten
   (−90° X) keine Identität.
6. `Transform`-Modus: Entity mit Elternteil steht nach N Frames an der
   erwarteten **Welt**-Position (das ist der Test, der die Welt/Lokal-Grenze
   festnagelt).
7. `applyRootMotion = false`: die Entity bewegt sich nicht, die Pose ist aber
   dieselbe wie mit Root Motion (Lock passiert trotzdem).
8. Crossfade: Delta bei `alpha = 0.5` ist das Mittel der beiden Clip-Deltas.
9. `CharacterController`-Modus gegen eine headless `PhysicsWorld`, wie es die
   bestehenden Physik-Tests tun: nach einem Step steht die Figur weiter vorn,
   und ihr Y ist von der Gravitation bestimmt, nicht vom Clip.
9b. **Kein Dauergleiten:** nicht-loopender Clip auf einer Entity **ohne**
   `MovementComponent`. Nachdem der Clip geklemmt hat, ist die horizontale
   Geschwindigkeit des Character Controllers 0 und die Figur steht nach
   weiteren Steps still. Ohne die Dauerschreib-Regel gleitet sie für immer.

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
3b. Ein Notify bei `time == 0` feuert im **ersten** Frame genau einmal, und
   danach einmal pro Runde. Das ist der Test für `lastSampledTime = -ε` und
   für die Nahtkonvention.
3c. Ein Frame, der anderthalb Runden überstreicht, feuert jedes Notify der
   vollen Runde einmal und die der halben einmal.
4. Notify State: `Begin` und `End` in getrennten Frames, in dieser Reihenfolge.
5. Nicht-loopender Clip: das Notify auf `duration` feuert genau einmal, danach
   nie wieder, egal wie viele Frames folgen.
6. Crossfade: bei `alpha = 0.2` feuert nur der ausgehende Clip. Und: nach dem
   Gewichtswechsel bei 0.5 schüttet der eingehende Playhead **keinen** Rückstau
   aus — das ist der Test für „`lastSampledTime` läuft für beide Playheads
   jeden Frame mit".
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

## 4b. In Schritt 2 getroffene Abweichungen

Schritt 2 (Root Motion) ist gebaut. Vier Stellen weichen bewusst von dem ab,
was oben steht; sie stehen hier, damit Schritt 3 und 4 sie nicht zurückdrehen.

1. **Kein `lastSampledTime` pro Playhead.** Die Spanne wird inline gerechnet:
   `tPrev` ist der Playhead vor dem Vorrücken, `tEnd = tPrev + dt * speed`.
   Für jeden in 2.2 aufgezählten Fall (frisch, geladene Szene, Transitionsstart,
   Transitionsabschluss) kommt dieselbe Spanne heraus — und in dem einen Fall,
   in dem sich beide unterscheiden, ist der gespeicherte Stand der falsche: ein
   Skript, das `playbackTime` setzt, würde ein Delta über die alte Clipstelle
   erzeugen, während die Pose schon von der neuen kommt. Die Figur teleportierte
   mit dem Playhead.
   **Für Schritt 3 heißt das:** `lastSampledTime` wird dort gebraucht, aber nur
   für die `-ε`-Regel (ein Notify auf `time == 0`), und ist dort neu anzulegen.
   Wer ihn anlegt, braucht zusätzlich ein `primed`-Flag, sonst schüttet eine
   geladene Szene im ersten Frame die Spanne von 0 bis zum gespeicherten
   Playhead aus.

2. **Die Verschiebung wird gegen `q(0)` gerechnet, nicht gegen `q(tPrev)`.**
   Die Formel in 2.1 (`inverse(q(tPrev)) * Δp`) legt die Verschiebung in den
   vollen lokalen Rahmen der Wurzel. Ein Blender-Export trägt konstante −90° X
   auf der Wurzel — genau die Neigung, vor der 2.2 beim Lock-Test warnt — und
   damit wird aus einem waagerechten Schritt ein senkrechter: die Figur
   klettert. Gerechnet wird darum
   `heading(t) = yaw(q(t) · q(0)⁻¹)`,
   `Δ.translation = R_y(−heading(tPrev)) · (p(t) − p(tPrev))`,
   `Δ.yaw = yaw(q(t) · q(tPrev)⁻¹)`.
   Für `q(0) = I` ist das die Formel aus 2.1. Der Lock ist dieselbe Rechnung
   rückwärts (`R_y(−heading(t)) · q(t)`), also „die Drehung raus, die Neigung
   drin" — deshalb ist `Zero` für die Rotation dasselbe wie `FirstFrame`, sie
   unterscheiden sich nur in der Translation. Test 4b liegt zweimal vor, mit und
   ohne Neigung.

3. **`AnimationClipAsset::hasRootMotion` steht auf `true`.** Nichts setzt das
   Feld heute (der glTF-Import weiß es nicht, siehe Risiko 4), ein
   `false`-Standard hieße also: Root Motion feuert für keinen einzigen Clip, der
   existiert. Wer die Import-Heuristik baut, kann den Standard mitdrehen.

4. **`applyRootMotion` + `PhysicsWorld*` sind ein `HE::RootMotionContext*`**
   statt zweier Parameter — `nullptr` heißt „extrahieren und sperren, aber nichts
   bewegen". Damit wächst `tickAnimation` um einen Parameter statt um zwei, und
   die Bauart ist die von `AnimatorHost* sync` daneben. Risiko 1 ist damit
   entschärft, aber nicht erledigt: `NotifyQueue*` kommt in Schritt 3 dazu.

Zu Risiko 2 (mehrere Pose-Treiber): entschieden wie empfohlen. Ein
`appliedThisFrame`-Flag auf der Komponente, von `rootMotionBeginFrame` gelöscht;
der zweite Treiber warnt gedrosselt und wendet nichts an.

---

## 4c. In Schritt 3 getroffene Abweichungen

Schritt 3 (Notifies) ist gebaut. Fünf Stellen weichen bewusst von Abschnitt 3 ab.

1. **Welche Kanten offen sind, ist eine Regel und nicht drei.** Abschnitt 3.3
   sagt „links offen, rechts geschlossen" und leitet daraus die Nahtkonvention
   ab. Wörtlich gebaut fällt Test 3b hinten runter: das Folgesegment nach einem
   Wrap wäre `(0, e]`, und ein Notify auf `time == 0` feuerte im **ersten** Frame
   (dank der Ursprungsregel unten) und **danach nie wieder**. Gebaut ist deshalb:
   **die einzige offene Kante ist der Ursprung.** Das Ziel ist zu, und jede Naht,
   die der Playhead überquert, ist zu — `duration` beim Verlassen einer Runde,
   `0` beim Betreten der nächsten. Vorwärts also `(a, D]`, `[0, D]`…, `[0, e]`,
   rückwärts spiegelbildlich. Damit feuert ein Notify auf der Naht genau einmal
   pro überquerter Naht, und ein Frame über zwei Runden feuert alles zweimal.

2. **`includeStart` statt `lastSampledTime = -ε`.** Schritt 2 hat
   `lastSampledTime` gestrichen (4b.1) und rechnet die Spanne inline; damit gibt
   es auch keinen Rückstau mehr, für den ein `primed`-Flag nötig wäre. Übrig
   bleibt nur der Zweck, für den das ε gedacht war: der erste Frame eines
   Playheads beginnt genau dort, wo er steht, und ein Notify auf dieser Stelle
   läge außerhalb der links offenen Spanne. Das ist jetzt ein `bool
   includeStart`, der die Ursprungskante für diesen einen Frame schließt. Ein
   Epsilon wäre still von `std::clamp(tPrev, 0, D)` im Walk aufgefressen worden,
   also von genau der Zeile, die für nicht-loopende Clips nötig ist.
   Das Flag pro Playhead heißt `notifiesPrimed` (Blend zwei, Zustandsmaschine
   zwei) und ist Laufzeit-Zustand, der **nicht** gespeichert wird.

3. **Notify States enden spätestens mit dem Clip.** `time + duration` wird auf
   `clip.duration` geklemmt. In die nächste Runde zu wrappen gäbe einem
   nicht-loopenden Clip ein `End` vor seinem `Begin`. Fallen beide Kanten in
   dieselbe Spanne, kommt `Begin` vor `End` (rückwärts gespiegelt).

4. **`animator.notifiesOf` nimmt einen Clip-PFAD, keine `clipId`.** Es gibt
   keinen UUID-Pin-Typ, in dem eine Id reisen könnte; ein Asset wird in dieser
   API über seinen Pfad erreicht (`c.content->loadAsset`, wie die Audio-Gruppe).
   Die Antwort ist die Autorenreihenfolge inklusive Doppelungen.

5. **Kein neues Codegen-Fixture.** Test 10 hängt an `fix/engine_events` statt an
   einer eigenen Klasse: dort fehlte genau die Form, die die drei Events haben —
   ein String-Argument auf einem Event **ohne** `elem`. Der Hook, den `HcCodegen`
   emittiert, wird aus `engineEvents()` abgeleitet, ein falsches `elem` gäbe also
   eine Methode, die nichts überschreibt und nie gerufen wird.

Geprüft und in Ordnung, was 3.2 offen ließ: der Pack-Pfad reserialisiert
AnimationClips **gar nicht** (`HpakWriter.cpp:285` gibt den Blob unverändert
zurück, nur Mesh/Material/Scene/Particles werden umgeschrieben), und
`AssetRefRetarget::retargetBlob` kopiert jeden Chunk generisch durch. Ein Notify
überlebt beides.

---

## 4d. In Schritt 4 getroffene Abweichungen

Schritt 4 (Editor und Doku) ist gebaut. Was der Editor jetzt kann, und die sechs
Stellen, an denen es anders kam als in „Schritt 4 — Editor und Doku" geplant.

### Was steht

* **Notify-Zeitleiste** im `SkeletalMeshEditorPanel`, unter dem Scrub-Regler, so
  wie empfohlen. Ein Lineal auf der 1-2-5-Leiter, eine Spur mit Rauten
  (Notifies) und Balken (Notify States), der Playhead darüber. Doppelklick auf
  leere Spur legt eines an, Ziehen verschiebt, Rechtsklick öffnet das Menü
  (Add Notify / Add Notify State / Delete), Klick auf leere Spur setzt den
  Playhead. Darunter die Zeilen des ausgewählten Ereignisses (Name, Time,
  Duration) mit einem Hinweissatz, der sagt, welche der beiden Formen es gerade
  ist. Die Arithmetik ist `HE::Ed::UITimelineView` aus `UITimelineMath.h`, bei
  Zoom 1 — der Clip füllt die Spur, es gibt nichts zu scrollen und keine
  Zoom-Bedienelemente zu erklären.
* **`hasRootMotion` pro Clip** als Kästchen „Root Motion" im selben Bereich.
  `Assets.h` kündigte den Schalter an („der per-clip editor toggle ist noch zu
  bauen"); das war Teil dieses Schritts und ist es jetzt.
* **Root-Motion-Vorschau in zwei Hälften.** Im Panel ein Aufsicht-Plot in der
  Ecke der Vorschau (Pfad, Start- und Endpunkt, eine Marke auf der Stelle des
  Playheads, die zurückgelegten Meter als Text). Im Viewport eine Linie unter
  der **ausgewählten** Entity mit `RootMotionComponent`, aus deren Weltposition
  und Blickrichtung gezeichnet. Beide bewegen nichts.
* **Speichern.** `AnimationClipAsset` wird über das neue
  `ContentManager::getAnimationClipMutable` direkt bearbeitet und mit
  `saveAsset` geschrieben; `CHUNK_ANOT` konnte beides schon seit Schritt 3.
* **Handbuch**: elf neue Einträge, `editor_help_audit --check` wieder bei null
  offenen Bedienelementen (728/728), `ctest` 124/124.

### Die Abweichungen

1. **Der Tab heißt nach dem Mesh, der Dirty-Eintrag nach dem Clip.** Der
   naheliegende Weg wäre gewesen, `EditorUI::tabHasUnsavedEdits(tabPfad)` für
   den Mesh-Pfad true sagen zu lassen. Das wäre eine Lüge in der
   Beenden-Abfrage: sie listet Pfade, und der Pfad, an dem sich etwas geändert
   hat, ist der des Clips. `SkeletalMeshEditorPanel::isDirty/appendDirtyPaths/
   save` antworten deshalb auf **Clip**-Pfade. Nebeneffekt und Absicht zugleich:
   zwei Tabs, die denselben Clip scrubben, teilen sich einen Eintrag statt einen
   pro Tab zu führen. Der Preis ist, dass der Tab-Titel keinen Dirty-Punkt trägt
   — dafür sitzt der Speichern-Knopf mit Zustand auf der Werkzeugleiste.
   `forget()` löscht seitdem beides: Tab-Zustand unter einem Mesh-Pfad, offene
   Bearbeitungen unter einem Clip-Pfad.

2. **Der geladene Clip IST der Bearbeitungspuffer.** Keine Kopie im Panel, wie
   der Zustandsmaschinen-Editor sie führt. Ein Notify, das auf der Spur
   verschoben wird, ist damit sofort das, wogegen die Animatoren feuern — was in
   einem Editor mit laufender Play-Session der ganze Punkt ist. „Dirty" ist
   folglich eine Notiz über eine **Datei**, kein zweiter Datenstand. Deshalb
   braucht die Namenszeile auch **keinen Scratch-Buffer**: die bekannte Falle
   (`IsItemDeactivatedAfterEdit` sieht nie den alten Wert) betrifft Undo-Schritte,
   und hier gibt es keinen zu verlieren.

3. **Das Kontextmenü wird von Hand geöffnet.** `BeginPopupContextItem` müsste
   direkt hinter der Spur stehen, weil es sich auf das letzte Item bezieht. Das
   Menü macht aber einen `Help::Scope("Notify Timeline")` auf, und das
   Deckungs-Audit liest die Datei von oben nach unten: ein Scope über den Zeilen
   Name/Time/Duration hätte deren Einträge beansprucht und drei Bedienelemente
   als offen gemeldet, obwohl sie zur Laufzeit unter `Mesh Viewer/` aufgelöst
   werden. Gebaut ist deshalb `IsItemClicked(Right)` → `OpenPopup`, und das
   `BeginPopup` steht ganz unten. Zweite Ordnungsfrage im selben Zug: der
   Rechtsklick wird **vor** dem Linksklick behandelt (`else if`), sonst scrubbt
   das Öffnen des Menüs den Playhead unter sich weg.

4. **Die Viewport-Linie ist ein berechneter Pfad, keine aufgezeichnete Spur.**
   Der Plan sagt „aufsummierter Pfad als Debug-Linie". Aufsummiert aus
   `lastDelta` wäre eine Spur, die erst entsteht, während man zusieht, pro Entity
   Zustand braucht und gedeckelt werden müsste (Abschnitt 3.4: im Editor wächst
   nichts unbegrenzt). Gezeichnet wird stattdessen `rootMotionPath` über den
   ganzen Clip: sofort da, ohne Zustand, ohne Deckel, und es beantwortet die
   Frage des Künstlers („wohin bringt mich dieser Clip") vollständig statt bis
   zum aktuellen Frame. Neu berechnet wird sie jedes Bild für **eine**
   ausgewählte Entity — ein Cache müsste merken, dass nebenan gerade der Clip
   umgeschrieben wird, und genau dann sieht jemand hin.

5. **Der Plot im Panel ist eine Aufsicht, keine Linie in der 3D-Vorschau.**
   `RenderSkeletalPreview` rendert im Backend; das Panel hat dessen Projektion
   nicht und könnte nichts in dasselbe Bild zeichnen. Von oben ist ohnehin die
   Ansicht, die „wie weit und wohin" beantwortet.

6. **Die Vorschau-Pose ist gesperrt.** `evaluateClipPose` bleibt, wie sie war;
   daneben steht `evaluateClipPoseLocked`, die den Wurzel-Joint nach denselben
   Optionen neutralisiert wie die drei Pose-Treiber, und das Panel benutzt sie.
   Ohne den Lock rutschte das Mesh von den eigenen Füßen weg, während der Pfad
   daneben behauptet, die Bewegung sei herausgenommen worden. Der Lock hängt am
   Clip-Schalter: „Root Motion" aus, und der Clip animiert wieder so, wie er
   exportiert wurde.

Was **nicht** gemacht wurde und offen bleibt: die Website-Roadmap („Skeletal
Animation", 78 %) und der Devlog. Beides beschreibt, was auf `main` steht, und
dieser Zweig ist nicht zusammengeführt; außerdem ist ein Deploy eine
Veröffentlichung und braucht eine ausdrückliche Zusage. Gehört in denselben
Handgriff wie der Merge.

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
