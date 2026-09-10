// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <dynarmic/interface/code_page.h>
#include "common/profiling.h"
#include "common/settings.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/nextendo_guest_call.h"

namespace Core {

using Vector = Dynarmic::A64::Vector;
using namespace Common::Literals;

namespace {
// [Nextendo] Real fix for Outbound's (title ID 0100ED9024EB8000) friend-invite join -- see
// the full explanation at the MemoryReadCode call site below. All of this is gated on the
// title ID and must never affect any other game.
//
// First attempt redirected NetworkManager.StartJoin(InviteData), the delegate
// OnGameReadyForSessionEvents calls with a ready invite -- confirmed live that
// OnGameReadyForSessionEvents itself is NEVER called on this platform (a diagnostic hook on
// its own entry never fired across multiple real invites, even from the multiplayer menu),
// so StartJoin never runs either, and the redirect there was inert. Redirecting instead from
// CheckForInvite itself, right where it already builds a byte-correct CustomData array (into
// x21) just before feeding empty strings into the dead InviteData/JoinPlatformSession
// pipeline -- CheckForInvite is proven to run every single frame regardless of game state.
constexpr u64 OutboundTitleId = 0x0100ED9024EB8000ULL;
// [Nextendo][DIAG] shared across MemoryReadCode/AddTicks -- see their comments.
u64 s_network_system_switch_instance = 0;
bool s_cached_invite_seen_logged = false;
// [Nextendo] Redesigned redirect point: right after guardB (the actual "did the native pop
// succeed" check), instead of after the whole guardC/indirect-call/guardD block. That block
// turned out to gate a real indirect call whose target is only valid when its own dictionary
// lookup succeeds (confirmed live -- forcing past it crashed by jumping through garbage). We
// don't need any of that logic at all: the raw popped bytes are already fully valid right
// after guardB, in x20 (data at +0x20), with the real byte count at [x29,#0x18] (x29 is
// CheckForInvite's own frame pointer, unchanged and still valid here). Redirecting this early
// also means we must manually restore the stack/frame CheckForInvite's own prologue set up,
// since we're bypassing its normal epilogue entirely -- see the trampoline's tail.
constexpr u64 CheckForInviteRedirectVA = 0x83F3ABA0ULL;
constexpr u32 CheckForInviteRedirectInstruction = 0x1400d730u;
constexpr u64 TrampolineVA = 0x83F70860ULL;
// NetworkManager.Instance isn't reachable from CheckForInvite's own context (NetworkSystemSwitch
// has no back-reference to it, and its generic-base static storage isn't statically resolvable
// without guessing at IL2Cpp's RGCTX layout). Instead it's captured opportunistically off any
// NetworkManager instance method we can prove actually runs (Host(), SendInvitation()) and
// stashed here; the trampoline loads it at runtime, not bake-time, so a capture that happens
// after the trampoline's first translation still takes effect on the next invite.
constexpr u64 MailboxVA = 0x83F70920ULL;
// [Nextendo] Hardened after a reproducible crash: the previous version trusted guardB to mean
// "there really is an invite," but a real invite never being queued still somehow reached this
// point with an unvalidated byte count read from [x29,#0x18] -- if the native pop genuinely
// never wrote that output (thrown before reaching it), it's leftover stack garbage, not zero.
// Now explicitly rejects a byte count that's <=0 or > the mailbox's own 1024-byte max before
// doing anything else, regardless of what the guest guards claim.
//
// Hand-assembled (keystone, ARM64) and disassembly-verified trampoline body, written into the
// confirmed-dead (zero real callers, checked across the whole binary) bodies of
// nn.friends.Friends.ShowFriendList / ShowUserDetailInfo / StartSendingFriendRequest. Reads
// NetworkManager.Instance from MailboxVA (bails out, with the stack/frame properly restored,
// if not yet captured), allocates a real System.String via Outbound's own string allocator
// sized to the popped byte count, copies the raw popped bytes in as UTF-16 chars, restores
// CheckForInvite's caller's frame/LR/SP (read via x29, CheckForInvite's own frame pointer),
// then tail-jumps into NetworkManager.Join(code, false) -- the same real, complete, working
// connect path a manual code-entry UI would use (confirmed via full disassembly: validates
// the code, sets sessionName, and calls Fusion's own NetworkRunner.JoinSessionLobby).
[[maybe_unused]] constexpr u32 TrampolineWords[] = {
    0xd100c3ff, 0xf9000bf4, 0xb9401ba9, 0x7100013f, 0x540004cd, 0x7110013f, 0x5400048c,
    0xf9000fe9, 0xd2812400, 0xf2b07ee0, 0xf9400000, 0xb4000340, 0xaa0003f3, 0xaa0903e0,
    0x9719b737, 0xf90013e0, 0xf9400be8, 0xb9401be9, 0xf94013ea, 0x91008108, 0x9100514a,
    0xaa1f03eb, 0xeb09017f, 0x540000ca, 0x386b690c, 0xd37ff96d, 0x782d694c, 0x9100056b,
    0x17fffffa, 0xaa1303e0, 0xf94013e1, 0x52800002, 0xf94007be, 0xf94003a8, 0x910103bf,
    0xaa0803fd, 0x17721b74, 0xf94007be, 0xf94003a8, 0x910103bf, 0xaa0803fd, 0xd65f03c0,
    0xf94007be, 0xf94003a8, 0x910103bf, 0xaa0803fd, 0xd65f03c0,
};
} // namespace

class DynarmicCallbacks64 : public Dynarmic::A64::UserCallbacks {
public:
    explicit DynarmicCallbacks64(ArmDynarmic64& parent, Kernel::KProcess* process)
        : m_parent{parent}, m_memory(process->GetMemory()),
          m_process(process), m_debugger_enabled{parent.m_system.DebuggerEnabled()},
          m_check_memory_access{m_debugger_enabled ||
                                !Settings::values.cpuopt_ignore_memory_aborts.GetValue()} {}

    u8 MemoryRead8(u64 vaddr) override {
        CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Read);
        return m_memory.Read8(vaddr);
    }
    u16 MemoryRead16(u64 vaddr) override {
        CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Read);
        return m_memory.Read16(vaddr);
    }
    u32 MemoryRead32(u64 vaddr) override {
        CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Read);
        return m_memory.Read32(vaddr);
    }
    u64 MemoryRead64(u64 vaddr) override {
        CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Read);
        return m_memory.Read64(vaddr);
    }
    Vector MemoryRead128(u64 vaddr) override {
        CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Read);
        return {m_memory.Read64(vaddr), m_memory.Read64(vaddr + 8)};
    }
    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        // [Nextendo] Everything below is gated on Outbound's title ID and must never affect
        // any other game.
        if (m_process->GetProgramId() == OutboundTitleId) {
            // [Nextendo][GUESTCALL] Capture NetworkManager.Instance (x0 at the entry of
            // any of its known instance methods). Runtime-base-aware, unlike the
            // hardcoded-VA checks further below -- see nextendo_guest_call.cpp.
            Core::NextendoGuestCall::ObserveInstanceCapture(
                vaddr, *m_process, m_parent.m_jit->GetRegister(0));

            // [Nextendo] MailboxVA sits inside the dead-code scratch region, which still
            // contains the REAL original compiled bytes of Outbound's own code until this
            // exact address is substituted -- MemoryReadCode substitution never touches
            // underlying guest memory, only what the JIT compiles as instructions there. So
            // before anything ever writes a real NetworkManager.Instance capture, a read of
            // MailboxVA returns leftover non-zero instruction bytes, not zero. The trampoline's
            // `cbz x0, bail` then misreads that garbage as a "valid" pointer and uses it as
            // Join()'s `this` -- confirmed live as the cause of a real (delayed) crash.
            // Explicitly zero it out before anything else can possibly run.
            static bool s_mailbox_initialized = false;
            if (!s_mailbox_initialized) {
                s_mailbox_initialized = true;
                const bool valid = m_memory.IsValidVirtualAddressRange(MailboxVA, 8);
                LOG_INFO(Core_ARM,
                         "[OpenPak][DIAG] mailbox init: vaddr={:#x} range_valid={} "
                         "value_before={:#x}",
                         vaddr, valid, valid ? m_memory.Read64(MailboxVA) : 0);
                if (valid) {
                    m_memory.Write64(MailboxVA, 0);
                    LOG_INFO(Core_ARM, "[OpenPak][DIAG] mailbox init: value_after={:#x}",
                             m_memory.Read64(MailboxVA));
                }
            }
            static bool s_join_movenext_logged = false;
            static bool s_sanity_logged = false;
            if (!s_sanity_logged && vaddr == 0x83F3AA90) {
                s_sanity_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] SANITY: CheckForInvite/Update reached "
                                    "(hook mechanism works)");
            }
            // Capture NetworkSystemSwitch's `this` (x0 at Update()'s entry, an instance
            // method) so AddTicks can poll _cachedInvite (this+0x28) continuously below --
            // MemoryReadCode only ever fires once per address (JIT caches the compiled
            // block after that), so it can't observe state on any later, real invocation.
            if (vaddr == 0x83F3AA90) {
                s_network_system_switch_instance = m_parent.m_jit->GetRegister(0);
            }
            // Opportunistic NetworkManager.Instance capture -- see MailboxVA's comment above.
            // Host() and SendInvitation() are both proven-live starter functions taking
            // NetworkManager itself as x0; stash it for the trampoline every time, not just
            // once, since the object can be recreated (e.g. on a scene reload).
            // [Nextendo][GUESTCALL] The hardcoded VAs here assumed one specific ASLR base
            // (0x80000000) and silently missed every other launch. The authoritative capture
            // now runs base-aware via ObserveInstanceCapture above (including joiner-side
            // NetworkManager.Start/OnEnable); only the guest-side MailboxVA mirror write is
            // kept here for the (currently disabled) trampoline diagnostics.
            {
                const u64 nm_base = GetInteger(m_process->GetEntryPoint());
                if (vaddr == nm_base + 0x1BF7570 || vaddr == nm_base + 0x1BFC760) {
                    const u64 x0 = m_parent.m_jit->GetRegister(0);
                    if (m_memory.IsValidVirtualAddressRange(MailboxVA, 8)) {
                        m_memory.Write64(MailboxVA, x0);
                    }
                    LOG_INFO(Core_ARM, "[OpenPak][DIAG] NetworkManager instance captured: {:#x}",
                             x0);
                }
            }
            // NetworkManager.Join(string, bool).MoveNext -- confirms the redirect actually
            // landed a real join attempt.
            if (!s_join_movenext_logged && vaddr == 0x81C00850) {
                s_join_movenext_logged = true;
                const u64 x0 = m_parent.m_jit->GetRegister(0);
                LOG_INFO(Core_ARM,
                         "[OpenPak][DIAG] Join.MoveNext reached via redirect! this={:#x}", x0);
            }
            // Real fix for the friend-invite join: NetworkSystemSwitch.JoinSession is a
            // confirmed no-op on this platform (never reads its own argument, never connects),
            // and NetworkManager.StartJoin(InviteData) -- the entry point that was supposed to
            // hand a ready invite to JoinPlatformSession -- is never even called, because its
            // only caller, OnGameReadyForSessionEvents, itself never runs here (confirmed live).
            // So an accepted invite's CustomData -- proven to arrive byte-correct via our
            // mailbox -- never turns into a real Fusion connection through any path Outbound's
            // own code takes on Switch. Instead we redirect from CheckForInvite itself, right
            // where it already has a byte-correct CustomData array built (in x21) and is about
            // to feed empty strings into the dead pipeline. See TrampolineWords' comment above
            // for what the trampoline itself does.
            // [Nextendo][DIAG] CheckForInvite's own internal guard checks, to find exactly
            // which one bails before ever reaching the redirect point.
            static bool s_guardA_logged = false;
            if (!s_guardA_logged && vaddr == 0x83F3AB64) {
                s_guardA_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] guardA (initial precondition) w0={:#x}",
                         m_parent.m_jit->GetRegister(0));
            }
            static bool s_guardB_logged = false;
            if (!s_guardB_logged && vaddr == 0x83F3AB9C) {
                s_guardB_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] guardB (pop succeeded?) w0={:#x}",
                         m_parent.m_jit->GetRegister(0));
            }
            static bool s_guardC_logged = false;
            if (!s_guardC_logged && vaddr == 0x83F3ABEC) {
                s_guardC_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] guardC (dictionary lookup) w0={:#x}",
                         m_parent.m_jit->GetRegister(0));
            }
            static bool s_bail1_logged = false;
            if (!s_bail1_logged && vaddr == 0x83F3AD90) {
                s_bail1_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] BAIL at 0x3f3ad90 (early return)");
            }
            static bool s_bail2_logged = false;
            if (!s_bail2_logged && vaddr == 0x83F3ADA8) {
                s_bail2_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] BAIL at 0x3f3ada8 (guardC failure path)");
            }
            // [Nextendo][DIAG] granular checkpoints inside/around the redirect+trampoline, to
            // see exactly how far execution gets if Join.MoveNext never fires.
            static bool s_redirect_reached_logged = false;
            if (!s_redirect_reached_logged && vaddr == CheckForInviteRedirectVA) {
                s_redirect_reached_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] CheckForInvite redirect point reached "
                                    "(x20 raw pop buffer={:#x})",
                         m_parent.m_jit->GetRegister(20));
            }
            static bool s_mailbox_checked_logged = false;
            if (!s_mailbox_checked_logged && vaddr == TrampolineVA + 0x2C) {
                s_mailbox_checked_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] trampoline: mailbox value = {:#x}",
                         m_parent.m_jit->GetRegister(0));
            }
            static bool s_pre_join_call_logged = false;
            if (!s_pre_join_call_logged && vaddr == TrampolineVA + 0x90) {
                s_pre_join_call_logged = true;
                LOG_INFO(Core_ARM,
                         "[OpenPak][DIAG] trampoline: about to call Join(), x0(this)={:#x} "
                         "x1(code str)={:#x}",
                         m_parent.m_jit->GetRegister(0), m_parent.m_jit->GetRegister(1));
            }
            // [Nextendo] DISABLED AGAIN: still crashes (same "jump to address 0" signature)
            // even with the byte-count validation added, and at a similarly early/consistent
            // time regardless of any real invite. That rules out the "garbage byte count"
            // theory -- the bug is somewhere more fundamental in this redirect's own
            // register/stack assumptions, and needs a reliable live debugger to pin down
            // rather than more static guessing. Do not re-enable blind.
            (void)CheckForInviteRedirectVA;
            (void)CheckForInviteRedirectInstruction;
            (void)TrampolineVA;
        }

        if (!m_memory.IsValidVirtualAddressRange(vaddr, sizeof(u32)))
            return std::nullopt;
        auto const aligned_vaddr = vaddr & ~Core::Memory::CITRON_PAGEMASK;
        if (last_code_addr != aligned_vaddr) {
            m_memory.ReadBlock(aligned_vaddr, &cached_code_page, sizeof(cached_code_page));
            last_code_addr = aligned_vaddr;
        }
        return cached_code_page.inst[(vaddr & Core::Memory::CITRON_PAGEMASK) / sizeof(u32)];
    }

    void MemoryWrite8(u64 vaddr, u8 value) override {
        if (CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write)) {
            m_memory.Write8(vaddr, value);
        }
    }
    void MemoryWrite16(u64 vaddr, u16 value) override {
        if (CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write)) {
            m_memory.Write16(vaddr, value);
        }
    }
    void MemoryWrite32(u64 vaddr, u32 value) override {
        if (CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write)) {
            m_memory.Write32(vaddr, value);
        }
    }
    void MemoryWrite64(u64 vaddr, u64 value) override {
        if (CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write)) {
            m_memory.Write64(vaddr, value);
        }
    }
    void MemoryWrite128(u64 vaddr, Vector value) override {
        if (CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write)) {
            m_memory.Write64(vaddr, value[0]);
            m_memory.Write64(vaddr + 8, value[1]);
        }
    }

    bool MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) override {
        return CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write) &&
               m_memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) override {
        return CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write) &&
               m_memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) override {
        return CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write) &&
               m_memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) override {
        return CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write) &&
               m_memory.WriteExclusive64(vaddr, value, expected);
    }
    bool MemoryWriteExclusive128(u64 vaddr, Vector value, Vector expected) override {
        return CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write) &&
               m_memory.WriteExclusive128(vaddr, value, expected);
    }

    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op,
                                         u64 value) override {
        switch (op) {
        case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU: {
            static constexpr u64 ICACHE_LINE_SIZE = 64;

            const u64 cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
            m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
            break;
        }
        case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
            m_parent.ClearInstructionCache();
            break;
        case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
        default:
            LOG_DEBUG(Core_ARM, "Unprocesseed instruction cache operation: {}", op);
            break;
        }

        m_parent.m_jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
    }

    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) override {
        switch (exception) {
        case Dynarmic::A64::Exception::WaitForInterrupt:
        case Dynarmic::A64::Exception::WaitForEvent:
        case Dynarmic::A64::Exception::SendEvent:
        case Dynarmic::A64::Exception::SendEventLocal:
        case Dynarmic::A64::Exception::Yield:
            return;
        case Dynarmic::A64::Exception::NoExecuteFault: {
            // [Nextendo][GUESTCALL] A fault at pc=0 while an injected call is running is
            // our intentional LR=0 sentinel return -- keep the log quiet and don't
            // pollute the fault counters/backtrace; PhysicalCore::OnFault handles it.
            if (pc == 0 && Core::NextendoGuestCall::SentinelPending()) {
                LOG_INFO(Core_ARM,
                         "[OpenPak][GUESTCALL] sentinel return reached (pc=0) -- chaining/"
                         "restoring");
                ReturnException(pc, PrefetchAbort);
                return;
            }

            LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", pc);

            m_consecutive_faults++;
            if (pc < 0x1000 || m_consecutive_faults > 2) {
                LOG_CRITICAL(Core_ARM,
                             "Fatal: fault at {:#016x} (consecutive={}), suspending thread",
                             pc, m_consecutive_faults);
                m_parent.LogBacktrace(m_process);
            }

            ReturnException(pc, PrefetchAbort);
            return;
        }
        default:
            m_consecutive_faults = 0;
            if (m_debugger_enabled) {
                ReturnException(pc, InstructionBreakpoint);
                return;
            }

            m_parent.LogBacktrace(m_process);
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})",
                         static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
            ReturnException(pc, PrefetchAbort);
            return;
        }
    }

    void CallSVC(u32 svc) override {
        m_parent.m_svc = svc;
        m_parent.m_jit->HaltExecution(SupervisorCall);
    }

    void AddTicks(u64 ticks) override {
        // [Nextendo][DIAG] AddTicks fires continuously during real execution (unlike
        // MemoryReadCode, which only ever fires once per address), so it's the only reliable
        // way to observe _cachedInvite's value on whatever real invocation of CheckForInvite
        // actually processes an invite, not just the first-ever (usually invite-less) one.
        if (m_process->GetProgramId() == OutboundTitleId && !s_cached_invite_seen_logged &&
            s_network_system_switch_instance != 0 &&
            m_memory.IsValidVirtualAddressRange(s_network_system_switch_instance + 0x28, 8)) {
            const u64 cached_invite = m_memory.Read64(s_network_system_switch_instance + 0x28);
            if (cached_invite != 0) {
                s_cached_invite_seen_logged = true;
                LOG_INFO(Core_ARM, "[OpenPak][DIAG] _cachedInvite became non-null: {:#x}",
                         cached_invite);
            }
        }
        ASSERT_MSG(!m_parent.m_uses_wall_clock, "Dynarmic ticking disabled");

        // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
        // rough approximation of the amount of executed ticks in the system, it may be thrown off
        // if not all cores are doing a similar amount of work. Instead of doing this, we should
        // device a way so that timing is consistent across all cores without increasing the ticks 4
        // times.
        u64 amortized_ticks = ticks / Core::Hardware::NUM_CPU_CORES;
        // Always execute at least one tick.
        amortized_ticks = std::max<u64>(amortized_ticks, 1);

        m_parent.m_system.CoreTiming().AddTicks(amortized_ticks);
    }

    u64 GetTicksRemaining() override {
        ASSERT_MSG(!m_parent.m_uses_wall_clock, "Dynarmic ticking disabled");

        return std::max<s64>(m_parent.m_system.CoreTiming().GetDowncount(), 0);
    }

    u64 GetCNTPCT() override {
        return m_parent.m_system.CoreTiming().GetClockTicks();
    }

    bool CheckMemoryAccess(u64 addr, u64 size, Kernel::DebugWatchpointType type) {
        if (!m_check_memory_access) {
            return true;
        }

        if (!m_memory.IsValidVirtualAddressRange(addr, size)) {
            LOG_CRITICAL(Core_ARM, "Stopping execution due to unmapped memory access at {:#x}",
                         addr);
            m_parent.m_jit->HaltExecution(PrefetchAbort);
            return false;
        }

        if (!m_debugger_enabled) {
            return true;
        }

        const auto match{m_parent.MatchingWatchpoint(addr, size, type)};
        if (match) {
            m_parent.m_halted_watchpoint = match;
            m_parent.m_jit->HaltExecution(DataAbort);
            return false;
        }

        return true;
    }

    void ReturnException(u64 pc, Dynarmic::HaltReason hr) {
        m_parent.GetContext(m_parent.m_breakpoint_context);
        m_parent.m_breakpoint_context.pc = pc;
        m_parent.m_jit->HaltExecution(hr);
    }

    ArmDynarmic64& m_parent;
    Core::Memory::Memory& m_memory;
    u64 m_tpidrro_el0{};
    u64 m_tpidr_el0{};
    Kernel::KProcess* m_process{};
    const bool m_debugger_enabled{};
    const bool m_check_memory_access{};
    u32 m_consecutive_faults{};
    static constexpr u64 MinimumRunCycles = 10000U;
    Dynarmic::CodePage cached_code_page;
    u64 last_code_addr = 0;
};

std::shared_ptr<Dynarmic::A64::Jit> ArmDynarmic64::MakeJit(Common::PageTable* page_table,
                                                           std::size_t address_space_bits) const {
    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = m_cb.get();

    // Memory
    if (page_table) {
        config.page_table = reinterpret_cast<void**>(page_table->entries.data());
        config.page_table_log2_stride = 5;
        config.page_table_address_space_bits = uint32_t(address_space_bits);
        config.page_table_pointer_mask_bits = Common::PageTable::ATTRIBUTE_BITS;
        config.silently_mirror_page_table = false;
        config.absolute_offset_page_table = true;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;

        config.fastmem_pointer = reinterpret_cast<uintptr_t>(page_table->fastmem_arena);
        config.fastmem_address_space_bits = uint32_t(address_space_bits);
        config.silently_mirror_fastmem = false;

        config.fastmem_exclusive_access = config.fastmem_pointer.has_value();
        config.recompile_on_exclusive_fastmem_failure = true;
    }

    // Multi-process state
    config.processor_id = uint8_t(m_core_index);
    config.global_monitor = &m_exclusive_monitor.monitor;

    // System registers
    config.tpidrro_el0 = &m_cb->m_tpidrro_el0;
    config.tpidr_el0 = &m_cb->m_tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    config.cntfrq_el0 = Hardware::CNTFREQ;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Timing
    config.wall_clock_cntpct = m_uses_wall_clock;
    config.enable_cycle_counting = !m_uses_wall_clock;

    // Code cache size
#ifdef ARCHITECTURE_arm64
    config.code_cache_size = u32(128_MiB);
#else
    config.code_cache_size = u32(512_MiB);
#endif

    // Allow memory fault handling to work
    if (m_system.DebuggerEnabled()) {
        config.check_halt_on_memory_access = true;
    }

    // null_jit
    if (!page_table) {
        // Don't waste too much memory on null_jit
        config.code_cache_size = u32(8_MiB);
    }

    // Safe optimizations
    if (Settings::values.cpu_debug_mode) {
        if (!Settings::values.cpuopt_page_tables) {
            config.page_table = nullptr;
        }
        if (!Settings::values.cpuopt_block_linking) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!Settings::values.cpuopt_return_stack_buffer) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        if (!Settings::values.cpuopt_fast_dispatcher) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        if (!Settings::values.cpuopt_context_elimination) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
        }
        if (!Settings::values.cpuopt_const_prop) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
        }
        if (!Settings::values.cpuopt_misc_ir) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
        }
        if (!Settings::values.cpuopt_reduce_misalign_checks) {
            config.only_detect_misalignment_via_page_table_on_page_boundary = false;
        }
        if (!Settings::values.cpuopt_fastmem) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_fastmem_exclusives) {
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_recompile_exclusives) {
            config.recompile_on_exclusive_fastmem_failure = false;
        }
        if (!Settings::values.cpuopt_ignore_memory_aborts) {
            config.check_halt_on_memory_access = true;
        }
    } else {
        const auto cpu_accuracy = Settings::values.cpu_accuracy.GetValue();

        // Unsafe optimizations
        if (cpu_accuracy == Settings::CpuAccuracy::Unsafe) {
            config.unsafe_optimizations = true;
            if (Settings::values.cpuopt_unsafe_unfuse_fma) {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
            }
            if (Settings::values.cpuopt_unsafe_reduce_fp_error) {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
            }
            if (Settings::values.cpuopt_unsafe_inaccurate_nan) {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
            }
            if (Settings::values.cpuopt_unsafe_fastmem_check) {
                config.fastmem_address_space_bits = 64;
            }
            if (Settings::values.cpuopt_unsafe_ignore_global_monitor) {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
            }
        }

        // Aggressive low-accuracy profile intended for CPU-bound workloads.
        if (cpu_accuracy == Settings::CpuAccuracy::UltraLow) {
            config.unsafe_optimizations = true;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
            config.fastmem_address_space_bits = 64;
        }

        // Curated optimizations
        if (cpu_accuracy == Settings::CpuAccuracy::Auto) {
            config.unsafe_optimizations = true;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
            config.fastmem_address_space_bits = 64;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }

        // Paranoia mode for debugging optimizations
        if (cpu_accuracy == Settings::CpuAccuracy::Paranoid) {
            config.unsafe_optimizations = false;
            config.optimizations = Dynarmic::no_optimizations;
        }
    }

    return std::make_shared<Dynarmic::A64::Jit>(config);
}

HaltReason ArmDynarmic64::RunThread(Kernel::KThread* thread) {
    CITRON_PROFILE_SCOPE("Dynarmic64::Run");
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Run());
}

HaltReason ArmDynarmic64::StepThread(Kernel::KThread* thread) {
    CITRON_PROFILE_SCOPE("Dynarmic64::Step");
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

u32 ArmDynarmic64::GetSvcNumber() const {
    return m_svc;
}

void ArmDynarmic64::GetSvcArguments(std::span<uint64_t, 8> args) const {
    Dynarmic::A64::Jit& j = *m_jit;

    for (size_t i = 0; i < 8; i++) {
        args[i] = j.GetRegister(i);
    }
}

void ArmDynarmic64::SetSvcArguments(std::span<const uint64_t, 8> args) {
    Dynarmic::A64::Jit& j = *m_jit;

    for (size_t i = 0; i < 8; i++) {
        j.SetRegister(i, args[i]);
    }
}

const Kernel::DebugWatchpoint* ArmDynarmic64::HaltedWatchpoint() const {
    return m_halted_watchpoint;
}

void ArmDynarmic64::RewindBreakpointInstruction() {
    this->SetContext(m_breakpoint_context);
}

ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process,
                             DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor},
      m_cb(std::make_unique<DynarmicCallbacks64>(*this, process)), m_core_index{core_index} {
    auto& page_table = process->GetPageTable().GetBasePageTable();
    auto& page_table_impl = page_table.GetImpl();
    m_jit = MakeJit(&page_table_impl, page_table.GetAddressSpaceWidth());
}

ArmDynarmic64::~ArmDynarmic64() = default;

void ArmDynarmic64::SetTpidrroEl0(u64 value) {
    m_cb->m_tpidrro_el0 = value;
}

void ArmDynarmic64::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    Dynarmic::A64::Jit& j = *m_jit;
    auto gpr = j.GetRegisters();
    auto fpr = j.GetVectors();

    // TODO: this is inconvenient
    for (size_t i = 0; i < 29; i++) {
        ctx.r[i] = gpr[i];
    }
    ctx.fp = gpr[29];
    ctx.lr = gpr[30];

    ctx.sp = j.GetSP();
    ctx.pc = j.GetPC();
    ctx.pstate = j.GetPstate();
    ctx.v = fpr;
    ctx.fpcr = j.GetFpcr();
    ctx.fpsr = j.GetFpsr();
    ctx.tpidr = m_cb->m_tpidr_el0;
}

void ArmDynarmic64::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    Dynarmic::A64::Jit& j = *m_jit;

    // TODO: this is inconvenient
    std::array<u64, 31> gpr;

    for (size_t i = 0; i < 29; i++) {
        gpr[i] = ctx.r[i];
    }
    gpr[29] = ctx.fp;
    gpr[30] = ctx.lr;

    j.SetRegisters(gpr);
    j.SetSP(ctx.sp);
    j.SetPC(ctx.pc);
    j.SetPstate(ctx.pstate);
    j.SetVectors(ctx.v);
    j.SetFpcr(ctx.fpcr);
    j.SetFpsr(ctx.fpsr);
    m_cb->m_tpidr_el0 = ctx.tpidr;
}

void ArmDynarmic64::SignalInterrupt(Kernel::KThread* thread) {
    m_jit->HaltExecution(BreakLoop);
}

void ArmDynarmic64::ClearInstructionCache() {
    CITRON_PROFILE_SCOPE("Dynarmic64::ClearCache");
    m_jit->ClearCache();
}

void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {
    CITRON_PROFILE_SCOPE("Dynarmic64::InvalidateCacheRange");
    m_jit->InvalidateCacheRange(addr, size);
}

} // namespace Core
