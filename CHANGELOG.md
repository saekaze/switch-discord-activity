# Changelog

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
