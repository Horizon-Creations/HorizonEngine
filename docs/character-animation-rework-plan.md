# Character- und Animations-Umbau

Umbau der Animations-Anbindung und der Klassen-Editor-Panels, Richtung Unreals
Blueprint-Pawn. Betrifft MASTERPLAN-Block **E5**.

---

## Ausgangslage

Die State Machine ist **autorierbar, aber nicht ansteuerbar** — und das steht so
nirgends. Der Masterplan listet unter E5, was zur Action-Game-Reife fehlt (Root
Motion, Layer, Blend-Trees, Notifies, IK) und liest sich damit als „Basis da,
Ausbau fehlt". Tatsächlich fehlt die Basis:

| Teil | Zustand |
|---|---|
| Laufzeit (`AnimationStateMachineSystem`) | funktioniert — States, Transitions, echter TRS-Crossfade, Parse-Cache, Laufzeitzustand pro Entity |
| Editor (`AnimatorStateMachineEditorPanel`, 421 Z.) | funktioniert — Canvas, Transitions ziehen, Clip-Slots, Default-Params, Collab-Sync |
| Tests | 8 Runtime- + 1 Graph-Roundtrip-Test |
| **Parameter setzen** | **existiert nicht.** Kein `animator.*` in der Registry, keine Lua-/Python-Bindung, kein HorizonCode-Knoten, kein Inspector-Feld |
| **Komponente anlegen** | **existiert nicht.** Nicht im Add-Component-Menü, nicht in `EntityHost::defaultComponents` |

Ein Setup „speed > 0.5 → Walk" wird also jeden Frame korrekt ausgewertet, und
`speed` bleibt für immer auf dem Default. Der Kommentar im System selbst
(`AnimationStateMachineSystem.cpp:78`) spricht von „a param a script already
tweaked at runtime" — er beschreibt eine API, die es nicht gibt.

**Nicht mehr aktuell:** Der Masterplan (Forts. 70) notiert, dass
`SkeletalMeshComponent`, `AnimatorComponent` und Nachbarn keine
Serializer-Anbindung haben und beim Speichern still verschwinden. Geprüft:
alle sieben speichern und laden heute. Das ist kein Blocker mehr.

---

## Das Modell

Connors Vorgabe, auf Unreal abgebildet:

| Unreal | Hier | Rolle |
|---|---|---|
| Animation Blueprint **EventGraph** | **Sync-Graph** | läuft pro Frame, liest die Figur, schreibt Parameter |
| Animation Blueprint **AnimGraph** | **State Machine** | liest Parameter, wählt Clip, blendet |
| Blueprint **Viewport / Components / Details** | neue Tabs im Klassen-Editor | siehe CP3/CP4 |

Zwei Regeln daraus:

1. **Der Sync-Graph ist der einzige Schreiber.** Die State Machine liest
   Parameter und schreibt sie nie — das ist heute schon so
   (`evalTransition` vergleicht nur) und bleibt eine Invariante durch
   Konstruktion, kein Laufzeit-Guard.
2. **Der Animations-Pass läuft nach dem Code-Pass.** Sonst liest die State
   Machine Werte von gestern.

---

## CP0 — Animation nach dem Code-Pass

**Das ist heute in PIE verletzt, und zwar genau andersherum als im Spiel.**

```
Spiel  (GameApplication::OnRender)    Editor (EditorApplication::OnRender)
  :937  updateScripts                   :1971  SceneSystems::tick   ← Animation ZUERST
  :944  Physik                          :1981  Physik
  :968  Kamera                          :1994  Kamera
  :983  playerHost.tick                 :2019  Skripte
  :985  entityHost.tick                 :2048  playerHost.tick
 :1009  SceneSystems::tick  ← Animation :2051  entityHost.tick
```

In PIE läuft der Animations-Pass also **vor allem anderen** — ein voller Frame
Nachlauf, und die umgekehrte Reihenfolge zur ausgelieferten Version. Ein
Preview, das eine andere Reihenfolge fährt als das Spiel, ist kein Preview.

**Umbau:** `SceneSystems::tick` in zwei Phasen teilen.

* `tickWorld` — Terrain, Navigation, Weather, GPU-Partikel, LOD. Läuft im Editor
  weiter **immer** (auch im Edit-Modus, wie heute).
* `tickAnimation` — `AnimationSystem`, `AnimationBlendSystem`,
  `AnimationStateMachineSystem`, `PropertyAnimationSystem`
  (`SceneSystems.cpp:95-98`, schon zusammenhängend).

Zielreihenfolge, in **beiden** Apps identisch:

```
Skripte → Physik → Kamera → Hosts → tickWorld → tickAnimation → Extraktion
```

Zwei Punkte, die dabei nicht untergehen dürfen:

* **`tickAnimation` muss vor der Extraktion bleiben.** Der Extractor konsumiert
  `boneMatrices` und `SkeletalMeshComponent::dirty`; nach hinten verschoben
  tauscht man einen Nachlauf gegen einen anderen.
* **`NavigationSystem` bewegt Transforms**, ist also Gameplay und gehört vor die
  Animation — in `tickWorld` ist es damit richtig einsortiert, aber es ist eine
  Entscheidung und keine Selbstverständlichkeit.

Testbar ohne Grafik: eine Welt mit FSM + Skript, das einen Parameter setzt, in
einer Runde ticken und prüfen, dass der Zustandswechsel **im selben** Frame
ankommt statt im nächsten.

---

## CP1 — Parameter setzen, und die Komponente anlegen können

Zwei kleine Teile, die zusammen das vorhandene Feature erst benutzbar machen.

**a) Registry-Zeilen** nach dem Muster von `material.setParam`
(`EngineApi.cpp:1479`):

```
animator.setParam(entity, name, value)
animator.getParam(entity, name) -> float
animator.getState(entity)       -> string   (Diagnose, read-only)
```

Gruppe `"animator"`, dazu in `isScriptGroup` — damit sind Lua, Python,
HorizonCode und der C++-Codegen auf einen Schlag bedient.

Diese Zeilen werden vom Sync-Graph (CP2) **nicht ersetzt**: ein Lua- oder
Python-Projekt hat keinen Sync-Graph und braucht trotzdem einen Weg an die FSM.
Der Sync-Graph ist der HorizonCode-native Pfad, die Registry der
sprachunabhängige.

**b) Die Komponente anlegbar machen.** Sie steht heute bewusst nicht im
Add-Component-Menü, mit Verweis auf einen „owning asset workflow" — den es nicht
gibt. Außerhalb von Serializer und Tests existiert im ganzen `src/` kein
`emplace<AnimatorStateMachineComponent>`.

**Die Regel ist die Abhängigkeit, nicht die Klasse:** eine State Machine
animiert ein Skelett, also gehört sie dorthin, wo eines ist. Der Menüeintrag
erscheint für Entities mit `SkeletalMeshComponent` und sonst nicht. Damit ist
sie auch an einer normalen Szenen-Entity verfügbar, ohne dass man sie „lose an
irgendwas" hängen kann.

---

## CP2 — Der Sync-Graph

Das Animator-Asset bekommt einen zweiten Graphen. Vorbild ist
`HorizonCodeClassAsset` (`Assets.h:296`), das schon `graphJson` + `baseClass` +
`componentBlob` trägt — hier also `graphJson` (die FSM) + `syncGraphJson`.

**Funktionsumfang bewusst eng.** Der Graph hat genau ein Event (`Update`, mit
`dt`), erreicht seinen Besitzer, und schreibt Parameter. Er soll keine Szenen
wechseln, nichts spawnen, nichts speichern.

Durchgesetzt über zwei vorhandene Mechanismen:

* die Knoten-Kategorien pro Host (`LevelScriptPanel.cpp:259` und
  `UIEditorPanel.cpp:1139` führen je eigene Listen) — für den Sync-Host also
  `Flow, Literals, Math, Logic, Variables, Debug`.
* eine Erlaubnisliste für Engine-Calls. `HcEditorUtil::drawEngineApiMenu`
  (`HcEditorUtil.cpp:844`) surft heute die **ganze** Registry ungefiltert; ein
  optionaler Gruppenfilter ist die kleinste Erweiterung. Erlaubt:
  `player`, `entity`, `transform`, `physics` (nur Lesezugriffe), `math`,
  `time`, `animator`.

**Wo die Parameter leben.** Sie bleiben auf der **Komponente**
(`AnimatorStateMachineComponent::params`, `map<string,float>`, pro Entity) —
nicht als Graph-Variablen, die pro Runtime-Instanz liegen. Der Sync-Graph
schreibt sie über dieselben `animator.setParam`-Zeilen aus CP1, nur mit seinem
Besitzer als Entity. Damit gibt es **einen** autoritativen Speicher und keine
zweite Wahrheit, die synchron gehalten werden müsste.

**Nicht vergessen — der Grund, warum E0 wehgetan hat:** der Sync-Graph muss
durch dieselbe Interpreter/Compiled-Dualität wie jeder andere Graph. Er
registriert sich bei der `Runtime` wie eine Klasse, wird vom Export
mitgesammelt, und läuft im gepackten Spiel kompiliert. Ein Graph-Asset, das
nicht ausgeliefert und verdrahtet wird, feuert dort nichts —
`GameInstance.hcode` hat das einmal vorgemacht.

---

## CP3 — Klassen-Editor: Code und Viewport trennen

*(Detailplanung folgt, sobald die Bestandsaufnahme der Panels vorliegt.)*

Zielbild nach Unreals Blueprint-Editor:

```
┌──────────────┬─────────────────────────────┬──────────────┐
│ Components   │  [Viewport] [Code]          │ Details      │
│              │                             │              │
│ Player       │   3D-Vorschau der Klasse    │ Eigenschaften│
│ ├ Capsule    │   mit Mesh, Collider,       │ der Auswahl  │
│ ├ SkelMesh   │   Kamera-Arm                │              │
│ └ Camera     │                             │ z.B. welche  │
│              │                             │ State Machine│
└──────────────┴─────────────────────────────┴──────────────┘
```

---

## CP4 — Viewport-Vorschau

*(Detailplanung folgt.)*

---

## Reihenfolge

CP0 und CP1 sind klein, voneinander unabhängig und schalten alles Weitere frei —
nach CP1 ist die State Machine zum ersten Mal aus Gameplay-Code bedienbar. CP2
ist der eigentliche Architektur-Schritt. CP3/CP4 sind der lange Teil; die
Viewport-Vorschau ist die Unbekannte.
