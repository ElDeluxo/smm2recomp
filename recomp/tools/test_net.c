// Tests for runtime/net/http.c (http:// and https://). Run by tools/test_net.sh, which starts the mocks.
// Usage: test_net <case>...   Environment: NET_TEST_TLS_PORT, NET_TEST_HTTP_PORT, RECOMP_HTTPS_CA (set per case),
// NET_TEST_CA (the throwaway CA's file).
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/net/http.h"

// what netcfg.c / http.c need from the runtime
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
bool game_net_config(const char *key, char *out, size_t cap) { (void)key, (void)out, (void)cap; return false; }
void game_net_config_set(const char *key, const char *val) { (void)key, (void)val; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", __func__); printf(__VA_ARGS__); printf("\n"); failures++; return; } } while (0)

static void url_for(char *u, size_t n, const char *fmt, const char *portenv) {
    const char *p = getenv(portenv);
    snprintf(u, n, fmt, p ? p : "0");
}

static void https_local_ok(void) {
    char u[128];
    url_for(u, sizeof u, "https://localhost:%s/hello", "NET_TEST_TLS_PORT");
    setenv("RECOMP_HTTPS_CA", getenv("NET_TEST_CA"), 1);
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    CHECK(st == 200, "status %d (%s)", st, r.error);
    CHECK(r.body && !strcmp(r.body, "hello"), "body %s", r.body ? r.body : "(none)");
    http_free(&r);
    unsetenv("RECOMP_HTTPS_CA");
}

static void https_untrusted(void) {
    char u[128];
    url_for(u, sizeof u, "https://localhost:%s/hello", "NET_TEST_TLS_PORT");
    unsetenv("RECOMP_HTTPS_CA");
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    CHECK(st == 0, "status %d, wanted a refusal", st);
    CHECK(strstr(r.error, "not trusted"), "error \"%s\"", r.error);
}

static void https_wrong_host(void) {  // the CA is right, the name in the URL is not on the certificate
    char u[128];
    url_for(u, sizeof u, "https://127.0.0.2:%s/hello", "NET_TEST_TLS_PORT");
    setenv("RECOMP_HTTPS_CA", getenv("NET_TEST_CA"), 1);
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    unsetenv("RECOMP_HTTPS_CA");
    CHECK(st == 0, "status %d, wanted a refusal", st);
}

static void https_live_nextendo(void) {
    unsetenv("RECOMP_HTTPS_CA");
    HttpResponse r;
    int st = http_request("GET", "https://nextendo.network/", NULL, NULL, 0, 15000, &r);
    CHECK(st >= 200 && st <= 399, "status %d (%s)", st, r.error);
    http_free(&r);
}

static void https_live_selfsigned_refused(void) {
    unsetenv("RECOMP_HTTPS_CA");
    HttpResponse r;
    int st = http_request("GET", "https://51.178.29.194/", NULL, NULL, 0, 15000, &r);
    CHECK(st == 0, "status %d, wanted a refusal", st);
    printf("  (error: %s)\n", r.error);
}

static void http_plain_still_works(void) {
    char u[128];
    url_for(u, sizeof u, "http://127.0.0.1:%s/hello", "NET_TEST_HTTP_PORT");
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    CHECK(st == 200, "status %d (%s)", st, r.error);
    CHECK(r.body && !strcmp(r.body, "hello"), "body");
    http_free(&r);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"https_local_ok", https_local_ok}, {"https_untrusted", https_untrusted}, {"https_wrong_host", https_wrong_host},
    {"https_live_nextendo", https_live_nextendo}, {"https_live_selfsigned_refused", https_live_selfsigned_refused},
    {"http_plain_still_works", http_plain_still_works},
};

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int found = 0;
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            if (strcmp(argv[i], cases[k].name)) continue;
            found = 1;
            int before = failures;
            cases[k].fn();
            printf("%s %s\n", failures == before ? "ok  " : "FAIL", cases[k].name);
        }
        if (!found) { printf("no case %s\n", argv[i]); failures++; }
    }
    return failures ? 1 : 0;
}
