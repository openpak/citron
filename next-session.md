# Next session — citron

Updated 2026-09-15.

Upstream Citron Neo (Switch, yuzu family) turned to face OpenPak: sign-in, identity, friends,
cloud saves, redirection, CA pinning, online counts. The reference fork of the integration
PRD and the code `openpak-client` was extracted from. No `openpak-v*` tag yet — releases are
the upstream-style continuous builds.

## Where things stand

- Account layer is OpenPak's since cd63ffcf2 (2026-09-10); the not-available list and env
  overrides are documented in README.md and OPENPAK.md.
- `openpak_api.h` declares SignIn and FetchCA (487b71ff8); the controller, chat and probe
  layers carry `OPENPAK_*` envs (`OPENPAK_API`, `OPENPAK_SERVER_IP`, `OPENPAK_NAT_IP`,
  `OPENPAK_CHAT_HOST`/`OPENPAK_CHAT_PORT`).
- Nextendo-era online work kept working: Photon tracing/redirect, guest-call join injection
  recovery, friend-data forwarding fixes.
- Title version pins updated (MK8D 4.0.0, Smash 13.0.5, SMB35 1.0.2).

## Next steps

- Decide the release story: an `openpak_release.yml` and a first `openpak-v*` tag, or stay on
  continuous builds — the PRD says every fork releases on `openpak-v*`.
- E0 refactor: move onto the shared `openpak-client` without behaviour change.
- Chat rooms: point `OPENPAK_CHAT_HOST` at a real chat service when one exists.

## Pointers

- [`../prds/`](../prds/README.md) — emulator-wide PRDs (`emulators/prds/` in the workspace):
  emulator-integration-prd.md (E0), emulator-network-profile-prd.md.
- `OPENPAK.md`, `README.md` — this fork's readmes.
