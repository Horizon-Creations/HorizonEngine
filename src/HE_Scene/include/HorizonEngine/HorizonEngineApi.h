#pragma once
#include <HorizonScene/EngineApi.h>             // HE::api: every engine function a node calls
#include <HorizonCode/HorizonCodeCompiled.h>    // the engine events: CompiledInstance's hooks
#include <HorizonCode/HorizonCodeGenSupport.h>  // hc::Transform / hc::Vec4, the pin shapes
#include <HorizonCode/HorizonCodeRuntime.h>     // Runtime::pushCreator
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// ── The engine API for generated HorizonCode C++ ─────────────────────────────
// One include for a class the HorizonCode → C++ codegen emits (and for any
// hand-written game code that wants the same surface). It adds no second list
// of functions: everything a node calls is a free function in HE::api, named by
// its registry row (ApiFn::cppCall, e.g. HE::api::animator::setParam for the
// "Set Param" node), and the row's `cppForm` says whether it takes the Ctx.
// What this header adds is the glue a direct call needs and the registry's
// thunk used to supply:
//
//   • Scope — the Ctx the call runs against, for one compiled instance;
//   • entityOrSelf / widgetOrSelf (EngineApi.h) — "left at 0 means me";
//   • pin:: — a pin type's C++ shape, and the conversion of a function's
//     answer into it (an Entity is an Int pin, an RGB colour a Color with
//     alpha 1), which is what `invoke` did for the interpreter.
//
// Events need nothing here: the engine's own events are CompiledInstance's
// virtual hooks (onTick(float), onClicked(int elem), …), one per entry of
// HorizonCode::engineEvents(), whose `hook` names the method.
//
// The same answer as the interpreter is the contract, and a test holds it to
// it: test_engine_api_direct.cpp calls every row directly, through these
// helpers, and compares with ApiFn::invoke.
//
// NB for generated code: do not `using namespace HE::api;`. It has a namespace
// `hc` of its own (HE::api::hc, the HorizonCode rows), and the directive makes
// every unqualified `hc::` in the generated body ambiguous with ::hc.
namespace HE::api {

// ── Scope: the Ctx of one compiled instance ──────────────────────────────────
// Open one where a generated body starts calling the engine and pass it where a
// function takes `Ctx&`:
//
//     HE::api::Scope api(m_ctx);
//     HE::api::animator::setParam(api, HE::api::entity::self(api), "speed", 1.5f);
//
// It asks the instance's Context for the Ctx the host would have built for an
// EngineCall node of this instance (Context::apiCtx — the same builder, so the
// same world, physics, content, caller and app services), and for as long as it
// lives, names the instance as the creator of whatever it spawns (the
// interpreter does that around each callApi). Without a host binding the Ctx
// stays empty and every call is the null-Ctx no-op an unbound callApi is.
//
// The Ctx is taken when the Scope opens. The interpreter takes it per call; the
// two differ only when the host's world changes between two calls of one body,
// which scene loads do not do (they switch at a frame boundary).
class Scope
{
public:
    explicit Scope(const HorizonCode::Context& hc)
    {
        if (hc.apiCtx) hc.apiCtx(m_ctx);
        if (m_ctx.runtime && m_ctx.self)
        {
            m_ctx.runtime->pushCreator(m_ctx.self);
            m_pushed = true;
        }
    }
    // A Ctx built by the caller (a tool, a test, hand-written game code).
    explicit Scope(const Ctx& c) : m_ctx(c)
    {
        if (m_ctx.runtime && m_ctx.self)
        {
            m_ctx.runtime->pushCreator(m_ctx.self);
            m_pushed = true;
        }
    }
    ~Scope()
    {
        if (m_pushed) m_ctx.runtime->popCreator();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    Ctx& ctx() { return m_ctx; }
    operator Ctx&() { return m_ctx; }

private:
    Ctx  m_ctx;
    bool m_pushed = false;
};

// ── pin: the C++ shape of a pin type ─────────────────────────────────────────
// The types generated code keeps its values in (the same as hc::toValue/raw
// take), so a direct call's arguments are just the variables it already has.
namespace pin {

template <PinType P> struct Of;
template <> struct Of<PinType::Float>     { using type = float; };
template <> struct Of<PinType::Double>    { using type = double; };
template <> struct Of<PinType::Bool>      { using type = bool; };
template <> struct Of<PinType::Int>       { using type = int; };
template <> struct Of<PinType::Enum>      { using type = int; };
template <> struct Of<PinType::String>    { using type = std::string; };
template <> struct Of<PinType::Vec2>      { using type = glm::vec2; };
template <> struct Of<PinType::Vec3>      { using type = glm::vec3; };
template <> struct Of<PinType::Vec4>      { using type = ::hc::Vec4; };
template <> struct Of<PinType::Color>     { using type = glm::vec4; };
template <> struct Of<PinType::Ref>       { using type = uint32_t; };
template <> struct Of<PinType::Transform> { using type = ::hc::Transform; };
template <> struct Of<PinType::Struct>    { using type = Value; };
template <PinType P> using type = typename Of<P>::type;

// A function's answer in the result pin's shape. Most are already that type;
// the conversions are the ones the registry's thunks make: an Entity (uint32_t)
// is an Int pin, a handle may come back as int for a Ref pin, and an RGB colour
// (glm::vec3) on a Color pin is opaque.
template <PinType P, typename X>
type<P> toPin(const X& x)
{
    if constexpr (P == PinType::Color && std::is_same_v<X, glm::vec3>)
        return glm::vec4(x, 1.0f);
    else if constexpr (P == PinType::Vec4)
        return ::hc::Vec4(glm::vec4(x));
    else if constexpr (std::is_arithmetic_v<X> && std::is_arithmetic_v<type<P>>)
        return static_cast<type<P>>(x);
    else
        return type<P>(x);
}

// The same for an array result: element by element.
template <PinType P, typename X>
std::vector<type<P>> toPinArray(const std::vector<X>& xs)
{
    std::vector<type<P>> out;
    out.reserve(xs.size());
    for (const X& x : xs) out.push_back(toPin<P>(x));
    return out;
}

} // namespace pin
} // namespace HE::api
