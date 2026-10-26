# Water surface draws white: UUID `{0x400, 1}` is owned by two engine assets (Thema 230, Schritt 2)

Meldung des Menschen: Die Wasserfläche wird komplett WEISS, beim Ziehen und nach Speichern + Neuladen, auf allen Backends.

## Befund (belegt)

`kEngineWaterMaterialId` (`DefaultAssets.h:93`) = `{0x400, 1}` ist **dieselbe UUID** wie `Textures/Landscape/T_Landscape_Grass_Albedo.hasset`
(`LandscapeTexGen/main.cpp:81`, `kTexBaseHi = 0x400`, Index 0). `MatGen/main.cpp:61` hat `kMatBaseHi = 0x400`, Water ist Eintrag 0.
Seit 13.10. bekannt (`docs/landscape-material-export-textures-abnahme-2026-10-13.md:67-69`, `weather-audio-survey-2026-10-08.md:119`),
als „eigenes Thema“ notiert, nie gemacht. Die Landschaftstexturen kommen per EngineContent-Sync in `~/Library/Application Support/HorizonEngine/EngineContentCache`
(Manifest-Eintrag `hi 0x400, lo 1`), ein frischer Checkout hat sie nicht, der Mensch schon.

Mechanismus (`ContentManager.cpp`):
* `isLoaded(id)` (`:2756`) und `ensureResident(id)` (`:3411`) sind **typblind**: `m_handleToUUID[id]` (`:656`) hält pro UUID genau einen Eintrag, der zuletzt
  registrierte Asset-Typ gewinnt.
* Ist die Grass-Albedo-Textur geladen (Landschaft im Projekt / Startszene), liefert `ensureResident({0x400,1})` **true**, ohne das Material zu laden.
  `WaterSurface.cpp:98` (`ensureWaterMaterial`) kehrt dort zurück, der Pfad-Fallback `:99` und die Warnung `:103` werden nie erreicht.
  Das 9c-Reparieren (`isLoaded(kEngineWaterMaterialId)`, Release-Zweig) ist aus demselben Grund wirkungslos.
* Renderer: `getMaterial({0x400,1})` = `nullptr` → `resolveFragment` (`MaterialShaderLibrary.cpp:1005`) false → Standard-PBR mit Basisfarbe (1,1,1) = **weiß**.
  Gemeinsamer Code, daher alle Backends. Kein Log, kein Shader-Fehler.

## Beweis (Metal, Release-Deploy `out/deploy/Editor`, Kopie mit `cp -c`, Zeuge `HE_DUMP_WATERBRUSH=paint` + `HE_DUMP_SKYTEST=1`)

| Lauf | Wasser-Fleck (Mittel RGB) |
|---|---|
| Config des Menschen (Projekt Catania, Cache vorhanden) | (135,145,171), blasses Grau-Weiß wie die weiße Standardlandschaft, Wasser-Shader nicht angewendet |
| `HE_CONFIG_DIR=<leer>` (kein Projekt, kein Cache geladen) | (94,113,139), blaues Wasser |
| Water.hasset aus der Kopie **gelöscht** | (135,145,171), identisch zum ersten Lauf, keine „not available“-Warnung |
| Water.hasset mit allen 56 Parameterfloats 0 bzw. 1 | ±1 gegenüber dem ersten Lauf, d. h. das Material wird nicht angewendet |
| lldb, Haltepunkt `ContentManager::getMaterial` bei `{0x400,1}` | `isLoaded=1`, `getTexture=0x79d0bd8480` (nicht null), Renderer fragt 5× `resolveFragment({0x400,1})` |

Damit erklärt: Schritt 1 (Run 1104) fand nichts, weil seine Läufe weder Cache noch Projekt mit der Textur hatten.

## Nicht die Ursache (geprüft)

* Water.hasset selbst: MTRL-Tail vollständig (`paramFloats` 56 ≤ 64, 14 Namen), kein Pipeline-/MSL-Fehler im Log, Pipeline wird gebaut.
* Landschaftsmaterial auf Wasser, generischer Undo (Run 1104).

## Fix (umgesetzt, 32371eb7)

Water liegt jetzt bei `{0x600, 1}` (`DefaultAssets.h` `kEngineWaterMaterialId`, `MatGen/main.cpp` `kMatBaseHi`); `Water.hasset` hat nur die
META-UUID geändert (Byte 47 `0x04` → `0x06`, der Rest ist byte-gleich zur Datei des Release-Zweigs; ein frischer `mat_gen`-Lauf auf dem Mac
unterscheidet sich im GLSL-Text von der Windows-Datei, darum der Byte-Patch). Altverweise: `SceneSerializer.cpp` `jsonToMaterialUuid` lenkt
`{0x400,1}` in Material-Feldern (MaterialComponent samt Slot-Overrides, LOD, Foliage, Rope, Trail, Partikel-Legacy, UI-Image) auf die neue UUID um;
`kLegacyEngineWaterMaterialId` in `DefaultAssets.h`. Texturverweise bleiben unberührt.

Tests (`tests/test_water_surface.cpp`): „a landscape texture on the water material's old id does not take the material away“ schlägt mit der alten
UUID fehl (`REQUIRE(water)` null, genau der Fehler) und besteht mit der neuen; „a scene saved under the old id still gets the water material“.
Läufe: `test_water_surface` 56/56, `test_engine_materials` 11/11, `test_inspector_ui` 13/13, Filter `*ater*,*cene*,*ngine*,*refab*,*erializ*` 964/964.
Metal, `HE_DUMP_WATERBRUSH=paint` mit der echten Config des Menschen (Projekt + Cache): vorher Fleck (135,145,171) ohne Wasser-Shader,
nachher blaues Wasser (132,154,178). D3D11/D3D12/Vulkan/OpenGL nicht gelaufen.

## Handoff

* **Offen:** Pakete, die vor dem Fix exportiert wurden, tragen Water unter der alten UUID; neu exportieren. Wasser-Verweise in einem alten Pak
  werden auf die neue UUID umgelenkt und fehlen dort bis zum Neu-Export.
* **Nicht verifiziert:** D3D/Vulkan/GL (gemeinsamer Pfad, aber nicht gelaufen); CI nicht gestartet.
* **Nächste Schritte:** PR `claude/water-uuid-collision-0x600` → `release/0.7.0`, CI Windows/Linux abwarten.
