# Entity-Cooking Schritt 4: Entkochen bei Änderung

Stand 16.11.2026, Thema 343. Setzt auf `docs/entity-cooking-cooked-static-2026-11-16.md` (Schritt 3).

## Was gebaut ist

Jede Mutation einer gekochten Entity schickt ihre Zeile auf `Pending` (oder baut den Block neu), **vor** dem nächsten Extract.
Die Zeile wird beim nächsten Walk von selbst wieder `Ready`, wenn die Entity kochbar bleibt. Es gibt keinen zweiten Zustand.

| Weg | Mechanismus | Stelle |
|---|---|---|
| Transform (Engine-API, Lua, Python, HorizonCode, Gizmo, Kollaboration, bewegter Parent) | `propagateTransforms` zeigt dem Block jede geschriebene Weltmatrix; eine `Ready`-Zeile mit anderer Matrix wird `Pending` (`worldWritten`). Kein Hook an den Schreibstellen: propagate erkennt Änderungen ohnehin per Byte-Vergleich | `TransformHierarchy.cpp` `Work::wrote` (3 Schreibstellen), `CookedStatic.h` `worldWritten` |
| Komponente hinzu/weg/ersetzt (Script, Animator, ... = Verdikt-Komponenten) | entt `on_construct/on_update/on_destroy` → `stale`; `refreshCookedStatic` baut beim nächsten Extract neu. Nur wenn die Entity eine Zeile hat oder Kinder (Regel 10) | `CookedStatic.cpp` `VerdictComponents`, `onVerdictChanged` |
| Mesh/Material hinzu/ersetzt/weg | dieselben Signale → die Zeile `Pending` | `OutputComponents`, `onOutputChanged` |
| Reparent | `structureEpoch` (schon vorhanden) → Neubau; Zeile ist danach `Pending` | unverändert |
| Inspector (rohe Feldschreibzugriffe) | `HE::uncookEntity(registry, entity)` für die gezeigte Entity, jeden Frame | `InspectorPanel.cpp` `renderForImpl` |
| Material-Parameter per Engine-API | schreibt das Material-Asset, nicht die Entity → `contentEpoch` → alle Zeilen `Pending` (Schritt 3) | unverändert |

Standard von `m_cookStatic` ist unverändert **aus** (der Wechsel auf Spiel/Export an ist der spätere Standard-Schritt).

## Entscheidungen

- Transform-Erkennung am einen Schreiber (`propagateTransforms`) statt an jeder API-Stelle: `entt`-Signale sehen rohe Pointer-Schreibzugriffe nicht.
  Der Vergleich ist gegen die Matrix der Zeile (nicht gegen "dirty"), also kein falscher Alarm, wenn ein Wert hin und zurück geschrieben wird.
- Signale nur für das, was sie sehen (hinzu/weg/ersetzt). Rohe Feldschreibzugriffe auf Mesh/Material gibt es nur im Inspector (dort explizit).
  Terrain/Wasser-Systeme (`TerrainSystem.cpp`, `WaterSurface.cpp`) schreiben `meshAssetId`/`materialAssetId` roh zur Laufzeit; Chunks sind `Walk`,
  Wasser nur mit Engine-Wasser-Material. Ein Wasserkörper mit eigenem Material wäre ungeprüft (siehe Pending).
- Eine Entity, die den Block verlassen hat (Blocker kam), wird nach dem Entfernen des Blockers **nicht** zurückgeholt, bis die Struktur sich ändert.
  Immer korrekt, nur nicht gekocht; ein Neubau je Entfernen wäre O(N) je Komponente auf jeder ungekochten Entity.
- Neubau (nicht Pro-Zeile-Entkochen) für Verdikt-Komponenten, weil ein neuer Blocker auf einem Parent die Kinder mitnimmt.

## Geprüft

- `he_tests` (Release, Metal-Mac): `*test_cooked_static.cpp` 14 Fälle / 25 311 Assertions grün (auch mit `HE_PROPAGATE_VERIFY=1`), davon neu 9:
  Transform über 8 Wege (roh, ScriptApi, `api::transform::setPosition/setWorldPosition/setRotation/setScale`, Registry-Zeile `transform.setPosition`
  = HorizonCode, Lua `horizon.setPosition`), gleicher Wert bleibt gekocht, 8× hin und her (Zeilen == `worldMatrixOf` jede Runde),
  Gruppe bewegt, Reparent hin/zurück/hin, 23 Komponententypen hinzu und weg, Blocker auf Parent, Material/Mesh ersetzt, `uncookEntity`.
  Jeder Test vergleicht den Extract Feld für Feld mit einem Extractor ohne Kochen, im selben Frame wie die Mutation.
  Grün auch: `test_cook_analysis` 6/105, `test_culling` 77/19849, `test_world_scale` 44/6926, `test_layer_mesh_instancing` 6/229,
  `test_engine_api` 130/11461, `test_transform_propagation` 14/1132.
- `HorizonEditor` baut und linkt (Release).
- Pixel A/B (`scripts/perf/frame_pixel_ab.sh`, Satz `frame`, 16 Shots, Metal, `HE_SKY_TIME=30`, AA aus): Kochen aus gegen `HE_COOK_STATIC=1`, alle 16 md5 gleich, keiner fehlt.
  Wie in Schritt 3: eine Szene mit vielen statischen Meshes ist nur über Würfelreihen/Foliage im Satz, nicht als 50k-Bild.
- **Nicht geprüft:** Python-Frontend (läuft über dieselbe `ScriptApi`, kein eigener Test; Python-Build nicht probiert), echte Kollaborationssitzung (nur der rohe
  Schreibweg getestet, den sie benutzt), Spielstart-Hook, Vulkan/D3D, Windows/Linux, **Laufzeitkosten des Hooks nicht gemessen** (kein `he_perf_capture`-Lauf in diesem Schritt;
  er kostet je geschriebener Matrix einen Array-Zugriff und nur bei einer `Ready`-Zeile einen 64-Byte-Vergleich).

## Handoff

- **Topic / step:** Thema 343, Schritt 4 von 4. Commit `35d73605d` (Code + Tests) + dieses Doc.
- **Pending:** (1) Standard von `m_cookStatic` für Spiel/Export umlegen und Editor aus lassen (eigener Schritt). (2) Hook-Kosten und Neubau-Kosten
  (`analyzeCookability` über alle Entities je `stale`/Epoche) messen, bevor der Standard an geht. (3) `analyzeCookability(reg)` loggt "Static aber blockiert"
  bei jedem Neubau erneut; Neubauten sind jetzt häufiger (stale). (4) Wasserkörper mit eigenem Material: `WaterSurface.cpp:256,322` schreibt Mesh/Material roh,
  ohne Zeilen-Entkochen (nur Engine-Wasser ist `Walk`). (5) `VerdictComponents` in `CookedStatic.cpp` muss mit `ownBlockers` in `CookAnalysis.cpp` Schritt halten
  (der Test fügt sie einzeln hinzu, fängt aber keinen neuen Typ, der in keiner Liste steht). (6) `TransformHierarchy.cpp` wird von anderen Themen geteilt; der Diff ist klein (`Work`).
- **Traps:** `entt`-Signale feuern bei `emplace_or_replace` auf vorhandener Komponente als `on_update`, bei rohem `get<T>().feld = x` gar nicht.
  `HE::api::Entity` ist `uint32_t`, `HorizonWorld`-Entity `entt::entity` (Test: `static_cast<uint32_t>`). Network-Default `replicates = true` blockiert.
