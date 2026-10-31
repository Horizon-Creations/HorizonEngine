// The landscape shaders' source (Thema 326, step 2 part A). The compilers (scripts/validate_embedded_shaders.py: glslangValidator)
// check each stage on its own; they do not check that the fragment shader's inputs are written by the vertex shader, and
// nothing checks the rules the CPU relies on. Those are checked here, as text.
#include "doctest.h"
#include <HorizonRendering/LandscapeShaderSource.h>

#include <map>
#include <regex>
#include <set>
#include <string>

namespace
{
// "in vec3 vWorldRel;" -> {"vWorldRel": "vec3"}; the same for "out".
std::map<std::string, std::string> interfaceOf(const std::string& src, const char* dir)
{
	std::map<std::string, std::string> r;
	const std::regex re(std::string("(?:^|\\n)\\s*") + dir + "\\s+(?:flat\\s+)?(\\w+)\\s+(\\w+)\\s*;");
	for (std::sregex_iterator it(src.begin(), src.end(), re), end; it != end; ++it) r[(*it)[2]] = (*it)[1];
	return r;
}

std::set<std::string> uniformsOf(const std::string& src)
{
	std::set<std::string> r;
	const std::regex re("(?:^|\\n)\\s*uniform\\s+(?:highp\\s+|mediump\\s+)?\\w+\\s+(\\w+)");
	for (std::sregex_iterator it(src.begin(), src.end(), re), end; it != end; ++it) r.insert((*it)[1]);
	return r;
}
} // namespace

TEST_CASE("landscape shader source: the fragment shader reads only what the vertex shader writes")
{
	const std::string vs = HE::glsl::kLandscapeVS, fs = HE::glsl::kLandscapeFS;
	const auto outs = interfaceOf(vs, "out");
	const auto ins = interfaceOf(fs, "in");
	REQUIRE_FALSE(ins.empty());
	for (const auto& [name, type] : ins)
	{
		INFO("fragment input " << name);
		REQUIRE(outs.count(name) == 1);
		CHECK(outs.at(name) == type);
	}
	CHECK(interfaceOf(vs, "in").count("aGrid") == 1);          // the only vertex attribute: the grid position
	CHECK(interfaceOf(vs, "in").size() == 1);
}

TEST_CASE("landscape shader source: GLSL 4.10 like the sky, no layout qualifiers (a Vulkan prelude must be able to take the body)")
{
	for (const std::string src : { std::string(HE::glsl::kLandscapeVS), std::string(HE::glsl::kLandscapeFS) })
	{
		CHECK(src.find("#version 410 core") != std::string::npos);
		CHECK(src.find("layout(") == std::string::npos);
		CHECK(src.find("layout (") == std::string::npos);
	}
}

TEST_CASE("landscape shader source: the uniforms are the ones the frame contract has, and the shared ones agree")
{
	const auto vs = uniformsOf(HE::glsl::kLandscapeVS), fs = uniformsOf(HE::glsl::kLandscapeFS);
	// what the vertex stage needs from LandscapeFrame / the patch / the images
	for (const char* u : { "uViewProj", "uCameraRel", "uWorld", "uHeightInfo", "uOrigin", "uTile", "uHeightRect", "uCoarseGrid", "uPatch",
	                       "uPatchPos", "uMorph", "uTileHeights", "uSlotTable", "uCoarseHeights" })
		CHECK_MESSAGE(vs.count(u) == 1, "vertex shader lacks " << u);
	for (const char* u : { "uWorld", "uHeightInfo", "uOrigin", "uTile", "uLayerColor", "uSunDir", "uSunColor", "uAmbient", "uSlotTable",
	                       "uTileWeights0", "uTileWeights1", "uTileWeights2" })
		CHECK_MESSAGE(fs.count(u) == 1, "fragment shader lacks " << u);
	// nothing else sneaks in unannounced: a new uniform needs the frame contract, the doc and this list
	CHECK(vs.size() == 14);
	CHECK(fs.size() == 12);
}

TEST_CASE("landscape shader source: the morph distance is horizontal plus the slab term, never the vertex's own height")
{
	const std::string vs = HE::glsl::kLandscapeVS;
	const size_t at = vs.find("float d = sqrt(");
	REQUIRE(at != std::string::npos);
	const std::string line = vs.substr(at, vs.find(';', at) - at);
	CHECK(line.find("uCameraRel.w") != std::string::npos);     // the slab term
	CHECK(line.find("d2") != std::string::npos);               // the horizontal part
	CHECK(line.find("vWorldRel") == std::string::npos);        // not the final position
	CHECK(line.find("uCameraRel.y") == std::string::npos);     // not the camera's height against the vertex's
	// the world's edge is discarded in the fragment stage, in absolute space
	const std::string fs = HE::glsl::kLandscapeFS;
	CHECK(fs.find("discard") != std::string::npos);
	CHECK(fs.find("uOrigin.xz") != std::string::npos);
}
