// D3D11ShaderManager / D3D12ShaderManager: shutdown + hot-reload leak check.
//
// Both managers used to be stubs whose cleanup() was an empty body with a
// TODO (audit of 2026-09-19, Thema 62). Neither renderer routes its shaders
// through them — D3D11Renderer/D3D12Renderer keep their blobs, shader objects,
// PSOs and root signatures as ComPtr members of their Impl and release them in
// Shutdown() — so the audit's "possible leak" was in dead code. The managers
// are real now, and this is the evidence that cleanup() lets go of everything:
//
//   * the test keeps its OWN ComPtr copy of each blob / shader object across
//     cleanup() and reads the COM refcount (AddRef → Release returns the new
//     count). Manager + test = 2 before, test alone = 1 after. Against the old
//     empty body the count would stay at 2 — this is the check that
//     discriminates, the container sizes alone would not.
//   * the loop runs several rounds on the same manager (there is no shader
//     hot-reload in the D3D renderers; a load→compile→cleanup loop on one
//     manager is what such a reload would do), so a manager that "forgot" an
//     entry instead of releasing it would show up as a growing count.
//
// Always WARP (the verdict must not depend on the runner's driver), so a CI
// runner without a GPU still gets it. D3D12 needs no device at all: its
// shader IS the bytecode blob.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "Backends/D3D11/D3D11ShaderManager.h"
#include "Backends/D3D12/D3D12ShaderManager.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "doctest.h"
#include "TestFsUtil.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

// The COM refcount as the object reports it: AddRef returns the new count,
// Release the count after the decrement — together they read without changing.
ULONG refcount(IUnknown* p)
{
	p->AddRef();
	return p->Release();
}

const char* kVS = R"(
struct VSIn  { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 1.0); return o; }
)";
const char* kPS = R"(
float4 main(float4 pos : SV_Position) : SV_Target { return float4(1, 0.5, 0.25, 1); }
)";
const char* kCS = R"(
RWBuffer<float> outBuf : register(u0);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { outBuf[id.x] = 1.0; }
)";
const char* kBroken = "float4 main() : SV_Target { return this_is_not_hlsl; }";

struct ShaderFiles
{
	std::filesystem::path dir, vs, ps, cs, broken;
	ShaderFiles()
	{
		dir = std::filesystem::temp_directory_path() / "he_d3d_shader_manager";
		std::filesystem::create_directories(dir);
		vs = dir / "test.vs.hlsl";
		ps = dir / "test.ps.hlsl";
		cs = dir / "test.cs.hlsl";
		broken = dir / "broken.ps.hlsl";
		std::ofstream(vs) << kVS;
		std::ofstream(ps) << kPS;
		std::ofstream(cs) << kCS;
		std::ofstream(broken) << kBroken;
	}
	~ShaderFiles() { he_test::removeAllQuiet(dir); }
};

// WARP, debug layer if the SDK layers are installed, plain otherwise.
bool createWarpDevice11(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& ctx, std::string& log)
{
	const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
	D3D_FEATURE_LEVEL got{};
	for (UINT flags : { UINT(D3D11_CREATE_DEVICE_DEBUG), UINT(0) })
	{
		const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
		                                     &want, 1, D3D11_SDK_VERSION, &device, &got, &ctx);
		if (SUCCEEDED(hr))
		{
			log = flags ? "debug layer on" : "no D3D11 debug layer on this machine";
			return true;
		}
		log = "D3D11CreateDevice(WARP) failed: " + std::to_string(hr);
	}
	return false;
}
} // namespace

TEST_CASE("D3D11ShaderManager: cleanup() releases every blob and shader object, repeatedly")
{
	ShaderFiles files;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	std::string log;
	REQUIRE_MESSAGE(createWarpDevice11(device, ctx, log), log);
	INFO(log);

	D3D11ShaderManager mgr;
	mgr.setDevice(device.Get());

	// Several shutdown/hot-reload rounds on ONE manager: each round loads,
	// compiles, groups, then cleans up — and the refcounts prove the manager
	// let go, every time, with nothing carried over into the next round.
	for (int round = 0; round < 6; ++round)
	{
		CAPTURE(round);
		CHECK(mgr.loadedShaderCount() == 0);
		CHECK(mgr.loadedProgramCount() == 0);

		ShaderHandle vs = mgr.load(files.vs.string().c_str(), HE::ShaderType::Vertex);
		ShaderHandle ps = mgr.load(files.ps.string().c_str(), HE::ShaderType::Fragment);
		ShaderHandle cs = mgr.load(files.cs.string().c_str(), HE::ShaderType::Compute);
		REQUIRE(vs.id != 0);
		REQUIRE(ps.id != 0);
		REQUIRE(cs.id != 0);
		CHECK_FALSE(vs.ready);
		REQUIRE(mgr.compile(vs));
		REQUIRE(mgr.compile(ps));
		REQUIRE(mgr.compile(cs));
		CHECK(vs.ready);
		CHECK(mgr.loadedShaderCount() == 3);

		ShaderProgramHandle prog = mgr.createProgram({ vs, ps });
		CHECK(prog.ready);
		CHECK(prog.id != 0);
		CHECK(prog.shaders.size() == 2);
		CHECK(mgr.loadedProgramCount() == 1);

		// Our own references, held across cleanup().
		std::vector<ComPtr<IUnknown>> held;
		for (const ShaderHandle& h : { vs, ps, cs })
		{
			ID3DBlob* blob = mgr.blobFor(h.id);
			ID3D11DeviceChild* obj = mgr.shaderFor(h.id);
			REQUIRE(blob != nullptr);
			REQUIRE(obj != nullptr);
			CHECK(blob->GetBufferSize() > 0);
			held.emplace_back(blob);
			held.emplace_back(obj);
		}
		// The stage objects really are what the handle type says.
		{
			ComPtr<ID3D11VertexShader>  asVS;
			ComPtr<ID3D11PixelShader>   asPS;
			ComPtr<ID3D11ComputeShader> asCS;
			CHECK(SUCCEEDED(mgr.shaderFor(vs.id)->QueryInterface(IID_PPV_ARGS(&asVS))));
			CHECK(SUCCEEDED(mgr.shaderFor(ps.id)->QueryInterface(IID_PPV_ARGS(&asPS))));
			CHECK(SUCCEEDED(mgr.shaderFor(cs.id)->QueryInterface(IID_PPV_ARGS(&asCS))));
		}
		// Manager + us.
		for (auto& p : held)
			CHECK(refcount(p.Get()) == 2);

		// A single release() mid-round (what a per-shader hot-reload does): that
		// shader is gone from the manager, the others are untouched.
		ID3DBlob* csBlobBefore = mgr.blobFor(cs.id);
		CHECK(csBlobBefore != nullptr);
		mgr.release(cs);
		CHECK(mgr.blobFor(cs.id) == nullptr);
		CHECK(mgr.shaderFor(cs.id) == nullptr);
		CHECK(mgr.loadedShaderCount() == 2);
		CHECK(refcount(held[4].Get()) == 1); // cs blob: only us now
		CHECK(refcount(held[5].Get()) == 1); // cs shader object: only us now
		CHECK(refcount(held[0].Get()) == 2); // vs blob still shared with the manager

		mgr.cleanup();

		CHECK(mgr.loadedShaderCount() == 0);
		CHECK(mgr.loadedProgramCount() == 0);
		CHECK(mgr.blobFor(vs.id) == nullptr);
		CHECK(mgr.shaderFor(vs.id) == nullptr);
		CHECK(mgr.blobFor(ps.id) == nullptr);
		CHECK(mgr.shaderFor(ps.id) == nullptr);
		// Only we are left holding them — the manager released its reference.
		for (auto& p : held)
			CHECK(refcount(p.Get()) == 1);
		held.clear();
	}

	// The device survives cleanup(): the manager is still usable afterwards.
	ShaderHandle again = mgr.load(files.ps.string().c_str(), HE::ShaderType::Fragment);
	CHECK(mgr.compile(again));
	CHECK(mgr.shaderFor(again.id) != nullptr);
	mgr.cleanup();
	CHECK(mgr.loadedShaderCount() == 0);
}

TEST_CASE("D3D11ShaderManager: failure paths do not leak or lie")
{
	ShaderFiles files;
	D3D11ShaderManager mgr; // no device on purpose

	// Missing file: invalid handle, nothing tracked.
	ShaderHandle missing = mgr.load((files.dir / "does-not-exist.hlsl").string().c_str(), HE::ShaderType::Vertex);
	CHECK(missing.id == 0);
	CHECK_FALSE(missing.ready);
	CHECK(mgr.loadedShaderCount() == 0);

	// Broken HLSL: compile() says no, the handle stays not-ready, no blob appears.
	ShaderHandle broken = mgr.load(files.broken.string().c_str(), HE::ShaderType::Fragment);
	REQUIRE(broken.id != 0);
	CHECK_FALSE(mgr.compile(broken));
	CHECK_FALSE(broken.ready);
	CHECK(mgr.blobFor(broken.id) == nullptr);
	// ... and createProgram refuses an uncompiled stage.
	ShaderProgramHandle prog = mgr.createProgram({ broken });
	CHECK_FALSE(prog.ready);
	CHECK(prog.id == 0);
	CHECK(mgr.loadedProgramCount() == 0);

	// Without a device: bytecode yes, shader object no (documented behaviour).
	ShaderHandle ps = mgr.load(files.ps.string().c_str(), HE::ShaderType::Fragment);
	REQUIRE(mgr.compile(ps));
	ComPtr<ID3DBlob> blob = mgr.blobFor(ps.id);
	REQUIRE(blob);
	CHECK(mgr.shaderFor(ps.id) == nullptr);
	CHECK(refcount(blob.Get()) == 2);

	// Unknown handle after cleanup: compile() fails cleanly instead of crashing.
	mgr.cleanup();
	CHECK(refcount(blob.Get()) == 1);
	CHECK_FALSE(mgr.compile(ps));
	CHECK(mgr.loadedShaderCount() == 0);
}

TEST_CASE("D3D12ShaderManager: cleanup() releases every bytecode blob, repeatedly")
{
	ShaderFiles files;
	D3D12ShaderManager mgr;

	for (int round = 0; round < 6; ++round)
	{
		CAPTURE(round);
		CHECK(mgr.loadedShaderCount() == 0);
		CHECK(mgr.loadedProgramCount() == 0);

		ShaderHandle vs = mgr.load(files.vs.string().c_str(), HE::ShaderType::Vertex);
		ShaderHandle ps = mgr.load(files.ps.string().c_str(), HE::ShaderType::Fragment);
		ShaderHandle cs = mgr.load(files.cs.string().c_str(), HE::ShaderType::Compute);
		REQUIRE(vs.id != 0);
		REQUIRE(ps.id != 0);
		REQUIRE(cs.id != 0);
		REQUIRE(mgr.compile(vs));
		REQUIRE(mgr.compile(ps));
		REQUIRE(mgr.compile(cs));
		CHECK(mgr.loadedShaderCount() == 3);

		ShaderProgramHandle prog = mgr.createProgram({ vs, ps });
		CHECK(prog.ready);
		CHECK(mgr.loadedProgramCount() == 1);

		std::vector<ComPtr<ID3DBlob>> held;
		for (const ShaderHandle& h : { vs, ps, cs })
		{
			ID3DBlob* blob = mgr.blobFor(h.id);
			REQUIRE(blob != nullptr);
			CHECK(blob->GetBufferSize() > 0);
			held.emplace_back(blob);
		}
		for (auto& p : held)
			CHECK(refcount(p.Get()) == 2);

		// Per-shader release (hot-reload of one stage) drops exactly that blob.
		mgr.release(cs);
		CHECK(mgr.blobFor(cs.id) == nullptr);
		CHECK(mgr.loadedShaderCount() == 2);
		CHECK(refcount(held[2].Get()) == 1);
		CHECK(refcount(held[0].Get()) == 2);

		// releaseProgram drops the grouping and nothing else.
		mgr.releaseProgram(prog);
		CHECK(mgr.loadedProgramCount() == 0);
		CHECK(mgr.blobFor(vs.id) != nullptr);

		mgr.cleanup();

		CHECK(mgr.loadedShaderCount() == 0);
		CHECK(mgr.loadedProgramCount() == 0);
		CHECK(mgr.blobFor(vs.id) == nullptr);
		CHECK(mgr.blobFor(ps.id) == nullptr);
		for (auto& p : held)
			CHECK(refcount(p.Get()) == 1);
		held.clear();
	}

	// Failure paths: missing file, broken HLSL, program from an uncompiled stage.
	ShaderHandle missing = mgr.load((files.dir / "does-not-exist.hlsl").string().c_str(), HE::ShaderType::Vertex);
	CHECK(missing.id == 0);
	ShaderHandle broken = mgr.load(files.broken.string().c_str(), HE::ShaderType::Fragment);
	REQUIRE(broken.id != 0);
	CHECK_FALSE(mgr.compile(broken));
	CHECK(mgr.blobFor(broken.id) == nullptr);
	ShaderProgramHandle prog = mgr.createProgram({ broken });
	CHECK_FALSE(prog.ready);
	mgr.cleanup();
	CHECK(mgr.loadedShaderCount() == 0);
	CHECK_FALSE(mgr.compile(broken));
}
#endif // _WIN32
