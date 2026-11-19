# Entity-Cooking Schritt 3: gekochte Datenblöcke (CookedStatic)

Stand 16.11.2026, Thema 343, Branch `claude/entity-cooking-statische-entities-auf-daten-reduzieren`.
Setzt auf `docs/entity-cooking-mobility-analysis-2026-11-14.md` (Schritt 2). Auto-LOD hat kein eigenes Handoff-Doc;
`grep -i auto-lod docs/` findet nur Plan-Erwähnungen (`asset-optimization-plan.md` §5, `foliage-instancing-lod-plan-2026-10-10.md`).
Extract-Frame-Reuse: `docs/render-extractor-shadow-pass-plan.md` §9.

## Was gebaut ist

- `src/HE_Scene/include/HorizonScene/CookedStatic.h` + `src/CookedStatic.cpp`: `HE::CookedStatic`, SoA-Zeilen je kochbarer
  Mesh-Entity (`entity`, `state`, `world`, `worldBounds`, `meshId`, `materialId`, `lod`, `castsShadow`, `receivesShadow`),
  `rowOfIndex` (Entity-Index → Zeile, ein Array-Zugriff, keine Registry). Liegt im `reg.ctx()` (`cookedStaticOf`), also
  teilen sich mehrere Extractors einer Welt einen Block.
- `refreshCookedStatic(reg, structureEpoch)`: baut die Zeilen aus `analyzeCookability(reg)` (kein zweiter Regelsatz), in der
  Reihenfolge von `view<Transform, Mesh>`, nur wenn sich die Epoche ändert. Zeilen sind erst `Pending`.
- `RenderExtractor.cpp` `extractMeshes`: Phase 2 kopiert eine `Ready`-Zeile statt die Entity neu zu bauen. Settle: der
  bestehende Walk-Pfad schreibt das Objekt wie immer, danach hält die Zeile, was er erzeugt hat (`Ready`), oder wird `Walk`
  (mehrere Sections, eigene `paramOverrides`, Engine-Wasser, TerrainChunk) bzw. bleibt `Pending` (Mesh nicht resident).
  Der Fast-Pfad kann so nur wiederholen, was der Walk schrieb. Reihenfolge von `out.objects` bleibt die des Walks.
- Schalter: `RenderExtractor::setCookStatic(bool)`, Start-Wert aus `HE_COOK_STATIC=1`. **Standard aus.**

## Entscheidungen

- **Entity-Handle:** die Registry-Entity bleibt mit allen Komponenten, `entt::entity` bleibt gültig und lesbar. Der Block ist ein
  READ-ONLY-Spiegel. Ein Wert, der danach in die Entity geschrieben wird (Transform, Material), wird von den Zeilen NICHT gesehen
  (rendert veraltet), bis Schritt 4 entkocht. Hook für Schritt 4: Zeile auf `Pending` setzen (oder `noteStructureChanged()`).
- **Eine Quelle der Wahrheit für "Block aktuell":** `HorizonWorld::structureEpoch()`; Inhaltswechsel
  (`ContentManager::contentEpoch`) setzt alle Zeilen auf `Pending` (`syncContent`). Kein neues `FrameKey`-Feld: der Block ist eine
  Funktion von Eingaben, die der Key schon hält (`structureEpoch`, `contentManager`, `materialEpoch`), und ein wiederverwendeter
  Walk liest keine Zeile.
- **Sichtbarkeit nie gecacht:** `MeshComponent::visible` und `ActiveFilter` werden weiter live in Phase 1 gelesen (viele Stellen
  schreiben `visible` roh). Test: `visible=false` auf einer gekochten Entity lässt das Objekt ohne Neubau verschwinden.
- **Standard aus:** ein Script/Editor/API-Schreibzugriff auf eine gekochte Entity würde heute veraltet rendern. Schritt 4 soll den
  Standard umlegen (Spielstart/Export), sobald die Entkoch-Hooks da sind.
- **Update-Systeme nicht angefasst (bewusst):** `Transforms::scan` ist das einzige generische System über alle Entities
  (Survey §3: die Typ-Systeme kosten Mesh-only-Entities schon 0). Gekochte Entities dort zu überspringen spart nur den `memcmp`
  gegen einen gleich teuren Zeilen-Lookup und würde bei einem Schreibzugriff die Transform-Hierarchie selbst statt nur den Spiegel
  veralten lassen. Gemessen: `Transforms::scan` 3,5 bis 4,2 ms/Frame an und aus, unverändert. Das Ziel "Update-Pfad messbar
  billiger" ist damit **nicht** erreicht (siehe Pending, braucht eine Entscheidung der Queen).
- Gruppen-Entities ohne Mesh bekommen keine Zeile.
- Reihenfolge bei gemischten Szenen: unverändert, die Zeile wird an der Stelle des Walks bedient (nicht hinten angehängt).

## Geprüft

- `he_tests`: `*test_cooked_static.cpp` 5 Fälle/1723 Assertions (Block aus dem Urteil, Extract == Walk Feld für Feld inkl.
  zweitem Frame aus den Zeilen, live `visible`, FrameScope-Reuse über eine Strukturänderung, später resident werdendes Mesh +
  Inhaltswechsel); `*test_cook_analysis.cpp` 6/105, `*test_culling.cpp` 77/19849, `*test_world_scale.cpp` 44/6926,
  `*test_layer_mesh_instancing.cpp` 6/229 grün.
- `HorizonGame` und `HorizonEditor` bauen und linken (Release, Metal-Mac).
- Pixel A/B (`scripts/perf/frame_pixel_ab.sh`, `HE_SKY_TIME=30`, AA aus, Metal): 16 Shots, aus gegen `HE_COOK_STATIC=1`,
  alle md5 gleich. Der Satz enthält Würfelreihen und Foliage; dass dort Zeilen `Ready` waren, ist über die Zeitmessung
  unten belegt, nicht über diese Shots allein. Eine Szene mit 50k statischen Meshes ist nicht als Bild verglichen.
- Messung (`world_streaming_ladder.sh`, Referenzwelt 50 568 Entities, Metal, M-Mac, Kamera `0,25,90,0,-0.25`, `--warmup 240`,
  300 Frames, `--no-counters`, abwechselnd aus/an, 3 Runden). `Extract::meshes` Selbstzeit ms/Frame:
  aus 13,58 / 13,58 / 12,03 (Median 13,58), an 7,48 / 8,19 / 6,99 (Median 7,48): **−45 %**. `Transforms::scan` unverändert
  (3,5 bis 4,2). Ganzer `cpuMs` p50 aus 67,3 / 61,5 / 54,8, an 53,9 / 60,2 / 53,1: nur Tendenz, **im Rauschen**.
  Bedingungen: Last 15 bis 40 (andere Bees kompilieren), Akku 100 %, Bildschirm gesperrt, Fenster nicht versteckt.
  Belastbar ist nur `Extract::meshes` alt/neu in dieser Sitzung. Rohdaten: `docs/perf-audit/raw-entity-cooking/mac_cook_*`.
- Nicht geprüft: Vulkan/D3D11/D3D12 (der geänderte Extractor-Code ist backend-unabhängig; weder gebaut noch gelaufen
  für diese Backends, nur Metal-Mac), Windows/Linux, Kosten des Neubaus (`analyzeCookability` über alle Entities je Epochenwechsel: nicht gemessen;
  im Editor mit `HE_COOK_STATIC=1` bei jedem Anlegen/Löschen), Play-Mode, 10k/100k.

## Handoff

- **Topic / step:** Thema 343, Schritt 3 von 4. Commit `3ee1c7f02` (Code + Tests) + dieses Doc + Rohdaten.
- **Done:** siehe oben.
- **Pending:** (1) Update-Pfad: kein Gewinn, siehe Entscheidung zu `Transforms::scan`; ein echter Gewinn braucht, dass gekochte
  Entities aus dem gescannten Storage verschwinden oder dass Schritt 4 einen Dirty-Hook je Zeile liefert. (2) Regel 11 (Zell-
  Streaming): eine Entity ohne `StreamingComponent` in gesplitteter Szene ist nicht erkennbar; Zelle entladen/laden bumpt die
  Epoche und baut den Block neu, daher korrekt, aber nicht billig. (3) `analyzeCookability(reg)` loggt "Static aber blockiert" bei
  jedem Neubau erneut. (4) Neubau-Kosten messen, bevor der Standard umgelegt wird. (5) Ein Spielstart-/Export-Aufruf
  (HorizonGame) existiert nicht: der Block entsteht lazy im ersten Extract nach dem Epochenwechsel.
- **Next steps (Schritt 4):** Entkoch-Hooks an die Liste aus Survey §8 (Zeile → `Pending`/`Walk` und aus dem Spiegel nehmen),
  Reparent/Zell-Entladen, danach den Standard von `m_cookStatic` auf an für Spiel/Export legen, Pixel-A/B auf einer Szene mit
  vielen statischen Meshes wiederholen.
- **Traps:** `kind==Unresolved` (Mesh nicht resident) bleibt `Pending`: in der Zeitmessung zählen nur aufgelöste Meshes. `Ready`
  kopiert die Weltmatrix vom ersten Walk nach dem Neubau. Eine Komponente, die zu einer lebenden gekochten Entity hinzukommt
  (Script), ändert die Epoche nicht.
