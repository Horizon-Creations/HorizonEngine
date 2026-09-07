# Animation-Layer + Bone-Masks, Blend Trees, IK

Plan für den zweiten Zuschnitt aus der Skeletal-Animation-Roadmap. Root Motion
und Notifies stehen auf main (`docs/skeletal-animation-root-motion-plan.md`);
offen sind laut Roadmap-Beschreibung noch drei Dinge, und die drei gehören
zusammen, weil alle drei dieselbe Stelle anfassen: die Pose-Auswertung selbst.

* **Layer + Bone-Masks + additives Blenden** — mehrere Posen übereinander, jede
  nur auf einem Teil des Skeletts.
* **Blend Trees (1D/2D)** — ein Zustand, der aus N Clips nach Parametern mischt,
  statt aus zwei nach einem festen Alpha.
* **IK** — Zwei-Knochen-Löser für Fussplatzierung, Look-At für Kopf/Wirbelsäule.

Branch: `claude/skeletal-animation-layers-blendtrees-ik`. Kein Merge nach main
aus diesem Zweig heraus.

> **Der Zweig ist 26 Commits hinter main, und keine einzige der hier zitierten
> Dateien existiert auf seinem aktuellen Stand.** Root Motion und Notifies sind
> nach dem Abzweig gelandet. Dieser Plan ist gegen **`origin/main @ 9ccb235a`**
> geschrieben und gegen nichts anderes. **Schritt 2 beginnt mit
> `git merge origin/main` in den Feature-Branch** (oder mit einem Rebase durch
> den Chefchen). Wer das überspringt, öffnet `AnimationStateMachineSystem.cpp`
> und findet dort weder Notifies noch Root Motion und schreibt den Rest des
> Plans gegen eine Datei, die es so nicht mehr gibt.

---

## 1. Was heute steht (Kartierung)

### 1.1 Drei Pose-Treiber, ein geteilter Kern

Drei Komponenten posen ein Skelett, jede mit ihrem eigenen System:

| Komponente | System | Was sie kann |
|---|---|---|
| `AnimatorComponent` | `AnimationSystem` | ein Clip, ein Playhead |
| `AnimatorBlendComponent` | `AnimationBlendSystem` | **zwei** Clips, ein `blendAlpha` |
| `AnimatorStateMachineComponent` | `AnimationStateMachineSystem` | Zustände + Übergänge, Crossfade über zwei Playheads |

Der geteilte Kern liegt in `src/HE_Scene/src/AnimationEval.h/.cpp` (interner
Header, nicht im öffentlichen Include-Pfad):

* `struct JointTRS` — Translation, Rotation (quat), Scale, pro Joint, lokal.
* `advancePlayback` — Playhead vorrücken, wrappen oder klemmen.
* `sampleJoint` / `sampleClip` — Keyframe-Interpolation in `JointTRS`.
* `blendTRS(a, b, alpha, out)` — **zweiwegig**, `mix`/`slerp`/`mix`.
* `composeBoneMatrices(mesh, localTRS, out)` — Vorwärtskinematik plus IBM.
* `lockRootJoint` — Root neutralisieren, nachdem die Motion heraus ist.

Dazu `RootMotionApply.h` (intern, kennt `HorizonWorld` und `PhysicsWorld`) und
`HorizonScene/RootMotion.h` (öffentlich, weil Preview und Tests dieselbe
Arithmetik brauchen).

### 1.2 Die Reihenfolge pro Entity, wie sie heute ist

Für alle drei Treiber gleich, hier am Zustandsautomaten
(`AnimationStateMachineSystem.cpp:228-343`):

```
1. sampleClip(ausgehender Clip)                  -> trsOut
2. rootMotionSampleClip(...)                     -> deltaOut, lockt trsOut
3. notifyCollectClip(ausgehender Playhead)
4. (im Crossfade) dasselbe für den eingehenden    -> trsIn, deltaIn
5. rootMotionApply(blendRootMotion(dOut,dIn,a))   <- die Entity bewegt sich HIER
6. blendTRS(trsOut, trsIn, alpha)                 -> final_trs
7. composeBoneMatrices(mesh, final_trs, smc.boneMatrices)
```

Zwei Dinge daran sind für diesen Plan entscheidend:

* **`rootMotionApply` steht VOR `composeBoneMatrices`, in allen drei Systemen.**
  Zwischen Schritt 5 und Schritt 7 ist also bereits eine Lücke, in der die Pose
  final vorliegt und die Entity ihre neue Position schon hat. Genau dort hängen
  Layer und IK ein, und genau deshalb ist der Einhängepunkt billig: kein System
  muss umgestellt werden, nur die eine Zeile `composeBoneMatrices(...)` wird in
  allen drei Treibern durch denselben Aufruf ersetzt.
* Die Reihenfolge der drei Systeme in `SceneSystems::tickAnimation` ist
  Clip → Blend → Zustandsautomat → Property, und der Kommentar dort sagt
  ausdrücklich: *„der letzte gewinnt auf einer Entity, die mehr als einen
  trägt (was heute nichts verhindert)"*. Das ist eine Warze, die dieser Plan
  nicht behebt, aber berücksichtigen muss (siehe 3.5).

### 1.3 Was der Kern heute NICHT kann

Vier Lücken, jede davon eine Voraussetzung für einen der drei Teile:

1. **`composeBoneMatrices` wirft die Modellraum-Matrizen weg.** Es baut intern
   `worldMats[]` (FK ohne IBM) und multipliziert direkt mit der Inverse-Bind-
   Matrix. IK braucht genau diese Zwischenstufe: ein Zwei-Knochen-Löser rechnet
   in Modellraum-Positionen von Hüfte, Knie und Fuss.
2. **`blendTRS` ist zweiwegig und schneidet ab.** `out.resize(min(a.size(),
   b.size()))` — ein Blend gegen einen kürzeren Vektor verliert stillschweigend
   die hinteren Joints. Für N-Wege-Blend-Spaces und für Layer reicht das nicht.
3. **Es gibt kein Konzept von „additiv".** Nirgends im Baum.
4. **Es gibt keine Maske.** Ein Blend gilt immer für das ganze Skelett.

### 1.4 Assets und Serialisierung

* `SkeletonJoint { name, parent, inverseBindMatrix }`, `SkeletalMeshAsset::
  skeleton` als flacher Vektor, **Eltern vor Kindern vorausgesetzt** (`compose
  BoneMatrices` liest `worldMats[parent]` bevor es `worldMats[i]` schreibt).
* `AnimationClipAsset { duration, channels, notifies, hasRootMotion }`.
* `AnimatorStateMachineAsset { graphJson, syncGraphJson }`, geparst nach
  `HE::AnimatorStateMachineGraph` (HE_Core, mit `HE_API`) — **weil
  ContentManager in HE_Core liegt und HE_Core nicht von HE_Scene abhängen darf.**
  Jeder neue Asset-Typ dieses Plans erbt diese Randbedingung.
* Chunk-IDs in `HAsset.h`: `CHUNK_SKEL`, `CHUNK_ASSY`, `CHUNK_ANOT`. Neue
  Chunks sind rückwärtsverträglich, weil ältere Dateien sie schlicht nicht
  haben und der Reader dann den Default nimmt.
* Komponenten werden in `SceneSerializer.cpp` von Hand geschrieben und gelesen
  (`AnimatorBlendComponent` ab :555/:1258, `RootMotionComponent` ab :567/:1271).
* `SceneSystems::collectAssetRefs` sammelt jede Asset-UUID, die eine Komponente
  hält. **Was dort fehlt, packt der Packer nicht ein** — das ist keine Warnung,
  sondern ein Arbeitsschritt in jedem der drei Folgeschritte.
* Der Editor-Hilfe-Audit läuft als ctest (`editor_help_audit`,
  `tests/CMakeLists.txt:508`). Jedes neue Bedienelement im Inspector braucht
  seinen Eintrag **im selben Schritt**, sonst ist die CI dieses Schritts rot.
  Deshalb gibt es hier keinen abschliessenden „Editor + Doku"-Schritt.

### 1.5 Die Vorschau hat keine ECS

`AnimationPreview` (öffentlich) ist der Weg des Editors in denselben Kern ohne
Entity: `evaluateClipPose`, `evaluateClipPoseLocked`, `rootMotionPath`. Jede
Erweiterung des Kerns, die der Editor zeigen soll, braucht hier ihr Gegenstück.

---

## 2. Die vier Fragen vom Brett, beantwortet

### 2.1 Wo hängen Layer/additive Blends ein — vor oder nach dem State-Machine-Blend?

**Danach.** Der Zustandsautomat (bzw. der Clip- oder Zwei-Clip-Treiber) erzeugt
die **Basispose**; der Layer-Stapel liegt darüber.

Grund: ein Layer ist per Definition etwas, das auf ein fertiges Ergebnis
aufgetragen wird — eine Nachlade-Animation auf dem Oberkörper gilt gegen
*das, was die Beine gerade tun*, egal ob das ein Idle, ein Lauf oder ein
Crossfade zwischen beiden ist. Ein Layer vor dem Crossfade würde vom Crossfade
wieder weggemischt.

Konkret: die Zeile `composeBoneMatrices(*mesh, final_trs, smc.boneMatrices)` in
allen drei Systemen wird ersetzt durch

```cpp
HE::poseFinalize(world, cm, dt, e, *mesh, final_trs, smc,
                 rootMotion ? rootMotion->physics : nullptr, notifies);
```

und `poseFinalize` macht: **Layer-Stapel → IK → FK → IBM → `boneMatrices`**.
Ein Aufruf statt drei identischer, und die drei Treiber bleiben sonst unberührt.

### 2.2 Wie werden Bone-Masks pro Skelett gespeichert?

**Als eigener Asset-Typ, über Joint-NAMEN, mit einem Auflösungs-Cache pro
(Maske, Skelett)-Paar.**

* Nicht inline auf dem Layer: „UpperBody" wird von der Ziel-, der Treffer- und
  der Gesten-Ebene jedes Humanoiden benutzt. Dieselbe Namensliste auf jedem
  Layer jeder Entity zu duplizieren ist genau das, was die Migration des
  Zustandsautomaten vom Inline-Feld zum Asset abgeschafft hat.
* Über Namen und nicht über Indizes: ein Re-Import des Meshes kann die
  Joint-Reihenfolge ändern, und eine Maske, die dann die Finger meint statt der
  Schulter, fällt niemandem auf.
* `HE::BoneMask` liegt in **HE_Core** mit `HE_API`, wie
  `AnimatorStateMachineGraph` und aus demselben Grund.
* Die Auflösung Namen → `std::vector<float>` (ein Gewicht pro Joint des
  konkreten Skeletts) wird auf der Komponente gecacht, in genau der Form, die
  `AnimatorStateMachineComponent` schon benutzt: `resolvedMaskId` +
  `resolvedForMeshId` + `maskDirty`. Neu aufgelöst wird, wenn sich eines von
  beiden ändert, nicht pro Frame.

**Regel:** ein Joint-Name, der im Skelett nicht vorkommt, ergibt Gewicht **0**
und eine Logzeile (einmal, `HE_LOG_THROTTLE`). Nicht 1. Eine Maske, die nichts
trifft, darf nicht stillschweigend „alles" bedeuten — das ist der Unterschied
zwischen „mein Layer wirkt nicht" und „mein Layer hat den ganzen Charakter
überschrieben".

### 2.3 Welche Blend-Tree-Datenstruktur kollidiert am wenigsten mit dem Graph-Editor-Muster?

**Ein flacher Sample-Vektor, kein Node-Graph — und eine Struktur für 1D und 2D.**

Die Begründung steht schon im Baum, im Kopf von
`AnimatorStateMachineGraph.h:8-12`:

> *„Deliberately simpler than HE::ParticleGraph/MaterialGraph: there is no
> evaluate() step. … This also means no GraphEditor node-graph model is needed
> for states — the editor just edits this struct's vectors directly."*

Der Animationsteil des Editors hat sich bewusst **gegen** das
GraphEditor-Node-Modell entschieden. Ein Blend Tree als Node-Graph würde diese
Entscheidung in demselben Panel wieder umdrehen. Ein Blend Space ist ausserdem
gar kein Graph: er ist eine Punktwolke in einem 1D- oder 2D-Parameterraum, und
sein natürlicher Editor ist ein Diagramm mit ziehbaren Punkten, kein Netz aus
Knoten und Kanten.

Eine Struktur für beide Fälle, weil 1D der 2D-Fall ist, der `y` ignoriert —
zwei Strukturen wären zwei Sampler, zwei Serialisierer und zwei Editoren:

```cpp
enum class BlendSpaceKind : uint8_t { OneD = 0, TwoD = 1 };

struct BlendSample {
    HE::UUID clipId;
    float    x = 0.0f, y = 0.0f;   // Position im Parameterraum
    float    speedScale = 1.0f;    // pro Sample, für Clips mit falschem Tempo
};

struct HE_API BlendSpace {
    BlendSpaceKind kind = BlendSpaceKind::OneD;
    std::string    paramX, paramY;             // Namen aus animator::params
    float          minX = 0, maxX = 1, minY = 0, maxY = 1;  // nur Editor-Achsen
    std::vector<BlendSample> samples;
    bool           looping = true;
};
```

### 2.4 Wie greift IK ein — und wo genau?

**Als letzte Stufe von `poseFinalize`, also NACH dem Layer-Stapel und NACH
`rootMotionApply`, unmittelbar vor der FK.**

> **Abweichung vom Brett.** Die Aufgabenbeschreibung sagt „nach der finalen Pose
> aber **vor** dem Root-Motion-Apply-Schritt". Das ist hier bewusst anders
> herum, aus zwei Gründen:
>
> 1. Im Modus `RootMotionComponent::Mode::Transform` schreibt `rootMotionApply`
>    die `TransformComponent` der Entity. Genau diese Transform braucht die
>    Fuss-IK, um ihren Strahl in die Welt zu setzen. Läuft IK davor, tastet sie
>    den Boden an der Position des letzten Frames ab, und die Füsse hinken dem
>    Körper genau um einen Frame hinterher — sichtbar an jeder Treppenkante.
> 2. Es gibt keinen Konflikt, der die andere Reihenfolge erzwingen würde: Root
>    Motion fasst **nur den Root-Joint** an (extrahieren und sperren), IK fasst
>    Beine, Becken und Wirbelsäule an. Die beiden schreiben nicht in dieselben
>    Joints.
>
> Im Modus `CharacterController` setzt `rootMotionApply` nur eine
> Geschwindigkeit für den *nächsten* Physikschritt; die Transform ändert sich
> in diesem Frame gar nicht. Dort sind die Füsse also ohnehin einen Frame
> hinterher, egal wo IK steht. Die zeitliche Glättung (5.4) verdeckt das, und
> es ist dieselbe Ein-Frame-Latenz, die `MovementSystem` schon hat.

**Falle, die schon viermal zugeschlagen hat:** `tc.worldMatrix` ist innerhalb
von `tickAnimation` **eine Frame alt** — geschrieben wird sie nur von
`propagateTransforms`, und das läuft in dieser Phase nicht. IK muss die
Weltmatrix über `HE::worldMatrixOf` / `HE::worldPositionOf` beziehen. (Siehe
den Root-Motion-Plan, der an derselben Stelle steht: PhysicsWorld spricht Welt,
die Skript-Transform-API lokal.)

**Kein neuer Kontext für IK.** `HE::RootMotionContext` trägt bereits den
`PhysicsWorld*` und ist ausserhalb einer Play-Session bereits null.
`poseFinalize` bekommt schlicht `rootMotion ? rootMotion->physics : nullptr`
weitergereicht — keine Signaturänderung in `SceneSystems`, `GameApplication`
oder `EditorApplication`. Daraus folgt die Aufteilung:

* **Fuss-IK** ist auf einen Bodenstrahl angewiesen. Ohne `PhysicsWorld` wird
  ihr Gewicht auf 0 gezwungen. Das ist dasselbe Argument, das der
  Root-Motion-Plan für den Lock führt: *eine Vorschau, die anders post als das
  Spiel, ist keine.* Ein gegen eine geratene Ebene gelöster Fuss wäre genau das.
* **Look-At** läuft **immer**, auch im Editor ohne Physik. Einen Kopf im
  Editor auf ein Ziel auszurichten und das Ergebnis zu sehen ist der ganze
  Zweck davon; ein Strahl kommt darin nicht vor.

---

## 3. Modell: Layer, Masken, additives Blenden

### 3.1 Die Komponente

Eine eigene Komponente, die orthogonal zu allen drei Treibern liegt:

```cpp
struct AnimationLayerComponent {
    struct Layer {
        std::string name;                 // nur für den Inspector
        enum class Source : uint8_t { Clip = 0, BlendSpace = 1 };
        enum class Mode   : uint8_t { Override = 0, Additive = 1 };
        Source   source = Source::Clip;
        Mode     mode   = Mode::Override;
        HE::UUID clipId;                  // Source::Clip
        HE::UUID blendSpaceId;            // Source::BlendSpace
        HE::UUID maskId;                  // {} = ganzes Skelett, Gewicht 1
        float    weight = 1.0f;           // 0 = aus, aber Playhead läuft weiter
        // Additiv: die Referenzpose, gegen die die Differenz gebildet wird.
        HE::UUID additiveRefClipId;       // {} = derselbe Clip bei t=0
        float    additiveRefTime = 0.0f;
        // Playhead, pro Layer
        float    playbackTime = 0.0f, playbackSpeed = 1.0f;
        bool     looping = true, playing = true;
        // Laufzeit, nicht gespeichert
        bool     notifiesPrimed = false;
    };
    std::vector<Layer> layers;            // in Reihenfolge auf die Basis gelegt

    // Auflösungs-Cache, in der Form von AnimatorStateMachineComponent
    HE::UUID                        resolvedForMeshId;
    std::vector<std::vector<float>> resolvedMasks;   // pro Layer, ein Gewicht je Joint
    bool                            masksDirty = true;
};
```

**Warum eine eigene Komponente und nicht Layer im
`AnimatorStateMachineGraph`?** Unity legt Layer in den Animator, jede Ebene mit
ihrem eigenen Zustandsautomaten. Das ist das grössere Modell und es ist nicht
falsch — aber es hilft nur Entities, die überhaupt einen Zustandsautomaten
tragen, und es zwingt den kompletten Laufzeitzustand des Automaten
(`currentStateName`, `clipTime`, `inTransition`, `transition*`) in einen
Vektor, samt Serialisierer und Editor-Panel.

Die eigene Komponente ist das Muster, das dieser Baum schon benutzt:
`RootMotionComponent` ist genau das — eine optionale Komponente, die verändert,
was die Treiber tun, statt in einen von ihnen eingebaut zu sein. Sie
funktioniert unter allen dreien, sie kostet nichts, wenn sie fehlt, und sie
verbaut das Unity-Modell nicht: eine spätere `Source::StateMachine` schiebt es
nach.

**Verschoben, mit Grund:** ein Layer, dessen Quelle selbst ein
Zustandsautomat ist. Das braucht die Laufzeitstruktur des Automaten als eigenen
Typ (heute steckt sie in Feldern der Komponente) und einen Editor, der
geschachtelte Automaten zeigt. Das ist ein eigener Zuschnitt, kein Nebenbei.

### 3.2 Auftragen

Layer werden in Indexreihenfolge auf die Basispose gelegt, jeder auf das
Ergebnis des vorigen — wie in Unity, und aus demselben Grund: alles andere
lässt sich nicht mehr im Kopf ausrechnen.

Effektives Gewicht pro Joint: `w_eff = layer.weight * mask[j]`.

**Override:**
```
out[j].rot   = slerp(base[j].rot, layer[j].rot, w_eff)
out[j].trans = mix  (base[j].trans, layer[j].trans, w_eff)
out[j].scale = mix  (base[j].scale, layer[j].scale, w_eff)
```

**Additive** — die Differenz gegen die Referenzpose, im **lokalen** Raum:
```
delta.rot   = inverse(ref[j].rot) * layer[j].rot
delta.trans = layer[j].trans - ref[j].trans
delta.scale = layer[j].scale - ref[j].scale      // additiv, nicht multiplikativ

out[j].rot   = base[j].rot * slerp(identity, delta.rot, w_eff)
out[j].trans = base[j].trans + delta.trans * w_eff
out[j].scale = base[j].scale + delta.scale * w_eff
```

Additive Skalierung ist **additiv** und nicht multiplikativ, damit `w_eff = 0`
exakt `delta * 0 = 0` ergibt und die Basis bitgenau durchreicht. Multiplikativ
müsste die Neutralität bei 1 liegen und die Gewichtung wäre ein `pow`, was
niemand debuggen will.

### 3.3 Zwei Regeln, keine Notizen

1. **Die Layer-Stufe ruft niemals `rootMotionSampleClip`.** Root Motion gehört
   der Basis. Ein Layer-Clip, der Motion in seinem Root trägt, trägt sie
   schlicht nicht bei.
2. **Das Translationsgewicht des Root-Joints ist in der Layer-Stufe immer 0**,
   unabhängig davon, was die Maske sagt. Ein Override-Layer, dessen Maske den
   Root einschliesst, würde sonst die eben extrahierte und gesperrte Motion
   wieder in die Pose zurückschreiben — und die Figur bewegt sich doppelt. Das
   ist derselbe Fehler, den `lockRootJoint` für die Basis verhindert, nur eine
   Stufe später.

### 3.4 Notifies aus Layern

Ein Layer feuert seine Notifies, sobald sein effektives Gewicht **> 0** ist —
nicht nach `kNotifyDominanceAlpha`.

Grund: `kNotifyDominanceAlpha` löst ein anderes Problem. Bei einem Zwei-Clip-
Blend sind beide Clips **Alternativen** derselben Sache (Gehen und Rennen), und
beide feuern zu lassen gäbe zwei Schritte pro Schritt. Ein Layer ist keine
Alternative zur Basis, sondern eine Ergänzung: „Magazin fällt" auf dem
Nachladen-Layer und „Fusstritt" auf der Basis sind zwei verschiedene Ereignisse
und sollen beide kommen.

Was **nicht** anders ist: die Priming-Disziplin. Ein Layer mit Gewicht 0 wird
trotzdem am Playhead vorbeigelaufen (`notifyCollectClip` mit `dominant=false`),
sonst hält er sein erstes Frame zurück und wirft es in dem Moment aus, in dem
das Gewicht über 0 kippt. Deshalb `notifiesPrimed` pro Layer.

### 3.5 `poseFinalize` läuft genau einmal pro Entity und Frame

`tickAnimation` ruft drei Treiber, und nichts verhindert, dass eine Entity zwei
davon trägt. Ohne Schutz würde `poseFinalize` zweimal laufen: Layer-Playheads
rückten doppelt vor, Notifies kämen doppelt, IK löste zweimal.

Deshalb bekommt `AnimationLayerComponent` (und `IkComponent`) ein
`finalizedThisFrame`, das in `poseFinalize` gesetzt und in
`HE::poseBeginFrame(world)` gelöscht wird — aufgerufen von
`SceneSystems::tickAnimation` direkt neben `rootMotionBeginFrame`. Der zweite
Treiber schreibt seine `boneMatrices` dann roh (`composeBoneMatrices`) und sagt
es einmal ins Log. Das Vorbild ist wörtlich `RootMotionComponent::
appliedThisFrame`, das für dieselbe Situation dieselbe Antwort gibt.

**Eine Entity mit `AnimationLayerComponent`, aber ohne Basistreiber**, wird
nicht unterstützt: die Komponente tut dann nichts und sagt es einmal ins Log.
Ein Rückfall auf die Bindepose wäre ein vierter Schreiber auf `boneMatrices`
für einen Fall, den niemand absichtlich baut.

---

## 4. Modell: Blend Spaces

### 4.1 Gewichtung

**1D** — nach `x` sortieren, das umschliessende Paar suchen, linear
interpolieren. Ausserhalb des Bereichs auf das Randsample klemmen (nicht
extrapolieren: ein Charakter, der bei Geschwindigkeit 12 doppelt so schnell die
Beine bewegt wie das schnellste Sample vorsieht, ist kein Feature).

**2D** — **Gradient Band** (Rune Skovbo Johansen, das Verfahren hinter Unitys
„Freeform Cartesian"). Etwa 30 Zeilen, keine Triangulierung, beliebige
Sample-Platzierung, liefert an einem Sample-Punkt exakt 1.0 für dieses Sample.
Delaunay (Unreals Weg) wäre der Alternative, braucht aber einen Triangulator,
den dieser Baum nicht hat, und einen Degeneriert-Fall (kollineare Samples), den
Gradient Band gar nicht kennt.

**Deckelung:** nur die **vier** schwersten Samples werden ausgewertet, die
Gewichte danach renormalisiert. Heute kostet ein Blend zwei
`sampleClip`-Durchläufe pro Entity und Frame; ein 3×3-Blend-Space wären neun.
Vier ist die Grenze, unter der die Kosten planbar bleiben, und ein fünftes
Sample mit spürbarem Gewicht heisst, dass der Raum zu dicht besetzt ist.

### 4.2 N-Wege-Mischung der Posen

`blendTRS` ist zweiwegig, und ein echtes N-Wege-Slerp für Quaternionen ist nicht
assoziativ. Der übliche und hier gewählte Weg ist die **iterative normalisierte
paarweise Mischung**, gesät mit dem schwersten Sample:

```
acc = pose[0]; accW = w[0];                     // Samples nach Gewicht absteigend
for i in 1..n-1:
    acc = blendTRS(acc, pose[i], w[i] / (accW + w[i]));
    accW += w[i];
```

Absteigend sortiert, damit die Kette beim dominanten Sample beginnt: bei
Quaternionen hängt das Ergebnis von der Reihenfolge ab, und die Abweichung ist
am kleinsten, wenn der grösste Beitrag der Anker ist.

**`blendTRS` muss dafür repariert werden:** heute `resize(min(a,b))`. Die
Akkumulation läuft über gleich grosse Vektoren, aber der Abschnitt bleibt eine
stille Falle, und ein Layer gegen eine kürzere Basis liefe genau hinein. Neu:
auf `max` resizen und die fehlende Seite als Default-TRS behandeln.

### 4.3 Phasensynchronisation — die Entscheidung, an der Locomotion hängt

Ein Blend Space läuft **nicht** auf einer absoluten Zeitachse, sondern auf einer
normalisierten **Phase** `p ∈ [0, 1)`.

Der Zwei-Clip-Blend heute wrappt jeden Clip gegen seine eigene Dauer auf einer
gemeinsamen absoluten Zeitachse (`AnimationBlendSystem.cpp:114-118`). Für Gehen
(1,2 s) und Rennen (0,8 s) heisst das: die beiden Zyklen laufen auseinander,
und mitten im Blend kreuzen sich die Füsse — der linke Fuss der einen Pose
trifft auf den rechten der anderen. Das ist der klassische Locomotion-Fehler,
und er ist der Grund, warum Blend Spaces überall auf Phase laufen.

```
Sample i wird gesampelt bei   p * dur_i
p rückt vor um                dt * speed / weightedDuration
weightedDuration            = Σ w_i * dur_i * (1 / speedScale_i)
```

Folgen, die im Code stehen müssen:

* Die Root-Motion-Spanne pro Sample ist `(pPrev * dur_i, pEnd * dur_i]`, die
  Notify-Spanne dieselbe. `rootMotionSampleClip` bekommt also weiterhin
  entwrappte Sekunden, nur eben pro Sample umgerechnet.
* `AnimatorStateMachineComponent::clipTime` bedeutet für einen Clip-Zustand
  **Sekunden** und für einen Blend-Space-Zustand **Phase**. Das ist eine
  Doppelbedeutung auf einem Feld, und sie muss im Header stehen, sonst rechnet
  der Nächste sie gegen `curClip->duration`.
* Ein Übergang **in** einen Blend-Space-Zustand startet bei `p = 0`.
* `weightedDuration <= 0` (alle Clips fehlen) heisst: Phase steht, kein Vorrücken.

### 4.4 Notifies aus einem Blend Space

Es feuert **das Sample mit dem grössten Gewicht**, die anderen werden nur
vorbeigelaufen. Das ist die wörtliche Verallgemeinerung von
`kNotifyDominanceAlpha` von zwei Clips auf N: dort feuert die schwerere Hälfte,
hier das schwerste Sample. Bei Gleichstand gewinnt der kleinere Index, damit das
Ergebnis nicht zwischen zwei Frames flattert.

### 4.5 Root Motion aus einem Blend Space

Pro Sample ein Delta über seine eigene Spanne, dann `blendRootMotion` über die
gewichtete Kette (dieselbe iterative Normalisierung wie bei der Pose, mit
denselben Gewichten). Andere Gewichte für Delta und Pose lassen die Figur genau
dort schneller oder langsamer werden, wo der Blend arbeitet — das steht schon
so im Zwei-Clip-Fall.

### 4.6 Erreichbarkeit: `PoseSource`

Ein Blend Space wird über zwei Wege benutzt:

* **Als Quelle eines Zustands.** `HE::AnimationState` bekommt neben `clipId` ein
  `blendSpaceId`. Gesetztes `blendSpaceId` gewinnt; alte Graphen haben es nicht
  und verhalten sich unverändert.
* **Als Quelle eines Layers** (`Layer::Source::BlendSpace`).

Damit `AnimationStateMachineSystem` nicht an jeder `curClip`-Stelle
verzweigt — und das sind ein Dutzend, zwischen Playhead, Notifies, Root Motion
und Pose — kommt ein kleiner interner Adapter in `AnimationEval.h`:

```cpp
struct PoseSource {              // Clip oder Blend Space, aus einer Hand
    float duration(...) const;   // Clip: clip->duration.  BlendSpace: weightedDuration
    void  sample(float tOrPhase, std::vector<JointTRS>& out) const;
    void  collectNotifies(...) const;
    bool  rootMotion(...) const;
};
```

Der Zustandsautomat fragt dann `PoseSource` und nicht `AnimationClipAsset*`.
Das ist der Umbau, der Schritt 3 gross macht, und ohne ihn wird der Automat zu
einer Kette von `if (bs) … else …`.

**Verschoben, mit Grund:** ein eigener `AnimatorBlendSpaceComponent` als
vierter Treiber. Er wäre billig, aber er verschlimmert die Warze aus 1.2 — vier
Systeme, die auf dieselben `boneMatrices` schreiben, letzter gewinnt. Wer einen
Blend Space ohne Automaten will, nimmt einen Layer mit Gewicht 1 und ohne Maske.

### 4.7 Vorschau

`AnimationPreview::evaluateBlendSpacePose(mesh, space, x, y, phase, out)` —
damit der Blend-Space-Editor die Pose an einem beliebigen Punkt des Diagramms
zeigen kann, ohne eine Entity zu haben.

---

## 5. Modell: IK

### 5.1 Der Umbau, den IK braucht

`composeBoneMatrices` wird aufgeteilt:

```cpp
void composeModelMatrices(const SkeletalMeshAsset&, const std::vector<JointTRS>&,
                          std::vector<glm::mat4>& outModel);       // FK, ohne IBM
void applyInverseBind    (const SkeletalMeshAsset&, const std::vector<glm::mat4>& model,
                          std::vector<glm::mat4>& outBoneMatrices);
```

`composeBoneMatrices` bleibt als Verkettung der beiden bestehen — Preview und
Tests rufen es, und es ist die Fassung, die ein Treiber ohne Layer und ohne IK
weiterhin nimmt.

IK bekommt `(mesh, localTRS, modelMats, entityWorldMatrix)`, löst in
Modellraum, schreibt **lokale Rotationen** nach `localTRS` zurück und frischt
den betroffenen Teilbaum in `modelMats` auf. Nur den Teilbaum: eine komplette
FK pro IK-Kette wäre für einen Zweibeiner dreimal die ganze Kinematik.

### 5.2 Zwei-Knochen-Löser

Klassisch über den Kosinussatz: aus Hüft-, Knie- und Fussposition im Modellraum
und der Zielposition ergibt sich der neue Kniewinkel; die Hüfte wird auf das
Ziel ausgerichtet, das Knie um den neuen Winkel gebeugt.

**Der Polvektor kommt aus der AKTUELLEN Kniestellung**, nicht aus einer festen
Vorwärtsachse. Ein fester Pol dreht das Knie auf jedem Skelett, dessen Bindepose
nicht zufällig dieselbe Achsenkonvention hat, nach hinten — und Blender-Exporte
tragen konstant −90° X auf dem Root, weshalb „vorne" dort nicht ist, wo man es
vermutet (dieselbe Falle, die der Root-Motion-Plan bei der
Frame-0-Referenzierung beschreibt).

**Unerreichbares Ziel** (weiter weg als Oberschenkel + Unterschenkel): Bein
gestreckt in Zielrichtung, nicht überstreckt und nicht zurück in die
Ausgangspose springend.

### 5.3 Fuss-Placement

Pro Fuss:

1. Strahl von `fussPositionWelt + up * traceUp` nach unten über
   `traceUp + traceDown`. `PhysicsWorld`-Raycast, wie ihn das Wettersystem für
   seine Bodenhöhen benutzt.
2. Trifft er nichts: Gewicht dieses Fusses geht auf 0 (Figur in der Luft).
3. Trifft er: Zielhöhe = Trefferpunkt + `footHeightOffset` (der Abstand vom
   Knöchel zur Sohle, den kein Skelett von sich aus kennt).
4. **Beckenabsenkung:** die Absenkung ist das **grösste negative** Offset über
   alle Füsse; das Becken wird um diesen Betrag gesenkt, die Ziele der Füsse um
   denselben Betrag mitgezogen. Ohne diesen Schritt überstreckt sich auf einer
   Stufe das untere Bein, und das ist der Unterschied zwischen „IK" und „IK,
   die auf Treppen funktioniert".
5. Erst danach der Zwei-Knochen-Löser pro Bein.
6. **Fussrotation** auf die Trefferflächennormale, geklemmt auf
   `maxPitchDegrees` / `maxRollDegrees`. Ungeklemmt stellt sich ein Fuss an
   einer Wand um 90° auf.

### 5.4 Zeitliche Glättung

Offsets werden pro Frame gegen den letzten Wert exponentiell nachgeführt
(`interpSpeed`). Ohne das springt der Fuss an jeder Treppenkante, weil der
Bodenstrahl dort in einem Frame um eine Stufenhöhe kippt. Der geglättete Wert
ist Laufzeitzustand, wird nicht gespeichert und beim Wechsel der Entity-Position
über eine Schwelle (Teleport) zurückgesetzt.

### 5.5 Look-At

Eine Kette von 1 bis 3 Joints (typisch Spine → Neck → Head) mit einem Gewicht
pro Kettenglied, das sich zu 1 summiert — so verteilt sich die Drehung, statt
den Kopf allein zu verdrehen.

* Ziel wahlweise als Weltpunkt oder als Ziel-Entity (dann `HE::worldPositionOf`,
  **nicht** `tc.worldMatrix`).
* Yaw und Pitch geklemmt (`maxYaw`, `maxPitch`), gemessen gegen die
  Vorwärtsrichtung der Kettenwurzel. Ungeklemmt dreht sich der Kopf um 180°,
  wenn das Ziel hinter der Figur steht.
* `interpSpeed` wie bei den Füssen — ein Ziel, das ausserhalb des Kegels
  springt, soll den Kopf nicht schnappen lassen.
* Läuft ohne `PhysicsWorld`, also auch im Editor (siehe 2.4).

### 5.6 Die Komponente

```cpp
struct IkComponent {
    struct FootIk {
        std::string hipJoint, kneeJoint, footJoint;   // leer = auto: footJoint + 2 Eltern
        float weight = 1.0f;
        float traceUp = 0.5f, traceDown = 0.6f;
        float footHeightOffset = 0.0f;
        float maxPitchDegrees = 30.0f, maxRollDegrees = 20.0f;
        float interpSpeed = 10.0f;
        bool  alignToNormal = true;
        float smoothedOffset = 0.0f;                  // Laufzeit
    };
    std::vector<FootIk> feet;
    bool  adjustPelvis = true;
    std::string pelvisJoint;                          // leer = Elternteil beider Hüften

    struct LookAt {
        bool enabled = false;
        std::vector<std::string> chain;               // Wurzel zuerst
        std::vector<float>       chainWeights;
        HE::UUID  targetEntityId;                     // {} = targetWorld benutzen
        glm::vec3 targetWorld{0.0f};
        float weight = 1.0f, maxYaw = 70.0f, maxPitch = 45.0f, interpSpeed = 8.0f;
        glm::vec2 smoothedAngles{0.0f};               // Laufzeit
    } lookAt;

    HE::UUID          resolvedForMeshId;              // Auflösungs-Cache
    std::vector<int>  resolvedJoints;
    bool              jointsDirty = true;
    bool              finalizedThisFrame = false;
};
```

---

## 6. Schnitt der Folgeschritte

Jeder Schritt bringt seine Editor-Bedienelemente **und deren Hilfeeinträge**
selbst mit — `editor_help_audit` ist ein ctest, kein Aufräumschritt.

### Schritt 2 — Pose-Pipeline, Layer und Bone-Masks

Beginnt mit `git merge origin/main`.

Dateien: `AnimationEval.h/.cpp` (`blendTRS` auf `max` statt `min`;
`composeModelMatrices`/`applyInverseBind`), neu
`src/HE_Scene/src/PoseFinalize.h/.cpp`, neu
`src/HE_Core/include/BoneMask/BoneMask.h` + `.cpp` (`HE_API`, JSON-Roundtrip
wie `AnimatorStateMachineGraph`), neu
`Components/AnimationLayerComponent.h`, `Assets.h` (`BoneMaskAsset`),
`HAsset.h` (`CHUNK_BMSK`), `ContentManager.cpp` (lesen/schreiben/registrieren),
die drei Animationssysteme (je eine Zeile), `SceneSystems.cpp`
(`poseBeginFrame`), `SceneSerializer.cpp`, `HorizonWorld::reserveComponent
Storage` (`AnimationLayerComponent` — eine Game-DLL kann sie zur Laufzeit als
erste anlegen), `SceneSystems::collectAssetRefs` (Layer-Clip- und Masken-IDs),
`InspectorPanel.cpp` + Hilfeeinträge, neues Masken-Editor-Panel (Joint-Baum mit
Häkchen und Gewichten).

Tests (`tests/test_animation_layers.cpp`), alle physikfrei:

1. Maskenauflösung: bekannter Joint bekommt sein Gewicht; **unbekannter Name
   ergibt 0, nicht 1**; leere `maskId` ergibt überall 1.
2. `masksDirty`/`resolvedForMeshId`: ein zweiter Tick mit unverändertem Mesh
   löst nicht neu auf; ein Mesh-Wechsel schon.
3. Override-Layer mit Gewicht 0 liefert die Basispose **bitgenau**.
4. Override-Layer mit Gewicht 1 und Vollmaske liefert die Layer-Pose bitgenau.
5. Maske trifft nur den Oberkörper: die Beinjoints der Ergebnispose sind
   identisch zur Basis, die Armjoints identisch zum Layer.
6. Additiv: Layer-Clip **gleich** der Referenzpose ergibt die Basis bitgenau
   (Delta ist Identität) — der Test, der `inverse(ref) * layer` von
   `layer * inverse(ref)` unterscheidet.
7. Additiv mit halbem Gewicht: die halbe Delta-Drehung, nicht die halbe
   absolute Drehung.
8. Root-Regel: Override-Layer mit Vollmaske auf einem Clip mit Root-Motion —
   die Root-Translation der Ergebnispose ist die gesperrte der Basis, nicht die
   des Layers.
9. Zwei Treiber auf einer Entity: `poseFinalize` läuft **einmal**, der
   Layer-Playhead ist nach einem Frame um `dt` vorgerückt, nicht um `2 dt`.
10. Layer-Notifies: ein Layer mit Gewicht 0.2 feuert (Basis feuert auch); ein
    Layer mit Gewicht 0 feuert nicht, hält sein Frame-0-Notify aber auch nicht
    zurück, wenn das Gewicht im nächsten Frame steigt.
11. `blendTRS` mit ungleich langen Vektoren: das Ergebnis hat `max`-Länge und
    die überzähligen Joints tragen den Default.
12. Serialisierung: Layer-Liste über Speichern/Laden identisch,
    `notifiesPrimed` und der Auflösungs-Cache **nicht** in der Datei.

### Schritt 3 — Blend Spaces (1D/2D)

Dateien: neu `src/HE_Core/include/BlendSpace/BlendSpace.h` + `.cpp` (`HE_API`,
JSON), `Assets.h` (`BlendSpaceAsset`), `HAsset.h` (`CHUNK_BLSP`),
`ContentManager.cpp`, `AnimatorStateMachineGraph.h` (`AnimationState::
blendSpaceId` + beide JSON-Leser + `animationStateToJson`/`FromJson` für die
Kollaborationsschicht), `AnimationEval.h/.cpp` (`PoseSource`, Gradient Band,
iterative Mischung), `AnimationStateMachineSystem.cpp` (auf `PoseSource`
umstellen), `PoseFinalize.cpp` (`Layer::Source::BlendSpace`),
`AnimationPreview.h/.cpp`, `collectAssetRefs`, neues
Blend-Space-Editor-Panel (Diagramm mit ziehbaren Punkten, Vorschau-Cursor) +
Hilfeeinträge, `AnimatorStateMachineEditorPanel.cpp` (Zustand kann eine
Blend-Space-Quelle wählen) + Hilfeeinträge.

Tests (`tests/test_blend_space.cpp`):

1. 1D exakt auf einem Sample: Gewicht 1 dort, 0 überall sonst; die Pose ist
   bitgenau `sampleClip` dieses Clips.
2. 1D zwischen zwei Samples: Gewichte summieren zu 1, linear in `x`.
3. 1D ausserhalb des Bereichs: **klemmt** auf das Randsample, extrapoliert nicht.
4. 2D Gradient Band: auf einem Sample-Punkt exakt 1.0; im Schwerpunkt dreier
   symmetrischer Samples je 1/3; Gewichte summieren immer zu 1.
5. Deckelung: bei sechs Samples mit Gewicht > 0 überleben vier, renormalisiert
   auf Summe 1.
6. **Phase:** Clips mit 1,2 s und 0,8 s, Gewichte je 0,5. Bei `p = 0.5` wird
   der eine bei 0,6 s und der andere bei 0,4 s gesampelt — der Test, der Phase
   von absoluter Zeit unterscheidet und ohne den die Füsse sich kreuzen.
7. Phasenvorschub: `weightedDuration` bei 50/50 ist 1,0 s; nach 1,0 s ist
   `p` wieder 0.
8. Root Motion: Delta bei 50/50 ist das Mittel der beiden Clip-Deltas über
   ihre **eigenen** Spannen.
9. Notifies: nur das schwerste Sample feuert; bei Gleichstand der kleinere
   Index, und zwar über mehrere Frames stabil.
10. N-Wege-Mischung: die Kette absteigend nach Gewicht liefert bei zwei Samples
    exakt `blendTRS`.
11. `AnimationState` mit gesetzter `blendSpaceId` gewinnt über `clipId`; ein
    Graph-JSON ohne `blendSpaceId` lädt unverändert.
12. Übergang in einen Blend-Space-Zustand startet bei `p = 0`.

### Schritt 4 — IK: Fuss-Placement und Look-At

Dateien: neu `src/HE_Scene/include/HorizonScene/AnimationIk.h` +
`src/HE_Scene/src/AnimationIk.cpp` (öffentlich, damit Preview und Tests
denselben Löser fahren — dieselbe Begründung wie bei `RootMotion.h`), neu
`Components/IkComponent.h`, `AnimationEval.h/.cpp` (die Aufteilung aus 5.1,
falls in Schritt 2 noch nicht erledigt), `PoseFinalize.cpp` (IK-Stufe),
`SceneSerializer.cpp`, `reserveComponentStorage` (`IkComponent`),
`AnimationPreview.h/.cpp` (Look-At in der Mesh-Vorschau),
`InspectorPanel.cpp` + Hilfeeinträge, Viewport-Gizmo für das Look-At-Ziel.

Tests (`tests/test_animation_ik.cpp`), 1–6 physikfrei:

1. Zwei-Knochen, erreichbares Ziel: der Fuss steht nach dem Lösen **exakt** auf
   dem Ziel (Toleranz 1e-4).
2. Unerreichbares Ziel: das Bein ist gestreckt (Kniewinkel 180°) und zeigt in
   die Zielrichtung; die Kettenlänge ist nicht überschritten.
3. Polvektor: ein Skelett, dessen Knie in der Ausgangspose nach vorn gebeugt
   ist, behält nach dem Lösen ein nach vorn gebeugtes Knie. Dasselbe Skelett
   mit nach hinten gebeugtem Knie behält es nach hinten. **Das ist der Test,
   der einen festen Pol von einem aus der Pose abgeleiteten unterscheidet.**
4. Ziel gleich der Ausgangsposition des Fusses: die Pose ist bitgenau unverändert.
5. Beckenabsenkung: ein Fuss 20 cm tiefer als der andere senkt das Becken um
   20 cm, und **beide** Beine bleiben innerhalb ihrer Kettenlänge.
6. Look-At: Ziel geradeaus ergibt keine Drehung; Ziel bei 90° Yaw wird auf
   `maxYaw` geklemmt; die Kettengewichte verteilen die Drehung im angegebenen
   Verhältnis.
7. `physics == nullptr`: die Fussgewichte sind 0 und die Ergebnispose ist
   **bitgenau** die ohne `IkComponent` — die Vorschau post wie das Spiel.
8. `physics == nullptr`: Look-At läuft trotzdem und verändert die Pose.
9. Strahl trifft nichts: das Gewicht dieses Fusses geht auf 0, das andere Bein
   bleibt gelöst.
10. Glättung: eine Bodenstufe von 20 cm bewegt den Fuss im ersten Frame um
    weniger als 20 cm und nähert sich über mehrere Frames an.
11. Gegen eine headless `PhysicsWorld` (wie die bestehenden Physiktests): eine
    Figur auf schrägem Boden bekommt beide Füsse auf die Fläche und eine
    Fussdrehung innerhalb der Klemmgrenzen.

---

## 7. Risiken und offene Punkte

* **Mesh-Space-Additive fehlt.** Unreal hat lokalen und Mesh-Raum-Additiv; der
  Mesh-Raum ist das, was ein Aim-Offset auf einer gedrehten Wirbelsäule richtig
  aussehen lässt. Dieser Plan macht nur den lokalen Raum, weil eine
  TRS-Pipeline ihn geschenkt bekommt. **Er wird billig, sobald Schritt 4
  `composeModelMatrices` eingeführt hat** — dann ist die Modellraum-Pose ohnehin
  da, und Mesh-Space-Additiv ist ein Modus mehr in derselben Schleife. Deshalb
  hier bewusst verschoben und nicht gestrichen.
* **Gradient Band ist O(N²)** in der Sample-Zahl. Bei den 5 bis 15 Samples, die
  ein Blend Space realistisch hat, ist das nichts; bei 200 wäre es etwas. Ein
  Log-Hinweis ab einer Sample-Zahl, die niemand absichtlich baut, reicht.
* **`blendTRS`s Abschnitt auf `min`** ist heute eine stille Datenlöschung, die
  bisher nur deshalb nicht auffiel, weil beide Seiten immer gleich lang waren.
  Schritt 2 fasst sie an; jeder bestehende Aufrufer muss dabei mitgeprüft werden.
* **Windows-CI:** neue Typen in HE_Core brauchen `HE_API`, neue Typen in
  HE_Scene dürfen es **nie** haben. `HE::BoneMask` und `HE::BlendSpace` liegen
  in HE_Core (ContentManager muss sie halten dürfen), die Komponenten in
  HE_Scene.
* **Vier Systeme auf denselben `boneMatrices`.** Der Plan verschlimmert die
  Warze nicht (kein vierter Treiber) und entschärft sie ein Stück
  (`finalizedThisFrame` sagt es jetzt ins Log), behebt sie aber nicht.
* **Kein Backend, kein Shader, kein Renderer.** Wie beim Root-Motion-Zuschnitt
  liegt alles in der Gameplay-Animationsschicht; die
  `boneMatrices`-Schnittstelle zum Renderer ändert sich nicht. Es gibt daher
  auch nichts an dieser Arbeit, das auf echter Hardware anders aussähe als im
  Test — mit einer Ausnahme: die visuelle Beurteilung von IK und Blend Spaces
  braucht ein Auge auf einer laufenden Figur, und die Tests oben prüfen
  Zahlen, keine Ästhetik.
