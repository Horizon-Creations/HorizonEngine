// Minimal Metal presenter: a control measurement for Metal::NextDrawable.
//
// Perf-Audit Thema 99, Schritt 2. It reproduces the engine's presentation path
// (CAMetalLayer with the same settings as MetalRenderer::CreateTarget, one
// command buffer per frame, an offscreen "scene" pass at viewport size, then a
// late nextDrawable, a fullscreen pass into the drawable, presentDrawable,
// commit) WITHOUT any of the engine's work. If this probe blocks in
// nextDrawable as long as the editor does, the wait is not caused by the
// engine's workload or layer setup.
//
//   clang -fobjc-arc -O2 -framework AppKit -framework Metal -framework QuartzCore \
//         scripts/perf/metal_present_probe.m -o /tmp/metal_present_probe
//   /tmp/metal_present_probe --load 0 --vsync 0 --label a-clear > out.json
//
// Options (defaults = baseline conditions):
//   --w/--h PX          drawable size in pixels (2840x1528, the editor window)
//   --sw/--sh PX        offscreen scene size (1718x884, the editor viewport)
//   --load N            fragment loop iterations per scene pixel (0 = clear only)
//   --vsync 0|1         layer.displaySyncEnabled
//   --fbonly 0|1        layer.framebufferOnly (engine: 0)
//   --drawables 2|3     layer.maximumDrawableCount (engine: default 3)
//   --warmup N --frames N
//   --calibrate         commit+wait each frame, prints the unshared GPU time of --load
//   --label S           copied into the JSON
// Prints one JSON object with per-frame arrays and a summary to stdout.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mach/mach_time.h>
#include <stdatomic.h>

static const char* kShader =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct V { float4 pos [[position]]; float2 uv; };\n"
"vertex V vs(uint vid [[vertex_id]]) {\n"
"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
"  V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
"fragment half4 fsLoad(V in [[stage_in]], constant uint& n [[buffer(0)]]) {\n"
"  float3 a = float3(in.uv, 0.5);\n"
"  for (uint i = 0; i < n; ++i) { a = fract(sin(a * 12.9898 + a.yzx * 78.233) * 43758.5453); }\n"
"  return half4(half3(a), 1); }\n"
"fragment half4 fsBlit(V in [[stage_in]], texture2d<half> t [[texture(0)]]) {\n"
"  constexpr sampler s(filter::linear); return t.sample(s, in.uv); }\n";

static double nowMs(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) mach_timebase_info(&tb);
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1.0e6;
}

static int cmpd(const void* a, const void* b)
{
	double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y;
}

static double pct(const double* v, int n, double p)
{
	if (n <= 0) return 0;
	double* s = malloc(sizeof(double) * n);
	memcpy(s, v, sizeof(double) * n);
	qsort(s, n, sizeof(double), cmpd);
	int i = (int)(p / 100.0 * (n - 1) + 0.5);
	double r = s[i];
	free(s);
	return r;
}

static void printArr(const char* name, const double* v, int n, BOOL comma)
{
	printf("  \"%s\": [", name);
	for (int i = 0; i < n; ++i) printf("%s%.3f", i ? "," : "", v[i]);
	printf("]%s\n", comma ? "," : "");
}

int main(int argc, const char** argv)
{
	@autoreleasepool
	{
		int w = 2840, h = 1528, sw = 1718, sh = 884, warmup = 300, frames = 600, drawables = 3;
		unsigned load = 0;
		BOOL vsync = NO, fbonly = NO, calibrate = NO;
		const char* label = "";
		for (int i = 1; i < argc; ++i)
		{
			const char* a = argv[i];
			const char* v = i + 1 < argc ? argv[i + 1] : "0";
			if      (!strcmp(a, "--w"))         { w = atoi(v); ++i; }
			else if (!strcmp(a, "--h"))         { h = atoi(v); ++i; }
			else if (!strcmp(a, "--sw"))        { sw = atoi(v); ++i; }
			else if (!strcmp(a, "--sh"))        { sh = atoi(v); ++i; }
			else if (!strcmp(a, "--load"))      { load = (unsigned)atoi(v); ++i; }
			else if (!strcmp(a, "--vsync"))     { vsync = atoi(v) != 0; ++i; }
			else if (!strcmp(a, "--fbonly"))    { fbonly = atoi(v) != 0; ++i; }
			else if (!strcmp(a, "--drawables")) { drawables = atoi(v); ++i; }
			else if (!strcmp(a, "--warmup"))    { warmup = atoi(v); ++i; }
			else if (!strcmp(a, "--frames"))    { frames = atoi(v); ++i; }
			else if (!strcmp(a, "--label"))     { label = v; ++i; }
			else if (!strcmp(a, "--calibrate")) { calibrate = YES; }
		}

		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		[NSApp finishLaunching];

		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		id<MTLCommandQueue> queue = [dev newCommandQueue];
		NSError* err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:@(kShader) options:nil error:&err];
		if (!lib) { fprintf(stderr, "shader: %s\n", err.localizedDescription.UTF8String); return 1; }

		MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"vs"];
		pd.fragmentFunction = [lib newFunctionWithName:@"fsLoad"];
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;   // engine: kSceneColorFormat
		id<MTLRenderPipelineState> loadPso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
		pd.fragmentFunction = [lib newFunctionWithName:@"fsBlit"];
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		id<MTLRenderPipelineState> blitPso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!loadPso || !blitPso) { fprintf(stderr, "pso: %s\n", err.localizedDescription.UTF8String); return 1; }

		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
		                                                                              width:sw height:sh mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		id<MTLTexture> scene = [dev newTextureWithDescriptor:td];

		NSScreen* screen = [NSScreen mainScreen];
		CGFloat scale = screen.backingScaleFactor;
		NSRect frame = NSMakeRect(0, 0, w / scale, h / scale);
		NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
		                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
		                                              backing:NSBackingStoreBuffered defer:NO];
		win.title = @"metal_present_probe";
		NSView* view = win.contentView;
		view.wantsLayer = YES;
		CAMetalLayer* layer = [CAMetalLayer layer];
		layer.device = dev;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;   // engine: kSwapchainFormat
		layer.framebufferOnly = fbonly;
		layer.opaque = YES;
		layer.displaySyncEnabled = vsync;
		layer.maximumDrawableCount = (NSUInteger)drawables;
		layer.contentsScale = scale;
		layer.drawableSize = CGSizeMake(w, h);
		view.layer = layer;
		[win center];
		[win makeKeyAndOrderFront:nil];
		[NSApp activateIgnoringOtherApps:YES];

		const int total = warmup + frames;
		double* delta = calloc(total, sizeof(double));
		double* nd    = calloc(total, sizeof(double));
		double* cpu   = calloc(total, sizeof(double));
		double* gpu   = calloc(total, sizeof(double));
		__block double* gpuOut = gpu;
		__block atomic_int completed = 0;

		double prevStart = nowMs();
		for (int f = 0; f < total; ++f)
		{
			@autoreleasepool
			{
				NSEvent* ev;
				while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
				                                   inMode:NSDefaultRunLoopMode dequeue:YES]))
					[NSApp sendEvent:ev];

				double t0 = nowMs();
				delta[f] = t0 - prevStart;
				prevStart = t0;

				id<MTLCommandBuffer> cb = [queue commandBuffer];
				MTLRenderPassDescriptor* sp = [MTLRenderPassDescriptor renderPassDescriptor];
				sp.colorAttachments[0].texture = scene;
				sp.colorAttachments[0].loadAction = MTLLoadActionClear;
				sp.colorAttachments[0].storeAction = MTLStoreActionStore;
				sp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.3, 0.5, 1);
				id<MTLRenderCommandEncoder> se = [cb renderCommandEncoderWithDescriptor:sp];
				if (load > 0)
				{
					[se setRenderPipelineState:loadPso];
					[se setFragmentBytes:&load length:sizeof(load) atIndex:0];
					[se drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				}
				[se endEncoding];

				double n0 = nowMs();
				id<CAMetalDrawable> drawable = calibrate ? nil : [layer nextDrawable];
				nd[f] = nowMs() - n0;
				if (drawable)
				{
					MTLRenderPassDescriptor* pp = [MTLRenderPassDescriptor renderPassDescriptor];
					pp.colorAttachments[0].texture = drawable.texture;
					pp.colorAttachments[0].loadAction = MTLLoadActionClear;
					pp.colorAttachments[0].storeAction = MTLStoreActionStore;
					id<MTLRenderCommandEncoder> pe = [cb renderCommandEncoderWithDescriptor:pp];
					[pe setRenderPipelineState:blitPso];
					[pe setFragmentTexture:scene atIndex:0];
					[pe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
					[pe endEncoding];
					[cb presentDrawable:drawable];
				}
				const int idx = f;
				[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
					if (done.GPUEndTime > done.GPUStartTime)
						gpuOut[idx] = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
					atomic_fetch_add(&completed, 1);
				}];
				[cb commit];
				if (calibrate) [cb waitUntilCompleted];
				cpu[f] = nowMs() - t0;
			}
		}
		while (atomic_load(&completed) < total) usleep(1000);

		const double* D = delta + warmup; const double* N = nd + warmup;
		const double* C = cpu + warmup;   const double* G = gpu + warmup;
		double sumD = 0; int ndFast = 0; int hist[7] = {0};
		for (int i = 0; i < frames; ++i)
		{
			sumD += D[i];
			if (N[i] < 1.0) ++ndFast;
			int k = (int)(D[i] / (1000.0 / 60.0) + 0.5); if (k > 6) k = 6; hist[k]++;
		}
		printf("{\n  \"label\": \"%s\", \"device\": \"%s\",\n", label, dev.name.UTF8String);
		printf("  \"config\": {\"w\": %d, \"h\": %d, \"sw\": %d, \"sh\": %d, \"load\": %u, \"vsync\": %d, "
		       "\"framebufferOnly\": %d, \"maximumDrawableCount\": %lu, \"calibrate\": %d, \"warmup\": %d, \"frames\": %d},\n",
		       w, h, sw, sh, load, vsync, fbonly, (unsigned long)layer.maximumDrawableCount, calibrate, warmup, frames);
		printf("  \"summary\": {\"fps\": %.2f, \"delta_p50\": %.2f, \"delta_p95\": %.2f, \"nd_p50\": %.2f, \"nd_p90\": %.2f, "
		       "\"nd_under_1ms\": %d, \"cpu_p50\": %.2f, \"gpu_min\": %.2f, \"gpu_p50\": %.2f, \"gpu_p90\": %.2f, "
		       "\"delta_in_vsync_intervals\": [%d,%d,%d,%d,%d,%d,%d]},\n",
		       1000.0 * frames / sumD, pct(D, frames, 50), pct(D, frames, 95), pct(N, frames, 50), pct(N, frames, 90),
		       ndFast, pct(C, frames, 50), pct(G, frames, 0), pct(G, frames, 50), pct(G, frames, 90),
		       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6]);
		printArr("deltaMs", D, frames, YES);
		printArr("nextDrawableMs", N, frames, YES);
		printArr("gpuSpanMs", G, frames, NO);
		printf("}\n");
	}
	return 0;
}
