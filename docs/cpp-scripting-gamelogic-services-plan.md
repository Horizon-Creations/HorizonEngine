# C++ GameLogic: Zugriff auf Physik, Input und Content + „Build and Reload" im Editor

> Stand: 2026-09-08 · Branch `claude/cpp-scripting-gamelogic-services` · Schritt 1 (Orientierung + Design, kein Feature-Code)
>
> Ziel: Ein natives C++-Spielprojekt erreicht dieselben Engine-Dienste wie Lua, Python und HorizonCode — heute erreicht es nur Savegames. Und der Editor bekommt den Knopf, der den vorbereiteten Hot-Reload-Pfad tatsächlich benutzt.

---

## 0. Ausgangslage, belegt

### Was ein GameLogic-Modul heute sieht

`IGameLogic` (src/HE_Core/include/IGameLogic.h) übergibt `HorizonWorld&` — und zwar als **Vorwärtsdeklaration**. Das Scaffold-CMakeLists (`CppScaffold::cmakeLists`, src/HE_Tools/src/FileOps/CppScaffoldTemplates.cpp:286) hängt genau **ein** Include-Verzeichnis an:

```
target_include_directories(GameLogic PRIVATE "${HORIZON_ENGINE_DIR}/src/HE_Core/include")
```

`HorizonWorld` liegt in HE_Scene. Das Modul kann den Typ also nicht einmal vollständig sehen, geschweige denn Methoden darauf rufen, und es linkt gegen nichts. Der `HorizonWorld&`-Parameter ist damit heute ein **undurchsichtiger Griff ohne Verwendung**.

Daraus folgt die zentrale Beobachtung dieses Designs: die injizierte Service-Tabelle ist nicht *eine* Möglichkeit, die Engine zu erreichen — sie ist die **einzige**. Alles, was C++-Gameplay je können soll, muss durch sie.

### Was es dadurch kann

Nur das, was `HeSaveServices` anbietet (src/HE_Core/include/HorizonGameServices.h): Save-Lebenszyklus, typisierte Feldzugriffe, vier Entity-Save-State-Rows. Physik, Input, Content: nichts. Bestätigt durch docs/gap-audit-2026-08-25.md:153.

### Was der Editor heute mit GameLogic macht

Nichts. `GameLogicLoader` ist in `HE::Application` als `m_logicLoader` eingebaut (src/HE_Core/include/Application/Application.h:270), die Hauptschleife tickt es (src/HE_Core/src/Application/Application.cpp:456) — aber **nur `GameApplication` ruft je `load()`** (src/HE_Game/src/GameApplication.cpp:1213). In HE_Editor taucht der Loader an keiner Stelle auf. Der Editor hat lediglich `EditorApplication::GameLogicDeltaTime` (src/HE_Editor/EditorApplication.cpp:7124), das außerhalb von PIE `0.0f` liefert — eine Vorkehrung für einen Fall, den es noch nicht gibt.

---

## 1. Das Injektionsmuster, kartiert

Der Präzedenzfall funktioniert in fünf Teilen. Jeder neue Dienst muss dieselben fünf haben.

| # | Teil | Ort |
|---|---|---|
| 1 | **Die Tabelle** — POD aus C-Funktionszeigern, `abiVersion` + `void* host` vorn | `HorizonGameServices.h`, `struct HeSaveServices` |
| 2 | **Der Empfangs-Export** — `HE_SetEngineServices(const HeSaveServices*)`, vom Makro `HE_IMPLEMENT_ENGINE_SERVICES()` in genau einer .cpp des Spiels definiert; prüft `abiVersion` und legt den Zeiger in `g_heSaveServices` ab | `HorizonGameServices.h` |
| 3 | **Die Füllfunktion** — `HE::api::fillSaveServices(out, binding)`, jede Zeile eine **captureless Lambda**, die zum C-Funktionszeiger zerfällt; `host` trägt das Binding | src/HE_Scene/src/EngineApi.cpp:6293 |
| 4 | **Das Binding** — `struct SaveServicesBinding { std::function<HorizonWorld*()> world; ContentManager* content; }`; die Welt wird **pro Aufruf** aufgelöst, damit ein Szenenwechsel keinen alten Zeiger in der Hand des Moduls stehen lässt | src/HE_Scene/include/HorizonScene/EngineApi.h:1629 |
| 5 | **Die Anwendung** — Tabelle + Binding liegen als Member der Application (müssen das Modul überleben), gefüllt und injiziert **nach `load()`, vor `onStart()`** | src/HE_Game/src/GameApplication.h:158, .cpp:1216–1222 |

Dazu die Spielseite: dünne `inline`-Wrapper in `namespace he` mit dem Vertrag „ohne Injektion ein sicherer No-Op mit Default-Rückgabe". Die Engine loggt laut, das Modul stürzt nicht ab.

**Konventionen, die schon feststehen und für alles Neue gelten:**

- Nur einfache C-Typen. Strings als UTF-8 `const char*` hinein.
- Strings heraus über `(host, char* buf, int cap)` → gibt die **volle** Länge zurück; zu kleiner Puffer heißt: größer machen und noch einmal rufen (`copyOut`, EngineApi.cpp:6273). Listen trennen mit `'\n'`.
- Entity = `uint32_t`, das rohe entt-Handle, wie es Skripte als `self.entityId` sehen.
- Die Tabelle gehört der **Engine** und ist für die ganze Lebensdauer des Moduls gültig.
- Erweitert wird nur **hinten** (append), mit Versionserhöhung.

---

## 2. Entscheidung A: keine `HeSaveServices` v2, sondern Geschwistertabellen unter einem Dach

**Entschieden:** drei neue, eigenständige POD-Tabellen — `HePhysicsServices`, `HeInputServices`, `HeContentServices` — mit je eigener `HE_*_ABI_VERSION`. Übergeben werden sie **nicht** einzeln, sondern über eine Dachstruktur und **einen zweiten Export**:

```c
#define HE_SERVICES_ABI_VERSION 1u

typedef struct HeEngineServices
{
    uint32_t                    abiVersion;   // HE_SERVICES_ABI_VERSION
    const HeSaveServices*       save;
    const HePhysicsServices*    physics;
    const HeInputServices*      input;
    const HeContentServices*    content;
} HeEngineServices;

typedef void (*FnSetEngineServicesV2)(const HeEngineServices*);
```

Der Export heißt `HE_SetEngineServicesV2`. `HE_IMPLEMENT_ENGINE_SERVICES()` definiert ab jetzt **beide** Exports und alle vier Globals.

**Warum nicht `HeSaveServices` erweitern und `HE_SAVE_ABI_VERSION` auf 2 heben?** Weil der Empfänger bei Mismatch die Tabelle *verwirft*:

```c
{ g_heSaveServices = (s && s->abiVersion == HE_SAVE_ABI_VERSION) ? s : nullptr; }
```

Ein bestehendes Modul, gegen v1 gebaut, verlöre unter einer v2-Engine **`he::save::*` komplett** — nicht nur das Neue, sondern das, was es schon benutzt. Die Save-Tabelle ist ausgeliefert; sie bleibt unangetastet.

**Der Verträglichkeitsvertrag in vier Fällen:**

| Modul | Engine | Ergebnis |
|---|---|---|
| alt (nur v1-Export) | neu | Loader findet `HE_SetEngineServicesV2` nicht, fällt auf `HE_SetEngineServices` zurück → Save funktioniert wie bisher, Physik/Input/Content sind No-Ops |
| neu | neu | V2, alles verdrahtet |
| neu | alt (kennt V2 nicht) | Engine ruft nur v1 → `g_heSaveServices` gesetzt, die drei anderen Globals bleiben null → No-Ops mit Default |
| alt | alt | unverändert |

In allen vier Fällen ist das Ergebnis ein **Zustand, kein Fehler** — genau die Formulierung, die `injectServices` heute schon benutzt.

**Versionsregel für die neuen Tabellen — `>=`, nicht `==`.** Der Empfänger nimmt eine Tabelle an, wenn `table->abiVersion >= HE_X_ABI_VERSION` (die Version, gegen die das *Modul* gebaut wurde). Begründung: bei reinem Anhängen ist die Struktur des Moduls ein **Präfix** der Struktur der Engine. Ist die Engine neuer, liest das Modul nur den Präfix — harmlos. Ist das *Modul* neuer, läse es über das Ende dessen hinaus, was die Engine beschrieben hat — ein Aufruf durch einen uninitialisierten Zeiger. Genau den verhindert `>=`. Empfehlung für Schritt 2: `HeSaveServices` bei derselben Gelegenheit von `==` auf `>=` umstellen; es gibt heute nur die Version 1, die Umstellung ist strikt permissiver und kann nichts brechen.

---

## 3. Entscheidung B: jede Brücke geht durch `HE::api::*`, nie direkt an PhysicsWorld/Input/ContentManager

`fillSaveServices` ruft `save::create`, `entity::findByName` und so weiter — es reimplementiert nichts. Das bleibt so, und zwar nicht aus Ordnungsliebe:

- **Die Welt/Lokal-Grenze.** `PhysicsWorld` spricht **Welt**, die Skript-Transform-API spricht **lokal**, umgerechnet wird in `EngineApi.cpp` — an der einen Grenze, die beide kennt. Das steht ausführlich an `HE::api::physics::setPosition` (EngineApi.h:392–417) und ist im Projekt bereits dreimal schiefgegangen. Eine zweite Umrechnung an der C-Grenze wäre das vierte Mal.
- **Charakter schlägt Rigid Body.** `setVelocity`/`setPosition` adressieren den Character Controller, wenn es einen gibt, sonst den Rigid Body. Diese Regel steht genau einmal im Code.
- **`sphereCast` ignoriert Trigger, `raycast` nicht.** Auch das ist eine Entscheidung, die einmal getroffen ist.
- **Die Registry bleibt die einzige Wahrheit.** Was durch `HE::api` geht, ist automatisch das, was HorizonCode-Nodes, Lua und Python sehen. Parität entsteht dann von selbst statt gepflegt werden zu müssen.

### Das Binding muss wachsen — und `physics` muss eine Funktion sein

`GameApplication::m_physicsWorld` ist ein `std::unique_ptr<PhysicsWorld>`, der **bei jedem Szenenwechsel neu gebaut wird** (Kommentar an GameApplication.h:147: „Rebuilt on every scene switch, because the bodies belong to the world that is going away"). Ein roher `PhysicsWorld*` im Binding wäre nach dem ersten `scene.load` ein baumelnder Zeiger im Besitz eines fremden dylibs.

**Entschieden:** `SaveServicesBinding` wird zu `GameServicesBinding` umbenannt (drei Aufrufstellen: GameApplication.h/.cpp, test_engine_api.cpp) und bekommt:

```cpp
struct GameServicesBinding
{
    std::function<HorizonWorld*()> world;     // wie bisher: pro Aufruf aufgelöst
    std::function<PhysicsWorld*()> physics;   // NEU, pro Aufruf — überlebt Szenenwechsel
    ContentManager*                content = nullptr;   // stabil, gehört der Application
};
```

`content` bleibt ein roher Zeiger: der ContentManager gehört der `Application` und wird über Szenenwechsel hinweg nicht ersetzt. Der Unterschied ist beabsichtigt und der Grund gehört als Satz an das Feld.

Der `Ctx`-Bauer für die Brücken (`bindingCtx`, EngineApi.cpp:6264) füllt entsprechend `c.physics = b->physics ? b->physics() : nullptr` mit.

---

## 4. Die ABI-Formen

Feste Regeln für alles Neue, damit Schritt 2 nichts entscheiden muss:

- **vec3** hinein als `const float v[3]`, heraus als `float out[3]` (Out-Parameter, kein Rückgabe-Struct). Grund: die Spielseite hat **kein glm** — sie sieht nur `src/HE_Core/include`. Die Wrapper bieten ein plattes `he::Vec3 { float x, y, z; }`, das in `HorizonGameServices.h` selbst definiert wird.
- **Structs quer über die Grenze** nur als POD mit fester Reihenfolge, per Out-Zeiger übergeben, nie als Rückgabewert. `HeRaycastHit` ist so einer.
- **`bool`** ist zulässig — `HeSaveServices` benutzt ihn bereits, und auf allen Zielplattformen (Itanium-C++-ABI, MSVC) ist er ein Byte mit den Werten 0/1.
- **Listen von Entities** wie Strings: `(host, …, uint32_t* out, int cap)` schreibt bis `cap` Einträge und gibt die **volle** Anzahl zurück.
- **UUIDs** als `HeAssetId { uint64_t hi, lo; }` — layoutgleich mit `HE::UUID` (src/HE_Core/include/Types/UUID.h:8). Bewusst **nicht** als String: es gibt im Projekt gar keine `UUID`↔String-Umwandlung, eine dafür zu erfinden wäre neue Oberfläche für nichts.

---

## 5. Die drei Tabellen

### 5.1 `HePhysicsServices` (v1)

Deckt exakt `HE::api::physics` ab, Zeile für Zeile. Keine Zeile ohne Vorbild dort.

```c
#define HE_PHYSICS_ABI_VERSION 1u

typedef struct HeRaycastHit
{
    bool     hit;
    uint32_t entity;
    float    point[3];
    float    normal[3];
    float    distance;
} HeRaycastHit;

typedef struct HePhysicsServices
{
    uint32_t abiVersion;
    void*    host;

    // Abfragen. point/normal sind WELT-Koordinaten.
    void (*raycast)(void* host, const float origin[3], const float dir[3],
                    float maxDist, HeRaycastHit* out);
    void (*sphereCast)(void* host, const float origin[3], const float dir[3],
                       float radius, float maxDist, HeRaycastHit* out);
    int  (*overlapSphere)(void* host, const float center[3], float radius,
                          uint32_t* out, int cap);          // → volle Anzahl

    // Kräfte. Alle drei brauchen einen DYNAMISCHEN Rigid Body → sonst false.
    bool (*addForce)  (void* host, uint32_t e, const float force[3]);
    bool (*addImpulse)(void* host, uint32_t e, const float impulse[3]);
    bool (*addTorque) (void* host, uint32_t e, const float torque[3]);

    // Geschwindigkeit in m/s. Character Controller schlägt Rigid Body.
    void (*setVelocity)(void* host, uint32_t e, const float v[3]);
    void (*getVelocity)(void* host, uint32_t e, float out[3]);
    bool (*isGrounded) (void* host, uint32_t e);

    // Teleport. position ist LOKAL — dieselbe Regel wie transform.setPosition.
    bool (*setPosition)        (void* host, uint32_t e, const float position[3]);
    bool (*setPositionAndReset)(void* host, uint32_t e, const float position[3]);

    bool (*hasPhysics)(void* host, uint32_t e);
    void (*setGravity)(void* host, const float g[3]);
    void (*getGravity)(void* host, float out[3]);
} HePhysicsServices;
```

Wrapper: `he::physics::raycast(origin, dir, maxDist) → he::RaycastHit`, `he::physics::overlapSphere(center, radius) → std::vector<uint32_t>` (zweistufig wie `fetchString`), Rest 1:1.

**Der Kommentar, der mitwandern muss:** dass `position` lokal ist, `raycast` aber Welt-Punkte meldet — die genannte Lücke aus EngineApi.h:400–410. Ohne diesen Satz an den Wrappern baut jemand „teleportiere auf das, was ich getroffen habe" falsch zusammen.

### 5.2 `HeInputServices` (v1)

`HE::api::input` ist ein **prozessglobaler Schnappschuss**; die Getter brauchen keinen `Ctx`. `host` bleibt trotzdem im Signaturkopf, damit alle Tabellen gleich aussehen.

```c
#define HE_INPUT_ABI_VERSION 1u

typedef struct HeInputServices
{
    uint32_t abiVersion;
    void*    host;

    // Tastatur/Maus. Namen sind SDL-Scancode-Namen ("W", "Space", "Escape").
    bool  (*keyDown)      (void* host, const char* name);
    bool  (*mouseButton)  (void* host, int index);   // 0=links, 1=rechts, 2=mitte
    void  (*mousePosition)(void* host, float out[2]);
    void  (*mouseDelta)   (void* host, float out[2]);
    float (*scrollDelta)  (void* host);

    // Gamepad. Namen aus SDL-Mapping-Tabellen, Xbox-Layout ("a", "leftx", …).
    // Sticks -1..+1 (Y positiv nach UNTEN), Trigger 0..1, deadzone-gefiltert.
    bool  (*gamepadConnected)(void* host);
    bool  (*gamepadButton)   (void* host, const char* name);
    float (*gamepadAxis)     (void* host, const char* name);

    // Input-Routing: 0 = GameOnly, 1 = GameAndUI, 2 = UIOnly (HE::api::input::Mode).
    int   (*mode)   (void* host);
    void  (*setMode)(void* host, int mode);
} HeInputServices;
```

Die App-Hooks (`setMouse`, `setKeysDown`, `pushSdlSnapshot`, `setGamepad`, `clear`, `setControllers`) kommen **nicht** in die Tabelle. Das sind Schreibpfade des Hosts; ein Spielmodul, das den Schnappschuss selbst überschreibt, ist ein Fehler, keine Funktion.

Wrapper: `he::input::keyDown(name)`, `he::input::mouseDelta() → he::Vec2`, `he::input::setModeUIOnly()` und Geschwister (die drei benannten Setter statt einer Zahl — dieselbe Begründung wie in EngineApi.h:1970).

### 5.3 `HeContentServices` (v1)

Hier ist die wichtigste Entscheidung eine **Weglassung**.

```c
#define HE_CONTENT_ABI_VERSION 1u

typedef struct HeAssetId { uint64_t hi, lo; } HeAssetId;   // layoutgleich mit HE::UUID

typedef struct HeContentServices
{
    uint32_t abiVersion;
    void*    host;

    // Laden über content-relativen Pfad. false = nicht gefunden/unlesbar.
    bool (*loadAsset)   (void* host, const char* relativePath, HeAssetId* out);
    bool (*unloadAsset) (void* host, HeAssetId id);
    bool (*isLoadedId)  (void* host, HeAssetId id);
    bool (*isLoadedPath)(void* host, const char* relativePath);
    // Typname des Assets ("StaticMesh", "Texture", …); "" = unbekannt.
    int  (*assetTypeName)(void* host, HeAssetId id, char* buf, int cap);
} HeContentServices;
```

**Es gibt keine `getStaticMesh`-Brücke und es wird nie eine geben.** Der ContentManager gibt Zeiger in einen dichten `SlotMap`-Vektor zurück; das **nächste** `loadAsset` invalidiert *alle* davon, samt der Strings, die sie besitzen — ein `a->path`, das man in eine ladende Funktion hineinreicht, stirbt in genau diesem Aufruf. Das steht als Warnung im Header selbst (ContentManager.h:51–68) und ist eine Projektlektion. Innerhalb einer Übersetzungseinheit ist das eine Falle; **über eine dylib-Grenze, deren Aufrufreihenfolge die Engine nicht kennt, wäre es eine Garantie für einen Absturz.** Über diese Grenze gehen nur Werte: UUID, bool, Text.

**Ein zusätzlicher Schritt für Schritt 2:** `HE::api` hat heute **gar keinen** `content`-Namensraum — Assets werden nur indirekt erreicht (`audio.play(pfad)`, `theme.set(pfad)`, `widget.addChild(pfad)`). Nach Entscheidung B darf die Brücke nicht am ContentManager vorbei; also braucht es zuerst ein kleines `HE::api::content` mit `load`/`unload`/`isLoaded`/`typeName` **samt Registry-Rows**. Das ist Mehrarbeit, aber es ist die richtige Richtung: davon bekommen Lua, Python und HorizonCode dieselbe Fähigkeit im selben Zug, statt dass C++ etwas kann, was die Skripte nicht können — was das Anliegen dieses Themas auf den Kopf stellen würde.

**Kosten, die dabei anfallen (in `tests/test_engine_api.cpp` verankert):** jede neue Registry-Row braucht ein `cppCall`, das genau eine reale, in der Tabelle eindeutige Funktion benennt, deren Signatur den `params` in Reihenfolge entspricht. Der Test „cppCall names one real, distinct callee per row" (test_engine_api.cpp:102) prüft das.

**Zu klären in Schritt 2:** ob es schon eine `HE::AssetType` → Name-Abbildung gibt; `ContentManager::assetType(UUID)` (ContentManager.h:474) liefert das Enum, ein Namens-Helfer wurde in diesem Schritt nicht gesucht.

---

## 6. Was bewusst NICHT in v1 kommt

**Layer-Queries.** Auf dem Brett stehen sie, aber es gibt sie nicht: weder `PhysicsWorld.h` noch die Kollisionskomponenten kennen `collisionLayer`, `layerMask`, `ObjectLayer` oder `collisionGroup`, und `HE::api::physics::raycast` hat keinen Layer-Parameter. Eine Layer-Brücke wäre nicht „durchreichen", sondern **ein eigenes Feature in Jolt, den Komponenten, dem Editor-UI und der Registry** — vier Ebenen unter dieser Grenze. Gehört in ein eigenes Thema; hier nur benannt.

**Input Actions.** Der Third-Person-Starter steuert über `InputActionAsset` („Move"-Achse, „Jump"-Button). Diese Werte sind aber **nicht abfragbar**: `PlayerHost` liefert sie als *Ereignisse* an HorizonCode-Instanzen (`Input.<Action>.Pressed`/`.Axis`, PlayerHost.h:16–17). Weder Lua noch Python kommen an sie heran — `HE::api::input` hat keine einzige Action-Row. Die Paritätsaussage des Themas ist also erfüllt, wenn C++ die **rohen** Abfragen bekommt: mehr haben die Textsprachen auch nicht. Der Weg dorthin, falls es später gewollt ist, ginge über ein `input.actionValue(name)` / `input.actionPressed(name)` in `HE::api` — und käme dann allen vier Frontends gleichzeitig zugute. Eine C++-Sonderlösung wäre das Gegenteil dessen, was dieses Thema will.

**Direkter Zugriff auf Asset-Objekte.** Siehe 5.3.

**Audio, Kamera, Szenenwechsel, Widgets.** Alles Kandidaten für weitere Geschwistertabellen; das Dach `HeEngineServices` ist genau dafür da. Nicht in dieser Runde, damit die erste Erweiterung des Musters klein und prüfbar bleibt.

---

## 7. Wo injiziert wird

### Spiel (GameApplication)

Der Block GameApplication.cpp:1213–1224 wächst um Binding-Felder und drei `fill*`-Aufrufe. Die Reihenfolge bleibt: `load()` → Binding füllen → Tabellen füllen → `injectServices()` → `onStart()`. Der Kommentar „Engine services (savegames) go in BEFORE onStart" gilt jetzt für alle vier und muss das auch sagen.

Member in GameApplication.h (bei `m_saveServices`, mit demselben Lebensdauer-Argument):
`m_gameServicesBinding`, `m_saveServices`, `m_physicsServices`, `m_inputServices`, `m_contentServices`, `m_engineServices` (das Dach).

`m_gameServicesBinding.physics = [this]() { return m_physicsWorld.get(); };`

### Loader

`GameLogicLoader::injectServices` bekommt eine Überladung bzw. wird auf das Dach umgestellt:

```cpp
bool injectServices(const ::HeEngineServices* services);   // sucht V2, fällt auf v1 zurück
```

Der Rückfallpfad ist Pflicht: `HE_SetEngineServicesV2` fehlt → `HE_SetEngineServices` mit `services->save` versuchen → fehlt auch der, dann die vorhandene INFO-Zeile („older scaffold"). Die alte Signatur bleibt als Überladung stehen, damit bestehende Tests und Aufrufer sich nicht ändern müssen.

### Editor (Schritt 3)

`EditorApplication` braucht dieselben Member. Die Bausteine sind da: `apiCtx(world, physics, content, …)` (EditorApplication.cpp:207) füllt bereits genau diese drei Dinge für PIE; das Binding wird aus denselben Quellen gespeist (`m_editorWorld`, `m_physicsWorld`).

---

## 8. `GameLogicLoader` kartiert (Vorarbeit für den Build-Reload-Knopf)

### Die `.hot-NNNN`-Mechanik, und warum sie so aussieht

`load()` (src/HE_Core/src/Application/GameLogicLoader.cpp:12) öffnet **niemals** den beobachteten Pfad direkt. Es kopiert nach `GameLogic.hot-0001.dylib`, `…-0002…` und lädt die Kopie. Zwei Gründe, beide im Code notiert: macOS' dyld cacht nach Pfad/Inode und kann beim Reload dasselbe Image zurückgeben; Windows sperrt die geladene Datei, sodass der nächste Build fehlschlägt.

Eigenschaften, die für den Knopf zählen und die niemand „aufräumen" darf:

- `s_loadCounter` ist **prozessstatisch**. Innerhalb einer Editor-Sitzung kollidiert kein Name, egal wie oft neu geladen wird.
- `m_lib.unload()` ist **best effort**. TLS und Obj-C können das alte Image festhalten. Genau deshalb wird ein Name nie wiederverwendet.
- Das Entfernen der Hot-Kopie in `unload()` ist ebenfalls best effort. Eine liegengebliebene `GameLogic.hot-0007.dylib` neben dem Projekt ist **normal**, kein Fehler.
- Schlägt das Kopieren fehl, wird das Original geladen. Kein harter Abbruch.

### `reload()` reinjiziert NICHT

```cpp
bool GameLogicLoader::reload(const std::filesystem::path& dllPath, HorizonWorld& world)
{
    unload(world);
    return load(dllPath);
}
```

`unload()` ruft `onStop()`, `load()` ruft **nicht** `onStart()` und **nicht** `injectServices()`. Der Knopf muss die vollständige Sequenz selbst fahren:

```
1. Bauen (blockierend, auf einem Worker)
2. loader.reload(pfad, welt)          → onStop alt, load neu
3. loader.injectServices(&m_engineServices)
4. loader.logic()->onStart(welt)
```

Wird Schritt 3 vergessen, ist das Modul nach dem ersten Reload stumm — kein Absturz, sondern lautlos tote Services. Das ist die Falle dieses Knopfes.

### Die entt-Falle, und warum sie hier entschärft ist

Projektlektion: entt-Komponentenpools gehören dem Modul, das den Typ **zuerst** anfasst; ein hot-geladenes Spielmodul darf das nie sein, sonst baumelt der Pool beim Entladen. `HorizonWorld` ruft dafür in seinem Konstruktor `reserveComponentStorage()` (src/HE_Scene/src/HorizonWorld.cpp:30).

Dieses Design **verschärft die Lage nicht** — im Gegenteil: das Modul sieht `HorizonWorld` gar nicht (Abschnitt 0) und kann keinen Pool anfassen. Alle ECS-Berührungen passieren auf der Engine-Seite der C-Grenze. Das ist ein Nebeneffekt der Tabellenlösung, der ausdrücklich erhalten bleiben soll: **kein Schritt darf `src/HE_Scene/include` in das Scaffold-CMakeLists aufnehmen.**

### Was zum Bauen noch fehlt

`HE::hccg::buildDylib` (src/HE_Scene/src/HcCodegen.cpp, ~4183–4212) macht bereits genau das Richtige — cmake configure, `--build … --config Release`, Ausgabe zeilenweise an `onLine`, volles Log nach `genDir/build.log` — ist aber auf **einen Artefaktnamen festgenagelt**:

```cpp
const char* names[] = { "libHorizonCodeGen.dylib", "libHorizonCodeGen.so", "HorizonCodeGen.dll" };
```

Für Schritt 3 heißt das: `buildDylib` um einen Parameter für die gesuchten Artefaktnamen erweitern (Default = die drei bestehenden, damit kein Aufrufer sich ändert) und mit `GameLogic.dylib` / `GameLogic.so` / `GameLogic.dll` aufrufen. Das Scaffold setzt `PREFIX ""`, es gibt also **kein** `lib`-Präfix — der Unterschied zur HorizonCodeGen-Liste ist real und leicht zu übersehen.

Weiter zu beachten für Schritt 3:

- Das Scaffold-CMakeLists braucht `-DHORIZON_ENGINE_DIR=…`. Der Editor kennt die Antwort bereits über `HE::hccg::resolveSdk(editorBaseDir)` (Env-Override → gestagete `SDK/` → `he_sdk_config.json`).
- `HE::hccg::setBundledCmakeDir` / `probeToolchain` / `ToolchainDialog` regeln „ist überhaupt eine Toolchain da" schon vollständig. Der Knopf muss nur denselben Zustand abfragen und darf sonst nichts Eigenes bauen.
- `BuildProgressDialog` existiert als UI für laufende cmake-Läufe und ist das naheliegende Ziel für `onLine`.
- Bauen ist blockierend → Worker-Thread, wie beim Export.
- Der Knopf gehört nur in ein Projekt mit `ProjectScriptLanguage::Cpp` und existierendem `Source/CMakeLists.txt`.
- Wo der Editor lädt und entlädt, hängt an `Application.cpp:456`: die Schleife tickt `m_logicLoader.logic()`, sobald `m_world` existiert — im Editor ist das die **Edit-Welt**. `EditorApplication::GameLogicDeltaTime` liefert außerhalb von PIE `0.0f`, `onUpdate` liefe also mit dt=0 statt gar nicht. Das ist kein sauberer Zustand: `onStart` hätte auf der Edit-Welt gefeuert. **Empfehlung für Schritt 3: der Editor lädt GameLogic beim Start von PIE und entlädt es beim Verlassen** — dann ist das Modul außerhalb von PIE nicht geladen und `logic()` ist null. Der „Build and Reload"-Knopf baut immer; das Neuladen macht er nur, wenn gerade gespielt wird, und sagt sonst „gebaut, wird beim nächsten Play geladen".

---

## 9. Checkliste der berührten Dateien

**Schritt 2 (Service-Tabellen):**

| Datei | Was |
|---|---|
| `src/HE_Core/include/HorizonGameServices.h` | drei neue Tabellen + `HeEngineServices` + `HeAssetId`/`HeRaycastHit`/`he::Vec2`/`he::Vec3`, `HE_IMPLEMENT_ENGINE_SERVICES()` um den V2-Export erweitern, `he::physics`/`he::input`/`he::content`-Wrapper |
| `src/HE_Scene/include/HorizonScene/EngineApi.h` | `SaveServicesBinding` → `GameServicesBinding` (+ `physics`-Resolver), `fillPhysicsServices`/`fillInputServices`/`fillContentServices`, neuer Namensraum `HE::api::content` |
| `src/HE_Scene/src/EngineApi.cpp` | die Füllfunktionen (Muster ab :6293), `bindingCtx` um `physics` erweitern, `content`-Implementierung + Registry-Rows |
| `src/HE_Core/include/Application/GameLogicLoader.h` + `.cpp` | `injectServices(const HeEngineServices*)` mit V2-Suche und v1-Rückfall |
| `src/HE_Game/src/GameApplication.h` (:155–159) + `.cpp` (:1213–1224) | Member + Füllen + Injizieren |
| `src/HE_Tools/src/FileOps/CppScaffoldTemplates.cpp` (:143–151, :330) | das Makro im Scaffold bleibt gleich (es wächst im Header), aber der README-Abschnitt und der Kommentar in `GameLogic.cpp` müssen die neuen Namensräume nennen |
| `tests/test_engine_api.cpp` (:2994–3044) | Injektionstest spiegeln: Version-Mismatch pro Tabelle, V2-vorhanden und V2-fehlend, No-Op-vor-Injektion |
| `docs/horizoncode-reference.md` (:390) | der Satz über `<HorizonGameServices.h>` listet heute nur `he::save`/`he::entity` |
| `docs/gap-audit-2026-08-25.md` (:153) | Lücke als geschlossen markieren |

**Schritt 3 (Build-Reload-Knopf):** `HcCodegen.h`/`.cpp` (`buildDylib`-Artefaktnamen), `EditorApplication.h`/`.cpp` (Loader-Lebenszyklus an PIE, Binding + Tabellen als Member, Build-Aktion auf einem Worker), `EditorUI.cpp` (Menüeintrag + Hilfe-Scope + Tooltip), `BuildProgressDialog` als Ausgabefenster.

**Nicht vergessen — zwei Projektregeln, die hier greifen:** jedes neue Bedienelement im Editor braucht einen Handbuch-Eintrag in einem offenen Help-Scope (die Deckung ist ein ctest), und `HE_Core`-API braucht `HE_API`, während `HE_Scene` es nie tragen darf.

---

## 10. Testplan

1. **Injektion und Rückfall** (test_engine_api.cpp, nach dem Vorbild :2994–3044): vor der Injektion sind alle `he::*::available()` false und jeder Aufruf ein Default; nach `HE_SetEngineServicesV2` sind alle vier gesetzt; nach `HE_SetEngineServices` allein ist nur `save` gesetzt; eine Tabelle mit zu **kleiner** `abiVersion` wird verworfen, ohne die anderen mitzunehmen.
2. **Zweistufige Puffer**: `overlapSphere` mit `cap` kleiner als die Trefferzahl gibt die volle Anzahl zurück und schreibt genau `cap` Einträge.
3. **Null-Binding-Toleranz**: jede Zeile mit `host = nullptr` und mit einem Binding, dessen `world`/`physics` null liefert — Default statt Absturz. Das ist der Vertrag, den `bindingCtx` heute schon erfüllt.
4. **Lokal vs. Welt**: `physics.setPosition` über die C-Grenze auf ein **verschachteltes** Entity, danach `HE::worldPositionOf` prüfen. Genau hier ist das Projekt dreimal gestolpert; ohne diesen Test ist die Grenze nicht belegt.
5. **Registry-Invarianten**: die bestehenden `cppCall`-Tests decken die neuen `content`-Rows automatisch ab.
6. **Ende-zu-Ende** (Schritt 3, manuell, nicht automatisierbar): ein Cpp-Projekt anlegen, in `GameLogic.cpp` einen Raycast plus `he::input::keyDown` schreiben, „Build and Reload" drücken, in PIE prüfen — dann die Quelle ändern und erneut drücken.

---

## 11. Offene Fragen für Schritt 2

- Gibt es eine `HE::AssetType` → Name-Abbildung, oder muss `assetTypeName` eine schreiben? (Abschnitt 5.3)
- Soll `HeSaveServices` bei dieser Gelegenheit von `==` auf `>=` umgestellt werden? Empfehlung: ja, mit einem Satz Begründung im Header. (Abschnitt 2)
- Bekommt `HE::api::content` auch `typeName`, oder bleibt das erst einmal `load`/`unload`/`isLoaded`? Empfehlung: alle vier, sonst hat die C-Tabelle eine Zeile ohne Registry-Vorbild.
