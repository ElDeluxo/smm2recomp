// Tests for runtime/net/http.c (http:// and https://). Run by tools/test_net.sh, which starts the mocks.
// Usage: test_net <case>...   Environment: NET_TEST_TLS_PORT, NET_TEST_HTTP_PORT, RECOMP_HTTPS_CA (set per case),
// NET_TEST_CA (the throwaway CA's file).
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/x509_crt.h"

#include "../runtime/net/http.h"
#include "../runtime/net/roots.h"

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

static void https_wrong_host(void) {  // the CA is right and the mock is reached, but the certificate names other.test
    char u[128];
    url_for(u, sizeof u, "https://localhost:%s/hello", "NET_TEST_BADHOST_PORT");
    setenv("RECOMP_HTTPS_CA", getenv("NET_TEST_CA"), 1);
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    unsetenv("RECOMP_HTTPS_CA");
    CHECK(st == 0, "status %d, wanted a refusal", st);
    CHECK(strstr(r.error, "not trusted"), "error \"%s\"", r.error);
}

static void embedded_roots_valid(void) {  // exactly the five roots, none expired
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    const char *pem = net_root_pems();
    int r = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1);
    CHECK(r == 0, "parse: %d", r);
    int n = 0;
    for (mbedtls_x509_crt *c = &crt; c && c->version; c = c->next) {
        n++;
        CHECK(!mbedtls_x509_time_is_past(&c->valid_to), "root %d expired", n);
        CHECK(!mbedtls_x509_time_is_future(&c->valid_from), "root %d not yet valid", n);
    }
    mbedtls_x509_crt_free(&crt);
    CHECK(n == 5, "%d roots, wanted 5", n);
}

static void https_live_nextendo(void) {
    unsetenv("RECOMP_HTTPS_CA");
    HttpResponse r;
    int st = http_request("GET", "https://nextendo.network/", NULL, NULL, 0, 15000, &r);
    CHECK(st >= 200 && st <= 399, "status %d (%s)", st, r.error);
    http_free(&r);
}

static void https_live_nextendo_embedded_only(void) {  // iOS / Android / Windows have no usable system bundle
    unsetenv("RECOMP_HTTPS_CA");
    setenv("RECOMP_HTTPS_NO_SYSTEM_ROOTS", "1", 1);
    HttpResponse r;
    int st = http_request("GET", "https://nextendo.network/", NULL, NULL, 0, 15000, &r);
    unsetenv("RECOMP_HTTPS_NO_SYSTEM_ROOTS");
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
    {"embedded_roots_valid", embedded_roots_valid},
    {"https_live_nextendo", https_live_nextendo}, {"https_live_nextendo_embedded_only", https_live_nextendo_embedded_only}, {"https_live_selfsigned_refused", https_live_selfsigned_refused},
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
