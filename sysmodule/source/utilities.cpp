#include <stdio.h>
#include <string.h>
#include <string>
#include <set>
#include <fstream>
#include <switch.h>
#include <json-c/json.h>
#include <curl/curl.h>
#include "utilities.hpp"
#include "logging.hpp"
#include "discord.hpp"

#define ICON_CACHE_PATH "sdmc:/config/switchrpc-eshop-iconcache.json"
// links to forwarder/homebrew icons we uploaded ourselves (not in the eShop)
#define UPLOAD_CACHE_PATH "sdmc:/config/switchrpc-uploaded-icons.json"
// eShop store pages ("" = the eShop doesn't list it)
// v2: 1.1.1 and older cached "no page" when the eShop search was merely
// blocked, so those answers aren't trusted
#define PAGE_CACHE_PATH "sdmc:/config/switchrpc-eshop-pages-v2.json"

static bool loadIconFromCache(const char* titleId, std::string& out_url, const char* path = ICON_CACHE_PATH) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    json_object* root = json_tokener_parse(contents.c_str());
    if (!root) return false;

    json_object* val;
    bool found = false;
    if (json_object_object_get_ex(root, titleId, &val)) {
        out_url = json_object_get_string(val);
        found = true;
    }

    json_object_put(root);
    return found;
}

static void saveIconToCache(const char* titleId, const char* url, const char* path = ICON_CACHE_PATH) {
    std::ifstream infile(path);
    std::string contents;
    if (infile.is_open()) {
        contents = std::string((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
        infile.close();
    }

    json_object* root = contents.empty() ? nullptr : json_tokener_parse(contents.c_str());
    if (!root) root = json_object_new_object();

    json_object_object_add(root, titleId, json_object_new_string(url));

    std::ofstream outfile(path, std::ios::trunc);
    if (outfile.is_open()) {
        outfile << json_object_to_json_string(root);
        outfile.close();
        writeToLog("[eShop] Saved icon to cache for %s", titleId);
    } else {
        writeToLog("[eShop] Failed to write icon cache");
    }

    json_object_put(root);
}

// 1 = found, 0 = the eShop doesn't list it, -1 = couldn't ask (blocked / offline)
static int eshopSearch(u64 titleId, std::string& out_url);
static int ecLinkStatus(const char* url);

bool fetchEshopIconUrl(u64 titleId, std::string& out_url) {
    char titleIdStr[17] = {0};
    snprintf(titleIdStr, sizeof(titleIdStr), "%016llx", (unsigned long long)titleId);

    if (loadIconFromCache(titleIdStr, out_url)) {
        writeToLog("[eShop] Cache hit for %s: %s", titleIdStr, out_url.c_str());
        return true;
    }
    return eshopSearch(titleId, out_url) > 0;
}

int getEshopPage(u64 titleId, std::string& out_url, bool cacheOnly) {
    char titleIdStr[17];
    snprintf(titleIdStr, sizeof(titleIdStr), "%016llx", (unsigned long long)titleId);
    if (loadIconFromCache(titleIdStr, out_url, PAGE_CACHE_PATH))
        return out_url.empty() ? 0 : 1;
    if (cacheOnly)
        return -1;
    // looked up once per boot when it couldn't be decided, so a blocked
    // network doesn't cost two timeouts on every status change
    static std::set<u64> undecided;
    if (undecided.count(titleId)) {
        out_url.clear();
        return 0;
    }
    std::string icon;
    int r = eshopSearch(titleId, icon);  // stores the page if there is one
    if (loadIconFromCache(titleIdStr, out_url, PAGE_CACHE_PATH) && !out_url.empty())
        return 1;
    if (r < 0) {
        // the search is blocked in some countries / networks: Nintendo's
        // short store link works too, and tells listed titles apart (30x vs 404)
        char ec[80];
        snprintf(ec, sizeof(ec), "https://ec.nintendo.com/apps/%016llX/GB", (unsigned long long)titleId);
        int e = ecLinkStatus(ec);
        if (e > 0) {
            saveIconToCache(titleIdStr, ec, PAGE_CACHE_PATH);
            out_url = ec;
            writeToLog("[eShop] Search unreachable, using the store link %s", ec);
            return 1;
        }
        if (e < 0) {
            undecided.insert(titleId);  // don't cache "no page" for what we couldn't check
            writeToLog("[eShop] Can't reach the eShop for %s, no button this time", titleIdStr);
            out_url.clear();
            return 0;
        }
    }
    saveIconToCache(titleIdStr, "", PAGE_CACHE_PATH);  // remember "no page"
    out_url.clear();
    return 0;
}

static int ecLinkStatus(const char* url) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return -1;
    if (code >= 300 && code < 400) return 1;
    if (code == 404 || code == 410) return 0;
    return -1;
}

static int eshopSearch(u64 titleId, std::string& out_url) {
    char titleIdStr[17] = {0};
    snprintf(titleIdStr, sizeof(titleIdStr), "%016llx", (unsigned long long)titleId);

    char url[512];
    snprintf(url, sizeof(url),
        "https://searching.nintendo-europe.com/en/select"
        "?q=application_id_s%%3A%s"
        "&wt=json&fq=type%%3AGAME"
        "&fl=application_id_s%%2Cimage_url_sq_s%%2Curl"
        "&rows=5"
        "&sort=dates_released_dts+asc",
        titleIdStr);

    CURL* curl = curl_easy_init();
    if (!curl) {
        writeToLog("[eShop] Failed to init cURL for icon lookup");
        return -1;
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* dns_cache = NULL;
    dns_cache = curl_slist_append(dns_cache, "searching.nintendo-europe.com:443:151.101.3.52");
    dns_cache = curl_slist_append(dns_cache, "searching.nintendo-europe.com:443:151.101.67.52");
    dns_cache = curl_slist_append(dns_cache, "searching.nintendo-europe.com:443:151.101.131.52");
    dns_cache = curl_slist_append(dns_cache, "searching.nintendo-europe.com:443:151.101.195.52");
    curl_easy_setopt(curl, CURLOPT_RESOLVE, dns_cache);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: SwitchRPC/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_slist_free_all(dns_cache);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        writeToLog("[eShop] cURL failed: %s", curl_easy_strerror(res));
        return -1;
    }
    if (code != 200) {
        writeToLog("[eShop] Search answered HTTP %ld", code);
        return -1;
    }

    json_object* root = json_tokener_parse(response.c_str());
    if (!root) {
        writeToLog("[eShop] Failed to parse JSON response");
        return -1;
    }

    bool found = false;
    json_object* resp_obj;
    if (json_object_object_get_ex(root, "response", &resp_obj)) {
        json_object* docs;
        if (json_object_object_get_ex(resp_obj, "docs", &docs)) {
            int n = json_object_array_length(docs);
            for (int i = 0; i < n; i++) {
                json_object* doc = json_object_array_get_idx(docs, i);
                json_object* app_id_obj;
                json_object* img_obj;
                if (json_object_object_get_ex(doc, "application_id_s", &app_id_obj) &&
                    json_object_object_get_ex(doc, "image_url_sq_s", &img_obj))
                {
                    const char* app_id = json_object_get_string(app_id_obj);
                    if (app_id && strcmp(app_id, titleIdStr) == 0) {
                        out_url = json_object_get_string(img_obj);
                        found = true;
                        json_object* page_obj;
                        if (json_object_object_get_ex(doc, "url", &page_obj)) {
                            std::string page = std::string("https://www.nintendo.com") + json_object_get_string(page_obj);
                            saveIconToCache(titleIdStr, page.c_str(), PAGE_CACHE_PATH);
                        }
                        break;
                    }
                }
            }
        }
    }

    json_object_put(root);

    if (found) {
        writeToLog("[eShop] Found icon for %s: %s", titleIdStr, out_url.c_str());
        saveIconToCache(titleIdStr, out_url.c_str());
    } else {
        writeToLog("[eShop] No icon found in EU eShop for %s", titleIdStr);
    }

    return found ? 1 : 0;
}

// HEAD-style check that a URL answers 200 with an image
static bool urlIsImage(const char* url) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    struct curl_slist* headers = curl_slist_append(NULL, "User-Agent: SwitchRPC/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    bool ok = false;
    if (curl_easy_perform(curl) == CURLE_OK) {
        long code = 0;
        char* type = NULL;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
        ok = code == 200 && type && strncmp(type, "image/", 6) == 0;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

// --- forwarders: upload the title's own icon so Discord can show it ---------
//
// Discord only takes an image *URL*. A forwarder's icon only exists on the
// console (in its control data), so it gets uploaded once to freeimage.host
// (public API key from freeimage.host/page/api) and the link is cached on the
// SD card - every later launch of that title reuses it.

#define FREEIMAGE_KEY "6d207e02198a847aa98d0a2a901485a5"

struct IconReader {
    const u8* data;
    size_t size;
    size_t pos;
};

static size_t iconRead(char* buffer, size_t size, size_t nitems, void* arg) {
    IconReader* r = (IconReader*)arg;
    size_t n = size * nitems;
    if (n > r->size - r->pos) n = r->size - r->pos;
    memcpy(buffer, r->data + r->pos, n);
    r->pos += n;
    return n;
}

static int iconSeek(void* arg, curl_off_t offset, int origin) {
    IconReader* r = (IconReader*)arg;
    if (origin != SEEK_SET || offset < 0 || (size_t)offset > r->size) return CURL_SEEKFUNC_FAIL;
    r->pos = (size_t)offset;
    return CURL_SEEKFUNC_OK;
}

// one upload attempt to freeimage.host (catbox=false) or catbox.moe (catbox=true)
static bool uploadTo(bool catbox, IconReader& reader, std::string& out_url) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    reader.pos = 0;
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part;
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
    // stream straight from the control data instead of copying 100+ KB
    curl_mime_data_cb(part, (curl_off_t)reader.size, iconRead, iconSeek, NULL, &reader);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, catbox ? "https://catbox.moe/user/api.php" : "https://freeimage.host/api/1/upload");
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    struct curl_slist* headers = curl_slist_append(NULL, "User-Agent: SwitchRPC/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const char* host = catbox ? "catbox" : "freeimage";
    writeToLog("[Icon] Uploading to %s (%zu bytes)...", host, reader.size);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code != 200) {
        writeToLog("[Icon] %s upload failed: %s (HTTP %ld)", host, res != CURLE_OK ? curl_easy_strerror(res) : "server error", code);
        return false;
    }

    bool ok = false;
    if (catbox) {
        // plain text answer: the file URL
        out_url = response.substr(0, response.find_first_of("\r\n"));
        ok = out_url.rfind("https://", 0) == 0;
    } else {
        json_object* root = json_tokener_parse(response.c_str());
        json_object *image, *url;
        if (root && json_object_object_get_ex(root, "image", &image) && json_object_object_get_ex(image, "url", &url)) {
            out_url = json_object_get_string(url);
            ok = out_url.rfind("https://", 0) == 0;
        }
        json_object_put(root);
    }
    if (!ok)
        writeToLog("[Icon] %s answer had no image link", host);
    return ok;
}

static bool uploadOwnIcon(u64 titleId, std::string& out_url) {
    char titleIdStr[17];
    snprintf(titleIdStr, sizeof(titleIdStr), "%016llx", (unsigned long long)titleId);
    if (loadIconFromCache(titleIdStr, out_url, UPLOAD_CACHE_PATH)) {
        writeToLog("[Icon] Reusing uploaded icon for %s: %s", titleIdStr, out_url.c_str());
        return true;
    }

    NsApplicationControlData* ctrl = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (!ctrl) {
        writeToLog("[Icon] Out of memory for the icon of %s", titleIdStr);
        return false;
    }
    u64 actual = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId, ctrl, sizeof(*ctrl), &actual)) ||
        actual <= sizeof(ctrl->nacp)) {
        writeToLog("[Icon] %s has no icon of its own", titleIdStr);
        free(ctrl);
        return false;
    }
    IconReader reader = { ctrl->icon, (size_t)(actual - sizeof(ctrl->nacp)), 0 };
    bool ok = uploadTo(false, reader, out_url) || uploadTo(true, reader, out_url);
    free(ctrl);

    if (ok) {
        writeToLog("[Icon] Uploaded: %s", out_url.c_str());
        saveIconToCache(titleIdStr, out_url.c_str(), UPLOAD_CACHE_PATH);
    }
    return ok;
}

IconKind resolveIconUrl(u64 titleId, std::string& out_url, bool cacheOnly) {
    // remember the last answer: the session is re-posted every 15 minutes
    static u64 lastTid = 0;
    static std::string lastUrl;
    static IconKind lastKind = Icon_Miss;
    if (titleId == lastTid && lastKind != Icon_Miss) {
        out_url = lastUrl;
        return lastKind;
    }

    char titleIdStr[17];
    snprintf(titleIdStr, sizeof(titleIdStr), "%016llx", (unsigned long long)titleId);

    IconKind kind = Icon_Miss;
    out_url.clear();
    if (titleId == HOMEBREW_TID) {
        out_url = NRO_ICON_URL;
        kind = Icon_Homebrew;
    } else if (loadIconFromCache(titleIdStr, out_url)) {
        kind = Icon_Official;
    } else if (loadIconFromCache(titleIdStr, out_url, UPLOAD_CACHE_PATH)) {
        kind = Icon_Homebrew;
    } else if (cacheOnly) {
        return Icon_Miss;  // caller shows the status now and comes back for the icon
    } else if (fetchEshopIconUrl(titleId, out_url)) {
        kind = Icon_Official;
    } else {
        char tinfoil[64];
        snprintf(tinfoil, sizeof(tinfoil), "https://tinfoil.media/ti/%016llX/512/512/", (unsigned long long)titleId);
        if (urlIsImage(tinfoil)) {
            out_url = tinfoil;
            kind = Icon_Official;
            saveIconToCache(titleIdStr, tinfoil);
            writeToLog("[Icon] Using tinfoil icon: %s", tinfoil);
        } else if (uploadOwnIcon(titleId, out_url)) {
            // forwarder / installed homebrew: its own icon
            kind = Icon_Homebrew;
        } else {
            // nothing usable at all -> generic homebrew icon
            out_url = NRO_ICON_URL;
            kind = Icon_Homebrew;
            writeToLog("[Icon] Unknown title %016llX, using the homebrew icon", (unsigned long long)titleId);
        }
    }

    lastTid = titleId;
    lastUrl = out_url;
    lastKind = kind;
    return kind;
}

bool g_pdmReady = false;

Screen getForegroundScreen(u64 appTid) {
    if (!g_pdmReady) return Screen_Game;

    s32 total = 0, first = 0, last = 0;
    if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &first, &last)) || total <= 0)
        return Screen_Game;

    // the most recent events are enough to know who has focus
    static PdmPlayEvent events[64];  // 3.5 KB, keep it off the stack
    s32 from = last - 63;
    if (from < first) from = first;
    s32 count = 0;
    if (R_FAILED(pdmqryQueryPlayEvent(from, events, 64, &count)) || count <= 0)
        return Screen_Game;

    // replay focus changes: a stack of applets that currently have focus
    struct Focus { u8 applet; u64 program; };
    Focus stack[16];
    int sp = 0;
    bool appSeen = false;
    for (s32 i = 0; i < count; i++) {
        const PdmPlayEvent& e = events[i];
        if (e.play_event_type != PdmPlayEventType_Applet)
            continue;
        u8 applet = e.event_data.applet.applet_id;
        u8 type = e.event_data.applet.event_type;
        u64 program = ((u64)e.event_data.applet.program_id[0] << 32) | e.event_data.applet.program_id[1];
        if (applet == AppletId_OverlayApplet)  // volume / quick settings overlay
            continue;
        if (applet == AppletId_application && program == appTid)
            appSeen = true;

        // drop older entries of the same applet, then push if it gained focus
        int k = 0;
        for (int j = 0; j < sp; j++)
            if (!(stack[j].applet == applet && (applet != AppletId_application || stack[j].program == program)))
                stack[k++] = stack[j];
        sp = k;
        if (type == PdmAppletEventType_InFocus) {
            if (sp == 16) { memmove(stack, stack + 1, sizeof(Focus) * 15); sp = 15; }
            stack[sp++] = { applet, program };
        }
    }

    // the game never showed up in the recent log: don't guess "HOME"
    if (appTid != 0 && !appSeen)
        return Screen_Game;

    for (int j = sp - 1; j >= 0; j--) {
        switch (stack[j].applet) {
        case AppletId_application:
            // only the running game counts (old entries from before a reboot don't)
            if (stack[j].program == appTid) return Screen_Game;
            continue;
        case AppletId_SystemAppletMenu:     return Screen_Home;
        case AppletId_LibraryAppletShop:    return Screen_Shop;
        case AppletId_LibraryAppletMyPage:  return Screen_MyPage;
        case AppletId_LibraryAppletWeb:
        case AppletId_LibraryAppletOfflineWeb: return Screen_Web;
        case AppletId_LibraryAppletMiiEdit: return Screen_MiiEdit;
        case AppletId_LibraryAppletPhotoViewer: return Screen_Album;
        default:
            // keyboard, controller setup, user select, errors, amiibo... are
            // dialogs. With a game open they're the game's own (it may even
            // have reported "out of focus" for them), so that's still playing.
            return appTid ? Screen_Game : Screen_Home;
        }
    }
    return Screen_Home;
}

const char* screenName(Screen s) {
    switch (s) {
    case Screen_Shop:    return "Nintendo eShop";
    case Screen_MyPage:  return "User Page";
    case Screen_Web:     return "Web Browser";
    case Screen_MiiEdit: return "Mii Maker";
    case Screen_Album:   return "Album";
    default:             return "HOME Menu";
    }
}

const char* screenPausedLabel(Screen s) {
    switch (s) {
    case Screen_Shop:    return "browsing the Nintendo eShop";
    case Screen_MyPage:  return "on the user page";
    case Screen_Web:     return "in the web browser";
    case Screen_MiiEdit: return "in the Mii Maker";
    case Screen_Album:   return "viewing screenshots";
    case Screen_Sleep:   return "sleep mode";
    default:             return "HOME Menu";
    }
}

// --- settings (written by the Switch Discord Activity app) -------------------------------

#define SETTINGS_PATH "sdmc:/config/switchrpc.ini"

Settings loadSettings() {
    Settings st;
    FILE* f = fopen(SETTINGS_PATH, "r");
    if (!f) return st;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val;
        if (sscanf(line, " %63[a-z_] = %d", key, &val) != 2) continue;
        bool on = val != 0;
        if (!strcmp(key, "enabled"))             st.enabled = on;
        else if (!strcmp(key, "paused"))         st.paused = on;
        else if (!strcmp(key, "system_screens")) st.systemScreens = on;
        else if (!strcmp(key, "show_timer"))     st.timer = on;
        else if (!strcmp(key, "show_icon"))      st.icon = on;
        else if (!strcmp(key, "show_game_name")) st.gameName = on;
        else if (!strcmp(key, "paused_time"))    st.pausedTime = on;
        else if (!strcmp(key, "dock_mode"))      st.dockMode = on;
        else if (!strcmp(key, "total_playtime")) st.totalPlaytime = on;
        else if (!strcmp(key, "eshop_button"))   st.eshopButton = on;
        else if (!strcmp(key, "homebrew_tag"))   st.homebrewTag = on;
        else if (!strcmp(key, "battery_saver"))  st.batterySaver = on;
    }
    fclose(f);
    return st;
}

bool isDocked() {
    ApmPerformanceMode mode;
    return R_SUCCEEDED(apmGetPerformanceMode(&mode)) && mode == ApmPerformanceMode_Boost;
}

u64 totalPlaytimeMinutes(u64 titleId) {
    if (!g_pdmReady || titleId == 0 || titleId == HOMEBREW_TID) return 0;
    PdmPlayStatistics stats;
    if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationId(titleId, false, &stats))) return 0;
    return stats.playtime / 1000000000ULL / 60;
}

bool g_ldrReady = false;

AlbumState getAlbumState() {
    u64 pid = 0;
    if (R_FAILED(pmdmntGetProcessId(&pid, HOMEBREW_TID)))
        return Album_None;
    if (!g_ldrReady)
        return Album_Homebrew;  // can't tell - Atmosphere's default is hbmenu

    // same answer for the same process, no need to ask the loader every poll
    static u64 lastPid = 0;
    static AlbumState lastState = Album_None;
    if (pid == lastPid)
        return lastState;

    // hbloader is a single module (it loads the .nro itself); Nintendo's
    // album is rtld + main + sdk (+ subsdk)
    LoaderModuleInfo mods[8];
    s32 n = 0;
    AlbumState st = Album_Homebrew;
    if (R_SUCCEEDED(ldrDmntGetProcessModuleInfo(pid, mods, 8, &n)))
        st = n >= 2 ? Album_Real : Album_Homebrew;
    writeToLog("[SwitchRPC] Album process has %d module(s): %s", (int)n, st == Album_Real ? "real album" : "hbmenu / .nro");
    lastPid = pid;
    lastState = st;
    return st;
}

bool isHomebrewRunning() {
    return getAlbumState() == Album_Homebrew;
}

#define STATUS_PATH "sdmc:/config/switchrpc_status"
static std::string g_statusState, g_statusText, g_statusIcon;
static u64 g_statusWritten = 0;

static void flushStatus() {
    FILE* f = fopen(STATUS_PATH, "w");
    if (!f) return;
    fprintf(f, "%llu\n%s\n%s\n%s\n%lld\n", (unsigned long long)getRawNowSec(), g_statusState.c_str(),
            g_statusText.c_str(), g_lastIconUrl.c_str(), (long long)g_timeOffset);
    fclose(f);
    g_statusWritten = getRawNowSec();
}

void writeStatus(const char* state, const char* text) {
    if (g_statusState == state && g_statusText == text && g_statusIcon == g_lastIconUrl &&
        getRawNowSec() - g_statusWritten < 60)
        return;
    g_statusState = state;
    g_statusText = text;
    g_statusIcon = g_lastIconUrl;
    flushStatus();
}

void statusHeartbeat() {
    if (getRawNowSec() - g_statusWritten >= 60)
        flushStatus();
}

// true when the console has a working internet connection right now
bool networkReady() {
    NifmInternetConnectionType type;
    u32 strength;
    NifmInternetConnectionStatus status;
    return R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
           status == NifmInternetConnectionStatus_Connected;
}

bool onBattery() {
    PsmChargerType charger;
    return R_SUCCEEDED(psmGetChargerType(&charger)) && charger == PsmChargerType_Unconnected;
}

void waitForNetworkReady() {
  writeToLog("[Network] Waiting for network to be ready...");

  while (true) {
    NifmInternetConnectionType connectionType;
    u32 wifiStrength;
    NifmInternetConnectionStatus connectionStatus;

    Result rc = nifmGetInternetConnectionStatus(&connectionType, &wifiStrength, &connectionStatus);
    
    if (R_SUCCEEDED(rc) && connectionStatus == NifmInternetConnectionStatus_Connected) {
      break;
    }

    svcSleepThread(1 * 1000 * 1000 * 1000ULL);
  }
}


// current process/title utilities

// Return 0 on success, non-zero on error (int holds the libnx Result)
Result get_app_tid(AppInfo *info) {
	Result rc = pmdmntGetApplicationProcessId(&info->pid);
	if (R_FAILED(rc)) return rc;
	return pmdmntGetProgramId(&info->tid, info->pid);
}

Result get_app_info(AppInfo *info) {
	u64 *out_pid = &info->pid;
	u64 *out_tid = &info->tid;
	
	Result rc;
	
	rc = pmdmntGetApplicationProcessId(out_pid);
	if (R_FAILED(rc)) return rc;
	
	rc = pmdmntGetProgramId(out_tid, *out_pid);
	if (R_FAILED(rc)) return rc;
	
	NsApplicationControlData* appControlData = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
	if (appControlData == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
	
	memset(appControlData, 0, sizeof(NsApplicationControlData));
    u64 appControlDataSize = sizeof(NsApplicationControlData);
	NacpLanguageEntry *languageEntry = NULL;
	
    Result res = MAKERESULT(Module_Libnx, LibnxError_NotFound);
	if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, *out_tid, appControlData, appControlDataSize, &appControlDataSize))) {
		const char* name = NULL;
		if (R_SUCCEEDED(nacpGetLanguageEntry(&appControlData->nacp, &languageEntry)) && languageEntry != NULL && languageEntry->name[0]) {
			name = languageEntry->name;
		} else {
			// no entry for the console language - take the first one that has a name
			for (int i = 0; i < 16 && !name; i++) {
				if (appControlData->nacp.lang[i].name[0])
					name = appControlData->nacp.lang[i].name;
			}
		}
		if (name) {
			strncpy(info->title_name, name, sizeof(info->title_name) - 1);
			info->title_name[sizeof(info->title_name) - 1] = '\0';
			res = 0;
		}
	}
	// still show *something* for a running title we can't read the name of
	if (R_FAILED(res)) {
		snprintf(info->title_name, sizeof(info->title_name), "%016llX", (unsigned long long)*out_tid);
		res = 0;
	}

	free(appControlData);

    return res;
}


// sleep/wake via psc.
// clocks are useless here - the whole sysmodule is frozen while asleep so time()
// and the system tick don't move. psc gives us a real event instead. the
// ReadySleep one fires *before* we sleep while wifi is still up, which is the
// only moment we can kill the discord session so it doesn't hang around during
// sleep. can't do the network call on this thread (tiny stack, and acking late
// would stall the sleep), so it just pokes the main thread and waits.
// id 0x5250 is a random unused id (system ones stop at 127), deps {fs, nifm} so
// we go down before them on sleep and come back after them on wake.

PscPmModule g_pscModule;
Thread g_pscThread;
bool g_pscReady = false;
volatile bool g_pscThreadRun = true;
volatile bool g_wokeFromSleep = false;
volatile bool g_sleepPending = false;
volatile bool g_shuttingDown = false;
volatile bool g_asleep = false;
UEvent g_pscWakeMain;
UEvent g_sleepPrepDone;

void pscThreadFunc(void* arg) {
    while (g_pscThreadRun) {
        // 1s timeout so we can bail out on shutdown
        if (R_FAILED(eventWait(&g_pscModule.event, 1000000000ULL))) continue;

        PscPmState state;
        u32 flags = 0;
        if (R_FAILED(pscPmModuleGetRequest(&g_pscModule, &state, &flags))) continue;

        // shutdown/reboot gets the same treatment as sleep: drop the presence
        // while wifi is still up, otherwise it lingers on Discord after power-off
        if (state == PscPmState_ReadySleep || state == PscPmState_ReadyShutdown) {
            // let the main thread kill the presence while wifi is still up, wait
            // a bit for it, then ack - never block the sleep for long
            g_shuttingDown = state == PscPmState_ReadyShutdown;
            g_sleepPending = true;
            ueventSignal(&g_pscWakeMain);
            waitSingle(waiterForUEvent(&g_sleepPrepDone), 8000000000ULL); // up to 8s (pausing = 2 requests)
        } else if (state == PscPmState_Awake) {
            g_wokeFromSleep = true;
            ueventSignal(&g_pscWakeMain);
        }

        pscPmModuleAcknowledge(&g_pscModule, state);
    }
}

int64_t g_timeOffset = 0;

u64 getCorrectedNowSec() {
    return (time_t)(getRawNowSec() + g_timeOffset);
}

u64 nowSec() {
    return getCorrectedNowSec();
}

u64 getRawNowSec() {
    u64 rawLocal = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &rawLocal)) || rawLocal == 0) {
        rawLocal = (u64)time(NULL);
    }
    return rawLocal;
}

extern time_t authTokenExpiry;
extern time_t sessionTokenExpiry;

void updateTimeOffset(int64_t newOffset) {
    int64_t delta = newOffset - g_timeOffset;
    g_timeOffset = newOffset;

    writeToLog("[Network] Synced time offset from Discord HTTP response: %lld seconds (delta: %llds)", 
               (long long)g_timeOffset, (long long)delta);

    // Shift active expiry targets so their remaining lifespans stay valid
    if (delta != 0) {
        if (authTokenExpiry > 0) {
            authTokenExpiry += delta;
        }
        if (sessionTokenExpiry > 0) {
            sessionTokenExpiry += delta;
        }
    }
}