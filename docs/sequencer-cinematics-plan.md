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
| `PropertyAnimClipAsset` | `src/HE_Core/include/ContentManager/Assets.h:695`, Chunk `CHUNK_PANM` (`HAsset.h:185`), Laden/Speichern `ContentManager.cpp:590`/`:1790` | Skalare Kanäle (`PropertyAnimChannel`: `times`/`values`) für 15 Ziele (`PropTarget`, `Assets.h:678`): Position/Rotation/Skalierung XYZ, Materialfarbe/-metallic/-roughness/-opacity | Ja, als Datenform der Property-Spur. Kein FOV, keine Sichtbarkeit |
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
Frames rechtzeitig (die kommt danach). Nur ein Kamera-Controller, der im nächsten Frame davor
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

Ein `.hasset` mit einem Chunk `CHUNK_SEQU` (Binärform wie alle Nachbarn):

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
  Dateien bleiben gültig, der alte Sequencer bekommt sie gleich mit).

Ein neuer Assettyp muss an **allen** diesen Stellen eingetragen werden. Das Audit hat
`PropertyAnimClip` einmal als „toten Assettyp“ gefunden, weil genau zwei davon fehlten:

1. `AssetType` in `src/HE_Core/include/Types/Enums.h` (Enum, beide Namens-Switches)
2. Beide Dispatch-Switches in `ContentManager.cpp` (Laden `:590`-Gegenstück, Speichern `:1790`-Gegenstück), Getter/Register/Acquire in `ContentManager.h`
3. `AssetStubWriter.cpp` (Anlegen aus dem Content Browser, Stub = leere Sequenz)
4. Content Browser: Anlegen-Menü, Symbol (`EditorApplication.h`, neben `m_iconPropertyAnimClip`), Doppelklick-/Tab-Dispatch
5. `SceneSystems::collectAssetRefs` (`SceneSystems.cpp:244`): die Sequenz der Abspielkomponente **und** die Assets in der Sequenz (Skelett-Clips, Ton). Prüfen, ob der Pack-Schritt nur die UUID-Hülle der Szene packt; dann muss er in die Sequenz hineinsehen wie beim State-Machine-`clipId`
6. `AssetRefScan` (UUID-Form im Chunk) und Rename-Retarget, sonst meldet der Lösch-Dialog „unreferenziert“
7. MCP: mindestens Lesen/Schreiben der Sequenz (Muster `McpToolsClip.cpp`)

### 3.3 Spurarten in v1

| Spur | Auswertung zur Zeit t | Baustein |
|---|---|---|
| **Property** (Transform, Material, FOV, Sichtbarkeit) | `sampleChannel` pro Kanal, Schreiben wie `applyAt` | `PropertyAnimChannel`, `PropertyAnimationSystem`. `applyAt` wird in „ein Kanal auf eine Entity“ zerlegt, damit Clip und Sequenz dieselbe Schreibfunktion benutzen |
| **Skeletal** | Aktive Sektion finden, Clipzeit = `(t - start) * playRate + clipOffset` (geloopt oder geklemmt), Pose über den Weg von `AnimationPreview::evaluateClipPose` direkt in `SkeletalMeshComponent::boneMatrices` | Eigener Auswertungsweg wegen §2.4. Läuft als **letzter** Skelett-Treiber im Frame, gewinnt also gegen den Animator der Entity. Überblenden zwischen zwei Sektionen kommt später |
| **Camera Cut** | Letzter Schnitt vor t bestimmt die Kamera; liegt t innerhalb `blendIn` nach dem Schnitt, Pose = Slerp/Lerp zwischen der Pose der vorigen Kamera zur Zeit t und der neuen, geformt mit `applyBlendCurve` | `BlendCurve`, `applyBlendCurve`, `SolvedPose`. Erster Schnitt mit `blendIn > 0`: Quelle ist die beim Start eingefrorene Gameplay-Kamerapose (`snapshotCameraPose`-Muster) |
| **Event** | Nur beim Abspielen: Spannen-Lauf über (tPrev, tEnd] | Aus `collectNotifies` gelöste Regel (§2.5). Zustellung über `NotifyQueue` + `AnimationNotifySystem::dispatch` an die gebundene Entity (ohne Bindung: an die abspielende Entity). Damit bekommen Lua, Python, HorizonCode und Sync-Graph das Ereignis ohne ein einziges neues Handler-API |
| **Audio** | Nur beim Abspielen: Sektion starten, wenn die Spanne ihren Start überquert; beim Stopp/Abbruch stoppen | `audio.play` / `audio.playAt` (an der gebundenen Entity). Springt die Sequenz mitten in eine Sektion (`setTime`), startet der Ton **nicht** mittendrin: das braucht Seek, und Seek gibt es nur im Pull-Modus (siehe Memory zu miniaudio) |

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
   Prüfen mit `he_shot.py` (nur optisch, Memory headless-visual-verification).
5. **Skript-API.** `sequence.*`-Zeilen, `SequenceFinished`, Parity-Fixture, Handbuch der Knoten.
   Ab hier ist eine Cutscene aus Skript und aus Szene heraus abspielbar, auch im gebauten Spiel.
6. **Editor-Tab „Cinematic“.** Streifen verallgemeinern (Zeilenarten, Gruppen), Bindungen,
   Vorschau-Sitzung mit Sichern/Zurückschreiben, Undo, Speichern/Neu-Laden-Vertrag, Anlegen aus
   dem Content Browser, Tooltips/Handbuch für jedes Bedienelement. Tests: Headless-Streifen,
   `he_uishot.py`, Vorschau lässt die Szene unverändert.
7. **Durch die Kamera schauen + MCP + Doku.** Viewport-Schalter, MCP-Werkzeuge, Handbuchseite
   „Cutscenes“, Website-Roadmap-Eintrag (Deploy nur nach Bestätigung).

Schritt 2 bis 5 sind je ein mittelgroßer Schritt, Schritt 6 ist der größte und darf bei Bedarf in
„Streifen“ und „Panel + Vorschau-Sitzung“ geteilt werden.

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
