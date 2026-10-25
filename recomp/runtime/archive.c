// Unpacking a downloaded archive (zip, rar 4 / 5, 7z, tar...; archive.h) with the system's own libarchive, loaded at
// run time: macOS and iOS have it (/usr/lib/libarchive.2.dylib), Windows 10+ too (archiveint.dll, behind its tar.exe),
// and most Linux systems (libarchive.so.13). Nothing is bundled. Entries are written one by one here, so a path that
// would land outside the destination (absolute, or with "..") is skipped, and links are never created.
#include "archive.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"  // (Windows: POSIX mkdir)
#include "rt.h"

#ifdef _WIN32
#include <windows.h>
#define LIB_OPEN(n) ((void *)LoadLibraryA(n))
#define LIB_SYM(h, n) ((void *)GetProcAddress((HMODULE)(h), n))
static const char *const k_libs[] = {"archiveint.dll", "archive.dll", NULL};
#else
#include <dlfcn.h>
#define LIB_OPEN(n) dlopen(n, RTLD_NOW | RTLD_LOCAL)
#define LIB_SYM(h, n) dlsym(h, n)
#ifdef __APPLE__
static const char *const k_libs[] = {"/usr/lib/libarchive.2.dylib", "libarchive.2.dylib", NULL};
#else
static const char *const k_libs[] = {"libarchive.so.13", "libarchive.so", NULL};
#endif
#endif

// (the few libarchive calls used; their C API is stable)
struct archive;
struct archive_entry;
static struct {
    bool tried, ok;
    struct archive *(*read_new)(void);
    int (*support_filter_all)(struct archive *);
    int (*support_format_all)(struct archive *);
    int (*open_filename)(struct archive *, const char *, size_t);
    int (*next_header)(struct archive *, struct archive_entry **);
    long long (*read_data)(struct archive *, void *, size_t);  // (la_ssize_t: 64 bits on every 64-bit host)
    int (*read_free)(struct archive *);
    const char *(*error_string)(struct archive *);
    const char *(*entry_pathname)(struct archive_entry *);
    const char *(*entry_pathname_utf8)(struct archive_entry *);
    unsigned (*entry_filetype)(struct archive_entry *);
} A;
#define AE_IFREG 0100000u
#define AE_IFDIR 0040000u
#define ARCHIVE_OK 0
#define ARCHIVE_EOF 1
#define ARCHIVE_WARN (-20)

static bool load(void) {
    if (A.tried) return A.ok;
    A.tried = true;
    void *h = NULL;
    for (int i = 0; k_libs[i] && !h; i++) h = LIB_OPEN(k_libs[i]);
    if (!h) { LOG("[archive] no libarchive on this system\n"); return false; }
    #define S(f, n) (*(void **)&A.f = LIB_SYM(h, n))
    S(read_new, "archive_read_new"), S(support_filter_all, "archive_read_support_filter_all");
    S(support_format_all, "archive_read_support_format_all"), S(open_filename, "archive_read_open_filename");
    S(next_header, "archive_read_next_header"), S(read_data, "archive_read_data"), S(read_free, "archive_read_free");
    S(error_string, "archive_error_string"), S(entry_pathname, "archive_entry_pathname");
    S(entry_pathname_utf8, "archive_entry_pathname_utf8"), S(entry_filetype, "archive_entry_filetype");
    #undef S
    A.ok = A.read_new && A.support_filter_all && A.support_format_all && A.open_filename && A.next_header &&
           A.read_data && A.read_free && A.entry_pathname && A.entry_filetype;
    if (!A.ok) LOG("[archive] libarchive is missing calls\n");
    return A.ok;
}

bool archive_available(void) { return load(); }

// a relative path with no "..", no drive and no leading slash; backslashes as slashes
static bool safe_path(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (!in || !*in || in[0] == '/' || in[0] == '\\' || (in[0] && in[1] == ':')) return false;
    for (const char *p = in; *p && o + 1 < cap; p++) out[o++] = *p == '\\' ? '/' : *p;
    out[o] = 0;
    for (char *s = out; s && *s;) {
        char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 2 && s[0] == '.' && s[1] == '.') return false;
        s = e ? e + 1 : NULL;
    }
    return o > 0;
}
static void mkdirs_for(char *path, bool include_last) {
    for (char *p = path + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    if (include_last) mkdir(path, 0755);
}

#ifdef _WIN32
// (Windows without archiveint.dll's calls: its tar.exe, libarchive's bsdtar, which refuses paths outside the folder)
static bool tar_exe(const char *archive_path, const char *dest, char *why, size_t why_cap) {
    char tar[MAX_PATH];
    snprintf(tar, sizeof tar, "%s\\System32\\tar.exe", getenv("SystemRoot") ? getenv("SystemRoot") : "C:\\Windows");
    mkdir(dest, 0755);
    const char *argv[] = {tar, "-xf", archive_path, "-C", dest, NULL};
    PlatProc p = plat_spawn(argv, NULL, false);
    if (!p) { snprintf(why, why_cap, "this system has nothing to unpack it with"); return false; }
    int code = plat_proc_wait(p, true);
    if (code != 0) snprintf(why, why_cap, "couldn't unpack it (tar.exe: %d)", code);
    LOG("[archive] %s: tar.exe %s\n", archive_path, code ? "failed" : "unpacked it");
    return code == 0;
}
#endif

bool archive_extract(const char *archive_path, const char *dest, void (*progress)(int files, void *user), void *user,
                     char *why, size_t why_cap) {
#ifdef _WIN32
    if (!load()) return tar_exe(archive_path, dest, why, why_cap);
#endif
    if (!load()) { snprintf(why, why_cap, "this system has no libarchive to unpack it"); return false; }
    struct archive *a = A.read_new();
    A.support_filter_all(a), A.support_format_all(a);
    if (A.open_filename(a, archive_path, 1 << 16) != ARCHIVE_OK) {
        snprintf(why, why_cap, "can't open the archive (%s)", A.error_string ? A.error_string(a) : "?");
        A.read_free(a);
        return false;
    }
    mkdir(dest, 0755);
    struct archive_entry *en;
    int files = 0, r;
    bool ok = true;
    static char buf[1 << 18];
    while ((r = A.next_header(a, &en)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        const char *name = A.entry_pathname_utf8 ? A.entry_pathname_utf8(en) : NULL;
        if (!name) name = A.entry_pathname(en);
        char rel[1024], full[1600];
        if (!safe_path(name, rel, sizeof rel)) { LOG("[archive] skipped %s\n", name ? name : "?"); continue; }
        snprintf(full, sizeof full, "%s/%s", dest, rel);
        unsigned type = A.entry_filetype(en) & 0170000u;  // (mode_t: 16 bits on Windows)
        if (type == AE_IFDIR) { mkdirs_for(full, true); continue; }
        if (type != AE_IFREG) continue;  // (links and the like: never made)
        mkdirs_for(full, false);
        FILE *f = fopen(full, "wb");
        if (!f) { snprintf(why, why_cap, "can't write %s (%s)", rel, strerror(errno)); ok = false; break; }
        long long n;
        while ((n = A.read_data(a, buf, sizeof buf)) > 0)
            if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { n = -1; break; }
        if (fclose(f) != 0 || n < 0) {
            snprintf(why, why_cap, n < 0 && A.error_string && A.error_string(a) ? A.error_string(a) : "write failed (disk full?)");
            ok = false;
            break;
        }
        files++;
        if (progress) progress(files, user);
    }
    if (ok && r != ARCHIVE_EOF) {
        snprintf(why, why_cap, "the archive is damaged (%s)", A.error_string && A.error_string(a) ? A.error_string(a) : "?");
        ok = false;
    }
    A.read_free(a);
    if (ok && !files) { snprintf(why, why_cap, "the archive is empty"); ok = false; }
    LOG("[archive] %s: %d files%s%s\n", archive_path, files, ok ? "" : ", failed: ", ok ? "" : why);
    return ok;
}
