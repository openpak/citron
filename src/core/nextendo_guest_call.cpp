// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/nextendo_guest_call.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/typed_address.h"
#include "core/arm/arm_interface.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"

namespace Core::NextendoGuestCall {

namespace {

constexpr u64 OutboundTitleId = 0x0100ED9024EB8000ULL;

// All RVAs below are from the Il2CppDumper dump of Outbound 0.6.0
// (/home/tobagin/outbound-re/, dump.cs), each verified by disassembling the
// decompressed .text with capstone. Runtime address = module base + RVA, where the
// module base is the process entry point address (what the loader logs as
// "loaded module main @ ..."). Nothing here may be hardcoded to a specific base --
// a previous attempt did that and silently missed every differently-ASLR'd launch.

// NetworkManager.Join(string code, bool newWaitForRoomToBecomeAvailable).
// Verified to only construct the <Join>d__86 coroutine state machine
// (<>4__this at +0x20, code at +0x30, the bool at +0x28) and return it -- it does
// NOT start the coroutine and does NOT read x3 (MethodInfo).
constexpr u64 JoinRva = 0x1BF76C0;
constexpr u32 JoinFirstWord = 0xa9bc7bfd; // stp x29, x30, [sp, #-0x40]!
// MonoBehaviour.StartCoroutine's managed entry, the exact tail-call StartJoinOnLoad
// uses. x0 = this (the NetworkManager MonoBehaviour), x1 = the IEnumerator; x2/x3
// unused. Null-checks both arguments.
constexpr u64 StartCoroutineRva = 0x4C1A070;
constexpr u32 StartCoroutineFirstWord = 0xa9bd7bfd; // stp x29, x30, [sp, #-0x30]!

// [base + this slot] -> +0x90 = System.String's Il2CppClass*, the same twice-live-
// confirmed chain used to read the cached empty PlatformSessionID string
// ([table+0x668] -> +0x90 -> +0xb8 -> string.Empty). Used to fabricate the room-code
// string argument with the game's own string klass.
constexpr u64 StringKlassChainSlotRva = 0x6A12668;

// NetworkManager instance methods proven to run on the joiner early (PersistentSingleton
// MonoBehaviour lifecycle), used for the one-shot Instance capture. Start/OnEnable both
// take `this` in x0. Host()/SendInvitation() are kept as additional (host-side) capture
// points for compatibility with the earlier diagnostics.
constexpr u64 NmStartRva = 0x1BF6840;
constexpr u64 NmOnEnableRva = 0x1BF6CC0;
constexpr u64 NmHostRva = 0x1BF7570;
constexpr u64 NmSendInvitationRva = 0x1BFC760;

enum class Stage {
    Idle,      // nothing armed/running
    Armed,     // waiting for the target thread's next scheduling
    Join,      // injected Join() is running; next sentinel return chains StartCoroutine
    Coroutine, // injected StartCoroutine() is running; next sentinel return restores
};

struct State {
    std::mutex mutex;
    std::atomic<Stage> stage{Stage::Idle};
    std::atomic<Kernel::KThread*> target_thread{nullptr};
    Kernel::Svc::ThreadContext saved{};
    std::atomic<u64> base{0};     // module base (calibrated, see CalibrateBase)
    std::atomic<u64> base_entry{0}; // entry point the cached base was calibrated from
    std::atomic<u64> instance{0}; // NetworkManager.Instance (captured via ObserveInstanceCapture)
    u64 string_addr = 0;          // fabricated System.String for the room code
    u64 sp_inject = 0;            // stack pointer handed to the injected calls
    std::string code;             // room code being joined
    std::chrono::steady_clock::time_point recorded_steady{}; // when RecordInvite ran
    std::string pending_code;     // invite that arrived before NetworkManager.Instance existed
};

State g_state;

bool IsRoomCodeChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// IL2CPP System.String: +0x0 klass, +0x8 monitor, +0x10 i32 length, +0x14 UTF-16 chars.
void WriteGuestString(Kernel::KProcess& process, u64 addr, const std::string& ascii,
                      u64 string_klass) {
    auto& memory = process.GetMemory();
    const u32 len = static_cast<u32>(ascii.size());
    memory.Write64(addr + 0x00, string_klass);
    memory.Write64(addr + 0x08, 0); // monitor
    memory.Write32(addr + 0x10, len);
    for (u32 i = 0; i < len; ++i) {
        memory.Write16(addr + 0x14 + i * 2, static_cast<u16>(ascii[i]));
    }
    memory.Write16(addr + 0x14 + len * 2, 0); // trailing NUL, like Unity's own strings
}

// The process entry point is NOT reliably the main module's load address: the NSP
// loader places `main` at the entry point, but the deconstructed-directory loader
// can place it at a different offset (observed live: entry point 0x6000 below the
// real base), and ASLR re-randomizes everything on every boot. Rather than assume,
// calibrate: scan candidate page-aligned bases around the entry point and take the
// one where BOTH target function prologues match their known instruction words.
// Two exact 32-bit words at two independent fixed offsets -- a coincidental match
// across +-1 MiB of candidates is not a realistic failure mode.
u64 CalibrateBase(Kernel::KProcess& process) {
    auto& memory = process.GetMemory();
    const u64 entry = GetInteger(process.GetEntryPoint());
    constexpr s64 kMinPage = -256; // -1 MiB
    constexpr s64 kMaxPage = 256;  // +1 MiB
    for (s64 k = kMinPage; k <= kMaxPage; ++k) {
        const u64 candidate = (entry + k * 0x1000) & ~u64(0xFFF);
        if (!memory.IsValidVirtualAddressRange(candidate + JoinRva, sizeof(u32)) ||
            !memory.IsValidVirtualAddressRange(candidate + StartCoroutineRva, sizeof(u32))) {
            continue;
        }
        if (memory.Read32(candidate + JoinRva) == JoinFirstWord &&
            memory.Read32(candidate + StartCoroutineRva) == StartCoroutineFirstWord) {
            return candidate;
        }
    }
    return 0;
}

// Brings the cached base up to date with the running process. Returns the calibrated
// base (0 = calibration failed). Recalibrates once per boot (the entry point changes
// with ASLR) and invalidates the captured instance across boots, since the old
// NetworkManager object belonged to a dead process image.
u64 RefreshBase(Kernel::KProcess& process) {
    const u64 entry = GetInteger(process.GetEntryPoint());
    if (g_state.base.load(std::memory_order_relaxed) != 0 &&
        g_state.base_entry.load(std::memory_order_relaxed) == entry) {
        return g_state.base.load(std::memory_order_relaxed);
    }
    const u64 base = CalibrateBase(process);
    g_state.base_entry.store(entry, std::memory_order_relaxed);
    g_state.base.store(base, std::memory_order_relaxed);
    g_state.instance.store(0, std::memory_order_relaxed);
    if (base == 0) {
        LOG_WARNING(Core_ARM,
                    "[Nextendo][GUESTCALL] base calibration failed around entry {:#x} -- game "
                    "version may have changed",
                    entry);
    } else {
        LOG_INFO(Core_ARM,
                 "[Nextendo][GUESTCALL] module base calibrated: {:#x} (entry {:#x}, delta {:+#x})",
                 base, entry, static_cast<s64>(base) - static_cast<s64>(entry));
    }
    return base;
}

} // namespace

void ObserveInstanceCapture(u64 vaddr, Kernel::KProcess& process, u64 x0) {
    if (process.GetProgramId() != OutboundTitleId || x0 == 0) {
        return;
    }
    const u64 base = RefreshBase(process);
    if (base == 0) {
        return;
    }
    if (vaddr == base + NmStartRva || vaddr == base + NmOnEnableRva ||
        vaddr == base + NmHostRva || vaddr == base + NmSendInvitationRva) {
        // Only accept an object that really IS a NetworkManager. MemoryReadCode fires
        // on every (re-)translation -- e.g. after a JIT cache flush -- and x0 at that
        // moment is only meaningful on the genuine first execution of the method; the
        // old klass!=0/cached!=0 heuristic accepted boot-time garbage, and the injected
        // Join coroutine then crashed reading fields off the wrong object (observed
        // live: guest svcBreak with a native backtrace right after StartCoroutine).
        // Il2CppClass layout: +0x10 = const char* name -- decisive.
        auto& memory = process.GetMemory();
        if (!memory.IsValidVirtualAddressRange(x0, 0x18)) {
            return;
        }
        const u64 klass = memory.Read64(x0);
        const u64 cached_ptr = memory.Read64(x0 + 0x10); // MonoBehaviour.m_CachedPtr
        if (klass == 0 || cached_ptr == 0 ||
            !memory.IsValidVirtualAddressRange(klass, 0x18)) {
            return;
        }
        const u64 name_ptr = memory.Read64(klass + 0x10);
        if (name_ptr == 0 || !memory.IsValidVirtualAddressRange(name_ptr, 16)) {
            return;
        }
        char name_buf[16] = {};
        memory.ReadBlockUnsafe(name_ptr, name_buf, sizeof(name_buf) - 1);
        if (std::string_view(name_buf) != "NetworkManager") {
            return;
        }
        u64 prev = g_state.instance.exchange(x0, std::memory_order_relaxed);
        if (prev != x0) {
            LOG_INFO(Core_ARM, "[Nextendo][GUESTCALL] NetworkManager instance captured: {:#x}",
                     x0);
            // Complete a pending arm (an invite that arrived before the instance existed).
            Stage expected = Stage::Idle;
            if (!g_state.pending_code.empty() &&
                g_state.stage.compare_exchange_strong(expected, Stage::Armed)) {
                g_state.code = std::move(g_state.pending_code);
                g_state.pending_code.clear();
                LOG_INFO(Core_ARM,
                         "[Nextendo][GUESTCALL] pending join of '{}' armed after instance capture",
                         g_state.code);
            }
        }
    }
}

void RecordInvite(Kernel::KThread& thread, Kernel::KProcess& process,
                  std::string_view room_code) {
    if (process.GetProgramId() != OutboundTitleId) {
        return;
    }
    if (std::getenv("NEXTENDO_NO_GUEST_JOIN") != nullptr) {
        return;
    }
    if (room_code.empty() || room_code.size() > 8 ||
        !std::all_of(room_code.begin(), room_code.end(), IsRoomCodeChar)) {
        LOG_WARNING(Core_ARM, "[Nextendo][GUESTCALL] invalid room code '{}' -- not recording",
                    room_code);
        return;
    }
    {
        std::scoped_lock lk{g_state.mutex};
        g_state.target_thread.store(&thread, std::memory_order_release);
        g_state.code = std::string(room_code);
        g_state.recorded_steady = std::chrono::steady_clock::now();
    }
    LOG_INFO(Core_ARM,
             "[Nextendo][GUESTCALL] invite recorded: room '{}' -- waiting for the user to "
             "accept (toast click) from the multiplayer menu",
             room_code);
}

bool ArmPendingInvite(Kernel::KProcess& process) {
    if (process.GetProgramId() != OutboundTitleId) {
        return false;
    }
    if (std::getenv("NEXTENDO_NO_GUEST_JOIN") != nullptr) {
        return false;
    }

    std::string room_code;
    {
        std::scoped_lock lk{g_state.mutex};
        const auto age = std::chrono::steady_clock::now() - g_state.recorded_steady;
        if (g_state.code.empty() || age > std::chrono::minutes(5)) {
            LOG_WARNING(Core_ARM, "[Nextendo][GUESTCALL] no fresh recorded invitation to accept");
            return false;
        }
        room_code = g_state.code;
    }
    Kernel::KThread* thread = g_state.target_thread.load(std::memory_order_acquire);
    if (thread == nullptr) {
        return false;
    }

    auto& memory = process.GetMemory();
    const u64 base = RefreshBase(process);
    if (base == 0) {
        return false;
    }

    // Verify the game binary is the build all RVAs were derived from. Refuse to inject
    // into anything that doesn't match -- a wrong address would execute arbitrary bytes.
    // (CalibrateBase already matched both prologues, so this is belt-and-braces.)
    const auto check = [&](u64 rva, u32 expected, const char* name) {
        if (!memory.IsValidVirtualAddressRange(base + rva, sizeof(u32))) {
            LOG_WARNING(Core_ARM, "[Nextendo][GUESTCALL] {} RVA {:#x} unmapped", name, rva);
            return false;
        }
        const u32 word = memory.Read32(base + rva);
        if (word != expected) {
            LOG_WARNING(Core_ARM,
                        "[Nextendo][GUESTCALL] {} RVA {:#x} signature mismatch: {:#010x} != "
                        "{:#010x} -- update RVAs for this game version",
                        name, rva, word, expected);
            return false;
        }
        return true;
    };
    if (!check(JoinRva, JoinFirstWord, "NetworkManager.Join") ||
        !check(StartCoroutineRva, StartCoroutineFirstWord, "MonoBehaviour.StartCoroutine")) {
        return false;
    }

    // Resolve System.String's klass via the live-confirmed static chain, so the
    // fabricated argument is a genuine-shaped string for THIS run.
    u64 string_klass = 0;
    if (memory.IsValidVirtualAddressRange(base + StringKlassChainSlotRva, sizeof(u64))) {
        const u64 slot = memory.Read64(base + StringKlassChainSlotRva);
        if (slot != 0 && memory.IsValidVirtualAddressRange(slot + 0x90, sizeof(u64))) {
            string_klass = memory.Read64(slot + 0x90);
        }
    }
    if (string_klass == 0) {
        LOG_WARNING(Core_ARM,
                    "[Nextendo][GUESTCALL] could not resolve System.String klass -- not arming");
        return false;
    }

    if (g_state.instance.load(std::memory_order_relaxed) == 0) {
        // NetworkManager hasn't run yet (capture happens at its first Start/OnEnable).
        // Keep the code pending and complete the arm the moment the instance shows up,
        // instead of losing the invitation.
        g_state.pending_code = std::string(room_code);
        LOG_WARNING(Core_ARM,
                    "[Nextendo][GUESTCALL] NetworkManager.Instance not captured yet -- join of "
                    "'{}' pending until capture",
                    room_code);
        return false;
    }

    Stage expected = Stage::Idle;
    if (!g_state.stage.compare_exchange_strong(expected, Stage::Armed)) {
        // A previous injection is stuck. Injections finish in ~120ms (Join -> StartCoroutine ->
        // context restore), so a non-Idle stage while a FRESH invite is being armed means the last
        // one never completed -- the game crashed or reloaded mid-coroutine, so the sentinel return
        // that resets us to Idle never fired. A new invite is the user asking to retry, so force the
        // state machine back and arm it, instead of refusing every future join until a citron
        // restart (observed: repeated "join not armed: injection already running").
        LOG_WARNING(Core_ARM,
                    "[Nextendo][GUESTCALL] previous injection stuck at stage {} -- force-resetting to "
                    "arm join of '{}'",
                    static_cast<int>(expected), room_code);
        g_state.stage.store(Stage::Armed, std::memory_order_relaxed);
    }
    g_state.code = std::string(room_code);
    LOG_INFO(Core_ARM,
             "[Nextendo][GUESTCALL] armed: joining room '{}' on thread {} (instance={:#x}, "
             "base={:#x})",
             room_code, thread->GetThreadId(), g_state.instance.load(std::memory_order_relaxed),
             base);
    return true;
}

void OnBeforeRun(Kernel::KThread& thread, ArmInterface& arm, Kernel::KProcess& process) {
    // Lock-free pre-check: this runs on every thread scheduling on every core.
    if (g_state.stage.load(std::memory_order_relaxed) != Stage::Armed ||
        g_state.target_thread.load(std::memory_order_acquire) != &thread) {
        // [Nextendo][DIAG] While an arm is outstanding, log (rate-limited) the threads
        // that ARE being scheduled, so a target-identity mismatch is visible instead
        // of a silent never-fire.
        if (g_state.stage.load(std::memory_order_relaxed) == Stage::Armed) {
            static thread_local u64 ticks = 0;
            if ((++ticks & 0x3FF) == 0) {
                LOG_WARNING(Core_ARM,
                            "[Nextendo][GUESTCALL] armed but scheduled thread {} (id={}) != "
                            "target {}",
                            fmt::ptr(&thread), thread.GetThreadId(),
                            fmt::ptr(g_state.target_thread.load(std::memory_order_relaxed)));
            }
        }
        return;
    }
    std::scoped_lock lk{g_state.mutex};
    if (g_state.stage.load(std::memory_order_relaxed) != Stage::Armed ||
        g_state.target_thread.load(std::memory_order_relaxed) != &thread) {
        return;
    }

    auto& memory = process.GetMemory();
    arm.GetContext(g_state.saved);
    const u64 saved_sp = g_state.saved.sp;
    if (saved_sp < 0x40000) {
        LOG_WARNING(Core_ARM, "[Nextendo][GUESTCALL] implausible SP {:#x} -- aborting", saved_sp);
        g_state.stage.store(Stage::Idle, std::memory_order_relaxed);
        return;
    }

    // Private scratch window in the thread's own stack, below the live frames: the
    // injected call runs with SP at the window's low edge (so its own frames grow away
    // from both the live stack and our data), and the fabricated string sits inside the
    // window, above the injected SP, where nothing else will touch it.
    g_state.sp_inject = (saved_sp - 0x2000) & ~u64(0xF);
    g_state.string_addr = g_state.sp_inject + 0x800;
    if (!memory.IsValidVirtualAddressRange(g_state.sp_inject, 0x2000)) {
        LOG_WARNING(Core_ARM,
                    "[Nextendo][GUESTCALL] stack window [{:#x}, {:#x}) not fully mapped -- "
                    "aborting",
                    g_state.sp_inject, saved_sp);
        g_state.stage.store(Stage::Idle, std::memory_order_relaxed);
        return;
    }

    u64 string_klass = 0;
    const u64 slot = memory.Read64(g_state.base.load(std::memory_order_relaxed) +
                                   StringKlassChainSlotRva);
    if (slot != 0 && memory.IsValidVirtualAddressRange(slot + 0x90, sizeof(u64))) {
        string_klass = memory.Read64(slot + 0x90);
    }
    if (string_klass == 0) {
        LOG_WARNING(Core_ARM, "[Nextendo][GUESTCALL] string klass vanished -- aborting");
        g_state.stage.store(Stage::Idle, std::memory_order_relaxed);
        return;
    }
    WriteGuestString(process, g_state.string_addr, g_state.code, string_klass);

    Kernel::Svc::ThreadContext ctx = g_state.saved;
    for (size_t i = 0; i < 29; ++i) {
        ctx.r[i] = 0;
    }
    ctx.fp = 0; // terminate frame walks at our injected frame
    ctx.r[0] = g_state.instance.load(std::memory_order_relaxed); // this
    ctx.r[1] = g_state.string_addr;                              // code
    ctx.r[2] = 0;                       // newWaitForRoomToBecomeAvailable = false
    ctx.lr = 0;                         // sentinel return
    ctx.sp = g_state.sp_inject;
    ctx.pc = g_state.base.load(std::memory_order_relaxed) + JoinRva;
    arm.SetContext(ctx);
    g_state.stage.store(Stage::Join, std::memory_order_relaxed);
    LOG_INFO(Core_ARM,
             "[Nextendo][GUESTCALL] firing Join('{}'): this={:#x} code_str={:#x} pc={:#x} "
             "sp={:#x} (saved sp={:#x})",
             g_state.code, ctx.r[0], ctx.r[1], ctx.pc, ctx.sp, saved_sp);
}

bool OnFault(Kernel::KThread& thread, ArmInterface& arm, Kernel::KProcess& process,
             bool data_abort) {
    std::scoped_lock lk{g_state.mutex};
    const Stage stage = g_state.stage.load(std::memory_order_relaxed);
    if (stage != Stage::Join && stage != Stage::Coroutine) {
        return false;
    }

    if (data_abort) {
        // A real fault inside the injected code. Restore the original context so the
        // thread isn't left suspended in a half-injected state, and let the normal
        // abort handling proceed from there.
        LOG_CRITICAL(Core_ARM,
                     "[Nextendo][GUESTCALL] real data abort inside injected call (stage={}) -- "
                     "restoring context",
                     stage == Stage::Join ? "Join" : "StartCoroutine");
        arm.SetContext(g_state.saved);
        g_state.stage.store(Stage::Idle, std::memory_order_relaxed);
        return true;
    }

    if (stage == Stage::Join) {
        // Join returned into the LR=0 sentinel. x0 now holds the <Join>d__86 state
        // machine; chain StartCoroutine(this, x0). NOTE: the live PC at the fault is
        // not exactly 0 -- dynarmic advances past the aborted fetch slot, observed
        // live as pc=0x4. Anything small is the sentinel; a real crash inside the
        // injected code faults at a full module-range address.
        Kernel::Svc::ThreadContext ctx{};
        arm.GetContext(ctx);
        const u64 state_machine = ctx.r[0];
        if (ctx.pc > 0x1000 || state_machine == 0) {
            LOG_CRITICAL(Core_ARM,
                         "[Nextendo][GUESTCALL] unexpected state after Join: pc={:#x} x0={:#x} "
                         "-- restoring",
                         ctx.pc, state_machine);
            arm.SetContext(g_state.saved);
            g_state.stage.store(Stage::Idle, std::memory_order_relaxed);
            return true;
        }
        LOG_INFO(Core_ARM,
                 "[Nextendo][GUESTCALL] Join returned state machine {:#x} -- starting coroutine",
                 state_machine);
        ctx.r[0] = g_state.instance.load(std::memory_order_relaxed);
        ctx.r[1] = state_machine;
        ctx.r[2] = 0;
        ctx.fp = 0;
        ctx.lr = 0;
        ctx.sp = g_state.sp_inject;
        ctx.pc = g_state.base.load(std::memory_order_relaxed) + StartCoroutineRva;
        arm.SetContext(ctx);
        g_state.stage.store(Stage::Coroutine, std::memory_order_relaxed);
        return true;
    }

    // Coroutine stage: StartCoroutine returned into the sentinel. Restore the original
    // context; the thread continues exactly where the invitation-poll svc left off, and
    // Unity drives the join coroutine from here on its own.
    arm.SetContext(g_state.saved);
    g_state.stage.store(Stage::Idle, std::memory_order_relaxed);
    g_state.target_thread.store(nullptr, std::memory_order_release);
    LOG_INFO(Core_ARM,
             "[Nextendo][GUESTCALL] StartCoroutine returned -- injection complete, guest "
             "context restored (room '{}')",
             g_state.code);
    return true;
}

bool SentinelPending() {
    const Stage stage = g_state.stage.load(std::memory_order_relaxed);
    return stage == Stage::Join || stage == Stage::Coroutine;
}

} // namespace Core::NextendoGuestCall
