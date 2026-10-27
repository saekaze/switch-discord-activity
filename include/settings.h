// The user's settings: sdmc:/config/switchrpc.ini ("key=0/1" lines).
// Written by the app and the overlay, read by the sysmodule every poll.
// Plain C, also included from C++ (the overlay).
#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SETTINGS_PATH "sdmc:/config/switchrpc.ini"

typedef enum {
    Set_Enabled,       // master switch
    Set_Paused,        // "Paused" while the game is in the background
    Set_Screens,       // HOME / eShop / Album without a game
    Set_Timer,         // elapsed time
    Set_Icon,          // game icon
    Set_GameName,      // game name (off: just "Playing a game")
    Set_Dock,          // Docked / Handheld
    Set_Playtime,      // total playtime
    Set_PausedTime,    // played time in the paused status
    Set_Button,        // eShop button
    Set_HbTag,         // "(homebrew)"
    Set_BatterySaver,  // sleep clears the status, slower polling on battery
    Set_Count
} SettingId;

// same order as SettingId
static const char *const SETTING_KEYS[Set_Count] = {
    "enabled", "paused", "system_screens", "show_timer", "show_icon", "show_game_name",
    "dock_mode", "total_playtime", "paused_time", "eshop_button", "homebrew_tag", "battery_saver",
};

static const bool SETTING_DEFAULTS[Set_Count] = {
    true, true, true, true, true, true, true, true, true, true, true, false,
};

// { English, Japanese }, same order as SettingId
static const char *const SETTING_LABELS[Set_Count][2] = {
    { "Discord status", "Discordステータス" },
    { "Paused in background", "一時停止を表示" },
    { "HOME / eShop / Album", "HOMEやeショップ" },
    { "Elapsed time", "経過時間" },
    { "Game icon", "ゲームアイコン" },
    { "Game name", "ゲーム名" },
    { "Docked / Handheld", "TV / 携帯モード" },
    { "Total playtime", "総プレイ時間" },
    { "Time while paused", "一時停止中の時間" },
    { "eShop button", "eショップボタン" },
    { "\"(homebrew)\" tag", "「(homebrew)」表記" },
    { "Battery saver", "バッテリー節約" },
};

// language: 0 = English, 1 = Japanese, -1 = not chosen yet (follow the console)
static inline void settings_load(bool *v, int *lang) {
    for (int i = 0; i < Set_Count; i++) v[i] = SETTING_DEFAULTS[i];
    if (lang) *lang = -1;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[128], key[64];
    int val;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %63[a-z_] = %d", key, &val) != 2) continue;
        if (!strcmp(key, "language") && lang) *lang = val;
        for (int i = 0; i < Set_Count; i++)
            if (!strcmp(key, SETTING_KEYS[i])) v[i] = val != 0;
    }
    fclose(f);
}

static inline void settings_save(const bool *v, int lang) {
    mkdir("sdmc:/config", 0777);
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "# Switch Discord Activity settings - change them in the app or the overlay\n");
    for (int i = 0; i < Set_Count; i++)
        fprintf(f, "%s=%d\n", SETTING_KEYS[i], v[i] ? 1 : 0);
    if (lang >= 0)
        fprintf(f, "language=%d\n", lang);
    fclose(f);
}
