// Shared settings for the sysmodule and the app.
#pragma once

#define SWITCHRPC_VERSION "1.0.0"

// Program id of the background module (atmosphere/contents/<id>/).
// Same id as llsc12/SwitchRPC, so installing this replaces it instead of
// running two presence modules side by side.
#define SYSMODULE_TID 0x3120000000000092ULL

// Discord application + OAuth2 bounce page. These belong to llsc12/SwitchRPC
// (https://github.com/llsc12/SwitchRPC); a fork with its own Discord app
// (Discord Social SDK enabled, public client) changes them here.
#define DISCORD_CLIENT_ID "1249119754522857616"
#define AUTH_SHIM         "https://static.llsc12.me/discord"
