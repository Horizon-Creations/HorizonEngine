# Entity-Cooking: Bestandsaufnahme und Baseline

Stand 13.11.2026, Branch `claude/entity-cooking-statische-entities-auf-daten-reduzieren`, HEAD
`3b2be965`. Thema 343, Schritt 1: wo die CPU-Zeit für unveränderliche Entities liegt, ob es
bereits ein Static-/Mobility-Flag gibt (nein), und welche Komponenten/Querverweise eine Entity
vom Kochen ausschließen. **Kein Produktivcode geändert in diesem Schritt.**

Vorgaben aus dem Thema gelesen, bevor gemessen wurde: `docs/render-extractor-shadow-pass-plan.md`
(FrameScope / ein Extract pro Frame, Thema 162 — **bereits im Code**, siehe §1) und
`docs/asset-optimization-plan.md` §5 (Auto-LOD ist ein eigenes, noch nicht gebautes Feature —
`LODSystem.cpp` existiert schon mit pro-UUID-Levels und per-Frame-Laufzeitauswahl, siehe §5 Regel 8
und §9).

## 1. Was aus Thema 162 schon da ist (nicht doppelt bauen)

`docs/render-extractor-shadow-pass-plan.md` §9 ist der Abschluss jenes Themas: ein persistenter
Extract pro Frame (`RenderExtractor::FrameScope`, auf Metal genutzt), eine dirty-flag-basierte
Transform-Propagierung (`TransformComponent::dirty`, `TransformHierarchy.cpp`) und ein
Lokale-Matrix-Cache. Im aktuellen Checkout bestätigt:

- `TransformHierarchy.cpp:170-208` (`scan`) markiert nur Entities, deren Wertvergleich sich
  geändert hat, als `dirty`; `:223-258` (`recomputeFlagged`/`propagateFrom`) rechnet nur
  geflaggte Teilbäume neu.
- Das verbleibende, von Thema 162 selbst benannte Problem (§9.7): *"der Walk selbst, nur
  geänderte Entities neu schreiben"* — das ist exakt dieses Thema (Entity-Cooking). Der Walk
  (`RenderExtractor::extractMeshes`) läuft laut jener Messung bei 50 574 Entities (Metal,
  ruhende Welt) mit 18,7 ms immer noch der größte Einzelposten des Frames, obwohl nichts sich
  bewegt.

Kein Konflikt mit FrameScope: Entity-Cooking würde den Walk selbst verkleinern (cooked Entities
tragen vorgerechnete Render-Objekte, die der Walk nicht mehr aus der ECS herleiten muss), nicht
die Cross-Pass-Wiederverwendung ändern, die Thema 162 baut.

## 2. Grep: existiert schon ein Static-/Mobility-Flag?

```
grep -rniE "isStatic|Mobility|bIsStatic" src/HE_Scene --include=*.h --include=*.cpp
```

**Kein Treffer.** (Ein breiterer Lauf nach `static.*bool` findet nur reguläre C++-`static`-Lokale
und -Methoden, z. B. `AudioEngine.h:136 static bool steamAudioCompiledIn()` — keine
Entity-Eigenschaft.) `RigidBodyComponent::type` hat zwar einen Wert `RigidBodyType::Static`
(`Types/Enums.h:253-258`), aber das ist eine Physik-Eigenschaft des Jolt-Body, keine
übergreifende "diese Entity ändert sich nie"-Markierung, die ein Cooking-System lesen könnte.
Schritt 2 fängt bei null an.

## 3. Hot paths: was jede Entity jeden Frame kostet

Geprüft: entt-`view<T...>()` iteriert nur die Entities, die ALLE verlangten Komponenten tragen
(die kleinste beteiligte Storage führt den Walk an). Ein `view<ScriptComponent>` oder
`view<AnimatorComponent>` kostet eine Entity ohne diese Komponente **nichts** — die ganze Liste
der `*System.cpp`-Dateien unter `src/HE_Scene/src` (`AnimationSystem`, `MovementSystem`,
`WeatherSystem`, `SequenceSystem`, `ParticleSystem`, `NavigationSystem`, `PropertyAnimationSystem`,
`RopeTrailSystem`, …, alle über `SceneSystems::tickWorld`/`tickAnimation`,
`SceneSystems.cpp:106-245`) ist also für eine reine Mesh-Entity bereits kostenlos, weil sie keine
dieser Komponenten trägt. Das ist **nicht** der Hebel dieses Themas.

Vier Stellen sind es, weil sie über `TransformComponent`, `MeshComponent` oder die komplette
Registry laufen, unabhängig davon, was sonst an der Entity hängt:

| Scope | Datei:Zeile | Was pro Entity passiert |
|---|---|---|
| `Transforms::scan` | `TransformHierarchy.cpp:170-208` | liest `TransformComponent` jeder Entity mit dieser Komponente, `memcmp` gegen den Cache-Key, setzt `dirty` |
| `Transforms::propagate` / `recomputeFlagged` | `TransformHierarchy.cpp:223-258` | steigt für jede geflaggte Entity die `HierarchyComponent`-Kette ab |
| `Extract::meshes` (`extractMeshes`) | `RenderExtractor.cpp:326-470+`, aufgerufen `:1609` | `view<TransformComponent, MeshComponent>()`: pro Entity `visible`/`ActiveFilter` prüfen, `RenderObject` (Mesh-/Material-UUID, Weltmatrix, Bounds) neu bauen — **jeden Frame neu**, auch wenn nichts sich geändert hat |
| `RefineBounds` | `FrameCull.h:77` | pro `RenderObject` die Bounds verfeinern |
| `FrustumCuller::cull` | `FrustumCuller.cpp:48-71` | pro `RenderObject` ein Frustum-Test, **2× pro Frame** gemessen (Schatten + Szene; gemessen, nicht angenommen — siehe §4.2) |
| `SortObjects` | `D3D12Renderer.cpp:11501` (gleiches Muster `D3D11Renderer.cpp`) | sortiert die `RenderObject`-Liste nach dem Extract, pro Backend-Aufruf — kein `RenderExtractor.cpp`-Code, aber genauso O(Entity-Zahl) |

Geprüft und **kein** Treffer (nicht Teil des Frame-Hot-Path):

- `HorizonWorld.cpp:544` (`for (auto e : m_registry.view<entt::entity>())`) — das ist in `clear()`,
  läuft nur beim Entladen der Welt, nicht pro Frame.
- `LODSystem.cpp:29` — `view<LODComponent, MeshComponent>()`, kostet eine Entity ohne
  `LODComponent` nichts.

`extractMeshes` selbst ist bereits block-parallelisiert (`parallel_for`, `kBlock = 2048`,
`RenderExtractor.cpp:447-467`) — der Gewinn aus Cooking ist nicht "parallel statt seriell",
sondern "gar nicht neu bauen, wenn die Entity sich seit dem letzten Cook nicht geändert hat".

## 4. Messung

### 4.1 Methodik

Werkzeug: `scripts/perf/gen_reference_world.py` (bereits vorhanden, Thema 153/162) erzeugt eine
`.hescene` mit N Mesh-Entities, 0 % Physik (`--physics 0`, reiner Würfel/Kugel-Mesh +
`TransformComponent`, kein Skript/Animator/Netzwerk — die Kriterien aus §5 sind damit für jede
erzeugte Entity erfüllt), gruppiert unter `N/100` Gruppen-Entities (Hierarchietiefe 2). Capture
über `scripts/he_perf_capture.py --no-counters`, Backend D3D12 (NN-WS03, RTX 4070), Kamera
`0,25,90,0,-0.25`, `HE_HIDDEN_WINDOW=1`, `HE_BACKGROUND_FPS=0`.

Editor-Binary: **selbst aus diesem Branch gebaut** (`C:/hwec1`, Release, MSVC/Ninja,
`cmake --preset x64-release -B C:/hwec1 -DDEPLOY_DIR=C:/hwec1/deploy …`), **nicht** aus einem der
auf diesem Gerät herumstehenden Fremd-Worktree-Deploys — zwei davon (`C:/hw21`, ein Deploy des
Themas 326/Landscape) wurden probiert und wieder verworfen: ihr `RenderExtractor.cpp` weicht vom
Stand dieses Branches ab (94 Zeilen, LOD-Material-Auflösung aus einem noch nicht gemergten Thema)
und — wichtiger — ihr Projekt-/Szenenlade-Pfad lud die 50k-Szene bei `--warmup 0` gar nicht
innerhalb der 600 aufgezeichneten Frames (kein `SceneLoadTiming`-Log, Walk/Scan praktisch 0 ms).
Mit genügend Warmup (`--warmup 5000`) lud die 50k-Szene bei `C:/hw21` dann doch, aber das zeigt nur,
dass sich auf einen fremden, divergenten Deploy für dieses Thema nicht verlassen lässt. Eigener
Build von HEAD vermeidet das.

**Falle, auf die die ersten Läufe (mit dem fremden `C:/hw21`-Deploy) liefen:** Projekt- und
Szenenladen laufen asynchron zur Frame-Schleife. Bei `--warmup 0` begann die
Profiler-Aufzeichnung dort sofort bei Frame 1, und für die 50k-Szene stand in den aufgezeichneten
600 Frames nirgends ein `SceneLoadTiming`-Log — die Welt blieb während der gesamten Aufzeichnung
leer, `Extract::meshes`/`Transforms::scan` lagen praktisch bei 0 ms. Für die 10k-Szene lud
`hw21` zwar (bei Frame ~393 von 600), aber ein Teil der aufgezeichneten Frames fiel trotzdem auf
die noch leere Welt, der Rest auf die geladene — das erklärt die anfängliche 6-fache Schwankung
(0,24 ms bis 1,45 ms CPU p50 für dieselbe 10k-Szene, je nachdem, wie schnell das Laden in diesem
Lauf zufällig war).

**Gegengeprüft, ob das am Engine-Code liegt oder am fremden Deploy:** mit dem eigenen Build
(`C:/hwec1`) lädt dieselbe 10k-Szene bei `--warmup 0` bei Frame 6
(`SceneLoadTiming: entities=10168 … f 6`, 13:41:11.823) — weit vor dem Aufzeichnungsende bei
Frame 601 (13:41:12.797) — keine Verzerrung, kein Treffer der Falle. Die Falle ist also eine
Eigenschaft des fremden, divergenten `hw21`-Deploys (vermutlich eine dort laufende,
nicht gemergte Async-Lade-Änderung), nicht des aktuellen Codes auf diesem Branch. Trotzdem
sicherer: für die eigentliche Messung wurde `--warmup 300` gesetzt und je Lauf im `.log` gegen
`SceneLoadTiming` geprüft, dass es sicher vor `Profiler: start requested` liegt — bei beiden
Größen lädt die Szene binnen 6 Frames, `--warmup 300` lässt dafür reichlich Spielraum und hält die
Laufzeit klein (10–14 s je Lauf).

MD5 der generierten Szenen (für Schritt 2/3, falls dieselben Eingaben später erneut gebraucht
werden — Datei selbst nicht eingecheckt, siehe oben):

```
static_10000.hescene  ec67be5437e044b38b38830427537491
static_50000.hescene  c4797979144161b5b04373dbb46c30c3
```

Reproduktion der Testszenen (deterministisch über `--seed`, Default 1, hier nicht übersteuert;
die `.hescene`-Dateien selbst sind nicht eingecheckt — 5,9 MB / 27,9 MB, wie `raw-streaming/` es
für Thema 153/162 schon hält):

```
python scripts/perf/gen_reference_world.py --count 10000 --extent 8000 --groups 100 --lights 64 \
  --physics 0 --template docs/perf-audit/scenes/landscape_noclouds.hescene --out <tmp>/static_10000.hescene
python scripts/perf/gen_reference_world.py --count 50000 --extent 8000 --groups 500 --lights 64 \
  --physics 0 --template docs/perf-audit/scenes/landscape_noclouds.hescene --out <tmp>/static_50000.hescene
```

Beide luden fehlerfrei (`SceneLoadTiming: entities=10168`/`entities=50568`, keine `WARN`/`ERROR`
außer den aus jedem Capture-Lauf bekannten SFTP-/Gateway-Meldungen, geprüft in den `.log`-Dateien
des Laufs). Rohdaten: 12 `.summary.json` (6 Wiederholungen × 2 Größen) unter
`docs/perf-audit/raw-entity-cooking/` — die `.log`-Dateien sind wie überall im Repo per
`.gitignore` nicht eingecheckt (`*.profile.json` zusätzlich per lokalem `.gitignore`, wie
`raw-streaming/` es schon hält).

### 4.2 Ergebnis

Eigener Build (`C:/hwec1`, HEAD `3b2be965`, D3D12, NN-WS03, RTX 4070), 6 Wiederholungen je
Größe, `--warmup 300` (bestätigt je Lauf: `SceneLoadTiming` liegt bei Frame 2 bzw. 6, also weit
vor dem Profiler-Start bei Frame 300 — siehe §4.1), 600 aufgezeichnete Frames. Alle Zeiten unten
sind **Summe der Scope-Selbstzeit ÷ 600 Frames** (nicht ÷ Aufruf-Zahl — ein Scope, der mehrfach
pro Frame läuft, zählt mit allen seinen Aufrufen in die Frame-Summe), Median über die 6
Wiederholungen, Spannweite in Klammern. "Aufrufe/Frame" aus den rohen `cpuScopes`-Zähldaten
(nicht aus der 20-Zeilen-`topScopesBySelfTime`-Liste, die zu kurz ist, um das zu zeigen).

| Scope | Aufrufe/Frame | 10 168 Entities, ms/Frame | 50 568 Entities, ms/Frame |
|---|---|---|---|
| `Extract::meshes` | 1 | 0,226 (0,222–0,231) | 1,835 (1,776–1,899) |
| `SortObjects` | 1 | 0,094 | 0,611 |
| `RefineBounds` | 1 | 0,143 (0,139–0,145) | 0,448 (0,436–0,464) |
| `FrustumCuller::cull` | **2** (Schatten + Szene) | 0,109 | 0,113 |
| `Transforms::scan` | 1 | 0,0245 (0,0236–0,0249) | 0,088 (0,086–0,095) |
| `SceneSystemsTick` (alle `*System.cpp` aus §3 zusammen) | 1 | nicht separat erfasst (unter Top-20-Schwelle) | 0,0198 |
| `LOD`/`Weather`/`Movement`/`Animation`/`ParticleSystem`/`Foliage` einzeln | 1 je | — | je < 0,002 |
| `OutlinerPanel::render` (**Editor-only — in einem Spiel/Export nicht vorhanden**) | 1 | 0,195 | 0,545 |
| **CPU/Frame insgesamt (`cpuMs` p50)** | — | **1,395** (1,383–1,446) | **5,756** (5,671–6,281) |

Rohe `cpuScopes`-Summe aller 58 erfassten Scopes (rep1 je Größe, zur Kontrolle gegen die
`cpuMs`-Zeile): 1,66 ms/Frame bei 10k, 5,84 ms/Frame bei 50k — deckt die gemessene Frame-Zeit ab
(Scopes können sich in Sonderfällen minimal überlappen, daher keine exakte Identität erwartet).
Bei 50k ist `Extract::meshes` mit 1,835 ms **~31 % der Frame-Zeit** — der mit Abstand größte
Posten, gefolgt von `SortObjects` (~10 %) und `RefineBounds` (~8 %); `OutlinerPanel::render`
(~10 %) zählt nicht, weil es nur im Editor läuft. `FrustumCuller::cull` bleibt von 10k auf 50k
praktisch konstant (0,109→0,113 ms trotz 2 Aufrufen/Frame) — der Kostenanteil hängt hier
offenbar nicht an der Entity-/`RenderObject`-Zahl, sondern an etwas anderem Fixem (z. B.
Kaskaden-Setup); das ist eine Beobachtung, keine erklärte Ursache (nicht weiter gemessen).

**Wall-Zeit, nicht reine CPU-Zeit:** `Extract::meshes`, `RefineBounds` und die `ExtractFilter`-Phase
laufen über `parallel_for` (`RenderExtractor.cpp:447-467`, `FrameCull.h:77`) auf bis zu 32
Worker-Threads (`ThreadPool starting with 32 worker(s)`, Log). Die obigen Zahlen sind also
Zeit auf dem Hauptthread (wall), nicht Summe der CPU-Zeit aller Worker — ein Teil des
überlinearen Wachstums von `Extract::meshes` (8,1× bei 5× Entities: 5 Blöcke à 2048 bei 10k
gegen 25 Blöcke bei 50k) könnte an unterschiedlicher Worker-Auslastung liegen, nicht zwingend an
Cache-Effekten. **Nicht weiter zerlegt — als unmessene Vermutung markiert, keine gemessene
Ursache.** Die Pro-Entity-ns-Zahlen (22 ns bzw. 37 ns für `Extract::meshes`) sind entsprechend
Wandzeit pro Entity, keine CPU-Zeit pro Entity.

**Play-Mode-Update-Pfad nicht gemessen:** `he_perf_capture.py` hat keinen Play-Mode-Schalter, nur
den Editor-Loop. Die o.g. `SceneSystemsTick`/Einzelsystem-Zahlen sind trotzdem eine Messung
(nicht nur eine Annahme) dafür, dass diese Systeme für reine Mesh-Entities ohne die jeweilige
Komponente vernachlässigbar sind (§3) — `tickWorld` läuft auch im Edit-Modus, nur `tickAnimation`
mit Sequencer-spezifischen Teilen lief hier ggf. nicht identisch zum Play-Modus.

Referenz (Metal, anderes Gerät, Thema 162 §9.3/9.4, ruhende Welt, NACH Thema 162): Walk
18,7 ms / 73,1 ms bei 50 574 / 200 074 Entities, `Transforms::scan` 1,4 ms / 5,1 ms,
`RefineBounds` 3,5 ms, `FrustumCuller::cull` 3,2 ms (bei 50k). Nicht direkt vergleichbar (anderes
Gerät — ein RTX-4070-Windows-Rechner ist kein Vergleichsmaßstab für einen Mac —, anderer Backend,
andere Mittelungsmethode) — hier nur als Größenordnung zitiert, nicht als Ziel. Auffällig: die
absoluten Werte hier liegen ca. 10–80× niedriger als die zitierten Metal-Werte für dieselbe
Entity-Zahl; das ist plausibel (deutlich stärkere CPU/GPU, D3D12 statt Metal, andere Szene/
Kamera-Framing), aber nicht Grund genug, die Metal-Zahlen für ungültig zu halten — beide sind
echte Messungen auf unterschiedlicher Hardware.

## 5. Kochbarkeitsregeln (Vorschlag für Schritt 2)

Eine Entity ist **nicht** kochbar, wenn mindestens eine davon zutrifft:

1. Trägt `ScriptComponent` (`Components/ScriptComponent.h`) — Lua/HorizonCode kann jedes Feld
   jeden Frame ändern.
2. Trägt `AnimatorComponent`, `AnimationLayerComponent`, `AnimatorBlendComponent`,
   `AnimatorStateMachineComponent`, `RootMotionComponent` oder `IkComponent` — die Pose ändert
   sich jeden Frame.
3. Trägt `PropertyAnimatorComponent` oder `SequencePlayerComponent` — eine Animation-Clip-/
   Sequence-Wiedergabe schreibt Felder direkt.
4. Trägt `RigidBodyComponent` mit `type != RigidBodyType::Static` (`Dynamic` oder `Kinematic`,
   `Types/Enums.h:253-258`) — Jolt bewegt den Body.
5. Trägt `CharacterControllerComponent`, `NavAgentComponent`, `MovementComponent` oder
   `ParticleSystemComponent` — Bewegungs-/Partikel-Systeme schreiben Transform/State jeden Frame.
6. Trägt `NetworkComponent` mit `replicates == true` (nicht: trägt die Komponente — ein
   abgeschaltetes `NetworkComponent` ist laut seinem eigenen Kommentar harmlos, `replicates`
   ist "THE switch") oder `ReplicatedVarsComponent` mit deklarierten Variablen.
7. Trägt `SaveStateComponent` — `entity.applySavedState` kann jederzeit ein Savegame über die
   Entity schreiben, außerhalb von Skript/Engine-API im engeren Sinn.
8. Trägt `LODComponent` mit mehr als einem Level (`levels.size() > 1`) — `LODSystem::update`
   (`LODSystem.cpp:29`) schreibt `MeshComponent::meshAssetId` abhängig von der Kameradistanz jeden
   Frame neu. Ein `LODComponent` mit nur dem Basis-Level ist unkritisch.
9. Trägt `TrailComponent` oder `RopeComponent` — beide schreiben jeden Frame ihre eigene
   Geometrie neu (Trail: "every frame is the whole point"; Rope: ändert sich, sobald ein
   Attachment sich bewegt). Trägt die Entity selbst `JointComponent`, zählt sie ohnehin schon über
   Regel 4 nicht (ein Joint braucht einen `RigidBodyComponent`, der hier nicht `Static` sein darf,
   damit der Joint etwas bewegt) — die eigentliche Pointe für `JointComponent`/`RopeComponent`
   steht in §6: eine ANDERE, selbst unauffällige Entity kann als Ziel/Attachment referenziert
   werden, ohne diese Komponenten selbst zu tragen.
   `CameraRigComponent`-Ziele NICHT hier einsortieren — §6 zeigt, dass ein Rig sein Ziel nur
   liest, nie schreibt; ein reines CameraRig-Ziel bleibt kochbar. `SplineComponent` ebenfalls
   NICHT — laut eigenem Header "renders nothing and builds nothing", reine Autorendaten ohne
   Laufzeitschreiber, der hier gefunden wurde.
10. Ist unter einem Vorfahren, der eine dieser Regeln erfüllt, oder dessen `HierarchyComponent`-
    Parent sich je ändern kann (`HorizonWorld::reparentEntity`) — eine gekochte Entity mit
    beweglichem Vorfahren hätte eine falsche Weltmatrix, sobald der Vorfahr sich bewegt.
11. Ist Ziel eines `StreamingComponent`-gesteuerten Cell-Streamings, dessen Zelle entladen werden
    kann (§7) — die Entity wird zerstört/neu erzeugt, ein gekochter Block müsste an den
    Zell-Lebenszyklus gebunden sein (offene Frage für Schritt 3/5).
12. Trägt `InactiveComponent`, oder ihr `MeshComponent::visible` ist `false` — beides wird über
    `entity::setVisible` (§8) jederzeit umschaltbar geschrieben, und der Extract-Walk filtert
    genau danach (`RenderExtractor.cpp:463-464`, §3).

**Bewusst KEINE Regel 13 "könnte per Engine-API-Handle referenziert werden":** ob eine Entity
jemals per `entity::setPosition`, `physics::addForce` & Co. (die Liste in §8) geschrieben wird,
lässt sich beim Kochen nicht wissen — `entity::findByName` gibt jedem Skript jederzeit ein Handle
auf JEDE Entity, auch eine kochbar aussehende. Eine Ausschlussregel dafür würde entweder alles
oder nichts treffen. Richtig ist das Gegenteil: Schritt 4 hängt an jede schreibende Funktion aus
§8 einen "entkoche zuerst, falls `e` gekocht ist"-Schritt — eine Laufzeitprüfung bei jedem
API-Aufruf, keine Vorab-Ausschlussregel hier.

Kochbar (keine der obigen Regeln): nur `TransformComponent` + `MeshComponent` + optional
`MaterialComponent`, `ColliderComponent` + `RigidBodyComponent{type=Static}`,
`LODComponent` mit einem Level, `HierarchyComponent` (wenn Vorfahren ebenfalls kochbar sind),
`EntityIdComponent`/`NameComponent` (reine Metadaten, keine Laufzeitwirkung).

## 6. Querverweise: eine Entity ohne jede "ausschließende" Komponente kann trotzdem Ziel sein

Durchgängiges Muster in diesem Code: ein Querverweis auf eine ANDERE Entity geht nie über ein
rohes `entt::entity`-Handle (das überlebt laut Kommentar "weder ein Save noch eine
Prefab-Instanziierung noch eine Collaboration-Session"), sondern über `HE::UUID`, aufgelöst über
`EntityIdComponent`:

- `JointComponent::target` (`JointComponent.h:50-55`) — "the OTHER body, by EntityIdComponent
  UUID — the way RopeComponent names its attachments and CameraRigComponent its target."
- `RopeComponent`-Attachments (gleiches Muster, `RopeComponent.h`).
- `SequencePlayerComponent`/Sequence-Asset-Actor-Bindings — "the actors themselves are the
  sequence's bindings, resolved by EntityIdComponent UUID when playback starts"
  (`SequencePlayerComponent.h`).
- `CameraRigComponent::target` — **liest nur** die Zielposition (`pivot = targetWorldPosition +
  pivotOffset`), schreibt sie nicht. Eine Entity, die nur CameraRig-Ziel ist, bleibt kochbar.

Für Schritt 2/3 heißt das: eine Entity, die statisch aussieht (keine der Regeln in §5), aber als
`JointComponent::target` oder Rope-Attachment von einer ANDEREN, nicht-kochbaren Entity referenziert
wird, braucht entweder (a) einen Ausschluss über einen Rückindex ("wer verweist per UUID auf
mich") oder (b) eine gekochte Darstellung, die über `EntityIdComponent`-UUID weiterhin auflösbar
bleibt (ein Türrahmen, der ein `Fixed`-Joint-Anker ist, bewegt sich selbst nie — PhysicsWorld
braucht nur seine `RigidBodyComponent`/`ColliderComponent`-Daten, keine volle ECS-Iteration). (b)
ist der sauberere Weg, aber mehr Aufwand; (a) ist die sichere Standardregel für den ersten Wurf.
Für CameraRig-Ziele ist kein Ausschluss nötig (reine Lesebeziehung).

## 7. Cell-Streaming-Wechselwirkung (nur notiert, nicht gelöst)

`StreamingComponent.h` (`CellSplit.h`) entscheidet, ob eine Entity beim Zellwechsel zerstört und
neu erzeugt wird (`Auto`) oder immer geladen bleibt (`Resident`, Default). Eine gekochte Entity in
einer `Auto`-Zelle würde beim Entladen zusammen mit der Zelle verschwinden — das ist vermutlich
der richtige Lebenszyklus (Cooking pro Zelle, nicht pro Welt), aber das ist eine Entwurfsfrage für
Schritt 3 (Datenstruktur) und Schritt 5 (Export), nicht für diesen Schritt.

## 8. Engine-API-Handles, die eine gekochte Entity zurück in eine normale verwandeln müssen (Schritt 4)

Grep in `src/HE_Scene/src/EngineApi.cpp`, nur gelistet, nicht gelesen (Datei zu groß für
CLAUDE.md-Budget):

| Funktion | Zeile | Schreibt |
|---|---|---|
| `entity::setVisible` | 388 | `MeshComponent::visible` / Active-Status (siehe §5 Regel 12) |
| `entity::setPosition` | 683 | `TransformComponent` lokal |
| `entity::setRotation` | 705 | `TransformComponent` lokal |
| `entity::setWorldPosition` | 735 | `TransformComponent` (Welt→Lokal) |
| `physics::addForce`/`addImpulse`/`addTorque` | 853-855 | Jolt-Body (braucht Dynamic/Kinematic — Regel 4 deckt das meist schon ab) |
| `physics::setVelocity`/`setAngularVelocity` | 863, 865 | Jolt-Body |
| `physics::addJoint`/`removeJoint`/`setJointMotor`/`setJointBreakForce`/`setJointCollideConnected` | 905-944 | `JointComponent` bzw. Jolt-Constraint — siehe §6 |
| `destroyObject` / `spawnClass` | 324, 307 | Entity-Lebenszyklus (strukturell) |
| `findByName` | 347 | nichts, aber gibt jedem Skript ein Handle auf JEDE Entity (siehe §5, Absatz nach Regel 12) |
| `material::setParam` (UI) / `setMaterialParam` | 962, 1722 | `MaterialComponent`-Override |
| `saveState`/`applyState`/`applySavedState` | 624, 640, 657 | beliebige Felder über `SaveStateComponent` |

Jede dieser Funktionen braucht in Schritt 4 einen "entkoche zuerst"-Hook, falls `e` eine gekochte
Entity ist (oder die Funktion lehnt gekochte Entities ab, bis entkocht wurde — Entwurfsfrage für
Schritt 4, hier nur die Liste der Eintrittspunkte).

## 9. Nicht Teil dieses Themas (zur Erinnerung, aus der Aufgabenbeschreibung)

GPU-seitige Änderungen, Auto-LOD-Generierung beim Cook (siehe §5 Regel 8 — `LODSystem` selektiert
bereits zur Laufzeit, aber generiert keine LOD-Kette; das bliebe getrennt), Foliage (eigenes Thema,
`FoliageComponent`/`FoliageSystem.cpp` laufen schon über ein einzelnes Component pro Terrain, nicht
pro Instanz, und sind von diesem Walk nicht betroffen), Editor-Verhalten editierbarer Entities.

## 10. Nächste Schritte (für Schritt 2)

1. Ein Mobility-/Cookable-Flag auf der Entity definieren (vermutlich ein Tag-Component statt ein
   Feld auf `TransformComponent`, analog zu `InactiveComponent` — kein bestehendes Feld, siehe §2).
2. Die Regeln aus §5 als eine Prüfung implementieren, die bei Szenen-Load / Export einmal über
   alle Entities läuft (gleiche Walk-Kosten wie heute, aber einmalig statt pro Frame).
3. Den Rückindex aus §6 (wer referenziert mich per UUID aus einem nicht-kochbaren Feld) bauen,
   bevor Entities tatsächlich eingefroren werden — sonst bricht ein Joint/eine Rope beim
   nächsten Zugriff.
4. Erst danach (Schritt 3) die gekochte Datenstruktur selbst und den Walk, der sie ausliest statt
   `extractMeshes`/`Transforms::scan` für diese Entities aufzurufen.
5. Schritt 4 (Entkochen) braucht die Liste aus §8 als Eintrittspunkte, plus Reparenting
   (`HorizonWorld::reparentEntity`) und Cell-Streaming-Zerstörung (§7) als weitere Trigger.

## Handoff

- **Topic / step:** Thema 343, Schritt 1 von 5 (Bestandsaufnahme/Baseline), Device NN-WS03.
- **Branch / commit:** `claude/entity-cooking-statische-entities-auf-daten-reduzieren` @ `3b2be965`
  (Stand zu Schrittbeginn; dieser Schritt fügt nur `docs/entity-cooking-survey-2026-11-13.md` +
  `docs/perf-audit/raw-entity-cooking/` hinzu, kein Produktivcode), gepusht: siehe unten.
- **Done:** Grep nach Static/Mobility-Flag (keiner), Hot-Path-Survey (§3) mit file:line, 51
  Component-Header unter `src/HE_Scene/include/HorizonScene/Components/` gegen Laufzeit-Mutation
  gelesen, Kochbarkeitsregeln (§5), Querverweis-Muster UUID-über-EntityIdComponent (§6),
  Engine-API-Eintrittspunkte (§8) per Grep gelistet, eigener Editor-Build gebaut und zwei
  Mess-Szenen (10k/50k statische Mesh-Entities) je 6× erfasst (§4).
- **Files touched:** `docs/entity-cooking-survey-2026-11-13.md` (dieses Dokument),
  `docs/perf-audit/raw-entity-cooking/*.summary.json` (Rohdaten, 12 Dateien) — keine Quelldatei
  geändert.
- **Verified:** `TransformHierarchy.cpp`/`RenderExtractor.cpp`/`FrameCull.h`/`FrustumCuller.cpp`/
  `D3D12Renderer.cpp:11501`-Zeilen durch Lesen bestätigt; `LODSystem.cpp:29` und
  `HorizonWorld.cpp:544` gelesen und als NICHT per-Frame-kostend für reine Mesh-Entities bestätigt;
  `JointComponent`/`RopeComponent`/`SequencePlayerComponent`-UUID-Muster durch Lesen der
  jeweiligen Header-Kommentare bestätigt; alle §8-Zeilennummern per direktem
  `grep -n "^void setPosition(Ctx" …` einzeln nachgeschlagen (nicht aus einem versetzten
  `awk`-Ausschnitt übernommen — das hatte beim ersten Entwurf alle Zeilen um 246 verschoben);
  §4.2-Zahlen aus den rohen `cpuScopes` (58 Scopes, nicht nur die 20 in `topScopesBySelfTime`)
  über 6 Wiederholungen je Größe, Frame-Summe statt Aufruf-Mittel; Summe aller Scopes (1,66 ms /
  5,84 ms) gegen `cpuMs` p50 (1,395 ms / 5,756 ms) als Plausibilitätskontrolle; der Async-Lade-Fehler
  (§4.1) an `C:/hwec1` mit `--warmup 0` gegengeprüft (lädt bei Frame 6, keine Verzerrung).
- **Not verified:** Play-Mode-Update-Pfad (kein Play-Mode-Schalter in `he_perf_capture.py`,
  siehe §4.2); Vulkan/D3D11/OpenGL auf diesem Gerät nicht gemessen (nur D3D12, RTX 4070); 100k/
  200k-Stufen nicht gemessen (nur 10k/50k, aus Zeitgründen); der genaue Grund für das überlineare
  Wachstum von `Extract::meshes` (Block-Parallelisierung vs. Cache — §4.2) nicht weiter zerlegt.
- **Decisions taken:** eigenen Editor aus HEAD gebaut statt eines Fremd-Deploys (Begründung §4.1,
  Gegenprüfung bestätigt: die Falle lag am fremden Deploy, nicht am Engine-Code); `--physics 0`
  für die Testszenen; Kochbarkeitsregeln als "ausschließen, bis Gegenteil bewiesen" formuliert,
  nicht als Positivliste, weil Querverweise (§6) sich nicht an der Entity selbst ablesen lassen;
  Engine-API-Referenzierbarkeit bewusst NICHT als Kochzeit-Regel aufgenommen (§5, nach Regel 12) —
  das lässt sich vorab nicht entscheiden, nur bei jedem API-Aufruf zur Laufzeit (Schritt 4).
- **Decisions suggested, not taken:** ob Joint-/Rope-Ziele einen Rückindex (Ausschluss, §6a) oder
  eine UUID-auflösbare gekochte Darstellung (§6b) bekommen — Entwurfsfrage für die Queen/Schritt 2;
  dieses Dokument empfiehlt (a) als sicheren ersten Wurf.
- **Pending:** Schritt 2 (Kochbarkeits-Markierung, §10) ist der nächste; dieser Schritt selbst hat
  keine offenen Teilaufgaben.
- **Traps:** Scene-Load läuft (zumindest bei einem fremden, divergenten Deploy) asynchron zur
  Frame-Schleife — ohne ausreichend `--warmup` verzerrt ein Mix aus leeren und geladenen Frames im
  selben Capture-Fenster die Zahlen um bis zu 6× (§4.1). Immer `SceneLoadTiming` im `.log` gegen
  den `Profiler: start requested`-Zeitstempel prüfen, bevor eine Zahl übernommen wird.
  `topScopesBySelfTime` im `.summary.json` ist auf 20 Einträge gekappt — für Scopes weiter unten
  (z. B. `SceneSystemsTick`, `LOD`) die vollen `summary.cpuScopes` aus dem (nicht eingecheckten)
  `.profile.json` lesen. Ein Scope-`count` ist die Aufrufzahl, nicht die Frame-Zahl — `selfMs`
  durch `count` teilt "pro Aufruf", nicht "pro Frame" (ein Scope wie `FrustumCuller::cull` läuft
  2×/Frame). Fremde Worktree-Deploys auf diesem Gerät (`C:/hw21`, weitere unter
  `C:\ProgramData\Hive\w\...`) können von main abweichen — per
  `diff src/HE_Scene/src/<Datei>.cpp <Fremd-Deploy-Quelle>` gegen den eigenen Branch prüfen, bevor
  ihr Binary für eine Messung verwendet wird.
- **Next steps:** §10 dieses Dokuments — Schritt 2 (Kochbarkeits-Markierung) beginnen.
