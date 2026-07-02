#include "HorizonScene/PyScriptBackend.h"

#ifdef HE_HAVE_PYTHON

#include "HorizonScene/ScriptApi.h"
#define PY_SSIZE_T_CLEAN
#include <Python.h>

// The interpreter is a process singleton driven from the main thread. A single
// active backend feeds the module functions the current world/physics through
// these file-statics (set by the backend; the `horizon` C functions read them).
namespace {
HorizonWorld* g_world   = nullptr;
PhysicsWorld* g_physics = nullptr;

// ── horizon.* native functions (thin shims over ScriptApi) ──────────────────
PyObject* py_log(PyObject*, PyObject* args)
{
	const char* msg = nullptr;
	if (!PyArg_ParseTuple(args, "s", &msg)) return nullptr;
	ScriptApi::log(msg);
	Py_RETURN_NONE;
}
PyObject* py_getName(PyObject*, PyObject* args)
{
	long id; if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	return PyUnicode_FromString(ScriptApi::getName(*g_world, (uint32_t)id).c_str());
}
PyObject* py_getVec(PyObject* args, glm::vec3 (*fn)(HorizonWorld&, uint32_t))
{
	long id; if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	const glm::vec3 v = fn(*g_world, (uint32_t)id);
	return Py_BuildValue("(fff)", v.x, v.y, v.z);
}
PyObject* py_setVec(PyObject* args, void (*fn)(HorizonWorld&, uint32_t, const glm::vec3&))
{
	long id; float x, y, z;
	if (!PyArg_ParseTuple(args, "lfff", &id, &x, &y, &z)) return nullptr;
	fn(*g_world, (uint32_t)id, {x, y, z});
	Py_RETURN_NONE;
}
PyObject* py_getPosition(PyObject*, PyObject* a) { return py_getVec(a, ScriptApi::getPosition); }
PyObject* py_setPosition(PyObject*, PyObject* a) { return py_setVec(a, ScriptApi::setPosition); }
PyObject* py_getRotation(PyObject*, PyObject* a) { return py_getVec(a, ScriptApi::getRotation); }
PyObject* py_setRotation(PyObject*, PyObject* a) { return py_setVec(a, ScriptApi::setRotation); }
PyObject* py_getScale(PyObject*, PyObject* a)    { return py_getVec(a, ScriptApi::getScale); }
PyObject* py_setScale(PyObject*, PyObject* a)    { return py_setVec(a, ScriptApi::setScale); }

PyObject* py_spawn(PyObject*, PyObject* args)
{
	long parent; const char* name = "Entity";
	if (!PyArg_ParseTuple(args, "l|s", &parent, &name)) return nullptr;
	return PyLong_FromUnsignedLong(ScriptApi::spawn(*g_world, (uint32_t)parent, name));
}
PyObject* py_destroy(PyObject*, PyObject* args)
{
	long id; if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	ScriptApi::destroy(*g_world, (uint32_t)id);
	Py_RETURN_NONE;
}
PyObject* py_raycast(PyObject*, PyObject* args)
{
	float ox, oy, oz, dx, dy, dz, maxd = 1000.0f;
	if (!PyArg_ParseTuple(args, "ffffff|f", &ox, &oy, &oz, &dx, &dy, &dz, &maxd)) return nullptr;
	const auto hit = ScriptApi::raycast(g_physics, {ox, oy, oz}, {dx, dy, dz}, maxd);
	if (!hit.hit) Py_RETURN_NONE;
	return Py_BuildValue("{s:k,s:f,s:f,s:f,s:f,s:f,s:f,s:f}",
		"entity", (unsigned long)hit.entityId,
		"x", hit.point.x, "y", hit.point.y, "z", hit.point.z,
		"nx", hit.normal.x, "ny", hit.normal.y, "nz", hit.normal.z,
		"distance", hit.distance);
}
PyObject* py_setVelocity(PyObject*, PyObject* args)
{
	long id; float vx, vy, vz;
	if (!PyArg_ParseTuple(args, "lfff", &id, &vx, &vy, &vz)) return nullptr;
	ScriptApi::setVelocity(g_physics, (uint32_t)id, {vx, vy, vz});
	Py_RETURN_NONE;
}
PyObject* py_isGrounded(PyObject*, PyObject* args)
{
	long id; if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	if (ScriptApi::isGrounded(g_physics, (uint32_t)id)) Py_RETURN_TRUE;
	Py_RETURN_FALSE;
}

PyMethodDef kHorizonMethods[] = {
	{"log",         py_log,         METH_VARARGS, "log(message)"},
	{"getName",     py_getName,     METH_VARARGS, "getName(entity) -> str"},
	{"getPosition", py_getPosition, METH_VARARGS, "getPosition(entity) -> (x,y,z)"},
	{"setPosition", py_setPosition, METH_VARARGS, "setPosition(entity, x, y, z)"},
	{"getRotation", py_getRotation, METH_VARARGS, "getRotation(entity) -> (x,y,z)"},
	{"setRotation", py_setRotation, METH_VARARGS, "setRotation(entity, x, y, z)"},
	{"getScale",    py_getScale,    METH_VARARGS, "getScale(entity) -> (x,y,z)"},
	{"setScale",    py_setScale,    METH_VARARGS, "setScale(entity, x, y, z)"},
	{"spawn",       py_spawn,       METH_VARARGS, "spawn(parent, name) -> entity"},
	{"destroy",     py_destroy,     METH_VARARGS, "destroy(entity)"},
	{"raycast",     py_raycast,     METH_VARARGS, "raycast(ox,oy,oz,dx,dy,dz,maxDist) -> dict|None"},
	{"setVelocity", py_setVelocity, METH_VARARGS, "setVelocity(entity, vx, vy, vz)"},
	{"isGrounded",  py_isGrounded,  METH_VARARGS, "isGrounded(entity) -> bool"},
	{nullptr, nullptr, 0, nullptr}
};
PyModuleDef kHorizonModule = {
	PyModuleDef_HEAD_INIT, "horizon", "HorizonEngine gameplay API", -1, kHorizonMethods,
	nullptr, nullptr, nullptr, nullptr
};
PyObject* PyInit_horizon() { return PyModule_Create(&kHorizonModule); }

bool g_pyInited = false;

// Fetch the current Python error into a string and clear it.
std::string takePyError()
{
	if (!PyErr_Occurred()) return {};
	std::string out;
#if PY_VERSION_HEX >= 0x030C0000
	if (PyObject* exc = PyErr_GetRaisedException())   // 3.12+: single exception object
	{
		if (PyObject* s = PyObject_Str(exc))
		{
			if (const char* c = PyUnicode_AsUTF8(s)) out = c;
			Py_DECREF(s);
		}
		Py_DECREF(exc);
	}
#else
	PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
	PyErr_Fetch(&type, &value, &tb);
	PyErr_NormalizeException(&type, &value, &tb);
	if (value)
	{
		if (PyObject* s = PyObject_Str(value))
		{
			if (const char* c = PyUnicode_AsUTF8(s)) out = c;
			Py_DECREF(s);
		}
	}
	Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(tb);
#endif
	if (out.empty()) out = "python error";
	return out;
}
} // namespace

// ── Impl ────────────────────────────────────────────────────────────────────
struct PyScriptBackend::Impl {
	HorizonWorld* world   = nullptr;
	PhysicsWorld* physics = nullptr;
	PyObject*     behaviorBase = nullptr;                 // horizon.Behavior
	std::unordered_map<std::string, PyObject*> classes;  // script name → class object
	struct Inst { PyObject* obj = nullptr; std::string script; };
	std::unordered_map<InstanceId, Inst> instances;
	InstanceId nextId = 1;

	PyObject* findInstance(InstanceId id)
	{
		auto it = instances.find(id);
		return it == instances.end() ? nullptr : it->second.obj;
	}
};

PyScriptBackend::PyScriptBackend(HorizonWorld& world)
	: m_impl(std::make_unique<Impl>())
{
	m_impl->world = &world;
	g_world = &world;

	if (!g_pyInited)
	{
		PyImport_AppendInittab("horizon", &PyInit_horizon);
		Py_Initialize();
		g_pyInited = true;
	}

	// Define horizon.Behavior (the required base class) once.
	PyObject* mod = PyImport_ImportModule("horizon");
	if (mod)
	{
		if (!PyObject_HasAttrString(mod, "Behavior"))
		{
			PyObject* dict = PyModule_GetDict(mod);
			Py_XDECREF(PyRun_String("class Behavior:\n    entity_id = 0\n",
			                        Py_file_input, dict, dict));
			PyErr_Clear();
		}
		m_impl->behaviorBase = PyObject_GetAttrString(mod, "Behavior"); // new ref
		Py_DECREF(mod);
	}
}

PyScriptBackend::~PyScriptBackend()
{
	if (m_impl)
	{
		for (auto& [id, inst] : m_impl->instances) Py_XDECREF(inst.obj);
		for (auto& [name, cls] : m_impl->classes)  Py_XDECREF(cls);
		Py_XDECREF(m_impl->behaviorBase);
	}
	if (g_world == (m_impl ? m_impl->world : nullptr)) { g_world = nullptr; g_physics = nullptr; }
	// Py_Finalize is intentionally NOT called: the interpreter is a process
	// singleton and may be reused by another backend instance.
}

bool PyScriptBackend::available()
{
	return true; // compiled in
}

void PyScriptBackend::setPhysicsWorld(PhysicsWorld* pw)
{
	m_impl->physics = pw;
	g_physics = pw;
}

// Exec `source`, find the single horizon.Behavior subclass, return it (new ref).
static PyObject* extractBehaviorClass(PyObject* base, const std::string& source,
                                      std::string& err)
{
	PyObject* globals = PyDict_New();
	PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
	PyObject* result = PyRun_String(source.c_str(), Py_file_input, globals, globals);
	if (!result) { err = takePyError(); Py_DECREF(globals); return nullptr; }
	Py_DECREF(result);

	PyObject *key, *val, *found = nullptr; Py_ssize_t pos = 0; int count = 0;
	while (PyDict_Next(globals, &pos, &key, &val))
	{
		if (PyType_Check(val) && val != base &&
		    PyObject_IsSubclass(val, base) == 1)
		{
			found = val; ++count;
		}
	}
	if (count != 1)
	{
		err = count == 0 ? "no horizon.Behavior subclass found in script"
		                 : "more than one horizon.Behavior subclass in script";
		Py_DECREF(globals);
		return nullptr;
	}
	Py_INCREF(found);
	Py_DECREF(globals);
	return found;
}

bool PyScriptBackend::loadScript(const std::string& name, const std::string& source)
{
	if (!m_impl->behaviorBase) { m_lastError = "python backend not initialized"; return false; }
	std::string err;
	PyObject* cls = extractBehaviorClass(m_impl->behaviorBase, source, err);
	if (!cls) { m_lastError = err; return false; }

	if (auto it = m_impl->classes.find(name); it != m_impl->classes.end())
		Py_DECREF(it->second);
	m_impl->classes[name] = cls;
	return true;
}

void PyScriptBackend::unloadScript(const std::string& name)
{
	for (auto it = m_impl->instances.begin(); it != m_impl->instances.end();)
	{
		if (it->second.script == name) { Py_XDECREF(it->second.obj); it = m_impl->instances.erase(it); }
		else ++it;
	}
	if (auto it = m_impl->classes.find(name); it != m_impl->classes.end())
	{
		Py_DECREF(it->second);
		m_impl->classes.erase(it);
	}
}

bool   PyScriptBackend::isScriptLoaded(const std::string& name) const { return m_impl->classes.count(name) > 0; }
size_t PyScriptBackend::loadedScriptCount() const { return m_impl->classes.size(); }
size_t PyScriptBackend::instanceCount() const { return m_impl->instances.size(); }

IScriptBackend::InstanceId PyScriptBackend::createInstance(const std::string& scriptName, uint32_t entityId)
{
	auto it = m_impl->classes.find(scriptName);
	if (it == m_impl->classes.end()) { m_lastError = "script '" + scriptName + "' not loaded"; return kInvalidInstance; }

	PyObject* obj = PyObject_CallNoArgs(it->second);
	if (!obj) { m_lastError = takePyError(); return kInvalidInstance; }
	PyObject* eid = PyLong_FromUnsignedLong(entityId);
	PyObject_SetAttrString(obj, "entity_id", eid);
	Py_DECREF(eid);

	const InstanceId id = m_impl->nextId++;
	m_impl->instances[id] = { obj, scriptName };
	return id;
}

void PyScriptBackend::destroyInstance(InstanceId id)
{
	auto it = m_impl->instances.find(id);
	if (it == m_impl->instances.end()) return;
	Py_XDECREF(it->second.obj);
	m_impl->instances.erase(it);
}

// Call an optional method; missing method = success no-op, exception = false+err.
bool PyScriptBackend::callOnStart(InstanceId id)
{
	PyObject* obj = m_impl->findInstance(id);
	if (!obj || !PyObject_HasAttrString(obj, "on_start")) return true;
	PyObject* r = PyObject_CallMethod(obj, "on_start", nullptr);
	if (!r) { m_lastError = takePyError(); return false; }
	Py_DECREF(r); return true;
}

bool PyScriptBackend::callOnUpdate(InstanceId id, float dt)
{
	PyObject* obj = m_impl->findInstance(id);
	if (!obj || !PyObject_HasAttrString(obj, "on_update")) return true;
	PyObject* r = PyObject_CallMethod(obj, "on_update", "f", dt);
	if (!r) { m_lastError = takePyError(); return false; }
	Py_DECREF(r); return true;
}

bool PyScriptBackend::callOnCollisionEnter(InstanceId id, uint32_t other)
{
	PyObject* obj = m_impl->findInstance(id);
	if (!obj || !PyObject_HasAttrString(obj, "on_collision_enter")) return true;
	PyObject* r = PyObject_CallMethod(obj, "on_collision_enter", "k", (unsigned long)other);
	if (!r) { m_lastError = takePyError(); return false; }
	Py_DECREF(r); return true;
}

bool PyScriptBackend::callOnCollisionExit(InstanceId id, uint32_t other)
{
	PyObject* obj = m_impl->findInstance(id);
	if (!obj || !PyObject_HasAttrString(obj, "on_collision_exit")) return true;
	PyObject* r = PyObject_CallMethod(obj, "on_collision_exit", "k", (unsigned long)other);
	if (!r) { m_lastError = takePyError(); return false; }
	Py_DECREF(r); return true;
}

std::vector<ScriptPropDef> PyScriptBackend::getScriptProperties(const std::string& name) const
{
	std::vector<ScriptPropDef> out;
	auto it = m_impl->classes.find(name);
	if (it == m_impl->classes.end()) return out;

	PyObject* dict = PyObject_GetAttrString(it->second, "__dict__");
	if (!dict) { PyErr_Clear(); return out; }
	PyObject* items = PyMapping_Items(dict); // list of (key, value)
	Py_DECREF(dict);
	if (!items) { PyErr_Clear(); return out; }

	const Py_ssize_t n = PyList_Size(items);
	for (Py_ssize_t i = 0; i < n; ++i)
	{
		PyObject* pair = PyList_GetItem(items, i); // borrowed
		PyObject* key  = PyTuple_GetItem(pair, 0);
		PyObject* val  = PyTuple_GetItem(pair, 1);
		const char* kname = PyUnicode_AsUTF8(key);
		if (!kname || kname[0] == '_') continue;     // skip dunders/privates

		ScriptPropDef def; def.name = kname;
		if (PyBool_Check(val))        { def.defaultVal.type = ScriptPropType::Bool;   def.defaultVal.b = (val == Py_True); }
		else if (PyLong_Check(val))   { def.defaultVal.type = ScriptPropType::Int;    def.defaultVal.i = (int)PyLong_AsLong(val); }
		else if (PyFloat_Check(val))  { def.defaultVal.type = ScriptPropType::Float;  def.defaultVal.f = (float)PyFloat_AsDouble(val); }
		else if (PyUnicode_Check(val)){ def.defaultVal.type = ScriptPropType::String; def.defaultVal.s = PyUnicode_AsUTF8(val); }
		else continue;                                // skip methods/other types
		out.push_back(std::move(def));
	}
	Py_DECREF(items);
	return out;
}

void PyScriptBackend::injectProperties(InstanceId id,
                                       const std::unordered_map<std::string, ScriptPropValue>& props)
{
	PyObject* obj = m_impl->findInstance(id);
	if (!obj) return;
	for (const auto& [key, v] : props)
	{
		PyObject* pv = nullptr;
		switch (v.type)
		{
		case ScriptPropType::Float:  pv = PyFloat_FromDouble(v.f); break;
		case ScriptPropType::Int:    pv = PyLong_FromLong(v.i); break;
		case ScriptPropType::Bool:   pv = PyBool_FromLong(v.b ? 1 : 0); break;
		case ScriptPropType::String: pv = PyUnicode_FromString(v.s.c_str()); break;
		}
		if (pv) { PyObject_SetAttrString(obj, key.c_str(), pv); Py_DECREF(pv); }
	}
	PyErr_Clear();
}

bool PyScriptBackend::hotReloadScript(const std::string& name, const std::string& source)
{
	if (!m_impl->behaviorBase) return false;
	std::string err;
	PyObject* newCls = extractBehaviorClass(m_impl->behaviorBase, source, err);
	if (!newCls) { m_lastError = err; return false; }   // keep old state on failure

	if (auto it = m_impl->classes.find(name); it != m_impl->classes.end())
		Py_DECREF(it->second);
	m_impl->classes[name] = newCls;

	// Rebind live instances to the new class — data in __dict__ is preserved.
	for (auto& [id, inst] : m_impl->instances)
		if (inst.script == name)
		{
			Py_INCREF(newCls);
			if (PyObject_SetAttrString(inst.obj, "__class__", newCls) != 0) PyErr_Clear();
			Py_DECREF(newCls);
		}
	return true;
}

#else // !HE_HAVE_PYTHON — no interpreter compiled in; safe no-op backend

struct PyScriptBackend::Impl {};
PyScriptBackend::PyScriptBackend(HorizonWorld&) : m_impl(nullptr)
{ m_lastError = "engine built without Python support"; }
PyScriptBackend::~PyScriptBackend() = default;
bool PyScriptBackend::available() { return false; }
void PyScriptBackend::setPhysicsWorld(PhysicsWorld*) {}
bool PyScriptBackend::loadScript(const std::string&, const std::string&) { return false; }
void PyScriptBackend::unloadScript(const std::string&) {}
bool PyScriptBackend::isScriptLoaded(const std::string&) const { return false; }
size_t PyScriptBackend::loadedScriptCount() const { return 0; }
size_t PyScriptBackend::instanceCount() const { return 0; }
IScriptBackend::InstanceId PyScriptBackend::createInstance(const std::string&, uint32_t) { return kInvalidInstance; }
void PyScriptBackend::destroyInstance(InstanceId) {}
bool PyScriptBackend::callOnStart(InstanceId) { return false; }
bool PyScriptBackend::callOnUpdate(InstanceId, float) { return false; }
bool PyScriptBackend::callOnCollisionEnter(InstanceId, uint32_t) { return false; }
bool PyScriptBackend::callOnCollisionExit(InstanceId, uint32_t) { return false; }
std::vector<ScriptPropDef> PyScriptBackend::getScriptProperties(const std::string&) const { return {}; }
void PyScriptBackend::injectProperties(InstanceId, const std::unordered_map<std::string, ScriptPropValue>&) {}
bool PyScriptBackend::hotReloadScript(const std::string&, const std::string&) { return false; }

#endif
