// Coverage self test for alloc_probe.dylib: performs known allocations through
// the paths the engine uses and prints what the probe counted for each.
//   clang++ -std=c++17 -fobjc-arc -O0 -framework Foundation -framework Metal \
//       scripts/perf/alloc_probe_selftest.mm -o /tmp/alloc_probe_selftest
//   DYLD_INSERT_LIBRARIES=/tmp/alloc_probe.dylib /tmp/alloc_probe_selftest
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>

struct Counts { uint64_t allocs, bytes, frees, mainAllocs, mainBytes, mainFrees, fileOps, mainFileOps; };
using ReadFn = void (*)(Counts *);
static ReadFn g_read;

template <class F> static void probe(const char *what, int expected, F &&f) {
    Counts a{}, b{};
    g_read(&a);
    f();
    g_read(&b);
    std::printf("%-44s expected>=%-5d allocs=%-6llu file=%llu\n", what, expected,
                (unsigned long long)(b.mainAllocs - a.mainAllocs),
                (unsigned long long)(b.mainFileOps - a.mainFileOps));
}

int main() {
    g_read = (ReadFn)dlsym(RTLD_DEFAULT, "he_alloc_probe_read");
    if (!g_read) { std::puts("probe not loaded"); return 1; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    probe("1000x malloc(64)", 1000, [] { for (int i = 0; i < 1000; i++) free(std::malloc(64)); });
    probe("1000x new int[16]", 1000, [] { for (int i = 0; i < 1000; i++) delete[] new int[16]; });
    probe("1000x std::vector<int>(100)", 1000, [] { for (int i = 0; i < 1000; i++) { std::vector<int> v(100); (void)v; } });
    probe("1000x std::string(40 chars)", 1000, [] { for (int i = 0; i < 1000; i++) { std::string s(40, 'x'); (void)s; } });
    probe("1000x [NSObject new]", 1000, [] { @autoreleasepool { for (int i = 0; i < 1000; i++) { NSObject *o = [NSObject new]; (void)o; } } });
    probe("1000x NSString stringWithFormat", 1000, [] { @autoreleasepool { for (int i = 0; i < 1000; i++) { NSString *s = [NSString stringWithFormat:@"%d-long-enough-string-%d", i, i]; (void)s; } } });
    probe("100x MTLRenderPassDescriptor", 100, [] { @autoreleasepool { for (int i = 0; i < 100; i++) { MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor]; (void)d; } } });
    probe("100x commandBuffer+commit", 100, [&] { @autoreleasepool { for (int i = 0; i < 100; i++) { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; } } });
    probe("100x stat()", 0, [] { struct stat st; for (int i = 0; i < 100; i++) stat("/tmp", &st); });
    return 0;
}
