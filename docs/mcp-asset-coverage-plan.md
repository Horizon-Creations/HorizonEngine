# MCP-Asset-Coverage-Plan: Material erstellen, Graph abfragen, und was sonst noch fehlt

Stand 20.09.2026. Ein Plan, kein Bauschritt: hier steht, wie zwei
Material-Werkzeuge aussehen sollen, die es noch nicht gibt, und daneben der
erste Katalog, der die MCP-Abdeckung **pro Asset-Typ** liest statt pro
Werkzeug-Familie. Das Familienverzeichnis und die Regeln, die über alle
Familien gelten, stehen in `docs/mcp-editor-integration-plan.md` §19 und werden
hier nicht wiederholt; §13 dort ist die Vorgeschichte der Material-Werkzeuge
und wird unten an zwei Stellen ausdrücklich revidiert.

Alles Folgende ist aus dem Quelltext abgelesen (`src/HE_Editor/McpTools*.cpp`,
`McpToolRegistry.cpp`, `AssetStubWriter.cpp`, `MaterialEditorPanel.cpp`,
`HE_Core/include/MaterialGraph/MaterialGraph.h`, `Types/Enums.h`,
`HE_Tools/src/AssetImporter/ImporterCommon.h`), nichts aus Erinnerung.

---

## 1. `material_create`: ein Basis-Material mit eigenem Graphen anlegen

### 1.1 Was heute passiert, Schritt für Schritt

Ein Client, der heute ein neues Material haben will, hat genau einen Weg:
`asset_create` mit `type: "Material"`. Der ruft `writeAssetStub`
(`AssetStubWriter.cpp:146`), und für `Material` schreibt der Stub-Writer
**nur den META-Chunk**: kein Graph-Chunk, kein Shader, keine Parameter. Der
Material-Editor fängt das beim Öffnen ab (`MaterialEditorPanel.cpp:448-453`):
ein Material ohne `nodeGraphJson` bekommt im Speicher
`MaterialGraph::makeDefault()` (Output-Knoten + ein ConstColor an Base Color,
`MaterialGraph.cpp:438`), und **nichts davon wird geschrieben, bis ein Mensch
den ersten Knoten anfasst**.

Für einen MCP-Client heißt das: das frisch angelegte Material ist auf der
Platte leer, `material_info` meldet `hasGraph: false` und `params: []`, und
`material_set_param` lehnt mit `no_params` ab. Die Kette „über MCP anlegen,
über MCP einstellen, an ein Mesh hängen" ist an ihrem zweiten Glied
unterbrochen. Das ist der Grund für dieses Werkzeug, nicht ein fehlender
Komfort.

`material_create_instance` (`McpToolsMaterial.cpp:799`) ist davon nicht
betroffen, weil eine Instanz keinen Graphen hat: sie erbt den kompilierten
Shader und die Parameterschicht des Elternteils per `syncMaterialInstance`.
Das erklärt, warum die Instanz zuerst gebaut wurde und der Master nicht.

### 1.2 Die Entscheidung aus §13.7, und warum sie hier revidiert wird

§13.7 und §19.5 des Integrationsplans halten fest: *kein* eigenes
`material_create`, weil „ein zweiter Weg dorthin eine zweite Theorie davon
wäre, was in einer frischen Datei steckt". Die Begründung bleibt richtig, die
Schlussfolgerung nicht: das Problem ist nicht ein zweiter Weg, sondern dass der
**eine** Weg (der Stub-Writer) für Materialien eine Datei erzeugt, die kein
Werkzeug dieser Schnittstelle weiterbenutzen kann. Der Stub für Widgets bekommt
einen leeren 1920×1080-Baum, der für Themes das Shipped-Default, der für
HorizonCode einen leeren Graphen. Der für Material bekommt nichts, und die
Lücke füllt heute ausschließlich das Panel im Speicher.

Es gibt zwei Bauweisen, und nur eine ist mit §19.5 verträglich:

* **(a) Der Stub-Writer bekommt den Default-Graphen, `material_create` ist ein
  dünner Wrapper darüber.** `writeAssetStub` schreibt für `Material` den
  Graph-Chunk von `MaterialGraph::makeDefault()` (bzw. der gewählten Vorlage,
  siehe 1.3). `material_create` wählt die Vorlage, ruft den Stub-Writer, lädt
  das Asset und schickt es durch **denselben Master-Schreibpfad, den
  `material_set_param` heute schon geht** (`McpToolsMaterial.cpp:640-760`):
  `nodeGraphJson` setzen → `regenerateMaterialFromGraph(id)` →
  `syncMaterialInstancesOf(rel)` → `saveAsset` → `reloadFromDisk`-Hook.
  Damit sind Shader, `graphParamNames`, `blendMode`, `domain` und
  `graphTexturePaths` beim Rückkehren gebacken, und die Antwort kann wie bei
  `material_create_instance` die Parameterliste mitliefern, damit der nächste
  Aufruf ein `material_set_param` sein kann.
* **(b) Ein eigenes Werkzeug mit eigenem Graph-Writer.** Das ist genau die
  zweite Theorie, vor der §19.5 warnt, und wird nicht empfohlen.

**Empfehlung: (a).** Nebenwirkung, die gewollt ist: `asset_create` mit
`type: "Material"` und der Content-Browser-Befehl „Material" liefern danach
dieselbe Datei wie `material_create` ohne `template` (Opaque-PBR, 1.3), weil
alle drei durch `writeAssetStub` gehen. Der Material-Editor öffnet so eine
Datei ohne den `makeDefault()`-Fallback, was diesen Fallback nicht abschafft
(alte Dateien und handgeschriebene Shader-Materialien ohne Graph gibt es
weiterhin), ihn aber für neue Dateien überflüssig macht.

Was `regenerateMaterialFromGraph` heute **nicht** tut und das Panel schon:
der Inline-Cross-Compile-Check (`MaterialEditorPanel.cpp:230-236`), die
Komplexitätsschätzung und die Thumbnail-Invalidierung. Der Check ist beim
Anlegen aus einer geprüften Vorlage entbehrlich; die Vorlagen sind endlich
und werden im Test durch `generateFragment` + `MaterialShaderLibrary`
gejagt, damit keine ausgeliefert wird, die magenta rendert.

### 1.3 Die Vorlagen, und warum sie Param-Knoten statt Konstanten tragen

Der Auftrag schlägt „simple Konstanten-Inputs" vor. Für einen menschlichen
Erstanwender im Panel ist das richtig (`makeDefault()` macht genau das).
Für einen MCP-Client ist es die falsche Wahl, aus einem Grund: **ein
ConstColor-Knoten ist über MCP nicht erreichbar, ein ParamColor-Knoten schon.**
`material_set_param` adressiert Slots aus `graphParamNames`, und die entstehen
nur aus Param-Knoten. Eine Vorlage aus Konstanten ist also wieder ein Material,
das der Client nicht einstellen kann, nur diesmal mit `hasGraph: true`.

Deshalb tragen alle Vorlagen die PBR-Eingänge als **benannte Parameter**
(Namen sind die Slot-Namen, die ein Client dann tippt):

| Vorlage (`template`) | Output `p[]` | Knoten → Output-Pin |
|---|---|---|
| `OpaquePBR` (Default) | lit=1, blend=Opaque(0), domain=Surface | `ParamColor "BaseColor"` (0.8, 0.8, 0.8) → Base Color (Pin 0); `ParamFloat "Metallic"` 0, Range 0..1 → Pin 1; `ParamFloat "Specular"` 0.5, 0..1 → Pin 2; `ParamFloat "Roughness"` 0.5, 0..1 → Pin 3; `ParamColor "Emissive"` (0,0,0) → Pin 4 |
| `Masked` | wie oben, blend=Masked(1), cutoff `p[2]`=0.5 | zusätzlich `ParamFloat "OpacityMask"` 1.0, 0..1 → Pin 5 |
| `Translucent` | wie oben, blend=Translucent(2) | zusätzlich `ParamFloat "Opacity"` 1.0, 0..1 → Pin 5 |
| `Unlit` | lit=0, blend=Opaque | nur `ParamColor "Color"` (1,1,1) → Base Color (Pin 0). Der Unlit-Tail schreibt `base + emissive` ohne Beleuchtung (`MaterialGraph.cpp:1301`); Emissive bleibt unverbunden (Default 0) |
| `UserInterface` | lit=0, domain=UserInterface(1) | `ParamColor "Color"` → Base Color; sonst nichts (die UI-Domäne erzwingt den unlit-Tail ohnehin, `MaterialGraph.cpp:1124`) |

Die Pin-Indizes sind `kMatOutput*Pin` aus `MaterialGraph.h`; `p[0]`=lit,
`p[1]`=Blend-Mode, `p[2]`=Mask-Cutoff, `p[3]`=Domäne stehen so im
Registry-Eintrag des Output-Knotens (`MaterialGraph.cpp:38`). Die
ParamFloat-Range wohnt in `p[1]`/`p[2]` desselben Knotens (§13.3), und
genau deshalb wird sie hier beim Anlegen gesetzt: ohne Range ist der
Parameter im Editor ein Zahlenfeld statt eines Schiebereglers, und
`material_set_param` setzt keine Range.

Bewusst **nicht** in den Vorlagen: `Normal` und `Ambient Occlusion`. Ein
`NormalMapSample`-Knoten braucht einen Texturpfad in `s`, sonst sampelt er
nichts Sinnvolles; ein unverbundener Normal-Pin heißt „Vertex-Normale", und
das ist für ein frisches Material die richtige Antwort. Texturen kommen erst
mit den Graph-Schreibwerkzeugen (Abschnitt 2.5) oder mit einem
`textures`-Argument in einer späteren Ausbaustufe (dann `TextureSample` mit
explizit verdrahtetem UV-Knoten: ein `TextureSample`, dessen UV-Eingang
unverbunden bleibt, sampelt konstant (0,0) und ist damit unsichtbar; der
Canvas verdrahtet das beim Drag-Drop von Hand, ein Werkzeug muss es selbst tun).

Die Knoten-Positionen (`x`, `y`) werden gesetzt wie in `makeDefault()`
(Eingänge links in einer Spalte, Output rechts), damit ein Mensch, der das
Material danach im Panel öffnet, kein Knäuel vorfindet.

### 1.4 Signatur und Wachen

```
material_create
  path      string   (Pflicht) content-relativ, z. B. "Materials/Rock.hasset";
                     ".hasset" wird ergänzt wie bei asset_create
  template  string   (optional) OpaquePBR | Masked | Translucent | Unlit | UserInterface
                     Default OpaquePBR
  → { path, uuid, kind: "master", template, blendMode, domain, params: [...] }
```

Die Wachen sind exakt die von `material_create_instance`
(`McpToolsMaterial.cpp:820-895`) und werden in denselben Helfer gezogen, damit
sie nicht zu zwei Kopien werden: `no_project`, `play_mode`, `materialsAllowed`
(Advanced-Shader-Effects-Gate des Projekts; MCP legt nichts an, was das Menü
nicht anbietet), `Engine/`-Ablehnung für das Ziel, `already_exists`,
`locked_by_other`, danach `publishCreate` und `onAssetAppeared`. Kein Undo, wie
bei jedem Content-Schreiber (§19.3 Regel 5).

Nicht Teil dieses Werkzeugs: **`MaterialFunction` anlegen.**
`makeDefaultFunction()` (FnInput → FnOutput) ist der Zwilling von
`makeDefault()` und gehört als zweiter `case` in denselben Stub-Writer-Umbau,
aber ein eigenes MCP-Werkzeug dafür lohnt erst, wenn der Funktionsgraph auch
lesbar ist (2.4).

---

## 2. `material_graph_info`: den Inhalt eines Materials abfragen

### 2.1 Was `material_info` heute sagt, und was es wegwirft

`material_info` mit `path` (`McpToolsMaterial.cpp:485-530`) liefert
`kind`, `hasGraph`, `blendMode`, `domain`, `textures`, `layers`,
`switchOverrides`, die Legacy-PBR-Skalare und vor allem die **Parameterliste**
mit Name/Art/Wert/Range/Gruppe/Tooltip. Es parst dafür den Graphen bereits
(`materialJson`, `McpToolsMaterial.cpp:328-331`), aber nur, um pro Parameter
`inGraph` zu beantworten, und wirft ihn dann weg. Die Struktur, also welche
Knoten es gibt, wie sie verdrahtet sind, was an welchem Output-Pin hängt,
sagt es nicht. Ein Assistent, der ein Material *verstehen* oder gezielt
*ändern* soll, sieht heute nur die Stellschrauben, nicht die Maschine.

### 2.2 Die Abwägung: Zusammenfassung oder Rohgraph

Drei Formen stehen zur Wahl:

1. **Roh-Dump von `nodeGraphJson`.** Billig, aber nutzlos: Knoten-Typen sind
   darin serialisierte Namen (`"Param (Float)"`, `"Texture Sample"`), Pins
   sind nackte Indizes, und der Output-Knoten trägt seine Bedeutung in
   `p[0..3]`. Ein Client müsste `MaterialGraph.h` kennen, um das zu lesen.
2. **Nur eine kompakte Zusammenfassung** („Base Color kommt von Texture
   Sample #7 (Textures/Rock.hasset), gespeist von UV #3"). Lesbar, aber sie
   trägt keine Knoten-Ids und Pin-Indizes, und genau die braucht jedes
   spätere Schreibwerkzeug (`material_connect` muss sagen *welcher* Knoten,
   *welcher* Pin).
3. **Strukturierter Graph plus Zusammenfassung.**

**Empfehlung: Form 3, in einem Werkzeug, mit Schalter für die billige
Hälfte.** Der Ausschlag kommt aus §19.5: Graph-*Editing* über MCP ist dort als
„der nächste große Schritt" benannt, und das Vorbild existiert schon zweimal
in der Registry: `hc_get` liefert Knoten mit aufgelöster Pin-Liste plus Links
und sagt in seiner Beschreibung, dass die Pin-Indizes nur dort herkommen;
`particle_info` liefert ebenfalls Knoten und Links. Ein `material_graph_info`,
das nur zusammenfasst, wäre am Tag, an dem `material_add_node` kommt, sofort
zu erweitern. Die Zusammenfassung ist trotzdem Pflicht, nicht Kür, weil die
häufigste Frage („was tut dieses Material?") sonst 40 Knoten lang ist.

Der Roh-Dump (Form 1) wird **nicht** angeboten: nichts, was der Client damit
könnte, fehlt in Form 3, und er würde das interne Format zur Schnittstelle
machen.

### 2.3 Antwortform

```
material_graph_info
  path          string  (Pflicht) Material ODER Material-Funktion
  summary_only  bool    (optional, Default false) nur "output" + "textures" + "functions"
  → {
      path, kind: "master" | "function",       // Instanzen: siehe unten
      graphVersion,
      output: {                                // nur master
        lit, blendMode, maskCutoff, domain,
        pins: [                                // ein Eintrag je Output-Pin, in kMatOutput*Pin-Reihenfolge
          { pin: 0, name: "Base Color", connected: true,
            source: { node: 7, pin: 0 },       // direkter Vorgänger (Reroutes übersprungen)
            chain: "Texture Sample #7 (Textures/Rock.hasset) ← UV #3",
            constant: [0.8, 0.8, 0.8] | null   // aus matGraphApproxSurface, wenn faltbar
          }, ...
        ]
      },
      interface: { inputs: [...], outputs: [...] },   // nur function: FnInput/FnOutput mit Name, Typ, Default
      nodes: [
        { id, type: "TextureSample",           // Enum-Name, nicht der Anzeigename
          displayName: "Texture Sample", category,
          x, y, p: [..nur paramCount Werte..], s,
          inputs:  [ { pin, name, type, default, connected } ],
          outputs: [ { pin, name, type } ],
          // Param-Knoten zusätzlich: paramName, kind, group, tooltip, min/max
          // FunctionCall zusätzlich: function (Pfad), pins aus matFunctionPins
          // LandscapeLayerBlend zusätzlich: layers
        }
      ],
      links: [ { srcNode, srcPin, dstNode, dstPin } ],
      comments: [ { id, text, x, y, w, h } ],
      textures:  [ "Textures/Rock.hasset", ... ],     // Slot-Reihenfolge wie graphTexturePaths
      functions: [ "MaterialFunctions/Blur.hasset" ],
      switches:  [ { name, default } ],
      warnings:  [ "Output pin 'Opacity' is connected but blend mode is Opaque (ignored)", ... ]
    }
```

Entscheidungen darin:

* **Pin-Namen werden aufgelöst** über `matNodeDesc(type)`; für
  `FunctionCall` über `matFunctionPins` des geladenen Funktionsgraphen, für
  `LandscapeLayerBlend` über `matLandscapeLayerNames(s)`, für den
  Output-Knoten über `matOutputPins(blendMode)`, damit Pin 5 als
  „OpacityMask" statt „Opacity" erscheint, wenn der Modus Masked ist. Das
  sind genau die Funktionen, die auch der Canvas benutzt, also kann die
  Antwort nicht vom Panel abweichen.
* **`type` ist der Enum-Name** (`TextureSample`), nicht der serialisierte
  Anzeigename (`"Texture Sample"`); ein späteres `material_add_node` nimmt
  denselben Namen. Der Anzeigename steht daneben, weil er im Panel steht.
* **`chain`** läuft vom Output-Pin rückwärts, überspringt `Reroute`, nennt
  je Knoten Typ, Id und das Nützlichste aus `s` (Texturpfad, Parametername,
  Funktionspfad), und bricht bei Tiefe 6 oder beim ersten Knoten mit mehr als
  einem verbundenen Eingang mit „…" ab. Mehr ist keine Zusammenfassung mehr,
  dafür gibt es `nodes`/`links`.
* **`constant`** kommt aus `matGraphApproxSurface`, das für Base Color und
  Emissive schon existiert; für Metallic/Roughness sind die Skalare ebenfalls
  drin. Kein neuer Falter.
* **`warnings`** sind die drei Dinge, die der Canvas anzeigt und ein Client
  sonst nicht sieht: Pin verbunden, aber vom Blend-Mode ausgeblendet; mehr als
  `kMatMaxGraphTextures` (4) Texturen (die überzähligen fallen still weg);
  mehr als `kMatMaxParams` (16) Parameternamen (die überzähligen werden als
  Literale gebacken).
* **Instanzen:** `kind: "instance"`, `parent`, und der Graph des
  **Elternteils** mit einem Feld `switchOverrides` obendrauf. Eine Instanz
  hat keinen eigenen Graphen, aber die Frage „was tut mein Material?" ist für
  eine Instanz genauso berechtigt, und die Antwort ist der Graph des Masters
  unter den Overrides der Instanz.
* **Material-Funktionen werden angenommen.** `material_info` lehnt sie mit
  `invalid_path` ab (zu Recht, es geht dort um Parameterslots); hier sind
  FnInput/FnOutput die Schnittstelle, und ohne dieses Werkzeug bleibt eine
  Funktion über MCP für immer unlesbar.

### 2.4 Was es kostet

Die Einzelform lädt das Asset, wie `material_info` (§13.6, Regel 4 in §19.3),
und zusätzlich jede aufgerufene Funktion, um deren Pins aufzulösen; das ist
derselbe Loader, den `regenerateMaterialFromGraph` benutzt. Kein
`generateFragment`-Lauf: für die Antwort braucht es keinen Shader.
`Mat`-Kopie wie heute (der Material-Pool ist ein dichter Vektor, Zeiger
überleben keinen Load; `McpToolsMaterial.cpp:120-130`).

### 2.5 Und danach: der Graph schreibbar

Nicht Teil dieses Plans, aber die Antwortform oben ist so geschnitten, dass
`material_add_node(path, type, x, y, p, s)`, `material_set_node`,
`material_remove_node`, `material_connect(path, srcNode, srcPin, dstNode,
dstPin)`, `material_disconnect`, `material_set_output(path, lit, blendMode,
maskCutoff, domain)` mit den Ids und Pin-Indizes aus `material_graph_info`
auskommen, mit den `hc_*`-Werkzeugen als Vorbild für Namen und Fehlercodes,
und mit dem Master-Schreibpfad aus 1.2 darunter. Der eine offene Punkt dort
ist §13.5: der offene, schmutzige Tab wird abgelehnt, und ein Graph-Edit
durch MCP am **sauberen** offenen Tab geht über `reloadFromDisk`, nicht über
den Undo-Stack des Panels.

---

## 3. Asset-Coverage-Katalog

### 3.1 Was die Registry heute trägt

84 handgeschriebene Werkzeuge (gezählt wie in §19.1): 82 in den 16
`McpTools*.cpp` plus `ping`/`scene_info` in `McpToolRegistry.cpp`, dazu die
dynamischen `api_*`. Pro Datei, mit dem, was sie **liest** (L), **schreibt/ändert** (S)
und **anlegt** (A):

| Datei | Werkzeuge | L | S | A |
|---|---|:-:|:-:|:-:|
| `McpToolsAsset.cpp` | `asset_resolve`, `asset_list`, `asset_create`, `asset_delete`, `asset_move` | Pfad/Typ/UUID/Größe | move/delete | Stub für 16 autorierte Typen |
| `McpToolsScene.cpp` | `scene_save`, `scene_create`, `scene_open` | (über `scene_info`) | save/open | leere Szene |
| `McpToolsEntity.cpp` | `entity_list/get/create/destroy/reparent/set_transform/set_components` | ✓ | ✓ | ✓ |
| `McpToolsPrefab.cpp` | `prefab_info`, `prefab_instantiate`, `prefab_save`, `prefab_instances` | Baum + Komponenten-Keys | platzieren | aus Szenen-Subtree |
| `McpToolsTerrain.cpp` | `terrain_info`, `terrain_heightmap`, `terrain_sculpt`, `terrain_paint` | ✓ | ✓ | – (Entity, kein Asset) |
| `McpToolsMaterial.cpp` | `material_info`, `material_set_param`, `material_create_instance` | Parameter, nicht Graph | Werte | nur Instanz |
| `McpToolsWidget.cpp` | `widget_tree/types/add/remove/move/set_properties/set_anchor/save` | Element-Baum | ✓ | (Stub via `asset_create`) |
| `McpToolsInput.cpp` | `input_bindable/actions/action_set/mappings/mapping_bind/mapping_unbind` | ✓ | ✓ | (Stub via `asset_create`) |
| `McpToolsType.cpp` | `type_info`, `type_field_set/remove`, `type_enum_set/remove` | ✓ Struct/Enum/SaveGameTemplate | ✓ | (Stub via `asset_create`) |
| `McpToolsHc.cpp` | `hc_documents/get/node_types/add_node/set_node/remove_node/connect/disconnect/set_pin_default/add_variable/set_variable/remove_variable/save` | ✓ **nur offene Dokumente** | ✓ | (Stub via `asset_create`) |
| `McpToolsParticle.cpp` | `particle_info`, `particle_set`, `particle_slot_set` | Knoten+Links+Eingänge | Eingangswerte, Mesh/Material-Slot | (Stub via `asset_create`) |
| `McpToolsAnimator.cpp` | `animator_info`, `animator_state_set/remove`, `animator_transition_set/remove`, `animator_param_set/remove`, `blendspace_info`, `blendspace_set`, `blendspace_sample_set/remove` | ✓ | ✓ | (Stub via `asset_create`) |
| `McpToolsClip.cpp` | `clip_info`, `clip_notify_set/remove`, `clip_root_motion_set` | Dauer/Kanäle/Notifies | Notifies, Root-Motion | – (importiert) |
| `McpToolsBuild.cpp` | `project_package`, `project_build`, `project_build_status` | Status | Build/Export starten | – |
| `McpToolsSettings.cpp` | `settings_get`, `settings_set` | .heproj + Prefs | ✓ | – |
| `McpToolsApi.cpp` | `api_list` + `api_*` | Registry | reine Funktionen aufrufen | – |

### 3.2 Die Lücken-Tabelle: alle 26 Asset-Typen

`HE::AssetType` (`Types/Enums.h:85-113`) hat 26 Werte; `Unknown` zählt nicht.
`Shader` wird von der Material-Codegen erzeugt und nie autoriert, hat also zu
Recht kein Werkzeug. Für die übrigen 25:

Legende: **Liste** = erscheint in `asset_list`/`asset_resolve` (das gilt für
alle, wird nicht wiederholt); **Detail** = ein Werkzeug sagt, was *drin* ist;
**Ändern** = Inhalt schreibbar; **Anlegen** = über MCP erzeugbar (Stub = nur
`asset_create`, leer bzw. Default).

| Asset-Typ | Detail | Ändern | Anlegen | Befund |
|---|:-:|:-:|:-:|---|
| **StaticMesh** | – | – | – | **Gar nichts.** Kein Import, keine Bounds/Sections/UV-Statistik, keine Material-Slots (die der Static-Mesh-Editor autoriert, `MeshMaterialSlots.h`). |
| **SkeletalMesh** | – | – | – | **Gar nichts.** Wie StaticMesh; zusätzlich kein Blick auf die Joint-Hierarchie, die `blendspace_*`/`animator_*` implizit voraussetzen. |
| **Texture** | – | – | – | **Gar nichts.** Kein Import, keine Breite/Höhe/Kanäle, keine Cook-Flags (`srgb`, `mipLevels`, `Assets.h:590-592`), die der Importer setzt und die für „Textur sieht falsch aus" die erste Frage sind. |
| **Audio** | – | – | – | **Gar nichts.** Kein Import, keine Länge/Sample-Rate/Kanäle/Format; die Analyse des Audio-Editors (Peak, Clipping, Stille, Loop-Naht) ist unerreichbar. |
| **Font** | – | – | – | **Gar nichts.** Kein Import, keine Größe. Niedrige Dringlichkeit: Fonts werden einmal importiert und dann referenziert. |
| **AnimationClip** | ✓ `clip_info` | ✓ Notifies, Root-Motion | – (importiert) | Autorierte Hälfte gedeckt. Kein Import (kommt mit dem Skinned-glTF), keine Bone-Masken-Zuordnung. |
| **Material** | Parameter ja, **Graph nein** | Werte ja, **Struktur nein** | **nur Instanz**; Master als leerer Stub | Abschnitte 1 und 2. |
| **MaterialFunction** | – (`material_info` lehnt ab) | – | Stub (META-only, wie Material) | Über MCP unlesbar und unschreibbar; 2.3 nimmt sie ins Lese-Werkzeug. |
| **Scene** | ✓ `scene_info` + `entity_*` | ✓ | ✓ | Gedeckt. |
| **Prefab** | ✓ | platzieren | ✓ `prefab_save` | Gedeckt; keine Vererbung/Overrides (bekannte Engine-Lücke, §14.7). |
| **Script** (Lua/Python) | – | – | Stub mit Starter-Template | **Quelltext ist weder lesbar noch schreibbar.** `ScriptAsset::sourceCode` ist ein String; das Panel ist ein Texteditor. Für einen Assistenten die naheliegendste Arbeit überhaupt, und die einzige Skript-Sprache mit MCP-Zugang ist heute HorizonCode. |
| **HorizonCodeClass** | ✓ **nur offene Tabs** | ✓ nur offene Tabs | Stub | `hc_documents` lässt „eine Klasse, die nie geöffnet wurde, bewusst weg". Ein Client kann eine Klasse anlegen (`asset_create`) und sie dann **nicht** bearbeiten, bis ein Mensch den Tab öffnet. Level-Script und GameInstance sind gedeckt. |
| **Widget** | ✓ Elementbaum | ✓ | Stub (leerer Baum) | Gedeckt bis auf den **Logikgraphen** des Widgets (§11.5) und `widget_set_canvas`/`widget_duplicate`. |
| **Theme** | – | – | Stub (Shipped-Default) | Kein Lesen/Setzen der Farbrollen, Größen, Schatten. Datenform ist JSON (`uiThemeToJson`), also ein billiges Paar `theme_get`/`theme_set`. |
| **InputAction** | ✓ | ✓ | Stub | Gedeckt. |
| **InputMappingContext** | ✓ | ✓ | Stub | Gedeckt. |
| **ParticleSystem** | ✓ Knoten+Links | Eingangswerte, Slots | Stub | Werte ja, Struktur nein (§17.5): kein Knoten anlegen/verdrahten, Random-Range-Eingänge nur lesbar. |
| **AnimatorStateMachine** | ✓ | ✓ | Stub | Gedeckt. |
| **BlendSpace** | ✓ | ✓ | Stub | Gedeckt. |
| **BoneMask** | – | – | Stub | Kein Lesen/Setzen der Joint-Liste. JSON-Form (`BoneMaskAsset::json`), billig; braucht aber Skeleton-Sicht (SkeletalMesh-Detail), um Joint-Namen zu validieren. |
| **StructType** | ✓ | ✓ | Stub | Gedeckt; Rename-Retarget fehlt engineweit (§15.5). |
| **EnumType** | ✓ | ✓ | Stub | Gedeckt. |
| **SaveGameTemplate** | ✓ `type_info` | ✓ `type_field_*` | Stub | Gedeckt (in den Typ-Werkzeugen, `McpToolsType.cpp:122-143`). |
| **PropertyAnimClip** (Sequencer) | – | – | Stub (META-only = leerer Clip) | **Nichts außer Anlegen.** Datenform ist flach (`duration` + `channels[]` aus `PropTarget`/`times[]`/`values[]`, `Assets.h:673-699`), also gut adressierbar: `sequence_info`, `sequence_key_set/remove`, `sequence_set_duration`. |
| **Shader** | – | – | – | Generiert, nie autoriert: korrekt ohne Werkzeug. |

Quer dazu zwei Dinge, die keinem Typ allein gehören:

* **Import fehlt als Vorgang.** `Importer::importSource(source, contentRoot,
  relDir)` (`ImporterCommon.h:308`) ist vom Editor aus erreichbar, routet
  nach Endung (inkl. Skinned-glTF → SkeletalMesh + AnimationClips, was ein
  Werkzeug **nicht** selbst nachbauen darf), und `Importer::reimport(asset,
  contentRoot)` läuft auf die bestehende UUID. Das Hindernis ist nicht der
  Importer, sondern **die Confinement-Regel**: `McpToolRegistry.h:7` verspricht
  „kein Dateizugriff", `McpToolCommon.h` prüft jeden Pfad gegen den
  Content-Root, und eine Importquelle liegt per Definition außerhalb. Siehe 3.3.
* **Mesh-Material-Slots** werden in zwei Panels autoriert (Static- und
  Skeletal-Mesh-Editor, sofort gespeichert, mit Undo über dem Tab) und sind
  über MCP unsichtbar. Ein Client, der ein Material erstellt (Abschnitt 1),
  kann es an eine Entity hängen (`entity_set_components`), aber nicht an das
  Mesh-Asset, das jede Instanz dieses Meshes benutzt.

### 3.3 Import: der Grenzfall, und wie die Grenze gezogen wird

Ein `asset_import` darf die Confinement-Regel nicht aufweichen zu „irgendein
absoluter Pfad", sonst liest der Editor auf Zuruf eines externen Clients
beliebige Dateien. Vorschlag, in dieser Reihenfolge streng:

1. **Quelle muss unter dem Projektverzeichnis liegen** (das Verzeichnis der
   `.heproj`, also auch `Source/`, `Raw/`, ein `Import/`-Ordner), aufgelöst
   und lexikalisch normalisiert wie `checkPath`, `..` also wirkungslos. Das
   deckt den Normalfall „der Mensch hat die FBX ins Projekt kopiert".
2. **Zusätzlich erlaubte Import-Wurzeln** als Editor-Einstellung
   (`Preferences ▸ Editor ▸ Remote Control ▸ Import roots`), sichtbar über
   `settings_get`, damit ein Client die Grenze *lesen* kann, statt sie durch
   Ablehnungen zu ertasten. Leer per Default.
3. **Kein URL-Import, kein Base64-Payload.** Beides wäre Dateizugriff durch
   die Hintertür bzw. genau das Modell-hantiert-Binärdaten, das diese
   Schnittstelle überall verweigert (§14, Prefab-Begründung).

Signatur: `asset_import(source, target_dir?, reimport_of?)` → die
geschriebenen Assets (ein glTF bringt Mesh + Material + Texturen +
Clips mit; `meshSidecarAssets` liefert die Liste nachträglich). Blockierte
Formate (`importBlockedReason`: FBX/OBJ/DAE ohne Assimp-Build) werden mit
diesem Satz abgelehnt, nicht mit „failed". Play-Modus, `Engine/`-Ziel und
Fremd-Lock wie bei `asset_create`.

### 3.4 Priorisierte Liste: was als Nächstes drankommt

Zehn Einträge, absteigend. Kriterium: wie viele Arbeitsabläufe ein
Assistent damit erst zu Ende bringen kann, geteilt durch den Bauaufwand.

1. **`material_create` mit Vorlagen (Abschnitt 1).** Schließt die Kette
   anlegen → einstellen → zuweisen; heute endet sie beim leeren Stub. Klein,
   weil der Schreibpfad schon existiert.
2. **`material_graph_info` (Abschnitt 2).** Ohne Struktur-Sicht kann ein
   Assistent ein bestehendes Material weder erklären noch gezielt umbauen,
   und jedes spätere Graph-Schreibwerkzeug braucht die Ids daraus. Nimmt
   Material-Funktionen gleich mit, die heute komplett unlesbar sind.
3. **`asset_import` für Mesh und Textur (3.3).** Die größte Lücke der
   Tabelle: fünf Asset-Typen ohne jeden Zugang, und ohne Import kann ein
   Assistent nur auf Vorhandenes zeigen. Braucht die Grenzentscheidung aus
   3.3 vorab, ist also mittelgroß; Audio/Font kommen im selben Werkzeug
   umsonst mit.
4. **`script_get`/`script_set` für Lua/Python-Quelltext.** Ein String
   lesen und schreiben, Sprache aus `CHUNK_SLNG`, Ablehnung bei offenem
   schmutzigen Tab wie überall. Der billigste Eintrag mit dem größten Hebel:
   die zwei Text-Skriptsprachen der Engine sind heute über MCP unsichtbar.
5. **HorizonCode-Klassen ohne offenen Tab.** Heute: `asset_create` legt eine
   Klasse an, `hc_documents` sieht sie nicht. Entweder ein `hc_open(path)`,
   das den Tab programmatisch öffnet (die Hooks dafür hat das Panel), oder ein
   Plattenweg wie bei Widgets (§11.2). Kleiner Eingriff, entblockt den
   gesamten Visual-Scripting-Ablauf für neue Klassen.
6. **Mesh-Detail + Material-Slots** (`mesh_info`, `mesh_slot_set`). Bounds,
   Sections, UV-Statistik, Skeleton-Joints; und das eine, was die Mesh-Panels
   schreiben. Ohne das hängt ein neues Material nur an Entities, nie am Asset.
7. **`texture_info` + Cook-Flags** (`srgb`, Mips). Die erste Frage bei
   „sieht falsch aus" ist „linear oder sRGB?", und die Antwort steht im
   Asset, nicht in der Szene. Kann in `asset_import` als Option und in einem
   Setter nachgezogen werden.
8. **Sequencer (`PropertyAnimClip`).** Flache Datenform, ein Panel, das
   heute die einzige Quelle ist. `sequence_info`/`sequence_key_set`/
   `sequence_key_remove`/`sequence_set_duration` reichen für „lass die Tür
   in zwei Sekunden aufgehen".
9. **Material-Graph schreiben** (`material_add_node`/`connect`/… , 2.5).
   Der in §19.5 benannte „nächste große Schritt"; kommt bewusst *nach* 1 und
   2, weil die Vorlagen den häufigsten Fall (ein PBR-Material mit Texturen)
   ohne Graph-Chirurgie abdecken und die Lese-Form die Ids liefert.
10. **`theme_get`/`theme_set` und `bonemask_get`/`bonemask_set`.** Beides
    JSON-Blobs mit fertigem (De-)Serializer, je ein Nachmittag; niedrig, weil
    selten und weil BoneMask ohne Skeleton-Sicht (6) nicht validierbar ist.

Nicht auf der Liste, bewusst: **Audio-Analyse** über MCP (Peak, Loop-Naht;
nett, aber niemand blockiert daran), **Font** (import reicht), **Partikelgraph
schreiben** (dieselbe Bauweise wie 9, danach), und alles aus §19.5
„Projekt-Ebene" (`project_open`/`project_create`), das keine Asset-Frage ist.

---

## 4. Kernentscheidungen in einem Absatz

`material_create` wird als Wrapper über einen erweiterten Stub-Writer gebaut
(der Default-Graph zieht in `writeAssetStub`, damit `asset_create`, das
Content-Browser-Menü und das neue Werkzeug dieselbe Datei schreiben) und
schickt das Ergebnis durch den Master-Schreibpfad von `material_set_param`;
die Vorlagen (OpaquePBR, Masked, Translucent, Unlit, UserInterface) tragen
benannte **Param**-Knoten mit Slider-Range, keine Konstanten, weil nur
Parameter über MCP erreichbar sind. `material_graph_info` liefert **immer
Struktur** (Knoten mit aufgelösten Pin-Namen, Links, Kommentare) **plus
Zusammenfassung** (je Output-Pin die Kette und der faltbare Konstantenwert),
mit `summary_only` für die billige Form, keinen Roh-Dump, und nimmt
Material-Funktionen und Instanzen an. Der Katalog zeigt fünf Asset-Typen
ohne jeden Zugang (Meshes, Textur, Audio, Font), deren gemeinsame Ursache der
fehlende Import ist, und der Import braucht vor dem Bau eine explizite
Grenze (Projektverzeichnis plus konfigurierte Import-Wurzeln), damit die
„kein Dateizugriff"-Zusage der Registry stehen bleibt.
