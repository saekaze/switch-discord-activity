// Self-update from the latest GitHub release.
//
// A release carries the plain files (exefs.nsp, the .nro, the .ovl) next to
// the zip, plus SHA256SUMS. Each file is downloaded to "<target>.new",
// checked, and only when all of them are good are they swapped in. The
// background module is then restarted so the new one runs right away.

#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "config.h"

#define API_LATEST "https://api.github.com/repos/" GITHUB_REPO "/releases/latest"
#define UA "switch-discord-activity/" SWITCHRPC_VERSION

static UpdateInfo g_info;
static Thread g_thread;
static bool g_thread_started;

static char g_nro_path[256] = "sdmc:/switch/switch-discord-activity.nro";

const UpdateInfo *update_info(void) { return &g_info; }

void update_set_nro_path(const char *path) {
    size_t n = path ? strlen(path) : 0;
    // hbloader passes "sdmc:/..."; anything else (e.g. nxlink) keeps the default
    if (n > 4 && n < sizeof(g_nro_path) && !strncmp(path, "sdmc:/", 6) && !strcasecmp(path + n - 4, ".nro"))
        snprintf(g_nro_path, sizeof(g_nro_path), "%s", path);
}

// --- http ------------------------------------------------------------------------

typedef struct { char *buf; size_t len; } Mem;

static size_t to_mem(char *p, size_t s, size_t n, void *u) {
    Mem *m = u;
    size_t add = s * n;
    char *nb = realloc(m->buf, m->len + add + 1);
    if (!nb) return 0;
    memcpy(nb + m->len, p, add);
    m->len += add;
    nb[m->len] = '\0';
    m->buf = nb;
    return add;
}

static CURL *new_curl(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // asset downloads redirect
    // same as everywhere else in this project: the console can't verify these
    // certificates. The files are checked against SHA256SUMS instead.
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    return c;
}

// GET into memory; returns the body (malloc'd) or NULL with err filled in.
static char *http_get(const char *url, const char *accept, char *err, size_t err_size) {
    CURL *c = new_curl(url);
    if (!c) { snprintf(err, err_size, "curl init"); return NULL; }
    Mem m = { 0 };
    struct curl_slist *h = NULL;
    if (accept) {
        char a[128];
        snprintf(a, sizeof(a), "Accept: %s", accept);
        h = curl_slist_append(h, a);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    }
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code != 200) {
        if (rc != CURLE_OK) snprintf(err, err_size, "%s", curl_easy_strerror(rc));
        else snprintf(err, err_size, "HTTP %ld", code);
        free(m.buf);
        return NULL;
    }
    return m.buf;
}

static size_t to_file(char *p, size_t s, size_t n, void *u) {
    return fwrite(p, 1, s * n, (FILE *)u);
}

static bool http_download(const char *url, const char *path, char *err, size_t err_size) {
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, err_size, "can't write %.100s", path); return false; }
    CURL *c = new_curl(url);
    if (!c) { fclose(f); snprintf(err, err_size, "curl init"); return false; }
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    bool ok = fclose(f) == 0 && rc == CURLE_OK && code == 200;
    if (!ok) {
        if (rc != CURLE_OK) snprintf(err, err_size, "%s", curl_easy_strerror(rc));
        else snprintf(err, err_size, "HTTP %ld", code);
        remove(path);
    }
    return ok;
}

// --- versions ----------------------------------------------------------------------

static void parse_ver(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v' || *s == 'V') s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

static bool newer(const char *tag, const char *current) {
    int a[3], b[3];
    parse_ver(tag, a);
    parse_ver(current, b);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// --- check ---------------------------------------------------------------------------

static json_object *fetch_latest(char *err, size_t err_size) {
    char *body = http_get(API_LATEST, "application/vnd.github+json", err, err_size);
    if (!body) return NULL;
    json_object *root = json_tokener_parse(body);
    free(body);
    if (!root) snprintf(err, err_size, "unreadable answer from GitHub");
    return root;
}

void update_check_now(void) {
    UpdateInfo r = { 0 };
    json_object *root = fetch_latest(r.err, sizeof(r.err));
    json_object *tag;
    if (root && json_object_object_get_ex(root, "tag_name", &tag)) {
        snprintf(r.latest, sizeof(r.latest), "%s", json_object_get_string(tag));
        r.available = newer(r.latest, SWITCHRPC_VERSION);
    } else if (root) {
        snprintf(r.err, sizeof(r.err), "no release found");
    }
    json_object_put(root);
    // the menu reads g_info from the main thread: fill it, then flag it done
    g_info.done = false;
    __sync_synchronize();
    memcpy(g_info.latest, r.latest, sizeof(r.latest));
    memcpy(g_info.err, r.err, sizeof(r.err));
    g_info.available = r.available;
    __sync_synchronize();
    g_info.done = true;
}

static void check_thread(void *arg) {
    (void)arg;
    update_check_now();
}

void update_check_async(void) {
    if (g_thread_started) return;
    if (R_SUCCEEDED(threadCreate(&g_thread, check_thread, NULL, NULL, 0x20000, 0x2C, -2)) &&
        R_SUCCEEDED(threadStart(&g_thread)))
        g_thread_started = true;
    else
        update_check_now();
}

void update_exit(void) {
    if (!g_thread_started) return;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_thread_started = false;
}

// --- install -------------------------------------------------------------------------

typedef struct {
    const char *asset;   // file name in the release
    const char *target;  // where it goes on the SD card
    bool present;
    char url[512];
} Part;

static void sha256_file(const char *path, char out_hex[65]) {
    out_hex[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return;
    Sha256Context ctx;
    sha256ContextCreate(&ctx);
    static u8 buf[0x10000];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256ContextUpdate(&ctx, buf, n);
    fclose(f);
    u8 hash[32];
    sha256ContextGetHash(&ctx, hash);
    for (int i = 0; i < 32; i++)
        sprintf(out_hex + i * 2, "%02x", hash[i]);
}

// "hash  name" lines -> the hash for `name`
static bool sum_for(const char *sums, const char *name, char out[65]) {
    for (const char *p = sums; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        if (len < sizeof(line)) {
            memcpy(line, p, len);
            line[len] = '\0';
            char h[65], n[160];
            if (sscanf(line, "%64s %*[ *]%159s", h, n) == 2 && !strcmp(n, name)) {
                snprintf(out, 65, "%s", h);
                return true;
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return false;
}

static bool module_running(void) {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, SYSMODULE_TID));
}

static void restart_module(UpdateLine line, void *ctx) {
    if (!module_running()) {
        // turned off on purpose (or never started): leave it that way
        line(ctx, "", "The background module isn't running - it uses the new version once started.");
        return;
    }
    if (R_FAILED(pmshellInitialize())) {
        line(ctx, "\x1b[33m", "The new module runs after a reboot.");
        return;
    }
    // the module cleans up its Discord session from the sessions list on start
    pmshellTerminateProgram(SYSMODULE_TID);
    svcSleepThread(500000000ULL);
    NcmProgramLocation loc = { .program_id = SYSMODULE_TID, .storageID = NcmStorageId_None };
    u64 pid = 0;
    Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_SUCCEEDED(rc)) line(ctx, "\x1b[32m", "Background module restarted with the new version.");
    else line(ctx, "\x1b[33m", "The new module runs after a reboot (restart failed: %04u-%04u).",
              2000 + R_MODULE(rc), R_DESCRIPTION(rc));
}

bool update_install(UpdateLine line, void *ctx) {
    char err[160] = "";
    line(ctx, "", "Looking for the latest release of %s...", GITHUB_REPO);
    json_object *root = fetch_latest(err, sizeof(err));
    json_object *tag, *assets;
    if (!root || !json_object_object_get_ex(root, "tag_name", &tag) ||
        !json_object_object_get_ex(root, "assets", &assets)) {
        line(ctx, "\x1b[31m", "   Can't reach GitHub: %s", err[0] ? err : "no release");
        json_object_put(root);
        return false;
    }
    const char *latest = json_object_get_string(tag);
    line(ctx, "", "   Installed v%s, latest %s", SWITCHRPC_VERSION, latest);
    if (!newer(latest, SWITCHRPC_VERSION)) {
        line(ctx, "\x1b[32m", "   You already have the latest version.");
        json_object_put(root);
        return true;
    }

    char nsp_dir[96], nsp[128];
    snprintf(nsp_dir, sizeof(nsp_dir), "sdmc:/atmosphere/contents/%016llX", (unsigned long long)SYSMODULE_TID);
    snprintf(nsp, sizeof(nsp), "%s/exefs.nsp", nsp_dir);
    Part parts[] = {
        { "exefs.nsp", nsp, false, "" },
        { "switch-discord-activity.nro", g_nro_path, false, "" },
        { "switch-discord-activity.ovl", "sdmc:/switch/.overlays/switch-discord-activity.ovl", false, "" },
    };
    const int nparts = sizeof(parts) / sizeof(parts[0]);
    char sums_url[512] = "";
    for (size_t i = 0; i < json_object_array_length(assets); i++) {
        json_object *a = json_object_array_get_idx(assets, i), *n, *u;
        if (!json_object_object_get_ex(a, "name", &n) || !json_object_object_get_ex(a, "browser_download_url", &u))
            continue;
        const char *name = json_object_get_string(n), *url = json_object_get_string(u);
        if (!strcmp(name, "SHA256SUMS")) snprintf(sums_url, sizeof(sums_url), "%s", url);
        for (int k = 0; k < nparts; k++)
            if (!strcmp(name, parts[k].asset)) {
                parts[k].present = true;
                snprintf(parts[k].url, sizeof(parts[k].url), "%s", url);
            }
    }
    json_object_put(root);
    if (!sums_url[0] || !parts[0].present || !parts[1].present) {
        line(ctx, "\x1b[31m", "   This release has no files for automatic updates.");
        line(ctx, "", "   Download the zip from github.com/%s/releases instead.", GITHUB_REPO);
        return false;
    }

    char *sums = http_get(sums_url, NULL, err, sizeof(err));
    if (!sums) {
        line(ctx, "\x1b[31m", "   Can't download SHA256SUMS: %s", err);
        return false;
    }

    bool fresh_install = access(nsp, F_OK) != 0;

    // download + verify everything first, swap only when all are good
    mkdir("sdmc:/atmosphere", 0777); mkdir("sdmc:/atmosphere/contents", 0777); mkdir(nsp_dir, 0777);
    mkdir("sdmc:/switch", 0777); mkdir("sdmc:/switch/.overlays", 0777);
    bool ok = true;
    for (int k = 0; k < nparts && ok; k++) {
        if (!parts[k].present) continue;
        char tmp[300], want[65], got[65];
        snprintf(tmp, sizeof(tmp), "%s.new", parts[k].target);
        line(ctx, "", "Downloading %s...", parts[k].asset);
        if (!http_download(parts[k].url, tmp, err, sizeof(err))) {
            line(ctx, "\x1b[31m", "   Failed: %s", err);
            ok = false;
        } else if (!sum_for(sums, parts[k].asset, want)) {
            line(ctx, "\x1b[31m", "   No checksum for it in SHA256SUMS.");
            ok = false;
        } else {
            sha256_file(tmp, got);
            if (strcasecmp(want, got)) {
                line(ctx, "\x1b[31m", "   Checksum mismatch - download damaged, nothing was changed.");
                ok = false;
            } else {
                line(ctx, "\x1b[32m", "   OK, checksum matches.");
            }
        }
    }
    free(sums);

    for (int k = 0; k < nparts; k++) {
        if (!parts[k].present) continue;
        char tmp[300];
        snprintf(tmp, sizeof(tmp), "%s.new", parts[k].target);
        if (!ok) { remove(tmp); continue; }
        remove(parts[k].target);  // FAT can't rename over an existing file
        if (rename(tmp, parts[k].target) != 0) {
            line(ctx, "\x1b[31m", "   Couldn't replace %s.", parts[k].target);
            ok = false;
        }
    }
    if (!ok) return false;

    // a fresh install needs the boot flag; an existing one keeps whatever the
    // user chose (turning the module off in ovl-sysmodules removes the flag)
    if (fresh_install) {
        char flags[128], flag[160];
        snprintf(flags, sizeof(flags), "%s/flags", nsp_dir);
        snprintf(flag, sizeof(flag), "%s/boot2.flag", flags);
        mkdir(flags, 0777);
        FILE *f = fopen(flag, "ab");
        if (f) fclose(f);
    }

    line(ctx, "\x1b[32m", "Updated to %s.", latest);
    restart_module(line, ctx);
    line(ctx, "\x1b[33m", "Restart this app (press + and open it again) to use the new version.");
    g_info.available = false;
    return true;
}
