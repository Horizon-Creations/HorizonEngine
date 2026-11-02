// Include hygiene for the header generated HorizonCode C++ includes (Thema 211
// step 6). It must compile on its own: it is the FIRST include of this file,
// with nothing before it that could supply a declaration it forgot.
#include <HorizonEngine/HorizonEngineApi.h>

#include "doctest.h"
#include <type_traits>

// The header must not open HE::api into the global scope: generated code
// spells ::hc helpers as plain `hc::`, and HE::api has an `hc` of its own.
static_assert(sizeof(hc::Vec4) == sizeof(glm::vec4), "::hc must stay unambiguous");

// The pin shapes are the types generated code already keeps its values in.
static_assert(std::is_same_v<HE::api::pin::type<HorizonCode::PinType::Float>, float>);
static_assert(std::is_same_v<HE::api::pin::type<HorizonCode::PinType::Int>, int>);
static_assert(std::is_same_v<HE::api::pin::type<HorizonCode::PinType::Vec3>, glm::vec3>);
static_assert(std::is_same_v<HE::api::pin::type<HorizonCode::PinType::Vec4>, hc::Vec4>);
static_assert(std::is_same_v<HE::api::pin::type<HorizonCode::PinType::Color>, glm::vec4>);
static_assert(std::is_same_v<HE::api::pin::type<HorizonCode::PinType::Ref>, uint32_t>);

TEST_CASE("HorizonEngineApi.h: pin conversions are the registry thunks' conversions")
{
    using HorizonCode::PinType;
    // An RGB colour on a Color pin is opaque (EngineApi.cpp's rgb()).
    const glm::vec4 c = HE::api::pin::toPin<PinType::Color>(glm::vec3(0.1f, 0.2f, 0.3f));
    CHECK(c.w == 1.0f);
    CHECK(c.x == 0.1f);
    // An Entity is an Int pin.
    CHECK(HE::api::pin::toPin<PinType::Int>(HE::api::Entity(42u)) == 42);
    // A Vec4 stays a Vec4, not a Color, once boxed.
    CHECK(hc::toValue(HE::api::pin::toPin<PinType::Vec4>(glm::vec4(1.0f))).type == PinType::Vec4);
    const auto ints = HE::api::pin::toPinArray<PinType::Int>(std::vector<HE::api::Entity>{ 1u, 2u });
    CHECK(ints == std::vector<int>{ 1, 2 });
}
