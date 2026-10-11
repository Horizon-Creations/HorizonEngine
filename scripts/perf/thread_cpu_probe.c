// thread_cpu_probe: per-thread CPU time of a running process, sampled from inside, with every thread
// tagged by the image that CREATED it. Pure measurement tool (Thema 178, step 1); nothing links against it.
//
//   clang -O2 -dynamiclib scripts/perf/thread_cpu_probe.c -o /tmp/he178/thread_cpu_probe.dylib
//   DYLD_INSERT_LIBRARIES=/tmp/he178/thread_cpu_probe.dylib HE_THREAD_PROBE_OUT=/tmp/x.tsv ./HorizonGame
//   python3 scripts/perf/thread_cpu_report.py /tmp/x.tsv --window 8
//
// Why the tag: Jolt names its workers only on Windows/Linux, so on macOS they are "(unnamed)" next to
// Metal's and SDL's own threads. Jolt's std::thread constructor is inline in libHorizonScene (Jolt is linked
// into it statically), so the return address of the pthread_create call lands in that image; the engine's
// ThreadPool ("Worker-N") is created from libHorizonCore. The tag is that image's name.
//
// Output (TSV, one row per thread per sample): t_ms  tid  tag  pthread-name  cpu_ms(user+system)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_T 512
static struct { uint64_t tid; char tag[48]; } g_tags[MAX_T];
static volatile int g_ntags;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE* g_out;
static double g_t0;
static uint64_t g_probeTid;

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

typedef struct { void* (*fn)(void*); void* arg; char tag[48]; } Wrap;

static void* trampoline(void* p) {
    Wrap w = *(Wrap*)p; free(p);
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    pthread_mutex_lock(&g_mu);
    if (g_ntags < MAX_T) { g_tags[g_ntags].tid = tid; strncpy(g_tags[g_ntags].tag, w.tag, 47); g_ntags++; }
    pthread_mutex_unlock(&g_mu);
    return w.fn(w.arg);
}

static int my_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg) {
    Wrap* w = (Wrap*)malloc(sizeof(Wrap));
    w->fn = fn; w->arg = arg; strcpy(w->tag, "?");
    Dl_info ci;
    if (dladdr(__builtin_return_address(0), &ci) && ci.dli_fname) {
        const char* b = strrchr(ci.dli_fname, '/'); b = b ? b + 1 : ci.dli_fname;
        if (!strncmp(b, "lib", 3)) b += 3;
        strncpy(w->tag, b, 47); w->tag[47] = 0;
        char* dot = strstr(w->tag, ".dylib"); if (dot) *dot = 0;
    }
    return pthread_create(t, a, trampoline, w);
}

typedef struct { const void* replacement; const void* replacee; } interpose_t;
__attribute__((used)) static const interpose_t g_interpose[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void*)my_pthread_create, (const void*)pthread_create },
};

static const char* tag_of(uint64_t tid, uint64_t mainTid) {
    if (tid == mainTid) return "main";
    const char* r = "(not created via pthread_create)";
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_ntags; ++i) if (g_tags[i].tid == tid) { r = g_tags[i].tag; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void sample(uint64_t mainTid) {
    thread_act_array_t ths; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &ths, &n) != KERN_SUCCESS) return;
    double t = now_ms() - g_t0;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        thread_identifier_info_data_t id; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
        thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
        thread_extended_info_data_t ei; mach_msg_type_number_t ec = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(ths[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &ic) == KERN_SUCCESS &&
            thread_info(ths[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS &&
            id.thread_id != g_probeTid) {
            const char* nm = thread_info(ths[i], THREAD_EXTENDED_INFO, (thread_info_t)&ei, &ec) == KERN_SUCCESS ? ei.pth_name : "";
            double cpu = (bi.user_time.seconds + bi.system_time.seconds) * 1e3 + (bi.user_time.microseconds + bi.system_time.microseconds) / 1e3;
            fprintf(g_out, "%.1f\t%llu\t%s\t%s\t%.2f\n", t, (unsigned long long)id.thread_id, tag_of(id.thread_id, mainTid), nm, cpu);
        }
        mach_port_deallocate(mach_task_self(), ths[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)ths, n * sizeof(thread_t));
    fflush(g_out);
}

static uint64_t g_mainTid;
static void* sampler(void* arg) {
    (void)arg;
    pthread_threadid_np(NULL, &g_probeTid);
    const char* ms = getenv("HE_THREAD_PROBE_MS");
    int period = ms ? atoi(ms) : 100;
    for (;;) { sample(g_mainTid); usleep(period * 1000); }
    return NULL;
}

__attribute__((constructor)) static void probe_init(void) {
    const char* path = getenv("HE_THREAD_PROBE_OUT");
    if (!path) return;
    g_out = fopen(path, "w");
    if (!g_out) return;
    g_t0 = now_ms();
    pthread_threadid_np(NULL, &g_mainTid);   // constructors run on the main thread
    pthread_t th; pthread_create(&th, NULL, sampler, NULL);   // original: calls from this image are not interposed
    pthread_detach(th);
}
