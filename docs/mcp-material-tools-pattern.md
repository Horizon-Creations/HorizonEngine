# MCP-Material-Tools: das Muster der hc_-Tools, und was davon für Materialien gilt

Stand 20.09.2026, Schritt 1 von Hive-Thema 72. Eine Notiz, kein Bauschritt.
Alles unten ist gegen den Code auf `claude/mcp-material-tools` (= `main`,
`da8944e5`) abgelesen; `docs/mcp-asset-coverage-plan.md` §1/§2 wurde als
Ausgangspunkt genommen und verifiziert.

## 0. Korrektur der Themen-Prämisse

„Bisher gibt es keinen Zugriff auf Materialien" stimmt nicht mehr. Es gibt
bereits drei Material-Werkzeuge in `src/HE_Editor/McpToolsMaterial.cpp`
(Commit `b32bbf7e`), registriert über `registerMaterialTools` in
`EditorApplication.cpp:7263`, mit 15 Testfällen in
`tests/test_mcp_tools_material.cpp`:

| Werkzeug | mutates | tut |
|---|:-:|---|
| `material_info` | nein | ohne `path`: Liste aller Materialien + Funktionen (lädt nichts); mit `path`: ein Material in voller Parameterform |
| `material_set_param` | ja | Wert eines Parameterslots setzen (Master: Node + Slot; Instanz: Slot + Override-Marke) |
| `material_create_instance` | ja | Instanz eines Masters anlegen |

Was tatsächlich fehlt, ist das, was der Plan skizziert, und beides existiert
weder auf `main` noch auf dem Plan-Zweig
(`claude/mcp-asset-coverage-plan-…`, per `git grep` geprüft):

1. **`material_create`**: Master-Material mit eigenem Graphen aus einer Vorlage
   (Plan §1). Heute liefert `asset_create type=Material` einen META-only-Stub
   ohne Graph, `material_info` meldet `hasGraph: false`, `material_set_param`
   lehnt mit `no_params` ab: die Kette anlegen → einstellen ist unterbrochen.
2. **`material_graph_info`**: Struktur des Graphen (Knoten, Links, Output-Pins)
   lesen, auch für Material-Funktionen und Instanzen (Plan §2).

Das sind die Kandidaten für die Schritte 2 und 3. Der Plan beschreibt beide
bereits in Signatur und Antwortform; diese Notiz wiederholt das nicht.

## 1. Die hc_-Tools: wo, wie viele, lesend oder schreibend

`src/HE_Editor/McpToolsHc.cpp` (1518 Zeilen), `registerHcTools(registry,
McpHcHooks)` aus `McpToolRegistry.h:240`, verdrahtet in
`EditorApplication.cpp:6992`. Tests: `tests/test_mcp_hc.cpp` (20 Fälle).

**13 Werkzeuge, drei lesend, zehn schreibend.** Die Frage aus dem Thema
(„sind die hc-Tools schreibend?") ist damit beantwortet: ja, überwiegend.

| lesend (`mutates=false`) | schreibend (`mutates=true`) |
|---|---|
| `hc_documents`, `hc_get`, `hc_node_types` | `hc_add_node`, `hc_set_node`, `hc_remove_node`, `hc_connect`, `hc_disconnect`, `hc_set_pin_default`, `hc_add_variable`, `hc_set_variable`, `hc_remove_variable`, `hc_save` |

## 2. Das Muster, Punkt für Punkt

### 2.1 Namen

* `<familie>_<verb>[_<objekt>]`, alles klein, Unterstriche, nie Punkte
  (`McpToolRegistry::enforceNameRule`: `^[a-zA-Z0-9_-]{1,64}$`, sonst wird
  die Registrierung abgelehnt).
* Verben: `info`/`get`/`documents`/`types` lesen; `add`/`set`/`remove`/
  `connect`/`disconnect`/`create`/`save` schreiben. `set_<objekt>` ist ein
  Upsert (`hc_set_node`, `hc_set_variable`).
* Die Material-Familie nutzt dieselbe Form: `material_info`,
  `material_set_param`, `material_create_instance`. Neue Namen:
  `material_create`, `material_graph_info` (Plan), später
  `material_add_node`/`material_connect`/… (Plan §2.5, nicht Teil dieses
  Themas).

### 2.2 Registrierung

```cpp
McpTool t;
t.name        = "hc_add_node";
t.mutates     = true;                       // Play-Mode-Gate + Konsolenlog
t.description = "…";                        // siehe 2.5
t.inputSchema = objectSchema(json{ {"key", keyProp()}, … }, { "key", "type" });
t.handler     = [h](const json& args) -> ToolResult { … };
registry.add(std::move(t));
```

* Hooks werden einmal in ein `std::shared_ptr<Hooks>` gehoben und in jede
  Lambda kopiert (Registry ist alleiniger Eigentümer).
* Schema-Helfer aus `McpToolCommon.h`: `objectSchema(props, required)` (setzt
  `additionalProperties:false`), `stringProp(what)`, `numberProp(what)`.
  Argument-Leser: `strArg`, `boolArg`, `intArg`, `numArg`, `hasArg`; falsch
  getypte oder fehlende Argumente lesen als Fallback, werfen nie.
* Die Material-Datei zieht jedes Werkzeug in eine eigene `addXxx(registry,
  content, h)`-Funktion; die hc-Datei registriert inline in einem Block je
  Werkzeug. Beides ist im Repo üblich; für Material bleibt `addXxx`.

### 2.3 Ein Türsteher pro Familie

Alle Gates sitzen in **einer** Funktion, die jeder Handler als Erstes ruft und
deren `failure` er unverändert zurückgibt:

* hc: `openDoc(hooks, args, mutating)` → `Doc { desc, g, ok, failure }`.
  Reihenfolge: `play_mode` (nur mutating) → `key` fehlt → `not_found` →
  `locked_by_other` (nur mutating). `beginEdit` läuft nie für eine abgelehnte
  Anfrage.
* Material: `openMat(content, hooks, args, forWrite)` → `Mat { rel, abs, id,
  ok, failure, …Kopie des Assets… }`. Reihenfolge: `checkPath`
  (`invalid_path`/`not_found`/`read_only`) → Header-Sniff (`invalid_path` für
  Funktion oder Nicht-Material) → nur forWrite: `Engine/`-Ablehnung →
  `play_mode` → `locked_by_other` → `dirty` → Load (`failed`).
  `no_project` prüft der Handler davor selbst (`content.contentRoot().empty()`).

Neue Material-Tools gehen durch `openMat` (bzw. für `material_create`, das
noch keine Datei hat, durch die Wachen von `material_create_instance`,
`McpToolsMaterial.cpp:820-921`, die laut Plan §1.4 in einen gemeinsamen Helfer
gezogen werden sollen).

### 2.4 Rückgabeform

* `ToolResult::ok(json)` mit einem **Objekt** (nie ein nacktes Array), Felder
  camelCase (`nextId`, `apiGroups`, `paramCount`, `reloadedInEditor`).
* Listen tragen `truncated` (hc: Zahl der weggelassenen Knoten; Material/Asset:
  `true` nur wenn gekappt), Default-Limit 200, `limit` Argument mit
  `minimum: 1`.
* **Nach einer Mutation kommt das volle Objekt zurück**, nicht nur `ok`:
  `hc_add_node` → `{ node: {…mit Pins…} }`, `hc_connect` → `{ connected,
  viaConversion, spawnedNodes, replacedLinks, link }`, `material_set_param` →
  Wert wie er jetzt in der Datei steht, `material_create_instance` → Pfad +
  Parameterliste. Grund: der nächste Aufruf soll ohne zweites `get` möglich
  sein.
* Nebenwirkungen werden benannt, nicht verschwiegen (`replacedLinks`,
  `spawnedNodes`, `graphDefaultUpdated`, `reloadedInEditor`).
* UUIDs als `[hi, lo]`-Paar (`uuidJson`), wie `asset_resolve`.
* Enum-Werte als Namen (`"Opaque"`, `"float"`, `"Vec3"`), nie als Zahl.

### 2.5 Fehlerform

`ToolResult::fail(code, message)`; `code` maschinenlesbar, `message` ein
Satz an das Modell, der sagt **was** falsch war **und** welches Werkzeug die
richtige Eingabe liefert („hc_node_types lists every name…", „asset_resolve
reports what a path holds…").

Vokabular, das die beiden Familien heute benutzen (gezählt):

| Code | hc | material | common | Bedeutung |
|---|:-:|:-:|:-:|---|
| `invalid_payload` | 34 | 6 | | Argument fehlt / falsche Form |
| `failed` | 9 | 3 | | Engine-Schicht hat abgelehnt, Log hat den Grund |
| `not_found` | 8 | | 1 | Dokument/Knoten/Datei gibt es nicht |
| `play_mode` | 1 | 2 | | mutierend während PIE |
| `locked_by_other` | 1 | 2 | | Peer hält das Dokument/Asset |
| `refused_by_policy` | 3 | | | Frontend blendet den Knotentyp aus |
| `save_failed` | 2 | | | `hc_save` |
| `invalid_path` | | 4 | 1 | Pfadform, Typ-Sniff, außerhalb Root |
| `read_only` | | | 1 | `Engine/` (via `failEngineReadOnly`) |
| `dirty` | | 1 | | offener Tab mit ungespeicherten Änderungen |
| `no_project` | | 1 | 1 | kein Content-Root |
| `already_exists`, `unknown_param`, `no_params`, `out_of_range` | | je 1 | | materialspezifisch |

Regel für neue Tools: **`invalid_payload`** für Argumentfehler (nicht
`invalid_args`/`invalid_argument`, die §19.3 des Integrationsplans als
gewachsenen Wildwuchs benennt), `invalid_path` für Pfadfragen, sonst die
Tabelle. Eine Ablehnung ist ein No-op: nichts geschrieben, nichts
regeneriert, kein Tab benachrichtigt (Test „every gate refuses without
writing").

### 2.6 Beschreibungstext

Englisch, 3–6 Sätze, Fließtext ohne Aufzählung, und immer diese Bausteine:

1. Was das Werkzeug liefert oder tut, in einem Satz.
2. Woher die Eingaben kommen („'type' is a name from hc_node_types",
   „That parameter list is what material_set_param addresses").
3. Was die Antwort trägt und warum („so the next call can wire it without a
   second hc_get").
4. Bei Lesern: ob geladen wird („The list form loads nothing; the
   single-material form loads the asset (and only it)").
5. Bei Schreibern: was mit dem offenen Tab passiert bzw. dass gespeichert wird.

Argument-Beschreibungen im Schema nennen Beispielwerte
(`'Materials/Rock.hasset'`) und Defaults („default 200").

## 3. Wo Material bewusst NICHT dem hc-Muster folgt

Das ist die Falle für die nächsten Schritte: „wie die hc-Tools" gilt für die
Schnittstelle (2.1–2.6), nicht für die Gates. Die Material-Familie hat in drei
Punkten die Gegenentscheidung getroffen, begründet im Kommentarblock über
`McpMaterialHooks` (`McpToolRegistry.h:620-708`) und in §19.3 Regel 3 des
Integrationsplans:

| | hc | material |
|---|---|---|
| Adressierung | `key` aus `hc_documents`, **nur offene Tabs**; unbekannt = `not_found` | content-relativer `path` durch `checkPath`; funktioniert auf geschlossenen Dateien; `Engine/` read-only |
| Offener Tab | wird **bearbeitet** (`beginEdit`/`endEdit`, Undo-Snapshot wie ein menschlicher Edit), Speichern durch `hc_save` oder den Menschen | schmutziger Tab → `dirty`-Ablehnung; sauberer Tab → `reloadFromDisk` nach dem Schreiben; **kein `material_save`**, jedes Werkzeug schreibt sofort auf die Platte |
| Undo | ja (Tab-Undo) | nein (Content-Schreiber, §19.3 Regel 5) |
| Kollab | Item-Level-Delta über `CollabDocSync` | nur Fremd-Lock-Prüfung + `publishCreate` für neue Dateien |

Neue Material-Tools bleiben bei der **rechten Spalte**. Von hc übernommen wird
nur: Namensform, `objectSchema`, `ToolResult`-Vokabular, Volles-Objekt-nach-
Mutation, `truncated`, Beschreibungsstil, und für `material_graph_info` die
Antwortform von `hc_get` (Knoten mit aufgelöster Pin-Liste + Links, Enum-Namen
statt Anzeigenamen).

## 4. Tests: das Muster

Beide Testdateien bauen die Registry ohne Fenster (`registerXxxTools` mit
Test-Hooks, `ContentManager` auf einem Temp-Content-Root aus `TestFsUtil.h`)
und prüfen je Werkzeug:

* Registrierungstest: `find(name)`, `enforceNameRule`, Schema ist Objekt,
  Beschreibung nicht leer, `mutates` stimmt.
* Jedes Gate refust unter dem erwarteten `errorCode` **und lässt die Datei
  byte für byte** unverändert (Material: „every gate refuses without writing").
* Die Antwort nach einer Mutation trägt das, was der nächste Aufruf braucht.
* Material zusätzlich: die geschriebene Änderung überlebt den Regenerate-Pfad
  des Panels (`materialGraphFromJson` → `generateFragment`), weil das die
  einzige Aussage ist, die das Asset-Round-Trip allein nicht beweist.

Für `material_create` heißt das laut Plan §1.2: jede Vorlage durch
`generateFragment` + `MaterialShaderLibrary` jagen, damit keine ausgeliefert
wird, die magenta rendert. Für `material_graph_info`: Antwort gegen einen
bekannten Graphen (Fixture aus `test_mcp_tools_material.cpp`, `Output ←
Param-Knoten jeder Art`) und gegen eine Material-Funktion prüfen.

## 5. Offen / Klärung „Slots zuweisen"

Das Thema nennt „Slots zuweisen" als möglichen Schreibzugriff. Zwei Lesarten,
beide **nicht** in Schritt 2/3:

* **Mesh-Material-Slots** (welches Material trägt Section 2 eines Static
  Mesh): das ist eine `mesh_*`-Familie, die es nicht gibt (Plan §3.2, Punkt 6
  der Prioritätenliste) und die laut Themenbeschreibung außerhalb liegt.
* **Textur-Slots im Material-Graphen**: das ist Graph-Editing
  (`material_add_node` mit `TextureSample` + explizit verdrahtetem UV-Knoten,
  Plan §2.5), kommt nach `material_graph_info`.

Parameter-Werte setzen (der dritte Sinn von „Eigenschaften setzen") ist mit
`material_set_param` bereits abgedeckt.
