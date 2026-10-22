// SMM2's order for the shader compile-ahead (runtime/nvn/shader_ahead.h): which RomFS files' shaders compile first.
// Measured on 100 Mario runs (recomp/docs/smm2-bringup.md "Shader compile-ahead"):
// - a course start draws the models shared by every course (Pack/StaticModel.pack: its first model carries most of
//   them) and its theme's background and ground (Model/<style>_DV_<theme>, Model/<style>_Field_<theme>), which the
//   game reads only a second or two before the first frame: rank 0, with the layouts.
// - the styles' own models (Pack/<style>_Model.pack) are loaded at boot, and the game sets their programs then, so
//   the compile-ahead already has them from the programs the game set; the effects (Effect/) come up in any course:
//   rank 1.
// - the rest (story mode's and the Mii costumes' models, MyWorld): rank 2.
// - agl's and gsys's shader archives (Pack/Static.pack) hold thousands of variants of which a few dozen are drawn, most
//   of them set up by the game at boot (the programs it set cover those): last, rank 3.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void shader_ahead_prefer(const char *rel);

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

// A course about to be played (the next one of an Endless / 100 Mario run, a download): its style's models and its
// themes' backgrounds and ground are read ahead of the rest (shader_ahead_prefer; the pattern taken last goes first)
void smm2_shader_hint_course(const char *style, int theme_main, int theme_sub) {
    static const char *const themes[] = {"plain", "underground", "castle", "airship", "water", "hauntedhouse", "snow",
                                         "desert", "athletic", "woods"};
    if (!style || strlen(style) != 2 || !strchr("M3W", style[0])) return;
    char p[64];
    snprintf(p, sizeof p, "Pack/%s_FieldAnime_*", style);
    shader_ahead_prefer(p);
    snprintf(p, sizeof p, "Pack/%s_Model.pack*", style);
    shader_ahead_prefer(p);
    int t[2] = {theme_sub, theme_main};
    for (int i = 0; i < 2; i++)
        if (t[i] >= 0 && t[i] < 10 && (i == 1 || t[0] != t[1])) {
            snprintf(p, sizeof p, "Model/%s_*%s*", style, themes[t[i]]);
            shader_ahead_prefer(p);
        }
}
// ... from the course data itself (decrypted: the header's style at 0xF1, the two areas' themes)
void smm2_shader_hint_course_data(const uint8_t *d, size_t n) {
    if (n < 0x2E0E0 + 1) return;
    char style[3] = {(char)d[0xF1], (char)d[0xF2], 0};
    smm2_shader_hint_course(style, d[0x200], d[0x2E0E0]);
}

int shader_ahead_rank(const char *rel) {
    if (starts(rel, "Pack/Static.pack")) return 3;
    if (starts(rel, "Pack/StaticModel.pack") || starts(rel, "Layout/") || starts(rel, "Mii/") || starts(rel, "Model/SceneModel") ||
        ((starts(rel, "Model/M1_") || starts(rel, "Model/M3_") || starts(rel, "Model/MW_") || starts(rel, "Model/WU_") ||
          starts(rel, "Model/3W_")) && (strstr(rel, "_DV_") || strstr(rel, "_Field_"))))
        return 0;
    if (starts(rel, "Effect/") || strstr(rel, "_Model.pack") || starts(rel, "Model/M1_") || starts(rel, "Model/M3_") ||
        starts(rel, "Model/MW_") || starts(rel, "Model/WU_") || starts(rel, "Model/3W_"))
        return starts(rel, "Pack/QuestModel.pack") || starts(rel, "Pack/MyWorld_Model.pack") ? 2 : 1;
    return 2;
}
