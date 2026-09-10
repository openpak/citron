# Citron — OpenPak edition

A fork of [Citron Neo](https://github.com/citron-neo/emulator) that plays supported Switch titles
online on the [OpenPak](https://openpak.org) network: sign in with your OpenPak account and the
emulator does the rest. No hosts-file edits, no external DNS, no certificate bypass.

It started life as the Nextendo Network edition of Citron. That integration is GPL like Citron
itself; this is the same code turned to face OpenPak's servers.

> [!WARNING]
> **This is a work in progress. Expect bugs.** If you hit a problem, open an issue with your
> `citron_log.txt` (Linux: `~/.local/share/citron/log/citron_log.txt`), the exact error code
> the game showed, the game and its version, and what you were doing. For network problems,
> set the log filter to `*:Info Service:Debug Service.SSL:Debug WebService:Debug` first.

## How it works

| | |
| --- | --- |
| Account | Email and password, sent to openpak.org over public TLS and nowhere else. What comes back is an API token and the account's Switch identity: PID, friend code, and the token title servers use to know who is playing. |
| Redirection | Nintendo's online hostnames resolve to the OpenPak server (`145.241.199.19` by default, from the Network settings page). |
| Certificates | Signing in fetches the OpenPak CA into `config/openpak/ca.pem`; from the next launch the redirected names are verified against it. Without the file, verification is off and the log says so. |
| Friends | The OpenPak friend graph, with the presence game servers report. Add by friend code, accept or decline. |
| Cloud saves | Pulled on launch and pushed on exit through `/api/v1/me/saves`, versioned: a save pushed from a stale base is kept as a conflict, never dropped. Manage them at openpak.org/account/saves. |
| Online counts | Players per title, from openpak.org's public status. |

Not on OpenPak (yet), so the corresponding buttons answer "not available": lobby and recent
players, reports, play history, BCAT seeds, chat rooms, editing your name or picture from the
emulator. Use the website for those that exist there.

## Setup

1. Build as you would upstream Citron (see `docs/`), or use a release build.
2. In the **OpenPak** menu, enable **Network Redirection**. The server address is already set.
3. In the same menu, choose **Sign In** and enter your OpenPak email and password.
4. Launch a supported game and enter its online mode.

Environment overrides: `OPENPAK_SERVER_IP` (the redirect target), `OPENPAK_API` (the account
API, https or loopback only, since it carries your token), `OPENPAK_ENABLE=1` for the command
line build, `OPENPAK_PHOTON_IP` for titles on Photon.

## Building

The upstream instructions apply. The GitHub workflows under `.github/workflows` build Linux
(x86_64, x86_64-v3, aarch64 AppImages), Windows, macOS and Android on dispatch.

## Licence

GPL-2.0-or-later, like Citron.
