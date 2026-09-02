// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// [Nextendo] Manufactured guest-call injection, built specifically so Outbound
// (title 0100ED9024EB8000) can be driven into its own real, working join-by-code
// path when a Nextendo friend invitation arrives.
//
// Background (see outbound-nextendo/HANDOFF.md and /home/tobagin/outbound-re/NOTES.md):
// Outbound's Switch build cannot consume a friend invitation through its own code.
// CheckForInvite() hands the correctly-delivered room-code bytes to InviteData as
// CustomData, but builds JoinSessionRequest from PlatformSessionID -- a hardcoded
// empty string -- and NetworkSystemSwitch.JoinSession itself is a confirmed no-op
// (it never reads its argument and never connects). The only complete, working join
// path in the binary is the same one a manual code-entry UI uses:
//   NetworkManager.Join(code, false)  -> returns a <Join>d__86 coroutine state machine
//   MonoBehaviour.StartCoroutine(that machine) -> Fusion connect to room `code`
// This module manufactures exactly that two-call sequence on the guest's Unity main
// thread, using only the game's own compiled code with its exact ABI -- no hand
// assembled trampolines (a previous hand-assembled trampoline attempt crashed and is
// documented as disabled in arm_dynarmic_64.cpp).
//
// Flow:
//  1. ARM  -- IApplicationFunctions::TryPopFromFriendInvitationStorageChannel (the
//             invite mailbox) calls ArmInviteJoin() when a real invitation with a
//             syntactically valid room code is delivered. This validates the game
//             binary's RVAs and the captured NetworkManager.Instance, then parks the
//             request against the calling (Unity main) thread.
//  2. FIRE -- PhysicalCore::RunThread calls OnBeforeRun() just before resuming a
//             guest thread. For the armed thread it saves the full CPU context and
//             rewrites it to call NetworkManager.Join(code, false) with LR=0 as the
//             sentinel return address, and a private scratch window carved out of
//             the stack space below the thread's live stack.
//  3. CHAIN/RESTORE -- Join returns into LR=0, which raises a NoExecuteFault.
//             PhysicalCore::RunThread sees the PrefetchAbort halt and calls OnFault();
//             the first fault chains StartCoroutine(this, x0), the second restores the
//             saved context and the thread continues exactly where the svc left off.

#pragma once

#include "common/common_types.h"

namespace Kernel {
class KProcess;
class KThread;
} // namespace Kernel

namespace Core {
class ArmInterface;
} // namespace Core

namespace Core::NextendoGuestCall {

// Called from IApplicationFunctions::TryPopFromFriendInvitationStorageChannel after a
// real invitation was popped, with the room code parsed out of the application
// parameter. No-ops (with a log) for any other title, if the binary RVAs don't match,
// or if NetworkManager.Instance hasn't been captured yet.
//
// NOTE: this only RECORDS the invitation (code + requesting thread). It does not arm
// anything: the injection must fire only when the USER accepts the invite (toast
// click) from a sane in-game state -- auto-firing on delivery joins from whatever
// screen the joiner happens to be on (title screen = guaranteed guest crash, observed
// live). See ArmPendingInvite.
void RecordInvite(Kernel::KThread& thread, Kernel::KProcess& process,
                  std::string_view room_code);

// Called from the toast click (user accepting the invite) while Outbound is running.
// Arms the recorded invitation against the recorded requesting thread. Returns false
// (with a log) when nothing was recorded or the recorded state is stale/invalid.
bool ArmPendingInvite(Kernel::KProcess& process);

// Called from PhysicalCore::RunThread immediately before running `thread`. If an
// injected call is armed against this thread, rewrites the CPU context to begin it.
void OnBeforeRun(Kernel::KThread& thread, ArmInterface& arm, Kernel::KProcess& process);

// Called from PhysicalCore::RunThread when a prefetch or data abort halt was observed.
// Returns true if the abort was our injected call's sentinel return and has been
// handled (either chained into the next call or restored to the original context).
bool OnFault(Kernel::KThread& thread, ArmInterface& arm, Kernel::KProcess& process,
             bool data_abort);

// Called from the dynarmic MemoryReadCode hook with the address about to be translated
// and x0's live value. Captures NetworkManager.Instance (x0 at the entry of any of its
// instance methods) the first time the JIT translates one of that class's known
// methods. Works on every module load base (the previous hardcoded-VA capture was
// tied to one specific ASLR base and silently missed every other launch).
void ObserveInstanceCapture(u64 vaddr, Kernel::KProcess& process, u64 x0);

// True while an injected call is running and the CPU is expected to return into our
// LR=0 sentinel. Used to keep the dynarmic NoExecuteFault path quiet for the
// intentional sentinel faults.
bool SentinelPending();

} // namespace Core::NextendoGuestCall
