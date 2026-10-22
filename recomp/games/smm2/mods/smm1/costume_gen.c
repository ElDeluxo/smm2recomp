// Mystery Mushroom costumes: the SMB1-style player models generated from the player's dumps (costume.h).
//
// For every SMM1 costume (content/Model/M1_Player_Chara_<res>.szs in the SMM1 dump: 24 x 24 frames) a copy of SMM2's
// own small SMB1 Mario (Pack/M1_Model.pack -> Model/M1_Player_MarioMdl.Nin_NX_NVN.zs from the game's romfs) is
// patched: its 25 textures (16 x 16, RGBA8 sRGB, in the BFRES's embedded BNTX) are replaced by the costume's frames at
// 24 x 24 and its quad grows from (-8, 0)-(8, 16) to (-12, 0)-(12, 24) (SMM1's costume quad). Skeleton, material and
// animations stay SMM2's, so every animation the game plays works; SMM2's texture names are mapped onto the costume's
// frames (walk.0-2 -> walk.0-2, CourseIn / dead / held -> wait.0, ...) [inf: the costume frames SMM2 has no slot for -
// b_dash, jump_fall, pole - are not used yet]. The appeal frame (the taunt) is kept apart (below).
//
// Written once, by the mods' shared RomFS overlay (mods/romfs_overlay.h: generator "costumes"), as Pack/M1_Model.pack
// (the original pack's files plus Model/M1_Player_Chara_<res>{,_2,_3,_4}.Nin_NX_NVN.zs, one data blob under four
// names), which replaces the game's. The .zs files are compressed with zstd (level 3), as the game's own are. With them,
// Mods/CostumeAppeal.bin (ours, nothing of the game's reads it): each costume's appeal frame as a model texture holds
// it. Since every model is the template with other texels, costume_look gives mods/costume.c a costume's texels to
// write over a loaded model's (another costume mid-course, the taunt pose).
#ifndef SMM1UI_STANDALONE
#include "../../../../runtime/platform.h"  // (mkdir with a mode and plat_remove_tree on every host)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bfres.h"
#include "lyt.h"
#include "smm1_data.h"
#include "tegra.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "../../../../third_party/zstd/zstd.c"

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
// compressed (level 3: about a tenth of the raw size; the pack stays near the game's own size)
static uint8_t *zstd_store(const uint8_t *d, size_t n, size_t *out_n) {
    size_t cap = ZSTD_compressBound(n);
    uint8_t *o = malloc(cap);
    size_t r = o ? ZSTD_compress(o, cap, d, n, 3) : 0;
    if (!o || ZSTD_isError(r)) { free(o); return NULL; }
    *out_n = r;
    return o;
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
#define APPEAL "Mods/CostumeAppeal.bin"  // (ours: the taunt frames, read by costume_look; nothing of the game's)
#define APPEAL_REC (48 + TEX_BYTES)
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
static void remove_old(void) {  // (the folder of the first version, before the shared overlay)
    struct stat st;
    if (!stat("mods-generated/costume-romfs", &st)) plat_remove_tree("mods-generated/costume-romfs");
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
    // the taunt's frames (APPEAL file: per costume its name, then its appeal.0 as the model's textures hold a frame)
    uint8_t *appeal = calloc((size_t)g_ncostumes + 1, APPEAL_REC);
    int nappeal = 0;
    for (int c = 0; c < g_ncostumes; c++) {
        Bfres *cb = costume_bfres(g_costumes[c]);
        if (!cb) { fprintf(stderr, LOGC "%s: not readable\n", g_costumes[c]); continue; }
        uint8_t *rec = appeal + (size_t)nappeal++ * APPEAL_REC;
        snprintf((char *)rec, 48, "%s", g_costumes[c]);
        frame_pixels(cb, "appeal.0", rec + 48);
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
        if (!zs) { fprintf(stderr, LOGC "%s: not compressed\n", g_costumes[c]); continue; }
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
    bool ok = made > 0 && rov_write(PACK, out, on) && rov_write(APPEAL, appeal, (size_t)nappeal * APPEAL_REC);
#endif
    free(appeal);
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
ROV_GENERATOR("costumes", 3, costume_overlay)  // (2: the models compressed; 3: the taunt frames)
#endif

// ---------------------------------------------------------------- Lanky Mario's Slim model (mods/lanky.c)
// SMM1's Weird Mushroom form is drawn with Model/M1_Player_SlimMarioMdl.szs (Pack/StaticSkin.pack of the SMM1 dump):
// 16 x 32 frames on the same quad as Super Mario. SMM2's own Super SMB1 Mario (Model/M1_Player_SpMarioMdl{,_2,_3,_4}:
// FMDL Su_mario_model, 21 textures of 16 x 32 RGBA8 sRGB) is copied with each texture's pixels replaced by the Slim
// frame of the same pose: sizes, quad, skeleton, materials and animations stay SMM2's, so nothing but the pixels
// changes. Written as Model/M1_Player_SlimMarioMdl{,_2,_3,_4}.Nin_NX_NVN.zs into Pack/M1_Model.pack, read through the
// overlay (rov_read) so the costumes' models in the same pack stay, whichever generator runs first.
//
// SMM1 has only Mario's Slim art. Luigi, Toad and Toadette (co-op: _2, _3, _4) get it recoloured with their own SMB1
// Super palette: each colour of SMM2's Super Mario maps to the colour at the same pixel of that character's Super
// model (the most frequent one over all 21 textures), and Slim's colours go through that map (the nearest key when a
// colour is not one of Super Mario's).

// SMM2's Super texture -> the Slim frame shown in its place (the same name otherwise: climb, jump, shoes_wait, stoop,
// swim, turn, wait, walk); Slim's appeal, b_dash and pole frames have no place in SMM2's Super model
static const char *slim_frame_for(const char *smm2_name) {
    static const char *map[][2] = {
        {"held.", "wait.0"},        {"jump2.", "jump_fall.0"}, {"karon_stoop.", "stoop.0"},
        {"karon_wait.", "wait.0"},  {"shoes_stoop.", "shoes_wait.0"}, {"CourseIn.", "wait.0"}, {"dead.", "wait.0"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strncmp(smm2_name, map[i][0], strlen(map[i][0]))) return map[i][1];
    return smm2_name;
}

// the BNTX's textures, each RGBA8 (bpp 4) with its mip 0 data in place: fn(name, data, w, h, block height log2)
typedef void (*BntxFn)(void *ctx, const char *name, uint8_t *data, int w, int h, int bhl);
static int bntx_each(uint8_t *b, size_t n, BntxFn fn, void *ctx) {
    if (n < 0x60 || memcmp(b, "BNTX", 4) || memcmp(b + 0x20, "NX  ", 4)) return -1;
    uint32_t count = le32(b + 0x24);
    size_t info = (size_t)le64(b + 0x28);
    if (!count || count > 64 || info + 8 * (size_t)count > n) return -1;
    int done = 0;
    for (uint32_t k = 0; k < count; k++) {
        size_t t = (size_t)le64(b + info + 8 * (size_t)k);
        if (t + 0xA0 > n || memcmp(b + t, "BRTI", 4)) return -1;
        size_t no = (size_t)le64(b + t + 0x60), ptrs = (size_t)le64(b + t + 0x70);
        if (no + 2 > n || ptrs + 8 > n) return -1;
        char name[64];
        snprintf(name, sizeof name, "%.*s", le16(b + no) < 63 ? le16(b + no) : 63, (const char *)b + no + 2);
        int w = (int)le32(b + t + 0x24), h = (int)le32(b + t + 0x28), bhl = (int)le32(b + t + 0x34);
        size_t size = le32(b + t + 0x50), data = (size_t)le64(b + ptrs);
        if (w <= 0 || h <= 0 || w > 64 || h > 64 || data + size > n || size < (size_t)w * (size_t)h * 4) continue;
        fn(ctx, name, b + data, w, h, bhl);
        done++;
    }
    return done;
}

// the embedded BNTX of a BFRES (in place)
static uint8_t *bfres_bntx(uint8_t *f, size_t n, size_t *bn) {
    if (n < 0x100 || memcmp(f, "FRES    ", 8)) return NULL;
    size_t ext = (size_t)le64(f + 0xB8);
    if (!ext || ext + 16 > n) return NULL;
    size_t boff = (size_t)le64(f + ext), bsz = le32(f + ext + 8);
    if (boff + bsz > n) return NULL;
    *bn = bsz;
    return f + boff;
}

// a model archive (.zs: zstd -> SARC with one .bfres) unpacked; its BFRES is a malloc'd copy
typedef struct { uint8_t *arc; size_t an; Entry *in; int nin, ibfres; uint8_t *bfres; size_t bn; } ModelZs;
static bool model_open(ModelZs *m, const uint8_t *z, size_t zn) {
    memset(m, 0, sizeof *m);
    m->ibfres = -1;
    m->arc = zstd_read(z, zn, &m->an);
    m->nin = m->arc ? sarc_read(m->arc, m->an, &m->in) : 0;
    for (int i = 0; i < m->nin; i++)
        if (strstr(m->in[i].name, ".bfres")) m->ibfres = i;
    if (m->ibfres < 0) return false;
    m->bn = m->in[m->ibfres].n;
    m->bfres = malloc(m->bn);
    memcpy(m->bfres, m->in[m->ibfres].d, m->bn);
    return true;
}
static void model_close(ModelZs *m) { free(m->arc), free(m->in), free(m->bfres); }
static uint8_t *model_store(const ModelZs *m, size_t *zn) {  // the archive again, with the edited BFRES, compressed
    const char *names[8];
    int ids[8];
    const uint8_t *d[8];
    size_t dn[8];
    int ni = m->nin < 8 ? m->nin : 8;
    for (int i = 0; i < ni; i++) {
        names[i] = m->in[i].name, ids[i] = i;
        d[i] = i == m->ibfres ? m->bfres : m->in[i].d, dn[i] = i == m->ibfres ? m->bn : m->in[i].n;
    }
    size_t an;
    uint8_t *arc = sarc_write(ni, names, ids, ni, d, dn, 0x1000, &an);
    uint8_t *zs = zstd_store(arc, an, zn);
    free(arc);
    return zs;
}

// ---- the recolour map (Super Mario's colour -> the character's)
#define PAL_MAX 64
typedef struct { uint32_t from, to; } PalEntry;
typedef struct { uint32_t from, to; int count; } PalVote;
typedef struct {
    const uint8_t *other;  // the character's BFRES BNTX (read only: textures looked up by name)
    size_t other_n;
    PalVote votes[512];
    int nvotes;
} PalCtx;
typedef struct { const char *want; uint8_t *data; int w, h, bhl; } TexFind;
static void tex_find(void *ctx, const char *name, uint8_t *data, int w, int h, int bhl) {
    TexFind *f = ctx;
    if (!f->data && !strcmp(name, f->want)) f->data = data, f->w = w, f->h = h, f->bhl = bhl;
}
static uint32_t px32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void pal_vote(void *ctx, const char *name, uint8_t *data, int w, int h, int bhl) {
    PalCtx *p = ctx;
    TexFind f = {name, NULL, 0, 0, 0};
    bntx_each((uint8_t *)p->other, p->other_n, tex_find, &f);
    if (!f.data || f.w != w || f.h != h) return;
    uint8_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4);
    tegra_deswizzle(data, a, (uint32_t)w, (uint32_t)h, 1, 4, (uint32_t)bhl, 0);
    tegra_deswizzle(f.data, b, (uint32_t)w, (uint32_t)h, 1, 4, (uint32_t)f.bhl, 0);
    for (int i = 0; i < w * h; i++) {
        if (!a[4 * i + 3]) continue;
        uint32_t x = px32(a + 4 * i), y = px32(b + 4 * i);
        int k = 0;
        while (k < p->nvotes && (p->votes[k].from != x || p->votes[k].to != y)) k++;
        if (k == p->nvotes) {
            if (p->nvotes == 512) continue;
            p->votes[p->nvotes++] = (PalVote){x, y, 0};
        }
        p->votes[k].count++;
    }
    free(a), free(b);
}
// from the votes: each source colour's most frequent target. Only for a character drawn like Mario (Luigi): where
// the shapes differ (Toad), the same pixel is often another part of the body, the votes disagree, and there is no map
// (0: Mario's colours are kept)
static int pal_build(const PalCtx *p, PalEntry *out) {
    int n = 0, agree = 0, all = 0;
    for (int i = 0; i < p->nvotes && n < PAL_MAX; i++) {
        int k = 0;
        while (k < n && out[k].from != p->votes[i].from) k++;
        if (k < n) continue;
        const PalVote *best = &p->votes[i];
        int total = p->votes[i].count;
        for (int j = i + 1; j < p->nvotes; j++)
            if (p->votes[j].from == best->from) {
                total += p->votes[j].count;
                if (p->votes[j].count > best->count) best = &p->votes[j];
            }
        agree += best->count, all += total;
        out[n++] = (PalEntry){best->from, best->to};
    }
    return all && agree * 10 >= all * 9 ? n : 0;
}
static uint32_t pal_map(const PalEntry *pal, int n, uint32_t c, int *missed) {
    if (!(c >> 24)) return c;  // (transparent)
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (pal[i].from == c) return pal[i].to;
        int d = 0;
        for (int s = 0; s < 24; s += 8) {
            int x = (int)(c >> s & 255) - (int)(pal[i].from >> s & 255);
            d += x < 0 ? -x : x;
        }
        if (d < bd) bd = d, best = i;
    }
    if (best >= 0 && bd <= 96) return pal[best].to;
    (*missed)++;
    return c;
}

// ---- the Slim frames into a Super model's textures
typedef struct { const Bfres *slim; const PalEntry *pal; int npal, done, missed; } SlimCtx;
static void slim_put(void *ctx, const char *name, uint8_t *data, int w, int h, int bhl) {
    SlimCtx *s = ctx;
    const char *want = slim_frame_for(name);
    int id = bfres_texture(s->slim, want);
    if (id < 0 && !strcmp(want, "jump_fall.0")) id = bfres_texture(s->slim, "jump.0");
    if (id < 0) id = bfres_texture(s->slim, "wait.0");
    const LTex *t = lyt_tex(id);
    if (!t || !t->px) return;
    uint8_t *lin = calloc((size_t)w * h, 4);
    for (int y = 0; y < h && y < t->h; y++)
        for (int x = 0; x < w && x < t->w; x++) {
            uint32_t c = px32(t->px + ((size_t)y * t->w + x) * 4);
            if (s->pal) c = pal_map(s->pal, s->npal, c, &s->missed);
            w32(lin + ((size_t)y * w + x) * 4, c);
        }
    tegra_swizzle(lin, data, (uint32_t)w, (uint32_t)h, 1, 4, (uint32_t)bhl, 0);
    free(lin);
    s->done++;
}

static Bfres *slim_bfres(void) {
    size_t pn, zn, an, bn;
    uint8_t *pack = smm1_dump_read("Pack/StaticSkin.pack", &pn);
    LSarc ps, inner;
    Bfres *b = NULL;
    if (pack && lyt_sarc_open(&ps, pack, pn)) {
        const uint8_t *z = lyt_sarc_get(&ps, "Model/M1_Player_SlimMarioMdl.szs", &zn);
        uint8_t *arc = z ? lyt_yaz0(z, zn, &an) : NULL;
        if (arc && lyt_sarc_open(&inner, arc, an)) {
            const uint8_t *bf = lyt_sarc_get(&inner, "Output.bfres", &bn);
            if (bf) b = bfres_load(bf, bn);
            lyt_sarc_close(&inner);
        }
        free(arc);
        lyt_sarc_close(&ps);
    }
    free(pack);
    return b;
}

#ifdef SMM1UI_STANDALONE
static uint8_t *lanky_read_pack(size_t *n) {  // (the costumes' test pack if made, else the game's)
    uint8_t *d = smm1_read_file(OUT_PACK, n);
    return d ? d : read_pack(n);
}
#else
#define lanky_read_pack read_pack
#endif

bool lanky_generate(void) {
    Bfres *slim = slim_bfres();
    if (!slim) {
        fprintf(stderr, LOGC "Lanky: SMM1's Model/M1_Player_SlimMarioMdl.szs not found (Pack/StaticSkin.pack of the "
                             "SMM1 dump: RECOMP_SMM1_DUMP)\n");
        return false;
    }
    size_t pn;
    uint8_t *pack = lanky_read_pack(&pn);
    if (!pack) { fprintf(stderr, LOGC "Lanky: SMM2's Pack/M1_Model.pack not found\n"); return false; }
    Entry *orig = NULL;
    int norig = sarc_read(pack, pn, &orig);
    static const char *suffix[4] = {"", "_2", "_3", "_4"};
    const uint8_t *tz[4] = {0};
    size_t tzn[4] = {0};
    for (int s = 0; s < 4; s++) {
        char want[96];
        snprintf(want, sizeof want, "Model/M1_Player_SpMarioMdl%s.Nin_NX_NVN.zs", suffix[s]);
        for (int i = 0; i < norig; i++)
            if (!strcmp(orig[i].name, want)) tz[s] = orig[i].d, tzn[s] = orig[i].n;
    }
    ModelZs mario;
    if (!tz[0] || !model_open(&mario, tz[0], tzn[0])) {
        fprintf(stderr, LOGC "Lanky: SMM2's Super SMB1 Mario model not readable\n");
        free(pack), free(orig);
        return false;
    }
    size_t mbn;
    uint8_t *mb = bfres_bntx(mario.bfres, mario.bn, &mbn);
    // the new pack: the files already there, then one Slim model per character (Mario's own colours for a character
    // whose Super model or palette is missing)
    int nn = 0, nblob = 0;
    const char *names[1024 + 4];
    int blob_of[1024 + 4];
    const uint8_t *blob[1024 + 4];
    size_t blob_n[1024 + 4];
    char snames[4][96];
    if (norig > 1024) norig = 1024;
    for (int i = 0; i < norig; i++)
        if (strncmp(orig[i].name, "Model/M1_Player_SlimMarioMdl", 28))  // (an earlier run's: made again below)
            names[nn] = orig[i].name, blob_of[nn++] = nblob, blob[nblob] = orig[i].d, blob_n[nblob++] = orig[i].n;
    int nkept = nblob, made = 0, mario_blob = -1;
    for (int s = 0; s < 4; s++) {
        snprintf(snames[s], sizeof snames[s], "Model/M1_Player_SlimMarioMdl%s.Nin_NX_NVN.zs", suffix[s]);
        ModelZs m;
        PalEntry pal[PAL_MAX];
        int npal = 0;
        size_t bn = 0;
        uint8_t *b = NULL;
        if (s > 0 && tz[s] && model_open(&m, tz[s], tzn[s]) && (b = bfres_bntx(m.bfres, m.bn, &bn)) && mb) {
            // the character's colours: Super Mario's texture vs the character's, pixel by pixel
            static PalCtx pc;
            memset(&pc, 0, sizeof pc);
            pc.other = b, pc.other_n = bn;
            uint8_t *mcopy = malloc(mbn);
            memcpy(mcopy, mb, mbn);
            bntx_each(mcopy, mbn, pal_vote, &pc);
            free(mcopy);
            npal = pal_build(&pc, pal);
        } else if (s > 0 && tz[s])
            model_close(&m);
        if (s > 0 && !npal) {  // (Mario's Slim model under the character's name: Toad, Toadette)
            if (tz[s]) model_close(&m);
            if (mario_blob < 0) continue;
            fprintf(stderr, LOGC "Lanky: Slim%s: Mario's (the character isn't drawn like Mario)\n", suffix[s]);
            names[nn] = snames[s], blob_of[nn++] = mario_blob, made++;
            continue;
        }
        if (s == 0) {
            model_open(&m, tz[0], tzn[0]);
            b = bfres_bntx(m.bfres, m.bn, &bn);
        }
        SlimCtx sc = {slim, npal ? pal : NULL, npal, 0, 0};
        int nt = b ? bntx_each(b, bn, slim_put, &sc) : -1;
        size_t zn = 0;
        uint8_t *zs = nt > 0 ? model_store(&m, &zn) : NULL;
        model_close(&m);
        if (!zs) { fprintf(stderr, LOGC "Lanky: the Slim model%s could not be made\n", suffix[s]); continue; }
        if (s > 0) fprintf(stderr, LOGC "Lanky: Slim%s: %d textures recoloured%s\n", suffix[s], sc.done,
                           sc.missed ? " (some pixels kept Mario's colours)" : "");
        if (s == 0) mario_blob = nblob;
        names[nn] = snames[s], blob_of[nn++] = nblob, blob[nblob] = zs, blob_n[nblob++] = zn;
        made++;
    }
    model_close(&mario);
    size_t on = 0;
    uint8_t *out = made ? sarc_write(nn, names, blob_of, nblob, blob, blob_n, 0x80, &on) : NULL;
#ifdef SMM1UI_STANDALONE
    bool ok = out && write_file(OUT_PACK, out, on);
#else
    bool ok = out && rov_write(PACK, out, on);
#endif
    fprintf(stderr, LOGC "Lanky: %d Slim Mario models generated into " PACK "\n", made);
    for (int b = nkept; b < nblob; b++) free((void *)blob[b]);
    free(out), free(pack), free(orig);
    return ok && made == 4;
}

#ifdef SMM1UI_STANDALONE
// (test_smm1ui --lanky <sheet.png>: the generated pack's Super and Slim models, a row each, every texture side by side)
typedef struct { uint8_t *sheet; int sw, row, col; } SheetCtx;
static void sheet_put(void *ctx, const char *name, uint8_t *data, int w, int h, int bhl) {
    SheetCtx *s = ctx;
    if (getenv("LANKY_SHEET_DIMS")) fprintf(stderr, "row %d %s %dx%d bhl %d\n", s->row, name, w, h, bhl);
    uint8_t lin[64 * 64 * 4];
    tegra_deswizzle(data, lin, (uint32_t)w, (uint32_t)h, 1, 4, (uint32_t)bhl, 0);
    for (int y = 0; y < h && y < 32; y++)
        for (int x = 0; x < w && x < 16; x++)
            memcpy(s->sheet + ((size_t)(s->row * 34 + y) * s->sw + s->col * 18 + x) * 4, lin + ((size_t)y * w + x) * 4, 4);
    s->col++;
}
int write_png(const char *path, const uint8_t *rgba, int w, int h);
bool lanky_sheet(const char *path) {
    size_t pn;
    uint8_t *pack = smm1_read_file(OUT_PACK, &pn);
    Entry *e = NULL;
    int n = pack ? sarc_read(pack, pn, &e) : 0;
    static const char *rows[8] = {"SpMarioMdl", "SlimMarioMdl", "SpMarioMdl_2", "SlimMarioMdl_2",
                                  "SpMarioMdl_3", "SlimMarioMdl_3", "SpMarioMdl_4", "SlimMarioMdl_4"};
    SheetCtx s = {NULL, 24 * 18, 0, 0};
    s.sheet = calloc((size_t)s.sw * 8 * 34, 4);
    for (int r = 0; r < 8; r++) {
        char want[96];
        snprintf(want, sizeof want, "Model/M1_Player_%s.Nin_NX_NVN.zs", rows[r]);
        for (int i = 0; i < n; i++) {
            ModelZs m;
            size_t bn;
            uint8_t *b;
            if (strcmp(e[i].name, want) || !model_open(&m, e[i].d, e[i].n)) continue;
            if ((b = bfres_bntx(m.bfres, m.bn, &bn))) s.row = r, s.col = 0, bntx_each(b, bn, sheet_put, &s);
            model_close(&m);
        }
    }
    bool ok = !write_png(path, s.sheet, s.sw, 8 * 34);
    free(s.sheet), free(e), free(pack);
    return ok;
}
#endif

#ifndef SMM1UI_STANDALONE
bool lanky_on(void);  // (mods/lanky.c: the Slim models only with the mod on)
static bool lanky_overlay(void) { return !lanky_on() || lanky_generate(); }
ROV_GENERATOR("lanky", 1, lanky_overlay)

// ---------------------------------------------------------------- a costume's look, as its model's textures hold it
// (mods/costume.c: another costume or the taunt pose mid-course, written over the loaded model's textures)
// The texel data of the costume's generated model, read back from the overlay's pack: one TEX_BYTES block per texture
// in the BNTX's order (the order the game makes the textures in, each block where the game's texture reads it); with
// appeal, every block is the costume's appeal frame instead. The count, 0 if there is no such costume; *out malloc'd.
// (Game thread only: the pack stays loaded once read.)
int costume_look(const char *res, bool appeal, uint8_t **out) {
    static RovSarc *pack;
    static RovBuf frames;
    *out = NULL;
    if (!pack) pack = rov_sarc_load(PACK);
    char name[160];
    snprintf(name, sizeof name, "Model/M1_Player_Chara_%s.Nin_NX_NVN.zs", res);
    RovBuf *z = pack ? rov_sarc_member(pack, name) : NULL;
    ModelZs m;
    if (!z || !model_open(&m, z->d, z->n)) {
        if (z) model_close(&m);
        return 0;
    }
    size_t bn;
    const uint8_t *b = bfres_bntx(m.bfres, m.bn, &bn);
    int count = 0;
    if (b && bn >= 0x60 && !memcmp(b, "BNTX", 4)) {
        uint32_t k = le32(b + 0x24);
        size_t brtd = (size_t)le64(b + 0x30);
        if (k && k <= 64 && brtd + 0x10 + (size_t)k * TEX_BYTES <= bn && !memcmp(b + brtd, "BRTD", 4)) {
            count = (int)k;
            *out = malloc((size_t)k * TEX_BYTES);
            memcpy(*out, b + brtd + 0x10, (size_t)k * TEX_BYTES);
        }
    }
    model_close(&m);
    if (count && appeal) {
        if (!frames.d && !rov_read(APPEAL, &frames)) frames.n = 0;
        const uint8_t *f = NULL;
        for (size_t o = 0; o + APPEAL_REC <= frames.n && !f; o += APPEAL_REC)
            if (!strncmp((const char *)frames.d + o, res, 48)) f = frames.d + o + 48;
        if (!f) { free(*out), *out = NULL; return 0; }
        for (int i = 0; i < count; i++) memcpy(*out + (size_t)i * TEX_BYTES, f, TEX_BYTES);
    }
    return count;
}
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
