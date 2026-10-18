#pragma once
// The table the generated engine_api_direct_gen.cpp (hc_codegen --api-direct)
// defines: one direct HE::api call per registry row that has one, with the
// row's Values unboxed into the pin types going in and boxed back coming out —
// the same interface as ApiFn::invoke, so test_engine_api_direct.cpp can run
// both on the same arguments and compare.
#include <HorizonScene/EngineApi.h>
#include <cstddef>
#include <vector>

namespace he_api_direct {

using Value = HorizonCode::Value;
using VV    = std::vector<Value>;

struct Row
{
    const char* id;
    VV (*call)(HE::api::Ctx& ctx, const VV& args);
};

// Registry order, every row whose cppForm is not Thunk.
const std::vector<Row>& rows();
// How many engine events the generated hook overrides covered.
size_t engineEventHookCount();

} // namespace he_api_direct
