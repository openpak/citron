// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/settings.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/nsd.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/memory.h"
#include "openpak/network_profile.h"

namespace Service::Sockets {

// The setting, then the environment; empty when neither names an address, which redirects
// nothing (as Eden has it).
static std::string GetConfiguredIp(const std::string& setting, const char* env_var) {
    if (!setting.empty()) {
        return setting;
    }
    if (const char* env = std::getenv(env_var); env && *env) {
        return env;
    }
    return {};
}

// [OpenPak] La redirection est-elle active ?
//
// Le reglage « enable_openpak » n'existe QUE dans la facade Qt (src/citron/main.cpp) : la facade
// SDL (citron_cmd) ne le cable nulle part et le reecrit a sa valeur par defaut, false, au
// demarrage. Mesure du 2026-08-25 : lance par citron-cmd, Splatoon 3 a resolu
// « t-dce9377b-lp1.lp1.t.npln.srv.nintendo.net » vers 34.49.112.177 — le VRAI serveur de Nintendo —
// alors que le fichier de configuration portait bien enable_openpak=true.
//
// On accepte donc aussi une activation par l'environnement, exactement comme GetConfiguredIp le
// fait deja pour les deux adresses. Une valeur vide, « 0 », « false » ou « no » ne l'active pas.
static bool RedirectionOpenPakActive() {
    if (Settings::values.enable_openpak.GetValue()) {
        return true;
    }
    const char* env = std::getenv("OPENPAK_ENABLE");
    if (env == nullptr || *env == '\0') {
        return false;
    }
    const std::string v = Common::ToLower(env);
    return v != "0" && v != "false" && v != "no" && v != "off";
}

static std::optional<std::string> GetOpenPakRedirectIp(const std::string& host) {
    if (!RedirectionOpenPakActive()) {
        return std::nullopt;
    }

    const std::string server_ip =
        GetConfiguredIp(Settings::values.openpak_server_ip.GetValue(), "OPENPAK_SERVER_IP");
    if (server_ip.empty()) {
        return std::nullopt;
    }

    // [OpenPak] The profile OpenPak publishes decides which names are redirected and where,
    // because it is generated from the live routing: a title served on a new hostname works
    // without a new build. Two things it says that a wildcard cannot: a name with an address of
    // its own (the NAT check compares what two addresses observe of one console, so its second
    // probe must not collapse onto the first), and a name that must be left alone entirely --
    // the console's own connection test measures OpenPak instead of the internet if redirected.
    // The same lookup as Eden's.
    if (const auto from_profile = openpak::NetworkProfile::RedirectFor(host, server_ip);
        from_profile.has_value()) {
        LOG_INFO(Service, "[OpenPak] Redirecting '{}' -> '{}' (network profile)", host,
                 *from_profile);
        return from_profile;
    }

    if (openpak::NetworkProfile::Loaded()) {
        // A profile in hand and no match means the name is not ours to answer.
        return std::nullopt;
    }

    // No profile yet (none stored, none fetched): the built-in list.
    if (host.starts_with("nncs2-") && host.ends_with(".n.n.srv.nintendo.net")) {
        const std::string nat_ip =
            GetConfiguredIp(Settings::values.openpak_nat_ip.GetValue(), "OPENPAK_NAT_IP");
        const std::string& target = nat_ip.empty() ? server_ip : nat_ip;
        LOG_INFO(Service, "[OpenPak] Redirecting NAT check host '{}' -> '{}'", host, target);
        return target;
    }

    // The library's built-in Switch families, inside the verified ceiling: the one list every
    // OpenPak client falls back to, instead of a copy of it here.
    static const std::vector<std::string> builtin_families =
        openpak::NetworkProfile::BuiltIn("switch").suffixes;
    if (openpak::NetworkProfile::NameInFamilies(host, builtin_families)) {
        LOG_INFO(Service, "[OpenPak] Redirecting Nintendo host '{}' -> '{}'", host, server_ip);
        return server_ip;
    }

    return std::nullopt;
}

// [OpenPak] Hold the FIRST npln resolution of the session until the startup translation burst
// has passed. A title's NPLN channel that comes up mid-burst parks without ever sending its first
// RPC -- the title looks online and freezes. Eden holds 3000 ms, verified live against the Stardew
// tenant (and the Ryujinx side of this integration measured the same), so that is the hold here
// too. The resolver shares the bsdsocket service with two extra host threads, so the hold parks
// one of three, not the only one.
static std::once_flag g_npln_delay_once;

static void MaybeDelayNplnInit(const std::string& host) {
    if (Common::ToLower(host).find("npln") == std::string::npos) {
        return;
    }
    std::call_once(g_npln_delay_once, [] {
        constexpr int wait_ms = 3000;

        LOG_INFO(Service,
                 "[OpenPak] Holding the first npln host resolution for {} ms while the startup "
                 "burst passes (see MaybeDelayNplnInit)",
                 wait_ms);

        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));

        LOG_INFO(Service, "[OpenPak] npln hold finished after {} ms", wait_ms);
    });
}

// [OpenPak] The answer shaped the way a console's own resolver shapes it: one entry per address,
// socket type and protocol left "any". The hints a title passes are ignored, so the host's
// getaddrinfo is free to answer with one entry per socket type -- stream, datagram and raw for a
// single address. A title that walks that list looking only for what it asked for finds nothing
// it can use and gives up before it opens a socket: Stardew stops at 2318-0007 with the address
// resolved and no connection attempted. Ryujinx has always written "0 = Any" here. Ported from
// Eden; off with the integration, so nothing else changes shape.
static std::vector<Network::AddrInfo> AnySocketTypeAddrInfo(
    const std::vector<Network::AddrInfo>& vec) {
    if (!RedirectionOpenPakActive()) {
        return vec;
    }

    std::vector<Network::AddrInfo> out;
    for (const Network::AddrInfo& entry : vec) {
        const bool already =
            std::any_of(out.begin(), out.end(), [&](const Network::AddrInfo& seen) {
                return seen.addr.ip == entry.addr.ip && seen.addr.portno == entry.addr.portno;
            });
        if (already) {
            continue;
        }
        Network::AddrInfo copy = entry;
        copy.socket_type = Network::Type::Unspecified;
        copy.protocol = Network::Protocol::Unspecified;
        out.push_back(std::move(copy));
    }
    return out;
}

// [OpenPak] nsd's substitution, the way hardware routes a name through nsd first: when the
// request asks for it, or when the name carries the '%' only nsd can fill in. Skipping it leaves
// distinct services sharing one name -- the NAT check's two probes above all, which must land on
// two different addresses.
static void ApplyNsdResolve(bool use_nsd_resolve, std::string& host) {
    if (!use_nsd_resolve && host.find('%') == std::string::npos) {
        return;
    }
    std::string resolved = NsdResolve(host);
    if (resolved != host) {
        LOG_INFO(Service, "[sfdnsres] NSD resolved '{}' -> '{}'", host, resolved);
        host = std::move(resolved);
    }
}

enum class NetDbError : s32 {
    Internal = -1,
    Success = 0,
    HostNotFound = 1,
    TryAgain = 2,
    NoRecovery = 3,
    NoData = 4,
};

SFDNSRES::SFDNSRES(Core::System& system_) : ServiceFramework{system_, "sfdnsres"} {
    static const FunctionInfo functions[] = {
        {0, &SFDNSRES::SetDnsAddresses, "SetDnsAddressesPrivateRequest"},
        {1, &SFDNSRES::GetDnsAddressList, "GetDnsAddressPrivateRequest"},
        {2, &SFDNSRES::GetHostByNameRequest, "GetHostByNameRequest"},
        {3, &SFDNSRES::GetHostByAddrRequest, "GetHostByAddrRequest"},
        {4, &SFDNSRES::GetHostStringError, "GetHostStringErrorRequest"},
        {5, &SFDNSRES::GetGaiStringErrorRequest, "GetGaiStringErrorRequest"},
        {6, &SFDNSRES::GetAddrInfoRequest, "GetAddrInfoRequest"},
        {7, &SFDNSRES::GetNameInfoRequest, "GetNameInfoRequest"},
        {8, &SFDNSRES::GetCancelHandleRequest, "GetCancelHandleRequest"},
        {9, &SFDNSRES::CancelRequest, "CancelRequest"},
        {10, &SFDNSRES::GetHostByNameRequestWithOptions, "GetHostByNameRequestWithOptions"},
        {11, &SFDNSRES::GetHostByAddrRequest, "GetHostByAddrRequestWithOptions"},
        {12, &SFDNSRES::GetAddrInfoRequestWithOptions, "GetAddrInfoRequestWithOptions"},
        {13, &SFDNSRES::GetNameInfoRequestWithOptions, "GetNameInfoRequestWithOptions"},
        {14, &SFDNSRES::ResolverSetOptionRequest, "ResolverSetOptionRequest"},
        {15, &SFDNSRES::GetOptions, "ResolverGetOptionRequest"},
    };
    RegisterHandlers(functions);
}

SFDNSRES::~SFDNSRES() = default;

static NetDbError GetAddrInfoErrorToNetDbError(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return NetDbError::Success;
    case GetAddrInfoError::AGAIN:
        return NetDbError::TryAgain;
    case GetAddrInfoError::NODATA:
        return NetDbError::HostNotFound;
    case GetAddrInfoError::SERVICE:
        return NetDbError::Success;
    default:
        return NetDbError::HostNotFound;
    }
}

static Errno GetAddrInfoErrorToErrno(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return Errno::SUCCESS;
    case GetAddrInfoError::AGAIN:
        return Errno::SUCCESS;
    case GetAddrInfoError::NODATA:
        return Errno::SUCCESS;
    case GetAddrInfoError::SERVICE:
        return Errno::INVAL;
    default:
        return Errno::SUCCESS;
    }
}

template <typename T>
static void Append(std::vector<u8>& vec, T t) {
    const size_t offset = vec.size();
    vec.resize(offset + sizeof(T));
    std::memcpy(vec.data() + offset, &t, sizeof(T));
}

static void AppendNulTerminated(std::vector<u8>& vec, std::string_view str) {
    const size_t offset = vec.size();
    vec.resize(offset + str.size() + 1);
    std::memmove(vec.data() + offset, str.data(), str.size());
}

// We implement gethostbyname using the host's getaddrinfo rather than the
// host's gethostbyname, because it simplifies portability: e.g., getaddrinfo
// behaves the same on Unix and Windows, unlike gethostbyname where Windows
// doesn't implement h_errno.
static std::vector<u8> SerializeAddrInfoAsHostEnt(const std::vector<Network::AddrInfo>& vec,
                                                  std::string_view host) {

    std::vector<u8> data;
    // h_name: use the input hostname (append nul-terminated)
    AppendNulTerminated(data, host);
    // h_aliases: leave empty

    Append<u32_be>(data, 0); // count of h_aliases
    // (If the count were nonzero, the aliases would be appended as nul-terminated here.)
    Append<u16_be>(data, static_cast<u16>(Domain::INET)); // h_addrtype
    Append<u16_be>(data, sizeof(Network::IPv4Address));   // h_length
    // h_addr_list:
    size_t count = vec.size();
    ASSERT(count <= UINT32_MAX);
    Append<u32_be>(data, static_cast<uint32_t>(count));
    for (const Network::AddrInfo& addrinfo : vec) {
        // On the Switch, this is passed through htonl despite already being
        // big-endian, so it ends up as little-endian.
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip));

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }
    return data;
}

// [OpenPak] The names Ryujinx refuses to resolve (Sfdnsres/Proxy/DnsBlacklist.cs), whatever
// their case: Nintendo's own production services, which a name that was not redirected must
// not reach.
static bool IsBlockedHost(const std::string& host) {
    static constexpr std::array<std::string_view, 7> blocked_suffixes{
        "-lp1.n.n.srv.nintendo.net",
        "-lp1.s.n.srv.nintendo.net",
        "-lp1.lp1.t.npln.srv.nintendo.net",
        "-lp1.znc.srv.nintendo.net",
        "-lp1.p.srv.nintendo.net",
        "-sb-api.accounts.nintendo.com",
        "-sb.accounts.nintendo.com",
    };
    const std::string name = Common::ToLower(host);
    return name == "accounts.nintendo.com" ||
           std::ranges::any_of(blocked_suffixes, [&name](std::string_view suffix) {
               return name.ends_with(suffix);
           });
}

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_DEBUG(Service, "called: use_nsd_resolve={}, cancel_handle={}, process_id={}",
              parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);

    LOG_INFO(Service, "[OpenPak] DNS resolve (GetHostByName) requested: host={}", host);

    // [OpenPak] See MaybeDelayNplnInit's declaration comment.
    MaybeDelayNplnInit(host);

    ApplyNsdResolve(parameters.use_nsd_resolve != 0, host);

    std::string query_host = host;
    const auto redirect = GetOpenPakRedirectIp(host);
    if (redirect.has_value()) {
        query_host = *redirect;
    } else if (IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    auto res = Network::GetAddressInfo(query_host, /*service*/ std::nullopt);
    if (!res.has_value()) {
        return {0, Translate(res.error())};
    }

    const std::vector<u8> data = SerializeAddrInfoAsHostEnt(res.value(), host);
    const u32 data_size = static_cast<u32>(data.size());
    ctx.WriteBuffer(data, 0);

    return {data_size, GetAddrInfoError::SUCCESS};
}

void SFDNSRES::GetHostByNameRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        NetDbError netdb_error;
        Errno bsd_errno;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .data_size = data_size,
    });
}

void SFDNSRES::GetHostByNameRequestWithOptions(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

static std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec,
                                         std::string_view host) {
    // Adapted from
    // https://github.com/switchbrew/libnx/blob/c5a9a909a91657a9818a3b7e18c9b91ff0cbb6e3/nx/source/runtime/resolver.c#L190
    std::vector<u8> data;

    for (const Network::AddrInfo& addrinfo : vec) {
        // serialized addrinfo:
        Append<u32_be>(data, 0xBEEFCAFE);                                        // magic
        Append<u32_be>(data, 0);                                                 // ai_flags
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.family)));      // ai_family
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.socket_type))); // ai_socktype
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.protocol)));    // ai_protocol
        Append<u32_be>(data, sizeof(SockAddrIn));                                // ai_addrlen
        // ^ *not* sizeof(SerializedSockAddrIn), not that it matters since they're the same size

        // ai_addr: BSD-style sockaddr_in, matching the SockAddrIn struct in sockets.h --
        // {u8 sin_len; u8 sin_family; u16 sin_port; u8 sin_addr[4]; u8 sin_zero[8];}. This used
        // to write sin_family as a single 2-byte big-endian value, which skips sin_len entirely
        // (leaving it implicitly 0x00) instead of emitting it as its own leading byte. A guest
        // resolver walker that trusts sin_len -- and gRPC-based titles (Splatoon 3) that build
        // their own connect() sockaddr straight out of this buffer -- reads a zero-length
        // address off a sin_len of 0 and falls back to connecting to 0.0.0.0. Exactly matches
        // Ryujinx-Reference's AddrInfo4.Length fix (was sizeof(Array4<byte>)=4, needed to be
        // sizeof(AddrInfo4)=16): sin_len must be the full sockaddr size, not folded away.
        // [OpenPak]
        Append<u8>(data, static_cast<u8>(sizeof(SockAddrIn)));              // sin_len
        Append<u8>(data, static_cast<u8>(Translate(addrinfo.addr.family))); // sin_family
        // On the Switch, the following fields are passed through htonl despite
        // already being big-endian, so they end up as little-endian.
        Append<u16_le>(data, addrinfo.addr.portno);                            // sin_port
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip)); // sin_addr
        data.resize(data.size() + 8, 0);                                       // sin_zero

        if (addrinfo.canon_name.has_value()) {
            AppendNulTerminated(data, *addrinfo.canon_name);
        } else {
            data.push_back(0);
        }

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }

    data.resize(data.size() + 4, 0); // 4-byte sentinel value

    return data;
}

static std::pair<u32, GetAddrInfoError> GetAddrInfoRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_DEBUG(Service, "called: use_nsd_resolve={}, cancel_handle={}, process_id={}",
              parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);

    LOG_INFO(Service, "[OpenPak] DNS resolve (GetAddrInfo) requested: host={}", host);

    // [OpenPak] See MaybeDelayNplnInit's declaration comment.
    MaybeDelayNplnInit(host);

    ApplyNsdResolve(parameters.use_nsd_resolve != 0, host);

    std::string query_host = host;
    const auto redirect = GetOpenPakRedirectIp(host);
    if (redirect.has_value()) {
        query_host = *redirect;
    } else if (IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    // [OpenPak] A literal IP has nothing to resolve -- return it as-is. [OpenPak] After the
    // redirect and blocklist checks, which a literal passes untouched. See
    // TryParseIPv4Literal's declaration comment in internal_network/network.h for why falling
    // through to a real resolution here (which is what happened before this check existed) was
    // the actual root cause of Splatoon 3's NPLN connections completing TCP+TLS+HTTP/2 and then
    // silently closing without ever sending a HEADERS frame -- confirmed live: citron's own
    // connect cycles for this exact hostname resolved and connected correctly, but every one
    // still sent only a single small request and closed within ~1s of the server's reply, on
    // every cycle regardless of timing -- consistent with the game building an HTTP/2
    // :authority header from a corrupted canonical name, not any citron-side socket/scheduling
    // issue (both were separately investigated at length and ruled out).
    if (Network::IPv4Address literal_ip;
        !redirect.has_value() && Network::TryParseIPv4Literal(host, literal_ip)) {
        LOG_DEBUG(Service, "[OpenPak] Host '{}' is already a literal address: returned as-is",
                  host);
        Network::AddrInfo entry{};
        entry.family = Network::Domain::INET;
        entry.socket_type = Network::Type::STREAM;
        entry.protocol = Network::Protocol::TCP;
        entry.addr.family = Network::Domain::INET;
        entry.addr.ip = literal_ip;
        entry.addr.portno = 0;
        entry.canon_name = host;

        const std::vector<u8> data = SerializeAddrInfo(AnySocketTypeAddrInfo({entry}), host);
        const u32 data_size = static_cast<u32>(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }

    std::optional<std::string> service = std::nullopt;
    if (ctx.CanReadBuffer(1)) {
        const std::span<const u8> service_buffer = ctx.ReadBuffer(1);
        service = Common::StringFromBuffer(service_buffer);
    }

    auto res = Network::GetAddressInfo(query_host, service);
    if (!res.has_value()) {
        return {0, Translate(res.error())};
    }

    if (redirect.has_value()) {
        // [OpenPak] GetAddressInfo never requests AI_CANONNAME, so canon_name comes back
        // unset for a normal resolution -- SerializeAddrInfo then writes an empty canonical
        // name. That's harmless for titles that never look at it, but this query just resolved
        // a REDIRECT TARGET (a literal IP string), not the real host, so even if canon_name
        // were populated by the OS resolver it would be the numeric IP, never the actual
        // hostname. A gRPC-based title (Splatoon 3) builds its HTTP/2 :authority header from
        // this canonical name -- sending it empty (or the wrong literal IP) instead of the real
        // npln hostname is exactly the shape of bug already fixed above for an already-literal
        // host (see that comment): TCP+TLS+HTTP/2 complete fine, then the connection is torn
        // down before a real HEADERS frame ever goes out. Force it back to the real host here,
        // matching that same fix.
        for (auto& addrinfo : res.value()) {
            addrinfo.canon_name = host;
        }

    }

    const std::vector<u8> data = SerializeAddrInfo(AnySocketTypeAddrInfo(res.value()), host);
    const u32 data_size = static_cast<u32>(data.size());
    ctx.WriteBuffer(data, 0);

    return {data_size, GetAddrInfoError::SUCCESS};
}

void SFDNSRES::GetAddrInfoRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        Errno bsd_errno;
        GetAddrInfoError gai_error;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .gai_error = emu_gai_err,
        .data_size = data_size,
    });
}

void SFDNSRES::GetGaiStringErrorRequest(HLERequestContext& ctx) {
    struct InputParameters {
        GetAddrInfoError gai_errno;
    };
    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    const std::string result = Translate(input.gai_errno);
    ctx.WriteBuffer(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    // Additional options are ignored
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x10);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .gai_error = emu_gai_err,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

void SFDNSRES::ResolverSetOptionRequest(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    [[maybe_unused]] const u32 option_name = rp.Pop<u32>();
    // Option value is in a buffer
    [[maybe_unused]] const auto option_value_buffer = ctx.ReadBuffer(0);

    LOG_WARNING(Service, "(STUBBED) sfdnsres::ResolverSetOptionRequest called. Option: {}, Value Size: {}", option_name, option_value_buffer.size());

    // [OpenPak] The answer carries an errno word, as Eden has it.
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

// New Stub Implementations
void SFDNSRES::SetDnsAddresses(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::SetDnsAddresses called");
    // Takes input buffer of SockAddrIn. No direct output apart from Result.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetDnsAddressList(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetDnsAddressList called");
    // Writes SockAddrIn list to output buffer.
    // Returns u32 count, Errno bsd_errno.
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Count
    rb.PushEnum(Errno::OPNOTSUPP);
}

void SFDNSRES::GetHostByAddrRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetHostByAddrRequest called (deprecated)");
    // Similar return to GetHostByName: NetDbError, Errno, data_size
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushEnum(NetDbError::Internal);
    rb.PushEnum(Errno::OPNOTSUPP);
    rb.Push<u32>(0); // data_size
}

void SFDNSRES::GetHostStringError(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetHostStringError called");
    // Similar to GetGaiStringError: takes error code, returns string in buffer.
    // Returns u32 data_size.
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // data_size
}

void SFDNSRES::GetCancelHandleRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetCancelHandleRequest called");
    // GetCancelHandleRequest(u64 pid_placeholder, pid) -> u32 handle
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
}

void SFDNSRES::CancelRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::CancelRequest called");
    // Takes handle. Returns Result.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetOptions(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetOptions called");
    // Takes option name. Returns option value (u32/buffer?), Errno.
    IPC::ResponseBuilder rb{ctx, 4}; // Result, value (u32 placeholder), errno
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // Placeholder for option value
    rb.PushEnum(Errno::OPNOTSUPP);
}

void SFDNSRES::GetAddrInfoRequestRaw(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetAddrInfoRequestRaw called");
    // Similar to GetAddrInfoRequest: Errno, GetAddrInfoError, data_size
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushEnum(Errno::OPNOTSUPP);
    rb.PushEnum(GetAddrInfoError::AGAIN); // Changed from INTERNAL to AGAIN
    rb.Push<u32>(0); // data_size
}

// Stubs for functions from original registration table not in Switchbrew sfdnsres
void SFDNSRES::GetNameInfoRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetNameInfoRequest called");
    IPC::ResponseBuilder rb{ctx, 5}; // Similar to GetAddrInfoRequest
    rb.Push(ResultSuccess);
    rb.PushEnum(Errno::OPNOTSUPP);
    rb.PushEnum(GetAddrInfoError::AGAIN); // Changed from INTERNAL to AGAIN
    rb.Push<u32>(0);
}

void SFDNSRES::GetNameInfoRequestWithOptions(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetNameInfoRequestWithOptions called");
    IPC::ResponseBuilder rb{ctx, 6}; // Similar to GetAddrInfoRequestWithOptions
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // data_size
    rb.PushEnum(GetAddrInfoError::AGAIN); // Changed from INTERNAL to AGAIN
    rb.PushEnum(NetDbError::Internal);    // This should be fine as NetDbError::Internal is defined
    rb.PushEnum(Errno::OPNOTSUPP);
}

} // namespace Service::Sockets
