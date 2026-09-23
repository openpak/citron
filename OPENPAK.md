# Citron for OpenPak

Fork of upstream Citron (Citron Neo) that plays supported Switch titles online on the OpenPak
network. This build **is the client, not an emulator with a mode** — OpenPak is on by default.
It started life as the Nextendo Network edition of Citron, and the shared client library was
extracted from that integration; since 2026-09-23 Citron consumes the library exactly as Eden
does. The integration is `openpak-client` vendored at `externals/openpak-client` (client half
linked into core, Qt dialogs into the frontend), hosted by `OpenPakHost`
(`src/citron/openpak_host.*`, ported from Eden's). Builds are the upstream-style continuous
builds (`.github/workflows/build-{linux,windows,macos,android}.yml`); no `openpak-v*` tag yet.

Surfaces:

- **Account** — email and password, sent to OpenPak over TLS and nowhere else; back come a
  website token and the account's Switch identity. Signing in also links the console: the acc
  service walks the library's console chain (dauth, BAAS device account, login bound to the
  running title) and hands titles the id_token OpenPak issued.
- **Profiles** — each Citron user profile is its own OpenPak account, one active at a time. A
  plain launch picks the profile (*OpenPak → OpenPak account at startup*: last used, ask, or one
  profile), offers the setup once (sign in, create an account, play offline), then goes online.
  `acc` answers only the active profile; deleting a profile forgets its account.
- **Presence** — the library's heartbeat keeps the account online, keeps the friends caches warm
  and polls the invitation inbox; closing the window says goodbye (three seconds at most). What
  it publishes is what the title declares: core tells the library which title runs and its NACP
  presence group (`Service::Friend::OpenPak::ApplicationStarted`/`Stopped`), and friend:u's
  10600/10601/10610 decide ONLINE or PLAYING and the appField.
- **Friends** — `friend:*` is Eden's port of the Ryujinx friends module
  (`src/core/hle/service/friend/openpak_friends.*`; `friend.cpp` is upstream's stubs plus two
  hook lines). Reads come from the library's BAAS caches only, writes go out on its worker, and
  the notification queue is signalled from a thread owned by the emulation session. Only the
  active profile, signed in, gets OpenPak's data.
- **Invitations** — an invitation to the running game asks Join or Ignore; Join leaves it in
  the game's friend invitation channel (`AppletManager::PushFriendInvitation`). A game sends
  them through MyPage, which with OpenPak on is always OpenPak's frontend applet
  (`am/frontend/applet_my_page.*` over `openpak::my_page`), with the Qt friend picker
  (`src/citron/openpak_friend_picker.*`) for "invite friends"; friend:m 30900/30901 send too.
- **Toasts** — a friend coming online or starting a game, a friend request, a game invitation.
- **Online status** — the list's OpenPak pill, the carousel's badge and the details panel show
  the catalogue's live/beta/alpha (`openpak::compatibility`, refreshed from the site at startup;
  backend in the tooltip) for every title it lists (`src/citron/openpak_online_status.*`). The
  twelve-title version table in the library only flags an installed version the servers refuse.
- **Cloud saves** — pulled before a title boots and pushed when it stops, every title, from the
  active profile's save folder, versioned (the Cloud saves page resolves conflicts).
- **Redirection** — Nintendo's online hostnames resolve to the OpenPak server, following the
  network profile OpenPak publishes (built-in wildcards until one is loaded). The NAT check's
  second probe goes to OpenPak's second responder.
- **Certificates** — guest TLS trusts the OpenPak CA and verifies as the title asks (OpenSSL
  backend; the Windows Schannel backend does not verify).

The library is pinned at 3a4d5d4 (the seven-page account window). Environment overrides: `OPENPAK_SERVER_IP`,
`OPENPAK_NAT_IP`, `OPENPAK_API` (https or loopback only), `OPENPAK_ENABLE`, `OPENPAK_NO_CERT=1`,
`OPENPAK_CHAT_HOST`/`OPENPAK_CHAT_PORT`, and the research redirects in `sfdnsres.cpp`. The
Android frontend has no OpenPak code.

PRDs: [`../prds/`](../prds/README.md) — emulator-wide PRDs live at `emulators/prds/` in the
workspace.
