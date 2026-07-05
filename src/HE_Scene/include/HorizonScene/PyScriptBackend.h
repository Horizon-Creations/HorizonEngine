#pragma once
#include <Scripting/IScriptBackend.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;

// Python (CPython) gameplay-script backend. Scripts define a subclass of
// horizon.Behavior; one instance per entity receives on_start/on_update/
// on_collision_enter/on_collision_exit. Exported properties are typed class
// attributes. The `horizon` module delegates to the shared ScriptApi, so the
// exposed API is identical to Lua's.
//
// The interpreter is a process-global singleton (Py_Initialize is process-wide);
// this backend drives it and must be used from the main thread (GIL). When the
// engine is built without CPython (HE_HAVE_PYTHON off), all methods are safe
// no-ops returning failure, so callers need no #ifdefs.
class HE_API PyScriptBackend final : public IScriptBackend
{
public:
	explicit PyScriptBackend(HorizonWorld& world);
	~PyScriptBackend() override;

	PyScriptBackend(const PyScriptBackend&)            = delete;
	PyScriptBackend& operator=(const PyScriptBackend&) = delete;

	// True when the engine was built with CPython and the interpreter is ready.
	static bool available();

	void setPhysicsWorld(PhysicsWorld* pw);
	void setContentManager(ContentManager* cm);

	bool   loadScript(const std::string& name, const std::string& source) override;
	void   unloadScript(const std::string& name) override;
	bool   isScriptLoaded(const std::string& name) const override;
	size_t loadedScriptCount() const override;
	size_t instanceCount() const override;

	InstanceId createInstance(const std::string& scriptName, uint32_t entityId) override;
	void       destroyInstance(InstanceId id) override;

	bool callOnStart(InstanceId id) override;
	bool callOnUpdate(InstanceId id, float dt) override;
	bool callOnCollisionEnter(InstanceId id, uint32_t otherEntityId) override;
	bool callOnCollisionExit(InstanceId id, uint32_t otherEntityId) override;

	std::vector<ScriptPropDef> getScriptProperties(const std::string& name) const override;
	void injectProperties(InstanceId id,
	                      const std::unordered_map<std::string, ScriptPropValue>& props) override;
	bool hotReloadScript(const std::string& name, const std::string& source) override;

	const std::string& lastError() const override { return m_lastError; }

private:
	struct Impl;                 // pImpl hides <Python.h> from the header
	std::unique_ptr<Impl> m_impl;
	std::string           m_lastError;
};
