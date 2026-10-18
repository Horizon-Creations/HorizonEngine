# Landscape: Sechsecke nach dem Nachladen der Textur-Arrays, Abnahme (Thema 210, Schritt 3)

Ursache und Gegenprobe im Detail: `docs/landscape-hex-reload-ursache-2026-10-18.md` (Schritt 1 + 2).
Dieses Dokument ist die Abnahme: Vorher/Nachher auf Metal und OpenGL, Tests, offene Punkte.

## Ursache

Die Sechsecke sind das Texture-Bombing des Auto-Landscape-Materials über dem **weißen
Ersatz-Array** (die weiße Normale wird je Hex-Zelle gedreht), kein falscher Layer, Mip oder
halb befülltes Array. Alle fünf Backends legten in `ResolveGraphTexture` einen
fehlgeschlagenen Resolve als `null` unter dem Pfad-Schlüssel `<pfad>#arr` ab und fragten nie
wieder; `InvalidateTexture` trifft nur `hi:lo`-Schlüssel. Ein Array, das nach dem ersten
Resolve ankommt (EngineContent-Download), blieb so für den ganzen Lauf weiß.

## Korrektur (Schritt 2, `d4f6eff9`)

| Stelle | Änderung |
|---|---|
| `src/HE_Rendering/include/HorizonRendering/GraphTextureMisses.h` | `HE::forgetGraphTextureMisses`: Fehlschläge fliegen raus, sobald `ContentManager::contentEpoch()` sich bewegt hat; geladene Texturen bleiben unberührt |
| `MetalRenderer.mm:9561`, `OpenGLRenderer.cpp:8938`, `VulkanRenderer.cpp:3657`, `D3D11Renderer.cpp:5027`, `D3D12Renderer.cpp:3693` | Aufruf vor dem Cache-Lookup, je Backend ein `…MissEpoch`-Member |
| `src/HE_Core/src/ContentManager/ContentManager.cpp:1359` (`pollAsyncResults`) | `noteContentChanged()`, wenn ein Remote-Download landet |
| `tests/test_landscape_reload.cpp` | 5 Fälle: Epoche bei Ankunft, spätes Array = Erstladen, Resolve im Ankunfts-Frame, kein Neuöffnen pro Frame, Quell-Pin auf alle fünf Backends |

## Vorher/Nachher (Schritt 3)

`scripts/landscape-reload-repro/cap210.sh out/cap210-step3 <EngineContentCache/Textures/Landscape> <fall>`,
Release-Deploy auf `a97157b3`, privates HOME je Lauf, GI=0 (bitgleich zwischen Läufen).

| Fall | Metal md5 | OpenGL md5 | Bild |
|---|---|---|---|
| present (Erstladen) | `016e8e05` (= Schritt 1) | `6bad630c` (= Schritt 1) | texturiert |
| **mid (Nachladen, arrived=3)** | **`016e8e05`** | **`6bad630c`** | **= Erstladen, bitgleich** |
| absent (kommen nie, Negativkontrolle) | `0fc72535` | `3a99e27d` | Sechsecke |
| Vorher: mid mit dem Schritt-1-Binary | `0fc72535` | `3a99e27d` | Sechsecke (Tabelle Ursache-Doku §3) |

- `present` ist md5-gleich zu Schritt 1, das Schritt-1-Bild `63-present-Metal.png` gilt also
  als Erstladen; `64-mid-Metal.png` (Schritt 1, md5 `0fc72535`) ist das Vorher, `absent` hier
  reproduziert es bitgleich. Ein zweites Vorher-Binary wurde darum nicht gebaut.
- `bmpdiff.py mid absent`: 748082 / 921600 px verschieden (Metal und GL), mid vs. present: md5-gleich.
- Bilder (nicht in git, im Hive-Thema abgelegt): `out/cap210-step3/{present,mid}-{Metal,OpenGL}.png`,
  `absent-OpenGL.png`. `Cannot load`-Zeilen: present 0, mid 5, absent 5 (keine Log-Flut).

Beobachtung, nicht dieser Fehler: auf dem **Schnee** sind auch beim Erstladen schwache
Hex-Zellen zu sehen (`present-OpenGL.png`, rechte Hälfte). Das ist das Bombing über der echten
Schnee-Textur, gehört zu Thema 180 (Auto-Landscape-Material 2.0).

## Handoff

- **Topic / step:** 210, Schritt 3 von 3
- **Branch / commit:** `claude/landscape-sechseckige-muster-nach-texturarray-nachladen` @ dieser Doku-Commit (gepusht: ja)
- **Done:** Abnahme: Nachladen ist auf Metal und OpenGL pixelgleich (md5) zum Erstladen, Tests grün, siehe oben
- **Files touched:** nur diese Doku; Code unverändert seit `a97157b3`
- **Verified:** `cmake --build out/build/release --target HorizonEditor he_tests -j 8` (ninja: no work to do, Binaries von Schritt 2 auf `a97157b3`; Deploy-dylibs per `cmp` gleich dem Build); `cap210.sh` present/mid/absent auf Metal + GL (Tabelle); `ctest -R "^(test_landscape_reload|test_contentmanager)$"` 2/2 (5/5 und 91/91 Fälle); voller `he_tests` (aus dem Build-Ordner): 5514/5517, rot `test_hpak:1188` und 2× `test_profiler` (dieselben drei wie Schritt 2); allein: `test_profiler` 18/18, `test_hpak` mit privatem HOME 59/59 (gefüllter Per-User-Cache bzw. Reihenfolge, nicht dieser Fix)
- **Not verified:** D3D11/D3D12/Vulkan zur Laufzeit (Projektregel; CI kompiliert sie, Lauf https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39135752135 lief beim Abschluss noch); echter SFTP-Download im Live-Editor: `cap210 mid` bewegt die Epoche über den Zeugen, nur der Unit-Test fährt `pollAsyncResults` (Fake-Materializer)
- **Decisions taken:** Vorher aus Schritt 1 übernommen statt altes Binary neu zu bauen, weil `present` md5-gleich ist und `absent` das alte `mid` bitgleich nachstellt
- **Pending:** CI grün inkl. Windows (D3D11/D3D12 werden nur dort kompiliert); D3D/Vulkan-Befund: gleiche Ursache lag dort vor und ist mit demselben Helfer behoben, aber nur kompiliert, nie gelaufen
- **Traps:** Build-Ordner dieses Worktrees ist `out/build/release`, nicht `cmake-build-release`; `he_tests` aus dem Build-Ordner starten; der Deploy darf die Arrays nicht in `EngineContent` haben (`cap210.sh` bricht sonst ab); `test_hpak:1188` rot bei gefülltem Per-User-Cache
- **Next steps:** 1. Queen: nach grüner CI nach `release/0.7.0` mergen. 2. Außerhalb dieses Themas (Ursache-Doku §4): Hot-Reload einer schon geladenen Pfad-Textur und `m_uiTexCache` haben dieselbe Pfad-Lücke. 3. Schwache Hex-Zellen im Schnee beim Erstladen an Thema 180 geben
