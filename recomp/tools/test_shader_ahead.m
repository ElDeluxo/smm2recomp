// Shader compile-ahead, alone (no game): scan a RomFS folder for the game's shaders (runtime/nvn/shader_ahead.c),
// translate each as the Metal compile-ahead does and, unless -n, compile the sources
// with Metal the way the renderer does. Measures the scan, the translation and the compiles (cold with a new -s salt;
// run it twice with the same salt for Metal's own cache).
//   build/test_shader_ahead [-n] [-j jobs] [-s salt] [-l limit] [-q utility|background|default] <romfs folder>
// make build/test_shader_ahead
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../runtime/nvn/maxwell/shader.h"
#include "../runtime/nvn/shader_ahead.h"

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static uint64_t fnv64(const char *t) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *t; t++) { h ^= (uint8_t)*t; h *= 0x100000001B3ull; }
    return h;
}

static id<MTLDevice> dev;
static MTLCompileOptions *vopts;
static dispatch_semaphore_t slots;
static dispatch_group_t group;
static NSMutableSet *seen;
static NSString *salt;
static bool compile = true;
static int limit = 1 << 30, nprog, nfail, nsrc;
static double t_translate, t_compile_sum, t_compile_max;
static size_t msl_bytes;
static int hist[8];  // compile ms: <5, <20, <50, <100, <200, <500, <1000, more
static NSLock *lock;

static NSCountedSet *per_file;  // (AHEAD_PER_FILE=1: new sources per RomFS file)
static const char *cur_archive;
static void compile_src(char kind, const char *msl) {
    uint64_t h = fnv64(msl);
    NSNumber *key = @(h ^ (uint64_t)kind);
    if ([seen containsObject:key]) return;
    [seen addObject:key];
    nsrc++;
    if (getenv("AHEAD_PRINT")) printf("%c_%016llx %s\n", kind, (unsigned long long)h, cur_archive);
    if (per_file) {
        NSString *a = @(cur_archive);
        NSRange r = [a rangeOfString:@":"];
        [per_file addObject:r.location == NSNotFound ? a : [a substringToIndex:r.location]];
    }
    msl_bytes += strlen(msl);
    if (!compile) return;
    NSString *name = [NSString stringWithFormat:@"%c_%016llx", kind, (unsigned long long)h];
    NSString *src = [@(msl) stringByReplacingOccurrencesOfString:@" main0(" withString:[NSString stringWithFormat:@" %@(", name]];
    if (salt.length) src = [src stringByAppendingString:salt];
    dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
    dispatch_group_enter(group);
    double t0 = now_ms();
    [dev newLibraryWithSource:src options:kind == 'v' ? vopts : nil completionHandler:^(id<MTLLibrary> l, NSError *e) {
        double dt = now_ms() - t0;
        [lock lock];
        t_compile_sum += dt;
        if (dt > t_compile_max) t_compile_max = dt;
        int b = dt < 5 ? 0 : dt < 20 ? 1 : dt < 50 ? 2 : dt < 100 ? 3 : dt < 200 ? 4 : dt < 500 ? 5 : dt < 1000 ? 6 : 7;
        hist[b]++;
        if (!l) { nfail++; if (nfail < 4) fprintf(stderr, "compile failed: %s\n", e.localizedDescription.UTF8String); }
        [lock unlock];
        dispatch_semaphore_signal(slots);
        dispatch_group_leave(group);
    }];
}

static ShaderInfo *translate_one(const AheadShader *p, char *kind, ShaderInfo **extra) {
    ShaderKey vkey = {0}, fkey = {0};
    vkey.flip_y = fkey.flip_y = getenv("AHEAD_NO_FLIP") == NULL;
    vkey.depth_minus_one = getenv("AHEAD_DEPTH_M1") != NULL;
    *extra = NULL;
    if (p->stage == 0) { *kind = 'v'; return maxwell_translate(p->code[0], p->cmax[0], STAGE_VERTEX, &vkey); }
    if (p->stage == 1) {
        *kind = 'f';
        fkey.vs_out_mask = 0xFFFFFFFFu;  // (as if the vertex shader wrote every input it reads)
        return maxwell_translate(p->code[1], p->cmax[1], STAGE_FRAGMENT, &fkey);
    }
    ShaderInfo *vi, *fi, *gi;
    *kind = 'v';
    if (maxwell_translate_program_msl(p->code, p->cmax, &vkey, &fkey, &vi, &fi, &gi)) {
        if (fi) maxwell_free(fi);
        if (gi) maxwell_free(gi);
        return vi ? (vi->msl ? (free(vi->msl), vi->msl = NULL, vi) : vi) : NULL;
    }
    *extra = fi;
    if (gi) maxwell_free(gi);
    return vi;
}

static bool on_program(const AheadShader *p, void *ud) {
    (void)ud;
    if (nprog >= limit) return false;
    nprog++;
    cur_archive = p->archive;
    char kind;
    ShaderInfo *extra;
    double t0 = now_ms();
    ShaderInfo *si = translate_one(p, &kind, &extra);
    t_translate += now_ms() - t0;
    if (!si || !si->msl) { if (nfail++ < 8 || getenv("AHEAD_VERBOSE")) fprintf(stderr, "%s: %s\n", p->archive, si ? si->error : "?"); }
    else { compile_src(kind, si->msl); if (extra && extra->msl) compile_src('f', extra->msl); }
    if (si) maxwell_free(si);
    if (extra) maxwell_free(extra);
    return true;
}

int main(int argc, char **argv) {
    int jobs = 2, opt;
    const char *qos = "utility";
    salt = @"";
    while ((opt = getopt(argc, argv, "nj:s:l:q:")) != -1) {
        if (opt == 'n') compile = false;
        else if (opt == 'j') jobs = atoi(optarg);
        else if (opt == 's') salt = [NSString stringWithFormat:@"\n// %s\n", optarg];
        else if (opt == 'l') limit = atoi(optarg);
        else if (opt == 'q') qos = optarg;
    }
    if (optind >= argc) { fprintf(stderr, "usage: %s [-n] [-j jobs] [-s salt] [-l limit] [-q qos] <romfs>\n", argv[0]); return 2; }
    dev = MTLCreateSystemDefaultDevice();
    vopts = [MTLCompileOptions new];
    vopts.preserveInvariance = YES;
    slots = dispatch_semaphore_create(jobs);
    group = dispatch_group_create();
    seen = [NSMutableSet new];
    if (getenv("AHEAD_PER_FILE")) per_file = [NSCountedSet new];
    lock = [NSLock new];
    maxwell_gmem_base = 0x7000000000ull + 0x240000000ull;  // (rt.h's HEAP_BASE / HEAP_MAX: they appear in the MSL)
    maxwell_gmem_size = 0x00D0000000ull;
    const char *romfs = argv[optind];
    qos_class_t q = !strcmp(qos, "background") ? QOS_CLASS_BACKGROUND : !strcmp(qos, "default") ? QOS_CLASS_DEFAULT : QOS_CLASS_UTILITY;
    __block AheadScanStats st;
    __block bool full;
    double t0 = now_ms();
    dispatch_semaphore_t fin = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(q, 0), ^{
        full = shader_ahead_scan(romfs, on_program, NULL, NULL, &st);
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
        dispatch_semaphore_signal(fin);
    });
    dispatch_semaphore_wait(fin, DISPATCH_TIME_FOREVER);
    double wall = now_ms() - t0;
    printf("scan: %d files, %d archives, %d shaders + %d programs (%d duplicates) in %.0f ms%s\n", st.files, st.archives,
           st.shaders, st.programs, st.dups, st.ms, full ? "" : " (stopped)");
    printf("translate: %d shaders in %.0f ms; %d distinct sources, %.1f MB of MSL; %d failures\n", nprog, t_translate, nsrc,
           msl_bytes / 1e6, nfail);
    if (per_file) {
        NSArray *keys = [per_file.allObjects sortedArrayUsingComparator:^NSComparisonResult(id a, id b) {
            return [@([per_file countForObject:b]) compare:@([per_file countForObject:a])];
        }];
        for (NSString *k in keys) printf("%6lu %s\n", (unsigned long)[per_file countForObject:k], k.UTF8String);
    }
    if (compile)
        printf("compile (%d jobs, qos %s): wall %.0f ms, %.0f ms of compiles (worst %.0f); <5 %d, <20 %d, <50 %d, <100 %d, "
               "<200 %d, <500 %d, <1000 %d, more %d\n", jobs, qos, wall, t_compile_sum, t_compile_max, hist[0], hist[1], hist[2],
               hist[3], hist[4], hist[5], hist[6], hist[7]);
    return 0;
}
