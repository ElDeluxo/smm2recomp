// Host OS layer: everything the runtime needs that differs between macOS and Windows.
// Implementations: runtime/platform/macos.c, runtime/platform/windows.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#elif defined(__linux__)
#include "platform/linux_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim [lo, hi) for the guest at exactly that address, readable/writable (pages may be backed lazily).
bool plat_reserve_guest(uint64_t lo, uint64_t hi);
// Make sure [addr, addr+size) inside the guest window is backed (before the OS itself writes there, e.g.
// file reads); a no-op where the whole window is backed from the start.
void plat_commit(uint64_t addr, uint64_t size);
// In-place reboot: every page of [lo, hi) back to zero (and unbacked), the window still claimed.
bool plat_reset_guest(uint64_t lo, uint64_t hi);
// Pages of [addr, addr+size) in the guest window the CPU wrote since the last call over them (write tracking: Windows
// MEM_WRITE_WATCH), their addresses into pages[] (at most max), and the tracking reset for the range. Returns the
// count (max: maybe more), or -1 where there is no tracking (macOS, Linux). Page size: *page.
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t addr, uint64_t size, int prot);  // 0 on success
#define PLAT_PAGE 0x4000ull  // largest page size we deal with (macOS 16K; multiple of Windows' 4K)

// ---- time
uint64_t plat_now_ns(void);                // monotonic
// The process's memory as the system counts it, in bytes (macOS / iOS: the physical footprint, what Activity Monitor
// shows and iOS's memory limit judges; Linux / Android: resident; Windows: the working set); *left: what iOS still
// allows before it ends the app (0 elsewhere). 0: not known.
uint64_t plat_memory_used(uint64_t *left);
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t
void plat_thread_realtime(uint64_t period_ns);  // periodic timing thread (vsync): don't coalesce its timers

// ---- process
void plat_init(void);                                // first thing in main()
bool plat_exe_path(char *buf, size_t size);
void plat_exec(char *const argv[]);  // replace the process (NULL = same command line); returns only on failure
#if defined(_WIN32) || defined(__linux__)
void plat_terminate(void);  // end the process now, skipping DLL teardown (exit status 0)
#endif

// Child processes (seed generator, game-file extraction). stdout + stderr go to log_path (NULL = /dev/null).
typedef intptr_t PlatProc;  // 0 = failed to start
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log);
int plat_proc_wait(PlatProc p, bool block);  // exit code, or -1 while still running (block = false)
void plat_proc_kill(PlatProc p);             // end it and what it started (then it's gone: no wait)
bool plat_remove_tree(const char *dir);      // rm -rf
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#define PLAT_VENV_PYTHON "venv/Scripts/python.exe"  // relative to the venv's parent
#else
#define PLAT_EXE_SUFFIX ""
#define PLAT_VENV_PYTHON "venv/bin/python"
#endif

#ifdef __ANDROID__
// The data folder: the app's external files folder (/sdcard/Android/data/<package>/files: adb push, a file manager),
// or its internal one (/data/data/<package>/files) when the game files are there (extracted/exefs/main): on Android
// 11+ the app may be unable to read what `adb push` put in the external folder (Permission denied), and
// tools/android_push_data.sh --internal copies them into the internal one as the app (sm35recomp a5e5395).
const char *plat_android_data_dir(void);
#endif

// ---- diagnostics
// Name of the host function containing pc (guest functions are f_<addr>), NULL if unknown.
const char *plat_symbol(uint64_t pc, uint64_t *offset);
void plat_print_backtrace(void);
void plat_fatal_notice(void);  // a fatal error is about to end the process: tell the user (Windows release: message box)
void plat_crash_handler_install(void);  // per thread where the OS needs it (alternate signal stack)
// Called by the crash handler (runtime/dispatch.c): true = fault handled (watchpoint), resume.
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

// The logs of the sessions before this one: <name>.previous.log moves to .previous2.log, that one to .previous3.log,
// before the last session's log takes its place - three earlier sessions kept, not one (owner, 2026-10-30: a black
// screen's log was gone by the time it was looked for, the app having been started again since).
#include <stdio.h>
#include <string.h>
static inline void plat_keep_logs(const char *prev) {
    size_t n = strlen(prev);
    const char *suf = ".previous.log";
    size_t k = strlen(suf);
    if (n < k || strcmp(prev + n - k, suf) || n + 2 >= 1100) return;
    char p2[1100], p3[1100];
    snprintf(p2, sizeof p2, "%.*s.previous2.log", (int)(n - k), prev);
    snprintf(p3, sizeof p3, "%.*s.previous3.log", (int)(n - k), prev);
    remove(p3);
    rename(p2, p3);
    rename(prev, p2);
}
