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

## Handoff

- **Topic / step:** Thema 343, Schritt 11 von (aktuell) 11. Nur Dokumentation, kein Produktivcode
  geändert. Branch `claude/entity-cooking-statische-entities-auf-daten-reduzieren` @ `f218b0f4a`
  (Stand zu Schrittbeginn).
- **Done:** Regeln 7, 8-Rest, 9 und das CameraRig-/Spline-Paar aus §5 je mit Code-Beleg geprüft
  (Dateien/Zeilen oben); drei konkrete Folgeschritt-Entwürfe (Regel 7 entfernen, Regel 9
  Ancestor-Entkopplung, `CameraRigComponent` als neuer Blocker), eine begründete Absage (Regel 8 Rest),
  eine Bestätigung ohne Änderungsbedarf (`SplineComponent`), eine Korrektur einer bestehenden
  Begründung ohne Verhaltensänderung (§6 CameraRig-Ziel).
- **Files touched:** dieses Dokument. `docs/entity-cooking-survey-2026-11-13.md` bewusst NICHT
  geändert in diesem Schritt (die drei Entwürfe sind noch nicht umgesetzt; ein Verweis dorthin ohne
  Umsetzung wäre irreführend) — wer einen der drei Entwürfe umsetzt, sollte §5/§6 dort zusammen mit dem
  Code ändern, wie Schritt 7/8 es vormachen.
- **Verified:** alle zitierten Zeilennummern per Lesen der jeweiligen Datei bestätigt (nicht aus dem
  Code-Map-Cache übernommen); `noteContentChanged()`/`contentEpoch` end-to-end gegen `replaceStaticMesh`/
  `registerStaticMesh` geprüft (kein Aufruf); `classInstanceOf`/`ScriptComponent`-Kopplung gegen
  `EntityHost.cpp:52` geprüft; `SplineComponent`-Schreibzugriffe in vier weiteren Systemdateien
  gegengeprüft (kein Treffer).
- **Not verified:** keiner der drei Entwürfe implementiert oder gebaut (Schritt verlangt das nicht);
  kein Perf-Lauf (reiner Doku-Schritt); ob Regel 8/12 denselben Ancestor-Fund wie Regel 9 hätten, ist
  benannt, aber nicht einzeln durch Lesen der jeweiligen Schreibpfade bestätigt.
- **Next steps:** einen der drei Entwürfe als eigenen Schritt umsetzen — Regel 7 (SaveState) ist am
  einfachsten und am nächsten an Schritt 7s bereits gelaufenem Muster; die Ancestor-Entkopplung (Regel 9)
  und der neue `CameraRigComponent`-Blocker können im selben Schritt mit, da beide nur
  `CookAnalysis.cpp`/`.h` und `CookedStatic.cpp` berühren.
