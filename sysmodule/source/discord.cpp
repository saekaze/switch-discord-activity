// discord.cpp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <time.h>
#include <set>
#include <fstream>

#include <switch.h>
#include <json-c/json.h>
#include <curl/curl.h>

#include "config.h"
#include "discord.hpp"
#include "logging.hpp"
#include "utilities.hpp"

const char* clientId = DISCORD_CLIENT_ID;

// Every request goes to gaming-sdk.com (the Discord Social SDK API host).
// It serves the same /api/v9 routes as discord.com, and stays reachable
// where discord.com itself is blocked (e.g. Russia) - with discord.com the
// session request just timed out there.

// discord authentication tokens
std::string refreshToken = "";
std::string authToken = "";
// token for the rich presence session to refresh it every 15 minutes or delete it when the app is closed
std::string sessionToken = "";
time_t sessionTokenExpiry = 0; // we derive this ourselves by getting the time of the request we did to make the session + 20 minutes.
// the time when the auth token will expire, so we can refresh it before it does
time_t authTokenExpiry = 0;

std::string g_lastError = "";
bool g_iconPending = false;
std::string g_lastIconUrl = "";
static long g_lastHttpCode = 0;

// one line, printable, short - goes into the status file the app shows
static std::string shorten(const std::string& in, size_t max = 140) {
    std::string out;
    for (char c : in) {
        if (out.size() >= max) break;
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    return out;
}


void getRefreshTokenFromFile() {
    std::ifstream file("sdmc:/config/switchrpc_token");
    if (!file.is_open()) {
        writeToLog("[Discord] No refresh token found at sdmc:/config/switchrpc_token");
        // keep any in-memory token: logout deletes the file first, and we still
        // need to authenticate to tear the sessions down before forgetting it
        return;
    }
    
    std::string line;
    if (std::getline(file, line)) {
        refreshToken = line;
        if (!refreshToken.empty() && refreshToken.back() == '\n') {
            refreshToken.pop_back();
        }
        if (!refreshToken.empty() && refreshToken.back() == '\r') {
            refreshToken.pop_back();
        }
        writeToLog("[Discord] Successfully read refresh token from file");
    } else {
        writeToLog("[Discord] Failed to read from sdmc:/config/switchrpc_token");
    }
    file.close();
}

void saveRefreshTokenToFile() {
    // save the refresh token to a file on the sd card
    std::ofstream file("sdmc:/config/switchrpc_token", std::ios::trunc);
    if (!file.is_open()) { 
        writeToLog("[Discord] Failed to open sdmc:/config/switchrpc_token for writing!");
        return; 
    }
    file << refreshToken;
    file.close();
    writeToLog("[Discord] Successfully saved new refresh token to file");
}

void saveSessionTokenToFile(const std::string& token) {
    std::ofstream file("sdmc:/config/switchrpc_sessions", std::ios::app);
    if (!file.is_open()) {
        writeToLog("[Discord] Failed to open sdmc:/config/switchrpc_sessions for appending!");
        return;
    }
    file << token << std::endl;
    file.close();
    writeToLog("[Discord] Appended new session token to file");
}

int curlDebugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userp) {
    (void)handle; // Unused
    (void)userp;  // Unused

    const char* type_str = "";
    switch (type) {
        case CURLINFO_TEXT:         type_str = "Info"; break;
        case CURLINFO_HEADER_IN:    type_str = "Recv Header"; break;
        case CURLINFO_HEADER_OUT:   type_str = "Send Header"; break;
        default: return 0; 
    }

    std::string text(data, size);

    while(!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }

    if (!text.empty()) {
        writeToLog("[cURL %s] %s", type_str, text.c_str());
    }

    return 0;
}

// true once a Discord response told us the real time (Date header)
static bool g_timeSynced = false;

static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    (void)userdata;
    size_t totalSize = size * nitems;
    std::string header(buffer, totalSize);

    if (header.size() >= 5 && strncasecmp(header.c_str(), "Date:", 5) == 0) {
        const char* dateStr = header.c_str() + 5;
        while (*dateStr == ' ' || *dateStr == '\t') dateStr++;

        time_t serverUtc = curl_getdate(dateStr, NULL);
        if (serverUtc != -1) {
            g_timeSynced = true;
            u64 rawLocal = getRawNowSec();
            if (rawLocal > 0) {
                int64_t newOffset = (int64_t)serverUtc - (int64_t)rawLocal;
                if (g_timeOffset != newOffset) {
                    updateTimeOffset(newOffset);
                }
            }
        }
    }

    return totalSize;
}

// curl helper for easy requests to discord,
// specify url, method, headers, and body as needed and a pointer to a string to store the response in, and the size of that string.
bool sendRequest(const char* url, const char* method, struct curl_slist* headers, const char* body, std::string* response) {
    writeToLog("[Discord] Sending %s request to %s", method, url);
    // token requests/responses carry secrets - never put those in the log
    const bool secret = strstr(url, "oauth2") != NULL;
    if (body != NULL && !secret) {
        writeToLog("[Discord] Request Body: %s", body);
    }

    CURL *curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        curl_slist* reqHeaders = NULL;
        reqHeaders = curl_slist_append(reqHeaders, "Accept: application/json");
        // discord social sdk sends these
        reqHeaders = curl_slist_append(reqHeaders, "User-Agent: Discord Embedded/0.0.8");
        // x super props
        /*
        {
            "browser":"Discord Embedded",
            "browser_user_agent":"Discord Embedded/0.0.8",
            "browser_version":"0.0.8",
            "client_build_number":304683,
            "design_id":0,
            "os":"Unknown",
            "release_channel":"unknown"
        }
        */
        // basically static, hardcoding b64 string.
        reqHeaders = curl_slist_append(reqHeaders, "X-Super-Properties: eyJicm93c2VyIjoiRGlzY29yZCBFbWJlZGRlZCIsImJyb3dzZXJfdXNlcl9hZ2VudCI6IkRpc2NvcmQgRW1iZWRkZWQvMC4wLjgiLCJicm93c2VyX3ZlcnNpb24iOiIwLjAuOCIsImNsaWVudF9idWlsZF9udW1iZXIiOjMwNDY4MywiZGVzaWduX2lkIjowLCJvcyI6IlVua25vd24iLCJyZWxlYXNlX2NoYW5uZWwiOiJ1bmtub3duIn0K");
        // host
        if (strstr(url, "discord.com") != NULL) {
            reqHeaders = curl_slist_append(reqHeaders, "Host: discord.com");
        } else if (strstr(url, "gaming-sdk.com") != NULL) {
            reqHeaders = curl_slist_append(reqHeaders, "Host: gaming-sdk.com");
        }

        reqHeaders = curl_slist_append(reqHeaders, "Connection: close");

        // iterate through the provided headers and add them to the request, overriding any duplicates of the default headers we set above
        for (curl_slist* header = headers; header != NULL; header = header->next) {
            reqHeaders = curl_slist_append(reqHeaders, header->data);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, reqHeaders);
        if (body != NULL)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        // don't let a request hang forever - matters for the pre-sleep cleanup,
        // it shouldn't hold up the console going to sleep
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        // need header data
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

        // temporary fix to failing dns resolution
        struct curl_slist *dns_cache = NULL;
        dns_cache = curl_slist_append(dns_cache, "discord.com:443:162.159.138.232");
        dns_cache = curl_slist_append(dns_cache, "discord.com:443:162.159.135.232");
        
        dns_cache = curl_slist_append(dns_cache, "gaming-sdk.com:443:104.18.41.99");
        dns_cache = curl_slist_append(dns_cache, "gaming-sdk.com:443:172.64.146.157");
        curl_easy_setopt(curl, CURLOPT_RESOLVE, dns_cache);

        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curlDebugCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            size_t total_size = size * nmemb;
            if (userdata != NULL) {
                std::string* resp = static_cast<std::string*>(userdata);
                resp->append(ptr, total_size); 
            }
            return total_size;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response); 
        
        CURLcode res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            g_lastError = std::string("Can't reach Discord: ") + curl_easy_strerror(res);
            writeToLog("[Discord] cURL request failed: %s", curl_easy_strerror(res));
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            if (response != NULL) {
                writeToLog("[Discord] cURL request successful. Response length: %zu", response->size());
                if (!secret)
                    writeToLog("[Discord] Response Body: %s", response->c_str());
            } else {
                writeToLog("[Discord] cURL request successful. (No response expected)");
            }
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        bool success = (http_code >= 200 && http_code < 300);
        g_lastHttpCode = res == CURLE_OK ? http_code : 0;
        if (res == CURLE_OK && !success) {
            writeToLog("[Discord] HTTP %ld from %s", http_code, url);
            char prefix[48];
            snprintf(prefix, sizeof(prefix), "Discord said HTTP %ld: ", http_code);
            // error bodies are safe to show even for oauth2 (no tokens in them)
            g_lastError = prefix + (response ? shorten(*response) : std::string());
            if (secret && response)
                writeToLog("[Discord] Error body: %s", shorten(*response, 400).c_str());
        }
        
        curl_slist_free_all(reqHeaders);
        curl_slist_free_all(headers); 
        curl_slist_free_all(dns_cache);
        curl_easy_cleanup(curl);
        return success;
    } else {
        writeToLog("[Discord] Failed to initialize cURL!");
        curl_slist_free_all(headers);
        return false;
    }
}

bool refreshAuthTokenIfNeeded() {
    // check that we have a refresh token, if not, we can't do anything
    getRefreshTokenFromFile();
    if (refreshToken.empty()) { 
        writeToLog("[Discord] Cannot refresh: Refresh token is empty");
        return false; 
    }

    // check if the auth token is expired or will expire in the next 5 minutes
    if (time(NULL) < authTokenExpiry - 300) { 
        writeToLog("[Discord] Auth token is still valid.");
        return true;
    }

    writeToLog("[Discord] Auth token expired or expiring soon. Refreshing...");

    const char* url = "https://gaming-sdk.com/api/v9/oauth2/token";
    const char* method = "POST";
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded; charset=utf-8");
    std::string bodyStr = "grant_type=refresh_token&refresh_token=" + refreshToken + "&client_id=" + clientId;

    std::string response = "";
    bool success = sendRequest(url, method, headers, bodyStr.c_str(), &response);

    if (!success) {
        writeToLog("[Discord] Failed to send request for auth token refresh!");
        if (g_lastHttpCode == 400 || g_lastHttpCode == 401)
            g_lastError = "Discord login expired - open Switch Discord Activity and log in again";
        return false;
    }

    json_object* json_response = json_tokener_parse(response.c_str());
    if (json_response == NULL) { 
        writeToLog("[Discord] Failed to parse JSON response from token refresh!");
        return false; 
    }
    
    json_object* json_auth_token;
    json_object* json_refresh_token;
    json_object* json_expires_in;
    
    if (json_object_object_get_ex(json_response, "access_token", &json_auth_token)) {
        authToken = json_object_get_string(json_auth_token);
        writeToLog("[Discord] Successfully extracted new access_token");
    }
    if (json_object_object_get_ex(json_response, "refresh_token", &json_refresh_token)) {
        refreshToken = json_object_get_string(json_refresh_token);
        writeToLog("[Discord] Successfully extracted new refresh_token");
    }
    if (json_object_object_get_ex(json_response, "expires_in", &json_expires_in)) {
        authTokenExpiry = getCorrectedNowSec() + json_object_get_int(json_expires_in);
    }

    json_object_put(json_response); // Frees the parsed json object

    // save the new refresh token to the sd card
    saveRefreshTokenToFile();
    return true;
}

bool authenticatedRequest(const char* url, const char* method, struct curl_slist* headers, const char* body, std::string* response) {
    refreshAuthTokenIfNeeded();
    
    if (authToken.empty()) {
        writeToLog("[Discord] WARNING: Attempted authenticated request but authToken is empty!");
        if (refreshToken.empty())
            g_lastError = "Not logged in - open Switch Discord Activity and log in";
        return false;
    }

    std::string authHeader = "Authorization: Bearer " + authToken;
    struct curl_slist* authHeaders = curl_slist_append(headers, authHeader.c_str());
    return sendRequest(url, method, authHeaders, body, response);
}

void discordCleanupStaleSessions() {
    std::ifstream file("sdmc:/config/switchrpc_sessions");
    if (!file.is_open()) {
        writeToLog("[Discord] No stale sessions file found. Skipping cleanup.");
        return;
    }

    writeToLog("[Discord] Found switchrpc_sessions file, cleaning up stale sessions...");
    
    std::set<std::string> uniqueTokens;
    std::string line;
    
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            uniqueTokens.insert(line);
        }
    }
    file.close();

    if (!sessionToken.empty() && uniqueTokens.count(sessionToken) > 0) {
        if (time(NULL) < sessionTokenExpiry) {
            uniqueTokens.erase(sessionToken);
        }
    }
    
    for (const std::string& token : uniqueTokens) {
        json_object* json_body = json_object_new_object();
        json_object_object_add(json_body, "token", json_object_new_string(token.c_str()));
        const char* body = json_object_get_string(json_body);

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

        writeToLog("[Discord] Deleting stale headless session...");
        authenticatedRequest("https://gaming-sdk.com/api/v9/users/@me/headless-sessions/delete", "POST", headers, body, NULL);

        json_object_put(json_body);
    }
    
    if (remove("sdmc:/config/switchrpc_sessions") == 0) {
        writeToLog("[Discord] Successfully removed sessions file.");
    } else {
        writeToLog("[Discord] Failed to remove sessions file after cleanup.");
    }
}

// Buttons in headless sessions aren't documented: try the RPC shape first,
// then labels + metadata, then give up on buttons (never on the status).
static int g_buttonMode = 0;  // 0 = [{label,url}], 1 = labels + metadata.button_urls, 2 = off

bool discordCreateHeadlessSession(u64 titleId, std::string titleName, u64 startRawSec, const bool includeToken,
                                  const bool quickIcon, const ActivityOpts& opts, const bool retried) {
    writeToLog("[Discord] Creating/Updating session for TID: %016llX (%s)", (unsigned long long)titleId, titleName.c_str());
    
    // eShop icon -> tinfoil -> the title's own icon -> hbmenu. quickIcon only
    // looks at the cache so a new game shows up instantly; the caller posts
    // again once it's found (g_iconPending).
    std::string iconUrl;
    IconKind kind = Icon_Official;
    if (opts.icon)
        iconUrl = opts.icon;
    else
        kind = resolveIconUrl(titleId, iconUrl, quickIcon);
    g_iconPending = kind == Icon_Miss;
    g_lastIconUrl = iconUrl;

    std::string stateStr;
    if (opts.state) {
        stateStr = opts.state;
    } else {
        stateStr = kind == Icon_Homebrew && opts.homebrewTag ? "on Nintendo Switch (homebrew)" : "on Nintendo Switch";
        stateStr += opts.extra;
    }
    const char* state = stateStr.c_str();

    // "Nintendo eShop" button - only games the eShop actually lists
    std::string page;
    bool button = false;
    if (opts.eshopButton && g_buttonMode < 2 && !opts.icon && titleId != HOMEBREW_TID && kind != Icon_Homebrew) {
        int r = getEshopPage(titleId, page, quickIcon);
        if (r < 0) g_iconPending = true;  // not looked up yet
        button = r > 0;
    }

    // make json body
    json_object* json_body = json_object_new_object();
    json_object* json_activities = json_object_new_array();
    json_object* json_activity = json_object_new_object();
    json_object_object_add(json_activity, "type", json_object_new_string("0"));
    json_object_object_add(json_activity, "flags", json_object_new_string("0"));  
    json_object_object_add(json_activity, "application_id", json_object_new_string(clientId));
    json_object_object_add(json_activity, "platform", json_object_new_string("desktop"));
    json_object_object_add(json_activity, "name", json_object_new_string(titleName.c_str()));
    json_object_object_add(json_activity, "state", json_object_new_string(state));

    // elapsed timer: discord shows (now - start). we pass a start moved forward
    // to exclude sleep, so it counts real playtime. timestamps are in ms.
    //
    // start is in the *console's* clock, which on modded consoles is often
    // minutes off (no time sync). a start in the future makes Discord show no
    // timer until real time catches up, so convert it with the offset learned
    // from Discord's Date header - and learn that offset first if needed.
    if (startRawSec != 0) {
        refreshAuthTokenIfNeeded();  // usually a request -> syncs the clock
        if (!g_timeSynced)
            sendRequest("https://gaming-sdk.com/api/v9/gateway", "GET", NULL, NULL, NULL);
        int64_t start = (int64_t)startRawSec + g_timeOffset;
        int64_t now = (int64_t)getCorrectedNowSec();
        if (start > now) start = now;
        json_object* json_timestamps = json_object_new_object();
        json_object_object_add(json_timestamps, "start", json_object_new_int64(start * 1000));
        json_object_object_add(json_activity, "timestamps", json_timestamps);
    }

    if (button) {
        json_object* buttons = json_object_new_array();
        if (g_buttonMode == 0) {
            json_object* btn = json_object_new_object();
            json_object_object_add(btn, "label", json_object_new_string("Nintendo eShop"));
            json_object_object_add(btn, "url", json_object_new_string(page.c_str()));
            json_object_array_add(buttons, btn);
        } else {
            json_object_array_add(buttons, json_object_new_string("Nintendo eShop"));
            json_object* meta = json_object_new_object();
            json_object* urls = json_object_new_array();
            json_object_array_add(urls, json_object_new_string(page.c_str()));
            json_object_object_add(meta, "button_urls", urls);
            json_object_object_add(json_activity, "metadata", meta);
        }
        json_object_object_add(json_activity, "buttons", buttons);
    }

    json_object* json_assets = json_object_new_object();
    if (!iconUrl.empty())
        json_object_object_add(json_assets, "large_image", json_object_new_string(iconUrl.c_str()));
    json_object_object_add(json_activity, "assets", json_assets);
    
    json_object_array_add(json_activities, json_activity);
    json_object_object_add(json_body, "activities", json_activities);
    
    if (includeToken && !sessionToken.empty()) {
        writeToLog("[Discord] Including session token in request.");
        json_object_object_add(json_body, "token", json_object_new_string(sessionToken.c_str()));
    }
    
    const char* body = json_object_get_string(json_body);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    std::string response;
    bool success = authenticatedRequest("https://gaming-sdk.com/api/v9/users/@me/headless-sessions", "POST", headers, body, &response);

    // free it once here - used to get freed twice on a failed no-token request
    // and trash the heap
    json_object_put(json_body);

    if (!success && button && g_lastHttpCode == 400) {
        // Discord didn't like the button: next shape, or no button at all
        g_buttonMode++;
        writeToLog("[Discord] Button rejected (%s), trying %s.", g_lastError.c_str(),
                   g_buttonMode == 1 ? "the other button format" : "without a button");
        return discordCreateHeadlessSession(titleId, titleName, startRawSec, includeToken, quickIcon, opts, retried);
    }

    if (!success) {
        writeToLog("[Discord] Failed to create/update headless session! (%s)", g_lastError.c_str());
        // access token rejected: force a refresh and try once more
        if (g_lastHttpCode == 401 && !retried) {
            writeToLog("[Discord] Got 401, refreshing the access token and retrying...");
            authToken = "";
            authTokenExpiry = 0;
            return discordCreateHeadlessSession(titleId, titleName, startRawSec, false, quickIcon, opts, true);
        }
        // if the attempt happened with an existing session token, try again without it in case the token was the issue
        if (includeToken) {
            writeToLog("[Discord] Retrying headless session creation without session token...");
            return discordCreateHeadlessSession(titleId, titleName, startRawSec, false, quickIcon, opts, retried);
        }
        return false;
    }

    json_object* json_response = json_tokener_parse(response.c_str());
    if (json_response == NULL) { 
        writeToLog("[Discord] Failed to parse headless session response JSON!");
        g_lastError = "Discord sent an unreadable answer";
        return false; 
    }
    
    json_object* json_session_token;
    if (json_object_object_get_ex(json_response, "token", &json_session_token)) {
        std::string newToken = json_object_get_string(json_session_token);
        if (newToken != sessionToken) {
            sessionToken = newToken;
            saveSessionTokenToFile(sessionToken);
        }

        sessionToken = newToken;
        sessionTokenExpiry = getCorrectedNowSec() + (15 * 60);

        writeToLog("[Discord] Status is live on Discord: %s", titleName.c_str());
    } else {
        writeToLog("[Discord] Headless session response did NOT contain a session token. That shouldn't happen if the session was created successfully and discord returned 200.");
    }

    json_object_put(json_response);
    return true;
}

static void deleteSessionToken(const std::string& token) {
    json_object* json_body = json_object_new_object();
    json_object_object_add(json_body, "token", json_object_new_string(token.c_str()));
    const char* body = json_object_get_string(json_body);
    struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json; charset=utf-8");
    authenticatedRequest("https://gaming-sdk.com/api/v9/users/@me/headless-sessions/delete", "POST", headers, body, NULL);
    json_object_put(json_body);
}

// Drop the old session first and give Discord a moment, then create the new
// one. If the activity never disappears, Discord treats the new session as
// the same activity and keeps its old timer and image - that's why the timer
// kept running through "paused" and late icons never showed up.
static bool replaceSession(u64 titleId, const std::string& name, u64 startRawSec, const ActivityOpts& opts,
                           bool quickIcon) {
    if (!sessionToken.empty()) {
        deleteSessionToken(sessionToken);
        sessionToken = "";
        sessionTokenExpiry = 0;
        svcSleepThread(2000000000ULL);
    }
    return discordCreateHeadlessSession(titleId, name, startRawSec, false, quickIcon, opts);
}

bool discordShow(u64 titleId, std::string name, u64 startRawSec, const ActivityOpts& opts) {
    if (!replaceSession(titleId, name, startRawSec, opts, true))
        return false;
    // icon / eShop page weren't cached: look them up now and show them
    if (g_iconPending) {
        writeToLog("[Discord] Icon or eShop page not cached yet, looking up and re-posting.");
        replaceSession(titleId, name, startRawSec, opts, false);
    }
    return true;
}

void discordDeleteHeadlessSession() {
    writeToLog("[Discord] Deleting headless session...");
    
    if (sessionToken.empty()) { 
        writeToLog("[Discord] No active session token to delete.");
        return; 
    }

    json_object* json_body = json_object_new_object();
    json_object_object_add(json_body, "token", json_object_new_string(sessionToken.c_str()));
    const char* body = json_object_get_string(json_body);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    authenticatedRequest("https://gaming-sdk.com/api/v9/users/@me/headless-sessions/delete", "POST", headers, body, NULL);

    json_object_put(json_body);
    
    // Clear the token so we don't try to use it again
    sessionToken = "";
    sessionTokenExpiry = 0;
    writeToLog("[Discord] Session deleted successfully.");
}

void discordLogout() {
    writeToLog("[Discord] Logging out - dropping session and stored tokens.");
    // delete the active session + any leftovers. these authenticate, which may
    // refresh (and rewrite) the token file, so we still have valid auth here.
    discordDeleteHeadlessSession();
    discordCleanupStaleSessions();
    // now forget everything and make sure we stay logged out even if a refresh
    // just rewrote the token file
    refreshToken = "";
    authToken = "";
    authTokenExpiry = 0;
    remove("sdmc:/config/switchrpc_token");
}
