// Self-update from the latest GitHub release.
#pragma once
#include <stdbool.h>

typedef struct {
    volatile bool done;       // the background check finished
    volatile bool available;  // a newer release exists
    char latest[32];          // its tag, e.g. "v1.1.0"
    char err[160];            // why the check failed ("" = fine)
} UpdateInfo;

// Starts checking GitHub in the background (call once, after curl_global_init).
void update_check_async(void);
const UpdateInfo *update_info(void);

// Where this .nro was started from (argv[0]); the update replaces that file.
// Default: sdmc:/switch/switch-discord-activity.nro
void update_set_nro_path(const char *path);

// Waits for the background check (call before curl_global_cleanup).
void update_exit(void);

// Checks again in the foreground (when the user asks).
void update_check_now(void);

// Line callback for progress: like printf, colour = "\x1b[3Xm" or "".
typedef void (*UpdateLine)(void *ctx, const char *color, const char *fmt, ...);

// Downloads the release files, verifies them against SHA256SUMS and replaces
// the installed ones, then restarts the background module.
// Returns true when everything was installed.
bool update_install(UpdateLine line, void *ctx);
