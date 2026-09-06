# Gameplay Camera Rigs: Lag, Shake, Blending, FOV-Kick

Fortsetzung von [`camera-rig-plan.md`](camera-rig-plan.md). Jener Plan hat das Rig
gebaut (First/Third Person, Rotationskopplung, Spring-Arm mit Kugel-Sweep) und
Lag, Shake, Blending und FOV-Kick ausdrücklich als „nicht in v1" geparkt. Dieses
Dokument holt genau diese vier nach.

Reine Gameplay/ECS-Schicht. Kein Backend-Risiko: die einzige Zeile außerhalb von
`HE_Scene`/`HE_Editor` ist ein additives FOV im Extractor.

---

## 1. Stand — was es schon gibt

Das Thema im Hive („Third-Person-Follow/Orbit-Kamera oben auf der bestehenden
Free-Fly-Kamera") beschreibt einen Zustand, den der Code hinter sich gelassen
hat. Das Rig existiert und ist verdrahtet:

| Teil | Ort | Zustand |
|---|---|---|
| `CameraRigComponent` | `src/HE_Scene/include/HorizonScene/Components/CameraRigComponent.h` | 16 Felder, beide Modi, Maus + Stick, Kollision |
| `CameraRigController` | `src/HE_Scene/src/CameraRigController.cpp`, `include/HorizonScene/CameraRigController.h` | statisch, zustandslos, `update()` + `findRigCamera()` |
| Spring-Arm-Verkürzung | `CameraRigController.cpp` (Boom-Kollision) über `PhysicsWorld::sphereCast` | **fertig** — Kugel-Sweep, kein Ray; ignoriert die Ziel-Entity |
| Aufrufstellen | `GameApplication.cpp:1459` (`updateCameraController`), `EditorApplication.cpp:2630` (`updatePlayCameraController`) | beide **nach** dem Physik-Step |
| Inspector | `InspectorPanel.cpp:1066-1172`, Add-Menü `:1843`, Outliner-Vorlage `OutlinerPanel.cpp:75` | vollständig |
| Serializer | `SceneSerializer.cpp:263` (save), `:808` (load); Binär ist CBOR derselben Struktur | vollständig |
| Skript-API | `EngineApi.h:583-600`, Impl `EngineApi.cpp:1036-1104`, 12 Registry-Zeilen `:2624-2647` | Lua/Python/HorizonCode/Codegen |
| Handbuch | `EditorHelp.cpp:77` (Komponente), `:370-410` (jedes Bedienelement) | vollständig |
| Tests | `tests/test_camera_rig.cpp` — 27 `TEST_CASE` | Platzierung, Look, Kopplung, Kollision, Mesh-Hiding |

Free-Fly (`FlyCameraController`) bleibt der Fallback: greift genau dann, wenn
`CameraRigController::update(...).driven == false` ist, also wenn es keine
Rig-Kamera gibt oder deren Ziel sich nicht auflösen lässt.

**Offen ist damit nur noch das, was `camera-rig-plan.md` §5 aufzählt:** Lag,
Shake, Blending, FOV-Kick, weiches Nachdrehen der gekoppelten Rotation.

### Rescope der Hive-Schritte 2–4

Schritt 2 („Core: CameraRigComponent + System (Follow/Orbit, Spring-Arm,
Lag) + Tests") ist zu drei Vierteln bereits Code auf `main`. Was übrig bleibt:

| Schritt | Lautete | Ist tatsächlich |
|---|---|---|
| 2 | Core: Komponente + System + Spring-Arm + Lag | **Solved-Pose-Umbau (§3) + Lag (§4) + Tests** |
| 3 | Shake, Blending, FOV-Kick + Tests | unverändert (§5, §6, §7) |
| 4 | Editor-Integration + Doku | unverändert, Umfang siehe §9 |

---

## 2. Frame-Einordnung und Zeit

Das Rig läuft mit `gameDt`, dem **skalierten** Delta
(`GameApplication.cpp:1459`, `EditorApplication.cpp:2630`). Beide Aufrufstellen
haben oben denselben Riegel:

```cpp
if (!m_mouseCaptured || !m_world || dt <= 0.0f) return;   // GameApplication.cpp:1196
if (!m_isPlaying || !m_editorWorld || dt <= 0.0f) return; // EditorApplication.cpp:5853
```

Daraus folgt ohne eine einzige neue Zeile: **bei `timeScale == 0` läuft das Rig
gar nicht.** Lag friert ein, Shakes stehen still, ein laufendes Blend hält an,
und auch die Maus dreht die Kamera nicht (der Riegel kommt vor dem Lesen von
`Input::mouse()`). In Zeitlupe dehnen sich Lag, Shake und Blend mit — die Kamera
gehört zur Spielwelt, das ist die richtige Kopplung.

Konsequenz für das parallele Thema **Time Control**: es braucht hier nichts zu
tun. Ein Hit-Stop friert die Kamera mit ein. Falls das unerwünscht ist (ein
Shake, der über den Hit-Stop hinweg weiterläuft, ist ein bekannter Trick), ist
der Ort dafür ein `unscaled`-Flag pro Shake (§5) plus ein zweiter `dt` im
`CameraLookInput` — die Quelle dafür gibt es bereits
(`HE::api::time::unscaledDeltaTime`, Registry-Zeile `time.unscaledDeltaTime`).
**Bewusst nicht in dieser Runde:** es müsste den Riegel oben aufbrechen und
schaltete damit auch die Maus im Pausenmenü wieder scharf.

---

## 3. Das Fundament: Solved Pose

Heute rechnet `update()` die Pose und schreibt sie direkt in den
`TransformComponent` der Kamera. Für Lag, Shake und Blending reicht das nicht:
alle drei brauchen die Pose als **Wert**, bevor sie im Transform landet.

```cpp
// Was ein Rig in diesem Frame ergeben HAT — Zwischenergebnis, kein Zustand.
struct SolvedPose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float     fovDegrees = 0.0f;   // Basis-FOV der Kamera + Kick
    bool      occluded   = false;
};
```

Neuer Ablauf von `CameraRigController::update`:

1. Aktive Kamera bestimmen (`findRigCamera`, unverändert).
2. **Jedes** Rig lösen, nicht nur das aktive — ein Blend interpoliert gegen eine
   Quell-Pose, und eine eingefrorene Quell-Pose ist der klassische Fehler dabei
   (die Kamera blendet von dort, wo die alte Kamera vor dem Umschalten *stand*,
   statt von dort, wo sie *wäre*).
3. Look-Input bekommt **nur das aktive Rig**. Ein Quell-Rig während eines Blends
   dreht sich nicht mehr mit der Maus — sonst zieht der Spieler an einer Kamera,
   die er gerade verlässt.
4. Kugel-Sweep nur für das aktive Rig und, während eines Blends, für das
   Quell-Rig. Alle anderen behalten ihre volle Armlänge. Damit bleiben die
   Physik-Queries bei höchstens zwei pro Frame, egal wie viele Rigs die Szene hat.
5. Blend anwenden (§6), dann Shake (§5) addieren, dann schreiben.

### Das Gesetz

> **Der `TransformComponent` der Kamera ist reine Ausgabe.** Das Rig liest ihn
> nie zurück. Jeder Zustand, den das Rig über Frames hinweg braucht, liegt in
> `CameraRigComponent`.

Warum das nicht verhandelbar ist: Lag braucht die geglättete Pose des letzten
Frames. Läge die im Transform, würde jede fremde Schreiboperation zum
Rig-Zustand — ein Gizmo-Zug im Editor, ein `camera.setPosition` aus einem
Skript, das Zurückspielen eines Undo-Snapshots. Und Shake ist ein additiver
Offset: einmal in den Transform geschrieben und wieder gelesen, akkumuliert er
sich in wenigen Sekunden in die Unendlichkeit.

### Neue Felder sind Laufzeitzustand, nicht Szenendaten

Alles, was §4–§7 hinzufügen (geglätteter Pivot, Arm-Yaw/Pitch, Shake-Liste,
Blend-Zustand, `fovOffset`) wird **nicht serialisiert**. Vorbild ist
`meshHiddenEntity`: Kommentar an der Deklaration, keine Zeile im Serializer,
keine Zeile im Inspector. Der Grund ist konkret: PIE speichert bei einem
Stop-Snapshot die Szene, und eine mitgespeicherte Lag-Pose oder ein halb
abgeklungener Shake wäre ab dann Autoreninhalt.

Die **Regler** dagegen (Lag-Geschwindigkeiten, Grenzwerte, Kurven) sind
Szenendaten und wandern durch Serializer, Inspector und Handbuch.

---

## 4. Lag

Zwei getrennte Glättungen, weil sie zwei verschiedene Dinge tun.

### 4.1 Positions-Lag am Pivot, nicht an der Kamera

```
pivotLagged = smooth(pivotLagged, targetPos + pivotOffset, lagSpeed, dt)
camPos      = pivotLagged + rot * arm - forward * armLength
→ dann erst der Sweep, aus pivotLagged heraus
```

Geglättet wird der **Pivot**, und der Sweep geht anschließend vom geglätteten
Pivot aus. Die Alternative — die fertige Kamerapose glätten — kollidiert mit der
Verkürzung: der Arm springt an einer Wand kurz, die Glättung zieht die Kamera
langsam hinterher, und das heißt sie kriecht sichtbar in die Wand hinein und
wieder heraus. Die Reihenfolge „erst Lag, dann Kollision" hält die Verkürzung
hart und sofort, wo sie hart und sofort sein muss.

### 4.2 Rotations-Lag nur an der Armrichtung

`rig.yaw` / `rig.pitch` bleiben der **ungeglättete Eingabezustand**. Das ist
gleichzeitig die Blickrichtung, die in den Transform geht, und der Wert, den
`camera.getRigYaw` und die Follow-Kopplung lesen.

Geglättet wird ein zweites Paar `armYaw` / `armPitch`, das **ausschließlich die
Boom-Richtung** bestimmt. Effekt: bei einer schnellen Drehung schwenkt der Blick
sofort mit, der Arm zieht nach, die Figur wandert dabei kurz aus der Bildmitte —
genau der Effekt, für den Rotations-Lag da ist.

Bewusst anders als Unreal, das auch die Blickrichtung mitglättet: eine 1:1-Maus
ist die Eigenschaft, die eine Kamera „direkt" anfühlen lässt, und die
Follow-Kopplung würde eine geglättete Blickrichtung an die Figur weiterreichen —
dann läuft der Spieler in eine Richtung, in die er nicht schaut.

### 4.3 Die drei Fallen

**Framerate-Unabhängigkeit.** `lerp(a, b, speed * dt)` ist framerate-abhängig,
und zwar so, dass es bei 60 Hz gut aussieht und bei 144 Hz anders. Der einzige
zulässige Glätter:

```cpp
const float alpha = 1.0f - std::exp(-speed * dt);
```

**Kürzester Weg beim Winkel.** `armYaw` von 179° auf −179° zu glätten läuft
sonst 358° in die falsche Richtung. Vor dem Glätten die Differenz in
(−180, 180] falten:

```cpp
float d = std::remainder(targetYaw - armYaw, 360.0f);
armYaw += d * alpha;
```

**Springen statt segeln.** Ohne Ausnahme reist die Kamera bei jedem Teleport
quer durch das Level. Drei Anlässe zum harten Setzen:

* erstes Frame, in dem dieses Rig läuft (`hasLagState == false`) — sonst startet
  jeder Levelstart mit einem Anflug aus dem Weltursprung,
* `distance(pivotLagged, pivot) > lagSnapDistance` (Respawn, Teleport, Cut),
* explizit über `camera.snapRig()` — Skripte, die selbst umsetzen, wissen es
  vorher.

Dazu `lagMaxDistance`: der geglättete Pivot wird nach dem Glätten auf diesen
Abstand zum echten Pivot geklemmt, damit ein sehr schnelles Ziel die Kamera nicht
beliebig weit abhängt.

### 4.4 Weiches Nachdrehen der Kopplung

Der Rest aus `camera-rig-plan.md` §5: `TargetYaw::FollowSmoothed` dreht das Ziel
mit `turnRate` (°/s) auf `rig.yaw` zu, statt es hart zu setzen. Gleiche
Kürzester-Weg-Regel wie oben. Die bestehende Regel bleibt: geschrieben wird
**genau ein Float** (`tt.rotation.y`), nie ein Quaternion-Roundtrip.

### 4.5 Felder

```cpp
struct Lag {
    bool  enabled       = false;   // ← aus, siehe unten
    float positionSpeed = 10.0f;   // 1/s; groß = straff
    float rotationSpeed = 15.0f;   // 1/s, wirkt auf armYaw/armPitch
    float maxDistance   = 2.0f;    // m, Klemme des Rückstands
    float snapDistance  = 5.0f;    // m, darüber wird gesetzt statt geglättet
};
```

**Default `enabled = false`.** Kollision durfte an sein, weil sie ein explizites
Argument hat (kein `physics` → keine Kollision) und ihr Fehlen wie ein Defekt
aussieht. Lag hat kein solches Argument: eingeschaltet würde er jede der 27
bestehenden Zusicherungen um einen Frame verschieben und jede gespeicherte Szene
anders aussehen lassen. Aus heißt: der Umbau in §3 ist bei Default-Werten
bit-identisch zum heutigen Verhalten.

---

## 5. Camera Shake

### 5.1 Modell

Ein Shake ist ein additiver Pose-Offset mit Hüllkurve:

```cpp
struct ShakeInstance {
    uint32_t  id        = 0;       // Handle zum Stoppen; 0 = frei
    glm::vec3 posAmplitude{0.05f}; // m, Kameraraum
    glm::vec3 rotAmplitude{0.5f};  // Grad, Pitch/Yaw/Roll
    float     frequency = 12.0f;   // Hz
    float     duration  = 0.4f;    // s; <= 0 = bis zum Stoppen
    float     blendIn   = 0.05f;
    float     blendOut  = 0.15f;
    float     elapsed   = 0.0f;
    uint32_t  seed      = 0;
};
```

**Value Noise, kein Sinus.** Ein Sinus liest sich als mechanisches Wackeln mit
erkennbarer Periode; eine Explosion nicht. Eine hash-basierte Wertrauschfunktion
mit Smoothstep-Interpolation über `elapsed * frequency`, pro Achse mit eigenem
Seed-Versatz, ist deterministisch, braucht keinen Zustand außer `elapsed` und
belegt nichts. Sie ist damit auch testbar — dasselbe `elapsed` ergibt denselben
Offset.

**Roll gehört dazu.** Er ist der Anteil, der einen Treffer als Treffer lesbar
macht, und die einzige Achse, die das Rig sonst nirgends benutzt (`rotation.z`
ist im normalen Pfad immer 0).

**Hüllkurve:** `env = min(1, elapsed/blendIn) * min(1, (duration-elapsed)/blendOut)`,
bei `duration <= 0` entfällt der zweite Faktor. Ein Shake ohne Ende ist der
Fall „Motorbrummen, solange das Fahrzeug läuft" und braucht deshalb das Handle.

**Ort der Auswertung:** eigene Datei `HE_Scene/CameraShake.h/.cpp`, freie
Funktion `evaluateShakes(std::span<ShakeInstance>, float dt) -> ShakeOffset`.
Der Zustand bleibt in der Rig-Komponente, aber die Mathematik hängt an nichts —
so kann sie später auch eine Fly- oder Editor-Kamera bedienen, ohne umzuziehen.

### 5.2 Shake nach der Kollision, aber geklemmt

Der Offset kommt nach der Verkürzung dazu, sonst würde die verkürzte Distanz
selbst mitzittern und der Sweep hätte jeden Frame ein anderes Ergebnis. Damit
kann Shake die Kamera in einem verkürzten Frame minimal in eine Fläche drücken.
Regel dagegen: **in einem Frame mit `occluded == true` wird der positionale
Shake-Betrag auf `collisionRadius` geklemmt.** Der Sweep hält den Mittelpunkt
ohnehin einen Radius vor der Fläche frei — der Shake darf diesen Puffer
aufbrauchen und nicht mehr. Rotations-Shake bleibt unbeschränkt, er bewegt die
Kamera nicht.

### 5.3 Shakes über ein Blend hinweg

Entscheidung: **Shakes gehören der Kamera, nicht dem Spieler, und überleben ein
Blend nicht.** Wer die eingehende Kamera schütteln will, ruft `playShake` nach
`blendTo`. Der Grund ist Vorhersagbarkeit: das Übertragen wirft die Frage auf,
was mit einem endlosen Shake passiert, dessen Auslöser die alte Kamera war, und
jede Antwort darauf ist eine Regel, die niemand im Kopf hat. Ein Shake auf einem
Rig, das gerade Quelle eines Blends ist, läuft weiter und geht anteilig ins
Ergebnis ein — er verschwindet mit dem Blend, statt abrupt.

Feste Kapazität (`std::array<ShakeInstance, 8>`), damit die Komponente
kopierbar und allokationsfrei bleibt. Ist sie voll, ersetzt ein neuer Shake den
mit der geringsten Restenergie.

---

## 6. Blending zwischen Rigs

### 6.1 Nur auf Ansage

Ein Blend startet **ausschließlich** über `camera.blendTo(entityId, seconds, curve)`.
Ein direkt gesetztes `isMain` bleibt ein harter Schnitt, so wie heute. Die
Alternative — Umschalten erkennen — bräuchte ein „welche Kamera war letztes
Frame aktiv" irgendwo, und der Controller ist bewusst zustandslos.

`blendTo` hat eine **harte Pflicht**: es löscht `isMain` auf jeder anderen
Kamera. Die Eindeutigkeit von `isMain` ist heute reine Konvention
(`camera-rig-plan.md` §1), und sowohl `findRigCamera` als auch
`RenderExtractor::extractCamera` nehmen die **erste** gefundene `isMain` in
entt-View-Reihenfolge — die sich ändert, sobald ein Pool wächst. Zwei
`isMain`-Kameras mitten im Blend heißt: das Bild springt zwischen zwei Posen hin
und her, in unregelmäßigen Frames. Das ist kein Feinschliff, das ist die
Voraussetzung dafür, dass Blending überhaupt reproduzierbar ist.

### 6.2 Zustand auf dem eingehenden Rig

```cpp
struct Blend {
    entt::entity from      = entt::null;   // Quell-Kamera, Laufzeit
    float        remaining = 0.0f;         // s
    float        duration  = 0.0f;         // s
    enum class Curve { Linear, SmoothStep, EaseOut } curve = Curve::SmoothStep;
};
```

Interpoliert wird `SolvedPose`: Position linear, Rotation als **Slerp** über
Quaternionen, FOV linear. Euler-Winkel zu lerpen ist genau der Fehler, der beim
Wechsel über ±180° Yaw eine volle Drehung erzeugt.

`t = 1 - remaining/duration`, dann durch die Kurve. `SmoothStep` als Default —
ein linearer Blend hat an beiden Enden einen sichtbaren Knick.

### 6.3 Abbruchfälle

* Quell-Kamera zerstört oder ungültig → Blend sofort beenden, aktive Pose
  schreiben. Nicht „von der letzten bekannten Pose weiterblenden": die ist ein
  Frame alt und die Welt bewegt sich weiter.
* Ziel der Quell-Kamera nicht mehr auflösbar → dasselbe.
* Neues `blendTo` während eines laufenden Blends → das neue gewinnt, Quelle ist
  die **aktuell angezeigte (interpolierte)** Pose. Sonst springt es.
  Das braucht eine Kopie der zuletzt geschriebenen Pose auf dem eingehenden Rig
  (`lastWritten`, Laufzeit) — der einzige Fall, in dem das Rig eine ausgegebene
  Pose behält, und ausdrücklich als Kopie im Bauteil, nicht als Rücklesen aus
  dem Transform.
* `seconds <= 0` → Schnitt, kein Blend.

---

## 7. FOV-Kick

`CameraComponent.fovDegrees` bleibt der **gesetzte** Wert: der Inspector zeigt
ihn, der Serializer speichert ihn, `camera.getFov`/`setFov` meinen ihn. Ein Kick
hineinzurechnen wäre derselbe Akkumulationsfehler wie Shake im Transform, nur
zusätzlich in der gespeicherten Szene.

Deshalb ein additives Laufzeitfeld:

```cpp
struct CameraComponent {
    float fovDegrees   = 60.0f;
    ...
    // Laufzeit, NICHT serialisiert: additiver Versatz aus FOV-Kick und
    // Rig-Blending. Die Projektion benutzt fovDegrees + fovOffset; alles, was
    // "das FOV der Kamera" meint, meint weiter fovDegrees.
    float fovOffset    = 0.0f;
};
```

Ein Verbraucher: `RenderExtractor.cpp:87`, `glm::radians(cam.fovDegrees + cam.fovOffset)`.
`IRenderer.h:84` (`worldPreviewVerticalFov`) rechnet auf der render-seitigen
`CameraDesc`, einer anderen Struktur — die bekommt den Summenwert bereits fertig
und braucht keine Änderung.

Der Kick selbst ist dieselbe Hüllkurve wie ein Shake, ohne Rauschen:
`amplitude` (Grad, auch negativ für einen Zoom hinein), `attack`, `hold`,
`decay`. Ein Wert, ein Slot pro Rig — mehrere gleichzeitige Kicks addieren sich
in der Praxis zu Unsinn, ein neuer Kick ersetzt den laufenden.

Beim Blending: das eingehende Rig schreibt
`cam.fovOffset = blendedFov - cam.fovDegrees`, wobei `blendedFov` aus den beiden
`SolvedPose::fovDegrees` (jeweils Basis + Kick) interpoliert ist. So blendet auch
ein Wechsel von einer 60°- auf eine 90°-Kamera weich.

Wenn kein Rig aktiv ist, muss `fovOffset` **auf 0 zurückgesetzt** werden, sonst
bleibt ein Kick stehen, nachdem das Rig die Kamera abgegeben hat.

---

## 8. Skript-API

Neue Registry-Zeilen (`EngineApi.cpp`, Kategorie „Camera"), jede automatisch in
Lua, Python, HorizonCode und C++-Codegen:

| Zeile | Signatur |
|---|---|
| `camera.setLagEnabled` / `getLagEnabled` | `(bool)` / `-> bool` |
| `camera.setLagSpeeds` | `(float position, float rotation)` |
| `camera.snapRig` | `()` — Lag beim nächsten Frame setzen statt glätten |
| `camera.playShake` | `(float posAmplitude, float rotAmplitude, float frequency, float duration) -> int` (Handle) |
| `camera.stopShake` | `(int handle)` |
| `camera.stopAllShakes` | `()` |
| `camera.kickFov` | `(float degrees, float attack, float hold, float decay)` |
| `camera.blendTo` | `(int cameraEntity, float seconds, int curve)` |
| `camera.isBlending` | `-> bool` |

Alle mit demselben null-toleranten Vertrag wie die bestehenden Rig-Zeilen: kein
Rig auf der Hauptkamera → No-op bzw. Null.

---

## 9. Was jede neue Zeile sonst noch kostet

Die Buchhaltung, die in Schritt 3 und 4 gerne vergessen wird und die von einem
Test erzwungen wird:

* **Jede Registry-Zeile** braucht zusätzlich einen Anzeigenamen in der Liste bei
  `EngineApi.cpp:2954-2960` (dort steht der bestehende `camera.*`-Block) und
  einen Eintrag in `HcNodeDocs.cpp:540-570` — sonst steht sie im
  HorizonCode-Palettenmenü ohne Namen und ohne Beschreibung da.
* **Jedes neue Inspector-Bedienelement** braucht eine Zeile in `EditorHelp.cpp`
  unter `"Camera Rig/<Label>"`. Die Handbuch-Deckung ist ein ctest (600/600);
  ein Regler ohne Eintrag lässt die Testsuite fallen, nicht den Editor.
* **Jedes serialisierte Feld** braucht beide Richtungen in `SceneSerializer.cpp`
  (§263 save, §808 load). Binär ist CBOR derselben JSON-Struktur, also kein
  zweiter Pfad — aber `c.value("name", default)` beim Laden ist Pflicht, sonst
  bricht jede ältere Szene.

---

## 10. Testplan

Zu `tests/test_camera_rig.cpp` (die 27 bestehenden Fälle müssen unverändert
grün bleiben — das ist die Zusicherung, dass §3 nichts am Verhalten dreht):

**Lag**
1. Framerate-Unabhängigkeit: 1 Sekunde in 60 Schritten und in 120 Schritten
   landet auf derselben Pose (ε ≈ 1e-3). Der Test, den `lerp(a,b,speed*dt)`
   nicht besteht.
2. Erstes Frame setzt hart: eine Kamera mit Lag steht sofort richtig, nicht
   irgendwo zwischen Ursprung und Ziel.
3. Teleport über `snapDistance` setzt hart; ein Schritt knapp darunter glättet.
4. `maxDistance` klemmt: ein sehr schnell bewegtes Ziel hängt die Kamera nie
   weiter als den Grenzwert ab.
5. Yaw-Lag über ±180: von 179° auf −179° geglättet läuft über 180, nicht über 0.
6. Lag aus (Default) ergibt exakt die heutige Pose.

**Shake**
7. Deterministisch: gleiche Parameter, gleiches `elapsed`, gleicher Offset.
8. Nach `duration` ist der Offset exakt 0 und der Slot frei.
9. `duration <= 0` läuft weiter, `stopShake(handle)` beendet ihn.
10. In einem verkürzten Frame überschreitet der positionale Offset
    `collisionRadius` nicht.
11. Shake verschiebt `rig.yaw`/`rig.pitch` nicht — der Eingabezustand bleibt sauber.

**Blend**
12. Bei `t == 0` exakt die Quell-Pose, bei `t == 1` exakt die Zielpose.
13. `blendTo` hinterlässt genau **eine** Kamera mit `isMain`.
14. Yaw-Slerp über ±180 nimmt den kurzen Weg.
15. Quell-Kamera mitten im Blend zerstört → beendet sauber, kein Zugriff auf
    ungültige Entities.
16. Zweites `blendTo` während eines Blends startet bei der aktuell angezeigten
    Pose (kein Sprung).

**FOV**
17. `fovDegrees` wird nie geschrieben — nach hundert Kick-Frames unverändert.
18. `fovOffset` ist nach Ablauf des Kicks exakt 0.
19. Ein Rig, das aufhört zu treiben, setzt `fovOffset` auf 0 zurück.

**Serialisierung**
20. Save/Load-Roundtrip erhält die Regler und lässt Laufzeitfelder (Lag-Pose,
    Shakes, Blend) auf Default — inklusive einer Szene ohne die neuen Schlüssel.

---

## 11. Nebenfunde

* **Website-Roadmap ist veraltet.** `docs/gap-audit-2026-08-25.md:65`: „Gameplay
  Camera Rigs" steht dort auf `planned`, existiert aber seit `7ad81c48`.
  Gehört mit dieser Runde zusammen aktualisiert (`Website/HorizonEngine/roadmap.json`).
* **`camera-rig-plan.md` §5** liest sich als aktueller Stand („bewusst nicht in
  v1") — bekommt einen Verweis hierher, damit niemand daraus schließt, dass Lag
  und Shake nicht kommen.
* Der Nebenfund aus `camera-rig-plan.md` §6 (`camPos` für `SceneSystems::tick`
  nimmt die erste Kamera ohne `isMain`-Prüfung) ist unverändert offen und wird
  durch §6.1 hier wichtiger: sobald `blendTo` die Eindeutigkeit von `isMain`
  erzwingt, ist er leicht zu beheben.
