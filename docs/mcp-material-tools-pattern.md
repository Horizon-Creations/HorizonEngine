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

## 6. Draht-Prüfung: die Tools über den echten MCP-Server aufrufen

Stand 21.09.2026, Schritt 4. Die Testfälle rufen Handler im Prozess; was ein
Client sieht (`content`/`isError`/`structuredContent` aus `McpBridge`, durch
`scripts/he_mcp.py` gereicht), prüft nur der Weg über den Socket. Der Editor
kann ihn ohne geöffnetes Projekt nicht liefern (alles wäre `no_project`),
deshalb gibt es in `tests/test_mcp_tools_material.cpp` den per Umgebung
geschalteten Fall „serve to a client": echte `McpBridge` + echtes
`registerMaterialTools` auf einem Temp-Root (Param-Fixture, Funktion, Stub,
optional EngineContent unter `Engine/`), gepumpt bis eine Stopp-Datei kommt.

```sh
EP=/tmp/he_mcp/mcp-endpoint.json; mkdir -p /tmp/he_mcp
HE_MCP_SERVE_MATERIAL=$EP HE_MCP_SERVE_SECONDS=150 \
HE_MCP_SERVE_ENGINE_CONTENT=build/src/HE_Editor/EngineContent \
  build/tests/he_tests -tc='mcp material tools: serve to a client*' &
# dann wie ein Client: initialize → tools/list → tools/call, zeilenweise
# JSON-RPC auf stdin/stdout von
python3 scripts/he_mcp.py --endpoint $EP
touch $EP.stop            # beendet den Server, der Testfall meldet SUCCESS
```

Mit laufendem Editor geht derselbe Weg ohne Testbinary: `HE_MCP=1` (und
`HE_MCP_PORT`) schaltet die Bridge ein, ohne die Config anzufassen; die
Endpoint-Datei liegt dann in `GlobalState::userDataDir()`.

Was der Draht am 21.09.2026 gezeigt hat (alle 5 Tools, 14 Fehlerfälle):

* Fehler kommen als `isError:true` mit `structuredContent:{code,message}`,
  der Text-Block ist dieselbe JSON, so wie `McpBridge.cpp` es verspricht.
* `additionalProperties:false` ist nur Schema: die Bridge validiert nicht,
  ein unbekanntes Argument wird ignoriert, `path: 42` liest als fehlend
  (`strArg`-Fallback wie bei hc).
* Fehlendes `path`: `material_info`/`material_graph_info` sagen
  `invalid_path` (gemeinsames `checkPath`, `McpToolCommon.cpp`),
  `material_create` sagt `invalid_payload` (eigene Vorprüfung). Beides
  deckt §2.5, innerhalb der Familie ist es uneinheitlich.
* `nodes[].type` trägt Enum-Namen (`ParamColor`), `output.pins[].chain`
  Anzeigenamen (`Param (Color) #2 (Tint)`). Ein künftiges
  `material_add_node` muss `type` gegen `nodes[].type` bzw. ein
  `material_node_types` prüfen, nie gegen `chain`.
* Die Listenform trägt `{path, type, loaded}`; `kind`/`parent`/`paramCount`
  nur für bereits residente Materialien, weil die Liste nichts lädt.

## 7. Graph-Editoren: `material_node_types`, `material_add_node`, `material_remove_node`

Stand 21.09.2026, Schritt 5. Das Gegenstück zu `hc_node_types`/`hc_add_node`/
`hc_remove_node`, in `McpToolsMaterial.cpp` hinter `material_graph_info`.
Die Familie hat damit 8 Werkzeuge. Was vom hc-Muster übernommen ist
(Volles-Objekt-nach-Mutation, `refused_by_policy` für „das Add-Menü bietet
es nicht an", `not_found` für eine fremde Id, `removedLinks` als Diff der
Link-Liste vor/nach `removeNode`), steht in §2. Hier nur, was neu ist.

### 7.1 Gemeinsame Helfer, für Schritt 6/7 gedacht

* `openGraphForEdit(cm, h, args)` → `GraphEdit { Mat m; MaterialGraph g; ok;
  failure }`. Reihenfolge: `checkPath` → Funktions-Sniff (`invalid_path`,
  eigener Text) → `openMat(forWrite)` (alle Gates aus §2.3) → Instanz
  (`no_graph`, nennt den Parent) → leeres `nodeGraphJson` (`no_graph`,
  nennt `material_create`) → Parse-Fehler (`failed`).
* `commitGraph(cm, h, m, g, out)`: **Trockenlauf** `generateFragment` auf
  dem geänderten Graphen, leeres GLSL = `failed` ohne Schreiben (der
  Regenerate-Pfad kehrt bei leerem GLSL still zurück und hätte sonst einen
  Graphen in die Datei gelegt, zu dem der Shader nicht passt). Dann in der
  Reihenfolge von `material_set_param`: Graph-JSON ins Asset →
  `regenerateMaterialFromGraph` → `syncMaterialInstancesOf` → `saveAsset` →
  `reloadFromDisk`. Ergänzt `path`, `nodeCount`, `linkCount`, `paramCount`,
  `syncedLoadedInstances`, `reloadedInEditor`.
* `nodeTypeByName` (Enum-Name **oder** Anzeigename, beide eindeutig),
  `masterTypeRefusal` (= `MaterialEditorPanel::listed` minus FunctionCall),
  `hasSlotNow` (hat das gespeicherte Material einen Slot dieses Namens).

### 7.2 Entscheidungen

| Frage | Entscheidung | Warum |
|---|---|---|
| Material-**Funktionen** beschreibbar? | **Nein**, `invalid_path` mit eigenem Text | FnInput/FnOutput sind die Pin-Liste jedes `FunctionCall` in jedem Aufrufer; Links dort sind nach Pin-**Index** gespeichert, nichts auf Material-Seite räumt sie auf. `openMat` bleibt bei „forWrite nie mit Funktion". Braucht einen Aufrufer-Sweep, eigener Schritt, Entscheidung beim Chefchen. |
| Instanz / Stub | neuer Code **`no_graph`** | Analog `no_params`; `invalid_path` wäre falsch, der Pfad ist ein Material. |
| `Output` anlegen/löschen, `FnInput`/`FnOutput` auf Master | `refused_by_policy` | Exakt `typeExcluded` bei hc: das Panel bietet es nicht an. |
| `FunctionCall` anlegen | ja, `s` Pflicht (`invalid_payload` leer, `checkPath`-Failure durchgereicht, falscher Typ `invalid_path`) | Das Panel bietet ihn über die Funktionsliste an, nicht über die Typliste; hier ist `s` die Liste. |
| `TextureSample`/`NormalMapSample` `s` | leer erlaubt (= Mesh-Textur), sonst `checkPath` + Sniff `Texture` | Wie das Kontextmenü „(mesh texture)". |
| `p` | 1–4 Zahlen, `invalid_payload` bei `paramCount == 0` | `ParamFloat` hat `paramCount 1`, nutzt aber `p[1]/p[2]` als Range; darum nicht auf `paramCount` gekappt. Dazu `min`/`max` (nur ParamFloat, min < max) und `group`/`tooltip` (nur Param-Knoten). |
| Anfangswerte im Katalog | `defaults {p, s}` aus einem Scratch-`MaterialGraph::addNode` | Kein Abschreiben der Tabelle, kann nicht driften. |
| Param-Knoten ohne Draht | `parameter.hasSlot: false` + `note` | Der Codegen läuft nur vom Output aus; ein unverbundener Param-Knoten hat keinen Slot, `material_set_param` kann ihn nicht setzen. Ehrlich melden statt „tunbar" behaupten. |
| Kommentar-Id als Node-Id | `not_found`, Text nennt „comment box" | Kommentare teilen den Id-Zähler, sind aber keine Knoten. |

### 7.3 Antwortform

* `material_node_types` (ohne Pflichtargument; `path` filtert wie das
  Add-Menü des Assets): `{ scope, nodeTypes[ { type, displayName, category,
  inputs[{pin,name,type,default}], outputs, paramCount, defaults{p,s},
  paramKind?, functionOnly?, dynamicPins?, requires? } ], functions[ { path,
  loadable, inputs, outputs | note } ], functionsTruncated }`. Die Typliste
  lädt nichts und wird nie gekappt; die Funktionsliste lädt jede gemeldete
  Funktion (Interface = ihr Graph), `limit` default 80. `Output` ist nie
  gelistet.
* `material_add_node` → `{ type, node (wie `graph_info`, Pins aufgelöst),
  parameter{name,kind,hasSlot}?, note?, …commitGraph }`.
* `material_remove_node` → `{ removed, removedType, removedLinks[],
  parameter{name,kind,slotRemoved}?, …commitGraph }`. `slotRemoved: false`,
  wenn ein zweiter Knoten desselben Namens bleibt.

### 7.4 Tests (`tests/test_mcp_tools_material.cpp`, 7 Fälle)

Katalog = Registry minus Output mit Editor-Defaults; jeder gelistete Typ geht
durch `add_node` und steht unter seiner Id in der Datei (frischer
`ContentManager`); Payload-Prüfungen mit 19 Ablehnungen, Datei byteweise
unverändert; `remove_node` auf „Metal" der Param-Fixture nimmt Link, Slot
(`material_info`, `codegenValueOf`, Output-Pin unverbunden) **und den Slot
der geladenen Instanz** mit; geteilter Name behält den Slot; Gates für beide
Editoren (play/lock/dirty/`no_graph`×2/Funktion/Engine/sauberer Tab →
`reloadedInEditor`); unter `HE_TESTS_HAVE_SHADERC` ein editiertes
`OpaquePBR`-Template (Roughness raus, Fresnel rein) durch Metal + GLSL410.

## 8. Drähte: `material_connect`, `material_disconnect`, `material_set_pin_default`

Stand 21.09.2026, Schritt 6. Das Gegenstück zu `hc_connect`/`hc_disconnect`/
`hc_set_pin_default`, in `McpToolsMaterial.cpp` hinter `material_remove_node`,
auf den Helfern aus §7.1 (`openGraphForEdit`, `commitGraph`). Die Familie hat
damit 11 Werkzeuge.

### 8.1 Was der Material-Graph anders kann als HorizonCode, und was daraus folgt

| Befund im Code | Folge für das Tool |
|---|---|
| `MaterialGraph::connect` prüft nur Richtung, Pin-**Bereich** und Selbstverbindung, **keine Typen** (`MaterialGraph.cpp:399`). Die Codegen `coerce`t jede Paarung (Float splattet hoch, vec4→vec3 lässt w fallen). Der Canvas nimmt jeden Draht an. | Typ-Mismatch ist **keine Ablehnung**. hc's `viaConversion` wird zu `coercion: "vec3 -> float"` (oder `null`); `allowCoercion: false` ist hc's `allowConversion: false` und lehnt mit `failed` ab. Es gibt keinen Konversionsknoten, der gespawnt werden könnte. |
| `connect` kennt keinen Zyklus-Schutz; die Codegen malt einen Zyklus **magenta** statt zu crashen (`emitNode`, `c.emitting`). | Der Trockenlauf in `commitGraph` fängt einen Zyklus **nicht** (GLSL ist nicht leer). Deshalb eigener Test `reaches(g, dst, src)` stromabwärts; Treffer = `failed`, Datei unangetastet. Der Canvas lässt den Zyklus zu, ein Client sieht aber kein Magenta. |
| Der Output-Pin `kMatOutputOpacityPin` (5) existiert in der Registry auf jedem Blend-Mode; ein opakes Material zeichnet und wertet ihn nicht aus. `connect` nähme ihn trotzdem (Range-Check). | Pins werden gegen **`resolvePins`** aufgelöst (die Liste, die der Canvas zeichnet und `graph_info` meldet), nicht gegen `connect`s Range. Opaque: Index 5, „Opacity" und „OpacityMask" → `invalid_payload`. Masked: „OpacityMask", Translucent: „Opacity". |
| `FunctionCall`-Pins sind dynamisch; `connect` akzeptiert dort **jeden** Index. Eine nicht ladbare Funktion hat auf dem Canvas keine Pins. | Ladbare Funktion: Pins = ihr Interface (`matFunctionPins`), Name **und** Index. Nicht ladbar (`FnGraphs::missing`): `invalid_payload` „could not be loaded". |
| Ein Eingang hält genau einen Link (`connect` ruft `disconnectInput`). | `replacedLinks` (Schlüssel wie hc) hat 0 oder 1 Eintrag. `material_disconnect` braucht nur `dstNode`+`dstPin`; `srcNode`/`srcPin` sind **optional** und werden, wenn gegeben, gegen den tatsächlichen Draht geprüft (`not_found` nennt die echte Quelle). Halb angegeben = `invalid_payload`. |
| **`MatGraphNode` hat keine Per-Pin-Defaults** (kein `pinDefaults` wie `HC::Node`); ein unverdrahteter Pin liest `MatPinDesc::def` der Registry; der Material-Canvas registriert kein `pinHasInlineEditor` (nur `HcGraphHost`). | `material_set_pin_default` **materialisiert den Default als Const-Knoten**: Float→`ConstFloat`, Vec2→`ConstVec2`, Vec3→`ConstColor`, Vec4→`ConstVec4`, links vom Zielknoten, eine Zeile pro Pin versetzt, per Draht in den Pin. Genau das, was die hc-Beschreibung als Alternative nennt („spending a literal node on it"), und ohne Format-/Codegen-Änderung (Thema schließt die aus). Für den Client identischer Effekt: dieser Pin liest jetzt X. |

### 8.2 Regeln von `material_set_pin_default`

* **Idempotent.** Hängt am Pin bereits ein Const-Knoten *genau des Typs, den
  das Tool für den Pin anlegen würde*, und speist der **nur diesen Pin**
  (`ownConstantInto`), wird er in place aktualisiert: `created: false`, gleiche
  Id, `nodeCount` unverändert. Sonst würde jeder Aufruf einen Waisen erzeugen.
* **Der Draht gewinnt.** Ist der Pin an irgendetwas anderes verdrahtet (Param,
  Math, ein Const anderen Typs, ein Const mit Fan-out), `failed` mit dem
  hc-Satz: erst `material_disconnect`. Gilt auch für `value: null`.
* **`value: null`** entfernt den eigenen Const-Knoten (`removedNode`, `pin.default`
  = was der Pin jetzt liest); ohne eigenen Const `{cleared:true, changed:false}`
  **ohne** `commitGraph` (hc kehrt vor `beginEdit` zurück).
* **Form aus dem Pin-Typ**, wie `material_set_param` aus dem Kind: Float → Zahl,
  vec2/vec3/vec4 → Array genau dieser Länge; sonst `invalid_payload` mit der
  erwarteten Form.

### 8.3 Antwortform

* `material_connect` → `{ connected, changed, link{srcNode,srcPin,srcName,
  dstNode,dstPin,dstName}, srcType, dstType, coercion|null, replacedLinks[],
  parameter{name,kind,hasSlot}?, …commitGraph }`. Derselbe Draht noch einmal:
  `changed: false`, nichts geschrieben. `parameter` nur, wenn die Quelle ein
  Param-Knoten ist — schließt die `note` aus `material_add_node` („noch nicht
  Richtung Output verdrahtet").
* `material_disconnect` → `{ disconnected, link, fallback{pin,name,type,default},
  parameter{…,hasSlot}?, …commitGraph }`.
* `material_set_pin_default` → `{ changed, created, pin{node,pin,name,type},
  constantNode (wie `graph_info`), …commitGraph }` bzw. `{ cleared, changed,
  removedNode, pin{…,default}, …commitGraph }`.
* Pin-Adresse überall: Integer-Index **oder** exakter Name, beides wie
  `material_graph_info` sie meldet. Ablehnung nennt alle Pins der Seite
  („'A' (0), 'B' (1)"). Leerer Name (Reroute-Pins heißen „") → Index nehmen.

### 8.4 Tests (`tests/test_mcp_tools_material.cpp`, 7 Fälle + 1 unter SHADERC)

Draht per Name und Index auf die Param-Fixture, `replacedLinks` = der
verdrängte Param-Draht, `codegenValueOf("Rough")` verliert und gewinnt den
Slot wieder, Koerzion vec3→float gemeldet und mit `allowCoercion:false`
abgelehnt (Bytes gleich); dynamische Pins (Opaque lehnt 5/„Opacity"/
„OpacityMask" ab, Masked nimmt „OpacityMask", `FunctionCall` per „Out",
Geister-Funktion abgelehnt); Zyklus direkt und über drei Knoten, Selbstdraht,
falsche Seite, 13 Payload-Ablehnungen; `disconnect` mit `fallback`, Slot weg
aus `material_info`+Codegen, falsche/halbe Quelle; `set_pin_default` Const
anlegen → in place → Fan-out macht ihn fremd → nach `disconnect` wieder eigen
→ `null` entfernt; Gates für alle drei (play/lock/dirty/`no_graph`×2/Funktion/
`not_found`/Engine/sauberer Tab → `reloadedInEditor` ×3); unter
`HE_TESTS_HAVE_SHADERC` ein umverdrahtetes `OpaquePBR` (BaseColor→Metallic
koerziert, Roughness als Const, Normal als ConstColor) durch Metal + GLSL410.

## 9. Knoten ändern: `material_set_node`

Stand 21.09.2026, Schritt 7. Das Gegenstück zu `hc_set_node`, in
`McpToolsMaterial.cpp` hinter `material_set_pin_default`, auf den Helfern aus
§7.1. Die Familie hat damit 12 Werkzeuge; ein Material-Graph ist per MCP jetzt
von Grund auf baubar (create → add_node → set_node → connect /
set_pin_default → remove_node), wie ein HC-Graph per hc_-Tools.

### 9.1 Bewusste Abweichungen vom hc-Vorbild

| hc_set_node | material_set_node | Warum |
|---|---|---|
| nimmt das **ganze Node-Objekt** (`node: {…}`) zurück | **Patch** aus den Feldern von `material_add_node`: `s`, `p`, `min`/`max`, `group`, `tooltip`, `position`, dazu `blendMode` (nur Output) | `HC::Node` ist groß und heterogen (Signatur, Pin-Defaults, Payloads), ein `MatGraphNode` sind sechs Felder, und `add_node` buchstabiert sie schon als Argumente. Ein Client, der `add_node` kennt, kennt `set_node`. Ein mitgeschicktes `type` wird angenommen, wenn es dem Ist-Typ entspricht (Echo von `graph_info`), sonst `invalid_payload`. |
| **Upsert**: fremde Id wird angelegt | **`not_found`** | `material_add_node` legt an; ein Upsert bräuchte `type` als Pflichtfeld und wäre ein zweites Anlegen. |
| Typwechsel erlaubt (Links jenseits des neuen Pin-Bereichs werden gekappt) | **Typ unveränderbar**, `invalid_payload` mit „remove + add" | Links liegen nach Pin-**Index** in der Datei; ein Multiply, das zum Lerp wird, behielte seine Drähte auf Pins, die jetzt etwas anderes bedeuten. |
| `droppedLinks` nach Typwechsel | `droppedLinks` nach **`s`-Wechsel** auf `FunctionCall` (Pins = Interface der neuen Funktion, aus `resolvePins`) und `LandscapeLayerBlend` | Das ist, was bei Materialien die Pin-Liste ändert. Nicht ladbare Funktion (`FnGraphs::missing`) → `invalid_payload` statt „alle Drähte weg" (Regel von `material_connect`). |
| — | `LandscapeLayerBlend`: Links werden erst per Layer-**Name** neu zugeordnet (`movedLinks {fromPin, toPin, layer}`), erst dann gekappt | `MaterialEditorPanel.cpp` ~547: Layer k entfernen heißt Layer k+1 **wird** k, die Drähte müssen mitrutschen; ein reiner Tail-Prune ließe jede Ebene nach der entfernten am Farbeingang ihres Vorgängers hängen. Über MCP kommt der ganze neue String, also Name-gegen-Name (erster Treffer). Ein alter Pin k **ohne** Namenstreffer behält seinen Draht, wenn Index k in der neuen Liste von keinem alten Namen belegt ist: das ist ein Rename, und das Panel lässt einen Draht beim Umbenennen auf seinem Pin (`renamedLayers {pin, from, to}`). Grass/Rock/Snow → Grass/Snow: Snow belegt 1, Rock fällt; Grass/Snow → Grass/Ice: 1 ist frei, Snows Draht ist jetzt Ices. |
| — | **Output-Knoten** setzbar: `p = [lit 0/1, blendMode 0/1/2, maskCutoff, domain 0/1]`, oder `blendMode` als Name; beide zusammen nur, wenn sie übereinstimmen. Wechsel auf Masked mit Cutoff ≤ 0 → 0.5 (`maskCutoffDefaulted`), wie der Tab-Header. | `add_node` macht nie einen Output; das ist der erste MCP-Weg, den Blend-Mode nach `material_create` zu ändern. Bereichsprüfung, weil ein Blend-Mode 7 die Datei still kaputt machte. Der Link in den Opacity-Pin bleibt bei Opaque in der Datei (Indizes sind blend-stabil), der Slot verschwindet. |
| — | Param-Knoten mit geändertem `p` bei **gleichem Namen**: der Slot im Param-Block wird **vor dem Regenerate** geschrieben (`parameter.blockWritten`), über `commitGraph`s neuen `touchAsset`-Hook, der erst **nach** dem Trockenlauf läuft, damit eine Ablehnung auch das Asset im Speicher unberührt lässt | Die dokumentierte Falle dieser Datei (`McpToolRegistry.h`): `regenerateMaterialFromGraph` snapshottet den Block **nach Namen** und stellt ihn wieder her; der Node-Default allein wäre nach dem Regenerate wieder der alte Blockwert. Bei Rename braucht es das nicht, der neue Slot entsteht frisch aus dem Knoten. |
| — | Rename von Param/StaticSwitch: `parameter.renamedFrom` / `switch.renamedFrom` + `note` | Instanz-Overrides (`instanceOverriddenParams`, `instanceSwitchNames`) hängen am Namen und gehen verloren, genau wie nach einem Rename im Material Editor. Ehrlich melden, nicht heilen. |
| schreibt immer | identische Werte → `changed: false`, nichts geschrieben; **kein** Feld → `invalid_payload` | Wie `material_connect` bei demselben Draht. |

### 9.2 Antwortform

`{ changed, node (wie graph_info, Pins aufgelöst), droppedLinks[], movedLinks[]?, renamedLayers[]?,
parameter{name, kind, hasSlot, renamedFrom?, blockWritten?}?, switch{name,
renamedFrom}?, blendMode/lit/domain (Output)?, maskCutoffDefaulted?, note?,
…commitGraph }`; bei `changed: false` nur `{ changed, path, node }`.

### 9.3 Tests (`tests/test_mcp_tools_material.cpp`, 6 Fälle + 1 unter SHADERC)

Wert auf verdrahtetem `ParamFloat` → Node-Default (`codegenValueOf`) **und**
Block (`savedBlockValueOf`) **und** geladene Instanz stimmen überein; gleicher
Wert nochmal → `changed:false`, Bytes gleich; Range/Group/Tooltip/Position in
der Datei, Wert überlebt die Range; Farbe 3 Komponenten; Const-Knoten ohne
`parameter`. Rename Metal→Shine: Master-Layout, Codegen, Instanz-Override weg
(`overridden:false`, folgt dem Parent), StaticSwitch analog. Output: Translucent
→ Opaque nimmt den `Flag`-Slot (Link bleibt in der Datei), Masked ohne Cutoff →
0.5, expliziter Cutoff bleibt, unlit + UI-Domain in `material_info`; 7
Ablehnungen. FunctionCall-Rebind auf Funktion ohne Eingänge → 2 `droppedLinks`,
Ausgangsdraht bleibt, `FnTint` aus der Funktion ist Slot; Ghost/Material/leerer
Stub abgelehnt. Layer-Blend Grass/Rock/Snow → Grass/Snow: Rock-Draht weg, Snow
rutscht 2→1 (`movedLinks`), Waise bleibt Knoten; Grass/Ice: Snows Draht bleibt als Ices (`renamedLayers`); nur Grass: Ices Draht weg. 16
Payload-Ablehnungen + alle Gates (play/lock/dirty/`no_graph`×2/Funktion/
`not_found`/Engine/sauberer Tab → `reloadedInEditor`). Unter
`HE_TESTS_HAVE_SHADERC`: `OpaquePBR` → Masked + unlit + Roughness 0.15 durch
Metal + GLSL410, `gen.blendMode == Masked`.
