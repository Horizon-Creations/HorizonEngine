# Savegame v2: Bestandsaufnahme der drei Restluecken (Thema 81, Schritt 1)

Stand: Zweig `claude/savegame-v2-remaining-gaps` auf `152659ff` (26.09.2026).
Ausgangspunkt war der Memory-Eintrag vom 07.08. mit drei "bewussten Luecken".
Nur Befund, keine Aenderung am Code.

| Luecke | Stand heute | Schwere | Folgeschritt noetig |
|---|---|---|---|
| 1. Struct-Codegen interpretiert | **geschlossen** seit `7f299619` (09.08.) | – | nein |
| 2. Rename-Retarget | **offen**, zwei Lesarten, fuenf konkrete Stellen | hoch (bricht Spieler-Saves) | ja |
| 3. Script-Var-Capture | **offen**, unveraendert seit `628838ae` | mittel (Feature fehlt) | ja |

---

## 1. Struct-Codegen: geschlossen

Der Memory-Eintrag ist veraltet. Zwei Tage danach hat `7f299619`
("HorizonCode-Codegen: Structs+Enums uebersetzen jetzt") den interpretierten
Fallback ersetzt, `3081c65a` hat danach einen Header pro Typ eingefuehrt.

- Jede Struct-Definition wird ein echtes C++-Aggregat in `hcgen_type_<Name>.h`,
  mit `toValue`/`fromValue_` und den `hc::`-Hooks (`zeroOf`, `raw`, `coerce`, `tagOf`)
  (`src/HE_Scene/src/HcCodegen.cpp:555` ff., `:780` ff.).
- Make/Break/Get/SetStructField, verschachtelte Structs, Enum-/Array-/Map-Felder,
  Struct-Arrays und Struct-Parameter sind gelowert. Felder werden gegen die
  Definition per Name aufgeloest, nie positionell.
- Paritaet ist getestet: Fixture `structs` in `HCGEN_CLASSES` (`tests/CMakeLists.txt:723`),
  `TEST_CASE("codegen parity: structs")` (`tests/test_horizoncode_codegen.cpp:1827`)
  vergleicht Interpreter und Kompilat (Seeding, structDefaults, Set/Make, Array-Felder).
  Dazu der Dylib-Pfad ueber `test_hcgen` / `test_hccompiled_loader.cpp`.

**Verbleibende Fallbacks, alle gewollt** (`HcCodegen.cpp:1284-1321`):
- Struct-Pin ohne `typeName` (generische Boundary, die `Graph::connect` zulaesst).
- Struct-/Enum-Definition zur Generierungszeit nicht in der `TypeRegistry`.
Beide sind "Fallback statt Raten" und werden im Export-Ergebnis gemeldet.

**Dokumentierte Nicht-Paritaet, bewusst akzeptiert**
(`docs/horizoncode-cpp-codegen-implementation-plan.md` §14.1 "Sharpened"): Kommt
ein Nicht-Struct-Value auf einem Struct-Pin an, sät der Interpreter bei
`SetStructField` aus der Definition neu, das Kompilat bleibt bei Nullen. Beide
Seiten sind in sich konsistent. Nicht wieder aufmachen.

**Folgeschritt:** keiner. Memory `savegames-v2-structs-enums.md` korrigieren.

---

## 2. Rename-Retarget: offen

Zwei Lesarten, die beide zutreffen.

### 2a. Asset verschieben/umbenennen (Lesart der Memory)

Typ-Referenzen sind **Asset-Pfade** (content-relativ). `HorizonCode::Value::typeName`,
`StructField::typeName` und der `TypeRegistry`-Key sind derselbe String
(`src/HE_Core/include/Types/TypeRegistry.h:12-14`). Der Retarget
(`AssetRefRetarget.cpp`) ersetzt jeden JSON-String, der dem alten Pfad gleicht.
Grundsaetzlich erfasst er typeNames also, er erreicht aber diese Stellen nicht:

1. **Save-Dateien der Spieler: haertester Fund.** `save.write` schreibt
   `"template": <Pfad des SaveGameTemplate>` (`src/HE_Scene/src/EngineApi.cpp:4568`),
   `save.load` loest ihn auf und **bricht ab**, wenn er fehlt
   (`EngineApi.cpp:4546` → `resolveTemplate`, `:4297`). Die Saves liegen in der
   Sandbox (`Saves/<id>.json`), die kein Retarget erreicht und die im
   ausgelieferten Spiel beim Spieler liegen. Verschiebt der Entwickler das
   Template, **laedt kein bestehender Spielstand mehr**. Verschachtelte Structs
   im Save tragen zwar `__type` (`EngineApi.cpp:4361`), gelesen wird aber ueber
   den `typeName` des Schema-Felds (`:4426-4436`). Das haengt also an Punkt 2.
2. **STDF/SGTP-Chunks werden nicht als JSON behandelt.** `isJsonChunk`
   (`AssetRefRetarget.cpp:31-36`) kennt CHUNK_STDF/ENDF/SGTP nicht. Als
   Binaerchunk ist reines JSON ab Offset 0 unsichtbar (kein Laengenpraefix,
   `enclosingJsonAt` findet keinen Anfang). Der Referenz-*Scan* weiss das und
   nimmt sie absichtlich auf (`AssetRefScan.cpp:24-34`, Kommentar nennt die
   Retarget-Luecke, eingefuehrt mit `8a092424`). Schaden: verschachtelte
   Struct-/Enum-Feld-`typeName` in **STDF** und Template-Feld-`typeName` in
   **SGTP** zeigen nach einem Move ins Leere. Das Schema-Feld hat dann keine
   Definition, `makeDefaultValue` liefert einen leeren Value, und gespeicherte
   Struct-Daten fallen still weg. ENDF selbst enthaelt keine Pfade, es mit
   aufzunehmen ist nur Symmetrie.
3. **`.hescene` wird nicht gewalkt.** `retargetTree` nimmt nur `*.hasset`
   (`AssetRefRetarget.cpp:267`), dazu `.heproj` (`ContentManager.cpp:2407-2455`).
   Das Level-Script liegt als JSON in der Szene (`SceneSerializer.cpp:935`,
   `"levelScript"`). Struct-/Enum-Variablen und -Knoten im Level-Script behalten
   den alten `typeName`. Der Editor-Queue (`EditorApplication.cpp:9633-9645`)
   walkt ebenfalls keine Szenen. (Klassen-Graphen in `.hasset`/HCGR sind dagegen
   abgedeckt.)
4. **In-Memory-Rekey ohne Typ-Assets.** `retargetAssetReferencesInMemory`
   (`ContentManager.cpp:2467, 2511-2520`) zieht `path` fuer 19 Slot-Maps nach,
   `m_structTypeAssets`, `m_enumTypeAssets` und `m_saveTemplateAssets` fehlen
   (auch `m_themeAssets`, `m_boneMaskAssets`). Ein offenes Typ-Asset wird beim
   naechsten Speichern an den alten Ort geschrieben (dieselbe Klasse Fehler,
   die der Kommentar dort beschreibt).
5. **`TypeRegistry` bleibt unter dem alten Pfad.** Nach einem Move bleibt der
   alte Key registriert, der neue kommt erst beim naechsten Laden dazu.
   `TypeRegistry::removeType` (`TypeRegistry.cpp:72`) hat **null Aufrufer**
   (auch beim Loeschen nicht). Dropdowns zeigen den Typ doppelt, Codegen und
   Saves loesen bis zum Projekt-Neuladen noch den alten Pfad auf.

### 2b. Feld / Enum-Eintrag umbenennen (Lesart des Themas)

Es gibt keine stabilen Feld- oder Eintrags-IDs. `StructField` und `EnumEntry`
haben nur `name` (`TypeRegistry.h:25-78`), und `TypeAssetPanel.cpp` verfolgt
Umbenennungen nicht.

- **Struct-Feld umbenannt:** Save-Felder und Struct-Werte sind name-keyed
  (`EngineApi.cpp:4552-4555`, `:4432-4435`). Das alte Feld wird ignoriert, das
  neue startet **still auf Default**. Dasselbe gilt fuer `Variable::structDefaults`
  im Graph ("a name the definition no longer has simply doesn't apply").
- **Enum-Eintrag umbenannt:** zwei Kodierungen mit entgegengesetzten Schwaechen.
  - Save-Datei: `v.i` als Zahl (`EngineApi.cpp:4347`, `:4401`), also sicher gegen
    Umbenennen, **unsicher gegen Umnummerieren**.
  - Graph-JSON / Variablen-Defaults / Feld-Defaults: Eintrags-**Name**
    (`HorizonCode.cpp:1698`, `TypeRegistry.cpp:252/308`), also sicher gegen
    Umnummerieren, **unsicher gegen Umbenennen**. Ein `findEntry`-Fehlgriff faellt
    still auf den ersten Eintrag zurueck.
  - Lua/Python (`horizon.enums.X.Y`) nennen den Namen im Quelltext. Das ist nicht
    automatisch retargetbar, ein Fall fuer eine Warnung.

### Vorgeschlagene Umsetzungsschritte

- **2-A (klein, hoher Nutzen):** CHUNK_STDF/ENDF/SGTP in `isJsonChunk` des
  Retargets aufnehmen. Typ-Slot-Maps in `retargetAssetReferencesInMemory`
  aufnehmen. `TypeRegistry` beim Move umschluesseln (alten Key entfernen, neuen
  eintragen) und `removeType` beim Loeschen aufrufen. `.hescene` in
  `retargetTree` aufnehmen (JSON-Pfad, analog `.heproj`). Tests in
  `test_asset_ref_retarget.cpp`.
- **2-B (Spieler-Saves):** Das Save darf nicht allein am Template-Pfad haengen.
  Beim Schreiben die Template-**UUID** mitschreiben (`templateId`). Beim Laden
  zuerst ueber den Pfad aufloesen, bei Fehlgriff ueber die UUID
  (`ContentManager` kennt UUID → Pfad, auch im Pak). Alte Saves ohne UUID
  bleiben auf dem Pfadweg.
- **2-C (Feld-/Eintrags-Renames):** optionale Alias-Liste pro Feld/Eintrag
  (`formerNames`), die das TypeAssetPanel beim Umbenennen automatisch fuellt.
  Lesen: `findField`/`findEntry` und die name-keyed Loader fallen auf Aliase
  zurueck (Save, `structDefaults`, Graph-Enum-Defaults). Alternativ stabile IDs,
  das ist aber ein Formatwechsel mit groesserer Kompatibilitaets-Geschichte.
  Alias ist der kleinere, rueckwaertskompatible Schritt.

---

## 3. Script-Var-Capture: offen

`entity.saveState` erfasst nur `transform` und `visible`
(`src/HE_Scene/src/EngineApi.cpp:422-439`, `SaveStateComponent.h:18-22`).
Seit `628838ae` unveraendert. Skriptzustand einer Entity geht verloren.

Was es pro Entity an Skriptzustand gibt (`ScriptComponent` zeigt auf **genau
eines**, `EntityHost.cpp:28-36`):

- **HorizonCode-Klasse** (EntityHost-Bindung): Instanzvariablen, lesbar und
  schreibbar ueber `Runtime::getVariable/setVariable(InstanceId, name)`
  (`HorizonCodeRuntime.h:157-158`), fuer interpretiert **und** kompiliert.
  Instanz ueber `api::entity::instance` (`EngineApi.cpp`, `EntityHost::instanceOf`).
- **Lua/Python-Skript** (ScriptContext): `IScriptBackend` kann nur
  `injectProperties` (schreiben vor `onStart`), **kein Zurueklesen**
  (`src/HE_Core/include/Scripting/IScriptBackend.h:167-170`). Das ist der
  eigentliche Mehraufwand.

**Vorlage fuer HC ist schon da: das `replicated`-Flag.** Es laeuft durch genau
die Stellen, die ein `saveGame`-Flag braucht:
- `Variable::replicated` (`HorizonCode.h:469`), JSON `"rep"` (`HorizonCode.cpp:1864`, `:1996-2003`)
- `CompiledVarInfo::replicated` (`HorizonCodeCompiled.h:37`), `VarSlots` (`HorizonCodeGenSupport.h:762/794/894`)
- Codegen `HcCodegen.cpp:3288`
- Runtime-Aufzaehlung fuer beide Pfade (`HorizonCodeRuntime.cpp:380-391`)
- UI-Haken `LevelScriptPanel.cpp:740`

### Vorgeschlagene Umsetzungsschritte

- **3-A (HC):** `Variable::saveGame` + `CompiledVarInfo`/`VarSlots`/Codegen
  analog `replicated`, `Runtime::savedVariables(id)`. `SaveStateComponent`
  bekommt `saveScriptVars` (Default an). `saveState` schreibt
  `"vars": { name: valueToJson }` (bestehender Save-Codec, Struct/Enum/Container
  schon drin), `applySavedState` setzt per `setVariable`, name-keyed und partiell.
  Ref-Variablen ausschliessen (wie bei `replicated`). Paritaets-Fixture fuer
  interpretiert und kompiliert. Haken im Variablen-Panel plus Doku.
- **3-B (Lua/Python):** `IScriptBackend::readProperties(id) → map` (Default
  leer). Lua und CPython lesen die in `M.properties` deklarierten Felder der
  Instanz zurueck. `saveState`/`applySavedState` nutzen `readProperties` /
  `injectProperties`. Nur deklarierte Properties, kein freies Table-Dumping.
- **3-C (optional):** C++-Projekte (`he::entity::*` in `HorizonGameServices.h`)
  bekommen denselben Weg nur, wenn jemand ihn braucht. Die C-ABI-Tabelle ist
  versioniert, ein Feld dazu heisst eine neue Version.

## Nachtrag Schritt 2 (26.09.): 2-C umgesetzt

Feld- und Eintrags-Renames laufen jetzt ueber Aliase (`formerNames`), wie
vorgeschlagen. Stand:

- **Modell/Codec:** `StructField::formerNames`, `EnumEntry::formerNames`
  (`TypeRegistry.h`). In STDF/ENDF/SGTP nur geschrieben, wenn nicht leer, also
  bleiben nie umbenannte Definitionen byte-gleich. Beim Schreiben fallen leere,
  doppelte und Aliase weg, die gleich einem lebenden Namen sind.
- **Eine Regel fuer alle Mehrdeutigkeiten:** ein lebender Name gewinnt immer.
  "x→y umbenannt, dann neues x angelegt" gibt alte x-Daten an das neue x.
- **Leser:** `findField`/`findEntry` suchen erst den aktuellen Namen, dann die
  Aliase. `StructDef::storedKey` liefert fuer name-keyed Speicher den Schluessel
  (aktueller Name, sonst neuester vorhandener Alias). Genutzt in `save.load`
  (Template-Felder), im verschachtelten Struct-Decoder des Saves, bei
  `structDefaults` (Interpreter + Override-Editor). Enum-Defaults in Graphen,
  Feld-Defaults, Codegen und C++-Header gehen ueber `findEntry` von selbst mit.
- **Retarget (Zurueckschreiben):** Spielstaende schreiben beim naechsten
  `save.write` die aktuellen Namen. Graphen: `syncTypeSignatures` schreibt
  beim Laden Variablen-Enum-Defaults, `defaultItems`/`defaultKeys` und
  `structDefaults`-Schluessel auf die aktuellen Namen um, Get/Set Struct Field
  folgt dem umbenannten Feld, und der Link-Remap nimmt Aliase mit, sodass ein
  Draht auch dann bleibt, wenn im selben Edit ein anderes Feld wegfiel.
- **Erzeuger:** TypeAssetPanel bucht beim Speichern jede Zeile, deren Name vom
  zuletzt gespeicherten abweicht (indexparallele Liste, Zeilen werden nie
  umsortiert), zeigt "Formerly: ..." an. MCP: `type_field_set` und
  `type_enum_set` haben `renameFrom`.
- **Tests:** `test_type_registry.cpp` (Codec, Lookups, Schatten-Regel, Ketten,
  Enum-Default ueber alten Namen), `test_engine_api.cpp` ("a save written before
  a field rename...", mit Negativkontrolle ohne Aliase und Rueckschreib-Pruefung),
  `test_horizoncode_types.cpp` (Graph laedt nach Rename+Loeschen), 
  `test_mcp_tools_type.cpp` (`renameFrom`).

**Weiter offen:**
- 2-A und 2-B (Asset verschieben/umbenennen, Template-UUID im Save). Das ist
  die andere Lesart von Luecke 2 und in diesem Schritt nicht angefasst.
- Enum-Umnummerieren: Saves speichern Enum-Werte als Zahl, ein Umnummerieren
  verschiebt Daten weiterhin. Unabhaengig vom Umbenennen.
- Skriptquelltext (`horizon.enums.X.Alt`, Lua/Python-Tabellen mit alten
  Feldnamen, `save.get("alt")`) wird nicht umgeleitet. Bewusst: `save.get`
  mit altem Namen bleibt ein lauter Fehler.

## Empfohlene Reihenfolge

2-A → 2-B → 3-A → 2-C → 3-B. 2-A ist der kleinste Schritt mit dem groessten
Schutz. 2-B verhindert den einzigen Fall, der beim Spieler kaputtgeht. 3-A hat
eine fertige Vorlage.
