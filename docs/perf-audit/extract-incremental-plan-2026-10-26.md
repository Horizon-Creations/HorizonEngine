# Plan: den Extract nicht in jedem Frame neu bauen (Punkt 3, 26.10.2026)

Status: **Plan, nichts davon ist umgesetzt.** Er folgt auf die Messung in `foliage-spans-parallel-shadow-2026-10-26.md` §6
(50k Objekte, D3D11/D3D12/Vulkan: `RenderExtractor::extract` etwa 4 ms von 8 bis 10 ms CPU, davon `Extract::meshes` etwa 3,3 ms).

## 1. Ausgangslage

`RenderExtractor::extract` läuft in jedem Frame die ganze Welt ab (`out.clear()`, dann `extractMeshes` und die anderen Extraktoren) und
baut `RenderWorld::objects` neu. Ein zweiter Aufruf im selben Frame mit gleichen Eingaben wird schon übersprungen (`beginFrame`,
`m_frameCached`, `sameFrameKey`); über Frames hinweg gibt es nichts. In einer Szene, in der sich nur die Kamera bewegt, ist fast
alles, was der Walk liefert, bit-gleich zum Vorframe.

Vorarbeit dieser Runde, die der Plan voraussetzt: `Extract::meshes` baut `RenderObject` direkt und parallel (Teil 1), die selten
gelesenen Felder liegen am Ende des Structs (Teil 2). Danach kostet ein voller Walk bei 50k Objekten etwa 1 bis 1,5 ms; der Plan lohnt
sich erst, wenn das nicht mehr reicht (Szenen ab etwa 300k Entities) oder die Kosten auf schwachen Kernen dominieren.

## 2. Warum Signale allein nicht reichen

Im Projekt werden Komponenten fast überall direkt per Referenz geschrieben (`reg.get<T>(e).field = ...`, Details-Panel, Script-API,
Animation, LOD, Serializer). entt feuert `on_update` nur über `patch`/`replace`. `HorizonWorld` abonniert heute nur
`EntityIdComponent`. Ein Plan, der auf `on_construct/on_update/on_destroy` baut, übersieht jeden direkten Schreibzugriff: das
Bild bliebe stehen, und es fiele erst im Spiel auf.

Was es schon gibt: `TransformComponent::dirty` (vom `TransformSystem` gesetzt und gelesen, Scopes `Transforms::scan/propagate`), also eine
verlässliche Änderungsquelle für die Welt-Matrizen. Für Mesh, Material, LOD, Inactive und TerrainChunk gibt es keine.

## 3. Varianten

| | Änderungsquelle | Vollständig? | Aufwand | Gewinn (50k, statisch) |
|---|---|---|---|---|
| A | entt-Signale + Schreib-Audit (jeder Schreibweg ruft `patch` oder `markRenderDirty`) | nur so gut wie das Audit | hoch: viele Schreibstellen in Editor, Script-API, Systemen | extract etwa 0,3 ms |
| B | **Validierungs-Walk:** je Entity die Quelldaten (Transform 64 B + Mesh-Flags + Material-ID + LOD-Zustand) mit einem Snapshot vergleichen, parallel; nur geänderte Objekte neu bauen | ja, es wird verglichen, nicht vermutet | mittel | etwa 0,5 bis 0,8 ms (Speicherbandbreite, parallel, ohne Content-Manager-Zugriffe) |
| C | Hybrid: Transform-`dirty` und Struktur-Signatur (Anzahl, Pool-Größen, Asset-Epoche) als Schnellpfad, B nur alle N Frames oder im Verify-Modus | wie B, mit B als Netz | mittel bis hoch | wie A im Schnitt, B als Rückfall |

Empfehlung: **B zuerst** (Vollständigkeit vor Geschwindigkeit; der Gewinn gegen den parallelen vollen Walk ist klein, aber er
wächst mit der Entity-Zahl und spart die Allokationen). **C erst, wenn B gemessen ist**, und nur mit dem Verify-Modus unten.
A nur als Ergänzung für einzelne Quellen, die ein eigenes Signal haben (Asset-Reload).

## 4. Was pro Frame weiter läuft

Nie aus dem Cache: Kamera und Projektion, Tag/Nacht (`applyDayNight`), Sonne und Kaskaden-Fit (hängt an Kamera und Szenen-Box), lokale
Schatten-Layer, Lichter (`extractLights`, `orderLightWindow`), Partikel, Niederschlag, Seile und Spuren, Skinned-Meshes (Bones ändern
sich jeden Frame), Foliage-LOD (`FoliageLodMemory`, hängt an der Kamera), Editor-Icons.
Aus dem Cache nur `objects` der statischen Meshes: `meshAssetId`, `materialAssetId`, `transform`, `worldBounds`, Flags, `sections`,
`paramOverride`, Landscape-Felder.

## 5. Ungültig machen von der Asset-Seite

Ein Objekt hängt von Assets ab: Mesh-Bounds (`StaticMeshAsset::bounds*`), Abschnitte (`sections`, `resolveEntitySlots`), Material
(Parameter-Overrides werden gegen `MaterialAsset::graphParamNames` zu einem Block gemergt). Ändert sich eines davon (Reload, Material
im Editor bearbeitet, Mesh gestreamt), muss das Objekt neu gebaut werden. Quelle: eine **Asset-Epoche** im `ContentManager`
(hochgezählt bei jedem Einfügen, Reload, Löschen und Material-Edit; die Stellen sind dieselben, an denen er heute seine Caches
verwirft, siehe `processPendingInvalidations` in den Backends). Eine geänderte Epoche = voller Walk.

## 6. Verify-Modus (das einzige Sicherheitsnetz)

`HE_EXTRACT_VERIFY=1`: in jedem Frame läuft der volle Walk zusätzlich zum Cache-Pfad, beide `RenderWorld` werden Feld für Feld
verglichen (Reihenfolge, Matrizen, Bounds, Flags, `sections`, `paramOverride`), der erste Unterschied wird mit Entity-ID und Feld
geloggt, im Test-Modus bricht der Lauf ab. Alle UI- und Render-Tests laufen einmal mit gesetztem Schalter, ebenso ein Spielstart und
je 5 Minuten Editor-Betrieb (Gizmo ziehen, Material tauschen, Entity aus- und einschalten, Szene laden und speichern, Prefab setzen).
`HE_EXTRACT_INCREMENTAL=0` schaltet den Cache aus (A/B und Notbremse).

## 7. Schritte

1. Walk in zwei Teile trennen: *Quelldaten lesen und vergleichen* und *Objekt bauen* (nach Teil 1 dieser Runde ist das schon fast so).
2. Snapshot-Struct je Entity (Transform, Mesh-Flags, Material-ID, LOD-Zustand, Active, Chunk-Verweis), parallel vergleichen, Ergebnis:
   geänderte Indizes + "Struktur gleich?" (gleiche Entity-Liste in gleicher Reihenfolge).
3. Bei gleicher Struktur: geänderte Objekte an ihrer Stelle neu bauen; sonst voller Walk (wie heute).
4. Asset-Epoche im `ContentManager`; Walk-Schlüssel = (Welt-Version, Asset-Epoche, Struktur).
5. Verify-Modus und Tests (Abschnitt 8), danach Messung gegen den vollen parallelen Walk. **Nur weitermachen, wenn die
   Messung mehr als etwa 1 ms bei 50k bringt**; sonst bleibt der volle Walk.
6. Hybrid C nur nach Messung von B.

## 8. Tests

- Eine Welt mit allem gemischt (mehrabschnittige Meshes, fremdes LOD0, Terrain-Chunk, inaktiv, unsichtbar, fehlendes Mesh,
  Param-Overrides, Foliage), mehr Entities als die Parallel-Schwelle; je Änderungsart (Transform, Mesh, Material, Sichtbarkeit,
  Active, Entity neu/gelöscht, Asset-Reload) ein Frame ändern und prüfen: Cache-Pfad gleich vollem Walk, Feld für Feld.
- Reihenfolge von `objects`: `RenderSorter::sort` ist nicht stabil bei gleichen Abständen, eine andere Reihenfolge ändert die
  Zeichenreihenfolge; der Cache-Pfad muss sie bit-gleich halten (Pixel-A/B der 28 Zeugen je Backend).

## 9. Risiken

- Ein Schreibweg, den der Vergleich nicht sieht (Daten ausserhalb des Snapshots, die ins Objekt eingehen): deshalb der Snapshot aus
  genau den Feldern, die `extractMeshes` liest, und der Verify-Modus.
- Speicher: der Snapshot (ca. 100 B je Entity) kommt zu den 280 B je `RenderObject` dazu.
- Metal und GL benutzen denselben Extractor: nichts hier ist Windows-spezifisch, ein Mac-Build und `frame_pixel_ab.sh all` sind Pflicht.

## Handoff

- **Topic / step:** Thema 238 (Frame-Hebel), Punkt 3 von 3: Plan, kein Code.
- **Branch / commit:** `claude/foliage-spans-windows`, PR 199 (Plan wird mit dem Teil-1/2-Commit gepusht).
- **Done:** Varianten, Quellen, Verify-Modus, Schritte und Messschwelle beschrieben.
- **Verified:** Aussagen über den Ist-Zustand (kein Signal ausser `EntityIdComponent`, `TransformComponent::dirty` vorhanden, `ActiveFilter` und
  Content-Manager-Getter rein lesend) aus dem Code gelesen; Kosten aus den Captures dieser Runde.
- **Not verified:** alle Gewinnschätzungen in §3; ob andere Systeme Komponenten ausserhalb des Snapshots nach dem Extract ändern.
- **Decisions suggested, not taken:** Variante B zuerst, C nur nach Messung, Schalter `HE_EXTRACT_INCREMENTAL` / `HE_EXTRACT_VERIFY`.
- **Next steps:** 1. Teil 1 und 2 dieser Runde messen (voller paralleler Walk). 2. Nur wenn extract danach noch über 1 ms liegt: Schritt 1 und 2.
- **Traps:** `RenderSorter::sort` ist bei gleichen Abständen unstabil; `RenderObject` wird in allen Backends gelesen; Direktzugriffe
  auf Komponenten feuern kein entt-Signal.
