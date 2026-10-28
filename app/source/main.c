// Switch Discord Activity app (.nro) - one-time Discord login.
//
// Log in once: scan the QR code with your phone, approve in Discord, done.
// The refresh token is saved to sdmc:/config/switchrpc_token and the
// background sysmodule keeps your status up to date on its own from then on.
//
// The login flow is the one from llsc12/SwitchRPC (MIT): Discord Social SDK
// OAuth2 with PKCE, bounced through static.llsc12.me back to this console.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <switch.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <json-c/json_util.h>
#include "config.h"
#include "qrcodegen.h"
#include "ui.h"
#include "update.h"

#define SYSMODULE_ID  SYSMODULE_TID
#define TOKEN_PATH    "sdmc:/config/switchrpc_token"
#define USER_PATH     "sdmc:/config/switchrpc_user"
#define LOG_PATH      "sdmc:/switchrpc_log.txt"
#define STATUS_PATH   "sdmc:/config/switchrpc_status"
#define MODULE_DIR    "sdmc:/atmosphere/contents/3120000000000092"
// the earlier PC-based version of this project ("Switch Discord RPC")
#define OLD_MODULE    "sdmc:/atmosphere/contents/420000000000D15C"
#define OLD_NRO       "sdmc:/switch/switch-rpc.nro"
#define LOGIN_PORT    45601
#define CLIENT_ID     DISCORD_CLIENT_ID
#define REDIRECT_URI  AUTH_SHIM
#define SCOPE         "openid sdk.social_layer_presence"


// --- small helpers ----------------------------------------------------------

static void b64url(const u8 *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        u32 v = in[i] << 16;
        if (i + 1 < len) v |= in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = tbl[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = tbl[v & 63];
    }
    out[o] = '\0';
}

static void urlencode(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    for (; *in && o + 4 < out_size; in++) {
        unsigned char c = *in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            o += snprintf(out + o, out_size - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

// rm -r
static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            remove_tree(child);
        }
        closedir(dir);
        rmdir(path);
    } else {
        remove(path);
    }
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static bool sysmodule_running(void) {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, SYSMODULE_ID));
}

static bool get_ip(char *out, size_t size) {
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0)
        return false;
    snprintf(out, size, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
    return true;
}

// QR code as a size*size grid (nonzero = dark), for ui_login
static int make_qr(const char *text, uint8_t *grid, int max) {
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, 1, 5, qrcodegen_Mask_AUTO, true))
        return 0;
    int size = qrcodegen_getSize(qr);
    if (size > max) return 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            grid[y * size + x] = qrcodegen_getModule(qr, x, y);
    return size;
}

// --- HTTPS (Discord) --------------------------------------------------------

static size_t collect(char *ptr, size_t size, size_t nmemb, void *userdata) {
    char **buf = userdata;
    size_t old = *buf ? strlen(*buf) : 0, add = size * nmemb;
    char *n = realloc(*buf, old + add + 1);
    if (!n) return 0;
    memcpy(n + old, ptr, add);
    n[old + add] = '\0';
    *buf = n;
    return add;
}

// Returns the parsed JSON body of a 2xx response, or NULL.
static json_object *http_json(const char *url, const char *post_body, const char *bearer) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char *body = NULL;
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/json");
    char auth[1024];
    if (bearer) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
        headers = curl_slist_append(headers, auth);
    }
    if (post_body) {
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    // same as the sysmodule: the console's CA store can't verify these hosts
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    json_object *out = NULL;
    long code = 0;
    if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code >= 200 && code < 300 && body)
            out = json_tokener_parse(body);
    }
    free(body);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return out;
}

static const char *jstr(json_object *o, const char *key) {
    json_object *v;
    return (o && json_object_object_get_ex(o, key, &v)) ? json_object_get_string(v) : NULL;
}

// --- login web server ------------------------------------------------------

typedef struct {
    int fd;
    char verifier[96];
    char challenge[48];
    char state[40];
    char ip[16];
    char code[256];
    bool got_code;
} Login;

static void send_str(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = send(fd, s, n, 0);
        if (w <= 0) return;
        s += w; n -= w;
    }
}

static void query_param(const char *query, const char *key, char *out, size_t size) {
    out[0] = '\0';
    size_t klen = strlen(key);
    for (const char *p = query; p && *p; ) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && *p != ' ' && i + 1 < size)
                out[i++] = *p++;
            out[i] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void serve_one(Login *lg, int c) {
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[4096];
    ssize_t n = recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';

    // "GET /path?query HTTP/1.1"
    char *path = strchr(req, ' ');
    if (!path) return;
    path++;
    char *end = strchr(path, ' ');
    if (end) *end = '\0';
    char *query = strchr(path, '?');
    if (query) *query++ = '\0';

    static char resp[4096];
    if (!strcmp(path, "/callback") && query) {
        char state[64];
        query_param(query, "state", state, sizeof(state));
        query_param(query, "code", lg->code, sizeof(lg->code));
        if (lg->code[0] && !strcmp(state, lg->state)) {
            lg->got_code = true;
            send_str(c, "HTTP/1.1 302 Found\r\nLocation: " AUTH_SHIM "/complete\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            send_str(c, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                        "Login link expired - start the login again on the Switch.");
        }
        return;
    }

    if (!strcmp(path, "/")) {
        char auth[1024], auth_enc[1536], redirect_enc[128], scope_enc[96];
        urlencode(REDIRECT_URI, redirect_enc, sizeof(redirect_enc));
        urlencode(SCOPE, scope_enc, sizeof(scope_enc));
        snprintf(auth, sizeof(auth),
                 "https://discord.com/oauth2/authorize?client_id=" CLIENT_ID
                 "&response_type=code&scope=%s&code_challenge=%s&code_challenge_method=S256"
                 "&state=%s&redirect_uri=%s",
                 scope_enc, lg->challenge, lg->state, redirect_enc);
        urlencode(auth, auth_enc, sizeof(auth_enc));
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 302 Found\r\nLocation: " AUTH_SHIM "?ip=%s&auth=%s\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n",
                 lg->ip, auth_enc);
        send_str(c, resp);
        return;
    }

    send_str(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

static bool login_start(Login *lg) {
    memset(lg, 0, sizeof(*lg));
    lg->fd = -1;
    if (!get_ip(lg->ip, sizeof(lg->ip)))
        return false;

    // PKCE
    u8 rnd[64], hash[32], st[16];
    randomGet(rnd, sizeof(rnd));
    b64url(rnd, sizeof(rnd), lg->verifier);
    sha256CalculateHash(hash, lg->verifier, strlen(lg->verifier));
    b64url(hash, sizeof(hash), lg->challenge);
    randomGet(st, sizeof(st));
    for (int i = 0; i < 16; i++)
        sprintf(lg->state + i * 2, "%02x", st[i]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(LOGIN_PORT),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return false;
    }
    lg->fd = fd;
    return true;
}

static void login_poll(Login *lg) {
    struct pollfd p = { .fd = lg->fd, .events = POLLIN };
    if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        int c = accept(lg->fd, NULL, NULL);
        if (c >= 0) {
            serve_one(lg, c);
            close(c);
        }
    }
}

static void login_stop(Login *lg) {
    if (lg->fd >= 0) close(lg->fd);
    lg->fd = -1;
}

// Swaps the code for tokens and stores the refresh token. Returns the
// Discord username on success.
static bool login_finish(Login *lg, char *user, size_t user_size) {
    char redirect_enc[128], body[1024];
    urlencode(REDIRECT_URI, redirect_enc, sizeof(redirect_enc));
    snprintf(body, sizeof(body),
             "grant_type=authorization_code&redirect_uri=%s&client_id=" CLIENT_ID "&code=%s&code_verifier=%s",
             redirect_enc, lg->code, lg->verifier);

    json_object *tok = http_json("https://gaming-sdk.com/api/v9/oauth2/token", body, NULL);
    const char *refresh = jstr(tok, "refresh_token"), *access = jstr(tok, "access_token");
    if (!refresh || !access) {
        json_object_put(tok);
        return false;
    }

    mkdir("sdmc:/config", 0777);
    FILE *f = fopen(TOKEN_PATH, "w");
    if (!f) {
        json_object_put(tok);
        return false;
    }
    fputs(refresh, f);
    fclose(f);

    snprintf(user, user_size, "your account");
    json_object *me = http_json("https://gaming-sdk.com/api/v9/oauth2/@me", NULL, access);
    json_object *u;
    if (me && json_object_object_get_ex(me, "user", &u)) {
        const char *name = jstr(u, "global_name");
        if (!name) name = jstr(u, "username");
        if (name) snprintf(user, user_size, "%s", name);
    }
    json_object_put(me);
    json_object_put(tok);
    FILE *uf = fopen(USER_PATH, "w");
    if (uf) { fputs(user, uf); fclose(uf); }
    return true;
}

// --- UI ------------------------------------------------------------------------

typedef enum { Screen_Main, Screen_Login, Screen_Test, Screen_IconsAsk, Screen_Icons, Screen_Diag, Screen_Update } Screen;

typedef struct {
    char fw[32], ams[32];
    bool on_sd, running, logged_in, online;
    char user[64];
    char launch[80];       // result of the last "start module" attempt
    const char *status_color;
    char status[200];      // what the module is doing right now (plain text)
    char status_state[16]; // live / idle / error / login / off, from the status file
    char status_raw[160];  // the module's text as is
    bool status_fresh;     // module wrote its status file recently = it's alive
    char icon[200];        // image URL the module sent to Discord
    long long clock_off;   // real time minus console clock, seconds
    bool have_clock;
    char log[20][200];
    int log_lines;
} Diag;

static void read_line_file(const char *path, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, size, f))
        out[strcspn(out, "\r\n")] = '\0';
    fclose(f);
}

static bool interesting(const char *line) {
    // the module logs every HTTP header; keep the lines that mean something
    return !strstr(line, "[cURL") && !strstr(line, "Response Body") && !strstr(line, "Request Body") &&
           !strstr(line, "Response length") && !strstr(line, "(No response expected)") &&
           !strstr(line, "Including session token") && line[0];
}

static void read_log(Diag *d) {
    d->log_lines = 0;
    d->status_color = "\x1b[33m";
    snprintf(d->status, sizeof(d->status), "Module hasn't written a log yet (is it running?)");
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return;

    // only the tail matters
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > 16384 ? size - 16384 : 0, SEEK_SET);
    if (size > 16384) { char skip[512]; fgets(skip, sizeof(skip), f); }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        // "[hh:mm:ss] message" -> one-line status from the latest meaningful event
        const char *msg = strchr(line, ']') ? strchr(line, ']') + 2 : line;
        if (strstr(line, "Status is live")) {
            d->status_color = "\x1b[32m";
            snprintf(d->status, sizeof(d->status), "%.*s", (int)ui_text_fit(msg, 90, false), msg);
        } else if (strstr(line, "Clearing") || strstr(line, "Sleep imminent")) {
            d->status_color = "";
            snprintf(d->status, sizeof(d->status), "No game running - status cleared. Start a game, wait ~10 s.");
        } else if (strstr(line, "Not logged in yet") || strstr(line, "Refresh token is empty")) {
            d->status_color = "\x1b[33m";
            snprintf(d->status, sizeof(d->status), "Not logged in - press A.");
        } else if (strstr(line, "HTTP 4") || strstr(line, "Failed") || strstr(line, "failed")) {
            d->status_color = "\x1b[31m";
            snprintf(d->status, sizeof(d->status), "%.*s", (int)ui_text_fit(msg, 90, false), msg);
        } else if (strstr(line, "Sysmodule started")) {
            d->status_color = "";
            snprintf(d->status, sizeof(d->status), "Module started. Start a game and wait ~10 s.");
        }

        if (!interesting(line))
            continue;
        // wrap instead of cutting: the reason for an error is at the end
        for (const char *p = line; *p;) {
            if (d->log_lines == 20) {
                memmove(d->log[0], d->log[1], sizeof(d->log[0]) * 19);
                d->log_lines = 19;
            }
            bool first = p == line;
            size_t n = ui_text_fit(p, first ? TEXT_COLS : TEXT_COLS - 2, true);
            snprintf(d->log[d->log_lines++], sizeof(d->log[0]), "%s%.*s", first ? "" : "  ", (int)n, p);
            p += n;
            while (*p == ' ') p++;
        }
    }
    fclose(f);
}

// The module rewrites sdmc:/config/switchrpc_status on every change and at
// least once a minute: "<unix time>\n<state>\n<text>\n".
static void read_status(Diag *d) {
    d->status_fresh = false;
    d->status_state[0] = d->status_raw[0] = '\0';
    FILE *f = fopen(STATUS_PATH, "r");
    if (!f) return;
    char when[32] = "", state[16] = "", text[160] = "";
    if (fgets(when, sizeof(when), f) && fgets(state, sizeof(state), f)) {
        if (!fgets(text, sizeof(text), f)) text[0] = '\0';
        char off[32] = "";
        d->icon[0] = '\0';
        if (fgets(d->icon, sizeof(d->icon), f))
            d->icon[strcspn(d->icon, "\r\n")] = '\0';
        d->have_clock = fgets(off, sizeof(off), f) != NULL;
        d->clock_off = strtoll(off, NULL, 10);
        state[strcspn(state, "\r\n")] = '\0';
        text[strcspn(text, "\r\n")] = '\0';
        snprintf(d->status_state, sizeof(d->status_state), "%s", state);
        snprintf(d->status_raw, sizeof(d->status_raw), "%s", text);
        u64 now = 0;
        timeGetCurrentTime(TimeType_Default, &now);
        u64 t = strtoull(when, NULL, 10);
        d->status_fresh = now >= t ? now - t <= 150 : true;

        d->status_color = !strcmp(state, "live") ? "\x1b[32m" : !strcmp(state, "error") ? "\x1b[31m"
                        : !strcmp(state, "login") || !strcmp(state, "off") ? "\x1b[33m" : "";
        if (!strcmp(state, "live"))
            snprintf(d->status, sizeof(d->status), "Live on Discord: %s", text);
        else
            snprintf(d->status, sizeof(d->status), "%s", text);
    }
    fclose(f);
}

static void diag_refresh(Diag *d, bool have_pm) {
    d->on_sd = file_exists(MODULE_DIR "/exefs.nsp") && access(MODULE_DIR "/flags/boot2.flag", F_OK) == 0;
    read_log(d);
    read_status(d);  // overrides the log-based guess when present
    d->running = (have_pm && sysmodule_running()) || d->status_fresh;
    d->logged_in = file_exists(TOKEN_PATH);
    read_line_file(USER_PATH, d->user, sizeof(d->user));

    NifmInternetConnectionType type;
    u32 strength;
    NifmInternetConnectionStatus st;
    d->online = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &st)) &&
                st == NifmInternetConnectionStatus_Connected;
}

static void diag_versions(Diag *d) {
    snprintf(d->fw, sizeof(d->fw), "?");
    snprintf(d->ams, sizeof(d->ams), "?");
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            snprintf(d->fw, sizeof(d->fw), "%s", fw.display_version);
        setsysExit();
    }
    if (R_SUCCEEDED(splInitialize())) {
        u64 v = 0;
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &v)))  // Atmosphere API version
            snprintf(d->ams, sizeof(d->ams), "%u.%u.%u",
                     (unsigned)((v >> 56) & 0xFF), (unsigned)((v >> 48) & 0xFF), (unsigned)((v >> 40) & 0xFF));
        splExit();
    }
}

static void start_module(Diag *d) {
    if (d->running) {
        snprintf(d->launch, sizeof(d->launch), "\x1b[32malready running, nothing to do\x1b[0m");
        return;
    }
    if (R_FAILED(pmshellInitialize())) {
        snprintf(d->launch, sizeof(d->launch), "\x1b[31mcan't talk to pm:shell\x1b[0m");
        return;
    }
    NcmProgramLocation loc = { .program_id = SYSMODULE_ID, .storageID = NcmStorageId_None };
    u64 pid = 0;
    Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_SUCCEEDED(rc))
        snprintf(d->launch, sizeof(d->launch), "\x1b[32mstarted\x1b[0m");
    else if (rc == MAKERESULT(Module_Kernel, KernelError_OutOfResource))
        // 2001-0132: no room for another system process (usually: it's already running)
        snprintf(d->launch, sizeof(d->launch), "\x1b[33mno free system memory (2001-0132) - reboot instead\x1b[0m");
    else
        snprintf(d->launch, sizeof(d->launch), "\x1b[31mAtmosphere refused, error %04u-%04u\x1b[0m",
                 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
}

static const char *mark(bool ok) { return ok ? "\x1b[32m[ OK ]\x1b[0m" : "\x1b[31m[FAIL]\x1b[0m"; }

#define DIAG_LINES 26
static char diag_buf[DIAG_LINES][160];

static int diag_lines(const Diag *d, const char **out) {
    int n = 0;
#define L(...) do { if (n < DIAG_LINES) { snprintf(diag_buf[n], sizeof(diag_buf[0]), __VA_ARGS__); out[n] = diag_buf[n]; n++; } } while (0)
    L("Switch Discord Activity v" SWITCHRPC_VERSION "   firmware %s   Atmosphere %s", d->fw, d->ams);
    L("%s Module on the SD card%s", mark(d->on_sd), d->on_sd ? "" : " - copy atmosphere/ to the SD root");
    L("%s Module running%s", mark(d->running), d->running ? "" : " - press Y to start it");
    if (d->launch[0]) L("       Start module: %s", d->launch);
    L("%s Logged in to Discord%s%s", d->logged_in ? mark(true) : "\x1b[33m[ -- ]\x1b[0m",
      d->logged_in ? " as " : "", d->logged_in ? (d->user[0] ? d->user : "you") : "");
    L("%s Internet", mark(d->online));
    L("Now: %s%.*s\x1b[0m", d->status_color ? d->status_color : "", (int)ui_text_fit(d->status, TEXT_COLS - 5, false), d->status);
    if (d->icon[0]) L("Icon: %.70s", d->icon);
    if (d->have_clock && (d->clock_off > 60 || d->clock_off < -60))
        L("\x1b[33mConsole clock is %lld min %s - the module corrects for it.\x1b[0m",
          (d->clock_off < 0 ? -d->clock_off : d->clock_off) / 60, d->clock_off < 0 ? "fast" : "slow");
    L("\x1b[36mLog\x1b[0m (sdmc:/switchrpc_log.txt):");
    int first = d->log_lines - (DIAG_LINES - n);
    for (int i = first < 0 ? 0 : first; i < d->log_lines; i++)
        L("\x1b[2m%s\x1b[0m", d->log[i]);
#undef L
    return n;
}

// --- settings (sdmc:/config/switchrpc.ini, read by the module) ----------------


static void save_settings(const bool *v, Lang lang) {
    settings_save(v, lang == Lang_Ja ? 1 : 0);
}

static Lang console_lang(void) {
    u64 code = 0;
    SetLanguage l;
    if (R_SUCCEEDED(setInitialize())) {
        bool ja = R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &l)) && l == SetLanguage_JA;
        setExit();
        return ja ? Lang_Ja : Lang_En;
    }
    return Lang_En;
}


// --- "Test" button: the module's Discord requests, step by step, on screen ----

#define SUPER_PROPS "X-Super-Properties: eyJicm93c2VyIjoiRGlzY29yZCBFbWJlZGRlZCIsImJyb3dzZXJfdXNlcl9hZ2VudCI6IkRpc2NvcmQgRW1iZWRkZWQvMC4wLjgiLCJicm93c2VyX3ZlcnNpb24iOiIwLjAuOCIsImNsaWVudF9idWlsZF9udW1iZXIiOjMwNDY4MywiZGVzaWduX2lkIjowLCJvcyI6IlVua25vd24iLCJyZWxlYXNlX2NoYW5uZWwiOiJ1bmtub3duIn0K"

typedef struct {
    char lines[24][200];
    int n;
    char session[512];   // test session token, deleted when leaving the screen
    char access[512];
} Test;

static void test_line(Test *t, const char *color, const char *fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *p = buf;
    do {
        if (t->n == 24) return;
        size_t n = ui_text_fit(p, TEXT_COLS, true);
        snprintf(t->lines[t->n++], sizeof(t->lines[0]), "%s%.*s\x1b[0m", color, (int)n, p);
        p += n;
        while (*p == ' ') p++;
    } while (*p);
}

static void draw_test(const Test *t, bool done) {
    const char *lines[24];
    for (int i = 0; i < t->n; i++) lines[i] = t->lines[i];
    ui_frame_begin();
    ui_lines(tr(T_TestTitle), lines, t->n, done ? tr(T_FooterBack) : tr(T_FooterWorking));
    ui_frame_end();
}

// Like the module's sendRequest: same headers, any status code comes back.
static char *discord_req(const char *url, const char *method, const char *ctype, const char *body,
                         const char *bearer, long *code, char *err, size_t err_size) {
    CURL *curl = curl_easy_init();
    char *resp = NULL;
    *code = 0;
    err[0] = '\0';
    if (!curl) { snprintf(err, err_size, "curl init failed"); return NULL; }
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "User-Agent: Discord Embedded/0.0.8");
    h = curl_slist_append(h, SUPER_PROPS);
    char tmp[700];
    if (ctype) { snprintf(tmp, sizeof(tmp), "Content-Type: %s", ctype); h = curl_slist_append(h, tmp); }
    if (bearer) { snprintf(tmp, sizeof(tmp), "Authorization: Bearer %s", bearer); h = curl_slist_append(h, tmp); }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        snprintf(err, err_size, "%s", curl_easy_strerror(rc));
    else
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, code);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return resp;
}

static void run_test(Test *t) {
    memset(t, 0, sizeof(*t));
    long code;
    char err[160];

    // 1. refresh token -> access token (exactly what the module does first)
    char refresh[1024] = "";
    read_line_file(TOKEN_PATH, refresh, sizeof(refresh));
    if (!refresh[0]) {
        test_line(t, "\x1b[31m", "1. Login: no token on the SD card - press B, then A to log in.");
        return;
    }
    test_line(t, "", "1. Refreshing the Discord login...");
    draw_test(t, false);
    char body[1400];
    snprintf(body, sizeof(body), "grant_type=refresh_token&refresh_token=%s&client_id=" CLIENT_ID, refresh);
    char *resp = discord_req("https://gaming-sdk.com/api/v9/oauth2/token", "POST",
                             "application/x-www-form-urlencoded; charset=utf-8", body, NULL, &code, err, sizeof(err));
    json_object *tok = resp ? json_tokener_parse(resp) : NULL;
    const char *new_refresh = jstr(tok, "refresh_token"), *access = jstr(tok, "access_token");
    if (!access) {
        if (err[0]) test_line(t, "\x1b[31m", "   FAILED: no connection (%s)", err);
        else test_line(t, "\x1b[31m", "   FAILED: HTTP %ld %s", code, resp ? resp : "");
        if (code == 400 || code == 401)
            test_line(t, "\x1b[33m", "   -> the login is no longer valid: press B, X, then A to log in again.");
        json_object_put(tok);
        free(resp);
        return;
    }
    snprintf(t->access, sizeof(t->access), "%s", access);
    if (new_refresh) {
        // refresh tokens rotate: keep the new one for the module
        FILE *f = fopen(TOKEN_PATH, "w");
        if (f) { fputs(new_refresh, f); fclose(f); }
    }
    test_line(t, "\x1b[32m", "   OK (HTTP %ld), scopes: %s", code, jstr(tok, "scope") ? jstr(tok, "scope") : "?");
    json_object_put(tok);
    free(resp);

    // 2. who am I
    test_line(t, "", "2. Checking the account...");
    draw_test(t, false);
    resp = discord_req("https://gaming-sdk.com/api/v9/oauth2/@me", "GET", NULL, NULL, t->access, &code, err, sizeof(err));
    json_object *me = resp ? json_tokener_parse(resp) : NULL, *u;
    if (me && json_object_object_get_ex(me, "user", &u))
        test_line(t, "\x1b[32m", "   OK, logged in as %s", jstr(u, "username") ? jstr(u, "username") : "?");
    else
        test_line(t, "\x1b[31m", "   FAILED: HTTP %ld %s%s", code, err, resp ? resp : "");
    json_object_put(me);
    free(resp);

    // 3. the headless session itself, same JSON shape as the module
    test_line(t, "", "3. Creating a test status \"Switch Discord Activity test\"...");
    draw_test(t, false);
    snprintf(body, sizeof(body),
             "{\"activities\":[{\"type\":\"0\",\"flags\":\"0\",\"application_id\":\"" CLIENT_ID "\","
             "\"platform\":\"desktop\",\"name\":\"Switch Discord Activity test\",\"state\":\"on Nintendo Switch\","
             "\"assets\":{\"large_image\":\"https://raw.githubusercontent.com/switchbrew/nx-hbmenu/master/icon.jpg\"}}]}");
    resp = discord_req("https://gaming-sdk.com/api/v9/users/@me/headless-sessions", "POST",
                       "application/json; charset=utf-8", body, t->access, &code, err, sizeof(err));
    json_object *sess = resp ? json_tokener_parse(resp) : NULL;
    if (code >= 200 && code < 300 && jstr(sess, "token")) {
        snprintf(t->session, sizeof(t->session), "%s", jstr(sess, "token"));
        test_line(t, "\x1b[32m", "   OK (HTTP %ld) - look at Discord now: \"Switch Discord Activity test\".", code);
        test_line(t, "", "   If it's there, Discord works and the problem is in the module.");
        test_line(t, "", "   If it's NOT there although this says OK: Discord -> Settings ->");
        test_line(t, "", "   Activity Privacy -> share your activity.");
    } else if (err[0]) {
        test_line(t, "\x1b[31m", "   FAILED: no connection (%s)", err);
    } else {
        test_line(t, "\x1b[31m", "   FAILED: HTTP %ld %s", code, resp ? resp : "");
    }
    json_object_put(sess);
    free(resp);
}

static void end_test(Test *t) {
    if (!t->session[0]) return;
    char body[700], err[160];
    long code;
    snprintf(body, sizeof(body), "{\"token\":\"%s\"}", t->session);
    free(discord_req("https://gaming-sdk.com/api/v9/users/@me/headless-sessions/delete", "POST",
                     "application/json; charset=utf-8", body, t->access, &code, err, sizeof(err)));
    t->session[0] = '\0';
}


// --- "Prepare icons" (L): icons for forwarders & co, uploaded once ------------
//
// Discord only shows images by URL. For every installed title no icon
// database knows, the app uploads the title's own icon (catbox.moe,
// freeimage.host as a fallback) and writes the link to the cache the module
// reads - so the module never has to upload anything itself.

#define ESHOP_CACHE  "sdmc:/config/switchrpc-eshop-iconcache.json"
#define UPLOAD_CACHE "sdmc:/config/switchrpc-uploaded-icons.json"
// titles already looked at that have no icon to upload ("none")
#define CHECKED_CACHE "sdmc:/config/switchrpc-icons-checked.json"

typedef struct {
    char lines[24][200];
    int n;
    int done, total, uploaded, failed, skipped, found, repaired;
} IconJob;

static void job_line(IconJob *j, const char *color, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *p = buf;
    do {
        if (j->n == 24) { memmove(j->lines[0], j->lines[1], sizeof(j->lines[0]) * 23); j->n = 23; }
        size_t n = ui_text_fit(p, TEXT_COLS, true);
        snprintf(j->lines[j->n++], sizeof(j->lines[0]), "%s%.*s\x1b[0m", color, (int)n, p);
        p += n;
        while (*p == ' ') p++;
    } while (*p);
}

static void draw_icons(const IconJob *j, bool finished) {
    const char *lines[24];
    for (int i = 0; i < j->n; i++) lines[i] = j->lines[i];
    char title[128];
    snprintf(title, sizeof(title), "%s  %d/%d", tr(T_IconsTitle), j->done, j->total);
    ui_frame_begin();
    ui_lines(title, lines, j->n, finished ? tr(T_FooterBack) : tr(T_FooterWorking));
    ui_frame_end();
}

static json_object *cache_load(const char *path) {
    json_object *o = json_object_from_file(path);
    if (!o || !json_object_is_type(o, json_type_object)) {
        json_object_put(o);
        o = json_object_new_object();
    }
    return o;
}

// Several titles are worked on at once (one thread, curl's multi interface):
// each request is mostly waiting on the network, so doing them one after the
// other made a full run take minutes. Per title, same as 1.0: the eShop icon,
// else tinfoil's, else the title's own icon uploaded (catbox.moe, then
// freeimage.host).
#define LANES 6
#define FREEIMAGE_KEY "6d207e02198a847aa98d0a2a901485a5"

typedef enum { St_Idle, St_Check, St_Eshop, St_Tinfoil, St_WaitUpload, St_Catbox, St_Freeimage } Stage;
typedef enum { Save_Nothing, Save_Eshop, Save_Upload, Save_NoIcon } SaveTo;

typedef struct {
    Stage st;
    u64 id;
    char tid[17], name[100];
    char saved[300];     // St_Check: the saved upload link
    u8 *icon;            // the title's own icon (NULL = it has none)
    size_t icon_len;
    CURL *c;
    curl_mime *mime;
    char *resp;
    char err[160];
} Lane;

typedef struct {
    IconJob *j;
    json_object *eshop, *uploads, *checked;
    bool eshop_down;     // the eShop search is blocked here: don't wait on it
    int eshop_fails, eshop_oks;
    bool tinfoil_down;   // same for tinfoil
    int tinfoil_fails, tinfoil_oks;
    int uploading;       // uploads running (at most UPLOADS, hosts throttle)
    bool last_pass;      // the retry pass: a failure now is final
    u64 *retry;          // titles to try again at the end (a host didn't answer)
    int nretry;
} Prep;

#define UPLOADS 2

static void lane_request(CURLM *m, Lane *l, Stage st) {
    l->st = st;
    free(l->resp);
    l->resp = NULL;
    l->mime = NULL;
    CURL *c = curl_easy_init();
    l->c = c;
    char url[300];
    long timeout = 10;
    switch (st) {
    case St_Check:
        // a real download of the first bytes: catbox answers HEAD with 200
        // even for files that are gone
        snprintf(url, sizeof(url), "%s", l->saved);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-1023");
        break;
    case St_Eshop:
        snprintf(url, sizeof(url),
                 "https://searching.nintendo-europe.com/en/select?q=application_id_s%%3A%s&wt=json&fq=type%%3AGAME"
                 "&fl=application_id_s%%2Cimage_url_sq_s&rows=5", l->tid);
        break;
    case St_Tinfoil:
        snprintf(url, sizeof(url), "https://tinfoil.media/ti/%016llX/512/512/", (unsigned long long)l->id);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-1023");  // some servers refuse HEAD
        break;
    default: {  // St_Catbox / St_Freeimage
        bool catbox = st == St_Catbox;
        snprintf(url, sizeof(url), "%s", catbox ? "https://catbox.moe/user/api.php" : "https://freeimage.host/api/1/upload");
        curl_mime *mime = curl_mime_init(c);
        curl_mimepart *part;
        if (catbox) {
            part = curl_mime_addpart(mime); curl_mime_name(part, "reqtype"); curl_mime_data(part, "fileupload", CURL_ZERO_TERMINATED);
            part = curl_mime_addpart(mime); curl_mime_name(part, "fileToUpload");
        } else {
            part = curl_mime_addpart(mime); curl_mime_name(part, "key");    curl_mime_data(part, FREEIMAGE_KEY, CURL_ZERO_TERMINATED);
            part = curl_mime_addpart(mime); curl_mime_name(part, "action"); curl_mime_data(part, "upload", CURL_ZERO_TERMINATED);
            part = curl_mime_addpart(mime); curl_mime_name(part, "format"); curl_mime_data(part, "json", CURL_ZERO_TERMINATED);
            part = curl_mime_addpart(mime); curl_mime_name(part, "source");
        }
        curl_mime_filename(part, "icon.jpg");
        curl_mime_type(part, "image/jpeg");
        curl_mime_data(part, (const char *)l->icon, l->icon_len);
        curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
        l->mime = mime;
        timeout = 40;
        break;
    }
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "SwitchRPC/1.0");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &l->resp);
    curl_easy_setopt(c, CURLOPT_PRIVATE, l);
    curl_multi_add_handle(m, c);
}

static void lane_abort(CURLM *m, Prep *p, Lane *l) {
    if (l->c && (l->st == St_Catbox || l->st == St_Freeimage))
        p->uploading--;
    if (l->c) {
        curl_multi_remove_handle(m, l->c);
        curl_easy_cleanup(l->c);
        l->c = NULL;
    }
    if (l->mime) { curl_mime_free(l->mime); l->mime = NULL; }
    free(l->resp); l->resp = NULL;
    free(l->icon); l->icon = NULL;
    l->st = St_Idle;
}

// the title is done (or failed): save what we learned, print one line
static void lane_finish(CURLM *m, Prep *p, Lane *l, SaveTo save, const char *url,
                        const char *color, const char *what) {
    IconJob *j = p->j;
    if (save == Save_Eshop) json_object_object_add(p->eshop, l->tid, json_object_new_string(url));
    else if (save == Save_Upload) json_object_object_add(p->uploads, l->tid, json_object_new_string(url));
    else if (save == Save_NoIcon) json_object_object_add(p->checked, l->tid, json_object_new_string("none"));
    if (save != Save_Nothing) {
        // saved as we go, so stopping halfway keeps what's done
        json_object_to_file_ext(ESHOP_CACHE, p->eshop, JSON_C_TO_STRING_PLAIN);
        json_object_to_file_ext(UPLOAD_CACHE, p->uploads, JSON_C_TO_STRING_PLAIN);
        json_object_to_file_ext(CHECKED_CACHE, p->checked, JSON_C_TO_STRING_PLAIN);
    }
    job_line(j, color, "%s: %s", l->name, what);
    j->done++;
    lane_abort(m, p, l);
}

// a host didn't answer for this title: try it again at the end, slower -
// guessing "unknown title" here is what labelled official games "(homebrew)"
static void lane_retry(CURLM *m, Prep *p, Lane *l, const char *why) {
    if (p->last_pass) {
        char what[220];
        snprintf(what, sizeof(what), "%s - tried again next time", why);
        p->j->failed++;
        lane_finish(m, p, l, Save_Nothing, NULL, "\x1b[31m", what);
        return;
    }
    u64 *more = realloc(p->retry, sizeof(u64) * (p->nretry + 1));
    if (more) {
        p->retry = more;
        p->retry[p->nretry++] = l->id;
    }
    lane_abort(m, p, l);
}

static void start_upload(CURLM *m, Prep *p, Lane *l, Stage st) {
    p->uploading++;
    lane_request(m, l, st);
}

static void lane_after_tinfoil(CURLM *m, Prep *p, Lane *l) {
    if (l->icon) {
        if (p->uploading < UPLOADS) start_upload(m, p, l, St_Catbox);
        else l->st = St_WaitUpload;  // started by the main loop when a slot frees up
    } else {
        lane_finish(m, p, l, Save_NoIcon, NULL, "\x1b[2m", "has no icon of its own");
    }
}

// one request finished: decide the next step for that title
static void lane_done(CURLM *m, Prep *p, Lane *l, CURLcode rc) {
    IconJob *j = p->j;
    long code = 0;
    char *type = NULL;
    curl_easy_getinfo(l->c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(l->c, CURLINFO_CONTENT_TYPE, &type);
    bool image = type && !strncmp(type, "image/", 6);
    curl_multi_remove_handle(m, l->c);
    curl_easy_cleanup(l->c);
    l->c = NULL;
    if (l->mime) { curl_mime_free(l->mime); l->mime = NULL; }

    switch (l->st) {
    case St_Check:
        if (rc == CURLE_OK && (code == 404 || code == 410)) {
            json_object_object_del(p->uploads, l->tid);
            job_line(j, "\x1b[33m", "%s: saved icon link stopped working - redoing it", l->name);
            lane_request(m, l, p->eshop_down ? St_Tinfoil : St_Eshop);
        } else {
            j->skipped++;
            j->done++;
            lane_abort(m, p, l);
        }
        return;
    case St_Eshop: {
        if (rc != CURLE_OK || code != 200) {
            // two failures before any answer: it's blocked here, skip it for the rest
            if (++p->eshop_fails >= 2 && p->eshop_oks == 0 && !p->eshop_down) {
                p->eshop_down = true;
                job_line(j, "\x1b[33m", "The eShop search doesn't answer here - using tinfoil and the titles' own icons.");
            }
            lane_request(m, l, St_Tinfoil);
            return;
        }
        p->eshop_oks++;
        char found[300] = "";
        json_object *root = json_tokener_parse(l->resp ? l->resp : ""), *r, *docs;
        if (root && json_object_object_get_ex(root, "response", &r) && json_object_object_get_ex(r, "docs", &docs)) {
            for (size_t i = 0; i < json_object_array_length(docs) && !found[0]; i++) {
                json_object *doc = json_object_array_get_idx(docs, i);
                const char *id = jstr(doc, "application_id_s"), *img = jstr(doc, "image_url_sq_s");
                if (id && img && !strcasecmp(id, l->tid)) snprintf(found, sizeof(found), "%s", img);
            }
        }
        json_object_put(root);
        if (found[0]) {
            j->found++;
            lane_finish(m, p, l, Save_Eshop, found, "\x1b[32m", "eShop icon");
        } else {
            lane_request(m, l, St_Tinfoil);
        }
        return;
    }
    case St_Tinfoil: {
        bool known = rc == CURLE_OK && (code == 200 || code == 206) && image;
        // tinfoil has every official title: "not there" means homebrew / forwarder
        bool unknown = rc == CURLE_OK && (code == 404 || code == 410 || ((code == 200 || code == 206) && !image));
        if (known || unknown) p->tinfoil_oks++;
        if (known) {
            char tf[80];
            snprintf(tf, sizeof(tf), "https://tinfoil.media/ti/%016llX/512/512/", (unsigned long long)l->id);
            j->found++;
            lane_finish(m, p, l, Save_Eshop, tf, "\x1b[32m", "tinfoil icon");
            return;
        }
        if (!unknown && !p->tinfoil_down && ++p->tinfoil_fails >= 2 && p->tinfoil_oks == 0) {
            // never answered: blocked here, so there's no telling official from homebrew
            p->tinfoil_down = true;
            job_line(j, "\x1b[33m", "tinfoil doesn't answer here - using the titles' own icons.");
        }
        if (unknown || p->tinfoil_down)
            lane_after_tinfoil(m, p, l);
        else
            lane_retry(m, p, l, "couldn't reach tinfoil");
        return;
    }
    case St_Catbox:
    case St_Freeimage: {
        bool catbox = l->st == St_Catbox;
        p->uploading--;
        char url[300] = "";
        if (rc != CURLE_OK) {
            snprintf(l->err + strlen(l->err), sizeof(l->err) - strlen(l->err), "%s%s: %s",
                     l->err[0] ? " / " : "", catbox ? "catbox" : "freeimage", curl_easy_strerror(rc));
        } else if (code == 200 && l->resp && catbox) {
            // plain text answer: the file URL
            l->resp[strcspn(l->resp, "\r\n")] = '\0';
            if (!strncmp(l->resp, "https://", 8)) snprintf(url, sizeof(url), "%s", l->resp);
        } else if (code == 200 && l->resp) {
            json_object *root = json_tokener_parse(l->resp), *img, *u;
            if (root && json_object_object_get_ex(root, "image", &img) && json_object_object_get_ex(img, "url", &u) &&
                !strncmp(json_object_get_string(u), "https://", 8))
                snprintf(url, sizeof(url), "%s", json_object_get_string(u));
            json_object_put(root);
        }
        if (url[0]) {
            j->uploaded++;
            lane_finish(m, p, l, Save_Upload, url, "\x1b[32m", catbox ? "uploaded (catbox)" : "uploaded (freeimage)");
            return;
        }
        if (rc == CURLE_OK)
            snprintf(l->err + strlen(l->err), sizeof(l->err) - strlen(l->err), "%s%s: HTTP %ld",
                     l->err[0] ? " / " : "", catbox ? "catbox" : "freeimage", code);
        if (catbox) {
            start_upload(m, p, l, St_Freeimage);
        } else {
            char why[200];
            snprintf(why, sizeof(why), "upload failed (%s)", l->err);
            l->err[0] = '\0';
            lane_retry(m, p, l, why);
        }
        return;
    }
    default:
        return;
    }
}

// starts a title in a free lane; false = nothing to do for it (skipped)
static bool lane_start(CURLM *m, Prep *p, Lane *l, u64 id, NsApplicationControlData *ctrl, bool from_scratch) {
    IconJob *j = p->j;
    memset(l, 0, sizeof(*l));
    l->id = id;
    snprintf(l->tid, sizeof(l->tid), "%016llx", (unsigned long long)id);
    json_object *tmp;
    bool check = false;
    if (!from_scratch) {
        if (json_object_object_get_ex(p->uploads, l->tid, &tmp)) {
            // uploaded before: only make sure the link still works
            snprintf(l->saved, sizeof(l->saved), "%s", json_object_get_string(tmp));
            check = true;
        } else if (json_object_object_get_ex(p->eshop, l->tid, &tmp) ||
                   json_object_object_get_ex(p->checked, l->tid, &tmp)) {
            j->skipped++;
            j->done++;
            return false;
        }
    }
    u64 actual = 0;
    snprintf(l->name, sizeof(l->name), "?");
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, id, ctrl, sizeof(*ctrl), &actual))) {
        NacpLanguageEntry *le = NULL;
        if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl->nacp, &le)) && le && le->name[0])
            snprintf(l->name, sizeof(l->name), "%.*s", (int)ui_text_fit(le->name, 30, false), le->name);
        if (actual > sizeof(ctrl->nacp)) {
            l->icon_len = actual - sizeof(ctrl->nacp);
            l->icon = malloc(l->icon_len);
            if (l->icon) memcpy(l->icon, ctrl->icon, l->icon_len);
        }
    }
    lane_request(m, l, check ? St_Check : p->eshop_down ? St_Tinfoil : St_Eshop);
    return true;
}

// Goes through every installed title.
//  - new titles only: titles done before are skipped (uploaded icons just get
//    their link checked, and are redone if the host deleted them); titles known
//    to have no icon of their own are skipped too.
//  - from scratch: every saved answer is forgotten and every title redone.
static void prepare_icons(IconJob *j, PadState *pad, bool from_scratch) {
    memset(j, 0, sizeof(*j));
    if (R_FAILED(nsInitialize())) {
        job_line(j, "\x1b[31m", "Can't open the title list (ns).");
        return;
    }
    // every installed title first (cheap), then the network part
    u64 *ids = NULL;
    s32 n = 0;
    static NsApplicationRecord recs[64];
    s32 offset = 0, count = 0;
    while (R_SUCCEEDED(nsListApplicationRecord(recs, 64, offset, &count)) && count > 0) {
        u64 *more = realloc(ids, sizeof(u64) * (n + count));
        if (!more) break;
        ids = more;
        for (s32 i = 0; i < count; i++) ids[n++] = recs[i].application_id;
        offset += count;
    }
    j->total = n;

    Prep p = { .j = j };
    if (from_scratch) {
        p.eshop = json_object_new_object();
        p.uploads = json_object_new_object();
        p.checked = json_object_new_object();
    } else {
        p.eshop = cache_load(ESHOP_CACHE);
        p.uploads = cache_load(UPLOAD_CACHE);
        p.checked = cache_load(CHECKED_CACHE);
    }
    NsApplicationControlData *ctrl = malloc(sizeof(*ctrl));
    CURLM *m = curl_multi_init();
    static Lane lanes[LANES];
    memset(lanes, 0, sizeof(lanes));
    job_line(j, "", from_scratch ? "Preparing every installed title again..." : "Checking installed titles...");

    s32 next = 0;
    bool stop = false;
    u64 last_draw = 0;
    while (ctrl && m) {
        // keep every lane busy (fewer in the retry pass, to go easy on the hosts)
        for (int k = 0; k < (p.last_pass ? 2 : LANES) && next < n && !stop; k++)
            while (lanes[k].st == St_Idle && next < n)
                lane_start(m, &p, &lanes[k], ids[next++], ctrl, from_scratch || p.last_pass);
        for (int k = 0; k < LANES && p.uploading < UPLOADS; k++)
            if (lanes[k].st == St_WaitUpload) start_upload(m, &p, &lanes[k], St_Catbox);
        int busy = 0;
        for (int k = 0; k < LANES; k++) busy += lanes[k].st != St_Idle;
        if (!busy && next >= n && p.nretry && !p.last_pass) {
            // a second, slower go at what a host didn't answer for
            job_line(j, "", "Trying %d title(s) again that a server didn't answer for...", p.nretry);
            draw_icons(j, false);
            svcSleepThread(2000000000ULL);
            free(ids);
            ids = p.retry;
            n = p.nretry;
            next = 0;
            p.retry = NULL;
            p.nretry = 0;
            p.last_pass = true;
            continue;
        }
        if (!busy) break;

        int running = 0;
        curl_multi_perform(m, &running);
        CURLMsg *msg;
        int left;
        while ((msg = curl_multi_info_read(m, &left))) {
            if (msg->msg != CURLMSG_DONE) continue;
            Lane *l = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&l);
            if (l) lane_done(m, &p, l, msg->data.result);
        }
        curl_multi_poll(m, NULL, 0, 30, NULL);

        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_B) {
            for (int k = 0; k < LANES; k++) lane_abort(m, &p, &lanes[k]);
            job_line(j, "\x1b[33m", "Stopped.");
            stop = true;
            break;
        }
        u64 now = armGetSystemTick();
        if (now - last_draw > armNsToTicks(100000000ULL)) {  // ~10 times a second
            draw_icons(j, false);
            last_draw = now;
        }
    }
    curl_multi_cleanup(m);
    free(ctrl);
    free(ids);
    free(p.retry);
    json_object_put(p.eshop);
    json_object_put(p.uploads);
    json_object_put(p.checked);
    nsExit();

    int fresh = j->found + j->uploaded + j->failed;
    if (fresh == 0 && !stop)
        job_line(j, "\x1b[32m", "All %d titles already have their icons - nothing new to do.", j->total);
    else
        job_line(j, "\x1b[1m", "Done: %d found, %d uploaded, %d already fine, %d to retry later.",
                 j->found, j->uploaded, j->skipped, j->failed);
    if (j->failed == 0 && fresh > 0)
        job_line(j, "", "The module uses these right away (no reboot needed).");
}

// the question before preparing: only what's new, or everything again
static void draw_icons_ask(void) {
    char a[200], b[200];
    snprintf(a, sizeof(a), "\x1b[1m%s\x1b[0m", tr(T_IconsAskNew));
    snprintf(b, sizeof(b), "\x1b[1m%s\x1b[0m", tr(T_IconsAskScratch));
    const char *lines[] = { a, tr(T_IconsAskNewDesc), "", b, tr(T_IconsAskScratchDesc) };
    ui_frame_begin();
    ui_lines(tr(T_Icons), lines, 5, tr(T_FooterIconsAsk));
    ui_frame_end();
}

// ---- update ---------------------------------------------------------------------

static void draw_update(const IconJob *j, bool finished) {
    const char *lines[24];
    for (int i = 0; i < j->n; i++) lines[i] = j->lines[i];
    ui_frame_begin();
    ui_lines(tr(T_UpdateTitle), lines, j->n, finished ? tr(T_FooterBack) : tr(T_FooterWorking));
    ui_frame_end();
}

// progress lines from update_install, drawn as they come
static void update_line(void *ctx, const char *color, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    job_line(ctx, color, "%s", buf);
    draw_update(ctx, false);
}

static void draw_login(const Login *lg, const char *note) {
    char url[48];
    snprintf(url, sizeof(url), "http://%s:%d", lg->ip, LOGIN_PORT);
    static uint8_t grid[64 * 64];
    int size = make_qr(url, grid, 64);
    ui_frame_begin();
    ui_login(url, grid, size, note);
    ui_frame_end();
}

// up/down walk the list; left/right jump between the two columns
static int move_sel(int sel, u64 down) {
    if (down & HidNpadButton_AnyDown) return (sel + 1) % Item_Count;
    if (down & HidNpadButton_AnyUp)   return (sel + Item_Count - 1) % Item_Count;
    bool l = down & HidNpadButton_AnyLeft, r = down & HidNpadButton_AnyRight;
    if (!l && !r) return sel;
    if (sel >= Item_FirstToggle && sel <= Item_LastToggle) {
        int k = sel - Item_FirstToggle, n = Item_LastToggle - Item_FirstToggle + 1;
        k = r ? k + TOGGLE_ROWS : k - TOGGLE_ROWS;
        if (k >= 0 && k < n) return Item_FirstToggle + k;
    } else if (sel >= Item_Login) {
        // two columns, filled row by row: stay on the same row
        int k = sel - Item_Login;
        if (r && k % 2 == 0 && Item_Login + k + 1 < Item_Count) return sel + 1;
        if (l && k % 2 == 1) return sel - 1;
    }
    return sel;
}

int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc > 0) update_set_nro_path(argv[0]);
    update_check_async();  // tells the menu if there's a newer release
    bool have_nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    bool have_pm   = R_SUCCEEDED(pmdmntInitialize());
    timeInitialize();

    bool set[Set_Count];
    int lang_setting;
    settings_load(set, &lang_setting);
    Lang lang = lang_setting < 0 ? console_lang() : lang_setting == 1 ? Lang_Ja : Lang_En;
    if (!ui_init(lang)) {
        // no graphics (shouldn't happen): leave quietly instead of crashing
        curl_global_cleanup();
        socketExit();
        return 0;
    }

    char msg[200] = "";
    Tone msg_tone = Tone_Normal;
    u64 msg_frame = 0;
    // the previous (PC-based) version of this project used another module id;
    // remove it completely so it neither boots nor shows up in ovl-sysmodules
    if (access(OLD_MODULE, F_OK) == 0) {
        remove_tree(OLD_MODULE);
        snprintf(msg, sizeof(msg), access(OLD_MODULE, F_OK) != 0
                 ? "Removed the old \"Switch Discord RPC\" module (gone after a reboot)."
                 : "Couldn't remove " OLD_MODULE " - delete it by hand.");
        msg_tone = Tone_Warn;
    }

    // this app used to be switch-rpc.nro: drop the old copy so the Homebrew
    // Menu doesn't list it twice
    if (!(argc > 0 && argv[0] && strstr(argv[0], "switch-rpc.nro")) && access(OLD_NRO, F_OK) == 0)
        remove(OLD_NRO);
    // store-page cache from 1.1.1 and older (replaced by -v2)
    remove("sdmc:/config/switchrpc-eshop-pages.json");

    Diag d = {0};
    diag_versions(&d);
    diag_refresh(&d, have_pm);
    Screen screen = Screen_Main;
    Login lg = { .fd = -1 };
    static Test test;
    static IconJob icon_job, update_job;
    int sel = Item_Master;
    u64 frame = 0;

    // every frame: input, then draw the current screen (the scene is animated)
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (screen == Screen_Main) {
            if (down & HidNpadButton_Plus)
                break;
            sel = move_sel(sel, down);

            bool toggle_master = (down & HidNpadButton_Minus) || (sel == Item_Master && (down & HidNpadButton_A));
            if (toggle_master) {
                set[Set_Enabled] = !set[Set_Enabled];
                save_settings(set, ui_lang());
                snprintf(msg, sizeof(msg), "%s", tr(set[Set_Enabled] ? T_MsgOn : T_MsgOff));
                msg_tone = set[Set_Enabled] ? Tone_Good : Tone_Warn;
            } else if (down & HidNpadButton_A) {
                if (sel >= Item_FirstToggle && sel <= Item_LastToggle) {
                    set[sel] = !set[sel];
                    save_settings(set, ui_lang());
                    snprintf(msg, sizeof(msg), "%s", tr(T_MsgSaved));
                    msg_tone = Tone_Normal;
                } else if (sel == Item_Lang) {
                    ui_set_lang(ui_lang() == Lang_En ? Lang_Ja : Lang_En);
                    save_settings(set, ui_lang());
                    msg[0] = '\0';
                } else if (sel == Item_Login) {
                    if (d.logged_in) {
                        // the module notices the missing file and clears your status
                        remove(TOKEN_PATH);
                        remove(USER_PATH);
                        snprintf(msg, sizeof(msg), "%s", tr(T_MsgLoggedOut));
                        msg_tone = Tone_Normal;
                    } else if (!have_nifm || !login_start(&lg)) {
                        snprintf(msg, sizeof(msg), "%s", tr(T_MsgNoWifi));
                        msg_tone = Tone_Bad;
                    } else {
                        screen = Screen_Login;
                        msg[0] = '\0';
                    }
                } else if (sel == Item_Test) {
                    screen = Screen_Test;
                    run_test(&test);
                } else if (sel == Item_Icons) {
                    screen = Screen_IconsAsk;
                } else if (sel == Item_Diag) {
                    screen = Screen_Diag;
                } else if (sel == Item_Update) {
                    screen = Screen_Update;
                    memset(&update_job, 0, sizeof(update_job));
                    update_install(update_line, &update_job);
                }
                frame = 0;  // refresh the state right away
            }
        } else if (screen == Screen_Diag) {
            if (down & (HidNpadButton_B | HidNpadButton_Plus)) { screen = Screen_Main; frame = 0; }
            if (down & HidNpadButton_Y) { start_module(&d); frame = 0; }
        } else if (screen == Screen_IconsAsk) {
            if (down & (HidNpadButton_A | HidNpadButton_Y)) {
                screen = Screen_Icons;
                prepare_icons(&icon_job, &pad, (down & HidNpadButton_Y) != 0);
            } else if (down & (HidNpadButton_B | HidNpadButton_Plus)) {
                screen = Screen_Main;
                frame = 0;
            }
        } else if (screen == Screen_Icons || screen == Screen_Update) {
            if (down & (HidNpadButton_B | HidNpadButton_Plus)) { screen = Screen_Main; frame = 0; }
        } else if (screen == Screen_Test) {
            if (down & (HidNpadButton_B | HidNpadButton_Plus)) { end_test(&test); screen = Screen_Main; frame = 0; }
        } else {  // login
            if (down & (HidNpadButton_B | HidNpadButton_Plus)) {
                login_stop(&lg);
                screen = Screen_Main;
                frame = 0;
            } else {
                login_poll(&lg);
                if (lg.got_code) {
                    login_stop(&lg);
                    draw_login(&lg, tr(T_MsgFinishing));
                    char user[64];
                    if (login_finish(&lg, user, sizeof(user))) {
                        snprintf(msg, sizeof(msg), tr(T_MsgLoginOk), user);
                        msg_tone = Tone_Good;
                    } else {
                        snprintf(msg, sizeof(msg), "%s", tr(T_MsgLoginFail));
                        msg_tone = Tone_Bad;
                    }
                    screen = Screen_Main;
                    frame = 0;
                }
            }
        }

        // a message stays ~3 seconds, then the help line comes back
        // (`frame` gets reset to force a refresh, so count separately)
        static u64 tick;
        static char last_msg[200];
        tick++;
        if (strcmp(last_msg, msg)) { snprintf(last_msg, sizeof(last_msg), "%s", msg); msg_frame = tick; }
        bool show_msg = msg[0] && tick - msg_frame < 180;

        // re-check the module / login state every 2 seconds
        if (frame % 120 == 0)
            diag_refresh(&d, have_pm);

        switch (screen) {
        case Screen_Main: {
            MenuModel m = {0};
            m.logged_in = d.logged_in;
            m.user = d.user;
            m.running = d.running;
            m.status_state = d.status_state;
            m.status_text = d.status_state[0] ? d.status_raw : d.status;  // no status file yet: log guess
            memcpy(m.set, set, sizeof(set));
            m.sel = sel;
            m.msg = show_msg ? msg : NULL;
            m.msg_tone = msg_tone;
            const UpdateInfo *upd = update_info();
            m.update_tag = upd->done && upd->available ? upd->latest : NULL;
            if (!ui_frame_begin()) goto done;
            ui_menu(&m);
            ui_frame_end();
            break;
        }
        case Screen_Diag: {
            const char *lines[DIAG_LINES];
            int n = diag_lines(&d, lines);
            if (!ui_frame_begin()) goto done;
            ui_lines(tr(T_DiagTitle), lines, n, tr(T_FooterDiag));
            ui_frame_end();
            break;
        }
        case Screen_Test:  draw_test(&test, true); break;
        case Screen_IconsAsk: draw_icons_ask(); break;
        case Screen_Icons: draw_icons(&icon_job, true); break;
        case Screen_Update: draw_update(&update_job, true); break;
        case Screen_Login: draw_login(&lg, NULL); break;
        }
        frame++;  // vsync paces the loop at 60 fps
    }

done:
    login_stop(&lg);
    end_test(&test);
    ui_exit();
    update_exit();
    if (have_pm)   pmdmntExit();
    if (have_nifm) nifmExit();
    curl_global_cleanup();
    socketExit();
    return 0;
}
