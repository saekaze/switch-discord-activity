// Include the most common headers from the C standard library
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <map>
#include <sys/stat.h>

// Include the main libnx system header, for Switch development
#include <switch.h>
#include <json-c/json.h>
#include <curl/curl.h>

#include "config.h"
#include "discord.hpp"
#include "logging.hpp"
#include "utilities.hpp"

// might rewrite this sysmodule in swift again.
// i didnt need to rewrite in c++ but i ran out of ideas to fix crashing in swift
// but it was just a big struct being put on stack in swift. 

// Size of the inner heap (adjust as necessary).
#define INNER_HEAP_SIZE 0x80000

#ifdef __cplusplus
extern "C" {
#endif

// Sysmodules should not use applet*.
u32 __nx_applet_type = AppletType_None;
SslServiceType __nx_ssl_service_type = SslServiceType_System;

// Sysmodules will normally only want to use one FS session.
u32 __nx_fs_num_sessions = 1;


// Newlib heap configuration function (makes malloc/free work).
void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    // Configure the newlib heap.
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

// Service initialization.
void __appInit(void)
{
    Result rc;

    // Open a service manager session.
    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    // Retrieve the current version of Horizon OS.
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro)); 
        setsysExit();
    }

    // Disable this if you don't want to use the filesystem.
    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    timeInitialize(); // time() for log timestamps, the refresh timer and token expiry
    nifmInitialize(NifmServiceType_System);

    // Disable this if you don't want to use the SD card filesystem.
    fsdevMountSdmc();
    initLog();

    // Add other services you want to use here.
    SocketInitConfig sockConf = {
        .tcp_tx_buf_size = 0x800,
        .tcp_rx_buf_size = 0x1000,
        .tcp_tx_buf_max_size = 0x2EE0,
        .tcp_rx_buf_max_size = 0x2EE0,

        .udp_tx_buf_size = 0x1000,
        .udp_rx_buf_size = 0x1000,

        .sb_efficiency = 4,

        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_System,
    };
    rc = socketInitialize(&sockConf);
    if (R_FAILED(rc))
        writeToLog("[SwitchRPC] Warning: socketInitialize failed with code 0x%08X. This might be due to memory constraints. Network features will not work. Maybe disable other sysmodules?", rc);
    curl_global_init(CURL_GLOBAL_DEFAULT);
	
	pmdmntInitialize();
	nsInitialize();
	setInitialize();
	pscmInitialize();
	g_pdmReady = R_SUCCEEDED(pdmqryInitialize());  // focus detection (HOME / eShop / ...)
	g_ldrReady = R_SUCCEEDED(ldrDmntInitialize());  // album vs hbmenu
	apmInitialize();                                  // docked / handheld
	psmInitialize();                                  // charger (battery saver)

    // Close the service manager session.
    smExit();
}

// Service deinitialization.
void __appExit(void)
{
    // stop the psc thread first, we're leaving anyway
    g_pscThreadRun = false;
    if (g_pscReady) {
        threadWaitForExit(&g_pscThread);
        threadClose(&g_pscThread);
        pscPmModuleFinalize(&g_pscModule);
        pscPmModuleClose(&g_pscModule);
    }
    pscmExit();
    if (g_pdmReady) pdmqryExit();
    if (g_ldrReady) ldrDmntExit();
    apmExit();
    psmExit();

    discordDeleteHeadlessSession();
    writeToLog("[SwitchRPC] Sysmodule exiting. Goodbye!");

    curl_global_cleanup();

    socketExit();
    setExit();
    nsExit();
    pmdmntExit();
    nifmExit();
    timeExit();

    // Close extra services you added to __appInit here.
    fsdevUnmountAll(); // Disable this if you don't want to use the SD card filesystem.
    fsExit(); // Disable this if you don't want to use the filesystem.
}

#ifdef __cplusplus
}
#endif


// logged in = the config app left a refresh token on the sd card
static bool isLoggedIn() {
    FILE* f = fopen("sdmc:/config/switchrpc_token", "r");
    if (f) { fclose(f); return true; }
    return false;
}

const int REFRESH_INTERVAL = 15 * 60;

// What should be on Discord right now.
// Want_Hidden = show nothing (turned off, or that case is switched off in the settings)
enum WantMode { Want_None, Want_Play, Want_Paused, Want_Screen, Want_Hidden };

struct Want {
    WantMode mode = Want_None;
    u64 tid = 0;              // game (Play/Paused), 0 for Screen
    Screen screen = Screen_Game;
    bool docked = false;      // only tracked when "Docked / Handheld" is on
    char name[513] = {0};

    bool operator==(const Want& o) const {
        return mode == o.mode && tid == o.tid && screen == o.screen && docked == o.docked && !strcmp(name, o.name);
    }
    bool operator!=(const Want& o) const { return !(*this == o); }
};

static std::string hms(u64 sec) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu", (unsigned long long)(sec / 3600),
             (unsigned long long)(sec / 60 % 60), (unsigned long long)(sec % 60));
    return buf;
}

// A whole .nro: header, then the asset section (icon, nacp, romfs) right
// behind the code. Catches a download that was cut off.
static bool nroComplete(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    u8 hdr[0x20], aset[0x38];
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && !memcmp(hdr + 0x10, "NRO0", 4);
    u32 size = 0;
    if (ok) memcpy(&size, hdr + 0x18, 4);
    ok = ok && fseek(f, size, SEEK_SET) == 0 && fread(aset, 1, sizeof(aset), f) == sizeof(aset) &&
         !memcmp(aset, "ASET", 4);
    u64 end = size;
    for (int i = 0; ok && i < 3; i++) {  // icon, nacp, romfs: offset + size from the asset header
        u64 off, len;
        memcpy(&off, aset + 8 + i * 16, 8);
        memcpy(&len, aset + 16 + i * 16, 8);
        if (len && size + off + len > end) end = size + off + len;
    }
    ok = ok && fseek(f, 0, SEEK_END) == 0 && (u64)ftell(f) >= end;
    fclose(f);
    return ok;
}

// The app's updater can't replace the app's own .nro while it's open: it
// leaves "<nro>.new" (checked against SHA256SUMS) and writes the path to
// PENDING_NRO. The app can't be open at boot, so finish the swap here.
// (The 1.1.0 updater left the .new without the path file.)
#define PENDING_NRO "sdmc:/config/switchrpc-pending-nro"
static void finishAppUpdate() {
    char target[300] = "sdmc:/switch/switch-discord-activity.nro";
    if (FILE* f = fopen(PENDING_NRO, "r")) {
        char line[300];
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!strncmp(line, "sdmc:/", 6)) snprintf(target, sizeof(target), "%s", line);
        }
        fclose(f);
    }
    char tmp[310];
    snprintf(tmp, sizeof(tmp), "%s.new", target);
    struct stat st;
    if (stat(tmp, &st) != 0) {
        remove(PENDING_NRO);
        return;
    }
    if (!nroComplete(tmp)) {
        writeToLog("[SwitchRPC] %s is incomplete, deleting it (update the app again).", tmp);
        remove(tmp);
        remove(PENDING_NRO);
        return;
    }
    remove(target);
    if (rename(tmp, target) == 0) {
        remove(PENDING_NRO);
        writeToLog("[SwitchRPC] Finished updating the app: %s", target);
    } else {
        writeToLog("[SwitchRPC] Couldn't finish updating the app (is it open?), trying again next boot.");
    }
}

int main(int argc, char* argv[])
{
    writeToLog("[SwitchRPC] Sysmodule v%s started successfully.", SWITCHRPC_VERSION);
    mkdir("sdmc:/config", 0777);
    finishAppUpdate();
    if (!isLoggedIn())
        writeToLog("[SwitchRPC] Not logged in yet - open the Switch Discord Activity app and log in.");
    if (!g_pdmReady)
        writeToLog("[SwitchRPC] pdm:qry unavailable - can't tell HOME menu from the game, always showing the game.");
    writeStatus("idle", "Module started, waiting for a game");

    Settings cfg = loadSettings();
    Want cur;                   // what Discord shows
    Want pending;               // seen on the previous poll, not applied yet
    bool wasLoggedIn = false;
    bool postOk = true;         // false = last post failed, retry soon
    u64 lastPostRaw = 0;

    // playtime of the current game: banked seconds + the running chunk
    u64 gameTid = 0;
    u64 played = 0;
    // playtime of games we switched away from while they kept running (e.g.
    // a quick visit to hbmenu), so coming back continues the timer
    std::map<u64, u64> bank;
    bool needCleanup = true;    // stale sessions from before a reboot / sleep
    bool waitingForNet = false; // last post skipped: no internet
    u64 playStartRaw = 0;       // 0 = not counting (paused / no game)
    u64 screenStartRaw = 0;     // when a no-game screen (HOME, eShop...) was entered

    char appName[513] = {0};    // NACP name cache for gameTid
    u64 appNameTid = 0;

    auto playedNow = [&]() -> u64 {
        return played + (playStartRaw ? getRawNowSec() - playStartRaw : 0);
    };

    // posts `w` (a fresh session replacing the old one, or an in-place refresh)
    auto post = [&](const Want& w, bool refresh) {
        if (w.mode == Want_None)
            return;
        if (!networkReady()) {
            // never block on the network: note it and post as soon as it's back
            postOk = false;
            waitingForNet = true;
            lastPostRaw = getRawNowSec();
            writeStatus("error", "No internet connection - waiting for it");
            return;
        }
        waitingForNet = false;
        if (w.mode == Want_Hidden) {
            discordDeleteHeadlessSession();
            postOk = true;
            lastPostRaw = getRawNowSec();
            writeStatus(cfg.enabled ? "idle" : "off", w.name);
            return;
        }

        std::string name = w.name, state;
        std::string statusText = w.name;
        std::string dock = cfg.dockMode ? (w.docked ? " \u00b7 Docked" : " \u00b7 Handheld") : "";
        u64 start = 0;
        ActivityOpts opts;
        opts.homebrewTag = cfg.homebrewTag;
        opts.eshopButton = cfg.eshopButton;
        if (!cfg.icon)
            opts.icon = "";
        // "Game name" off: say *that* you play, not *what* (no icon / button either)
        bool anon = !cfg.gameName && (w.mode == Want_Play || w.mode == Want_Paused);
        if (anon) {
            name = "Nintendo Switch";
            opts.icon = "";
            opts.eshopButton = false;
        }

        switch (w.mode) {
        case Want_Play: {
            if (cfg.timer)
                start = getRawNowSec() - playedNow();
            std::string extra = dock;
            u64 mins = cfg.totalPlaytime && !anon ? totalPlaytimeMinutes(w.tid) : 0;
            if (mins >= 60)
                extra += " \u00b7 " + std::to_string(mins / 60) + " h total";
            else if (mins > 0)
                extra += " \u00b7 " + std::to_string(mins) + " min total";
            if (anon) {
                state = "Playing a game" + extra;
                opts.state = state.c_str();
            } else {
                opts.extra = extra;
            }
            break;
        }
        case Want_Paused:
            state = std::string("Paused \u00b7 ") + screenPausedLabel(w.screen);
            if (cfg.pausedTime)
                state += " \u00b7 " + hms(playedNow());
            opts.state = state.c_str();
            statusText += std::string(" (paused: ") + screenPausedLabel(w.screen) + ")";
            break;
        case Want_Screen:
            if (cfg.timer)
                start = screenStartRaw;
            state = "on Nintendo Switch" + dock;
            opts.state = state.c_str();
            opts.icon = "";           // no game, no game icon
            opts.eshopButton = false;
            break;
        default:
            return;
        }

        if (refresh)
            postOk = discordCreateHeadlessSession(w.tid, name, start, true, true, opts);
        else
            postOk = discordShow(w.tid, name, start, opts);
        lastPostRaw = getRawNowSec();
        if (postOk)
            writeStatus("live", statusText.c_str());
        else
            writeStatus("error", g_lastError.empty() ? "Discord refused the status (see log)" : g_lastError.c_str());
    };

    // switch Discord over to `w`, keeping the playtime books straight
    auto apply = [&](const Want& w) {
        u64 now = getRawNowSec();
        if (playStartRaw) {                    // stop the running chunk
            played += now - playStartRaw;
            playStartRaw = 0;
        }
        // a hidden game (settings) still keeps its playtime
        u64 newGame = (w.mode == Want_Play || w.mode == Want_Paused || w.mode == Want_Hidden) ? w.tid : 0;
        if (newGame != gameTid) {
            // hbmenu opened over a running game: keep the game's time for later
            if (gameTid && gameTid != HOMEBREW_TID && newGame == HOMEBREW_TID)
                bank[gameTid] = played;
            gameTid = newGame;
            auto it = bank.find(newGame);
            played = it != bank.end() ? it->second : 0;
            if (it != bank.end()) bank.erase(it);
        }
        if (w.mode == Want_Play)
            playStartRaw = now;
        if (w.mode == Want_Screen && (cur.mode != Want_Screen || cur.screen != w.screen))
            screenStartRaw = now;

        writeToLog("[SwitchRPC] Now: mode %d, %s (TID %016llX, screen %d)", (int)w.mode, w.name,
                   (unsigned long long)w.tid, (int)w.screen);
        cur = w;
        post(w, false);
    };

    // register with psc and start the thread that watches for sleep/wake
    {
        ueventCreate(&g_pscWakeMain, true);
        ueventCreate(&g_sleepPrepDone, true);
        static const u32 pscDeps[] = { PscPmModuleId_Fs, PscPmModuleId_Nifm };
        Result prc = pscmGetPmModule(&g_pscModule, (PscPmModuleId)0x5250, pscDeps,
                                     sizeof(pscDeps) / sizeof(pscDeps[0]), true);
        if (R_SUCCEEDED(prc)) {
            if (R_SUCCEEDED(threadCreate(&g_pscThread, pscThreadFunc, NULL, NULL, 0x4000, 0x2C, -2))) {
                threadStart(&g_pscThread);
                g_pscReady = true;
                writeToLog("[SwitchRPC] PSC power-state monitoring active.");
            } else {
                // no ack thread = we'd stall power transitions, so unregister
                pscPmModuleFinalize(&g_pscModule);
                pscPmModuleClose(&g_pscModule);
                writeToLog("[SwitchRPC] Warning: could not start PSC thread; sleep detection disabled.");
            }
        } else {
            writeToLog("[SwitchRPC] Warning: pscmGetPmModule failed (0x%08X); sleep detection disabled.", prc);
        }
    }

    while (true) {
        // just woke up: forget what's shown so the next poll re-posts it
        // (that replaces the "paused - sleep mode" session)
        if (g_wokeFromSleep) {
            g_wokeFromSleep = false;
            g_asleep = false;
            needCleanup = true;
            writeToLog("[SwitchRPC] Wake from sleep (PSC).");
            cur = Want();
            pending = Want();
        }

        // user hit "log out" in the config app (token file is gone) - drop the
        // session and stop pushing until they log back in
        bool loggedIn = isLoggedIn();
        if (wasLoggedIn && !loggedIn && !g_asleep) {
            writeToLog("[SwitchRPC] Logged out, clearing presence.");
            discordLogout();
            cur = Want();
            pending = Want();
        }
        wasLoggedIn = loggedIn;
        if (!loggedIn)
            writeStatus("login", "Not logged in - open Switch Discord Activity and log in");

        // settings changed in the app: re-post everything with the new options
        Settings newCfg = loadSettings();
        if (newCfg != cfg) {
            writeToLog("[SwitchRPC] Settings changed (enabled=%d).", (int)newCfg.enabled);
            cfg = newCfg;
            cur = Want();
            pending = Want();
        }

        if (needCleanup && loggedIn && networkReady()) {
            discordCleanupStaleSessions();
            needCleanup = false;
        }

        if (!g_asleep && loggedIn) {
            Want w;
            AppInfo info = {0};
            AlbumState album = getAlbumState();
            if (album == Album_Homebrew) {
                // hbmenu / .nro in the foreground (album applet) - hbloader
                // doesn't tell us which .nro, so it's just "Homebrew"
                w.mode = Want_Play;
                w.tid = HOMEBREW_TID;
                strcpy(w.name, "Homebrew");
            } else if (R_SUCCEEDED(get_app_tid(&info)) && info.tid != 0) {
                // a game is open - in front, or suspended behind HOME / eShop / ...
                if (info.tid != appNameTid) {
                    if (R_SUCCEEDED(get_app_info(&info))) {
                        memcpy(appName, info.title_name, sizeof(appName));
                        appNameTid = info.tid;
                    }
                }
                // the real album is a library applet: if it's open, it's in front
                Screen sc = album == Album_Real ? Screen_Album : getForegroundScreen(info.tid);
                w.mode = sc == Screen_Game ? Want_Play : Want_Paused;
                w.tid = info.tid;
                w.screen = sc;
                memcpy(w.name, appName, sizeof(w.name));
                // only one game runs at a time: forget banked time of closed ones
                for (auto it = bank.begin(); it != bank.end();)
                    it = it->first != info.tid ? bank.erase(it) : std::next(it);
            } else {
                bank.clear();  // no game running
                // no game: HOME menu (incl. settings, news...), eShop, user page...
                Screen sc = album == Album_Real ? Screen_Album : getForegroundScreen(0);
                if (sc == Screen_Game) sc = Screen_Home;
                w.mode = Want_Screen;
                w.screen = sc;
                snprintf(w.name, sizeof(w.name), "%s", screenName(sc));
            }

            if (cfg.dockMode && w.mode != Want_Paused)
                w.docked = isDocked();
            // the settings can hide some or all of it
            if (!cfg.enabled) {
                w.mode = Want_Hidden;
                w.docked = false;
                snprintf(w.name, sizeof(w.name), "Turned off in Switch Discord Activity");
            } else if (w.mode == Want_Paused && !cfg.paused) {
                w.mode = Want_Hidden;
                snprintf(w.name, sizeof(w.name), "Game in the background - hidden (settings)");
            } else if (w.mode == Want_Screen && !cfg.systemScreens) {
                w.mode = Want_Hidden;
                w.docked = false;
                snprintf(w.name, sizeof(w.name), "No game open - hidden (settings)");
            }

            u64 now = getRawNowSec();
            if (w != cur) {
                // must be seen on two polls in a row (~2 s) so a quick overlay
                // or button press doesn't flicker the status - except the very
                // first post and a newly launched game
                bool newGame = w.mode == Want_Play && w.tid != cur.tid;
                if (w == pending || cur.mode == Want_None || newGame)
                    apply(w);
                pending = w;
            } else {
                pending = w;
                // last post failed (no network yet, Discord hiccup...): try again
                if (cur.mode == Want_Hidden) {
                    // nothing on Discord, nothing to keep alive
                }
                else if (!postOk && (now - lastPostRaw >= 30 || (waitingForNet && networkReady()))) {
                    writeToLog("[SwitchRPC] Retrying failed post.");
                    post(cur, false);
                }
                // keep the session alive (Discord drops it after ~20 min); a
                // paused session also gets its "played" time refreshed
                else if (now - lastPostRaw >= REFRESH_INTERVAL) {
                    writeToLog("[SwitchRPC] Periodic session refresh.");
                    post(cur, true);
                }
            }
        }
        if (g_asleep && cfg.batterySaver && g_pscReady) {
            // battery saver: the module does nothing at all while the console
            // sleeps (it may still be woken for background downloads) - no
            // polling, no SD writes - until the PSC thread reports the wake
            waitSingle(waiterForUEvent(&g_pscWakeMain), UINT64_MAX);
        } else {
            statusHeartbeat();
            // check every 2s so a game switch / HOME press shows up quickly;
            // battery saver on battery: every 5s
            u64 pollSec = cfg.batterySaver && onBattery() ? 5 : 2;
            waitSingle(waiterForUEvent(&g_pscWakeMain), pollSec * 1000ULL * 1000 * 1000);
        }

        // about to sleep or shut down, wifi still up - act now before we freeze,
        // stop polling until we wake, then let the psc thread ack
        if (g_sleepPending) {
            g_sleepPending = false;
            if (playStartRaw) {                // bank the playtime, the timer resumes minus sleep
                played += getRawNowSec() - playStartRaw;
                playStartRaw = 0;
            }

            if (!networkReady()) {
                writeToLog("[SwitchRPC] Sleep/shutdown without internet - nothing to send.");
            } else if (g_shuttingDown || gameTid == 0 || !isLoggedIn() || !cfg.enabled || !cfg.paused || cfg.batterySaver) {
                // battery saver: one quick request and the radio can rest -
                // no "paused" status that would need Discord to drop it later
                writeToLog("[SwitchRPC] %s (PSC). Clearing Discord presence.", g_shuttingDown ? "Shutting down" : "Sleep imminent");
                discordDeleteHeadlessSession();
            } else {
                // sleep: keep the game on Discord with the time frozen. Discord
                // drops it by itself after ~15-20 min since we can't refresh while asleep.
                writeToLog("[SwitchRPC] Sleep imminent (PSC). Showing the game as paused.");
                Want sleepW = cur;
                sleepW.mode = Want_Paused;
                sleepW.tid = gameTid;
                sleepW.screen = Screen_Sleep;
                memcpy(sleepW.name, appName, sizeof(sleepW.name));
                if (gameTid == HOMEBREW_TID) strcpy(sleepW.name, "Homebrew");
                post(sleepW, false);
                if (!postOk)
                    discordDeleteHeadlessSession();
            }
            g_asleep = true;
            writeStatus("idle", g_shuttingDown ? "Console shutting down"
                              : gameTid && !cfg.batterySaver && cfg.paused ? "Console asleep - shown as paused on Discord"
                              : "Console asleep");

            ueventSignal(&g_sleepPrepDone);
        }
    }

    return 0;
}
