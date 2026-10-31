# Landscape im Weltmaßstab: Boden für die Physik je Kachel (Schritt 3, Teil a)

Stand 31.10.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 3 von 8 ("Physik, Navigation, Foliage und Wasser je Tile, Floating Origin mit Tiles").
Vorher: `landscape-world-scale-store-2026-10-31.md` (Store, Kachel-Format), `landscape-world-scale-renderer-2026-10-31.md` (Quadtree, Laufzeit, Shader-Quelle).

**Dieses Dokument deckt die Physik ab. Navigation, Foliage und Wasser (§6) sind nicht gemacht, nur untersucht. Es wurde nichts gemessen:
weder die Jolt-Schrittzeit noch die Aufbauzeit eines Bodenfeldes, noch eine Fahrt über 50 km (alles Plan-§4-Abnahme auf NN-WS03).** Schritt 1521 bleibt offen.

## 1. Was gebaut ist

| Datei | Aufgabe |
|---|---|
| `PhysicsWorld::addGroundField / removeGroundField / hasGroundField / groundFieldCount` | statisches Jolt-Höhenfeld im Terrain-Kanal, vom Aufrufer geschlüsselt, ohne Entität; zählt in `bodyCount()`; `clear()` nimmt es mit; `shiftOrigin` verschiebt es wie jeden Körper |
| `LandscapePhysics.*` | Boden je geladener Kachel um die Anker, Entladen mit Hysterese, Aufbau-Budget, `holdsAt` für `setRegionHold` |
| `LandscapeRuntime` (erweitert) | `setPhysics(PhysicsWorld*, config)`, `setPhysicsAnchors`, `groundHolds`; `updateAt(..., origin)` |
| `tests/test_landscape_physics.cpp` | 7 Testfälle mit echtem Jolt |

## 2. Warum je Kachel

Die einzelne Landschaft hat **ein** Jolt-`HeightFieldShape` für ihre ganze Fläche (`PhysicsWorld.cpp`, `buildHeightFieldShape`, bis 1025 x 1025 Proben). 100 km mit 2 m Raster wären 50 000 Proben je Seite, 5 GB roh. Stattdessen:

* **Jede geladene Kachel im Umkreis eines Ankers** (Kamera, Spieler, Fahrzeug) bekommt ein eigenes Feld aus ihrer Stufe 0 (257 x 257 Proben, 2 m; `level` kann gröber sein, 129 Proben bei 4 m). Nachbarn teilen ihre Randproben, die Felder schließen ohne Lücke aneinander.
* **Radius 1,5 km** (`radius`); Boden bleibt, bis **jeder** Anker weiter als `radius · keepFactor` (1,25) weg ist: dieselbe Hysterese wie beim Store. Bei 512 m Kacheln sind das rund 27 Felder um einen Anker.
* **Nächste zuerst, höchstens `maxBuildsPerUpdate` (2) je Aufruf.** Ein Feld baut Jolt auf dem Hauptthread; wie lange das dauert, ist **nicht gemessen** (ob es auf einen Worker gehört, entscheidet eine Messung auf NN-WS03).
  Kacheln, die der Store noch nicht hat, werden zurückgestellt (`stats().deferred`) und beim nächsten Aufruf versucht.
* **Quelle sind nur die residenten Kacheln des Stores.** Ein Boden ist eine Kopie der Höhen (Jolt komprimiert sie); er überlebt, wenn der Store die Kachel inzwischen verdrängt hat, bis der Anker weg ist.

## 3. Floating Origin

`PhysicsWorld::shiftOrigin` bewegt **alle** Jolt-Körper (`GetBodies`), also auch die Bodenfelder; es muss nichts mitgeführt werden. `LandscapePhysics::update` bekommt bei jedem Aufruf den **aktuellen** `world.origin()` und baut neue Felder bei `absolute Ecke − Ursprung`.
Das Zusammenspiel ist getestet: Boden vor einer Verschiebung, Boden danach, kein Spalt dazwischen (Strahl durch alle Felder, Höhenunterschied an den Kanten gemessen).

## 4. Fallen und Entscheidungen

* **`kGroundOwner` statt `kNoEntity`.** Jolt-Körper tragen die Entität in `UserData`, und `raycast` überspringt den Körper, dessen Entität der "ignoriere"-Parameter nennt, dessen Standard `kNoEntity` (0xFFFFFFFF) ist. Ein Boden, der `kNoEntity` meldet, wird von **jedem** Standard-Raycast übersprungen
  (der erste Entwurf tat das; die Tests fanden es als "alle Strahlen gehen ins Leere", nachdem der Flachfeld-Test nur wegen der Entität 42 bestanden hatte). Boden ohne Besitzer meldet `PhysicsWorld::kGroundOwner` (0xFFFFFFFE, keine echte Entität); mit der Laufzeit meldet er die Landschafts-Entität.
  Wer einen Treffer in das Register nachschlägt, muss `kGroundOwner` kennen.
* **Halten, bis der Boden da ist.** Ein Körper über einer Kachel ohne Boden fällt durch. `LandscapePhysics::holdsAt` ist der Test für `PhysicsWorld::setRegionHold` (derselbe Mechanismus, mit dem `cellHolds` Körper über ungebauten Zellen festhält); außerhalb der Welt hält es nie.
  Eine Anwendung mit Zellen und Landschaft muss **beide** Tests in einem Lambda verbinden (`setRegionHold` nimmt nur einen). Getestet: ein Kasten über einer Kachel ohne Boden bewegt sich nicht, fällt danach auf den Boden und bleibt darauf.
* **Reihenfolge im Bild:** `LandscapeStore::update`, `LandscapeRuntime::update` (baut Boden), dann `PhysicsWorld::step`. Mit `LandscapeRuntime` ist das ein Aufruf, `update(world, dt)` vor dem Schritt.
* **Jolt packt jedes Feld für sich auf 8 Bit je 2 x 2-Block.** Gemessen auf dieser Testwelt (rau, 16-m-Raster, 1000 m Höhenumfang): größte Abweichung des Jolt-Bodens von der Kachelhöhe **0,20 m** an 3136 Gitterpunkten, größte Stufe über eine Kachelkante **0,21 m**.
  Das sind die gepackten Höhen zweier Felder mit denselben Randproben. Die einzelne Landschaft hat dieselbe Packung. Mit dem echten 2-m-Raster sind die Blöcke 4 m breit, der Fehler dort kleiner (nicht gemessen); ein Charakter mit Stufenhöhe 0,3 m merkt 0,2 m nicht, ein Fahrzeug mit steifer Federung eher.
* **Shape-Aufbau ohne Entität:** `HeightFieldShapeSettings(heights, offset 0, scale (step, 1, step), samples)`, Blockgröße 2; die Probenzahl muss 2ⁿ+1 sein (die Kachel-Mips sind es: 257, 129, 65, 33).
* **Körperreserve:** jedes Feld zählt gegen `kMaxBodies` (65 536); `bodyCount()` zählt sie mit, damit `cellBodiesFit` die Reserve richtig sieht. Bei ~27 Feldern je Anker ist das kein Druck.

## 5. Wie die Anwendung es benutzt (Teil B)

```cpp
m_landscape.setPhysics(&physicsWorld, cfg);                    // einmal, nach dem Start der Laufzeit
m_landscape.setPhysicsAnchors({ players..., vehicles... });    // absolute Positionen, jeden Frame
m_landscape.update(*m_world, gameDt);                          // VOR physicsWorld.step()
physicsWorld.setRegionHold([&](const glm::vec3& p) {           // neben cellHolds
    return m_landscape.groundHolds(p, m_world->origin()) || cellHolds(...);
});
```
Aufrufstelle: neben `updateCellStreaming` (`GameApplication.cpp:3297`), der Spiel-Schritt der Physik kommt danach. **Vor dem Zerstören der `PhysicsWorld` `setPhysics(nullptr)`** (die Laufzeit hält den Zeiger).

## 6. Was von Schritt 3 offen ist (untersucht, nicht gebaut)

| Teil | Befund heute | Was nötig ist |
|---|---|---|
| **Navigation** | `NavigationSystem.cpp` backt **ein** Recast-Höhenfeld (`rcCreateHeightfield` über die ganze Fläche) aus allen Meshes; Terrain-Chunks sind dabei gewöhnliche LOD-Entitäten | Recast-**Kacheln** (Tile-Cache) um die Anker, aus den Boden-Höhen der Kacheln statt aus Chunk-Meshes; größte Teilaufgabe |
| **Foliage** | `FoliageSystem.cpp` / `FoliagePaint.cpp` fragen `terrainHeightAt(tc, lx, lz)`, Steigung und Ebenengewichte direkt am `TerrainComponent` | eine Abfrageschicht über dem Store (Höhe, Steigung, Gewichte an einer absoluten Stelle aus den residenten Kacheln, dieselbe Triangulierung wie der Boden), dann Foliage-Puffer je Kachel/Region |
| **Wasser** | `water::Field` ist **eines** je `TerrainComponent` (Raster `res²`, Körper, Küstenschnitt gegen die Landschaftshöhe) | Felder je Region schneiden, Küstenschnitt gegen die Kachelhöhen; Datenmodell-Änderung |
| **Floating Origin mit Kacheln** | Physik und Frame sind ursprungsrelativ und getestet (§3); der Renderer bekommt Float-Positionen relativ zum Ursprung (`LandscapeFrame`) | Verschiebung im Zusammenspiel mit dem Renderer: Teil B |

Die Abfrageschicht (Höhe/Steigung/Gewicht an einer Stelle) ist das, was Foliage, Navigation und Skripte gemeinsam brauchen, und ist rein CPU: sinnvoll als nächstes.

## 7. Verifikation

* Verifiziert (Mac, echtes Jolt, ohne Zeitmessung): 7 Testfälle in `tests/test_landscape_physics.cpp` (Flachfeld, Schlüssel einmal, Eingaben; Höhen und Kachelränder ohne Lücke; Entladen und zwei Anker; Verschiebung des Ursprungs; Halten und Fallen; echte 257²-Größe und gröbere Mip; über die Laufzeit mit Anker-Liste).
  Zusätzlich im `he_tests`-Binary und mit den Nachbarn (siehe PR).
* Nicht verifiziert: Aufbauzeit eines Feldes, Jolt-Schrittzeit mit den Feldern (Plan: "unverändert gegenüber Catania"), Fahrzeug/Charakter über 50 km ohne Ruckeln, Verhalten mit mehreren Ankern in verschiedenen Teilen der Welt über Zeit, echte Heightmaps, Windows/Linux-CI für den neuen Code (Läufe werden durch Folge-Pushes abgebrochen).

## Handoff

- **Topic / step:** Hive-Topic 326, Schritt 3 von 8, **Teil a (Physik)**; Navigation, Foliage, Wasser offen
- **Branch / commit:** `claude/landscape-weltmassstab-100km` (PR 214), pushed: siehe PR
- **Done:** Bodenfelder je Kachel mit Hysterese, Budget und Halte-Test; Einbindung in die Laufzeit; Floating-Origin-Verhalten getestet; der `kNoEntity`-Fehler gefunden und behoben.
- **Files touched:** `PhysicsWorld.h/.cpp` (Ground-API, `bodyCount`, `clear`), `LandscapePhysics.*`, `LandscapeRuntime.*`, `tests/test_landscape_physics.cpp`; außerdem Korrekturen der Schritt-1/2-Tests und des Stores (Quelle nur beim Laden halten) wegen roter CI.
- **Verified:** §7. **Not verified:** §7.
- **Decisions taken:** je Kachel statt ein Riesenfeld; Radius 1,5 km mit Hysterese 1,25; Aufbau-Budget 2 je Aufruf; Boden meldet die Landschafts-Entität oder `kGroundOwner`; Halte-Test über den vorhandenen `setRegionHold`.
- **Decisions suggested, not taken:** Aufbau der Felder auf einem Worker (erst nach einer Messung); gröbere Mip für Boden jenseits einiger hundert Meter; eine Abfrageschicht über dem Store (§6) als Grundlage für Foliage und Navigation.
- **Pending (Mensch):** Raster/Authoring weiter offen; Freigabe für Messungen auf NN-WS03 (Schrittzeit, Aufbau, Fahrt über 50 km).
- **Traps:** `kNoEntity` als Besitzer versteckt den Boden vor jedem Raycast (§4); Jolt packt 8 Bit je Block, Kanten können bis ~0,2 m stufen (§4); `setRegionHold` nimmt nur einen Test, Zellen und Landschaft müssen in einem Lambda verbunden werden; Windows kann keine Datei löschen, die ein Leser noch offen hält.
- **Next steps:** 1. Abfrageschicht über dem Store (Höhe, Steigung, Gewichte an absoluter Stelle; Test gegen den Boden).  2. Foliage je Kachel auf dieser Schicht.  3. Recast-Kacheln.  4. Wasser je Region.  5. Messungen auf NN-WS03: Aufbauzeit, Schrittzeit, 50-km-Fahrt.
