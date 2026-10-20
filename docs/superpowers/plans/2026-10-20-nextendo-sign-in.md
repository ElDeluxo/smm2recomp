# Nextendo Network backend (sign-in, game login, routing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the recomp signs a player in to Nextendo Network (OAuth PKCE in the browser), turns that into a NEX game
token, and lets the game's own NEX client log in to Nextendo's SMM2 server with `backend=nextendo`.

**Architecture:** a new account module (`runtime/net/nextendo.c`) does OAuth and the token calls over a new HTTPS
path in `runtime/net/http.c` (mbedTLS with real certificate checks). `netcfg.c` gains the Nextendo backend: Nintendo
host names resolve to Nextendo's IPs, game TLS to them is pinned to their certificate, and `acc` serves the
Nextendo PID, username and an RS256 ID token carrying the game token in `nnex`. The Mods tab gets an "Online" section
with the backend choice and a Sign in / Sign out row.

**Tech Stack:** C11 runtime, Mbed TLS 3.6 (`third_party/mbedtls`), SDL3, Dear ImGui (`runtime/ui/*.mm`), Apple
Security + AuthenticationServices frameworks, Python 3 for the mock server.

**Spec:** `research/specs/recomp-nextendo.md` (§2.1 client flow, §3.2 R5–R7, §5 phases N4–N5). The owner registered
the app on 2026-10-20, which replaces phase N0's Discord step.

## Global Constraints

- Client ID `nxc_R-JWtJ6JrrgY`: a public application (PKCE, no secret). Scopes `identity game.matchmaking`.
- Redirects: desktop `http://127.0.0.1:<free port>/callback`; iOS `smm2recomp://callback`.
- Endpoints: authorize `https://nextendo.network/oauth/authorize`; token `POST https://nextendo.network/oauth/token`
  (on 404 retry `POST https://nextendo.network/api/oauth/token`); game token `GET https://nextendo.network/api/nex-token`
  with `Authorization: Bearer <access_token>`, which returns `nex_token, pid, friend_code, username, expires_in`.
  Gate reason: `GET https://nextendo.network/api/online-status` (Bearer).
- Lifetimes: access token 1 h; refresh token 30 days, rotated on every refresh (store the new one at once); game token 24 h.
- Game server `51.178.29.194`; NAT check `164.132.111.120` (for `nncs2-*` only). Both can be overridden
  (`nextendo_server=`, `nextendo_nat=`).
- Game-server TLS pin: SPKI SHA-256 `d2e7388e48ee30cfbecfbeb522874f2cc6b85f7aa1105c57669f3c2aa85fb30e`
  (self-signed `CN=nintendo-private-server`, valid until 2036-06-19). It can be overridden with `nextendo_tls_pin=`.
- Never log, print or put in a URL: access tokens, refresh tokens, the nex token, the PID. Log lengths and states only.
- Nintendo is never reachable. Only the redirect table's names resolve; every other name gets NXDOMAIN, as today.
- No Nextendo code (PolyForm Shield) and no citron code (GPL) is copied. Write everything from the spec and the
  published docs.
- Comment and naming style: match `runtime/net/netcfg.c` / `token.c` (file-top comment explaining the module, short
  lowercase static helpers, `LOG` / `NLOG`).
- `scripts/check.sh` passes (it includes `make -C recomp GAME=smm2 runtime-objs`), and so does
  `scripts/check_no_proprietary.py` on the changed files.

## Review Focus

- The refresh token rotates and the old one dies a minute later. A crash between the refresh and the save must not
  sign the player out silently: save before using the new access token. Test: `refresh_rotates_and_persists`.
- No network, or nextendo.network down, at launch: the game must start, with the backend reported as unavailable and
  a status message. No hang longer than the 10 s timeout. Test: `prepare_offline_times_out`.
- The browser was closed or the player denied access (`?error=access_denied`), or a second Sign in was started: the
  listener must stop, the state goes back to signed out with a message, and the port is freed. Tests:
  `callback_error_denied`, `begin_twice_cancels_first`.
- A forged callback (wrong `state`) must be refused. Test: `callback_state_mismatch`.
- A different certificate on the game server (a rotation, or a MITM) must fail closed with a clear log line. It must
  not fall back to "any". Test: `pin_mismatch_refused` (Task 3).

---

### Task 1: HTTPS in `http_request` with real certificate checks

**Files:**
- Modify: `recomp/runtime/net/http.c`, `recomp/runtime/net/http.h` (https:// URLs)
- Create: `recomp/runtime/net/roots.c` (embedded roots, PEM strings), `recomp/runtime/net/roots.h`
- Modify: `recomp/Makefile` (add `$(B)/rt/net/http.o: RTFLAGS += $(MBEDTLS_FLAGS)` and the same for `roots.o`)
- Test: `recomp/tools/test_net.c`, `recomp/tools/test_net.sh`, `recomp/tools/net_mock.py`

**Interfaces:**
- Produces: `http_request()` (unchanged signature) now takes `https://host[:port]/path`.
  - TLS 1.2 through mbedTLS over the existing socket code, with the BIO callbacks on the fd.
  - SNI and hostname check use the URL's host. Verification is required.
  - Trust: the system bundle when one exists (`/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`,
    `/etc/pki/tls/certs/ca-bundle.crt`), plus the embedded roots always.
  - `RECOMP_HTTPS_CA=<pem file>` replaces both. It's for tests only and is honoured only when the host is a local
    address (`net_ip_is_local`).
  - On a failure, `error` says `"TLS: certificate not trusted"` / `"TLS: handshake failed"`.
- Produces: `const char *net_root_pems(void)`: the embedded roots as one NUL-terminated PEM string. The roots are GTS
  Root R1, GTS Root R4, GlobalSign Root CA, ISRG Root X1 and ISRG Root X2. Extract them with
  `security find-certificate -c "<name>" -p /System/Library/Keychains/SystemRootCertificates.keychain` and check each
  SHA-256 fingerprint against `openssl x509 -fingerprint -sha256`.

- [ ] **Step 1: write `tools/net_mock.py`**: a Python HTTP(S) mock (stdlib only) with `--tls <cert> <key>` and
  `--port`. It has two routes for now: `GET /hello` returns 200 `hello`, and `GET /echo-headers` returns the request
  headers. It grows in Task 2. `test_net.sh` makes a throwaway CA and a leaf for `127.0.0.1`/`localhost` in `build/net_test/`
  with `openssl req`.
- [ ] **Step 2: write `tools/test_net.c`** (`main` runs named cases; exit 1 on any failure) with these cases:
  - `https_local_ok`: with `RECOMP_HTTPS_CA` set to the test CA, `http_request("GET","https://localhost:<p>/hello")`
    gives 200 and body `hello`.
  - `https_untrusted`: without `RECOMP_HTTPS_CA`, the same request gives 0 and an error containing `"not trusted"`.
  - `https_live_nextendo` (only when `NET_TEST_LIVE=1`): `GET https://nextendo.network/` gives a status in 200..399.
  - `https_live_selfsigned_refused` (only when `NET_TEST_LIVE=1`): `GET https://51.178.29.194/` gives 0.
  - `http_plain_still_works`: `http://127.0.0.1:<p>/hello` (mock without `--tls`) gives 200.

  `test_net.sh` compiles `test_net.c` with `runtime/net/{http,json,roots}.c` and the mbedTLS library sources (the
  `MBEDTLS_FLAGS` from the Makefile), starts the mocks, runs the cases and prints `ALL PASS`.
- [ ] **Step 3: run `recomp/tools/test_net.sh`.** Expected: FAIL (https unsupported).
- [ ] **Step 4: implement the https path and `roots.c`.**
- [ ] **Step 5: run `tools/test_net.sh`, then `NET_TEST_LIVE=1 tools/test_net.sh`.** Expected: `ALL PASS` both times.
  Also run `make -s GAME=smm2 -j8 runtime-objs`.
- [ ] **Step 6: commit** `recomp net: https:// in http_request (Mbed TLS, system + embedded roots, verified)`.

### Task 2: Nextendo account module: PKCE sign-in, token store, game token

**Files:**
- Create: `recomp/runtime/net/nextendo.c`, `recomp/runtime/net/nextendo.h`
- Create: `recomp/runtime/net/secure_store.h`, `recomp/runtime/net/secure_store.c` (file backend, `#ifndef __APPLE__`),
  `recomp/runtime/net/secure_store_apple.m` (Keychain: `SecItemAdd` / `SecItemCopyMatching` / `SecItemDelete`,
  `kSecClassGenericPassword`, service `GAME_BUNDLE_ID ".nextendo"`, `kSecAttrAccessibleAfterFirstUnlock`)
- Modify: `recomp/Makefile` (new objects, `MBEDTLS_FLAGS` for `nextendo.o`, `-framework Security` on Apple)
- Modify: `recomp/tools/net_mock.py` (OAuth routes), `recomp/tools/test_net.c` (cases below)

**Interfaces:**
- Consumes: `http_request` (Task 1), `json_*` (`runtime/net/json.h`), mbedTLS sha256 and ctr_drbg.
- Produces (`nextendo.h`):
  ```c
  typedef enum { NX_SIGNED_OUT, NX_WAITING_BROWSER, NX_SIGNED_IN, NX_ERROR } NxState;
  typedef struct { NxState state; char username[64]; char message[160]; } NxStatus;
  void nextendo_status(NxStatus *out);                    // thread-safe snapshot, for the UI each frame
  bool nextendo_signed_in(void);                          // a refresh token is stored
  // Desktop: opens a listener on 127.0.0.1:<free port> (thread; 5 min timeout), returns the authorize URL to open.
  // Starting again cancels a pending one. iOS: redirect = "smm2recomp://callback", no listener.
  bool nextendo_begin_sign_in(const char *redirect_uri /* NULL = loopback */, char *url_out, size_t cap);
  bool nextendo_finish_sign_in(const char *callback_url); // "...?code=..&state=.." (iOS; the listener calls it too)
  void nextendo_sign_out(void);                           // forget the tokens (store + memory)
  // Launch: refresh -> access token -> GET /api/nex-token. Blocks for at most timeout_ms. True when a game token is
  // ready. Refetches when the cached one has less than 1 h left.
  bool nextendo_prepare_game_login(int timeout_ms);
  uint64_t nextendo_pid(void);
  bool nextendo_username(char *out, size_t cap);
  size_t nextendo_nex_token(char *out, size_t cap);       // 0: none
  // test hook: the API base ("https://nextendo.network" by default); env RECOMP_NEXTENDO_API, local addresses only
  void nx_pkce_challenge(const char *verifier, char out[44]); // base64url(SHA-256), no padding
  ```
- Produces (`secure_store.h`): `bool secure_get(const char *key, char *out, size_t cap); bool secure_set(const char
  *key, const char *val); void secure_del(const char *key);`.
  - The keys are `refresh_token` and `username` (the nex token and PID are kept in memory only).
  - The file backend writes `net/nextendo.txt` with mode 0600 and the comment "Private: Nextendo sign-in. Never share
    this file."
  - `RECOMP_SECURE_STORE=file` forces the file backend on Apple (for the tests).
- Authorize URL query, in this order: `response_type=code&client_id=nxc_R-JWtJ6JrrgY&redirect_uri=<enc>&scope=identity%20game.matchmaking&state=<16 random bytes b64url>&code_challenge=<S256>&code_challenge_method=S256`.
- The loopback listener answers `/callback` with a small HTML page: "Signed in to Nextendo. You can close this tab
  and go back to the game." on success, and the error text otherwise.

- [ ] **Step 1: extend `net_mock.py`** with these routes:
  - `GET /oauth/authorize`: redirects 302 to `redirect_uri?code=C1&state=<state>`. With `?deny=1` it redirects with
    `error=access_denied` instead.
  - `POST /oauth/token`: for `authorization_code`, checks that `client_id` and `code=C1` are right and that SHA-256 of
    `code_verifier` matches the challenge from the authorize call. For `refresh_token`, it accepts only the latest
    refresh token, rotating `R1` → `R2` → ….
  - `GET /api/nex-token`: Bearer `A*` → `{"nex_token":"nx2.test.sig","pid":1234567,"friend_code":"SW-0000-0000-0000","username":"tester","expires_in":86400}`.
  - `--offline`: `/oauth/token` sleeps 30 s.
- [ ] **Step 2: add the test cases** to `test_net.c`. All of them run with `RECOMP_NEXTENDO_API=http://127.0.0.1:<p>`
  and `RECOMP_SECURE_STORE=file` in a temporary data folder.
  - `pkce_rfc7636`: verifier `dBjftJeZ4CVP-mA92tlbS2aM3hTxbYYHlA6oH1qEr-k` gives
    `E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbBGJSqKsLwI`.
  - `signin_loopback_full`: begin, then the test does a GET on the authorize URL itself and follows the 302 to the
    listener. Then `nextendo_status().state == NX_SIGNED_IN` (within 5 s), `nextendo_prepare_game_login(5000)` is true,
    `nextendo_pid() == 1234567`, the username is `tester`, and the nex token is `nx2.test.sig`.
  - `refresh_rotates_and_persists`: after two `prepare_game_login` calls in fresh processes (or after resetting the
    module), the store holds the mock's latest refresh token.
  - `callback_state_mismatch`: `nextendo_finish_sign_in("…?code=C1&state=bad")` is false, and the state is not
    `NX_SIGNED_IN`.
  - `callback_error_denied`: the `deny=1` flow ends in `NX_ERROR` with a message containing "denied", and the
    listener port is closed (connect fails).
  - `begin_twice_cancels_first`: the first listener's port is closed after the second begin.
  - `prepare_offline_times_out`: with the mock in `--offline`, `prepare_game_login(2000)` returns false in under 3 s.
  - `no_secret_in_logs`: capture `LOG` output for the whole flow and assert it contains none of `R1`, `A1`,
    `nx2.test.sig` or `1234567`. Stub `LOG` in the test build to a buffer.
- [ ] **Step 3: run `tools/test_net.sh`.** Expected: the new cases FAIL.
- [ ] **Step 4: implement `nextendo.c` and `secure_store*`.**
- [ ] **Step 5: run `tools/test_net.sh`.** Expected: `ALL PASS`. Then run `make -s GAME=smm2 -j8 runtime-objs`.
- [ ] **Step 6: commit** `recomp net: Nextendo sign-in (OAuth PKCE, loopback), token store (Keychain / private file), game token`.

### Task 3: The Nextendo backend: routing, pinned TLS, account identity, ID token

**Files:**
- Modify: `recomp/runtime/net/netcfg.c`, `netcfg.h`
- Modify: `recomp/runtime/services/ssl.c` (new trust `NET_TRUST_PINNED`)
- Modify: `recomp/runtime/services/acc.c` (PID, nickname)
- Modify: `recomp/runtime/net/token.c` (RS256 Nextendo token)
- Modify: `recomp/runtime/services/bsd.c` (only if the UDP policy needs it)
- Modify: `recomp/docs/mods.md` ("The game's own network stack": the Nextendo backend)
- Test: `recomp/tools/test_net.c` (netcfg cases; `netcfg.c` builds into the test with a stub `game_net_config`)

**Interfaces:**
- Consumes: `nextendo_signed_in`, `nextendo_prepare_game_login`, `nextendo_pid`, `nextendo_username`, `nextendo_nex_token` (Task 2).
- Produces:
  - `NET_BACKEND_NEXTENDO` becomes available when `nextendo_signed_in()`. Its option description changes to "Nextendo
    Network: Course World and online play on the community servers. Needs a Nextendo account (sign in below)."
  - The effective backend is `nextendo` when the setting is nextendo **and** an account is signed in. Otherwise it is
    `ours`, with `LOG` "[net] backend nextendo: not signed in; using our server".
  - `net_real_sockets()` is true for `LOCAL_NEX` and `NEXTENDO`.
  - `NetTrust` gains `NET_TRUST_PINNED`. `net_tls_trust()` returns it for backend nextendo.
    `bool net_tls_pin(uint8_t out[32])` returns the SPKI SHA-256 to require.
  - New `bool net_dest_allowed_udp(uint32_t ip)`: for nextendo, any address (PIA peers); for local-nex, as before.
    `net_dest_allowed` (TCP) for nextendo allows only the server IP and the NAT IP.
- Routing for nextendo, first match wins:
  1. `net_hosts=` entries (local addresses only, as today)
  2. `nncs2-lp1.n.n.srv.nintendo.net` → NAT IP
  3. `*.nintendo.net`, `*.nintendo.com`, `*.nintendowifi.net`, `*.nintendo.co.jp`, `*.nintendoswitch.cn` → server IP

  Rules 2–3 come from the built-in Nextendo targets. Give `add_rule` a `bool allow_public` parameter for those; user
  `net_hosts=` stays local-only.
- `ssl.c` for `NET_TRUST_PINNED`:
  - `VERIFY_OPTIONAL`, then after the handshake hash the peer certificate's SPKI (`mbedtls_pk_write_pubkey_der`) and
    compare it with the pin.
  - A mismatch returns `R_VERIFY_FAILED` and logs `"[ssl] <sni>: certificate pin mismatch (expected Nextendo's game server); refused"`.
  - SNI and ALPN stay as they are.
- `acc.c`: with backend nextendo, `GetAccountId` returns `nextendo_pid()`, and Profile `Get` / `GetBase` give the
  username as the nickname (cut to 32 bytes, UTF-8 safe). `LoadIdTokenCache` calls `nextendo_prepare_game_login(10000)`
  first.
- `token.c`: with backend nextendo, `net_id_token` makes an **RS256** JWT signed with a local RSA-2048 key. The key is
  made once with mbedTLS `GENPRIME` and stored in `net/nextendo-jwt-key.pem` (0600).
  - Header: `{"alg":"RS256","typ":"JWT","kid":"nextendo-baas-key-1"}`.
  - Claims: `iss` = `https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com`, `jku` = iss + `/1.0.0/certificates`,
    `aud` `ed9e2f05d286f7b8`, `sub` = PID as 16 hex digits, `iat`, `exp` = iat + 3 h, `jti` random, `typ` `id_token`,
    `di` / `bs:did` random 16 hex digits (kept in the key file), `sn` `XAW10000000000`, `hm` true,
    `nintendo` {`ai` title ID, `av` version}, `tv` = `GAME_DISPLAY_VERSION`, `nnex` = `nextendo_nex_token()`.
  - If there's no nex token, return 0 (the game then reports its own login error). Never send an empty `nnex`.

- [ ] **Step 1: add the test cases** to `test_net.c`:
  - `nextendo_routes`: with the backend set to nextendo and a signed-in stub, `net_resolve("g22306d00-lp1.s.n.srv.nintendo.net")`
    gives 51.178.29.194, `nncs2-lp1.n.n.srv.nintendo.net` gives 164.132.111.120, `nncs1-lp1.n.n.srv.nintendo.net` gives
    51.178.29.194, and `www.google.com` is false.
  - `nextendo_dest`: TCP `net_dest_allowed` is true for both Nextendo IPs and false for 8.8.8.8. UDP is true for
    203.0.113.5.
  - `nextendo_not_signed_in_falls_back`: `net_backend()` is `NET_BACKEND_OURS`.
  - `id_token_rs256`: split the token, base64url-decode the header and claims, check `alg` RS256 and that `nnex` equals
    the stub's token, verify the signature with the stored key's public half (`mbedtls_pk_verify`), and check that the
    token fits 2048 bytes.
  - `pin_live_ok` (only when `NET_TEST_LIVE=1`): a TLS handshake through the `ssl.c` pin check (factor it as
    `bool ssl_pin_check(const mbedtls_x509_crt *peer, const uint8_t pin[32])` so the test can call it on a handshake it
    does itself) against 51.178.29.194:443 with SNI `g22306d00-lp1.s.n.srv.nintendo.net` is accepted.
  - `pin_mismatch_refused`: `ssl_pin_check` with the local test leaf from Task 1 and the Nextendo pin returns false.
- [ ] **Step 2: run `tools/test_net.sh`.** Expected: the new cases FAIL.
- [ ] **Step 3: implement netcfg / ssl / acc / token.**
- [ ] **Step 4: run `tools/test_net.sh` and `NET_TEST_LIVE=1 tools/test_net.sh`.** Expected: `ALL PASS`. Run
  `make -s GAME=smm2 -j8 runtime-objs` and `scripts/check.sh`.
- [ ] **Step 5: regression.** Default backend `ours`, one game run with `RECOMP_NET_LOG=1`: no socket, no lookup, as
  in Proof 1's "default backend unchanged". With `backend=local-nex` and `tools/nex_test_server`, Course World's top
  screen still opens (Proof 2).
- [ ] **Step 6: commit** `recomp net: backend nextendo - Nextendo routing, pinned game TLS, PID / username / RS256 ID token with nnex`.

### Task 4: Settings: the Online section (backend choice, Sign in / Sign out), iOS sign-in sheet

**Files:**
- Create: `recomp/runtime/ui/online_rows.mm` (the section; drawn from `tab_mods`)
- Create: `recomp/runtime/ui/nextendo_ios.mm` (`ASWebAuthenticationSession`; built for ios/iossim only)
- Modify: `recomp/runtime/ui/menu.mm` (`tab_mods` calls `online_rows()` before `launcher_settings_rows()`; `have_mods`
  is also true when `online_rows_available()`)
- Modify: `recomp/games/smm2/mods/settings.c` (remove the "Online: local NEX test server" toggle and `t_local_nex`;
  the choice replaces them)
- Modify: `recomp/Makefile` (new UI objects; `-framework AuthenticationServices` on iOS)

**Interfaces:**
- Consumes: `net_backend_options`, `net_backend_select`, `net_backend_setting` (netcfg); `nextendo_*` (Task 2).
- Produces: `extern "C" bool online_rows_available(void);` (true on every game with netcfg; SMM2 always) and
  `extern "C" void online_rows(void);`.
- Rows, inside `opt_section("Online")`:
  1. `opt_choice("Online backend", ...)` over the available options' labels. Changing it calls `net_backend_select`
     and shows "Takes effect at the next start" in the description. Nextendo is greyed out (`opt_disable_next`) until
     signed in.
  2. `opt_action("Nextendo account", value, desc)`. The value is `"Sign in"` when signed out, `"Waiting for the browser…"`
     while waiting, the username when signed in, and the message on error.
     - Activating it while signed out: on desktop/Android, `nextendo_begin_sign_in(NULL, url)` then `SDL_OpenURL(url)`.
       On iOS, `nextendo_ios_sign_in()`.
     - Activating it while signed in shows `opt_action("Sign out", "", "Forget this computer's Nextendo sign-in.")`
       underneath, which calls `nextendo_sign_out()`.
     - Description: "Sign in on nextendo.network in your browser. The game never sees your password; it keeps a
       sign-in token for this device."
- `nextendo_ios.mm`: `extern "C" void nextendo_ios_sign_in(void)`.
  - It calls `nextendo_begin_sign_in("smm2recomp://callback", url)`, then starts `ASWebAuthenticationSession` with
    `callbackURLScheme` `smm2recomp`, `prefersEphemeralWebBrowserSession = NO`, and the key window as the presentation
    anchor.
  - Its completion handler calls `nextendo_finish_sign_in(url.absoluteString)` and, on a cancel, sets the error message
    "Sign-in cancelled".

- [ ] **Step 1: build macOS** (`make GAME=smm2 -j8`), open the menu (the `/run` skill), take a screenshot of the
  Mods tab, and check that the Online section shows "Online backend: Our server" and "Nextendo account: Sign in".
- [ ] **Step 2: macOS sign-in against the mock** (`RECOMP_NEXTENDO_API=http://127.0.0.1:<p>`): activate the row; the
  browser opens the mock, redirects, and the row shows `tester`. Choose Nextendo, restart, and check that the
  setting stuck and the row is still signed in.
- [ ] **Step 3: build iOS** (`tools/make_ios_app.sh`), install it on the iPad (see `~/.claude/CLAUDE.md` / memory
  ipad-device-testing), and check that the row opens the sign-in sheet. The live sheet needs the owner, so it's fine
  to stop at the sheet appearing.
- [ ] **Step 4: commit** `recomp ui: Online section - backend choice, Nextendo Sign in / Sign out (browser; iOS sign-in sheet)`.

### Task 5: Live: the owner signs in, Course World on Nextendo

**Files:** `PROGRESS.md`, `DECISIONS.md`, `research/specs/recomp-nextendo.md` (status), `recomp/docs/mods.md`.

- [ ] **Step 1:** the owner adds both redirect URIs on the portal: `http://127.0.0.1/callback` and `smm2recomp://callback`.
- [ ] **Step 2:** the owner signs in from the macOS build (live nextendo.network). Check in the log: `[nextendo]
  signed in`, a game token fetched (length only), and the backend in effect is nextendo.
- [ ] **Step 3:** start the game with `RECOMP_NET_LOG=1` and open Course World. Pass: the game resolves
  `g22306d00-lp1…` to 51.178.29.194, the pin passes, the NEX login is accepted, and Course World's top screen opens and
  one course downloads and plays. If the login is refused, fetch `/api/online-status`, show its reason in the status
  row, and record it.
- [ ] **Step 4:** update the records and commit `Nextendo: live sign-in and Course World (records)`.
