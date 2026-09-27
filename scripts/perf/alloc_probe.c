// alloc_probe: counts heap allocations and file-metadata syscalls of a process,
// split into main thread vs. other threads, and optionally records the call
// stacks of main-thread allocations. Pure measurement tool for the perf audit
// (Thema 99, Schritt 3); nothing in the engine links against it.
//
//   clang -O2 -dynamiclib scripts/perf/alloc_probe.c -o /tmp/alloc_probe.dylib
//   DYLD_INSERT_LIBRARIES=/tmp/alloc_probe.dylib HE_ALLOC_PROBE_OUT=/tmp/x.txt ./HorizonEditor
//
// The engine side (EngineProfiler) looks the three he_alloc_probe_* functions up
// with dlsym(RTLD_DEFAULT, ...). Without the dylib nothing changes.
//
// Coverage: malloc/calloc/realloc/valloc/posix_memalign/aligned_alloc, the typed
// malloc_type_* entry points newer SDKs emit, malloc_zone_*, and operator new
// (libc++abi). Calls from the dyld shared cache (Metal, Foundation, libobjc) are
// covered as far as dyld applies interposing there; check with the self test
// (alloc_probe_selftest.m) before trusting absolute numbers.
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint64_t allocs, bytes, frees;
    uint64_t mainAllocs, mainBytes, mainFrees;
    uint64_t fileOps, mainFileOps;
} he_alloc_probe_counts;

static _Atomic uint64_t g_allocs, g_bytes, g_frees, g_mAllocs, g_mBytes, g_mFrees, g_file, g_mFile;
static _Atomic int g_armed;
// Reentrancy depth per thread (>0 while inside our own wrappers: new -> malloc,
// backtrace, dump). pthread TSD instead of __thread: a dylib's first TLV access
// mallocs, which would recurse into the wrapper.
static pthread_key_t g_key;
static int g_keyOk;
static inline int depth(void) { return g_keyOk ? (int)(intptr_t)pthread_getspecific(g_key) : 1; }
static inline void depthAdd(int d) {
    if (g_keyOk) pthread_setspecific(g_key, (void *)(intptr_t)(depth() + d));
}
__attribute__((constructor)) static void probeInit(void) {
    if (pthread_key_create(&g_key, NULL) == 0) g_keyOk = 1;
}

// ---- stack table (main thread only, while armed) --------------------------
#define FR 18
#define SKIP 2
#define TBITS 15
#define TSIZE (1u << TBITS)
typedef struct {
    uint64_t hash, count, bytes;
    int kind; // 0 alloc, 1 file op
    int n;
    void *pc[FR];
} Entry;
static Entry g_tab[TSIZE];
static _Atomic uint64_t g_dropped;
static pthread_mutex_t g_tabLock = PTHREAD_MUTEX_INITIALIZER;

static void record(int kind, size_t sz) {
    void *buf[FR + SKIP];
    depthAdd(1);
    int n = backtrace(buf, FR + SKIP);
    depthAdd(-1);
    if (n <= SKIP) return;
    void **pc = buf + SKIP;
    n -= SKIP;
    uint64_t h = 1469598103934665603ull ^ (uint64_t)kind;
    for (int i = 0; i < n; i++) { h ^= (uint64_t)pc[i]; h *= 1099511628211ull; }
    if (!h) h = 1;
    pthread_mutex_lock(&g_tabLock);
    uint32_t i = (uint32_t)(h >> 17) & (TSIZE - 1);
    for (uint32_t probe = 0; probe < 64; probe++, i = (i + 1) & (TSIZE - 1)) {
        Entry *e = &g_tab[i];
        if (e->hash == h) { e->count++; e->bytes += sz; pthread_mutex_unlock(&g_tabLock); return; }
        if (!e->hash) {
            e->hash = h; e->count = 1; e->bytes = sz; e->kind = kind; e->n = n;
            memcpy(e->pc, pc, sizeof(void *) * (size_t)n);
            pthread_mutex_unlock(&g_tabLock);
            return;
        }
    }
    pthread_mutex_unlock(&g_tabLock);
    atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
}

static inline void onAlloc(size_t sz) {
    if (depth()) return;
    atomic_fetch_add_explicit(&g_allocs, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bytes, sz, memory_order_relaxed);
    if (pthread_main_np()) {
        atomic_fetch_add_explicit(&g_mAllocs, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_mBytes, sz, memory_order_relaxed);
        if (atomic_load_explicit(&g_armed, memory_order_relaxed)) record(0, sz);
    }
}
static inline void onFree(void *p) {
    if (!p || depth()) return;
    atomic_fetch_add_explicit(&g_frees, 1, memory_order_relaxed);
    if (pthread_main_np()) atomic_fetch_add_explicit(&g_mFrees, 1, memory_order_relaxed);
}
static inline void onFile(void) {
    if (depth()) return;
    atomic_fetch_add_explicit(&g_file, 1, memory_order_relaxed);
    if (pthread_main_np()) {
        atomic_fetch_add_explicit(&g_mFile, 1, memory_order_relaxed);
        if (atomic_load_explicit(&g_armed, memory_order_relaxed)) record(1, 0);
    }
}

// ---- wrappers ---------------------------------------------------------------
// Only the outermost call counts: the real function runs with depth() > 0, so a
// calloc that internally goes through malloc_zone_calloc is one allocation.
#define ALLOC(T, sz, call) { onAlloc(sz); depthAdd(1); T r_ = call; depthAdd(-1); return r_; }
#define FREE(call) { onFree(p); depthAdd(1); call; depthAdd(-1); }
static void *w_malloc(size_t s) ALLOC(void *, s, malloc(s))
static void *w_calloc(size_t c, size_t s) ALLOC(void *, c * s, calloc(c, s))
static void *w_realloc(void *p, size_t s) ALLOC(void *, s, realloc(p, s))
static void *w_valloc(size_t s) ALLOC(void *, s, valloc(s))
static int w_posix_memalign(void **o, size_t a, size_t s) ALLOC(int, s, posix_memalign(o, a, s))
static void *w_aligned_alloc(size_t a, size_t s) ALLOC(void *, s, aligned_alloc(a, s))
static void w_free(void *p) FREE(free(p))

static void *w_zmalloc(malloc_zone_t *z, size_t s) ALLOC(void *, s, malloc_zone_malloc(z, s))
static void *w_zcalloc(malloc_zone_t *z, size_t c, size_t s) ALLOC(void *, c * s, malloc_zone_calloc(z, c, s))
static void *w_zrealloc(malloc_zone_t *z, void *p, size_t s) ALLOC(void *, s, malloc_zone_realloc(z, p, s))
static void *w_zmemalign(malloc_zone_t *z, size_t a, size_t s) ALLOC(void *, s, malloc_zone_memalign(z, a, s))
static void w_zfree(malloc_zone_t *z, void *p) FREE(malloc_zone_free(z, p))

// Typed allocation entry points (macOS 15+ SDKs rewrite malloc() calls to these).
typedef unsigned long long he_mtid;
extern void *malloc_type_malloc(size_t, he_mtid) __attribute__((weak_import));
extern void *malloc_type_calloc(size_t, size_t, he_mtid) __attribute__((weak_import));
extern void *malloc_type_realloc(void *, size_t, he_mtid) __attribute__((weak_import));
extern void *malloc_type_aligned_alloc(size_t, size_t, he_mtid) __attribute__((weak_import));
extern int malloc_type_posix_memalign(void **, size_t, size_t, he_mtid) __attribute__((weak_import));
extern void *malloc_type_zone_malloc(malloc_zone_t *, size_t, he_mtid) __attribute__((weak_import));
extern void *malloc_type_zone_calloc(malloc_zone_t *, size_t, size_t, he_mtid) __attribute__((weak_import));
extern void *malloc_type_zone_realloc(malloc_zone_t *, void *, size_t, he_mtid) __attribute__((weak_import));
static void *w_tmalloc(size_t s, he_mtid t) ALLOC(void *, s, malloc_type_malloc(s, t))
static void *w_tcalloc(size_t c, size_t s, he_mtid t) ALLOC(void *, c * s, malloc_type_calloc(c, s, t))
static void *w_trealloc(void *p, size_t s, he_mtid t) ALLOC(void *, s, malloc_type_realloc(p, s, t))
static void *w_taligned(size_t a, size_t s, he_mtid t) ALLOC(void *, s, malloc_type_aligned_alloc(a, s, t))
static int w_tposix(void **o, size_t a, size_t s, he_mtid t) ALLOC(int, s, malloc_type_posix_memalign(o, a, s, t))
static void *w_tzmalloc(malloc_zone_t *z, size_t s, he_mtid t) ALLOC(void *, s, malloc_type_zone_malloc(z, s, t))
static void *w_tzcalloc(malloc_zone_t *z, size_t c, size_t s, he_mtid t) ALLOC(void *, c * s, malloc_type_zone_calloc(z, c, s, t))
static void *w_tzrealloc(malloc_zone_t *z, void *p, size_t s, he_mtid t) ALLOC(void *, s, malloc_type_zone_realloc(z, p, s, t))

// operator new / delete (Itanium mangling), counted once: the malloc they call
// internally runs with depth() > 0.
extern void *_Znwm(size_t);
extern void *_Znam(size_t);
extern void *_ZnwmSt11align_val_t(size_t, size_t);
extern void _ZdlPv(void *);
extern void _ZdaPv(void *);
static void *w_new(size_t s) { onAlloc(s); depthAdd(1); void *p = _Znwm(s); depthAdd(-1); return p; }
static void *w_newa(size_t s) { onAlloc(s); depthAdd(1); void *p = _Znam(s); depthAdd(-1); return p; }
static void *w_newal(size_t s, size_t a) { onAlloc(s); depthAdd(1); void *p = _ZnwmSt11align_val_t(s, a); depthAdd(-1); return p; }
static void w_del(void *p) { onFree(p); depthAdd(1); _ZdlPv(p); depthAdd(-1); }
static void w_dela(void *p) { onFree(p); depthAdd(1); _ZdaPv(p); depthAdd(-1); }

// File metadata / open calls (std::filesystem, fopen, directory scans).
extern int stat(const char *, struct stat *);
extern int lstat(const char *, struct stat *);
static int w_stat(const char *p, struct stat *b) { onFile(); return stat(p, b); }
static int w_lstat(const char *p, struct stat *b) { onFile(); return lstat(p, b); }
static int w_access(const char *p, int m) { onFile(); return access(p, m); }
static int w_open(const char *p, int f, ...) {
    int mode = 0;
    if (f & O_CREAT) { va_list ap; va_start(ap, f); mode = va_arg(ap, int); va_end(ap); }
    onFile();
    return open(p, f, mode);
}
static FILE *w_fopen(const char *p, const char *m) { onFile(); return fopen(p, m); }

#define INTERPOSE(repl, orig) \
    __attribute__((used)) static struct { const void *r; const void *o; } _ip_##repl \
        __attribute__((section("__DATA,__interpose"))) = { (const void *)(repl), (const void *)(orig) };

INTERPOSE(w_malloc, malloc)
INTERPOSE(w_calloc, calloc)
INTERPOSE(w_realloc, realloc)
INTERPOSE(w_valloc, valloc)
INTERPOSE(w_posix_memalign, posix_memalign)
INTERPOSE(w_aligned_alloc, aligned_alloc)
INTERPOSE(w_free, free)
INTERPOSE(w_zmalloc, malloc_zone_malloc)
INTERPOSE(w_zcalloc, malloc_zone_calloc)
INTERPOSE(w_zrealloc, malloc_zone_realloc)
INTERPOSE(w_zmemalign, malloc_zone_memalign)
INTERPOSE(w_zfree, malloc_zone_free)
INTERPOSE(w_tmalloc, malloc_type_malloc)
INTERPOSE(w_tcalloc, malloc_type_calloc)
INTERPOSE(w_trealloc, malloc_type_realloc)
INTERPOSE(w_taligned, malloc_type_aligned_alloc)
INTERPOSE(w_tposix, malloc_type_posix_memalign)
INTERPOSE(w_tzmalloc, malloc_type_zone_malloc)
INTERPOSE(w_tzcalloc, malloc_type_zone_calloc)
INTERPOSE(w_tzrealloc, malloc_type_zone_realloc)
INTERPOSE(w_new, _Znwm)
INTERPOSE(w_newa, _Znam)
INTERPOSE(w_newal, _ZnwmSt11align_val_t)
INTERPOSE(w_del, _ZdlPv)
INTERPOSE(w_dela, _ZdaPv)
INTERPOSE(w_stat, stat)
INTERPOSE(w_lstat, lstat)
INTERPOSE(w_access, access)
INTERPOSE(w_open, open)
INTERPOSE(w_fopen, fopen)

// ---- API for the engine (looked up via dlsym) --------------------------------
__attribute__((visibility("default"))) void he_alloc_probe_read(he_alloc_probe_counts *o) {
    o->allocs = atomic_load(&g_allocs); o->bytes = atomic_load(&g_bytes); o->frees = atomic_load(&g_frees);
    o->mainAllocs = atomic_load(&g_mAllocs); o->mainBytes = atomic_load(&g_mBytes);
    o->mainFrees = atomic_load(&g_mFrees);
    o->fileOps = atomic_load(&g_file); o->mainFileOps = atomic_load(&g_mFile);
}

__attribute__((visibility("default"))) void he_alloc_probe_arm(int on) {
    if (on && !getenv("HE_ALLOC_PROBE_STACKS")) return; // stacks only on request (slow)
    atomic_store(&g_armed, on);
}

static int byCount(const void *a, const void *b) {
    const Entry *x = *(const Entry *const *)a, *y = *(const Entry *const *)b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

// Writes the recorded main-thread stacks, most frequent first. Each frame is
// printed as "image!symbol+off" plus "@image 0xload 0xpc" so atos can resolve
// non-exported symbols afterwards (scripts/perf/alloc_probe_report.py).
__attribute__((visibility("default"))) void he_alloc_probe_dump(const char *path, uint64_t frames) {
    depthAdd(1);
    atomic_store(&g_armed, 0);
    const char *p = path ? path : getenv("HE_ALLOC_PROBE_OUT");
    if (!p) { depthAdd(-1); return; }
    FILE *f = fopen(p, "w");
    if (!f) { depthAdd(-1); return; }
    static Entry *list[TSIZE];
    uint32_t n = 0;
    uint64_t tot[2] = {0, 0};
    for (uint32_t i = 0; i < TSIZE; i++)
        if (g_tab[i].hash) { list[n++] = &g_tab[i]; tot[g_tab[i].kind] += g_tab[i].count; }
    qsort(list, n, sizeof(list[0]), byCount);
    fprintf(f, "# alloc_probe stacks: frames=%llu stacks=%u mainAllocs=%llu mainFileOps=%llu dropped=%llu\n",
            (unsigned long long)frames, n, (unsigned long long)tot[0], (unsigned long long)tot[1],
            (unsigned long long)atomic_load(&g_dropped));
    for (uint32_t k = 0; k < n; k++) {
        Entry *e = list[k];
        fprintf(f, "STACK kind=%s count=%llu bytes=%llu\n", e->kind ? "file" : "alloc",
                (unsigned long long)e->count, (unsigned long long)e->bytes);
        for (int i = 0; i < e->n; i++) {
            Dl_info di;
            if (dladdr(e->pc[i], &di) && di.dli_fname) {
                const char *img = strrchr(di.dli_fname, '/');
                img = img ? img + 1 : di.dli_fname;
                fprintf(f, "  %s!%s+%lu @%s 0x%lx 0x%lx\n", img, di.dli_sname ? di.dli_sname : "?",
                        (unsigned long)((char *)e->pc[i] - (char *)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)),
                        di.dli_fname, (unsigned long)di.dli_fbase, (unsigned long)e->pc[i]);
            } else {
                fprintf(f, "  ?!? @? 0x0 0x%lx\n", (unsigned long)e->pc[i]);
            }
        }
    }
    fclose(f);
    depthAdd(-1);
}
