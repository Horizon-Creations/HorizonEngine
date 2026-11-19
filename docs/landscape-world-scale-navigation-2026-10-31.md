# Landscape im Weltmaßstab: Navigation je Kachel (Schritt 3, Teil d)

Stand 31.10.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 3 von 8 ("Physik, Navigation, Foliage und Wasser je Tile, Floating Origin mit Tiles").
Vorher: `landscape-world-scale-physics-2026-10-31.md`, `landscape-world-scale-foliage-water-2026-10-31.md`.

**Mit diesem Dokument ist die CPU-Seite von Schritt 3 vollständig: Physik, Navigation, Foliage und Wasser je Kachel, Floating Origin. Gezeichnet wird weder Foliage noch Wasser (Renderer, Teil B). Es wurde nichts gemessen: keine Bauzeit einer Navigationskachel, kein Speicher, keine Pfadsuche über 50 km (Plan-§4-Abnahme auf NN-WS03).**

## 1. Was gebaut ist

| Datei | Aufgabe |
|---|---|
| `LandscapeNavigation.*` | Detour-Kachel je Landschaftskachel, auf dem Job-Pool aus den Höhen der Kachel gebaut, vom Hauptthread in **ein** `dtNavMesh` eingefügt und wieder herausgenommen; `attach()` legt Mesh und Query in ein `NavMeshComponent` |
| `buildLandscapeNavTile` | der reine Recast-Bau einer Kachel (thread-sicher, ohne Zustand) |
| `LandscapeRuntime` (erweitert) | `setNavigation(config)`, `setNavigationAnchors`, `navigation()`; `update(world, ...)` nimmt die Nav-Agenten als Anker und legt das Mesh in das `NavMeshComponent` der Szene |
| `tests/test_landscape_navigation.cpp` | 7 Testfälle mit echtem Recast und echtem `NavigationSystem::moveTo` |

`NavigationSystem` ist unverändert: es liest `navMesh`/`navQuery` des ersten `NavMeshComponent` und addiert `world.origin()`. Eine Landschaftswelt braucht dort nichts Neues.

## 2. Warum je Kachel

`NavigationSystem::bake` ist ein einziger Recast-Lauf über alle Dreiecke einer Szene und verweigert Gitter über 4096 x 4096 Zellen. 100 km bei 0,5 m wären 200 000 x 200 000 Zellen. Stattdessen "tiled" im Sinne von Detour:

* **Detour-Kachel (x, z) = Landschaftskachel (x, z)**, Ursprung der Welt als Detour-Ursprung, `tileM` als Kachelgröße. Detour verbindet Nachbarkacheln beim Einfügen selbst; ein Pfad läuft über eine Kachelgrenze wie über jede Polygonkante.
* **Absolute Koordinaten** (Float): wie beim einzelnen Mesh ("in absoluten Koordinaten gebacken, mit dem Ursprung addiert abgefragt", `FloatingOrigin.h`). Bei 100 km ist die Float-Auflösung rund 1 cm.
* **Anker** sind, was läuft: im `update(world, ...)` jeder Nav-Agent, bei laufenden Agenten auch ihr Ziel. Gewollt sind Kacheln innerhalb `radius` (600 m), behalten bis `radius · 1,25`; nächste zuerst, höchstens `maxStartsPerUpdate` (1) neue Bauten je Aufruf, `maxInFlight` (2).
* **Gebaut wird eine Kachel erst, wenn ihre Nachbarn im Speicher sind** (deren Höhen füllen den Rand); am Weltrand wiederholt der Rand die Kante. Der Store muss also mindestens eine Kachel weiter laden als der Navigationsradius (Standard 8 km gegen 600 m); sonst wartet die Kachel (`stats().deferred`).
* **Höhenfenster:** die Kachel plus ein Streifen der Nachbarn, so breit wie der Recast-Rand (`walkableRadius + 3` Zellen) plus eine Probe. Dreiecke mit "\"-Diagonale wie der Renderer, gegen den Uhrzeigersinn von oben (nach oben zeigende Normale = begehbar).
* **Eine Kachel ohne begehbaren Boden** (See, Klippen) ist fertig und kostet nichts (`stats().empty`).

## 3. Fallen

* **Die Steigungsgrenze ist nicht `maxSlope`.** Recasts Ledge-Filter verwirft eine Spanne, deren Nachbarn auf beiden Seiten zusammen mehr als die Kletterhöhe abweichen; auf einer Rampe heißt das: Kletterhöhe ≥ 2 · Anstieg je Zelle. Wirksame Grenze: `min(maxSlope, atan(Kletterzellen · cellHeight / (2 · cellSize)))`. Mit dem Standard des einzelnen Meshes (Climb 0,9 m, Höhenzelle 0,25 m, Zelle 0,5 m) wären das **37 Grad** statt 45. Die Landschaft setzt deshalb `walkableClimb = 1,0 m` (4 Höhenzellen, 45 Grad). Test: 40 Grad verbindet, 50 Grad nicht, und mit Climb 0,9 m wird der 40-Grad-Fall rot.
* **256 Kachel-Plätze** (`kMaxTiles`), 14 Bit für Polygone (16 384 je Kachel), 10 Bit Salz: die 32-bit-`dtPolyRef`. Bei rund 16 Kacheln je Anker sind das etwa 16 weit verstreute Agenten. Ist alles belegt, startet kein neuer Bau; schlägt `addTile` doch fehl, wird die Kachel wieder gewollt (kein "fertig ohne Mesh"). Eine Kachel ab 16 384 Polygonen wird verweigert (`failed`). **Dieser Pfad ist nicht getestet** (256 Kacheln mit Recast zu bauen wäre kein Unit-Test).
* **Teilpfade:** `findPath` zu einem Ziel, dessen Korridor keine Kacheln hat, liefert eine Route bis zum Rand des Vorhandenen; `NavigationSystem::planPath` behandelt das als Erfolg, der Agent läuft und bleibt stehen, und neu geplant wird nur, wenn sich `targetPos` um mehr als 0,25 m bewegt. Das ist **Verhalten des bestehenden Systems**, nicht geändert. Ein weiter Weg braucht ein Skript, das nachfragt, sobald die Kacheln da sind. `moveTo` zu einer Kachel ohne Mesh gibt false.
* **`findPath` hat 256 Polygone** (`NavigationSystem`), der Query-Knotenpool 2048 wie beim einzelnen Mesh: ein sehr langer Weg wird gekürzt.
* **`minRegionArea`/`mergeRegionArea` in Zellen, wie `bake()`.** (Eine Quadrierung wie im Recast-Beispiel wäre 8-fach anders bei derselben `NavMeshConfig`.)
* **Das Mesh bleibt im `NavMeshComponent`, wenn die Landschaft geht.** `reset()` und `setNavigation(nullptr)` leeren die Laufzeit, nicht die Komponente: Agenten laufen auf dem letzten Stand der Kacheln weiter. Abhängen kann der Aufrufer (`navMesh = nullptr`); eine neue Landschaft hängt ihr eigenes Mesh ein, sobald sich der Zeiger unterscheidet.
* **Die Tests laufen mit 1 m Zellen** (Topologie genügt) und Climb 1,5 m; nur der Fall "40 Grad / 50 Grad" nutzt die Standardwerte der Landschaft (0,5 m).
* **Windows:** Aufräumen wartet auf laufende Bauten (5 s), Destruktor und `clear()`; die Tests geben die Pack-Datei nach dem Lauf frei.

## 4. Was offen ist

* **Wasser ist begehbar** (Agenten waten, wie beim einzelnen Mesh); keine Tiefengrenze, keine Wasser-Fläche als Hindernis.
* **Keine Entity-Geometrie:** Häuser, Brücken, Props sind nicht im Mesh (Entity-Zellen, Schritt 5).
* **Keine Off-Mesh-Verbindungen**, keine Bereichs-Kosten (alle Polygone Fläche 63, Filter Standard).
* **Kein Neubau bei Änderung einer Kachel** (der Store hält unveränderliche Kacheln; Sculpt kommt mit dem Editor, Schritt 6).
* **Kein Backen im Editor, keine Anzeige** (`extractNavMeshWireframe` arbeitet auf dem Komponenten-Mesh und zeigt, was drin ist).
* **Nichts gemessen** (§ oben). Der Aufbau einer Kachel mit 0,5 m Zelle ist ein Gitter von rund 1030 x 1030 Zellen und liegt auf dem Low-Pool; wie lange das dauert und ob 2 gleichzeitige Bauten reichen, entscheidet NN-WS03.

## 5. Verifikation

Verifiziert (`cmake-build-bench`, echte `he_tests`): `test_landscape_navigation` 7 Fälle dreimal hintereinander grün; `test_landscape_*` 113, `test_navigation*` 14, `test_foliage*` 76, `test_physics*` 134, `test_world_scale*` 44, `test_scene_serializer*` 46, `test_cell_*` 34, `test_engine_api*` 140, alle grün. CI Windows, Linux, Linux Vulkan und macOS für den Stand `43a90014` (Foliage und Wasser) grün; die Navigations-Commits sind bis zum Push nicht in der CI.
Mutationsprüfungen, jeweils rot: Recast-Rand 0 (Pfade über Kachelgrenzen und im Laufzeit-Test brechen), Kacheln nie herausnehmen, Climb 0,9 m (40-Grad-Rampe nicht mehr begehbar).
**Nicht verifiziert:** Zeiten, Speicher, der Fall "alle 256 Plätze belegt", ein Pack mit echtem Gelände (alle Tests laufen auf Funktionshöhen und prozeduralen Quellen), Windows/Linux-Lauf der Navigations-Tests.

## Handoff

1. `LandscapeRuntime::setNavigation(&config)` einschalten, Agenten und `NavMeshComponent` in die Szene; `update(world, dt)` baut und hängt ein. Ohne Konfiguration baut die Laufzeit nichts.
2. Die Stellen, an denen es leicht falsch geht: die Steigungsgrenze aus Climb und Zellgröße (§3), das Nachfragen bei Teilpfaden, das Mesh im Komponenten nach dem Ende der Landschaft.
3. Schritt 3 ist damit auf der CPU-Seite fertig. Offen für 1521 im strengen Sinn: Zeichnen von Foliage und Wasser (Renderer, Teil B) und alle Messungen auf NN-WS03.
