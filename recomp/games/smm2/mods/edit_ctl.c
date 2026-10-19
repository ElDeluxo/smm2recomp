// A live control channel for tests (RECOMP_EDIT_CTL=<file>): a thread reads the lines appended to <file> and acts on
// them while the game runs, so a test (or a person at a shell) can drive menus step by step and look at each step:
//
//   tap <x> <y> [frames]      a finger on the touch screen at game pixel x, y (1280x720), held for frames (default 6)
//   btn <A|B|X|Y|L|R|ZL|ZR|PLUS|MINUS|UP|DOWN|LEFT|RIGHT>[+...] [frames]   controller buttons (test input, default 6)
//   shot <name>               the window to build/<name>.png (in the data folder)
//   peek <addr> [len]         guest memory as hex in the log (addr: 0x... or main+0x...)
//   wait <frames>             the next command that many frames later
//
// Test input has to be on (LARECOMP_INPUT set, e.g. "999999:A:1"). Nothing here runs unless the variable is set.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mod.h"

extern uint64_t g_test_buttons, g_test_buttons_until;  // runtime/game_hooks.c (weak), read by runtime/hle/input.c
extern char g_window_shot_request[256];
extern uint64_t renderer_present_count(void);
extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c

static const struct { const char *name; uint64_t bit; } BUTTONS[] = {
    {"A", 1 << 0}, {"B", 1 << 1}, {"X", 1 << 2}, {"Y", 1 << 3}, {"L", 1 << 6}, {"R", 1 << 7}, {"ZL", 1 << 8},
    {"ZR", 1 << 9}, {"PLUS", 1 << 10}, {"MINUS", 1 << 11}, {"LEFT", 1 << 12}, {"UP", 1 << 13}, {"RIGHT", 1 << 14},
    {"DOWN", 1 << 15},
};

static void wait_frames(uint64_t n) {
    uint64_t until = renderer_present_count() + n;
    while (renderer_present_count() < until) usleep(5000);
}

static uint64_t parse_addr(const char *s) {
    if (!strncmp(s, "main+", 5)) return MAIN_ADDR(strtoull(s + 5, NULL, 0));
    return strtoull(s, NULL, 0);
}

static void run_line(char *line) {
    char cmd[16] = "", a[64] = "", b[32] = "", d[32] = "";
    int n = sscanf(line, "%15s %63s %31s %31s", cmd, a, b, d);
    if (n < 1) return;
    LOG("[edit-ctl] %s", line);
    if (!strcmp(cmd, "tap") && n >= 3) {
        int frames = n >= 4 ? atoi(d) : 6;
        touch_test(atoi(a), atoi(b), true);
        wait_frames((uint64_t)frames);
        touch_test(atoi(a), atoi(b), false);
    } else if (!strcmp(cmd, "btn") && n >= 2) {
        uint64_t bits = 0;
        for (char *t = strtok(a, "+"); t; t = strtok(NULL, "+"))
            for (size_t i = 0; i < sizeof BUTTONS / sizeof *BUTTONS; i++)
                if (!strcmp(t, BUTTONS[i].name)) bits |= BUTTONS[i].bit;
        uint64_t frames = n >= 3 ? strtoull(b, NULL, 0) : 6, now = renderer_present_count();
        g_test_buttons = bits, g_test_buttons_until = now + frames;
        wait_frames(frames + 2);
        g_test_buttons = 0;
    } else if (!strcmp(cmd, "shot") && n >= 2) {
        snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", a);
        wait_frames(3);
    } else if (!strcmp(cmd, "peek") && n >= 2) {
        uint64_t at = parse_addr(a), len = n >= 3 ? strtoull(b, NULL, 0) : 0x40;
        if (len > 0x400) len = 0x400;
        for (uint64_t o = 0; o < len; o += 16) {
            char row[128];
            int k = snprintf(row, sizeof row, "%010llx:", (unsigned long long)(at + o));
            for (uint64_t i = 0; i < 16 && o + i < len; i++) k += snprintf(row + k, sizeof row - (size_t)k, " %02x", g8(at + o + i));
            LOG("[edit-ctl] %s\n", row);
        }
    } else if (!strcmp(cmd, "poke") && n >= 3) {  // poke <addr> <byte>: one byte of guest memory
        s8(parse_addr(a), (uint8_t)strtoul(b, NULL, 0));
    } else if (!strcmp(cmd, "find") && n >= 3) {  // find <addr> <name> [depth]: objects reachable by pointers from
                                                  // addr (0x200 bytes each, depth levels) holding "<name>\0"
        uint64_t q[4096], lvl[4096], via[4096], from[4096];
        int head = 0, tail = 0, depth = n >= 4 ? atoi(d) : 2;
        size_t len = strlen(b) + 1;
        q[tail] = parse_addr(a), via[tail] = 0, from[tail] = 0, lvl[tail++] = 0;
        while (head < tail) {
            uint64_t o = q[head], l = lvl[head++];
            for (uint64_t k = 0; k + len <= 0x200; k++)
                if (!memcmp((const void *)(uintptr_t)(o + k), b, len))
                    LOG("[edit-ctl] find: %010llx +%#llx (depth %llu, from %010llx +%#llx)\n", (unsigned long long)o,
                        (unsigned long long)k, (unsigned long long)l, (unsigned long long)from[head - 1],
                        (unsigned long long)via[head - 1]);
            if ((int)l >= depth) continue;
            for (uint64_t k = 0; k < 0x200 && tail < 4096; k += 8) {
                uint64_t p = g64(o + k);
                if (p < 0x7100000000ull || p >= 0x7320000000ull || (p & 7)) continue;  // (the heap only)
                bool seen = false;
                for (int i = 0; i < tail && !seen; i++) seen = q[i] == p;
                if (!seen) q[tail] = p, from[tail] = o, via[tail] = k, lvl[tail++] = l + 1;
            }
        }
    } else if (!strcmp(cmd, "wait") && n >= 2) {
        wait_frames(strtoull(a, NULL, 0));
    }
}

static void *ctl_thread(void *arg) {
    const char *path = arg;
    long done = 0;
    char line[256];
    for (;;) {
        usleep(50000);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fseek(f, done, SEEK_SET) == 0)
            while (fgets(line, sizeof line, f)) {
                if (!strchr(line, '\n')) break;  // (a line still being written: next time)
                done = ftell(f);
                run_line(line);
            }
        fclose(f);
    }
    return NULL;
}

__attribute__((constructor)) static void edit_ctl_start(void) {
    const char *path = getenv("RECOMP_EDIT_CTL");
    if (!path || !*path) return;
    pthread_t t;
    pthread_create(&t, NULL, ctl_thread, (void *)path);
    pthread_detach(t);
}
