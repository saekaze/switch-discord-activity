#pragma once

#include <string>
#include <switch.h>

/**
 * Waits until the network is ready before proceeding.
*/
void waitForNetworkReady();

/// Internet connection up right now? (never blocks)
bool networkReady();

/// Running on battery: handheld without a charger.
bool onBattery();

/**
 * Looks up the eShop square icon URL for a title from Nintendo's EU search API.
 * Returns true and sets out_url on success, false if the title isn't found.
 *
 * @param titleId  The title ID of the game (as a u64).
 * @param out_url  Output: the image_url_sq_s from the EU eShop (500×500 JPEG).
 */
bool fetchEshopIconUrl(u64 titleId, std::string& out_url);

// Pseudo title id used while a .nro runs through hbloader (album applet).
#define HOMEBREW_TID 0x010000000000100DULL

// Shown for homebrew and for titles no icon database knows (forwarders etc).
#define NRO_ICON_URL "https://raw.githubusercontent.com/switchbrew/nx-hbmenu/master/icon.jpg"

enum IconKind {
    Icon_Miss,      ///< cacheOnly lookup found nothing yet - out_url is empty
    Icon_Official,  ///< eShop / tinfoil game icon
    Icon_Homebrew,  ///< forwarder's own uploaded icon or the generic hbmenu icon
    Icon_Unknown,   ///< the icon hosts didn't answer: no image for now, asked again later
};

/**
 * Picks the large image for a title: eShop icon, then tinfoil, then the
 * title's own icon uploaded once (forwarders), then the generic homebrew
 * icon. With cacheOnly it never touches the network (instant), and returns
 * Icon_Miss if the answer isn't cached yet.
 */
IconKind resolveIconUrl(u64 titleId, std::string& out_url, bool cacheOnly);

/// set by a cacheOnly resolveIconUrl that used a saved link not checked yet
/// this boot: the caller posts first, then checks it (resolveIconUrl again
/// without cacheOnly) and re-posts only if the link was dead
extern bool g_iconCheckLater;

/**
 * eShop store page of a title. 1 = found, 0 = the eShop doesn't list it,
 * -1 = not looked up yet (only with cacheOnly).
 */
int getEshopPage(u64 titleId, std::string& out_url, bool cacheOnly);

/// sdmc:/config/switchrpc.ini, written by the Switch Discord Activity app.
struct Settings {
    bool enabled = true;        ///< master switch: off = no Discord status at all
    bool paused = true;         ///< game in the background -> "Paused" (off: hide)
    bool systemScreens = true;  ///< HOME / eShop / ... without a game (off: hide)
    bool timer = true;          ///< elapsed time
    bool icon = true;           ///< game icon
    bool gameName = true;       ///< off: "Nintendo Switch" / "Playing a game" - hides which game
    bool pausedTime = true;     ///< played time in the paused status
    bool dockMode = true;       ///< " · Docked" / " · Handheld"
    bool totalPlaytime = true;  ///< " · 152 h total"
    bool eshopButton = true;    ///< "Nintendo eShop" button for Switch games
    bool homebrewTag = true;    ///< "(homebrew)" for forwarders & co
    bool batterySaver = false;  ///< sleep clears the status, slower polling on battery

    bool operator==(const Settings& o) const {
        return enabled == o.enabled && paused == o.paused && systemScreens == o.systemScreens &&
               timer == o.timer && icon == o.icon && gameName == o.gameName && pausedTime == o.pausedTime &&
               dockMode == o.dockMode && totalPlaytime == o.totalPlaytime &&
               eshopButton == o.eshopButton && homebrewTag == o.homebrewTag &&
               batterySaver == o.batterySaver;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
Settings loadSettings();

/// TV mode: the console runs in boost mode while docked.
bool isDocked();
/// Total playtime of a game from the system's own statistics (0 if unknown).
u64 totalPlaytimeMinutes(u64 titleId);

enum AlbumState { Album_None, Album_Homebrew, Album_Real };
extern bool g_ldrReady;

/**
 * The album applet is either Nintendo's album or, on Atmosphere, hbmenu /
 * a .nro (hbloader). Tells them apart by how many modules the process has.
 */
AlbumState getAlbumState();

/**
 * True while hbmenu / a .nro is running from the album applet.
 */
bool isHomebrewRunning();

/// What's on screen.
enum Screen {
    Screen_Game,    ///< the game itself (or something it opened: keyboard, controller setup...)
    Screen_Home,    ///< HOME menu - also System Settings, News, ... (all part of the HOME menu)
    Screen_Shop,    ///< Nintendo eShop
    Screen_MyPage,  ///< user page / profile, friends
    Screen_Web,     ///< web browser
    Screen_MiiEdit, ///< Mii maker
    Screen_Album,   ///< Nintendo's album (screenshots / videos) - not hbmenu
    Screen_Sleep,   ///< not a screen: used for the sleep-mode "paused" status
};

extern bool g_pdmReady;

/**
 * Works out what's in the foreground from the play log (pdm:qry), which
 * records every focus change between the game and system screens.
 * appTid = the running game (0 if none); returns Screen_Game when unsure.
 */
Screen getForegroundScreen(u64 appTid);

/// "HOME Menu", "Nintendo eShop", ... (activity name when no game is open)
const char* screenName(Screen s);
/// "HOME Menu", "browsing the Nintendo eShop", "sleep mode"... (paused game)
const char* screenPausedLabel(Screen s);

/**
 * Status for the Switch Discord Activity app, in sdmc:/config/switchrpc_status:
 * "<unix time>\n<state>\n<text>\n". state is live / error / idle / login.
 * Rewritten on every change and at least once a minute (heartbeat), so the
 * app can tell the module is alive.
 */
void writeStatus(const char* state, const char* text);
void statusHeartbeat();

typedef struct {
	u64 pid;          ///< process id
	u64 tid;          ///< title id
	char title_name[513];  ///< Title name 512 + 1 for null terminator
} AppInfo;

/**
 * Retrieves information about the currently running application, including its process ID, title ID, and title name.
 * 
 * @param info Pointer to an AppInfo structure that will be filled with the application information.
 * @return Returns 0 on success, or a non-zero value on error (the int holds the libnx Result).
 */
Result get_app_info(AppInfo *info);

/**
 * Like get_app_info but only pid + title id (no name lookup) - cheap enough
 * to call every couple of seconds.
 */
Result get_app_tid(AppInfo *info);

extern PscPmModule g_pscModule;
extern Thread g_pscThread;
extern bool g_pscReady;
extern volatile bool g_pscThreadRun;
extern volatile bool g_wokeFromSleep;
extern volatile bool g_sleepPending;
extern volatile bool g_shuttingDown;  ///< the pending power-down is a shutdown/reboot, not sleep
extern volatile bool g_asleep;
extern UEvent g_pscWakeMain;
extern UEvent g_sleepPrepDone;

void pscThreadFunc(void*);

// system clock may be off from real time, we depend on discord to find that offset.
// Share single global time offset across all translation units
extern int64_t g_timeOffset; 

/**
 * Returns the raw time without corrections.
 */
u64 getRawNowSec();

/**
 * Returns current epoch timestamp (seconds) corrected using HTTP Date header offsets.
 */
u64 getCorrectedNowSec();

/**
 * Convenience wrapper for getCorrectedNowSec().
 */
u64 nowSec();

/**
 * Updates the global time offset between local Switch clock and server real time,
 * adjusting any active expiry timestamps by the offset delta.
 */
void updateTimeOffset(int64_t newOffset);