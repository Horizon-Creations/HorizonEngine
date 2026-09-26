# Prefab-Verknüpfung und Submeshes: Bestandsaufnahme (Stand 26.09.2026)

Thema 83, Schritt 1. Die Themenbeschreibung nahm an, dass (1) Prefab-Instanzen einer Asset-Änderung
nicht folgen und (2) Meshes mit mehreren Materialien nicht granular behandelt werden. Grundlage der
Prüfung sind `git merge-base`/`git rev-list` auf HEAD `152659ff` und das Lesen des Codes. **Es gab
keinen Editor- und keinen Build-Lauf.** Jede Zeile unten trägt ihren Prüfstatus:

- **geprüft**: Aufrufstelle bzw. fehlende Aufrufstelle per `grep` über `src/` belegt und gelesen
- **gelesen**: aus dem Code abgeleitet, aber nicht zur Laufzeit oder per Test belegt
- **nicht geprüft**: nur aus `gap-audit-2026-08-25.md` übernommen

Kurzfassung: **Beide Kernlücken sind geschlossen, die Themenbeschreibung ist veraltet.** Das
Prefab-System hat seit dem 14.09. Instanzen mit Bindungen, Overrides, Propagation, Revert und
Push-to-Prefab. Submeshes haben seit dem 13./17.09. eine Section-Tabelle, Zeichnen pro Slot auf allen
fünf Backends (auch skinned), LOD-Slot-Mapping und Material-Overrides pro Entity und Slot. Offen sind
Randpunkte, und der wichtigste ist, dass die Propagation **nicht live** ist: Sie läuft nur an festen
Punkten, nicht wenn das Asset sich ändert.

Auf den drei Feature-Zweigen liegt kein unverschmolzener Commit mehr (`git rev-list --count
HEAD..origin/claude/<zweig>` = 0 für `prefab-verknuepfung-mit-overrides`, `submesh-material-slots`
und `submesh-material-slots-d3d11-d3d12-vulkan-port-skeletal-draw`).

| Punkt | Urteil | Merge auf HEAD |
|---|---|---|
| Prefab-Instanz folgt dem Asset | **da**, aber nur bei Öffnen/Speichern/Push, nicht live | `d7e51c44` (Thema 33) |
| Submeshes / Material-Slots | **da**, Randpunkte offen | `f08f5ff8` (Thema 30) + D3D/Vulkan/Skeletal-Port |

---

## 1. Prefab-Verknüpfung: da, aber nicht live

### Was vorhanden ist

- `PrefabInstanceComponent` (`src/HE_Scene/include/HorizonScene/Components/PrefabInstanceComponent.h`)
  auf der Wurzel jeder Platzierung: Asset-UUID, Bindungen Template-Record ↔ Instanz-Entity,
  Override-Marker `{Template-Entity, Komponente, Property}`. Ersetzt `PrefabLinkComponent`.
- `SceneSerializer::syncPrefabInstance(s)` (`SceneSerializer.cpp:2927ff`, `:3214ff`): wendet die
  Records des Assets auf die gebundenen Entities an, lässt Overrides stehen, legt neue Records an,
  entfernt verlorene (außer wenn darunter etwas hier geändert wurde), adoptiert Alt-Instanzen ohne
  Bindungen.
- Override-Aufzeichnung über die Auswahl, sobald sich die Undo-Revision bewegt
  (`EditorApplication::recordPrefabEdits`, `EditorApplication.cpp:9672`).
- Inspector: Revert pro Override, Revert von Entfernen/Hinzufügen, Push-to-Prefab
  (`EditorApplication.cpp:9736-9838`). Outliner kennzeichnet Instanzen (`OutlinerPanel.cpp:522`, `:964`).
- MCP-Werkzeuge (`McpToolsPrefab.cpp`), Tests in `tests/test_prefab.cpp`,
  `tests/test_scene_serializer.cpp`, `tests/test_mcp_tools_prefab.cpp`, `tests/test_outliner_filter.cpp`.

### Wann synchronisiert wird (geprüft)

`EditorApplication::syncPrefabInstances` (`EditorApplication.cpp:9650`) wird genau hier gerufen:
Start (`:1758`), Szene öffnen (`:9938`, `:10028`), vor dem Speichern (`:9851`), nach Push-to-Prefab
(`:9835`). Sonst nirgends.

### Restliste, nach Wirkung sortiert

1. **In einer Collaboration-Session gibt es gar keine Propagation** (geprüft). `syncPrefabInstances`
   bricht bei `m_collab.inSession()` ab (`EditorApplication.cpp:9653-9658`), `recordPrefabEdits`
   ebenso (`:9696-9710`). Begründung im Code: der Pass schreibt direkt in die Welt statt über
   `EditorCommands` und würde nicht repliziert. In einer Session sind Instanzen also reine Kopien, und
   ein Edit an einer Instanz ist nicht vor dem nächsten Sync nach der Session geschützt.
2. **Verschachtelte Prefabs: äußere Instanz überschreibt vermutlich die innere** (gelesen, **nicht
   per Test belegt**). `syncPrefabInstance` sammelt `records` ungefiltert (`SceneSerializer.cpp:2927-2948`).
   Die Lösch-Logik nimmt Records einer verschachtelten Platzierung ausdrücklich aus
   (`insideNestedPlacement`, `:3003-3024`), die Anwende-Schleife (`:3104-3175`) nicht. Liegt eine
   Platzierung von Prefab I in Prefab O, schreibt der Sync von O seine beim Speichern von O eingefrorene
   Kopie der I-Werte auf dieselben Entities, die der Sync von I aus dem neueren I-Asset gesetzt hat.
   Wer gewinnt, entscheidet die Iterationsreihenfolge von `registry.view<PrefabInstanceComponent>()`
   (`:3227`). Kein Test deckt „I ändert sich, Platzierung steckt in O" ab (die Nested-Fälle in
   `test_prefab.cpp:1116`, `:1681` betreffen Löschen und Struktur). **Zuerst einen Test schreiben.**
3. **„Save as Prefab" verknüpft die Quelle nicht** (geprüft). `savePrefabOf`
   (`OutlinerPanel.cpp:164-233`) schreibt das Asset, lässt die Quell-Entity aber ohne
   `PrefabInstanceComponent`. Gestempelt wird nur beim Viewport-Drop (`ViewportPanel.cpp:1251`) und
   über den Blob-Schlüssel `prefab` (`SceneSerializer.cpp:1304`, MCP-Weg). Wer ein Objekt zum Prefab
   macht, hat danach im Level eine unverknüpfte Kopie und muss neu platzieren.
4. **Änderung auf der Platte zieht nicht nach** (geprüft). `ContentManager::pollHotReload` lädt eine
   geänderte Prefab-Datei neu (`ContentManager.cpp:2322ff`, generisch über alle geladenen Pfade), aber
   der Switch im Editor (`EditorApplication.cpp:2746-2790`) hat keinen `AssetType::Prefab`-Fall. Nach
   Git-Pull, Source-Control-Sync oder externem Schreiben bleiben offene Instanzen bis zum nächsten
   Öffnen oder Speichern veraltet. Dasselbe gilt für ein Prefab-Update eines Peers
   (`applyAssetBytes`, `:8293`, lädt nur neu).
5. **Export nimmt Szenen so, wie sie auf der Platte liegen** (geprüft: kein `syncPrefab` in
   `src/HE_Game`, `src/HE_Tools`, `ExportDialogPanel.cpp`, `McpToolsBuild.cpp`). Push-to-Prefab
   synchronisiert nur die gerade offene Szene. Eine andere Szene mit Instanzen desselben Prefabs geht
   mit dem alten Stand in den Export, bis sie einmal geöffnet und gespeichert wurde.
6. **Kein isolierter Prefab-Bearbeitungsmodus** (geprüft: `AssetType::Prefab` kommt in
   `src/HE_Editor` nur bei Icon, Filter, Kachel, Tutorial und MCP vor, es gibt keinen Tab dafür).
   Ein Prefab ändert man nur über eine Instanz und Push-to-Prefab.
7. **Kein Prefab-Spawn aus Skripten** (geprüft). Im Engine-API kommt „prefab" nicht vor. Laufzeit-Spawn
   gibt es nur für HorizonCode-Klassen (`entity.spawnClass`, `EntityHost.cpp:159` nutzt dabei intern
   `instantiatePrefab` auf dem Komponenten-Blob der Klasse). Ein Prefab-Asset lässt sich aus Lua,
   Python oder HC nicht instanziieren (Audit-Zeile 151 gilt weiter).

---

## 2. Submeshes und Material-Slots: da, Randpunkte offen

### Was vorhanden ist

- `MeshSection {indexOffset, indexCount, materialPath, materialId}` und `sections` in
  `StaticMeshAsset` und `SkeletalMeshAsset` (`src/HE_Core/include/ContentManager/Assets.h:100ff`),
  Chunk MSEC, Alt-Assets laden als eine Section (`meshSectionsOf`, `meshSectionsCover`).
- Importer bauen Sections: glTF (`MeshImporter.cpp`, `SkeletalMeshImporter.cpp`) und Assimp
  (`AssimpMeshImport.cpp`).
- Zeichnen pro Slot: `GeometryPass` macht aus `RenderObject::sections` einen DrawCall pro Section
  (`CommandBuffer.h:20-31`). Alle fünf Backends wenden den Index-Bereich an (geprüft: `DrawIndexRange`
  in `D3D11Renderer.cpp:1117`, `D3D12Renderer.cpp:1164`, `VulkanRenderer.cpp:53`,
  `OpenGLRenderer.cpp:42`, Metal `MetalRenderer.mm:5732`), auch im Skinned-Pfad.
- Material-Overrides pro Entity und Slot: `MaterialComponent::slotOverrides`, serialisiert
  (`SceneSerializer.cpp:258`, `:1037`), im Inspector als Liste „Slot Overrides"
  (`InspectorPanel.cpp:1808-1860`). Präzedenz: Slot-Override, dann Ganz-Mesh-Override, dann
  Section-Material (`RenderExtractor::resolveEntitySlots`, `RenderExtractor.cpp:192`).
- LOD-Slot-Mapping (`HE::lodSlotMap`, `RenderExtractor.cpp:180-241`).
- Slot-Editor im Static- und Skeletal-Mesh-Tab mit eigenem Undo (`MeshMaterialSlots.{h,cpp}`).
- Pak baut die Section-Materialien als UUID ein (`HpakWriter.cpp`, laut Commit `f08f5ff8`, hier
  nicht nachgelesen).

### Restliste

1. **Kein Material-Tausch aus Skripten** (geprüft). Das Material-API kennt nur `material.getParam`
   und `material.setParam` (`EngineApi.cpp:5627-5629`). Weder das Ganz-Mesh-Material noch ein
   einzelner Slot lässt sich zur Laufzeit umstellen. Im Audit (4.2, Stand 17.09.) als offen notiert.
2. **Section ausblenden geht nicht** (geprüft: kein Sichtbarkeitsfeld pro Section in `src/`). Einen
   einzelnen Teil eines Meshes (etwa ein Zubehörteil) abschalten kann man nur, indem man ihm ein
   transparentes Material gibt.
3. **Slots haben keine Namen** (geprüft: kein Namensfeld in `MeshSection`). Inspector und Mesh-Tab
   zeigen „Slot 0…N". Wie viel vom Materialnamen aus glTF/FBX über den Pfad des erzeugten Materials
   ankommt, ist nicht geprüft. Ein geleerter Slot ist jedenfalls nur noch über seine Nummer
   zuzuordnen.
4. **Kein „als Einzel-Meshes importieren"** (geprüft: keine entsprechende Option in `HE_Tools`/
   `HE_Editor`). Ein Mesh mit mehreren Nodes/Primitives wird zu einem Asset mit mehreren Sections,
   nie zu mehreren Assets oder einem Prefab aus Einzelteilen.
5. **Nicht geprüft, laut Audit 4.2 (17.09.) offen:** ein Slot-Material, das erst nach dem ersten
   Zeichnen auf der Platte erscheint, wird nicht nachgeladen (der Extractor merkt sich den
   Fehlschlag in `missing`, `RenderExtractor.cpp:129-153`); ein Slot mit einem Pfad, den nichts mehr
   auflöst, lässt sich im Mesh-Tab nur überschreiben, nicht leeren; die D3D/Vulkan-Skinned-Schleifen
   sind nur per Windows-CI kompiliert, nicht auf Hardware gesehen.

---

## 3. Vorschlag für Folgeschritte

Nur ein Vorschlag, die Planung macht der Chefchen. Reihenfolge nach Wirkung pro Aufwand:

1. **Test für verschachtelte Propagation** (S): I in O platzieren, I ändern, Sync, Wert prüfen,
   beide Iterationsreihenfolgen erzwingen. Falls rot: in der Anwende-Schleife Records von
   `insideNestedPlacement` genauso auslassen wie in der Lösch-Logik.
2. **Live-Propagation im Editor** (S–M): Hot-Reload-Switch um `AssetType::Prefab` ergänzen und
   `syncPrefabInstances("reload")` rufen; dasselbe nach `applyAssetBytes` für Prefab-Pfade. Außerhalb
   von Sessions reicht das. Dazu Undo-Verhalten klären (der Pass schreibt heute ohne Snapshot).
3. **„Save as Prefab" verknüpft die Quelle** (S): nach dem Schreiben `PrefabInstanceComponent` mit
   Bindungen auf die Quell-Entity stempeln (die Bindungen ergeben sich aus den UUIDs, die
   `buildSubtreeJson` schon schreibt).
4. **Export-Sync** (S–M): vor dem Packen jede Szene laden, `syncPrefabInstances` auf einer
   Wegwerf-Welt laufen lassen und das Ergebnis packen, oder beim Push alle Szenen mit Instanzen des
   Assets nachziehen.
5. **Skript-API** (M): `prefab.spawn(path, parent, x, y, z)` und `material.set(entity, id)` /
   `material.setSlot(entity, slot, id)` als Registry-Rows (Merkliste „Neue Registry-Row: drei (vier)
   Stellen").
6. **Collab-Propagation** (L): Sync über `EditorCommands` statt direkt, damit er repliziert, oder
   nur der Asset-Besitzer synchronisiert und die Änderungen gehen als Entity-Updates raus.
7. **Submesh-Randpunkte** (je S–M): Slot-Namen im MSEC-Chunk, Sichtbarkeit pro Section
   (`MaterialComponent`-Maske), Import-Option „als Einzel-Meshes + Prefab".
8. **Isolierter Prefab-Editor** (L): eigener Tab mit Wegwerf-Welt wie der Klassen-Tab.

---

## 4. Stand nach Schritt 2 (26.09.2026)

Thema 83, Schritt 2: jeder Restpunkt einzeln bewertet. Gebaut wurden `he_tests` und
`HorizonEditor` im Worktree (macOS, Debug), gelaufen sind die Testdateien `test_prefab.cpp`,
`test_scene_serializer.cpp`, `test_mcp_tools_prefab.cpp`, `test_outliner_filter.cpp`,
`test_editor_help.cpp` und `test_culling.cpp`, **nicht** die ganze Suite und **kein** Editor-Lauf.
Was nur kompiliert ist, steht dabei.

### Prefab

| # | Punkt | Urteil | Beleg |
|---|---|---|---|
| 2 | Verschachtelte Prefabs | **Defekt belegt und behoben.** Der Test war in der Reihenfolge „innen zuerst" rot: O schrieb seine eingefrorene Kopie über die neuen I-Werte (Wert, Name, Range). Jetzt wendet O auf Records einer lebenden inneren Platzierung nur an, was sein Autor dort geändert hat (Override-Liste im Nested-Block von O's Blob) plus was I's Sync nie schreibt (Transform der inneren Wurzel), und übergibt die Marker an die Tabelle der inneren Platzierung. Zweiter Defekt dabei gefunden und behoben: ein Record, den I gewinnt und O später per Push übernimmt, wurde von beiden Syncs angelegt (zwei Kopien, beide Reihenfolgen). | `a28a4bb7`, `b9c045b8`; 6 Tests `PrefabNested:*`, je beide Sync-Reihenfolgen; Negativkontrolle für „frisch angelegte innere Platzierung" |
| 4 | Änderung auf der Platte zieht nicht nach | **Behoben** für Hot-Reload: ein Sync pro Poll, wenn ein Prefab neu gelesen wurde; Undo-Eintrag „Prefab Update" nur, wenn sich etwas bewegt hat (`EditorUndo::pushSnapshot`), weil der Poll auch ein eben selbst gepushtes Prefab neu liest (`saveAsset` setzt die gemerkte mtime nicht). Während Play aufgeschoben bis zum ersten Poll danach. Peer-Update (`applyAssetBytes`) bleibt ohne Sync: es kommt nur in Sessions vor, und dort ist der Sync ganz aus (Punkt 1). | `e4dc5e87`; **nur kompiliert**, der Pfad Hot-Reload → Sync ist nicht zur Laufzeit gesehen (der Sync selbst ist getestet) |
| 3 | „Save as Prefab" verknüpft die Quelle nicht | **Behoben** im Outliner: `SceneSerializer::linkPrefabSource` stempelt Identitäts-Bindungen über die Records des geschriebenen Blobs, mit Undo-Snapshot; nicht während Play, nicht in Sessions. MCP `prefab_save` bleibt bewusst beim dokumentierten „die Szene wird nicht geändert" (Weltänderungen laufen dort über `EditorCommands`), die Beschreibung sagt jetzt, dass es darin vom Outliner abweicht. Nebenbefund: die Beschreibung von `prefab_instances` behauptete noch, Platzierungen folgten dem Asset nicht; korrigiert. Hilfetext `outliner.prefab` ergänzt. | `369750aa`; Tests `PrefabSaveAs:*` (Identität, Sync ohne Änderung, Push in beide Richtungen, Neuverknüpfung, verschachtelte behält ihren Link) |
| 5 | Export nimmt Szenen, wie sie auf der Platte liegen | **Behoben.** Der Export lädt jede Szene ohnehin in eine Wegwerf-Welt; dazwischen läuft jetzt `syncPrefabInstances` gegen den ContentManager. Die Szenendatei bleibt unverändert, das Export-Log sagt, wenn etwas nachgezogen wurde. | `b5df80d3`; **nur kompiliert**, kein Export-Lauf |
| 1 | Keine Propagation in Collab-Sessions | **Offen, eigener Schritt (L).** Der Sync müsste über `EditorCommands` laufen oder nur beim Asset-Besitzer, mit Replikation als Entity-Updates. Das ist ein Umbau der Replikationsseite, kein Rand. | — |
| 6 | Kein isolierter Prefab-Bearbeitungsmodus | **Offen, eigener Schritt (L).** Eigener Tab mit Wegwerf-Welt wie der Klassen-Tab; Push-to-Prefab über eine Instanz deckt den Arbeitsablauf heute ab. | — |
| 7 | Kein Prefab-Spawn aus Skripten | **Offen, eigener Schritt (M).** Neben den vier Registry-Stellen (Merkliste „Neue Registry-Row") braucht es im gepackten Spiel ein Prefab im Pak, das nur per Pfad aus einem Skript genannt wird, also eine Entscheidung zum Referenz-Abschluss beim Export. Zusammen mit Submesh-Punkt 1 als ein Schritt sinnvoll. | — |

### Submeshes

| # | Punkt | Urteil | Beleg |
|---|---|---|---|
| 5a | Slot-Material, das erst nach dem ersten Zeichnen erscheint | **Bestätigt und behoben.** Der Extractor vergaß seine Fehlliste nur bei einem anderen ContentManager, also praktisch erst beim Neustart. Jetzt `ContentManager::contentEpoch()`, bewegt von jedem erfolgreichen `saveAsset` und vom Content-Refresh des Editors (Import, Pull, kopierte Datei); der Extractor leert seine Liste, wenn es sich bewegt hat, und schaut pro fehlendem Pfad einmal neu. | `3a0cf50a`; Test „a slot material that appears after first sight …" in `test_culling.cpp` |
| 5b | Slot mit verwaistem Pfad lässt sich nicht leeren | **Bestätigt und behoben.** Das Asset-Feld bietet „(none)"/Clear nur für eine aufgelöste UUID. Der Mesh-Tab zeigt neben „(missing: …)" jetzt einen eigenen Clear-Knopf (`setSlot` mit Null-UUID leerte den Pfad schon). | `028b21c9`; **nur kompiliert**, UI nicht gesehen |
| 5c | D3D/Vulkan-Skinned-Schleifen nur per CI kompiliert | **Unverändert offen**, braucht Windows-Hardware. | — |
| 1 | Kein Material-Tausch aus Skripten | **Offen, eigener Schritt (M)**, zusammen mit Prefab-Punkt 7: `material.set(entity, path)` / `material.setSlot(entity, slot, path)` als Registry-Rows. Die Datenseite ist fertig (`MaterialComponent::slotOverrides`, der Extractor liest sie jeden Frame). | — |
| 2 | Section ausblenden | **Offen, Feature (S–M).** Maske auf `MaterialComponent`, Skip im Extractor, Checkbox in der Slot-Liste. Kein Defekt, ein transparentes Material ist der heutige Umweg. | — |
| 3 | Slots haben keine Namen | **Offen, Feature (M).** Formatänderung am MSEC-Chunk plus Importer (glTF-Materialname, Assimp) plus Anzeige. | — |
| 4 | Kein „als Einzel-Meshes importieren" | **Offen, Feature (M–L).** Import-Option, die pro Node/Primitive ein Asset und ein Prefab daraus schreibt. | — |

**Fazit:** Von den Randpunkten mit Fehlverhalten (verschachtelte Prefabs, Nachziehen von der
Platte, Save-as-Prefab, Export, Slot-Material spät, Slot nicht leerbar) ist keiner mehr offen.
Übrig sind Erweiterungen: Skript-API (M, ein Schritt für Prefab-Spawn und Material-Tausch),
Section-Sichtbarkeit, Slot-Namen, Einzel-Mesh-Import, dazu die zwei großen Umbauten
Collab-Propagation und isolierter Prefab-Editor. Vorschlag für den Chefchen: Skript-API als
nächster Schritt, die übrigen als eigene Themen.
