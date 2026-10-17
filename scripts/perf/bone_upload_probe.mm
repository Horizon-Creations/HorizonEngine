// CPU cost of one skinned object's bone-palette upload on Metal, before and
// after the upload-ring change (perf audit REN-302/402, Thema 216).
//
//   clang++ -std=c++17 -O2 -fobjc-arc -framework Foundation -framework Metal \
//       scripts/perf/bone_upload_probe.mm -o /tmp/bone_upload_probe
//   /tmp/bone_upload_probe [objects-per-frame=200] [bones=64] [frames=300]
//
// old:  128-matrix identity fill + copy of the pose + newBufferWithBytes (8 KB,
//       shared) per object, released at the end of the frame's autorelease pool
//       (EncodeSkinnedObjects before d4c1754b).
// ring: the pose plus identity up to the mesh's boneSlots written into a slice
//       of a long-lived 1 MB shared chunk at a 256-byte aligned offset
//       (MetalRenderer::UploadBonePalette). Chunk bookkeeping is not timed; it is
//       a vector push per MB.
// Only the CPU side of encoding is measured: no command buffer, no GPU work, so
// residency and the in-flight lifetime of the old per-object buffers (kept alive
// by the encoder until completion) are not in the number.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Mat4 { float m[16]; };
static const Mat4 kIdentity = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};

int main(int argc, char** argv)
{
	const int objects = argc > 1 ? std::atoi(argv[1]) : 200;
	const int bones   = argc > 2 ? std::atoi(argv[2]) : 64;
	const int frames  = argc > 3 ? std::atoi(argv[3]) : 300;
	constexpr int kMaxBones = 128;
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	if (!dev) { std::puts("no Metal device"); return 1; }

	std::vector<Mat4> pose(bones);
	for (int i = 0; i < bones; ++i) { pose[i] = kIdentity; pose[i].m[12] = float(i); }

	using clk = std::chrono::steady_clock;
	auto perObjectUs = [&](auto&& frame) {
		for (int w = 0; w < 10; ++w) frame();            // warm-up
		const auto t0 = clk::now();
		for (int f = 0; f < frames; ++f) frame();
		const double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
		return us / (double(frames) * objects);
	};

	std::vector<Mat4> scratch(kMaxBones, kIdentity);
	const double oldUs = perObjectUs([&] {
		@autoreleasepool {
			NSMutableArray* keep = [NSMutableArray arrayWithCapacity:objects]; // the encoder's retain
			for (int o = 0; o < objects; ++o)
			{
				std::fill(scratch.begin(), scratch.end(), kIdentity);
				std::copy_n(pose.begin(), std::min(bones, kMaxBones), scratch.begin());
				id<MTLBuffer> b = [dev newBufferWithBytes:scratch.data() length:kMaxBones * sizeof(Mat4)
				                                  options:MTLResourceStorageModeShared];
				[keep addObject:b];
			}
		}
	});

	const size_t chunkSize = size_t(1) << 20;
	std::vector<id<MTLBuffer>> chunks;
	const int slots = std::min(std::max(bones, 1), kMaxBones);
	const double ringUs = perObjectUs([&] {
		size_t chunk = 0, used = 0;
		for (int o = 0; o < objects; ++o)
		{
			const size_t len = slots * sizeof(Mat4);
			size_t at = (used + 255) / 256 * 256;
			if (chunk >= chunks.size() || at + len > chunkSize)
			{
				if (chunk < chunks.size()) ++chunk;
				if (chunk >= chunks.size())
					chunks.push_back([dev newBufferWithLength:chunkSize options:MTLResourceStorageModeShared]);
				at = 0;
			}
			auto* dst = reinterpret_cast<Mat4*>(static_cast<uint8_t*>(chunks[chunk].contents) + at);
			std::copy_n(pose.begin(), std::min(bones, kMaxBones), dst);
			std::fill(dst + std::min(bones, kMaxBones), dst + slots, kIdentity);
			used = at + len;
		}
	});

	std::printf("objects/frame=%d bones=%d frames=%d\n", objects, bones, frames);
	std::printf("old  (fill + newBufferWithBytes 8 KB): %.3f us/object, %.3f ms/frame\n", oldUs, oldUs * objects / 1000.0);
	std::printf("ring (slice write, %d matrices):       %.3f us/object, %.3f ms/frame\n", slots, ringUs, ringUs * objects / 1000.0);
	std::printf("ring chunks used: %zu x 1 MB\n", chunks.size());
	return 0;
}
