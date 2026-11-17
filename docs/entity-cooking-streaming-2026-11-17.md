# Entity-Cooking Schritt 7: Streaming-Entities, Regel 11 entfällt

Stand 17.11.2026, Thema 343. Setzt auf `docs/entity-cooking-export-2026-11-17.md` (Schritt 5).

## Befund (vor jeder Änderung geprüft)

1. **entt-Handles sind beim Zell-Reload NICHT stabil, UUIDs sind es.** `CellStreamer` zerstört eine Zelle mit
   `world.destroyEntity(root)` und baut sie beim Wiederladen über `SceneSerializer::loadAdditiveFromJson` neu
   (`CellStreamer.cpp` ~367-391, ~613). `AdditiveOptions::preserveIds` (für C++-gesplittete Zellen, Kopf `streaming`) erhält die
   `EntityIdComponent`-UUIDs, nie das `entt::entity`: entt vergibt den Slot mit neuer Version neu. Ein `CookedStatic::entity`-Eintrag über eine
   Entladung hinweg wäre ein hängendes Handle; `rowOf` vergleicht deshalb das ganze Handle samt Version.
2. **Der Block hält nie ein Handle über einen Neubau hinaus.** Jedes `createEntity`/`destroyEntity`/`reparentEntity` bewegt
   `HorizonWorld::structureEpoch` (`HorizonWorld.cpp:138,144,422,485,550,615,642`); `refreshCookedStatic` baut dann die Zeilen aus der lebenden
   Registry (`reg.view<TransformComponent, MeshComponent>()`) neu. Eine entladene Zelle verschwindet also mit ihren Zeilen, eine neu
   geladene wird frisch analysiert und gekocht. Die UUID-basierten Seeds (Schritt 5) lösen ihre UUIDs über `idMap` beim Laden auf, nie gespeichert als Handle.
3. **Regel 11 war nur ein Stellvertreter.** Sie traf ausschließlich `StreamingComponent::Mode::Auto` (`CookAnalysis.cpp`); die Entities einer
   Zelle tragen sonst keine Zellmarke (`CookAnalysis.h`: "carry no marker"), waren also schon vorher kochbar und korrekt. Die Komponente steht
   außerdem im Header: "Read by the splitter only: the runtime never sees it". Die Regel verbot nur die Entities, die der Autor
   ausdrücklich als streambar markiert hatte, und schützte vor etwas, das Fakt 2 schon abdeckt.

## Entscheidung

- **Regel 11 entfällt.** `CookBlocker::Streaming` (Bit 1<<10, nicht persistiert, nur `CookAnalysis`/Tests lesen es) ist weg, ebenso die
  Überwachung von `StreamingComponent` in `CookedStatic.cpp` (`VerdictComponents`) und der Include. Auto-markierte Platzierungen sind jetzt kochbar.
- **Kein Zell-Seed.** Das Kochurteil pro Zelle mitzubacken bringt nichts, solange der Neubau nicht inkrementell ist:
  (a) der Seed aus Schritt 5 gilt für **eine** Epoche (`seedEpoch == structureEpoch`, einmalig, ganze Registry); ein additiver Zell-Load erzeugt viele Entities und bewegt die Epoche je Slice,
  es gibt keine Epoche, an die ein Zell-Seed gebunden werden könnte. (b) `refreshCookedStatic` analysiert bei JEDEM Epochenwechsel die ganze Welt
  (`analyzeCookability(reg)`), ein Zell-Seed spart davon nichts, solange die übrige Welt neu analysiert wird.
  Die Zell-Dateien im Export tragen den `cookedStatic`-Key schon (Export-Schleife über alle `.hescene`, Schritt 5), `loadAdditiveFromJson` ignoriert ihn
  (offener Punkt 6 aus Schritt 5), das ist für ein späteres inkrementelles Refresh gedacht.

## Geprüft

- `test_cook_analysis`: Fall "11 streaming" kehrt um (Auto und Resident kochbar).
- `test_cooked_static`: neu "CookedStatic cells: a streamed cell is cooked, leaves with its cell and comes back cooked": echter
  `splitWorldIntoCells` + `CellStreamer`, 30 Meshes, Zelle 10 m, vier davon `Mode::Auto`. Laden → Entladen → Laden: cooked Extract
  Feld für Feld gleich der Walk (`checkSameObjects`), Auto-Entities je Zeile vorhanden/`Ready`, nach dem Entladen keine Zeile auf ein totes Handle,
  Zeilenzahl nach dem Neuladen wie vorher. Das ist der Ersatz für ein Pixel-A/B (Export/Editor läuft nicht headless); `RenderObject` ist, was die
  Backends zeichnen.
- Gelaufen (Release, Metal-Mac, `out/build/release`, `he_tests` nach der Änderung neu gebaut): `test_cook_analysis` 6/104, `test_cooked_static` 19/26 914 (der neue Fall ist der 19.), `test_cell_split` 13/410, `test_cell_runtime` 8/373, `test_cell_village` 1/18 697,
  `test_cell_state` 8/260, `test_cell_scripts` 4/128, `test_scene_serializer` 46/1835: alle grün, die Cell-Tests unverändert. Der volle `he_tests`-Lauf ist nicht gelaufen. Frame-Zeit: nicht gemessen (die Änderung berührt keinen Pfad je Frame).

## Handoff

- **Topic / step:** Thema 343, Schritt 7. Dateien: `CookAnalysis.{h,cpp}`, `CookedStatic.cpp`, `tests/test_cook_analysis.cpp`, `tests/test_cooked_static.cpp`, `docs/entity-cooking-survey-2026-11-13.md` (Regel 11 gestrichen).
- **Pending:** (1) **Inkrementelles Refresh** ist der eigentliche Gewinn für gestreamte Welten: heute kostet jeder Zell-Load/-Unload (je Slice, ein Neubau pro Frame mit Epochenwechsel)
  eine volle `analyzeCookability` (Schritt 5 Messung: 4,9 ms bei 50k, 17 ms bei 200k). Dafür braucht es ein Urteil, das über Neubauten lebt (Gültigkeit
  gegen Reparent/Vorfahren-Ketten, Schritt-4-Signale) und danach erst den Zell-Seed als Eingabe. Eigener Schritt, nicht gemessen in diesem. (2) In einem gestreamten Spiel erzeugt
  `CellRuntime::update` vermutlich vor dem ersten Extract Entities, das verwirft den Szenen-Seed aus Schritt 5 (Epoche): dann gewinnt der Ladepfad dort nichts. Nicht gemessen.
  (3) Kein echter Export + Spiel-Lauf, kein echtes Pixel-A/B.
- **Traps:** Wer `StreamingComponent` wieder ins Urteil nimmt, muss es auch in `VerdictComponents` (`CookedStatic.cpp`) halten, sonst bleibt ein später aufgesetzter Auto-Mark unbemerkt.
