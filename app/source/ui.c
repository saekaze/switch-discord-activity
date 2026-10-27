// Switch Discord Activity app - graphical UI (SDL2 + SDL_ttf + SDL_image).
//
// A torii in front of the moon on a mountain at dusk, a frog sitting under it,
// a few sakura petals;
// the menu is plain type on a shade that fades into the picture, with
// vermilion only as an accent and a seal stamp as the on/off switch.
// All art is drawn by art/make_art.py into romfs/.

#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#ifdef __SWITCH__
#include <switch.h>
#define ASSET(p) "romfs:/" p
#else
#define ASSET(p) "romfs/" p
#endif

#define SCREEN_W 1280
#define SCREEN_H 720


// --- strings -------------------------------------------------------------------

static Lang g_lang = Lang_En;

static const char *const strings[T_Count][2] = {
    [T_Title]        = { "Switch Discord Activity", "Switch Discord Activity" },
    [T_Subtitle]     = { "Discord status for your Switch", "SwitchのDiscordステータス" },
    [T_Discord]      = { "DISCORD", "Discord" },
    [T_Module]       = { "MODULE", "モジュール" },
    [T_Now]          = { "NOW", "現在" },
    [T_LoggedInAs]   = { "logged in as %s", "%s でログイン中" },
    [T_NotLoggedIn]  = { "not logged in - choose \"Log in with Discord\"", "未ログイン ―「Discordでログイン」を選択" },
    [T_Running]      = { "running", "動作中" },
    [T_NotRunning]   = { "not running - see Diagnostics", "停止中 ―「診断とログ」を確認" },
    [T_TurnedOff]    = { "turned off - nothing is shown on Discord", "オフ ― Discordには何も表示されません" },
    [T_Live]         = { "Live: %s", "表示中: %s" },
    [T_Master]       = { "Discord status", "Discordステータス" },
    [T_On]           = { "ON", "オン" },
    [T_Off]          = { "OFF", "オフ" },
    [T_MasterOnDesc] = { "On  ·  shown on your Discord profile", "オン ・ Discordのプロフィールに表示中" },
    [T_MasterOffDesc]= { "Off  ·  nothing is shown", "オフ ・ 何も表示されません" },
    [T_WhatToShow]   = { "WHAT TO SHOW", "表示する項目" },
    [T_AccountTools] = { "ACCOUNT & TOOLS", "アカウントとツール" },
    [T_Login]        = { "Log in with Discord", "Discordでログイン" },
    [T_Logout]       = { "Log out (%s)", "ログアウト (%s)" },
    [T_Test]         = { "Test connection", "接続テスト" },
    [T_Icons]        = { "Prepare icons", "アイコンを準備" },
    [T_Diag]         = { "Diagnostics & log", "診断とログ" },
    [T_Lang]         = { "Language: English", "言語: 日本語" },
    [T_Update]       = { "Check for updates", "アップデートを確認" },
    [T_UpdateTo]     = { "Update to %s", "%s にアップデート" },
    [T_UpdateTitle]  = { "Update", "アップデート" },
    [T_UpdateAvail]  = { "update %s available", "アップデート %s があります" },
    [T_FooterMenu]   = { "[↑↓] select   [A] toggle / open   [-] status on/off   [+] exit",
                         "[↑↓] 選択   [A] 切替 / 開く   [-] ステータス オン/オフ   [+] 終了" },
    [T_FooterBack]   = { "[B] back", "[B] 戻る" },
    [T_FooterDiag]   = { "[B] back   [Y] start the module", "[B] 戻る   [Y] モジュールを起動" },
    [T_FooterWorking]= { "working...   [B] stop", "処理中…   [B] 中止" },
    [T_LoginTitle]   = { "Log in with Discord", "Discordでログイン" },
    [T_LoginStep1]   = { "1. Scan the QR code with your phone", "1. スマホでQRコードを読み取る" },
    [T_LoginStep1b]  = { "(phone on the same Wi-Fi as the Switch)", "（Switchと同じWi-Fiに接続してください）" },
    [T_LoginOr]      = { "or open this in any browser:", "またはブラウザでこのURLを開く:" },
    [T_LoginStep2]   = { "2. Tap \"Authorize\" in Discord", "2. Discordで「認証」をタップ" },
    [T_LoginStep3]   = { "3. Done - this screen continues by itself.", "3. 完了 ― この画面は自動で進みます" },
    [T_FooterLogin]  = { "[B] cancel", "[B] キャンセル" },
    [T_TestTitle]    = { "Discord test", "Discord 接続テスト" },
    [T_IconsTitle]   = { "Preparing icons", "アイコンを準備中" },
    [T_DiagTitle]    = { "Diagnostics", "診断" },
    [T_NotShowing]   = { "Not showing up? Discord > Settings > Activity Privacy > share your activity.",
                         "表示されない場合: Discord > 設定 > アクティビティ設定 > アクティビティを共有" },
    [T_MsgSaved]     = { "Saved - Discord updates in a few seconds.", "保存しました ― 数秒でDiscordに反映されます" },
    [T_MsgOn]        = { "Discord status is ON.", "Discordステータスをオンにしました" },
    [T_MsgOff]       = { "Discord status is OFF - it disappears in a few seconds.",
                         "Discordステータスをオフにしました ― 数秒で消えます" },
    [T_MsgLoggedOut] = { "Logged out.", "ログアウトしました" },
    [T_MsgNoWifi]    = { "Not connected to Wi-Fi - connect and try again.", "Wi-Fiに接続されていません" },
    [T_MsgLoginOk]   = { "Logged in as %s. Start a game - it shows up in a few seconds.",
                         "%s でログインしました。ゲームを起動すると数秒で表示されます" },
    [T_MsgLoginFail] = { "Login failed - try again.", "ログインに失敗しました。もう一度お試しください" },
    [T_MsgFinishing] = { "Got it, finishing login...", "ログインを完了しています…" },
};


static const char *const help_texts[Item_Count][2] = {
    [Item_Master]    = { "Turns the whole Discord status on or off.", "Discordステータス全体をオン/オフします。" },
    [Set_Paused]     = { "Game open but you're in the HOME Menu, eShop...: show it paused (off: hide it).",
                         "ゲーム中にHOMEメニューなどを開いたとき「一時停止」と表示（オフ: 非表示）。" },
    [Set_Screens]    = { "No game open: show HOME Menu, eShop, Album... (off: show nothing).",
                         "ゲームを起動していないとき、HOMEメニュー・eショップ・アルバムを表示。" },
    [Set_Timer]      = { "Shows how long you've been playing.", "プレイ中の経過時間を表示します。" },
    [Set_Icon]       = { "Shows the game's icon.", "ゲームのアイコンを表示します。" },
    [Set_GameName]   = { "Off: only \"Playing a game\" - nobody sees which game.",
                         "オフにすると、どのゲームを遊んでいるかは表示されません。" },
    [Set_Dock]       = { "Adds \"Docked\" or \"Handheld\".", "「Docked（TV）」「Handheld（携帯）」を表示します。" },
    [Set_Playtime]   = { "Adds the game's total playtime, e.g. \"152 h total\".", "そのゲームの総プレイ時間を表示します。" },
    [Set_PausedTime] = { "Shows the time played so far on the paused status.", "一時停止中に、それまでのプレイ時間を表示します。" },
    [Set_Button]     = { "A button under the status that opens the game's eShop page.",
                         "ステータスの下にeショップのページを開くボタンを表示します。" },
    [Set_HbTag]      = { "Writes \"(homebrew)\" after forwarders and other non-eShop titles.",
                         "フォワーダーなどeショップにないソフトに「(homebrew)」と表記します。" },
    [Set_BatterySaver] = { "Module fully idle in sleep (status cleared); checks less often on battery.",
                         "スリープ中はモジュールを完全に休止し、バッテリー駆動中は確認の間隔を長くします。" },
    [Item_Login]     = { "Log in once with your phone - then it works on its own.",
                         "スマホで一度ログインするだけで、あとは自動で動作します。" },
    [Item_Test]      = { "Sends a test status to Discord and shows every step.",
                         "テスト用のステータスを送信して、各ステップの結果を表示します。" },
    [Item_Icons]     = { "Uploads forwarder icons now (backup if icons are missing).",
                         "フォワーダーのアイコンを今すぐアップロードします（表示されない場合の予備）。" },
    [Item_Diag]      = { "Versions, module state, log - send a photo if something's wrong.",
                         "バージョン・モジュールの状態・ログ。不具合のときは写真を送ってください。" },
    [Item_Update]    = { "Downloads the newest version from GitHub and installs it.",
                         "GitHubから最新版をダウンロードしてインストールします。" },
    [Item_Lang]      = { "English / 日本語", "English / 日本語" },
};

void ui_set_lang(Lang lang) { g_lang = lang; }
Lang ui_lang(void) { return g_lang; }
const char *tr(TextId id) { return strings[id][g_lang]; }
const char *tr_setting(int s) { return SETTING_LABELS[s][g_lang]; }
const char *tr_help(int item) { return help_texts[item][g_lang]; }

// --- SDL state -------------------------------------------------------------------

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *t_bg, *t_torii, *t_frog, *t_petal, *t_stamp_on, *t_stamp_off, *t_shade;

enum { F_Label, F_Small, F_Body, F_Item, F_Head, F_Title, F_Count };
static const int font_px[F_Count] = { 14, 16, 19, 21, 25, 44 };
static TTF_Font *g_font[F_Count];

typedef struct { float x, y, vy, phase, spin; } Petal;
#define PETALS 14
static Petal petals[PETALS];

// washi paper ink on a dusk background; vermilion only as an accent
static const SDL_Color C_Ink    = { 242, 234, 216, 255 };
static const SDL_Color C_Text   = { 205, 198, 186, 255 };
static const SDL_Color C_Dim    = { 146, 138, 158, 255 };
static const SDL_Color C_Faint  = { 255, 255, 255, 34 };
static const SDL_Color C_Verm   = { 214, 64, 44, 255 };
static const SDL_Color C_Good   = { 160, 208, 160, 255 };
static const SDL_Color C_Warn   = { 232, 192, 120, 255 };
static const SDL_Color C_Bad    = { 230, 104, 88, 255 };
static const SDL_Color C_Info   = { 150, 196, 220, 255 };
// names used by the ANSI colour parser
#define C_White  C_Ink
#define C_Red    C_Bad
#define C_Green  C_Good
#define C_Yellow C_Warn
#define C_Cyan   C_Info

static SDL_Texture *load_tex(const char *path) {
    SDL_Surface *s = IMG_Load(path);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
    SDL_FreeSurface(s);
    return t;
}

// dark shade on the left that fades out before the torii - the menu sits on it
static SDL_Texture *make_shade(void) {
    const int w = 860, h = 8;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    for (int x = 0; x < w; x++) {
        float t = x < 470 ? 0.0f : (x - 470) / (float)(w - 470);
        float a = 1.0f - t * t * (3 - 2 * t);  // smoothstep
        Uint32 px = SDL_MapRGBA(s->format, 12, 9, 24, (Uint8)(226 * a));
        for (int y = 0; y < h; y++)
            ((Uint32 *)((Uint8 *)s->pixels + y * s->pitch))[x] = px;
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static bool open_fonts(void) {
#ifdef __SWITCH__
    // the console's own font covers English and Japanese
    PlFontData fd;
    if (R_FAILED(plInitialize(PlServiceType_User)) ||
        R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)))
        return false;
    for (int i = 0; i < F_Count; i++)
        g_font[i] = TTF_OpenFontRW(SDL_RWFromConstMem(fd.address, fd.size), 1, font_px[i]);
#else
    const char *path = getenv("UI_FONT");
    if (!path) path = "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf";
    for (int i = 0; i < F_Count; i++)
        g_font[i] = TTF_OpenFont(path, font_px[i]);
#endif
    for (int i = 0; i < F_Count; i++)
        if (!g_font[i]) return false;
    return true;
}

static void reset_petal(Petal *p, bool anywhere) {
    p->x = 760 + rand() % 560;
    p->y = anywhere ? rand() % SCREEN_H : -20 - rand() % 200;
    p->vy = 0.35f + (rand() % 100) / 220.0f;
    p->phase = (rand() % 628) / 100.0f;
    p->spin = (rand() % 360);
}

bool ui_init(Lang lang) {
    g_lang = lang;
#ifdef __SWITCH__
    romfsInit();
#endif
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
    g_win = SDL_CreateWindow("Switch Discord Activity", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H, 0);
    if (!g_win) return false;
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) return false;
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();
    if (!open_fonts()) return false;

    t_bg = load_tex(ASSET("bg.png"));
    t_torii = load_tex(ASSET("torii.png"));
    t_frog = load_tex(ASSET("frog.png"));
    t_petal = load_tex(ASSET("petal.png"));
    t_stamp_on = load_tex(ASSET("stamp_on.png"));
    t_stamp_off = load_tex(ASSET("stamp_off.png"));
    t_shade = make_shade();

    srand(42);
    for (int i = 0; i < PETALS; i++)
        reset_petal(&petals[i], true);
    return true;
}

// --- text (cached textures) --------------------------------------------------------

typedef struct {
    uint32_t hash;
    int font;
    SDL_Color color;
    SDL_Texture *tex;
    int w, h;
    uint32_t used;
} CachedText;

#define CACHE_SIZE 256
static CachedText cache[CACHE_SIZE];
static uint32_t frame_no;

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static CachedText *text_tex(int font, SDL_Color c, const char *s) {
    uint32_t h = fnv(s) ^ (uint32_t)(font * 977) ^ ((uint32_t)c.r << 16 | c.g << 8 | c.b);
    int oldest = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        CachedText *e = &cache[i];
        if (e->tex && e->hash == h && e->font == font) { e->used = frame_no; return e; }
        if (!cache[i].tex || cache[i].used < cache[oldest].used) oldest = i;
    }
    CachedText *e = &cache[oldest];
    if (e->tex) SDL_DestroyTexture(e->tex);
    memset(e, 0, sizeof(*e));
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font[font], s, c);
    if (!surf) return NULL;
    e->tex = SDL_CreateTextureFromSurface(g_ren, surf);
    e->w = surf->w; e->h = surf->h;
    SDL_FreeSurface(surf);
    e->hash = h; e->font = font; e->color = c; e->used = frame_no;
    return e;
}

static int text_w(int font, const char *s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(g_font[font], s, &w, &h);
    return w;
}

// align: 0 left, 1 center, 2 right. Returns the width drawn.
static int text(int x, int y, int font, SDL_Color c, int align, const char *s) {
    if (!s || !*s) return 0;
    CachedText *e = text_tex(font, c, s);
    if (!e || !e->tex) return 0;
    if (align == 1) x -= e->w / 2;
    if (align == 2) x -= e->w;
    SDL_Rect dst = { x, y, e->w, e->h };
    SDL_RenderCopy(g_ren, e->tex, NULL, &dst);
    return e->w;
}

static int textf(int x, int y, int font, SDL_Color c, int align, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return text(x, y, font, c, align, buf);
}

// text with "\x1b[31m"-style colours (lines produced by the test / icon jobs)
static void ansi_text(int x, int y, int font, const char *s) {
    SDL_Color c = C_White;
    char seg[512];
    size_t n = 0;
    for (const char *p = s;; p++) {
        if (*p == '\x1b' || !*p) {
            seg[n] = '\0';
            x += text(x, y, font, c, 0, seg);
            n = 0;
            if (!*p) break;
            int code = 0;
            const char *q = p + 2;
            while (*q && *q != 'm') {
                if (*q >= '0' && *q <= '9') code = code * 10 + (*q - '0');
                if (*q == ';') code = 0;
                q++;
            }
            switch (code) {
            case 0: case 37: c = C_White; break;
            case 1: break;
            case 2: c = C_Dim; break;
            case 31: c = C_Red; break;
            case 32: c = C_Green; break;
            case 33: c = C_Yellow; break;
            case 36: c = C_Cyan; break;
            }
            p = *q ? q : q - 1;
            continue;
        }
        if (n < sizeof(seg) - 1) seg[n++] = *p;
    }
}

// --- shapes ------------------------------------------------------------------------

static void fill(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(g_ren, &r);
}

static void outline(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderDrawRect(g_ren, &r);
}

static void ring(int cx, int cy, int r, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    for (int i = 0; i < 64; i++) {
        float a0 = i * 6.2832f / 64, a1 = (i + 1) * 6.2832f / 64;
        SDL_RenderDrawLine(g_ren, cx + (int)(cosf(a0) * r), cy + (int)(sinf(a0) * r),
                           cx + (int)(cosf(a1) * r), cy + (int)(sinf(a1) * r));
    }
}

static void sprite(SDL_Texture *t, int x, int y, double angle, float scale, int alpha) {
    if (!t) return;
    int w, h;
    SDL_QueryTexture(t, NULL, NULL, &w, &h);
    SDL_Rect dst = { x, y, (int)(w * scale), (int)(h * scale) };
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_RenderCopyEx(g_ren, t, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
}

// small caps label: letter-spaced in English, as is in Japanese
static int label(int x, int y, SDL_Color c, const char *s) {
    if (g_lang == Lang_Ja) return text(x, y, F_Small, c, 0, s);
    int x0 = x;
    char ch[2] = { 0, 0 };
    for (; *s; s++) {
        ch[0] = *s;
        x += (*s == ' ' ? 6 : text(x, y, F_Label, c, 0, ch)) + 3;
    }
    return x - x0;
}

// "WHAT TO SHOW ———————" across a column block
static void section(int x, int y, int x_end, const char *s) {
    int w = label(x, y, C_Dim, s);
    fill(x + w + 12, y + 9, x_end - (x + w + 12), 1, C_Faint);
}

// [A] -> outlined round button, [↑↓] -> outlined pill
static int glyph(int x, int y, const char *lbl) {
    int tw = text_w(F_Small, lbl);
    if (tw <= 12) {
        ring(x + 11, y + 11, 11, C_Text);
        text(x + 11, y + 2, F_Small, C_Ink, 1, lbl);
        return 22;
    }
    int w = tw + 16;
    outline(x, y, w, 22, C_Text);
    text(x + w / 2, y + 2, F_Small, C_Ink, 1, lbl);
    return w;
}

static void footer(const char *s) {
    fill(64, SCREEN_H - 40, SCREEN_W - 128, 1, C_Faint);
    int x = 64, y = SCREEN_H - 30;
    char buf[64];
    while (*s) {
        if (*s == '[') {
            const char *e = strchr(s, ']');
            if (e && e - s < (int)sizeof(buf)) {
                memcpy(buf, s + 1, e - s - 1);
                buf[e - s - 1] = '\0';
                x += glyph(x, y, buf) + 8;
                s = e + 1;
                continue;
            }
        }
        const char *e = strchr(s, '[');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
        x += text(x, y + 2, F_Small, C_Dim, 0, buf);
        s += n;
    }
}

// --- the scene -----------------------------------------------------------------------

bool ui_frame_begin(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        if (ev.type == SDL_QUIT) return false;
    frame_no++;
    float t = SDL_GetTicks() / 1000.0f;

    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    if (t_bg) SDL_RenderCopy(g_ren, t_bg, NULL, NULL);
    // the torii frames the moon
    sprite(t_torii, 773, 200, 0, 1.0f, 255);
    // a frog sits under the torii and hops a little now and then
    float hop = fmodf(t, 7.0f);
    int fy = hop < 0.45f ? (int)(sinf(hop / 0.45f * 3.14159f) * 10) : 0;
    sprite(t_frog, 940, 546 - fy, 0, 1.0f, 255);

    for (int i = 0; i < PETALS; i++) {
        Petal *p = &petals[i];
        p->y += p->vy;
        p->x += sinf(t * 1.1f + p->phase) * 0.5f - 0.2f;
        p->spin += 0.8f;
        if (p->y > SCREEN_H + 20) reset_petal(p, false);
        sprite(t_petal, (int)p->x, (int)p->y, p->spin, 0.85f, 200);
    }
    if (t_shade) {
        SDL_Rect dst = { 0, 0, 860, SCREEN_H };
        SDL_RenderCopy(g_ren, t_shade, NULL, &dst);
    }
    return true;
}

void ui_frame_end(void) {
    SDL_RenderPresent(g_ren);
}

// --- screens -----------------------------------------------------------------------

#define X0 84          // left edge of the content
#define COL_W 312      // toggle column width
#define COL2 (X0 + COL_W + 46)
#define X_END (COL2 + COL_W)

static void title_block(const char *title, const char *subtitle) {
    fill(64, 46, 4, 46, C_Verm);
    int w = text(X0, 38, F_Title, C_Ink, 0, title);
    if (subtitle) text(X0 + w + 20, 62, F_Small, C_Dim, 0, subtitle);
}

// one row: selection band + vermilion tick, label, optional On/Off on the right
static void row(int x, int y, int w, bool selected, bool dim, const char *lbl, int state /* -1 none, 0 off, 1 on */) {
    if (selected) {
        fill(x - 14, y - 3, w + 22, 28, (SDL_Color){ 255, 255, 255, 18 });
        fill(x - 14, y - 3, 3, 28, C_Verm);
    }
    SDL_Color c = dim ? C_Dim : selected ? C_Ink : C_Text;
    text(x, y, F_Item, c, 0, lbl);
    if (state < 0) return;
    const char *v = tr(state ? T_On : T_Off);
    int vw = text_w(F_Small, v);
    text(x + w, y + 3, F_Small, state && !dim ? C_Ink : C_Dim, 2, v);
    int sx = x + w - vw - 20;
    if (state) fill(sx, y + 7, 10, 10, dim ? C_Dim : C_Verm);
    else       outline(sx, y + 7, 10, 10, C_Dim);
}

void ui_menu(const MenuModel *m) {
    title_block(tr(T_Title), NULL);  // the name already says what it is
    if (m->update_tag)
        textf(X0, 92, F_Small, C_Verm, 0, tr(T_UpdateAvail), m->update_tag);

    // what's going on
    int y = 118;
    const int vx = X0 + 130;
    label(X0, y + 4, C_Dim, tr(T_Discord));
    if (m->logged_in) textf(vx, y, F_Body, C_Text, 0, tr(T_LoggedInAs), m->user && *m->user ? m->user : "you");
    else              text(vx, y, F_Body, C_Warn, 0, tr(T_NotLoggedIn));
    label(X0, y + 30, C_Dim, tr(T_Module));
    text(vx, y + 26, F_Body, m->running ? C_Text : C_Bad, 0, tr(m->running ? T_Running : T_NotRunning));
    label(X0, y + 56, C_Dim, tr(T_Now));
    const char *st = m->status_state ? m->status_state : "";
    char now[256];
    SDL_Color nc = C_Text;
    if (!m->set[Set_Enabled] || !strcmp(st, "off")) { snprintf(now, sizeof(now), "%s", tr(T_TurnedOff)); nc = C_Dim; }
    else if (!strcmp(st, "live")) { snprintf(now, sizeof(now), tr(T_Live), m->status_text); nc = C_Good; }
    else if (!strcmp(st, "login")) { snprintf(now, sizeof(now), "%s", tr(T_NotLoggedIn)); nc = C_Warn; }
    else { snprintf(now, sizeof(now), "%.70s", m->status_text ? m->status_text : ""); if (!strcmp(st, "error")) nc = C_Bad; }
    text(vx, y + 52, F_Body, nc, 0, now);

    // the master switch: a seal stamp
    y = 212;
    bool on = m->set[Set_Enabled];
    if (m->sel == Item_Master) {
        fill(X0 - 14, y - 6, X_END - X0 + 22, 80, (SDL_Color){ 255, 255, 255, 18 });
        fill(X0 - 14, y - 6, 3, 80, C_Verm);
    }
    sprite(on ? t_stamp_on : t_stamp_off, X0, y - 2, -5.0, 0.82f, 255);
    text(X0 + 92, y + 6, F_Head, C_Ink, 0, tr(T_Master));
    text(X0 + 92, y + 40, F_Small, on ? C_Text : C_Dim, 0, tr(on ? T_MasterOnDesc : T_MasterOffDesc));

    // what to show: two columns
    y = 300;
    section(X0, y, X_END, tr(T_WhatToShow));
    for (int i = Item_FirstToggle; i <= Item_LastToggle; i++) {
        int k = i - Item_FirstToggle;
        int x = k < TOGGLE_ROWS ? X0 : COL2;
        row(x, y + 30 + (k % TOGGLE_ROWS) * 30, COL_W, m->sel == i, !on, tr_setting(i), m->set[i]);
    }

    // account & tools
    y = 516;
    section(X0, y, X_END, tr(T_AccountTools));
    for (int i = Item_Login; i < Item_Count; i++) {
        int k = i - Item_Login;
        char lbl[128];
        switch (i) {
        case Item_Login:
            if (m->logged_in) snprintf(lbl, sizeof(lbl), tr(T_Logout), m->user && *m->user ? m->user : "");
            else snprintf(lbl, sizeof(lbl), "%s", tr(T_Login));
            break;
        case Item_Test:  snprintf(lbl, sizeof(lbl), "%s", tr(T_Test)); break;
        case Item_Icons: snprintf(lbl, sizeof(lbl), "%s", tr(T_Icons)); break;
        case Item_Diag:  snprintf(lbl, sizeof(lbl), "%s", tr(T_Diag)); break;
        case Item_Update:
            if (m->update_tag) snprintf(lbl, sizeof(lbl), tr(T_UpdateTo), m->update_tag);
            else snprintf(lbl, sizeof(lbl), "%s", tr(T_Update));
            break;
        default:         snprintf(lbl, sizeof(lbl), "%s", tr(T_Lang)); break;
        }
        row(k % 2 ? COL2 : X0, y + 30 + (k / 2) * 30, COL_W, m->sel == i, false, lbl, -1);
    }

    // a message for a moment after an action, otherwise what the selected item does
    if (m->msg && *m->msg) {
        SDL_Color mc = m->msg_tone == Tone_Good ? C_Good : m->msg_tone == Tone_Warn ? C_Warn
                     : m->msg_tone == Tone_Bad ? C_Bad : C_Ink;
        text(X0, SCREEN_H - 72, F_Small, mc, 0, m->msg);
    } else {
        text(X0, SCREEN_H - 72, F_Small, C_Dim, 0, tr_help(m->sel));
    }
    footer(tr(T_FooterMenu));
}

void ui_lines(const char *title, const char *const *lines, int n, const char *foot) {
    title_block(title, NULL);
    for (int i = 0; i < n; i++)
        ansi_text(X0, 120 + i * 21, F_Small, lines[i]);
    footer(foot);
}

void ui_login(const char *url, const uint8_t *qr, int size, const char *note) {
    title_block(tr(T_LoginTitle), NULL);
    int y = 124;
    text(X0, y, F_Item, C_Ink, 0, tr(T_LoginStep1));
    text(X0 + 22, y + 30, F_Small, C_Dim, 0, tr(T_LoginStep1b));
    text(X0 + 22, y + 58, F_Small, C_Text, 0, tr(T_LoginOr));
    text(X0 + 22, y + 80, F_Head, C_Info, 0, url);
    text(X0, y + 130, F_Item, C_Ink, 0, tr(T_LoginStep2));
    text(X0, y + 166, F_Item, C_Ink, 0, tr(T_LoginStep3));

    // QR code on washi paper with a thin vermilion frame
    int cy = 340;
    if (qr && size > 0) {
        int mod = 280 / (size + 4);
        int card = mod * (size + 4);
        fill(X0 - 4, cy - 4, card + 8, card + 8, C_Verm);
        fill(X0, cy, card, card, (SDL_Color){ 246, 240, 226, 255 });
        for (int yy = 0; yy < size; yy++)
            for (int xx = 0; xx < size; xx++)
                if (qr[yy * size + xx])
                    fill(X0 + (xx + 2) * mod, cy + (yy + 2) * mod, mod, mod, (SDL_Color){ 24, 18, 30, 255 });
        if (note && *note) text(X0 + card + 30, cy + card / 2 - 10, F_Body, C_Warn, 0, note);
    } else if (note && *note) {
        text(X0, cy, F_Body, C_Warn, 0, note);
    }
    footer(tr(T_FooterLogin));
}

void ui_screenshot(const char *path) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

void ui_exit(void) {
    for (int i = 0; i < CACHE_SIZE; i++)
        if (cache[i].tex) SDL_DestroyTexture(cache[i].tex);
    SDL_Texture *ts[] = { t_bg, t_torii, t_frog, t_petal, t_stamp_on, t_stamp_off, t_shade };
    for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); i++)
        if (ts[i]) SDL_DestroyTexture(ts[i]);
    for (int i = 0; i < F_Count; i++)
        if (g_font[i]) TTF_CloseFont(g_font[i]);
    TTF_Quit();
    IMG_Quit();
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
#ifdef __SWITCH__
    plExit();
    romfsExit();
#endif
}
