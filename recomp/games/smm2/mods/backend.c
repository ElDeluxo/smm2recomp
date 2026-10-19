// The online backend for SMM2 (runtime/net/netcfg.h): its settings live in smm2mods.ini with the mod's, under these
// keys (environment variables win, as for the mod's own settings):
//
//   backend=ours | local-nex | nextendo     RECOMP_NET_BACKEND
//   nex_test_server=<ip>                    RECOMP_NET_TEST_SERVER
//   net_hosts=<name=ip;...>                 RECOMP_NET_HOSTS
//   tls_trust=local-ca | any                RECOMP_NET_TLS_TRUST
//   tls_ca=<path>                           RECOMP_NET_TLS_CA
//   id_token_file=<path>                    RECOMP_NET_ID_TOKEN_FILE
//   nex_token_file=<path>                   RECOMP_NET_NEX_TOKEN_FILE
//   net_log=1                               RECOMP_NET_LOG
//
// backend=ours (default) is the mod's path: the overrides answer Course World from our course server. Any other
// backend switches the mod's online overrides off (mod_hundred() is false, settings.c), so the game's own NEX
// client runs on the runtime's network stack instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

void mod_ini_set(const char *key, const char *val);  // settings.c

static const char *ini_path(void) { return getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini"; }

bool game_net_config(const char *key, char *out, size_t cap) {
    static const char *const keys[] = {"backend", "nex_test_server", "net_hosts", "tls_trust", "tls_ca",
                                       "id_token_file", "nex_token_file", "net_log"};
    bool known = false;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) known |= !strcmp(keys[i], key);
    FILE *f = known ? fopen(ini_path(), "r") : NULL;
    if (!f) return false;
    char line[1024];
    bool found = false;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || strncmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1, *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        snprintf(out, cap, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}

void game_net_config_set(const char *key, const char *val) { mod_ini_set(key, val); }
