# Backend-Parität — D3D11 / D3D12 / Vulkan an Metal heranführen

Stand: 2026-08-11. Grundlage: belegter Feature-Abgleich aller fünf Backends (60 Features,
240 Einzelprüfungen mit `Datei:Zeile`, plus adversarische Gegenprobe).

Metal ist heute mit Abstand das vollständigste Backend (13 211 Zeilen gegenüber 8 286 auf
Vulkan, 7 202 auf D3D12, 4 247 auf D3D11). Dieses Dokument sagt, **was genau** fehlt, **was
davon Portierung statt Neuentwurf ist** und **in welcher Reihenfolge** es sich lohnt.

---

## 0. Methode — warum diesen Zahlen zu trauen ist

Header-Signaturen und Code-Kommentare beweisen nichts: ein `SetSSAOSettings`-Override kann
eine Struct wegspeichern, die niemand liest, und der D3D12-Header behauptet Dinge über
andere Backends, die nicht stimmen. Der Abgleich hat deshalb eine harte Beweisregel benutzt:

> Ein Feature gilt nur als **vorhanden**, wenn für alle drei Punkte eine `Datei:Zeile`
> vorliegt: (a) Shader-Quelltext existiert, (b) Pipeline/PSO wird erzeugt, (c) der Pass wird
> im Frame-Pfad tatsächlich encodiert. Zwei von drei = **teilweise**. Alles, was nur aus
> einer Override-Signatur, einem Kommentar oder der Dateigröße abgeleitet ist = **fehlend**.

Eine zweite, adversarische Runde hat jede Behauptung zu widerlegen versucht — in beide
Richtungen: falsche Lücken (Backend löst es anders) und falsche „vorhanden" (toter
Codepfad). Das hat 13 Statuskorrekturen ergeben, darunter der schwerwiegendste Befund
dieses Dokuments (§1.1).

**Die vier Befunde in §1 habe ich anschließend selbst nachvollzogen** — mit `fxc` bzw. einem
eigens gegen `he_shadercompiler` gebauten Testprogramm. Sie sind nicht abgeleitet, sondern
reproduziert. Alles andere stammt aus dem Abgleich und trägt seine Belege im Text.

---

## 1. Vier Befunde, die den Plan bestimmen

### 1.1 Der gesamte GI-Stack auf D3D11 **und** D3D12 ist zur Laufzeit tot

Das ist kein fehlendes Feature. Der Code ist vollständig da — er läuft nur nie.

`src/Backends/D3D_Shared/HlslSources.h:630` öffnet einen `[unroll]`-Loop, der `break`
(`:632`) und `continue` (`:635`, `:636`, `:638`) enthält und zweimal dynamisch in eine
Vektor-Komponente schreibt (`localVis[i] = 0.0`, `:638` und `:640`). Diese Kombination ist
unter `cs_5_0` nicht übersetzbar. Der Kommentar direkt darüber (`:628-629`) behauptet
ausdrücklich das Gegenteil („so the dynamic vector-component write `localVis[i]` stays
FXC/SM5.0-safe") — er ist falsch.

Selbst reproduziert, Kernel exakt so zusammengesetzt wie zur Laufzeit
(`kGiTraversalHLSL` + `kGiShadowCSHLSL`):

```
error X3500: array reference cannot be used as an l-value; not natively addressable  (localVis[i] = 0.0)
error X3511: forced to unroll loop, but unrolling failed                             ([unroll] for (int i = 0; i < 4; ++i))
compilation failed; no code produced
```

`GiProbeCS` aus derselben Datei kompiliert dagegen sauber — der Fehler sitzt isoliert im
Local-Light-Loop des Shadow-Kernels.

**Die Folgen sind viel größer als der Fehler.** Beide Backends werten den Compile-Erfolg als
Gesamt-Gate:

- D3D11: `D3D11Renderer.cpp:1677` schlägt fehl → `:1706 giSupported = false`
- D3D12: `D3D12Renderer.cpp:4069` schlägt fehl → `:4204-4213 giSupported = false; return;`

Damit sind auf beiden Backends tot: DDGI-Probe-Update, DDGI-Feldsampling im Shading,
ray-getracte Direktionalschatten, die lokale RT-Schattenmaske, der Software-RT-Fallback über
`HE::GiBvh` und das Multi-Bounce-Feedback.

**Und das Bitterste:** Auf D3D12 steht `createGiHwPipelines()` (`D3D12Renderer.cpp:4220`)
**hinter** diesem `return`. Der komplette DXR-1.1-Pfad — separat mit `dxc -T cs_6_5`
offline gebaut, per CMake deployt, und der sauber kompiliert — ist deshalb **unerreichbar**.
Es gibt keine Hardware, auf der er je gelaufen wäre.

Der naheliegende Fix ist klein und trägt: den Loop durch vier Aufrufe einer Hilfsfunktion
mit literalem Index ersetzen, die einen Skalar zurückgibt (kein dynamischer Lvalue, kein
`break`/`continue`). Ich habe die gepatchte Variante gebaut — sie kompiliert unter `cs_5_0`
sauber (nur noch eine Registerdruck-Warnung X4714).

### 1.2 Warum das überhaupt passieren konnte

Die eingebetteten HLSL-Strings werden **zur Laufzeit** von `D3DCompile` übersetzt. Kein
Build-Schritt fasst sie an. Ein kaputter Shader bricht deshalb nichts sichtbar — er schaltet
sein Feature still ab, und auf macOS, wo entwickelt wurde, fällt das nie auf.

Genau dieselbe Klasse gilt für die MSL-Strings in `MetalRenderer.mm` und die GLSL-Strings in
`OpenGLRenderer.cpp`. **Ausgenommen sind die beiden Backends mit Datei-Shadern**: Vulkans 30
`.glsl` (via `glslc`) und D3D12s zwei `gi_*_hw.hlsl` (via `dxc`) werden zur Buildzeit
geprüft — und sind entsprechend heil.

Das ist der Grund, warum P0b eine Build-Prüfung enthält und nicht nur P0 einen Bugfix.

Zur Beruhigung: Ich habe **alle** eingebetteten HLSL-Shader durch `fxc` geschickt — 21
Strings, 29 Entry-Point/Profil-Kombinationen. Genau einer schlägt fehl, der aus §1.1. Es
liegt also kein zweiter stiller Ausfall dieser Art herum. Für die MSL- und GLSL-Strings ist
das nicht geprüft (auf Windows nicht sinnvoll ausführbar für MSL); dort bleibt das Risiko
bestehen, bis P0b die GLSL-Seite mitnimmt.

### 1.3 SSR ist **keine** Metal-Exklusivität — die Shader sind schon portabel

Der naheliegende Schluss aus dem Code („SSR gibt es nur auf Metal, also muss es dreimal neu
entworfen werden") ist falsch. Alle vier SSR-Shader liegen als kanonisches GLSL 450 im
geteilten Cross-Compiler, und jeder Accessor hat neben dem Metal-Zweig einen generischen
`else`-Zweig auf `toTarget(backend)`:

| Shader | Quelle | Accessor |
|---|---|---|
| `kSSRTraceFS` | `MaterialShaderLibrary.cpp:1116` | `:1554` (generisch `:1569`) |
| `kSSRCompositeFS` | `:1369` | `:1573` (generisch `:1595`) |
| `kSSRBlurFS` | `:1479` | `:1599` (generisch `:1610`) |
| `kSSRRoughMixFS` | `:1509` | `:1537` (generisch `:1550`) |

Dieser `else`-Zweig wurde für SSR allerdings **nie aufgerufen** — einziger Aufrufer heute ist
`MetalRenderer.mm:10812-10815`. Ob er trägt, war damit unbewiesen. Ich habe die vier Quellen
extrahiert und gegen `he_shadercompiler` durch alle drei Zielformate geschickt:

```
kSSRTraceFS       HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK
kSSRCompositeFS   HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK   (mit injectPreamble)
kSSRBlurFS        HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK
kSSRRoughMixFS    HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK
```

**SSR auf D3D/Vulkan ist damit Verdrahtungsarbeit, keine Shader-Entwicklung.** Das ist die
größte Einzelkorrektur an der Aufwandsschätzung in diesem Dokument.

> **Korrektur 2026-08-22: Der Absatz oben gilt für Vulkan — für D3D ist er FALSCH.**
>
> Die Prüfung damals hat nur den **Cross-Compile** gemessen, nicht die Übersetzung des
> Ergebnisses. `Compiled.ok` bedeutet „SPIRV-Cross hat HLSL erzeugt", nicht „FXC nimmt es
> an". Genau diese Lücke hat §1.1 (der tote GI-Stack) schon einmal ausgenutzt, und sie hat
> hier ein zweites Mal zugeschlagen.
>
> Nachgemessen: das generierte HLSL auf Platte geschrieben und durch `fxc /T ps_5_0`
> geschickt. **Alle vier Shader fallen durch:**
>
> | Shader | SPIR-V (Vulkan) | GLSL 410 | `ps_5_0` | Grund |
> |---|:--:|:--:|:--:|---|
> | `ssrTrace` | OK | OK | **X3511** | Loop über `steps` nicht abrollbar (115 Iterationen) |
> | `ssrBlur` | OK | OK | **X4567** | `cbuffer register(b23)`, `ps_5_0` hat b0–b13 |
> | `ssrRoughMix` | OK | OK | **X4567** | dito |
> | `ssrComposite` | OK | OK | **X4567 + Sampler** | b23 **und** 19 Sampler, `ps_5_0` hat s0–s15 |
>
> Ursache ist dieselbe wie bei A4: SPIRV-Cross bildet GLSL-Binding N stumpf auf
> `register(sN)`/`register(bN)` ab, Metal hat dafür eine Pin-Tabelle
> (`compileMslPinned`), HLSL hat **keine**. `ShaderCompiler.cpp` setzt für `HlslSm50` nur
> `shader_model=50`; `add_hlsl_resource_binding` kommt im ganzen Baum nicht vor.
>
> **Das verbindet zwei Baustellen:** ein Resource-Remap für das HLSL-Ziel löst A4
> (Graph-Materialien auf D3D) **und** drei der vier SSR-Shader auf einen Schlag. Damit ist
> es die Arbeit mit der größten Hebelwirkung im ganzen Plan — nicht mehr nur „A4".
>
> > **Stand 2026-08-22: Remap gebaut, A4 auf D3D behoben.**
> >
> > `he::shaderc::compileHlslPinned` + `HlslPin` ist das HLSL-Gegenstück zu `MslPin`, das
> > es für Metal seit jeher gibt. Die Pin-Tabelle für Material-Fragmente steht in
> > `MaterialShaderLibrary::fragment` neben der Metal-Tabelle.
> >
> > Die Rechnung geht exakt auf, deshalb ist es eine feste Liste und keine Heuristik: ein
> > beleuchtetes Graph-Material referenziert **16** Sampler-Bindings — `heTex0`,
> > `heTexP0..3`, die beiden GI-Masken, CSM, Schatten-Atlas, Sky-Cube, AO, die beiden
> > DDGI-Atlanten, Forward-SSR, Forward-GI-Reflection und die Wolkenschatten. Zehn liegen
> > schon unter s16 und behalten ihre Nummer; **sechs müssen umziehen**, und unterhalb der
> > Grenze sind genau sechs Register frei (s0, s1, s3, s8, s9, s14). Texturen sind nie das
> > Problem (t0..t127), nur Sampler — deshalb wird ausschließlich die Sampler-Hälfte
> > verdichtet.
> >
> > **Beleg:** „A4 material PS compile failed" erscheint auf D3D11 und D3D12 **nicht mehr**
> > (vorher bei jedem Graph-Material), und der Szenen-Frame ändert sich auf beiden —
> > D3D11, D3D12 und Vulkan liefern denselben Bildmittelwert, zeichnen also dasselbe.
> > Vorher zeichneten alle drei den eingebauten PBR-Ersatz. Die Mechanik selbst wurde
> > vorab isoliert nachgewiesen: derselbe Shader ohne Pins wird von `fxc /T ps_5_0`
> > abgelehnt, mit Pins übersetzt er.
> >
> > **Was weiterhin NICHT geht, und warum es nicht am Umnummerieren liegt:**
> > `heLandscapeWeights` (Binding 14) wäre das **siebzehnte**. Es gibt kein freies
> > Register mehr, und man kann auch keins schaffen: FXC lehnt zwei `SamplerState` auf
> > demselben Register ab (X4500, gemessen), und SPIRV-Cross erzeugt pro Binding einen
> > eigenen — auch wenn man beide auf denselben Slot pinnt (ebenfalls gemessen). Metals
> > Ausweg, einen Sampler inline als constexpr zu erzeugen, hat unter SM 5.0 kein
> > Gegenstück. Ein Landschafts-Graph-Material scheitert also weiterhin auf D3D, jetzt
> > aber mit einer **benennbaren** Grenze statt pauschal.
> > Der Weg dorthin wäre, in der geteilten Präambel Texturen und Sampler zu trennen, damit
> > EIN `SamplerState` viele Texturen bedienen kann — das funktioniert nachweislich
> > (gemessen), ist aber eine Änderung an jedem Backend und gehört in eine eigene Phase.
> >
> > **Beide losen Enden inzwischen geschlossen — und das Ergebnis ist das deutlichste
> > dieser ganzen Arbeit.** D3D11 und D3D12 binden jetzt die vollständige Präambel
> > (Typen exakt: `heCsm`/`heLocalShadow` als `Texture2DArray`, `heSkyEnv` als
> > `TextureCube`, echte Ressource wo vorhanden, sonst getypter Default in der Richtung,
> > die den Term im Shader wegfaltet — weiß für AO, schwarz für SSR/GI-Reflexion). D3D12
> > hat dafür Root-Signature und Heap von 8 auf 16 Slots erweitert, mit sechs
> > Deskriptor-Bereichen, weil der Registerraum Löcher hat. Und die Material-Kachel
> > nimmt auf beiden jetzt den Graph-Pfad.
> >
> > Kacheln gegen OpenGL, vorher → nachher:
> >
> > | Kachel | D3D11 | D3D12 |
> > |---|---|---|
> > | `material` | 60,0 % → **0,0 %** | 60,0 % → **0,0 %** |
> > | `material_function` | 63,6 % → **0,0 %** | 63,6 % → **0,0 %** |
> >
> > Die Material-**Vorschau** trifft auf allen drei Zielbackends **maxdiff = 1**. D3D11 und
> > D3D12 zeichneten dort vorher eine grüne PBR-Ersatzkugel.
> >
> > Zwei Punkte, die bewusst offen bleiben:
> > * **D3D11 und D3D12 laufen bei zwei Gates auseinander.** D3D12 setzt `fog.w` (AO) und
> >   `giProbe.y` (DDGI) für Graph-Materialien so, wie es die eingebauten Shader dort schon
> >   tun und wie OpenGL es tut; D3D11 lässt beide zu, weil es `giGridOrigin`/`giGridCounts`
> >   nie füllt und ein geöffnetes Probe-Gate ohne Gitter Müll schattiert. In der
> >   Kachelszene zeigt sich der Unterschied nicht (beide 0,0 %). D3D12s Wahl ist die
> >   GL-treue; D3D11 zieht nach, sobald es das Gitter füllt.
> > * **Landschafts-Graph-Materialien** scheitern weiterhin auf D3D — das ist der
> >   Sampler-Deckel, nicht diese Arbeit. Siehe den Umbau unten.
>
> **Aber Umnummerieren allein reicht für `ssrComposite` nicht.** Der braucht 19 verschiedene
> Sampler; `ps_5_0` kennt 16. Da hilft keine Tabelle. Entweder teilen sich mehrere Texturen
> einen `SamplerState` (SPIRV-Cross erzeugt aus GLSLs kombinierten Samplern je ein eigenes
> Paar, obwohl fast alle linear-clamp sind), oder der Pfad geht auf D3D12 über `dxc` und
> SM 6.x statt `fxc`/SM 5.0. Das ist eine Entwurfsentscheidung, keine Fleißarbeit.
>
> `ssrTrace` ist davon unabhängig: der Loop braucht ein `[loop]`-Attribut statt
> Abrollen — betrifft aber die geteilte GLSL-Quelle und damit auch Metal.
>
> Belegt mit einem Wegwerf-Programm gegen `he_materialshader` + `he_shadercompiler`;
> die erzeugten `.hlsl` liegen dem Befund zugrunde, nicht eine Ableitung.

Für den *Forward*-SSR kommt hinzu: der Konsument ist bereits in jedes Zielbackend
einkompiliert (`heSSRFwd`, `MaterialShaderLibrary.cpp:257`, Mix-Zweig `:504-508`), er ist nur
beweisbar unerreichbar, weil `heLight.ssr` außerhalb von Metal nirgends geschrieben wird. Die
Library sagt das selbst (`:496-497`: Gates sind 0, „dead code after constant folding").

### 1.4 Der Deferred-Resolve ist teilweise schon portabel — die Sperre ist eine Zeile

`compileResolveVariant` (`MaterialShaderLibrary.cpp:1024`) übersetzt den **nicht-tile,
nicht-clustered** Resolve für *jedes* Backend. Tile- und Clustered-Variante werden dagegen
hart abgewiesen:

```cpp
if (backend != Backend::Metal) {
    // GL keeps the sampled non-clustered resolve (no SSBOs in GL 4.1, no
    // framebuffer fetch); the tile/clustered variants are Metal-only.
    if (tile || clustered) return {};
```

Die Begründung ist **GL-4.1-spezifisch** — und gilt für D3D11/D3D12/Vulkan nicht: die haben
Structured Buffers bzw. SSBOs. Die Clustered-Variante ist für sie also nicht technisch
gesperrt, sondern nur von dieser Zeile. Für die Tile-Variante gilt das nur eingeschränkt
(siehe §4).

---

## 2. Paritätsmatrix

`JA` = alle drei Belege vorhanden · `~` = teilweise · `--` = fehlend.
**OpenGL ist nicht Ziel dieses Plans, sondern Referenz**: wo GL `JA` steht, existiert bereits
eine nicht-Apple-Implementierung — Portierung statt Neuentwurf.

### Post-Processing & Basispässe — weitgehend erreicht

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| HDR-Tonemap / PostProcessPass | JA | JA | JA | JA |
| Bloom | JA | JA | JA | JA |
| SSAO / HBAO / GTAO + Blur | JA | JA | JA | JA |
| FXAA | JA | JA | JA | JA |
| Transparenz-Pfad | JA | JA | JA | JA |
| Skinned-Geometry-Pass | JA | JA | JA | JA |
| Debug-Linien | JA | JA | JA | JA |
| ImGui-Anbindung | JA | JA | JA | JA |
| Viewport-Target + `CaptureViewport` | JA | JA | JA | JA |
| `FrameGpuStats` (Whole-Frame) | JA | JA | JA | JA |
| Lens-Flare-Overlay | JA | -- | -- | -- |
| SSAO über G-Buffer-Tiefenrekonstruktion | JA | -- | -- | -- |

### Schatten

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| CSM-Depth-Pass (3 Kaskaden) | JA | ~ | ~ | ~ |
| CSM-Sampling + 3×3-PCF (Forward) | JA | ~ | ~ | ~ |
| Normal-Offset + Slope-Scaled Bias | JA | ~ | ~ | ~ |
| Punkt-/Spot-Schatten (16-Layer-Atlas) | JA | -- | -- | -- |
| Shadow-Cascade-Debug (`SetShadowDebug`) | JA | -- | -- | -- |
| CSM + Atlas für Graph-Materialien (`heLitP`) | ~ | -- | -- | -- |

### Global Illumination — hier wirkt §1.1

Die `~` auf D3D11/D3D12 sind **ausschließlich** Folge des toten Kernels. Der Code ist
geschrieben; er läuft nicht.

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Ray-getracte Schatten ersetzen CSM | JA | JA | JA | JA |
| DDGI-Probe-Update | JA | ~ | ~ | JA |
| DDGI-Feldsampling im Shading | JA | ~ | ~ | JA |
| RT-Shadow-Pass (Direktional) | JA | ~ | ~ | JA |
| RT-Schattenmaske Punkt/Spot | JA | ~ | ~ | JA |
| Software-RT-Fallback (`HE::GiBvh`) | JA | ~ | ~ | JA |
| Multi-Bounce-Probe-Feedback | JA | ~ | ~ | JA |
| HW-RT-Acceleration-Structures | -- | -- | ~ | JA |
| RT-GI-Reflections | JA | -- | -- | -- |
| Painted-Landscape-Tabelle | JA | -- | -- | -- |
| Mehrfach-Spiegel-Bounces | -- | -- | -- | -- |

### Deferred / Lighting

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Deferred G-Buffer-Pass (4 MRT) | JA | -- | -- | -- |
| Zwei-Pass-Resolve (gesampelt) | JA | -- | -- | -- |
| `heLitP` Lighting-ABI-Fill | JA | ~ | ~ | ~ |
| G-Buffer-Variante für Graph-Materials | JA | -- | -- | -- |
| RenderPath-Umschaltung Forward/Deferred | ~ | -- | -- | -- |
| Tile-Memory-Resolve (Framebuffer-Fetch) | -- | -- | -- | -- |
| Deferred Decals | -- | -- | -- | -- |
| Clustered-Lighting-Build | -- | -- | -- | -- |
| SSR deferred / SSR forward | -- | -- | -- | -- |

### Himmel & Wetter

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Skybox mit Rayleigh/Mie/Ozon-Atmosphäre | JA | ~ | ~ | ~ |
| Nachthimmel (Sterne, Nebel, 3D-Aurora, Meteore) | JA | ~ | ~ | ~ |
| Mond (Phase, Corona, prozedurale Oberfläche) | JA | ~ | ~ | ~ |
| Wolken-Raymarch (Dome + 3D-Volumen) | JA | ~ | ~ | ~ |
| Low-Res-Cloud-Pass + Reprojektion | JA | -- | -- | -- |
| Cirrus, Kondensstreifen, Regenbogen, God-Rays | JA | -- | -- | -- |
| GPU-Wetter-Partikel | JA | -- | -- | -- |
| Sky-Env-Bake (IBL-Cubemap) | ~ | -- | -- | -- |

### Editor-Integration — der geschlossenste Block

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Material-Vorschau (D3D: PBR-Fallback bis A4 steht) | JA | ~ | ~ | JA |
| Skeletal-Vorschau (Bone-Overlay) | JA | JA | JA | JA |
| Partikel-Vorschau | JA | JA | JA | JA |
| Asset-Thumbnails — Mesh | JA | JA | JA | JA |
| Asset-Thumbnails — Material (nur PBR-Fallback, Graph-Shader fehlt: A4) | JA | ~ | ~ | ~ |
| Asset-Thumbnails — Skeletal | JA | JA | JA | JA |
| Partikel-Thumbnails | JA | JA | JA | JA |
| Widget-Thumbnails | JA | JA | JA | JA |
| `WarmupMaterials` | JA | JA | JA | JA |
| `InvalidateMaterial/Mesh/Texture` | JA | ~ | ~ | ~ |
| Multi-Window (alle vier: Swapchain + Clear + Present, **kein** Szeneninhalt) | JA | JA | JA | JA |
| Per-Pass-GPU-Timing | JA | JA | JA | JA |
| Shadow-Cascade-Debug (`SetShadowDebug`) — hängt hinter CSM | JA | -- | -- | -- |
| Detailed-Capture (1 Command-Buffer/Pass) | JA | ~ | -- | -- |

**Keine Lücke, anders gelöst:** D3D12 hat kein `GetViewportTexture`-Override, löst dasselbe
aber über `GetViewportD3DResource` + `HasViewportResourceChanged`. Nicht nachziehen.

---

## 3. D3D11 — die Obergrenze offen ausgesprochen

„D3D" meint hier zwei sehr verschiedene Backends. D3D11 ist auf **CS 5.0** festgelegt und hat
**keine Hardware-Raytracing-API** — kein DXR, kein `RayQuery`, kein `ID3D11Device5`
(Grep nach DXR/RayQuery/`ACCELERATION_STRUCTURE` in `D3D11Renderer.cpp`: null Treffer).

Für D3D11 gilt deshalb dauerhaft:

- GI läuft **nur** über den Software-RT-Pfad (`HE::GiBvh`). Das ist kein Defizit des Plans —
  es ist derselbe Pfad, der auf Metal ohne HW-RT greift, und er ist der portabelste.
- Clustered Lighting ist über Structured Buffers machbar, Tile-Memory-Resolve nicht.
- Alles unter P0/P2/P3/P5 ist ohne Einschränkung erreichbar.

Wo im Folgenden „D3D" steht und D3D11 abweicht, ist es benannt.

---

## 4. Was **nicht** portiert wird — und warum

Ein Plan, der Metal-Exklusives als Lücke führt, erzeugt Arbeit, die nie fertig wird.

**Tile-Memory-Deferred-Resolve** (`MetalRenderer.mm:10620`) liest den G-Buffer per
Framebuffer-Fetch aus Tile-Speicher, statt gespeicherte Texturen zu sampeln. Das ist ein
Apple-GPU-Merkmal (MSL 2.3, `[[color(n)]]`).
→ **D3D12: kein Äquivalent.** Der ehrliche Zielzustand ist der Zwei-Pass-Resolve.
→ **Vulkan: eingeschränkt möglich** über Subpass-Input-Attachments innerhalb eines
Render-Passes — aber das ist ein eigener Entwurf, kein Port. Nicht in diesem Plan.

**Deferred Decals** (`:10693`) und **Clustered-Lighting-Build** (`:10454`) hängen im Metal-Code
am offenen Tile-G-Buffer-Pass. Der *Algorithmus* ist portabel (Clustered braucht nur
Structured Buffers/SSBOs, siehe §1.4), die *Einbettung* nicht. Sie stehen deshalb erst nach
P4 an, nicht darin.

**Echte Hit-Normalen über Tier-2-Argument-Buffer** (`refl-true-hit-normals-argbuffer`) ist
ein Metal-Ressourcenmodell-Merkmal. Auf D3D12/Vulkan wäre das Bindless — anderer Entwurf.

---

## 5. Phasenplan

Konvention wie in `docs/ssr-plan.md`, `docs/deferred-renderer-plan.md` und
`docs/gi-reflections-plan.md`: jede Phase ist einzeln commit- und verifizierbar, nach jeder
Phase läuft die Engine, jede Phase endet mit einem konkreten Abnahme-Gate.

Reihenfolge nach Nutzen pro Aufwand — nicht nach Feature-Größe.

### P0 — Den toten GI-Stack auf D3D reparieren (Bugfix, kein Feature)

> **Stand 2026-08-11: ERLEDIGT und auf Hardware abgenommen.**
>
> Der Shader-Fix sitzt in `HlslSources.h` (`giLocalVisOne` + vier literale Aufrufe).
> Zusätzlich wird `createGiPipelines()` jetzt **eifrig aus `Initialize`** aufgerufen
> (`D3D11Renderer.cpp`, `D3D12Renderer.cpp`) statt erst im ersten GI-Draw: der Compile
> gehört in die Initialisierung, nicht in einen Frame, und ein Fehlschlag muss
> `giSupported` löschen, *bevor* `GetCapabilities()` zum ersten Mal gelesen wird. Der
> alte Aufruf in `runGiShadow` bleibt als idempotentes Sicherheitsnetz stehen.
>
> Belege: `fxc`-Sweep über alle eingebetteten HLSL-Shader von **1 von 29 Fehlschlägen auf 0**;
> `x64-release`-Build grün; und auf echter Hardware loggt die Engine jetzt
>
> ```
> D3D11Renderer: GI pipelines built (compute ray tracing active)
> D3D12Renderer: GI pipelines built (compute ray tracing active)
> D3D12Renderer: GI hardware ray tracing available (DXR 1.1 inline RayQuery)
> ```
>
> — vorher stand dort „GI pipeline build failed — GI disabled", und die DXR-Zeile war
> strukturell unerreichbar. Sie erscheint hier zum ersten Mal.
>
> **Zwei Dinge bleiben offen** (kleiner als P0, eigene Aufgaben):
> 1. Ob der GI-**Draw** pro Frame auf D3D tatsächlich läuft, ist derzeit **nicht
>    beobachtbar**: D3D hat keinerlei GI-Laufzeit-Logs, GL dagegen schon
>    (`OpenGLRenderer.cpp:5785`, „GI probe grid NxMxK"). Ein Gegenstück auf D3D wäre
>    die billigste Abhilfe und Voraussetzung für jede spätere GI-Verifikation.
> 2. Die Bleed-Zeugenszene taugt nicht als Gate — siehe Verifikations-Notiz unten.

Die mit Abstand höchste Rendite im ganzen Dokument: sechs Features auf zwei Backends plus der
komplette DXR-Pfad auf D3D12 sind bereits geschrieben und werden durch ~14 Zeilen HLSL
blockiert.

1. `HlslSources.h:626-641`: den `[unroll]`-Loop durch vier Aufrufe einer Hilfsfunktion
   `giLocalVisOne(int idx, float3 P, float3 N)` mit **literalem** Index ersetzen, die einen
   Skalar zurückgibt. Kein dynamischer Vektor-Lvalue, kein `break`/`continue`.
   Den falschen Kommentar `:628-629` mitkorrigieren.
2. Prüfen, ob D3D11 nach dem Fix `giSupported` tatsächlich behält (`:1677` → `:1706`), und
   ob auf D3D12 der DXR-Zweig (`D3D12Renderer.cpp:4220`) jetzt erreicht wird. Er sollte —
   sobald der SW-Compile durchläuft, ist `ok` wahr, das frühe `return` (`:4204-4213`)
   entfällt und `createGiHwPipelines()` läuft. **Für P0 ist keine Umstellung nötig.**

> **Nicht tun: `createGiHwPipelines()` vor den `if (!ok) return`-Block ziehen.** Der
> Blockkommentar `:4218-4219` sagt, es sei ein „optional DXR 1.1 upgrade **on top of** the
> working SW pipelines" — der Fehlerpfad hat unmittelbar davor `giShadowPSO`/`giProbePSO`
> und die Heaps `Reset()`. Vorziehen hieße, den HW-Aufbau auf abgeräumten Ressourcen laufen
> zu lassen. Ob die Kopplung *dauerhaft* bestehen bleiben soll (heute reißt ein künftiger
> SW-Shader-Bruch den DXR-Pfad wieder mit), ist eine **offene Frage** — sie verlangt zu
> prüfen, was `createGiHwPipelines()` tatsächlich aus dem SW-Aufbau liest. Das ist bewusst
> nicht Teil von P0.

**Verifikation — Teil 1 (statisch, erledigt):**
`fxc /T cs_5_0 /E GiShadowCS` auf dem konkatenierten `kGiTraversalHLSL + kGiShadowCSHLSL`
läuft fehlerfrei durch (vorher: X3500 + X3511). Der Sweep über alle 29
Entry-Point/Profil-Kombinationen ist grün.

**Verifikation — Teil 2 (GPU, erledigt):** Abgenommen über das Init-Log (siehe Statusblock
oben). Der *naheliegende* Weg — ein Bild-A/B — funktioniert dagegen **nicht**; hier steht,
was ich probiert habe, damit es niemand ein zweites Mal probiert:

- Ein Headless-A/B `HE_DUMP_RHI=D3D11|D3D12|OpenGL` × `HE_DUMP_GI=1|0` mit der
  Bleed-Zeugenszene (`HE_DUMP_MATERIALTEST=1 HE_DUMP_GIBLEED=1`, bis `HE_DUMP_FRAMES=60`)
  erzeugt zwar auf allen Backends Bilder, aber GI-an und GI-aus sind praktisch identisch
  (max. 1 LSB) — **auch auf OpenGL, wo im selben Lauf „GI probe grid 7x6x7 (294 probes)"
  geloggt wird, GI also nachweislich lief.** Der Dump bildet GI somit auf *keinem* Backend
  ab; das ist ein Mangel von Szene/Kamera, nicht von D3D. Wer es erneut versucht, muss
  zuerst Kamera und Tageszeit (`HE_DUMP_CAMX/Y/Z`, `HE_DUMP_YAW/PITCH`, `HE_DUMP_TOD`) so
  setzen, dass die Sphären-Unterseite über dem roten Boden im Bild liegt.
- Eine Sackgasse beim Loggen: den Editor live laufen zu lassen und abzuschießen verliert
  das gepufferte Log (bricht reproduzierbar bei ~8 kB ab, egal ob PowerShell-Pipe,
  `Start-Process -RedirectStandardOutput` oder `cmd`-Umleitung). Nur der saubere Exit über
  `HE_DUMP_QUIT=1` liefert ein vollständiges Log — und genau deshalb muss alles, was man
  beobachten will, **bis zum Ende von `Initialize`** passiert sein. Das ist der zweite
  Grund für die eifrige Pipeline-Erzeugung.

> **Widerlegte Vermutung** (steht hier, weil sie naheliegt und falsch ist): Der GI-Block
> hängt hinter `if (io.output.id != kBackbufferTarget) return;` (`D3D11Renderer.cpp:3550`),
> was wie ein Ausschluss der Offscreen-Viewport-Ansicht aussieht. Ist es nicht.
> `kBackbufferTarget = 0` heißt „aktives Ausgabeziel" (`RenderTarget.h:28`), und der
> `GeometryPass` trägt diese id **immer** (`RenderPass.cpp:160`) — in beiden Pfaden. Der
> Gate trennt nur Geometrie- von Shadow-Pass.

### P0b — Shader-Prüfung in den Build ziehen

> **Stand 2026-08-11: ERLEDIGT.**
> `scripts/validate_embedded_shaders.py` + CMake-Ziel `HeValidateShaders` (in `ALL`,
> abschaltbar über `-DHE_VALIDATE_SHADERS=OFF`). Deckt **75 Shader** ab: 36 HLSL-Jobs
> über `fxc` (SM 5.0) und 39 GLSL-Stages über `glslangValidator` — deutlich mehr als die
> 29, die ich für §1.1 von Hand geprüft hatte. Laufzeit ~3 s.
>
> Beide Eigenschaften sind nachgewiesen, nicht angenommen:
> * Stellt man den Original-`[unroll]`-Loop wieder her, meldet das Skript Exit 1 mit
>   X3500 + X3511 auf genau `kGiShadowCSHLSL`.
> * Ein absichtlich fehlendes Semikolon lässt den **Build** abbrechen
>   (`FAILED: … HeValidateShaders` → `ninja: build stopped`).
>
> Fehlt fxc oder glslangValidator, wird die betreffende Hälfte mit Meldung übersprungen,
> statt fehlzuschlagen — ein Checkout ohne Windows- bzw. Vulkan-SDK baut weiterhin.

P0 repariert einen Fall. P0b sorgt dafür, dass der nächste überhaupt auffällt (§1.2).

Ein CMake-/CI-Schritt, der die eingebetteten Shader-Strings extrahiert und übersetzt:
HLSL gegen `fxc` (SM 5.0), GLSL aus `OpenGLRenderer.cpp` gegen `glslangValidator`. Die
beiden Datei-Shader-Pfade (Vulkan `.glsl`, D3D12 `gi_*_hw.hlsl`) sind bereits abgedeckt und
brauchen nichts.

Der HLSL-Umfang ist bekannt und klein: **21 Strings, 29 Entry-Point/Profil-Kombinationen**,
verteilt auf drei Dateien — `HlslSources.h` (geteilt) sowie je eine eigene Kopie von
Szene/Sky/FXAA/Bloom/UI in `D3D11Renderer.cpp:65-725` und `D3D12Renderer.cpp:397`. Zwei
Strings sind reine Präludien ohne eigenen Entry (`kGiTraversalHLSL`, `kSkyFuncHLSL`); der
Extraktor muss die Konkatenationen der Aufrufstellen nachbilden — `kSkyFuncHLSL + kSceneHLSL`
(`D3D11Renderer.cpp:2309`), `kSkyFuncHLSL + kSkyPSHLSL` (`:2802`), `kGiTraversalHLSL +
kGiShadowCSHLSL` (`:1677`) und `+ kGiProbeCSHLSL` (`:1679`). Ohne das erzeugt er
Falschmeldungen (`undeclared identifier 'skyColor'`).

**Verifikation:** erledigt, siehe Statusblock oben. Zwei Fallstricke, die beim Bau der
Extraktion aufgefallen sind und die jede spätere Erweiterung wieder treffen werden:

* **`main` ist nicht immer ein Pixel-Shader.** `kFSTriangleVS` wird mit Entry `main` als
  `vs_5_0` übersetzt (`D3D12Renderer.cpp:1295`, `:3040`, `:4079`). Deshalb gibt es im
  Skript eine explizite Ausnahmetabelle statt einer Namens-Heuristik.
* **D3D12 hat eigene Kopien unter eigenen Namen** — `kSkyPSHLSL12` (`:95`) braucht dasselbe
  `kSkyFuncHLSL`-Präludium wie D3D11s `kSkyPSHLSL`. Wer nur nach dem D3D11-Namen sucht,
  hält die D3D12-Kopie fälschlich für kaputt.

Gegen genau dieses Verrotten hat das Skript eine **Abdeckungsprüfung**: existiert ein
Shader-String, den kein Job erfasst, schlägt es mit „not covered by any job" fehl. Ein
Validator, der neue Shader stillschweigend überspringt, ist schlechter als keiner — er
liest sich wie ein bestandener Test.

### P1 — Editor-Integration nachziehen (D3D11, D3D12, Vulkan)

> **Stand 2026-08-19: P1a und P1b ERLEDIGT und per Bild-A/B abgenommen. P1c/P1d/P1e offen.**
>
> Umgesetzt auf **allen drei** Zielbackends: `WarmupMaterials`, `RenderAssetThumbnail`,
> `RenderWidgetThumbnail`, `RenderParticleThumbnail`. Neu und geteilt:
> `include/HorizonRendering/PreviewFraming.h` (Framing-Konstanten, `boundsCenter/Extent`,
> `meshOrbit`), `HE::hlsl::kMeshPreviewHLSL` für D3D11+D3D12 und
> `shaders/mesh_preview.{vert,frag}` für Vulkan — Portierungen von GLs
> `kMeshPreviewVS/FS`.
>
> **Das Abnahme-Gate, und es ist diesmal ein Bild und keine Log-Zeile:**
> `HE_DUMP_THUMB` ruft *alle drei* Thumbnail-Einstiegspunkte über
> `AssetThumbnailCache` auf und schreibt die zurückgegebenen PIXEL als PPM. Vorher
> meldete jedes der drei Backends **6 fehlende Kacheln**, jetzt **0**; statt 2 Kacheln
> (die beiden CPU-erzeugten) entstehen **8**.
>
> Gegen OpenGL, 128×128, subpixelgenau verglichen (`maxdiff` = größte Abweichung
> eines einzelnen Kanalwertes, 0–255):
>
> | Kachel | D3D11 | D3D12 | Vulkan | Befund |
> |---|:--:|:--:|:--:|---|
> | `mesh` | **byte-gleich** | **byte-gleich** | **byte-gleich** | Framing, Clip-Space, Shader, Readback stimmen |
> | `particles` | **byte-gleich** | **byte-gleich** | **byte-gleich** | |
> | `material_pbr` | 1 Subpixel, maxdiff 1 | dito | dito | Rundung, kein Befund |
> | `material` | 60 % ≠ | 60 % ≠ | 60 % ≠ | erwartet — siehe A4 unten |
> | `material_function` | 64 % ≠ | 64 % ≠ | 64 % ≠ | erwartet — siehe A4 unten |
> | `widget` | maxdiff 106 | maxdiff 106 | maxdiff 220 | erwartet — siehe UI-Alpha unten |
>
> `mesh` und `particles` sind auf allen drei Backends **byte-identisch** mit OpenGL,
> `material_pbr` weicht in **genau einem** Subpixel um **1** ab. Das ist der eigentliche
> Beleg dieser Phase: Framing-Header, Clip-Space-Fix, portierter Shader und
> Readback-Vertrag sind gleichzeitig richtig — jeder einzelne Fehler darin hätte eine
> sichtbare Abweichung erzeugt.
>
> Die drei Zielbackends sind **untereinander byte-gleich** — einzige Ausnahme 24 Pixel
> (0,1 %) in einem 8×4-Feld der Vulkan-Widget-Kachel, Glyphen-Kantenglättung.
> OpenGL ist in allen acht Kacheln unverändert; der Port hat die Referenz nicht angefasst.
>
> **Die GL-Referenz ist kalt erzeugt.** Der Vergleich wurde mit beiseitegeschobenem
> `%APPDATA%\HorizonCreations\HorizonEngine\glprogcache` wiederholt (Log: „built a
> material program from canonical GLSL" ×2, „binary cache" ×0). Kalt und warm liefern
> in allen sechs Kacheln dasselbe Bild — die Referenzseite hängt also nicht am
> `glProgramBinary`-Pfad, und die Byte-Gleichheit oben gilt gegen beide.
> Anmerkung zur Sauberkeit des A/B: der Vorher-Lauf hatte zusätzlich `HE_DUMP_PREVIEW=1`
> gesetzt, der Nachher-Lauf nicht. Das Gate 6→0 stammt aus den Thumb-Läufen beider
> Seiten, auf die diese Variable nachweislich nicht wirkt (anderer Codepfad, und ohne
> backendseitigen PPM-Writer schreibt sie ohnehin nichts).
>
> **Die beiden Abweichungen sind benannt, nicht übersehen:**
> 1. **`material` / `material_function`** zeigen dieselbe Kugel in identischer Lage und
>    Größe, aber flach im `baseColor` des Materials statt im Graph-Shader. Ursache ist
>    die A4-Lücke: D3D11 und Vulkan binden vier Kopien der weißen Default-Textur an
>    `heTexP0..3`, D3D12 legt die Slots als NULL-SRVs an (samplen als 0,0,0,0). Ein
>    Graph-Material wäre dort weiß bzw. schwarz — **schlechter** als die ehrliche
>    PBR-Ersatzkugel. Deshalb zeichnen alle drei bewusst nur den Fallback. Das wird
>    mit dem Graph-Textur-Cache geschlossen, nicht hier.
> 2. **`widget`** unterscheidet sich um eine gleichmäßige Alpha-Verschiebung im Panel.
>    Das ist **keine** Regression dieses Ports: GLs UI-Pass mischt mit
>    `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`, was auch Alpha erfasst — Metal
>    (`MetalRenderer.mm:6019`), D3D11, D3D12 und Vulkan setzen alle
>    `sourceAlpha = ONE / destAlpha = ZERO`. **Bei diesem Detail ist GL der Ausreißer
>    und die drei Zielbackends stimmen jetzt mit Metal überein.** Wer das angleicht,
>    muss entscheiden welche der beiden Seiten recht hat, und das ist eine Änderung am
>    gemeinsamen UI-Pass, nicht am Thumbnail-Pfad.
>
> **Drei Nebenbefunde waren ein einziger Bug — inzwischen behoben.** Sie standen vor
> P1c und sind deshalb sofort mit erledigt worden:
> D3D11 beendete den Headless-Lauf mit `0xC0000374` (Heap-Korruption), D3D12 mit
> `0xC0000005`, und Vulkan leakte 10 Objekte an `vkDestroyDevice`.
>
> Ursache war `EditorApplication.cpp:965`: `m_backend` wurde dort **nach** der
> ImGui-Initialisierung noch einmal aus der persistierten Konfiguration gelesen.
> `CreateRenderer()` (`:210`) hatte es zuvor korrekt aus `GetConfig().backend` gesetzt —
> und nur das kennt die `HE_DUMP_RHI`-Übersteuerung. Sobald beide auseinanderliefen, war
> ImGui auf dem einen Backend initialisiert, während jedes spätere `switch (m_backend)`
> den Zweig des anderen nahm: `NewFrame`, `RenderDrawData` und vor allem der Shutdown,
> der dann ein ImGui-Backend über die Datenstruktur eines anderen fuhr. Daher drei
> verschiedene Symptome aus einer Zeile. Das verräterische Zeichen stand die ganze Zeit
> im Log: „Failed to initialize OpenGL loader!" in einem D3D11-Lauf.
>
> Der Fix ist die **Streichung** der Zuweisung; `:210` ist jetzt einziger Schreiber.
> Bewusst **nicht** über `setSelectedRHI(m_backend)` geradegezogen — `writeConfig()`
> würde die Übersteuerung sonst in die `config.json` des Nutzers schreiben und sein
> gespeichertes Backend bei jedem Headless-Lauf still ändern.
>
> Beleg, volle Matrix persistiertes RHI × `HE_DUMP_RHI`, alle 16 Zellen:
> **16/16 exit 0, 0 Leaks, 0-mal „Failed to initialize OpenGL loader"** (vorher stürzten
> die Zellen mit ungleichem Paar ab bzw. leakten). Die acht Thumbnail-Kacheln sind auf
> allen vier Backends unverändert.
>
> Das war zugleich eine **Falle für P1c**: derselbe verfälschte Wert landet über
> `EditorApplication.cpp:4545` in `AppContext.backend`, und die Vorschau-Panels leiten
> daraus ihr `flipY` ab (`ctx.backend == OpenGL`). Unter `HE_DUMP_RHI` hätte also jede
> Vorschau auf genau den drei Zielbackends kopfüber gestanden — in genau den Läufen, mit
> denen man sie abnimmt.
>
> Zusätzlich abgesichert: `D3D12DescriptorHeapAllocator::Alloc`
> (`EditorApplication.cpp:119`) gibt bei erschöpftem Heap jetzt Null-Handles zurück,
> statt im Release-Build (wo `IM_ASSERT` leer ist) einen Index aus dem Nichts zu nehmen
> und den daraus gebauten Deskriptor hinter den Heap zeigen zu lassen.
>
> `HeValidateShaders` deckt jetzt **44** HLSL-Jobs statt 36, alle grün. Testsuite
> 1738/1739 — der eine Fehlschlag ist `test_input_gamepad.cpp:612` (SDL-Virtual-Pad),
> und `tests/CMakeLists.txt:98` linkt bewusst **kein** GPU-Backend, kann also nicht
> von dieser Änderung stammen.
>
> **P1c ist inzwischen ebenfalls erledigt — siehe den Block direkt darunter.** Offen
> bleiben P1d (Multi-Window auf D3D11/D3D12) und P1e. `SetShadowDebug` ist dabei **hart
> blockiert** — keines der drei Backends hat ein Kaskaden-Array, das man tönen könnte
> (`D3D11Renderer.cpp:3596`, `D3D12Renderer.cpp:5559`, `VulkanRenderer.cpp:3822`), es
> hängt also hinter CSM. `InvalidateTexture` bleibt bewusst aus: keines der drei hat
> einen texturschlüssel-basierten Cache, den es leeren könnte — der Override wäre ein
> korrekt aussehender No-Op und würde eine Matrixzelle umlegen, ohne etwas zu tun.
> (Vulkan hat inzwischen einen Graph-Textur-Cache, aber nur für den Vorschau-Pfad.)

> **Stand 2026-08-19, Nachtrag: P1c ERLEDIGT und per Bild-A/B abgenommen.**
>
> `RenderMaterialPreview`, `RenderSkeletalPreview` und `RenderParticlePreview` auf allen
> drei Zielbackends, dazu die Voraussetzung `localBounds` am Skeletal-Mesh-Struct — womit
> auch der `ThumbnailKind::SkeletalMesh`-Zweig aus P1b nachgezogen ist, der vorher `false`
> zurückgab.
>
> **Abnahme über `HE_PREVIEW_DUMP`**, das die drei Backends jetzt ebenso implementieren
> wie GL und Metal (vorher gab es auf ihnen **keinen** Pixel-Zeugen für eine Vorschau —
> der Frame-Dump taugt nicht dafür, weil `EditorApplication.cpp:2748` den
> Overlay-Callback auf `nullptr` setzt und ImGui damit nie im Bild ist). 512×512, gegen
> OpenGL:
>
> | Backend | maxdiff | Befund |
> |---|:--:|---|
> | **Vulkan** | **1** | zeichnet das **echte Graph-Material** — praktisch pixelgleich mit GL |
> | D3D11 | 214 | PBR-Ersatzkugel, A4/X4509 (unten) |
> | D3D12 | 214 | dito, **byte-gleich mit D3D11** |
>
> Vulkan ist damit das erste Zielbackend, das eine Node-Graph-Material-Vorschau so
> anzeigt wie die Referenz. D3D11 und D3D12 zeichnen dieselbe Kugel in derselben Lage,
> Größe und Kameraführung — nur die Graph-Shading fehlt.
>
> Drei Festlegungen, die alle drei gleich umsetzen, damit sie nicht auseinanderlaufen:
> 1. **Ein persistentes ImGui-Handle pro Vorschau-Target**, registriert nur beim
>    (Neu-)Anlegen. Die Panels rufen die Einstiegspunkte **jeden Frame** ungetaktet auf
>    (`SkeletalMeshEditorPanel.cpp:228`, `LevelScriptPanel.cpp:1846`,
>    `ParticleGraphEditorPanel.cpp:284`) — eine Registrierung pro Aufruf hätte die
>    64 Deskriptoren in etwa einer Sekunde aufgebraucht. So kostet P1c **drei**.
> 2. **Angeforderte Größe auf Vielfache von 64 aufgerundet**, damit das Ziehen eines
>    Splitters nicht pro Pixel ein Target neu anlegt (und einen Deskriptor verliert).
>    Die Projektion nimmt trotzdem das **angeforderte** Seitenverhältnis: `ImGui::Image`
>    skaliert die gerundete Textur auf den Bereich, und nur so bleibt eine Kugel im
>    Endbild rund.
> 3. **Ein Klemmbereich pro Einstiegspunkt.** Die Skelett-Vorschau geht bis 2048, die
>    anderen bis 1024 — genau wie GL (`OpenGLRenderer.cpp:8588`). Steht jetzt als
>    `HE::clampSkeletalPreviewSize` im geteilten Header, nachdem zwei Backends es
>    zunächst unterschiedlich gemacht hatten.
>
> Skinning ist auf allen drei **echt**, keine Bind-Pose-Notlösung: der Szenen-Skinning-Pfad
> ist aus einem Aufruf außerhalb des Frames nicht erreichbar (sein PS liest Shadow-Map,
> AO, GI-Atlanten), deshalb bekam jede Vorschau ein eigenes kleines Skinning-Programm —
> dieselbe Entscheidung, die GL mit `kSkelPreviewVS` getroffen hat.
>
> Gesamtstand nach P1a–P1c, alle vier Backends: **exit 0, 8/8 Kacheln, 0 fehlende,
> Vorschau-PPM vorhanden, 0 Leaks.** Testsuite 1739/1739. `HeValidateShaders` deckt jetzt
> **91** eingebettete Shader ab (vorher 75).
>
> Bekannte Abweichungen, alle benannt: der Bone-Overlay ist 1 px statt GLs 2 px (weder
> D3D noch Vulkan-ohne-`wideLines` kennen `glLineWidth`), und `HE_DUMP_PREVIEW_STRESS`
> fordert Größen wie 200/216/232 an, die auf 256 aufrunden — dessen PPM ist also nicht
> maßgleich mit GLs. Der reguläre 512er-Schuss, auf dem das Gate steht, ist es.

Der geschlossenste Block der Matrix: elf Features, auf allen drei Zielbackends fehlend, und
**GL ist überall die portable Referenz** — kein Apple-Mechanismus, kein neuer Shader.
Nutzerwirkung ist unmittelbar: heute bleiben Content-Browser-Thumbnails und Vorschaufenster
leer, sobald jemand das Backend umstellt.

- P1a — `InvalidateTexture` vervollständigen (`~` in allen dreien), `WarmupMaterials`.
- P1b — Thumbnails: Asset, Partikel, Widget (Offscreen-Target + Readback existiert überall
  schon, siehe `CaptureViewport` = `JA`).
- P1c — Vorschauen: Material, Skeletal (Bone-Overlay), Partikel.
- P1d — Multi-Window auf D3D11/D3D12 (`AttachWindow`/`DetachWindow`/`RenderWindow`).
  Vulkan hat es bereits. Achtung: Metals Variante legt nur die Swapchain an und rendert
  **keinen Inhalt** — Zielzustand ist GL, nicht Metal.

> **Stand 2026-08-22: P1d ERLEDIGT — aber die Vorgabe oben war in beide Richtungen falsch,
> und das ist der interessantere Teil.**
>
> „Zielzustand ist GL, nicht Metal" trägt nicht: GLs `RenderWindow`
> (`OpenGLRenderer.cpp:11898`) löscht schwarz und trägt wörtlich
> `// TODO: secondary-window draw calls`. Metal tut dasselbe. **Kein** Backend zeichnet
> Szeneninhalt in ein Zweitfenster. Der ehrliche Zielzustand ist deshalb: eigene Swapchain
> pro Fenster, löschen, präsentieren — mehr gibt es nirgends abzuschreiben.
>
> Umgesetzt auf D3D11 und D3D12, bewusst mit **auffälliger Farbe statt Schwarz**
> (RGBA 0.16/0.22/0.34): ein schwarzes Clear ist von „der Override lief nie" nicht zu
> unterscheiden, das Feature wäre also unprüfbar geblieben. Ebenso bewusst **ohne
> Overlay-Injektion** — siehe unten, das ist keine Auslassung sondern ein Befund.
>
> **Drei Fehler, die erst sichtbar wurden, weil dieser Pfad zum ersten Mal überhaupt lief:**
> 1. `Application::createSecondaryWindow` setzte `props.api` nicht, der Default ist OpenGL
>    (`Window.h:23`). Jedes Zweitfenster bekam also `SDL_WINDOW_OPENGL` samt GL-Kontext —
>    auf jedem Backend. Auf Vulkan fehlte dadurch `SDL_WINDOW_VULKAN`, die Surface-Erzeugung
>    schlug fehl, und der Wurf verließ einen Pfad ohne `try/catch`. **Die API war auf keinem
>    Nicht-GL-Backend jemals aufrufbar.**
> 2. Vulkans Zweitfenster reichte `&cmd` an den Overlay-Callback, der Primärpfad `cmd`
>    (`:596`), und der Editor castet auf `VkCommandBuffer` (`EditorApplication.cpp:912`).
>    Da `VkCommandBuffer` selbst ein Zeigertyp ist, kompilierten beide Formen. Der
>    Vertragskommentar in `IRenderer.h` beschrieb die **falsche** Variante.
> 3. Nach Behebung von (1) und (2) lief die Injektion erstmals — und war *inhaltlich*
>    falsch: 15 Validierungsfehler, weil der Zweitfenster-Render-Pass nur ein Attachment
>    hat, ImGuis Pipeline aber gegen den Primär-Pass **mit Tiefe** gebaut wurde
>    („pDepthStencilAttachment ... VK_ATTACHMENT_UNUSED while the second is 1"). Deshalb
>    fliegt die Injektion überall raus: der Callback zeichnet ohnehin die Draw-Daten des
>    HAUPT-Viewports, ins Zweitfenster gespiegelt gäbe das ein abgeschnittenes Duplikat.
>    Abgedockte Editor-Panels bedient ImGui über seine eigenen Viewport-Backends
>    (`ImGuiConfigFlags_ViewportsEnable`), nicht über diese API.
> 4. Vulkans `renderWindowData` wartete auf `frameFence[frameIndex]`, griff aber auf
>    `cmdBufs[imageIndex]` zu — verschiedene Indexräume. Der Primärpfad löst genau das mit
>    `m_imagesInFlight` und einem Kommentar dazu (`:341-346`); dem Zweitfenster-Pfad fehlte
>    die Tabelle. Ergebnis waren „is in use"-Meldungen bei Reset, Begin und Submit.
>
> **Abnahme über `HE_DUMP_SECONDWINDOW=<Frames>`** (neu, `Application.cpp`): legt ein
> Zweitfenster an, präsentiert N Frames, räumt ab und beendet sauber. `=0` ist der
> **Kontrolllauf** — gleiche Schleife, gleiche Frame-Zahl, kein Fenster. Ohne diesen
> Vergleich ließe sich nicht sagen, ob eine Validierungsmeldung vom Zweitfenster kommt oder
> ohnehin im Hauptpfad steht; genau daran wäre die Abnahme sonst gescheitert.
>
> | Backend | attach/detach | exit | Validierung mit Fenster | Kontrolllauf |
> |---|:--:|:--:|:--:|:--:|
> | OpenGL | 1/1 | 0 | 0 | 0 |
> | D3D11 | 1/1 | 0 | 0 | 0 |
> | D3D12 | 1/1 | 0 | 0 | 0 |
> | Vulkan | 1/1 | 0 | 26 | **28** |
>
> Vorher meldeten D3D11 und D3D12 **null** attach/detach — die Overrides existierten nicht.
> Vulkans 26 sind vollständig im Kontrolllauf enthalten (28 ohne Fenster, gleiche Kategorien):
> ein **vorbestehendes** Problem des Hauptpfads, das nur nie jemand gesehen hat, weil Vulkan
> headless nie über die Hauptschleife lief. Eigene Aufgabe, nicht P1d.
>
> Was der Zeuge belegt, ist bewusst **kein Bild**, sondern der Lebenszyklus: Swapchain
> angelegt, N Frames präsentiert, sauber abgeräumt, keine zusätzlichen Validierungsfehler.
> Ein Pixelbeleg bräuchte ein `CaptureWindow` auf vier Backends; das steht in keinem
> Verhältnis zu einem Clear.
>
> **Ehrliche Einordnung, damit die Matrixzelle nicht mehr verspricht als sie hält:** die
> Engine-Multi-Window-API hat bis heute **keinen Aufrufer** — `createSecondaryWindow` wird
> nirgends im Baum benutzt. Abgedockte Editor-Fenster laufen über ImGui-Viewports, die auf
> D3D11 und D3D12 ohnehin schon funktionieren. P1d schließt die Lücke in der Matrix und
> repariert vier echte Fehler auf dem Weg; einen sichtbaren Unterschied im Editor gibt es
> heute nicht.
>
> **Widerlegt:** die Vermutung, GLs `RenderWindow` lasse den falschen Kontext aktuell.
> `Render()` stellt den Primärkontext gleich zu Beginn wieder her (`:11276-11278`) — GL war
> hier korrekt, der Kommentar dort sagt es jetzt auch.
- P1e — `SetShadowDebug` (Cascade-Tint) und Per-Pass-GPU-Timing. Letzteres ist auf D3D12
  Timestamp-Queries pro Pass, auf Vulkan `vkCmdWriteTimestamp` — Whole-Frame-Timing steht
  überall schon, die Slot-Verwaltung ist die eigentliche Arbeit.

> **Stand 2026-08-22: P1e zur Hälfte erledigt. Per-Pass-Timing steht, `SetShadowDebug`
> bleibt hart blockiert.**
>
> **Per-Pass-GPU-Timing** auf D3D11, D3D12 und Vulkan, jeweils als Erweiterung des
> vorhandenen Whole-Frame-Rings, nicht als zweiter Mechanismus daneben. Gemeinsam neu:
> `HE::kMaxTimedPasses` (16) neben `kGpuTimerRing`, und die Modus-Whitelist in
> `ProfilerPanel.cpp` kennt jetzt `d3d11-timer`, `d3d12-timer` und `vulkan-timer` — die
> zu vergessen wäre der stillste Fehler dieser Phase gewesen: die Zahlen blieben richtig,
> würden aber als nicht-additive „per-encoder spans" gelabelt und nicht mehr summiert.
>
> **Abnahme über `HE_DUMP_PROFILE=<Frames>`** (neu, `Application.cpp`) plus
> `scripts/he_profile_ab.py`. Der Zeuge nimmt nach einem Vorlauf N Frames auf und schreibt
> die normale `profile_<stamp>.json`. Verglichen werden **Namen, Modus und Plausibilität —
> nicht die Zeiten**, die zwischen Backends und GPUs nichts miteinander zu tun haben.
>
> Der Zeuge unterschied **vor** der Änderung sauber: OpenGL lieferte `gl-timer` mit Pässen
> in 20/20 Frames, die drei Ziele `whole-frame` und **gar kein** `gpu`-Array. Danach:
>
> | Backend | Modus | Frames mit Pässen | Ring-Vorlauf |
> |---|---|:--:|:--:|
> | OpenGL | `gl-timer` | 20/20 | 0 |
> | D3D11 | `d3d11-timer` | 20/20 | 0 |
> | D3D12 | `d3d12-timer` | 17/17 | 3 |
> | Vulkan | `vulkan-timer` | 16/16 | 4 |
>
> Der Vorlauf ist kein Fehler: Ergebnisse werden `kGpuTimerRing` Frames später abgeholt,
> damit das Auslesen die Pipeline nicht anhält. OpenGL und D3D11 umgehen ihn mit einem
> Same-Frame-Reap im Detail-Modus, D3D12 und Vulkan haben kein Gegenstück — bewusst nicht
> nachgerüstet, weil das den bestehenden Whole-Frame-Pfad mit verändert hätte.
>
> Das Gate ist auf allen dreien `EngineProfiler::isRecording()`, **einmal pro Frame
> gelatcht** (genau wie GL) — ein Umschalten mitten im Frame kann also kein `Begin` ohne
> sein `End` hinterlassen. Bei geschlossenem Profiler wird keine einzige Query erzeugt.
>
> **Was diese Abnahme NICHT belegt, und das ist die wichtigere Zeile:** im
> Headless-Lauf ohne geladenes Projekt melden die drei Ziele nur `Sky+Clouds` und `UI`,
> OpenGL dagegen alle neun. Ursache ist nicht die Instrumentierung, sondern die Platzierung:
> GLs Scopes liegen **außerhalb** der Early-Returns und messen deshalb auch einen leeren
> Opaque-Pass, die der drei Ziele liegen **innerhalb**, und bei leerer Szene kehrt
> `DrawScene` vorher zurück. Ebenso existieren Bloom/Tonemap/AA nur im HDR-Viewport-Zweig,
> den ein projektloser Lauf nicht nimmt. Die vollständige Neunerliste ist damit
> **ungeprüft**; belegt ist der Mechanismus (Modus, echte Pass-Zeilen, Additivität,
> Kostenfreiheit bei geschlossenem Profiler), nicht die Deckungsgleichheit der Pass-Sätze.
> Wer das schließen will, braucht einen Zeugen, der ein Projekt mit Geometrie lädt.
>
> **`SetShadowDebug` bleibt hart blockiert**, unabhängig nachgemessen: Kaskaden-Bezeichner
> kommen in OpenGL **94-mal** und in Metal **73-mal** vor, in D3D11 und D3D12 je **einmal**
> und in Vulkan **gar nicht**. Alle drei halten `csmSplits.w` auf 0 und binden ein
> Null-/Dummy-Kaskaden-Array. Es gibt keinen Kaskadenindex zum Einfärben. Das ist keine
> Paritätslücke, sondern ein fehlendes Feature (CSM) mit offenem Umfang — es gehört in eine
> eigene Phase und nicht in P1e.
>
> Nebenbei behoben: `ProfSessionInfo.backend` las `getSelectedRHI()` und trug damit unter
> `HE_DUMP_RHI` das falsche Etikett — jede Aufnahme hätte behauptet, vom persistierten
> Backend zu stammen, und ein Backend-Vergleich hätte zwei Dateien mit demselben Namen
> verglichen. Dieselbe Falle wie in `EditorApplication::OnInit`.

**Verifikation:** Editor unter jedem Backend starten, Content-Browser öffnen — Thumbnails
müssen erscheinen und wie unter GL aussehen. `HE_DUMP_THUMB` / `HE_DUMP_PREVIEW` /
`HE_DUMP_PREVIEWMESH` headless gegen die GL-Ausgabe. Terrain-Sculpting (feuert
`InvalidateMesh` pro Chunk und Frame) darf weder crashen noch Deskriptoren leaken.

### P2 — SSR verdrahten (D3D11, D3D12, Vulkan — GL optional mit)

Nach §1.3 ist die Shader-Seite fertig und bewiesen übersetzbar. Zu bauen ist nur der
C++-Rahmen.

**Scope-Hinweis:** Gefragt waren D3D und Vulkan. GL steht hier trotzdem, weil die Matrix
zeigt, dass GL SSR ebenfalls nicht hat, und weil der Cross-Compiler auch GLSL 410 liefert —
die Verdrahtung ist dieselbe. Das ist ein Vorschlag, keine stillschweigende
Scope-Erweiterung: wer nur D3D und Vulkan will, lässt P2 für GL einfach weg.

- P2a — `SetSSRSettings`-Override in allen drei Zielbackends (heute nirgends vorhanden,
  `IRenderer.h:243` bleibt der No-Op-Default).
- P2b — MRT-Reflection-Prepass: Metals Variante braucht drei Attachments (View-Position,
  oktaedrische Weltnormale, NDC-Tiefe). Heute hat D3D11 ein einzelnes Positions-RT
  (`:1311`), D3D12 genau drei RTVs im `ssaoRtvHeap` (`:2322`), Vulkan einen
  Ein-Attachment-`m_ssaoPosRenderPass`. **Das ist der eigentliche Aufwand der Phase.**
- P2c — Trace/Blur/RoughMix/Composite-Pässe an die vorhandenen
  `MaterialShaderLibrary::ssr*(backend)`-Accessoren hängen.
- P2d — `heLight.ssr` im Lighting-ABI-Fill schreiben, damit der bereits einkompilierte
  Forward-Konsument (`heSSRFwd`) aufhört, wegkonstantengefaltet zu werden.

**Verifikation:** `HE_DUMP_SSR=1`, `HE_DUMP_SSRTEST`, `HE_DUMP_SSRTESTWALL`,
`HE_DUMP_SSRQUALITY`, `HE_DUMP_SSRTESTROUGH` — die Testszenen existieren bereits und sind
auf Metal eingefahren. Pro Backend gegen den Metal-Referenzshot.

> **Stand 2026-08-22: P2 auf Vulkan verdrahtet — aber NICHT bildlich belegt. D3D blockiert.**
>
> **D3D11/D3D12 stehen still**, aus dem in §1.3 nachgetragenen Grund: die erzeugte HLSL
> übersteht `fxc /T ps_5_0` nicht. Das ist keine Verdrahtung mehr, sondern hängt am
> HLSL-Resource-Remap, den auch A4 braucht.
>
> **Auf Vulkan steht die Kette:** `SetSSRSettings`, `supportsScreenSpaceReflections` (nur
> wenn die Pipelines wirklich gebaut wurden), ein MRT-Reflection-Prepass, Trace → Blur →
> RoughMix an den vorhandenen `MaterialShaderLibrary::ssr*(SpirV)`-Accessoren, und
> `heLight.ssr` im Lighting-ABI-Fill. Vier neue Profiler-Zeilen (`SSRPrepass`, `SSRTrace`,
> `SSRBlur`, `SSRMix`). Ausgeschaltet: zwei bool-Tests pro Frame, kein Render-Pass, keine
> Barriere, kein Copy.
>
> **Zwei Abweichungen vom Plan, beide begründet:**
> * Der Prepass hat **zwei** Attachments, nicht Metals drei. `kSSRTraceFS` liest genau
>   `heGB1` und `heGBDepth`; Metals dritte (View-Position) existiert nur, weil Metal den
>   Pass in seinen SSAO-Positions-Pass faltet. Hier wäre sie ein Vollbild-RGBA16F, das
>   niemand sampelt.
> * **`ssrComposite` ist nicht verdrahtbar** und das liegt nicht am Shader. Er braucht mit
>   Präambel 21 Deskriptoren, darunter `heGB0/1/2` — einen echten G-Buffer — und
>   `heSkyEnv` als vorgefilterte Sky-Cubemap. Vulkan hat weder das eine (P4) noch das
>   andere (P3). Er ist zudem strukturell ein Deferred-Pass: er *addiert* den
>   Specular-Term nach, den der Resolve ausgelassen hat, und ein Forward-Shader hat seinen
>   eigenen schon angewandt.
>
> **Was fehlt, und das ist die entscheidende Zeile:** die vorhandenen SSR-Testszenen
> **belegen auf Vulkan nichts**. Gemessen: `HE_DUMP_SSRTEST` mit `HE_DUMP_SSR=1` und `=0`
> liefert ein **byte-gleiches** Bild. Die Szenen bauen einfache `MaterialAsset`s, die durch
> `shaders/scene.frag` laufen — und der hat keinen Reflexions-Eingang (sein eigener
> Drift-Kopf sagt das, die Bindings enden bei 7). Metal funktioniert dort, weil Metal
> `ssrTex` in seinen **eigenen** eingebauten Fragment-Shader gelegt hat. Der Vulkan-Weg
> erreicht damit heute nur Node-Graph-Materialien über `heSSRFwd`.
> Verdrahtet und fehlerfrei ist also belegt; **dass Pixel anders aussehen, nicht.** Wer das
> schließen will, braucht entweder eine Graph-Material-Spiegelszene als Zeugen oder einen
> Reflexions-Eingang in `scene.frag` — Letzteres ist die eigentliche Metal-Parität.
>
> **Nebenbefund, und er betrifft die vorherige Phase:** Vulkan meldete in den
> Headless-Läufen **17** Validierungsfehler, die ich zuvor übersehen hatte — meine
> P1-Abnahmen haben nur `has not been destroyed` gezählt (Leaks, korrekt null) und daraus
> fälschlich „sauber" gelesen. Ursache war `m_thumbImage` aus P1b: der Widget-Kachel-Pfad
> leiht sich `m_uiViewportRP`, dessen `finalLayout` `SHADER_READ_ONLY_OPTIMAL` ist, aber
> das Bild wurde ohne `VK_IMAGE_USAGE_SAMPLED_BIT` angelegt. Reiner Vertragsbruch, kein
> Pixelfehler — deshalb waren alle acht Kacheln trotzdem byte-gleich. Eine Zeile behebt es.
> **Jetzt: 0 Validierungsfehler auf allen vier Backends.**

### P3 — Himmel und Wetter nachziehen

Größter reiner Shader-Portierungsblock — und der am besten vorbereitete: `sky.frag:1-50`
listet **selbst** auf, was gegenüber GL fehlt, und GL ist ausdrücklich die Referenz
(`sky.frag:6-8`; Metal spiegelt GL, nicht umgekehrt).

Wichtig: die Audit-1a-Entscheidung dort lautete „DOCUMENT the drift, do not port". Diese
Phase kehrt sie um. Das ist eine bewusste Änderung, keine Übersehung.

- P3a — Atmosphäre: `atmoScatter`/`atmoRaySphere` (Rayleigh/Mie/Ozon) ersetzt den alten
  Gradienten `skyColor()`. **Größter optischer Einzelunterschied.** Laut Abgleich kein
  infrastruktureller Blocker in allen drei Backends — Pass, PSO/Pipeline, CB/UBO und die
  gemeinsame `BuildSkyFrameParams`-Übersetzung stehen. Reine Shader-Portierung aus
  `OpenGLRenderer.cpp:2465-2526`. Da `kSkyFuncHLSL` zwischen D3D11 und D3D12 geteilt ist,
  zahlt sich die HLSL-Portierung doppelt aus.
- P3b — CB/UBO-Erweiterungen. Die tatsächliche Fehlerquelle dieser Phase: `SkyFrameParams`
  ist 336 Bytes, der Vulkan-Block 160 (`sky.frag:35-45`). `SkyUBOData`
  (`VulkanRenderer.cpp:61-69`) und der std140-Block müssen **offset-synchron** wachsen.
  D3D12 hat einige Slots bereits, D3D11 nicht — die Erweiterung ist von D3D12 nach D3D11
  1:1 abschreibbar.
- P3c — Mond (Phase, Corona, prozedurale Oberfläche), Nachthimmel v2→v3.4 (3-Farb-Nebel,
  Milchstraßen-Rift, 3D-Aurora, Meteore).
- P3d — Wolken: `applyClouds` auf `out float outT` erweitern (Transmittanz wird intern
  bereits berechnet), dann 3D-Volumen-Raymarch und die Zusatz-Layer (Cirrus,
  Kondensstreifen, Regenbogen, God-Rays, Sonnenscheibe/Glare).
- P3e — Low-Res-Cloud-Pass (Viertelauflösung + Reprojektion). Braucht ein zusätzliches
  RGBA16F-Target samt Sicht-Paar; ein HDR-Target ist **nicht** der Blocker, das existiert.
- P3f — GPU-Wetter-Partikel. **Compute ist nicht der Blocker** — alle drei haben
  nachweislich Compute (D3D11 `:1678-1680`/`:1986`, D3D12 `:4109-4113`/`:4643`, Vulkan
  `:5762-5763`). Es fehlen Partikelpool-Buffer, Simulations-Kernel und der
  Vertex-Pull-Billboard-Pfad.

**Verifikation:** Die `HE_DUMP_*`-Familie deckt diesen Bereich am dichtesten ab —
`HE_DUMP_SKYTEST`, `HE_DUMP_TOD`/`HE_DUMP_SKYTIME`, `HE_DUMP_CLOUDMODE`,
`HE_DUMP_LOWRESCLOUDS`, `HE_DUMP_NEBULA`, `HE_DUMP_AURORA`, `HE_DUMP_MOONPHASE`,
`HE_DUMP_GODRAYS`, `HE_DUMP_CIRRUS`, `HE_DUMP_CONTRAILS`, `HE_DUMP_METEORS`,
`HE_DUMP_RAIN`. Tageszeit-Sweep pro Backend gegen GL. Nach P3b zusätzlich ein UBO-Offset-Test:
ein Feld setzen und prüfen, dass **genau** das im Shader ankommt.

### P4 — Deferred-Pfad (D3D12, Vulkan; D3D11 nach Maßgabe)

Erst hier, weil es das erste ist, das echte neue Infrastruktur braucht: heute wertet **kein**
Zielbackend `SetRenderPath` aus (`grep -c RenderPath` = 0 in allen dreien).

- P4a — MRT-G-Buffer-Target (4 Attachments + Tiefe) und der eigene Extract/Cull/Sort-Zweig.
- P4b — Zwei-Pass-Resolve. Der Shader dafür wird **schon heute** für jedes Backend übersetzt
  (§1.4) — nur der Pass fehlt.
- P4c — G-Buffer-Variante für Node-Graph-Materialien inkl. Forward-Routing-Fallback.
- P4d — `SetRenderPath`-Gate scharf schalten.
- P4e — *Optional, danach:* die Clustered-Sperre in `compileResolveVariant:1030` für
  D3D11/D3D12/Vulkan öffnen (die GL-4.1-Begründung trägt für sie nicht) und den
  CPU-Scatter-Build portieren.

**Verifikation:** `HE_DUMP_RENDERPATH` + `HE_DUMP_GBUFFER` pro Backend; Forward- und
Deferred-Shot derselben Szene müssen einander entsprechen und beide dem GL-Forward-Shot.

### P5 — GI-Reflections (D3D12, Vulkan)

Zuletzt, weil hier als einzigem Block **kein Shader existiert**, den man portieren könnte:
`HlslSources.h` hat im GI-Bereich (`:405-875`) kein `kGiRefl*`, und
`src/HE_Rendering/shaders/` kein `gi_refl*`. GL hat einen Kernel — aber mit ausdrücklich
dokumentierten Einschränkungen (`gi-reflections-plan.md:316-327`: kein Bounce-Loop, kein SSR
in der Kaskade, kein Sky-Cubemap-Sampling, Terrain und skinned Meshes nicht in der BVH).

Voraussetzung, die vor dem Kernel steht: `GiInst` (`HlslSources.h:455`,
`gi_probe.comp:19`) trägt heute weder Landschaftsindex noch Metallic/Roughness pro Instanz.
Ohne diese Felder sind Painted-Landscape-Tabelle und Multi-Bounce-Spiegelkette nicht
darstellbar. Das ist der eigentliche erste Schritt, nicht der Kernel.

D3D11 nimmt an dieser Phase nicht teil (§3).

**Verifikation:** `HE_DUMP_GIREFL`, `HE_DUMP_GIREFLTEST`, `HE_DUMP_GIREFLQUALITY`,
`HE_DUMP_GIREFLROUGH`, `HE_DUMP_GIREFLBLUR`, `HE_DUMP_GIREFLBOUNCES`,
`HE_DUMP_GIREFLLANDSCAPE` gegen die Metal-Referenz.

---

## 6. Veraltete Dokumentation (nicht stillschweigend korrigiert)

Beim Abgleich sind drei Stellen aufgefallen, die heute etwas Falsches behaupten. Sie sind
hier festgehalten statt einfach umgeschrieben, weil das Absicht sein könnte:

1. **`CopilotDocs/windows-gpu-verification-checklist.md`, Abschnitt A4** — „Aktuell rendert
   jedes über den Material-Node-Graph gebaute Material auf D3D/Vulkan gar nicht wie gebaut
   (die Renderer rufen `MaterialShaderLibrary` nie auf — nur GL+Metal tun das)."
   Das stimmt nicht mehr: D3D11 (`:2458`), D3D12 (`:5414`) und Vulkan (`:1977`) rufen die
   Library alle auf, und CMake linkt `he_materialshader` in jedes Backend. A4 ist
   implementiert und wartet nur noch auf die Hardware-Abnahme.

2. **`CopilotDocs/ROADMAP.md:20`** — „Backends (GL 4.1/4.6, Metal, Vulkan*, D3D11/12*)
   🟡 Clear + ImGui-Overlay, keine Draw-Calls". Stand Juni 2026 und lange überholt; alle
   fünf Backends zeichnen Geometrie.

3. **`HlslSources.h:627-628`** — der Kommentar behauptet FXC/SM5.0-Sicherheit für genau die
   Konstruktion, die FXC ablehnt (§1.1). Wird in P0 mitkorrigiert.

Ebenfalls erwähnenswert, ohne Fehler zu sein: `docs/deferred-renderer-plan.md:34-48` ist ein
überholter Vorgänger-Statusblock, der `:8-32` desselben Dokuments widerspricht. Maßgeblich
ist `:8-32`.

---

## 7. Was dieser Plan nicht leistet

Er ist **blind gegenüber der tatsächlichen Optik**. Es gibt keine GPU-Paritätstests — die
Suite deckt nur die CPU-Seite ab (`test_rendergraph`, `test_gi_bvh`, `test_culling`), und die
CI verifiziert ausschließlich, dass kompiliert wird. Jede Phase hier endet deshalb mit einem
`HE_DUMP_*`-A/B gegen OpenGL bzw. Metal auf echter Hardware; „kompiliert grün" ist in diesem
Repo nachweislich kein Beleg dafür, dass ein Feature überhaupt läuft — §1.1 ist der Beweis.
