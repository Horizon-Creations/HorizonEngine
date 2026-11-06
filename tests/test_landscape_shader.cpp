// The landscape shaders' source (Thema 326, step 2 part A). The compilers (scripts/validate_embedded_shaders.py: glslangValidator)
// check each stage on its own; they do not check that the fragment shader's inputs are written by the vertex shader, and
// nothing checks the rules the CPU relies on. Those are checked here, as text.
#include "doctest.h"
#include <HorizonRendering/LandscapeShaderSource.h>
#include <Backends/Vulkan/VulkanLandscapeLayout.h>

#include <cctype>
#include <cstddef>
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
	                       "uTileWeights0", "uTileWeights1", "uTileWeights2", "uCoarseWeights0", "uCoarseWeights1", "uCoarseWeights2",
	                       "uTex", "uTexMaps", "uAlbedoArray", "uNormalArray", "uMaskArray" })
		CHECK_MESSAGE(fs.count(u) == 1, "fragment shader lacks " << u);
	// nothing else sneaks in unannounced: a new uniform needs the frame contract, the doc and this list
	CHECK(vs.size() == 14);
	CHECK(fs.size() == 20);
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

TEST_CASE("landscape shader source: the MSL port has every GLSL uniform as a member of its two structs, and the bindings MetalLandscape uses")
{
	const std::string msl = HE::glsl::kLandscapeMSL;
	std::set<std::string> uniforms = uniformsOf(HE::glsl::kLandscapeVS);
	for (const std::string& u : uniformsOf(HE::glsl::kLandscapeFS)) uniforms.insert(u);
	// the images are textures in MSL, not struct members
	const std::set<std::string> images = { "uTileHeights", "uSlotTable", "uCoarseHeights", "uTileWeights0", "uTileWeights1", "uTileWeights2",
	                                       "uCoarseWeights0", "uCoarseWeights1", "uCoarseWeights2", "uAlbedoArray", "uNormalArray", "uMaskArray" };
	int members = 0;
	for (const std::string& u : uniforms)
	{
		if (images.count(u)) continue;
		std::string m = u.substr(1);                        // uViewProj -> viewProj
		m[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(m[0])));
		INFO("GLSL uniform " << u << " as MSL member " << m);
		CHECK((msl.find(" " + m + ";") != std::string::npos || msl.find(" " + m + "[") != std::string::npos));
		++members;
	}
	CHECK(members == 17);                                   // the vertex stage's 11 plus the fragment stage's 5 (layer colours, sun colour and direction, ambient, tex)
	// entry points (MetalLandscape.mm looks them up by name) and the indices it binds: buffers 20 and 21, vertex textures 20-22, fragment 20-29
	CHECK(msl.find("vertex LsVertexOut vertexMain") != std::string::npos);
	CHECK(msl.find("fragment float4 fragmentMain") != std::string::npos);
	for (const char* b : { "[[buffer(20)]]", "[[buffer(21)]]" }) CHECK(msl.find(b) != std::string::npos);
	for (int t = 20; t <= 29; ++t) CHECK(msl.find("[[texture(" + std::to_string(t) + ")]]") != std::string::npos);
	CHECK(msl.find("[[texture(30)]]") == std::string::npos);
	CHECK(msl.find("[[texture(19)]]") == std::string::npos);   // nothing below 20: MetalRenderer owns those
	// the same distance for the morph as the GLSL: horizontal plus the slab term, never the vertex's own height
	const size_t at = msl.find("float d = sqrt(");
	REQUIRE(at != std::string::npos);
	const std::string line = msl.substr(at, msl.find(';', at) - at);
	CHECK(line.find("G.cameraRel.w") != std::string::npos);
	CHECK(line.find("d2") != std::string::npos);
	CHECK(msl.find("discard_fragment") != std::string::npos);   // the world's edge
	CHECK(msl.find("layout(") == std::string::npos);
}

TEST_CASE("landscape shader source: the HLSL port has every GLSL uniform as a member of its two constant buffers, and the registers the D3D classes bind")
{
	const std::string hlsl = HE::glsl::kLandscapeHLSL;
	std::set<std::string> uniforms = uniformsOf(HE::glsl::kLandscapeVS);
	for (const std::string& u : uniformsOf(HE::glsl::kLandscapeFS)) uniforms.insert(u);
	const std::set<std::string> images = { "uTileHeights", "uSlotTable", "uCoarseHeights", "uTileWeights0", "uTileWeights1", "uTileWeights2",
	                                       "uCoarseWeights0", "uCoarseWeights1", "uCoarseWeights2", "uAlbedoArray" };
	// The GLSL and the MSL read the group's NORMAL and MASK arrays too (02.11.2026); the D3D port does not yet: its ground is lit with the
	// geometric normal and no AO. This list is the known gap (Windows work order, docs/landscape-world-scale-windows-auftrag-2026-11-02.md §1):
	// when the port has them, they come out of this list and into the checks above.
	const std::set<std::string> notPortedYet = { "uTexMaps", "uNormalArray", "uMaskArray" };
	for (const std::string& u : notPortedYet) CHECK_MESSAGE(uniforms.count(u) == 1, "the GLSL no longer has " << u << ": take it off the list");
	// `patch` is an HLSL keyword: the per-patch block spells uPatch as lsPatch, everything else as the MSL does
	int members = 0;
	for (const std::string& u : uniforms)
	{
		if (images.count(u) || notPortedYet.count(u)) continue;
		std::string m = u == "uPatch" ? std::string("LsPatch") : u.substr(1);
		m[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(m[0])));
		INFO("GLSL uniform " << u << " as HLSL member " << m);
		CHECK((hlsl.find(" " + m + ";") != std::string::npos || hlsl.find(" " + m + "[") != std::string::npos));
		++members;
	}
	CHECK(members == 16);
	// entry points (D3D11Landscape / D3D12Landscape compile them by name), the two blocks and the ten images: b12/b13, t32-t41, nothing else
	CHECK(hlsl.find("VSMain(uint vid : SV_VertexID)") != std::string::npos);
	CHECK(hlsl.find("float4 PSMain(") != std::string::npos);
	CHECK(hlsl.find("cbuffer LsGlobals : register(b12)") != std::string::npos);
	CHECK(hlsl.find("cbuffer LsPatch : register(b13)") != std::string::npos);
	for (int t = 32; t <= 41; ++t) CHECK(hlsl.find("register(t" + std::to_string(t) + ")") != std::string::npos);
	CHECK(hlsl.find("register(t42)") == std::string::npos);
	CHECK(hlsl.find("register(t31)") == std::string::npos);    // nothing in the range the D3D11 renderer and its graph materials bind
	CHECK(hlsl.find("register(s13)") != std::string::npos);     // the albedo array's sampler, the only one (D3D11Landscape and D3D12Landscape make it)
	CHECK(hlsl.find("register(s", hlsl.find("register(s13)") + 1) == std::string::npos);
	// the same distance for the morph as the GLSL: horizontal plus the slab term, never the vertex's own height
	const size_t at = hlsl.find("float d = sqrt(");
	REQUIRE(at != std::string::npos);
	const std::string line = hlsl.substr(at, hlsl.find(';', at) - at);
	CHECK(line.find("cameraRel.w") != std::string::npos);
	CHECK(line.find("d2") != std::string::npos);
	CHECK(hlsl.find("discard;") != std::string::npos);          // the world's edge
	// the derivatives are taken before the discard: a divergent discard must not make them undefined
	CHECK(hlsl.find("ddx(") < hlsl.find("discard;"));
	CHECK(hlsl.find("layout(") == std::string::npos);
}

// ─── Vulkan: the same GLSL behind a prelude ───────────────────────────────────────────────────────────────────────────────────
namespace
{
// "layout(std140, set = 0, binding = 0) uniform LsGlobals { ... };" -> member name -> std140 offset, and the block's size.
struct Std140Block { std::map<std::string, size_t> offset; size_t size = 0; };
Std140Block parseStd140(const std::string& src, const std::string& block)
{
	Std140Block r;
	const size_t at = src.find("uniform " + block);
	REQUIRE(at != std::string::npos);
	const size_t open = src.find('{', at), close = src.find("};", open);
	REQUIRE((open != std::string::npos && close != std::string::npos));
	const std::regex member("\\s*(\\w+)\\s+(\\w+)(?:\\[(\\d+)\\])?\\s*;");
	const std::string body = src.substr(open + 1, close - open - 1);
	size_t off = 0;
	for (std::sregex_iterator it(body.begin(), body.end(), member), end; it != end; ++it)
	{
		const std::string type = (*it)[1], name = (*it)[2];
		const size_t count = (*it)[3].matched ? std::stoul((*it)[3]) : 1;
		size_t align = 16, size = 16;          // vec4, ivec4, a vec3 (12 bytes in a 16-byte row) and an array element are one row
		if (type == "mat4") size = 64;
		else if (type == "vec3") size = 12;
		else REQUIRE_MESSAGE((type == "vec4" || type == "ivec4"), "unexpected member type " << type);
		if (count > 1) size = 16 * count;       // std140 rounds an array element up to a row
		off = (off + align - 1) / align * align;
		r.offset[name] = off;
		off += size;
	}
	r.size = (off + 15) / 16 * 16;
	return r;
}
} // namespace

TEST_CASE("landscape shader source (Vulkan): the prelude replaces every GL declaration, the body is the GL one, and the stages link")
{
	const std::string vs = HE::glsl::BuildLandscapeVertexGLSL450(), fs = HE::glsl::BuildLandscapeFragmentGLSL450();
	REQUIRE_FALSE(vs.empty());
	REQUIRE_FALSE(fs.empty());
	for (const std::string* src : { &vs, &fs })
	{
		CHECK(src->rfind("#version 450", 0) == 0);
		CHECK(src->find("#version 410") == std::string::npos);
		CHECK(src->find("//#LANDSCAPE_BODY#") == std::string::npos);     // the marker is the cut, not part of the result
		CHECK(uniformsOf(*src).empty());                                  // no loose uniform is left: all of them are block members or images with a binding
	}
	// every name the GL stage declares as a uniform is declared by the prelude as a block member, a push constant or an image
	struct Stage { const char* name; std::string glsl; const std::string* built; };
	for (const Stage& st : { Stage{ "vertex", HE::glsl::kLandscapeVS, &vs }, Stage{ "fragment", HE::glsl::kLandscapeFS, &fs } })
	{
		for (const std::string& u : uniformsOf(st.glsl))
		{
			INFO(st.name << " uniform " << u);
			CHECK((st.built->find(" " + u + ";") != std::string::npos || st.built->find(" " + u + "[") != std::string::npos));
		}
	}
	// the body after the cut is byte for byte the GL body
	const std::string marker = "//#LANDSCAPE_BODY#";
	const std::string glVs = HE::glsl::kLandscapeVS, glFs = HE::glsl::kLandscapeFS;
	CHECK(vs.size() > glVs.substr(glVs.find(marker) + marker.size()).size());
	CHECK(vs.compare(vs.size() - glVs.substr(glVs.find(marker) + marker.size()).size(), std::string::npos, glVs.substr(glVs.find(marker) + marker.size())) == 0);
	CHECK(fs.compare(fs.size() - glFs.substr(glFs.find(marker) + marker.size()).size(), std::string::npos, glFs.substr(glFs.find(marker) + marker.size())) == 0);
	// the interface: location 0 in the vertex stage is the grid, location 0 between the stages is the world position
	CHECK(vs.find("layout(location = 0) in vec2 aGrid;") != std::string::npos);
	CHECK(vs.find("layout(location = 0) out vec3 vWorldRel;") != std::string::npos);
	CHECK(fs.find("layout(location = 0) in vec3 vWorldRel;") != std::string::npos);
	// push constants are the per-patch uniforms and nothing else, in the vertex stage only
	CHECK(vs.find("layout(push_constant)") != std::string::npos);
	CHECK(fs.find("push_constant") == std::string::npos);
}

TEST_CASE("landscape shader source (Vulkan): LsGlobals is the same block in both stages, and the C++ mirror puts every member at its std140 offset")
{
	const std::string vs = HE::glsl::BuildLandscapeVertexGLSL450(), fs = HE::glsl::BuildLandscapeFragmentGLSL450();
	const Std140Block bv = parseStd140(vs, "LsGlobals"), bf = parseStd140(fs, "LsGlobals");
	CHECK(bv.offset == bf.offset);                                  // one block, so the two stages read one layout
	CHECK(bv.size == sizeof(HE::vkls::Globals));
	const auto at = [&](const char* n) { REQUIRE(bv.offset.count(n) == 1); return bv.offset.at(n); };
	CHECK(at("uViewProj") == offsetof(HE::vkls::Globals, viewProj));
	CHECK(at("uCameraRel") == offsetof(HE::vkls::Globals, cameraRel));
	CHECK(at("uWorld") == offsetof(HE::vkls::Globals, world));
	CHECK(at("uHeightInfo") == offsetof(HE::vkls::Globals, heightInfo));
	CHECK(at("uOrigin") == offsetof(HE::vkls::Globals, origin));
	CHECK(at("uTile") == offsetof(HE::vkls::Globals, tile));
	CHECK(at("uHeightRect") == offsetof(HE::vkls::Globals, heightRect));
	CHECK(at("uCoarseGrid") == offsetof(HE::vkls::Globals, coarseGrid));
	CHECK(at("uLayerColor") == offsetof(HE::vkls::Globals, layerColor));
	CHECK(at("uSunDir") == offsetof(HE::vkls::Globals, sunDir));
	CHECK(at("uSunColor") == offsetof(HE::vkls::Globals, sunColor));
	CHECK(at("uAmbient") == offsetof(HE::vkls::Globals, ambient));
	CHECK(at("uTex") == offsetof(HE::vkls::Globals, tex));
	CHECK(at("uTexMaps") == offsetof(HE::vkls::Globals, texMaps));
	CHECK(bv.offset.size() == 14);                                  // every member the C++ struct has, nothing more

	// the push-constant block: the three per-patch uniforms, 48 bytes
	const Std140Block pc = parseStd140(vs, "LsPatch");
	CHECK(pc.offset.at("uPatch") == offsetof(HE::vkls::PatchPush, patch));
	CHECK(pc.offset.at("uPatchPos") == offsetof(HE::vkls::PatchPush, patchPos));
	CHECK(pc.offset.at("uMorph") == offsetof(HE::vkls::PatchPush, morph));
	CHECK(pc.size == sizeof(HE::vkls::PatchPush));

	// the images: set 0, the binding numbers the C++ side lays its descriptor set out with, each once
	const std::map<std::string, unsigned> bindings = {
		{ "uTileHeights", HE::glsl::kLandscapeVkBindingTileHeights }, { "uSlotTable", HE::glsl::kLandscapeVkBindingSlotTable },
		{ "uCoarseHeights", HE::glsl::kLandscapeVkBindingCoarseHeights }, { "uTileWeights0", HE::glsl::kLandscapeVkBindingTileWeights },
		{ "uTileWeights1", HE::glsl::kLandscapeVkBindingTileWeights + 1 }, { "uTileWeights2", HE::glsl::kLandscapeVkBindingTileWeights + 2 },
		{ "uCoarseWeights0", HE::glsl::kLandscapeVkBindingCoarseWeights }, { "uCoarseWeights1", HE::glsl::kLandscapeVkBindingCoarseWeights + 1 },
		{ "uCoarseWeights2", HE::glsl::kLandscapeVkBindingCoarseWeights + 2 }, { "uAlbedoArray", HE::glsl::kLandscapeVkBindingAlbedo },
		{ "uNormalArray", HE::glsl::kLandscapeVkBindingNormal }, { "uMaskArray", HE::glsl::kLandscapeVkBindingMask } };
	CHECK(HE::glsl::kLandscapeVkBindingCount == bindings.size() + 1);
	for (const auto& [name, binding] : bindings)
	{
		const std::string decl = "layout(set = 0, binding = " + std::to_string(binding) + ")";
		bool found = false;
		for (const std::string* src : { &vs, &fs })
		{
			const size_t n = src->find(" " + name + ";");
			if (n == std::string::npos) continue;
			found = true;
			const size_t lineStart = src->rfind('\n', n) + 1;
			const std::string line = src->substr(lineStart, n - lineStart);
			INFO(name << ": '" << line << "'");
			CHECK(line.find("binding = " + std::to_string(binding) + ")") != std::string::npos);
			CHECK(line.find("set = 0") != std::string::npos);
		}
		CHECK_MESSAGE(found, name << " is declared by neither stage");
	}
	CHECK(HE::vkls::worldUsable(HE::LandscapeRenderStatic{}) == false);   // an empty world is not drawable
}
