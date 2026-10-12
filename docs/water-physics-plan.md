# Wasser-Physik: Auftrieb, Strömung und Eintauchen, Konzept (Thema 179, Schritt 1)

Stand 11.10.2026, Zweig `claude/wasser-physik-auftrieb-stroemung-eintauchen`, Basis `origin/release/0.7.0` (`35c184a3`).
Nur Konzept, kein Produktcode. Gelesen wurden das Wassermodell vom Zweig
`claude/prozedurales-wasser-spline-pinsel-lake-werkzeug` (Thema 174, Stand `82e76e79`, **noch nicht in `release/0.7.0`**),
das Wassermaterial (`docs/water-shader-plan.md`, `src/HE_Tools/src/MatGen/main.cpp`), die Physik-Schicht
(`PhysicsWorld.h/.cpp`) und die Threading-Dokumente (`docs/physics-threading-baseline.md`, `-budget.md`).
Zahlen in diesem Dokument sind entweder gelesen (mit Fundstelle) oder ausdrücklich als **Schätzung** gekennzeichnet.
Gemessen wurde nichts.

> **Stand nach Schritt 2 (12.10.2026): Abschnitt 13 am Ende beschreibt, was gebaut wurde, und wo er dem Konzept widerspricht,
> gilt Abschnitt 13.** Das betrifft vor allem 5.1 (zwei Dämpfungen statt `drag`, keine Wellen je Wasserkörper), 3.2 (Region je
> Landschaft statt je Körper) und 3.4 (schwimmende Körper dürfen nicht schlafen; Aufwecken bei Pegeländerung ist nicht gebaut).

## 0. Kurzfassung

| Frage | Entscheidung |
|---|---|
| Wo steht die Wasserhöhe | Im Wassermodell des Landschaftsfelds (`TerrainComponent::water`, `HE::water::Field`). Die Physik liest das **Feld**, nicht die Wasser-Entity. Neue reine Abfrage `HE::water::surfaceAt(index, x, z, simTime)` in `WaterQuery.h`, Skript-Zeile `water.surfaceAt` |
| Was heißt "mit Wellen" | Pegel des Körpers plus **zwei Sinuszüge** je Wasserkörper, nur für die Physik (Amplitude bei den Standardwerten 0,16 m und 0,08 m). Kein Nachbau des Shaders (FBM-Verzerrung, Hex-Bombing). Zeit ist die **Simulationszeit** der Physik, nie die Renderzeit |
| Auftrieb | Jolt rechnet das eingetauchte Volumen (`Body::GetSubmergedVolume`, `Body::ApplyBuoyancyImpulse`, Jolt 5.5.0). Die Wasserfläche ist je Körper **eine Ebene** am Schwerpunkt. Der Auftriebsfaktor kommt aus Dichte: `b = Wasserdichte / Körperdichte`, begrenzt auf 0,02 bis 4 |
| Dämpfung | Jolts Strömungswiderstand (linear quadratisch, angular linear), Faktoren 0,5 und 0,05, je Wasserkörper mit `drag` skalierbar |
| Strömung | `Body::physics.flow`, ein Vektor (terrain-lokal XZ, m/s) je Wasserkörper, geht als `inFluidVelocity` in Jolts Widerstand. Kein Geschwindigkeitsfeld |
| Parameter | In `HE::water::Body::physics` (gespeichert mit dem Wasserfeld). Am Starrkörper zwei Felder: `affectedByWater`, `density`. **Keine neue Komponente** (Abschnitt 5) |
| Störungsliste | Laufzeitliste `RippleField` (16 Einträge), in `HorizonWorld`. Übergabe an alle fünf Backends über einen **Anhang an `MaterialShaderLibrary::Lighting`** (33 vec4, +528 Byte), nicht über Materialparameter (die 16 Slots sind voll). Gelesen von einem neuen Graph-Knoten `Water Ripple` im Wassermaterial |
| Zeitbudget | Der Auftrieb läuft vor `Update()`, ist per Zähler begrenzt (256 nasse Körper je Schritt, Reihumbedienung wie bei den Charakteren), seine Zeit wird gebucht und im Profiler gezeigt. Ohne Wasser in der Szene kostet er eine Abfrage `index.empty()` |
| Determinismus | Keine neue Wandzeit-Abhängigkeit, keine Änderung ohne Wasser, Netz-Vertrag unberührt (Abschnitt 8) |
| Machbar bis 12.10. 07:00 | Schritte 2, 3 (Metal und OpenGL) und 4 (Code plus CI) ja. Schritt 5 in der Minimalfassung. Schritt 6 nur teilweise. Voraussetzung ist, dass das Wassermodell vorher in `release/0.7.0` liegt (Abschnitt 11) |

## 1. Bestand (belegt)

### 1.1 Wassermodell (Thema 174, nicht auf diesem Zweig)

Alles in diesem Abschnitt steht auf `origin/claude/prozedurales-wasser-spline-pinsel-lake-werkzeug` (21 Commits vor
`release/0.7.0`; `prozedurales-wasse-12` merged `release/0.7.0` dort hinein und legt den PR nach `release/0.7.0` an).
**Dieser Zweig enthält davon nichts.** Namen und Signaturen unten sind aus `82e76e79` gelesen und beim Merge gegenzuprüfen.

* `HE::water::Field` steckt **in** `TerrainComponent::water` (`WaterField.h`). Kein eigenes ECS-Component. Ein Raster
  (`res` Zellen je Seite, Standard 256) über den 0..1-UV-Bereich der Landschaft mit `coverage` (0..255) und `owner`
  (Körper-Id), dazu `std::vector<Body> bodies`.
* `Body`: `id` (stabil, 16 Bit), `level` (**terrain-lokales** Y), `sourceSpline`, `polygon`, `syncKey` (Laufzeit).
  Der Pegel gilt je Körper, nicht je Zelle.
* Einstellungen am Feld: `clipToGround` (Standard an), `shoreOvershoot` (0,05 m). Das Netz endet dort, wo der Boden aus dem Wasser
  steigt. Die Zellen bleiben die Absicht, die Fläche ist das, was nach dem Boden übrig bleibt.
* Lesen: `bodyAt(tc, x, z)` (nächste Zelle, `coverage >= kWet` = 128), `levelAt(tc, x, z, level)`, `coverageAt`,
  `cellRect`, `chunkCellRect`, `anyWater`. Alle nehmen **terrain-lokale** Koordinaten.
* Laufzeitzustand am Feld: `dirty`, Rechteck, `revision` (Zähler für Caches). Ein Wasser-Edit setzt **nur** das, nie
  `TerrainComponent::dirty/regionDirty` (sonst baut `TerrainSystem` alle Chunks neu).
* Speichern: Zellen als Lauflängenstrom, Körper als Liste (`"waterRes"`, `"waterBodies"` im Szenentext). `pristine()`
  heißt "nichts zu speichern"; ein vom Standard abweichendes `res` allein macht es unrein (Lehre aus Thema 174).
* `HE::api` Gruppe `water` (14 Zeilen, **Weltkoordinaten**, die Landschaft als Entity): unter anderem `water.bodyAt`,
  `water.levelAt` (Wet, Level), `water.lakeBody`, `water.wetCells`, `water.paint/erase`, `water.createLake`. Die Zeilen
  rechnen an der Kante über `HE::worldMatrixOf` der Landschaft (nie über die gespeicherte `worldMatrix`) in den
  terrain-lokalen Raum um. Das ist das Vorbild für die Physik.

### 1.2 Wasserfläche als Entity

`WaterSurface::update` (aus `TerrainSystem::updateTerrains`, außerhalb des Dirty-Gates) hält **je Körper** eine
Kind-Entity der Landschaft mit `WaterSurfaceComponent` (`terrain`, `body`, `meshId`, Hash) + `MeshComponent` +
`MaterialComponent{kEngineWaterMaterialId}`. Das Netz ist **flach** (Eckengitter, Marching Squares, Ear-Clipping), nicht unterteilt.
Die Entity wird **nie gespeichert**, ist im Outliner versteckt und wird von `HE::isTerrainGenerated(reg, e)` erfasst
(`TerrainChunkComponent.h`). Ein neuer generierter Kind-Typ gehört dort hinein.
**Die Fläche hat keinen Collider** (Test `Water surface: physics does not see the sheet, a body dropped over a lake reaches
the ground`, `tests/test_water_surface.cpp`). Daran ändert dieses Thema nichts: die Physik fragt das Feld, sie bekommt keinen
Wasserkörper in Jolt.

### 1.3 Wassermaterial

`Engine/Materials/Water.hasset` (`kEngineWaterMaterialId = {0x400, 1}`), erzeugt von `mat_gen`
(`src/HE_Tools/src/MatGen/main.cpp`), lit, Translucent, **222+ Knoten, rein Fragment**. Die Wellen sind gerichtete Sinuszüge mit
analytischer Normale, **keine Vertexverschiebung** (die Fläche ist flach, `docs/water-shader-plan.md` §2, §5a). Sichtbare
Ringe sind also Normalen- und Farbstörung im Fragment-Shader, keine Geometrie.

* **Alle 16 `HeParams`-Slots sind belegt** (`Variation` ist der 16.). Ein 17. Parameter wird nicht abgelehnt, sondern still als Literal
  eingebacken (`water-shader-plan.md` §2). Die Störungsliste kann deshalb **nicht** über Materialparameter laufen.
* Wellenparameter `WaveA/B/C` = (Richtung in Grad, 0 = +X, 90 = +Z; Tempo m/s; Wellenlänge m; Steilheit). Aktuelle Standardwerte aus
  `MatGen/main.cpp:532-541`: A = (20, 1, 14, 0,07), B = (335, 0,8, 5, 0,10), C = (70, 0,5, 1,6, 0,09). Amplitude einer Welle
  `a = Steilheit * Länge / (2π)`: A 0,156 m, B 0,080 m, C 0,023 m.
* Die Wellenphase wird mit FBM verzerrt und über ein Hex-Raster (`Variation`) gebombt. Der Shader ist deshalb **nicht** auf der CPU
  nachbaubar, und die Wellenwerte liegen im Materialasset oder in Pro-Entity-Überschreibungen, nicht im Wasserfeld.
* Zeit im Shader ist `heLight.sunDir.w` (Renderzeit).

### 1.4 Physik-Anbindung

* `PhysicsWorld` tauscht ausschließlich **Weltposen** mit Jolt aus (Raumkonvention im Kopf von `PhysicsWorld.h`). Wer eine Weltmatrix
  einer Entity braucht, nimmt `HE::worldMatrixOf` / `HE::worldPositionOf`, nie `TransformComponent::worldMatrix` (ein Frame zu alt, in
  `SceneSystems::tickWorld` propagiert nichts). Für den Auftrieb ist nur die **Landschaft** umzurechnen; die Körper stehen bei Jolt
  schon in Weltkoordinaten.
* `PhysicsWorld::step` (`PhysicsWorld.cpp:3104`): `applyRegionHold` (3118), `physicsSystem.Update` (3124), Reap und Rückschreiben der
  Körper (3178), Charaktere (3311). Der Aufruf ist `HE::advanceFixedSteps` mit festem Schritt (Spiel `GameApplication.cpp:3195`, Editor
  `EditorApplication.cpp:3506`), höchstens fünf Schritte je Frame.
* Frame-Reihenfolge: **Spiel** Skripte (3182) → Physik (3195) → Kollisions-Dispatch → Kamera → `tickWorld` (3324, enthält
  `updateTerrains` und damit `WaterSurface::update`) → Render. **Editor** `tickWorld` (3493) → Physik (3506) → Skripte nach dem
  Schritt. Weil die Physik das Feld liest und nicht die Wasser-Entity, ist diese Reihenfolge für den Auftrieb ohne Belang. Ein
  Wasser-Edit eines Frames wirkt im nächsten Schritt.
* Körper werden in `buildBodyFor` (1582) gebaut: Masse aus `RigidBodyComponent::mass` (`CalculateInertia`), Reibung, Rückprall, Sensor aus
  `ColliderComponent::isTrigger`. **Dämpfung wird nirgends gesetzt** (Jolt-Standard 0,05 linear und angular). `m_impl->dynamicBodies`
  (715) führt die dynamischen Körper; die Entity-Id steckt in `Body::GetUserData()` (1682).
* Jolt ist 5.5.0 (`CMakeLists.txt:110`). `Body::GetSubmergedVolume(surfacePos, normal, total, submerged, relCenterOfBuoyancy)` und
  `Body::ApplyBuoyancyImpulse(...)` (Ebenen- und Volumen-Überladung; `BodyInterface` hat die Ebenen-Überladung und weckt schlafende
  Körper) liegen im Quellbaum unter `Jolt/Physics/Body/Body.h:223-256`. Die Rechnung (`Body.cpp:216-266`): `fluid_density =
  buoyancy * Masse / Gesamtvolumen`, Auftriebsimpuls `-fluid_density * eingetaucht * gravityFactor * g * dt`, quadratischer
  Strömungswiderstand relativ zu `inFluidVelocity`, Drehwiderstand proportional zum eingetauchten Anteil. **Gleichgewicht bei
  eingetauchtem Anteil `1 / buoyancy`**, unabhängig von der Masse.
* Terrain hat Kollision (Höhenfeld, `buildTerrainBodyFor`), die Wasserfläche nicht (1.2).

### 1.5 Zeitbudget (Thema 178, `docs/physics-threading-budget.md`)

Das Budget (Standard 4 ms je Schritt, `HE_PHYSICS_BUDGET_MS`) begrenzt, was mit der Zahl der Entities wächst: Charaktere
(Aufschub mit nachgeholter Zeit, `PhysicsBudgetRules::steppedDt/carryOwed`, `kMaxOwedSteps = 4`, die ersten 32 immer), gebuchte Abfragen
(`QueryCharge`) und Körperarbeit (`BodyWorkCharge`), Events per Zähler. `Update()` selbst ist **nicht** im Budget. Baseline
(`physics-threading-baseline.md`): bei 20 000 Körpern ist der serielle Teil des Schritts 57 %, `applyRegionHold` allein 19 % (bevor die
Sonde `holdNeeded` es ausschaltete), also zählt jede Schleife über alle Körper. Der Auftrieb darf deshalb **nicht** alle dynamischen
Körper anfassen (Abschnitt 3).

### 1.6 Netz

`GameReplication` repliziert **nur** `TransformComponent` (Position, Rotation, quantisiert) für Entities mit `NetworkComponent`,
serverautoritativ, Interpolation fremder Entities. Prediction ist die vom Spiel gestellte `MoveFn` für die eigene Entity, kein
Jolt-Körper wird repliziert oder vorhergesagt (`docs/gameplay-replication-plan.md` §1.1). Jolt ist ohne
`CROSS_PLATFORM_DETERMINISTIC` gebaut (kein Schalter in den CMake-Dateien).

## 2. Wasserhöhe-Abfrage

Neu: `src/HE_Scene/include/HorizonScene/WaterQuery.h`, `src/HE_Scene/src/WaterQuery.cpp`, Namespace `HE::water`. Rein, ohne Jolt,
ohne Renderer, mit Landschaft auf dem Stack testbar (wie `WaterField`).

```cpp
struct WaveTrain { float directionDeg = 20.0f, speed = 1.0f, length = 14.0f, steepness = 0.07f; };

struct Surface {              // Antwort, alles in Weltkoordinaten
    bool        wet = false;
    entt::entity terrain = entt::null;
    uint16_t    body = 0;
    float       level = 0;    // Pegel ohne Wellen (Welt-Y)
    float       height = 0;   // mit Wellen (Welt-Y)
    glm::vec3   normal{0,1,0};// Wellennormale
    float       depth = 0;    // height minus Boden, 0 wenn trocken
    glm::vec3   flow{0};      // m/s
    float       density = 1000;
    float       splash = 1;
};

class WaterIndex {            // Schnappschuss, je Physikschritt neu aufgebaut
public:
    static WaterIndex build(HorizonWorld&);
    bool empty() const;                                  // kein nasser Körper irgendwo
    bool surfaceAt(float x, float z, double t, Surface&) const;  // t = Simulationszeit
    float heightAt(float x, float z, double t) const;    // NaN wenn trocken
    // pro Wasserkörper: Welt-AABB für die Broadphase-Abfrage (Abschnitt 3)
    void forEachBody(auto&& fn) const;
};
```

**Ablauf einer Abfrage.** Je Landschaft mit nicht leerem Wasser (`!water.pristine()` und mindestens ein Körper) liegt im Index:
Entity, `localToWorld` und `worldToLocal`, **zusammengesetzt mit `HE::worldMatrixOf` bei `build` (nie `worldMatrix`)**, Zeiger auf das
Feld, Welt-AABB. Eine Abfrage: AABB-Test → Welt nach terrain-lokal → `bodyAt` (eine Zelle) → Pegel → bei `clipToGround`
Gegenprobe `terrainHeightAt(local) <= level + shoreOvershoot`, sonst trocken (spiegelt das Netz) → Wellen → in Welt zurück. Mehrere
Treffer (überlappende Landschaften): die höchste Fläche gewinnt, bei Gleichstand die kleinere Entity-Id (nie Hash-Reihenfolge).
Kosten (Schätzung): eine Zelle, zwei `sin`, ein bilinearer Höhenwert, im Bereich von 0,1 bis 0,3 µs.

**Wellen.** Zwei Züge je Wasserkörper (`BodyPhysics::waves[2]`), Standard = `WaveA` und `WaveB` des Wassermaterials:

```
a_i = steepness_i * length_i / (2π)        k_i = 2π / length_i       d_i = (cos dir_i, sin dir_i)  (0 Grad = +X, 90 = +Z wie im Material)
h(x,z,t) = Σ a_i * sin( k_i * (d_i · (x,z) - speed_i * t) )
∂h/∂(x,z) = Σ a_i * k_i * cos( … ) * d_i        normal = normalize(-∂h/∂x, 1, -∂h/∂z)
```

`x,z` sind **Weltkoordinaten** (wie `WorldPos.xz` im Shader, damit die Phase über Landschaftsgrenzen stetig bleibt), `t` ist die
**Simulationszeit** (`PhysicsWorld` addiert `dt` je `step`; ohne Physik, also im Editiermodus, ist `t = 0`). Phase 0, kein FBM, kein
Hex-Bombing: der Shader und die Physik zeigen dieselbe **Größenordnung** und Richtung, nicht dieselbe Phase. Die größte Abweichung
zwischen sichtbarer und physikalischer Fläche ist die Amplitude der Physikwellen (0,24 m bei den Standardwerten, bei `steepness = 0`
exakt flach). Das ist bewusst gewählt und im Inspector zu benennen. Die Wellen wirken **nicht** auf die Form der Fläche (flaches Netz).

**Was die Fläche annimmt.** Der Pegel `Body::level` ist terrain-lokal; in Welt-Y wird er über `localToWorld` gewonnen. Die Fläche gilt
als waagrecht in Welt-Y (wie die Annahme des Shaders "ungefähr waagrechte Fläche", `water-shader-plan.md` §5a). Eine um X oder Z
gekippte Landschaft ist nicht Teil dieses Themas. Verschiebung, Drehung um Y und Skalierung werden unterstützt.

**Skript-Zeilen** (Abschnitt 7): `water.surfaceAt(x, z)`, `water.depthAt(x, z)`, `water.flowAt(x, z)`, `water.submerged(entity)`, alle ohne
Landschaften-Handle, alle in Welt. Sie lesen den Index der Welt (bei Bedarf einmal je Aufruf gebaut, im Editiermodus ohne
Physik mit `t = 0`).

## 3. Auftriebsmodell

### 3.1 Wo und für wen

In `PhysicsWorld::step`, **nach `applyRegionHold()`, vor `physicsSystem.Update()`** (Jolt verlangt den Impuls für den nächsten Schritt;
`inDeltaTime` ist die Schrittlänge). Neue Funktion `PhysicsWorld::applyWater(HorizonWorld&, float dt)` in `PhysicsWorld.cpp` (Jolt-Teil),
die reine Mathematik in `WaterQuery.cpp` (testbar ohne Jolt).

Für jeden Körper, der **alle** Bedingungen erfüllt, wirkt der Auftrieb:
* `RigidBodyType::Dynamic` (nach einem Herabstufen auf Static nichts), kein Sensor (`ColliderComponent::isTrigger`), kein gehaltener Körper
  (Region-Hold), kein Charakter-Proxy (der ist kinematisch);
* `RigidBodyComponent::affectedByWater` (neu, Standard **an**);
* sein Schwerpunkt liegt über nassem Wasser (`surfaceAt` ist `wet`).

**Verhaltensänderung, ausdrücklich:** Ein dynamischer Körper in einem See fällt nicht mehr bis zum Grund, sondern schwimmt oder sinkt nach
Dichte. Das Wasser aus Thema 174 ist unveröffentlicht (0.7.0), es gibt keine Projekte in freier Wildbahn, die sich darauf verlassen.
Einzige bekannte Stelle im Repo: der 174-Test oben lässt einen 1-kg-Ball (r = 0,5 m, also Dichte rund 2 kg/m³) in einen See fallen und
erwartet den Grund. Mit Auftrieb schwimmt er. **Schritt 2 muss diesen Test anpassen** (`affectedByWater = false` oder `density = 8000`;
er prüft weiter, dass die Fläche keinen Collider hat).

### 3.2 Welche Körper werden angefasst (Kosten)

Keine Schleife über alle dynamischen Körper (Baseline 1.5). Stattdessen je Wasserkörper eine **Broadphase-Abfrage**
(`PhysicsSystem::GetBroadPhaseQuery().CollideAABox`, nur die bewegliche Objektebene, die auch `overlapBox` benutzt) mit der
Welt-AABB des Körpers: XZ aus dem Zellenrechteck der nassen Zellen, Y von `level - 256` bis `level + Wellenhöhe + 8`. Kosten wachsen
mit den Körpern **im** Wasser, nicht mit der Szene. Die Treffer werden gesammelt, dedupliziert und **nach Entity-Id sortiert**
(Reihenfolge der Broadphase ist baumabhängig, die Wirkung je Körper nicht, aber die Reihumbedienung unten muss deterministisch sein).

Ohne Wasser in der Szene (`index.empty()`, also keine Landschaft mit nasser Zelle) endet `applyWater` nach einer Prüfung. Das ist der
einzige Preis für Szenen ohne Wasser.

### 3.3 Rechnung je Körper

```
plane      = (x, h(x,z,t), z) am Schwerpunkt, Normale aus den Wellen (Welt)
V, Vsub, cob = body.GetSubmergedVolume(plane)           // Jolt, exakt für Box/Kugel/Kapsel, Polygonschnitt für Hüllen
rhoBody    = (RigidBody.density > 0) ? RigidBody.density : mass / V
b          = clamp( surface.density / rhoBody, kMinBuoyancy = 0.02, kMaxBuoyancy = 4 )
Body::ApplyBuoyancyImpulse(V, Vsub, cob, b, linDrag*drag, angDrag*drag, flowWorld, gravity, dt)
```

* **Warum eine Dichte und nicht Jolts 0..2**: Jolts Faktor ist `Wasserdichte * V / Masse` (`Body.cpp:219`). Der Autor denkt in "Holz
  schwimmt, Stein sinkt", nicht in diesem Faktor. Gleichgewicht ist bei eingetauchtem Anteil `rhoBody / rhoWasser`.
* **Warum die obere Grenze 4**: ein Körper mit Dichte weit unter Wasser (Standardmasse 1 kg bei 1 m³) hätte `b = 1000`. Der Auftrieb ist
  eine Feder mit `ω² = b * g / Höhe`; bei 60 Hz bleibt das explizite Verfahren nur für `ω * dt < 2` stabil. Das gibt `b < 1470 * Höhe in m`,
  also ab `b` von rund 150 bei 10 cm Höhe schwingt der Körper auf (1 m³-Kiste mit 1 kg: `b = 1000`, knapp stabil und kaum gedämpft). Mit `b <= 4` taucht jeder Körper mindestens zu einem Viertel ein und bleibt stabil. **Startwert,
  Schritt 2 misst die Stabilität an dünnen Körpern und darf ihn ändern** (Test "settles", s. u.).
* **Dichte am Körper**: `RigidBodyComponent::density` (kg/m³), Standard **0 = aus Masse und Colliderfolgen**. Wer "dieser Block
  taucht zur Hälfte ein" will, setzt 500 und muss die Masse nicht verfälschen (die Masse bestimmt Stöße, die Dichte nur den
  Auftrieb). Referenzwerte für den Hilfetext: Holz 500, Eis 920, Wasser 1000, Stein 2600, Eisen 7800.
* **Dämpfung** (linear und angular): Jolts Widerstand in `ApplyBuoyancyImpulse`, Startwerte `linearDrag = 0,5`, `angularDrag = 0,05`
  (`PhysicsTuning`-Konstanten, Jolts Hinweise "etwa 0,5" und "etwa 0,01", der Drehwiderstand kommt bei Quader mit rund
  `6 * angularDrag` je Sekunde heraus). Mit `Body::physics.drag` je Wasserkörper multiplizierbar (Sumpf 3, Öl 0,5). Die normale
  Körperdämpfung von Jolt (0,05) bleibt unverändert.
* **Aufruf**: Volumen-Überladung (Gesamt- und eingetauchtes Volumen schon bekannt), damit `GetSubmergedVolume` nur einmal läuft und der
  eingetauchte Anteil `Vsub / V` für Ringe und `water.submerged` abfällt. Körper über `BodyLockInterfaceNoLock` (Haupt-Thread, vor
  `Update`, wie das Rückschreiben); Schreibzugriff-Variante und Aufwecken in Schritt 2 gegen den Jolt-Quelltext prüfen.

### 3.4 Schlafende Körper

Nur **aktive** Körper bekommen den Impuls. Ein auf dem Grund ruhender schwerer Körper würde sonst jeden Schritt geweckt und nie schlafen.
Geweckt wird ein schlafender Körper in der Abfrage nur, wenn sich das Wasser unter ihm geändert hat: der Index merkt sich je Landschaft
`Field::revision` und vergleicht beim Aufbau; geändert → die Körper im betroffenen AABB werden mit `ActivateBody` aufgeweckt.
`setPhysics` (Abschnitt 5) erhöht `revision` deshalb auch. Ein schwimmender Körper auf bewegten Wellen schläft ohnehin nicht, einer im
glatten Wasser (`steepness = 0`) darf im Gleichgewicht schlafen.

### 3.5 Grenzen, ehrlich

* **Eine Ebene am Schwerpunkt.** Ein Boot von 10 m über 14 m Wellen sieht nur die Welle in seiner Mitte; es neigt sich nicht mit der
  Wellenform. Mehrere Probepunkte wären der nächste Ausbau.
* Schwerpunkt über Land, Körper zur Hälfte im Wasser: kein Auftrieb (der Index sagt trocken).
* Ein Körper mit Schwerpunkt über Wasser A und Rumpf in Wasser B nimmt A.
* Wellen schieben nicht seitlich (Jolt wirkt senkrecht); Verdriftung kommt nur aus `flow`.
* Seile, Gelenke, Ragdolls: jeder Körper einzeln wie ein Starrkörper, keine Sonderbehandlung.
* **Charaktere schwimmen nicht** (CharacterVirtual, eigener Bewegungsweg). Die Abfragezeilen liefern, was ein Schwimm-Skript braucht;
  ein eingebautes Schwimmen ist nicht Teil dieses Themas.

## 4. Strömung

Ein Vektor je Wasserkörper: `BodyPhysics::flow` (terrain-lokal XZ, m/s, Standard 0). In Welt: um die Y-Drehung der Landschaft gedreht,
`flowWorld = R_y(terrain) * (flow.x, 0, flow.y)`. Er wird **nur** als `inFluidVelocity` an Jolts Widerstand gegeben: ein Körper wird
auf die Strömungsgeschwindigkeit hin gezogen (quadratisch, mit Fläche und Dichte des Körpers), er bekommt keine Zusatzkraft aus der
Wellenneigung. Schlafen: ein im Strom treibender Körper bleibt über der Schlafschwelle (0,03 m/s) und schläft nicht.

Ebenfalls: `RippleField` verschiebt die Mitte laufender Ringe mit der Strömung am Ort der Störung (Abschnitt 6), damit Ringe flussabwärts
wandern. Die Wellen des Shaders folgen der Strömung **nicht** (`WaveA/B/C` bleiben Materialparameter); eine Schaltfläche "Wellenrichtung aus
Strömung" im Editor ist ein optionaler Zusatz von Schritt 5, kein Muss.

Nicht Teil: ortsabhängige Strömung (Flussbett entlang einer Spline, Wirbel), Strömung für Charaktere, Strömung als Textur.
Eine Strömung entlang einer Spline wäre eine eigene Erweiterung am `Body` (`sourceSpline` ist schon da).

## 5. Parameter und Komponenten

### 5.1 Wasserkörper (gespeichert mit dem Wasserfeld)

Neues Unterobjekt `HE::water::Body::physics` (`BodyPhysics`, in `WaterField.h`):

| Feld | Typ, Standard | Bedeutung |
|---|---|---|
| `flow` | vec2, (0,0) | m/s, terrain-lokal XZ (Abschnitt 4) |
| `waves[2]` | `WaveTrain` | nur Physik: Richtung°, Tempo m/s, Länge m, Steilheit. Standard = `WaveA` (20°, 1, 14, 0,07) und `WaveB` (335°, 0,8, 5, 0,10) |
| `density` | float, 1000 | kg/m³ des Wassers (Salzwasser 1025) |
| `drag` | float, 1 | Faktor auf lineare und angulare Dämpfung in diesem Wasser |
| `splash` | float, 1 | Faktor auf die Stärke der Ringe, 0 = keine Ringe |

* **Gespeichert nur bei Abweichung**: im `"waterBodies"`-Eintrag ein Schlüssel `"physics"`, der nur existiert, wenn mindestens ein Wert vom
  Standard abweicht, und nur die abweichenden Unterschlüssel enthält. Bestehende Szenen und Szenen ohne Wasser bleiben **byte-gleich**
  (Lehre aus 174: ein Standardfeld, das immer geschrieben wird, bricht Tests wie "kein Wasser im Szenentext").
* **Gleichheit und Undo**: `Body` bekommt die neuen Felder in `sameContent`, in `Delta::sameBodies`/`makeDelta` (`bodiesBefore/After`)
  und in `sanitize` (nicht endlich → Standard; `flow` ≤ 50 m/s, `density` 1..20000, `drag` 0..10, `splash` 0..4, `length` 0,1..500,
  `steepness` 0..1).
* **Setzen**: `water::setPhysics(tc, bodyId, const BodyPhysics&)`, erhöht `Field::revision`, setzt **nie** `dirty` oder das Rechteck (die
  Fläche ändert sich nicht, das Netz wird nicht neu gebaut), ändert nie `TerrainComponent::dirty`. Ein Test prüft, dass das Netz
  danach nicht neu gebaut wird. Im Inspector nie über `changed` (setzt `t->dirty`, baut alle Chunks): dieselbe Falle wie `setShoreClip`.

### 5.2 Starrkörper

`RigidBodyComponent` bekommt zwei Felder, beide bei Standard **nicht** serialisiert:

| Feld | Standard | Bedeutung |
|---|---|---|
| `affectedByWater` | `true` | `false`: Auftrieb, Dämpfung und Ringe ignorieren diesen Körper (Geschosse, Trümmer) |
| `density` | `0` | kg/m³ für den Auftrieb; 0 = aus Masse durch Collider-Volumen |

Beide werden **pro Schritt gelesen** und gehören deshalb nicht in `PlayPhysicsInputs` (`EditorApplication.cpp`, Neubau des Körpers beim
Ändern); eine Änderung im Details-Panel während Play wirkt sofort.

### 5.3 Warum keine neue Komponente

Erwogen wurde eine `BuoyancyComponent` (Opt-in je Körper). Abgelehnt: Die Themenvorgabe sagt, Körper im Wasser bekommen Auftrieb
und Dämpfung; ein Opt-in macht aus "Kiste ins Wasser legen" einen zweiten Arbeitsgang; alle drei Einstellungen (Dichte, Opt-out) passen
als Felder an den Starrkörper, der sie ohnehin hat. Die Wasserseite braucht keine Komponente, weil das Wasser in `TerrainComponent::water`
wohnt (174) und die Wasser-Entities generiert und ungespeichert sind.

**Falls Schritt 2 oder 5 doch eine Komponente anlegt** (zum Beispiel für Schwimmen von Charakteren), gelten die sechs Stellen
(`new-component-registration-places`, Stand 174 bestätigt):
1. `SceneSerializer.cpp` Speichern und Laden;
2. `HE_SCENE_COMPONENT_KEYS` (X-Makro, `SceneSerializer.cpp:2483`), für Remove und Reset;
3. `SceneSerializer::isKnownComponentKey` (`:2733`, handgepflegte Menge; fehlt sie, fällt `test_inspector_prefab_keys`);
4. `InspectorPrefabKeys.cpp` Label → Schlüssel;
5. `kComponentScopes` in `EditorHelp.cpp` (`:8829`; fehlt es, fällt `test_editor_help` "help key with no area", sobald es
   Hilfeschlüssel `<Scope>/…` gibt);
6. `HorizonScene.h` Sammel-Include, Zeile "Add Component" in `InspectorPanel.cpp`, Hilfeeinträge (`scripts/editor_help_audit.py --check`),
   dazu `OutlinerFilter.cpp`.
Kopieren, Duplizieren, Undo und Details-Kopie erben die Komponente über den Serializer.

## 6. Störungen: Ringe und Wellen im Wassermaterial

### 6.1 Was sichtbar wird

Ein Aufprall oder eine Bewegung im Wasser erzeugt **Ringe**, die von der Stelle auslaufen und abklingen: Normalenstörung im Wassershader
(Wellenkamm als Ring) plus ein wenig Schaum am Kamm. Keine Geometrie (Netz flach). Die Ringe sind **reine Anzeige**: die Physik liest sie
nie zurück, es gibt keine Rückkopplung und damit keine neue Determinismusfrage.

### 6.2 Quellen

| Art | Auslöser | Stärke (Startwerte, Schritt 3 stimmt sie am Bild ab) |
|---|---|---|
| Aufprall (`kind 0`) | Körper wechselt von "nicht eingetaucht" (< 2 % Anteil) zu eingetaucht, Eintauchgeschwindigkeit ≥ 1 m/s (senkrechte Komponente relativ zur Fläche) | `clamp(v / 6, 0,15, 1) * splash`, Startradius `clamp(0,6 * Hüllradius, 0,15, 3)` |
| Spur (`kind 1`) | Körper schneidet die Fläche (Anteil 2 % bis 98 %), Tempo ≥ 0,6 m/s | alle 0,12 s Simulationszeit, `clamp(v / 8, 0, 0,5) * splash`, kleiner Radius |
| Skript (`kind 2`) | `water.disturb(x, z, radius, strength)` | wie gegeben, 0..1 |

Zustand je Körper für den Wechsel (Anteil im vorigen Schritt) liegt in `PhysicsWorld::Impl`, wird beim Entfernen des Körpers und beim
Verlassen des Wassers gelöscht. Höchstens 4 Auslösungen je Schritt (aufsteigend nach Entity-Id, deterministisch); ein neuer Eintrag
innerhalb von 0,3 m eines jungen Eintrags derselben Art frischt diesen auf, statt einen neuen anzulegen (ein Haufen von 200 Kisten
verstopft die Liste nicht).

### 6.3 Die Liste

Neu: `src/HE_Scene/include/HorizonScene/WaterRipples.h/.cpp`, `HE::water::RippleField`, **ein Objekt je `HorizonWorld`**
(`HorizonWorld::ripples()`), Laufzeitzustand: nie gespeichert, nicht im Undo-Schnappschuss, beim Laden und Entladen der Szene geleert.
Besitzer-Vorschlag `HorizonWorld`, weil `PhysicsWorld::step` die Welt ohnehin bekommt und der Server (ohne Renderer) denselben Weg hat;
Schritt 3 darf einen anderen Besitzer wählen, solange Erzeuger und Anwendungsschleife dasselbe Objekt sehen.

```cpp
struct Ripple { glm::vec2 pos; float radius; float strength; float age; glm::vec2 drift; uint8_t kind; };   // Position, Radius, Stärke, Alter
class RippleField {
public:
    static constexpr int kMax = 16;
    void add(const Ripple&);            // bei voll: ersetzt den schwächsten, ältesten Eintrag
    void advance(float dt);             // Alter += dt, Mitte += drift*dt, Eintrag weg wenn strength*exp(-age/τ) < 0,02
    void clear();
    int  count() const;
    void pack(WaterRippleBlock& out) const;
};
```

### 6.4 Übergabe an die Shader aller fünf Backends

**Weg: ein Anhang an die bestehende Material-Beleuchtungs-ABI**, nicht Materialparameter (16 von 16 belegt) und kein neuer Puffer.

1. `HE_Core/include/Renderer/WaterRipples.h`: POD `WaterRippleBlock { float head[4]; float a[16][4]; float b[16][4]; }`.
   `IRenderer::SetWaterRipples(const WaterRippleBlock&)` / `GetWaterRipples()` mit Standardablage wie `SetEnvironmentSettings`
   (`IRenderer.h:643`), also **keine** Änderung der Backend-Klassen. Aufgerufen neben `SetEnvironmentSettings` im Spiel
   (`GameApplication.cpp:3538`) und Editor (`EditorApplication.cpp:11734`) nach `ripples().advance(dt)`; Material-Vorschau und UI-Pässe
   rufen nichts auf und lassen `head[0] = 0`.
2. `MaterialShaderLibrary::Lighting` (`MaterialShaderLibrary.h:40-177`) wird **am Ende** erweitert (append-only, der Kopf bleibt):
   ```cpp
   float waterRipples[4]    = {};    // x = Anzahl aktiv (0..16), y = Ringtempo m/s, z = Abklingzeit s, w = reserviert
   float waterRippleA[16][4] = {};   // xy = Mitte (Welt XZ), z = Startradius m, w = Stärke 0..1
   float waterRippleB[16][4] = {};   // x = Alter s, y = Art, zw = reserviert (Drift wird CPU-seitig in die Mitte eingerechnet)
   ```
   Das sind 33 vec4 = **528 Byte**. Die Struct ist heute 524 Floats = 2096 Byte groß (aus den Feldern gezählt, Schritt 3 prüft mit
   `static_assert`), danach 2624 Byte. **Metal übergibt den Block mit `setFragmentBytes` (`MetalRenderer.mm:9595, 13304, 13992 …`), dessen
   Grenze 4096 Byte ist**: Reserve danach 1472 Byte. Die anderen vier Backends binden einen Puffer der Größe `sizeof(Lighting)`
   (D3D11 `:4530`, D3D12 `:8662`, Vulkan `:2756`, OpenGL `:3708`), Grenzen 64 KiB beziehungsweise 16 KiB, kein Thema.
3. Derselbe Block im GLSL-Vorspann `kLightingPreamble` (`MaterialShaderLibrary.cpp:199-230`), nach `weather2`:
   ```glsl
   vec4 waterRipples;       // x = Anzahl, y = Ringtempo, z = Abklingzeit
   vec4 waterRippleA[16];   // xy = Mitte, z = Startradius, w = Stärke
   vec4 waterRippleB[16];   // x = Alter, y = Art
   ```
   Die Vertexstufe (WPO) schneidet ihren Block **aus demselben String** (`wpoLightingBlock`, `:873`), bleibt also automatisch
   deckungsgleich. Die Tests `tests/test_engine_materials.cpp:775/828/902` prüfen, dass die übersetzte Blockgröße gleich
   `sizeof(Lighting)` ist; ein vergessenes Feld auf einer Seite fällt dort auf (auf Mac offline, auf Windows und Linux in CI).
4. `HE::FillMaterialWater(const WaterRippleBlock&, Lighting&)` in `LightPacking.cpp`, **neben `FillMaterialWeather`** an jeder Stelle, die
   ein Vorwärts-Material zeichnet: Metal `MetalRenderer.mm:13196, 14572, 15904`, OpenGL `OpenGLRenderer.cpp:7678, 11404`, D3D11
   `D3D11Renderer.cpp:6443`, D3D12 `D3D12Renderer.cpp:10201`, Vulkan `VulkanRenderer.cpp:7755`. Die Stellen mit nur `FillMaterialWind`
   (Metal 9585, OpenGL 8807, Vorschau) bleiben ohne Ringe. Wasser ist Translucent und läuft auf allen Backends im Vorwärtspfad nach
   dem Deferred-Resolve (`water-shader-plan.md` §3), die Resolve-Pässe brauchen es nicht.
5. **Graph-Knoten `Water Ripple`** (neuer `MatNodeType` am Enum-Ende): Eingang Position (Vec2, Standard `WorldPos.xz`), Ausgänge
   Höhe (float), Neigung (Vec2), Schaum (float). Er erzeugt einen Hilfsaufruf `heWaterRipple(vec2 p)`, der über
   `int(heLight.waterRipples.x)` Einträge läuft (bei 0 Einträgen eine einheitliche Verzweigung, kein Aufwand). Dem Muster des
   `Weather`-Knotens (`MaterialGraph.cpp:760`) folgend. Pro Eintrag:
   ```
   d     = |p - c|                              age = B.x
   front = r0 + ringSpeed * age                 envelope = strength * exp(-age / τ)
   w     = 0.35 + 0.25 * age                    (Ringbreite wächst)
   h     = envelope * exp(-((d - front) / w)²) * cos(k * (d - front)) / (1 + d)      k = 2π / 0.6
   slope = ∂h/∂d * (p - c) / d                  foam = envelope * exp(-((d - front) / (0.5 w))²)
   ```
   Summe über die Einträge. Die Konstanten sind **Startwerte** von Schritt 3; **die Schnittstelle (Blocklayout, Knotenanschlüsse) ist der Vertrag**.
6. Im Wassergraph (`mat_gen`, `MatGen/main.cpp`): `Neigung` wird vor der Beleuchtung von der Wellennormale abgezogen
   (`N = normalize(N - (slope.x, 0, slope.y))`), `Schaum` zur Schaummaske addiert. **Kein neuer Parameter** (Slot 16 voll).
   Danach `Water.hasset` neu erzeugen (zweimal laufen lassen und `cmp`) **und ins Deploy kopieren**, sonst sieht `he_shot` den alten Shader
   (Lehre Thema 152); `kWaterKnobs` und die DeepColor-Pins in `test_engine_materials.cpp` bleiben unverändert.
   Ein neuer Knoten braucht zusätzlich: Registerzeile, Emit-Fall und Hilfetext in `MaterialGraph.cpp`, die Schleifengrenze "Every node type
   has a registry entry" in `tests/test_material_graph.cpp` (letzter Enum-Wert), den Namen in `McpToolsMaterial.cpp` (typeName-Switch),
   einen Widget-Fall in `MaterialEditorPanel.cpp`.

**Alter statt Geburtszeit**: Die CPU hält das Alter und schreibt es jeden Frame (16 Einträge, vernachlässigbar). Eine Geburtszeit gegen
`sunDir.w` hätte zwei Uhren gekoppelt (Renderzeit gegen Spielzeit mit Zeitskala und Pause).

**Backend-Parität.** Die Shaderquelle ist **eine** GLSL-Quelle, die über glslang/SPIRV-Cross nach MSL, HLSL, GLSL410 und SPIR-V geht. Es
gibt keinen backend-eigenen Shadercode für die Ringe. Pro Backend entsteht nur das `FillMaterialWater` an den Zeilen oben. Prüfbar auf dem
Mac: Übersetzung des Wassermaterials nach HLSL und SPIR-V (`test_engine_materials`, `glslangValidator -D -V -e`), Blockgrößentests,
Metal und OpenGL im Bild (`he_shot.py`). Nicht auf dem Mac prüfbar: D3D11, D3D12 und Vulkan im Lauf.

### 6.5 Kosten im Shader

Bis 16 Durchläufe je Fragment der Wasserfläche, je mit `length`, `exp`, `cos` (Schätzung: einige Dutzend ALU-Operationen), nur bei aktiven
Ringen; ohne Ring eine einheitliche Verzweigung. Gemessen wird in Schritt 3 mit `he_perf_capture.py` (Pass-p50 mit und ohne Ringe,
`perf-measurement-on-m5`: Fremdlast und Stromsparen prüfen).

## 7. Editor und Skript-API

### 7.1 Editor (Schritt 5)

* **Wasserkörper**: im Water-Bereich der Landschaft (Inspector "Terrain" und Water-Panel aus 174) je ausgewähltem Körper ein Abschnitt
  "Physics": Density, Flow (X, Z), Drag, Splash, Wellenzüge (zweimal Richtung, Tempo, Länge, Steilheit). Alles über `water::setPhysics`
  (Abschnitt 5.1), ein Undo-Schritt (Szenenschnappschuss, Wasser ist darin).
* **Starrkörper**: im Inspector "Rigid Body" die Zeilen "Affected By Water" und "Density" (Hilfetext mit den Referenzdichten).
* **Hilfe**: Einträge unter `Water/…` und `Rigid Body/…` in `EditorHelp.cpp`, `scripts/editor_help_audit.py --check` läuft im `ctest`.
* **Profiler**: Abschnitt "Physics budget" bekommt `buoyancy (ms)`, `wet bodies`, `buoyancy deferred` (Abschnitt 8).
* **Projekt**: `Config/ProjectSettings.json` `"physics": { "waterBodiesPerStep": 256 }`, nur in der Datei wie `eventsPerPoll`, und
  `HE_PHYSICS_WATER=0` als Umgebungsschalter (Auftrieb aus, für Server-Vergleiche und A/B).
* **Optional, zuerst streichen**: "Wellen aus Material übernehmen" (liest `WaveA/B` des Flächenmaterials), "Wellenrichtung aus Strömung",
  Dichte-Voreinstellungen als Auswahl, Fall-Taste "Drop" im Water-Panel zum Ausprobieren, MCP-Werkzeug für die Physikwerte.

### 7.2 Skript-API (Schritt 5)

Die Gruppe `water` existiert nach 174 (14 Zeilen, Weltkoordinaten) und steht in `isScriptGroup` (`EngineApi.cpp:8552` auf dem 174-Zweig
gelesen). Neue Zeilen, **alle in Welt, ohne
Landschaften-Handle** (der Aufrufer hat eine Position oder einen Körper, selten die Landschaft):

| Zeile | Art | Bedeutung |
|---|---|---|
| `water.surfaceAt(x, z)` → (wet, height) | rein | Höhe der Fläche mit Wellen, `wet = false` wenn trocken (Muster `levelAt`) |
| `water.depthAt(x, z)` → float | rein | Wassertiefe an der Stelle, 0 trocken |
| `water.flowAt(x, z)` → Vec3 | rein | Strömung in Welt, 0 trocken |
| `water.submerged(entity)` → float | rein | eingetauchter Anteil 0..1 des Körpers (letzter Schritt); 0 ohne Körper oder trocken |
| `water.disturb(x, z, radius, strength)` | exec | Ring auslösen (Fußtritt, Schuss) |
| `water.setFlow(terrain, body, x, z)` | exec | Strömung eines Körpers setzen (terrain-lokal wie gespeichert) |

Jede Zeile braucht (`engine-api-row-three-places`): Display-Name-Map am Ende von `EngineApi.cpp`, Beschreibung (≥ 20 Zeichen) in
`HcNodeDocs.cpp`, die Parity-Fixture, falls die Gruppe eine hat. Eine vierte Stelle (C++-GameLogic-Servicetabelle, ABI-Version, Probe)
entfällt: `water` kommt in `HorizonGameServices.h` auf dem 174-Zweig nicht vor (gelesen, 0 Treffer); wer die Zeilen dort anbieten will,
legt die Tabelle neu an und folgt dem Muster der Rumble-Zeilen (Thema 85). Die vier reinen Zeilen sind
über `McpToolsApi` automatisch MCP-Werkzeuge; exec-Zeilen sind es nicht (Lehre 174), ein mutierender MCP-Weg wäre ein eigenes Werkzeug
und gehört zu den optionalen Teilen.
`ctest -R test_hc_node_docs` und der volle `ctest` laufen lassen, nicht nur die eigenen Fälle.

## 8. Zeitbudget und Determinismus (die zwei geforderten Prüfungen)

### 8.1 Passt der Auftrieb ins Physik-Zeitbudget (Thema 178)?

**Ja, mit drei Auflagen.** Der Auftrieb ist neue serielle Arbeit **vor** `Update()`, also in dem Teil, der laut Baseline mit den Körpern
wächst. Er wird deshalb wie die bestehende Körperarbeit behandelt:

1. **Er wächst nicht mit der Szene, sondern mit den Körpern im Wasser** (Broadphase-Abfrage je Wasserkörper, 3.2) und ist ohne Wasser
   ein Test auf `index.empty()`.
2. **Obergrenze per Zähler, nicht per Zeit** (die Regel aus dem Budgetdokument für Events: "Zählen statt Zeit, weil eine Zahl auf jeder
   Maschine gleich wirkt"): höchstens `waterBodiesPerStep` = 256 nasse Körper je Schritt. Die übrigen warten und bekommen beim
   nächsten Mal den Impuls mit der **Schuld an Zeit** (`dt + owed`, `PhysicsBudgetRules::steppedDt/carryOwed`, höchstens
   `kMaxOwedSteps`), genau wie ein aufgeschobener Charakter. Der Impuls ist linear in `dt`, die Summe bleibt also gleich, nur später.
   Reihenfolge: größte Schuld zuerst, dann kleinste Entity-Id (nie Hash-Reihenfolge). Über der Obergrenze sinkt ein Körper
   langsamer (zu wenig Auftrieb im Mittel), das steht als `buoyancy deferred` im Profiler.
3. **Die Zeit wird gebucht** (wie `BodyWorkCharge`): `PhysicsBudgetStats` bekommt `buoyancyMs`, `wetBodies`, `buoyancyDeferred`. Die Zeit
   des Auftriebs geht vom Anteil der Charaktere ab (`chargedMs`, `PhysicsWorld.cpp:3290`), die ersten 32 Charaktere bleiben garantiert. Der
   Auftrieb selbst wird **nicht** nach Zeit aufgeschoben, nur nach Zähler.

**Schätzung** der Kosten (nicht gemessen): `GetSubmergedVolume` ist für Quader, Kugel und Kapsel geschlossen und billig, bei
Konvexhüllen ein Polygonschnitt über alle Flächen und teurer. Mit 1 bis 3 µs je nassem Körper kosten 256 Körper 0,25 bis 0,75 ms, also 6
bis 19 % des Standardbudgets von 4 ms. Schritt 2 misst das mit einem Bench nach Muster `scripts/perf/physics_load_*.py` (Quader, Kugeln,
Hüllen mit 50 und 500 Flächen) und senkt `waterBodiesPerStep` oder führt für große Hüllen eine Näherung (Hüllkugel) ein, wenn der Wert
über 1,5 µs je Körper liegt. Ein Profiler-Scope `Buoyancy` in `step` kommt dazu (die Baseline nennt fehlende Scopes als Lücke).

### 8.2 Keine Determinismus-Änderung für das Netzwerk

* **Netzvertrag unberührt**: Repliziert werden nur Position und Rotation der Entities (1.6). Der Auftrieb ändert die Jolt-Körper des
  Servers (oder des Spiels) und nichts, was ein Client simuliert oder vorhersagt. Clients sehen die interpolierten Transforms.
* **Szenen ohne Wasser bleiben bitgleich zu vorher**: der Pfad endet bei `index.empty()`, die Schleifen von `step` ändern sich nicht. Beweis
  als Test in Schritt 2 (Negativkontrolle): dieselbe Szene mit 64 Körpern, 300 Schritte, einmal mit `HE_PHYSICS_WATER=0`, einmal ohne
  Wasser: Positionen und Rotationen byte-gleich.
* **Die Zeit ist Simulationszeit**: Wellen, Spuren und Ringalter in der Physik laufen mit `dt` je `step`, nie mit `sunDir.w`,
  `steady_clock` oder der Renderzeit. Das ist die einzige Stelle, an der eine Wandzeit in die Simulation hätte geraten können.
* **Reihenfolgenunabhängig**: der Impuls je Körper hängt nur vom Zustand dieses Körpers, dem unveränderlichen Index und `dt` ab; die
  Reihumbedienung sortiert nach (Schuld, Entity-Id). Weder Hash- noch Broadphase-Reihenfolge geht ein.
* **Threadanzahl** ändert nichts (Auftrieb läuft auf dem Haupt-Thread vor `Update`, Jolt ist unabhängig von der Workerzahl).
* **Das Budget** bleibt wandzeitabhängig, wo es das schon war (Charaktere). Dass die Auftriebszeit den Charakteranteil verkleinert,
  macht keinen Starrkörper zeitabhängig, nur die Aufschubentscheidung der Charaktere; `HE_PHYSICS_BUDGET_MS=0` bleibt der Schalter für
  bitgenaue Server.
* **Ringe** sind Anzeige, nicht Zustand der Simulation; sie verändern keinen Körper.
* **Grenze, ehrlich**: Jolt ist ohne `CROSS_PLATFORM_DETERMINISTIC` gebaut, und die Wellen benutzen `std::sin` (libm unterscheidet sich
  zwischen Plattformen im letzten Bit). Bitgleiche Simulation über Plattformen hinweg war nie zugesagt und ist es auch jetzt nicht. Käme je
  ein Lockstep- oder Rollback-Modell, wäre `std::sin` durch eine eigene Tabelle zu ersetzen. Der Auftrieb ist dafür bereits eine reine
  Funktion aus (Körperpose, Flächenebene, Strömung, festes `dt`).

## 9. Tests (Plan)

Alle neuen Tests mutationsgeprüft (Zusicherung einzeln umkehren, Test rot sehen, wiederherstellen), `HE_CONFIG_DIR`/`near`/`far`/`nan`-Regeln
der Windows-Portabilität beachten (`windows-ci-portability-rules`).

| Datei | Inhalt |
|---|---|
| `tests/test_water_query.cpp` (Schritt 2) | Pegel ohne Wellen = `levelAt` in Welt; Landschaft verschoben, um Y gedreht, skaliert (Weltmatrix zusammengesetzt, nicht gespeichert); trocken außerhalb, am Rand, bei `clipToGround`; zwei Landschaften (höchste gewinnt, Gleichstand kleinere Id); Wellen: Amplitude, Summe, Normale gegen Differenzenquotient, `steepness = 0` flach; Zeit nur als Argument (gleiche `t` → gleiche Antwort); `physics` im Szenentext nur bei Abweichung, Rundlauf, `sameContent`, `Delta`, `sanitize` (NaN, Grenzen); `setPhysics` baut das Netz nicht neu |
| `tests/test_water_buoyancy.cpp` (Schritt 2) | Kiste (Dichte 500) schwimmt, eingetaucht ≈ 50 % (±10 %), kommt zur Ruhe; schwerer Körper (Dichte 2500) sinkt und liegt auf dem Grund, schläft; sehr leichter und sehr dünner Körper schwingt nicht auf (Höhenspanne nach 3 s < 2 cm); Aufprall bremst (Geschwindigkeit nach Eintauchen < vor Eintauchen); `flow` treibt ab, `flow = 0` nicht; `affectedByWater = false` fällt durch; Sensor und Kinematik unberührt; schlafender Körper wird bei Pegeländerung geweckt; Obergrenze (300 nasse Körper, 256 je Schritt, alle in 3 Schritten bedient, `buoyancyDeferred` zählt); **Negativkontrolle: ohne Wasser byte-gleich zu `HE_PHYSICS_WATER=0`**; zwei Läufe byte-gleich; 174-Test angepasst |
| `tests/test_water_ripples.cpp` (Schritt 3) | `RippleField`: Einfügen, Voll → schwächster ersetzt, Zusammenfassen innerhalb 0,3 m, Altern/Verfall, Drift, `pack` (Layout, `head[0]`), Auslöser: Aufprall ab 1 m/s, nicht darunter, Spur rate-begrenzt, höchstens 4 je Schritt |
| bestehend, erweitert | `test_engine_materials.cpp` (Blockgrößen, übersetzte Wasser-Shader auf allen Backends), `test_material_graph.cpp` (Knoten), `test_scene_serializer.cpp` (Starrkörper-Felder byte-gleich bei Standard), `test_editor_help`, `test_hc_node_docs`, `test_inspector_ui` |
| Bild (Schritt 3 und 4) | Metal und OpenGL: `HE_DUMP_WATERRIPPLE` (Zeuge: Ring bei t = 0,2 / 0,8 / 1,6 s, Kontrolle ohne Ring byte-gleich zu vorher, dieselbe Zeit zweimal byte-gleich); Vulkan: neuer Fall in `scripts/he_vk_imagetests.py` (Schlüssel `samples` plus `checks`, `allow`-Schlüssel gegen `ALLOWED_VALIDATION` prüfen) |

## 10. Schnittstellenvertrag und Aufteilung der Schritte 2 bis 6

Damit die Schritte nebeneinander laufen können, sind die Schnittstellen hier festgelegt; ändern darf sie nur, wer den Vertrag im Hive nachzieht.

| Schritt | Inhalt | Dateien (neu = fett) | Hängt ab von |
|---|---|---|---|
| 2 Physik | `BodyPhysics`, `setPhysics`, Serializer, `WaterQuery`, `applyWater`, `simTime()`, Budgetfelder, `RigidBodyComponent`-Felder, Tests, 174-Test anpassen | `WaterField.h/.cpp`, `SceneSerializer.cpp`, `RigidBodyComponent.h`, **`WaterQuery.h/.cpp`**, `PhysicsWorld.h/.cpp`, `PhysicsBudget.h`, `PhysicsTuning.h`, `ProfilerPanel` (Felder), **`test_water_query.cpp`**, **`test_water_buoyancy.cpp`**, `tests/CMakeLists.txt`, `HE_Scene/CMakeLists.txt` | 174 in `release/0.7.0` |
| 3 Ringe Metal/GL | `RippleField`, `WaterRippleBlock`, `IRenderer`-Ablage, `Lighting`-Anhang, Vorspann, `FillMaterialWater`, Knoten, Wassergraph, `Water.hasset`, Fill-Stellen Metal und GL, Ablauf in der Anwendungsschleife, Zeuge | **`WaterRipples.h/.cpp`** (Scene), **`Renderer/WaterRipples.h`**, `IRenderer.h`, `MaterialShaderLibrary.h/.cpp`, `LightPacking.h/.cpp`, `MaterialGraph.h/.cpp`, `MatGen/main.cpp`, `EngineContent/Materials/Water.hasset`, `MetalRenderer.mm`, `OpenGLRenderer.cpp`, `GameApplication.cpp`, `EditorApplication.cpp`, Knoten-Stellen (Abschnitt 6.4 Punkt 6), **`test_water_ripples.cpp`**. Auslöser aus der Physik erst nach Schritt 2 (Haken in `applyWater`) | 174 für den Wassergraph; **ABI und `RippleField` unabhängig von Schritt 2**, Ringe lassen sich vorher per Zeuge (`ripples().add`) zeigen |
| 4 D3D/Vulkan | `FillMaterialWater` an den vier Stellen (D3D11, D3D12, Vulkan), Offline-Übersetzung HLSL/SPIR-V, Vulkan-Bildfall, Lauf auf NN-WS03 und lavapipe | `D3D11Renderer.cpp`, `D3D12Renderer.cpp`, `VulkanRenderer.cpp`, `scripts/he_vk_imagetests.py`, ggf. `D3D12MaterialRootSignature.h` (Kommentar) | ABI aus Schritt 3 (Commit mit Struct und Vorspann) |
| 5 Editor/API | Inspector-Zeilen, Hilfe, `water`-Zeilen, Profilerzeilen | `InspectorPanel.cpp`, `TerrainTools.cpp`, `EditorHelp.cpp`, `EngineApi.h/.cpp`, `HcNodeDocs.cpp`, `ProfilerPanel`, Tests | Schnittstellen aus Schritt 2 (Header genügt) |
| 6 Abnahme | Vollbau, volles `ctest`, Messung (Auftrieb µs je Körper, Ringe Pass-p50), CI-Läufe lesen, Bericht | Doku | alle |

Reihenfolge der Commits mit Wirkung auf die anderen: Schritt 2 zuerst `WaterField.h` (Struct) und `WaterQuery.h` (Header, auch leer
implementiert) pushen, Schritt 3 zuerst `Lighting`-Anhang und Vorspann pushen, dann können 4 und 5 beginnen.

## 11. Realismus bis zur Frist 12.10. 07:00

Stand der Prüfung: `hive_deadline` meldet 442 Minuten.

**Voraussetzung (blockierend):** Dieser Zweig enthält das Wassermodell nicht (1.1). `prozedurales-wasse-12` merged gerade `release/0.7.0`
in den Wasserzweig und legt den PR nach `release/0.7.0` an. Erst wenn dieser PR in `release/0.7.0` liegt, kann ein Schritt 2 bauen:
dann `git merge origin/release/0.7.0` in den Themenzweig (Zweig der eigenen Release, kein Geschwisterzweig). Lehnt der Klassifikator den
Merge ab, nicht umgehen, die Queen fragen (Memory `auto-mode-classifier-blocks-pr-merge-and-website-edit`; der Weg "Datei byte-gleich übernehmen, Neues in
eigene Datei" aus `hive-worker-depends-on-unmerged-pr` ist nur der Notweg, hier viele Dateien betroffen). Schritt 3 kann den ABI-Teil
(Struct, Vorspann, `RippleField`, `IRenderer`, Fill-Stellen) ohne das Wassermodell bauen, nur Wassergraph und Zeuge brauchen es.

Schätzung der Dauer je Schritt einschließlich Bau (Mac, geteilte Kerne, bis zu acht Arbeiter; **Schätzung, nicht gemessen**):

| Schritt | Aufwand | Einschätzung |
|---|---|---|
| 2 Physik | 2 bis 2,5 h (zwei Bauten von HE_Scene und `he_tests`, Bench) | **machbar**, wenn 174 bis etwa 02:00 in `release/0.7.0` liegt |
| 3 Ringe Metal/GL | 2,5 h (Knoten, Regeneration, neun Fill-Stellen, Zeuge, Messung) | **machbar**, ABI-Teil sofort startbar; schwerster Schritt |
| 4 D3D/Vulkan | 1 h Code plus 1 bis 1,5 h CI (Windows 17 bis 54 min je Lauf) | **machbar**, nur wenn Schritt 3 bis etwa 03:30 den ABI-Commit gepusht hat; ein roter CI-Lauf braucht eine zweite Runde |
| 5 Editor/API | 1,5 bis 2 h | **Minimalfassung machbar** (Inspector-Zeilen, Hilfe, vier reine Zeilen plus `disturb`); die optionalen Teile (7.1) fallen weg |
| 6 Abnahme | 1 h plus CI | **teilweise**: voller `ctest` ja, Windows und Vulkan nur so weit die CI vor 07:00 durchläuft |

Parallel: 2 und 3 gleichzeitig, 5 nach den Headern von 2, 4 nach dem ABI-Commit von 3, 6 zuletzt. Der engste Pfad ist 174 → 3 → 4 → CI,
etwa 5 bis 6 h. **Streichreihenfolge bei Zeitnot**: (1) optionale Editorteile, MCP-Werkzeug, (2) Spurringe (`kind 1`) behalten nur den
Aufprall, (3) Dichte-Voreinstellungen und "Wellen aus Material", (4) `setFlow` als Skript-Zeile, (5) Bench für Konvexhüllen (dafür
Hüllenkörper vorerst als Kugel nähern und das ehrlich dokumentieren). **Nicht streichen**: Negativkontrolle "ohne Wasser bitgleich",
Blockgrößentests, Zeuge Metal/GL, CI-Lauf.

**Widerspruch zu einer festen Regel der Queen:** Das Memory `no-d3d-vulkan-work-on-mac` sagt, D3D11/D3D12/Vulkan-Arbeit nicht zu
vergeben, weil sie auf dem Mac nicht prüfbar ist. Das Thema verlangt Schritt 4 ausdrücklich, mit Abnahme über NN-WS03 und lavapipe. Vorschlag:
Schritt 4 klein halten (vier Fill-Zeilen, die Shaderquelle ist geteilt), auf dem Mac nur Offline-Übersetzung und Blockgrößen prüfen,
die Abnahme der CI überlassen und im Ergebnis ehrlich als "kompiliert, CI-geprüft, nie auf echter Hardware gesehen" ausweisen. **Die
Entscheidung gehört der Queen.**

## 12. Nicht Teil, Risiken, offene Punkte

Nicht Teil (Themenvorgabe und Entscheidungen oben): Fluidsimulation, Partikelgischt, Unterwasser-Postprocessing, Schwimmen von Charakteren,
gekippte Landschaften, Vertexwellen am Wassernetz, Rückkopplung der Ringe auf Körper, ortsabhängige Strömung, mehrere Probepunkte pro Körper,
Phasengleichheit zwischen Shaderwellen und Physikwellen, Wasser ohne Landschaft (freie Wasservolumen).

Risiken:
1. Der Auftrieb bei sehr leichten oder sehr dünnen Körpern (Stabilität bei 60 Hz): Grenze 4 ist ein Startwert, Test "settles" in Schritt 2.
2. Hüllenkörper mit vielen Flächen: Kosten von `GetSubmergedVolume` unbekannt, daher Bench vor der Zusage von 256 je Schritt.
3. `setFragmentBytes` (Metal) hat nach dem Anhang 1472 Byte Reserve; jede weitere ABI-Erweiterung ist dort die nächste Wand.
4. `Water.hasset` muss neu erzeugt und ins Deploy kopiert werden; ein vergessener Schritt zeigt auf dem Bild "keine Ringe" und sieht wie ein Fehler
   im Shader aus.
5. Windows-Fallen in neuen Tests: keine Bezeichner `near`/`far`, kein `nan` (`quiet_NaN` nehmen), kein `HE_API` in HE_Scene.
6. Der 174-Test (`Water surface: physics does not see the sheet…`) wird mit Auftrieb rot, wenn er nicht angepasst wird (3.1).
7. Das Wassermodell liegt noch nicht in `release/0.7.0`; Namen aus 1.1 können sich beim Merge der Konflikte geändert haben.

Offene Entscheidungen für die Queen:
* **D1** Schritt 4 trotz Regel gegen D3D/Vulkan auf dem Mac (Abschnitt 11).
* **D2** `affectedByWater` Standard `true` (Verhaltensänderung für dynamische Körper im Wasser) gegen Opt-in. Empfehlung: `true`.
* **D3** Besitzer des `RippleField`: `HorizonWorld` (Vorschlag) gegen ein Feld in `TerrainSystem`.

## 13. Stand nach Schritt 2 (12.10.2026): was gebaut wurde, wo es vom Konzept abweicht, was gemessen wurde

Schritt 2 (Wasserhöhe-Abfrage und Auftrieb in der Physik-Schicht) ist gebaut: `HE_Core/include/Physics/WaterWaves.h`,
`HE_Scene/WaterQuery.h/.cpp`, `BodyPhysics` in `WaterField.h`, `PhysicsWorld::applyWater`, zwei Felder am Starrkörper, Budgetzahlen,
Tests `test_water_query.cpp` und `test_water_buoyancy.cpp`. Gebaut und getestet auf dem Mac (Release, shaderc ON). Windows und
Linux nur über die CI. **Wo dieser Abschnitt den Abschnitten 1 bis 12 widerspricht, gilt dieser Abschnitt.**

### 13.1 Abweichungen vom Konzept

| Konzept | Gebaut | Grund |
|---|---|---|
| 5.1 ein Faktor `drag` | `BodyPhysics::linearDrag` (0,5) und `angularDrag` (0,05), je 0 bis 10, dazu `enabled` | Die Schrittvorgabe nennt lineare **und** angulare Dämpfung und Ein-/Ausschalten je Wasser |
| 5.1 `waves[2]` je Wasserkörper | keine Wellen am Körper. Die Wellen sind die des Materials, **eine** Quelle in `Physics/WaterWaves.h` (`kWaveA/B/C`, `waveAmplitude`, `sampleWaves`); die Physik folgt `kWaveA` + `kWaveB` | "Eine gemeinsame Quelle, keine Kopie": `mat_gen` baut die Knöpfe `WaveA/B/C` aus denselben Konstanten, `Water.hasset` bleibt **byte-gleich** (neu erzeugt und mit `cmp` verglichen), `test_engine_materials` pinnt das gelieferte Asset gegen den Header. Eigene Wellen je Körper wären eine Kopie, die auseinanderläuft |
| 3.2 Broadphase-Abfrage je Wasserkörper | je **Landschaft**: das Rechteck der nassen Zellen, Y vom tiefsten Pegel minus 512 m bis zum höchsten Pegel plus Wellenhöhe plus 1 m. Das Rechteck wird zwischengespeichert, Schlüssel (Entity, `Field::revision`, Auflösung, Raster, Körperzahl) | einfacher; ein Körper trägt seine Zellen ohnehin nur über `bodyAt` ein, die Gegenprobe macht `surfaceAt` je Kandidat |
| 3.4 schlafende Körper bei Pegeländerung wecken | **nicht gebaut.** `Field::revision` wird geführt (`setPhysics`, jeder Zellenedit, Undo-Replay), `WaterIndex::revisionOf` liest sie, nichts nutzt sie | Zeit |
| 3.4 nur aktive Körper | gebaut, mit zwei Ergänzungen: ein **teilweise eingetauchter** Körper bekommt `SetAllowSleeping(false)` und schwimmt damit nie ein, bis er ganz unter oder ganz über Wasser ist oder das Wasser ausgeschaltet wird; ein Körper in Ruhe behält den Anteil, mit dem er zur Ruhe kam (`submergedFraction`) | siehe 13.3 |
| 8.1 Auflage 3 (Zeit buchen) | gebaut: `PhysicsBudgetStats::buoyancyMs`, `wetBodies`, `buoyancyDeferred`; die Zeit zählt in `chargedMs` und kommt vom Anteil der Charaktere ab. **Nicht** gebaut (Schritt 5): Profiler-Zeilen, Projekt-JSON-Schlüssel `waterBodiesPerStep`, Profiler-Scope `Buoyancy` | Zeit |
| 6 Ringe, 7 Editor und Skript-API | nicht Teil von Schritt 2. Haken für Schritt 3: `PhysicsWorld::submergedFraction(entity)` (Eintritt = der Anteil springt von 0 auf größer 0) und `simTime()`; für Schritt 5: `WaterIndex::build(world).surfaceAt(x, z, physics.simTime(), s)` | |

Der Besitz der Wassereinstellungen: `HE::water::Body::physics` (`BodyPhysics`), gesetzt über `water::setPhysics(tc, id, p)`
(klemmt auf die Grenzen, erhöht `Field::revision`, setzt **nie** `dirty`, das Rechteck oder `TerrainComponent::dirty`, es wird kein Netz neu
gebaut). Gespeichert wird `"physics"` im `waterBodies`-Eintrag **nur bei Abweichung** und nur mit den abweichenden Schlüsseln;
Starrkörper: `"affectedByWater": false` und `"density"` nur bei Abweichung. Eine Szene ohne Abweichung ist byte-gleich zu vorher.

### 13.2 Gemessen (Mac, Release, ein Physik-Thread, Test "what 256 floating bodies cost", gemeldet und nicht geprüft)

* **256 halb eingetauchte Quader (1 m³, Dichte 500), Obergrenze 256:** 0,28 bis 0,38 ms je Schritt, **1,1 bis 1,5 µs je Körper**, schlechtester
  Schritt 0,49 ms. Das sind 7 bis 10 % des Standardbudgets von 4 ms bei voller Obergrenze, in der Spanne der Schätzung aus 8.1 (1 bis 3 µs).
  **Nicht gemessen:** Konvexhüllen mit vielen Flächen (der Plan nannte sie als Risiko; `GetSubmergedVolume` schneidet dort jede Fläche), Windows, Linux.
* **Ohne Wasser in der Szene:** `applyWater` ist eine Ansicht über die `TerrainComponent`s plus `empty()`, wird **nicht** ans Budget gebucht, und
  64 fallende und stapelnde Körper über 300 Schritte sind **bitgleich** zum Lauf mit `setWaterPhysics(false)` (Positionen und Drehungen mit
  `memcmp`). Dasselbe gilt für Wasser in der Ecke, an das kein Körper herankommt. Die Gegenprobe (derselbe Aufbau über einem See) unterscheidet
  sich, der Vergleich ist also nicht leer. Zwei Läufe derselben nassen Szene sind bitgleich.
* **Netz:** nichts davon wird repliziert (nur `TransformComponent`), die Wellen laufen auf der Simulationszeit (`PhysicsWorld::simTime`, Summe der `dt`,
  Doppelte Genauigkeit), nie auf `sunDir.w` oder `steady_clock`. Jolt ist weiter ohne `CROSS_PLATFORM_DETERMINISTIC` gebaut, `std::sin` ist
  plattformabhängig im letzten Bit: bitgleich **innerhalb** einer Plattform, nicht zwischen Plattformen (wie schon vorher).

### 13.3 Grenzen und was die Tests sagen (ehrlich)

* **Starke Dämpfung.** Jolts Widerstand greift am Auftriebsschwerpunkt unter dem Massenmittelpunkt an. Ein riesiger Stoß (`linearDrag` 10 bei 4 m/s) stößt
  auch die Drehung an, das explizite Verfahren verlässt seinen Stabilitätsbereich, und die Geschwindigkeit fällt auf fast 0 und kriecht auf rund 0,8 m/s
  zurück (gemessen). Der brauchbare Bereich ist 0 bis etwa 3 (Jolt empfiehlt 0,5); die Grenze 10 ist eine Klemme gegen Unsinn, keine Empfehlung.
* **Kriechen im ruhigen Wasser.** Ein auf dem Seegang schaukelnder Quader wandert ohne Strömung in 12 s rund 1,6 m (quadratischer Widerstand eines
  schaukelnden Körpers hebt sich nicht auf). Mit 1 m/s Strömung sind es über 8 m. Die Tests prüfen "klar weniger als mit Strömung", nicht "null".
* **Einschlafen.** Die Planungsrechnung sagte voraus, dass ein Körper auf dem Seegang in der Flaute zwischen zwei Wellen unter Jolts Schlafschwelle
  (0,03 m/s für 0,5 s) fällt und einfriert. Das ließ sich **nicht reproduzieren**: sechs Quader und sechs Kugeln über 40 s schliefen auch ohne den
  Schutz nie ein (Mutation: `SetAllowSleeping(false)` entfernt, Test blieb grün). Der Schutz bleibt als Vorsicht; **seine Notwendigkeit ist nicht
  belegt.** Die andere Hälfte, das **Zurückgeben** der Schlaferlaubnis, ist belegt: ohne sie liegt der gesunkene Stein wach auf dem Grund und wird in jedem
  Schritt bedient (Mutation: Test `served == 0` wurde rot).
* **Eine Ebene am Schwerpunkt** (3.5) und **kein Aufwecken bei Pegeländerung** (13.1): ein Körper, der auf trockenem Ufer eingeschlafen ist, wenn der Pegel
  steigt, schwimmt erst, wenn ihn etwas berührt oder weckt.
* **Nicht getestet:** Hüllen (Konvex), Seile und Gelenke im Wasser, überlappende Landschaften mit Körpern darauf (nur die Abfrage ist getestet), Landschaften mit
  Drehung um X oder Z (nicht unterstützt, 2), Windows, Linux, D3D und Vulkan (nicht Teil dieses Schritts).

## 14. Stand nach Schritt 3 (12.10.2026): Ringe im Wassermaterial, Metal und OpenGL

Schritt 3 (Wassermaterial reagiert auf Körper) ist gebaut: die Physik hebt bei Eintauchen, Austauchen und Bewegung im Wasser Ringe in eine kleine,
begrenzte Liste, die Anwendungsschleife packt sie jeden Frame in einen Block, jeder Graph-Shader liest ihn über die Beleuchtungs-ABI, und der Wassergraph
neigt damit seine Normale und weißt den Kamm. Gebaut und im Bild belegt auf **Metal und OpenGL** (Mac). **D3D11, D3D12 und Vulkan sind in diesem Schritt
nicht angefasst** (Queen, Schritt 4): ihre Dateien enthalten kein `FillMaterialWater`, sie zeichnen das Wasser ohne Ringe, kompilieren aber den neuen
Shadertext (gemeinsame Quelle, Tests unten). Wo dieser Abschnitt den Abschnitten 1 bis 13 widerspricht, gilt dieser Abschnitt.

### 14.1 Was gebaut wurde

| Teil | Wo | Inhalt |
|---|---|---|
| Block | `HE_Core/include/Renderer/WaterRipples.h` | POD `WaterRippleBlock { head[4]; a[16][4]; b[16][4] }` (head: Anzahl, Ringtempo m/s, Abklingzeit s; a: Mitte XZ, Startradius, Stärke; b: Alter, Art), `kWaterRippleMax = 16`, Arten `WaterRippleKind` (0 Aufprall, 1 Spur, 2 Skript, 3 Austritt) |
| Ablage | `IRenderer::SetWaterRipples/GetWaterRipples` | nicht virtuell, Member ganz am Ende der Klasse: keine vtable-Stelle und kein älterer Offset verschiebt sich |
| ABI | `MaterialShaderLibrary::Lighting` + `kLightingPreamble` | drei Felder am Ende (`waterRipples`, `waterRippleA[16]`, `waterRippleB[16]`, 33 vec4 = 528 Byte, gesamt 2624 Byte); `LightPacking.cpp` hält per `static_assert` Größe und Blockfelder gleich und die Gesamtgröße unter Metals 4096 Byte (`setFragmentBytes`) |
| Füllen | `HE::FillMaterialWater` (`LightPacking`) | Metal: Szenen-Pass (`FillMaterialLighting`) und G-Buffer-Pass; OpenGL: das gemeinsame `fillMatLight` (Vorwärts, G-Buffer, Resolve). Nicht in den UI-Passes und der Material-Vorschau: dort bleibt der Anhang 0 = keine Ringe |
| Knoten | `MatNodeType::WaterRipple` ("Water Ripple", Kategorie Input) | Eingang Position (Vec2, ohne Draht die Welt-XZ des Fragments), Ausgänge Height (m), Slope (Vec2, Gradient der Höhe), Foam (0..1). Nur Fragmentstufe, in einem WPO-Körper liefert er Nullen. Eine Hilfsfunktion `heWaterRipple` über höchstens 16 Einträge, bei Anzahl 0 läuft die Schleife nie |
| Graph | `MatGen/main.cpp` → `Water.hasset` | Neigung wird **nach** dem Gust-Faktor zur Wellenneigung addiert und geht durch das **eine** `Normalize3` (Fresnel, Refraktion, Kaustik und Glanzlicht sehen sie), Schaum wird zur Schaummaske addiert (mit dem Lace aufgebrochen), beides mit Distanz-Fade 30 bis 120 m. Kein neuer Parameter (alle 16 Slots sind belegt) |
| Liste | `HE_Scene/WaterRipples.h/.cpp` | `HE::water::RippleField` (16 Einträge): `add`, `advance`, `clear`, `pack`, `effectiveStrength`; Konstanten `kRingSpeed` 1,1 m/s, `kDecayTime` 1,4 s, `kMinStrength` 0,02, `kMergeDistance` 0,3 m, `kMergeAge` 0,25 s; `RippleTriggers` für die Auslöser |
| Besitzer | `PhysicsWorld::waterRipples()` | die Liste liegt in `PhysicsWorld::Impl`, wird in `step()` mit dem `dt` der Physik gealtert (Simulationszeit) und mit der Welt (`clear()`) geleert |
| Auslöser | `PhysicsWorld::applyWater` (`raiseRings`) | Anteil unter Wasser im Vorschritt gegen den jetzigen, Geschwindigkeit **vor** dem Auftriebsimpuls; Aufprall, Austritt und Spur, höchstens 4 + 2 je Schritt |
| Schleife | `HE::water::pushRipples(renderer, physics)` | einmal je Frame neben dem Umgebungs-Push: Spiel (`GameApplication.cpp`) und Editor (`pushEnvironment`, nur im Play-Modus) |
| Zeugen | `HE_DUMP_WATERRIPPLE`, `HE_DUMP_WATERDROP` | Ringe aus Text (Ort, Radius, Stärke, Alter) und die ganze Kette mit echter Physik (Kiste fällt in einen See) |

Die Regeln der Auslöser (`RippleTriggers`, `raiseRings`):

* **Aufprall (Art 0):** der Anteil unter Wasser springt von unter 2 % auf 2 % oder mehr **und** die Geschwindigkeit in die Fläche beträgt mindestens 1 m/s.
  Stärke `clamp(v / 6, 0,15, 1) * splash`, Startradius = horizontale halbe Ausdehnung des Körpers, 0,15 bis 3 m. Ein Körper, der schon im Wasser steht (Szenenstart),
  oder der langsam hineinkommt, macht keinen Aufprall; danach wird die erste Spur um ein Intervall verschoben, damit sie nicht auf dem Aufprall im selben Schritt liegt.
* **Austritt (Art 3):** der Anteil fällt unter 2 % bei mindestens 1,5 m/s nach oben. Stärke `clamp(v / 8, 0,1, 0,6) * splash`, Radius 0,8 der Ausdehnung.
* **Spur (Art 1):** der Körper schneidet die Fläche (Anteil 2 % bis 98 %) und bewegt sich **relativ zur Strömung** mit mindestens 0,6 m/s: alle 0,12 s Simulationszeit
  ein kleiner Ring (Stärke `clamp(v / 8, 0, 0,5) * splash`, Radius halbe Ausdehnung, 0,12 bis 1 m). Spuren verschmelzen nicht (sie sind die Linie hinter dem Körper).
* **Die Strömung trägt den Ring** (`drift` = Strömung des Wasserkörpers in Welt-XZ). `splash` am Wasserkörper (`BodyPhysics::splash`) skaliert alles, 0 = dieses Wasser macht keine Ringe.
* **Grenzen:** höchstens 4 Aufprälle und Austritte und 2 Spuren je Schritt, in der Reihenfolge der Bedienung (nach Schuld, dann Entity-Id: deterministisch); die Liste hat 16 Einträge;
  voll ersetzt ein stärkerer Ring den schwächsten, ein schwächerer als alle wird verworfen; ein junger Ring derselben Art (keine Spur) innerhalb von 0,3 m wird aufgefrischt statt verdoppelt;
  ein Ring unter 2 % Reststärke fliegt raus. Die Rechnung steckt in `applyWater`, also im gebuchten Teil (`buoyancyMs`).

Der Shader (Startwerte, am Bild abgestimmt, die **Schnittstelle** — Blocklayout und Knotenanschlüsse — ist der Vertrag): ein Ring ist ein Wellenpaket auf seiner Front `r0 + Tempo * Alter`,
ein Kosinus mit einer Wellenlänge `clamp(r0, 0,3, 1,2)` m unter einer Gauß-Hülle der Breite `Wellenlänge * (0,7 + 0,35 * Alter)`, Stärke `s * exp(-Alter / τ) / sqrt(Front / r0)`.
Die Neigung ist der **analytische** Gradient der Höhe (Normale und Höhe stimmen überein), skaliert auf 0,5 bei voller Stärke, unabhängig von der Wellenlänge; der Schaum ist die Hülle am Kamm,
dünn und nur für starke junge Ringe (`0,7 * smoothstep(0,45, 1,0, Stärke)`, die Krone eines Aufpralls, nach einer halben Sekunde weg).

### 14.2 Abweichungen vom Konzept (Abschnitt 6)

| Konzept | Gebaut | Grund |
|---|---|---|
| 6.3 Besitzer `HorizonWorld::ripples()` (D3 offen) | `PhysicsWorld::waterRipples()` | `PhysicsWorld.h` hat 42 Include-Stellen, `HorizonWorld.h` 180; das Alter läuft in `step()` auf der Simulationszeit (8.2), ohne dass die Anwendung es tickt; die Liste wird mit der Welt geleert. Ein Skript (Schritt 5, `water.disturb`) fügt über denselben Zugriff hinzu |
| 6.1 / 12: Ringe sind reine Anzeige | unverändert gebaut. **Der Schritttext verlangt mehr:** "Die Wellenhöhe aus Schritt 2 enthält diese Störungen, damit Optik und Physik übereinstimmen" | `WaterIndex::surfaceAt` liest die Ringe **nicht**. Das hält die Determinismus-Aussage von 8.2 (Ringe verändern keinen Körper) und das Zeitbudget. Die Rückkopplung wäre ein CPU-Spiegel der Ringhöhe in `surfaceAt` (Höhe `0,5 / k * Hülle * cos`, bei vollem Ring etwa 6 cm), bei leerer Liste bit-gleich zu heute. **Entscheidung der Queen (12.10.): Anzeige-only**, bewusste Abweichung vom Schritttext; Optik und Physik können im Zentimeterbereich der Ringhöhe auseinanderliegen. Möglicher Folgeschritt (rund 30 bis 40 Minuten mit Tests), später zu entscheiden |
| 6.2 Spuren verschmelzen mit jungen Ringen | Spuren verschmelzen nie | Sonst klebt ein Ring am bewegten Körper, statt eine Linie dahinter zu lassen |
| 6.2 Arten 0 bis 2 | zusätzlich Art 3 (Austritt) | Der Schritttext verlangt Eintauch- **und Austauch**-Ereignisse |
| 6.4 Punkt 4: Füllen an neun Stellen | Metal (2) und OpenGL (1 gemeinsame Stelle); **D3D11, D3D12, Vulkan nicht** | Queen: D3D/Vulkan-Dateien gehören zu Schritt 4 mit der Hardware-Abnahme. Die UI-Pässe und die Vorschau bleiben ohne Ringe (Plan: dort nichts nötig) |
| 6.4 Punkt 1: `IRenderer::SetWaterRipples` "wie `SetEnvironmentSettings`" | nicht virtuell | Kein Backend überschreibt es; ein neues `virtual` verschöbe alle späteren vtable-Stellen |
| 6.4 Punkt 5: Formelkonstanten | andere Startwerte (siehe oben) | Plan nannte sie ausdrücklich Startwerte; Neigung statt Höhe als Maß (0,5 = Neigung bei voller Stärke) |

### 14.3 Belegt und gemessen (Mac, Release, shaderc ON; gemeldet, wo nicht ausdrücklich geprüft)

**Bilder** (Headless-Dump, `scripts/he_shot.py`, `HE_SKY_TIME=1.0`, frischer `HE_CONFIG_DIR`, Vorwärtspfad, AA aus, `WATERTEST=floor TOD=0.4 CAMY=4 CAMZ=6 PITCH=-25`, Bilder in `docs/water-physics/`):

* **Kontrolle ohne Ringe = Bild vor der Änderung, Byte für Byte**, auf Metal (md5 `ecdaf6f1…`) und OpenGL (`bf28f2ec…`), obwohl der Shadertext des Wassers sich geändert hat (der Ring-Zweig läuft bei Anzahl 0 nicht, die Summen addieren 0).
* **Ring** (`HE_DUMP_WATERRIPPLE="0,-3,0.8,1,<Alter>"`) bei 0,2 / 0,8 / 1,6 s: bei 0,2 s die weiße Krone mit einem Ring darum, bei 0,8 s drei bis vier Ringe, bei 1,6 s weiche, größere Ringe. Auf Metal **und** OpenGL.
  Dieselbe Eingabe zweimal ergibt dasselbe Bild (Metal `0e134fe5…`, OpenGL `b84cec49…` bei 0,8 s). Metal gegen OpenGL: 5 Pixel mehr abweichend als im Kontrollbild (7,907 % gegen 7,906 %, größte Abweichung 5 von 255; das ist der Unterschied der Backends, nicht der Ring).
* **Die ganze Kette** (`HE_DUMP_WATERDROP=0.8`, echte `PhysicsWorld`): eine Ein-Meter-Kiste mit halber Dichte fällt aus 3 m in einen See, landet in Schritt 45, 0,8 s später liegen 3 Ringe auf dem Wasser, die Kiste taucht ein, um sie laufen Ringe. Das Bild auf Metal und OpenGL; dieselbe Szene ohne Ringe
  (`HE_DUMP_WATERDROPRINGS=0`) unterscheidet sich in 3,25 % der Pixel (dem Ring).

**Tests** (alle neu mit Negativkontrollen): `tests/test_water_ripples.cpp` 14 Fälle, 298 Zusicherungen (Liste, Block, Lighting-Anhang samt **Offsets im SPIR-V**, Auslöser, Szenen ohne Ring: ohne Wasser, Wasser aus, `splash` 0, Start im Wasser, langsamer Eintritt; Drosselung, Simulationszeit, Determinismus Byte für Byte).
`test_material_graph` (Knoten Water Ripple: Text, Fragment-Stufe und WPO-Nullen, Rundlauf, Querkompilierung aller Knoten für Metal und GLSL, GL 4.30 gelinkt 87/87), `test_engine_materials` (Blockgröße auf SPIR-V, HLSL und D3D12-Layout gleich `sizeof(Lighting)`, `Water.hasset` trägt `heWaterRipple`). Voller `ctest`: siehe Ergebnis im Hive-Bericht (Abschnitt unten ergänzt, falls abweichend).
**Mutationsprobe** (sechs Mutationen in einem Bau gesetzt, die Fälle rot gesehen, zurückgenommen): Spuren verschmelzen, Geschwindigkeitsgrenze des Aufpralls, `advance` im Physikschritt, Klemme der Anzahl in `FillMaterialWater`, Ringtempo im Block (`head[1]`) — **fünf rot, eine gleichwertig**:
der Frühausstieg bei `splash <= 0` in `raiseRings` ist nur eine Abkürzung, denn die Stärke wird ohnehin mit `splash` multipliziert und `add` verwirft Ringe unter 2 %.

**Kosten.** Auslöser: der Bench `what 256 floating bodies cost` (Schritt 2) meldet jetzt 0,36 ms je Schritt, 1,41 µs je Körper, schlechtester Schritt 0,47 ms (Schritt 2: 0,28 bis 0,38 ms und 1,1 bis 1,5 µs): kein Unterschied außerhalb der Streuung. Die Rechnung steckt in `applyWater`, also im gebuchten Teil.
**Nicht gemessen:** die Kosten des Shaders (Pass-p50 mit und ohne Ringe; `he_perf_capture.py` braucht ein Projekt mit Wasserszene), Windows, Linux, D3D11, D3D12, Vulkan.

### 14.4 Grenzen, ehrlich

* **Anzeige, keine Geometrie.** Die Ringe lehnen die Normale und weißen den Kamm; das Netz der Fläche bleibt flach. An der Silhouette (flacher Blick) und von nahe am Wasser sieht man
  sie nur über das Licht, nicht als Höhe. Sie wirken bis 120 m (Distanz-Fade 30 bis 120 m, wie der Kräusel-Fade der Wellen).
* **Welt-XZ, ohne Bindung an einen Wasserkörper.** Ein Ring gilt an dieser Stelle für **jede** Wasserfläche mit dem Wassermaterial: zwei Flächen übereinander (See über See) zeigen beide den
  Ring, ebenso eine Wasserebene neben dem See, die dasselbe Material benutzt. Die Physik kennt die Höhe der Fläche, an der sie den Ring erzeugt hat, der Shader nicht.
* **Simulationszeit.** Bei angehaltener Physik (Zeitskala 0, Pause, nicht gespielt) altern die Ringe nicht und stehen still; im Editor ohne Play gibt es keine (`pushEnvironment` schiebt dann
  den leeren Block). Ein Frame mit mehreren Physikschritten altert sie mehrfach und zeichnet den letzten Stand (höchstens fünf Schritte, 83 ms: nicht zu sehen).
* **Bounded.** 16 Einträge: ein Spiel mit vielen schnellen Körpern verliert die schwächsten Ringe (das ist die Absicht, die Obergrenze ist auch die Kostengrenze des Shaders).
  Ein sehr großer Körper (Hüllradius über 3 m) macht einen Ring von höchstens 3 m Startradius.
* **Nur Wassermaterial.** Der Knoten steckt in `Water.hasset`; Material-Instanzen davon erben ihn, ein eigenes Wassermaterial muss `Water Ripple` selbst einbauen (Neigung von der Normale abziehen,
  Schaum addieren, wie `mat_gen` es tut).
* **D3D11, D3D12, Vulkan.** Nicht angefasst. Sie binden `sizeof(Lighting)` Byte (der Anhang ist also da, 0 Ringe), rufen aber `FillMaterialWater` nicht auf, zeichnen deshalb **keine Ringe**.
  Den neuen Shadertext kompilieren sie (gemeinsame Quelle, `test_engine_materials`, `test_material_graph`, auf dem Mac gegen glslang, SPIRV-Cross und die Reflexion, auf Windows und Linux in der CI), **gesehen hat ihn
  dort keine Hardware.** Schritt 4 trägt das `FillMaterialWater` an den drei Stellen nach (Abschnitt 6.4 Punkt 4) und nimmt die Abnahme.
* **Optik und Physik.** `surfaceAt` liest die Ringe nicht (14.2): ein Körper reitet auf den Wellen des Materials, nicht auf seinen eigenen Ringen. Der Unterschied ist die Ringhöhe (bei voller Stärke etwa 6 cm).
