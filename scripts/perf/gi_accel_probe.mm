// Cost of Metal's GI acceleration-structure upkeep, before and after the
// dirty check and the batched BLAS build (perf audit REN-404/405/406, Thema 216).
//
//   clang++ -std=c++20 -O2 -fobjc-arc -framework Foundation -framework Metal \
//       scripts/perf/gi_accel_probe.mm -o /tmp/gi_accel_probe
//   /tmp/gi_accel_probe [instances=1000] [unique-meshes=100] [frames=300]
//
// BLAS (REN-406), all meshes new in one frame (a scene load):
//   old:   per mesh a command buffer, build, commit, waitUntilCompleted
//          (BuildBLAS before 8b421011): the render thread is blocked for all of it.
//   new:   one encoder on one command buffer, a scratch per build, commit, no
//          wait: the CPU time is the encode; "GPU done" is when the builds
//          finish, which the frame never waits for.
// TLAS per frame (REN-404/405), `instances` casters over `unique-meshes` BLAS,
// up to three frames in flight:
//   old:   linear BLAS search, fresh instance/colour/land/mesh-pointer/mesh-index
//          buffers, a fresh TLAS + scratch, build, commit (every frame).
//   kept:  map lookup, gather, byte compare with the live build: equal, nothing
//          else (a static scene after the change).
//   moved: as kept, but every transform changed: instance descriptors into a
//          ring slice, fresh TLAS + scratch, build, commit; tables stay.
// Not in the numbers: the per-object material resolution (giInstanceSurface,
// one ContentManager lookup, unchanged by this step) and the extractor walk.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct Mat4 { float m[16]; };
struct Vec4 { float v[4]; };

static id<MTLBuffer> makeGrid(id<MTLDevice> dev, int g, bool index, float seed)
{
	if (!index)
	{
		std::vector<float> v;
		for (int y = 0; y <= g; ++y)
			for (int x = 0; x <= g; ++x)
			{
				const float fx = x / float(g), fy = y / float(g);
				v.insert(v.end(), { fx, 0.1f * std::sin(seed + fx * 7.0f) * fy, fy, 0, 1, 0, fx, fy });
			}
		return [dev newBufferWithBytes:v.data() length:v.size() * sizeof(float)
		                       options:MTLResourceStorageModeShared];
	}
	std::vector<uint32_t> i;
	for (int y = 0; y < g; ++y)
		for (int x = 0; x < g; ++x)
		{
			const uint32_t a = y * (g + 1) + x, b = a + 1, c = a + g + 1, d = c + 1;
			i.insert(i.end(), { a, c, b, b, c, d });
		}
	return [dev newBufferWithBytes:i.data() length:i.size() * sizeof(uint32_t)
	                       options:MTLResourceStorageModeShared];
}

static MTLPrimitiveAccelerationStructureDescriptor* blasDesc(id<MTLBuffer> vb, id<MTLBuffer> ib, int g)
{
	MTLAccelerationStructureTriangleGeometryDescriptor* geom =
		[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	geom.vertexBuffer  = vb;
	geom.vertexStride  = sizeof(float) * 8;
	geom.vertexFormat  = MTLAttributeFormatFloat3;
	geom.indexBuffer   = ib;
	geom.indexType     = MTLIndexTypeUInt32;
	geom.triangleCount = NSUInteger(g * g * 2);
	geom.opaque        = YES;
	MTLPrimitiveAccelerationStructureDescriptor* d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	d.geometryDescriptors = @[ geom ];
	return d;
}

int main(int argc, char** argv)
{
	const int N = argc > 1 ? std::atoi(argv[1]) : 1000;
	const int M = argc > 2 ? std::atoi(argv[2]) : 100;
	const int F = argc > 3 ? std::atoi(argv[3]) : 300;
	const int G = 32; // 2048 triangles per mesh
	@autoreleasepool
	{
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (!dev || ![dev supportsRaytracing]) { std::printf("no ray tracing\n"); return 1; }
		id<MTLCommandQueue> q = [dev newCommandQueue];
		std::printf("%s, %d instances, %d unique meshes (%d tris each), %d frames\n",
		            dev.name.UTF8String, N, M, G * G * 2, F);

		std::vector<id<MTLBuffer>> vbs, ibs;
		for (int m = 0; m < M; ++m)
		{
			vbs.push_back(makeGrid(dev, G, false, float(m)));
			ibs.push_back(makeGrid(dev, G, true, 0));
		}

		// ── BLAS: a scene load, every mesh new ──────────────────────────────
		std::vector<id<MTLAccelerationStructure>> blas(M);
		for (int pass = 0; pass < 2; ++pass) // pass 0 warms the driver up, pass 1 is reported
		{
			auto t0 = Clock::now();
			for (int m = 0; m < M; ++m)
			@autoreleasepool {
				MTLPrimitiveAccelerationStructureDescriptor* d = blasDesc(vbs[m], ibs[m], G);
				MTLAccelerationStructureSizes s = [dev accelerationStructureSizesWithDescriptor:d];
				blas[m] = [dev newAccelerationStructureWithSize:s.accelerationStructureSize];
				id<MTLBuffer> scratch = [dev newBufferWithLength:s.buildScratchBufferSize options:MTLResourceStorageModePrivate];
				id<MTLCommandBuffer> cb = [q commandBuffer];
				id<MTLAccelerationStructureCommandEncoder> e = [cb accelerationStructureCommandEncoder];
				[e buildAccelerationStructure:blas[m] descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
				[e endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
			}
			const double oldMs = msSince(t0);

			t0 = Clock::now();
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLAccelerationStructureCommandEncoder> e = [cb accelerationStructureCommandEncoder];
			for (int m = 0; m < M; ++m)
			{
				MTLPrimitiveAccelerationStructureDescriptor* d = blasDesc(vbs[m], ibs[m], G);
				MTLAccelerationStructureSizes s = [dev accelerationStructureSizesWithDescriptor:d];
				blas[m] = [dev newAccelerationStructureWithSize:s.accelerationStructureSize];
				id<MTLBuffer> scratch = [dev newBufferWithLength:s.buildScratchBufferSize options:MTLResourceStorageModePrivate];
				[e buildAccelerationStructure:blas[m] descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
			}
			[e endEncoding];
			[cb commit];
			const double newCpuMs = msSince(t0);
			[cb waitUntilCompleted];
			const double newDoneMs = msSince(t0);
			if (pass == 1)
				std::printf("BLAS x%d   old (wait per mesh): %8.2f ms CPU blocked | new (batched): %6.2f ms CPU, GPU done after %6.2f ms\n",
				            M, oldMs, newCpuMs, newDoneMs);
		}

		// ── TLAS per frame ───────────────────────────────────────────────────
		std::vector<int>  meshOf(N);
		std::vector<Mat4> xform(N);
		std::vector<Vec4> colorSrc(N * 2);
		for (int i = 0; i < N; ++i)
		{
			meshOf[i] = (i * 7919) % M;
			xform[i]  = Mat4{{ 1,0,0,0, 0,1,0,0, 0,0,1,0, float(i % 100), 0, float(i / 100), 1 }};
			colorSrc[i * 2]     = Vec4{{ 0.5f, 0.5f, 0.5f, 0 }};
			colorSrc[i * 2 + 1] = Vec4{{ 0, 0, 0, 0.5f }};
		}
		dispatch_semaphore_t inFlight = dispatch_semaphore_create(3);
		std::atomic<double> gpuMsSum{0.0};
		std::atomic<int>    gpuCount{0};

		// One TLAS build from the given arrays; `ringBuf` non-nil → descriptors there.
		auto buildTlas = [&](const std::vector<uint32_t>& idx, const std::vector<Mat4>& xf,
		                     const std::vector<id<MTLAccelerationStructure>>& uniq,
		                     id<MTLBuffer> ringBuf, size_t ringOff) -> id<MTLAccelerationStructure>
		{
			const NSUInteger n = idx.size();
			id<MTLBuffer> ib = ringBuf ? ringBuf
				: [dev newBufferWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor) * n
				                   options:MTLResourceStorageModeShared];
			auto* inst = (MTLAccelerationStructureInstanceDescriptor*)((uint8_t*)ib.contents + ringOff);
			for (NSUInteger i = 0; i < n; ++i)
			{
				inst[i].accelerationStructureIndex     = idx[i];
				inst[i].options                         = MTLAccelerationStructureInstanceOptionOpaque;
				inst[i].mask                            = 0xFF;
				inst[i].intersectionFunctionTableOffset = 0;
				for (int r = 0; r < 3; ++r)
					for (int c = 0; c < 4; ++c)
						inst[i].transformationMatrix.columns[c][r] = xf[i].m[c * 4 + r];
			}
			NSMutableArray* arr = [NSMutableArray arrayWithCapacity:uniq.size()];
			for (id<MTLAccelerationStructure> b : uniq) [arr addObject:b];
			MTLInstanceAccelerationStructureDescriptor* d = [MTLInstanceAccelerationStructureDescriptor descriptor];
			d.instancedAccelerationStructures = arr;
			d.instanceCount                   = n;
			d.instanceDescriptorBuffer        = ib;
			d.instanceDescriptorBufferOffset  = ringOff;
			MTLAccelerationStructureSizes s = [dev accelerationStructureSizesWithDescriptor:d];
			id<MTLAccelerationStructure> tlas = [dev newAccelerationStructureWithSize:s.accelerationStructureSize];
			id<MTLBuffer> scratch = [dev newBufferWithLength:std::max<NSUInteger>(1, s.buildScratchBufferSize)
			                                         options:MTLResourceStorageModePrivate];
			dispatch_semaphore_wait(inFlight, DISPATCH_TIME_FOREVER);
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLAccelerationStructureCommandEncoder> e = [cb accelerationStructureCommandEncoder];
			for (id<MTLAccelerationStructure> b : uniq) [e useResource:b usage:MTLResourceUsageRead];
			[e buildAccelerationStructure:tlas descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
			[e endEncoding];
			[cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
				gpuMsSum = gpuMsSum + (c.GPUEndTime - c.GPUStartTime) * 1000.0;
				gpuCount = gpuCount + 1;
				dispatch_semaphore_signal(inFlight);
			}];
			[cb commit];
			return tlas;
		};

		struct Snap { std::vector<void*> blas; std::vector<uint32_t> idx; std::vector<Mat4> xf;
		              std::vector<Vec4> col; std::vector<int32_t> land; };
		auto drain = [&] { for (int i = 0; i < 3; ++i) dispatch_semaphore_wait(inFlight, DISPATCH_TIME_FOREVER);
		                   for (int i = 0; i < 3; ++i) dispatch_semaphore_signal(inFlight); };

		for (int mode = 0; mode < 3; ++mode)
		{
			const char* name = mode == 0 ? "old  " : mode == 1 ? "kept " : "moved";
			gpuMsSum = 0.0; gpuCount = 0;
			Snap last, cur;
			std::unordered_map<void*, uint32_t> slot;
			id<MTLAccelerationStructure> live = nil;
			id<MTLBuffer> ring = [dev newBufferWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor) * N * 3 + 768
			                                      options:MTLResourceStorageModeShared];
			std::vector<double> cpu;
			for (int f = 0; f < F + 10; ++f)
			@autoreleasepool {
				if (mode == 2) // every caster moves a little every frame
					for (int i = 0; i < N; ++i) xform[i].m[13] = 0.001f * f;
				auto t0 = Clock::now();
				cur.blas.clear(); cur.idx.clear(); cur.xf.clear(); cur.col.clear(); cur.land.clear();
				std::vector<id<MTLAccelerationStructure>> uniq;
				if (mode == 0)
				{
					for (int i = 0; i < N; ++i)
					{
						id<MTLAccelerationStructure> b = blas[meshOf[i]];
						int k = -1;
						for (size_t u = 0; u < uniq.size(); ++u) if (uniq[u] == b) { k = int(u); break; }
						if (k < 0) { k = int(uniq.size()); uniq.push_back(b); }
						cur.idx.push_back(uint32_t(k));
						cur.xf.push_back(xform[i]);
						cur.col.push_back(colorSrc[i * 2]); cur.col.push_back(colorSrc[i * 2 + 1]);
						cur.land.push_back(-1);
					}
					id<MTLBuffer> c  = [dev newBufferWithBytes:cur.col.data() length:cur.col.size() * sizeof(Vec4) options:MTLResourceStorageModeShared];
					id<MTLBuffer> l  = [dev newBufferWithBytes:cur.land.data() length:cur.land.size() * 4 options:MTLResourceStorageModeShared];
					id<MTLBuffer> mi = [dev newBufferWithBytes:cur.idx.data() length:cur.idx.size() * 4 options:MTLResourceStorageModeShared];
					std::vector<uint64_t> ptrs;
					for (size_t u = 0; u < uniq.size(); ++u) { ptrs.push_back(vbs[u].gpuAddress); ptrs.push_back(ibs[u].gpuAddress); }
					id<MTLBuffer> mp = [dev newBufferWithBytes:ptrs.data() length:ptrs.size() * 8 options:MTLResourceStorageModeShared];
					(void)c; (void)l; (void)mi; (void)mp;
					live = buildTlas(cur.idx, cur.xf, uniq, nil, 0);
				}
				else
				{
					slot.clear();
					for (int i = 0; i < N; ++i)
					{
						void* b = (__bridge void*)blas[meshOf[i]];
						auto [it, added] = slot.try_emplace(b, uint32_t(cur.blas.size()));
						if (added) cur.blas.push_back(b);
						cur.idx.push_back(it->second);
						cur.xf.push_back(xform[i]);
						cur.col.push_back(colorSrc[i * 2]); cur.col.push_back(colorSrc[i * 2 + 1]);
						cur.land.push_back(-1);
					}
					auto same = [](const auto& a, const auto& b)
					{ return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0; };
					const bool tables = same(cur.blas, last.blas) && same(cur.idx, last.idx)
					                 && same(cur.col, last.col) && same(cur.land, last.land);
					if (!(live && tables && same(cur.xf, last.xf)))
					{
						for (void* b : cur.blas) uniq.push_back((__bridge id<MTLAccelerationStructure>)b);
						const size_t off = (size_t(f % 3) * sizeof(MTLAccelerationStructureInstanceDescriptor) * N + 255) & ~size_t(255);
						live = buildTlas(cur.idx, cur.xf, uniq, ring, off);
						std::swap(last, cur);
					}
				}
				if (f >= 10) cpu.push_back(msSince(t0));
			}
			drain();
			std::sort(cpu.begin(), cpu.end());
			const double p50 = cpu[cpu.size() / 2], p95 = cpu[cpu.size() * 95 / 100];
			std::printf("TLAS %s  CPU/frame p50 %7.3f ms  p95 %7.3f ms  | TLAS builds %4d, GPU %6.3f ms each\n",
			            name, p50, p95, gpuCount.load(), gpuCount ? gpuMsSum / gpuCount : 0.0);
		}
	}
	return 0;
}
