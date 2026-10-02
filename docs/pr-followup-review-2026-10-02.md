# Folgepunkte aus den offenen PRs #71–#75 (Review, 2026-10-02)

Hive-Thema 121, Ergebnis von Schritt 1 (`review-offene-prs--1`, nur lesend), festgehalten in Schritt 2.
Stand der Prüfung: main `0513f8d4`.

**Gelesen:** `gh pr view` #71–#75, die Doku-Diffs der PR-Zweige (Nebula-Analyse, CopilotDocs-Checkliste,
`widget-pre-construct-design`, `d3d12-imgui-flicker-befund`, `clustered-lighting-forward-plan`) und die
begleitenden Dokus außerhalb der PRs: GI-DDGI-Analyse (Zweig Thema 120) und Deferred-Analyse (Zweig
Thema 116). Die Code-Diffs wurden nach Offen-Markern gegrept. Jeder Punkt ist gegen main und die
Hive-Suche abgeglichen. Bekannte HW-Abnahmen auf NN-WS03 sind weggelassen.

**Quelle:** [PR] = steht schon im PR-Text · [Doku] = steht nur in Doku/Code.

## A. Themenkandidaten (je ein eigenes Thema, keiner hat bisher eines)

| # | Quelle | Punkt | Einschätzung |
|---|--------|-------|--------------|
| A1 | PR #71 + Doku 116/120 | Vulkan wird nirgends gezeichnet. Die CI hat keinen Software-ICD, es gibt nur `spirv-val` und Reflexion. lavapipe (Linux-CI) oder SwiftShader würde Bildtests für Nebula, Clustered, GI und Deferred auf dem SPIR-V-Pfad ermöglichen. Thema 60 deckt nur den Build ab. | JA, hoch (Hebel für vier Themen) |
| A2 | PR #75 | Die eingebauten Forward-Shader Metal `fragmentMain` und GL `kUnlitFS` lesen weiter nur das 8er-Fenster. Auf Metal sehen Built-in-Materialien im Forward-Pfad also 8 Lichter, Graph-Materialien jetzt 256. | JA |
| A3 | PR #75 | Der Exporter backt weiter `fragment()` (`ExportDialogPanel.cpp:353`, `EditorApplication.cpp:5028`). Ausgelieferte Spiele mit Pak-Blobs bleiben beim 8er-Fenster, funktional korrekt, aber ohne Gewinn. `MaterialShaderVariant` muss kennzeichnen, welche Variante gebacken ist, damit der Exporter `fragmentClustered` backen kann. | JA, mittel |
| A4 | Doku 116 §2.1/§8 | Der Spielpfad (Swapchain-Zweig) von D3D11, D3D12 und Vulkan hat kein HDR, Tonemap, Bloom, AA, TAA oder SSR, nur der Editor-Viewport hat es. In 9/109 nur als „Grenze C6" geführt. Offene Frage an den Menschen: Sollen D3D/Vulkan für ausgelieferte Spiele gleichwertig sein? | Mensch, dann JA (≈ 200–400 Z./Backend) oder Hinweis in der Export-Doku |
| A5 | PR #74 | Vulkan hat dasselbe H2-Muster: `ImGui_ImplVulkan_RemoveTexture` gibt das Viewport-Descriptor-Set sofort frei (`EditorApplication.cpp:4161`), passend zu `vkFreeDescriptorSets … in use`. `DeferredSlotFreeList.h` lässt sich wiederverwenden. | JA, klein |
| A6 | Doku #74 (Flicker-Befund Schritt 4) | `HE_DUMP_RHI` ohne Dump-Modus stürzt im ersten UI-Frame ab: `m_backend` kommt am Ende von `OnInit` wieder aus der Config (`EditorApplication.cpp:1179` gegen `:311–326`). Schon zweimal passiert (Thema 74 und 97), auf main ungefixt. | JA, klein |
| A7 | PR #75 | Unter `MTL_DEBUG_LAYER=1` scheitert jeder Material-Draw an fehlenden Samplern 5–12/14, schon vor #75. Das versperrt Metal-Validierungsläufe. | JA, klein |
| A8 | PR #75 | Im Zeugen `MANYLIGHTS` ist der Boden im Deferred-Pfad deutlich heller als im Forward-Pfad (Ambient/IBL), nicht untersucht. Möglicher Paritätsfehler auf Metal. | JA, Analyse |
| A9 | Doku 120 §2.3/1 | Forward-SSR für Graph-Materialien ist auf Vulkan tot: Der `ssr.x`-Zweig liegt im `fog.z`-Block (`MaterialShaderLibrary.cpp:565–584`), und Vulkan setzt `fog[2]` nie. Die Doku verweist aufs SSR-Thema, dort steht es nicht. | JA (oder an 109 anhängen) |
| A10 | Doku 120 §2.3/2+3 | Parität bei GI aus: D3D11, D3D12 und Vulkan setzen für Graph-Materialien weder `heSkyEnv` (`fog[2]`) noch heAO (`fog[3]`). Graph-Materialien bekommen flaches Ambient, Built-ins analytisches Himmels-Ambient. | JA, mittel |
| A11 | Doku #72 §2.4/§2.3 | PreConstruct zur Entwurfszeit im Designer (der Unreal-Hauptzweck), mit Sandbox-Frage, dazu ein Knoten „Is Design Time". | Mensch, dann JA |
| A12 | Doku #72 §2.4 | Expose on Spawn: Create Widget mit Pins für öffentliche Variablen, gesetzt zwischen PreConstruct und Construct. | Mensch, dann JA |
| A13 | PR #71 | Kein Drift-Wächter Metal ↔ GL für den Himmel (`kSkyMSL` wird von Hand gepflegt). Das Rezept für einen normalisierten doctest-Diff steht in der Nebula-Analyse. | JA, klein (1 Schritt) |

### Empfohlene Reihenfolge neuer Themen

1. A1 Vulkan-Laufzeitzeuge
2. A2 Built-in-Forward geclustert (Metal zuerst)
3. A6 + A5 + C9, kleine Editor-Fixes (`HE_DUMP_RHI`, Vulkan-/D3D12-ImGui-Slots)
4. A3 Exporter backt die Cluster-Variante
5. A9/A10 Graph-Material-Parität auf D3D/Vulkan
6. A7, A8, A13 je ein kleiner Schritt
7. B als ein Sammelthema „Doku-Altlasten"

A4, A11 und A12 erst nach Entscheidung des Menschen.

## Anhang: Kleinere Punkte aus Schritt 1

### B. Doku- und Kommentar-Altlasten (ein Sammelthema)

- **B1 [PR #71]** Der neue Hinweiskasten in `CopilotDocs/windows-gpu-verification-checklist.md` markiert A4-Aussagen nur als „vermutlich überholt": D3D12-Root-Sig seit `d0df511a`, `heLandscapeWeights` auf s0 seit `554a43d4`, `sharesSamplerRegister` existiert nicht mehr, „CI prüft nur Compile". Einzeln geprüft und korrigiert ist keine davon.
- **B2 [Doku #71, Lücken Pkt. 4]** Der Hive-Thementext von 115 ist schon korrigiert. Der Website-Roadmap-Eintrag zur Nebula-Parität ist ungeprüft. Deploy nur nach Rückfrage.
- **B3 [Doku 120]** Auf main bestätigt: `IRenderer.h:233–237` („DDGI Metal-only, false on every other backend") und `VulkanRenderer.cpp:8108–8112` („keeps supportsGlobalIllumination = false") sind veraltet. Sie sind die Quelle der falschen Prämisse von Thema 120.
- **B4 [Doku #75 §5]** Der Header-Kommentar zum Deferred-Resolve nennt die GI-Lokalmaske „v1 limitation", der Code wendet sie längst an.
- **B5 [Doku 116 §4.1]** `docs/backend-parity-plan.md` §1.4/P4b behauptet, der Resolve werde für jedes Backend übersetzt. Für HLSL stimmt das nicht: ungepinnt, FXC X4509.

### C. Kleinkram an den PRs selbst (kein eigenes Thema)

- **C1 [Code #72]** Die neue Warnung in `fromJson` („Custom event 'PreConstruct' is now an engine event") kommt für jeden Graphen, also auch für HC-Klassen und Entities, vermutlich auch bei jedem `createWidget` erneut (Graph pro Instanz geparst). Der Text spricht nur von Widgets. Ungeprüft: was ein alter Custom-Event „PreConstruct" auf einer HC-Klasse jetzt tut.
- **C2 [PR #72]** Die Log-Zeile in `createWidget` liest `graph.nodes.empty()` nach `std::move(graph)`. Fix in einer Zeile.
- **C3 [Doku #72 §3]** Checklistenzeile „Tooltips/HcNodeDocs für Lifecycle-Events" ist nicht erledigt, kein Diff dort.
- **C4 [Doku #72 §3, Test 4]** Ein Laufzeittest, dass ein kompiliertes `onPreConstruct`-Override aufgerufen wird, fehlt. Geprüft sind nur Deskriptor und Codegen-Text.
- **C5 [PR #72]** Website-Zweig `claude/widget-pre-construct-docs` (`dfeff22`) mergen und deployen, nach Rückfrage.
- **C6 [Doku #72 §2.2]** Thumbnails (`AssetThumbnailCache.cpp:442`) und `__uiStyleWitness` führen Nutzer-Graphen mit Nebenwirkungen aus, jetzt PreConstruct und Construct. War vorher schon so. Kandidat für eine kleine Analyse.
- **C7 [PR #73]** Sichtprüfung im echten Editor mit Construct-Graph steht aus, auf dem Mac machbar.
- **C8 [PR #73, Ursache]** Der Editor tickt Widgets vor den erzeugenden Skripten. Ob andere skriptgesteuerte Widget-Änderungen deshalb einen Frame zu spät kommen, ist nicht untersucht. `animate()` ist nicht betroffen.
- **C9 [PR #74]** `D3D12Renderer::DestroyImGuiTexture` (`:11254`) ist ein No-op, Icon-Slots kommen nie zurück. Ist der Heap voll und kein Slot geparkt, endet `Alloc` in `IM_ASSERT` und gibt Null-Handles zurück. Ob die Icon-Anzahl das je erreicht, ist ungeprüft. Passt zu A5.
- **C10 [Doku #74]** D3D12 hat kein `SetMaximumFrameLatency`/Waitable-Swapchain, DXGI läuft bis zu 3 Frames voraus. Eher etwas für Thema 104 (Latenz).
- **C11 [Code #71]** In `test_sky_shader.cpp:907–921` wird der Vergleich D3D12 ↔ D3D11 nur per MESSAGE übersprungen, wenn der D3D11-Fall fehlt. Unkritisch.
- **C12 [Doku #75 §5]** Der Resolve sampelt im Cluster-Loop mit `texture()`. Für einen späteren HLSL-Resolve muss das `textureLod` werden. Als Notiz ins geparkte Thema 116.

### D. Merge-Konflikte (per `git merge-tree --write-tree` geprüft)

- Alle fünf PR-Zweige mergen konfliktfrei auf `origin/main`; ebenso #72 × #73 (trotz Warnung in PR #72), #74 × #75, #71 × #75, #72 × #74.
- **#75 × Zweig von Thema 120 (`9d157e54`) kollidiert** in `D3D12MaterialRootSignature.h` und `LightPacking.cpp`. Die GI-Doku plant Vulkan Schritt 4 gegen das alte DSL in `VulkanRenderer.cpp:2318–2353`; mit #75 steht das in der Tabelle `VulkanMaterialLayout.h`. **Empfehlung:** #75 zuerst mergen, Thema 120 darauf rebasen, Schritt 4 gegen `VulkanMaterialLayout.h` planen.
- Thema 120 braucht für die Abnahme noch den Zeugen `HE_DUMP_GIBLEED` mit einer Graph-Kugel neben einer Built-in-Kugel (GI-Doku §4), am besten als Schritt in Thema 120.
