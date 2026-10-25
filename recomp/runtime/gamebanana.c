// GameBanana in the Mods tab (gamebanana.h).
#include "gamebanana.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strncasecmp
#include <sys/stat.h>

#include "archive.h"
#include "net/http.h"
#include "net/json.h"
#include "platform.h"  // plat_remove_tree; (Windows: POSIX mkdir)
#include "rt.h"
#include "usermods.h"

#define API "https://gamebanana.com/apiv11"
#define MAX_LIST 100
#define TMP USERMODS_DIR "/.download"  // (dot folders: not mods)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static bool g_started;

static GbMod g_list[MAX_LIST];
static int g_nlist;
static GbState g_list_st;
static char g_list_msg[160];
static char g_search[128];
static bool g_want_list;

static GbMod g_cache[64];  // details fetched
static int g_ncache;
static int g_want_details[16], g_nwant;

static struct { int id, fi; bool enable; } g_inst_req;
static bool g_want_install;
static GbState g_inst_st;
static int g_inst_id;
static float g_inst_progress = -1;
static char g_inst_msg[200];
static volatile int g_cancel;

bool gb_available(void) { return game_gamebanana_id() > 0; }

// ---------------------------------------------------------------- helpers
static Json *get_json(const char *url, char *why, size_t cap) {
    HttpResponse r;
    int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, 20000, &r);
    if (st != 200 || !r.body) {
        snprintf(why, cap, st ? "GameBanana answered %d" : "couldn't reach GameBanana (%s)", st, st ? "" : r.error);
        http_free(&r);
        return NULL;
    }
    Json *j = json_parse(r.body, r.len);
    http_free(&r);
    if (!j) snprintf(why, cap, "GameBanana's answer couldn't be read");
    return j;
}
static void url_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
            out[o++] = (char)*p;
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
    }
    out[o] = 0;
}
// HTML description -> plain text (tags out, a few entities, blank runs folded; <br> / </p> / <li> as new lines)
static void html_text(const char *h, char *out, size_t cap) {
    size_t o = 0;
    bool space = true;
    for (const char *p = h; p && *p && o + 2 < cap;) {
        if (*p == '<') {
            bool nl = !strncasecmp(p, "<br", 3) || !strncasecmp(p, "</p", 3) || !strncasecmp(p, "<li", 3) ||
                      !strncasecmp(p, "</h", 3) || !strncasecmp(p, "</div", 5);
            const char *e = strchr(p, '>');
            p = e ? e + 1 : p + strlen(p);
            if (nl && o && out[o - 1] != '\n') out[o++] = '\n', space = true;
            continue;
        }
        if (*p == '&') {
            static const struct { const char *e, *c; } ents[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
                {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}, {"&apos;", "'"}};
            bool hit = false;
            for (size_t i = 0; i < sizeof ents / sizeof *ents; i++)
                if (!strncmp(p, ents[i].e, strlen(ents[i].e))) {
                    out[o++] = ents[i].c[0], p += strlen(ents[i].e), hit = true, space = false;
                    break;
                }
            if (hit) continue;
        }
        if (*p == '\r' || *p == '\n' || *p == '\t' || *p == ' ') {
            if (!space) out[o++] = ' ', space = true;
            p++;
            continue;
        }
        out[o++] = *p++, space = false;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\n')) o--;
    out[o] = 0;
}
static void copy_str(char *dst, size_t cap, const Json *j) { snprintf(dst, cap, "%s", json_str(j, "")); }

// ---------------------------------------------------------------- the jobs
static void do_list(void) {
    char search[128], url[512], why[200] = "";
    pthread_mutex_lock(&g_lock);
    snprintf(search, sizeof search, "%s", g_search);
    pthread_mutex_unlock(&g_lock);
    if (search[0]) {
        char q[400];
        url_escape(search, q, sizeof q);
        snprintf(url, sizeof url, API "/Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=%d&_sSearchString=%s"
                 "&_nPerpage=50", game_gamebanana_id(), q);
    } else {
        snprintf(url, sizeof url, API "/Mod/Index?_nPerpage=50&_aFilters%%5BGeneric_Game%%5D=%d&_sSort=Generic_MostDownloaded",
                 game_gamebanana_id());
    }
    Json *j = get_json(url, why, sizeof why);
    GbMod tmp[MAX_LIST];
    int n = 0;
    for (size_t i = 0; j && i < json_len(json_get(j, "_aRecords")) && n < MAX_LIST; i++) {
        Json *r = json_at(json_get(j, "_aRecords"), i);
        if (strcmp(json_str(json_get(r, "_sModelName"), "Mod"), "Mod")) continue;
        GbMod *m = &tmp[n++];
        memset(m, 0, sizeof *m);
        m->id = (int)json_num(json_get(r, "_idRow"), 0);
        copy_str(m->name, sizeof m->name, json_get(r, "_sName"));
        copy_str(m->author, sizeof m->author, json_get(json_get(r, "_aSubmitter"), "_sName"));
        copy_str(m->category, sizeof m->category, json_get(json_get(r, "_aRootCategory"), "_sName"));
    }
    json_free(j);
    pthread_mutex_lock(&g_lock);
    memcpy(g_list, tmp, sizeof *tmp * (size_t)n);
    g_nlist = n;
    g_list_st = j || n ? GB_IDLE : GB_ERROR;
    snprintf(g_list_msg, sizeof g_list_msg, "%s", n ? "" : *why ? why : "nothing found");
    if (n) g_list_st = GB_IDLE;
    pthread_mutex_unlock(&g_lock);
    LOG("[gamebanana] list%s%s: %d mods%s%s\n", search[0] ? " for " : "", search, n, *why ? ": " : "", why);
}

static GbMod *cache_find(int id) {
    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i].id == id) return &g_cache[i];
    return NULL;
}
static bool fetch_details(int id, GbMod *m, char *why, size_t cap) {
    char url[300];
    snprintf(url, sizeof url, API "/Mod/%d?_csvProperties=_sName,_aFiles,_nDownloadCount,_nLikeCount,_aSubmitter,"
             "_sLicense,_aCategory,_aRootCategory,_sText,_sVersion", id);
    Json *j = get_json(url, why, cap);
    if (!j) return false;
    memset(m, 0, sizeof *m);
    m->id = id;
    m->details = true;
    copy_str(m->name, sizeof m->name, json_get(j, "_sName"));
    copy_str(m->author, sizeof m->author, json_get(json_get(j, "_aSubmitter"), "_sName"));
    copy_str(m->category, sizeof m->category, json_get(json_get(j, "_aCategory"), "_sName"));
    if (!m->category[0]) copy_str(m->category, sizeof m->category, json_get(json_get(j, "_aRootCategory"), "_sName"));
    copy_str(m->license, sizeof m->license, json_get(j, "_sLicense"));
    copy_str(m->version, sizeof m->version, json_get(j, "_sVersion"));
    m->downloads = (int)json_num(json_get(j, "_nDownloadCount"), 0);
    m->likes = (int)json_num(json_get(j, "_nLikeCount"), 0);
    html_text(json_str(json_get(j, "_sText"), ""), m->text, sizeof m->text);
    Json *files = json_get(j, "_aFiles");
    for (size_t i = 0; i < json_len(files) && m->nfiles < GB_MAX_FILES; i++) {
        Json *f = json_at(files, i);
        int k = m->nfiles++;
        m->file_id[k] = (int)json_num(json_get(f, "_idRow"), 0);
        copy_str(m->file_name[k], sizeof m->file_name[k], json_get(f, "_sFile"));
        copy_str(m->file_desc[k], sizeof m->file_desc[k], json_get(f, "_sDescription"));
        m->file_size[k] = (uint64_t)json_num(json_get(f, "_nFilesize"), 0);
    }
    json_free(j);
    return true;
}
static void do_details(int id) {
    GbMod m;
    char why[200];
    bool ok = fetch_details(id, &m, why, sizeof why);
    pthread_mutex_lock(&g_lock);
    if (ok) {
        GbMod *c = cache_find(id);
        if (!c) c = &g_cache[g_ncache < 64 ? g_ncache++ : 63];
        *c = m;
    }
    pthread_mutex_unlock(&g_lock);
}

static void set_inst(GbState st, float progress, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_inst(GbState st, float progress, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_lock);
    g_inst_st = st, g_inst_progress = progress;
    vsnprintf(g_inst_msg, sizeof g_inst_msg, fmt, ap);
    pthread_mutex_unlock(&g_lock);
    va_end(ap);
}
static void on_progress(unsigned long long done, unsigned long long total, void *u) {
    (void)u;
    pthread_mutex_lock(&g_lock);
    g_inst_progress = total ? (float)((double)done / (double)total) : -1;
    snprintf(g_inst_msg, sizeof g_inst_msg, "Downloading: %.0f of %.0f MB", done / 1048576.0, total / 1048576.0);
    pthread_mutex_unlock(&g_lock);
}
static void on_unpack(int files, void *u) {
    (void)u;
    pthread_mutex_lock(&g_lock);
    snprintf(g_inst_msg, sizeof g_inst_msg, "Unpacking: %d files", files);
    pthread_mutex_unlock(&g_lock);
}
static void do_install(int id, int fi, bool enable) {
    GbMod m;
    char why[300] = "";
    g_cancel = 0;
    set_inst(GB_BUSY, -1, "Getting the mod's details");
    if (!fetch_details(id, &m, why, sizeof why)) { set_inst(GB_ERROR, -1, "Not installed: %s", why); return; }
    if (fi < 0 || fi >= m.nfiles) { set_inst(GB_ERROR, -1, "Not installed: it has no files to download"); return; }
    mkdir(USERMODS_DIR, 0755);
    plat_remove_tree(TMP);
    mkdir(TMP, 0755);
    char url[200], arc[600], dir[300];
    snprintf(url, sizeof url, "https://gamebanana.com/dl/%d", m.file_id[fi]);
    const char *ext = strrchr(m.file_name[fi], '.');
    snprintf(arc, sizeof arc, TMP "/download%s", ext && strlen(ext) < 8 ? ext : "");
    snprintf(dir, sizeof dir, TMP "/unpacked");
    LOG("[gamebanana] installing %s (%d) file %s (%llu bytes)\n", m.name, id, m.file_name[fi], (unsigned long long)m.file_size[fi]);
    HttpResponse r;
    int st = http_download(url, arc, NULL, 30000, on_progress, NULL, &g_cancel, &r);
    if (st != 200) {
        set_inst(GB_ERROR, -1, "Not installed: %s", st ? "GameBanana didn't send the file" : r.error);
        plat_remove_tree(TMP);
        return;
    }
    set_inst(GB_BUSY, -1, "Unpacking");
    if (!archive_extract(arc, dir, on_unpack, NULL, why, sizeof why)) {
        set_inst(GB_ERROR, -1, "Not installed: %s", why);
        plat_remove_tree(TMP);
        return;
    }
    remove(arc);
    UserModInfo info = {0};
    snprintf(info.name, sizeof info.name, "%s", m.name);
    snprintf(info.author, sizeof info.author, "%s", m.author);
    snprintf(info.category, sizeof info.category, "%s", m.category);
    snprintf(info.license, sizeof info.license, "%s", m.license);
    snprintf(info.version, sizeof info.version, "%s", m.version);
    snprintf(info.url, sizeof info.url, "https://gamebanana.com/mods/%d", id);
    snprintf(info.description, sizeof info.description, "%s", m.text);
    info.gb_id = id;
    set_inst(GB_BUSY, -1, "Installing");
    int n = usermods_install_dir(dir, &info, enable, why, sizeof why);
    plat_remove_tree(TMP);
    if (!n) { set_inst(GB_ERROR, -1, "Not installed: %s", why); return; }
    if (n > 1) set_inst(GB_IDLE, 1, "Installed: %d versions (the first is on; switch under Installed)", n);
    else set_inst(GB_IDLE, 1, "Installed%s", enable ? " and on: it shows from the next start" : "");
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_want_list && !g_nwant && !g_want_install) pthread_cond_wait(&g_wake, &g_lock);
        bool list = g_want_list, inst = g_want_install;
        int det = g_nwant ? g_want_details[--g_nwant] : 0;
        int iid = g_inst_req.id, ifi = g_inst_req.fi;
        bool ien = g_inst_req.enable;
        g_want_list = false, g_want_install = false;
        pthread_mutex_unlock(&g_lock);
        if (inst) do_install(iid, ifi, ien);
        if (list) do_list();
        if (det) do_details(det);
    }
    return NULL;
}
static void start(void) {  // (g_lock held)
    if (g_started) return;
    g_started = true;
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_detach(t);
}

// ---------------------------------------------------------------- the menu's side
void gb_refresh(const char *search) {
    if (!gb_available()) return;
    pthread_mutex_lock(&g_lock);
    start();
    snprintf(g_search, sizeof g_search, "%s", search ? search : "");
    g_want_list = true;
    g_list_st = GB_BUSY;
    snprintf(g_list_msg, sizeof g_list_msg, "Loading from GameBanana...");
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}
GbState gb_list_state(char *msg, int cap) {
    pthread_mutex_lock(&g_lock);
    GbState s = g_list_st;
    if (msg) snprintf(msg, (size_t)cap, "%s", g_list_msg);
    pthread_mutex_unlock(&g_lock);
    return s;
}
int gb_list(GbMod *out, int max) {
    pthread_mutex_lock(&g_lock);
    int n = g_nlist < max ? g_nlist : max;
    for (int i = 0; i < n; i++) {
        GbMod *c = cache_find(g_list[i].id);
        out[i] = c ? *c : g_list[i];
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
void gb_details(int id) {
    pthread_mutex_lock(&g_lock);
    start();
    bool have = cache_find(id) != NULL;
    for (int i = 0; i < g_nwant && !have; i++) have = g_want_details[i] == id;
    if (!have && g_nwant < 16) g_want_details[g_nwant++] = id, pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}
bool gb_get(int id, GbMod *out) {
    pthread_mutex_lock(&g_lock);
    GbMod *c = cache_find(id);
    if (c) *out = *c;
    pthread_mutex_unlock(&g_lock);
    return c != NULL;
}
void gb_install(int id, int fi, bool enable) {
    pthread_mutex_lock(&g_lock);
    start();
    if (g_inst_st != GB_BUSY) {
        g_inst_req.id = id, g_inst_req.fi = fi, g_inst_req.enable = enable;
        g_want_install = true;
        g_inst_id = id, g_inst_st = GB_BUSY, g_inst_progress = -1;
        snprintf(g_inst_msg, sizeof g_inst_msg, "Starting");
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}
GbState gb_install_state(int *id, float *progress, char *msg, int cap) {
    pthread_mutex_lock(&g_lock);
    GbState s = g_inst_st;
    if (id) *id = g_inst_id;
    if (progress) *progress = g_inst_progress;
    if (msg) snprintf(msg, (size_t)cap, "%s", g_inst_msg);
    pthread_mutex_unlock(&g_lock);
    return s;
}
void gb_install_cancel(void) { g_cancel = 1; }
