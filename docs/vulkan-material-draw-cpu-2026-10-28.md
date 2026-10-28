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

## Offen, in Reihenfolge
1. Restliche Spitzen (p99 ~12 ms, einzelne Frames bis 60 ms): `GiUploadBlas` mit `deviceWaitIdle()` bei Kompaktierung/Buffer-Wachstum
   (57 ms in einem Frame), und Bursts in `GiBuildBlas` (CPU-BVH je Chunk-Naht, ~0,6 ms je Mesh) wenn viele Nähte zugleich wechseln. Kompaktierung in neuen
   Buffer statt Idle; BVH-Bau in Jobs oder pro Frame budgetieren. Der Stitch-Cache (Backlog 8) vermeidet die Neubauten ganz.
2. GPU-Spitzen (p95 3-4,4 ms gegen p50 3 ms) sind nicht erklärt.
3. GI-Backlog Punkt 1-4 nur nach neuer Messung: Hardwarepfad kostet 1,3 ms insgesamt, Höhenfeld dort wäre < 0,2 ms Gewinn.
4. Gleiches Memo/Set-Sharing für D3D11/D3D12/GL prüfen: `resolveShaders` wird von allen Backends pro Draw aufgerufen.
