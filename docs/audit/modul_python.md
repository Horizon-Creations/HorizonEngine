# Modulbericht Python – HorizonPython

Pfad: `src/HE_Python` · Teilberichte: 1 (`_parts/Python_*.md`)

## Modulzusammenfassung

**14 Funde**: Hoch 1, Mittel 6, Niedrig 7 · Typ: Vereinfachung 8, Performance 6, Beides 0 · Kategorien: A 5, B 2, C 3, D 1, E 0, F 3

| Teilbericht | Hoch | Mittel | Niedrig |
|---|---|---|---|
| Python_1.md | 1 | 6 | 7 |

### Hoch-Funde (Index)

- **PYT-101** – Hook-Aufruf: doppelte Attributsuche mit frisch erzeugten Python-Strings, keine Präsenz-Cache pro Klasse

## Zusammenfassungen der Teilberichte

**Python_1.md**: Das Plugin ist eine einzige Datei (1728 Zeilen) mit klarer Struktur und sehr ausführlichen Begründungskommentaren. Das Problem liegt weniger in der Logik als im Aufrufpfad: Jeder per-Frame-Hook (`on_update`, Input, Kollision) und jeder `horizon.<group>.<fn>`-Aufruf bezahlt unnötig viel Fixkosten (String-Erzeugung für Attributnamen, doppelte Attributsuche, Python-Lambda-Zwischenframe, Heap-Vektoren). Daneben gibt es viel Copy-Paste (Hook-Aufrufer, Float-Sequenz-Parser, Vec-nach-Liste-Konverter) und zwei kleine Korrektheitsrisiken (`std::to_string(float)` im Codegen, `long` für Entity-IDs). Funde: 1 Hoch, 6 Mittel, 7 Niedrig. Keine Messdaten vorhanden; alle Effektangaben sind Schätzungen.

---

## Cross-Modul-Hinweise

**Python_1.md**

- `src/HE_Core/include/Types/TypeRegistry.h:155` – `getStruct(const std::string&, StructDef&)` liefert eine Kopie; ein `const StructDef* findStruct(...)` würde mehrere Konverter (Python wie Lua `luaToStructValue`) entlasten (PYT-103).
- `src/HE_Scene/src/EngineApi.cpp:8773` – `find(const std::string&)` ist bereits indiziert (gut), erzwingt aber einen `std::string`-Temp je Aufruf; eine `string_view`-Überladung oder Zeiger-Bindung (PYT-102) würde das entfernen.
- `src/HE_Editor/InspectorPanel.cpp:3250` – ruft `getScriptProperties` im Zeichencode jedes Frames auf (PYT-107); Cache dort oder im Backend.
- `src/HE_Scene/.../ScriptContext.cpp` (Lua-Twin der Struct-Bootstrap-Codegen) und `ScriptEngine.cpp` – dieselben Dispatch-Kosten (Attributpräsenz) sollten auf der Lua-Seite geprüft werden; Hot-Path-Aufrufer von `callOnUpdate` loggen vermutlich Fehler pro Frame (nicht geprüft).

## Unsicher / Messung nötig

**Python_1.md**

- Tatsächlicher Anteil von Dispatch-Overhead (PYT-101/102) an der Skriptzeit: Benchmark mit N=1k/5k Instanzen, leerem `on_update`, und Schleife mit `horizon.math.*`.
- Ob Hooks zur Laufzeit an Instanzen gesetzt werden (relevant für Hook-Bitmasken-Cache, PYT-101).
- Ob entt-Entity-IDs ≥ 2^31 praktisch auftreten (PYT-113).
- Ob `HasAttr` in der unterstützten CPython-Version (3.8-3.13) bei fehlendem Attribut eine Exception allokiert (versionsabhängig).
- Größe typischer Struct-/Map-Werte über die Python-Grenze (PYT-103, PYT-112).


---
# Funde (vollständig, nach Teilbericht)

## Python Slice 1 – src/HE_Python/src/PyScriptBackend.{h,cpp}

## Zusammenfassung

Das Plugin ist eine einzige Datei (1728 Zeilen) mit klarer Struktur und sehr ausführlichen Begründungskommentaren. Das Problem liegt weniger in der Logik als im Aufrufpfad: Jeder per-Frame-Hook (`on_update`, Input, Kollision) und jeder `horizon.<group>.<fn>`-Aufruf bezahlt unnötig viel Fixkosten (String-Erzeugung für Attributnamen, doppelte Attributsuche, Python-Lambda-Zwischenframe, Heap-Vektoren). Daneben gibt es viel Copy-Paste (Hook-Aufrufer, Float-Sequenz-Parser, Vec-nach-Liste-Konverter) und zwei kleine Korrektheitsrisiken (`std::to_string(float)` im Codegen, `long` für Entity-IDs). Funde: 1 Hoch, 6 Mittel, 7 Niedrig. Keine Messdaten vorhanden; alle Effektangaben sind Schätzungen.

---

### PYT-101 – Hook-Aufruf: doppelte Attributsuche mit frisch erzeugten Python-Strings, keine Präsenz-Cache pro Klasse
- **ID**: PYT-101
- **Kategorie**: B / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: PyScriptBackend.cpp:1368-1484 (alle `callOn*`), 1424-1431 (`callNotifyMethod`), 1500-1509 (`callOnNetEvent`)
- **Problem**: Jeder Hook-Aufruf macht `PyObject_HasAttrString(obj, "on_update")` und danach `PyObject_CallMethod(obj, "on_update", "f", dt)`. Beide C-Strings werden bei jedem Aufruf in ein neues `str`-Objekt umgewandelt (Allokation + Hash), zwei Attributsuchen (Instanz-Dict + MRO), `HasAttr` auf ein fehlendes Attribut baut je nach CPython-Version ein AttributeError-Objekt auf und verwirft es. `CallMethod` mit Format-String parst zusätzlich `"f"`/`"sff"` per `Py_BuildValue`. Das läuft pro Instanz und Frame (`on_update`) sowie pro Input-Event/Kollision, auch für Instanzen, die den Hook gar nicht definieren (häufigster Fall: Skript ohne `on_collision_enter` etc.). Zudem löst `callOnX` zuerst `findInstance` per `unordered_map` aus.
- **Code-Auszug**:
```cpp
PyObject* obj = m_impl->findInstance(id);
if (!obj || !PyObject_HasAttrString(obj, "on_update")) return true;
PyObject* r = PyObject_CallMethod(obj, "on_update", "f", dt);
```
- **Lösung**: (1) Hook-Namen einmal als interned `PyObject*` anlegen (Tabelle `enum class Hook` → `PyObject*`, bei Konstruktion via `PyUnicode_InternFromString`). (2) In `loadScript`/`hotReloadScript` pro Klasse eine Bitmaske „definierte Hooks" berechnen (`PyObject_HasAttr(cls, name)`); `Inst` hält Zeiger auf diese Klassen-Info. Hook ohne Bit = sofort `return true`, null Python-Kosten. (3) Aufruf mit `PyObject_CallMethodOneArg` / `PyObject_CallMethodNoArgs` bzw. Vectorcall (`PyObject_VectorcallMethod`) statt Format-String. Dynamisch pro Instanz gesetzte Hooks (Attribut an der Instanz) sind ein Randfall: entweder Bitmaske nur als Negativ-Cache für Klassen ohne den Namen nutzen und bei Instanz-Dict-Treffer nicht cachen, oder Verhalten dokumentieren. (4) Alle Hooks über einen Helper `callHook(id, Hook, args...)` laufen lassen (siehe PYT-104).
- **Erwarteter Effekt**: Schätzung: pro nicht definiertem Hook ~100-300 ns gespart, pro definiertem `on_update` ~30-50 % der Dispatch-Overhead-Zeit. Bei tausenden Skriptinstanzen messbar; Skript-Body dominiert aber bei echter Arbeit.
- **Risiko / Verifikation**: Skripte, die Hooks zur Laufzeit an der Instanz setzen (`self.on_update = ...`) bzw. Hot-Reload ändern die Hook-Menge → Maske bei Reload neu berechnen und Instanz-Override-Fall per Test abdecken (Skript-Tests in `tests/` für Python-Backend, z. B. nach `test_*script*` greppen). Vorher/nachher mit 5000 Instanzen messen.

### PYT-102 – `_engineCall`-Pfad: Python-Lambda-Zwischenframe, String-Lookup und Heap pro Aufruf
- **ID**: PYT-102
- **Kategorie**: B / C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: PyScriptBackend.cpp:710-748 (`py_engineCall`), 917-949 (`bootstrapEngineApiGroups`), 56-78 (`apiCtx`)
- **Problem**: Jede Registry-Funktion (`horizon.math.*`, `physics.*`, `audio.*` …) wird als Python-Lambda `lambda *a, _id='…': horizon._engineCall(_id, *a)` exportiert. Pro Aufruf: Lambda-Frame, `*a`-Tupel packen/entpacken, globaler Lookup `horizon` + Attribut `_engineCall`, in C++ `PyUnicode_AsUTF8` + `HE::api::find(const std::string&)` (temporärer `std::string` + Hash), `apiCtx()` (Kopie aller Dienste), `std::vector in` (Heap), `fn->invoke` liefert `std::vector` (Heap), `PyList_New(0)` + `PyList_Append` und bei ≥2 Ergebnissen `PyList_AsTuple` (zweite Allokation). `math.*` wird typischerweise pro Frame und Entity benutzt.
- **Code-Auszug**:
```cpp
src += "horizon." + group + "." + name +
       " = lambda *a, _id='" + id + "': horizon._engineCall(_id, *a)\n";
...
const HE::api::ApiFn* fn = id ? HE::api::find(id) : nullptr;
...
PyObject* out = PyList_New(0);
```
- **Lösung**: Pro Registry-Zeile ein `PyCFunction` mit gebundenem `ApiFn*` erzeugen (`PyCMethod_New`/`PyCFunction_New` mit `self` = `PyCapsule` oder `PyLong` des Zeigers; Registry-Vektor ist laut `EngineApi.cpp:8773ff` prozessstabil) und direkt per `PyObject_SetAttrString` an `horizon.<group>` hängen. Dadurch entfallen Bootstrap-String-Codegen, Lambda-Frame, Id-Lookup und `PyUnicode_AsUTF8`. Ergebnis direkt bauen: bei `results.size()==1` das Objekt zurückgeben, sonst `PyTuple_New(n)` mit `PyTuple_SET_ITEM`. `in`/Ergebnis mit `reserve` oder kleinem Stack-Puffer. Die drei `net.*`-Überschreibungen (944-946) als reguläre `METH_VARARGS`-Funktionen direkt an `horizon.net` setzen statt per Lambda-Reassign.
- **Erwarteter Effekt**: Schätzung: 30-50 % weniger Overhead pro Registry-Aufruf (nur Dispatch, nicht der Registry-Body).
- **Risiko / Verifikation**: Script-sichtbares Verhalten (Skalar-vs.-Tupel-Rückgabe, `__name__`/`repr` der Funktion, Fehlertyp bei zu wenigen Argumenten) muss identisch bleiben; Lua/Python-Paritätstests prüfen. Vor dem Umbau mit einer Schleife aus 1e6 `horizon.math.clamp` messen.

### PYT-103 – Struct-/Wert-Konvertierung kopiert `HorizonCode::Value` und `StructDef` pro Element
- **ID**: PYT-103
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: PyScriptBackend.cpp:378-405 (`pyFieldValueToObj`), 462-478 (`pyStructValueToDict`), 639-656 (`pyObjToStructValue`), 735-737, 1519-1556
- **Problem**: Zum „Entpacken" eines Array-/Map-Elements wird jedes Element komplett kopiert, nur um `isArray`/`container` zurückzusetzen (`HorizonCode::Value item = v.items[i]; item.isArray = false;`). `Value` trägt `std::string`, mehrere Vektoren (items, keys) und Typnamen → tiefe Kopien, bei verschachtelten Structs/Maps rekursiv. `pyStructValueToDict` und `pyObjToStructValue` legen pro (verschachteltem) Struct ein `HE::StructDef def` an und füllen es per `getStruct(..., def)` (Kopie von Feldvektor samt Namen) plus bei letzterem `makeDefaultValue`. In `py_engineCall` wird `res[i]` für Array-Ergebnisse kopiert (Zeile 735), nur um `isArray = true` zu setzen. Betrifft RPC-Argumente, `on_rep_`/`on_changed_`-Callbacks und Listen-Resultate wie `physics.raycastAll`.
- **Code-Auszug**:
```cpp
HorizonCode::Value item = v.items[i];
item.isArray = false;
item.container = HorizonCode::ContainerKind::None;
PyList_SetItem(l, (Py_ssize_t)i, pyFieldValueToObj(item, depth + 1));
```
- **Lösung**: Den Konverter in `valueToObj(const Value&, int depth, bool asElement)` aufteilen: `asElement=true` ignoriert `isArray`/`container` (Schalter statt Kopie). Für Array-Ergebnisse in `py_engineCall` `res` nicht-const halten und `std::move`en bzw. denselben Schalter nutzen. `StructDef` per `const StructDef*`/Referenz aus der Registry holen, falls `TypeRegistry` einen nicht-kopierenden Zugriff bietet (sonst dort `const StructDef* findStruct()` ergänzen; Cross-Modul, siehe unten).
- **Erwarteter Effekt**: Schätzung: bei großen Arrays/verschachtelten Structs 2-5× weniger Allokationen in der Konvertierung; bei Skalaren kein Unterschied.
- **Risiko / Verifikation**: Rein interne Umstellung. Absichern mit bestehenden Struct-/Map-Roundtrip-Tests (nach `PyStruct`/`Python` in `tests/` greppen); ein Test mit Map-in-Struct-in-Array ergänzen.

### PYT-104 – Elf nahezu identische Hook-Aufrufer
- **ID**: PYT-104
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: PyScriptBackend.cpp:1368-1484, 1590-1599
- **Problem**: `callOnStart/Update/CollisionEnter/Exit/BeginOverlap/EndOverlap/InputAxis/InputAxis2D/Timer/CheatDetected/UIEvent/NetEvent` wiederholen exakt denselben 5-Zeiler (findInstance → HasAttr → CallMethod → Fehler → DECREF). Nur `callNotifyMethod` ist abstrahiert. Jede Änderung (z. B. PYT-101, Fehlerkontext) muss an ~12 Stellen erfolgen.
- **Code-Auszug**:
```cpp
PyObject* r = PyObject_CallMethod(obj, "on_timer", "i", handle);
if (!r) { m_lastError = takePyError(); return false; }
Py_DECREF(r); return true;
```
- **Lösung**: Ein privater Helper `template<class... A> bool callHook(InstanceId, const char* name, const char* fmt, A... a)` (bzw. nach PYT-101 mit `Hook`-Enum) und `PyObject* finish(PyObject* r)` für die Fehlerbehandlung (`r ? Py_DECREF(r), true : (m_lastError = takePyError(), false)`). Jeder `callOnX` wird eine Zeile. `callOnCollisionEnter/Exit/BeginOverlap/EndOverlap` unterscheiden sich nur im Namen.
- **Erwarteter Effekt**: ca. -90 Zeilen, ein Ort für Hook-Optimierungen; keine Laufzeitänderung.
- **Risiko / Verifikation**: Gering. `callRpc` (liefert `true` nach Fehler) und `callOnRep/Changed` behalten ihre abweichende Semantik (Rückgabewert) bewusst bei.

### PYT-105 – Handgeschriebene `py_*`-Shims: ~40 Funktionen mit gleicher Form
- **ID**: PYT-105
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: PyScriptBackend.cpp:81-319
- **Problem**: Die meisten Shims sind `ParseTuple → ScriptApi::x(*g_world, id, …) → Py_RETURN_NONE/Bool`; `py_getVec/py_setVec/py_setVecApi` zeigen, dass die Duplikation schon teilweise erkannt wurde, aber `getUIPosition/getUISize`, `showWidget/hideWidget/destroyWidget`, `isUIVisible/isWidgetVisible/isGrounded` etc. sind weiterhin Einzelkopien. Ein `if (x) Py_RETURN_TRUE; Py_RETURN_FALSE;` kommt 6× vor (`PyBool_FromLong` genügt).
- **Code-Auszug**:
```cpp
if (ScriptApi::isWidgetVisible(*g_world, (int)id)) Py_RETURN_TRUE;
Py_RETURN_FALSE;
```
- **Lösung**: Kleine Helper `retBool(bool)` und je ein `idCall(args, fn)` für „(long id) → void"-Varianten; keine Template-Metaprogrammierung nötig, Lambdas ohne Capture reichen (`static PyObject* py_hideWidget = …` bleibt als Tabelleneintrag). Bewusst nicht die Registry-Route nehmen (Arity-Bruch, siehe Kommentar 331-335).
- **Erwarteter Effekt**: -60..80 Zeilen. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Gering; Signaturen/Docstrings in `kHorizonMethods` unverändert lassen.

### PYT-106 – Float-Sequenz-Parser und Vec-nach-Liste viermal kopiert, Fehlerzustand bleibt hängen
- **ID**: PYT-106
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (+ kleine Korrektheit)
- **Ort**: PyScriptBackend.cpp:347-366, 414-446 (zu Python), 569-616 (von Python)
- **Problem**: `pyToVec3` sowie die `case`-Zweige Vec2/Vec3/Vec4/Color von `pyObjToFieldValue` sind dieselbe Schleife mit anderem N und Default; ebenso bauen `pyVec3List` und die Vec2/3/4/Color-Zweige von `pyFieldValueToObj` Listen mit handgeschriebenen `PyList_SetItem`-Reihen. `PySequence_Size(o)` wird in jeder Schleifeniteration neu berechnet. Wenn `PyNumber_Check` true ist, aber `PyFloat_AsDouble` fehlschlägt (z. B. `__float__` wirft), bleibt ein Python-Fehler gesetzt (nur Double/Int/Float-Zweige rufen `PyErr_Clear`), der später als scheinbar fremder Fehler in der nächsten C-API-Operation auftaucht.
- **Code-Auszug**:
```cpp
for (int i = 0; i < 4 && i < (int)PySequence_Size(o); ++i)
{
	PyObject* it = PySequence_GetItem(o, i);
	if (it && PyNumber_Check(it)) r[i] = (float)PyFloat_AsDouble(it);
	Py_XDECREF(it);
}
```
- **Lösung**: `template<int N> void readFloats(PyObject* o, float (&dst)[N])` bzw. `bool pyReadFloats(PyObject*, float* dst, int n)` (Größe einmal vor der Schleife, `PyErr_Clear()` im Fehlerfall) und `PyObject* pyFloatList(const float* v, int n)` (`PyList_New(n)` + Schleife). `glm::vec*` per `&v.x`/`glm::value_ptr`. Reduziert 7 Blöcke auf je 2 Zeilen.
- **Erwarteter Effekt**: ca. -70 Zeilen; Zeilen-Fehlerzustand konsistent.
- **Risiko / Verifikation**: Verhalten identisch bei gültigen Eingaben; für fehlerhafte `__float__` neuer Test (Struct-Feld mit Objekt, dessen `__float__` wirft).

### PYT-107 – `getScriptProperties` kopiert das gesamte Klassen-`__dict__` und wird vom Inspector pro Frame aufgerufen
- **ID**: PYT-107
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: PyScriptBackend.cpp:1601-1651; Aufrufer HE_Editor/InspectorPanel.cpp:3250
- **Problem**: Pro Aufruf: `__dict__` holen (mappingproxy), `PyMapping_Items` materialisiert eine Liste aus Tupeln für ALLE Klassenattribute (inkl. Methoden und Dunder), danach Filterung. Der Aufruf geschieht im Inspector-Zeichencode (`auto defs = ctx.propScriptEngine->getScriptProperties(s->moduleName);`), d. h. pro Frame und pro sichtbarer Script-Komponente. Rückgabe ist ein neuer `std::vector<ScriptPropDef>` mit Strings. (Editor-only, daher nicht Hoch.)
- **Code-Auszug**:
```cpp
PyObject* dict = PyObject_GetAttrString(it->second, "__dict__");
PyObject* items = PyMapping_Items(dict); // list of (key, value)
```
- **Lösung**: Ergebnis pro Klasse cachen (`classes[name]` → `{PyObject* cls; std::vector<ScriptPropDef> props;}`), in `loadScript`/`hotReloadScript` einmal berechnen, `getScriptProperties` gibt eine Kopie/Referenz zurück. Alternativ im Inspector pro Frame nicht neu abfragen (Cross-Modul). Der Cache muss bei Reload invalidiert werden, Defaults ändern sich nur dann (Klassenattribute können zur Laufzeit mutiert werden; das ist für Editor-Defaults irrelevant, aber ein Verhaltensunterschied).
- **Erwarteter Effekt**: Schätzung: pro Aufruf 5-20 µs → ~0.5 µs; nur Editor-Frame-Zeit.
- **Risiko / Verifikation**: Test: Skript laden, Properties lesen, `hotReloadScript` mit geändertem Default, erneut lesen. Messen, ob es überhaupt sichtbar ist.

### PYT-108 – `std::to_string(float)` im Struct-Default-Codegen verliert Präzision und kann ungültiges Python erzeugen
- **ID**: PYT-108
- **Kategorie**: F (Korrektheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: PyScriptBackend.cpp:1005-1035 (`lit`)
- **Problem**: Double wird mit `%.17g` ausgegeben, Float/Vec2/Vec3/Vec4/Color/Transform dagegen mit `std::to_string` (feste 6 Nachkommastellen, Locale-abhängig; `1e-7f` → `0.000000`, `NaN`/`inf` → `nan`/`inf`, was in Python ein `NameError` bzw. ungültiger Literal ist). Ein Fehler im Bootstrap-Text wird durch `PyRun_SimpleString` + `PyErr_Clear()` verschluckt (der Interpreter druckt nach stderr), danach fehlen ALLE `horizon.structs.*`/`enums.*` ab der Stelle. Zusätzlich verwendet `std::snprintf` ohne `#include <cstdio>`.
- **Code-Auszug**:
```cpp
case P::Float:
default:        return std::to_string(v.f);
case P::Vec3:   return "[" + std::to_string(v.v3.x) + ...
```
- **Lösung**: Ein `fmtNum(double)`-Helper mit `%.9g` (Float) / `%.17g` (Double), `nan`→`float('nan')`, `inf`→`float('inf')`; `<cstdio>` inkludieren. Alternativ Defaults nicht als Quelltext, sondern direkt via `pyFieldValueToObj` + `PyObject_Call`-Closure bauen: `horizon.structs.<Name>` wird ein `PyCFunction` mit Struct-Pfad, das bei Aufruf `pyStructValueToDict(makeDefaultValue(path))` liefert. Das entfernt die zweite Serialisierung (die auch noch mit der Lua-Seite synchron gehalten werden muss) und damit auch das `std::function`-Konstrukt (siehe PYT-111).
- **Erwarteter Effekt**: Korrekte Defaults; Bootstrap schneller (kein Parse), weniger Code.
- **Risiko / Verifikation**: Test: Struct mit `float 1e-7`, `NaN`-Default und Vec3 `0.1234567` anlegen, `horizon.structs.X()` im Skript vergleichen. Lua-Twin (`ScriptContext.cpp`) prüfen, ob dasselbe Problem besteht.

### PYT-109 – Instanztabelle: String pro Instanz und lineare Scans bei Unload/Reload
- **ID**: PYT-109
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: PyScriptBackend.cpp:1183-1192, 1325-1331, 1355, 1685-1691
- **Problem**: `Inst` speichert `std::string script` pro Instanz (Heap bei langen Pfaden wie `Scripts/Enemies/Grunt.py`), `unloadScript` und `hotReloadScript` laufen linear über alle Instanzen und vergleichen Strings. `findInstance` ist ein `unordered_map<InstanceId,…>`-Lookup bei jedem Hook, obwohl `InstanceId` fortlaufend vergeben wird (`nextId++`).
- **Code-Auszug**:
```cpp
struct Inst { PyObject* obj = nullptr; std::string script; };
std::unordered_map<InstanceId, Inst> instances;
```
- **Lösung**: Statt des Strings einen `ClassInfo*`/Klassenindex (passt zur Hook-Maske aus PYT-101) speichern; Vergleich wird ein Pointer-Vergleich. Instanzen in `std::vector<Inst>` mit Freiliste/Generation statt Hash-Map, wenn die Host-Seite IDs nicht wiederverwendet (prüfen: `IScriptBackend.h`).
- **Erwarteter Effekt**: Schätzung: wenige Prozent beim Dispatch, deutlich weniger Heap bei vielen Instanzen. Unload/Reload sind selten → sonst Niedrig.
- **Risiko / Verifikation**: `InstanceId`-Semantik des Hosts (Stabilität nach `destroyInstance`) prüfen; Tests für Create/Destroy/Reload.

### PYT-110 – Tote Zustände in `Impl` (`physics`, `content`, teils `world`)
- **ID**: PYT-110
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: PyScriptBackend.cpp:1179-1181, 1264, 1270, 1198, 1257
- **Problem**: `m_impl->physics` und `m_impl->content` werden gesetzt, aber nie gelesen (Grep: nur Zuweisungen); gültig sind ausschließlich die Datei-Statics `g_physics/g_content`. `world` wird nur im Destruktor zum Vergleich benutzt. Zwei Wahrheiten für denselben Zustand.
- **Code-Auszug**:
```cpp
m_impl->physics = pw;
g_physics = pw;
```
- **Lösung**: Felder entfernen (nur `world` für den Destruktorvergleich behalten). Alternativ die Statics durch einen „aktives Backend"-Zeiger ersetzen und daraus lesen; das würde zugleich das fragile Nullsetzen im Destruktor (1251-1257) unnötig machen.
- **Erwarteter Effekt**: Weniger Zustand, weniger Verwechslungsgefahr.
- **Risiko / Verifikation**: Gering.

### PYT-111 – Rekursives `std::function` und doppelter Name `pyStr`
- **ID**: PYT-111
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: PyScriptBackend.cpp:959-969, 970-1036, 1058
- **Problem**: Das Literal-Generieren nutzt eine rekursive Lambda über `std::function` (Heap + indirekter Aufruf pro Knoten), und die lokale Lambda `pyStr(const std::string&)` verdeckt die namespace-weite Funktion `pyStr(PyObject*)` (Zeile 1058) – gleiche Namen, verschiedene Aufgaben. Das Ganze läuft bei jeder Backend-Konstruktion (Play-Start im Editor).
- **Code-Auszug**:
```cpp
std::function<std::string(const HorizonCode::Value&)> lit =
	[&](const HorizonCode::Value& v) -> std::string {
```
- **Lösung**: `lit` und `quote` als freie Funktionen im anonymen Namespace (`pyLiteral`, `pyQuote`); `<functional>` entfällt. Entfällt komplett, falls PYT-108 (direkte Konstruktion) umgesetzt wird.
- **Erwarteter Effekt**: Lesbarkeit; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Gering.

### PYT-112 – Doppelte Dedup-Schleifen O(n²) beim Map-/Set-Import
- **ID**: PYT-112
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: PyScriptBackend.cpp:507-516, 530-543
- **Problem**: Für jeden neuen Map-Schlüssel bzw. Set-Eintrag wird linear gegen alle bisherigen verglichen (`scalarValueEquals`). Bei einem Python-Dict/-Liste mit n Einträgen O(n²). Nur relevant bei großen Containern pro Struct; sonst unproblematisch.
- **Code-Auszug**:
```cpp
for (const V& have : map.keys)
	if (HorizonCode::scalarValueEquals(have, key, f.keyType)) { dup = true; break; }
```
- **Lösung**: Für Map mit Python-Dict ist Duplikat nur möglich, wenn zwei verschiedene Python-Schlüssel auf denselben Engine-Schlüssel kollabieren (z. B. `1` und `1.5` bei Int). Für skalare Typen Int/String ein `unordered_set` der kanonischen Werte führen, sonst bei Linearscan bleiben und ab z. B. 64 Einträgen umschalten. Nur umsetzen, wenn Messung/Nutzung große Container zeigt.
- **Erwarteter Effekt**: Nur bei n > ~100 relevant (Schätzung).
- **Risiko / Verifikation**: Gleichheitssemantik von `scalarValueEquals` (Float-Toleranz?) muss erhalten bleiben → unsicher, nicht ohne Test.

### PYT-113 – Entity-IDs als `long` / `"l"` (Windows 32 Bit) und `PY_SSIZE_T_CLEAN` nach Standard-Headern
- **ID**: PYT-113
- **Kategorie**: F (Plattform)
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: PyScriptBackend.cpp:90, 130, 827-828, 17-26, 1-7
- **Problem**: Entity-IDs sind `uint32_t`, werden aber mit `"l"`/`long` gelesen (unter Windows 32 Bit signed) und teils als `PyLong_FromUnsignedLong`, teils `"k"` zurückgegeben. Werte ≥ 2^31 führen unter Windows zu `OverflowError`, unter Linux/macOS (64-Bit `long`) nicht → plattformabhängiges Verhalten (unsicher, ob entt-Versionsbits je so hohe IDs erzeugen; messen/prüfen). Außerdem soll `Python.h` laut CPython-Doku vor allen Standard-Headern inkludiert werden; hier stehen davor `<functional>`, `<cstdint>` und die Engine-Header (mögliche `_POSIX_C_SOURCE`-Redefinitionswarnungen), und `PY_SSIZE_T_CLEAN` wird erst nach den Engine-Includes definiert.
- **Code-Auszug**:
```cpp
long id; if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
... (uint32_t)id
```
- **Lösung**: Einheitlich `unsigned int id` mit `"I"` lesen und `PyLong_FromUnsignedLong` zurückgeben; `PY_SSIZE_T_CLEAN` + `Python.h` als erste Includes der Datei.
- **Erwarteter Effekt**: Konsistentes Plattformverhalten, weniger Warnungen.
- **Risiko / Verifikation**: `"I"` prüft keinen Überlauf (anders als `"l"`); negative IDs würden bisher als große uint32 durchgereicht. Mit Tests für Negativ-/Großwerte absichern.

### PYT-114 – `takePyError`/`pyErrorSite`: Zwei CPython-Versionspfade und manuelle Traceback-Iteration
- **ID**: PYT-114
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: PyScriptBackend.cpp:1080-1135, 1141-1174
- **Problem**: Die Traceback-Schleife holt pro Frame vier Attribute per String (`tb_frame`, `f_code`, `co_filename`, `tb_lineno`, `tb_next`) und verwaltet Referenzen manuell (`if (t != tb) Py_DECREF(t)`), was leicht fehleranfällig ist. Der Pfad läuft bei jeder Skriptausnahme; ein Skript, das in `on_update` jeden Frame wirft, erzeugt pro Frame diese Arbeit plus Stringbau (und vermutlich Log-Spam beim Aufrufer; Cross-Modul).
- **Code-Auszug**:
```cpp
PyObject* next = PyObject_GetAttrString(t, "tb_next");
if (t != tb) Py_DECREF(t);
```
- **Lösung**: Nur die Fehlerseite: `PyFrame_GetCode`/`PyCode_Addr2Line` bzw. `PyTraceBack`-Struktur (`tb_lineno`, `tb_frame`) mit `PyFrame_GetCode` verwenden (ab 3.9 stabil), oder in einem kleinen Python-Snippet (`traceback.extract_tb`) einmal pro Fehler auswerten. Nicht dringend, es handelt sich um einen Fehlerpfad.
- **Erwarteter Effekt**: Weniger Code, geringere Fehleranfälligkeit; Laufzeit nur im Fehlerfall.
- **Risiko / Verifikation**: Bestehende Tests für `<script>:<line>`-Format (`HE::parseScriptErrorLocation`) laufen lassen; SyntaxError-Sonderfall bleibt.

---

## Gelesen
Ja: `PyScriptBackend.h` (93 Zeilen) und `PyScriptBackend.cpp` (1728 Zeilen) vollständig gelesen; zusätzlich gezielte Greps in `EngineApi.cpp`, `TypeRegistry.h`, `InspectorPanel.cpp`.
