# Asset-Optimierung für Packaged Builds — „Cook"-Pipeline

**Kernbefund:** Die `.hpak`-Pipeline ist stark im *Verpacken* (UUID-Baking, zstd/LZ4 + AES,
Referenz-Graph-Culling, On-Demand-Streaming), aber sie versendet die **Edit-Time-Repräsentation
der Asset-Daten unverändert**. Alles, was der Editor zum Bearbeiten braucht (deinterleavte
`float32`-Vertex-Arrays, `uint32`-Indizes, rohe RGBA8-Texturen ohne Mipmaps, Shader-Quelltext),
wird 1:1 ins Spiel geliefert — und die Runtime zahlt die Aufbereitung **bei jedem Laden neu**.

**Die Idee:** Ein **Pack-Time-„Cook"-Schritt**, der jedes Asset einmalig in eine GPU-native /
Runtime-optimale Form transformiert, sodass das gepackte Spiel beim Laden und zur Laufzeit minimal
arbeitet. Architektonisch spiegelt das exakt das schon vorhandene **Dual-Mode-Muster**
(Editor liest Pfade → Packaged liest gebackene UUIDs): der Editor liest weiter die rohe Form,
der Packaged-Build die gekochte.

> Alle Datei:Zeile-Referenzen unten sind gegen den aktuellen Stand (main) verifiziert.

---

## 0. Umsetzungsstand

- ✅ **Cook-Fundament** (Tier 0 #1): `cookForPack()` in `HpakWriter` Pass 2 (Geschwister von
  `rewriteRefsForPack`), `PackSettings::cook`, in `settingsFingerprint` (Toggle → Re-Pack),
  `ProjectExporter` cookt jeden Export. Dual-Mode-Parse in `ContentManager` (gekochter Chunk →
  GPU-ready, sonst rohe SoA).
- ✅ **Mesh-Cook** (Tier 1 #4, Teil): Static-Meshes werden pack-time in den exakten interleavten
  8-float-VBO (`CHUNK_MVBO`) + gebackene AABB umgeschrieben; alle 5 Backends laden ihn direkt
  (GL+Metal verifiziert, D3D/Vulkan blind nach identischem Muster). Entfernt den First-Draw-
  Interleave-Loop + AABB-Scan. **Offen daran:** uint16-Indizes, Vertex-Quantisierung, Skeletal-Cook.
- ✅ **Textur-Cook: Mipmaps + Format-Tag** (Tier 1 #3, Teil): RGBA8-Texturen bekommen die volle
  Mip-Kette pack-time in `PIXL` gebacken (`mipLevels`/`format`/`srgb` im `TXMI`-Tail, back-compat).
  GL lädt die Levels direkt (kein Runtime-`glGenerateMipmap`); **Metal bekommt endlich Mipmaps**
  (`mipmapLevelCount` + `mipFilter` — fixt das Minification-Aliasing) — beide verifiziert. D3D/
  Vulkan bleiben unangetastet: alle 5 Backends lesen Level 0 als führende Bytes, angehängte Mips
  sind byte-kompatibel ignoriert. **Offen daran:** GPU-Kompression — auf Apple-Silicon-Metal
  bedeutet das **ASTC** (nicht BC; Apple-GPUs können kein BC), auf Desktop/Intel/GL BC via
  `stb_dxt`; braucht einen ASTC-Encoder (astcenc) + den Cook-Cache. sRGB-Tag ist verdrahtet, aber
  noch nicht gesetzt (braucht Textur-Rollen-Info aus dem Material).
- ⬜ **GPU-Textur-Kompression** (ASTC/BC) + **sRGB-Aktivierung** — als Nächstes.

---

## 1. Ist-Zustand: Was geliefert wird vs. was die Runtime dafür zahlt

| Asset | Liefert heute | Runtime zahlt (Datei:Zeile) |
|---|---|---|
| **StaticMesh** | SoA `vector<float>` pos/norm/uv + `vector<uint32>` indices, getrennte Chunks (`Assets.h:27-30`) | Interleave-Loop pos3+norm3+uv2 → 8-float/32 B-VBO **bei erstem Draw auf dem Main-Thread** + volle AABB-Scan (`OpenGLRenderer.cpp:3400-3420`, `MetalRenderer.mm:3155-3177`); `uint32`-Indizes **immer** (`:3280 / :3899`) |
| **SkeletalMesh** | zusätzlich `uint32` boneIDs + `float32` weights = 32 B/Vtx (`Assets.h:50-51`) | zero-padded uint32/float4-Buffer neu gebaut (`:3524-3577 / :3257-3289`) — könnten 8 B/Vtx sein |
| **Texture** | rohes **RGBA8**, keine GPU-Kompression, **keine Mipmaps** (`Assets.h:110`) | GL: `glTexImage2D(GL_RGBA8)` + synchrones `glGenerateMipmap` bei erstem Draw (`OpenGLRenderer.cpp:3472-3475`). **Metal: `RGBA8Unorm, mipmapped:NO`** (`:3198`) → Aliasing/Shimmer + **GL↔Metal-Parität kaputt** (Korrektheitsbug). VRAM 4× vs. BC7 |
| **Shader** | `ShaderAsset.sourceCode` — **totes Payload**, kein Runtime-Consumer (`ContentManager.cpp:597`) | Real-Shader sind pro Backend hand-geschrieben, ~16 GL + ~15 Metal Compiles **synchron beim `Initialize`** (Startup-Latenz), **kein** Per-Frame-/First-Draw-Hitch (eine geteilte Scene-Pipeline) |
| **Audio** | rohes s16-PCM, voll RAM-resident (`Assets.h:97`, `ContentManager.cpp:177`) | pro Voice erneut in `ma_audio_buffer` kopiert (`AudioEngine.cpp:121/203`); kein Streaming für lange Musik |
| **Animation** | rohe `float`-Keyframes (`Assets.h:145-146`) | O(n)-`findBracket`-Scan pro Channel/Frame (`AnimationEval.cpp:11-20`) |
| **Font** | rohe TTF-Bytes (`Assets.h:104`) | **nirgends konsumiert** — UISystem nutzt einkompiliertes ProggyClean → reines totes Payload |
| **Scene/Prefab** | CBOR (`SceneSerializer.cpp:1178`) | bereits ok (base64-gepackte Float-Arrays); keine Aktion |

**Zwei Per-Frame-Steady-State-Kosten**, die der Cook mitadressieren kann:
- Der Extractor rechnet **jede** World-Matrix **jeden Frame** neu; das `dirty`-Flag wird gesetzt,
  aber nie gelesen (`RenderExtractor.cpp:36-47`).
- Frustum- + Shadow-Cull laufen gegen einen **Unit-Cube-Proxy** statt echter Mesh-Bounds →
  zu große Sichtbarkeitsmenge, Overdraw, lockere Shadow-Frustum-Fit. Eine **gebackene AABB** fixt
  beides.

---

## 2. Cook-Architektur

**Einbauort (ein Punkt):** In `HpakWriter::addDirectory`, Pass 2, wird heute
`blob = rewriteRefsForPack(pe.bytes, pathToUuid)` gebildet (`HpakWriter.cpp:311`). Der Cook wird
ein Geschwister davon:

```cpp
std::vector<uint8_t> blob = cook(rewriteRefsForPack(pe.bytes, pathToUuid), target);
```

- **`cook(blob, target)`** ist eine freie Funktion pro Asset-Typ, die rohe Chunks in gekochte
  Varianten umschreibt und rohe Chunks droppt (z. B. `VERT/NORM/TEXC → VBUF`, `PIXL → PIXC`,
  `+ BNDS`). Unbekannte/uncookbare Chunks laufen wie heute verbatim durch (`HpakWriter.cpp:115`).
- **Versionierte gekochte Chunk-IDs** neben den rohen. Der **eine geteilte Parser**
  (`parseAndRegisterAsset`) bekommt einen Zweig: „gekochter Chunk vorhanden → GPU-ready laden,
  sonst rohe Form (Editor/Hot-Reload/WIP)". Das ist dasselbe Dual-Mode wie MRFU/MTLU/SCNU heute.
- **Cook-Target** = `{Platform, GPU-Family}` (BC vs. ASTC, uint16-Grenze etc.). Kommt aus dem
  bestehenden `ExportPlatform` (Host/Windows/macOS/Linux) — der Cook ist target-abhängig, deshalb:
- **Separater Cook-Cache**, NICHT der Incremental-Pak-Cache. Begründung: ASTC/BC-Transcode ist um
  Größenordnungen teurer als zstd; der Incremental-Cache invalidiert auf Codec/Encrypt/Key
  (`settingsFingerprint`) und kann Cook-Targets nicht unterscheiden. Der Cook-Cache wird gekeyed auf
  `(content-hash des rohen Blobs, cook-target)` und liegt neben dem `.hpak.manifest`.
  → Textur-Recook passiert nur, wenn sich Textur oder Target ändern.

**Verifikation-Prinzip (wie bei jeder bisherigen Packaging-Arbeit):** Jeder Cook wird gegen die
Runtime byte-/pixel-getreu verifiziert und mit einem Fallback auf die rohe Form ausgeliefert, bis
das gekochte Format universell ist. Editor-/Loose-Load und Hot-Reload dürfen nie brechen.

---

## 3. Priorisierte Roadmap (Impact × Aufwand)

### Tier 0 — Fundament (klein, ermöglichend)
1. **Cook-Hook + versionierte Chunks + Dual-Mode-Parser** (M, backend-neutral). Ohne Perf-Gewinn,
   aber die Grundlage für alles darunter.
2. **Textur-Tag: Format-Enum (RGBA8/BC7/BC5/BC4/BC1/ASTC_\*) + Colorspace (linear/sRGB) + mipLevelCount**
   in `Assets.h` + TXMI-Chunk (S, backend-neutral). Der Dreh- und Angelpunkt — muss zuerst rein.

### Tier 1 — Höchster Hebel (hier anfangen)
3. **Textur-Cook: BCn + vorberechnete Mipmaps + sRGB-korrekt** (L, GL+Metal). **Der größte
   Einzelgewinn.** Spart ~4× VRAM (2048²-Albedo: 16 MB→~4 MB), Bandbreite, Load-Time (GL droppt
   `glGenerateMipmap`), Pak-Größe — **und fixt den Metal-Mip-Aliasing-Bug** (Korrektheit + Parität).
   Das ist der offene Masterplan-Punkt **1.6 Textur-Kompression**.
   - Cross-Backend-Baseline = **BC** (BC7 Albedo, BC5 Normal, BC4 ORM/Single-Channel, BC1 simpel).
   - **Achtung macOS-GL 4.1:** BC7/BPTC ist erst GL-Core 4.2 → nur über `GL_ARB_texture_compression_bptc`
     erreichbar; die Extension muss per `glGetString(GL_EXTENSIONS)` geprüft werden (tut die Engine
     heute nicht). Ohne Extension: BC1/BC3 (S3TC, breit verfügbar) oder RGBA8-Fallback.
   - **ASTC ist Metal-only** (Apple Silicon) — als optionaler Extra-Cook-Target später, NICHT als
     universelles Format (GL 4.1 kann kein ASTC sampeln).
4. **Mesh-Cook: pre-interleaved VBO + gebackene AABB (+ uint16-Indizes wenn `vtxCount ≤ 65535`)**
   (M, backend-neutral). Führt den Interleave-Loop, der heute in beiden Backends lebt, **einmal
   beim Pack** aus → Runtime `memcpy`t direkt in den VBO. Entfernt den First-Draw-Hitch + AABB-Scan.
   uint16 halbiert Index-Bandbreite/-VRAM für den Normalfall (Draw-Sites lesen dann eine
   Index-Breite statt hartem UInt32). Die gebackene AABB **fixt zusätzlich das Unit-Cube-Culling**
   → weniger Overdraw + engere Shadow-Fit.

### Tier 2 — Solide Gewinne
5. **Vertex-Quantisierung** (L, GL+Metal): Positionen snorm16 relativ zur AABB, Normalen
   oct-encoded snorm16, UVs half2 → 32 B→~16 B/Vtx. Braucht Dequant-Uniform + Vertex-Format-Decls
   in beiden Backends + Sichtprüfung (präzisions-sensitiv).
6. **Skinning-Quantisierung** (M, GL+Metal): boneIDs `uint8`×4 + weights `unorm8`×4 → 32 B→8 B/Vtx
   (Gewichte renormalisieren; Fallback uint16 bei >256 Joints).
7. **Echte-Bounds-Culling** (M, backend-neutral): nutzt die gebackene AABB aus #4 im Extractor
   statt Unit-Cube → weniger Draws/Overdraw, bessere Shadow-Fit.
8. **Transform-Dirty-Flags / statische World-Matrizen** (M, backend-neutral): das `dirty`-Flag
   wieder auswerten; optional beim Cook eine „static"-Kennung backen, damit `propagateFrom`
   unveränderliche Szenen-Objekte überspringt.

### Tier 3 — Startup / Größe
9. **Metal `MTLBinaryArchive`** pro GPU-Family beim Pack backen, beim `Initialize` laden (L, Metal):
   spart Startup-Compile-Latenz; Fallback auf Source-Compile bei Family-Mismatch.
10. **GL Program-Binary-Cache** (M, GL): `glGetProgramBinary` beim ersten Start on-disk cachen
    (gekeyed auf Vendor/Renderer/Version + Source-Hash), danach `glProgramBinary`. **Kein**
    Pack-Time-Bake (treiber-/GPU-abhängig).
11. **Startup-Compiles parallelisieren** (M): die ~16/~15 Compiles beim `Initialize` laufen seriell
    auf dem Main-Thread; Metal-Compile ist thread-safe.
12. **Totes Payload cullen** (S): `ShaderAsset`/CHUNK_SRC und `FontAsset.fontData` werden nie
    konsumiert → nicht mehr mitpacken (freie Größe).

### Tier 4 — Niedriger Hebel
13. **Audio**: geteilter immutabler Buffer über Voices (statt per-Voice-`memcpy`) + Streaming/
    Kompression für lange Musik (L).
14. **Animation**: Keyframe-Reduktion (kollineare Keys droppen) + Quantisierung (Rotationen als
    smallest-three 3×16-bit) — Größe + Sample-Kosten (M, per-Clip-Fehlerbudget).

---

## 4. Empfehlung: die ersten drei Schritte

1. **Tier 0 (#1 + #2)** — Cook-Hook + Textur-Tag. Ein Wochenende, keine sichtbare Wirkung, aber
   ohne sie geht nichts.
2. **Textur-Cook BC + Mipmaps + sRGB (#3)** — der mit Abstand größte Runtime-Gewinn (VRAM,
   Bandbreite, Load-Hitch) und fixt nebenbei einen echten Metal-Renderbug. Zuerst BC1/BC3-Baseline
   (überall lauffähig) + gebackene Mips, dann BC7 hinter der bptc-Extension-Prüfung.
3. **Mesh-Cook: interleaved VBO + AABB + uint16 (#4)** — entfernt den sichtbarsten Streaming-Hitch
   („Mesh ploppt beim ersten Sichtbarwerden") und verbessert das Culling gratis.

**Vor allem:** erst **messen**. Es gibt bereits den In-Engine-Profiler (F9-Capture, Per-Pass-GPU
auf HW verifiziert) — eine Baseline (Load-Zeit pro Asset, VRAM, Frame-Zeit einer typischen Szene)
vor dem ersten Cook macht jeden Gewinn belegbar statt behauptet. macOS-GL-Timer-Queries sind
allerdings deaktiviert (`OpenGLRenderer.cpp:2464`), also GPU-Messung primär auf Metal.

---

## 5. Bewusst NICHT / später

- **Tangenten backen** — kein Shader konsumiert heute Tangenten; erst wenn Normal-Mapping landet
  (dann ist der Cook-Hook schon da). Jetzt würde es nur Vertices aufblähen.
- **ASTC als Universalformat** — redundant zu BC, solange der GL-4.1-Backend existiert; nur als
  Metal-/Apple-Silicon-Extra sinnvoll.
- **Shader→SPIR-V-Bytecode** — blockiert durch die offene Lücke 3.2 (Shader-Cross-Compile) und die
  wacklige SPIR-V-Story auf GL 4.1. Metal `MTLBinaryArchive` ist der gangbare Teil.
- **Scene→flaches POD-Blob** — hohe Schema-Versionierungs-Last für kleinen Gewinn; CBOR ist gut
  genug. Nur bei pathologisch großen Szenen.
- **LOD-Kette backen** — LOD ist hier bereits pro-UUID-Selektion (`LODSystem.cpp`); eine Auto-LOD-
  Generierung beim Cook ist ein eigenes Feature, kein reiner Datentransform.
