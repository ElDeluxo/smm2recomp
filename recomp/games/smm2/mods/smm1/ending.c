// SMM1's 100 Mario Challenge ending (ending.h), as Block.rpx v208 plays it [obs: research/specs/smm1-ending.md]:
//
// 1. NetEndingDemoScene, the rescue (rescue.c): fades in from black (~25 frames [inf]), Mario walks to Peach (Toad on
//    Easy) in the style of the course just cleared; once he has stopped the balloon Play_EndingText_00 shows one line
//    (In 24, Wait looping, Out 300 frames later): T_ThankYou_<row><variant>, row by difficulty (Easy 1, Normal 2,
//    Expert 3, Super Expert 3), variant by the costume Mario wore at the goal (0 none). Then the scene fades to black.
// 2. NetEndingScene: on black, after its fade-in, the staff credits play once at speed 1 - Nwk_StaffCredit_00 (16
//    rows, InOut 1420 frames, "END") for Normal and Expert, _01 (8 rows, 1020, "TO BE CONTINUED...") for Easy, _03
//    (6 rows, 1020, "THE END") for Super Expert - listing each cleared course (name, maker) under "Course Design".
//    Each row is drawn in SMM2's look instead of SMM1's Mii row (owner, 2026-10-19: the makers' Miis are not there):
//    SMM2's own course card (Play_CourseIn_00 from the game's romfs: the yellow course-name banner with the course ID
//    and the maker underneath, no Mii, no flag), placed where SMM1's row is on each frame, so the roll keeps SMM1's
//    timing.
//    180 frames after the credits the Mystery Mushroom costume window opens (the costumes mod: hundred_costume_reward;
//    off unless costume_reward=1: costume.c costume_reward_on).
// Neither scene reads the controller: nothing skips [obs].
#include "ending.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rescue.h"
#include "smm1_audio.h"

__attribute__((weak)) void hundred_costume_reward(int difficulty) { (void)difficulty; }
// the costumes mod (mods/costume.c): a costume not owned yet for the won run, NULL without the mod or when none is left
// (tests without the mod: RECOMP_HUNDRED_TEST_REWARD=<res>)
__attribute__((weak)) const char *costume_reward_pick(int difficulty) { (void)difficulty; return getenv("RECOMP_HUNDRED_TEST_REWARD"); }
// (the reward is off unless asked for: costume.c costume_reward_on; the offline test tool's reward test asks by its env)
__attribute__((weak)) bool costume_reward_on(void) { return getenv("RECOMP_HUNDRED_TEST_REWARD") != NULL; }
void costume_name(const char *res, char *name, size_t cap, char *title, size_t tcap);  // costume_gen.c
int costume_sprite(const char *res);
__attribute__((weak)) int hundred_ending_costume(void) { return -1; }  // costume id at the goal (-1 none)

#define FADE 25  // the default fader: a 100-frame animation at speed 4 [inf]

struct Ending {
    int d, tick, rescue_len, credits_start, credits_len, end, text_in, text_out, style;
    Lay *rows[16];  // SMM2 course cards, one a cleared course
    int nrows;
    bool done, reward_asked;
    Lay *cm;        // the reward: SMM1's costume window (Cmn_CMListWindow_01) with the new costume
    int reward_tex, reward_at;
    Rescue *rescue;
    Lay *text, *credits;
};

static int costume_variant(int id) {  // [obs 0x024e8c84]
    if (id < 0 || id == 0) return 0;  // none, Mario
    if (id == 1) return 1;            // Peach
    if (id == 6) return 2;            // Toad
    if (id == 3) return 3;            // Bowser
    return 4;
}

Ending *ending_new(int d, int lives, int n, const struct hundred_credit *c) {
    (void)lives;
    return ending_new_style(d, n, c, 0, NULL);
}

Ending *ending_new_style(int d, int n, const struct hundred_credit *c, int style, const struct hundred_look *look) {
    Ending *e = calloc(1, sizeof *e);
    e->d = d < 0 ? 0 : d > 3 ? 3 : d;
    e->style = style == 4 ? 3 : style < 0 || style > 3 ? 0 : style;  // (SM3DW has no SMM1 scene: NSMBU's, the 3-D one [inf])
    e->rescue = rescue_new(e->style, e->d != 0, look);
    e->rescue_len = rescue_length(e->rescue);
    e->text = lay_open("Play_EndingText_00");
    if (e->text) {
        char label[32];
        snprintf(label, sizeof label, "T_ThankYou_%d%d", e->d == 0 ? 1 : e->d == 1 ? 2 : 3, costume_variant(hundred_ending_costume()));
        lay_text_from(e->text, "T_ThankYou_00", label);
    }
    e->text_in = rescue_text_in(e->rescue), e->text_out = rescue_text_out(e->rescue);
    if (e->text && e->rescue) {  // the balloon points at Peach's (Toad's) head [obs: Cemu]
        float xy[2];
        rescue_balloon_anchor(e->rescue, xy);
        lyt_set_pane(e->text->in, "N_All_00", xy, -1);
    }
    static const char *which[4] = {"Nwk_StaffCredit_01", "Nwk_StaffCredit_00", "Nwk_StaffCredit_00", "Nwk_StaffCredit_03"};
    e->credits = lay_open(which[e->d]);
    e->credits_start = e->rescue_len + FADE + FADE;
    if (e->credits) {
        lay_hold(e->credits, "InOut", 0);
        e->credits_len = lay_ticks(e->credits, "InOut", 1);
        for (int i = 0; i < 16; i++) {
            char part[32], p[96];
            snprintf(part, sizeof part, "L_MiiInfo_%02d", i);
            if (lyt_pane_index(e->credits->res, part) < 0) continue;
            lay_show(e->credits, part, false);  // (SMM2's card is drawn in its place)
            if (i >= n) continue;
            Lay *row = lay_open("nx:Play_CourseIn_00");
            if (!row) {  // (no SMM2 layout: SMM1's row, without the Mii's flag)
                lay_show(e->credits, part, true);
                snprintf(p, sizeof p, "%s/T_CourseName_00", part), lay_text(e->credits, p, c[i].name);
                snprintf(p, sizeof p, "%s/T_MiiName_00", part), lay_text(e->credits, p, c[i].maker);
                snprintf(p, sizeof p, "%s/P_Frame_00", part), lay_show(e->credits, p, false);
                snprintf(p, sizeof p, "%s/L_NationalFlag_00", part), lay_show(e->credits, p, false);
                continue;
            }
            lay_hold_end(row, "AnimDefault");
            lay_text(row, "T_CourseName_00", c[i].name);
            lay_text(row, "T_PlayerName_00", c[i].maker);
            lay_text(row, "T_CourseID_00", c[i].code);
            static const char *hide[] = {"N_Align_00", "N_Info_00", "N_EventAlign_00", "N_EventFirst_00",
                                         "P_GhostVolumenone_00", "L_LoadingGhost_00", "L_MiiIcon_00",
                                         "L_NationalFlag_00", "N_Caracter_00", NULL};
            for (int k = 0; hide[k]; k++) lay_show(row, hide[k], false);
            lay_show(row, "N_MakerAnim_00", true);
            // the run's order on the card's own number badge (Story mode's course number) [inf: a choice]
            lay_show(row, "W_WorldNo_00", false), lay_show(row, "T_WorldNum_00", false);
            lay_show(row, "W_QuestNo_00", true), lay_show(row, "T_QuestNo_00", true);
            char num[8];
            snprintf(num, sizeof num, "%d", i + 1);
            lay_text(row, "T_QuestNo_00", num);
            // as the game lays the card out in Endless [obs: its course-in card]: the name centred (no quest / world
            // number), the course ID at the left of the maker strip; the maker's name moves right into the Mii's
            // place, level with the ID's margin [inf: a choice, there is no Mii]
            float centre[2] = {0, 0}, maker[2] = {64, 0};
            lyt_set_pane(row->in, "T_CourseName_00", centre, -1);
            lyt_set_pane(row->in, "N_Maker_00", maker, -1);
            // the name and ID in the course name's weight: their border-less brown is a hairline at the roll's scale
            // [inf: a choice; the game draws them at full size]
            for (int k = 0; k < 2; k++) {
                int pi = lyt_pane_index(row->res, k ? "T_CourseID_00" : "T_PlayerName_00");
                if (pi < 0) continue;
                LPane *p = &row->res->pane[pi];
                p->txt.flags &= ~4;
                memcpy(row->res->mat[p->txt.mat].black, row->res->mat[p->txt.mat].white, 3);
            }
            e->rows[i] = row;
            if (i + 1 > e->nrows) e->nrows = i + 1;
        }
    }
    e->end = e->credits_start + e->credits_len + 180;
    return e;
}
void ending_free(Ending *e) {
    if (!e) return;
    rescue_free(e->rescue);
    lay_free(e->text), lay_free(e->credits), lay_free(e->cm);
    for (int i = 0; i < e->nrows; i++) lay_free(e->rows[i]);
    free(e);
}
int ending_length(const Ending *e) { return e ? e->end : 0; }
int ending_step(Ending *e, const UiIn *in) {
    // nothing skips [obs]; the reward window closes with A
    e->tick++;
    if (e->done) return EV_NONE;
    if (e->text) {  // the balloon: In when Mario has stopped, then Wait (looping) until Out [obs 0x024e8c84]
        if (e->tick == e->text_in) lay_play(e->text, "In", e->tick);
        if (e->tick == e->text_in + (int)lay_length(e->text, "In")) lay_stop(e->text, "In"), lay_play_loop(e->text, "Wait", e->tick);
        if (e->tick == e->text_out) lay_stop(e->text, "Wait"), lay_play(e->text, "Out", e->tick);
    }
    if (e->tick == 1) {  // EndingPeachBGM / EndingKinopioBGM by style [obs data: SLink2DB]
        static const char *peach[4] = {"M1_BGM_Ending", "M3_BGM_EndingDemo2", "MW_BGM_Ending2", "WU_STRM_BGM_LAST_BOSS2_HARP"};
        static const char *toad[4] = {"M1_BGM_Ending", "M3_BGM_EndingDemo1", NULL, "WU_STRM_BGM_LAST_BOSS_HARP_Dummy"};
        const char *s = e->d ? peach[e->style] : toad[e->style];
        if (s) smm1_bgm(s, false);
        else {  // SMW's Toad tune is a wave of the scene's sound bank (NetEndingDemoScene.bars) [obs data]
            smm1_bgm(NULL, false);
            smm1_se_bank("NetEndingDemoScene");
            smm1_se("SuperMarioWorld_21_RescueEgg");
        }
    }
    if (e->tick == e->rescue_len) smm1_bgm_fade(FADE);
    if (e->tick == e->rescue_len + FADE)  // the staff roll: Easy and Super Expert "easy", Normal and Expert "normal"
        smm1_bgm(e->d == 0 || e->d == 3 ? "BGM_100mario_staffroll_easy" : "BGM_100mario_staffroll_normal", false);
    if (e->tick == e->credits_start && e->credits) lay_play(e->credits, "InOut", e->tick);
    if (e->tick >= e->end && !e->reward_asked) {  // 180 frames after the credits: the Mystery Mushroom reward
        e->reward_asked = true;
        hundred_costume_reward(e->d);
        const char *res = costume_reward_on() ? costume_reward_pick(e->d) : NULL;  // (off: the ending goes straight on)
        if (res && (e->cm = lay_open("Cmn_CMListWindow_01"))) {
            char name[96], title[128];
            costume_name(res, name, sizeof name, title, sizeof title);
            lay_text(e->cm, "T_Name_00", name);
            lay_text(e->cm, "T_Title_00", title);
            lay_play(e->cm, "In", e->tick);
            e->reward_tex = costume_sprite(res);
            e->reward_at = e->tick;
            e->end = e->tick + 480;  // (closes by itself, or with A after a second)
        }
    }
    if (e->cm && e->tick > e->reward_at + 60 && in && (in->pressed & BT_A)) e->end = e->tick;
    if (e->tick >= e->end && e->reward_asked) {
        e->done = true;
        return EV_DONE;
    }
    return EV_NONE;
}

static uint8_t g_bg[4] = {0, 0, 0, 255};
void ending_draw(Ending *e, LQuads *out) {
    int t = e->tick;
    if (t < e->rescue_len + FADE) {
        rescue_draw(e->rescue, t, out, g_bg);
        if (e->text && t >= e->text_in && t < e->text_out + lay_ticks(e->text, "Out", 1)) lay_draw(e->text, t, out);
        if (t < FADE) fade_quad(out, 1 - (float)t / FADE);
        if (t >= e->rescue_len) fade_quad(out, (float)(t - e->rescue_len) / FADE);
        return;
    }
    g_bg[0] = g_bg[1] = g_bg[2] = 0, g_bg[3] = 255;
    if (e->cm && t >= e->reward_at) {  // the reward: the costume window with the new costume standing in it, 12x
        lay_draw(e->cm, t, out);
        float a = (float)(t - e->reward_at) / 20;
        if (e->reward_tex >= 0 && a > 0) {
            LQuad q;
            memset(&q, 0, sizeof q);
            float x0 = -144, y0 = 120, sz = 288;
            float P[4][2] = {{x0, y0 - 72}, {x0 + sz, y0 - 72}, {x0, y0 - 72 - sz}, {x0 + sz, y0 - 72 - sz}};
            float U[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
            memcpy(q.pos, P, sizeof P), memcpy(q.uv0, U, sizeof U);
            for (int k = 0; k < 4; k++) q.col[k][0] = q.col[k][1] = q.col[k][2] = 255, q.col[k][3] = (uint8_t)(a > 1 ? 255 : a * 255);
            memset(q.white, 255, 4);
            q.tex0 = e->reward_tex, q.tex1 = -1;
            q.flags = LQ_BLEND_NORMAL | LQ_COMB_NONE << LQ_COMB_C | LQ_COMB_NONE << LQ_COMB_A;
            if (out->n == out->cap) out->cap = out->cap ? out->cap * 2 : 256, out->q = realloc(out->q, sizeof(LQuad) * (size_t)out->cap);
            out->q[out->n++] = q;
        }
        return;
    }
    if (e->credits) lay_draw(e->credits, t, out);
    // the SMM2 cards where SMM1's rows are: the card's banner (1150 x 162, centred 223 above its root) at 0.6, so the
    // cards keep gaps between them [inf: a choice, SMM1 had no such cards]
    for (int i = 0; e->credits && i < e->nrows; i++) {
        if (!e->rows[i]) continue;
        char part[32];
        snprintf(part, sizeof part, "L_MiiInfo_%02d", i);
        float cr[4][2], a;
        bool vis;
        if (!lyt_pane_global(e->credits->in, part, cr, &a, &vis) || a <= 0) continue;
        float cx = (cr[0][0] + cr[3][0]) / 2, cy = (cr[0][1] + cr[3][1]) / 2;
        const float k = 0.66f;  // (107 high on SMM1's 120-unit row pitch: the text reads better, gaps stay)
        float xy[2] = {cx, cy - 223 * k}, sc[2] = {k, k};
        lyt_set_pane(e->rows[i]->in, "N_All_00", xy, a * 255);
        lyt_set_pane_scale(e->rows[i]->in, "N_All_00", sc);
        lay_draw(e->rows[i], t, out);
    }
    int cf = t - (e->rescue_len + FADE);
    if (cf < FADE) fade_quad(out, 1 - (float)cf / FADE);
}
void ending_background(Ending *e, uint8_t rgba[4]) {
    (void)e;
    memcpy(rgba, g_bg, 4);
}
