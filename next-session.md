# Next session — citron

Updated 2026-09-24.

Citron (yuzu family, Qt) with the OpenPak layer on the shared `openpak-client` C++ library
(moved onto it 2026-09-23); Citron keeps only its hook lines. OPENPAK.md is the fork readme.

Current status 2026-09-24: latest tag `v0.1.6` (v0.1.0 on 09-23, v0.1.2–v0.1.6 on 09-24).
Since v0.1.0: the UX-spec window, dialogs, settings page, toasts and hotkey; the signed redirect
ceiling (openpak-client e180a57); the CTR Demonware key IPS; pinned-NPLN instruction changes
(SMB Wonder 1.2.1); `LoadIdTokenCacheDeprecated` hands titles the OpenPak id_token; Windows
and macOS build again.

## Where things stand

- Full Switch parity with Ryujinx via `openpak-client` as of 2026-09-23
  (`../prds/emulator-integration-prd.md` §2a): sign-in, profiles, presence, friends, requests,
  blocks, invitations send/receive, cloud saves, OpenPak window and menu, compatibility list,
  network profile, title online path, guest TLS with OpenSSL.
- Releases: CI only on `v*.*.*` tags (+ manual dispatch), GitHub-hosted runners; `release.yml`
  creates the release and the Linux AppImages (x86_64, v3, aarch64), Android APK, Windows zip
  (clang-cl) and macOS dmg attach to it.

## Next steps

- Verify on Windows and macOS: the builds compile, nothing in git shows a run-through.
- TLS: Schannel verifies like OpenSSL but is unverified on Windows; SecureTransport (macOS
  without OpenSSL) does not verify.
- Follow Ryujinx: every Ryujinx change is ported here through `openpak-client` (§2a).

## Scratch (research and throwaway work)

Decompiles, Ghidra projects, dumps, exefs/romfs extracts, packet captures,
strace and emulator logs, probe harnesses: put them in
`~/REPOS/Openpak/scratch/<topic>`. That folder is a local mount of the media pool,
outside every repository, so nothing in it is committed. Never use `/tmp` (a
shared 15 GB RAM disk) or elsewhere on `/home` for this. Keys and signing
material never go there. Rule: `docs/playbooks/conventions.md` in the workspace.
