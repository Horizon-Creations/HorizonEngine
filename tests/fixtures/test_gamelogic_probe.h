// Shared between the test_gamelogic_services fixture library and the test that
// loads it. Every method is a single call through one of the he::* wrappers,
// made INSIDE the loaded library — which is the whole point: the test asserts on
// values that crossed the C-ABI boundary in both directions.
//
// Why an interface and not exported probe functions: GameLogicLoader dlopens a
// uniquely named .hot-NNNN COPY of the library, so a dlopen of the original path
// from the test would give a second image with its own (never injected) globals.
// The only handle onto the image that actually received the tables is the
// IGameLogic* the loader hands out, so the probes hang off that. Single
// inheritance, no data members — the static_cast in the test is a plain
// zero-offset downcast.
#pragma once
#include <IGameLogic.h>
#include <HorizonGameServices.h>

struct ITestServicesProbe : IGameLogic
{
    // What the module saw at the two lifecycle points that matter: the tables
    // have to be live in onStart (a game loads its save there) and stay live
    // across onUpdate.
    virtual bool servicesAvailableAtStart() const = 0;
    virtual bool physicsAvailableAtStart()  const = 0;
    virtual bool inputAvailableAtStart()    const = 0;
    virtual int  updateCount()              const = 0;
    virtual bool physicsAvailableAtUpdate() const = 0;

    virtual bool saveAvailable()    const = 0;
    virtual bool physicsAvailable() const = 0;
    virtual bool inputAvailable()   const = 0;

    // ── Physics ─────────────────────────────────────────────────────────────
    virtual he::RaycastHit doRaycast(const he::Vec3& origin, const he::Vec3& dir,
                                     float maxDist) const = 0;
    virtual he::RaycastHit doSphereCast(const he::Vec3& origin, const he::Vec3& dir,
                                        float radius, float maxDist) const = 0;
    // Writes up to `cap` ids, returns how many the wrapper actually produced.
    virtual int  doOverlapSphere(const he::Vec3& center, float radius,
                                 uint32_t* out, int cap) const = 0;
    virtual bool doAddForce(uint32_t entity, const he::Vec3& v)   const = 0;
    virtual bool doAddImpulse(uint32_t entity, const he::Vec3& v) const = 0;
    virtual bool doAddTorque(uint32_t entity, const he::Vec3& v)  const = 0;
    virtual void doSetVelocity(uint32_t entity, const he::Vec3& v) const = 0;
    virtual he::Vec3 doGetVelocity(uint32_t entity) const = 0;
    virtual bool doIsGrounded(uint32_t entity)      const = 0;
    virtual bool doSetPosition(uint32_t entity, const he::Vec3& localPos) const = 0;
    virtual bool doSetPositionAndReset(uint32_t entity, const he::Vec3& localPos) const = 0;
    virtual bool doHasPhysics(uint32_t entity) const = 0;
    virtual void doSetGravity(const he::Vec3& g) const = 0;
    virtual he::Vec3 doGetGravity() const = 0;

    // ── Input ───────────────────────────────────────────────────────────────
    virtual bool  doKeyDown(const char* name)  const = 0;
    virtual bool  doMouseButton(int index)     const = 0;
    virtual he::Vec2 doMousePosition()         const = 0;
    virtual he::Vec2 doMouseDelta()            const = 0;
    virtual float doScrollDelta()              const = 0;
    virtual bool  doGamepadConnected()         const = 0;
    virtual bool  doGamepadButton(const char* name) const = 0;
    virtual float doGamepadAxis(const char* name)   const = 0;
    virtual int   doMode()                     const = 0;
    virtual void  doSetModeUIOnly()            const = 0;
    virtual void  doSetModeGameAndUI()         const = 0;
    // Straight at the table, past the three named setters — the only way to
    // reach the out-of-range guard the wrappers cannot produce.
    virtual void  doSetModeRaw(int mode)       const = 0;
};
