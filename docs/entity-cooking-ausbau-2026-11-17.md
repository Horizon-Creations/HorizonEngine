# Entity-Cooking Schritt 11: was sich am Cooking noch ausbauen lässt

Stand 17.11.2026, Thema 343, HEAD `f218b0f4a`. Reiner Untersuchungsschritt (kein Code geändert),
nach demselben Muster wie Schritt 7 (Regel 11 entfallen, `docs/entity-cooking-streaming-2026-11-17.md`)
und Schritt 8 (Regel 8 präzisiert, `docs/entity-cooking-lod-2026-11-17.md`): für jede genannte Regel
entweder ein konkreter Entwurf oder eine begründete Absage, nur mit Code-Beleg.

## 1. Regel 7 (`SaveStateComponent`) — **lockerbar, Entwurf unten**

### Befund

`SaveStateComponent` ist reines Opt-in: die einzigen Schreiber sind `entity.saveState` (liest nur)
und `entity.applySavedState` (`EngineApi.cpp:657-669`, `applyStateJson` `:572-621`), beide
script-aufrufbar, beide gated auf Play-Mode (`saveStateGuard`, `:416-418`). Kein System tickt
`SaveStateComponent` jeden Frame (`grep SaveStateComponent src/HE_Scene/src/SceneSystems.cpp` —
kein Treffer). `applyStateJson` schreibt genau drei Dinge, und alle drei sind bereits durch
bestehende, regelunabhängige Mechanismen abgedeckt:

1. **Transform** (`EngineApi.cpp:577-613`, roh `tc->position/rotation/scale`): das ist exakt der Weg,
   den Schritt 4 schon behandelt — `propagateTransforms` vergleicht die geschriebene Weltmatrix jeder
   `Ready`-Zeile byteweise (`docs/entity-cooking-uncook-2026-11-16.md` Zeile 1, `worldWritten`), ganz
   unabhängig davon, WER geschrieben hat. Ein `applySavedState`-Transform-Restore wird also so sicher
   erkannt wie jeder andere Transform-Schreiber (Gizmo, Skript, Kollaboration).
2. **Sichtbarkeit** (`EngineApi.cpp:615-616`, ruft `setVisible(c, e, …)` → `setEntityVisible`
   → `EntityVisibility.cpp:19`, roh `m->visible = visible`): dieser rohe Schreibzugriff löst **kein**
   `entt`-Signal aus (bestätigt die Lektion zu `EntityVisibility.cpp` in `.hive/kontext.md`) — aber er
   muss es auch nicht, weil `RenderExtractor.cpp:529` `meshView.get<MeshComponent>(e).visible` **für
   jede Entity, gekocht oder nicht**, in Phase 1 des Walks jedes Frame frisch liest (Kommentar
   `:552`: "visible / Active were decided in phase 1"). Eine gekochte Zeile cacht nur Mesh/Material/
   Bounds (`OutputComponents`, `CookedStatic.cpp:67`), nie den Sichtbarkeits-Filter selbst. Ein
   `applySavedState`, das eine Entity ausblendet, funktioniert für eine gekochte Entity schon heute
   korrekt, ganz ohne Entkoch-Hook.
3. **Skript-Variablen** (`applyScriptVars`, `EngineApi.cpp:500-528`): wirkt nur, wenn
   `classInstanceOf(c, e)` eine laufende HorizonCode-Instanz findet; `EntityHost` bindet solche
   Instanzen ausschließlich über `view<ScriptComponent>()` (`EntityHost.cpp:52`) — eine Entity ohne
   `ScriptComponent` hat nie eine Instanz, `applyScriptVars` loggt dann nur eine Warnung und schreibt
   nichts. Jede Entity, für die dieser Pfad überhaupt etwas tut, trägt also schon `ScriptComponent`
   und ist bereits über Regel 1 blockiert — Regel 7 schützt hier vor nichts, was Regel 1 nicht ohnehin
   abdeckt.

Gegengeprüft, dass ein `replaceStaticMesh`/`registerStaticMesh` (relevant für den Rope/Terrain-Teil
unten, nicht für SaveState) keine globale Invalidierung auslöst: `ContentManager::replaceRuntimeAsset`
(`ContentManager.cpp:2756-2774`) und `registerRuntimeAsset` (`:2738-2753`) rufen `noteContentChanged()`
nie — `contentEpoch` bewegt sich nur bei echten Asset-Neuimporten (`:1402`, `:2414`, `:3456`), nicht bei
einem Laufzeit-Replace. Für SaveState selbst ist das nicht der Weg; genannt, weil derselbe Beleg auch
Abschnitt 3 unten trägt.

### Entwurf für einen Folgeschritt

Regel 7 entfernen, analog zu Regel 11 (Schritt 7):

- `CookAnalysis.h`: `CookBlocker::SaveState` (`1 << 6`) löschen (Bit-Lücke bleibt, keine
  Umnummerierung — exakt wie `1 << 10`/Streaming seit Schritt 7 eine Lücke ist); Kommentarblock
  Zeile 17-23 um einen Satz "Regel 7 entfallen, siehe Schritt 11" ergänzen.
- `CookAnalysis.cpp`: die Zeile `if (reg.all_of<SaveStateComponent>(e)) …` (`:63`) und das
  `#include "HorizonScene/Components/SaveStateComponent.h"` (`:23`) entfernen.
- `CookedStatic.cpp`: `SaveStateComponent` aus `VerdictComponents` (`:64`) entfernen — sonst löst ein
  späteres Hinzufügen/Entfernen der Komponente weiterhin unnötig `stale` aus, obwohl das Bit nicht mehr
  existiert (dieselbe Pflicht, die Schritt 7 für `StreamingComponent` schon notiert hat, `:51` dort).
- `tests/test_cook_analysis.cpp`: den "7 savestate"-Fall umkehren (kochbar statt blockiert), analog zum
  "11 streaming"-Fall aus Schritt 7.
- `docs/entity-cooking-survey-2026-11-13.md` §5 Regel 7: "entfallen (Schritt 11, …)" vermerken.

Kein Entkoch-Hook an `saveState`/`applySavedState` nötig — die drei Schreibwege sind, wie oben belegt,
bereits durch den Transform-Vergleich, den Live-Sichtbarkeits-Filter bzw. Regel 1 gedeckt. Nicht
geprüft: Play-Mode-Lauf mit echtem Save/Load auf einer gekochten Entity (kein Play-Mode in
`he_perf_capture.py`, dieselbe Lücke wie Schritt 1 §4.2 sie schon nennt).

## 2. Regel 8 Rest (`refinedMeshId` / Terrain-Tessellation) — **begründete Absage**

### Befund

`TerrainSystem::updateTessellation` (`TerrainSystem.cpp:806-951`) läuft jeden Tick aus
`SceneSystems.cpp:120` heraus (`HE_PROFILE_SCOPE_N("Terrain")`, kein Gate außer "Terrain hat
`tessellationDistance > 0`). Für jeden Chunk einer tessellierenden Landschaft wird jeden Tick die
Kameradistanz neu bestimmt (`:890`) und gegen `tessellationDistance * kTessBuildMargin/kTessReleaseMargin`
geprüft (`:901-933`) — ein Chunk am Rand der Tessellationsdistanz wechselt dabei fortlaufend zwischen
`fillTessLevel` (setzt `refinedMeshId`, `:316`) und `emptyTessLevel` (löscht es, `:268`), budgetiert auf
`kTessBuildsPerTick = 2` (`:149`) neue Builds pro Tick, nicht auf Null. Genau das ist der Kern des
0.8.0-Release-Ziels (100×100-km-Flug mit Tile-Streaming) — der Kameraflug durch eine tessellierende
Landschaft hält die Chunks nahe der Grenze dauerhaft in Bewegung, nicht selten/nie. Das ist keine
Annahme, sondern derselbe Mechanismus, den Schritt 8 schon für die `refinedMeshId`-Ausnahme zitiert hat
(`CookAnalysis.cpp:66`, Kommentar "TerrainSystem hooks it in and out at runtime").

Eine Klarstellung, die Schritt 8 noch nicht ausdrücklich macht: `refinedMeshId` wird **nicht**
serialisiert (`grep -n refined src/HE_Scene/src/SceneSerializer.cpp` — kein Treffer; `LODComponent.h:26`
führt es als reines Laufzeitfeld). Ein frisch geladener Chunk hat also `refinedMeshId == {}` und besteht
Regel 8 zunächst — er wird gekocht, bis `TerrainSystem` beim ersten Mal in Tessellationsreichweite kommt
das Feld setzt. Der eigentliche Rückfall auf `Walk` läuft dabei über den Fast-Pfad-Mechanismus aus
Schritt 8 (RenderExtractor.cpp:349/560-590): sobald `LODSystem::update` die gezeichnete Mesh auf die
verfeinerte umschaltet, passt sie zu keinem der in der Zeile gehaltenen Level mehr, die Zeile läuft im
Walk und wird neu gesettelt — nicht die Regel-8-Analyse selbst erkennt das live, sondern der
Mesh-Id-Abgleich, der für jede LOD-Zeile ohnehin läuft. Das ist eine Bestätigung des bestehenden
Mechanismus, kein neuer Fund, und ändert nichts an der Absage: der Chunk-Bereich, der den Nutzen
brächte (nahe der Tessellationsgrenze), ist exakt der, der am häufigsten umschaltet.

**Nicht erneut geprüft** (liegt in Schritt 8s eigenem, getestetem Bereich, hier nur zitiert): ob der
Fast-Pfad→Walk-Übergang im allerersten Refined-Frame einen Frame Verzögerung hat. Schritt 8s Tests
("Kamerafahrt durch beide Grenzen", 22 Fälle, siehe `docs/entity-cooking-lod-2026-11-17.md`) decken das
bereits ab.

### Warum kein Entwurf

Anders als bei LOD-Leveln (Schritt 8) oder Streaming (Schritt 7) gibt es hier keine "selten geänderte"
Eigenschaft, die sich vom volatilen Teil trennen ließe: die Zeilen, die vom Kochen profitieren würden
(tessellationsfähige Chunks), sind beim Release-Flaggschiff-Szenario (Kameraflug) laufend in Bewegung.
Ein Cooking, das sofort nach dem ersten Settle wieder zurückfällt, kostet mehr (Settle-Overhead plus
Walk) als es spart. Schritt 8s Entscheidung (nur `refinedMeshId == {}` kocht) bleibt die richtige Grenze.

## 3. Regel 9 (`TrailComponent`/`RopeComponent`) — **für die Entity selbst Absage, Ancestor-Entkopplung ist der Entwurf**

### Befund: die Regel kostet die Entity selbst nichts (mehr)

`extractRopes` (`RenderExtractor.cpp:963-986`) und `extractTrails` (`:996-1015`) laufen über eigene
`view<TransformComponent, RopeComponent>()`/`view<TrailComponent>()` — **nicht** über `extractMeshes`.
Der Header beider Komponenten sagt es ausdrücklich: "Deliberately NOT a MeshComponent on the same
entity" (`RenderExtractor.cpp:960-961`). Da eine kochbare Zeile laut Survey §5 ein `MeshComponent`
voraussetzt, hätte eine Rope/Trail-Entity nie eine `CookedStatic`-Zeile bekommen, Regel 9 oder nicht —
sie blockiert für die Entity selbst nichts, was nicht schon durch das Fehlen von `MeshComponent`
feststeht. `extractRopes` selbst ist zudem schon billig (nur UUID-Referenz, keine
`resolveEntitySlots`-Auflösung wie `extractMeshes`), und ein `replaceStaticMesh` beim Rope-Rebuild
(`RopeTrailSystem.cpp:346`) bewegt `contentEpoch` nicht (siehe Beleg oben, Abschnitt 1) — es invalidiert
also auch keine fremden Zeilen.

Geprüft, ob Rope-Attachments (anders als Joint-Targets) überkonservativ in §6 einsortiert sind: nein.
`resolveControlPoints` (`RopeTrailSystem.cpp:64-84`) liest nur die Weltposition der Attachment-Entity
(`HE::worldPositionOf`, `:82`), schreibt nie zurück — ein Attachment bleibt kochbar, exakt wie §6 es für
Joint-Targets und CameraRig-Ziele schon festhält.

### Befund: die Regel vergiftet unbeteiligte Kinder über Regel 10

Der eigentliche Kostenpunkt ist `lineBlocked` (`CookAnalysis.cpp:84-97`): `bool blocked =
ownBlockers(reg, e) != 0` behandelt JEDEN gesetzten Blocker gleich, wenn ein Vorfahre geprüft wird —
unabhängig davon, ob der Blocker etwas über die Weltmatrix der Entity aussagt oder nur über ihren
Render-Inhalt. `RopeTrailSystem::update` (`RopeTrailSystem.cpp:284-359`) schreibt niemals
`TransformComponent` der Rope/Trail-Entity selbst — nur `rope.runtimeMeshId`/`builtHash` bzw.
`trail.points`. Eine Rope/Trail-Entity, die als Gruppenknoten im Outliner über statischen
Dekorationsmeshes sitzt (z. B. eine hängende Kette mit einem Schild als Kind), blockiert deren Kochen
heute über Regel 10, obwohl ihre eigene Weltmatrix so stabil ist wie die jeder gewöhnlichen
`MeshComponent`-Entity.

### Entwurf für einen Folgeschritt

`ownBlockers` behält das `Geometry`-Bit für die Entity selbst (kostet nichts, da ohnehin kein
`MeshComponent`), aber `lineBlocked` soll beim Vorfahren-Walk ein zweites, engeres Prädikat benutzen,
das `CookBlocker::Geometry` ausklammert:

```cpp
// Blockers that say the ancestor's WORLD TRANSFORM may change — Geometry (Trail/Rope) only
// rebuilds the entity's OWN render content, never its transform (RopeTrailSystem.cpp never
// writes TransformComponent of the rope/trail entity itself).
constexpr uint16_t kTransformBlockers = ~bit(CookBlocker::Geometry);

bool lineBlocked(const entt::registry& reg, entt::entity e, AncestorMemo* memo)
{
    ...
    bool blocked = (ownBlockers(reg, e) & kTransformBlockers) != 0;
    ...
}
```

Touch-Liste: `CookAnalysis.cpp` (`lineBlocked`, neue Konstante), `tests/test_cook_analysis.cpp` (neuer
Fall: statisches Mesh-Kind unter einem Rope/Trail-Elternknoten bleibt kochbar), Survey §5 Regel 9 um den
Ancestor-Befund ergänzen.

**Verwandter, hier nicht geprüfter Punkt:** derselbe `ownBlockers(reg, e) != 0`-Mechanismus
(`CookAnalysis.cpp:89`) behandelt auch die Regeln 8 (Lod) und 12 (Switchable) gleich — falls deren
Blocker ebenfalls nur den Render-Inhalt und nicht die Transform-Stabilität der Entity selbst betreffen,
hätten sie dasselbe Ancestor-Problem. Nicht untersucht, weil außerhalb der für diesen Schritt genannten
Regeln; ein Kandidat für einen eigenen Folgeschritt, keine Vermutung ohne eigene Prüfung der jeweiligen
Schreibpfade.

## 4. `CameraRigComponent`-Ziel und `SplineComponent` — Fazit aus §5/§6 bestätigt sich in der Praxis, mit einer Korrektur und einem neuen Befund

### Das Ziel bleibt kochbar — aber §6s Begründung ("liest nur, schreibt nie") ist falsch

`CameraRigController.cpp` schreibt sehr wohl auf die TARGET-Entity, in zwei Fällen, beide im Header von
`CameraRigComponent.h` selbst dokumentiert:

- `targetYaw == Follow` (`CameraRigController.cpp:381-387`): `tt.rotation.y = rig.yaw;` — roh, jeden
  Frame, auf `TransformComponent` der Ziel-Entity.
- `targetYaw == FollowSmoothed` (`:399-404`): `tt.rotation.y += clamp(delta, -step, step);` — ebenso
  jeden Frame, nur gedämpft statt hart gesetzt.
- `hideTargetMesh` (First-Person, `:569-576`, `applyMeshVisibility` `:28-38`): roh
  `m->visible = !hide; m->dirty = true;` auf `MeshComponent` der Ziel-Entity, beim Betreten/Verlassen
  First-Person.

Trotzdem bleibt die Schlussfolgerung ("Ziel bleibt kochbar") richtig — nur aus einem anderen Grund als
§6 nennt: beide Schreibwege laufen durch die bereits bestehenden, regelunabhängigen Sicherungen aus
Abschnitt 1 oben — der Transform-Schreibzugriff durch den `worldWritten`-Bytevergleich (Schritt 4), der
Sichtbarkeits-Schreibzugriff durch den Live-Filter in `RenderExtractor.cpp:529`. Ein Ziel mit aktivem
`Follow`/`FollowSmoothed` würde seine Zeile deshalb laufend `Pending`↔`Ready` wechseln (korrekt, nur
nicht besonders nützlich gekocht) — kein Korrektheitsproblem, aber die Begründung in
`docs/entity-cooking-survey-2026-11-13.md` §6 ("schreibt sie nicht") sollte auf diese drei Zeilen
korrigiert werden, sonst verlässt sich der nächste Schritt auf eine falsche Prämisse.

### Neuer Befund: die RIG-Entity selbst fehlt als Blocker

`CameraRigController.cpp:531-548` schreibt JEDEN Frame, in dem der Rig aktiv ist, `ct.position`/
`ct.rotation` der KAMERA-Entity selbst (`f.camera`, die Entity, die `CameraRigComponent` trägt) —
dieselbe Transform-Änderung, die Regel 5 (Locomotion) für `MovementComponent` & Co. bereits als Blocker
führt. `CookAnalysis.cpp:42-76` prüft `CameraRigComponent` an keiner Stelle. Eine Rig-Entity, die
zusätzlich ein `MeshComponent` trägt (eine sichtbare Kameragehäuse-/Drohnen-Mesh — ungewöhnlich, aber von
keiner Regel ausgeschlossen), würde heute als kochbar durchgehen, dann aber jeden Frame über den
Transform-Vergleich sofort wieder `Pending`, bevor die Zeile je genutzt wird — das ist der worst case,
den Entity-Cooking vermeiden soll: Kochbuchhaltung (Zeilen-Lookup, Pending-Markierung, Neubau) zusätzlich
zum ohnehin fälligen Walk, nie ein tatsächliches `Ready`.

### Entwurf für einen Folgeschritt

`ownBlockers` um einen Check auf `CameraRigComponent` der Entity selbst ergänzen (Locomotion-Bit
wiederverwenden, da semantisch dieselbe Aussage — "schreibt die eigene Transform laufend"):

```cpp
if (reg.all_of<CharacterControllerComponent>(e) || reg.all_of<NavAgentComponent>(e) ||
    reg.all_of<MovementComponent>(e) || reg.all_of<ParticleSystemComponent>(e) ||
    reg.all_of<CameraRigComponent>(e))
    b |= bit(CookBlocker::Locomotion);
```

Touch-Liste: `CookAnalysis.cpp` (`#include CameraRigComponent.h`, obige Zeile), `CookAnalysis.h`
(Kommentar zu Bit 5 um "CameraRig" ergänzen), `CookedStatic.cpp` (`CameraRigComponent` zu
`VerdictComponents`, damit ein später hinzugefügter Rig eine bereits gekochte Kameragehäuse-Entity
sofort entkocht), `tests/test_cook_analysis.cpp` (neuer Fall). Kein Performancegewinn zu erwarten (Rigs
mit eigenem `MeshComponent` sind ein Rand-/Editorfall), aber verhindert die oben beschriebene
Buchhaltungs-Verschwendung — eine Korrektheits-/Sauberkeits-Lücke, kein Hot-Path-Fund.

### `SplineComponent`: bestätigt, kein Entwurf nötig

Breiter nachgesucht als nur Zuweisungen auf `controlPoints`/`closed` (`FloatingOrigin.cpp`,
`MovementSystem.cpp`, `SequenceSystem.cpp`, `EntityHost.cpp` — kein Treffer in allen vieren). Die
einzigen Laufzeit-Stellen sind `WaterLake.cpp` (liest `SplineComponent` für den Seen-Sync, schreibt sie
nie; `convertBody` `:793-830` erzeugt eine NEUE Entity mit frischer `SplineComponent` einmalig bei der
Konvertierung, nicht als laufende Mutation einer bestehenden) und `EngineApi.cpp:1446-1457`
(`splineOf`, nur eine Existenzprüfung für `terrain.makeLake`, liest die Kontrollpunkte, schreibt nichts).
Eine `SplineComponent`-Entity hat ohnehin kein `MeshComponent` (reine Autorendaten, "renders nothing"),
ihre Kochbarkeit ist also wie bei Rope/Trail nur über Regel 10 für etwaige Kinder relevant — und da
nichts sie zur Laufzeit schreibt, ist sie dort korrekt unblockiert. Keine Änderung nötig.

## Umsetzung (Schritt 13, 18.11.2026)

Die drei Entwürfe aus §1, §3 und §4 sind gebaut; Survey §5/§6 ist mitgezogen.

- **Regel 7 entfernt:** `CookBlocker::SaveState` (`1 << 6`) gelöscht, Lücke bleibt; `SaveStateComponent` raus aus `ownBlockers`
  und `VerdictComponents`. `CookAnalysis.h` nennt Regel 7 neben Regel 11 als entfallen.
- **Regel 9, Vorfahren-Check:** `lineBlocked` blendet `Geometry` aus (`kAncestorIgnored`); die Entity selbst behält das Bit.
  Rope/Trail über einem Kind: Kind kochbar, Rope/Trail blockiert, ein Script auf dem Rope/Trail gibt weiter `Ancestor` an die Kinder.
- **CameraRig:** `CameraRigComponent` auf der Entity selbst setzt das Locomotion-Bit; `CameraRigComponent` ersetzt `SaveStateComponent`
  in `VerdictComponents`. Beide Listen (`CookAnalysis.cpp` `ownBlockers`, `CookedStatic.cpp` `VerdictComponents`) sind synchron.
- **Befund zur Fragestellung "rendert veraltet?":** nein. Ohne den Blocker bekam die Rig-Entity eine Zeile, der echte
  `CameraRigController::update` schrieb die Transform, `propagateTransforms` erkannte das per Byte-Vergleich, und der Extract
  war Feld für Feld gleich dem Walk (der Test lief rot nur an `rowOf(cam) < 0`, nicht an der Ausgabe). Der Blocker spart also
  die Pending-Buchhaltung je Frame, korrigiert aber keinen falsch gezeichneten Frame.
- **Tests:** `test_cook_analysis` (Regel 5 mit CameraRig, Regel 7 kochbar, neuer Fall Regel 9 Vorfahre), `test_cooked_static`
  (SaveState-Entity gekocht, `saveState` → Verschieben → `applySavedState` zeigt die gespeicherte Position und `applyState`
  mit `visible:false` blendet aus; CameraRig-Entity mit echtem Controller über vier Frames; `addAndCheck<CameraRigComponent>`).

## Pixel-A/B (Schritt 14, 18.11.2026)

Done-Kriterium 4 aus dem Gate von Schritt 13 verlangt den `HE_COOK_STATIC=1`-Pixel-A/B auf einer Szene mit
CameraRig, einem Trail/Rope mit Kindern und einer SaveState-Entity — die drei Fälle, die Schritt 13 im
Cooking geändert hat. Es gab dafür noch keine Dump-Szene; `scripts/perf/frame_pixel_ab.sh` ist zudem
macOS/Metal-only (zsh, `/sbin/md5`, `RHI=Metal` fest) und lief auf diesem NN-WS03-Windows-Rechner nicht.

**Neue Witness-Szene:** `HE_DUMP_COOKTEST=1` in `EditorApplication.cpp` (neben `HE_DUMP_ROPETEST`,
gleiches Muster: `createEntity`/`TransformComponent`/`MeshComponent{HE::kDefaultCubeMeshId}`):
- `CookTestFloor`, eine flache Bodenplatte — fängt die Schatten der drei Test-Würfel auf, sonst hätte der
  Schattenpfad (`fitDirectionalShadow`, liest `worldBounds`/`castsShadow` einer gekochten Zeile) nichts zu
  zeigen.
- `CookTestRopeChild0`/`CookTestRopeChild1`, zwei Mesh-**Kinder** (Mehrzahl, wie im Done-Kriterium) von
  `CookTestRopeParent` (trägt `RopeComponent`, kein eigenes `MeshComponent` — die Rope-Geometrie selbst
  zeichnet über `RopeComponent::runtimeMeshId`, einen Pfad, den Cooking nicht anfasst, deshalb hier nicht
  über `RopeTrailSystem::update` materialisiert). Prüft Regel 9 (Ancestor-Entkopplung, `kAncestorIgnored`):
  der Elternteil ist selbst blockiert (Geometry), die Kinder nicht.
- `CookTestRig`, trägt `CameraRigComponent` direkt. Prüft den neuen CameraRig-Blocker (Locomotion): die
  Entity darf **keine** Zeile bekommen.
- `CookTestSaveState`, trägt `SaveStateComponent`. Prüft Regel 7 (entfernt): die Entity kocht wie jedes
  andere statische Mesh.

Zusätzlich, direkt vor dem BMP-Schreiben (nach den `HE_DUMP_FRAMES`-Settle-Frames, `EditorApplication.cpp`
bei der Zeile `std::vector<uint8_t> rgba;`): ein Log pro `CookTest*`-Entity mit `rowOf`/`state`, über
`reg.ctx().find<HE::CookedStatic>()` — **nicht** `cookedStaticOf`, das den Block anlegen und damit den
`HE_COOK_STATIC=0`-Lauf durchs bloße Nachschauen kochen würde. Ohne diesen Zeilen-Beleg wäre ein
Pixel-Gleichstand mehrdeutig: er würde genauso aussehen, wenn das Cooking in dieser Szene nie angesprungen
wäre.

**Lauf:** `cmake --build C:\hwtp9 --target HorizonEditor -j 8` (Scene-DLL frisch verlinkt, neuer als
`11104a557`), dann `HorizonEditor.exe` direkt mit `HE_DUMP_PATH`/`HE_DUMP_QUIT` (kein `he_shot.py` — das
Skript ist Metal/macOS-spezifisch und hätte für diesen Lauf erst umgeschrieben werden müssen, was außerhalb
des Schritts liegt). Gemeinsame Flags: `HE_CONFIG_DIR`/`HE_COLLAB_OFFLINE=1` (privat), `HE_HIDDEN_WINDOW=1`,
`HE_SKY_TIME=30`, `HE_DUMP_SKYTEST=1 HE_DUMP_COOKTEST=1`, `HE_DUMP_RHI=D3D11`, `HE_DUMP_AA=0`,
`HE_DUMP_CAMY=2 HE_DUMP_CAMZ=2 HE_DUMP_PITCH=0`, `HE_DUMP_FRAMES=5` (Standard 3, hier mehr Abstand).

*Himmel/Wolken sind auf diesem Build nicht reproduzierbar* — zwei `HE_COOK_STATIC=0`-Läufe mit
`HE_DUMP_TOD=0.5 HE_DUMP_COVERAGE=0.3` (wie `DC`/`SI` in `frame_pixel_ab.sh`), sonst ohne jede
Codeänderung, unterschieden sich in 222741 von 3686454 Bytes. Eingegrenzt in zwei Schritten:
1. `HE_DUMP_NOSKY=1 HE_DUMP_CLOUDMODE=0 HE_DUMP_COVERAGE=0` (kein Himmel) — zwei Läufe byte-gleich.
2. Himmel wieder an, nur Wolken aus (`HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0 HE_DUMP_TOD=0.35`, kein
   `NOSKY`) — ebenfalls zwei Läufe byte-gleich. Also sind die Wolken (nicht der Himmel/die Sonne an sich)
   die Quelle — deckungsgleich mit dem Vorbehalt im Kommentarkopf von `frame_pixel_ab.sh` ("GI and the
   low-resolution clouds are NOT here: they are not bit-reproducible"), hier für D3D11 statt Metal neu
   angetroffen. Nicht untersucht, WAS an der Wolkenberechnung auf D3D11 nicht an `HE_SKY_TIME` hängt —
   außerhalb dieses Schritts, da kein Cooking-Code.

Alle A/B-Läufe liefen deshalb mit Sonne an, Wolken aus (`HE_DUMP_COVERAGE=0 HE_DUMP_CLOUDMODE=0
HE_DUMP_TOD=0.35`), damit der Schattenpfad (Sonne, nicht nur Ambient) tatsächlich mitläuft — zwei
Render-Pfade, je mit Rauschboden:

| Lauf | RENDERPATH | `HE_COOK_STATIC` | md5 |
|---|---|---|---|
| `fwd_off1` | 0 (forward) | unset | `02b09ca4…` |
| `fwd_off2` | 0 (forward) | unset (Rauschboden) | `02b09ca4…` |
| `fwd_on1`  | 0 (forward) | `1` | `02b09ca4…` |
| `def_off1` | 1 (deferred) | unset | `45de121b…` |
| `def_on1`  | 1 (deferred) | `1` | `45de121b…` |

Jede Zeile einer Spalte ist identisch — der Rauschboden (`fwd_off1`/`fwd_off2`) UND der Cooking-Vergleich
(`*_off*` gegen `*_on1`) auf beiden Pfaden.

**Zeilen-Beleg (log, nicht nur Pixel):**
```
fwd_off1 (HE_COOK_STATIC unset — kein Block):
  CookTestSaveState    row=-1  state=no row
  CookTestRig          row=-1  state=no row
  CookTestRopeChild1   row=-1  state=no row
  CookTestRopeChild0   row=-1  state=no row
  CookTestRopeParent   row=-1  state=no row
  CookTestFloor        row=-1  state=no row

fwd_on1 / def_on1 (HE_COOK_STATIC=1):
  CookTestSaveState    row=0   state=Ready
  CookTestRig          row=-1  state=no row   ← Locomotion-Blocker greift, keine Zeile
  CookTestRopeChild1   row=1   state=Ready    ← Regel 9: Ancestor-Geometry geht nicht an das Kind
  CookTestRopeChild0   row=2   state=Ready
  CookTestRopeParent   row=-1  state=no row   ← Geometry-Blocker greift auf dem Parent selbst
  CookTestFloor        row=3   state=Ready
```
Das belegt beide Seiten: der `off`-Lauf hatte überhaupt keinen Cooked-Block (`reg.ctx().find` fand nichts —
echt uncooked, nicht nur zufällig gleich aussehend), der `on`-Lauf kochte genau die vier erwarteten Zeilen
und ließ Rig/RopeParent korrekt aus. Zusammen mit den identischen md5 oben ist das der vollständige Beleg:
Cooking griff, und es änderte kein Pixel.

Bildkontrolle (angehängt, `fwd_on1.png`): alle vier Würfel samt Boden und Schatten sichtbar — kein leeres
oder einfarbiges Bild, das die Gleichheit trivial gemacht hätte.

**Zusätzlich** `he_tests.exe --source-file=*test_cook_analysis.cpp,*test_cooked_static.cpp`: 35/35 Fälle,
56484/56484 Assertions grün (nach `he_tests`-Rebuild auf diesem HEAD) — der Ready/Pending-Zustandsbeleg auf
Datenebene (Feld-für-Feld-Vergleich), unabhängig von der Editor-Dump-Szene oben.

**Was das NICHT zeigt:** Der Editor-Dump ist Edit-Mode. `CameraRigController::update` und
`applySavedState`/`saveState` (play-mode-gated, `EngineApi.cpp:416-418`) laufen hier nicht — dieser A/B
beweist Render-Gleichheit der STATISCHEN Konfiguration, nicht das Verhalten der dynamischen Pfade. Die
sind bereits über `test_cooked_static`s Feld-für-Feld-Fälle abgedeckt (Schritt 13: "CameraRig-Entity mit
echtem Controller über vier Frames", "SaveState-Entity … `applySavedState`").

## Handoff

- **Topic / step:** Thema 343, Schritt 14 von (aktuell) 14. Branch
  `claude/entity-cooking-statische-entities-auf-daten-reduzieren`.
- **Done:** Done-Kriterium 4 aus Schritt 13s Gate erfüllt — Pixel-A/B auf einer
  Rig/Rope(-mit-zwei-Kindern)/SaveState-Szene mit `HE_COOK_STATIC=1` gegen `=0`, bit-identisch (md5), D3D11,
  Forward UND Deferred, plus ein Zeilen-Log, das zeigt, dass die Zeilen wirklich `Ready` wurden statt dass
  der Vergleich nur zufällig gleich aussah. Vorangegangene Schritt-11-Punkte (Regeln 7/9/CameraRig je mit
  Code-Beleg, Regel-8-Rest-Absage, Spline-Bestätigung) waren schon vor diesem Schritt erledigt.
- **Files touched:** `src/HE_Editor/EditorApplication.cpp` (neuer `HE_DUMP_COOKTEST`-Block, drei Includes,
  ein Zeilen-Log nach den Settle-Frames), dieses Dokument.
- **Verified:** Scene-DLL im Build neuer als `11104a557`; Rauschboden (je zwei `HE_COOK_STATIC=0`-Läufe,
  Forward UND der Himmel/Wolken-Eingrenzungslauf) byte-gleich, bevor der eigentliche Vergleich gezogen
  wurde; Zeilen-Log bestätigt, welche vier Entities kochten und welche zwei nicht; Bild zeigt alle vier
  Subjekte plus Schatten; `he_tests`-Cook-Suiten (35/35) grün; voller `he_tests`-Lauf unter Scratch-APPDATA
  (`C:\hwtp9\scratch_appdata`): 5983/5984 Fälle, 1952832/1952833 Assertions grün. Der eine Fehlschlag
  (`test_game_build_config.cpp:156`, "GameBuildConfig: the generator writes the checked-in fixture header")
  ist unabhängig von diesem Schritt — die Datei/der Generator (`GameBuildConfig`/`ProjectSettings`) wurde
  hier nicht angefasst, der jüngste Commit auf dieser Historie, der sie berührt, ist Audio-Arbeit
  (`5321d891d`, anderes Thema); die Checked-in-Fixture ist vermutlich nicht nachgezogen worden. Nicht
  weiter verfolgt (außerhalb des Schritts).
- **Not verified:** Play-mode-Pfade (CameraRig-Controller, SaveState-Apply) im Dump selbst — nicht nötig,
  s.o., von `test_cooked_static` abgedeckt. Die D3D11-Wolken-Nichtreproduzierbarkeit (Ursache) ist nicht
  untersucht, nur eingegrenzt und umgangen. Aus Schritt 11 weiterhin offen: ob Regel 8/12 (`LOD`,
  `Switchable`) denselben Ancestor-Fund wie Regel 9 hätten (§3, "Verwandter, hier nicht geprüfter Punkt") —
  dieser Schritt hat nur Regel 9 behandelt, Regel 8/12 sind kein Teil von Done-Kriterium 4.
- **Next steps:** Regel-8/12-Ancestor-Frage aus Schritt 11 ist der nächste Kandidat, falls das Thema
  weitergeht. Die D3D11-Wolken-Nichtreproduzierbarkeit (Ursache, nicht nur Umgehung) wäre für einen anderen
  Pixel-A/B-Schritt relevant, hier nicht verfolgt (kein Cooking-Bezug).
