// End-to-end loader test (plan §10.4): the generated parity-fixture classes,
// built into a REAL shared library (test_hcgen, CMake) with the C manifest
// export, loaded through CompiledClassTable — the exact code path the shipped
// game takes at startup. hc_codegen bakes engineVersion "dev" into the fixture
// manifest (tools/hc_codegen), which doubles as the version-handshake probe.
#include "doctest.h"
#include <HorizonCode/HcCompiledLoader.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <filesystem>

using namespace HorizonCode;

#ifdef HE_TEST_HCGEN_LIB

TEST_CASE("CompiledClassTable rejects a library built for another engine version")
{
	CompiledClassTable table;
	CHECK_FALSE(table.load(HE_TEST_HCGEN_LIB, "some-other-engine-build"));
	CHECK_FALSE(table.loaded());
	CHECK(table.size() == 0);
	CHECK(table.find("fix/ref_target") == nullptr);
	CHECK(table.create("fix/ref_target") == nullptr);
}

TEST_CASE("CompiledClassTable loads the manifest and instantiates classes from the dylib")
{
	CompiledClassTable table;
	REQUIRE(table.load(HE_TEST_HCGEN_LIB, "dev"));
	CHECK(table.loaded());
	CHECK(table.size() > 10);   // every parity fixture registered

	// Idempotent: a second load is a no-op success (process-lifetime library).
	CHECK(table.load(HE_TEST_HCGEN_LIB, "dev"));

	// Missing key → null (the caller's interpreter-fallback cue).
	CHECK(table.create("Content/DoesNotExist.hasset") == nullptr);

	// A dylib-created instance is a full citizen of the Runtime.
	Runtime rt;
	CompiledPtr inst = table.create("fix/ref_target");
	REQUIRE(inst != nullptr);
	const InstanceId id = rt.addCompiled(std::move(inst));
	REQUIRE(id != 0);

	rt.fireEvent(id, "Construct");
	CHECK(rt.getVariable(id, "constructed").f == 1.0f);
	CHECK(rt.getVariable(id, "hp").f == 100.0f);

	std::vector<Value> results;
	REQUIRE(rt.callFunction(id, "Damage", true, { Value::ofFloat(30.0f) }, &results));
	REQUIRE(results.size() == 1);
	CHECK(results[0].f == 70.0f);
	CHECK(rt.getVariable(id, "hp").f == 70.0f);

	// Private functions stay gated across the dylib boundary too.
	CHECK_FALSE(rt.callFunction(id, "Heal", true));
}

TEST_CASE("CompiledClassTable: a missing library is the quiet interpreted case")
{
	CompiledClassTable table;
	CHECK_FALSE(table.load("/nonexistent/path/libHorizonCodeGen.dylib", "dev"));
	CHECK_FALSE(table.loaded());
}

#endif // HE_TEST_HCGEN_LIB
