// Shader compile-ahead: the game's shaders from its files in the RomFS (shader_ahead.h).
#include "shader_ahead.h"

#include <dirent.h>
#include <stddef.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../platform.h"  // (pread, mkdir on Windows)
#include "../zstd_lib.h"

__attribute__((weak)) int shader_ahead_rank(const char *rel) { (void)rel; return 0; }

#define MAX_DEPTH 4
#define MAX_UNPACKED (256u << 20)  // (a compressed member that claims more is skipped)
#define CODE_BOUND 0x10000         // the renderer's translation bound without compiler constants
#define CODE_MAGIC 0x12345678u     // an NVN shader blob: this, 0x2c zero bytes, the SPH at +0x30, code at +0x80

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

// ---- shaders seen (open addressing on a hash of their bytes)
typedef struct { uint64_t *slot; size_t cap, n; } HashSet;
static bool set_add(HashSet *s, uint64_t h) {  // (false: already there)
    if (!h) h = 1;
    if ((s->n + 1) * 2 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        uint64_t *ns = calloc(cap, sizeof *ns);
        if (!ns) return true;
        for (size_t i = 0; i < s->cap; i++)
            if (s->slot[i]) {
                size_t j = s->slot[i] & (cap - 1);
                while (ns[j]) j = (j + 1) & (cap - 1);
                ns[j] = s->slot[i];
            }
        free(s->slot);
        s->slot = ns, s->cap = cap;
    }
    size_t j = h & (s->cap - 1);
    while (s->slot[j]) {
        if (s->slot[j] == h) return false;
        j = (j + 1) & (s->cap - 1);
    }
    s->slot[j] = h;
    s->n++;
    return true;
}

typedef struct FileList FileList;
typedef struct {
    AheadFn fn;
    void *ud;
    const volatile int *stop;
    HashSet seen;
    AheadScanStats *st;
    bool halted, in_prefer;
    int rank;
    const char *root;
    FileList *files;  // (the files not scanned yet: a file the game reads is scanned in between, scan_preferred)
} Scan;
static void scan_preferred(Scan *s);

// ---- decompression
static uint8_t *yaz0(const uint8_t *d, size_t n, size_t *out_n) {
    if (n < 16) return NULL;
    uint32_t size = be32(d + 4);
    if (size > MAX_UNPACKED) return NULL;
    uint8_t *o = malloc(size ? size : 1);
    if (!o) return NULL;
    size_t i = 16, w = 0;
    while (w < size) {
        if (i >= n) { free(o); return NULL; }
        uint8_t code = d[i++];
        for (int b = 0; b < 8 && w < size; b++) {
            if (code & (0x80 >> b)) {
                if (i >= n) { free(o); return NULL; }
                o[w++] = d[i++];
                continue;
            }
            if (i + 2 > n) { free(o); return NULL; }
            uint32_t b1 = d[i], b2 = d[i + 1];
            i += 2;
            size_t dist = ((b1 & 0xF) << 8 | b2) + 1, len = b1 >> 4;
            if (!len) {
                if (i >= n) { free(o); return NULL; }
                len = (size_t)d[i++] + 0x12;
            } else len += 2;
            if (dist > w) { free(o); return NULL; }
            for (size_t k = 0; k < len && w < size; k++, w++) o[w] = o[w - dist];
        }
    }
    *out_n = size;
    return o;
}

static uint8_t *unzstd(const uint8_t *d, size_t n, size_t *out_n) {
    unsigned long long sz = ZSTD_getFrameContentSize(d, n);
    if (sz == ZSTD_CONTENTSIZE_ERROR || sz == ZSTD_CONTENTSIZE_UNKNOWN || sz > MAX_UNPACKED) return NULL;
    uint8_t *o = malloc(sz ? (size_t)sz : 1);
    if (!o) return NULL;
    size_t r = ZSTD_decompress(o, (size_t)sz, d, n);
    if (ZSTD_isError(r)) { free(o); return NULL; }
    *out_n = r;
    return o;
}

// ---- shader blobs
static int blob_stage(const uint8_t *p, size_t avail) {  // (0 vertex, 1 fragment, 2 geometry, -1 not a shader)
    if (avail < 0x88 || le32(p) != CODE_MAGIC) return -1;
    for (int i = 4; i < 0x30; i += 4) if (le32(p + i)) return -1;
    uint32_t sph0 = le32(p + 0x30);
    uint32_t kind = sph0 & 0x1F, version = (sph0 >> 5) & 0x1F, type = (sph0 >> 10) & 0xF;
    if (version != 3) return -1;
    return kind == 1 && type == 1 ? 0 : kind == 2 && type == 5 ? 1 : kind == 1 && type == 4 ? 2 : -1;
}

// A BNSH's programs: "grsc" at 0x60 (variation count @+0x1c, array @+0x20; 0x40 each, the
// binary program @+0x10). A program: stage offsets @+8 (vertex, hull, domain, geometry, pixel, compute). A stage:
// control @+8, code @+0x10, code size @+0x18, control size @+0x1c; the control's compiler-constant offset @+0x700.
static const uint8_t *bnsh_stage(const uint8_t *b, size_t n, uint64_t so, size_t *cmax) {
    if (!so || so + 0x20 > n) return NULL;
    uint64_t ctl = le64(b + so + 8), code = le64(b + so + 0x10);
    uint32_t csz = le32(b + so + 0x18), ksz = le32(b + so + 0x1c);
    if (!csz || code + csz > n || blob_stage(b + code, n - code) < 0) return NULL;
    uint32_t coff = ctl && ksz >= 0x704 && ctl + 0x704 <= n ? le32(b + ctl + 0x700) : 0;
    *cmax = coff && coff <= csz ? coff : csz;
    return b + code;
}
static void scan_bnsh(Scan *s, const uint8_t *b, size_t n, const char *name) {
    if (n < 0xa0 || memcmp(b + 0x60, "grsc", 4)) return;
    uint32_t fsize = le32(b + 0x1c);
    if (fsize && fsize < n) n = fsize;
    uint32_t nv = le32(b + 0x60 + 0x1c);
    uint64_t va = le64(b + 0x60 + 0x20);
    for (uint32_t i = 0; i < nv && !s->halted; i++) {
        uint64_t v = va + (uint64_t)i * 0x40;
        if (v + 0x40 > n) break;
        uint64_t prog = le64(b + v + 0x10);
        if (!prog || prog + 0x38 > n) continue;
        AheadShader sh = {.stage = 2, .archive = name, .rank = s->rank};
        sh.code[0] = bnsh_stage(b, n, le64(b + prog + 8), &sh.cmax[0]);
        sh.code[1] = bnsh_stage(b, n, le64(b + prog + 8 + 4 * 8), &sh.cmax[1]);
        sh.code[2] = bnsh_stage(b, n, le64(b + prog + 8 + 3 * 8), &sh.cmax[2]);
        if (!sh.code[0] || !sh.code[1] || (le64(b + prog + 8 + 3 * 8) && !sh.code[2])) continue;
        uint64_t h = 0x9e3779b97f4a7c15ull;
        for (int k = 0; k < 3; k++) if (sh.code[k]) h = fnv(h, sh.code[k], sh.cmax[k]);
        if (!set_add(&s->seen, h)) { s->st->dups++; continue; }
        s->st->programs++;
        if (!s->fn(&sh, s->ud)) s->halted = true;
    }
}

// Every shader blob in d (256-byte aligned, as NVN needs code): each reaches up to the next one (the translator stops
// at the code's end, before the compiler constants that follow)
static void emit_shader(Scan *s, const uint8_t *p, size_t len, int stage, const char *name) {
    if (stage > 1) return;  // (geometry shaders: only with their program, scan_bnsh)
    if (len > CODE_BOUND) len = CODE_BOUND;
    if (!set_add(&s->seen, fnv(0xcbf29ce484222325ull ^ (uint64_t)stage, p, len))) { s->st->dups++; return; }
    AheadShader sh = {.stage = stage, .archive = name, .rank = s->rank};
    sh.code[stage] = p;
    sh.cmax[stage] = len;
    s->st->shaders++;
    if (!s->fn(&sh, s->ud)) s->halted = true;
}
static void scan_shaders(Scan *s, const uint8_t *d, size_t n, const char *name) {
    size_t prev = (size_t)-1;
    int prev_stage = -1;
    for (size_t o = 0; o + 0x88 <= n && !s->halted; o += 0x100) {
        int st = blob_stage(d + o, n - o);
        if (st < 0) continue;
        if (prev != (size_t)-1) emit_shader(s, d + prev, o - prev, prev_stage, name);
        prev = o, prev_stage = st;
    }
    if (prev != (size_t)-1 && !s->halted) emit_shader(s, d + prev, n - prev, prev_stage, name);
}

static void scan_blob(Scan *s, const uint8_t *d, size_t n, const char *name, int depth);

static void scan_sarc(Scan *s, const uint8_t *d, size_t n, const char *name, int depth) {
    if (n < 0x20) return;
    uint16_t hl = le16(d + 4);
    uint32_t data = le32(d + 0xC);
    if (le16(d + 6) != 0xFEFF || hl + 12 > n || memcmp(d + hl, "SFAT", 4)) return;  // (little-endian only: the Switch's)
    uint16_t cnt = le16(d + hl + 6);
    size_t fat = hl + 12, fnt = fat + (size_t)cnt * 16;
    if (fnt + 8 > n || memcmp(d + fnt, "SFNT", 4)) return;
    s->st->archives++;
    for (uint16_t i = 0; i < cnt && !s->halted; i++) {
        if (s->stop && *s->stop) { s->halted = true; break; }
        scan_preferred(s);  // (a file the game has just started reading goes first, even in the middle of a big pack)
        const uint8_t *e = d + fat + (size_t)i * 16;
        uint32_t attr = le32(e + 4), lo = le32(e + 8), hi = le32(e + 12);
        if ((uint64_t)data + hi > n || lo > hi) continue;
        char member[512];
        size_t no = fnt + 8 + (size_t)(attr & 0xFFFF) * 4;
        const char *mname = (attr >> 24) && no < n ? (const char *)d + no : "?";
        int mlen = (attr >> 24) && no < n ? (int)strnlen(mname, n - no) : 1;
        snprintf(member, sizeof member, "%s:%.*s", name, mlen, mname);
        scan_blob(s, d + data + lo, hi - lo, member, depth + 1);
    }
}

static void scan_blob(Scan *s, const uint8_t *d, size_t n, const char *name, int depth) {
    if (n < 16 || depth > MAX_DEPTH || s->halted) return;
    uint8_t *own = NULL;
    size_t on = 0;
    if (!memcmp(d, "Yaz0", 4)) own = yaz0(d, n, &on);
    else if (le32(d) == 0xFD2FB528u) own = unzstd(d, n, &on);
    if (own) d = own, n = on;
    if (!memcmp(d, "SARC", 4)) scan_sarc(s, d, n, name, depth);
    else {
        // whole programs from the BNSH archives (aligned blocks in shader archives): a model's fragment shaders as its
        // programs pair them (an input its vertex shader doesn't write reads a constant)
        for (size_t o = 0; o + 0xa0 <= n && !s->halted; o += 8)
            if (d[o] == 'B' && !memcmp(d + o, "BNSH\0\0\0\0", 8)) scan_bnsh(s, d + o, n - o, name);
        scan_shaders(s, d, n, name);
    }
    free(own);
}

// ---- files
typedef struct { char *rel; int rank; long long size; } FileEnt;
#define PREFER_MAX_SIZE (4ll << 20)  // (the files read for the game first: a model, not a whole pack)
struct FileList { FileEnt *v; int n, cap; };

static void list_files(const char *root, const char *rel, FileList *fl, int depth) {
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *dir = depth <= 8 ? opendir(path) : NULL;
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char r[1024], full[1100];
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", root, r);
        struct stat sb;
        if (stat(full, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { list_files(root, r, fl, depth + 1); continue; }
        if (!S_ISREG(sb.st_mode) || sb.st_size < 0x100) continue;
        int rank = shader_ahead_rank(r);
        if (rank < 0) continue;
        if (fl->n == fl->cap) {
            int cap = fl->cap ? fl->cap * 2 : 256;
            FileEnt *nv = realloc(fl->v, (size_t)cap * sizeof *nv);
            if (!nv) break;
            fl->v = nv, fl->cap = cap;
        }
        fl->v[fl->n].rel = strdup(r);
        fl->v[fl->n].rank = rank;
        fl->v[fl->n].size = (long long)sb.st_size;
        fl->n++;
    }
    closedir(dir);
}
static int by_rank(const void *a, const void *b) {
    const FileEnt *x = a, *y = b;
    return x->rank != y->rank ? (x->rank < y->rank ? -1 : 1) : strcmp(x->rel, y->rel);
}

static void scan_file(Scan *s, const char *root, const char *rel) {
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat sb;
    uint8_t head[8];
    if (fstat(fd, &sb) != 0 || sb.st_size < 0x100 || pread(fd, head, sizeof head, 0) != (ssize_t)sizeof head) { close(fd); return; }
    // (archives and files that may hold shaders; audio, textures and the rest aren't read)
    bool want = !memcmp(head, "SARC", 4) || !memcmp(head, "Yaz0", 4) || le32(head) == 0xFD2FB528u || !memcmp(head, "BNSH", 4) ||
                !memcmp(head, "BAHS", 4) || !memcmp(head, "VFXB", 4) || !memcmp(head, "FSHA", 4);
    if (!want) { close(fd); return; }
    size_t n = (size_t)sb.st_size;
#ifndef _WIN32
    void *m = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    s->st->files++;
    scan_blob(s, m, n, rel, 0);
    munmap(m, n);
#else
    uint8_t *m = malloc(n);
    bool ok = m && pread(fd, m, n, 0) == (ssize_t)n;
    close(fd);
    if (ok) { s->st->files++; scan_blob(s, m, n, rel, 0); }
    free(m);
#endif
}

// Files the game has just read (shader_ahead_prefer): scanned next, ahead of the rest
#define PREFER_N 32
static pthread_mutex_t prefer_lock = PTHREAD_MUTEX_INITIALIZER;
static char *prefer_ring[PREFER_N];
static int prefer_head, prefer_count;
static volatile int scanning;

void shader_ahead_prefer(const char *rel) {
    if (!scanning || !rel) return;
    while (*rel == '/') rel++;
    pthread_mutex_lock(&prefer_lock);
    int at = (prefer_head + prefer_count) % PREFER_N;
    if (prefer_count == PREFER_N) free(prefer_ring[at]), prefer_head = (prefer_head + 1) % PREFER_N;  // (the oldest goes)
    else prefer_count++;
    prefer_ring[at] = strdup(rel);
    pthread_mutex_unlock(&prefer_lock);
}
static char *prefer_take(void) {  // (the newest first: what the game needs soonest)
    pthread_mutex_lock(&prefer_lock);
    char *r = NULL;
    if (prefer_count) r = prefer_ring[(prefer_head + --prefer_count) % PREFER_N];
    pthread_mutex_unlock(&prefer_lock);
    return r;
}

static bool glob_match(const char *pat, const char *str) {  // ('*' only)
    for (; *pat; pat++, str++) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;
            if (!pat[1]) return true;
            for (; *str; str++) if (glob_match(pat + 1, str)) return true;
            return false;
        }
        if (*pat != *str) return false;
    }
    return !*str;
}

// The files asked for since the last look (the game reading them, or a game's hint), if they're still to be scanned: now
static void scan_preferred(Scan *s) {
    if (s->in_prefer || !s->files) return;
    s->in_prefer = true;
    for (char *want; !s->halted && (want = prefer_take());) {
        bool hint = strchr(want, '*') != NULL;
        for (int k = 0; k < s->files->n && !s->halted; k++) {
            FileEnt *f = &s->files->v[k];
            if (!f->rel || !(hint ? glob_match(want, f->rel) : !strcmp(f->rel, want))) continue;
            if (!hint && f->size > PREFER_MAX_SIZE) break;  // (a big archive read at load isn't drawn from at once)
            int rank = s->rank;
            s->rank = -1;
            scan_file(s, s->root, f->rel);
            s->rank = rank;
            s->st->preferred++;
            free(f->rel);
            f->rel = NULL;
            if (!hint) break;
        }
        free(want);
    }
    s->in_prefer = false;
}

bool shader_ahead_scan(const char *romfs, AheadFn fn, void *ud, const volatile int *stop, AheadScanStats *st) {
    AheadScanStats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);
    double t0 = now_ms();
    FileList fl = {0};
    list_files(romfs, "", &fl, 0);
    qsort(fl.v, (size_t)fl.n, sizeof *fl.v, by_rank);
    Scan s = {.fn = fn, .ud = ud, .stop = stop, .st = st, .root = romfs, .files = &fl};
    scanning = 1;
    for (int i = 0; i < fl.n && !s.halted && !(stop && *stop); i++) {
        scan_preferred(&s);
        if (!fl.v[i].rel) continue;
        s.rank = fl.v[i].rank;
        scan_file(&s, romfs, fl.v[i].rel);
        free(fl.v[i].rel);
        fl.v[i].rel = NULL;
    }
    scanning = 0;
    for (int i = 0; i < fl.n; i++) free(fl.v[i].rel);
    for (char *w; (w = prefer_take());) free(w);
    free(fl.v);
    free(s.seen.slot);
    st->ms = now_ms() - t0;
    return !s.halted && !(stop && *stop);
}

// ---- programs as the game sets their shaders (nvnProgramSetShaders): copied, kept for the backend's compile-ahead:
// the newest first while they're fresh (the game draws what it has just set up soonest), the backlog oldest first (what
// it set up at boot - agl's post effects, the styles' models - is what every course draws)
#define SET_MAX 4096
typedef struct SetItem { AheadShader sh; uint8_t *bytes; double at; } SetItem;
static pthread_mutex_t set_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t set_cond = PTHREAD_COND_INITIALIZER;
static SetItem *set_stack[SET_MAX];
static int set_len, set_dropped;
static HashSet set_seen;
static volatile int set_on;

void shader_ahead_set_enable(bool on) { set_on = on; }

void shader_ahead_program_set(const uint8_t *const code[3], const size_t cmax[3]) {
    if (!set_on || !code[0] || !code[1]) return;
    uint64_t h = 0x84222325cbf29ce4ull;
    size_t total = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t tag = code[i] ? (uint8_t)(i + 1) : 0;
        h = fnv(h, &tag, 1);
        if (code[i]) h = fnv(h, code[i], cmax[i]), total += cmax[i];
    }
    pthread_mutex_lock(&set_lock);
    bool fresh = set_add(&set_seen, h);
    pthread_mutex_unlock(&set_lock);
    if (!fresh) return;
    SetItem *it = calloc(1, sizeof *it);
    uint8_t *b = it ? malloc(total ? total : 1) : NULL;
    if (!b) { free(it); return; }
    it->bytes = b;
    it->at = now_ms();
    it->sh.stage = 2;
    it->sh.rank = -2;
    it->sh.archive = "(set by the game)";
    for (int i = 0; i < 3; i++)
        if (code[i]) {
            memcpy(b, code[i], cmax[i]);
            it->sh.code[i] = b, it->sh.cmax[i] = cmax[i];
            b += cmax[i];
        }
    pthread_mutex_lock(&set_lock);
    if (set_len == SET_MAX) {  // (full: the oldest goes)
        free(set_stack[0]->bytes), free(set_stack[0]);
        memmove(set_stack, set_stack + 1, (SET_MAX - 1) * sizeof *set_stack);
        set_len--, set_dropped++;
    }
    set_stack[set_len++] = it;
    pthread_cond_signal(&set_cond);
    pthread_mutex_unlock(&set_lock);
}

const AheadShader *shader_ahead_set_next(int wait_ms, double max_age_ms) {  // (max_age_ms < 0: the oldest)
    pthread_mutex_lock(&set_lock);
    if (!set_len && wait_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait_ms / 1000;
        ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
        pthread_cond_timedwait(&set_cond, &set_lock, &ts);
    }
    SetItem *it = NULL;
    if (set_len && max_age_ms < 0) {
        it = set_stack[0];
        memmove(set_stack, set_stack + 1, (size_t)(--set_len) * sizeof *set_stack);
    } else if (set_len && now_ms() - set_stack[set_len - 1]->at <= max_age_ms) it = set_stack[--set_len];
    pthread_mutex_unlock(&set_lock);
    return it ? &it->sh : NULL;
}

int shader_ahead_set_pending(void) {
    pthread_mutex_lock(&set_lock);
    int n = set_len;
    pthread_mutex_unlock(&set_lock);
    return n;
}

void shader_ahead_set_done(const AheadShader *s) {
    if (!s) return;
    SetItem *it = (SetItem *)(void *)((uint8_t *)s - offsetof(SetItem, sh));
    free(it->bytes);
    free(it);
}
