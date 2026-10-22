// A whole program's MSL, as the Metal renderer draws it (shader.h): the vertex stage (with a geometry stage combined
// into it) and the fragment stage. Shared by the renderer's first use and the compile-ahead (shader_ahead.h), which
// must produce the same sources byte for byte: they are what Metal's shader cache and cache/ are keyed by.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

typedef struct { char *s; size_t n, cap; } Sb;
static void sb_add(Sb *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if (b->n + (size_t)k + 1 > b->cap) {
        size_t cap = (b->n + (size_t)k + 1) * 2;
        char *ns = realloc(b->s, cap);
        if (!ns) return;
        b->s = ns, b->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    b->n += (size_t)k;
}

const char *maxwell_translate_program_msl(const uint8_t *const code[3], const size_t cmax[3], const ShaderKey *vkey_in,
                                          ShaderKey *fkey, ShaderInfo **vi_out, ShaderInfo **fi_out, ShaderInfo **gi_out) {
    static _Thread_local char why[300];
    ShaderInfo *vi = NULL, *fi = NULL, *gi = NULL;
    ShaderKey gvkey;
    const ShaderKey *vkey = vkey_in;
    *vi_out = *fi_out = *gi_out = NULL;
    if (code[2]) {  // geometry stage: translated first (the vertex function carries its varyings)
        gi = *gi_out = maxwell_translate(code[2], cmax[2], STAGE_GEOMETRY, vkey);
        if (!gi->msl) { snprintf(why, sizeof why, "GS translate failed: %s", gi->error); return why; }
        gvkey = *vkey;
        gvkey.vs_fn = 1;
        gvkey.gs_out_mask = gi->out_mask;
        vkey = &gvkey;
    }
    vi = *vi_out = maxwell_translate(code[0], cmax[0], STAGE_VERTEX, vkey);
    if (vi->msl && gi) {  // the combined vertex function: vertex shader for the point, then the geometry stage
        if ((vi->cbuf_mask >> GS_CBUF_BASE) & gi->cbuf_mask) {
            snprintf(why, sizeof why, "GS cbufs clash with the vertex shader's");
            return why;
        }
        Sb m = {0};
        sb_add(&m, "%s", vi->msl);
        sb_add(&m, "vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
               vi->vs_entry_params, gi->gs_params);
        sb_add(&m, "  VOut vso = vs_main(in, iid, iid%s);\n", vi->vs_call_args);
        sb_add(&m, "%s", "  uint tri = vid / 3u, corner = vid % 3u;  // triangle tri of the emitted strip\n"
                         "  uint want = (tri & 1u) ? (corner == 0u ? tri + 1u : corner == 1u ? tri : tri + 2u) : tri + corner;\n"
                         "  VOut out = vso;\n");
        sb_add(&m, "%s", gi->msl);
        if (vkey->flip_y) sb_add(&m, "%s", "  out.pos.y = -out.pos.y;\n");
        if (vkey->flip_x) sb_add(&m, "%s", "  out.pos.x = -out.pos.x;\n");
        if (vkey->depth_minus_one) sb_add(&m, "%s", "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        sb_add(&m, "%s", "  return out;\n}\n");
        if (!m.s) { snprintf(why, sizeof why, "out of memory"); return why; }
        free(vi->msl);
        vi->msl = m.s;
        vi->out_mask = gi->out_mask;  // (what the fragment stage gets)
    }
    if (!vi->msl) { snprintf(why, sizeof why, "VS translate failed: %s", vi->error); return why; }
    fkey->vs_out_mask = vi->out_mask;
    fi = *fi_out = maxwell_translate(code[1], cmax[1], STAGE_FRAGMENT, fkey);
    if (!fi->msl) { snprintf(why, sizeof why, "FS translate failed: %s", fi->error); return why; }
    return NULL;
}
