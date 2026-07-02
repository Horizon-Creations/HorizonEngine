<!-- Erzeugt aus dem scripting-design-Workflow (Map: Lua-ScriptEngine + GameLogicLoader; 
     Research: CPython/pocketpy/MicroPython empirisch + C++-Hot-Reload-Patterns; 02.07.2026) -->

# Design: Python & C++ als zusätzliche Gameplay-Sprachen (Lua bleibt)

> **USER-ENTSCHEIDUNGEN (02.07.2026):**
> 1. **Python-Runtime: CPython** (echtes Python, volle stdlib) — bewusst GEGEN die
>    pocketpy-Empfehlung in §2, mit dem vollen Ökosystem als Produktargument. Die
>    §2-Analyse bleibt als Begründungs-Doku stehen; Konsequenzen: dev-seitig gegen
>    System-/Homebrew-Python (find_package(Python3 COMPONENTS Development.Embed))
>    bauen, GIL = alles Scripting auf dem Main-Thread, und das **Shipping-Bundling**
>    (Windows: embeddable zip ~11 MB; macOS: python-build-standalone ~25 MB + rpath/
>    Codesign in der DMG-Pipeline) wird ein eigener Packaging-Schritt in 6.3.
> 2. **C++-Scope: Modul-Pfad wie in §3** (eine GameLogic-Lib pro Spiel, kein
>    per-Entity-C++ in v1).
>
> **UMSETZUNGSSTAND:** C1+C2 fertig (GameLogicLoader implementiert mit
> unique-hot-copy-Regel, GameApplication lädt GameLogic.dylib/dll neben der Exe,
> onStart nach Szenen-Load, Unload im Shutdown; Test gegen echte Fixture-Dylib,
> 512 Tests grün). Offen: A (IScriptBackend-Refactor), P (CPython-Backend),
> C3-C5 (Editor-Template, Build&Reload, Hot-Reload-Watch, Packaging), Doku.

## 1. ARCHITEKTUR — Ein Interface für Skriptsprachen, C++ bleibt ein eigener Pfad

**Entscheidung: Zweiteilung.**
- **Lua + Python** teilen sich ein gemeinsames `IScriptBackend`-Interface (per-Entity-Skripte, Quelltext als Asset, Hot-Reload per Source).
- **C++** ist KEIN `IScriptBackend`. Es ist ein Modul-Level-Pfad (ein GameLogic-Lib pro Spiel), dessen Gerüst mit `IGameLogic`/`GameLogicLoader`/`GameLoop` bereits existiert. Ein per-Entity-C++-Behavior-System wäre unverhältnismäßig (siehe §5).

### 1.1 IScriptBackend

Das Interface existiert der Form nach schon — `ScriptEngine.h:40-97` und `ScriptContext.h:39-66` sind fast deckungsgleich, nur nicht virtuell. Extraktion nach `HE_Core/include/Scripting/IScriptBackend.h`:

```cpp
class IScriptBackend {
public:
    using InstanceId = uint64_t;
    virtual ~IScriptBackend() = default;

    virtual bool loadScript(const std::string& name, const std::string& source) = 0;
    virtual void unloadScript(const std::string& name) = 0;
    virtual InstanceId createInstance(const std::string& name, uint32_t entityId) = 0;
    virtual void destroyInstance(InstanceId) = 0;

    virtual bool callOnStart(InstanceId) = 0;
    virtual bool callOnUpdate(InstanceId, float dt) = 0;
    virtual bool callOnCollisionEnter(InstanceId, uint32_t other) = 0;
    virtual bool callOnCollisionExit(InstanceId, uint32_t other) = 0;

    virtual std::vector<ScriptPropDef> getScriptProperties(const std::string& name) const = 0;
    virtual void injectProperties(InstanceId,
        const std::unordered_map<std::string, ScriptPropValue>&) = 0;
    virtual bool hotReloadScript(const std::string& name, const std::string& source) = 0;
    virtual const std::string& lastError() const = 0;
};
```

- `LuaScriptBackend` = dünner Wrapper um den bestehenden `ScriptEngine` + die `horizon`-Registrierung aus `ScriptContext` (raw Lua C API bleibt unangetastet, `ScriptEngine.cpp` ändert sich praktisch nicht).
- `PyScriptBackend` = neu (§2).
- `ScriptContext` wird zu `ScriptHost`: besitzt beide Backends, hält `HorizonWorld*`/`PhysicsWorld*` (heute in der Lua-Registry unter `__horizonWorld`/`__horizonPhysics`, `ScriptContext.cpp:14-31`) und routet per Sprache.

**InstanceId-Kollision:** beide Backends zählen ab 1 (`ScriptEngine.cpp:111-112`). Der `ScriptHost` kodiert die Sprache in die oberen 8 Bit der öffentlichen InstanceId (`(uint64_t(lang) << 56) | backendId`), damit `m_scriptInstances` in `EditorApplication.cpp:957-963` und die uint32→InstanceId-Map im `CollisionSystem` (`CollisionSystem.h:12-40`) unverändert eine flache ID speichern können. Dispatch (`callOnUpdate` etc.) maskiert und routet.

### 1.2 Sprachauswahl: Dateiendung entscheidet, Component bleibt schlank

- **Import:** `.lua` → `ScriptLanguage::Lua`, `.py` → `ScriptLanguage::Python`. Die Endung wird beim Import bestimmt und als neues Feld `ScriptAsset::language` gespeichert (`Assets.h:85-88` bekommt ein `uint8 language`; hpak: entweder als 1-Byte-Feld im Asset-Header serialisieren oder — pragmatischer — beim Laden aus dem gespeicherten Asset-Namen/Quellpfad ableiten und nur im Loose-Format persistieren; CHUNK_SRC selbst bleibt reiner Text). Das Icon-Mapping kennt `.py` bereits (`EditorUI.cpp:3392`).
- **`ScriptComponent` (`ScriptComponent.h:8-16`) bleibt unverändert** — kein Sprachfeld. Die Sprache ist eine Eigenschaft des Assets, nicht der Komponente; die Komponente referenziert nur `scriptAssetId`. Damit funktionieren Prefabs/Szenen-Serialisierung ohne Migration.
- Aufrufstellen, die anzupassen sind: `EditorApplication.cpp:1440-1443` (createInstance → injectProperties → callOnStart) und `:957-970` (Update + Collision-Dispatch) gehen künftig durch den `ScriptHost`; der schaut die Sprache am Asset nach.

### 1.3 Gemeinsame Engine-API-Schicht

Die 13 `lua_horizon_*`-Funktionen (`ScriptContext.cpp:40-234`: log, getName, get/setPosition, get/setRotation, get/setScale, spawn, destroy, raycast, setVelocity, isGrounded) werden in sprachneutrale freie Funktionen extrahiert (`HE_Scene/ScriptApi.h`, z. B. `HE::ScriptApi::setPosition(HorizonWorld&, uint32_t, float, float, float)`). Lua- und Python-Bindings sind dann nur noch Marshalling-Hüllen um dieselben Funktionen — die API-Fläche bleibt garantiert 1:1 identisch, und neue API-Funktionen werden einmal geschrieben, zweimal gebunden.

---

## 2. PYTHON — Empfehlung: pocketpy v2.x (vendored, ~1 MB), nicht CPython

**Entscheidung: pocketpy v2.1.8, gepinnt, als `third_party/pocketpy/pocketpy.c|.h` ins Repo vendored.**

Ehrlicher Tradeoff für dich als Nutzer:

| | pocketpy | CPython (embeddable/pbs) |
|---|---|---|
| Ship-Kosten | ~1 MB im Binary, **null** Bundle-/Signier-/Notarisierungs-Aufwand | ~11 MB (Win-Zip) / ~25 MB (macOS via python-build-standalone) + rpath-Fixes + Codesigning von ~80 dylibs im DMG-Workflow |
| Sprache | ~80 % Python: Klassen, Vererbung, Decorators, Generatoren, f-Strings, Comprehensions. **Fehlt: Generator-Expressions (`sum(x for x in …)` ist SyntaxError!), async/await, try/finally, Mehrfachvererbung, Descriptors** | 100 % echtes Python, volle stdlib, PyPI |
| Stabilität | crash-fähig (SIGBUS auf legalem Code reproduziert) → Version pinnen + Engine-Testsuite | produktionsstabil, aber GIL serialisiert alles |
| Performance | ~CPython-3.9-Klasse, schneller als Lua | vergleichbar |

Für Gameplay-Glue eines Solo-Devs, dessen Skripte als Quelltext im .hpak leben, gewinnt pocketpy klar — vor allem, weil dein DMG-/Notarisierungs-Pipeline-Aufwand (Memory: macos-build-msl-validation) bei CPython massiv wachsen würde. **Eskalationskriterium:** Erst wenn „Modder schreiben echtes Python mit stdlib/PyPI" ein Produkt-Feature wird, auf CPython+pybind11 umsteigen — das Interface aus §1.1 macht das zu einem Backend-Tausch, nicht zu einem Redesign.

### 2.1 Skript-Kontrakt (Python-Seite)

Lua nutzt ein Modul-Table-Pattern (`ScriptEngine.h:13-17`). Python-idiomatisch ist eine Klasse:

```python
class Main(horizon.Behavior):
    # exportierte Properties = Klassenattribute mit typisierten Defaults
    speed = 2.5          # Float
    hp = 100             # Int
    label = "Player"     # String

    def on_start(self): ...
    def on_update(self, dt): ...
    def on_collision_enter(self, other_id): ...
    def on_collision_exit(self, other_id): ...
```

- Konvention: genau **eine** `horizon.Behavior`-Subklasse pro Datei (Name egal, Loader findet sie; 0 oder >1 = Ladefehler mit klarer Meldung).
- `self.entity_id` wird bei `createInstance` gesetzt (Analogon zu `ScriptContext.cpp:282`).
- Alle Callbacks optional (silent no-op) — identisch zur Lua-Semantik in `ScriptEngine.cpp:124-201`.
- **Properties:** `getScriptProperties` liest Klassenattribute mit Typen float/int/bool/str und mappt sie auf `ScriptPropType` (`ScriptTypes.h:6-22`). Das bestehende Editor-UI (`EditorUI.cpp:4911-4962`) arbeitet auf `ScriptPropDef` und ist damit **unverändert wiederverwendbar** — inklusive Overrides in `ScriptComponent.properties` und `injectProperties` vor `on_start` (Timing wie `EditorApplication.cpp:1440-1443`).
- **Hot-Reload:** Modul neu ausführen, dann bei allen Live-Instanzen `inst.__class__ = NewClass` setzen. Das ist exakt die Lua-Semantik aus `ScriptEngine.cpp:345-390` (Funktionen gepatcht, Datenfelder in `__dict__` bleiben erhalten) — in Python sogar einfacher. `ScriptSystem::pollHotReload` (`ScriptSystem.h:15-26`) bleibt wie es ist; nur `ctx.hotReloadScript` routet per Sprache.

### 2.2 Binding-Schicht

`PyScriptBackend` registriert ein natives Modul `horizon` über die pocketpy-C-API (`py_newnativefunc` + `PY_CHECK_ARGC`/`PY_CHECK_ARG_TYPE`), Rückgabe über `py_retval()`. Alle 13 Funktionen delegieren an die extrahierten `ScriptApi`-Funktionen (§1.3). Mehrfach-Rückgaben wie `getPosition → x, y, z` werden in Python als Tupel zurückgegeben. World/Physics-Pointer hängen als Member am Backend (kein Registry-Trick nötig wie bei Lua).

### 2.3 Pak/Asset-Integration und Shipping

- `.py` wird wie `.lua` importiert: Quelltext → `ScriptAsset.sourceCode`, CHUNK_SRC unverändert, `registerScript` (`ContentManager.cpp:698`) unverändert. Kein neuer Chunk-Typ.
- Shipping: pocketpy kompiliert in HE_Core hinein. **Nichts** zusätzlich in .app/DMG oder neben die .exe — der einzige echte Deployment-Vorteil dieser Wahl, und er ist groß.
- Absicherung: Version pinnen; kleine Engine-Testsuite gegen den Interpreter (die bekannten Lücken als Negativ-Tests: Genexpr = SyntaxError, `import a, b` = Bug); Skript-Ausführung als crash-fähig behandeln (CrashHandler 6.5 existiert). Eine Doku-Seite „Python-Dialekt-Grenzen" für dich als Skripter (kein Genexpr, kein async, kein try/finally, kein `__slots__`/`__del__`, int ist i64).

---

## 3. C++ — GameLogicLoader fertig bauen, nicht neu erfinden

### 3.1 Was schon da ist (viel)

- **ABI komplett definiert:** `IGameLogic.h:6-27` — `extern "C" HE_CreateGameLogic/HE_DestroyGameLogic` + `onStart/onUpdate/onStop(world)`.
- **Plattform-Layer fertig:** `DynLib.cpp:33-63` (LoadLibraryW/dlopen mit RTLD_NOW|RTLD_LOCAL, GetProcAddress/dlsym).
- **Frame-Integration fertig:** `GameLoop::tick(world, logic, dt)` ruft `logic->onUpdate` im Fixed-Timestep (`GameLoop.cpp:11-23`) — bekommt heute nur überall `nullptr`.
- **Verankerung fertig:** `Application.h:79,130` (`m_logicLoader` + Accessor).

### 3.2 Die Lücke (klein und klar umrissen)

1. **`GameLogicLoader.cpp:8-24` implementieren** (~60 Zeilen): `load()` = DynLib::load → beide Symbole auflösen → `HE_CreateGameLogic()`; `unload(world)` = `logic->onStop(world)` → `destroyFn_(logic_)` → `lib_.unload()`; `reload()` = unload + load — aber mit macOS-Regel: **vor jedem dlopen die frische dylib auf einen eindeutig nummerierten Namen kopieren** (`GameLogic_0007.dylib`), nie einen Pfad wiederverwenden, `dlclose` als best-effort behandeln (dyld-Caching + TLS/ObjC können das alte Image pinnen — Godot #90108). Verbot von `thread_local` im Game-Code dokumentieren.
2. **GameApplication-Wiring:** beim Start neben der Executable nach `GameLogic.dylib`/`GameLogic.dll` suchen; wenn vorhanden, `logicLoader().load()`, `onStart(world)` nach dem Szenen-Load, und `logic()` in `gameLoop().tick()` reichen. Fehlt die Lib: normaler Betrieb (reines Skript-Spiel).
3. **API-Fläche:** Da Engine und Game-Lib immer zusammen mit demselben Compiler gebaut werden (Solo-Dev, AppleClang/MSVC, gleiche CMake-Config), ist das Doom-3-Modell richtig: Game-Code linkt gegen die HE_Core/HE_Scene-Header und benutzt `HorizonWorld&` direkt — **kein** C-Funktionszeiger-Registry (Our Machinery), **kein** GameMemory-Arena-Protokoll (Handmade Hero). Regeln statt Maschinerie: (a) Persistenter Zustand gehört ins ECS, nicht in Member der IGameLogic-Instanz; (b) Reload = `onStop` → neue Lib → `onStart` (Zustandsverlust im Logic-Objekt akzeptiert, ECS überlebt); (c) keine Funktionszeiger/Objekte aus der Game-Lib engine-seitig cachen; (d) Debug/Release nicht mischen (Editor-Build-Button nutzt dieselbe Config wie der laufende Editor).
4. **Hot-Reload (nur Editor, Play Mode):** Model A — mtime der dylib pro Frame pollen (analog `ScriptSystem::pollHotReload`); bei Änderung `reload()` mit Copy-to-unique-Name. Dazu ein Editor-Button „Build & Reload", der `cmake --build <builddir> --target GameLogic` shellt (Model-B-UX auf Model-A-Unterbau; dein CLion-ninja-Pfad ist bekannt). **Hot-Reload ist hier nur auf macOS testbar** — Windows-Pfad (Copy wegen File-Lock, gleiche Logik) blind mitschreiben, symmetrisch halten.
5. **Projekt-Template aus dem Editor:** „File → New C++ Game Project" erzeugt einen Ordner mit `CMakeLists.txt` (`add_library(GameLogic SHARED src/GameLogic.cpp)`, Include-Pfade auf die Engine, Output-Dir = neben Editor-Executable bzw. konfigurierter Watch-Pfad) und `src/GameLogic.cpp` (IGameLogic-Skelett + die beiden `extern "C"`-Exports). Kein eigenes Build-Tool, kein Toolchain-Discovery — der Nutzer (du) hat CMake+ninja ohnehin.
6. **Shipping:** kompilierte Game-Lib liegt **neben der Executable, NICHT im .hpak** (Code-Signing, plattformspezifische Binaries, dyld-Constraints). Build-Pipeline (6.3) erweitern: dylib in die .app kopieren + im bestehenden Stamp-Check/Codesign-Schritt mitsignieren; auf Windows dll neben die exe. Packaged Builds laden einmal, kein Watcher (Kommentar in `GameLogicLoader.h:12-13` sagt das bereits). Ein `HORIZON_STATIC_GAME`-Static-Link-Target ist nice-to-have → Follow-up, nicht v1.

**Bewusst NICHT in v1:** per-Entity-C++-Behaviors über `ScriptComponent`. Falls später gewünscht: die Game-Lib registriert in `onStart` benannte Behaviors in einer `NativeBehaviorRegistry`, und ein dünnes Adapter-Backend löst `moduleName` dagegen auf — sauberer Follow-up, ändert nichts am jetzigen Design.

---

## 4. MEILENSTEINE (geordnet, testbar)

**A — Refactor (Voraussetzung, ~1 Sitzung):**
- A1: `IScriptBackend` extrahieren, `LuaScriptBackend`-Wrapper, `ScriptContext`→`ScriptHost`, Sprach-Tag in InstanceId-Highbits. **Test:** alle bestehenden Skript-Tests grün, Verhalten byte-identisch. macOS voll testbar.
- A2: `ScriptApi`-Extraktion der 13 Funktionen aus `ScriptContext.cpp:40-234`; Lua-Bindings rufen sie. **Test:** bestehende Tests grün.

**P — Python:**
- P1: pocketpy vendoren, `PyScriptBackend` mit load/exec/Fehlerpfad. **Test:** Unit-Tests (Kompilierfehler → lastError, exec). macOS + headless testbar.
- P2: `horizon`-Modul, 13 Bindings gegen `ScriptApi`. **Test:** Paritäts-Test Lua vs. Python (gleiches Verhalten pro Funktion, headless mit HorizonWorld).
- P3: Behavior-Klassen-Kontrakt, `createInstance`/`entity_id`, Callbacks. **Test:** Unit + Play-Mode (drehender Würfel per .py) — visuell via `scripts/he_shot.py` verifizierbar.
- P4: Properties (Extraktion aus Klassenattributen, Injection, Editor-UI). **Test:** Unit + Editor-Sichtprüfung.
- P5: `.py`-Import → ScriptAsset, hpak-Roundtrip (Language-Feld). **Test:** Pack/Load-Roundtrip-Test.
- P6: Hot-Reload (`__class__`-Patch) + `pollHotReload`-Routing. **Test:** Unit (Datenfeld überlebt, Funktion gepatcht).
- P7: Collision-Callbacks über CollisionSystem. **Test:** bestehendes Collision-Test-Muster in Python nachbauen.
- Alles auf macOS testbar; Windows blind, aber risikoarm (pocketpy = plattformneutrales C11).

**C — C++:**
- C1: `GameLogicLoader` implementieren. **Test:** Unit-Test mit Mini-Test-dylib, die die Testsuite selbst per CMake baut (load → onStart schreibt ins World → unload). macOS testbar; Windows-Zweig blind.
- C2: GameApplication-Wiring (Lib neben exe finden, in `tick` reichen). **Test:** Sample-Game-Lib bewegt Entity, headless-Shot.
- C3: Editor-Template-Generator + „Build & Reload"-Button. **Test:** manuell im Editor (macOS).
- C4: Hot-Reload im Play Mode (mtime-Watch, unique-name copy). **Test:** nur macOS, manuell + halbautomatisch (Test tauscht dylib aus, prüft neues Verhalten). Windows-Reload = blind, als Follow-up-Verifikation markieren.
- C5: Packaging (dylib in .app + Codesign-Stamp; dll neben exe). macOS end-to-end testbar über die bestehende DMG-Pipeline; Windows blind.

**Eigene Follow-ups (nicht Teil dieses Vorhabens):** Windows-Verifikationspass (beide Sprachen), `NativeBehaviorRegistry` (per-Entity-C++), `HORIZON_STATIC_GAME`, CPython-Eskalation.

---

## 4b. IMPLEMENTIERUNGSSTATUS (2026-07-02)

**Backend-Entscheidung:** CPython (echtes Python 3.14, per `find_package(Python3 COMPONENTS Development.Embed)` → `HE_HAVE_PYTHON`) statt pocketpy — bewusste User-Entscheidung (siehe Header). Ohne CPython kompiliert das Backend zu No-op-Stubs (`PyScriptBackend::available()==false`), Callers brauchen keine `#ifdef`s.

**Fertig & getestet (macOS, 529 Cases / 7188 Assertions grün):**
- **A1/A2:** `IScriptBackend` + `ScriptApi` (13 Funktionen) extrahiert; Lua- und Python-Backend marshallen beide gegen `ScriptApi`. Routing in `ScriptContext`: Sprache im High-Byte der `InstanceId` (Lua==0 → bit-identisch), `backendForName` für namensbasierte Calls, `loadScript(name, src, lang)`. Coexistence-Test (Lua+Python gleichzeitig, keine Id-Kollision) grün.
- **P1–P3, P6, P7:** `PyScriptBackend.cpp` (pImpl, `horizon`-Inittab-Modul, `horizon.Behavior`-Kontrakt, `self.entity_id`, `on_start`/`on_update`/`on_collision_enter`/`on_collision_exit`, fehlender Handler = No-op-Erfolg, Exception → `lastError`, Hot-Reload via `__class__`-Patch mit `__dict__`-Erhalt). 16 Test-Cases in `tests/test_python_scripting.cpp` (12 backend-direkt + 4 durch `ScriptContext`).
- **P4 (Backend):** `getScriptProperties` (typisierte Klassenattribute → `ScriptPropDef`), `injectProperties`. Getestet.
- **P5:** Sprache lebt am `ScriptAsset::language` (nicht am Pfad — Skripte sind `.hasset`-Blobs ohne Endung), persistiert als 1-Byte `CHUNK_SLNG` (fehlt → Lua, back-compat). Roundtrip über Store/LZ4/Zstd-Pack + Mount getestet; Script-Blobs gehen beim Packen verbatim durch (`HpakWriter.cpp:57`).
- **Editor:** Content-Create-Menü „Script (Lua)" / „Script (Python)" mit Sprach-Starter-Template und `CHUNK_SLNG` bereits im Stub (`tryCreate` schreibt META direkt, umgeht den CM-Save-Pfad). Play-Mode-Routing über `asset->language`.

**Bewusst zurückgestellt (Follow-up):**
- **Editor-Property-*Preview* für Python** (`EditorUI.cpp:4912`, `propScriptEngine` ist ein Lua-only `ScriptEngine`). Skripte laufen und `injectProperties` funktioniert auch ohne; nachrüstbar, da `getScriptProperties` weltfrei ist (nur load+read, kein `g_world`). Vorsicht: ein zweites `PyScriptBackend` im Editor teilt sich die prozessglobalen `g_world`/`g_physics`-Statics.
- **GameApplication treibt keine Skripte** (weder Lua noch Python) — Skripte laufen nur im Editor-Play-Mode. Pre-existing (`GameApplication` tickt keine ECS-Systeme). Kein Regress dieses Vorhabens.
- **C++-Scripting C3/C4/C5** (Editor-Template/Build-Button, mtime-Hot-Reload, Packaging) offen.

---

## 5. RISIKEN / BEWUSST NICHT BAUEN

**Nicht bauen:**
- **CPython/pybind11 jetzt** — 11-25 MB pro Plattform, macOS-rpath/Notarisierung von ~80 Shared Objects, GIL. Nur bei explizitem Modding-Feature.
- **MicroPython** — Footprint-Vorteil auf Desktop irrelevant, schlechteste Embedding-Ergonomie.
- **Live++/blink/jet-live/RCC++** — Live++ ist Windows-only + Abo (Engine ist macOS-first); RCC++ invasiv und alternd; jet-live ein Forschungsspielzeug.
- **GameMemory-Arena/State-Preserving-C++-Reload** — Handmade-Hero-Maschinerie lohnt nicht; ECS ist der persistente Zustand, Reload = onStop/onStart.
- **C-ABI-Funktionszeiger-Registry** — nur nötig bei Fremd-Compilern/Binary-Mods; du kontrollierst beide Seiten.
- **Game-Libs im .hpak** — kompilierter Code gehört neben die Executable (Signing, Plattform-Binaries).
- **Python-Sandboxing** — Skripte sind Trusted Content wie Lua heute; pocketpy-Crashfähigkeit wird über Pinning + Testsuite + CrashHandler adressiert, nicht über eine Sandbox.
- **Sprachfeld im ScriptComponent** — Sprache lebt am Asset; Komponente bleibt migrationsfrei.

**Risiken mit Gegenmaßnahme:**
- pocketpy-Interpreter-Bugs (SIGBUS auf legalem Code reproduziert) → Version pinnen, Engine-seitige Interpreter-Testsuite, Skriptausführung als crash-fähig einstufen.
- Python-Dialekt-Lücken frustrieren beim Schreiben (Genexpr/async/finally) → einseitige Doku „HorizonPython-Grenzen" + klare SyntaxError-Meldungen.
- macOS-dlclose pinnt alte Images → unique-name copies, dlclose best-effort, `thread_local`-Verbot im Game-Code.
- Debug/Release-Mismatch Engine↔Game-dylib → Editor-Build-Button erzwingt die Config des laufenden Editors.
- InstanceId-Kollision Lua/Python → Sprach-Tag in Highbits (§1.1), CollisionSystem/EditorApplication-Maps unverändert.

**Schlüsseldateien:** `src/HE_Core/include/Scripting/ScriptEngine.h`, `src/HE_Core/include/Scripting/ScriptTypes.h`, `src/HE_Scene/include/HorizonScene/ScriptContext.h`, `src/HE_Scene/src/ScriptContext.cpp`, `src/HE_Scene/include/HorizonScene/Components/ScriptComponent.h`, `src/HE_Scene/include/HorizonScene/ScriptSystem.h`, `src/HE_Core/include/ContentManager/Assets.h` (ScriptAsset:85-88), `src/HE_Core/include/IGameLogic.h`, `src/HE_Core/include/Application/GameLogicLoader.h`, `src/HE_Core/src/Application/GameLogicLoader.cpp` (Stubs:8-24), `src/HE_Core/src/Platform/DynLib.cpp`, `src/HE_Core/src/Application/GameLoop.cpp` (tick:11-23), `src/HE_Editor/EditorApplication.cpp` (:957-970, :1440-1443), `src/HE_Editor/EditorUI.cpp` (:3392, :4911-4962).