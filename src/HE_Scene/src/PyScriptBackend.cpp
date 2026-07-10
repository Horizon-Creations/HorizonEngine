#include "HorizonScene/PyScriptBackend.h"

#ifdef HE_HAVE_PYTHON

#include "HorizonScene/ScriptApi.h"
#include "HorizonScene/EngineApi.h"   // HE::api registry (registry-driven groups)
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <algorithm>
#include <limits>
#include <string>
#include <vector>

// The interpreter is a process singleton driven from the main thread. A single
// active backend feeds the module functions the current world/physics through
// these file-statics (set by the backend; the `horizon` C functions read them).
namespace {
HorizonWorld*   g_world   = nullptr;
PhysicsWorld*   g_physics = nullptr;
ContentManager* g_content = nullptr;

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
// setMaterialParam(entity, name, x [, y, z, w]) — 1..4 components; missing → 0.
PyObject* py_setMaterialParam(PyObject*, PyObject* args)
{
	long id; const char* name = nullptr; float x = 0, y = 0, z = 0, w = 0;
	if (!PyArg_ParseTuple(args, "lsf|fff", &id, &name, &x, &y, &z, &w)) return nullptr;
	const bool ok = ScriptApi::setMaterialParam(*g_world, g_content, (uint32_t)id, name, {x, y, z, w});
	if (ok) Py_RETURN_TRUE;
	Py_RETURN_FALSE;
}
// getMaterialParam(entity, name) -> (x, y, z, w)
PyObject* py_getMaterialParam(PyObject*, PyObject* args)
{
	long id; const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "ls", &id, &name)) return nullptr;
	const glm::vec4 v = ScriptApi::getMaterialParam(*g_world, g_content, (uint32_t)id, name);
	return Py_BuildValue("(ffff)", v.x, v.y, v.z, v.w);
}

// ── In-game UI ────────────────────────────────────────────────────────────────
PyObject* py_setUIText(PyObject*, PyObject* args)
{
	long id; const char* text = nullptr;
	if (!PyArg_ParseTuple(args, "ls", &id, &text)) return nullptr;
	ScriptApi::setUIText(*g_world, (uint32_t)id, text);
	Py_RETURN_NONE;
}
PyObject* py_getUIText(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	return PyUnicode_FromString(ScriptApi::getUIText(*g_world, (uint32_t)id).c_str());
}
PyObject* py_setUIColor(PyObject*, PyObject* args)
{
	long id; float r = 0, g = 0, b = 0, a = 1.0f;
	if (!PyArg_ParseTuple(args, "lfff|f", &id, &r, &g, &b, &a)) return nullptr;
	ScriptApi::setUIColor(*g_world, (uint32_t)id, {r, g, b, a});
	Py_RETURN_NONE;
}
PyObject* py_getUIColor(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	const glm::vec4 c = ScriptApi::getUIColor(*g_world, (uint32_t)id);
	return Py_BuildValue("(ffff)", c.r, c.g, c.b, c.a);
}
PyObject* py_setUIVisible(PyObject*, PyObject* args)
{
	long id; int vis = 1;
	if (!PyArg_ParseTuple(args, "lp", &id, &vis)) return nullptr;
	ScriptApi::setUIVisible(*g_world, (uint32_t)id, vis != 0);
	Py_RETURN_NONE;
}
PyObject* py_isUIVisible(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	if (ScriptApi::isUIVisible(*g_world, (uint32_t)id)) Py_RETURN_TRUE;
	Py_RETURN_FALSE;
}
PyObject* py_setUIPosition(PyObject*, PyObject* args)
{
	long id; float x = 0, y = 0;
	if (!PyArg_ParseTuple(args, "lff", &id, &x, &y)) return nullptr;
	ScriptApi::setUIPosition(*g_world, (uint32_t)id, {x, y});
	Py_RETURN_NONE;
}
PyObject* py_getUIPosition(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	const glm::vec2 v = ScriptApi::getUIPosition(*g_world, (uint32_t)id);
	return Py_BuildValue("(ff)", v.x, v.y);
}
PyObject* py_setUISize(PyObject*, PyObject* args)
{
	long id; float w = 0, h = 0;
	if (!PyArg_ParseTuple(args, "lff", &id, &w, &h)) return nullptr;
	ScriptApi::setUISize(*g_world, (uint32_t)id, {w, h});
	Py_RETURN_NONE;
}
PyObject* py_getUISize(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	const glm::vec2 v = ScriptApi::getUISize(*g_world, (uint32_t)id);
	return Py_BuildValue("(ff)", v.x, v.y);
}
PyObject* py_setUIMaterialParam(PyObject*, PyObject* args)
{
	long id; const char* name = nullptr; float x = 0, y = 0, z = 0, w = 0;
	if (!PyArg_ParseTuple(args, "lsf|fff", &id, &name, &x, &y, &z, &w)) return nullptr;
	const bool ok = ScriptApi::setUIMaterialParam(*g_world, g_content, (uint32_t)id, name, {x, y, z, w});
	if (ok) Py_RETURN_TRUE;
	Py_RETURN_FALSE;
}

// ── Live widgets + cursor ─────────────────────────────────────────────────────
PyObject* py_createWidget(PyObject*, PyObject* args)
{
	const char* path = nullptr;
	if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
	return PyLong_FromLong(ScriptApi::createWidget(*g_world, g_content, path));
}
PyObject* py_destroyWidget(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	ScriptApi::destroyWidget(*g_world, (int)id);
	Py_RETURN_NONE;
}
PyObject* py_showWidget(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	ScriptApi::showWidget(*g_world, (int)id);
	Py_RETURN_NONE;
}
PyObject* py_hideWidget(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	ScriptApi::hideWidget(*g_world, (int)id);
	Py_RETURN_NONE;
}
PyObject* py_setWidgetZOrder(PyObject*, PyObject* args)
{
	long id, z;
	if (!PyArg_ParseTuple(args, "ll", &id, &z)) return nullptr;
	ScriptApi::setWidgetZOrder(*g_world, (int)id, (int)z);
	Py_RETURN_NONE;
}
PyObject* py_isWidgetVisible(PyObject*, PyObject* args)
{
	long id;
	if (!PyArg_ParseTuple(args, "l", &id)) return nullptr;
	if (ScriptApi::isWidgetVisible(*g_world, (int)id)) Py_RETURN_TRUE;
	Py_RETURN_FALSE;
}
PyObject* py_callWidgetFunction(PyObject*, PyObject* args)
{
	long id; const char* name = nullptr;
	if (!PyArg_ParseTuple(args, "ls", &id, &name)) return nullptr;
	if (ScriptApi::callWidgetFunction(*g_world, (int)id, name)) Py_RETURN_TRUE;
	Py_RETURN_FALSE;
}
PyObject* py_showCursor(PyObject*, PyObject*)
{
	ScriptApi::setCursorVisible(true);
	Py_RETURN_NONE;
}
PyObject* py_hideCursor(PyObject*, PyObject*)
{
	ScriptApi::setCursorVisible(false);
	Py_RETURN_NONE;
}

// ── Registry-driven engine API (HE::api) ────────────────────────────────────
// A single generic dispatcher marshals HorizonCode Values by pin type, so a whole
// group of engine functions is exposed by iterating the registry — no per-function
// C shim. Args after the id are read per the entry's param types (a vec2 = 2
// numbers, a Color = 4 — the same spread as the hand-written bindings); results
// spread the same way (1 → scalar, more → tuple). A Python bootstrap (built from
// the registry) wraps each entry as horizon.<group>.<fn>. First group: the pure
// Math library. Gameplay groups keep their ergonomic shims until ScriptApi is
// inverted onto HE::api.
HorizonCode::Value pyReadValue(PyObject* args, Py_ssize_t& idx, HorizonCode::PinType t)
{
	using P = HorizonCode::PinType; using V = HorizonCode::Value;
	auto num = [&](){ PyObject* o = PyTuple_GetItem(args, idx++); return o ? (float)PyFloat_AsDouble(o) : 0.0f; };
	switch (t)
	{
	case P::Bool:   { PyObject* o = PyTuple_GetItem(args, idx++); return V::ofBool(o && PyObject_IsTrue(o)); }
	case P::Int:    { PyObject* o = PyTuple_GetItem(args, idx++); return V::ofInt(o ? (int)PyLong_AsLong(o) : 0); }
	case P::String: { PyObject* o = PyTuple_GetItem(args, idx++); const char* s = o ? PyUnicode_AsUTF8(o) : nullptr; return V::ofString(s ? s : ""); }
	case P::Vec2:   { float x = num(), y = num(); return V::ofVec2({ x, y }); }
	case P::Color:  { float r = num(), g = num(), b = num(), a = num(); return V::ofColor({ r, g, b, a }); }
	case P::Ref:    { PyObject* o = PyTuple_GetItem(args, idx++); return V::ofRef(o ? (uint32_t)PyLong_AsUnsignedLong(o) : 0u); }
	case P::Float:
	default:        return V::ofFloat(num());
	}
}

// Append a Value's Python representation(s) to `out` (a list), spreading vecs.
void pyAppendValue(PyObject* out, const HorizonCode::Value& v, HorizonCode::PinType t)
{
	using P = HorizonCode::PinType;
	auto add = [&](PyObject* o){ PyList_Append(out, o); Py_XDECREF(o); };
	switch (t)
	{
	case P::Bool:   add(PyBool_FromLong(v.b)); break;
	case P::Int:    add(PyLong_FromLong(v.i)); break;
	case P::String: add(PyUnicode_FromString(v.s.c_str())); break;
	case P::Vec2:   add(PyFloat_FromDouble(v.v2.x)); add(PyFloat_FromDouble(v.v2.y)); break;
	case P::Color:  add(PyFloat_FromDouble(v.col.x)); add(PyFloat_FromDouble(v.col.y));
	                add(PyFloat_FromDouble(v.col.z)); add(PyFloat_FromDouble(v.col.w)); break;
	case P::Ref:    add(PyLong_FromUnsignedLong(v.ref)); break;
	case P::Float:
	default:        add(PyFloat_FromDouble(v.f)); break;
	}
}

// _engineCall(id, *args) → scalar | tuple | None. The Python bootstrap wraps this.
PyObject* py_engineCall(PyObject*, PyObject* args)
{
	const Py_ssize_t n = PyTuple_Size(args);
	if (n < 1) { PyErr_SetString(PyExc_TypeError, "_engineCall(id, ...) needs an id"); return nullptr; }
	PyObject* idObj = PyTuple_GetItem(args, 0);
	const char* id = idObj ? PyUnicode_AsUTF8(idObj) : nullptr;
	const HE::api::ApiFn* fn = id ? HE::api::find(id) : nullptr;
	if (!fn) { PyErr_Format(PyExc_KeyError, "unknown engine function '%s'", id ? id : "?"); return nullptr; }

	HE::api::Ctx c{ g_world, g_physics, g_content };
	std::vector<HorizonCode::Value> in; in.reserve(fn->params.size());
	Py_ssize_t idx = 1;                                   // arg 0 is the id
	for (const auto& p : fn->params) in.push_back(pyReadValue(args, idx, p.type));
	if (PyErr_Occurred()) return nullptr;

	const std::vector<HorizonCode::Value> res = fn->invoke(c, in);
	PyObject* out = PyList_New(0);
	for (size_t i = 0; i < fn->results.size(); ++i)
		pyAppendValue(out, i < res.size() ? res[i] : HorizonCode::Value{}, fn->results[i].type);
	const Py_ssize_t outN = PyList_Size(out);
	if (outN == 0) { Py_DECREF(out); Py_RETURN_NONE; }
	if (outN == 1) { PyObject* only = PyList_GetItem(out, 0); Py_INCREF(only); Py_DECREF(out); return only; }
	PyObject* tup = PyList_AsTuple(out); Py_DECREF(out); return tup;
}

PyMethodDef kHorizonMethods[] = {
	{"log",         py_log,         METH_VARARGS, "log(message)"},
	{"_engineCall", py_engineCall,  METH_VARARGS, "_engineCall(id, *args) — dispatch through the HE::api registry"},
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
	{"setMaterialParam", py_setMaterialParam, METH_VARARGS, "setMaterialParam(entity, name, x[,y,z,w]) -> bool"},
	{"getMaterialParam", py_getMaterialParam, METH_VARARGS, "getMaterialParam(entity, name) -> (x,y,z,w)"},
	{"setUIText",     py_setUIText,     METH_VARARGS, "setUIText(entity, text)"},
	{"getUIText",     py_getUIText,     METH_VARARGS, "getUIText(entity) -> str"},
	{"setUIColor",    py_setUIColor,    METH_VARARGS, "setUIColor(entity, r, g, b[, a])"},
	{"getUIColor",    py_getUIColor,    METH_VARARGS, "getUIColor(entity) -> (r,g,b,a)"},
	{"setUIVisible",  py_setUIVisible,  METH_VARARGS, "setUIVisible(entity, visible)"},
	{"isUIVisible",   py_isUIVisible,   METH_VARARGS, "isUIVisible(entity) -> bool"},
	{"setUIPosition", py_setUIPosition, METH_VARARGS, "setUIPosition(entity, x, y)"},
	{"getUIPosition", py_getUIPosition, METH_VARARGS, "getUIPosition(entity) -> (x,y)"},
	{"setUISize",     py_setUISize,     METH_VARARGS, "setUISize(entity, w, h)"},
	{"getUISize",     py_getUISize,     METH_VARARGS, "getUISize(entity) -> (w,h)"},
	{"setUIMaterialParam", py_setUIMaterialParam, METH_VARARGS, "setUIMaterialParam(entity, name, x[,y,z,w]) -> bool"},
	{"createWidget",       py_createWidget,       METH_VARARGS, "createWidget(assetPath) -> widgetId"},
	{"destroyWidget",      py_destroyWidget,      METH_VARARGS, "destroyWidget(widgetId)"},
	{"showWidget",         py_showWidget,         METH_VARARGS, "showWidget(widgetId)"},
	{"hideWidget",         py_hideWidget,         METH_VARARGS, "hideWidget(widgetId)"},
	{"setWidgetZOrder",    py_setWidgetZOrder,    METH_VARARGS, "setWidgetZOrder(widgetId, z)"},
	{"isWidgetVisible",    py_isWidgetVisible,    METH_VARARGS, "isWidgetVisible(widgetId) -> bool"},
	{"callWidgetFunction", py_callWidgetFunction, METH_VARARGS, "callWidgetFunction(widgetId, name) -> bool"},
	{"showCursor",         py_showCursor,         METH_NOARGS,  "showCursor() — release the mouse for UI"},
	{"hideCursor",         py_hideCursor,         METH_NOARGS,  "hideCursor() — capture the mouse for look"},
	{nullptr, nullptr, 0, nullptr}
};
PyModuleDef kHorizonModule = {
	PyModuleDef_HEAD_INIT, "horizon", "HorizonEngine gameplay API", -1, kHorizonMethods,
	nullptr, nullptr, nullptr, nullptr
};
PyObject* PyInit_horizon() { return PyModule_Create(&kHorizonModule); }

// Build horizon.<group>.<fn> wrappers from the registry (each forwards to
// _engineCall). Run once, after the horizon module exists. Registry-driven:
// adding an entry to a registered group exposes it here for free.
void bootstrapEngineApiGroups()
{
	std::string src = "import horizon, types\n";
	std::string lastGroup;
	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		const std::string id = fn.id;
		const auto dot = id.find('.');
		if (dot == std::string::npos) continue;       // only namespaced ("math.clamp")
		const std::string group = id.substr(0, dot), name = id.substr(dot + 1);
		// Registry-driven groups exposed as horizon.<group>.<fn>. The flat
		// gameplay functions keep their ergonomic hand-written bindings until
		// ScriptApi is inverted onto HE::api. NB: a packed vec3 (Color) param
		// spreads as 4 numbers (x, y, z, _) on this path. Widening = add a name.
		static const char* kGroups[] = { "math", "random", "time", "input",
		                                 "string", "camera", "env", "entity", "audio",
	                                 "debug", "fs", "save", "scene" };
		bool exposed = false;
		for (const char* gname : kGroups) if (group == gname) { exposed = true; break; }
		if (!exposed) continue;
		if (group != lastGroup)
		{
			src += "if not isinstance(getattr(horizon, '" + group + "', None), types.SimpleNamespace):\n"
			       "    horizon." + group + " = types.SimpleNamespace()\n";
			lastGroup = group;
		}
		src += "horizon." + group + "." + name +
		       " = lambda *a, _id='" + id + "': horizon._engineCall(_id, *a)\n";
	}
	PyRun_SimpleString(src.c_str());
	PyErr_Clear();
}

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
	HorizonWorld*   world   = nullptr;
	PhysicsWorld*   physics = nullptr;
	ContentManager* content = nullptr;
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

	// Layer the registry-driven groups (horizon.math.*, …) onto the module once.
	static bool s_engineApiBootstrapped = false;
	if (!s_engineApiBootstrapped) { bootstrapEngineApiGroups(); s_engineApiBootstrapped = true; }
}

PyScriptBackend::~PyScriptBackend()
{
	if (m_impl)
	{
		// Dropping the last ref to an instance runs its Python finalizer (__del__)
		// synchronously here, and a finalizer may call horizon.setVelocity/raycast/
		// isGrounded. The owner often frees the PhysicsWorld *before* destroying this
		// backend (e.g. editor play-mode stop resets physics, then the script
		// context), leaving g_physics dangling. Null it first so those calls hit the
		// null-guard in ScriptApi instead of dereferencing freed memory. g_world
		// outlives the backend on every teardown path, so it stays valid here.
		g_physics = nullptr;
		g_content = nullptr; // same dangling concern as g_physics (freed before backend)
		for (auto& [id, inst] : m_impl->instances) Py_XDECREF(inst.obj);
		for (auto& [name, cls] : m_impl->classes)  Py_XDECREF(cls);
		Py_XDECREF(m_impl->behaviorBase);
	}
	if (g_world == (m_impl ? m_impl->world : nullptr)) { g_world = nullptr; g_physics = nullptr; g_content = nullptr; }
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

void PyScriptBackend::setContentManager(ContentManager* cm)
{
	m_impl->content = cm;
	g_content = cm;
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

bool PyScriptBackend::callOnUIEvent(InstanceId id, UIScriptEvent ev)
{
	const char* fn = ev == UIScriptEvent::Click      ? "on_click" :
	                 ev == UIScriptEvent::HoverEnter ? "on_hover_enter" : "on_hover_exit";
	PyObject* obj = m_impl->findInstance(id);
	if (!obj || !PyObject_HasAttrString(obj, fn)) return true;
	PyObject* r = PyObject_CallMethod(obj, fn, nullptr);
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
		else if (PyLong_Check(val))
		{
			// ScriptPropValue::i is int32. Clamp rather than let the C cast turn an
			// out-of-range positive (e.g. 0xFFFFFFFF) into a negative, and treat a
			// too-big-for-long-long value (10**40) as 0 without leaking the error.
			def.defaultVal.type = ScriptPropType::Int;
			long long lv = PyLong_AsLongLong(val);
			if (PyErr_Occurred()) { PyErr_Clear(); lv = 0; }
			lv = std::max<long long>(std::numeric_limits<int>::min(),
			         std::min<long long>(std::numeric_limits<int>::max(), lv));
			def.defaultVal.i = static_cast<int>(lv);
		}
		else if (PyFloat_Check(val))  { def.defaultVal.type = ScriptPropType::Float;  def.defaultVal.f = (float)PyFloat_AsDouble(val); }
		else if (PyUnicode_Check(val))
		{
			// A valid str can still fail strict-UTF-8 encoding (lone surrogate) and
			// return NULL; assigning NULL to std::string is a strlen(NULL) crash.
			const char* sv = PyUnicode_AsUTF8(val);
			if (!sv) { PyErr_Clear(); continue; }
			def.defaultVal.type = ScriptPropType::String; def.defaultVal.s = sv;
		}
		else continue;                                // skip methods/other types
		out.push_back(std::move(def));
	}
	Py_DECREF(items);
	PyErr_Clear();  // never leave a pending error for the next C-API call
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
void PyScriptBackend::setContentManager(ContentManager*) {}
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
bool PyScriptBackend::callOnUIEvent(InstanceId, UIScriptEvent) { return false; }
std::vector<ScriptPropDef> PyScriptBackend::getScriptProperties(const std::string&) const { return {}; }
void PyScriptBackend::injectProperties(InstanceId, const std::unordered_map<std::string, ScriptPropValue>&) {}
bool PyScriptBackend::hotReloadScript(const std::string&, const std::string&) { return false; }

#endif
