# Entity-Cooking Schritt 5: Export packt das Kochurteil, Projekt-Setting

Stand 17.11.2026, Thema 343. Setzt auf `docs/entity-cooking-cooked-static-2026-11-16.md` (Schritt 3) und
`docs/entity-cooking-uncook-2026-11-16.md` (Schritt 4).

## Was gebaut ist

- **Wo der Export wirklich schreibt:** nicht `hpak_packer`/`asset_compiler` (die kennen keine Szenen), sondern
  `ExportDialogPanel.cpp` (zwei `saveToMemory`-Aufrufe: Startszene und die Schleife über alle `.hescene`). Die gepackte Szene ist ein
  CBOR-Blob (`json::to_cbor(buildSceneJson)`), kein HAsset mit Chunks. Das "Tail-Chunk" ist deshalb ein **optionaler Top-Level-Key
  `cookedStatic`** im Szenen-JSON (Geschwister von `levelScript`/`cells`/`streaming`): ältere Leser laufen daran vorbei, fehlt er, wird beim
  ersten Extract wie bisher analysiert. Keine Versionsnummer bewegt sich.
- **Inhalt = nur das Urteil**: die UUIDs der Entities, die `analyzeCookability` freigab (16 Byte je Entity, base64, ein String; `{version:1,
  cookable}`). Keine Matrix, keine Bounds, keine entt-Handles. Die Zeilen sind nach dem Laden `Pending` wie sonst; die erste Walk füllt sie.
  Das sind die drei Fallen der Handoffs (Matrix vom ersten Walk, `Unresolved` bleibt Pending, rohe Skript-Schreibzugriffe).
- `SceneSerializer::saveToMemory(world, out, bool packCookVerdict = false)` schreibt den Key (`addCookedSeed`); `applySceneJson` liest ihn als
  letzte Anweisung (`applyCookedSeed`, solange `idMap` lebt) und ruft `seedCookedStatic(reg, entities, world.structureEpoch())`.
- `CookedStatic`: `seed`, `seedEpoch`, `enabled`, Zähler `seededBuilds`/`analysedBuilds`. `refreshCookedStatic` nimmt den Seed genau dann,
  wenn Epoche gleich, nicht `stale`; sonst `analyzeCookability`. Der Seed ist einmalig (wird nach dem Neubau geleert).
- Ungültig (Analyse übernimmt): ein Entity wurde zwischen Laden und erstem Extract erzeugt/gelöscht/umgehängt (Epoche), eine
  Urteils-Komponente kam/ging irgendwo (`onVerdictChanged` setzt vor dem ersten Bau immer `stale`), ein UUID im Key löst nicht auf oder
  hat kein Transform+Mesh (ganzer Seed verworfen, Warnung, `enabled` bleibt aus).
- **Schalter:** `ProjectSettings::game.cookStaticEntities` (Default **aus**, JSON `game.cookStaticEntities`), Zeile "Cook static entities"
  unter Project Settings ▸ General ▸ Performance, Help-Eintrag `Project General/Cook static entities`.
  Ein-Schalter-Semantik: an = der Export schreibt das Urteil; **eine Welt, die aus so einer Szene lädt, kocht** (`CookedStatic::enabled`,
  `RenderExtractor.cpp` liest es neben `HE_COOK_STATIC`). Kein Backend-/IRenderer-Weg nötig. Ein Pack ohne den Key (älterer Export oder
  Setting aus) zeichnet wie vorher.
- **Defaults (Entscheidung):** Setting aus. Editor aus (nur `HE_COOK_STATIC=1`), Export aus, Spiel kocht nur, wenn das Pack den Key trägt.
  Das Umlegen des Defaults für alle Projekte ist ein späterer Schritt.

## Geprüft

- `he_tests` (Release, Metal-Mac): `*test_cooked_static.cpp` 18 Fälle / 26 676 Assertions grün, davon neu 4: Pack mit Urteil lädt, Block
  ist vor dem Extract nicht gebaut, danach `seededBuilds==1 && analysedBuilds==0`, Zeilen == statische Menge, alle `Ready`, Extract Feld für
  Feld gleich einer Welt aus einem **Pack ohne den Key** (zwei Frames); Pack ohne Key (alte Form): kein Block, Zeichnen wie die Quellwelt;
  Skript vor dem ersten Extract auf einer als statisch gelisteten Entity: Analyse, Entity nicht gekocht (der Test fand zuerst einen echten
  Fehler: `onVerdictChanged` hörte vor dem ersten Bau nichts); Entity nach dem Laden erzeugt: Analyse; Urteil nennt entferntes Entity:
  Seed verworfen. Grün auch: `test_cook_analysis` 6/105, `test_project_settings` 33/624, `test_scene_serializer` 46/1835,
  `test_cell_split` 13/410.
- `scripts/editor_help_audit.py --check`: 1244/1244, 0 fehlend.
- **Messung (Ladekosten, das ist der Gewinn dieses Schritts):** Wegwerf-Test, `refreshCookedStatic` direkt nach `loadFromMemory`, Mittel
  aus 5, Release, M-Mac, Last ~3: 10 000 Entities 1,03 ms → 0,12 ms; 50 000: 4,87 → 0,57 ms; 200 000: 16,9 → 2,4 ms. Blob +4 %
  (50k: 14,20 MB gegen 13,67 MB unkomprimiert, LZ4 im Pak packt die UUIDs kaum). Die Differenz fällt bei jedem Spiel-Szenenladen an,
  nicht je Frame. Der Wegwerf-Test ist nicht eingecheckt.
- **Frame-Zeit: nicht neu gemessen.** Dieser Schritt ändert den Pfad je Frame nicht; die Zeilen sind dieselben wie bei `HE_COOK_STATIC=1`
  (Schritt 3: `Extract::meshes` −45 % in der 50k-Referenzwelt, Metal, Rohdaten `docs/perf-audit/raw-entity-cooking/mac_cook_*`).
- **Pixel-A/B: nicht auf einem echten Export gelaufen.** Der Export läuft über den Editor-Dialog, nicht headless. Ersatz: der Test
  vergleicht das ganze `RenderObject` je Zeile (Transform, Bounds, Mesh/Material, LOD, Schatten, Sections, Param) einer geseedeten Welt mit
  einer Welt aus dem Pack ohne Key.
- Gebaut: siehe Handoff unten.

## Handoff

- **Topic / step:** Thema 343, Schritt 5. Dateien: `SceneSerializer.cpp` (`addCookedSeed`/`applyCookedSeed`, `saveToMemory`), `CookedStatic.{h,cpp}`,
  `RenderExtractor.cpp` (1 Zeile: `packedCook->enabled`), `ProjectSettings.{h,cpp}`, `ProjectSettingsPanel.cpp`, `ExportDialogPanel.cpp`, `EditorHelp.cpp`.
- **Pending:** (1) Echter Export + Spiel-Lauf (Pixel-A/B und Frame-Zeit mit Setting an/aus) auf der 50k-Welt; braucht den Editor-Export-Dialog.
  (2) Default des Settings für alle Projekte umlegen. (3) Seed-Überleben im Spiel: jedes `createEntity` (Spieler-Spawn in `OnStart`) vor dem
  ersten Extract bumpt die Epoche und verwirft den Seed; dann läuft die Analyse wie vorher (korrekt, nur kein Gewinn). Nicht gemessen, wie oft das
  im echten Spiel passiert. (4) Rohe Feldschreibzugriffe auf Urteilsfelder (`RigidBody.type`, `NetworkComponent.replicates`, `LOD.levels`,
  Mobility-Modus) zwischen Laden und erstem Extract sieht der Seed nicht (die Analyse beim lazy Bau sähe sie); Transform-Schreibzugriffe fängt
  `worldWritten` weiter. (5) Die Kommentare in `RenderExtractor.h` zu "wird nicht entkocht" sind seit Schritt 4 veraltet. (6) Sceneladen
  per `loadAdditiveFromMemory` liest den Key nicht (neue Entities, ungekocht bis zum nächsten Neubau).
- **Traps:** doctest + `entt::null`: `REQUIRE((e != entt::null))`. Python-`str.replace` ohne Treffer ändert still nichts (der erste Versuch
  der `onVerdictChanged`-Änderung wurde nie angewandt, der Test fing es). Die Gesamtgröße der Szene wächst um 16 Byte je kochbarer Entity.
