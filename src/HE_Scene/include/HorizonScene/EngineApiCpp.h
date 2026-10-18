#pragma once
#include "HorizonScene/EngineApi.h"
#include <string>
#include <vector>

// ── Spelling HE::api calls as C++ source ─────────────────────────────────────
// The generator side of HorizonEngine/HorizonEngineApi.h: given a registry row,
// how generated code writes the direct call. Everything is read off the row
// (cppCall, cppForm, params, selfDefault), so a row added to the registry is
// callable from generated C++ without touching this file.
//
// Used by `hc_codegen --api-direct`, which writes the parity source
// test_engine_api_direct.cpp compiles (every non-Thunk row called directly and
// compared with ApiFn::invoke), and meant for the HorizonCode emitter.
namespace HE::api::cpp {

// "Float", "Vec3", … — the PinType enumerator, for `HorizonCode::PinType::<name>`.
const char* pinTypeName(PinType t);

// The C++ type a value of this pin lives in (HE::api::pin::type, spelled out):
// "float", "glm::vec3", "std::vector<int>".
std::string valueType(PinType t, bool isArray);

// The direct call of `fn`: `cppCall(ctx, args…)` for CppForm::Ctx, `cppCall(args…)`
// for Free. `args` holds one C++ expression per param, already of the pin's
// C++ type. A selfDefault param goes through entityOrSelf / widgetOrSelf, which
// is the substitution the interpreter's thunk makes. Empty for a Thunk row
// (generated code keeps hc::callApi there) and for a wrong argument count.
std::string callExpr(const ApiFn& fn, const std::string& ctx,
                     const std::vector<std::string>& args);

// The C++ type of an engine event hook's argument, as CompiledInstance declares
// it: the glm types and strings by const reference, the rest by value.
const char* hookArgType(PinType arg);
// A CompiledInstance hook's parameter list for an engine event, exactly as the
// hook declares it ("int elem, const std::string& arg"). `argUsed` = false
// leaves the argument unnamed.
std::string hookParams(const HorizonCode::EngineEventDesc& e, bool argUsed);

// The whole parity source: one direct-call thunk per non-Thunk row (args unboxed
// by pin type, the call, the answer boxed back by pin type), the table of them
// (he_api_direct::rows()), and a class overriding every engine event's hook.
// Included by nothing but the test; regenerated on every he_tests build.
std::string directCallsSource();

} // namespace HE::api::cpp
