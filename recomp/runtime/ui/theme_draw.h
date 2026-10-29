// Pixel-art drawing for the NES menu theme (the look of https://nostalgic-css.github.io/NES.css/): notched boxes with
// inset shadows, and small icons drawn pixel by pixel. Used by menu.mm and option_rows.mm.
#pragma once
#include <math.h>
#include <string.h>

#include "imgui.h"

#define NES_RGB(r, g, b) IM_COL32(r, g, b, 255)
// NES.css's palette
#define NES_WHITE   NES_RGB(255, 255, 255)
#define NES_BLACK   NES_RGB(33, 37, 41)      // #212529
#define NES_GREY    NES_RGB(173, 175, 188)   // #adafbc
#define NES_BLUE    NES_RGB(32, 156, 238)    // #209cee  (primary)
#define NES_BLUE_D  NES_RGB(0, 107, 179)     // #006bb3
#define NES_GREEN   NES_RGB(146, 204, 65)    // #92cc41  (success)
#define NES_GREEN_D NES_RGB(74, 165, 46)     // #4aa52e
#define NES_YELLOW  NES_RGB(247, 213, 29)    // #f7d51d  (warning)
#define NES_ORANGE  NES_RGB(229, 148, 0)     // #e59400
#define NES_RED     NES_RGB(231, 110, 85)    // #e76e55  (error)
#define NES_RED_D   NES_RGB(140, 32, 34)     // #8c2022

// A bitmap: rows of '.' (clear) or a digit 1-9 picking pal[digit - 1]; each pixel px screen pixels square.
template <size_t N>
static inline void nes_icon(ImDrawList *dl, ImVec2 p, const char *const (&rows)[N], float px, const ImU32 *pal,
                            bool flip = false) {
    for (size_t y = 0; y < N; y++) {
        int w = (int)strlen(rows[y]);
        for (int x = 0; x < w; x++) {
            if (rows[y][x] == '.') continue;
            int xx = flip ? w - 1 - x : x;
            dl->AddRectFilled(ImVec2(floorf(p.x + xx * px), floorf(p.y + (float)y * px)),
                              ImVec2(floorf(p.x + (xx + 1) * px), floorf(p.y + (float)(y + 1) * px)), pal[rows[y][x] - '1']);
        }
    }
}
template <size_t N> static inline int nes_icon_w(const char *const (&rows)[N]) { return (int)strlen(rows[0]); }
template <size_t N> static inline int nes_icon_h(const char *const (&)[N]) { return (int)N; }

static const char *const kNesArrow[] = {"1....", "11...", "111..", "1111.", "11111", "1111.", "111..", "11...", "1...."};  // points right
static const char *const kNesCross[] = {"11...11", "111.111", ".11111.", "..111..", ".11111.", "111.111", "11...11"};

// The NES.css container: a box whose corners are one step (u px) - the corner pixel missing, the border a stair - with an
// optional inset shadow along the bottom and right (the buttons' look). Fill / shadow alpha 0 skips them.
static inline void nes_box(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float u, ImU32 shadow = 0) {
    u = floorf(u < 1 ? 1 : u);
    a = ImVec2(floorf(a.x), floorf(a.y));
    b = ImVec2(floorf(b.x), floorf(b.y));
    dl->AddRectFilled(ImVec2(a.x + 2 * u, a.y + u), ImVec2(b.x - 2 * u, b.y - u), fill);
    dl->AddRectFilled(ImVec2(a.x + u, a.y + 2 * u), ImVec2(b.x - u, b.y - 2 * u), fill);
    if (shadow & IM_COL32_A_MASK) {
        dl->AddRectFilled(ImVec2(a.x + u, b.y - 2 * u), ImVec2(b.x - u, b.y - u), shadow);
        dl->AddRectFilled(ImVec2(b.x - 2 * u, a.y + 2 * u), ImVec2(b.x - u, b.y - u), shadow);
    }
    dl->AddRectFilled(ImVec2(a.x + u, a.y), ImVec2(b.x - u, a.y + u), border);          // top
    dl->AddRectFilled(ImVec2(a.x + u, b.y - u), ImVec2(b.x - u, b.y), border);          // bottom
    dl->AddRectFilled(ImVec2(a.x, a.y + u), ImVec2(a.x + u, b.y - u), border);          // left
    dl->AddRectFilled(ImVec2(b.x - u, a.y + u), ImVec2(b.x, b.y - u), border);          // right
    dl->AddRectFilled(ImVec2(a.x + u, a.y + u), ImVec2(a.x + 2 * u, a.y + 2 * u), border);  // the step in each corner
    dl->AddRectFilled(ImVec2(b.x - 2 * u, a.y + u), ImVec2(b.x - u, a.y + 2 * u), border);
    dl->AddRectFilled(ImVec2(a.x + u, b.y - 2 * u), ImVec2(a.x + 2 * u, b.y - u), border);
    dl->AddRectFilled(ImVec2(b.x - 2 * u, b.y - 2 * u), ImVec2(b.x - u, b.y - u), border);
}
