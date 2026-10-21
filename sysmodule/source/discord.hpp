// discord.hpp

#pragma once

#include <switch.h>
#include <string>

// ------------------------------------------------------------------
// Public Discord API Functions
// ------------------------------------------------------------------

/**
 * Creates or updates a Discord Headless Session to show the current game.
 * 
 * @param titleId The Title ID of the currently running game.
 * @param startRawSec Console-clock time (getRawNowSec) the elapsed timer counts from; 0 to omit it.
 *                    Converted to real time with the offset from Discord's Date header.
 * @param includeToken Whether to include the current sessionToken in the request.
 */
/// How an activity looks, besides the game name and timer.
struct ActivityOpts {
    const char* state = nullptr;  ///< replaces the whole second line
    const char* icon = nullptr;   ///< image URL instead of the title's icon ("" = no image)
    std::string extra;            ///< appended to the default second line, e.g. " · Docked · 152 h total"
    bool homebrewTag = true;      ///< "(homebrew)" for titles without an official icon
    bool eshopButton = false;     ///< "Nintendo eShop" button for games the eShop knows
};

/**
 * Creates (or with includeToken refreshes) the headless session.
 * quickIcon: only use cached icon / eShop page; sets g_iconPending if
 * something is still to be looked up.
 * startRawSec: console-clock start of the elapsed timer, 0 = no timer.
 */
bool discordCreateHeadlessSession(u64 titleId, std::string titleName, u64 startRawSec, const bool includeToken,
                                  const bool quickIcon, const ActivityOpts& opts, const bool retried = false);

/**
 * Shows an activity by deleting the previous session and creating a fresh
 * one (Discord keeps the old timer/image if the activity never goes away).
 * If the icon or eShop page wasn't cached it's looked up afterwards and the
 * session replaced once more.
 */
bool discordShow(u64 titleId, std::string name, u64 startRawSec, const ActivityOpts& opts);

/// Set when a quickIcon post had no cached icon.
extern bool g_iconPending;
/// Icon URL of the last post, for the app.
extern std::string g_lastIconUrl;

/// Human readable reason for the last failed Discord request (shown in the app).
extern std::string g_lastError;

/**
 * Deletes the active Discord Headless Session.
 * Call this when the game closes or the sysmodule is exiting.
 */
void discordDeleteHeadlessSession();

/**
 * Deletes all known sessions by clearing the sessions file.
 * Call this on startup or when waking from sleep to clean up any sessions that should no longer be active.
 */
void discordCleanupStaleSessions();

/**
 * Tears down the active session and forgets all stored tokens.
 * Call when the user logs out from the config app.
 */
void discordLogout();