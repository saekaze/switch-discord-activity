// Switch Discord Activity app - graphical UI (SDL2), English / Japanese.
//
// Platform independent on purpose: on a PC the same code renders previews
// (see tools/preview.c), on the Switch it uses the console's shared font.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { Lang_En, Lang_Ja } Lang;

// Settings, in sdmc:/config/switchrpc.ini order (read by the sysmodule).
typedef enum {
    Set_Enabled,     // master switch
    Set_Paused,      // "Paused" while the game is in the background
    Set_Screens,     // HOME / eShop / Album without a game
    Set_Timer,       // elapsed time
    Set_Icon,        // game icon
    Set_GameName,    // game name (off: just "Playing a game")
    Set_Dock,        // Docked / Handheld
    Set_Playtime,    // total playtime
    Set_PausedTime,  // played time in the paused status
    Set_Button,      // eShop button
    Set_HbTag,       // "(homebrew)"
    Set_Count
} SettingId;

extern const char *const set_keys[Set_Count];

// Menu items: the master switch, the toggles (same index as the setting), then actions.
enum {
    Item_Master = Set_Enabled,
    Item_FirstToggle = Set_Paused,
    Item_LastToggle = Set_Count - 1,
    Item_Login = Set_Count,
    Item_Test,
    Item_Icons,
    Item_Diag,
    Item_Lang,
    Item_Count
};
#define TOGGLE_ROWS 5  // toggles are shown in two columns of 5

typedef enum { Tone_Normal, Tone_Good, Tone_Warn, Tone_Bad } Tone;

typedef struct {
    bool logged_in;
    const char *user;
    bool running;
    const char *status_state;  // live / idle / error / login / off (from the module) or ""
    const char *status_text;
    bool set[Set_Count];
    int sel;
    const char *msg;
    Tone msg_tone;
} MenuModel;

// Translated strings.
typedef enum {
    T_Title, T_Subtitle,
    T_Discord, T_Module, T_Now,
    T_LoggedInAs, T_NotLoggedIn, T_Running, T_NotRunning, T_TurnedOff, T_Live,
    T_Master, T_On, T_Off, T_MasterOnDesc, T_MasterOffDesc,
    T_WhatToShow, T_AccountTools,
    T_Login, T_Logout, T_Test, T_Icons, T_Diag, T_Lang,
    T_FooterMenu, T_FooterBack, T_FooterDiag, T_FooterWorking,
    T_LoginTitle, T_LoginStep1, T_LoginStep1b, T_LoginOr, T_LoginStep2, T_LoginStep3, T_FooterLogin,
    T_TestTitle, T_IconsTitle, T_DiagTitle,
    T_NotShowing,
    T_MsgSaved, T_MsgOn, T_MsgOff, T_MsgLoggedOut, T_MsgNoWifi, T_MsgLoginOk, T_MsgLoginFail, T_MsgFinishing,
    T_Count
} TextId;

bool ui_init(Lang lang);
void ui_exit(void);
void ui_set_lang(Lang lang);
Lang ui_lang(void);
const char *tr(TextId id);
const char *tr_setting(int setting);       // label of a toggle
const char *tr_help(int item);             // one-line help for a menu item

bool ui_frame_begin(void);                 // draws the scene; false = window closed (PC)
void ui_frame_end(void);

void ui_menu(const MenuModel *m);
// A text page (test / icons / diagnostics). Lines may contain \x1b[31m-style colours.
void ui_lines(const char *title, const char *const *lines, int n, const char *footer);
// QR login page. qr = size*size bytes, nonzero = dark module.
void ui_login(const char *url, const uint8_t *qr, int qr_size, const char *note);
// Saves the current frame as a BMP (PC previews).
void ui_screenshot(const char *path);
