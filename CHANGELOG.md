# Changelog

## [1.1.2] - 2026-10-28

### Fixed
- **The eShop button never appeared** where Nintendo's eShop search is blocked: a failed lookup was saved as "this game has no store page". The module now falls back to Nintendo's short store link (`ec.nintendo.com/apps/<id>/GB`), which also tells listed games apart. Store pages are looked up again once, since the old answers can't be trusted.
- Note: Discord may not show the buttons of your **own** status to you, and you can't click them yourself. Check from another account.

## [1.1.1] - 2026-10-28

### Fixed
- **Prepare icons** stopped at "couldn't reach the eShop" and never made an icon on networks that block Nintendo's eShop search. It now goes on to tinfoil and the title's own icon, like 1.0.0 did.
- Long lines in the app were cut off instead of wrapped, and a Japanese (or other non-English) name could be cut in the middle of a character. They now wrap, and names are shortened cleanly.

## [1.1.0] - 2026-10-27

### New
- **Tesla overlay.** Every setting is now in the overlay menu too (`switch/.overlays/switch-discord-activity.ovl`), with the login and the current status at the top. Needs nx-ovlloader and Tesla Menu or Ultrahand. Changes made in the app and in the overlay show up in each other.
- **Updates from the app.** The menu says when a newer release is out, and *Check for updates* downloads it from GitHub, checks it against `SHA256SUMS`, replaces the module, the app and the overlay, and restarts the module. Nothing is replaced unless every file checks out.
- **Battery saver** (off by default). Sleep clears the status instead of showing *paused*, and the module does nothing at all until the console wakes up (no polling, no SD card writes, even when the console wakes up in the background for downloads). On battery it checks every 5 s instead of every 2 s.
- **Prepare icons only handles new titles.** Titles done before (and titles with no icon of their own) are skipped without going online, so a second run takes seconds. Titles that failed are tried again next time.

### Fixed
- The module no longer waits for the network in a loop that blocked everything else: at boot, after sleep and before each post. Without internet it says so in the app and posts as soon as the connection is back.
- Opening the Homebrew Menu over a running game and going back no longer restarts the game's timer.
- Stale sessions that couldn't be deleted (no internet, Discord error) were forgotten and stayed on Discord. They are kept and deleted later.
- Login and session expiry checks used the console clock instead of the corrected one, so a wrong clock could make a fresh token look expired (or the other way round).
- A network hiccup while preparing icons made the app upload the title's own icon instead of using the eShop one.
- Left/right in the bottom part of the menu jumped between rows.

## [1.0.0] - 2026-10-21

First release. Based on [llsc12/SwitchRPC](https://github.com/llsc12/SwitchRPC).

### Discord status
- Works where `discord.com` is blocked: all requests go to `gaming-sdk.com`.
- `on Nintendo Switch`, optionally `· Docked` / `· Handheld` and `· N h total`.
- The game is shown as **paused** while it's in the background (HOME Menu, eShop, user page, Mii Maker, web browser, sleep mode); the timer stops and resumes where it left off.
- Without a game: `HOME Menu`, `Nintendo eShop`, `Album`, `User Page`... (optional).
- Homebrew from the Homebrew Menu shows as `Homebrew`. The real album is told apart from hbmenu.
- Forwarders and other titles the eShop doesn't know get their own icon, uploaded once (freeimage.host, catbox.moe as a fallback); the eShop and tinfoil are tried first.
- Optional *Nintendo eShop* button with the game's store page.
- The timer starts right away even with a wrong console clock.
- New game shown within ~2 s; the icon follows if it had to be looked up.
- Status cleared on power off / reboot; kept as paused during sleep.
- Failed posts are retried every 30 s; an expired access token is refreshed on 401.
- Tokens are never written to the log.

### App
- Graphical menu (SDL2) in English and Japanese: torii in front of the moon, a frog, sakura petals.
- One-button on/off (−), and switches for each part of the status: pause, system screens, elapsed time, icon, game name, dock mode, total playtime, time while paused, eShop button, "(homebrew)".
- Login with a QR code on screen.
- Diagnostics page, step-by-step Discord test, icon preparation.
- Removes the old PC-based "Switch Discord RPC" module (`420000000000D15C`) and the old `switch/switch-rpc.nro` if present.
