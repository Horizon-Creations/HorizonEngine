# Landscape im Weltmaßstab: das Zeichnen auf OpenGL, Metal, D3D11, D3D12 und Vulkan (Schritt 2, Teil B)

Stand 02.11.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 2 von 8 ("Quadtree-Renderer, Vulkan zuerst").
Vorher: `landscape-world-scale-renderer-2026-10-31.md` (Teil A: Quadtree, Gitter, Atlas, Laufzeit, Shader-Quelle).

**Teil B ist für OpenGL und Metal (Mac), D3D11, D3D12 und seit 02.11.2026 (Abend) auch Vulkan (alle auf NN-WS03, RTX 4070) gebaut, ausgeführt und auf der echten GPU abgenommen** (§9/§9a): die Backends zeichnen die Patches des Quadtrees, im echten Renderer (Forward und Deferred) und in einem eigenen Prüfwerkzeug. "Vulkan zuerst" im Schritt-Titel war eine Reihenfolge, kein Verzicht auf die anderen Backends: der Datenpfad (§2) ist für alle derselbe.
**Schritt 1520 ist damit für OpenGL, Metal, D3D11, D3D12 und Vulkan geschlossen** (Software-Backend zeichnet die Landschaft nicht, nicht Teil dieses Werkzeugs) — Zeit-, Bild- und Tiefenzahlen liegen für Vulkan jetzt vor (§9a), CPU/GPU-Budget eingehalten.

## 1. Was gebaut ist

| Datei | Aufgabe |
|---|---|
| `HE_Core/Renderer/LandscapeRenderFrame.h` | der Frame in Typen, die die Backends sehen (sie linken nur `HorizonCore`): Patches, **schon gepackte** Bild-Bytes, Tabellen-Änderungen, die statischen Bilder; `LandscapeProvider` |
| `IRenderer::SetLandscapeProvider` | nicht virtuell (wie `SetWaterRipples`: kein Verschieben der vtable), Member zuletzt |
| `LandscapeRuntime::buildRenderFrame / renderProvider` | `LandscapeFrame` + die Bytes aus `landscapePackTile` / `landscapePackCoarse`; die statischen Bilder einmal je Landschaft (`generation`) |
| `OpenGLLandscape.*` (`Backends/OpenGL`) | Programm, Gitter, Atlas-Bilder, Uploads, Zeichnen; GL 4.1 Core |
| `MetalLandscape.*` (`Backends/Metal`) | dasselbe in Metal; reines C++-Header |
| `kLandscapeMSL` in `LandscapeShaderSource.h` | die MSL-Fassung der beiden Shader, von Hand, neben dem GLSL |
| `OpenGLRenderer::DrawLandscape`, `MetalRenderer::EncodeLandscape` | der Haken im Szenen-Pass |
| `GameApplication`, `EditorApplication` | `LandscapeRuntime` je Frame, Provider einmal gesetzt; im Editor im Bearbeiten-Modus um die Editor-Kamera (`update(..., cameraOverride)`) |
| `tools: landscape_gpu_check` | Prüfwerkzeug (§6) |
| Tests | `test_landscape_runtime` (Render-Frame), `test_landscape_shader` (MSL gegen GLSL) |

## 2. Der Vertrag zwischen Anwendung und Backend

Das Backend ruft, einmal je Szenen-Frame (Metal: ganz oben in `EncodeScene`, vor den Sky-only-Abkürzungen; GL: im Haken), `provider(viewProj, zeroToOne, frame)` mit der Matrix, mit der es **gleich selbst rastert** (so entfällt "die Anwendung rechnet Editor-Kamera und Seitenverhältnis nach"), und führt den Frame aus, bevor es zeichnet: **jeder gebaute Frame muss ausgeführt werden**, die Laufzeit zählt die Uploads als erledigt.
Der Ursprung (`world.origin()`) ist Sache des Providers: der Render-World trägt nur ursprungsrelative Positionen, das Backend kennt ihn nicht. Der Provider ruft die Laufzeit auf dem Hauptthread; nur der Haupt-Szenenpass fragt, nie eine Vorschau oder ein Thumbnail.

| Teil | Inhalt | Wann neu |
|---|---|---|
| `LandscapeRenderStatic` (geteilt, `shared_ptr`) | Zahlen (Kachelgröße, Weltrand, Höhenskala, Fenster, Kappen), Mip-Rechtecke, Grobwelt-Höhen (gestapelt) und -Gewichte, Ebenenfarben, das 65²-Gitter als Indizes samt Quadranten-Bereichen | wenn `generation` wechselt: alle Bilder neu, Slot-Tabelle auf 65535 |
| `uploads[]` | je Slot: `heightW x heightH` `uint16` und `weightPages` Seiten `weightW x weightH x 4` Bytes | je Frame, höchstens `maxUploadsPerFrame` (8) |
| `tableUpdates[]` | Eintrag `(z & (D-1)) * D + (x & (D-1))`, Slot oder 65535 | je Frame, in Reihenfolge, **nach** den Uploads |
| `patches[]` | Stufe, Quadranten, Grobwelt ja/nein und Raster, ganzzahlige Eck-Zelle, Ecke und Größe ursprungsrelativ, Morph-Bereich | je Frame |

Die Kacheln selbst bleiben im Store: die Bytes sind gepackt, bevor der Frame das Haus verlässt, der `TilePtr` wird danach losgelassen (Test: `use_count() == 2`).

## 3. Bilder, je Backend

| Bild | Format | OpenGL | Metal |
|---|---|---|---|
| Kachel-Höhen | R16UI, eine Schicht je Slot | `GL_TEXTURE_2D_ARRAY` (`glTexImage3D`: `glTexStorage3D` fehlt unter macOS 4.1), Einheit 21 | `texture2d_array<ushort>`, Textur 20 (Vertex) |
| Slot-Tabelle | R16UI, `D x D` | `GL_TEXTURE_2D`, 22 | `texture2d<ushort>`, 21 (Vertex) / 20 (Fragment) |
| Grobwelt-Höhen | R16UI, gestapelt | `GL_TEXTURE_2D`, 23 | `texture2d<ushort>`, 22 (Vertex) |
| Kachel-Gewichte, 3 Seiten | RGBA8 | 3 Arrays, 24 bis 26 | `texture2d_array<float>`, 21 bis 23 (Fragment) |
| Grobwelt-Gewichte, 3 Seiten | RGBA8 | 3 Texturen, 27 bis 29 | `texture2d<float>`, 24 bis 26 (Fragment) |

Alles wird mit ganzzahligen Abrufen gelesen (`texelFetch` / `read`), nichts gefiltert; in GL bekommt jede Textur trotzdem `NEAREST`, sonst ist sie unvollständig. Die Einheiten liegen **über allem, was der Renderer nutzt** (GL: ab 21, der Renderer geht bis 20; Metal: ab 20, der Renderer bis 16),
damit der Haken keine Bindung des Renderers überschreibt, die später im Pass noch gebraucht wird. Das Metal-Struct `LsGlobals` (768 Bytes, Puffer 20) und `LsPatch` (48 Bytes, Puffer 21 je Draw) haben ein C++-Spiegelbild mit `static_assert` auf die Größe.

## 4. Wo gezeichnet wird, und die Konventionen

* **Position:** im Opaque-Pass nach den Skinned-Meshes, vor dem Himmel (GL: vor `GpuTimerEndPass("Opaque")`, Metal: zwischen `"Skinned"` und `drawSky()`), mit Tiefentest und Tiefenschreiben, ohne Culling, ohne Blending. Der Himmel (`LEQUAL`, z = 1, kein Schreiben) füllt nur, was die Landschaft übrig lässt. Auf beiden Pfaden (Forward, Deferred) läuft derselbe Haken, nach dem Resolve.
* **Metal-Tiefe (Fund):** der Metal-Szenenpass rastert mit der **unkorrigierten GL-Matrix** (`u.mvp = viewProj * model`, `HE::kMetalClipFix` wird dort nicht angewandt; der G-Buffer legt die GL-NDC-Tiefe ab). Damit Landschaft, Objekte und Himmel sortiert zusammenpassen, nimmt `MetalLandscape::encode` dieselbe Matrix und der Frustum-Aufruf `zeroToOne = false`. Folge, wie für jeden Szenen-Draw: Geometrie näher als etwa das Doppelte der Near-Ebene wird abgeschnitten.
* **GL-Zustand:** `StateGuard` setzt Programm, VAO, aktive Einheit, Tiefenzustand, Culling, Blending und Unpack-Ausrichtung zurück. Metal setzt Pipeline und Tiefenzustand selbst; jeder folgende Pass des Renderers setzt seine wieder. **Shader und Pipeline entstehen erst beim ersten aktiven Frame**: eine Szene ohne Landschaft (Provider gesetzt, `frame.active` falsch) übersetzt nichts, zeichnet nichts und lässt die Sky-only-Abkürzungen des Metal-Renderers wie sie waren.
* **Beleuchtung:** erste Richtungslicht-Farbe mal Intensität, `sunDirection`, `ambient` der Render-World; Ebenen 0-3 aus dem Albedo-Array der Texturgruppe in Welt-UVs (§5a), die übrigen Ebenenfarben flach (Material-Graph später, siehe Teil-A-Dokument §8).

## 5. Bekannte Grenzen (nicht gelöst)

| Grenze | Folge |
|---|---|
| keine Schatten, kein SSAO, kein GI, kein Nebel, keine SSR | die Landschaft ist nicht im G-Buffer und nimmt an keinem dieser Passes teil; auf dem GL-Deferred-Pfad ist sie reiner Forward-Draw |
| **TAA-Geschwindigkeit** | die Landschaft schreibt keine Bewegungsvektoren: bei Kamerabewegung mit TAA Geisterbilder zu erwarten (nicht untersucht) |
| Metal-Uploads per `replaceRegion` | ein noch laufender GPU-Frame könnte einen eben neu belegten Slot lesen; betrifft nur Kacheln, die die geladene Scheibe verlassen haben, also höchstens einen Frame einer fernen Kachel (Ring-Puffer wären die Lösung) |
| je Patch ein `glUniform`-Block bzw. `setVertexBytes` | rund 180 Patches in 203 Draws im Test; **ob das reicht, ist nicht gemessen** |
| Draw-Aufrufe je Patch, kein Instancing, kein Frustum-Test auf der GPU | bewusst einfach |
| GL: 10 Texturen-Einheiten ab 21 (die zehnte ist das geliehene Albedo-Array) | ein Kontext mit weniger als 31 kombinierten Einheiten schaltet die Landschaft ab (Warnung, einmal) |
| der Renderer ruft den Provider **zweimal**, wenn Viewport-Frame und Screenshot im selben App-Frame laufen | der zweite bekommt keine Uploads mehr (sie sind beim ersten ausgeführt), die Bilder bleiben im selben Backend; ein Screenshot ohne vorherigen Viewport-Frame liefert die Uploads selbst |
| `m_landscapeBound` (Spiel, Editor) ist einmalig | ein neu erzeugter Renderer müsste den Provider neu setzen, und die Laufzeit `reset()` (sonst hält sie Uploads für Bilder, die der neue Renderer nicht hat) |
| Foliage und Wasser (Schritt 3b/3c) hängen **seit 02.11.2026** an Spiel und Editor (§5b); Physik und Navigation (`setPhysics`, `setNavigation`) weiterhin **nicht** | Spiel und Editor rufen `update` (Store, Atlas) und `LandscapeWorldProxies::sync`; `setPhysics` und `setNavigation` sind Aufrufer-Sache und noch nicht verdrahtet |

## 5a. Material mit Welt-UVs (02.11.2026)

Die Ebenen 0-3 der zwölf Gewichtskanäle (Grass, Dirt, Rock, Snow der Gruppe) lesen die ersten vier Slices des **Albedo-Arrays der Texturgruppe** (`AutoLandscapeGroup::arrayPaths[0]`), kachelnd in **absoluten Weltmetern**: `uv = x,z / textureMeters`. Die übrigen acht Kanäle und jede Ebene ohne Array bleiben flach.

| Stelle | Inhalt |
|---|---|
| `LandscapeWorldComponent::textureGroup` (0 Mountains, 1 Forest, 2 Grassland, 3 Desert, -1 flach; Standard 0) und `textureMeters` (8) | gelesen, wenn die Landschaft beginnt (ein Wechsel danach wirkt erst mit einem neuen Pack); **nicht serialisiert**, wie die Komponente |
| `LandscapeRenderStatic::albedoArrayPath`, `textureMeters` | der Vertrag; leer = flach |
| Renderer | löst den Pfad wie ein Material-Array auf (`ResolveGraphTexture(.., array = true)`); fehlt das Array (nicht geladen, nicht heruntergeladen), kommt das weiße Array zurück und die Ebene bleibt **flach**, bis es da ist |
| Shader | `uTex = (1/Meter, Array da, fract(origin.xz / Meter))`: der Anteil des Ursprungs wird in double auf der CPU gebildet (ein Float bei 100 km wäre 8 mm grob und flimmert), der Shader rechnet `vWorldRel.xz * uTex.x + uTex.zw`. Der Zweig ist auf einer Uniform, die Ableitungen von `texture()` bleiben definiert |

**Normal und Maske (derselbe Tag):** das Normal-Array der Gruppe stört die Flächennormale der Ebenen 0-3 (Tangentialraum wie OpenGL: Rot = +u = +x, Grün = +v = +z; die Ebenen ohne Textur tragen eine flache Normale bei), das Masken-Array liefert **AO** (Rot) und dämpft den Umgebungsanteil. Die Pfade stehen in `LandscapeRenderStatic::normalArrayPath` / `maskArrayPath`, die Uniforms heißen `uTexMaps`, `uNormalArray`, `uMaskArray` (Metal: `LsGlobals.texMaps`, Texturen 28 und 29); fehlt eines, fällt nur dieses weg. GL braucht damit 32 kombinierte Einheiten. Geprüft: ein Normal-Array, das zur Sonne (+x, +z) neigt, hellt den Boden um rund 24 Stufen auf, eines, das abneigt, dunkelt es um rund 66 Stufen ab, flach ändert nichts (GL und Metal, gleiches Vorzeichen); AO 0,5 dunkelt (123,5 auf 113,6). Mit dem echten Forest-Array in beiden echten Renderern kommt mehr Kontrast auf den Boden.
**Nicht** gebaut: Rauheit und Höhe aus der Maske, Höhenblend zwischen Ebenen, Anti-Kachel-Muster (Texture-Bombing), anisotrope Filterung (GL und Metal haben dieselbe lineare Probe, damit sie übereinstimmen), die Kanal-zu-Rolle-Zuordnung über Ebene 3 hinaus, Path und Water.

Geprüft (Mac, `landscape_gpu_check`): Szene 3 (Rampe, 100 % Ebene 0, Slice 0 = rot | gelb): alle 83 153 Bodenpixel weichen von der flachen Farbe ab, rot und gelb zu je etwa der Hälfte; dasselbe Bild mit anderem Floating Origin: 12 Pixel mehr als 16/255 daneben (absichtlich falscher Ursprungsanteil: 9 941 Pixel, der Test schlägt an); OpenGL gegen Metal 55 von 129 600 Pixeln mehr als 2/255. Mit `--arrays <EngineContent-Wurzel>` zeichnen beide echten Renderer den Boden aus dem **echten Forest-Array** (nicht mehr flaches Grün, Textur sichtbar).

## 5b. Foliage und Wasser (02.11.2026)

Foliage und Wasser brauchen keinen Backend-Code: `HE::LandscapeWorldProxies` (HorizonScene) legt sie als **versteckte Entities** in die Welt, und jedes Backend zeichnet sie über die Wege, die es schon hat.

* **Foliage:** je (Kachel, Ebene) eine Entity mit `FoliageComponent` (genau diese Ebene, `store` = der Store der Kachel, ohne `cachedInstances` und Dichtemaske), Position = `centre - world.origin()`. Der Extraktor nimmt sie durch dieselbe Bucket-Logik wie bei der einzelnen Landschaft.
* **Wasser:** je Fläche der Region eine Entity mit `MeshComponent` (das Netz gehört der Entity) und dem Wassermaterial, Position = `centre + (center.x, level, center.y) - origin`; ohne Schatten.
* `LandscapeProxyComponent` markiert sie; `isTerrainGenerated` kennt sie (nie gespeichert, nie im Outliner, nie gepickt, nie repliziert). Aufruf: `sync()` einmal je Frame nach `LandscapeRuntime::update` (Spiel: `updateLandscape`, Editor: im Landschaftsblock). Der Ursprung wandert, ohne dass etwas neu gebaut wird.
* **Nicht dabei:** tessellierte Wasserfläche (es gibt das einfache Wasser), Wellen für Auftrieb und die Unterwasserkamera (die lesen ein `water::Field` eines `TerrainComponent`; `waterSurfaceAt` der Laufzeit antwortet), Wasser außerhalb der Region, Dichtemasken der Foliage.
* Test: `test_landscape_world_proxies.cpp` (echte Kacheln, echte Streuung, echte Wasserregion). **Nicht geschaut:** wie es gerendert aussieht; es ist als Entities und Daten geprüft, nicht als Bild.

## 5c. Messungen (02.11.2026, MacBook Air M5, am Netzteil)

`landscape_gpu_check --measure`: eine Kamera fliegt mit 40 m/s über eine 12 km große prozedurale Welt (24 x 24 Kacheln, Standard-Atlas 836 Plätze), 2560 x 1440 offscreen, 600 Bilder, wie ein 60-fps-Spiel getaktet; 407 Kacheln resident, 189 Patches je Bild (höchstens 192), 0,03 Kachel-Uploads je Bild. Der Boden ist flach schattiert (kein Texturlesen).

| Was | Metal p50 / p95 / max | OpenGL p50 / p95 / max |
|---|---|---|
| Laufzeit `updateAt` (CPU) | 0,030 / 0,065 / 0,206 ms | 0,059 / 0,103 / 0,487 ms |
| Frame bauen (CPU) | 0,025 / 0,046 / 2,385 ms | 0,048 / 0,095 / 2,955 ms |
| Draws aufzeichnen (CPU) | 0,031 / 0,059 / 0,946 ms | **0,985 / 1,948 / 2,185 ms** |
| **Landschaft CPU gesamt** | **0,088 / 0,165 / 3,369 ms** | **1,097 / 2,133 / 5,092 ms** |
| Landschafts-Pass GPU | **4,87 / 8,01 / 9,38 ms** (GPU-Zeit des Command Buffers) | 6,99 / 10,20 / 10,95 ms (Wanduhr bis `glFinish`, Obergrenze; Apple-GL hat keine Zeitabfragen) |

Foliage und Wasser (CPU, 8 x 8 Kacheln mit See, zwei Pflanzenebenen, 20 m/s, 600 Bilder; 24 Foliage-Entities, 1 Wasser-Entity, 37 742 Pflanzen gehalten): `updateAt` mit Foliage und Wasser 0,036 / 0,058 / 0,131 ms, `LandscapeWorldProxies::sync` 0,021 / 0,035 / 0,111 ms.

**Lesart gegen Plan §4 (CPU < 0,5 ms, GPU < 1,5 ms auf der 4070 bei 1440p):**
* **CPU:** Metal ist weit darunter, **OpenGL liegt mit 1,1 ms über dem Ziel**: ein `glUniform`-Satz und ein Draw je Patch (rund 200 Draws). Das ist der Preis der Patch-Uniforms; ein Weg darunter wäre ein Uniform-Puffer je Frame mit Instanzdaten oder Multi-Draw. Nicht gebaut.
* **GPU:** 4,9 ms auf dem Integrierten der M5 bei 1440p ist **nicht vergleichbar** mit der Abnahme auf der 4070 und kein Urteil darüber. Der Pass schreibt ein 1440p-HDR-Ziel samt Tiefe und liest je Pixel zwölf Gewichte; wie viel davon Bandbreite ist, wurde nicht getrennt. Die Abnahme bleibt NN-WS03.
* Die Maxima (3 ms) sind Bilder mit Atlas-Uploads.
* **Nicht gemessen:** GPU-Speicher im Betrieb, Textur-Pfad (Albedo, Normal, Mask) in der Zeit, Last durch Foliage und Wasser auf der GPU, TAA, Windows.

## 6. Das Prüfwerkzeug `landscape_gpu_check`

Kein Test (braucht Fenstersystem und GPU, nicht in der CI; `src/HE_Tools/src/LandscapeGpuCheck`). Zwei Schichten je Backend:

1. **Offscreen**: die Backend-Klasse (`OpenGLLandscape` im eigenen GL-Kontext, `MetalLandscape` in eigenem Pass, RGBA16F + Depth32F wie der Renderer) zeichnet eine **Rampe** `y = 600 + 0.1 · (x − 3072)` über 32 x 32 Kacheln, alles Farbe der Ebene 0. Eine Ebene wird auf jeder Stufe exakt wiedergegeben, also ist die Höhe an jedem Pixel bekannt. Je Pixel: Strahl gegen Ebene, dann
   *Loch/Zuviel* (gezeichnet, wo die Ebene ist, bis 5 km), *Tiefe* (Abstand zur gezeichneten Fläche gegen Abstand zur Ebene, Toleranz 0,35 m + 3e-4 · Abstand) und *Farbe* (Ebenenfarbe mal Ambient + Sonne · n·l, 4/255, bis 5 km, also auch über die Grobwelt-Gewichte).
   Danach springt die Kamera 10 km (der Atlas fasst rund 190 der 1024 Kacheln: Slots werden frei und wiederverwendet, `releases > 0`), und dieselben Prüfungen laufen auf denselben Bildern. Eine zweite Szene (die fBm-Hügel der Engine) wird nur auf "Boden da" geprüft und als PNG geschrieben.
2. **Engine**: der **echte Renderer** (`RendererFactory`, verstecktes Fenster, Szenen-Pass des Backends, **Forward und Deferred**) zeichnet dieselbe Welt über `SetLandscapeProvider` und `RenderSceneImage`; geprüft wird, ob dort Boden ist, wo die Ebene ist (>= 97 % grün), und nichts Grünes darüber (<= 3 %). Das Bild ist tone-gemappt, also keine Farbgenauigkeit.

3. **OpenGL gegen Metal**: beide Backends zeichnen dieselben Frames; mehr als 0,1 % der Pixel mehr als 2/255 auseinander ist ein Unterschied zwischen den beiden Portierungen, kein Rundungsfehler. Das ist die einzige Stelle, an der ein Morph- oder Abruf-Fehler in nur **einer** der beiden Fassungen auffällt: eine Ebene morpht auf sich selbst, die Hügel sind es, die zählen.

Ergebnis dieses Laufs (Mac, Apple-Silikon), **gezählt, nicht gemessen**:

| | OpenGL | Metal |
|---|---|---|
| Rampe, erste Position | 180 Patches in 203 Draws; Löcher 0, Zuviel 0; Tiefe schlecht 6 von 80 176 px; Farbe schlecht 0 von 81 143 (größte Abweichung 1/255) | gleiche Patches und Draws; Löcher 0, Zuviel 0; Tiefe schlecht 0 von 80 176; Farbe schlecht 0 |
| Rampe, nach 10 km | 175 Patches in 196 Draws; Löcher 1; Tiefe schlecht 4 von 62 608; Farbe 0 | wie links, Tiefe schlecht 0 |
| Atlas | 382 Uploads = 382 der Laufzeit; 191 Slots freigegeben; 0 abgelehnt | gleich |
| Echter Renderer, Forward / Deferred | Boden 100 % grün, Himmel 1 % grün; beide OK | gleich |
| GL gegen Metal | Rampe: 0 von 129 600 Pixeln über 2/255 verschieden; Hügel: 18 von 518 400 (9 davon über 16/255, Kanten) | |

**Absichtlich eingebaute Fehler** werden auf beiden Backends gefunden: ohne `- uOrigin.y` im Höhenshader stimmt jede Tiefe nicht mehr (größte Abweichung 2392 m); ohne die Grobwelt-Gewichte werden ferne Pixel grau (857 Pixel falsch, bis 65/255); ohne Morph nur in der MSL-Fassung weichen 14 791 von 518 400 Hügel-Pixeln ab (der Vergleich unter 3 schlägt an). Zwei weitere Fehler dieser Art hatte schon Teil A in den CPU-Tests gefunden.

## 7. Verifikation

Verifiziert (Mac, ohne Zeitmessung): `landscape_gpu_check` auf OpenGL und Metal wie in §6 (offscreen und echter Renderer, Forward und Deferred); `he_tests`: `test_landscape_*` 115 Testfälle (darunter der Render-Frame und die MSL-gegen-GLSL-Prüfung), `test_foliage*`, `test_physics*`, `test_world_scale*`, `test_scene_serializer*`, `test_cell_*`, `test_navigation*`, `test_sky*` grün;
`validate_embedded_shaders.py`: 60 GLSL-Shader übersetzen (der MSL-Teil wird hier übersprungen, weil dem Mac die Metal-Toolchain fehlt; `kLandscapeMSL` ist dafür ergänzt, und die Laufzeit-Übersetzung per `newLibraryWithSource` hat sie bestanden). `HorizonGame` linkt; `EditorApplication.cpp` übersetzt.
**Nicht verifiziert:** jede Zeit; GPU-Speicher im Betrieb (die Rechnung steht im Teil-A-Dokument: rund 494 KB je Slot); Verhalten mit TAA in Bewegung; ein Editor- oder Spiel-Lauf mit einer echten Szene (der Engine-Modus des Werkzeugs nimmt die Renderer-Schnittstelle, nicht die Anwendung); Windows- und Linux-Lauf des Prüfwerkzeugs (es wird dort nur gebaut); Intel-Macs (Metal `Managed`-Speicher ist vorgesehen, nicht gelaufen);
Vulkan: nichts davon ausgeführt (§9). (D3D11 und D3D12: §8.)

## 8. D3D11 und D3D12 (Windows)

Ein Shader für beide: `kLandscapeHLSL` in `LandscapeShaderSource.h`, von Hand aus der MSL (SM 5.0, fxc). Vertex-ID statt Vertex-Puffer (`SV_VertexID` ist im Indexed-Draw der Indexwert: `(id % 65, id / 65)`, kein Input-Layout), zwei Konstantenblöcke (`LsGlobals` b12 = 784 Bytes mit dem `tex`-Block der Welt-UVs, `LsPatch` b13 = 48 Bytes; `patch` ist in HLSL ein Schlüsselwort, das Mitglied heißt `lsPatch`), neun Bilder `t32`-`t40` nur mit `Load()` und das **geliehene Albedo-Array** der Welt-UVs (§5a) auf `t41` mit einem Sampler `s13` (linear, wiederholend, mit Mips; D3D12: statischer Sampler, D3D11: Sampler-Zustand der Klasse). Das Array wird **bedingungslos** am Anfang des Pixel-Shaders gelesen (Ableitungen brauchen einheitlichen Fluss, vor dem `discard`), `tex.y` wählt zwischen Array und flachen Farben; fehlt es, ist der SRV null. Der Renderer löst `albedoArrayPath` wie GL mit `resolveGraphTexture(.., array = true)` auf. Die SRVs liegen über `t31`, weil der D3D11-Renderer und die Graph-Materialien `t0`-`t26` belegen und nach dem Landscape-Draw nicht neu binden. Das C++-Spiegelbild beider Blöcke samt `static_assert` und die Füllfunktionen stehen in `D3D_Shared/LandscapeD3D.h` (geteilt).

| Datei | Aufgabe |
|---|---|
| `D3D11Landscape.{h,cpp}` | Shader, Zustände, Index-Puffer, Atlas-Bilder (`UpdateSubresource` je Slot), Slot-Tabelle als CPU-Spiegel (ein `UpdateSubresource` je geänderter Tabelle); `draw` setzt alles, was es anfasst (Shader, IA, Raster, Tiefe, Blend, b12-b13, t32-t41, Sampler s13), über einen `StateGuard` zurück |
| `D3D12Landscape.{h,cpp}` | Root-Signatur (b12 Root-CBV, b13 zwölf Root-Konstanten, Tabelle t32-t41, statischer Sampler s13), eigener Descriptor-Heap (ein Ring von 8 Tabellen, der Albedo-Descriptor wird je Draw neu geschrieben), ein PSO je Farbformat (RGBA16F / RGBA8, Tiefe D32F), Upload-Ring (4 Puffer, Barrieren SRV -> COPY_DEST -> SRV), CBV-Ring (32 Slots); Wechsel von Bildern oder Wachsen des Rings ruft `waitIdle` (= `waitForAllFrames`) |
| `D3D11Renderer.cpp`, `D3D12Renderer.cpp` | Haken (siehe unten) |
| `tools: D3D11Backend.cpp, D3D12Backend.cpp` | Offscreen-Backends des Prüfwerkzeugs; `main.cpp` vergleicht jedes Paar (OpenGL ist die Referenz, dazu D3D11 gegen D3D12) |

**Haken, anders als auf GL/Metal:** `DrawScene` zeichnet den Himmel **zuerst** und verlässt die Funktion früh, wenn die Szene keine Objekte hat. Eine Szene nur aus Landschaft würde nie in den Render-Graph kommen. Darum: der Provider wird **direkt nach dem Himmel** gefragt (vor beiden frühen Rückkehren, mit der Matrix, die auch die Objekte bekommen), die Uploads werden sofort ausgeführt (jeder gebaute Frame muss ausgeführt werden), gezeichnet wird im Opaque-Pass nach den Skinned-Meshes und vor TAA-Geschwindigkeit/Decals (Forward und Deferred, nach dem G-Buffer-Resolve), und auf den beiden frühen Rückkehren sofort. D3D12 baut nach dem Draw den Szenen-Zustand wieder auf (`restoreSceneRoot()` + `scenePso`, wie der Skinned-Pass).

**Tiefe und Matrix:** der Szenen-Pass beider D3D-Backends rastert wie Metal die **unkorrigierte GL-Matrix** (`kD3DClipFix` wird dort nicht angewandt, nur in der Welt-Vorschau); die Tiefe im Puffer ist die GL-NDC-Tiefe der nicht abgeschnittenen Hälfte. Der Haken reicht dieselbe Matrix und `zeroToOne = false` durch, die Prüfwerkzeug-Backends melden `depthIsNdc() = true`. Folge wie auf Metal: Geometrie näher als etwa das Doppelte der Near-Ebene wird abgeschnitten.

**Ergebnis** (NN-WS03, Release, RTX 4070 für alle drei APIs, gezählt, nicht gemessen):

| | OpenGL | D3D11 | D3D12 |
|---|---|---|---|
| Rampe, erste Position | 180 Patches in 203 Draws; Löcher 0, Zuviel 0; Tiefe schlecht 3 von 80 176; Farbe 0 (max. 1/255) | gleich, Tiefe schlecht **0** | gleich, Tiefe schlecht **0** |
| Rampe, nach 10 km | 175 in 196; Löcher 1; Tiefe schlecht 2 von 62 608 | gleich, Tiefe schlecht 0 | gleich, Tiefe schlecht 0 |
| Atlas | 382 Uploads = 382 der Laufzeit | gleich | gleich |
| Echter Renderer, Forward / Deferred | Boden 100 %, Himmel < 1 % grün | gleich | gleich |
| gegen OpenGL | | Rampe 0 von 129 600, Hügel 5 von 518 400 Pixeln über 2/255 | dasselbe |
| Welt-UV-Szene (§5a, Slice 0 = rot, 1 = gelb) | 83 153 von 83 153 Bodenpixeln weichen von Flach ab (rot 40 380, gelb 42 773); anderer Ursprung: 10 Pixel über 16/255 daneben | rot 40 382, gelb 42 771; Ursprung: 12 Pixel | dasselbe |
| gegen OpenGL, Welt-UV | | 22 von 129 600 Pixeln über 2/255 (6 über 16/255) | dasselbe |
| D3D12 gegen D3D11 | | | Rampe 0, Hügel 0, Welt-UV 0 Pixel über 2/255 |

D3D12 mit Debug-Layer und `HE_GPU_DEBUG=1` (Werkzeug und echter Renderer, Forward und Deferred): keine Meldung. Dafür liest `RenderSceneImage` die InfoQueue jetzt auch (`drainGpuDebugMessages`, vorher nur `Render()`). Auf D3D11 gibt es nur Bildbelege (kein Debug-Layer im Renderer).
**Absichtlich eingebaute Fehler** werden auf beiden Backends gefunden (HLSL geändert, nur diese Quelle, dann zurück): ohne `- origin.y` stimmt jede Tiefe nicht mehr (größte Abweichung 2392 m, wie auf GL/Metal), ohne Morph weichen D3D11 und D3D12 von OpenGL bei 14 788 von 518 400 Hügel-Pixeln ab (6287 über 16/255).
`validate_embedded_shaders.py --hlsl-only` übersetzt `kLandscapeHLSL` (vs_5_0, ps_5_0; es läuft auch im CMake-Build-Schritt); `test_landscape_shader` prüft die HLSL-Fassung gegen die GLSL-Uniforms, Register und die Reihenfolge `ddx` vor `discard`.

## 9. Vulkan (02.11.2026, gebaut auf dem Mac)

**Gebaut, übersetzt und als Quelltext geprüft, damals nie ausgeführt** (seit §9a auf der 4070, seit §9b auch auf dem Mac über MoltenVK). Die Mac-Maschine hatte damals keinen Vulkan-Lader (nur `libMoltenVK`, `HE_VULKAN_ENABLED` war dort aus), und der CI-Job `vulkan-lavapipe` baut nur den Editor und zeichnet keine Landschaft.

| Datei | Aufgabe |
|---|---|
| `LandscapeShaderSource.h` | `kLandscapeVulkanVertexPrelude` / `kLandscapeVulkanFragmentPrelude` (GLSL 4.50) und `BuildLandscapeVertexGLSL450()` / `BuildLandscapeFragmentGLSL450()`: der GL-Text wird an der Marke `//#LANDSCAPE_BODY#` geschnitten, **alles darüber** (Version, lose Uniforms, `in`/`out`) ersetzt das Vorspiel, der Körper danach bleibt **Zeichen für Zeichen** der GL-Körper. Normal-/Mask-Array und Welt-UVs sind damit ohne Portierung dabei |
| `Backends/Vulkan/VulkanLandscapeLayout.h` | C++-Spiegel von `LsGlobals` (std140, 800 Bytes, Set 0 Binding 0, beide Stufen) und `LsPatch` (Push-Konstanten, 48 Bytes, Vertex), `static_assert` auf jeden Offset; `fillGlobals` / `fillPatch`; kein Vulkan-Header, he_tests darf ihn einbinden |
| `Backends/Vulkan/VulkanLandscape.{h,cpp}` | Shader nach SPIR-V (`he::shaderc`), Descriptor-Set-Layout, Pipelines je Render-Pass, Atlas-Bilder, Uploads, Zeichnen; bekommt vom Renderer nur geliehen, was es braucht (`Env`) |
| `VulkanRenderer.{h,cpp}` | `landscapeBegin` / `landscapeDraw` und die Haken in `DrawScene` (suche `landscapeBegin`) |
| `tools: VulkanBackend.cpp` | Offscreen-Backend des Prüfwerkzeugs (eigene Instanz und Gerät, kein Fenster), `--only vulkan`, im Vergleichspaar OpenGL/Vulkan, `--measure` mit Zeitstempel-Abfragen (`gpuMs`) |

**Bindungen (Set 0):** 0 = `LsGlobals`; 1 Kachel-Höhen (`usampler2DArray`, Vertex), 2 Slot-Tabelle (`usampler2D`, Vertex + Fragment), 3 Grobwelt-Höhen (Vertex), 4-6 Kachel-Gewichte, 7-9 Grobwelt-Gewichte, 10 Albedo-, 11 Normal-, 12 Mask-Array der Texturgruppe (Fragment). Die zwölf Bilder sind kombinierte Bild-Sampler; alles wird mit `texelFetch` über einen NEAREST-Sampler gelesen, nur die drei Gruppen-Arrays mit dem linearen, wiederholenden Sampler des Renderers (`m_albedoSampler`). Die Weiten der Gewichtsseiten liest der Shader aus der Textur selbst (`textureSize`); es gibt keine Bindung je Seitenzahl.

**Matrix und Tiefe (Entscheidung):** der Vulkan-Szenen-Pass rastert `kVulkanClipFix * projection * view` (y nach unten, Tiefe 0..1, bei TAA verwackelt). Die Landschaft bekommt **genau diese** Matrix (auch der Provider, mit `zeroToOne = true`); sie ändert also nichts an der Tiefenkonvention, und anders als bei D3D und Metal (§4, §8) gibt es **keine Einschränkung nahe der Near-Ebene**. `dFdy` zeigt wie in Metal nach unten, das `if (n.y < 0) n = -n` des Fragment-Shaders fängt das ab.

**Haken:** wie D3D (§8): der Provider wird in `DrawScene` **direkt nach dem Himmel** gefragt (vor beiden frühen Rückkehren), die Uploads werden sofort aufgezeichnet (jeder gebaute Frame wird ausgeführt). Gezeichnet wird im offenen Szenen-Pass: Forward nach den undurchsichtigen Draws, Deferred nach der Beleuchtungs-Auflösung und dem Forward-Replay (im Pass `m_hdrLoadRP`, der zu `m_postFxSceneRP` kompatibel ist, also reicht die eine HDR-Pipeline), und auf den beiden frühen Rückkehren sofort (Szene ohne Objekte). Danach bindet der Haken Szenen-Pipeline und Set 0 wieder an (wie die Decals).

**Uploads:** ein Kopieren darf nicht im offenen Render-Pass stehen. Darum zeichnet `upload()` in den **Upload-Batch** des Renderers auf (`uploadCmd` / `stageUpload`; er wird in `submitGraphics` vor dem Frame auf derselben Queue abgeschickt). Je Bild: Barriere SHADER_READ -> TRANSFER_DST (wartet auf die Lesezugriffe **früherer** Frames), Kopie je Slot (eine Schicht), Barriere zurück. Ein Slot, der einer anderen Kachel gegeben wird, wird deshalb nie halb beschrieben gelesen (anders als bei Metal, §5). Beim Beginn einer Landschaft werden alle Bilder neu gemacht und **genullt** (`vkCmdClearColorImage`: Vulkan nullt Speicher nicht, und eine Gewichtsseite, die die Kacheln nicht haben, muss null sein); wechselt die Landschaft, wird der Batch abgeschickt und `deviceWaitIdle` gerufen, bevor die alten Bilder gehen. Atlas-Grenze: `maxImageArrayLayers` (garantiert sind nur 256, der Standard-Atlas hat 836 Plätze; NVIDIA, AMD und lavapipe bieten 2048); eine Welt mit mehr Plätzen, als das Gerät Schichten hat, wird abgelehnt (`rejected`).

**Texturgruppe:** die drei Arrays löst der Renderer mit `resolveGraphTexture(UUID{}, Pfad, array = true)` auf. Das ist dieselbe Funktion wie bei den Material-Texturen und ruft `HE::forgetGraphTextureMisses` selbst (Fehlschläge werden je `contentEpoch()` vergessen), ein Array, das nach dem ersten Frame ankommt, wird also gefunden (kein zweiter Cache, keine Nachfrage je Frame). Fehlt eines, bindet der Haken das weiße Array des Renderers (nie ein Null-Descriptor) und der Shader liest es nicht (`uTex.y` / `uTexMaps`).

**Prüfung (Mac, ohne Vulkan-Lauf):**
* `validate_embedded_shaders.py`: beide Stufen übersetzen mit `glslangValidator -V` und **linken als Paar** (`-l`); die Anzahl der GLSL-Shader ist 63 (vorher 60). `glslangValidator -V -q` meldet für `LsGlobals` 800 Bytes und die Offsets 0, 64, 80, 96, 112, 128, 144, 272 (und weiter) wie der C++-Spiegel, `LsPatch` 48 Bytes.
* `test_landscape_shader` (jetzt 8 Fälle): das Vorspiel deklariert jeden GL-Uniform-Namen (Block, Push-Konstante oder Bild mit Bindung), der Körper ist der GL-Körper, die Schnittstelle (Orte) stimmt, `LsGlobals` ist in beiden Stufen derselbe Block und **jeder std140-Offset des Textes** (im Test berechnet) **ist `offsetof` des C++-Spiegels**; die Bindungsnummern stehen je einmal fest. Negativkontrolle: ein herausgenommenes Mitglied im Vorspiel lässt den Test an sechs Stellen scheitern.
* `-fsyntax-only` (Mac, clang, `-DHE_VULKAN_ENABLED`, MoltenVK-Header) auf `VulkanRenderer.cpp`, `VulkanLandscape.cpp`, `VulkanBackend.cpp` und `main.cpp` des Werkzeugs mit `HE_CHECK_VULKAN`: ohne Fehler; Negativkontrollen (ein falsch geschriebener Name in einer Kopie) schlagen jeweils an.

**Nicht geprüft (auf dem Mac):** jeder Lauf; siehe §9a, jetzt auf der 4070 nachgeholt.

## 9a. Vulkan, Abnahme auf NN-WS03 (RTX 4070, 02.11.2026, Release-Build)

**`landscape_gpu_check` lief zum ersten Mal auf einem echten Vulkan-Treiber** (`NVIDIA GeForce RTX 4070, Vulkan 1.4.357, driver 0x9b404000`, von zwei gemeldeten Geräten das erste gewählt). Vorher fehlte das: ein CMake-Fix (`HE_Tools/CMakeLists.txt`, im `if(HE_VULKAN_ENABLED)`-Block) kopiert `${CMAKE_BINARY_DIR}/Shaders` jetzt POST_BUILD neben `landscape_gpu_check.exe`, genau wie es `HorizonEditor`s Deploy schon tat — ohne das lief die Offscreen-Prüfung (eigenes Gerät, kein `IRenderer`) durch, aber "Vulkan engine, forward/deferred" scheiterte mit "scene shaders missing — scene will not draw" (0 % Boden), weil `VulkanRenderer` `<exe-Verzeichnis>/Shaders/*.spv` von der Platte lädt und nur GL/D3D11/D3D12 ihr HLSL/GLSL zur Laufzeit aus eingebetteten Strings übersetzen.

**Offscreen (`--only vulkan`, eigenes Gerät, kein `IRenderer`):** Rampe erste Position: Loch 0, Extra 0, Tiefe 9 von 80176 px daneben, Farbe 0 daneben (schlimmster Fall 1/255). Rampe nach 10-km-Sprung: Loch 1 (von 63437), Tiefe 47 von 62608 daneben, Farbe 0 daneben. Hügel: 55,7 % Boden im Bild. Welt-UV (Normal-/Mask-Array): Textur weicht wie erwartet von der Flachfarbe ab, Normalen Richtung Sonne (+z/+x) heller, AO 0,5 dunkler — alle Tally-Prüfungen `OK`, `stats().rejected == 0` (Atlas mit 836 Plätzen passt unter `maxImageArrayLayers` der 4070).

**Im echten Renderer (`IRenderer`, Forward und Deferred):** Boden 100 % grün (80176/80176), Himmel < 1,1 % grün (496/46447 Forward, 500/46447 Deferred) — beide `OK`.

**Bild gegen OpenGL (selbe 4070, je Backend ein eigener, isolierter Prozesslauf, Pixel-Diff extern mit `System.Drawing` statt dem Werkzeug-eigenen Vergleich — Grund siehe unten):**

| Szene | Pixel > 2/255 | Anteil | Pixel > 16/255 |
|---|---|---|---|
| ramp | 0 / 129 600 | 0 % | 0 |
| hills | 2 / 518 400 | 0,0004 % | 0 |
| worlduv | 22 / 129 600 | 0,017 % | 6 |

Alle drei klar unter der 0,1-%-Grenze aus dem Werkzeug-Header — Vulkan und OpenGL zeichnen dieselbe Geometrie.

**Zeiten (`--measure`, 600 Frames bei 2560 × 1440, 40 m/s über eine 12-km-fBm-Welt, p50/p95/max/mean in ms):**

| | CPU gesamt (Runtime-Update + Frame bauen + Draws aufzeichnen) | GPU |
|---|---|---|
| **Vulkan** | 0,165 / 0,215 / 9,266 / 0,226 | **0,188 / 0,195 / 1,177 / 0,193** (echte `vkCmdWriteTimestamp`-Zeitstempel, kein Wandzeit-Ersatz — `VulkanBackend.cpp:181-186`) |
| OpenGL (Referenz, selbe 4070) | 0,225 / 0,295 / 3,835 / 0,243 | 0,609 / 2,151 / 18,369 / 0,942 (Wandzeit über `glFinish`, nicht GPU-intern) |

**Plan-§4-Budget (CPU < 0,5 ms, GPU < 1,5 ms, 1440p) eingehalten, mit Luft:** Vulkans p50/p95 liegen auf CPU und GPU deutlich darunter; der einzelne CPU-Ausreißer (9,266 ms, ein Frame von 600) kommt vom Store/Atlas beim Streamen, nicht vom Zeichnen selbst (`build the frame` / `record the draws` sind die betroffenen Posten, nicht `runtime update`). OpenGLs GPU-Zahl ist nicht direkt vergleichbar (Wandzeit inkl. CPU-Sync-Stall vs. Vulkans reiner GPU-Zeitstempel), aber selbst so liegt ihr p50 unter 1,5 ms.

**Neuer Fund, nicht behoben (Schritt 1521+, nicht dieser Schritt):** `landscape_gpu_check` **ohne** `--only` (alle vier Backends GL→D3D11→D3D12→Vulkan in einem Prozess) stürzt zuverlässig ab — Ausnahme `0x87d` in `KERNELBASE.dll`, mitten in Vulkans Initialisierung (nach `swapchain created`, vor `render pass created`) der ENGINE-Prüfung, direkt nachdem D3D11 und D3D12 im selben Prozess schon gelaufen und abgebaut waren. Jedes Backend **allein** (`--only vulkan`, `--only gl`, …) ist in jedem hier gelaufenen Versuch sauber (exit 0). Deshalb oben der externe, isolierte Pixel-Diff statt des Werkzeug-eigenen Vergleichspasses. Vermutung: Zustand/Treiber-Konflikt beim API-Wechsel auf demselben Adapter innerhalb eines Prozesses, nicht spezifisch an `VulkanLandscape`. Lesson 613 im Hive, Dateien `src/HE_Tools/src/LandscapeGpuCheck/*`.

**Nicht geprüft:** `--arrays` (echtes Forest/Grassland/Desert-Array) — auf NN-WS03 ist kein lokaler EngineContent-Cache mit den gestagten Texturen vorhanden (560 MiB, nicht in git, siehe `scripts/landscape-textures/stage_polyhaven.py`); TAA in Bewegung; Validierungsschichten (kein `VK_LAYER_KHRONOS_validation` angefordert); Intel/AMD; ein Editor- oder Spiel-Lauf mit echter Szene statt des Prüfwerkzeugs.

## 9b. Vulkan auf dem Mac über MoltenVK (06.11.2026, MacBook Air M5, Release)

Homebrew liefert jetzt `vulkan-loader` 1.4.363 und `molten-vk` 1.4.1 (ICD `/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`); `cmake -DCMAKE_PREFIX_PATH=/opt/homebrew` findet `libvulkan.dylib` und schaltet `HE_VULKAN_ENABLED` an, `RendererVulkan` baut und linkt auf dem Mac.

**Erster Lauf scheiterte erwartungsgemäß:** `vkCreateInstance failed` im Werkzeug, `Vulkan: vkCreateInstance` im Renderer. Grund: MoltenVK ist ein *Portability*-Treiber, und ein Khronos-Lader ab 1.3.216 versteckt ihn, solange die Instanz ihn nicht anfordert. Fix (in beiden Pfaden, nur aktiv, wenn der Lader die Erweiterung anbietet, also ohne Wirkung auf Windows, Linux und lavapipe):
* Instanz: `VK_KHR_portability_enumeration` einschalten und `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` in `ci.flags` setzen: `VulkanRenderer::createInstance()` und `VulkanBackend.cpp::init`.
* Gerät: `VK_KHR_portability_subset` einschalten, wenn das Gerät es meldet (Pflicht laut Spezifikation): `VulkanRenderer::createDevice()` und `VulkanBackend.cpp::init`.

**Gelaufen** (`landscape_gpu_check --only vulkan`, `--only gl`, `--only metal`, je ein eigener Prozess wegen Lesson 613): Gerät `Apple M5 (Vulkan 1.2.334, driver 0x28a1)`, Offscreen `OK`, Engine Forward `OK` (Boden 100 % grün, 80176/80176), Engine Deferred `OK` (80176/80176), `rejected == 0`.

**Bild, Vulkan gegen Metal gegen OpenGL (alle drei auf derselben M5, Pixel > 2/255 von 129 600 bzw. 518 400):**

| Szene | Vulkan - OpenGL | Vulkan - Metal | OpenGL - Metal |
|---|---|---|---|
| ramp | 0 | 0 | 0 |
| hills | 15 (0,003 %) | 7 (0,001 %) | 18 (0,004 %) |
| worlduv | 55 (0,042 %) | 0 | 55 (0,042 %) |

Die Landschaft liegt also auf allen drei Backends auf dem Pixel gleich (Vulkan und Metal sind bei worlduv bitgleich). Die beiden Engine-Szenen (`engine_forward`, `engine_deferred`) weichen zwischen allen Backends um 4 bis 20 % der Pixel um mehr als 2/255 ab (auch OpenGL gegen Metal), das ist Himmel/Tonemapping/Postfx der Backends und kein Landschaftsfehler; das Werkzeug prüft dort nur den Boden-Anteil.

**Neu auf MoltenVK, nicht behoben:** `material set 0 needs 17 fragment samplers (16 without heLandscapeWeights); device maxPerStageDescriptorSamplers 16 -> heLandscapeWeights LEFT OUT (layer-blend materials draw built-in)`. MoltenVK ohne Argument Buffers meldet 16 Sampler je Stufe; das trifft den Material-Pfad von `VulkanRenderer` (Layer-Blend-Material der alten Terrain-Chunks), **nicht** den Quadtree (`VulkanLandscape` hat eigene Bindungen und zeichnet voll, siehe oben). Mit `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1` (Umgebung) würde der Lader mehr melden; nicht probiert.

**`--arrays` (Forest-Texturarrays, 06.11.2026):** auf Vulkan, Metal und OpenGL je ein Lauf mit `--arrays <EngineContent-Root> --only <backend>`; die drei `T_Landscape_Forest_{Albedo,Normal,Mask}_Array.hasset` (je 111 MB) lagen nicht im Deploy-Ordner (`out/deploy/Editor/EngineContent/Textures/Landscape` ist leer) und nicht im `EngineContentCache` (dort nur Mountains/Mix), sondern als Dateien im Scratchpad einer früheren Session (`.../scratchpad/srv/Textures/Landscape`); ein Root mit Symlinks darauf genügte. Ergebnis auf allen drei gleich: `OK`, Boden nicht mehr das flache Grün (0,0 % grün), mittlerer Schritt zwischen Nachbarpixeln 1,45/255 (also Textur im Boden); das Vulkan-Bild zeigt den dunklen Waldboden. Pixel gegen die anderen Backends (Engine-Bild mit Himmel): Vulkan-OpenGL 17,8 %, Vulkan-Metal 10,3 %, OpenGL-Metal 12,7 % über 2/255, wie bei den Engine-Szenen ohne Arrays (Himmel/Tonemapping, §9b oben).
**Falle:** `landscape_gpu_check` muss nach jeder Änderung an `LandscapeSource.h` (Klassengröße von `LandscapePackWriter`) neu gebaut werden; ein nur über `he_tests` aufgefrischtes `libHorizonScene.dylib` lässt das alte Werkzeug in `initCoarse` mit EXC_BAD_ACCESS sterben.

**Nicht gemessen:** keine `--measure`-Zahlen als Abnahme (die Plan-§4-Zahlen gelten für die 4070, §9a); Validierungsschichten; ein Editor-/Spiel-Lauf mit echter Szene unter MoltenVK.

## Handoff

- **Topic / step:** 326, Schritt 1520 (Quadtree-Renderer), Hive-Schritt 9 (Vulkan-Abnahme auf NN-WS03). **Schritt 1520 ist jetzt geschlossen** für OpenGL, Metal, D3D11, D3D12 und Vulkan — §9a trägt die zuvor fehlende Vulkan-Abnahme (Zeit/Bild/Tiefe) auf der echten 4070 nach.
- **Done (dieser Lauf, Commit siehe `git log`):** `landscape_gpu_check` einmal pro Backend isoliert laufen lassen (`--only vulkan`, `--only gl`, je mit und ohne `--measure`) auf NN-WS03/RTX 4070. Offscreen- und Engine-Prüfung (Forward+Deferred) für Vulkan `OK`; Pixel-Diff Vulkan/OpenGL extern (ramp 0 %, hills 0,0004 %, worlduv 0,017 %, alle < 0,1 %); CPU/GPU-Zeiten unter dem Plan-§4-Budget (§9a-Tabelle); `he_tests --source-file=*test_landscape_*` 121/121 grün (1 045 374 Prüfungen). CMake-Fix: `HE_Tools/CMakeLists.txt` kopiert `${CMAKE_BINARY_DIR}/Shaders` jetzt POST_BUILD neben `landscape_gpu_check.exe` (nur Vulkan braucht `*.spv` von der Platte; ohne das lief nur die Offscreen-Prüfung, nicht Forward/Deferred).
- **Files:** `src/HE_Tools/CMakeLists.txt` (Shaders-Deploy fürs Werkzeug, im `HE_VULKAN_ENABLED`-Block); Doku dieser Abschnitt (§9a) in `docs/landscape-world-scale-gpu-2026-11-01.md`. Kein Renderer-Code geändert (nicht Teil dieses Schritts).
- **Verified (NN-WS03, RTX 4070, echter Treiber, Release-Build):** siehe §9a — Offscreen OK, Engine Forward/Deferred OK, Pixel-Diff OK, Zeiten OK, 121 Landscape-Tests grün.
- **Not verified:** `--arrays` mit echten Texturarrays (kein lokaler EngineContent-Cache auf NN-WS03); TAA in Bewegung; Validierungsschichten; Intel/AMD; ein Editor-/Spiel-Lauf mit echter Szene statt des Prüfwerkzeugs.
- **Decisions taken:** keine neuen Architektur-Entscheidungen (reine Abnahme); der CMake-Fix ist eine Werkzeug-Vervollständigung nach demselben Muster wie `HorizonEditor`s eigener Shaders-Deploy, kein Eingriff in `VulkanRenderer`/`VulkanLandscape`.
- **Pending (für 1521+, nicht diesen Schritt):** der neue Absturz-Fund bei einem kombinierten Lauf aller Backends in einem Prozess (§9a, Lesson 613) — Ursache ungeklärt, wirkt wie ein API-Wechsel-Konflikt auf demselben Adapter, nicht spezifisch an der Landschaft; D3D11/D3D12 lesen Normal/Mask noch nicht (Windows-Auftrag §1.2); Schritt 3 an die Anwendung hängen; Schatten/GI (Schritt 4); `--arrays`-Lauf auf Vulkan, sobald ein EngineContent-Cache auf NN-WS03 steht.
- **Traps (unverändert aus §9, jetzt auf der 4070 bestätigt):** `VulkanLandscape::draw` hängt an `m_currentFrame`; Upload-Batch ist ein eigener Command Buffer (`Env::flush` vor dem Zerstören genannter Bilder); `maxImageArrayLayers` < Atlas → `rejected`, nichts im Log (hier: 0, also kein Problem auf der 4070); ohne `HE_HAVE_SHADERC` ist die Landschaft aus. **Neu:** nur Vulkan lädt Shader von der Platte (`<exe>/Shaders/*.spv`) — jedes neue Werkzeug/Target, das `RendererVulkan` linkt, braucht den `copy_directory`-Schritt aus `HE_Tools/CMakeLists.txt` oder scheitert lautlos mit "scene shaders missing" nur in der Engine-Prüfung, nicht im Offscreen-Pfad. **Neu:** GL/D3D11/D3D12/Vulkan in einem Prozess (kein `--only`) stürzt ab — bei künftigen Läufen des Werkzeugs einzeln pro Backend aufrufen.
- **Next steps:** 1. Schritt 1521 (oder wo auch immer Mehrbackend-Robustheit des Werkzeugs hingehört): den Absturz aus §9a untersuchen. 2. `--arrays`-Lauf auf Vulkan nachholen, sobald Texturarrays lokal verfügbar sind. 3. Physik/Navigation an die Anwendung hängen (Schritt 3 des Plans). 4. Schatten/GI auf der Höhen-Clipmap (Schritt 4, Topic-Schritt 10).
