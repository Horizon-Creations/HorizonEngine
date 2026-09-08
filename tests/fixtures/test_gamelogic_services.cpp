// Test fixture: a native game-logic library shaped exactly like a real one.
//
// It links NOTHING and sees exactly one include directory, src/HE_Core/include —
// the same diet CppScaffold::cmakeLists puts a generated game project on. That is
// deliberate and it is half the test: if <HorizonGameServices.h> ever grows a
// dependency on glm, HE_Scene or any engine library, this file stops compiling
// long before a user's project does. HorizonWorld& arrives as an opaque
// forward-declared reference and is never touched, which is the other half —
// every engine call below goes through the injected C-ABI tables.
#include "test_gamelogic_probe.h"

HE_IMPLEMENT_ENGINE_SERVICES()

namespace {

class TestServicesLogic final : public ITestServicesProbe {
public:
	void onStart(HorizonWorld&) override
	{
		m_saveAtStart    = he::save::available();
		m_physicsAtStart = he::physics::available();
		m_inputAtStart   = he::input::available();
		m_contentAtStart = he::content::available();
	}

	void onUpdate(HorizonWorld&, float) override
	{
		++m_updates;
		m_physicsAtUpdate = he::physics::available();
	}

	void onStop(HorizonWorld&) override {}

	TestServiceSymbols symbols() const override
	{
		TestServiceSymbols s;
		s.saveAccessor    = reinterpret_cast<uintptr_t>(&he::detail::svc);
		s.physicsAccessor = reinterpret_cast<uintptr_t>(&he::detail::physSvc);
		s.inputAccessor   = reinterpret_cast<uintptr_t>(&he::detail::inputSvc);
		s.contentAccessor = reinterpret_cast<uintptr_t>(&he::detail::contentSvc);
		s.saveGlobal      = reinterpret_cast<uintptr_t>(&g_heSaveServices);
		s.saveTable       = reinterpret_cast<uintptr_t>(g_heSaveServices);
		return s;
	}

	bool servicesAvailableAtStart() const override { return m_saveAtStart; }
	bool physicsAvailableAtStart()  const override { return m_physicsAtStart; }
	bool inputAvailableAtStart()    const override { return m_inputAtStart; }
	bool contentAvailableAtStart()  const override { return m_contentAtStart; }
	int  updateCount()              const override { return m_updates; }
	bool physicsAvailableAtUpdate() const override { return m_physicsAtUpdate; }

	bool saveAvailable()    const override { return he::save::available(); }
	bool physicsAvailable() const override { return he::physics::available(); }
	bool inputAvailable()   const override { return he::input::available(); }
	bool contentAvailable() const override { return he::content::available(); }

	he::RaycastHit doRaycast(const he::Vec3& o, const he::Vec3& d, float maxDist) const override
	{ return he::physics::raycast(o, d, maxDist); }
	he::RaycastHit doSphereCast(const he::Vec3& o, const he::Vec3& d,
	                            float radius, float maxDist) const override
	{ return he::physics::sphereCast(o, d, radius, maxDist); }
	int doOverlapSphere(const he::Vec3& center, float radius,
	                    uint32_t* out, int cap) const override
	{
		const std::vector<uint32_t> hits = he::physics::overlapSphere(center, radius);
		const int n = (int)hits.size() < cap ? (int)hits.size() : cap;
		for (int i = 0; i < n && out; ++i) out[i] = hits[(size_t)i];
		return (int)hits.size();
	}
	bool doAddForce(uint32_t e, const he::Vec3& v)   const override { return he::physics::addForce(e, v); }
	bool doAddImpulse(uint32_t e, const he::Vec3& v) const override { return he::physics::addImpulse(e, v); }
	bool doAddTorque(uint32_t e, const he::Vec3& v)  const override { return he::physics::addTorque(e, v); }
	void doSetVelocity(uint32_t e, const he::Vec3& v) const override { he::physics::setVelocity(e, v); }
	he::Vec3 doGetVelocity(uint32_t e) const override { return he::physics::getVelocity(e); }
	bool doIsGrounded(uint32_t e)      const override { return he::physics::isGrounded(e); }
	bool doSetPosition(uint32_t e, const he::Vec3& p) const override
	{ return he::physics::setPosition(e, p); }
	bool doSetPositionAndReset(uint32_t e, const he::Vec3& p) const override
	{ return he::physics::setPositionAndReset(e, p); }
	bool doHasPhysics(uint32_t e) const override { return he::physics::hasPhysics(e); }
	void doSetGravity(const he::Vec3& g) const override { he::physics::setGravity(g); }
	he::Vec3 doGetGravity() const override { return he::physics::getGravity(); }

	bool  doKeyDown(const char* name)  const override { return he::input::keyDown(name); }
	bool  doMouseButton(int index)     const override { return he::input::mouseButton(index); }
	he::Vec2 doMousePosition()         const override { return he::input::mousePosition(); }
	he::Vec2 doMouseDelta()            const override { return he::input::mouseDelta(); }
	float doScrollDelta()              const override { return he::input::scrollDelta(); }
	bool  doGamepadConnected()         const override { return he::input::gamepadConnected(); }
	bool  doGamepadButton(const char* n) const override { return he::input::gamepadButton(n); }
	float doGamepadAxis(const char* n)   const override { return he::input::gamepadAxis(n); }
	int   doMode()                     const override { return (int)he::input::mode(); }
	void  doSetModeUIOnly()            const override { he::input::setModeUIOnly(); }
	void  doSetModeGameAndUI()         const override { he::input::setModeGameAndUI(); }
	void  doSetModeRaw(int mode) const override
	{ if (g_heInputServices) g_heInputServices->setMode(g_heInputServices->host, mode); }

	he::AssetId doLoadAsset(const char* path) const override
	{ return he::content::load(path ? path : ""); }
	bool doUnloadAsset(const he::AssetId& id)  const override { return he::content::unload(id); }
	bool doIsAssetLoaded(const he::AssetId& id) const override { return he::content::isLoaded(id); }
	int  doAssetTypeName(const he::AssetId& id, char* buf, int cap) const override
	{
		const std::string name = he::content::typeName(id);
		if (buf && cap > 0)
		{
			const int n = (int)name.size() < cap - 1 ? (int)name.size() : cap - 1;
			for (int i = 0; i < n; ++i) buf[i] = name[(size_t)i];
			buf[n] = '\0';
		}
		return (int)name.size();
	}
	int doAssetTypeNameRaw(const he::AssetId& id, char* buf, int cap) const override
	{
		if (!g_heContentServices) return 0;
		const HeAssetId cid{ id.hi, id.lo };
		return g_heContentServices->assetTypeName(g_heContentServices->host, cid, buf, cap);
	}

private:
	bool m_saveAtStart      = false;
	bool m_physicsAtStart   = false;
	bool m_inputAtStart     = false;
	bool m_contentAtStart   = false;
	bool m_physicsAtUpdate  = false;
	int  m_updates          = 0;
};

} // namespace

extern "C" HE_GAME_API IGameLogic* HE_CreateGameLogic()              { return new TestServicesLogic(); }
extern "C" HE_GAME_API void        HE_DestroyGameLogic(IGameLogic* p) { delete p; }
