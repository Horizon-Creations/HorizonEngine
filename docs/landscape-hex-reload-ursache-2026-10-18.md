# Landscape: Sechsecke nach dem Nachladen der Textur-Arrays, Ursache (Thema 210, Schritt 1)

Stand 2026-10-18, Mac (Apple M5), Release-Build dieses Zweigs, Metal und OpenGL.

## 1. Ergebnis in einem Satz

Kommen die drei Arrays des Auto-Landschaftsmaterials **nach** dem ersten Auflösen ihrer Slots an
(Download in den EngineContent-Cache, Datei kam später auf die Platte), rendert das Material für den
Rest des Laufs mit dem **weißen Ersatz-Array**, weil Metal und OpenGL den Fehlschlag unter dem
**Pfad-Schlüssel** `<pfad>#arr` als `null` cachen und nie neu fragen. Das Sechseck-Muster ist das
Textur-Bombing des Materials über diesem weißen Array.

## 2. Ursache (Datei, Zeile, warum)

| Stelle | Was passiert |
|---|---|
| `src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm:9558-9564` (`ResolveGraphTexture`) | Fehlschlag wird als `nullptr` unter `path#arr` eingetragen (`emplace(key, retained)`), jeder spätere Aufruf liefert aus dem Cache `m_whiteArrayTexture` |
| `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp:8935-8947` | dasselbe: `0` wird gecacht, danach immer `m_whiteArrayTex` |
| `MetalRenderer.mm:16715-16727`, `OpenGLRenderer.cpp:10929-10941` | die einzige Invalidierung (`InvalidateTexture`) baut den Schlüssel aus `hi:lo` der UUID; ein Pfad-Eintrag (lose Material-Datei, `graphTextureIds` leer) wird nie getroffen |
| `src/HE_Core/src/ContentManager/ContentManager.cpp:1094-1099` (`loadAsset(Pfad)`) | liefert für ein noch nicht angekommenes Remote-Asset absichtlich `UUID{}` ("the caller asks again next frame"). Die Renderer fragen aber nicht noch einmal. Der ContentManager selbst hat keinen Negativ-Cache. |
| `VulkanRenderer.cpp:3653-3654` (nur gelesen) | der Kommentar nennt die Annahme, die nicht mehr stimmt: "resolveTextureRef loads synchronously, so 'still null' means unloadable, not 'not yet' (mirrors GL)". Seit dem Pfad-Download (Doku `auto-landscape-material-textures.md` §20.2) kann es "not yet" sein. |

Die Lücke steht schon als offener Punkt in `docs/auto-landscape-material-textures.md` §20.4 (1)
("Im Lauf, der den Download anstößt, bleibt die Textur weiß"). Neu ist hier der Nachweis, dass genau
das das gemeldete Sechseck-Muster ist, und die Gegenprobe.

**Warum Sechsecke und nicht einfach weiß:** Albedo und Maske weiß sind gleichförmig, aber das weiße
Normal-Array ist eine konstante, schräge Tangenten-Normale (1,1,1). `NormalMapArrayBombSample`
dreht sie je Hex mit (`AutoLandscapeMaterial.cpp:107-138`, §9 "transpose(R)"), also wird jedes Hex
anders beleuchtet. Ohne Bombing (Schalter aus) ist dieselbe Lage eine glatte weiße Fläche (Tabelle,
`absentnobomb`). Kein falscher Layer-Index, keine falschen Mips: das Array ist gar nicht gebunden.

## 3. Gegenprobe

Zeuge `HE_DUMP_AUTOLANDARRIVE=<ordner>` (`src/HE_Editor/EditorApplication.cpp:8755`, Aufruf mitten in
den Settle-Frames `:10041`): kopiert die drei `T_Landscape_*_Array.hasset` in den Cache und lädt sie,
`=early` vor dem Szenenaufbau, sonst nach Frame 8 von 16. Skript
`scripts/landscape-reload-repro/cap210.sh OUT ARRAYDIR CASE [RHI]` (privates `HOME` je Lauf, also
leerer Cache; Deploy dieses Worktrees hat die Arrays nicht in `EngineContent`, die echte Cache-Kopie
wird nur gelesen), Bildvergleich `bmpdiff.py`. Kamera wie `cap158auto.sh`, GI=0.

| Fall | Metal md5 | OpenGL md5 | Bild |
|---|---|---|---|
| present (vor dem Start im Cache = Erstladen) | `016e8e05` | `6bad630c` | texturiert |
| present, zweiter Lauf | `016e8e05` | `6bad630c` | bitgleich |
| early (kommen vor dem ersten Resolve an) | `016e8e05` | `6bad630c` | = Erstladen |
| **mid** (kommen nach dem ersten Resolve an, geladen: 2048², 5 Layer, 12 Mips) | `0fc72535` | `3a99e27d` | **Sechsecke** |
| absent (kommen nie) | `0fc72535` | `3a99e27d` | = mid, bitgleich |
| absentnobomb | `a0f2b62f` | `ac0683bb` | glatt weiß, keine Sechsecke |
| **mid + Probe** (null nicht gecacht, Metal+GL, *nicht committet*) | `016e8e05` | `6bad630c` | **= Erstladen** |

present gegen mid: 748 082 von 921 600 Pixeln anders (beide Backends). `mid` = `absent` bitgenau heißt:
ein spätes Eintreffen ist für den Renderer dasselbe wie nie. Die Probe (Metal `:9563`, GL `:8946`
`emplace` nur bei Erfolg) schaltet das Muster aus, die Rücknahme wieder ein: das ist die Zeile.

**Für Schritt 2 gemessen:** Mit der Probe fragt jeder Frame wieder, das gibt 25 (Metal) bzw. 97 (GL)
`[ERROR] Cannot load asset` in 16 Frames und je ein Datei-Öffnen. Der Negativ-Cache hat also einen
Zweck. Der Fix darf ihn nur verwerfen, wenn sich etwas geändert hat (`contentEpoch()` bewegt sich in
`pollAsyncResults` beim Eintreffen eines Downloads; oder `isRemoteAssetPending(Pfad)` beim Ablegen),
nicht blind pro Frame.

## 4. Nicht der Fall hier

- **Hot-Reload einer schon geladenen Datei** (Array auf der Platte überschrieben, `pollHotReload`):
  der Editor ruft für Texturen nur `InvalidateMaterial` (`EditorApplication.cpp:3490`), der
  Pfad-Eintrag bleibt. Ergebnis wäre das *alte* Array, keine Sechsecke. Dieselbe Lücke (Pfad-Schlüssel
  nie invalidiert), anderer Auslöser. Nicht nachgestellt.
- **GI-Reflexionen** (`GiAutoLandscape.cpp`): lesen Mittelwerte aus dem CPU-Asset, nicht aus dem
  Renderer-Cache, alle Läufe mit GI=0. Nicht betroffen.
- **D3D11/D3D12/Vulkan:** dieselbe Bauart, gelesen, nicht ausgeführt (Projektregel): D3D11
  `D3D11Renderer.cpp:5017-5031`, D3D12 `D3D12Renderer.cpp:3681-3695`, Vulkan `VulkanRenderer.cpp:3645-3660`
  cachen den Fehlschlag ebenso unter dem Pfad-Schlüssel.

## 5. Schritt 2: Behebung

Zwei Hälften, beide nötig:

| Stelle | Änderung |
|---|---|
| `src/HE_Rendering/include/HorizonRendering/GraphTextureMisses.h` (neu) | `HE::forgetGraphTextureMisses(cache, seenEpoch, epoch, isMiss)`: einmal je Epoche fliegen die Fehlschläge raus, geladene Texturen bleiben unberührt |
| `ResolveGraphTexture` in Metal (`MetalRenderer.mm:9561`), GL (`OpenGLRenderer.cpp:8938`), Vulkan (`VulkanRenderer.cpp:3657`, gibt halb gebaute Reste per `destroyMaterialTex` frei), D3D11 (`D3D11Renderer.cpp:5027`), D3D12 (`D3D12Renderer.cpp:3693`) | ruft den Helfer vor dem Cache-Lookup; je Backend ein `…MissEpoch`-Member neben dem Cache |
| `ContentManager.cpp:1359` (`pollAsyncResults`, Remote-Zweig) | `noteContentChanged()`, wenn ein Download gelandet ist. Vorher bewegte nur der Zeuge die Epoche von Hand (`EditorApplication.cpp:8794`), der echte SFTP-Weg nie |

Der Negativ-Cache bleibt: ein nie ladbarer Pfad wird je Epoche einmal gefragt, nicht pro Frame.
Keine Daten oder Formate berührt.

**Test** `tests/test_landscape_reload.cpp` (4 Fälle): Epoche bewegt sich beim Eintreffen; "mid"
(Backend fragt, Fehlschlag gecacht, Download landet) liefert dieselben Pixel und Layer wie "present";
ein fehlender Pfad wird in 16 Frames einmal geladen; Quell-Pin, dass alle fünf Backends den Helfer
**vor** `resolveTextureRef` rufen. Ohne Korrektur (Stand `337ccd7f` + Test + Helfer) 3 von 4 rot.

**Gegenprobe mit dem behobenen Editor** (`cap210.sh`, Release-Deploy dieses Worktrees):

| Fall | Metal md5 | OpenGL md5 | `Cannot load`-Zeilen |
|---|---|---|---|
| present | `016e8e05` (= Schritt 1) | `6bad630c` (= Schritt 1) | 0 |
| **mid** | **`016e8e05`** | **`6bad630c`** | 5 |
| absent | `0fc72535` | `3a99e27d` | 5 (keine Flut wie bei der Probe: 25/97) |

## Handoff

### Schritt 2 (neuester)

- **Topic / step:** 210, Schritt 2 von 3; Zweig `claude/landscape-sechseckige-muster-nach-texturarray-nachladen`, Fix `d4f6eff9`, Doku danach, gepusht
- **Done:** Ursache in allen fünf Backends + `ContentManager` behoben, Test geschrieben (Abschnitt 5)
- **Verified:** `he_tests --source-file='*test_landscape_reload.cpp'` 4/4 grün, mit Test + Helfer aber ohne Backend-/CM-Änderung 3/4 rot (gebaut, gelaufen); `test_contentmanager` 91/91; voller `he_tests`-Lauf 5513/5516, rot nur `test_hpak:1188` (bekannt, gefüllter Per-User-Cache) und 2× `test_profiler` (allein 2× grün, reihenfolgeabhängig, berührt nichts hiervon); `cap210.sh` present/mid/absent auf Metal + GL mit dem neuen Deploy (Tabelle §5); Vulkan `-fsyntax-only` gegen MoltenVK rc 0, Negativkontrolle (Helfer vertippt) schlägt fehl
- **Not verified:** D3D11/D3D12 kompiliert (nur Windows-CI); Vulkan/D3D zur Laufzeit; echter SFTP-Download im Live-Editor (der Unit-Test fährt den `registerRemoteAsset`-Weg mit Fake-Materializer, `cap210.sh` den Zeugen, der die Epoche selbst bewegt)
- **Decisions taken:** Epoche statt `isRemoteAssetPending` (das gibt es nicht; Frame 1 zeichnet vor dem Manifest); Negativ-Cache behalten, nur je Epoche vergessen; Helfer header-only in HorizonRendering, damit der Test ihn ohne GPU benutzt; Quell-Pin statt Renderer-Instanz im Test
- **Traps:** doctest-`*_MESSAGE` mit `std::string(a) + b` kompiliert nicht (Operator-Vorrang im Makro), vorher in eine Variable; `ninja` liegt nicht im PATH (`CMAKE_MAKE_PROGRAM` aus der CMakeCache, CLion-ninja); `compile_commands.json` im Release-Baum ist unvollständig, Flags für den Vulkan-Check per `ninja -t commands` vom GL-Objekt
- **Next steps:** 1. Schritt 3 Abnahme: Vorher/Nachher-Bilder (Schritt-1-Binary vs. dieses), CI grün inkl. Windows (D3D11/D3D12/Vulkan kompilieren). 2. Nicht in diesem Thema, nur notiert: Hot-Reload einer schon geladenen Pfad-Textur (§4) und `m_uiTexCache` haben dieselbe Pfad-Lücke

### Schritt 1

- **Topic / step:** 210, Schritt 1 von 3
- **Branch / commit:** `claude/landscape-sechseckige-muster-nach-texturarray-nachladen` @ siehe `git log` (Zeuge `374a3072`, Doku danach), gepusht
- **Done:** Muster auf Metal und OpenGL nachgestellt, Ursache auf die Zeile belegt (Abschnitte 2 und 3). Nichts behoben.
- **Files touched:** `src/HE_Editor/EditorApplication.cpp:8755-8797` + `:10040` (Zeuge, nur mit Env aktiv), `scripts/landscape-reload-repro/{cap210.sh,bmpdiff.py}`, diese Doku
- **Verified:** Release-Build `out/build/release` (HorizonEditor, rc 0), 18 Aufnahmen, Tabelle oben; Probe gebaut, gemessen, zurückgenommen (`git checkout`), sauber neu gebaut
- **Not verified:** echter SFTP-Download (der Zeuge legt die Dateien selbst ab, kein Manifest, `isRemoteAssetPending` war also falsch); D3D/Vulkan; Hot-Reload-Fall aus Abschnitt 4
- **Decisions taken:** Zeuge committet (nur Env-gesteuert, wie `HE_DUMP_LAYERREPAINT=mid`), Schritt 2 und 3 brauchen ihn für Vorher/Nachher; Probe bewusst nicht committet
- **Decisions suggested, not taken:** Fix in allen fünf Backends gleich, damit kein Backend anders reagiert (§20.4 (1) hatte genau deshalb gewartet); D3D/Vulkan-Teil dann nur per CI und Syntax-Check
- **Traps:** Deploy darf die Arrays nicht in `EngineContent` haben (das Skript bricht sonst ab); Cache hängt an `HOME` (`GlobalState::userDataDir`), nie den echten Cache anfassen; `md5` heißt im Skript `/sbin/md5`; ohne Negativ-Cache Log-Flut (Abschnitt 3)
- **Next steps:** 1. In `ResolveGraphTexture` (Metal, GL) einen Fehlschlag mit der `contentEpoch()` merken und beim Lesen verwerfen, wenn sie sich bewegt hat (oder nur bei `isRemoteAssetPending` nicht cachen und Eintreffen per Epoche). 2. Dieselbe Änderung in D3D11/D3D12/Vulkan. 3. Unit-Test ohne GPU: das Muster aus dem Cache-Verhalten herausziehen (kleiner Helfer, den alle Backends nutzen) und mit `ContentManager` + spät angelegter Datei prüfen; dazu `cap210.sh … mid` muss md5-gleich zu `present` werden (Metal `016e8e05`, GL `6bad630c` gelten nur für diesen Stand). 4. Optional: Pfad-Invalidierung beim Hot-Reload einer Textur (Abschnitt 4).
