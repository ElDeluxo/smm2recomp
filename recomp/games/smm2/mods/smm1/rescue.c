// SMM1's rescue scene (rescue.h), as Block.rpx v208 plays it [obs, research/specs/smm1-ending.md §2]:
//   - DemoPeach at (416, 48): <Style>_Object_peach's peach_model (Normal, Expert, Super Expert) or kinopio_model
//     (Easy), with field_model (the ground) at (0, 48, -1000); the wait_peach / wait_kinopio texture pattern plays
//     in SMW and NSMBU only
//   - demo Mario at (-32, 47): waits 30 frames, walks right 1.5 units a frame ('run', rate 1.125) until x = 384,
//     stands ('wait') 30 frames, then the balloon text runs 360 frames (Out at 300), then 120 more frames (300 in
//     NSMBU with Peach) before the credits are asked for
//   - orthographic camera centred on Mario, clamped so the 384 x 216 view [inf] stays inside x 0-1152, y 0-432
//   - Mario as he reached the last goal: his power-up's model (Fire, SMB3's Raccoon, SMB1's Big Mushroom, SMW's cape,
//     NSMBU's Fire and Propeller; Super Mario's for the forms SMM1 doesn't have), in a Goomba's Shoe / Stiletto
//     hopping all the way (SMB1, SMB3) or on Yoshi (SMW) [obs: smm1-ending.md §2.4]
// Beyond SMM1 (owner, 2026-10-30): the backdrop is the course theme's distant view (SMM1's sky for ground, airship and
// SMM2's own themes), and in co-op Luigi walks a step behind Mario. Not drawn: NSMBU's Yoshi, Toad and Toadette.
// Positions are course units (16 a block); the view fills the 1280 x 720 layout space.
#include "rescue.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bfres.h"
#include "smm1_data.h"

#define VIEW_W 384.0f
#define VIEW_H 216.0f

// One demo player: Mario, and in co-op Luigi walking a step behind him (Toad and Toadette have no SMM1 art: left out).
// The power-up's model in the scene's style (SMM1's own forms: Super Mario's model for the forms SMM1 doesn't have),
// what carries him (a Goomba's Shoe in SMB1 / SMB3, Yoshi in SMW, as 0x025ee86c does), and the shoe's hop, simulated
// tick by tick.
typedef struct {
    int form, mount;  // SMM2's (hundred.h)
    bool luigi;       // Mario's art with his reds made green [departure: SMM1 has no Luigi]
    bool cape;        // SMW's cape: Super Mario with MW_Player_Mant
    float dx;         // his place in the line (Mario 0)
    Bfres *mario;
    const BModel *mdl;
    int sim_t;  // the shoe's hop up to this tick (0x025eefbc, 0x025eed54)
    float y, vy;
    bool hop_done, falling;
    const char *anim;  // the shoe's pose ("shoes_jump" / "shoes_jumpup" / "shoes_wait"), NULL for the walk's
} Walker;

struct Rescue {
    int style, theme;
    bool peach, own_sky;  // own_sky: the style's sky (SMM1's stage); else the course theme's distant view
    Bfres *obj, *sky;     // sky: the distant view behind the stage (<Style>_DV_sky, or the theme's)
    Bfres *common, *cape, *yoshi, *shoe[2];  // <Style>_Player_CommonAnm, MW_Player_Mant, MW_Player_Yoshi, the shoes
    const BModel *field, *npc;
    const BTexPat *npc_wait;
    Walker w[2];
    int nw;
    int walk_end;  // the frame Mario reaches x = 384
};

static const char *STYLE[4] = {"M1", "M3", "MW", "WU"};

static Bfres *load(const char *name) {
    size_t n;
    uint8_t *d = smm1_model(name, &n);
    Bfres *b = d ? bfres_load(d, n) : NULL;
    free(d);  // (bfres_load decoded what it keeps)
    if (!b) fprintf(stderr, "[smm1ui] rescue: model %s missing\n", name);
    return b;
}

// the 2-D styles' player model for SMM2's form (hundred.h) [inf: SMM2's forms onto SMM1's models]
static const char *model_2d(int style, int form) {
    switch (form) {
    case 0: return "MarioMdl";
    case 2: return "FireMarioMdl";
    case 5: return style == 1 ? "TnkMarioMdl" : "SpMarioMdl";  // (SMB3's Raccoon)
    case 6: return style == 0 ? "MegaMarioMdl" : "SpMarioMdl";  // (SMB1's Big Mushroom)
    default: return "SpMarioMdl";                               // (the cape is Super Mario with the cape on)
    }
}

// the theme's distant view in SMM1's Model/ (<Style>_DV_<name>): ground, airship and SMM2's own themes keep SMM1's sky
static const char *theme_view(int theme) {
    switch (theme) {
    case 1: return "underground";
    case 2: return "castle";
    case 4: return "water";
    case 5: return "hauntedhouse";
    default: return "sky";
    }
}

Rescue *rescue_new(int style, bool peach, const struct hundred_look *look) {
    Rescue *r = calloc(1, sizeof *r);
    r->style = style < 0 || style > 3 ? 0 : style, r->peach = peach;
    r->theme = look ? look->theme : 0;
    char name[64];
    snprintf(name, sizeof name, "%s_Object_peach", STYLE[r->style]);
    r->obj = load(name);
    r->own_sky = !strcmp(theme_view(r->theme), "sky");
    if (r->style >= 1 || !r->own_sky) {  // (SMB1's sky is part of its ground model)
        snprintf(name, sizeof name, "%s_DV_%s", STYLE[r->style], theme_view(r->theme));
        size_t n;
        uint8_t *d = smm1_model(name, &n);  // (not every style has every theme's: none is a black backdrop)
        r->sky = d ? bfres_load(d, n) : NULL;
        free(d);
    }
    if (r->obj) {
        r->field = bfres_model(r->obj, "field_model");
        r->npc = bfres_model(r->obj, peach ? "peach_model" : "kinopio_model");
        if (r->style >= 2) r->npc_wait = bfres_texpat(r->obj, peach ? "wait_peach" : "wait_kinopio");
    }
    int players = look && look->players > 1 ? 2 : 1;
    for (int i = 0; i < players; i++) {
        Walker *w = &r->w[r->nw];
        w->form = look ? look->form[i] : 0, w->mount = look ? look->mount[i] : 0;
        w->luigi = i == 1, w->dx = i ? -26 : 0;
        if (r->style == 3) {
            snprintf(name, sizeof name, "WU_Player_MarioMdl");
            w->mount = 0;  // (NSMBU's Yoshi is a 3-D rig of its own: not drawn [inf: left out])
        } else {
            w->cape = r->style == 2 && w->form == 4;
            snprintf(name, sizeof name, "%s_Player_%s", STYLE[r->style], model_2d(r->style, w->form));
        }
        w->mario = load(name);
        if (!w->mario) {
            snprintf(name, sizeof name, "%s_Player_%s", STYLE[r->style], w->form ? "SpMarioMdl" : "MarioMdl");
            w->mario = load(name);
        }
        if (!w->mario) continue;
        w->mdl = w->mario->nmodels ? &w->mario->models[0] : NULL;
        r->nw++;
    }
    bool shoe = false, yoshi = false, cape = false;
    for (int i = 0; i < r->nw; i++) {
        shoe |= r->w[i].mount && r->style <= 1, yoshi |= r->w[i].mount && r->style == 2, cape |= r->w[i].cape;
    }
    if (r->style <= 2) {
        snprintf(name, sizeof name, "%s_Player_CommonAnm", STYLE[r->style]);
        r->common = load(name);
    }
    if (cape) r->cape = load("MW_Player_Mant");
    if (yoshi) r->yoshi = load("MW_Player_Yoshi");
    for (int k = 0; shoe && k < 2; k++) {
        snprintf(name, sizeof name, "%s_Enemy_kutsu_kuribo%c", STYLE[r->style], 'A' + k);
        r->shoe[k] = load(name);
    }
    // walk: x from -32 by 1.5 a frame; the step that would reach 384 sets x = 384
    r->walk_end = 30 + (int)ceilf((384 - 1.5f + 32) / 1.5f);
    return r;
}
void rescue_free(Rescue *r) { free(r); }  // (the models' textures stay in the texture table)

int rescue_text_in(const Rescue *r) { return r->walk_end + 30; }
int rescue_text_out(const Rescue *r) { return rescue_text_in(r) + 300; }
int rescue_length(const Rescue *r) { return rescue_text_in(r) + 360 + (r->style == 3 && r->peach ? 300 : 120); }

void rescue_balloon_anchor(const Rescue *r, float xy[2]) {
    (void)r;  // the same point in every style and for Toad: about 40 units above DemoPeach's spot (416, 48), with the
              // camera at its end (Mario at 384) [obs: Cemu, SMB1 and NSMBU captures, balloon box 186-427 high]
    float camx = fminf(fmaxf(384, VIEW_W / 2), 1152 - VIEW_W / 2), camy = fminf(fmaxf(47, VIEW_H / 2), 432 - VIEW_H / 2);
    xy[0] = (416 - 1.8f - camx) * (1280 / VIEW_W);
    xy[1] = (48 + 39.3f - camy) * (720 / VIEW_H);
}

static float mario_x(const Rescue *r, int t) {
    if (t < 30) return -32;
    float x = -32 + 1.5f * (float)(t - 30);
    return t >= r->walk_end || x > 384 ? 384 : x;
}

// The Goomba's Shoe's hop [obs, smm1-ending.md §2.4]: PlayerWalk's enter (tick 30) starts it at vy 2.5 with
// `shoes_jump`; each tick of the walk and the stop the speed takes the gravity -0.34 (fastest fall -4), the first
// falling tick sets `shoes_jump` again, and on landing he hops again (`shoes_jumpup`) while walking, else lands into
// `shoes_wait` and stops. Simulated up to tick t (again from the start if t went back).
static void walker_hop(const Rescue *r, Walker *w, int t) {
    if (!w->mount || r->style > 1) return;
    if (t < w->sim_t) w->sim_t = 0, w->y = w->vy = 0, w->hop_done = w->falling = false, w->anim = NULL;
    for (int k = w->sim_t + 1; k <= t; k++) {
        if (k == 30) {
            w->vy = 2.5f, w->falling = false, w->anim = "shoes_jump";
            continue;
        }
        if (k < 30 || w->hop_done) continue;
        w->vy = fmaxf(w->vy - 0.34f, -4.0f);
        w->y += w->vy;
        if (!w->falling && w->vy < 0) w->falling = true, w->anim = "shoes_jump";
        if (w->y < 0) {
            w->y = 0, w->falling = false;
            if (k < r->walk_end) w->vy = 2.5f, w->anim = "shoes_jumpup";
            else w->anim = "shoes_wait", w->hop_done = true;
        }
    }
    w->sim_t = t;
}

// a pattern of the player's model, else the style's shared player animations (which name the model's textures)
static const BTexPat *pattern(const Rescue *r, const Bfres *b, const char *name) {
    const BTexPat *p = b ? bfres_texpat(b, name) : NULL;
    return p ? p : r->common ? bfres_texpat(r->common, name) : NULL;
}
static float pat_frame(const BTexPat *p, float f) { return p && p->frames > 0 ? fmodf(f, (float)p->frames) : 0; }

// Co-op's Luigi: Mario's textures with their reds made green (the red texels: red over 1.6 times green and blue),
// one copy a texture [departure: SMM1 has no Luigi; inf: his cap and shirt green, the rest Mario's]
static int g_recolour;  // (set while Luigi is drawn)
static int luigi_tex(int tex) {
    static struct { int from, to; } map[96];
    static int n;
    if (tex < 0) return tex;
    for (int i = 0; i < n; i++)
        if (map[i].from == tex) return map[i].to;
    const LTex *t = lyt_tex(tex);
    if (!t || !t->px || n == 96) return tex;
    uint8_t *px = malloc((size_t)t->w * t->h * 4);
    memcpy(px, t->px, (size_t)t->w * t->h * 4);
    for (size_t i = 0; i < (size_t)t->w * t->h; i++) {
        uint8_t *c = px + i * 4;
        if (c[0] > 40 && c[0] * 5 > c[1] * 8 && c[0] * 5 > c[2] * 8) {
            uint8_t g = c[1];
            c[1] = c[0], c[0] = g;
        }
    }
    int id = lyt_tex_add(t->w, t->h, px);
    if (t->srgb) lyt_tex_srgb(id);
    map[n].from = tex, map[n].to = id, n++;
    return id;
}

// one model's shapes at a world placement, through the camera, as triangles; pose: its bones' matrices (a skeletal
// animation's frame), NULL for the rest pose; skip: a shape left out (by name), NULL for none
static void draw_model_ex(const Bfres *b, const BModel *m, const BTexPat *pat, float frame, float px, float py, float pz,
                          float camx, float camy, bool mirror, bool smooth, uint32_t blend, uint8_t alpha,
                          float (*pose)[3][4], const char *skip, LQuads *out);
static void draw_model(const Bfres *b, const BModel *m, const BTexPat *pat, float frame, float px, float py, float pz,
                       float camx, float camy, bool mirror, bool smooth, LQuads *out) {
    draw_model_ex(b, m, pat, frame, px, py, pz, camx, camy, mirror, smooth, LQ_BLEND_NORMAL, 255, NULL, NULL, out);
}
static void draw_model_ex(const Bfres *b, const BModel *m, const BTexPat *pat, float frame, float px, float py, float pz,
                          float camx, float camy, bool mirror, bool smooth, uint32_t blend, uint8_t alpha,
                          float (*pose)[3][4], const char *skip, LQuads *out) {
    if (!m) return;
    const float kx = 1280 / VIEW_W, ky = 720 / VIEW_H;
    // back to front within the model (the SMB1 sky sits at z -1 behind the ground)
    int order[64], n = m->nshapes < 64 ? m->nshapes : 64;
    float zs[64];
    for (int i = 0; i < n; i++) {
        const BShape *s = &m->shapes[i];
        float z = s->nverts ? s->pos[0][2] : 0;
        if (s->bone >= 0 && s->bone < m->nbones) z += (pose ? pose[s->bone] : m->bones[s->bone].world)[2][3];
        zs[i] = z, order[i] = i;
    }
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && zs[order[k]] < zs[order[k - 1]]; k--) {
            int tmp = order[k];
            order[k] = order[k - 1], order[k - 1] = tmp;
        }
    for (int oi = 0; oi < n; oi++) {
        const BShape *s = &m->shapes[order[oi]];
        if (!s->idx || !s->pos || s->mat >= m->nmats) continue;
        if (skip && (!strcmp(s->name, skip) || (s->bone >= 0 && s->bone < m->nbones && !strcmp(m->bones[s->bone].name, skip))))
            continue;
        const BMat *mt = &m->mats[s->mat];
        const char *texname = pat ? bfres_texpat_at(pat, mt->name, frame) : NULL;
        int tex = texname ? bfres_texture(b, texname) : -1;
        if (tex < 0 && mt->ntex) tex = bfres_texture(b, mt->tex[0]);
        if (g_recolour) tex = luigi_tex(tex);
        const float (*w)[4] = s->bone < 0 || s->bone >= m->nbones ? NULL
                            : pose ? (const float (*)[4])pose[s->bone] : (const float (*)[4])m->bones[s->bone].world;
        for (int i = 0; i + 2 < s->nidx; i += 3) {
            LQuad q;
            memset(&q, 0, sizeof q);
            float P[3][2], U[3][2];
            for (int k = 0; k < 3; k++) {
                const float *v = s->pos[s->idx[i + k] < (uint32_t)s->nverts ? s->idx[i + k] : 0];
                float x = v[0], y = v[1], z = v[2];
                if (w) {
                    float wx = w[0][0] * x + w[0][1] * y + w[0][2] * z + w[0][3];
                    float wy = w[1][0] * x + w[1][1] * y + w[1][2] * z + w[1][3];
                    x = wx, y = wy;
                }
                if (mirror) x = -x;
                P[k][0] = (x + px - camx) * kx, P[k][1] = (y + py - camy) * ky;
                const float *uv = s->uv[s->idx[i + k] < (uint32_t)s->nverts ? s->idx[i + k] : 0];
                U[k][0] = uv[0] * mt->srt[0] + mt->srt[3], U[k][1] = uv[1] * mt->srt[1] + mt->srt[4];
            }
            (void)pz;
            // corners TL, TR, BL = vertices 0, 1, 2; BR = TR + BL - TL (the rasterizer keeps the half s + t < 1)
            for (int k = 0; k < 3; k++) {
                q.pos[k][0] = P[k][0], q.pos[k][1] = P[k][1];
                q.uv0[k][0] = U[k][0], q.uv0[k][1] = U[k][1];
            }
            q.pos[3][0] = P[1][0] + P[2][0] - P[0][0], q.pos[3][1] = P[1][1] + P[2][1] - P[0][1];
            q.uv0[3][0] = U[1][0] + U[2][0] - U[0][0], q.uv0[3][1] = U[1][1] + U[2][1] - U[0][1];
            memset(q.col, 255, sizeof q.col);
            for (int k = 0; k < 4; k++) q.col[k][3] = alpha;
            memset(q.black, 0, 4), memset(q.white, 255, 4);
            q.tex0 = tex, q.tex1 = -1;
            q.flags = LQ_TRI | blend | (smooth ? LQ_LINEAR : 0) | (uint32_t)(mt->wrap_s[0] & 3) << LQ_WRAP_S0 |
                      (uint32_t)(mt->wrap_t[0] & 3) << LQ_WRAP_T0 | LQ_COMB_NONE << LQ_COMB_C | LQ_COMB_NONE << LQ_COMB_A;
            if (out->n == out->cap) out->cap = out->cap ? out->cap * 2 : 256, out->q = realloc(out->q, sizeof(LQuad) * (size_t)out->cap);
            out->q[out->n++] = q;
        }
    }
}

// NSMBU: Mario is a skinned 3-D model (body + head) with skeletal animations ('walk' at rate 3.0, 'wait'). It is
// drawn into an image of its own with a depth buffer and simple lighting (2x supersampled), then put in the scene as
// one quad. The camera is the scene's orthographic one [inf: the game uses a perspective camera here]; the lighting
// is a guess fitted to SMM1's frames (Cemu) [inf].
typedef struct {
    float p[3][3];  // layout x, y and depth (larger = nearer)
    float uv[3][2];
    float shade[3];
    int tex;
} Tri3;
static void xform(const float m[3][4], const float v[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = m[k][0] * v[0] + m[k][1] * v[1] + m[k][2] * v[2] + m[k][3];
}
static void xform3(const float m[3][4], const float v[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = m[k][0] * v[0] + m[k][1] * v[1] + m[k][2] * v[2];
}
static float light(const float n[3]) {
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-6f) return 1;
    static const float L[3] = {-0.35f, 0.55f, 0.76f};  // from the upper left, in front
    float d = (n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / l;
    return 0.42f + 0.68f * (d > 0 ? d : 0);
}
static void mmul34(const float a[3][4], const float b[3][4], float o[3][4]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) o[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0);
}
static void inv34(const float m[3][4], float o[3][4]) {  // affine inverse
    float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-12f) { memset(o, 0, sizeof(float) * 12); o[0][0] = o[1][1] = o[2][2] = 1; return; }
    float id = 1 / det;
    o[0][0] = (e * i - f * h) * id, o[0][1] = (c * h - b * i) * id, o[0][2] = (b * f - c * e) * id;
    o[1][0] = (f * g - d * i) * id, o[1][1] = (a * i - c * g) * id, o[1][2] = (c * d - a * f) * id;
    o[2][0] = (d * h - e * g) * id, o[2][1] = (b * g - a * h) * id, o[2][2] = (a * e - b * d) * id;
    for (int r = 0; r < 3; r++) o[r][3] = -(o[r][0] * m[0][3] + o[r][1] * m[1][3] + o[r][2] * m[2][3]);
}
// NSMBU's Fire Mario: the same models with the second set of textures (player_body.1, player_head.1) [inf: the
// archive's .0 / .1 pairs]
static int g_tex_set;
static int tex_of(const Bfres *b, const char *name) {
    int tex = -1;
    size_t l = strlen(name);
    if (g_tex_set && l > 2 && !strcmp(name + l - 2, ".0")) {
        char alt[40];
        snprintf(alt, sizeof alt, "%.*s.%d", (int)l - 2, name, g_tex_set);
        tex = bfres_texture(b, alt);
    }
    if (tex < 0) tex = bfres_texture(b, name);
    return g_recolour ? luigi_tex(tex) : tex;
}
static void add_shapes(const Bfres *b, const BModel *m, float (*bones)[3][4], const float (*root)[4], float px, float py,
                       float camx, float camy, Tri3 **tris, int *n, int *cap) {
    const float kx = 1280 / VIEW_W, ky = 720 / VIEW_H;
    float (*inv)[3][4] = NULL;  // the bones' inverse rest (bind) matrices, for smooth skins
    for (int si = 0; si < m->nshapes && !inv; si++)
        if (m->shapes[si].skin_count >= 2) {
            inv = malloc(sizeof *inv * (size_t)(m->nbones + 1));
            for (int i = 0; i < m->nbones; i++) inv34(m->bones[i].world, inv[i]);
        }
    for (int si = 0; si < m->nshapes; si++) {
        const BShape *s = &m->shapes[si];
        if (!s->idx || !s->pos || s->mat >= m->nmats) continue;
        const BMat *mt = &m->mats[s->mat];
        if (!strcmp(mt->name, "mat_player_hair")) continue;  // (the hair without the cap: hidden under it) [inf]
        int tex = mt->ntex ? tex_of(b, mt->tex[0]) : -1;
        for (int i = 0; i + 2 < s->nidx; i += 3) {
            Tri3 t;
            for (int k = 0; k < 3; k++) {
                uint32_t vi = s->idx[i + k] < (uint32_t)s->nverts ? s->idx[i + k] : 0;
                int bone = s->skin_count >= 1 && s->idx0 ? bfres_skin_bone(m, s->idx0[vi][0]) : s->bone;
                if (bone < 0 || bone >= m->nbones) bone = 0;
                float a[3], c[3], na[3] = {0, 0, 1}, nc[3];
                if (s->skin_count >= 2 && s->idx0 && s->w0 && inv) {  // smooth skin: the bones' blend (from bind space)
                    memset(a, 0, sizeof a);
                    float nn[3] = {0, 0, 0};
                    for (int w = 0; w < s->skin_count && w < 4; w++) {
                        float wt = s->w0[vi][w];
                        if (wt <= 0) continue;
                        int bb = bfres_skin_bone(m, s->idx0[vi][w]);
                        if (bb < 0 || bb >= m->nbones) continue;
                        float sk[3][4], pp[3], np[3];
                        mmul34(bones[bb], inv[bb], sk);
                        xform(sk, s->pos[vi], pp);
                        for (int q = 0; q < 3; q++) a[q] += wt * pp[q];
                        if (s->nrm) {
                            xform3(sk, s->nrm[vi], np);
                            for (int q = 0; q < 3; q++) nn[q] += wt * np[q];
                        }
                    }
                    if (s->nrm) memcpy(na, nn, sizeof na);
                } else if (s->skin_count >= 2) {  // (no weights: the bind pose)
                    memcpy(a, s->pos[vi], sizeof a);
                    if (s->nrm) memcpy(na, s->nrm[vi], sizeof na);
                } else {
                    xform(bones[bone], s->pos[vi], a);
                    if (s->nrm) xform3(bones[bone], s->nrm[vi], na);
                }
                xform(root, a, c);
                xform3(root, na, nc);
                t.p[k][0] = (c[0] + px - camx) * kx, t.p[k][1] = (c[1] + py - camy) * ky, t.p[k][2] = c[2];
                t.uv[k][0] = s->uv[vi][0] * mt->srt[0] + mt->srt[3], t.uv[k][1] = s->uv[vi][1] * mt->srt[1] + mt->srt[4];
                t.shade[k] = s->nrm ? light(nc) : 1;
            }
            // back faces away (counter-clockwise triangles face the camera)
            float area = (t.p[1][0] - t.p[0][0]) * (t.p[2][1] - t.p[0][1]) - (t.p[2][0] - t.p[0][0]) * (t.p[1][1] - t.p[0][1]);
            if (area <= 0) continue;
            t.tex = tex;
            if (*n == *cap) *cap = *cap ? *cap * 2 : 1024, *tris = realloc(*tris, sizeof(Tri3) * (size_t)*cap);
            (*tris)[(*n)++] = t;
        }
    }
    free(inv);
}

// the images the players are drawn into: a ring of six, two a frame (the raster thread may still read the previous
// frame's)
#define MIMG_W 256
#define MIMG_H 384
#define MSS 2
#define MIMG_N 6
static int g_mimg[MIMG_N] = {-1, -1, -1, -1, -1, -1}, g_mimg_next;
static float g_dec_lut[256];
static float srgb_dec(uint8_t c) {
    if (g_dec_lut[255] == 0)
        for (int i = 0; i < 256; i++) {
            float f = i / 255.0f;
            g_dec_lut[i] = f <= 0.04045f ? f / 12.92f : powf((f + 0.055f) / 1.055f, 2.4f);
        }
    return g_dec_lut[c];
}
static uint8_t srgb_enc(float l) {
    l = l < 0 ? 0 : l > 1 ? 1 : l;
    float c = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1 / 2.4f) - 0.055f;
    return (uint8_t)(c * 255 + 0.5f);
}
static void tex_sample(const LTex *t, float u, float v, float out[4]) {
    if (!t || !t->px) { out[0] = out[1] = out[2] = out[3] = 1; return; }
    u -= floorf(u), v -= floorf(v);
    float fx = u * t->w - 0.5f, fy = v * t->h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0;
    int xa = (x0 % t->w + t->w) % t->w, xb = (xa + 1) % t->w, ya = (y0 % t->h + t->h) % t->h, yb = (ya + 1) % t->h;
    const uint8_t *q[4] = {t->px + ((size_t)ya * t->w + xa) * 4, t->px + ((size_t)ya * t->w + xb) * 4,
                           t->px + ((size_t)yb * t->w + xa) * 4, t->px + ((size_t)yb * t->w + xb) * 4};
    float w[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
    for (int c = 0; c < 4; c++) {
        float acc = 0;
        for (int k = 0; k < 4; k++) acc += w[k] * (c < 3 && t->srgb ? srgb_dec(q[k][c]) : q[k][c] / 255.0f);
        out[c] = acc;
    }
}
// the triangles into the image (pixel (x, y) of the image covers layout point (ox + x / MSS, oy - y / MSS))
static void raster3(const Tri3 *tris, int n, float ox, float oy, uint8_t *px, float *zb) {
    for (int i = 0; i < n; i++) {
        const Tri3 *t = &tris[i];
        float X[3], Y[3];
        for (int k = 0; k < 3; k++) X[k] = (t->p[k][0] - ox) * MSS, Y[k] = (oy - t->p[k][1]) * MSS;
        float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
        if (fabsf(area) < 1e-6f) continue;
        int x0 = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), x1 = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
        int y0 = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), y1 = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > MIMG_W) x1 = MIMG_W;
        if (y1 > MIMG_H) y1 = MIMG_H;
        const LTex *tx = lyt_tex(t->tex);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                float sx = x + 0.5f, sy = y + 0.5f;
                float w0 = ((X[1] - sx) * (Y[2] - sy) - (X[2] - sx) * (Y[1] - sy)) / area;
                float w1 = ((X[2] - sx) * (Y[0] - sy) - (X[0] - sx) * (Y[2] - sy)) / area;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * t->p[0][2] + w1 * t->p[1][2] + w2 * t->p[2][2];
                size_t o = (size_t)y * MIMG_W + x;
                if (z <= zb[o]) continue;
                float c[4];
                tex_sample(tx, w0 * t->uv[0][0] + w1 * t->uv[1][0] + w2 * t->uv[2][0],
                           w0 * t->uv[0][1] + w1 * t->uv[1][1] + w2 * t->uv[2][1], c);
                if (c[3] < 0.5f) continue;
                float sh = w0 * t->shade[0] + w1 * t->shade[1] + w2 * t->shade[2];
                zb[o] = z;
                for (int k = 0; k < 3; k++) px[o * 4 + k] = srgb_enc(c[k] * sh);
                px[o * 4 + 3] = 255;
            }
    }
}

// NSMBU's models for SMM2's form: small (SMB / SMH), Propeller (PLMB / PLMH), else big (MB / MH; Fire with its
// second textures) [inf: the forms SMM1's NSMBU has; the others big]
static void draw_wu_mario(Rescue *r, const Walker *wk, int t, float mx, float my, float camx, float camy, LQuads *out) {
    bool small = wk->form == 0, prop = wk->form == 3;
    const BModel *body = bfres_model(wk->mario, small ? "SMB_model" : prop ? "PLMB_model" : "MB_model");
    const BModel *head = bfres_model(wk->mario, small ? "SMH_model" : prop ? "PLMH_model" : "MH_model");
    if (!body) body = bfres_model(wk->mario, small ? "SMB_model" : "MB_model"), head = bfres_model(wk->mario, small ? "SMH_model" : "MH_model");
    if (!body) return;
    g_tex_set = wk->form == 2, g_recolour = wk->luigi;
    bool walking = t >= 30 && t < r->walk_end;
    const BSkelAnim *a = bfres_skelanim(wk->mario, walking ? "walk" : "wait");
    float frame = a && a->frames > 0 ? fmodf((float)(walking ? t - 30 : t >= r->walk_end ? t - r->walk_end : 0) * (walking ? 3.0f : 1.0f), (float)a->frames) : 0;
    float (*bones)[3][4] = calloc((size_t)body->nbones + 1, sizeof *bones);
    if (!small) bfres_pose(body, a, frame, bones);
    else bfres_pose_retarget(body, a, frame, bones);  // (the animations are the big skeleton's)
    // facing right: the player's yaw 0x2ccccccc (63 degrees from facing the camera) [obs 0x026e09b8]
    const float yaw = 0.175f * 6.2831853f, c = cosf(yaw), sn = sinf(yaw), k = 1.0f;
    const float root[3][4] = {{c * k, 0, sn * k, 0}, {0, k, 0, 0}, {-sn * k, 0, c * k, 0}};
    Tri3 *tris = NULL;
    int n = 0, cap = 0;
    add_shapes(wk->mario, body, bones, root, mx, my, camx, camy, &tris, &n, &cap);
    if (head) {  // the head on the body's face joint [inf]
        int fb = 0;
        for (int i = 0; i < body->nbones; i++)
            if (!strcmp(body->bones[i].name, "face_1")) fb = i;
        float (*hb)[3][4] = calloc((size_t)head->nbones + 1, sizeof *hb);
        bfres_pose(head, NULL, 0, hb);
        for (int i = 0; i < head->nbones; i++) {
            float o[3][4];
            for (int rr = 0; rr < 3; rr++)
                for (int c = 0; c < 4; c++)
                    o[rr][c] = bones[fb][rr][0] * hb[i][0][c] + bones[fb][rr][1] * hb[i][1][c] + bones[fb][rr][2] * hb[i][2][c] + (c == 3 ? bones[fb][rr][3] : 0);
            memcpy(hb[i], o, sizeof o);
        }
        add_shapes(wk->mario, head, hb, root, mx, my, camx, camy, &tris, &n, &cap);
        free(hb);
    }
    // the image: Mario's bounding box in layout units, MSS pixels a unit
    float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            bx0 = fminf(bx0, tris[i].p[k][0]), bx1 = fmaxf(bx1, tris[i].p[k][0]);
            by0 = fminf(by0, tris[i].p[k][1]), by1 = fmaxf(by1, tris[i].p[k][1]);
        }
    if (n && bx1 - bx0 < MIMG_W / MSS - 2 && by1 - by0 < MIMG_H / MSS - 2) {
        int slot = g_mimg_next;
        g_mimg_next = (g_mimg_next + 1) % MIMG_N;
        if (g_mimg[slot] < 0) {
            g_mimg[slot] = lyt_tex_add(MIMG_W, MIMG_H, calloc((size_t)MIMG_W * MIMG_H, 4));
            lyt_tex_srgb(g_mimg[slot]);
        }
        const LTex *img = lyt_tex(g_mimg[slot]);
        float ox = floorf(bx0) - 1, oy = ceilf(by1) + 1;
        float *zb = malloc(sizeof(float) * MIMG_W * MIMG_H);
        for (int i = 0; i < MIMG_W * MIMG_H; i++) zb[i] = -1e9f;
        memset(img->px, 0, (size_t)MIMG_W * MIMG_H * 4);
        raster3(tris, n, ox, oy, img->px, zb);
        free(zb);
        LQuad q;
        memset(&q, 0, sizeof q);
        float w = (float)MIMG_W / MSS, h = (float)MIMG_H / MSS;
        float P[4][2] = {{ox, oy}, {ox + w, oy}, {ox, oy - h}, {ox + w, oy - h}};
        float U[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        memcpy(q.pos, P, sizeof P), memcpy(q.uv0, U, sizeof U);
        memset(q.col, 255, sizeof q.col);
        memset(q.white, 255, 4);
        q.tex0 = g_mimg[slot], q.tex1 = -1;
        q.flags = LQ_LINEAR | LQ_BLEND_NORMAL | LQ_COMB_NONE << LQ_COMB_C | LQ_COMB_NONE << LQ_COMB_A;
        if (out->n == out->cap) out->cap = out->cap ? out->cap * 2 : 1024, out->q = realloc(out->q, sizeof(LQuad) * (size_t)out->cap);
        out->q[out->n++] = q;
    }
    free(tris);
    free(bones);
    g_tex_set = 0, g_recolour = 0;
}

// SMB3 / SMW: the distant view's 512 x 512 sky texture behind everything, one texel a unit, repeating sideways, fixed
// in the world and drifting 0.1 texel a frame [obs: Cemu, fitted to SMM1's frames: texel u = x + k + t / 10 with k
// 239 (SMB3) / 47 (SMW), v = 495.8 - y]
// Another theme's distant view (the course's theme, not SMM1's: departure): the view's own model (camera-facing quads
// in course units from y 0, some cut from a texture atlas, its tiles' pattern animation playing), drawn through the
// scene's camera without parallax [inf].
static void draw_view_model(const Rescue *r, int t, float camx, float camy, LQuads *out) {
    const BTexPat *p = r->sky->npat ? &r->sky->pats[0] : NULL;
    for (int i = 0; i < r->sky->nmodels; i++)
        draw_model(r->sky, &r->sky->models[i], p, pat_frame(p, (float)t), 0, 0, -2000, camx, camy, false, false, out);
}
static void draw_sky2d(const Rescue *r, int t, float camx, float camy, LQuads *out) {
    if (!r->sky || !r->sky->nmodels) return;
    const BModel *m = &r->sky->models[0];
    if (!m->nmats || !m->mats[0].ntex) return;
    if (!r->own_sky) {
        draw_view_model(r, t, camx, camy, out);
        return;
    }
    int tex = bfres_texture(r->sky, m->mats[0].tex[0]);
    const LTex *lt = lyt_tex(tex);
    if (!lt) return;
    float k = r->style == 1 ? 239 : 47;
    float x0 = camx - VIEW_W / 2, y1 = camy + VIEW_H / 2;
    float u0 = (x0 + k + (float)t / 10) / lt->w, v0 = (495.8f - y1) / lt->h;
    float du = VIEW_W / lt->w, dv = VIEW_H / lt->h;
    LQuad q;
    memset(&q, 0, sizeof q);
    float P[4][2] = {{-640, 360}, {640, 360}, {-640, -360}, {640, -360}};
    float U[4][2] = {{u0, v0}, {u0 + du, v0}, {u0, v0 + dv}, {u0 + du, v0 + dv}};
    memcpy(q.pos, P, sizeof P), memcpy(q.uv0, U, sizeof U);
    memset(q.col, 255, sizeof q.col);
    memset(q.white, 255, 4);
    q.tex0 = tex, q.tex1 = -1;
    q.flags = LQ_BLEND_NORMAL | 1u << LQ_WRAP_S0 | LQ_COMB_NONE << LQ_COMB_C | LQ_COMB_NONE << LQ_COMB_A;
    if (out->n == out->cap) out->cap = out->cap ? out->cap * 2 : 256, out->q = realloc(out->q, sizeof(LQuad) * (size_t)out->cap);
    out->q[out->n++] = q;
}

// NSMBU: WU_DV_sky is a 3-D distant view drawn by NSMBU's sky shaders (shading masks tinted by shader constants), not
// reproduced here. In this scene it does not move (no parallax, no drift over the walk [obs: Cemu]), so it is
// approximated as a fixed picture: the sky's gradient sampled from SMM1's frames and its cloud bank
// (Hightest_backLong2) over the lower half fading into the haze [inf: colours and placement fitted to Cemu frames].
static void lin_col(uint8_t out[4], int r, int g, int b, int a) {
    const int c[3] = {r, g, b};
    for (int k = 0; k < 3; k++) {
        float f = c[k] / 255.0f;
        f = f <= 0.04045f ? f / 12.92f : powf((f + 0.055f) / 1.055f, 2.4f);
        out[k] = (uint8_t)(f * 255 + 0.5f);
    }
    out[3] = (uint8_t)a;
}
static void push_quad(LQuads *out, const LQuad *q) {
    if (out->n == out->cap) out->cap = out->cap ? out->cap * 2 : 256, out->q = realloc(out->q, sizeof(LQuad) * (size_t)out->cap);
    out->q[out->n++] = *q;
}
static void band(LQuads *out, float y0, float y1, const int top[4], const int bot[4], int tex, float u0, float u1, float v0, float v1) {
    LQuad q;
    memset(&q, 0, sizeof q);
    float P[4][2] = {{-640, 360 - y0}, {640, 360 - y0}, {-640, 360 - y1}, {640, 360 - y1}};
    float U[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
    memcpy(q.pos, P, sizeof P), memcpy(q.uv0, U, sizeof U);
    lin_col(q.col[0], top[0], top[1], top[2], top[3]), lin_col(q.col[1], top[0], top[1], top[2], top[3]);
    lin_col(q.col[2], bot[0], bot[1], bot[2], bot[3]), lin_col(q.col[3], bot[0], bot[1], bot[2], bot[3]);
    memset(q.white, 255, 4);
    q.tex0 = tex, q.tex1 = -1;
    q.flags = LQ_LINEAR | LQ_BLEND_NORMAL | 1u << LQ_WRAP_S0 | 1u << LQ_WRAP_T0 | LQ_COMB_NONE << LQ_COMB_C |
              LQ_COMB_NONE << LQ_COMB_A;
    push_quad(out, &q);
}
// Another theme (the course's: departure): NSMBU's distant views are 3-D scenes of their own (WU_DV_<theme>, NSMBU's
// shaders), so each is stood in for by one of its pictures over the whole view [inf: the picture, its scale and tint
// chosen by eye]: the water's background picture, the castle's wall, the ghost house's wall, the cave's rock (grey,
// tinted by its shader in NSMBU: here a dark blue).
static bool draw_theme_wu(const Rescue *r, LQuads *out) {
    static const struct { int theme; const char *tex; float us, vs; int tint[4]; } T[] = {
        {4, "dv_suityu_haikei", 3.56f, 1, {255, 255, 255, 255}},
        {2, "DV_siro_Wall3", 2.5f, 1.4f, {255, 255, 255, 255}},
        {5, "kabe", 2.5f, 1.4f, {150, 135, 175, 255}},
        {1, "iwa_yokotex", 5, 2.8f, {70, 95, 125, 255}},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (T[i].theme != r->theme) continue;
        int tex = r->sky ? bfres_texture(r->sky, T[i].tex) : -1;
        if (tex < 0) return false;
        band(out, 0, 720, T[i].tint, T[i].tint, tex, 0, T[i].us, 0, T[i].vs);
        return true;
    }
    return false;
}
static void draw_sky_wu(const Rescue *r, LQuads *out) {
    if (!r->own_sky) {
        draw_theme_wu(r, out);  // (none of its pictures: black)
        return;
    }
    // the gradient (screen rows of the 1280 x 720 frame)
    static const int c0[4] = {20, 112, 220, 255}, c1[4] = {13, 156, 236, 255}, c2[4] = {120, 215, 250, 255},
                     c3[4] = {205, 255, 255, 255}, c4[4] = {212, 255, 255, 255};
    band(out, 0, 150, c0, c1, -1, 0, 1, 0, 1);
    band(out, 150, 300, c1, c2, -1, 0, 1, 0, 1);
    band(out, 300, 420, c2, c3, -1, 0, 1, 0, 1);
    band(out, 420, 720, c3, c4, -1, 0, 1, 0, 1);
    // the cloud bank: white over the gradient, in from row 250, out into the haze by row 470
    int tex = r->sky ? bfres_texture(r->sky, "Hightest_backLong2") : -1;
    if (tex < 0) return;
    static const int w0[4] = {232, 236, 255, 0}, w1[4] = {228, 234, 255, 235}, w2[4] = {214, 236, 255, 0};
    band(out, 250, 330, w0, w1, tex, 0, 2.5f, 0.05f, 0.35f);
    band(out, 330, 470, w1, w2, tex, 0, 2.5f, 0.35f, 0.9f);
}

static int bone_index(const BModel *m, const char *name) {
    for (int i = 0; m && i < m->nbones; i++)
        if (!strcmp(m->bones[i].name, name)) return i;
    return -1;
}

// One player of the 2-D styles, back to front [obs: smm1-ending.md §2.4, what 0x025ee86c builds; inf: the order]:
// Yoshi's body (playing run_body / wait_body on its skeleton; the rider on its `mario` bone, with the rider's poses
// yoshi_run / yoshi_wait, the stand at rate 1.2), the cape (mt_walk, then mt_down
// once and mt_wait / mt_yoshi_wait), Mario, Yoshi's head (on the body's `head` bone), the shoe over his legs (blinking while it hops; small Mario 8 higher in
// it, his shoe poses being head-only).
static void draw_walker(Rescue *r, Walker *w, int t, float camx, float camy, LQuads *out) {
    float x = mario_x(r, t) + w->dx, y = 47;
    if (r->style == 3) {
        draw_wu_mario(r, w, t, x, y, camx, camy, out);
        return;
    }
    if (!w->mdl) return;
    walker_hop(r, w, t);
    bool walking = t >= 30 && t < r->walk_end, yoshi = w->mount && r->style == 2 && r->yoshi;
    bool shoe = w->mount && r->style <= 1 && (r->shoe[0] || r->shoe[1]);
    float run_f = (float)(t - 30) * 1.125f, stand_f = (float)(t - r->walk_end) * (yoshi ? 1.2f : 1.0f);
    float ax = x, ay = y + w->y;
    const char *anim = walking ? "run" : "wait";
    const BModel *yhead = NULL;  // (Yoshi's head: in front of the rider, its quad at z 0.6)
    const BTexPat *yhp = NULL;
    float yhf = 0, yhx = 0, yhy = 0;
    g_recolour = w->luigi;
    if (yoshi) {
        const BModel *body = bfres_model(r->yoshi, "Player_Yoshi_Body_model"), *head = bfres_model(r->yoshi, "Player_Yoshi_Head_model");
        const char *bn = walking ? "run_body" : "wait_body";
        const BSkelAnim *sa = bfres_skelanim(r->yoshi, bn);
        const BTexPat *bp = bfres_texpat(r->yoshi, bn), *hp = bfres_texpat(r->yoshi, "wait_head");
        float f = walking ? run_f : stand_f;
        if (body) {
            float (*bones)[3][4] = calloc((size_t)body->nbones + 1, sizeof *bones);
            bfres_pose(body, sa, sa && sa->frames > 0 ? fmodf(f, (float)sa->frames) : 0, bones);
            draw_model_ex(r->yoshi, body, bp, pat_frame(bp, f), ax, ay, 0, camx, camy, false, false, LQ_BLEND_NORMAL, 255,
                          bones, NULL, out);
            int hb = bone_index(body, "head"), mb = bone_index(body, "mario");
            if (head && hb >= 0) yhead = head, yhp = hp, yhf = f, yhx = ax + bones[hb][0][3], yhy = ay + bones[hb][1][3];
            if (mb >= 0) ax += bones[mb][0][3], ay += bones[mb][1][3];
            free(bones);
        }
        anim = walking ? "yoshi_run" : "yoshi_wait";
    }
    if (shoe) {
        anim = w->anim ? w->anim : "shoes_wait";
        if (!pattern(r, w->mario, anim)) anim = "shoes_wait";
        if (w->form == 0) ay += 8;
    }
    if (w->cape && r->cape && r->cape->nmodels) {
        const char *cn = walking ? "mt_walk" : yoshi ? "mt_yoshi_wait" : "mt_wait";
        float cf = walking ? run_f : 0;
        const BTexPat *down = bfres_texpat(r->cape, "mt_down");
        if (!walking && down && t - r->walk_end < down->frames) cn = "mt_down", cf = (float)(t - r->walk_end);
        const BTexPat *cp = bfres_texpat(r->cape, cn);
        draw_model(r->cape, &r->cape->models[0], cp, pat_frame(cp, cf), ax, ay, 0, camx, camy, false, false, out);
    }
    const BTexPat *p = pattern(r, w->mario, anim);
    if (!p) p = pattern(r, w->mario, walking ? (r->style == 3 ? "walk" : "run") : "wait");
    if (!p && walking) p = pattern(r, w->mario, "walk");
    draw_model(w->mario, w->mdl, p, pat_frame(p, walking ? run_f : (float)(t - r->walk_end)), ax, ay, 0, camx, camy, false,
               false, out);
    if (yhead)  // (its tongue, shown only while he eats, left out)
        draw_model_ex(r->yoshi, yhead, yhp, pat_frame(yhp, yhf), yhx, yhy, 0, camx, camy, false, false, LQ_BLEND_NORMAL, 255,
                      NULL, "TongueL", out);
    if (shoe) {
        Bfres *sb = r->shoe[w->mount == 2] ? r->shoe[w->mount == 2] : r->shoe[0] ? r->shoe[0] : r->shoe[1];
        const BTexPat *sp = bfres_texpat(sb, "wait");
        if (sb->nmodels)
            draw_model(sb, &sb->models[0], sp, w->hop_done ? 0 : pat_frame(sp, (float)t), x, y + w->y, 0, camx, camy, false,
                       false, out);
    }
    g_recolour = 0;
}

void rescue_draw(Rescue *r, int t, LQuads *out, uint8_t bg[4]) {
    bg[0] = bg[1] = bg[2] = 0, bg[3] = 255;  // what is behind the ground model is [?]: black
    float mx = mario_x(r, t), my = 47;
    float camx = fminf(fmaxf(mx, VIEW_W / 2), 1152 - VIEW_W / 2), camy = fminf(fmaxf(my, VIEW_H / 2), 432 - VIEW_H / 2);
    // back to front: the sky, the ground model (z -1000), Peach / Toad, Luigi, Mario
    if (r->style == 3) draw_sky_wu(r, out);
    else draw_sky2d(r, t, camx, camy, out);
    bool smooth = r->style == 3;  // (NSMBU's textures are filtered; the 2-D styles' pixel art is not)
    // (SMB1's ground model carries its sky: left out under another theme's distant view)
    if (r->field)
        draw_model_ex(r->obj, r->field, NULL, 0, 0, 48, -1000, camx, camy, false, smooth, LQ_BLEND_NORMAL, 255, NULL,
                      r->style == 0 && !r->own_sky ? "sky" : NULL, out);
    if (r->field && r->style == 3)  // NSMBU's lit ground shows about 1.2 times its texture (linear) [inf: Cemu frames]
        draw_model_ex(r->obj, r->field, NULL, 0, 0, 48, -1000, camx, camy, false, smooth,
                      (1u << LQ_BLEND_OP) | (4u << LQ_BLEND_SRC) | (1u << LQ_BLEND_DST), 56, NULL, NULL, out);
    if (r->npc) draw_model(r->obj, r->npc, r->npc_wait, (float)t, 416, 48, 0, camx, camy, false, smooth, out);
    for (int i = r->nw - 1; i >= 0; i--) draw_walker(r, &r->w[i], t, camx, camy, out);
}
