// Mystery Mushroom costumes: the SMB1-style player models generated from the player's dumps (costume.h).
//
// For every SMM1 costume (content/Model/M1_Player_Chara_<res>.szs in the SMM1 dump: 24 x 24 frames) a copy of SMM2's
// own small SMB1 Mario (Pack/M1_Model.pack -> Model/M1_Player_MarioMdl.Nin_NX_NVN.zs from the game's romfs) is
// patched: its 25 textures (16 x 16, RGBA8 sRGB, in the BFRES's embedded BNTX) are replaced by the costume's frames at
// 24 x 24 and its quad grows from (-8, 0)-(8, 16) to (-12, 0)-(12, 24) (SMM1's costume quad). Skeleton, material and
// animations stay SMM2's, so every animation the game plays works; SMM2's texture names are mapped onto the costume's
// frames (walk.0-2 -> walk.0-2, CourseIn / dead / held -> wait.0, ...) [inf: the costume frames SMM2 has no slot for -
// b_dash, jump_fall, pole, appeal - are not used yet].
//
// Written once, by the mods' shared RomFS overlay (mods/romfs_overlay.h: generator "costumes"), as Pack/M1_Model.pack
// (the original pack's files plus Model/M1_Player_Chara_<res>{,_2,_3,_4}.Nin_NX_NVN.zs, one data blob under four
// names), which replaces the game's. The .zs files are zstd frames of raw blocks (no compressor needed).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bfres.h"
#include "lyt.h"
#include "smm1_data.h"
#include "tegra.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "../../../../third_party/zstd/zstddeclib.c"

#define LOGC "[costume] "

// ---------------------------------------------------------------- little-endian helpers
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v), w32(p + 4, (uint32_t)(v >> 32)); }
static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->d = realloc(b->d, b->cap);
    }
    if (p) memcpy(b->d + b->n, p, n);
    else memset(b->d + b->n, 0, n);
    b->n += n;
}
static void buf_pad(Buf *b, size_t a) { buf_put(b, NULL, align_up(b->n, a) - b->n); }

// ---------------------------------------------------------------- SARC (little-endian) read / write
typedef struct { char name[128]; const uint8_t *d; size_t n; } Entry;
static int sarc_read(const uint8_t *d, size_t n, Entry **out) {
    *out = NULL;
    LSarc s;
    if (!lyt_sarc_open(&s, d, n)) return 0;
    *out = calloc((size_t)s.count + 1, sizeof **out);
    for (int i = 0; i < s.count; i++) {
        snprintf((*out)[i].name, sizeof (*out)[i].name, "%s", s.e[i].name);
        (*out)[i].d = d + s.e[i].start, (*out)[i].n = s.e[i].end - s.e[i].start;
    }
    int c = s.count;
    lyt_sarc_close(&s);
    return c;
}
static uint32_t sarc_hash(const char *s) {
    uint32_t h = 0;
    for (; *s; s++) h = h * 0x65 + (uint32_t)(int8_t)*s;
    return h;
}
typedef struct { const char *name; uint32_t hash; int blob; } Node;
static int node_cmp(const void *a, const void *b) {
    uint32_t x = ((const Node *)a)->hash, y = ((const Node *)b)->hash;
    return x < y ? -1 : x > y;
}
// names[i] -> blobs[blob_of[i]] (several names may share one blob)
static uint8_t *sarc_write(int n, const char *const *names, const int *blob_of, int nblobs, const uint8_t *const *blob,
                           const size_t *blob_n, size_t align, size_t *out_n) {
    Node *nd = calloc((size_t)n + 1, sizeof *nd);
    for (int i = 0; i < n; i++) nd[i] = (Node){names[i], sarc_hash(names[i]), blob_of[i]};
    qsort(nd, (size_t)n, sizeof *nd, node_cmp);
    Buf names_b = {0};
    uint32_t *name_off = calloc((size_t)n + 1, 4);
    for (int i = 0; i < n; i++) {
        name_off[i] = (uint32_t)names_b.n;
        buf_put(&names_b, nd[i].name, strlen(nd[i].name) + 1);
        buf_pad(&names_b, 4);
    }
    size_t head = 0x14 + 0xC + (size_t)n * 16 + 8 + names_b.n, data0 = align_up(head, align);
    size_t *boff = calloc((size_t)nblobs + 1, sizeof *boff), dn = 0;
    for (int b = 0; b < nblobs; b++) boff[b] = dn, dn = align_up(dn + blob_n[b], align);
    size_t total = data0 + dn;
    uint8_t *o = calloc(1, total);
    memcpy(o, "SARC", 4), w16(o + 4, 0x14), o[6] = 0xFF, o[7] = 0xFE, w32(o + 8, (uint32_t)total), w32(o + 12, (uint32_t)data0);
    w16(o + 16, 0x0100);
    uint8_t *f = o + 0x14;
    memcpy(f, "SFAT", 4), w16(f + 4, 0xC), w16(f + 6, (uint32_t)n), w32(f + 8, 0x65);
    for (int i = 0; i < n; i++) {
        uint8_t *e = f + 0xC + (size_t)i * 16;
        w32(e, nd[i].hash), w32(e + 4, 0x01000000 | name_off[i] / 4);
        w32(e + 8, (uint32_t)boff[nd[i].blob]), w32(e + 12, (uint32_t)(boff[nd[i].blob] + blob_n[nd[i].blob]));
    }
    uint8_t *t = f + 0xC + (size_t)n * 16;
    memcpy(t, "SFNT", 4), w16(t + 4, 8);
    memcpy(t + 8, names_b.d, names_b.n);
    for (int b = 0; b < nblobs; b++) memcpy(o + data0 + boff[b], blob[b], blob_n[b]);
    free(nd), free(names_b.d), free(name_off), free(boff);
    *out_n = total;
    return o;
}

// ---------------------------------------------------------------- zstd
static uint8_t *zstd_read(const uint8_t *d, size_t n, size_t *out_n) {
    unsigned long long sz = ZSTD_getFrameContentSize(d, n);
    if (sz == ZSTD_CONTENTSIZE_ERROR || sz == ZSTD_CONTENTSIZE_UNKNOWN || sz > (64u << 20)) return NULL;
    uint8_t *o = malloc((size_t)sz + 1);
    size_t r = ZSTD_decompress(o, (size_t)sz, d, n);
    if (ZSTD_isError(r)) { free(o); return NULL; }
    *out_n = r;
    return o;
}
// a zstd frame of raw (stored) blocks
static uint8_t *zstd_store(const uint8_t *d, size_t n, size_t *out_n) {
    Buf b = {0};
    uint8_t h[9] = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};  // magic; descriptor: 4-byte content size, single segment
    w32(h + 5, (uint32_t)n);
    buf_put(&b, h, 9);
    size_t off = 0;
    do {
        size_t k = n - off > 0x1FFFF ? 0x1FFFF : n - off;
        bool last = off + k >= n;
        uint32_t bh = (uint32_t)(last ? 1 : 0) | 0u << 1 | (uint32_t)k << 3;
        uint8_t hb[3] = {(uint8_t)bh, (uint8_t)(bh >> 8), (uint8_t)(bh >> 16)};
        buf_put(&b, hb, 3);
        buf_put(&b, d + off, k);
        off += k;
    } while (off < n);
    *out_n = b.n;
    return b.d;
}

// ---------------------------------------------------------------- the patch
// SMM2's texture -> the costume frame shown in its place
static const char *frame_for(const char *smm2_name) {
    static const char *map[][2] = {
        {"CourseIn.", "wait.0"}, {"dead.", "wait.0"}, {"held.", "wait.0"}, {"jump2.", "jump.0"},
        {"karon_stoop.", "stoop.0"}, {"karon_wait.", "wait.0"}, {"shoes_stoop.", "shoes_wait.0"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strncmp(smm2_name, map[i][0], strlen(map[i][0]))) return map[i][1];
    return smm2_name;  // climb.N, jump.0, shoes_wait.0, stoop.0, swim.N, turn.0, wait.0, walk.N: the same names
}

#define CW 24
#define CH 24
#define TEX_BYTES 4096  // 24 x 24 RGBA8, block height 4 GOBs: 2 GOBs wide x 32 rows
#define TEX_BHL 2

// one costume frame as 24 x 24 RGBA8 swizzled (block-linear); missing frames fall back to wait.0
static void frame_pixels(const Bfres *c, const char *want, uint8_t out[TEX_BYTES]) {
    int id = bfres_texture(c, want);
    if (id < 0) id = bfres_texture(c, "wait.0");
    const LTex *t = lyt_tex(id);
    uint8_t lin[CW * CH * 4];
    memset(lin, 0, sizeof lin);
    if (t && t->px)
        for (int y = 0; y < CH && y < t->h; y++) memcpy(lin + (size_t)y * CW * 4, t->px + (size_t)y * t->w * 4, (size_t)(t->w < CW ? t->w : CW) * 4);
    memset(out, 0, TEX_BYTES);
    tegra_swizzle(lin, out, CW, CH, 1, 4, TEX_BHL, 0);
}

// the embedded BNTX rebuilt with the costume's frames; NULL if it doesn't look like SMM2's Mario
static uint8_t *bntx_patch(const uint8_t *b, size_t n, const Bfres *c, size_t *out_n) {
    if (n < 0x60 || memcmp(b, "BNTX", 4) || memcmp(b + 0x20, "NX  ", 4)) return NULL;
    uint32_t count = le32(b + 0x24);
    size_t info = (size_t)le64(b + 0x28), brtd = (size_t)le64(b + 0x30), rlt = le32(b + 0x18), fsize = le32(b + 0x1C);
    if (!count || count > 64 || brtd + 0x10 > n || rlt >= n || fsize > n || memcmp(b + brtd, "BRTD", 4)) return NULL;
    size_t data0 = brtd + 0x10, data_end = data0 + (size_t)count * TEX_BYTES, nrlt = align_up(data_end, 0x1000);
    size_t rlt_n = fsize - rlt, total = nrlt + rlt_n;
    uint8_t *o = calloc(1, total);
    memcpy(o, b, brtd + 0x10);
    for (uint32_t k = 0; k < count; k++) {
        size_t t = (size_t)le64(b + info + 8 * (size_t)k);
        if (t + 0xA0 > brtd || memcmp(b + t, "BRTI", 4)) { free(o); return NULL; }
        size_t no = (size_t)le64(b + t + 0x60);
        char name[64];
        snprintf(name, sizeof name, "%.*s", le16(b + no) < 63 ? le16(b + no) : 63, (const char *)b + no + 2);
        w32(o + t + 0x24, CW), w32(o + t + 0x28, CH), w32(o + t + 0x34, TEX_BHL), w32(o + t + 0x50, TEX_BYTES);
        size_t ptrs = (size_t)le64(b + t + 0x70);
        w64(o + ptrs, data0 + (size_t)k * TEX_BYTES);
        frame_pixels(c, frame_for(name), o + data0 + (size_t)k * TEX_BYTES);
    }
    w64(o + brtd + 8, nrlt - brtd);  // BRTD: its block runs to the relocation table
    memcpy(o + nrlt, b + rlt, rlt_n);
    w32(o + nrlt + 4, (uint32_t)nrlt);
    uint32_t nsec = le32(o + nrlt + 8);
    for (uint32_t s = 0; s < nsec; s++) {
        uint8_t *sec = o + nrlt + 0x10 + (size_t)s * 0x18;
        uint32_t off = le32(sec + 8), sz = le32(sec + 12);
        if (off <= brtd && brtd < off + sz) w32(sec + 12, (uint32_t)(nrlt - off));
    }
    w32(o + 0x18, (uint32_t)nrlt), w32(o + 0x1C, (uint32_t)total);
    *out_n = total;
    return o;
}

// the template BFRES with the costume: new BNTX (in place of the old, growing the file) and the bigger quad
static uint8_t *bfres_patch(const uint8_t *f, size_t n, const Bfres *c, size_t *out_n) {
    if (n < 0x100 || memcmp(f, "FRES    ", 8)) return NULL;
    size_t rlt = le32(f + 0x18), fsize = le32(f + 0x1C), ext = (size_t)le64(f + 0xB8);
    if (rlt >= n || fsize > n || !ext || ext + 16 > n) return NULL;
    size_t boff = (size_t)le64(f + ext), bsz = le32(f + ext + 8);
    if (boff + bsz > rlt) return NULL;
    size_t nb_n;
    uint8_t *nb = bntx_patch(f + boff, bsz, c, &nb_n);
    if (!nb) return NULL;
    size_t nrlt = align_up(boff + nb_n, 0x1000), rlt_n = fsize - rlt, total = nrlt + rlt_n;
    uint8_t *o = calloc(1, total);
    memcpy(o, f, boff);
    memcpy(o + boff, nb, nb_n);
    free(nb);
    memcpy(o + nrlt, f + rlt, rlt_n);
    w32(o + nrlt + 4, (uint32_t)nrlt);
    uint32_t nsec = le32(o + nrlt + 8);
    for (uint32_t s = 0; s < nsec; s++) {
        uint8_t *sec = o + nrlt + 0x10 + (size_t)s * 0x18;
        uint32_t off = le32(sec + 8), sz = le32(sec + 12);
        if (off <= boff && boff < off + sz) w32(sec + 12, (uint32_t)(nrlt - off));
    }
    w32(o + 0x18, (uint32_t)nrlt), w32(o + 0x1C, (uint32_t)total);
    w32(o + ext + 8, (uint32_t)nb_n);
    // the quad: half-float positions (-8,0,0,1) (8,0,0,1) (-8,16,0,1) (8,16,0,1) -> SMM1's (-12, 0)-(12, 24)
    static const uint16_t q16[16] = {0xC800, 0, 0, 0x3C00, 0x4800, 0, 0, 0x3C00, 0xC800, 0x4C00, 0, 0x3C00, 0x4800, 0x4C00, 0, 0x3C00};
    static const uint16_t q24[16] = {0xCA00, 0, 0, 0x3C00, 0x4A00, 0, 0, 0x3C00, 0xCA00, 0x4E00, 0, 0x3C00, 0x4A00, 0x4E00, 0, 0x3C00};
    uint8_t pat[32], rep[32];
    for (int i = 0; i < 16; i++) w16(pat + 2 * i, q16[i]), w16(rep + 2 * i, q24[i]);
    int quads = 0;
    for (size_t i = 0; i + 32 <= boff; i += 2)
        if (!memcmp(o + i, pat, 32)) memcpy(o + i, rep, 32), quads++;
    if (quads != 1) fprintf(stderr, LOGC "the template's quad was found %d times\n", quads);
    *out_n = total;
    return o;
}

// ---------------------------------------------------------------- generation
#define PACK "Pack/M1_Model.pack"
#ifdef SMM1UI_STANDALONE  // (tools/test_smm1ui --costumes: the files of the data folder)
#define OUT_PACK "mods-generated/costume-test/" PACK
static uint8_t *read_pack(size_t *n) { return smm1_read_file("extracted/romfs/" PACK, n); }
#else  // (the game: the shared overlay)
#include "../romfs_overlay.h"
static uint8_t *read_pack(size_t *n) {
    RovBuf b = {0};
    if (!rov_read(PACK, &b)) return NULL;
    *n = b.n;
    return b.d;
}
#endif

static bool mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    return true;
}
#ifndef SMM1UI_STANDALONE
#include <ftw.h>
static int rm_entry(const char *p, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(p);
}
static void remove_old(void) {  // (the folder of the first version, before the shared overlay)
    struct stat st;
    if (!stat("mods-generated/costume-romfs", &st)) nftw("mods-generated/costume-romfs", rm_entry, 8, FTW_DEPTH | FTW_PHYS);
}
#endif
static __attribute__((unused)) bool write_file(const char *path, const uint8_t *d, size_t n) {
    mkdirs(path);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    fclose(f);
    return ok;
}

static char (*g_costumes)[48];
static int g_ncostumes;
int costume_count(void) { return g_ncostumes; }
const char *costume_res(int i) { return i >= 0 && i < g_ncostumes ? g_costumes[i] : NULL; }

// the SMM1 costumes of the dump: Model/M1_Player_Chara_<res>.szs, minus alternate texture sets (_W water, _L)
static void list_costumes(void) {
    if (g_costumes) return;
    char (*names)[96] = NULL;
    int n = smm1_dump_list("Model", "M1_Player_Chara_", &names);
    g_costumes = calloc((size_t)n + 1, sizeof *g_costumes);
    for (int i = 0; i < n; i++) {
        char res[96];
        snprintf(res, sizeof res, "%s", names[i] + strlen("M1_Player_Chara_"));
        char *dot = strstr(res, ".szs");
        if (!dot) continue;
        *dot = 0;
        size_t l = strlen(res);
        if (l > 2 && (!strcmp(res + l - 2, "_W") || !strcmp(res + l - 2, "_L"))) continue;
        snprintf(g_costumes[g_ncostumes++], 48, "%s", res);
    }
    free(names);
}

static Bfres *costume_bfres(const char *res) {
    char rel[160];
    snprintf(rel, sizeof rel, "Model/M1_Player_Chara_%s.szs", res);
    size_t zn, an, bn;
    uint8_t *z = smm1_dump_read(rel, &zn);
    uint8_t *arc = z ? lyt_yaz0(z, zn, &an) : NULL;
    free(z);
    Bfres *b = NULL;
    LSarc s;
    if (arc && lyt_sarc_open(&s, arc, an)) {
        const uint8_t *bf = lyt_sarc_get(&s, "Output.bfres", &bn);
        if (bf) b = bfres_load(bf, bn);
        lyt_sarc_close(&s);
    }
    free(arc);
    return b;
}

bool costume_generate(void) {
    list_costumes();
    if (!g_ncostumes) {
        fprintf(stderr, LOGC "no SMM1 costumes found (the SMM1 dump: RECOMP_SMM1_DUMP)\n");
        return false;
    }
    size_t pn;
    uint8_t *pack = read_pack(&pn);
    if (!pack) { fprintf(stderr, LOGC "SMM2's Pack/M1_Model.pack not found\n"); return false; }
    Entry *orig = NULL;
    int norig = sarc_read(pack, pn, &orig);
    const uint8_t *tz = NULL;
    size_t tzn = 0;
    for (int i = 0; i < norig; i++)
        if (!strcmp(orig[i].name, "Model/M1_Player_MarioMdl.Nin_NX_NVN.zs")) tz = orig[i].d, tzn = orig[i].n;
    size_t tn = 0;
    uint8_t *tarc = tz ? zstd_read(tz, tzn, &tn) : NULL;
    Entry *inner = NULL;
    int ninner = tarc ? sarc_read(tarc, tn, &inner) : 0;
    int ibfres = -1;
    for (int i = 0; i < ninner; i++)
        if (strstr(inner[i].name, ".bfres")) ibfres = i;
    if (ibfres < 0) {
        fprintf(stderr, LOGC "SMM2's small SMB1 Mario model not readable\n");
        free(pack), free(orig), free(tarc), free(inner);
        return false;
    }
    // the new pack: the original files, then four names per costume on one blob each
    int maxn = norig + 4 * g_ncostumes, nn = 0, nblob = 0;
    const char **names = calloc((size_t)maxn + 1, sizeof *names);
    int *blob_of = calloc((size_t)maxn + 1, sizeof *blob_of);
    const uint8_t **blob = calloc((size_t)(norig + g_ncostumes) + 1, sizeof *blob);
    size_t *blob_n = calloc((size_t)(norig + g_ncostumes) + 1, sizeof *blob_n);
    char (*cnames)[96] = calloc((size_t)(4 * g_ncostumes) + 1, sizeof *cnames);
    for (int i = 0; i < norig; i++) names[nn] = orig[i].name, blob_of[nn++] = nblob, blob[nblob] = orig[i].d, blob_n[nblob++] = orig[i].n;
    int made = 0;
    for (int c = 0; c < g_ncostumes; c++) {
        Bfres *cb = costume_bfres(g_costumes[c]);
        if (!cb) { fprintf(stderr, LOGC "%s: not readable\n", g_costumes[c]); continue; }
        size_t fn;
        uint8_t *nf = bfres_patch(inner[ibfres].d, inner[ibfres].n, cb, &fn);
        if (!nf) { fprintf(stderr, LOGC "%s: the template could not be patched\n", g_costumes[c]); continue; }
        const char *in_names[8];
        int in_blob[8];
        const uint8_t *in_d[8];
        size_t in_n[8];
        int ni = ninner < 8 ? ninner : 8;
        for (int i = 0; i < ni; i++) {
            in_names[i] = inner[i].name, in_blob[i] = i;
            in_d[i] = i == ibfres ? nf : inner[i].d, in_n[i] = i == ibfres ? fn : inner[i].n;
        }
        size_t an;
        uint8_t *arc = sarc_write(ni, in_names, in_blob, ni, in_d, in_n, 0x1000, &an);
        free(nf);
        size_t zn;
        uint8_t *zs = zstd_store(arc, an, &zn);
        free(arc);
        blob[nblob] = zs, blob_n[nblob] = zn;
        static const char *suffix[4] = {"", "_2", "_3", "_4"};
        for (int s = 0; s < 4; s++) {
            snprintf(cnames[4 * c + s], 96, "Model/M1_Player_Chara_%s%s.Nin_NX_NVN.zs", g_costumes[c], suffix[s]);
            names[nn] = cnames[4 * c + s], blob_of[nn++] = nblob;
        }
        nblob++, made++;
    }
    size_t on;
    uint8_t *out = sarc_write(nn, names, blob_of, nblob, blob, blob_n, 0x80, &on);
#ifdef SMM1UI_STANDALONE
    bool ok = made > 0 && write_file(OUT_PACK, out, on);
#else
    bool ok = made > 0 && rov_write(PACK, out, on);
#endif
    fprintf(stderr, LOGC "%d costumes generated into " PACK " (%.1f MB)\n", made, on / 1e6);
    for (int b = norig; b < nblob; b++) free((void *)blob[b]);
    free(out), free(names), free(blob_of), free(blob), free(blob_n), free(cnames);
    free(pack), free(orig), free(tarc), free(inner);
    return ok;
}

#ifndef SMM1UI_STANDALONE
// (the costumes mod on: the models; off: nothing. mods/costume.c)
bool costumes_on(void);
static bool costume_overlay(void) {
    if (!costumes_on()) return true;
    remove_old();
    return costume_generate();
}
ROV_GENERATOR("costumes", 1, costume_overlay)
#endif

// ---------------------------------------------------------------- the costume table (SMM1's Mush/CharaMarioTable.byaml)
// BYML v1, big-endian: root array of dictionaries with ResName (string) and Rare (int: the category: 0 always owned,
// 1 / 2 / 3 / 6 the 100 Mario pools of Easy / Normal / Expert / Super Expert, 4 / 5 / 7 others) [obs data]
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static const char *byml_str(const uint8_t *d, size_t n, size_t table, uint32_t i) {
    if (!table || table + 4 > n || d[table] != 0xC2 || i >= be24(d + table + 1)) return "";
    size_t o = table + be32(d + table + 4 + 4 * (size_t)i);
    return o < n ? (const char *)d + o : "";
}
static struct { char res[48]; int rare; } *g_table;
static int g_ntable = -1;
static void load_table(void) {
    if (g_ntable >= 0) return;
    g_ntable = 0;
    size_t pn, bn;
    uint8_t *pack = smm1_dump_read("Pack/Static.pack", &pn);
    LSarc s;
    if (!pack || !lyt_sarc_open(&s, pack, pn)) { free(pack); return; }
    const uint8_t *b = lyt_sarc_get(&s, "Mush/CharaMarioTable.byaml", &bn);
    if (b && bn > 0x10 && b[0] == 'B' && b[1] == 'Y') {
        size_t keys = be32(b + 4), strs = be32(b + 8), root = be32(b + 12);
        if (root + 4 <= bn && b[root] == 0xC0) {
            uint32_t count = be24(b + root + 1);
            size_t vals = root + 4 + ((count + 3) & ~3u);
            g_table = calloc(count + 1, sizeof *g_table);
            for (uint32_t i = 0; i < count && vals + 4 * (size_t)i + 4 <= bn; i++) {
                if (b[root + 4 + i] != 0xC1) continue;
                size_t dict = be32(b + vals + 4 * (size_t)i);
                if (dict + 4 > bn) continue;
                uint32_t ne = be24(b + dict + 1);
                char res[48] = "";
                int rare = -1;
                for (uint32_t e = 0; e < ne && dict + 4 + 8 * (size_t)e + 8 <= bn; e++) {
                    const uint8_t *en = b + dict + 4 + 8 * (size_t)e;
                    const char *key = byml_str(b, bn, keys, be24(en));
                    uint8_t type = en[3];
                    uint32_t v = be32(en + 4);
                    if (!strcmp(key, "ResName") && type == 0xA0) snprintf(res, sizeof res, "%s", byml_str(b, bn, strs, v));
                    if (!strcmp(key, "Rare") && type == 0xD1) rare = (int)v;
                }
                if (res[0]) snprintf(g_table[g_ntable].res, 48, "%s", res), g_table[g_ntable++].rare = rare;
            }
        }
    }
    lyt_sarc_close(&s);
    free(pack);
}
// the costume's category (Rare), -1 if the table doesn't list it
int costume_rare(const char *res) {
    load_table();
    for (int i = 0; i < g_ntable; i++)
        if (!strcmp(g_table[i].res, res)) return g_table[i].rare;
    return -1;
}
void costume_list(void) { list_costumes(); }

// the costume's English name and series title (SMM1's Cmn_CharaMarioName / Cmn_CharaMarioTitle, UTF-8)
static void msg_utf8(const char *table, const char *label, char *out, size_t cap) {
    out[0] = 0;
    size_t n;
    uint8_t *d = smm1_common_msbt(table, &n);
    LMsbt t;
    if (d && lyt_msbt_parse(&t, d, n)) {
        const LMsg *m = lyt_msbt_get(&t, label);
        size_t k = 0;
        for (int i = 0; m && i < m->n && k + 4 < cap; i++) {
            uint32_t c = m->text[i];
            if (c == LYT_ARG || c == '\n') continue;
            if (c < 0x80) out[k++] = (char)c;
            else if (c < 0x800) out[k++] = (char)(0xC0 | c >> 6), out[k++] = (char)(0x80 | (c & 63));
            else out[k++] = (char)(0xE0 | c >> 12), out[k++] = (char)(0x80 | (c >> 6 & 63)), out[k++] = (char)(0x80 | (c & 63));
        }
        out[k] = 0;
    }
    free(d);
}
void costume_name(const char *res, char *name, size_t cap, char *title, size_t tcap) {
    smm1_data_open();
    if (name) msg_utf8("Cmn_CharaMarioName", res, name, cap);
    if (title) msg_utf8("Cmn_CharaMarioTitle", res, title, tcap);
    if (name && !name[0]) snprintf(name, cap, "%s", res);
}
// the costume's standing frame as a texture (wait.0), -1 if missing
int costume_sprite(const char *res) {
    static struct { char res[48]; int tex; } cache[8];
    static int nc;
    for (int i = 0; i < nc; i++)
        if (!strcmp(cache[i].res, res)) return cache[i].tex;
    Bfres *b = costume_bfres(res);
    int tex = b ? bfres_texture(b, "wait.0") : -1;
    if (nc < 8) snprintf(cache[nc].res, 48, "%s", res), cache[nc++].tex = tex;
    return tex;
}
