# Citron for OpenPak

Fork of upstream Citron (Citron Neo) that plays supported Switch titles online on the OpenPak
network: sign in with an OpenPak account and the emulator does the rest — no hosts-file edits,
no external DNS, no certificate bypass. It started life as the Nextendo Network edition of
Citron; that integration is GPL-2.0-or-later like Citron itself, and since 2026-09-10 it is
the same code turned to face OpenPak's servers. The API and account layers live in
`src/web_service/openpak_api.*` and `src/common/openpak_account.*`; the wider UI keeps its
`nextendo_*` file names. Builds are the upstream-style continuous builds
(`.github/workflows/build-{linux,windows,macos,android}.yml`); no `openpak-v*` tag yet.

Surfaces (this is the reference fork of emulators/prds/emulator-integration-prd.md — E0,
from whose code `openpak-client` was extracted):

- **Account** — email and password, sent to openpak.org over public TLS and nowhere else;
  back come an API token and the account's Switch identity: PID, friend code, and the token
  title servers use to know who is playing.
- **Redirection** — Nintendo's online hostnames resolve to the OpenPak server
  (`145.241.199.19` by default, from the Network settings page). The NAT check stays on real
  DNS, which OpenPak does not serve.
- **Certificates** — signing in fetches the OpenPak CA into `config/openpak/ca.pem`; from the
  next launch the redirected names are verified against it. Without the file, verification is
  off and the log says so.
- **Friends** — the OpenPak friend graph, with the presence game servers report. Add by
  friend code, accept or decline.
- **Cloud saves** — pulled on launch and pushed on exit through `/api/v1/me/saves`, versioned:
  a save pushed from a stale base is kept as a conflict, never dropped. Manage them at
  openpak.org/account/saves.
- **Online counts** — players per title, from openpak.org's public status.

What OpenPak has no server for answers "not available" instead of failing: lobby and recent
players, reports, play history, BCAT seeds, chat rooms, editing your name or picture from the
emulator. The chat rooms connect only where `OPENPAK_CHAT_HOST` (and `OPENPAK_CHAT_PORT`)
point. Environment overrides: `OPENPAK_SERVER_IP` (redirect target), `OPENPAK_NAT_IP`,
`OPENPAK_API` (https or loopback only, since it carries the token), `OPENPAK_ENABLE=1` for
the command-line build, `OPENPAK_PHOTON_IP` for titles on Photon.

PRDs: [`../prds/`](../prds/README.md) — emulator-wide PRDs live at `emulators/prds/` in the
workspace.
