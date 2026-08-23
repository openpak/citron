// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>

#include "core/hle/kernel/k_event.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/bsdnu.h"
#include "core/hle/service/sockets/dnspriv.h"
#include "core/hle/service/sockets/ethc.h"
#include "core/hle/service/sockets/nsd.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"

namespace Service::Sockets {

namespace {
// [OpenPak] See the declaration comment in sockets.h.
Kernel::KEvent* g_bsd_deferral_event = nullptr;
}

void SetBsdDeferralEvent(Kernel::KEvent* event) {
    g_bsd_deferral_event = event;
}

Kernel::KEvent* GetBsdDeferralEvent() {
    return g_bsd_deferral_event;
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("bsd:a", std::make_shared<BSD>(system, "bsd:a"));
    server_manager->RegisterNamedService("bsd:s", std::make_shared<BSD>(system, "bsd:s"));
    server_manager->RegisterNamedService("bsd:u", std::make_shared<BSD>(system, "bsd:u"));
    server_manager->RegisterNamedService("bsd:nu", std::make_shared<BSDNU>(system));
    server_manager->RegisterNamedService("bsdcfg", std::make_shared<BSDCFG>(system));
    server_manager->RegisterNamedService("dns:priv", std::make_shared<DNSPRIV>(system));
    server_manager->RegisterNamedService("ethc:c", std::make_shared<ETHC_C>(system));
    server_manager->RegisterNamedService("ethc:i", std::make_shared<ETHC_I>(system));
    server_manager->RegisterNamedService("nsd:a", std::make_shared<NSD>(system, "nsd:a"));
    server_manager->RegisterNamedService("nsd:u", std::make_shared<NSD>(system, "nsd:u"));
    server_manager->RegisterNamedService("sfdnsres", std::make_shared<SFDNSRES>(system));
    // [OpenPak] Previously ran 6 extra worker threads here, on the (incorrect) theory that this
    // matched Ryujinx-Reference's threading model. It doesn't: Ryujinx runs exactly ONE dedicated
    // host thread for its entire Bsd service (ServerBase/ServerLoop), so every socket IPC call is
    // strictly serialized in arrival order with no possibility of two calls' HLE handlers running
    // concurrently or completing out of order. citron's pool let up to 7 threads (each an
    // anonymous, low-priority ThreadType::Dummy KThread with no identity tie to the calling guest
    // thread) race for socket IPC work, so a later-arriving operation's worker could finish before
    // an earlier one's if the host OS scheduler favored it -- breaking the ordering guarantee some
    // guest code (e.g. a one-shot, non-blocking WaitSynchronization probe right after bringing up
    // a connection) implicitly depends on. Reverted to 0 extra threads -- single dedicated thread,
    // matching Ryujinx's actual model exactly -- to test whether this ordering gap explains
    // Splatoon 3's deterministic RST_STREAM-before-HEADERS NPLN failure (see the Splatoon 3
    // connectivity investigation memory). If this reintroduces the pool-exhaustion symptom the
    // prior comment described (concurrent blocking Connect()/DNS calls queuing up other socket
    // traffic), that's real signal too -- Ryujinx's own single-thread design apparently tolerates
    // it, so citron's HLE call durations may need addressing directly rather than papered over
    // with concurrency.
    server_manager->StartAdditionalHostThreads("bsdsocket", 0);

    // [OpenPak] See sockets.h's declaration comment on SetBsdDeferralEvent.
    Kernel::KEvent* deferral_event{};
    server_manager->ManageDeferral(&deferral_event);
    SetBsdDeferralEvent(deferral_event);

    // [OpenPak] Nothing else re-signals this event when a *regular* socket (as opposed to an
    // eventfd) becomes readable -- SendImpl only signals it for eventfd sends, so a deferred
    // Poll() waiting on a plain TCP/TLS socket only ever gets re-checked when something
    // unrelated happens to write that title's own eventfd. Confirmed directly against Splatoon
    // 3: its gRPC handshake reply sat fully received and ACKed in the kernel socket buffer for
    // the entire ~20s lifetime of a connection attempt with zero re-checks, until an unrelated
    // watchdog gave up and tore the connection down -- the deferred poll was simply never woken
    // to notice the data had already arrived. A steady low-overhead heartbeat closes that gap:
    // any outstanding deferred Poll() gets re-evaluated at least this often (each re-check is
    // just a non-blocking WSAPoll -- microseconds).
    //
    // [OpenPak] 50ms -> 1ms, matching Ryujinx-Reference's own measured fix for this identical
    // mechanism (ServerBase.cs's loopTimeout). Their own instrumentation on a live session found
    // 10,043 of 10,587 Send->Recv round trips landing in the 50-54ms bracket (avg 50.4ms) against
    // ~6ms of real network time -- i.e. this heartbeat interval was quantizing every round trip up
    // to itself, not just adding latency. That caps the whole HTTP/2 event loop at ~20
    // iterations/sec: their measured result at 50ms was minutes to load, 1.3MB of control-frame
    // churn with zero application-level calls succeeding, ending in a communication error --
    // matching Citron's own single-round-trip-then-die pattern exactly. At 1ms the heartbeat drops
    // below real network time and the transport becomes network-bound again instead of
    // heartbeat-bound.
    std::jthread deferral_heartbeat([deferral_event](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (deferral_event) {
                deferral_event->Signal();
            }
        }
    });

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Sockets
