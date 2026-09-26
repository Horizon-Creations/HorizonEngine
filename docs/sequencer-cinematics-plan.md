# Sequencer / Cinematics: Entwurf

Hive-Thema 84, Schritt 1. Entwurf und Fahrplan, noch kein Code.

Die Beschreibung am Brett geht davon aus, dass ein Sequencer fehlt. **Das stimmt so nicht
mehr:** Seit Thema 36 (main `88bd6b97`, 14.09.2026) gibt es einen Sequencer, allerdings nur als
Editor für **einen** `PropertyAnimClip`, den eine Entity selbst abspielt. Was fehlt, ist die
Cinematics-Schicht darüber: eine Zeitleiste, die **mehrere** Entities zugleich treibt, Kameras
schneidet und blendet, Skelett-Animationen, Ereignisse und Ton setzt, und die im gebauten Spiel
genauso läuft wie in der Vorschau. Um diese Schicht geht es hier.

---

## 1. Stand: was es schon gibt

| Teil | Ort | Was es kann | Brauchbar für Cinematics |
|---|---|---|---|
| `PropertyAnimClipAsset` | `src/HE_Core/include/ContentManager/Assets.h:695`, Chunk `CHUNK_PANM` (`HAsset.h:185`), Laden/Speichern `ContentManager.cpp:596`/`:1802` | Skalare Kanäle (`PropertyAnimChannel`: `times`/`values`) für 15 Ziele (`PropTarget`, `Assets.h:678`): Position/Rotation/Skalierung XYZ, Materialfarbe/-metallic/-roughness/-opacity | Ja, als Datenform der Property-Spur. Kein FOV, keine Sichtbarkeit |
| `PropertyAnimationSystem` | `src/HE_Scene/include/HorizonScene/PropertyAnimationSystem.h`, `src/HE_Scene/src/PropertyAnimationSystem.cpp` | `sampleChannel` (linear, an den Enden gehalten), `applyAt(world, cm, e, clip, t)` schreibt alle Kanäle in Transform/Material, `advance` ist die Abspielkopf-Regel | Ja. `applyAt` ist genau die Funktion „Zustand zur Zeit t“, die Vorschau und Laufzeit teilen müssen |
| `PropertyAnimatorComponent` | `Components/PropertyAnimatorComponent.h` | `clipId`, Zeit, Tempo, Loop, Playing, pro Entity | Nein. Eine Entity spielt sich selbst ab, es gibt keinen gemeinsamen Takt über Entities hinweg |
| Sequencer (Thema 36) | `src/HE_Editor/SequencerPanel.{h,cpp}` (Tab, Werkzeugleiste, Undo, Speichern, Akteure), `src/HE_Editor/SequencerTimeline.{h,cpp}` (Spurliste, Lineal, Scrubbing, Keys, Kurvenansicht, ohne `AppContext`) | Keys setzen, verschieben, löschen mit Sortier-Invariante; Kurvenansicht; Vorschau über `applyAt` an allen Entities, die den Clip abspielen | Die Streifen-Logik (`insertKey`/`moveKey`/`removeKey`, Lineal, Scrub) ja. Das Panel selbst ist auf einen Clip und skalare Spuren zugeschnitten |
| Zeitachsen-Mathe | `src/HE_Editor/UITimelineMath.h:41` (`UITimelineView`) | Sekunden zu Pixeln, 1-2-5-Lineal, Mausrad-Zoom; gemeinsam mit der Animationsleiste des UI-Designers | Ja, unverändert |
| Headless-Tests der Zeitleiste | `tests/test_sequencer_timeline.cpp` (14 `TEST_CASE`) | Treiben den Streifen über einen Clip im Speicher, ohne GPU | Ja, als Muster für den neuen Streifen |
| Kamera-Blend | `CameraRigController::blendTo` (`CameraRigController.h:112`, Impl. `CameraRigController.cpp:580`), `BlendCurve` + `applyBlendCurve` (`CameraPose.h`) | Übergabe an eine Kamera über n Sekunden mit Linear/SmoothStep/EaseOut; setzt `isMain` eindeutig | **Nur halb.** Siehe §2.2: in eine Kamera ohne Rig wird geschnitten, nicht geblendet |
| Posen-Schnappschuss | `snapshotCameraPose` (`CameraRigController.cpp:254`) | Friert die Weltpose einer Kamera ohne Rig ein, damit ein Blend von ihr aus starten kann | Ja: Rückblende von der Cutscene-Kamera zum Spieler-Rig funktioniert heute schon |
| Animation-Notifies | `HE::collectNotifies` (`AnimationNotify.h`, Impl. `AnimationNotify.cpp:86`), `NotifyQueue`, `AnimationNotifySystem::dispatch` | Feuerregel über die Spanne (tPrev, tEnd], Fire/Begin/End, Rückwärtslauf gespiegelt; Zustellung an Lua/Python, HorizonCode-Klasse und Sync-Graph | Die Regel ja, aber sie hängt an `AnimationClipAsset`. Die Zustellung ja, unverändert |
| Skelett-Pose zu einer Zeit | `AnimationPreview::evaluateClipPose` (`AnimationPreview.h:19`) | Pose eines Clips zur Zeit t ohne ECS | Ja, als Grundlage einer Skelett-Spur |
| Stabile Entity-Identität | `EntityIdComponent` (UUID, beim Erzeugen vergeben, beim Laden wiederhergestellt), `HorizonWorld::findByEntityId` (`HorizonWorld.h:33`) | Überlebt Speichern/Laden und Git-Merges | Ja, damit bindet die Sequenz ihre Akteure |
| Takt im Frame | `SceneSystems::tickAnimation` (`SceneSystems.cpp:197`), gerufen in `GameApplication.cpp:3032` und im Editor | Animation nach Gameplay und Welt, vor der Extraktion; `PropertyAnimation` ist der letzte Treiber (`:218`) | Ja, hier hängt sich das Sequenz-System an |
| Skript-API | `camera.blendTo`, `audio.play`/`playAt`, `time.setTimeScale`, `input.setModeGameOnly` (Registry in `EngineApi.cpp`) | Heute der einzige Weg zu einer Cutscene: alles von Hand per Skript | Werden intern wiederverwendet, nicht dupliziert |

Das Bereitschafts-Audit (`docs/game-readiness-audit-2026-08-27.md:633`) fasst es in einem Satz:
„die Bausteine sind alle da … Was fehlt, ist ausschließlich das Autorieren.“ Das stimmt für den
Laufzeitteil nur zur Hälfte, siehe §2.

---

## 2. Lücken

### 2.1 Kein Asset für mehrere Akteure

Ein `PropertyAnimClip` kennt kein Ziel: er wird von der Entity abgespielt, auf der der Animator
sitzt. Eine Cutscene braucht „Kamera A fährt, Figur B läuft, Tür C geht auf“ auf **einer** Uhr.
Das ist ein anderes Asset, keine Erweiterung (§3.2).

### 2.2 Kamera-Blend in eine Kamera ohne Rig ist ein Schnitt

`CameraRigController.cpp:603`: `if (!rig) return true; // switched; a camera without a rig cannot
blend`. Eine Cutscene-Kamera ist per Keyframes gefahren und hat kein Rig. Also:

- **Rein in die Cutscene** (Spieler-Rig zu Cutscene-Kamera): heute harter Schnitt.
- **Zwischen zwei Cutscene-Kameras**: heute harter Schnitt.
- **Raus aus der Cutscene** (Cutscene-Kamera zum Spieler-Rig): funktioniert, weil das Ziel ein Rig
  hat und die Quelle über `snapshotCameraPose` eingefroren wird.

Folge für den Entwurf: Blends, deren Ziel eine Sequenz-Kamera ist, rechnet die Sequenz selbst
(§3.4). Das ist sogar besser als ein Umbau von `blendTo`, weil beide Posen reine Funktionen der
Sequenzzeit sind. Damit ist der Blend scrubbar und in der Vorschau exakt das, was das Spiel zeigt.

### 2.3 Wer steuert die Kamera?

`GameApplication::updateCameraController` (`GameApplication.cpp:2290`, Aufruf `:2945`): Kann das
Rig nicht treiben, übernimmt `FlyCameraController::update` (`:2328`) und bewegt **die
`isMain`-Kamera** per Maus und Tastatur. Eine Cutscene-Kamera ohne Rig, die `isMain` ist, würde
der Spieler also während der Cutscene wegdrehen. Im Editor-PIE gilt dasselbe
(`EditorApplication.cpp:8925`, Fly-Fallback `:9012`). `input.setModeGameOnly` hilft dagegen nicht,
das steuert nur, ob die UI Eingaben bekommt. Nötig ist eine Abfrage „besitzt gerade eine Sequenz
die Kamera?“, die beide Anwendungen vor dem Kamera-Controller stellen.

Dazu die Reihenfolge im Frame: `updateCameraController` läuft **vor** `tickWorld`/`tickAnimation`.
Setzt die Sequenz Schnitt und Pose erst in `tickAnimation`, ist das für die Extraktion desselben
Frames rechtzeitig: die kommt danach und ruft selbst `propagateTransforms`
(`RenderExtractor.cpp:63`), die Weltmatrix der Kamera ist also frisch. Wer innerhalb des Systems
eine Weltposition braucht (Blend-Quelle), nimmt `HE::worldPositionOf` bzw. propagiert selbst wie
`CameraRigController::update`, nicht `worldMatrix`. Nur ein Kamera-Controller, der im nächsten Frame davor
läuft, darf nichts zurückschreiben, und genau das verhindert die Abfrage.

### 2.4 Skelett-Clips lassen sich nicht auf eine Zeit setzen

`AnimationSystem.cpp:25`: `if (!animator.playing) continue;`. Der Trick der heutigen
Sequencer-Vorschau (Abspielzeit setzen, `applyAt` rufen) überträgt sich nicht auf Skelett-Clips:
ein angehaltener Animator wird gar nicht ausgewertet, ein laufender läuft auf seiner eigenen Uhr
weiter. Eine Skelett-Spur braucht einen eigenen Auswertungsweg (§3.3).

### 2.5 Die Feuerregel der Notifies hängt an `AnimationClipAsset`

`HE::collectNotifies(const AnimationClipAsset&, …)`. Die Regel selbst (offener Ursprung,
geschlossene Nähte, Rückwärtslauf gespiegelt, `includeStart` für den ersten Frame) ist genau die,
die eine Ereignisspur braucht. Sie muss aus der Signatur gelöst werden: der Spannen-Lauf über eine
Liste von `{name, time, duration}`, `collectNotifies` ruft ihn dann nur noch auf.

### 2.6 Die heutige Vorschau schreibt in die Szene und räumt nicht auf

`SequencerPanel.cpp:439` (`previewActors`) schreibt bei jedem Frame in die Transforms der
Edit-Welt, Material-Spuren sogar in das **gemeinsame** `MaterialAsset`
(`PropertyAnimationSystem.cpp`, Material-Zweig). Nach dem Scrubben steht die Szene woanders als
vorher, und alle Entities mit demselben Material haben die Farbe mitbekommen. Für einen Clip auf
einer Plattform ist das verschmerzbar, für eine Cutscene mit zehn Akteuren und einer Kamera nicht:
die Szene wäre nach jeder Vorschau verstellt. Die Cinematics-Vorschau braucht eine
Vorschau-Sitzung mit Sichern und Zurückschreiben (§3.6).

### 2.7 Kein FOV, keine Sichtbarkeit als Spurziel

`PropTarget` endet bei `MatOpacity`. Für Kamerafahrten fehlt mindestens FOV, für Cutscenes
Sichtbarkeit (Akteur ein-/ausblenden).

---

## 3. Entwurf

### 3.1 Umfang: Editor-Vorschau UND Laufzeit

**Empfehlung: beides, von Anfang an über dieselbe Auswertungsfunktion.** Eine Cutscene, die nur
im Editor läuft, ist keine; das Audit nennt Cutscenes ausdrücklich als Spielbaustein. Die Regel,
die der vorhandene Sequencer schon für einen Clip einhält, gilt für die ganze Sequenz:

> `SequenceEval::evaluate(seq, bindings, t)` ist die einzige Stelle, die aus einer Zeit einen
> Weltzustand macht. Laufzeit und Vorschau rufen sie beide. Was beim Scrubben im Viewport steht,
> ist damit per Konstruktion das, was das Spiel zeigt.

Nur zwei Dinge sind nicht zeitpunkt-rein und laufen deshalb **nur beim Abspielen**, nicht beim
Scrubben: Ereignisse (feuern über die Spanne tPrev..tEnd) und Ton (wird gestartet, nicht
gesampelt). Beim Scrubben feuert nichts und klingt nichts; das ist in jeder großen Engine so und
verhindert, dass ein Hin-und-her-Ziehen zwanzig Explosionen auslöst.

### 3.2 Datenformat: eigener Assettyp `Sequence`

**Empfehlung: neuer `AssetType::Sequence`, kein Ausbau von `PropertyAnimClip`.** Der Clip bleibt,
was er ist (eine Entity, die sich selbst im Kreis bewegt: Plattform, pulsierendes Material). Eine
Sequenz hat Bindungen und verschiedene Spurarten, das passt nicht in `CHUNK_PANM`.

Ein `.hasset` mit einem Chunk `CHUNK_SEQU`. **Geändert in Schritt 2: JSON statt Binärform.**
Die Referenzsuche (`AssetRefScan.cpp`) und das Umbenennen (`AssetRefRetarget.cpp`) sehen eine
UUID nur als `{"hi":…,"lo":…}` in einem JSON-Chunk; sechzehn rohe Bytes in einem Binär-Chunk sind
für beide unsichtbar, und der Lösch-Dialog hätte einen Clip, den eine Cutscene abspielt, als
„unreferenziert“ gemeldet. Alle Autoren-Assets seit `PANM` (BLSP, BMSK, THEM, ASMG, SGTP) sind
aus demselben Grund JSON. Das Asset hält trotzdem die **geparste** Form (nicht den Text wie
`BlendSpaceAsset`), weil die Laufzeit jedes Frame auswertet. Umgesetzt in
`Sequence/SequenceJson.{h,cpp}`, Datenmodell `SequenceAsset` in `Assets.h`. Die Struktur:

```
SequenceAsset
  duration         float
  frameRate        float        // nur fürs Einrasten im Editor, Auswertung ist kontinuierlich
  bindings[]       { slot: u16, name: string, entityId: UUID }
  tracks[]         { kind: u8, binding: u16 (0xFFFF = keine), payload }
      kind = Property   payload = PropertyAnimChannel (unverändert wiederverwendet)
      kind = Skeletal   payload = sections[] { clipId, start, end, clipOffset, playRate, loop }
      kind = CameraCut  payload = cuts[]     { time, binding, blendIn, BlendCurve }
      kind = Event      payload = events[]   { name, time, duration }   // = AnimationNotify
      kind = Audio      payload = sections[] { assetId, start, volume, pitch, binding? }
```

- **Bindung über `EntityIdComponent`-UUID**, aufgelöst mit `HorizonWorld::findByEntityId`, nie
  über entt-Handles. Der `name` ist nur Anzeige und Fallback-Hinweis im Editor („Akteur fehlt:
  Tür_Nord“), die Auflösung läuft nie über den Namen.
- **Überschreibungen pro Abspieler**: Eine Sequenz, die „den Spieler“ braucht, kennt dessen UUID
  nicht (der Charakter wird zur Laufzeit gespawnt). Deshalb trägt die abspielende Komponente eine
  Liste `slot → Entity` (§3.5), die Vorrang vor der UUID im Asset hat, und die Skript-API kann
  einen Slot vor dem Start belegen.
- **Spurziele**: `PropTarget` wird um `CameraFov` und `Visible` erweitert (hinten angehängt, alte
  Dateien bleiben gültig, der alte Sequencer bekommt sie gleich mit). `Visible` ist ein Schalter,
  kein Wert: linear gesampelt stünde er zwischen zwei Keys auf 0,5. Regel für dieses Ziel: Stufe,
  der Wert des letzten Keys vor t gilt (und der Streifen zeichnet ihn als Stufe).

Ein neuer Assettyp muss an **allen** diesen Stellen eingetragen werden. Das Audit hat
`PropertyAnimClip` einmal als „toten Assettyp“ gefunden, weil genau zwei davon fehlten:

1. `AssetType` in `src/HE_Core/include/Types/Enums.h` (Enum, beide Namens-Switches)
2. Beide Dispatch-Switches in `ContentManager.cpp` (Laden neben `:596`, Speichern neben `:1802`), Getter/Register/Acquire in `ContentManager.h`
3. `AssetStubWriter.cpp` (Anlegen aus dem Content Browser, Stub = leere Sequenz)
4. Content Browser: Anlegen-Menü, Symbol (`EditorApplication.h`, neben `m_iconPropertyAnimClip`), Doppelklick-/Tab-Dispatch
5. `SceneSystems::collectAssetRefs` (`SceneSystems.cpp:244`): die Sequenz der Abspielkomponente **und** die Assets in der Sequenz (Skelett-Clips, Ton). Gepackt wird ohnehin alles: `HpakWriter` läuft rekursiv über die ganzen Content-Wurzeln (`HpakWriter.cpp:943`). `collectAssetRefs` entscheidet aber, was beim Szenenstart vorab gestreamt wird (`GameApplication::streamSceneAssets`, `GameApplication.cpp:1430`). Ein Clip, der nur in der Sequenz steht, wäre beim ersten Auswerten noch nicht geladen, und die Spur würde still nichts tun. Also entweder `collectAssetRefs` schaut in die geladene Sequenz hinein, oder das System lädt beim Start nach (`loadAssetAsync`) und wertet erst aus, wenn alles da ist. Dieselbe Lücke hat heute schon der State-Machine-Asset (`AnimatorStateMachineComponent` wird eingetragen, die Clips darin nicht); Schritt 2 prüft das dort mit
6. `AssetRefScan` (UUID-Form im Chunk) und Rename-Retarget, sonst meldet der Lösch-Dialog „unreferenziert“
7. MCP: mindestens Lesen/Schreiben der Sequenz (Muster `McpToolsClip.cpp`)

### 3.3 Spurarten in v1

| Spur | Auswertung zur Zeit t | Baustein |
|---|---|---|
| **Property** (Transform, Material, FOV, Sichtbarkeit) | `sampleChannel` pro Kanal, Schreiben wie `applyAt` | `PropertyAnimChannel`, `PropertyAnimationSystem`. `applyAt` wird in „ein Kanal auf eine Entity“ zerlegt, damit Clip und Sequenz dieselbe Schreibfunktion benutzen |
| **Skeletal** | Aktive Sektion finden, Clipzeit = `(t - start) * playRate + clipOffset` (geloopt oder geklemmt), Pose über den Weg von `AnimationPreview::evaluateClipPose` direkt in `SkeletalMeshComponent::boneMatrices` | Eigener Auswertungsweg wegen §2.4. Läuft als **letzter** Skelett-Treiber im Frame, gewinnt also gegen den Animator der Entity. Offen für Schritt 3: `tickAnimation` klammert die Treiber mit `poseBeginFrame`/`poseEndFrame` (Layer-Stapel, IK, `PoseFinalize.h`). Die Sequenz muss dort als Basis-Treiber zählen, sonst laufen Layer und IK auf ihrer Pose nicht oder doppelt. Überblenden zwischen zwei Sektionen kommt später |
| **Camera Cut** | Letzter Schnitt vor t bestimmt die Kamera; liegt t innerhalb `blendIn` nach dem Schnitt, Pose = Slerp/Lerp zwischen der Pose der vorigen Kamera zur Zeit t und der neuen, geformt mit `applyBlendCurve` | `BlendCurve`, `applyBlendCurve`, `SolvedPose`. Erster Schnitt mit `blendIn > 0`: Quelle ist die beim Start eingefrorene Gameplay-Kamerapose (`snapshotCameraPose`-Muster) |
| **Event** | Nur beim Abspielen: Spannen-Lauf über (tPrev, tEnd] | Aus `collectNotifies` gelöste Regel (§2.5). Zustellung über `NotifyQueue` + `AnimationNotifySystem::dispatch` an die gebundene Entity (ohne Bindung: an die abspielende Entity). Damit bekommen Lua, Python, HorizonCode und Sync-Graph das Ereignis ohne ein einziges neues Handler-API |
| **Audio** | Nur beim Abspielen: Sektion starten, wenn die Spanne ihren Start überquert; beim Stopp/Abbruch stoppen | `audio.play` / `audio.playAt` (an der gebundenen Entity). Springt die Sequenz mitten in eine Sektion (`setTime`), startet der Ton **nicht** mittendrin: das braucht Seek, und Seek (wie auch die Länge) liefert miniaudio nur im Pull-Modus des Decoders, nicht auf dem heutigen Abspielweg |

### 3.4 Kamera: Besitz, Schnitt, Blend

- Die Sequenz **besitzt die Kamera**, solange sie läuft und eine Camera-Cut-Spur hat. Abfrage
  `SequenceSystem::ownsCamera(reg)`; `GameApplication::updateCameraController` und
  `EditorApplication::updatePlayCameraController` fragen sie als Erstes und lassen Rig und Fly
  dann in Ruhe.
- Ein Schnitt setzt `isMain` eindeutig (dieselbe Pflicht, die `blendTo` hat; am besten als kleine
  gemeinsame Hilfsfunktion herausgezogen) und schreibt die ausgewertete Pose.
- **Rein und zwischen Cutscene-Kameras** blendet die Sequenz selbst (§2.2).
- **Raus**: am Ende (oder beim Abbruch) `CameraRigController::blendTo(Spieler-Rig, blendOut,
  curve)`. Das funktioniert heute schon, weil die Quelle ohne Rig eingefroren wird.
  `blendOut` ist ein Feld der Abspielkomponente, kein Teil des Assets, weil dieselbe Sequenz in
  einem Menü-Level ohne Spieler-Rig gar kein Ziel hat.

### 3.5 Laufzeit: Komponente, System, Takt, API

```
SequencePlayerComponent
  sequenceId        UUID
  autoplay          bool     // beim Szenenstart
  loop              bool
  playRate          float
  blendOutSeconds   float, blendOutCurve BlendCurve
  lockPlayerInput   bool     // Charaktersteuerung aus, solange die Sequenz läuft
  bindingOverrides  [{ slot, entity }]   // Laufzeit, nicht serialisiert; siehe §3.2
  // Laufzeitzustand: time, prevTime, playing, firstFrame (für includeStart)
```

- **`SequenceSystem::update`** läuft am **Ende** von `SceneSystems::tickAnimation`, nach
  `PropertyAnimation`: Die Sequenz gewinnt gegen jeden anderen Treiber auf derselben Entity, und
  ihre Ereignisse landen in derselben `NotifyQueue`, die direkt danach zugestellt wird
  (`GameApplication.cpp:3039`, Editor `EditorApplication.cpp:3368`). Keine neue Stelle in den
  beiden Anwendungen außer der Kamera-Abfrage aus §3.4.
- **Zeit**: Spiel-dt (`gameDt`), also wirkt `time.setTimeScale` und die Pause. Ein Schalter
  „ungeskalierte Zeit“ ist denkbar, aber nicht v1.
- **Eingabe**: `lockPlayerInput` sperrt die Steuerung des besessenen Charakters über
  `PlayerHost`; wie genau (Controller-Tick aussetzen oder Eingabe filtern), klärt Schritt 4 am Code.
- **Skript-API** (Registry-Zeilen, alle vier Frontends auf einmal): `sequence.play(entity)`,
  `stop`, `pause`, `setTime`, `getTime`, `isPlaying`, `bindSlot(entity, slot, target)`. Ende einer
  Sequenz: ein reserviertes Ereignis `SequenceFinished` an die abspielende Entity über denselben
  Notify-Weg, damit kein neuer Handler-Typ nötig ist. Jede neue Zeile braucht die drei Stellen
  aus der Registry-Regel (Display-Name-Map, `HcNodeDocs`, `isScriptGroup`) und die Parity-Fixture.
- **Serialisierung** der Komponente im `SceneSerializer`, Inspector-Abschnitt, Handbuch-Eintrag.

### 3.6 Editor: Timeline-UI und Vorschau

**Empfehlung: eigener Tab „Cinematic“ neben dem vorhandenen Sequencer, der denselben Streifen
benutzt.** Der Name „Sequencer“ ist vergeben und bleibt dem Clip-Editor. Zwei getrennte Tabs, weil
es zwei getrennte Assets sind; der Umbau sitzt im Streifen, nicht im Panel.

- **Streifen verallgemeinern**: `SequencerTimeline` zeichnet heute eine Zeile pro skalarem Kanal.
  Er bekommt Zeilenarten: Key-Zeile (wie heute, für Property-Spuren), Sektionszeile (Balken mit
  Anfang/Ende ziehen, für Skeletal und Audio), Markenzeile (Punkte mit Namen, für Event und Cut).
  Zeilen sind nach Bindungen gruppiert und einklappbar, oben die Camera-Cut-Zeile. Lineal, Scrub,
  Zoom und die Key-Funktionen bleiben, wie sie sind. Weiter **ohne `AppContext`**, damit die
  Headless-Tests nach dem Muster von `test_sequencer_timeline.cpp` den Streifen treiben können.
- **Bindungen**: „Ausgewählte Entities binden“ (liest deren `EntityIdComponent`), fehlende Akteure
  rot mit gespeichertem Namen, „neu binden“.
- **Vorschau-Sitzung** (§2.6): Beim ersten Scrub/Play werden von jeder gebundenen Entity die
  Komponenten gesichert, die die Sequenz schreibt (Transform, Material-Werte, Sichtbarkeit,
  Kamera-`isMain`/FOV, Knochenmatrizen), beim Stopp, Tab-Wechsel, Schließen, Speichern und vor
  jedem Undo-Snapshot zurückgeschrieben. Die Szene wird durch Vorschau nie „dirty“. Material-Spuren
  schreiben in der Vorschau in eine Instanz-Überschreibung statt ins gemeinsame Asset, falls es die
  gibt; sonst in das Asset mit Sicherung wie oben. Das behebt nebenbei auch den alten Sequencer.
- **Durch die Kamera schauen**: Der Viewport kann heute nicht durch eine Szenenkamera rendern
  (`editor-basics-inventory-2026-09-24.md` nennt nur Frustum-Linien). Ein Schalter „Vorschau
  durch Schnittkamera“ ist für Cinematics Pflicht und ein eigener Schritt.

---

## 4. Zu entscheiden (mit Empfehlung)

| Frage | Empfehlung | Warum |
|---|---|---|
| Nur Editor-Vorschau oder auch Laufzeit? | Beides, eine Auswertungsfunktion | Cutscenes sind ein Spielbaustein; getrennte Pfade driften |
| Eigenes Asset oder `PropertyAnimClip` ausbauen? | Eigenes `Sequence` | Mehrere Akteure und Spurarten; der Clip bleibt für Selbstläufer |
| Bindung? | `EntityIdComponent`-UUID + Slot-Überschreibung zur Laufzeit | Überlebt Speichern und Merge; gespawnte Akteure (Spieler) brauchen den Slot |
| Blend in eine Cutscene-Kamera? | Rechnet die Sequenz selbst | `blendTo` schneidet dorthin; Sequenz-Blend ist scrubbar |
| Ereignisse zustellen? | Über `NotifyQueue`/`AnimationNotifySystem` | Kein neues Handler-API in vier Frontends |
| Eigener Tab oder alter Sequencer-Tab? | Eigener Tab „Cinematic“, gemeinsamer Streifen | Zwei Assets, ein Streifen |
| Skelett-Sektionen überblenden? | Nicht in v1 | Braucht Posen-Blend in der Spur; erst Grundlage |

---

## 5. Fahrplan

Jeder Punkt ist ein Hive-Schritt auf `claude/sequencer-cinematics`, in dieser Reihenfolge. Daten
und Laufzeit vor UI, damit jede Stufe ohne Editor testbar ist.

1. **Entwurf** (dieses Dokument).
2. **Asset + Auswertung.** `AssetType::Sequence`, `SequenceAsset`, `CHUNK_SEQU` laden/speichern,
   alle sieben Stellen aus §3.2, `PropTarget` um `CameraFov`/`Visible` erweitern, Feuerregel aus
   `collectNotifies` lösen (bestehende Notify-Tests müssen unverändert grün bleiben),
   `SequenceEval::evaluate` für Property- und Camera-Cut-Spuren als reine Funktion. Tests:
   Rundlauf, alte `.hasset` ohne Chunk, Stub, Auswertung an Keys/zwischen Keys/an Schnitten.
3. **Laufzeit.** `SequencePlayerComponent` (Serializer, Inspector, Handbuch), `SequenceSystem` am
   Ende von `tickAnimation`, Bindungsauflösung mit Slot-Überschreibung, Skeletal-Spur (§2.4),
   Event- und Audio-Spur über `NotifyQueue` bzw. `audio.play`. Tests mit echter Welt: Takt,
   Loop, Ende, Ereignisse genau einmal pro Durchlauf, fehlender Akteur wird übersprungen.
4. **Kamera und Eingabe.** `ownsCamera`, Gates in `GameApplication` und Editor-PIE, Blend rein
   und zwischen Cutscene-Kameras, Blend raus über `blendTo`, `lockPlayerInput`. Tests: Fly-Fallback
   bewegt die Kamera während der Sequenz nicht, Blend-Pose zur Zeit t, Rückgabe an das Rig.
   Zusätzlich optisch mit `scripts/he_shot.py` (Bildvergleich nur als Sichtprüfung, kein Test).
5. **Skript-API.** `sequence.*`-Zeilen, `SequenceFinished`, Parity-Fixture, Handbuch der Knoten.
   Ab hier ist eine Cutscene aus Skript und aus Szene heraus abspielbar, auch im gebauten Spiel.
6. **Editor-Tab „Cinematic“.** Streifen verallgemeinern (Zeilenarten, Gruppen), Bindungen,
   Vorschau-Sitzung mit Sichern/Zurückschreiben, Undo, Speichern/Neu-Laden-Vertrag, Anlegen aus
   dem Content Browser, Tooltips/Handbuch für jedes Bedienelement. Tests: Headless-Streifen,
   `he_uishot.py`, Vorschau lässt die Szene unverändert.
7. **Durch die Kamera schauen + MCP + Doku.** Viewport-Schalter, MCP-Werkzeuge, Handbuchseite
   „Cutscenes“, Website-Roadmap-Eintrag (Deploy nur nach Bestätigung).

Schritt 2 bis 5 sind je ein mittelgroßer Schritt, Schritt 6 ist der größte und darf bei Bedarf in
„Streifen“ und „Panel + Vorschau-Sitzung“ geteilt werden. Achtung bei der Reihenfolge 6 vor 7:
Bis „durch die Kamera schauen“ steht, sieht man Kameraschnitte im Editor nur als Frustum, nicht
als Bild. Soll Schritt 6 schon für Kameraarbeit taugen, den Viewport-Schalter aus 7 vorziehen und
mit 6 zusammenlegen.

### Stand nach Schritt 2 (Asset + Auswertung)

Umgesetzt:

- `AssetType::Sequence` (Enum, Name, kollab-synchronisierbar), `SequenceAsset` mit allen fünf
  Spurarten im Datenmodell (`Assets.h`), `CHUNK_SEQU` als JSON (siehe §3.2), Laden/Speichern im
  `ContentManager` inkl. `unloadAsset`/Umbenennen/Projektwechsel-Ketten. Kein Chunk = leere
  Sequenz; ein Chunk, der kein JSON-Objekt ist, lässt das Laden **scheitern** (sonst würde das
  nächste Speichern die Datei leer überschreiben). Unbekannte Spurarten/Ziele werden verworfen
  und gezählt.
- `PropTarget::CameraFov` und `PropTarget::Visible` (hinten angehängt, `kLastPropTarget`).
  `Visible` wird als Stufe gesampelt (`PropertyAnimationSystem::isStepTarget`, in
  `sampleChannel`), schreibt die `visible`-Flags von Mesh, SkeletalMesh, Light, Particle, Rope,
  Trail, nie `InactiveComponent`. `applyAt` ist in `applyChannel` (ein Wert, ein Ziel, eine
  Entity) zerlegt; Clip und Sequenz schreiben über dieselbe Funktion. Der alte Sequencer bietet
  die zwei neuen Ziele an (Gruppen „Camera“/„Visibility“); der Streifen zeichnet `Visible` noch
  linear, die Stufen-Darstellung gehört zu Schritt 6.
- `HE::collectNotifySpan(notifies, duration, …)`: die Feuerregel ohne `AnimationClipAsset`,
  `collectNotifies` ruft sie nur noch auf. Die 37 alten Notify-Testfälle laufen unverändert.
- `HE::SequenceEval` (`SequenceEval.h`): `evaluate(seq, t)` rein, liefert Property-Schreibwerte
  und den Kamerazustand (aktive Kamera, Blend-Quelle, geformtes `alpha`); `resolveBindings`
  (UUID, Slot-Überschreibung hat Vorrang); `apply` schreibt über `applyChannel`. Die Signatur
  ist damit `evaluate(seq, t)` + `apply(…, bindings)` statt eines einzigen
  `evaluate(seq, bindings, t)`, damit die Auswertung ohne Welt testbar ist.
- Referenzsuche und Retarget kennen `CHUNK_SEQU`; `HE::sequenceAssetRefs` liefert die Clips und
  Töne einer Sequenz für die Vorlade-Liste.
- Content Browser: Typfilter „Sequence“, Symbol (Glyphe des Property-Clips, andere Tönung),
  Namensanzeige in Asset-Slots. Der Stub-Writer kann eine Sequenz anlegen
  (`isCreatableAssetType`, `assetTypeFromName("Sequence")`); im laufenden Editor bietet
  `asset_create` sie trotzdem noch **nicht** an, weil dort `creatableTypes` gilt, und das spiegelt
  mit Absicht das Anlegen-Menü (siehe unten).

Bewusst **nicht** in Schritt 2, mit Grund:

- Die **Blend-Pose** zwischen zwei Kameras: `evaluate` sagt nur, welche Kamera, von welcher und
  wie weit. Die Pose braucht beide Weltposen und ist Teil von Schritt 4 („Blend-Pose zur Zeit t“).
- `collectAssetRefs` in die Sequenz schauen lassen: es gibt noch keine Abspielkomponente, an der
  die Sequenz hängt; das kommt mit ihr in Schritt 3 (`sequenceAssetRefs` liegt bereit).
- Anlegen-Menüeintrag im Content Browser und `creatableTypes` des Editors
  (`EditorApplication.cpp`, „MCP must not create what the menu refuses“, beide gehören
  zusammen): ohne den Cinematic-Tab wäre das ein Asset, das man anlegen, aber nicht öffnen
  kann. Kommt mit Schritt 6, dann gleichzeitig mit einem Handbuch-Eintrag „New Asset/…“.
- Eigene MCP-Lese-/Schreibwerkzeuge für den Inhalt einer Sequenz: Schritt 7 nennt sie
  ausdrücklich.

Befund zur Prüfung aus §3.2 Punkt 5: **Die Lücke beim State-Machine-Asset ist echt.**
`SceneSystems::collectAssetRefs` trägt nur `stateMachineAssetId` ein, nicht die Clips in der State
Machine (und nicht die Sample-Clips eines Blend Space). Die Getter des `ContentManager` laden
nicht nach, also fehlen solche Clips in einem gestreamten Paket beim ersten Auswerten. Nicht in
diesem Schritt behoben; eigener Punkt.

Nebenbefund: Die Ketten in `ContentManager::unloadAsset`, `rekeyAssetPaths` und
`forgetProjectContent` führen schon vor diesem Schritt nicht alle Pools (Theme, BoneMask,
BlendSpace, Struct/Enum/SaveGame fehlen teils). `Sequence` steht in allen dreien; die alten
Lücken sind nicht angefasst.

### Stand nach Schritt 3 (Laufzeit)

Umgesetzt:

- `SequencePlayerComponent` (`Components/SequencePlayerComponent.h`): gespeichert werden nur
  `sequenceId`, `autoplay`, `loop`, `playRate`. Abspielkopf, Zustand, aufgelöste Bindungen,
  Slot-Überschreibungen und Ton-Handles sind Sitzungszustand und werden nie gespeichert (das
  PIE-Snapshot ist Speichern/Neuladen, eine zweite Sitzung startet also wieder mit Autoplay).
  Serializer (JSON und CBOR über denselben Weg), X-Makro-Schlüssel `sequenceplayer`,
  Prefab-Schlüssel, Inspector-Abschnitt „Sequence Player“ (zeigt in PIE Zustand und Zeit),
  Add-Component-Zeile unter „Animation“ **ohne** Skelett-Pflicht, Hilfe-Einträge (Audit 981/981).
  Eine neue Komponente braucht außer Serializer und X-Makro noch zwei Stellen, die erst die
  Nachbar-Tests zeigen: `SceneSerializer::isKnownComponentKey` (sonst meldet
  `test_inspector_prefab_keys` den Prefab-Schlüssel als unbekannt) und `kComponentScopes` in
  `EditorHelp.cpp` (sonst hat der Hilfe-Schlüssel keinen Bereich im Handbuch).
- `SequenceSystem` (`SequenceSystem.h`) mit **zwei** Aufrufen statt einem am Ende von
  `tickAnimation` (Abweichung von §3.5, mit Grund):
  - `begin` **vor** den drei Skelett-Treibern: Uhr vorrücken, Ereignisse und Ton über
    (tPrev, tEnd], und jedes Skelett, das eine Sektion zur neuen Zeit posiert, **beanspruchen**
    (`SkeletalMeshComponent::sequencePosed`). Animator, Blend und State Machine überspringen
    beanspruchte Skelette ganz (Uhr steht, keine Notifies, keine Pose, beim State Machine auch
    der Sync-Graph). Damit ist die Sequenz der **Basis-Treiber** aus §3.3: Layer und IK laufen
    genau einmal auf ihrer Pose, nicht doppelt und nicht mit der Warnung für zwei Treiber.
    Beansprucht wird nur, wenn Clip und Mesh geladen sind; sonst behält der Animator das Skelett.
  - `apply` **nach** dem Property Animator: erst die Property-Spuren (die Sequenz gewinnt gegen
    einen Clip auf demselben Ziel), dann die Skelette über `sampleClip` + `poseFinalize`, damit IK
    von der gerade geschriebenen Transform aus castet.
- **Sitzungs-Gate:** `tickAnimation` hat einen neuen, optionalen Parameter
  `HE::SequenceContext*` (AudioEngine, PhysicsWorld). Null heißt: keine Sequenz rückt vor,
  schreibt oder tönt. `GameApplication` übergibt ihn immer, der Editor nur während PIE (dasselbe
  `playing` wie für Root Motion und Notifies). Das ist die zweite Änderung an den Anwendungen
  neben der Kamera-Abfrage aus §3.4, und sie ist nötig: ohne sie würde eine Cutscene mit
  Autoplay die Edit-Welt verstellen, und das würde gespeichert.
- Transport als C++ (`play`, `pause`, `stop`, `setTime`, `bindSlot`), damit die Tests die Uhr
  treiben können; Schritt 5 registriert dafür nur noch die Zeilen. `setTime` verschiebt den
  Anfang der nächsten Spanne mit, übersprungene Ereignisse feuern also nicht; ein gestoppter
  Spieler schreibt den neuen Zeitpunkt einmal.
- **Ereignisse:** `collectNotifySpan` pro Event-Spur an den gebundenen Akteur, ohne Bindung an
  den Besitzer, bei fehlendem Akteur gar nicht. Ereignisse am Ende eines nicht schleifenden
  Laufs feuern genau einmal (geschlossenes Ziel).
- **Ton:** dieselbe Spannen-Regel über die Startzeiten der Sektionen, Start mit
  `AudioEngine::play` (ohne Bindung, flach) bzw. `playSpatial` an der Weltposition des Akteurs.
  Nur vorwärts; rückwärts und beim Springen startet nichts (`seekSound` gäbe es inzwischen, ein
  Start mitten in der Sektion bleibt trotzdem außen vor). `pause()` pausiert die Töne der
  Sequenz mit der Uhr, `play()` setzt sie fort, `stop()` stoppt sie, das natürliche Ende lässt
  sie ausklingen.
- **Skelett-Sektionen:** aktiv ist die mit dem spätesten Start, deren [start, end] t enthält
  (Gleichstand: die später gelistete), wie bei den Schnitten; Clipzeit geloopt oder geklemmt.
  Root Motion aus einer Sektion wird nicht angewendet (§6).
- **Vorladen:** `collectAssetRefs` nennt die Sequenz der Komponente; ihre Clips und Töne folgen
  ihr über einen `Sequence`-Fall in `ContentManager::expandFrontier` (gestreamtes Paket) bzw.
  einen zweiten Durchgang in `preloadAssetRefs` (Editor, lose Dateien).
- Tests: `tests/test_sequence_runtime.cpp` (16 Fälle) gegen echte Welt über
  `tickAnimation`: Gate ohne Sitzung, Takt und Play Rate, Ende und Neustart, Pause/Stop gegen
  einen Property Animator, Loop mit Ereignissen genau einmal pro Durchlauf (auch Notify-States),
  Ereignis am Ende, Ereignis an Akteur/Besitzer, fehlender Akteur übersprungen, `setTime` ohne
  Zwischenereignisse, Slot-Überschreibung, nicht geladene und leere Sequenz, Sektionsregel,
  Skelett-Übernahme vom Animator und Rückgabe, Clip nicht geladen, Ton mit Pause/Fortsetzen (noDevice-AudioEngine),
  rückwärts, Serializer, Asset-Referenzen.

Bewusst **nicht** in Schritt 3:

- Kamera (`ownsCamera`, Schnitt setzt `isMain`, Blends), `blendOut*` und `lockPlayerInput` an der
  Komponente: Schritt 4. Die Felder kommen mit ihrem Verhalten, damit der Inspector keine
  Schalter zeigt, die nichts tun. Heute wertet die Laufzeit die Camera-Cut-Spur nicht aus.
- `SequenceFinished` und die `sequence.*`-Zeilen: Schritt 5. Das Ende ist heute an
  `playing == false` mit `time == duration` erkennbar.
- Innerhalb eines Frames kommen Ereignisse Spur für Spur, nicht über alle Spuren nach Zeit
  sortiert.

### Stand nach Schritt 4 (Kamera und Eingabe)

Umgesetzt:

- **Kamera-Besitz als Lease** (`SequenceSystem.cpp`, `CameraLease` im Kontext der Registry,
  `SequenceSystem::ownsCamera`). Die Lease liegt nicht an der Komponente, weil sie ihren Besitzer
  überleben muss: Wird der Besitzer mitten in der Cutscene zerstört, gibt der nächste Frame die
  Sicht trotzdem zurück. `SequencePlayerComponent::cameraOwned` ist die Hälfte des Besitzers. Eine
  Lease, deren Besitzer davon nichts weiß (Szene unter denselben Entity-IDs neu geladen), ist
  veraltet und wird verworfen. Ohne `SequenceContext` (Edit-Frames des Editors zwischen zwei
  PIE-Sitzungen) wird sie immer verworfen.
- **Übernahme beim ersten aktiven Schnitt, nicht bei `play()`** (Abweichung von §3.3, dort hieß
  es „beim Start eingefroren“): Bis zum ersten Schnitt gehört die Kamera noch dem Spieler. Beim
  Übernehmen wird die Kamera gemerkt, die gerade zu sehen ist, und ihre Weltpose eingefroren (die
  Quelle eines Blends in den ersten Schnitt). Danach geben alle Rigs frei
  (`CameraRigController::releaseAll`: versteckter Körper zurück, `fovOffset` 0, Blend und
  Lag-Pose vergessen). Nach einem Schnitt auf „keine Kamera“ wird beim nächsten Schnitt erneut
  übernommen und neu eingefroren. `SequenceEval.h` sagt dasselbe.
- **Schnitt**: `CameraRigController::makeMain` (aus `blendTo` herausgezogen) setzt `isMain`
  eindeutig, in jedem Frame, in dem die Sequenz die Kamera hält.
- **Blend rein und zwischen Cutscene-Kameras**: in `apply` nach den Property-Spuren, zwischen der
  Quellpose (eingefrorene Gameplay-Pose oder die Kamera des vorigen Schnitts zur selben Zeit t)
  und der Pose der aktiven Kamera. Position per `mix`, Drehung per `slerp`, FOV als `fovOffset`
  (`fovDegrees` wird nie geschrieben). Die Pose wird in die aktive Kamera geschrieben. Ihre
  Transform und ihr `fovOffset` werden vorher gesichert und am Anfang des nächsten `apply`
  zurückgeschrieben, **bevor** die Spuren laufen. Eine platzierte Schnittkamera ohne Keys steht
  nach dem Blend also exakt wieder dort, wo sie steht, und eine gefahrene liest ihre Keys.
  Weltposen über `worldMatrixOf` (die Spuren haben die Transform gerade erst bewegt).
- **Rückgabe** (Ende, `stop()`, Schnitt auf keine Kamera, fehlender Akteur oder ein Slot ohne
  `CameraComponent`, zerstörter Besitzer): `CameraRigController::blendTo` in die gemerkte
  Gameplay-Kamera über `blendOutSeconds`/`blendOutCurve` der Komponente. Bei einer Rig-Kamera
  bekommt deren Transform im Übergabe-Frame zusätzlich die zuletzt gezeigte Cutscene-Pose. Sonst
  zeigt dieser Frame (die Rückgabe passiert in `tickAnimation`, nach dem Kamera-Controller) die
  Pose, die das Rig **vor** der Cutscene hatte. Eine Kamera ohne Rig (Fly) bekommt keine Pose,
  weil sie sonst teleportiert würde; dort ist die Rückgabe ein Schnitt. Nach `stop()` oder mit
  zerstörtem Besitzer gibt der Kopf von `apply` zurück, **bevor** die Blend-Schreibung des
  Vorframes zurückgenommen wird: Wer mitten im Blend-In überspringt, übergibt von der Pose, die
  zu sehen ist, nicht von der platzierten.
- **Eine Sequenz zur Zeit hält die Kamera.** Ein zweiter Spieler, dessen Schnitt aktiv wird,
  spielt weiter und übernimmt, sobald der erste loslässt (im selben oder im nächsten Frame).
- **Gates in beiden Anwendungen**: `GameApplication::updateCameraController` und
  `EditorApplication::updatePlayCameraController` kehren zurück, solange `ownsCamera` gilt. Im
  Editor sitzt das Gate **nach** dem Wiederherstellen der Maus-Capture, damit der Spieler nach der
  Cutscene nicht neu klicken muss.
- **`lockPlayerInput`**: `SequenceSystem::locksPlayerInput` (irgendein spielender oder pausierter
  Spieler mit dem Schalter) geht als neuer Parameter an `PlayerHost::tick`, in beiden
  Anwendungen. Dort ist die Sperre der dritte Grund für Stille neben Pause und UI-only, mit
  derselben Ausnahme: Aktionen mit „run while paused“ kommen durch, damit Überspringen und
  Pausenmenü gehen. Die Kamera-Sperre ist davon unabhängig. Standard ist **aus**, weil eine
  schleifende Ambient-Sequenz dem Spieler sonst dauerhaft die Steuerung nähme.
- Komponentenfelder `blendOutSeconds` (Standard 0 = Schnitt), `blendOutCurve`, `lockPlayerInput`:
  Serializer (JSON und CBOR, Kurve als Index wie bei `camera.blendTo`), Inspector (Statuszeile
  zeigt „holds the camera“), Hilfe. Audit 984/984.
- Tests (`tests/test_sequence_runtime.cpp`, jetzt 27 Fälle): Übernahme erst am Schnitt und
  Rückgabe am Ende; ein auf `ownsCamera` gesperrter Kamera-Controller bewegt die Schnittkamera
  nicht (Stellvertreter für den Fly-Fallback, der headless kein SDL lesen kann, mit
  Negativkontrolle ohne Gate); Blend rein von der eingefrorenen Gameplay-Pose (Position, Drehung,
  FOV zur Zeit t) und danach die unveränderte Schnittkamera; Blend zwischen zwei Kameras, von
  denen eine fährt; Rückgabe an ein echtes Rig mit Blend-Out, ohne veralteten Frame (mit
  Negativkontrolle: ohne das Schreiben der Übergabe-Pose wird der Test rot); `stop()` mitten im
  Blend übergibt von der Pose auf dem Schirm (war vor dem Fix rot); Körper des
  First-Person-Rigs sichtbar; `stop()`, Schnitt auf keine Kamera und Slot ohne Kamera; eine
  Sequenz zur Zeit und zerstörter Besitzer; Lease ohne Sitzung verworfen; `locksPlayerInput`;
  Serializer um die drei Felder erweitert. Dazu in `tests/test_player_host.cpp`: gesperrter Frame
  still wie eine Pause, `runWhilePaused` kommt durch.

Bewusst **nicht** in Schritt 4:

- **Sichtprüfung mit `scripts/he_shot.py`** (§5, Schritt 4): nicht möglich. Der Headless-Dump
  rendert die Edit-Welt durch die Editor-Kamera und startet nie PIE. Ohne Sitzung gibt es keinen
  `SequenceContext`, es läuft also keine Sequenz, und die Szenenkamera wird nicht gezeigt. Das
  gehört zu „durch die Kamera schauen“ (Schritt 7) oder zu einem Dump-Schalter, der PIE startet.
  Die Verdrahtung in beiden Anwendungen ist per `grep` und Build belegt, nicht per Bild.
- `sequence.*`-Zeilen und `SequenceFinished`: Schritt 5.
- Rückgabe, wenn der Besitzer durch **Deaktivieren** (`InactiveComponent`) statt Zerstören
  wegfällt: Die Sequenz läuft dann weiter wie in Schritt 3 und hält die Kamera. Das entscheidet
  sich mit den Skript-Zeilen in Schritt 5.
- Look-Eingabe während einer Sequenz, die die Kamera nicht hält: Das Rig dreht dann weiter mit
  der Maus, auch mit `lockPlayerInput`. Die Sperre gilt der Steuerung, nicht dem Blick.

### Stand nach Schritt 5 (Skript-API)

Umgesetzt:

- **Acht Registry-Zeilen** in der neuen Gruppe `sequence` (Kategorie „Sequence“), dünn über den
  Transport aus Schritt 3: `play` (→ `started`), `pause`, `stop`, `setTime`, `getTime`,
  `duration`, `isPlaying`, `bindSlot`. Die Entity ist immer der **Besitzer** (die Entity mit dem
  Sequence Player), nie ein Akteur. Ohne Welt, ohne Spieler oder mit ungültigem Handle antworten
  sie neutral (false/0), wie jede andere Zeile gegen einen leeren `Ctx`. `duration` kam zu den
  sieben aus §3.5 dazu: `setTime` ohne die Länge ist Raten. `isPlaying` heißt „die Uhr läuft“,
  pausiert ist also false.
- Die drei Registry-Stellen: Anzeigenamen („Play Sequence“ …), `HcNodeDocs` für alle acht,
  `"sequence"` in `isScriptGroup`. Damit gibt es `horizon.sequence.*` in Lua und Python ohne
  eine Zeile Binding-Code. **Keine vierte Stelle:** `sequence` gehört wie `animator` nicht zur
  C++-GameLogic-Schnittstelle (`HorizonGameServices.h` hat keine Tabelle dafür).
- **`SequenceFinished`** (`SequenceSystem::kSequenceFinished`, ein reservierter Name) geht als
  gewöhnliche Notify an den Besitzer, über dieselbe `NotifyQueue`, die direkt nach
  `tickAnimation` zugestellt wird. Lua/Python hören es in `onAnimationNotify`, HorizonCode in
  `OnAnimationNotify`, ohne neuen Handler und ohne neue Stelle in den Anwendungen. Es kommt
  - beim natürlichen Ende (vorwärts und rückwärts), im selben Frame nach den Ereignissen des
    letzten Frames und nach der Kamera-Rückgabe;
  - bei `stop()` eines laufenden (auch pausierten) Spielers. **Entscheidung:** Eine Skip-Taste ist
    `stop()`, und „Steuerung zurückgeben, nächstes Level laden“ muss auch nach einem Skip laufen.
    `stop()` hat keine Queue, also merkt es sich das (`finishedPending`) und der nächste
    `begin()` mit Sitzung sendet es (dasselbe Muster wie `stopAudio`);
  - nicht von einer Schleife (sie endet nie von selbst), nicht bei `stop()` eines gestoppten
    Spielers, nicht bei zerstörtem Besitzer.
- **Deaktivierter Besitzer = `stop()`** (die offene Frage aus Schritt 4). `begin()` stoppt einen
  Spieler, dessen Besitzer (oder ein Vorfahr) aus ist: Kamera zurück, Eingabesperre weg, Töne
  aus, `SequenceFinished`. `play()` auf einem abgeschalteten Besitzer antwortet false. Wieder
  einschalten startet nicht neu. Einen Vorläufer für „einfrieren und fortsetzen“ gibt es nicht
  (die Animations-Treiber kennen `InactiveComponent` gar nicht), und eingefroren hielte ein
  unsichtbares Objekt Kamera und Steuerung fest.
- **`bindSlot` per Bindungsname statt Slot-Nummer (Abweichung von §3.5):** Der Autor sieht den
  Namen, nie die Nummer, dasselbe Argument wie bei `animator.setLayerWeight`. Der Name bleibt
  ein Name (`SequencePlayerComponent::namedOverrides`), bis die Bindungen aufgelöst werden,
  weil die Sequenz beim Binden vor `play()` noch streamen kann. Doppelter Name: der zuerst
  gelistete. Ein unbekannter Name wird dann gewarnt. Eine Überschreibung per Nummer
  (`SequenceSystem::bindSlot`, C++) gewinnt gegen eine per Name für denselben Slot. Ziel `0`
  (in dieser API „keine Entity“) hebt die Überschreibung auf; ein Ziel, das es nicht gibt, wird
  mit Warnung abgelehnt statt als 0 gelesen (sonst stünde der Platzhalter aus dem Asset wieder
  in der Szene).
- **Parity-Fixture** `fxSequenceTransport` (`sequence_transport` in `HCGEN_CLASSES`): alle acht
  Zeilen in Reihenfolge, dazu `OnAnimationNotify` mit `string.equals` auf „SequenceFinished“.
- Tests (`tests/test_sequence_runtime.cpp`, jetzt 33 Fälle): Ende genau einmal vorwärts, rückwärts,
  am Besitzer, keins aus einer Schleife; `stop()` laufend/pausiert sendet, gestoppt nicht, ohne
  Sitzung erst im nächsten Frame mit Queue; abgeschalteter Besitzer gibt Kamera und Eingabe
  zurück, `play()` verweigert, Einschalten startet nicht; `bindSlot` per Name vor dem Laden,
  doppelter/unbekannter Name, Nummer schlägt Name; die Zeilen gegen echte Welt; **Lua von Anfang
  bis Ende** (`horizon.sequence.play` startet, `onAnimationNotify` hört „Line“ und
  „SequenceFinished“ über `AnimationNotifySystem::dispatch`). Der alte Fall „non-looping …
  exactly once“ zählt jetzt vier Namen statt drei (das Ende kommt nach „Z“). Negativkontrolle:
  ohne die Deaktivierungszeile und ohne `"sequence"` in `isScriptGroup` werden genau die drei
  zugehörigen Fälle rot. Dazu `codegen parity: sequence_transport`.

Bewusst **nicht** in Schritt 5:

- Eine Zeile, die die Sequenz eines Spielers zur Laufzeit wechselt (`setSequence(path)`): Es
  gibt keinen Fall, den ein zweiter Sequence Player nicht besser löst.
- `isPaused`: pausiert und gestoppt unterscheidet `getTime` (gestoppt steht auf 0).
- Handbuchseite „Cutscenes“ und MCP-Werkzeuge: Schritt 7. Die acht Knoten stehen im Knoten-Handbuch
  (`HcNodeDocs`).
- Der Python-Weg ist nicht eigens getestet: Er baut seine Tabelle aus derselben
  `isScriptGroup`-Liste wie Lua, und der Lua-Fall geht ihn von Anfang bis Ende.

---

## 6. Bewusst außen vor (v1)

- Kurven jenseits linear (Bezier-Tangenten); der Streifen hat eine Kurvenansicht, die Auswertung
  bleibt linear wie `sampleChannel`.
- Verschachtelte Sequenzen (Shots/Sub-Sequenzen).
- Gespawnte Akteure („Spawnables“), die nur während der Sequenz existieren.
- Untertitel/Dialog-Spur; bis dahin über Event-Spur + `ui.setText`.
- Überblenden zwischen Skelett-Sektionen, Root-Motion aus Sequenz-Sektionen.
- Render-Export (Film als Video rendern), Fade-Spur, Partikel-Spur.
- Netzwerk: eine Sequenz läuft lokal pro Client. Replikation des Starts ist ein eigenes Thema.
- Ungeskalierte Zeit.
