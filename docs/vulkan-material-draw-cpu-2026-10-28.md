# Vulkan: CPU-Kosten der Graph-Material-Draws (28.10.2026)

Anlass: Catania-Projekt (HEProject), Landschaft komplett im Bild = 60-70 fps, weggeschaut = 500 fps. Vermutet war GI
(`docs/gi-landscape-optimization-backlog-2026-10-27.md`); die Messung sagt etwas anderes.

## Befund
* Überblickskamera `--cam 0,150,1150,0,-0.2`, Vulkan, GI wie in der Config des Nutzers, RTX 4070 (NN-WS03): **65 fps, CPU 15,1 ms, GPU 4,3 ms**.
  GI aus: 67 fps. GI kostet auf dem Hardwarepfad ~1,3 ms GPU / 0,8 ms CPU. Die 5,3 ms GIShadow aus dem HUD sind auf keinem Backend reproduzierbar.
* Der Scope `Scene` war 13,4 ms CPU bei 230 Draws (58 us/Draw). Aufgeschlüsselt: `resolveShaders` 27 us/Draw (kopiert pro Draw den ganzen
  GLSL-Text des Landschaftsmaterials und hasht ihn), Descriptor-Update 16 us/Draw (~25 Writes), Alloc 2 us.
* `VulkanRenderer.cpp` (Instanz-Setup) hatte `const bool gpuDebug = true;` ("TEMPORARILY forced ON"): die Khronos-Validation lief in **jedem** Build.

## Geändert (nur Vulkan-Backend; D3D11/D3D12/GL/Metal unverändert)
1. `drawDCVk`: Memo `matFwdMemo`/`matGbMemo` (pro Frame, pro Material) für `resolveShaders`/`resolveGBufferShaders` + `foldDisplacement`.
2. Material-Set 0: Bindings 1, 3, 9 sind `UNIFORM_BUFFER_DYNAMIC` (`Binding::dynamic` in `VulkanMaterialLayout.h`); der Ring-Slot ist der
   Dynamic Offset. Ein Set pro Hash der Descriptor-Writes (`m_matSetCache[frame]`, mit dem Pool geleert) statt eines Sets pro Draw.
   Pool: eigener Poolsize-Eintrag für DYNAMIC. Der SPIR-V-Test vergleicht weiter `row.kind`, deshalb ist `dynamic` ein eigenes Feld.
3. Validation nur in `_DEBUG` oder mit `HE_GPU_DEBUG` (vorher immer). **Das nimmt dem Nutzer die Validation-Logs ohne Setup.**

## Ergebnis (gleiche Pose, 600 Frames)
| | fps | CPU p50 | GPU p50 |
|---|---|---|---|
| vorher | 65 | 15,1 ms | 4,3 ms |
| 1+2 (Validation noch an) | 244 | 3,9 ms | 2,6 ms |
| 1+2+3 | 280 | 3,2 ms | 3,1 ms |
Wegschauen 305 fps. Jetzt GPU-begrenzt.

## Verifiziert
* Bild: Vulkan, GI/SSAO aus, Frame 350, Pose wie oben: Landschaft (y>=440) gegen alten Build hw237 **Maximalabweichung 0**; Himmel weicht ab (Wolken
  sind zeitabhängig). Zwei neue Läufe untereinander: ebenfalls 0 auf der Landschaft.
* `he_tests --source-file=*test_material_graph.cpp`: 120/120. Mit `HE_GPU_DEBUG=1` keine Descriptor-/Draw-Fehler zur Laufzeit, nur das bekannte Abbau-Rauschen bei Frame 702.
* Nicht verifiziert: D3D11/D3D12/GL/Metal (nicht angefasst), Szenen mit Cluster-Lichtern (`m_clusterReady`) und WPO-Materialien (Binding 9) im Bild,
  Windows ohne Vulkan SDK.

## Spitzen beim freien Flug (Folgeschritt, gleicher Tag)
Messung `HE_PERF_CAM_FLY=1.5`: 106 von 600 Frames > 2x Median, ~10 ms CPU. Ursachen: `processPendingInvalidations` rief `deviceWaitIdle()`,
sobald ein erneuertes Mesh ein HW-BLAS hatte (3,2 ms im Mittel), und `buildGiHwBlas` baute jedes BLAS auf einem Einmal-Command-Buffer mit
`vkQueueWaitIdle` (2,4 ms).
* Ersetzte HW-BLAS gehen in `m_retiredHwBlas` (k_maxFramesInFlight+2 Frames, wie `m_retiredMeshes`) statt Device-Idle; Idle bleibt nur bei Material/Textur/Skelett.
* `buildGiHwBlas` legt AS + Adresse an und stellt den Build in `m_giHwBlasPending`; `flushGiHwBlasBuilds(cmd)` (direkt nach `updateGiAccel()`) zeichnet
  alle Builds in den Frame-Command-Buffer auf, mit Barrier AS-Write -> AS-Read vor dem TLAS-Build. Input/Scratch leben in `m_giHwBlasTemp` bis die Frames in Flight durch sind.
* Ergebnis (Flug, 600 Frames): CPU p95 10,7 -> 7,4 ms, p99 19,4 -> 11,9 ms, Frames > 2x Median 106 -> 79 (mit Validation 17).
* Bild GI an, Pose A: neue Läufe untereinander pixelgleich, gegen hw237 kein Pixel > 32 Abweichung. Pose B (nur Wasser) ist wegen animierter Wellen nicht vergleichbar.
* Mit `HE_GPU_DEBUG=1` keine neuen Validation-Fehler (nur das Abbau-Rauschen). Nicht mit Synchronization-Validation geprüft.

## Restliche Spitzen (Folgeschritt 2, gleicher Tag)
Flug 3000 Frames, `HE_PERF_CAM_FLY=1.5`: CPU p99 8,9 -> 7,6 ms, p99.9 17,8 -> 11,8 ms, Maximum 39,4 -> 14,3 ms, Frames > 3x Median 14 -> 5.
* **Kein Device-Idle beim Wachstum** (`uploadGiBuffer(..., fresh=true)`): Rewrite (Kompaktierung) und Wachstum von `m_giNodeBuf`/`m_giTriBuf` legen einen neuen
  Buffer an, der alte wartet in `m_retiredGiBufs` (k_maxFramesInFlight+2 Frames). Vorher `deviceWaitIdle()` + Neuanlage: 34-57 ms in einem Frame.
  Sicher, weil beide Leser (`runGi`, `runGiReflections`) ihre Descriptor-Sets pro Frame neu schreiben.
* **BVH-Refit statt Neubau** (`GiBvhRefitCache`, `refitGiBvh` in `GiBvh.h/.cpp`, in D3D11, D3D12, OpenGL, Vulkan; Metal baut kein CPU-BVH): eine
  Mesh-ID, die zum zweiten Mal gebaut wird, merkt sich Baum + Quell-Dreiecksnummern; gleicher Index-Hash -> Dreiecke neu lesen, Bounds von unten nach oben.
  Nach 24 Refits Neubau, höchstens 192 Einträge. Nahtvarianten und Sculpting haben dieselbe Topologie. `HE_GI_BVH_REFIT=0` schaltet es ab.
  `GiBuildBlas` Vulkan 0,6 -> 0,05 ms je Aufruf; OpenGL (kein 500-m-Schnitt) 70,2 -> 5,9 ms je 400 Frames bei gleicher Zahl Builds.
  Die Nahtvernähung selbst bleibt (Thema 190: ohne sie schwarze Linien in GI), nur der BVH-Bau dafür ist billiger.
* **Buffer-Pool für HW-BLAS** (`m_giHwBufPool`, Größenklassen ab 64 KiB, max. 256 MB): freigegebene BLAS-/Scratch-/Input-Buffer werden wiederverwendet statt
  `vkAllocateMemory` (0,3-12 ms je BLAS). `GiBuildHwBlas` 0,51 -> 0,01 ms im Mittel, max 12,3 -> 0,4 ms.
* Verifiziert: `test_gi_bvh` 9/9 (drei neue Fälle: Refit gleich Brute-Force auf verschobenen Vertices, Cache-Zählung, Erneuerung nach 24 Refits); relevante he_tests-Gruppen
  807/808 (der eine Fehlschlag ist ein Pfadtrenner-Vergleich durch den Scratch-APPDATA mit `/`, `test_contentmanager.cpp:1928`, unabhängig). Bild GI an, Pose A, Vulkan/OpenGL
  gleich wie vorher (OpenGL gegen hw237 exakt 0). Mit `HE_GPU_DEBUG=1` kein neuer Validation-Fehler (gleiches Abbau-Rauschen).
* Nicht verifiziert: Ein Flug-Bildvergleich Refit an/aus ist nicht aussagekräftig, weil zwei Läufe mit gleichen Einstellungen schon bis ~6,7 mittlere Abweichung
  haben (Streaming/LOD-Zeitpunkte); ein synchrones Paar (OpenGL) lag bei 0,05. D3D11/D3D12-Pfade sind nur per Rauchtest + Unit-Test belegt.
* Die Frames um p95 (~7 ms) sind keine CPU-Arbeit mehr: `WaitFrame` ist dort ~1 ms länger und der Rest steckt in `Render` ohne Kind-Scope, die CPU wartet auf die GPU
  (GPU p50 3,3, p95 ~4 ms).

## GPU-Seite (Folgeschritt 3, gleicher Tag)
Neue GPU-Zeitmarken in Vulkan (`Sky`, `Opaque`, `GBuffer`/`Resolve`, `Transparent`, `GIShadow`/`GIFilter`/`GIProbes`/`GIRefl`, gleiche Namen wie D3D11). Befund (Forward-Pfad, p50):
GIRefl 0,96 ms war der größte Einzelposten, danach Opaque 0,86, Sky 0,61 (Überblick) bzw. Transparent 0,47 (Wasser); die HW-GI selbst (Schatten, Filter, Proben) < 0,15 ms.
* **`gi_refl_hw.comp`** (Ray Query): `gi_refl.comp` mit Closest-Hit und Schatten-Any-Hit über die TLAS statt über die 257 Instanzen im SW-BVH; TLAS-Binding 11,
  eigenes Set/Pool/Pipeline (`createGiReflHwPipeline`). Läuft, wenn die diffuse GI an ist und die TLAS dieses Frame steht, sonst der SW-Kernel; `HE_GI_REFL_HW=0` erzwingt SW.
* **HW-BLAS aus den SW-Dreiecken** (`buildGiHwBlas(id, range)`, nicht indiziert, Reihenfolge des SW-BVH): der Primitive-Index eines Treffers ist die Dreiecksnummer im Slice,
  `giTris[instanz.triOffset + prim]` ist dasselbe Dreieck wie im SW-Kernel. Schatten-/Proben-Kernel lesen das nicht.
* Hand-Kopie-Konvention: `gi_refl_hw.comp` ist in den Guards von `tests/test_culling.cpp` (Cone-Hash, DDGI-Sampling) neben `gi_refl.comp` aufgenommen; 105/105 in culling/gi_*.
* Verifiziert: Bild HW gegen SW (`HE_GI_REFL_HW=0`, gleiches Binary, GI + Reflexionen an): Überblick identisch (Abweichung 0,000), Wasserpose mittlere Abweichung 1,78 gegen 0,93
  Wellenrauschen (HW-Lauf gegen HW-Lauf), 0,017 % der Pixel > 32; Reflexionen an gegen aus ändern dasselbe Bild um 10,4. Visuell kein Unterschied. Mit `HE_GPU_DEBUG=1`
  1500 Frames Flug ohne Validation-Fehler. Nicht verifiziert: D3D12 (DXR-Kernel hat noch kein Reflexions-Gegenstück), Reflexionen bei GI aus (SW-Pfad, unverändert).

## Wasser-Reflexionen: Reichweite (Folgeschritt 4)
Befund (Catania, See bei x~130..230, z~490..710, Hügel daneben): Mit GI-Reflexionen an spiegelte der See den Hügel nur bis 200 m (`GIReflectionsSettings::maxDistance`, nicht einstellbar)
und schnitt ihn mitten ab; was hinter dem Limit liegt, fällt auf die Himmels-Cubemap zurück, und die enthält kein Gelände (Horizont/ferne Ufer fehlten im Wasser).
Der Himmel selbst wird nicht getroffen: ein Strahl, der nichts trifft, liefert im Kernel nichts (`continue`, Konfidenz 0), und `heLitP` behält dann seinen Cubemap-Term
(`MaterialShaderLibrary.cpp` Reflexions-Kaskade). Das bleibt so.
* `runGiReflections`: der HW-Kernel traced mindestens `kGiHwReflMinDistance` = 4000 m (Ray Query: GIRefl 0,02 ms bei 200, 1500 und 4000 m); die SW-Kernel behalten die Einstellung.
* Bild See-Pose: Hügel wird ganz gespiegelt (Abweichung zum 1500-m-Versuch 0,9, zum alten Stand 2,5). Nicht verifiziert: SW-Backends (D3D11/D3D12/GL) haben weiter 200 m, die
  Kosten einer größeren Reichweite dort sind nicht gemessen; ob die Cubemap die Wolken enthält, ist nicht geprüft (die Himmelsspiegelung im Wasser ist schwach, das ist die Gewichtung im Wasser-Material).

## Zeiten, vorher und nachher (Vulkan, RTX 4070, HEProject/Catania, GI + Reflexionen an, 2000x1125 hidden)
| Situation | Start (vor allem) | nach Material-/Validation-Fix | jetzt |
|---|---|---|---|
| Überblick, fps | 65 | 280 | **356** |
| Überblick, CPU / GPU p50 | 15,1 / 4,26 ms | 3,2 / 3,06 | **2,31 / 2,31** |
| Wegschauen / Wasser, fps | 291 | 305 | **386** |
| Wegschauen, CPU / GPU p50 | 3,19 / 3,25 ms | 3,03 / 3,02 | **2,28 / 2,26** |
| Flug, fps | - | 223 | **374** (3000 Frames) |
| Flug, CPU p99 / max | - | 19,4 / 56,6 ms | **5,6 / 18,2** |
GPU-Zeilen Überblick, p50: GIRefl 0,96 -> **0,07**, Opaque 0,86 -> 1,04 (nicht untersucht; kann Messschwankung oder GPU-Takt sein, die Gesamtsumme sinkt trotzdem um 0,75 ms), Sky 0,61 -> 0,61, Shadow 0,11. Wasser: GIRefl 0,94 -> 0,02, Opaque 0,72, Transparent 0,47, Sky 0,24.

## Offen, in Reihenfolge
1. GPU-Rest (2,3 ms): Opaque 0,7-1,0 ms (Landschaftsmaterial, 577k Dreiecke, kein Z-Prepass), Sky 0,6 ms (Wolken-Raymarch, bei viel Himmel), Transparent 0,47 ms (Wasser).
   Kandidaten: Wolken in halber Auflösung/zeitlich verteilt, Z-Prepass nur für die Landschaft, Wasser-Shader messen. Alles ändert das Bild oder braucht eine Qualitätsentscheidung.
2. `gi_refl_hw` auch für D3D12 (DXR, `gi_refl_hw.hlsl`) und die SW-Kernel für OpenGL/D3D11 mit einer Top-Level-BVH statt der linearen Instanzliste (GIShadow 1,08 ms auf D3D11).
3. Restliche CPU-Ausreißer: Wachstum von `GiBlasStore` (Vektor-Realloc) und der Voll-Upload bei Kompaktierung.
4. Gleiches Memo/Set-Sharing für D3D11/D3D12/GL prüfen: `resolveShaders` wird von allen Backends pro Draw aufgerufen.
