// Renders the app's screens on a PC (SDL offscreen) to check the layout. From app/:
//   cc tools/preview.c source/ui.c source/qrcodegen.c -Isource $(sdl2-config --cflags --libs) -lSDL2_ttf -lSDL2_image -lm -o /tmp/preview
//   SDL_VIDEODRIVER=offscreen /tmp/preview out_dir
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "qrcodegen.h"

static void shot(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
    ui_screenshot(path);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    if (!ui_init(Lang_En)) { fprintf(stderr, "ui_init failed\n"); return 1; }

    MenuModel m = {0};
    m.logged_in = true; m.user = "saekaze"; m.running = true;
    m.status_state = "live"; m.status_text = "Mario Kart 8 Deluxe";
    for (int i = 0; i < Set_Count; i++) m.set[i] = true;
    m.set[Set_HbTag] = false; m.set[Set_PausedTime] = false;
    m.sel = Set_Dock;
    m.msg = NULL; m.msg_tone = Tone_Normal;

    for (int f = 0; f < 90; f++) { ui_frame_begin(); ui_menu(&m); ui_frame_end(); }
    shot(dir, "menu_en");

    ui_set_lang(Lang_Ja);
    m.sel = Item_Lang; m.msg = NULL;
    ui_frame_begin(); ui_menu(&m); ui_frame_end();
    shot(dir, "menu_ja");

    m.set[Set_Enabled] = false; m.sel = Item_Master; m.msg = tr(T_MsgOff); m.msg_tone = Tone_Warn;
    ui_frame_begin(); ui_menu(&m); ui_frame_end();
    shot(dir, "menu_off_ja");

    uint8_t q[qrcodegen_BUFFER_LEN_FOR_VERSION(5)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
    qrcodegen_encodeText("http://192.168.1.42:45601", tmp, q, qrcodegen_Ecc_LOW, 1, 5, qrcodegen_Mask_AUTO, true);
    int n = qrcodegen_getSize(q);
    uint8_t grid[64 * 64];
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) grid[y * n + x] = qrcodegen_getModule(q, x, y);
    ui_frame_begin(); ui_login("http://192.168.1.42:45601", grid, n, NULL); ui_frame_end();
    shot(dir, "login_ja");

    ui_set_lang(Lang_En);
    const char *lines[] = {
        "1. Refreshing the Discord login...",
        "\x1b[32m   OK (HTTP 200), scopes: openid sdk.social_layer_presence\x1b[0m",
        "2. Checking the account...",
        "\x1b[32m   OK, logged in as saekaze\x1b[0m",
        "3. Creating a test status \"Switch Discord Activity test\"...",
        "\x1b[31m   FAILED: no connection (Timeout was reached)\x1b[0m",
    };
    ui_frame_begin(); ui_lines(tr(T_TestTitle), lines, 6, tr(T_FooterBack)); ui_frame_end();
    shot(dir, "test_en");
    ui_exit();
    return 0;
}
