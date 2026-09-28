// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/fs/file.h"
#include "common/hex_util.h"
#include "common/settings.h"
#include "common/socket_types.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/interface_list.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"

using Common::Expected;
using Common::Unexpected;

namespace Service::Sockets {

namespace {

// Best-effort PRUDP-Lite header decode for P2P match traffic (SYN/CONNECT/DATA/DISCONNECT/PING +
// flags). Only the header is plaintext; the RMC payload inside stays opaque. Returns empty for
// anything that isn't a PRUDP-Lite packet (magic 0x80, >=12 bytes) so callers can skip it.
std::string DescribePrudpLite(std::span<const u8> data) {
    static constexpr std::array<const char*, 5> type_names{"SYN", "CONNECT", "DATA", "DISCONNECT",
                                                            "PING"};
    if (data.size() < 12 || data[0] != 0x80) {
        // TEMPORARY DIAGNOSTIC: dump the full packet (not just a 16-byte prefix) so a non-PRUDP-Lite
        // protocol (e.g. Pia P2P) can actually be read back out of the log instead of guessed at.
        const auto head = data.subspan(0, std::min<size_t>(data.size(), 1500));
        return fmt::format("raw[{}]={}", data.size(), Common::HexToString(head, false));
    }
    const u16 type_flags = static_cast<u16>(data[8] | (data[9] << 8));
    const u8 type = type_flags & 0xF;
    const u16 flags = type_flags >> 4;
    std::string out = fmt::format("prudp type={}", type < type_names.size() ? type_names[type]
                                                                            : std::to_string(type));
    if (flags & 0x001) out += "|ACK";
    if (flags & 0x002) out += "|Reliable";
    if (flags & 0x004) out += "|NeedACK";
    if (flags & 0x200) out += "|MultiACK";
    out += fmt::format(" id={}", static_cast<u16>(data[10] | (data[11] << 8)));
    return out;
}

// Queued from an earlier send's ICMP error, not from this receive.
bool IsTransientDatagramError(Errno bsd_errno) {
    return bsd_errno == Errno::CONNREFUSED || bsd_errno == Errno::CONNRESET;
}

bool IsConnectionBased(Type type) {
    switch (type) {
    case Type::STREAM:
        return true;
    case Type::DGRAM:
        return false;
    case Type::RAW:
        // [OpenPak] Only reached for SOCK_RAW + IPPROTO_ICMP (see SocketImpl's guard);
        // ICMP echo pings are connectionless.
        return false;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return false;
    }
}

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), std::min(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
void PutValue(std::span<u8> buffer, const T& t) {
    std::memcpy(buffer.data(), &t, std::min(sizeof(T), buffer.size()));
}

// [OpenPak] A guest sockaddr in either family's shape, as the host address this layer speaks.
// IPv4 is {len, family=2, port, addr[4], zero[8]}; IPv6 is {len, family=28, port, flowinfo[4],
// addr[16], scope_id[4]}, 28 bytes. An NPLN title's gRPC stack dials its dual-mode AF_INET6
// socket with the v4-mapped form of the resolver's IPv4 answer, so the address is the mapped
// tail; :: is "any" and ::1 is loopback. A genuine IPv6 address has nowhere to go here and is
// refused (nullopt -> EAFNOSUPPORT). Anything else of 16 bytes or more is read as IPv4. Ported
// from Eden, where refusing the family outright poisoned Stardew's whole gRPC channel.
std::optional<Network::SockAddrIn> ParseGuestSockAddr(std::span<const u8> addr) {
    constexpr size_t v6_size = 28;
    if (addr.size() >= v6_size && addr[1] == static_cast<u8>(Domain::INET6)) {
        Network::SockAddrIn result{};
        result.family = Network::Domain::INET;
        result.portno = static_cast<u16>(addr[2] << 8 | addr[3]);

        const u8* const v6 = addr.data() + 8;
        const bool leading_zeros = std::all_of(v6, v6 + 10, [](u8 byte) { return byte == 0; });
        const bool mapped = leading_zeros && v6[10] == 0xff && v6[11] == 0xff;
        const bool zero_tail = leading_zeros && v6[10] == 0 && v6[11] == 0 && v6[12] == 0 &&
                               v6[13] == 0 && v6[14] == 0;
        if (mapped) {
            std::memcpy(result.ip.data(), v6 + 12, result.ip.size());
        } else if (zero_tail && v6[15] == 0) {
            result.ip = {0, 0, 0, 0};
        } else if (zero_tail && v6[15] == 1) {
            result.ip = {127, 0, 0, 1};
        } else {
            return std::nullopt;
        }
        return result;
    }
    if (addr.size() >= sizeof(SockAddrIn)) {
        return Translate(GetValue<SockAddrIn>(addr));
    }
    return std::nullopt;
}

} // Anonymous namespace

void BSD::PollWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->PollImpl(write_buffer, read_buffer, nfds, timeout);
}

void BSD::PollWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SelectWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) =
        bsd->SelectImpl(nfds, timeout, read_in, write_in, error_in, read_out, write_out, error_out);
}

void BSD::SelectWork::Response(HLERequestContext& ctx) {
    if (read_out.size() > 0) {
        ctx.WriteBuffer(read_out, 0);
    }
    if (write_out.size() > 0) {
        ctx.WriteBuffer(write_out, 1);
    }
    if (error_out.size() > 0) {
        ctx.WriteBuffer(error_out, 2);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::AcceptWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->AcceptImpl(fd, write_buffer);
}

void BSD::AcceptWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::ConnectWork::Execute(BSD* bsd) {
    bsd_errno = bsd->ConnectImpl(fd, addr);
}

void BSD::ConnectWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD::RecvWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvImpl(fd, flags, message);
}

void BSD::RecvWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::RecvFromWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvFromImpl(fd, flags, message, addr);
}

void BSD::RecvFromWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message, 0);
    if (!addr.empty()) {
        ctx.WriteBuffer(addr, 1);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(addr.size()));
}

void BSD::SendWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendImpl(fd, flags, message);
}

void BSD::SendWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SendToWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendToImpl(fd, flags, message, addr);
}

void BSD::SendToWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::RegisterClient(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    // Read LibraryConfigData structure
    struct LibraryConfigData {
        u32 version;
        u32 tcp_tx_buf_size;
        u32 tcp_rx_buf_size;
        u32 tcp_tx_buf_max_size;
        u32 tcp_rx_buf_max_size;
        u32 udp_tx_buf_size;
        u32 udp_rx_buf_size;
        u32 sb_efficiency;
    };

    const auto config = rp.PopRaw<LibraryConfigData>();
    const u64 transfer_memory_size = rp.Pop<u64>();
    [[maybe_unused]] const auto transfer_memory_handle = ctx.GetCopyHandle(0);
    const u64 pid = ctx.GetPID();

    LOG_INFO(Service, "called, version={} pid={} transfer_memory_size={:#x}",
             config.version, pid, transfer_memory_size);
    LOG_DEBUG(Service, "  TCP: tx={:#x} rx={:#x} tx_max={:#x} rx_max={:#x}",
              config.tcp_tx_buf_size, config.tcp_rx_buf_size,
              config.tcp_tx_buf_max_size, config.tcp_rx_buf_max_size);
    LOG_DEBUG(Service, "  UDP: tx={:#x} rx={:#x} sb_efficiency={}",
              config.udp_tx_buf_size, config.udp_rx_buf_size, config.sb_efficiency);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD::StartMonitoring(HLERequestContext& ctx) {
    LOG_INFO(Service, "called");

    // StartMonitoring initializes network event monitoring for BSD sockets
    // This command has no documented input parameters in switchbrew
    // It enables proper event handling for socket operations
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void BSD::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    const auto [fd, bsd_errno] = SocketImpl(static_cast<Domain>(domain), static_cast<Type>(type),
                                            static_cast<Protocol>(protocol));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD::Select(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    const auto read_set = [&ctx](size_t index) {
        const auto live = ctx.CanReadBuffer(index) ? ctx.ReadBuffer(index) : std::span<const u8>{};
        return std::vector<u8>(live.begin(), live.end());
    };

    // [OpenPak] The parked select (Ryujinx IClient.cs Select), as Eden has it. A select with an
    // event fd in its sets is ended by another guest thread's Write() to that event fd, which a
    // wait held on this thread would keep queued behind itself. So: one non-blocking pass, and
    // with nothing ready the request is parked the way a deferred Poll is, re-run by the
    // deferral heartbeat until something is ready or the park window is over. The window is
    // fixed, not the guest's timeout: that argument is a 16-byte timeval read here as one s32.
    constexpr auto SelectParkWindow = std::chrono::milliseconds{100};

    DeferredSelectState state;
    bool had_snapshot = false;
    {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        if (const auto it = deferred_select_snapshots.find(&ctx);
            it != deferred_select_snapshots.end()) {
            state = it->second;
            had_snapshot = true;
        }
    }
    if (!had_snapshot) {
        state.read_in = read_set(0);
        state.write_in = read_set(1);
        state.error_in = read_set(2);
    }

    const bool parkable =
        nfds > 0 && GetBsdDeferralEvent() != nullptr &&
        SelectSetIncludesEventFd(state.read_in, state.write_in, state.error_in);

    SelectWork work{
        .nfds = nfds,
        .timeout = parkable ? 0 : timeout,
        .read_in = state.read_in,
        .write_in = state.write_in,
        .error_in = state.error_in,
        .read_out = std::vector<u8>(ctx.GetWriteBufferSize(0)),
        .write_out = std::vector<u8>(ctx.GetWriteBufferSize(1)),
        .error_out = std::vector<u8>(ctx.GetWriteBufferSize(2)),
    };
    work.Execute(this);

    if (parkable && work.ret == 0 && work.bsd_errno == Errno::SUCCESS) {
        if (!had_snapshot) {
            // Nothing ready yet: keep the sets as they were read, since the re-run must not
            // read guest memory again, and give the thread up.
            state.deadline = std::chrono::steady_clock::now() + SelectParkWindow;
            std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
            deferred_select_snapshots[&ctx] = std::move(state);
            ctx.SetIsDeferred();
            return;
        }
        if (std::chrono::steady_clock::now() < state.deadline) {
            ctx.SetIsDeferred();
            return;
        }
        // The window is over: answered below as 0 with no error, the three sets cleared.
    }

    if (had_snapshot) {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        deferred_select_snapshots.erase(&ctx);
    }
    work.Response(ctx);
}

void BSD::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    // [OpenPak] See sockets.h's deferred_poll_snapshots declaration comment: if this ctx was
    // already deferred once, reuse the pollfd bytes captured back then instead of re-reading
    // live guest memory now, which another IPC call could have reused in the meantime.
    std::vector<u8> read_buffer;
    bool had_snapshot = false;
    std::optional<std::chrono::steady_clock::time_point> existing_deadline;
    {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        if (const auto it = deferred_poll_snapshots.find(&ctx); it != deferred_poll_snapshots.end()) {
            read_buffer = it->second.read_buffer;
            existing_deadline = it->second.deadline;
            had_snapshot = true;
        }
    }
    if (!had_snapshot) {
        const auto live_buffer = ctx.ReadBuffer();
        read_buffer.assign(live_buffer.begin(), live_buffer.end());
    }

    // [OpenPak] See sockets.h's SetBsdDeferralEvent declaration comment. Covers any nonzero
    // timeout (matching Ryujinx-Reference's own `timeout != 0` condition), not just an infinite
    // one -- a bounded wait's own deadline is tracked in DeferredPollState and enforced below on
    // each re-check, so deferring it can't silently turn it into an unbounded wait. Also requires
    // an eventfd in the set and a deferral event to wait on; an ordinary title's poll (no eventfd,
    // e.g. Splatoon 2/MK8/SSBU) is untouched and still blocks exactly as before, matching
    // Ryujinx-Reference's own NEX-vs-gRPC split for this same failure.
    if (timeout != 0 && GetBsdDeferralEvent() != nullptr &&
        PollSetIncludesEventFd(read_buffer, nfds)) {
        std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
        auto [ret, bsd_errno] = PollImpl(write_buffer, read_buffer, nfds, /*timeout=*/0);

        // [OpenPak] Deadline expired while nothing became ready -- answer zero ready
        // descriptors instead of deferring again, exactly like a real bounded poll() once its
        // time is up. [OpenPak] The pollfd array goes back too, every revents cleared, as Eden
        // has it: the guest reads it whatever the count says.
        if (ret == 0 && bsd_errno == Errno::SUCCESS && existing_deadline &&
            std::chrono::steady_clock::now() >= *existing_deadline) {
            std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
            deferred_poll_snapshots.erase(&ctx);
            if (write_buffer.size() > 0) {
                ctx.WriteBuffer(write_buffer);
            }
            IPC::ResponseBuilder rb{ctx, 4};
            rb.Push(ResultSuccess);
            rb.Push<s32>(0);
            rb.PushEnum(Errno::SUCCESS);
            return;
        }

        if (ret == 0 && bsd_errno == Errno::SUCCESS) {
            // Nothing ready yet -- give up this thread instead of blocking it, so BSD's other
            // worker threads (and this one) stay free to service the eventfd Write() IPC that
            // would end this wait. ServerManager::CompleteSyncRequest re-invokes this handler
            // (re-parsing the same, still-buffered request) whenever the deferral event fires.
            if (!had_snapshot) {
                std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
                const auto deadline = timeout == -1
                                          ? std::optional<std::chrono::steady_clock::time_point>{}
                                          : std::optional{std::chrono::steady_clock::now() +
                                                           std::chrono::milliseconds(timeout)};
                deferred_poll_snapshots[&ctx] = DeferredPollState{read_buffer, deadline};
            }
            LOG_DEBUG(Service, "[OpenPak] Poll deferred (nfds={} timeout={}), eventfd in set",
                      nfds, timeout);
            ctx.SetIsDeferred();
            return;
        }
        // Completing now (successfully or with an error) -- drop the snapshot, if any.
        if (had_snapshot) {
            std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
            deferred_poll_snapshots.erase(&ctx);
        }
        // Already had something to report (or an error) -- same response shape as the normal
        // (non-deferred) path below, just without going through Network::Poll's own blocking
        // wait since PollImpl above already did the check.
        if (write_buffer.size() > 0) {
            ctx.WriteBuffer(write_buffer);
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(bsd_errno);
        return;
    }

    // [OpenPak] Not taking the deferred path this call -- make sure no stale snapshot lingers
    // under this ctx pointer (defensive: guards against an HLERequestContext address ever being
    // reused for a genuinely different, later request).
    if (had_snapshot) {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        deferred_poll_snapshots.erase(&ctx);
    }

    ExecuteWork(ctx, PollWork{
                         .nfds = nfds,
                         .timeout = timeout,
                         .read_buffer = read_buffer,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    ExecuteWork(ctx, AcceptWork{
                         .fd = fd,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, BindImpl(fd, ctx.ReadBuffer()));
}

void BSD::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, ConnectWork{
                         .fd = fd,
                         .addr = ctx.ReadBuffer(),
                     });
}

void BSD::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetPeerNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetSockNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const auto optname = static_cast<OptName>(rp.Pop<u32>());

    std::vector<u8> optval(ctx.GetWriteBufferSize());

    LOG_DEBUG(Service, "called. fd={} level={} optname=0x{:x} len=0x{:x}", fd, level, optname,
              optval.size());

    const Errno err = GetSockOptImpl(fd, level, optname, optval);

    ctx.WriteBuffer(optval);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(err == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(err);
    rb.Push<u32>(static_cast<u32>(optval.size()));
}

void BSD::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    BuildErrnoResponse(ctx, ListenImpl(fd, backlog));
}

void BSD::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 cmd = rp.Pop<u32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    const auto [ret, bsd_errno] = FcntlImpl(fd, static_cast<FcntlCmd>(cmd), arg);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const OptName optname = static_cast<OptName>(rp.Pop<u32>());
    const auto optval = ctx.ReadBuffer();

    LOG_DEBUG(Service, "called. fd={} level={} optname=0x{:x} optlen={}", fd, level,
              static_cast<u32>(optname), optval.size());

    const Errno bsd_errno = SetSockOptImpl(fd, level, optname, optval);
    if (bsd_errno != Errno::SUCCESS) {
        // [OpenPak] Named, because a bare "not supported" says nothing about whether it matters.
        // Ported from Eden.
        LOG_WARNING(Service, "setsockopt fd={} level={:#x} optname={:#x} ({} bytes) failed: {}",
                    fd, level, static_cast<u32>(optname), optval.size(),
                    static_cast<u32>(bsd_errno));
    }
    BuildErrnoResponse(ctx, bsd_errno);
}

void BSD::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    BuildErrnoResponse(ctx, ShutdownImpl(fd, how));
}

void BSD::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={}", fd, flags, ctx.GetWriteBufferSize());

    if (DeferBlockingReceive(ctx, fd, flags)) {
        return;
    }

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={} addrlen={}", fd, flags,
              ctx.GetWriteBufferSize(0), ctx.GetWriteBufferSize(1));

    if (DeferBlockingReceive(ctx, fd, flags)) {
        return;
    }

    ExecuteWork(ctx, RecvFromWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .addr = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                     });
}

void BSD::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={}", fd, flags, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    ExecuteWork(ctx, SendToWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(0),
                         .addr = ctx.ReadBuffer(1),
                     });
}

void BSD::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = 0,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetWriteBufferSize());

    std::vector<u8> message(ctx.GetWriteBufferSize());
    const auto [ret, bsd_errno] = RecvImpl(fd, 0, message);
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    BuildErrnoResponse(ctx, CloseImpl(fd));
}

void BSD::DuplicateSocket(HLERequestContext& ctx) {
    struct InputParameters {
        s32 fd;
        u64 reserved;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    struct OutputParameters {
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x8);

    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    // [OpenPak] bsd:u may not duplicate a socket (Ryujinx IClient.cs, DuplicateSocket).
    if (is_user) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.PushRaw(OutputParameters{
            .ret = -1,
            .bsd_errno = Errno::NOENT,
        });
        return;
    }

    Expected<s32, Errno> res = DuplicateSocketImpl(input.fd);
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .ret = res.value_or(0),
        .bsd_errno = res ? Errno::SUCCESS : res.error(),
    });
}

void BSD::EventFd(HLERequestContext& ctx) {
    // EventFd(nn::socket::EventFdFlags flags, u64 initval): flags first, four bytes of padding,
    // then the 64-bit initial value, as Ryujinx and Eden read it. Read the other way round the
    // counter starts at whatever the flags were and the flags are lost, so an event fd is born
    // already signalled and never non-blocking.
    IPC::RequestParser rp{ctx};
    const u32 flags = rp.Pop<u32>();
    rp.Pop<u32>(); // padding
    const u64 initval = rp.Pop<u64>();

    LOG_DEBUG(Service, "called. initval={} flags={}", initval, flags);

    // [OpenPak] The counter is the event fd, as Eden has it: a write adds to it, a read takes it,
    // and Poll answers from it. The loopback socket behind it carries nothing; it is there so the
    // descriptor closes, duplicates and answers fcntl like any other.
    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::MFILE);
        return;
    }

    auto socket = std::make_shared<Network::Socket>();
    Network::Errno net_err = socket->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                                Network::Protocol::UDP);
    const Network::SockAddrIn loopback{
        .family = Network::Domain::INET,
        .ip = {127, 0, 0, 1},
        .portno = 0,
    };
    if (net_err == Network::Errno::SUCCESS) {
        net_err = socket->Bind(loopback);
    }
    Network::SockAddrIn bound{};
    if (net_err == Network::Errno::SUCCESS) {
        std::tie(bound, net_err) = socket->GetSockName();
    }
    if (net_err == Network::Errno::SUCCESS) {
        bound.ip = loopback.ip;
        net_err = socket->Connect(bound);
    }
    if (net_err != Network::Errno::SUCCESS) {
        LOG_ERROR(Service, "Failed to create eventfd backing socket, errno={}",
                  static_cast<int>(net_err));
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Translate(net_err));
        return;
    }

    file_descriptors[fd] = FileDescriptor{};
    FileDescriptor& descriptor = *file_descriptors[fd];
    descriptor.socket = std::move(socket);
    descriptor.domain = Network::Domain::INET;
    descriptor.type = Network::Type::DGRAM;
    descriptor.protocol = Network::Protocol::UDP;
    descriptor.is_connection_based = false;
    descriptor.event_value = std::make_shared<std::atomic<u64>>(initval);

    // [OpenPak] nn::socket::EventFdFlags: Semaphore = 1, NonBlocking = 4. An event fd never
    // blocks whatever the flags say: a read with nothing pending answers EAGAIN instead of
    // parking a bsdsocket worker thread (Ryujinx EventFileDescriptor.cs).
    constexpr u32 EventFdSemaphore = 1u << 0;
    descriptor.event_semaphore = (flags & EventFdSemaphore) != 0;
    if (descriptor.socket->SetNonBlock(true) == Network::Errno::SUCCESS) {
        descriptor.flags |= Network::FLAG_O_NONBLOCK;
    }

    LOG_INFO(Service, "[OpenPak] New eventfd fd={} initval={}", fd, initval);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(Errno::SUCCESS);
}

void BSD::RegisterClientShared(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RegisterClientShared");
    IPC::ResponseBuilder rb{ctx, 4}; // Match RegisterClient response style
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // ret (0 for success)
    rb.Push<s32>(0); // BSD errno (0 for success, consistent with RegisterClient stub)
}

template <typename Work>
void BSD::ExecuteWork(HLERequestContext& ctx, Work work) {
    work.Execute(this);
    work.Response(ctx);
}

std::pair<s32, Errno> BSD::SocketImpl(Domain domain, Type type, Protocol protocol) {
    // Horizon/libnx defines these as socket-creation flags OR'ed into the base type.
    // Preserve SOCK_NONBLOCK instead of merely stripping it: otherwise a guest polling
    // recvfrom() becomes a literal blocking host call and can stall the title indefinitely.
    const bool socket_nonblock = (static_cast<u32>(type) & 0x20000000) != 0;
    type = static_cast<Type>(static_cast<u32>(type) & ~0x20000000);

    // SOCK_CLOEXEC has no distinct meaning for Citron's emulated descriptor table, but it must
    // not be passed to Translate(Type) as part of the base socket type.
    type = static_cast<Type>(static_cast<u32>(type) & ~0x10000000);

    // [OpenPak] With the flags gone some titles are left asking for Type::Unspecified, which
    // Translate() cannot map, so the call could never succeed for anybody. Risk of Rain 2 asks for
    // exactly that -- 0x10000000, SOCK_CLOEXEC over a base type of 0 -- right after resolving
    // localhost, and on Ryujinx the failure cost the emulator a null dereference ~105 ms later,
    // four runs running, while a real Switch played on. Mapping it to STREAM is what got Ryujinx
    // into an RoR2 lobby (2026-09-26, measured). It cannot regress a working title: the
    // alternative for Unspecified is a call that always fails.
    //
    // STREAM was wrong, and the guest said so (openpak/ryujinx 989a12600): mapped that way the
    // socket binds tcp/7777 -- Mirror's default port -- and the very next call is recvfrom, which
    // answers ENOTCONN because a listening TCP socket has nothing to receive from. The title took
    // the error, never called accept, and played on alone. socket + bind + recvfrom with no accept
    // is a UDP server, so DGRAM is the answer.
    if (type == Type::Unspecified) {
        LOG_INFO(Service, "socket type 0 -> DGRAM (base type unset by the guest)");
        type = Type::DGRAM;
    }

    // [OpenPak] bsd:u may not open SEQPACKET or RAW sockets, the ICMP ping socket excepted
    // (Ryujinx IClient.cs, SocketInternal).
    if (is_user && (type == Type::SEQPACKET || type == Type::RAW)) {
        if (domain != Domain::INET || type != Type::RAW || protocol != Protocol::ICMP) {
            return {-1, Errno::NOENT};
        }
    }

    // [OpenPak] In airplane mode a stream socket is refused before it takes a descriptor, as
    // Eden has it.
    if (Settings::values.airplane_mode.GetValue() && IsConnectionBased(type)) {
        LOG_ERROR(Service, "Airplane mode is enabled, cannot create socket");
        return {-1, Errno::NOTCONN};
    }

    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    file_descriptors[fd] = FileDescriptor{};
    FileDescriptor& descriptor = *file_descriptors[fd];
    // ENONMEM might be thrown here

    auto room_member = room_network.GetRoomMember().lock();
    const bool using_proxy = room_member && room_member->IsConnected();

    LOG_INFO(Service, "New socket fd={} domain={} type={} protocol={} proxy={}",
             fd, domain, type, protocol, using_proxy);

    // Store socket type information for pooling
    descriptor.domain = Translate(domain);
    descriptor.type = Translate(type);
    descriptor.protocol = Translate(protocol);
    descriptor.is_connection_based = IsConnectionBased(type);

    if (using_proxy) {
        descriptor.socket = std::make_shared<Network::ProxySocket>(room_network);
        descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
        LOG_DEBUG(Service, "Created new ProxySocket for fd={}", fd);
    } else {
        descriptor.socket = std::make_shared<Network::Socket>();
        // [OpenPak] Initialize's Errno must not be dropped. A guest that tests `fd < 0` reads the
        // descriptor we would otherwise hand back as a perfectly good socket, then calls methods on
        // one that was never opened. Measured on Ryujinx with Risk of Rain 2 (2026-09-26), which
        // asks for socket type 0x10000000 -- SOCK_CLOEXEC over a base type of 0: the host rejects
        // it, and ~100 ms after the emulator reported success the title dereferenced null and died.
        // Citron reaches the same end by a shorter road, because it never looked at the result.
        const Network::Errno init_errno =
            descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
        if (init_errno != Network::Errno::SUCCESS) {
            LOG_ERROR(Service, "Socket creation failed for fd={} domain={} type={} protocol={}", fd,
                      domain, type, protocol);
            file_descriptors[fd].reset();
            return {-1, Translate(init_errno)};
        }
        if (type == Type::DGRAM) {
            // Guest P2P (Pia) traffic can arrive as a burst of several large datagrams within
            // single-digit milliseconds of each other (observed: a friend-island-visit payload
            // delivered as ~5 fragments up to 880 bytes each, back-to-back). The OS default
            // SO_RCVBUF is small enough that a burst like this can silently overflow it before
            // the guest's own poll loop drains it, dropping datagrams with no trace on either
            // side. Request a generous buffer up front; this only widens headroom and cannot
            // change any protocol behavior, so it's safe even if this guess turns out wrong.
            constexpr u32 kGenerousUdpRcvBuf = 1024 * 1024;
            descriptor.socket->SetRcvBuf(kGenerousUdpRcvBuf);
        }
    }

    if (socket_nonblock) {
        const auto nonblock_errno = descriptor.socket->SetNonBlock(true);
        if (nonblock_errno != Network::Errno::SUCCESS) {
            file_descriptors[fd].reset();
            return {-1, Translate(nonblock_errno)};
        }
        descriptor.flags |= Network::FLAG_O_NONBLOCK;
    }

    return {fd, Errno::SUCCESS};
}

bool BSD::PollSetIncludesEventFd(std::span<const u8> read_buffer, s32 nfds) const {
    if (nfds <= 0 || read_buffer.size() < static_cast<size_t>(nfds) * sizeof(PollFD)) {
        return false;
    }
    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));
    for (const PollFD& pollfd : fds) {
        if (pollfd.fd < 0 || pollfd.fd >= static_cast<s32>(MAX_FD)) {
            continue;
        }
        const auto& descriptor = file_descriptors[pollfd.fd];
        if (descriptor && descriptor->event_value) {
            return true;
        }
    }
    return false;
}

std::pair<s32, Errno> BSD::PollImpl(std::vector<u8>& write_buffer, std::span<const u8> read_buffer,
                                    s32 nfds, s32 timeout) {
    if (nfds <= 0) {
        // poll(NULL, 0, timeout) is a portable sleep idiom titles use for pacing/backoff
        // between retries. Real poll() actually blocks for the requested duration; honor
        // that here instead of returning instantly, or a title's own retry-count budget
        // burns through in microseconds instead of the real time it was paced for.
        if (timeout > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        }
        // [OpenPak] With no entries the wait ends with nothing ready: 0 and no error (Ryujinx
        // IClient.cs, Poll).
        return {0, Errno::SUCCESS};
    }
    if (read_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }
    if (write_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }

    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));

    // Initialize revents to zero to ensure clean state
    for (PollFD& pollfd : fds) {
        pollfd.revents = PollEvents{};
    }

    if (timeout >= 0) {
        const s64 seconds = timeout / 1000;
        const u64 nanoseconds = 1'000'000 * (static_cast<u64>(timeout) % 1000);

        if (seconds < 0) {
            return {-1, Errno::INVAL};
        }
        if (nanoseconds > 999'999'999) {
            return {-1, Errno::INVAL};
        }
    } else if (timeout != -1) {
        return {-1, Errno::INVAL};
    }

    // [OpenPak] A dead/invalid fd used to fail the WHOLE poll call (return {0, SUCCESS}
    // immediately, discarding every other entry's real state) the instant it was found, even
    // when other fds in the same set -- the eventfd gRPC polls alongside its socket, say -- were
    // genuinely ready. Real poll() reports POLLNVAL on just THAT entry's line and still evaluates
    // the rest. Matches Ryujinx-Reference's own fix for this exact bug (ServerBase.cs's
    // IClient.Poll dead-descriptor handling).
    std::vector<bool> is_valid(fds.size(), true);
    bool any_invalid = false;
    for (size_t i = 0; i < fds.size(); ++i) {
        PollFD& pollfd = fds[i];
        ASSERT(False(pollfd.revents));

        if (pollfd.fd >= static_cast<s32>(MAX_FD) || pollfd.fd < 0) {
            LOG_ERROR(Service, "File descriptor handle={} is invalid", pollfd.fd);
            is_valid[i] = false;
            any_invalid = true;
            continue;
        }

        const std::optional<FileDescriptor>& descriptor = file_descriptors[pollfd.fd];
        if (!descriptor) {
            LOG_TRACE(Service, "File descriptor handle={} is not allocated", pollfd.fd);
            is_valid[i] = false;
            any_invalid = true;
        }
    }

    // Only valid entries get a real host-side poll; invalid ones are reported directly below.
    std::vector<size_t> valid_indices;
    std::vector<Network::PollFD> host_pollfds;
    bool has_event_fd = false;
    bool event_ready = false;
    for (size_t i = 0; i < fds.size(); ++i) {
        if (!is_valid[i]) {
            continue;
        }
        const FileDescriptor& descriptor = *file_descriptors[fds[i].fd];
        if (descriptor.event_value) {
            // [OpenPak] An event fd is answered from its counter, never from the host, as Eden
            // has it. Poll only observes: the read that follows takes the counter.
            //
            // [OpenPak] gRPC polls its wakeup eventfd with a zero event mask (valid POSIX:
            // normally means "only tell me about errors"), but expects readability to be
            // reported anyway once it writes that eventfd. Matches Ryujinx-Reference's
            // EventFileDescriptorPollManager fix for the same grpc behavior -- without it this
            // poll never reports ready and grpc's wakeup loop stalls.
            has_event_fd = true;
            const bool wants_in =
                True(fds[i].events & PollEvents::In) || fds[i].events == PollEvents{};
            if (wants_in && descriptor.event_value->load() > 0) {
                fds[i].revents = PollEvents::In;
                event_ready = true;
            }
            continue;
        }
        valid_indices.push_back(i);
        Network::PollFD entry;
        entry.socket = descriptor.socket.get();
        entry.events = Translate(fds[i].events);
        entry.revents = Network::PollEvents{};
        host_pollfds.push_back(entry);
    }

    // [OpenPak] The bsdsocket threads are shared with every send and receive of the title, so a
    // long poll must not hold up a datagram transport: with a DGRAM socket open and no event fd
    // in the set, the wait is cut to 5 ms and an empty pass answers 0 with no error (Ryujinx
    // IClient.cs, ConcurrentUdpPollSliceMs). Without one the poll blocks as it was asked to.
    constexpr s32 ConcurrentUdpPollSliceMs = 5;
    s32 host_timeout = (any_invalid || event_ready) ? 0 : timeout;
    if (!has_event_fd && (host_timeout == -1 || host_timeout > ConcurrentUdpPollSliceMs) &&
        std::ranges::any_of(file_descriptors, [](const std::optional<FileDescriptor>& open) {
            return open && !open->event_value && open->type == Network::Type::DGRAM;
        })) {
        host_timeout = ConcurrentUdpPollSliceMs;
    }

    // host_pollfds.empty() happens when every entry was invalid or an event fd: nothing to
    // host-poll for, and nothing to wait out here.
    std::pair<s32, Network::Errno> result{0, Network::Errno::SUCCESS};
    if (!host_pollfds.empty()) {
        result = Network::Poll(host_pollfds, host_timeout);
    }

    for (size_t j = 0; j < valid_indices.size(); ++j) {
        const size_t i = valid_indices[j];
        fds[i].revents = Translate(host_pollfds[j].revents);
    }

    s32 real_count = 0;
    for (size_t i = 0; i < fds.size(); ++i) {
        if (!is_valid[i]) {
            fds[i].revents = PollEvents::Nval;
        }
        if (fds[i].revents != PollEvents{}) {
            ++real_count;
        }
    }
    std::memcpy(write_buffer.data(), fds.data(), nfds * sizeof(PollFD));

    // An event fd found ready is a success however the host poll of the rest turned out.
    if (host_pollfds.empty() || event_ready) {
        return {real_count, Errno::SUCCESS};
    }
    if (result.second != Network::Errno::SUCCESS) {
        return Translate(result);
    }
    return {real_count, Errno::SUCCESS};
}

namespace {
// fd_set is a plain byte array, bit i (LSB-first within each byte) == fd i.
void ExtractFdsFromMask(std::span<const u8> mask, std::vector<s32>& out) {
    for (size_t byte_idx = 0; byte_idx < mask.size(); ++byte_idx) {
        const u8 current = mask[byte_idx];
        for (int bit = 0; bit < 8; ++bit) {
            if (current & (1u << bit)) {
                out.push_back(static_cast<s32>(byte_idx * 8 + bit));
            }
        }
    }
}

void SetFdInMask(std::vector<u8>& mask, s32 fd) {
    const size_t byte_idx = static_cast<size_t>(fd) / 8;
    if (byte_idx < mask.size()) {
        mask[byte_idx] |= static_cast<u8>(1u << (fd % 8));
    }
}
} // Anonymous namespace

// [OpenPak] Whether any of a select's three sets names an event fd, which is what makes it one
// to park rather than wait on (see Select).
bool BSD::SelectSetIncludesEventFd(std::span<const u8> read_in, std::span<const u8> write_in,
                                   std::span<const u8> error_in) const {
    std::vector<s32> fds;
    ExtractFdsFromMask(read_in, fds);
    ExtractFdsFromMask(write_in, fds);
    ExtractFdsFromMask(error_in, fds);
    return std::ranges::any_of(fds, [](s32 fd) {
        return fd < static_cast<s32>(MAX_FD) && file_descriptors[fd] &&
               file_descriptors[fd]->event_value;
    });
}

std::pair<s32, Errno> BSD::SelectImpl(s32 nfds, s32 timeout, std::span<const u8> read_in,
                                      std::span<const u8> write_in, std::span<const u8> error_in,
                                      std::vector<u8>& read_out, std::vector<u8>& write_out,
                                      std::vector<u8>& error_out) {
    std::ranges::fill(read_out, 0);
    std::ranges::fill(write_out, 0);
    std::ranges::fill(error_out, 0);

    std::vector<s32> read_fds;
    std::vector<s32> write_fds;
    std::vector<s32> error_fds;
    ExtractFdsFromMask(read_in, read_fds);
    ExtractFdsFromMask(write_in, write_fds);
    ExtractFdsFromMask(error_in, error_fds);

    if (nfds <= 0 || (read_fds.empty() && write_fds.empty() && error_fds.empty())) {
        return {0, Errno::SUCCESS};
    }

    // One poll entry per unique fd, requesting whichever of In/Out it was asked about.
    // Err/Hup/Nval come back from the host poll() unconditionally, regardless of what
    // was requested, matching POSIX poll() semantics.
    struct Entry {
        s32 fd;
        Network::PollEvents requested{};
    };
    std::vector<Entry> entries;
    const auto add = [&](const std::vector<s32>& fds, Network::PollEvents event) {
        for (const s32 fd : fds) {
            const auto it =
                std::ranges::find_if(entries, [fd](const Entry& e) { return e.fd == fd; });
            if (it != entries.end()) {
                it->requested |= event;
            } else {
                entries.push_back({fd, event});
            }
        }
    };
    add(read_fds, Network::PollEvents::In);
    add(write_fds, Network::PollEvents::Out);

    std::vector<s32> polled_fds;
    std::vector<Network::PollFD> host_pollfds;
    std::vector<s32> event_fds;
    polled_fds.reserve(entries.size());
    host_pollfds.reserve(entries.size());
    s32 ready = 0;
    for (const Entry& entry : entries) {
        if (entry.fd < 0 || entry.fd >= static_cast<s32>(MAX_FD) || !file_descriptors[entry.fd] ||
            !file_descriptors[entry.fd]->socket) {
            continue;
        }
        if (const auto& counter = file_descriptors[entry.fd]->event_value) {
            // [OpenPak] An event fd is answered from its counter, as in PollImpl.
            const bool readable =
                True(entry.requested & Network::PollEvents::In) && counter->load() > 0;
            const bool writable = True(entry.requested & Network::PollEvents::Out);
            if (readable) {
                SetFdInMask(read_out, entry.fd);
            } else if (!writable && True(entry.requested & Network::PollEvents::In)) {
                // Not ready yet: read again after the wait below.
                event_fds.push_back(entry.fd);
            }
            if (writable) {
                SetFdInMask(write_out, entry.fd);
            }
            if (readable || writable) {
                ++ready;
            }
            continue;
        }
        polled_fds.push_back(entry.fd);
        host_pollfds.push_back(Network::PollFD{
            .socket = file_descriptors[entry.fd]->socket.get(),
            .events = entry.requested,
            .revents = Network::PollEvents{},
        });
    }

    // [OpenPak] The host cannot see an event fd being written, so a wait with one in the set is
    // a bounded one, and the counters are read again after it (as Eden has it). This is the path
    // taken only when the select could not be parked (see Select), which asks for no wait at all.
    constexpr s32 EventFdSelectSliceMs = 250;
    s32 host_timeout = timeout;
    if (ready > 0) {
        host_timeout = 0;
    } else if (!event_fds.empty() && (timeout < 0 || timeout > EventFdSelectSliceMs)) {
        host_timeout = EventFdSelectSliceMs;
    }

    const auto [poll_ret, poll_errno] = Translate(Network::Poll(host_pollfds, host_timeout));
    if (poll_errno != Errno::SUCCESS) {
        return {poll_ret, poll_errno};
    }

    for (const s32 fd : event_fds) {
        if (file_descriptors[fd] && file_descriptors[fd]->event_value &&
            file_descriptors[fd]->event_value->load() > 0) {
            SetFdInMask(read_out, fd);
            ++ready;
        }
    }

    // error_fds only ever reports out-of-band/exceptional conditions; a plain closed/errored
    // socket surfaces through the read or write set it was asked about, same as real select().
    constexpr auto err_like =
        Network::PollEvents::Err | Network::PollEvents::Hup | Network::PollEvents::Nval;
    for (size_t i = 0; i < host_pollfds.size(); ++i) {
        const s32 fd = polled_fds[i];
        const Network::PollEvents revents = host_pollfds[i].revents;
        bool counted = false;
        if (True(host_pollfds[i].events & Network::PollEvents::In) &&
            True(revents & (Network::PollEvents::In | err_like))) {
            SetFdInMask(read_out, fd);
            counted = true;
        }
        if (True(host_pollfds[i].events & Network::PollEvents::Out) &&
            True(revents & (Network::PollEvents::Out | err_like))) {
            SetFdInMask(write_out, fd);
            counted = true;
        }
        if (True(revents & (Network::PollEvents::Err | Network::PollEvents::Hup))) {
            SetFdInMask(error_out, fd);
            counted = true;
        }
        if (counted) {
            ++ready;
        }
    }

    return {ready, Errno::SUCCESS};
}

std::pair<s32, Errno> BSD::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    auto [result, bsd_errno] = descriptor.socket->Accept();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return {-1, Translate(bsd_errno)};
    }

    file_descriptors[new_fd] = FileDescriptor{};
    FileDescriptor& new_descriptor = *file_descriptors[new_fd];
    new_descriptor.socket = std::move(result.socket);
    new_descriptor.is_connection_based = descriptor.is_connection_based;
    // [OpenPak] The accepted socket is of the listening one's kind, so SO_TYPE and the datagram
    // checks read it right.
    new_descriptor.domain = descriptor.domain;
    new_descriptor.type = descriptor.type;
    new_descriptor.protocol = descriptor.protocol;

    const SockAddrIn guest_addr_in = Translate(result.sockaddr_in);
    if (write_buffer.size() > sizeof(guest_addr_in)) {
        write_buffer.resize(sizeof(guest_addr_in)); // the IPv4 shape, even on a v6 listener
    }
    PutValue(write_buffer, guest_addr_in);

    return {new_fd, Errno::SUCCESS};
}

Errno BSD::BindImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        LOG_ERROR(Service, "Bind failed: Invalid fd={}", fd);
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    // [OpenPak] Any sockaddr of 16 bytes or more is read; a shorter one is EINVAL (Ryujinx
    // IClient.cs, Bind).
    if (addr.size() < sizeof(SockAddrIn)) {
        return Errno::INVAL;
    }

    // [OpenPak] A 28-byte buffer here is a real sockaddr_in6 (an IPv6 bind) -- Citron doesn't
    // support IPv6 sockets, and misreading its first 16 bytes as a 4-byte IPv4 address plus 8
    // bytes of the IPv6 payload used to silently corrupt every field read below it and spam
    // assertion failures downstream (network.cpp's TranslateFromSockAddrIn: it only knows
    // Domain::INET). Titles that try IPv6 first and fall back to IPv4 on failure (Splatoon 3's
    // gRPC/NPLN stack does) need a clean, real EAFNOSUPPORT here to take that fallback path
    // properly -- garbage/EINVAL from a corrupted parse instead left the whole connection
    // sequence unable to ever complete or definitively fail, looping forever.
    //
    // [OpenPak] An IPv6 sockaddr is now read properly (see ParseGuestSockAddr): the socket is
    // dual-mode, and only a genuine IPv6 address still gets the clean EAFNOSUPPORT.
    const auto parsed_addr = ParseGuestSockAddr(addr);
    if (!parsed_addr) {
        LOG_WARNING(Service, "Bind fd={} with unsupported address (size={}), returning "
                              "EAFNOSUPPORT", fd, addr.size());
        return Errno::AFNOSUPPORT;
    }
    const Network::SockAddrIn host_addr = *parsed_addr;

    LOG_INFO(Service, "Bind fd={} to {}:{}", fd,
             Network::IPv4AddressToRedactedString(host_addr.ip), host_addr.portno);

    FileDescriptor& descriptor = *file_descriptors[fd];
    // [OpenPak] A title rebinds the same fixed port across retries, and without reuse the second
    // bind fails while the first socket is still being torn down (Ryujinx ManagedSocket.cs,
    // Bind). Best effort: a refusal is not the guest's to see.
    if (descriptor.type == Network::Type::DGRAM && host_addr.portno != 0) {
        (void)descriptor.socket->SetReuseAddr(true);
    }

    const auto result = Translate(descriptor.socket->Bind(host_addr));
    if (result != Errno::SUCCESS) {
        LOG_ERROR(Service, "Bind fd={} failed with errno={}", fd, static_cast<int>(result));
    }
    return result;
}

namespace {

// [OpenPak] Whether this address is the OpenPak server itself: the address every redirected name
// resolves to. Those are ours, they are TCP, and they are the ones a title's gRPC stack (NPLN
// above all) dials. Ported from Eden.
bool OpenPakServerTarget(const Network::SockAddrIn& addr) {
    if (!Settings::values.enable_openpak.GetValue()) {
        return false;
    }

    std::string ip = Settings::values.openpak_server_ip.GetValue();
    if (ip.empty()) {
        if (const char* env = std::getenv("OPENPAK_SERVER_IP"); env != nullptr && *env != '\0') {
            ip = env;
        }
    }
    if (ip.empty()) {
        return false;
    }

    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }

    return addr.ip[0] == (a & 0xff) && addr.ip[1] == (b & 0xff) && addr.ip[2] == (c & 0xff) &&
           addr.ip[3] == (d & 0xff);
}

// [OpenPak] A title's gRPC stack creates its first call while the channel is still connecting
// and then polls the socket with a zero event mask -- which can never report POLLOUT -- so a
// connect answered EINPROGRESS leaves the completion undeliverable and the title waits forever.
// A connect aimed at the OpenPak server is waited out instead, for up to 2000 ms, and answers
// SUCCESS when it completes within that; everything else keeps EINPROGRESS. Ported from Eden.
Errno CompleteConnectSync(Network::SocketBase& socket, const Network::SockAddrIn& addr,
                          Errno result) {
    if (result != Errno::INPROGRESS || !OpenPakServerTarget(addr)) {
        return result;
    }

    constexpr s32 connect_wait_ms = 2000;
    constexpr auto completed =
        Network::PollEvents::Out | Network::PollEvents::Err | Network::PollEvents::Hup;
    std::vector<Network::PollFD> wait{
        Network::PollFD{&socket, Network::PollEvents::Out, Network::PollEvents{}}};
    if (Network::Poll(wait, connect_wait_ms).first != 1 || False(wait[0].revents & completed)) {
        return result;
    }

    const auto [pending_err, getsockopt_err] = socket.GetPendingError();
    if (getsockopt_err != Network::Errno::SUCCESS || pending_err != Network::Errno::SUCCESS) {
        return result;
    }

    LOG_INFO(Service, "[OpenPak] Connect to the OpenPak server completed synchronously");
    return Errno::SUCCESS;
}

} // Anonymous namespace

Errno BSD::ConnectImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        LOG_ERROR(Service, "Connect failed: Invalid fd={}", fd);
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    // [OpenPak] Any sockaddr of 16 bytes or more is read; a shorter one is EINVAL (Ryujinx
    // IClient.cs, Connect).
    if (addr.size() < sizeof(SockAddrIn)) {
        return Errno::INVAL;
    }

    // [OpenPak] See BindImpl: a v4-mapped IPv6 sockaddr is dialled through the dual-mode
    // socket; only a genuine IPv6 address gets the clean EAFNOSUPPORT rejection.
    const auto parsed_addr = ParseGuestSockAddr(addr);
    if (!parsed_addr) {
        LOG_WARNING(Service, "Connect fd={} with unsupported address (size={}), returning "
                              "EAFNOSUPPORT", fd, addr.size());
        return Errno::AFNOSUPPORT;
    }
    const Network::SockAddrIn translated_addr = *parsed_addr;

    LOG_INFO(Service, "Connect fd={} to {}:{}{}", fd,
             Network::IPv4AddressToRedactedString(translated_addr.ip), translated_addr.portno,
             addr[1] == static_cast<u8>(Domain::INET6) ? " (v4-mapped IPv6)" : "");

    Network::SocketBase& socket = *file_descriptors[fd]->socket;
    Errno result = Translate(socket.Connect(translated_addr));
    result = CompleteConnectSync(socket, translated_addr, result);

    // [OpenPak] A connect on a socket that is already connected is a success, as Eden has it.
    if (result == Errno::ISCONN) {
        LOG_DEBUG(Service, "Connect fd={} returned ISCONN - socket already connected", fd);
        return Errno::SUCCESS;
    }
    if (result != Errno::SUCCESS) {
        LOG_ERROR(Service, "Connect fd={} failed with errno={}", fd, static_cast<int>(result));
    } else {
        LOG_INFO(Service, "Connect fd={} succeeded", fd);
    }
    return result;
}

Errno BSD::GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetPeerName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    // [OpenPak] A buffer shorter than a sockaddr_in takes what fits, not an assert (as Eden).
    if (write_buffer.size() > sizeof(guest_addrin)) {
        write_buffer.resize(sizeof(guest_addrin));
    }
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD::GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetSockName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    // [OpenPak] A buffer shorter than a sockaddr_in takes what fits, not an assert (as Eden).
    if (write_buffer.size() > sizeof(guest_addrin)) {
        write_buffer.resize(sizeof(guest_addrin));
    }
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD::ListenImpl(s32 fd, s32 backlog) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    return Translate(file_descriptors[fd]->socket->Listen(backlog));
}

std::pair<s32, Errno> BSD::FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};

    FileDescriptor& descriptor = *file_descriptors[fd];

    switch (cmd) {
    case FcntlCmd::GETFL:
        // ponytail: F_GETFL ignores its third arg per POSIX; Outbound/Fusion's transport passes a
        // non-zero one during gameplay-session socket setup. Don't ASSERT (it spammed Critical and is
        // spurious) -- just return the flags.
        return {descriptor.flags, Errno::SUCCESS};
    case FcntlCmd::SETFL: {
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        const Errno bsd_errno = Translate(descriptor.socket->SetNonBlock(enable));
        if (bsd_errno != Errno::SUCCESS) {
            return {-1, bsd_errno};
        }
        descriptor.flags = arg;
        return {0, Errno::SUCCESS};
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented cmd={}", cmd);
        return {-1, Errno::SUCCESS};
    }
}

namespace {
u64 FeignedSockOptKey(u32 level, OptName optname) {
    return static_cast<u64>(level) << 32 | static_cast<u32>(optname);
}
} // Anonymous namespace

void BSD::RememberFeignedSockOpt(FileDescriptor& descriptor, u32 level, OptName optname,
                                 std::span<const u8> optval) {
    descriptor.feigned_sockopts[FeignedSockOptKey(level, optname)].assign(optval.begin(),
                                                                          optval.end());
}

void BSD::EchoFeignedSockOpt(const FileDescriptor& descriptor, u32 level, OptName optname,
                             std::vector<u8>& optval) {
    const auto stored = descriptor.feigned_sockopts.find(FeignedSockOptKey(level, optname));
    if (stored != descriptor.feigned_sockopts.end() && !stored->second.empty()) {
        optval.resize(std::min(optval.size(), stored->second.size()));
        std::copy_n(stored->second.begin(), optval.size(), optval.begin());
        return;
    }
    LOG_WARNING(Service, "(STUBBED) getsockopt level={:#x} optname={:#x} never set, echoing zeros",
                level, static_cast<u32>(optname));
    std::ranges::fill(optval, 0);
}

Errno BSD::GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    // [OpenPak] IPPROTO_TCP / TCP_NODELAY. A portable networking stack (Splatoon 3's
    // gRPC/NintendoSDK_NPLN stack does this) commonly disables Nagle's algorithm on connect
    // and reads it back to confirm before trusting the socket -- the generic INVAL below used
    // to fail that check and made the game abandon an otherwise-fine socket, retrying the
    // whole connection sequence from scratch forever. See SocketBase::GetNoDelay's declaration
    // comment in internal_network/sockets.h.
    if (level == 6 && static_cast<u32>(optname) == 1) {
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        auto [enabled, getsockopt_err] = file_descriptors[fd]->socket->GetNoDelay();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            optval.resize(sizeof(u32));
            PutValue(optval, static_cast<u32>(enabled ? 1 : 0));
        }
        return Translate(getsockopt_err);
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        // [OpenPak] SetSockOptImpl already tolerates any level it doesn't specifically implement
        // (returns SUCCESS, matching Ryujinx-Reference's own generic tolerant fallback) -- this must
        // do the same rather than INVAL, or a title that sets an option at some other level (e.g.
        // IPPROTO_IPV6 on a dual-stack socket) and reads it straight back to confirm sees a set-ok/
        // get-fails mismatch. Ryujinx-Reference's own comment on this exact asymmetry: "grpc-core
        // sets TCP_NODELAY / SO_REUSEADDR / etc. and reads them straight back; a set-ok / get-
        // EOPNOTSUPP mismatch made it close before connect." Echo back zeroed bytes of the
        // requested size rather than failing outright.
        EchoFeignedSockOpt(*file_descriptors[fd], level, optname, optval);
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    // [OpenPak] See GetReuseAddr's declaration comment in internal_network/sockets.h --
    // REUSEADDR/KEEPALIVE/BROADCAST used to have a setter but no matching getter, so a title
    // that verifies one of these right after setting it (Splatoon 3's gRPC/NintendoSDK_NPLN
    // stack does) always got INVAL back and abandoned an otherwise-fine socket.
    //
    // [OpenPak] A buffer of four bytes or more takes the value, as Eden has it.
    auto write_u32_opt = [&](std::pair<u32, Network::Errno> result) -> Errno {
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        auto [value, getsockopt_err] = result;
        if (getsockopt_err == Network::Errno::SUCCESS) {
            optval.resize(sizeof(u32));
            PutValue(optval, value);
        }
        return Translate(getsockopt_err);
    };
    auto write_bool_opt = [&](std::pair<bool, Network::Errno> result) -> Errno {
        return write_u32_opt({result.first ? 1u : 0u, result.second});
    };

    // [OpenPak] SO_LINGER. A title that sets this and reads it back to confirm it took
    // previously got INVAL, same failure class as the TCP_NODELAY/REUSEADDR gaps above.
    if (optname == OptName::LINGER) {
        if (optval.size() < sizeof(Linger)) {
            return Errno::INVAL;
        }
        auto [onoff, linger, getsockopt_err] = socket->GetLinger();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            optval.resize(sizeof(Linger));
            PutValue(optval, Linger{.onoff = onoff ? 1u : 0u, .linger = linger});
        }
        return Translate(getsockopt_err);
    }

    switch (optname) {
    case OptName::ERROR_: {
        if (optval.size() < sizeof(Errno)) {
            return Errno::INVAL;
        }
        auto [pending_err, getsockopt_err] = socket->GetPendingError();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            Errno translated_pending_err = Translate(pending_err);
            optval.resize(sizeof(Errno));
            PutValue(optval, translated_pending_err);
        }
        return Translate(getsockopt_err);
    }
    case OptName::REUSEADDR:
        return write_bool_opt(socket->GetReuseAddr());
    case OptName::KEEPALIVE:
        return write_bool_opt(socket->GetKeepAlive());
    case OptName::BROADCAST:
        return write_bool_opt(socket->GetBroadcast());
    // [OpenPak] Read from the host, not echoed from what was set, as Eden has it.
    case OptName::SNDBUF:
        return write_u32_opt(socket->GetSndBuf());
    case OptName::RCVBUF:
        return write_u32_opt(socket->GetRcvBuf());
    case OptName::SNDTIMEO:
        return write_u32_opt(socket->GetSndTimeo());
    case OptName::RCVTIMEO:
        return write_u32_opt(socket->GetRcvTimeo());
    case OptName::TYPE:
        return write_u32_opt(socket->GetSocketType());
    default:
        // [OpenPak] Everything the set side tolerated without applying (NOSIGPIPE and any option
        // this build does not know, Nintendo's 0x80000001 among them) is echoed back as it was
        // set -- zeros if it never was -- with SUCCESS. A title's network stack (NPLN's gRPC
        // above all) sets an option and reads it straight back before trusting the socket; a
        // set that answers SUCCESS and a get that answers INVAL reads as a broken socket and
        // the connection is abandoned. Ported from Eden.
        EchoFeignedSockOpt(*file_descriptors[fd], level, optname, optval);
        return Errno::SUCCESS;
    }
}

Errno BSD::SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval) {
    if (!IsFileDescriptorValid(fd))
        return Errno::BADF;
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    // [OpenPak] The value as Ryujinx reads it (ManagedSocket.cs, SetSocketOption): an int from
    // four bytes or more, the first byte from fewer, and EINVAL when there is nothing to read.
    const auto read_value = [&optval]() -> std::optional<u32> {
        if (optval.empty()) {
            return std::nullopt;
        }
        if (optval.size() >= sizeof(u32)) {
            return GetValue<u32>(optval);
        }
        return u32{optval[0]};
    };

    // [OpenPak] IPPROTO_TCP / TCP_NODELAY -- see the matching comment in GetSockOptImpl.
    if (level == 6 && static_cast<u32>(optname) == 1) {
        const auto value = read_value();
        if (!value) {
            return Errno::INVAL;
        }
        return Translate(file_descriptors[fd]->socket->SetNoDelay(*value != 0));
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        LOG_WARNING(Service,
                    "(STUBBED) Unknown setsockopt level={} optname={:#x}, remembered and "
                    "returning SUCCESS for compatibility",
                    level, static_cast<u32>(optname));
        RememberFeignedSockOpt(*file_descriptors[fd], level, optname, optval);
        return Errno::SUCCESS;
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    Network::SocketBase* const socket = descriptor.socket.get();

    // [OpenPak] Anything this build does not know is remembered for the matching get and never
    // applied, whatever its size: Nintendo's linger-shaped 0x80000001 carries eight bytes.
    switch (optname) {
    case OptName::REUSEADDR:
    case OptName::KEEPALIVE:
    case OptName::BROADCAST:
    case OptName::LINGER:
    case OptName::SNDBUF:
    case OptName::RCVBUF:
    case OptName::SNDTIMEO:
    case OptName::RCVTIMEO:
    case OptName::NOSIGPIPE:
        break;
    default:
        LOG_WARNING(Service,
                    "(STUBBED) Unimplemented optname={} (0x{:x}) optlen={}, remembered "
                    "and returning SUCCESS for compatibility",
                    static_cast<u32>(optname), static_cast<u32>(optname), optval.size());
        RememberFeignedSockOpt(descriptor, level, optname, optval);
        return Errno::SUCCESS;
    }

    const auto value = read_value();
    if (!value) {
        return Errno::INVAL;
    }

    switch (optname) {
    case OptName::LINGER: {
        // The seconds are the second int, when there is one (Ryujinx ManagedSocket.cs).
        const u32 seconds = optval.size() >= sizeof(Linger) ? GetValue<Linger>(optval).linger : 0;
        return Translate(socket->SetLinger(*value != 0, seconds));
    }
    case OptName::REUSEADDR:
        return Translate(socket->SetReuseAddr(*value != 0));
    case OptName::KEEPALIVE:
        return Translate(socket->SetKeepAlive(*value != 0));
    case OptName::BROADCAST:
        return Translate(socket->SetBroadcast(*value != 0));
    case OptName::SNDBUF:
        return Translate(socket->SetSndBuf(*value));
    case OptName::RCVBUF:
        return Translate(socket->SetRcvBuf(*value));
    case OptName::SNDTIMEO:
        return Translate(socket->SetSndTimeo(*value));
    case OptName::RCVTIMEO: {
        const Errno result = Translate(socket->SetRcvTimeo(*value));
        descriptor.has_receive_timeout = result == Errno::SUCCESS && *value != 0;
        return result;
    }
    case OptName::NOSIGPIPE:
        // No host getter here, so what was set is what a later get reads back.
        LOG_WARNING(Service, "(STUBBED) setting NOSIGPIPE to {}", *value);
        RememberFeignedSockOpt(descriptor, level, optname, optval);
        return Errno::SUCCESS;
    default:
        // Unreachable: every other optname already returned above.
        return Errno::SUCCESS;
    }
}

Errno BSD::ShutdownImpl(s32 fd, s32 how) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    const Network::ShutdownHow host_how = Translate(static_cast<ShutdownHow>(how));
    return Translate(file_descriptors[fd]->socket->Shutdown(host_how));
}

std::pair<s32, Errno> BSD::RecvImpl(s32 fd, u32 flags, std::vector<u8>& message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    // [OpenPak] An event fd answers from its counter and never blocks: the whole count, which
    // the read clears, or 1 off a semaphore (Ryujinx EventFileDescriptor.cs, Read).
    if (descriptor.event_value) {
        if (message.size() < sizeof(u64)) {
            return {-1, Errno::INVAL};
        }

        u64 value = descriptor.event_value->load();
        do {
            if (value == 0) {
                return {-1, Errno::AGAIN};
            }
        } while (!descriptor.event_value->compare_exchange_weak(
            value, descriptor.event_semaphore ? value - 1 : 0));

        const u64 count = descriptor.event_semaphore ? 1 : value;
        std::memcpy(message.data(), &count, sizeof(count));
        return {static_cast<s32>(sizeof(u64)), Errno::SUCCESS};
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] = Translate(descriptor.socket->Recv(flags, message));

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD::RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                        std::vector<u8>& addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    Network::SockAddrIn addr_in{};
    Network::SockAddrIn* p_addr_in = nullptr;
    if (descriptor.is_connection_based) {
        // Connection based file descriptors (e.g. TCP) zero addr
        addr.clear();
    } else {
        // Datagram (UDP): receive the sender's address. (Previously this path returned
        // AGAIN unconditionally, which silently broke all UDP recvfrom/MSG_PEEK.)
        p_addr_in = &addr_in;
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    auto [ret, bsd_errno] = Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));

    // P2P shares one socket across every peer, so one unreachable peer must not read as the
    // network dropping. The failed call consumed the queued error; take the next datagram.
    if (!descriptor.is_connection_based) {
        for (int attempt = 0; attempt < 16 && IsTransientDatagramError(bsd_errno); ++attempt) {
            LOG_WARNING(Service, "Discarding queued ICMP error on fd={} errno={}", fd,
                        static_cast<int>(bsd_errno));
            std::tie(ret, bsd_errno) =
                Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));
        }
    }

    if (bsd_errno != Errno::SUCCESS && bsd_errno != Errno::AGAIN) {
        LOG_WARNING(Service, "RecvFrom fd={} failed with errno={}", fd,
                    static_cast<int>(bsd_errno));
    }

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    if (p_addr_in) {
        if (ret < 0) {
            addr.clear();
        } else {
            // [OpenPak] A buffer shorter than a sockaddr_in takes what fits, not an assert (as
            // Eden).
            if (addr.size() > sizeof(SockAddrIn)) {
                addr.resize(sizeof(SockAddrIn));
            }
            const SockAddrIn result = Translate(addr_in);
            PutValue(addr, result);
            LOG_DEBUG(Service, "RecvFrom fd={} <- {}:{} len={} {}", fd,
                      Network::IPv4AddressToRedactedString(addr_in.ip), addr_in.portno, ret,
                      DescribePrudpLite(std::span<const u8>{message.data(),
                                                            static_cast<size_t>(std::max(ret, 0))}));

            // [OpenPak] The NAT check's answer is 16 bytes: [type][external port][external ip]
            // [server ip]. It is the only place this console is told how the outside world sees
            // it, so it is worth a line (as Eden has it).
            if (ret == 16 && (addr_in.portno == 10025 || addr_in.portno == 10125)) {
                LOG_INFO(Service, "[OpenPak] NAT check: external address {}.{}.{}.{} (from {}:{})",
                         message[8], message[9], message[10], message[11],
                         Network::IPv4AddressToRedactedString(addr_in.ip), addr_in.portno);
            }
        }
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD::SendImpl(s32 fd, u32 flags, std::span<const u8> message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};
    FileDescriptor& descriptor = *file_descriptors[fd];

    // [OpenPak] Writing an event fd adds to its count and wakes whoever is polling it (Ryujinx
    // EventFileDescriptor.cs, Write).
    if (descriptor.event_value) {
        u64 value{};
        if (message.size() >= sizeof(u64)) {
            std::memcpy(&value, message.data(), sizeof(value));
        }
        if (message.size() < sizeof(u64) || value == std::numeric_limits<u64>::max()) {
            return {-1, Errno::INVAL};
        }

        descriptor.event_value->fetch_add(value);

        // [OpenPak] Wake any Poll() this game deferred waiting on this eventfd. See
        // sockets.h's SetBsdDeferralEvent declaration comment.
        if (Kernel::KEvent* deferral_event = GetBsdDeferralEvent()) {
            deferral_event->Signal();
        }
        return {static_cast<s32>(sizeof(u64)), Errno::SUCCESS};
    }

    return Translate(descriptor.socket->Send(message, flags));
}

std::pair<s32, Errno> BSD::SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                      std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};

    FileDescriptor& descriptor = *file_descriptors[fd];

    // With no address the datagram goes out with the host's send, to the connected peer.
    Network::SockAddrIn addr_in;
    Network::SockAddrIn* p_addr_in = nullptr;
    if (!addr.empty()) {
        // [OpenPak] A sockaddr shorter than a sockaddr_in is EINVAL, as Eden has it.
        if (addr.size() < sizeof(SockAddrIn)) {
            return {-1, Errno::INVAL};
        }
        const auto parsed_addr = ParseGuestSockAddr(addr);
        if (!parsed_addr) {
            LOG_WARNING(Service, "SendTo fd={} with unsupported address (size={})", fd,
                        addr.size());
            return {-1, Errno::AFNOSUPPORT};
        }
        addr_in = *parsed_addr;
        p_addr_in = &addr_in;
    }

    if (!descriptor.is_connection_based && p_addr_in) {
        LOG_DEBUG(Service, "SendTo fd={} -> {}:{} len={} {}", fd,
                  Network::IPv4AddressToRedactedString(p_addr_in->ip), p_addr_in->portno,
                  message.size(), DescribePrudpLite(message));
    }

    return Translate(descriptor.socket->SendTo(flags, message, p_addr_in));
}

Errno BSD::CloseImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    std::shared_ptr<Network::SocketBase> socket_to_close;
    {
        std::lock_guard lock(fd_table_mutex);
        if (!file_descriptors[fd]->socket)
            return Errno::BADF;
        socket_to_close = file_descriptors[fd]->socket;
        file_descriptors[fd].reset();

        // [OpenPak] A duplicate shares its host socket with the descriptor it was made from (ssl
        // duplicates the one a connection is given), so the host socket goes only with the last
        // of them, as Ryujinx counts references (BsdContext.cs). Closed with the first, a TLS
        // connection going away took the title's own socket with it.
        for (const auto& other : file_descriptors) {
            if (other && other->socket == socket_to_close) {
                return Errno::SUCCESS;
            }
        }
    }

    const Errno bsd_errno = Translate(socket_to_close->Close());
    LOG_INFO(Service, "Close socket fd={}", fd);

    return bsd_errno;
}

Expected<s32, Errno> BSD::DuplicateSocketImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Unexpected(Errno::BADF);
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return Unexpected(Errno::MFILE);
    }

    file_descriptors[new_fd] = file_descriptors[fd];
    return new_fd;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD::GetSocket(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return std::nullopt;
    }
    return file_descriptors[fd]->socket;
}

s32 BSD::FindFreeFileDescriptorHandle() noexcept {
    for (s32 fd = 0; fd < static_cast<s32>(file_descriptors.size()); ++fd) {
        if (!file_descriptors[fd]) {
            return fd;
        }
    }
    return -1;
}

// [OpenPak] A blocking recv on a datagram socket with nothing to read sat inside the host recv on
// one of the three threads every socket of the title shares. A game that binds a UDP port and waits
// for a peer waits forever, and one such wait per thread stops everything else with it: on Ryujinx
// Terraria's NPLN stream went silent in the same microsecond as its blocking recvfrom on udp/8888
// (openpak/ryujinx 001c04871). The request is parked the way a deferred poll is, and the deferral
// heartbeat re-runs it until the socket is readable.
//
// Datagram sockets only, as on Ryujinx. A socket with a receive timeout returns by itself and is
// left alone, so its timeout stays the host's to keep; a socket closed while its recv is parked
// answers EBADF on the next pass.
bool BSD::DeferBlockingReceive(HLERequestContext& ctx, s32 fd, u32 flags) {
    if (GetBsdDeferralEvent() == nullptr || fd < 0 || fd >= static_cast<s32>(MAX_FD) ||
        !file_descriptors[fd]) {
        return false;
    }
    FileDescriptor& descriptor = *file_descriptors[fd];
    if (!descriptor.socket || descriptor.is_connection_based || descriptor.event_value ||
        descriptor.has_receive_timeout ||
        (descriptor.flags & Network::FLAG_O_NONBLOCK) != 0 ||
        (flags & Network::FLAG_MSG_DONTWAIT) != 0) {
        return false;
    }
    // A socket with no host descriptor (a room proxy) cannot be polled here.
    if (descriptor.socket->GetFD() == static_cast<decltype(descriptor.socket->GetFD())>(-1)) {
        return false;
    }

    std::vector<Network::PollFD> probe{
        Network::PollFD{descriptor.socket.get(), Network::PollEvents::In, Network::PollEvents{}}};
    if (Network::Poll(probe, 0).first != 0) {
        // Readable, or in error and about to say so: answer in this pass.
        return false;
    }

    ctx.SetIsDeferred();
    return true;
}

bool BSD::IsFileDescriptorValid(s32 fd) const noexcept {
    if (fd >= static_cast<s32>(MAX_FD) || fd < 0) {
        LOG_ERROR(Service, "Invalid file descriptor handle={}", fd);
        return false;
    }
    if (!file_descriptors[fd]) {
        LOG_ERROR(Service, "File descriptor handle={} is not allocated", fd);
        return false;
    }
    return true;
}

void BSD::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    // Lock the table so CloseImpl doesn't delete a socket while we are iterating
    std::lock_guard lock(fd_table_mutex);

    // We must ensure we only deliver the packet ONCE
    std::vector<Network::SocketBase*> processed_sockets;

    for (auto& optional_desc : file_descriptors) {
        if (optional_desc.has_value() && optional_desc->socket) {
            Network::SocketBase* socket_ptr = optional_desc->socket.get();

            // If we haven't given this specific socket the packet yet...
            if (std::find(processed_sockets.begin(), processed_sockets.end(), socket_ptr) == processed_sockets.end()) {
                socket_ptr->HandleProxyPacket(packet);
                processed_sockets.push_back(socket_ptr);
            }
        }
    }
}

BSD::BSD(Core::System& system_, const char* name, bool is_user_)
    : ServiceFramework{system_, name}, room_network{system_.GetRoomNetwork()}, is_user{is_user_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD::RegisterClient, "RegisterClient"},
        {1, &BSD::StartMonitoring, "StartMonitoring"},
        {2, &BSD::Socket, "Socket"},
        {3, &BSD::SocketExempt, "SocketExempt"},
        {4, &BSD::Open, "Open"},
        {5, &BSD::Select, "Select"},
        {6, &BSD::Poll, "Poll"},
        {7, &BSD::Sysctl, "Sysctl"},
        {8, &BSD::Recv, "Recv"},
        {9, &BSD::RecvFrom, "RecvFrom"},
        {10, &BSD::Send, "Send"},
        {11, &BSD::SendTo, "SendTo"},
        {12, &BSD::Accept, "Accept"},
        {13, &BSD::Bind, "Bind"},
        {14, &BSD::Connect, "Connect"},
        {15, &BSD::GetPeerName, "GetPeerName"},
        {16, &BSD::GetSockName, "GetSockName"},
        {17, &BSD::GetSockOpt, "GetSockOpt"},
        {18, &BSD::Listen, "Listen"},
        {19, &BSD::Ioctl, "Ioctl"},
        {20, &BSD::Fcntl, "Fcntl"},
        {21, &BSD::SetSockOpt, "SetSockOpt"},
        {22, &BSD::Shutdown, "Shutdown"},
        {23, &BSD::ShutdownAllSockets, "ShutdownAllSockets"},
        {24, &BSD::Write, "Write"},
        {25, &BSD::Read, "Read"},
        {26, &BSD::Close, "Close"},
        {27, &BSD::DuplicateSocket, "DuplicateSocket"},
        {28, &BSD::GetResourceStatistics, "GetResourceStatistics"},
        {29, &BSD::RecvMMsg, "RecvMMsg"},
        {30, &BSD::SendMMsg, "SendMMsg"},
        {31, &BSD::EventFd, "EventFd"},
        {32, &BSD::RegisterResourceStatisticsName, "RegisterResourceStatisticsName"},
        {33, &BSD::RegisterClientShared, "RegisterClientShared"},
        {34, &BSD::GetSocketStatistics, "GetSocketStatistics"},
        {35, &BSD::NifIoctl, "NifIoctl"},
        {36, &BSD::Unknown36, "Unknown36"},
        {37, &BSD::Unknown37, "Unknown37"},
        {38, &BSD::Unknown38, "Unknown38"},
        {39, &BSD::Unknown39, "Unknown39"},
        {40, &BSD::Unknown40, "Unknown40"},
        {200, &BSD::SetThreadCoreMask, "SetThreadCoreMask"},
        {201, &BSD::GetThreadCoreMask, "GetThreadCoreMask"},
    };
    // clang-format on

    RegisterHandlers(functions);

    {
        std::lock_guard lock(fd_table_mutex);
        ++instance_count;
    }

    // [OpenPak] The descriptor table is shared by bsd:u, bsd:s and bsd:a, so one of them
    // listening is enough: every instance listening would hand each packet to each socket once
    // per instance.
    if (!is_user) {
        return;
    }
    if (auto room_member = room_network.GetRoomMember().lock()) {
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [this](const Network::ProxyPacket& packet) { OnProxyPacketReceived(packet); });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }
}

BSD::~BSD() {
    if (is_user) {
        if (auto room_member = room_network.GetRoomMember().lock()) {
            room_member->Unbind(proxy_packet_received);
        }
    }

    // [OpenPak] The shared table outlives any one service: it is emptied when the last of them
    // goes, so a service closing does not take the others' sockets with it.
    std::lock_guard lock(fd_table_mutex);
    if (--instance_count == 0) {
        for (auto& descriptor : file_descriptors) {
            descriptor.reset();
        }
    }
}

std::unique_lock<std::mutex> BSD::LockService() {
    // Do not lock socket IClient instances.
    return {};
}

BSDCFG::BSDCFG(Core::System& system_, const char* name) : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSDCFG::SetIfUp, "SetIfUp"},
        {1, &BSDCFG::SetIfUpWithEvent, "SetIfUpWithEvent"},
        {2, &BSDCFG::CancelIf, "CancelIf"},
        {3, &BSDCFG::SetIfDown, "SetIfDown"},
        {4, &BSDCFG::GetIfState, "GetIfState"},
        {5, &BSDCFG::DhcpRenew, "DhcpRenew"},
        {6, &BSDCFG::AddStaticArpEntry, "AddStaticArpEntry"},
        {7, &BSDCFG::RemoveArpEntry, "RemoveArpEntry"},
        {8, &BSDCFG::LookupArpEntry, "LookupArpEntry"},
        {9, &BSDCFG::LookupArpEntry2, "LookupArpEntry2"},
        {10, &BSDCFG::ClearArpEntries, "ClearArpEntries"},
        {11, &BSDCFG::ClearArpEntries2, "ClearArpEntries2"},
        {12, &BSDCFG::PrintArpEntries, "PrintArpEntries"},
        {13, &BSDCFG::Unknown13, "Unknown13"},
        {14, &BSDCFG::Unknown14, "Unknown14"},
        {15, &BSDCFG::Unknown15, "Unknown15"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

// BSDCFG Service Method Stubs
void BSDCFG::SetIfUp(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfUp");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::SetIfUpWithEvent(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfUpWithEvent");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::CancelIf(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called CancelIf");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::SetIfDown(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfDown");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::GetIfState(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetIfState");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::DhcpRenew(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called DhcpRenew");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::AddStaticArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called AddStaticArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::RemoveArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RemoveArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::LookupArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called LookupArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::LookupArpEntry2(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called LookupArpEntry2");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::ClearArpEntries(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ClearArpEntries");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::ClearArpEntries2(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ClearArpEntries2");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::PrintArpEntries(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called PrintArpEntries");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::Unknown13(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown13 (Cmd13)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::Unknown14(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown14 (Cmd14)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSDCFG::Unknown15(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown15 (Cmd15)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::GetResourceStatistics(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetResourceStatistics");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::GetSocketStatistics(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetSocketStatistics");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::GetThreadCoreMask(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetThreadCoreMask");
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<u64>(0);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::Ioctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Ioctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::NOTTY);
}

void BSD::NifIoctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called NifIoctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::NOTTY);
}

void BSD::Open(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Open");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::ACCES);
}

// [OpenPak] BSD::RecvMMsg's real implementation is defined below SendMMsg, alongside the
// shared MMsg* wire-format helpers both need (see bsd.h for its declaration).

void BSD::RegisterResourceStatisticsName(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RegisterResourceStatisticsName");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

namespace {
// [OpenPak] Wire format for a single nn::socket msghdr inside a SendMMsg/RecvMMsg buffer.
// Mirrors Ryujinx-Reference's BsdMsgHdr::Deserialize/Serialize byte-for-byte (itself matching
// the real NX ABI): msg_namelen, [name], iov_count, iov_count * (u64 len, [data]),
// control_len, [control], flags, then a trailing "length" field the real syscall uses to
// report bytes transferred for that specific message.
struct MMsgEntry {
    std::vector<u8> name;
    std::vector<std::vector<u8>> iov;
    std::vector<u8> control;
    u32 flags = 0;
    u32 length = 0;
};

// [OpenPak] Diagnostic only -- logs the TLS record type/length (and handshake message type,
// if applicable) of a SendMMsg/RecvMMsg payload, matching Ryujinx-Reference's own
// "[DIAG] Bsd.SendMMsg TLS hs=0x.. len=.." logging (ManagedSocket.cs), generalized to cover
// every record type (Ryujinx's only fires for content-type Handshake) so a final small
// ApplicationData or Alert record right before a connection close is visible too. The TLS
// record header (type, version, length) is always plaintext even once the session is
// encrypted -- only the payload itself is opaque -- so this costs nothing and reveals real
// protocol state without decrypting anything.
void LogTlsRecordDiag(const char* what, std::span<const u8> data) {
    if (data.size() < 5) {
        return;
    }
    const u8 content_type = data[0];
    if (content_type < 0x14 || content_type > 0x17) {
        return; // not a TLS record (or content_type is genuinely opaque post-handshake noise)
    }
    const u16 record_len = static_cast<u16>((data[3] << 8) | data[4]);
    const char* type_name = [content_type] {
        switch (content_type) {
        case 0x14:
            return "ChangeCipherSpec";
        case 0x15:
            return "Alert";
        case 0x16:
            return "Handshake";
        case 0x17:
            return "ApplicationData";
        default:
            return "?";
        }
    }();
    if (content_type == 0x16 && data.size() > 5) {
        LOG_INFO(Service, "[OpenPak][DIAG] {} TLS type={}(0x{:02x}) hs=0x{:02x} record_len={} buf_len={}",
                 what, type_name, content_type, data[5], record_len, data.size());
    } else {
        LOG_INFO(Service, "[OpenPak][DIAG] {} TLS type={}(0x{:02x}) record_len={} buf_len={}", what,
                 type_name, content_type, record_len, data.size());
    }
}

bool MMsgReadU32(std::span<const u8>& data, u32& out) {
    if (data.size() < sizeof(u32)) {
        return false;
    }
    std::memcpy(&out, data.data(), sizeof(u32));
    data = data.subspan(sizeof(u32));
    return true;
}

bool MMsgReadU64(std::span<const u8>& data, u64& out) {
    if (data.size() < sizeof(u64)) {
        return false;
    }
    std::memcpy(&out, data.data(), sizeof(u64));
    data = data.subspan(sizeof(u64));
    return true;
}

bool MMsgReadBytes(std::span<const u8>& data, size_t count, std::vector<u8>& out) {
    if (data.size() < count) {
        return false;
    }
    out.assign(data.begin(), data.begin() + count);
    data = data.subspan(count);
    return true;
}

// Mirrors BsdMMsgHdr::Deserialize -- false on a malformed/short buffer (EFAULT in the
// reference implementation).
bool MMsgDeserialize(std::span<const u8> data, s32 vlen, std::vector<MMsgEntry>& out) {
    if (data.empty() || vlen < 0) {
        return false;
    }
    data = data.subspan(1); // leading header byte -- ignored (real hardware ignores it too)

    out.resize(static_cast<size_t>(vlen));
    for (auto& msg : out) {
        u32 name_len;
        if (!MMsgReadU32(data, name_len)) {
            return false;
        }
        if (name_len > 0 && !MMsgReadBytes(data, name_len, msg.name)) {
            return false;
        }

        u32 iov_count;
        if (!MMsgReadU32(data, iov_count)) {
            return false;
        }
        msg.iov.resize(iov_count);
        for (auto& segment : msg.iov) {
            u64 iov_len;
            if (!MMsgReadU64(data, iov_len)) {
                return false;
            }
            if (iov_len > 0 && !MMsgReadBytes(data, static_cast<size_t>(iov_len), segment)) {
                return false;
            }
        }

        u32 control_len;
        if (!MMsgReadU32(data, control_len)) {
            return false;
        }
        if (control_len > 0 && !MMsgReadBytes(data, control_len, msg.control)) {
            return false;
        }

        if (!MMsgReadU32(data, msg.flags)) {
            return false;
        }
        if (!MMsgReadU32(data, msg.length)) {
            return false;
        }
    }
    return true;
}

// Mirrors BsdMMsgHdr::Serialize -- writes the same shape back with each entry's (now updated)
// length field, so the guest can see how many bytes actually went out per message.
void MMsgSerialize(std::vector<u8>& out, const std::vector<MMsgEntry>& messages) {
    out.push_back(0x8);
    for (const auto& msg : messages) {
        const auto append_u32 = [&](u32 value) {
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(value));
        };
        const auto append_u64 = [&](u64 value) {
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(value));
        };

        append_u32(static_cast<u32>(msg.name.size()));
        out.insert(out.end(), msg.name.begin(), msg.name.end());

        append_u32(static_cast<u32>(msg.iov.size()));
        for (const auto& segment : msg.iov) {
            append_u64(segment.size());
            out.insert(out.end(), segment.begin(), segment.end());
        }

        append_u32(static_cast<u32>(msg.control.size()));
        out.insert(out.end(), msg.control.begin(), msg.control.end());

        append_u32(msg.flags);
        append_u32(msg.length);
    }
}

// [OpenPak] A named or control-carrying message is a datagram shape that cannot go out as one
// stream send; titles only use the plain form. Ported from Eden.
bool MMsgIsPlain(const std::vector<MMsgEntry>& messages) {
    return std::ranges::all_of(messages, [](const MMsgEntry& msg) {
        return msg.name.empty() && msg.control.empty();
    });
}

// [OpenPak] Spreads the byte count one send or recv moved back over the messages it covered,
// and answers with how many messages it reached. A message only partly filled still counts: a
// stream read almost never fills the room offered. Nothing moved is 0, the end of the stream.
// Ported from Eden.
s32 MMsgSpreadTransferred(std::vector<MMsgEntry>& messages, size_t transferred) {
    if (transferred == 0) {
        return 0;
    }

    size_t index = 0;
    size_t left = transferred;
    while (left > 0 && index < messages.size()) {
        MMsgEntry& msg = messages[index];

        size_t capacity = 0;
        for (const auto& segment : msg.iov) {
            capacity += segment.size();
        }

        size_t stored;
        if (left > capacity) {
            stored = capacity;
            ++index;
        } else {
            stored = left;
        }

        msg.length = static_cast<u32>(stored);
        left -= stored;
    }

    return static_cast<s32>(std::min(index + 1, messages.size()));
}
} // Anonymous namespace

void BSD::SendMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 vlen = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} vlen={} flags=0x{:x}", fd, vlen, flags);

    // [OpenPak] Was a hard EOPNOTSUPP stub that never touched the socket at all -- every
    // title that uses sendmmsg() for its TLS/gRPC handshake data (Splatoon 3's
    // NintendoSDK_gRPC_For_NPLN stack does) had that data silently dropped: the underlying
    // TCP connection could complete perfectly (confirmed: SO_ERROR came back 0) and the game
    // would still sit forever waiting for a response to a handshake it never actually sent.
    if (!IsFileDescriptorValid(fd)) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::BADF);
        return;
    }

    // [OpenPak] This buffer is a "receive" (B) descriptor: the game pre-populates it with the
    // message data to send and expects the service to both read that AND write the per-message
    // length fields back into the SAME buffer afterward (a real in-out MapAlias, not the usual
    // one-way A-in/B-out split -- matches Ryujinx-Reference reading and writing the exact same
    // ReceiveBuff[0] region). ctx.ReadBuffer() only ever looks at A/X descriptors and silently
    // returned empty for this call; read B's raw guest memory directly instead.
    std::vector<u8> raw_buffer;
    if (!ctx.BufferDescriptorB().empty() && ctx.BufferDescriptorB()[0].Size() > 0) {
        raw_buffer.resize(ctx.BufferDescriptorB()[0].Size());
        ctx.GetMemory().ReadBlock(ctx.BufferDescriptorB()[0].Address(), raw_buffer.data(),
                                   raw_buffer.size());
    } else {
        auto fallback = ctx.ReadBuffer(0);
        raw_buffer.assign(fallback.begin(), fallback.end());
    }

    std::vector<MMsgEntry> messages;
    if (!MMsgDeserialize(raw_buffer, vlen, messages)) {
        LOG_ERROR(Service, "SendMMsg fd={} vlen={}: malformed message buffer", fd, vlen);
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::INVAL);
        return;
    }

    if (!MMsgIsPlain(messages)) {
        LOG_WARNING(Service, "SendMMsg fd={} vlen={}: named or control message", fd, vlen);
        BuildErrnoResponse(ctx, Errno::OPNOTSUPP);
        return;
    }

    // [OpenPak] The messages go out as one send, their buffers end to end, as Eden has it.
    std::vector<u8> payload;
    for (const auto& msg : messages) {
        for (const auto& segment : msg.iov) {
            payload.insert(payload.end(), segment.begin(), segment.end());
        }
    }

    LogTlsRecordDiag("SendMMsg", payload);

    s32 sent = 0;
    Errno bsd_errno = Errno::SUCCESS;
    if (!payload.empty()) {
        std::tie(sent, bsd_errno) = SendImpl(fd, flags, payload);
    }

    s32 ret = -1;
    if (bsd_errno == Errno::SUCCESS) {
        ret = MMsgSpreadTransferred(messages, static_cast<size_t>(std::max(sent, 0)));
        std::vector<u8> write_buffer;
        MMsgSerialize(write_buffer, messages);
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::RecvMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 vlen = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();
    rp.Pop<u32>(); // reserved, unused -- matches Ryujinx-Reference's own RecvMMsg signature
    struct TimeVal {
        u64 tv_sec;
        u64 tv_usec;
    };
    static_assert(sizeof(TimeVal) == 16);
    [[maybe_unused]] const auto timeout = rp.PopRaw<TimeVal>();

    LOG_DEBUG(Service, "called. fd={} vlen={} flags=0x{:x}", fd, vlen, flags);

    // [OpenPak] Same fix as SendMMsg above (was a hard EOPNOTSUPP stub) -- a title that
    // batches its sends via sendmmsg() needs the matching recvmmsg() to read the response, or
    // the handshake goes out fine and the reply can never come back (confirmed directly:
    // Splatoon 3 calls this immediately after SendMMsg on the same fd). `timeout` is
    // deliberately unused, matching Ryujinx-Reference's own reference implementation (see its
    // TODO on ManagedSocket.RecvMMsg) -- non-blocking behavior still comes the normal way,
    // through FLAG_MSG_DONTWAIT / O_NONBLOCK.
    if (!IsFileDescriptorValid(fd)) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::BADF);
        return;
    }

    std::vector<u8> raw_buffer;
    if (!ctx.BufferDescriptorB().empty() && ctx.BufferDescriptorB()[0].Size() > 0) {
        raw_buffer.resize(ctx.BufferDescriptorB()[0].Size());
        ctx.GetMemory().ReadBlock(ctx.BufferDescriptorB()[0].Address(), raw_buffer.data(),
                                   raw_buffer.size());
    } else {
        auto fallback = ctx.ReadBuffer(0);
        raw_buffer.assign(fallback.begin(), fallback.end());
    }

    std::vector<MMsgEntry> messages;
    if (!MMsgDeserialize(raw_buffer, vlen, messages)) {
        LOG_ERROR(Service, "RecvMMsg fd={} vlen={}: malformed message buffer", fd, vlen);
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(-1);
        rb.PushEnum(Errno::INVAL);
        return;
    }

    if (!MMsgIsPlain(messages)) {
        LOG_WARNING(Service, "RecvMMsg fd={} vlen={}: named or control message", fd, vlen);
        BuildErrnoResponse(ctx, Errno::OPNOTSUPP);
        return;
    }

    // [OpenPak] One recv into the room all the messages offer, end to end, as Eden has it.
    size_t capacity = 0;
    for (const auto& msg : messages) {
        for (const auto& segment : msg.iov) {
            capacity += segment.size();
        }
    }

    std::vector<u8> buffer(capacity);
    s32 received = 0;
    Errno bsd_errno = Errno::SUCCESS;
    if (capacity > 0) {
        std::tie(received, bsd_errno) = RecvImpl(fd, flags, buffer);
    }

    s32 ret = -1;
    if (bsd_errno == Errno::SUCCESS) {
        const size_t actual = static_cast<size_t>(std::max(received, 0));
        LogTlsRecordDiag("RecvMMsg", std::span<const u8>(buffer).first(actual));

        size_t offset = 0;
        for (auto& msg : messages) {
            for (auto& segment : msg.iov) {
                const size_t take = std::min(segment.size(), actual - offset);
                std::memcpy(segment.data(), buffer.data() + offset, take);
                offset += take;
                if (offset >= actual) {
                    break;
                }
            }
            if (offset >= actual) {
                break;
            }
        }

        ret = MMsgSpreadTransferred(messages, actual);
        std::vector<u8> write_buffer;
        MMsgSerialize(write_buffer, messages);
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SetThreadCoreMask(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetThreadCoreMask [15.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::ShutdownAllSockets(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ShutdownAllSockets");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::SocketExempt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    // [OpenPak] A socket like any other, shut down for reading once made. Ported from Eden.
    auto [fd, bsd_errno] = SocketImpl(static_cast<Domain>(domain), static_cast<Type>(type),
                                      static_cast<Protocol>(protocol));
    if (bsd_errno == Errno::SUCCESS) {
        bsd_errno = ShutdownImpl(fd, 0);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD::Unknown36(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown36 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::Unknown37(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown37 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::Unknown38(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown38 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::Unknown39(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown39 [20.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::Unknown40(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown40 [20.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(Errno::OPNOTSUPP);
}

void BSD::Sysctl(HLERequestContext& ctx) {
    // [OpenPak] getifaddrs(): NPLN's WebRTC gathers connection candidates from it; refused, a
    // host offers a joiner no address and the join times out (2321-5248). Ported from Eden and
    // Ryujinx. Every other query stays unsupported, as before.
    const auto mib_bytes = ctx.ReadBuffer(0);
    std::vector<s32> mib(std::min<std::size_t>(mib_bytes.size() / sizeof(s32), 16));
    std::memcpy(mib.data(), mib_bytes.data(), mib.size() * sizeof(s32));
    const std::size_t new_size = ctx.CanReadBuffer(1) ? ctx.GetReadBufferSize(1) : 0;
    const std::size_t old_size = ctx.CanWriteBuffer(0) ? ctx.GetWriteBufferSize(0) : 0;

    s32 ret = -1;
    Errno bsd_errno = Errno::OPNOTSUPP;
    u32 length = 0;

    const auto iface = Network::GetSelectedNetworkInterface();
    if (InterfaceList::Matches(mib) && new_size == 0 && iface) {
        const auto list = InterfaceList::Build(Network::TranslateIPv4(iface->ip_address),
                                               Network::TranslateIPv4(iface->subnet_mask));
        length = static_cast<u32>(list.size());
        if (old_size != 0 && old_size < list.size()) {
            bsd_errno = Errno::NOMEM; // no buffer is the size probe; a short one is ENOMEM
        } else {
            if (old_size != 0) {
                ctx.WriteBuffer(list);
            }
            ret = 0;
            bsd_errno = Errno::SUCCESS;
        }
    } else {
        LOG_WARNING(Service, "(STUBBED) Sysctl mib=[{}] old={} new={}", fmt::join(mib, ","),
                    old_size, new_size);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(length);
}

} // namespace Service::Sockets
