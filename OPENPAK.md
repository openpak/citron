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
- **Presence** — the library's heartbeat keeps the account online, publishes what is running,
  keeps the guest friend cache warm and polls the invitation inbox; closing the window says
  goodbye (three seconds at most).
- **Invitations** — an invitation to the running game asks Join or Ignore; Join leaves it in
  the game's friend invitation channel (`AppletManager::PushFriendInvitation`).
- **Cloud saves** — pulled before a title boots and pushed when it stops, every title, from the
  active profile's save folder, versioned (the Cloud saves page resolves conflicts).
- **Redirection** — Nintendo's online hostnames resolve to the OpenPak server, following the
  network profile OpenPak publishes (built-in wildcards until one is loaded). The NAT check's
  second probe goes to OpenPak's second responder.
- **Certificates** — guest TLS trusts the OpenPak CA and verifies as the title asks (OpenSSL
  backend; the Windows Schannel backend does not verify).

The friend service (`friend:*`) is still Citron's own, fed by the library's friend cache; Eden's
port of the Ryujinx friends module comes later. Environment overrides: `OPENPAK_SERVER_IP`,
`OPENPAK_NAT_IP`, `OPENPAK_API` (https or loopback only), `OPENPAK_ENABLE`, `OPENPAK_NO_CERT=1`,
`OPENPAK_CHAT_HOST`/`OPENPAK_CHAT_PORT`, and the research redirects in `sfdnsres.cpp`. The
Android frontend has no OpenPak code.

PRDs: [`../prds/`](../prds/README.md) — emulator-wide PRDs live at `emulators/prds/` in the
workspace.
