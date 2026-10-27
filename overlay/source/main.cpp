// Switch Discord Activity - Tesla overlay.
//
// Every setting of the app, reachable from the overlay menu while a game is
// running. It edits the same sdmc:/config/switchrpc.ini the app writes; the
// module re-reads it every poll, so changes apply within a couple of seconds.
// Logging in, icons and updates stay in the app (they need the full screen).
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "settings.h"

#define USER_PATH   "sdmc:/config/switchrpc_user"
#define STATUS_PATH "sdmc:/config/switchrpc_status"

namespace {

int g_lang = 0;  // 0 = English, 1 = Japanese

enum Text { T_On, T_Off, T_WhatToShow, T_LoggedIn, T_NotLoggedIn, T_Running, T_NotRunning, T_Live, T_AppHint, T_Count };

const char *const TEXTS[T_Count][2] = {
    { "On", "オン" },
    { "Off", "オフ" },
    { "What to show", "表示する項目" },
    { "Discord: %s", "Discord: %s" },
    { "Not logged in - log in from the app", "未ログイン - アプリでログインしてください" },
    { "Module running", "モジュール動作中" },
    { "Module not running", "モジュール停止中" },
    { "Live: %s", "表示中: %s" },
    { "Log in, icons and updates: in the app", "ログイン・アイコン・アップデートはアプリで" },
};

const char *tr(Text t) { return TEXTS[t][g_lang]; }

void readLine(const char *path, char *out, size_t size, int skip = 0) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    for (int i = 0; i <= skip; i++)
        if (!fgets(out, size, f)) { out[0] = '\0'; break; }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
}

bool moduleRunning() {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, SYSMODULE_TID));
}

// the three lines at the top: account, module, what Discord shows now
struct State {
    char user[64] = "";
    bool running = false;
    char state[16] = "", text[160] = "";

    void refresh() {
        readLine(USER_PATH, user, sizeof(user));
        running = moduleRunning();
        // status file: time, state, text, ...
        readLine(STATUS_PATH, state, sizeof(state), 1);
        readLine(STATUS_PATH, text, sizeof(text), 2);
    }
};

State g_state;

class MainGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("Discord Activity", "v" SWITCHRPC_VERSION);
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            const tsl::Color dim = tsl::style::color::ColorDescription, good = tsl::Color(0x6, 0xD, 0x8, 0xF);
            char line[200];
            if (g_state.user[0]) snprintf(line, sizeof(line), tr(T_LoggedIn), g_state.user);
            else snprintf(line, sizeof(line), "%s", tr(T_NotLoggedIn));
            r->drawString(line, false, x + 3, y + 22, 17, r->a(g_state.user[0] ? tsl::style::color::ColorText : dim));
            r->drawString(tr(g_state.running ? T_Running : T_NotRunning), false, x + 3, y + 46, 17,
                          r->a(g_state.running ? tsl::style::color::ColorText : tsl::Color(0xF, 0x8, 0x5, 0xF)));
            if (!strcmp(g_state.state, "live")) {
                snprintf(line, sizeof(line), tr(T_Live), g_state.text);
                r->drawString(line, false, x + 3, y + 70, 17, r->a(good), w - 6);
            } else if (g_state.text[0]) {
                r->drawString(g_state.text, false, x + 3, y + 70, 17, r->a(dim), w - 6);
            }
        }), 86);

        // master switch first, the rest under a header, same order as the app
        for (int i = 0; i < Set_Count; i++) {
            if (i == Set_Paused)
                list->addItem(new tsl::elm::CategoryHeader(tr(T_WhatToShow)));
            auto item = new tsl::elm::ToggleListItem(SETTING_LABELS[i][g_lang], m_set[i], tr(T_On), tr(T_Off));
            item->setStateChangedListener([this, i](bool on) { save(i, on); });
            m_items[i] = item;
            list->addItem(item);
        }

        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            r->drawString(tr(T_AppHint), false, x + 3, y + 30, 15, r->a(tsl::style::color::ColorDescription));
        }), 45);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        // once a second: the module's status, and settings changed elsewhere (the app)
        if (m_frame++ % 60) return;
        g_state.refresh();
        bool now[Set_Count];
        settings_load(now, nullptr);
        for (int i = 0; i < Set_Count; i++)
            if (now[i] != m_set[i]) {
                m_set[i] = now[i];
                m_items[i]->setState(now[i]);
            }
    }

    MainGui() { settings_load(m_set, nullptr); }

private:
    // re-read before writing, so a change made in the app meanwhile isn't lost
    void save(int i, bool on) {
        int lang;
        settings_load(m_set, &lang);
        m_set[i] = on;
        settings_save(m_set, lang);
    }

    bool m_set[Set_Count];
    tsl::elm::ToggleListItem *m_items[Set_Count] = {};
    u64 m_frame = 0;
};

class DiscordOverlay : public tsl::Overlay {
public:
    void initServices() override {
        fsdevMountSdmc();
        // the app's language choice, or the console's
        bool set[Set_Count];
        int lang;
        settings_load(set, &lang);
        if (lang < 0 && R_SUCCEEDED(setInitialize())) {
            u64 code = 0;
            SetLanguage l;
            if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &l)))
                lang = l == SetLanguage_JA ? 1 : 0;
            setExit();
        }
        g_lang = lang == 1 ? 1 : 0;
        g_state.refresh();
    }

    void exitServices() override { fsdevUnmountAll(); }

    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<MainGui>(); }
};

}  // namespace

int main(int argc, char **argv) {
    return tsl::loop<DiscordOverlay>(argc, argv);
}
