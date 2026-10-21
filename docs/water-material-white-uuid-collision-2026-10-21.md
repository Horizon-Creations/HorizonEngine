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

## Fix (nicht umgesetzt, Grenze des Auftrags: Water.hasset nicht ändern)

Empfohlen: Water eine **freie UUID** geben, z. B. neuer Block `0x600` (`kMatBaseHi`), also
1. `MatGen/main.cpp:61` `kMatBaseHi` und `DefaultAssets.h:93` `kEngineWaterMaterialId` ändern, `static_assert` (`MatGen:62`) nachziehen;
2. `Water.hasset` neu erzeugen (nur die 16 META-UUID-Bytes ändern sich; auf dem Release-Zweig, dort ist die Datei 273523 Byte, dieser Zweig hat noch die alte 243094);
3. Altverweise: `MaterialComponent`/Szenen mit `{0x400,1}` als Material beim Laden auf die neue UUID umlenken (eine Textur kann dort nie gemeint sein);
4. Tests: `test_engine_materials.cpp` (3 Stellen), `test_water_surface.cpp` (2), `test_inspector_ui.cpp` (1) prüfen; neuer Test „Engine-UUIDs sind eindeutig“
   (Water-UUID ≠ `0x400..0x411`) und „WaterSurface bekommt sein Material, auch wenn die Textur mit der alten UUID zuerst geladen wurde“.
Nicht tun: Material per Pfad über die Textur laden (`loadAsset`) – es überschriebe `m_handleToUUID[{0x400,1}]` und nähme der Landschaft die Textur;
die Texturen umnummerieren (liegen auf dem Server, Verweise in Landschaftsmaterialien).

## Handoff

* **Verifiziert:** Ursache oben (Metal, lldb, A/B-Läufe). Release-Zweig `origin/release/0.7.0` (4d8772d2); mein Zweig basiert auf altem Stand ohne Thema 206 (Merge im Auftrag verboten/vom Klassifikator abgelehnt).
* **Nicht verifiziert:** D3D11/D3D12/Vulkan/OpenGL (gleicher gemeinsamer Pfad, aber nicht gelaufen; D3D/Vulkan auf dem Mac nie prüfbar); ob der Mensch die Textur im Cache hat (aus der Beschreibung „Fallback weiß“ geschlossen, passt zum Mac-Cache).
* **Entschieden:** keine Code-Änderung (Limit „Water.hasset nicht ändern“ steht der einzigen sauberen Lösung im Weg). Nur dieses Dokument.
* **Nächste Schritte:** Queen entscheidet über den Fix oben; er gehört auf `release/0.7.0` (oder diesen Zweig zuerst auf den Release-Zweig bringen), nicht auf den alten Stand.
