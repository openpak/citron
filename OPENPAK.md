# Citron for OpenPak

Fork of upstream Citron (Citron Neo) that plays supported Switch titles online on the OpenPak
network. This build **is the client, not an emulator with a mode** — OpenPak is on by default.
It started life as the Nextendo Network edition of Citron, and the shared client library was
extracted from that integration; since 2026-09-23 Citron consumes the library exactly as Eden
does. The integration is `openpak-client` vendored at `externals/openpak-client` (client half
linked into core, Qt dialogs into the frontend), hosted by `OpenPakHost`
(`src/citron/openpak_host.*`, ported from Eden's). Ryujinx is the reference for the Switch
integration (`emulators/prds/emulator-integration-prd.md` §2a): whatever lands there is ported to
Eden and here, through the library wherever it can be.

Surfaces:

- **Account** — email and password, sent to OpenPak over TLS and nowhere else; back come a
  website token and the account's Switch identity. Signing in also links the console: the acc
  service walks the library's console chain (dauth, BAAS device account, login bound to the
  running title) and hands titles the id_token OpenPak issued.
- **Profiles** — each Citron user profile is its own OpenPak account, one active at a time. A
  plain launch picks the profile (last used, ask, or one profile; the choice gets its UI on
  the spec's Configure → OpenPak page), offers the setup once (sign in, create an account, play offline), then goes online.
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
  (`am/frontend/applet_my_page.*` over `openpak::my_page`), with the library's friend picker
  (`openpak/qt/friend_picker.h`, the same one Eden uses) for "invite friends"; friend:m
  30900/30901 send too.
- **Controller** — the OpenPak window and the friend picker take Citron's controller navigation
  (`OpenPakHost::CreateNavigation` over `util/controller_navigation.*`: D-pad/stick, A, B, L/R
  for pages), and the game's input is suspended while they are up; mouse and keyboard work as
  before.
- **Toasts** — a friend coming online or starting a game, a friend request, a game invitation.
- **Online status** — the list's OpenPak pill, the carousel's badge and the details panel show
  the catalogue's live/beta/alpha (`openpak::compatibility`, refreshed from the site at startup;
  backend in the tooltip) for every title it lists (`src/citron/openpak_online_status.*`). The
  twelve-title version table in the library only flags an installed version the servers refuse.
- **Push** — besides the heartbeat's poll, the library holds the Penne push connection a
  console holds (`openpak/push.h`): a delivered friend request, acceptance, removal, invitation
  or presence change re-reads the list it concerns at once. Downlink only; presence stays on
  the REST PATCH. `OPENPAK_NO_PUSH=1` turns it off.
- **Blocking** — friend:m 30400–30403 block and 30402 unblocks against BAAS
  (`baas::BlockUser`/`UnblockUser`); the block list, friend list and request boxes re-sync
  after the write.
- **BCAT** — with OpenPak on, a title's `RequestSyncDeliveryCache` fills its delivery cache from
  the news service's dataset (`openpak::bcat`, `/api/emulator/v1/bcat/titles/<tid>`,
  sha256-checked, cached for offline launches), through the OpenPak BCAT backend in
  `bcat/service_creator.cpp`.
- **NAT type** — the account window's NAT pill runs the console's own Test Connection exchange
  (`openpak::nat`) against nncs1/nncs2 and shows the letter A–F, mapping and filtering in the
  tooltip.
- **Cloud saves** — pulled before a title boots and pushed when it stops, every title, from the
  active profile's save folder, versioned (the Cloud saves page resolves conflicts).
- **Redirection** — Nintendo's online hostnames resolve to the OpenPak server, following the
  network profile OpenPak publishes (`openpak::NetworkProfile`, fetched at the first sign-in,
  cached; built-in wildcards until one is loaded). The NAT check's second probe goes to
  OpenPak's second responder.
- **Certificates** — guest TLS trusts the OpenPak CA in addition to the system roots and
  verifies as the title asks, by IP SAN when the title names an address. Every CI build uses the
  OpenSSL backend (`ENABLE_OPENSSL=ON`); the Schannel backend, used only by a Windows build
  without OpenSSL, applies the same rules (not yet compiled or run on Windows). The macOS
  SecureTransport backend, likewise only without OpenSSL, still does not verify.
- **The title online path** — nsd, getaddrinfo, the BSD socket layer, nifm and ssl as a title's
  own online stack (NEX, NPLN/gRPC, Photon) needs them; compared with Eden's function by
  function below. Stardew Valley 1.6.15.13 gets the build-scoped patch of its own X509 check
  (`core/loader/nso.cpp`), as in Ryujinx and Eden.

Settings: `enable_openpak`, `openpak_server_ip`, `openpak_nat_ip`. Environment overrides:
`OPENPAK_SERVER_IP`, `OPENPAK_NAT_IP`, `OPENPAK_API` (https or loopback only), `OPENPAK_ENABLE`,
`OPENPAK_NO_CERT=1`, `OPENPAK_CHAT_HOST`/`OPENPAK_CHAT_PORT`, `OPENPAK_PHOTON_IP`; tracing:
`CITRON_SSL_TRACE=1` (guest TLS in the clear), `SSLKEYLOGFILE`; and the research redirects and
probes (`NEXTENDO_*`) in `sfdnsres.cpp` and `bsd.cpp`, all off unless set.

Menu: the top-level **OpenPak** menu (and the top bar's OpenPak button, the same menu) as the
UX spec has it (`emulators/prds/openpak-ux-spec.md` §3.1, built by `OpenPakHost::PopulateMenu`,
identical in Eden): *Sign in to OpenPak...* or *Signed in as {name}*, Friends, Invitations,
Cloud saves, Mods, News, Status, *OpenPak settings...*, *OpenPak website*, *Sign out...* (with
the §3.5 confirmation). Population, the Chat Rooms prototype, the redirect toggle and the
startup submenu are no longer in it; the startup choice and redirect move to the spec's
Configure → OpenPak page (C2, not done yet), and the chat overlay is still reachable from the
account window's invite-to-chat and a chat-invite toast. The *Toggle OpenPak account* hotkey
still opens the window.

## Android

Citron's Android (`src/android`) carries Eden's Android OpenPak layer, package names aside: native
Material screens as `emulators/prds/openpak-ux-spec.md` §4 has them, in Kotlin `utils/OpenPak.kt`,
`utils/OpenPakUi.kt` and `fragments/OpenPakFragment.kt`, over one JSON bridge
`jni/openpak_native.cpp`. It adds profile management to the OpenPak settings (Citron's Android has
no profile screen of its own). Cloud saves are pulled in `InitializeEmulation` and pushed in
`ShutdownEmulation` (`jni/native.cpp`); the strings are `res/values/openpak_strings.xml`. Every
request says which build asks: `X-OpenPak-Client: citron/<version>+<hash>`.

## Builds and releases

Continuous builds from `main`: `.github/workflows/build-linux.yml` builds this tree (x86_64,
x86_64-v3 on the self-hosted runner; aarch64 under qemu only when the dispatch sets
`aarch64: true`, because it holds a runner for hours) and attaches the AppImages to
the `nightly-linux` release without artifacts; the release step runs for whichever legs made it.
`build-windows.yml` and `build-macos.yml` publish their own nightlies the same way (GitHub-hosted
runners). `build-android.yml` (dispatched by hand) builds the standard and Snapdragon 8 Elite
APKs and attaches them to `nightly-android` the same way. No `openpak-v*` tag yet. Local desktop build: `build-openpak/` (system libraries,
nlohmann_json in `build-openpak/deps`).

PRDs: [`../prds/`](../prds/README.md) — emulator-wide PRDs live at `emulators/prds/` in the
workspace.

## Online path: parity with Eden

Citron's online path started as the Nextendo work (comments tagged `[Nextendo]`); Eden's was
written later from the same Ryujinx references (its commits of 2026-09-13 to 09-17). Compared
function by function on 2026-09-23. "Ported" means Eden's behaviour is now in Citron; "kept"
means Citron's own version stays because it is equivalent or does more.

| Area / function | Eden has | Citron had | Decision |
| --- | --- | --- | --- |
| nsd `Resolve`/`ResolveEx` | `NsdResolve`: every `%` -> `lp1`, accounts -> BAAS names, a few names passed through | first `%` only; accounts rewrite (plus `api.accounts` / `-sb-api` aliases) | ported (`NsdResolve`, every `%`), Citron's extra aliases kept |
| sfdnsres `use_nsd_resolve` | routed through `NsdResolve` | own partial copy of the substitution, flag logged as "ignored" | ported: `NsdResolve` when the flag is set or the name has a `%` |
| getaddrinfo `ai_addrlen` / `sin_len` | 16 | `sizeof(SockAddrIn)`, which is 16 in Citron | already equivalent |
| getaddrinfo port byte order | `u16_le` (Eden reverted its `u16_be` experiment) | `u16_le` | already equivalent |
| getaddrinfo any-socktype answers | one entry per address, type/protocol "any", with OpenPak on | host's entries as-is; literal IPs answered STREAM/TCP | ported (both paths) |
| canonical name on a redirect | the name asked for | the same, plus literal IPs answered without a lookup | kept Citron's (does more) |
| NPLN hold | 3 s on the first npln getaddrinfo | 0 ms unless `NEXTENDO_NPLN_DELAY_MS` | ported: 3 s default, env override kept; deadline-watch diagnostic only with the override |
| redirect table / network profile | `openpak::NetworkProfile` then built-in list | the same, plus opt-in research redirects (Photon, EOS, Demonware, ...) | kept Citron's |
| BSD `Poll` deferral (eventfd in set) | snapshot, one non-blocking pass, `SetIsDeferred`, eventfd write signals | the same machinery | already equivalent; ported the one gap: an expired deferred poll writes its pollfd array back |
| deferral wake-ups | 10 ms recheck thread while deferrals are pending | 1 ms heartbeat (Ryujinx's measured interval) | kept Citron's (lower latency) |
| `PollImpl` zero-mask entries | eventfd zero mask reads as In; any other zero-mask socket polled for In/Out | eventfd zero mask reads as In | kept Citron's: answering Out for a connected socket nobody asked about makes every poll return at once |
| `PollImpl` infinite wait | 250 ms slices answered `ETIMEDOUT` | waits as asked | kept Citron's: gRPC waits always carry an eventfd and take the deferred path; the slice changes every other title's poll |
| `PollImpl` invalid fd | returns early with `{0, SUCCESS}` | per-entry `POLLNVAL`, rest still evaluated | kept Citron's (correct) |
| `PollImpl` TLS plaintext pending | no | reports readable when the SSL layer holds plaintext | kept Citron's |
| eventfd | atomic counter; reads flags then initval | loopback datagram self-pipe, `Read` drains and sums; reads initval then flags | kept Citron's (Ryujinx's argument order, no counter/host drift); ported non-blocking `EventFdFlags` (4) |
| `Read` | plain recv | eventfd-aware drain | kept Citron's |
| connect to the OpenPak server | synchronous completion, 2 s, OpenPak server only | synchronous completion for every connect, 5 s (`Socket::Connect`) | kept Citron's (covers more) |
| IPv6 (guest domain 28) | dual-mode sockets, v4-mapped connect/bind | socket created, bind/connect refused `EAFNOSUPPORT` | ported: dual-mode sockets, v4-mapped (and `::`, `::1`) in bind/connect/sendto, peers answered in IPv4 shape |
| `getsockopt` readback | echoes every tolerated/unknown option, SO_TYPE, timeouts, buffers | NODELAY, REUSEADDR, KEEPALIVE, BROADCAST, LINGER, `0x80000001`; everything else `INVAL` | ported (`feigned_sockopts`, SO_TYPE) |
| `setsockopt` failure log | names level and option | generic | ported |
| `sendmmsg`/`recvmmsg` | one send/recv spread over the messages; -1 on error | per-message send/recv, SNI injection per record; 0 with errno on error | kept Citron's shape, ported the results (-1 when nothing moved), later messages non-blocking, short stream read ends the batch |
| ICMP error on a UDP socket | discarded, next datagram read | the same | already equivalent |
| getifaddrs (`Sysctl`) | yes | ported earlier (743f08021) | already equivalent |
| nifm `SetExclusiveClient` | accepted | unimplemented | ported |
| nifm `IsAnyInternetRequestAccepted` / `IsAnyForegroundRequestAccepted` | host network and not airplane mode | always yes / host network only | ported |
| ssl `DoNotCloseSocket` | no duplicate, answers -1 | duplicated the fd, closed it with the connection (closing the title's shared socket) | ported |
| ssl `Pending` | `SSL_pending` | backend `Pending` plus the bsd poll registry | kept Citron's (does more) |
| ssl ALPN offered | the title's list whenever supplied | the title's h2/http/1.1 list for NPLN hosts, http/1.1 for the rest (NEX's WebSocket upgrade) | kept Citron's |
| ssl ALPN read back (`GetNextAlpnProto`) | negotiated protocol | stub, always NoSupport | ported (state Negotiated + name) |
| ssl verification (OpenSSL) | OpenPak CA + host roots, verify option honoured, IP SAN for IP literals | ported earlier (bb28e2a35) | already equivalent |
| ssl verification (Schannel) | not built (Eden compiles OpenSSL everywhere) | never verified; no longer compiled | fixed: same rules as OpenSSL (see the Schannel commit); unverified on Windows |
| `EDEN_SSL_TRACE` | handshake results and clear-text reads/writes | debug-level hex of reads/writes only | ported as `CITRON_SSL_TRACE=1` |
| NPLN worker freeze tracing (`physical_core.cpp`) | svc-0x1C pollfd/cached-mask dump | Citron's own deadline watch and `[DIAG]` logs | not ported: a one-off investigation, not part of the path |

Citron's own research hooks (`NEXTENDO_*` redirects and ports, the Stardew TLS probe, SNI
injection, the post-handshake recv grace wait, UDP socket parking, the deferred TCP close) have
no Eden counterpart and are unchanged.
