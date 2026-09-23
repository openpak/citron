// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/settings.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/kernel/svc/nextendo_deadline_watch.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/memory.h"
#include "openpak/network_profile.h"

namespace Service::Sockets {

static std::mutex g_last_host_mutex;
static std::unordered_map<std::string, std::string> g_last_host_for_ip;

void SetLastHostForIp(const std::string& ip, const std::string& host) {
    std::lock_guard lock(g_last_host_mutex);
    g_last_host_for_ip[ip] = host;
}

std::string GetLastHostForIp(const std::string& ip) {
    std::lock_guard lock(g_last_host_mutex);
    auto it = g_last_host_for_ip.find(ip);
    if (it != g_last_host_for_ip.end()) {
        return it->second;
    }
    return "";
}

// [Nextendo] See sfdnsres.h's declaration comment. Real bug this exists for (Splatoon 3,
// confirmed via Ryujinx-Nextendo hitting the identical failure on the same guest binary):
// its gRPC channel resolves a redirected Nextendo hostname correctly here, then later loses
// that address in its own connection-establishment plumbing and calls connect() with a
// zeroed IP -- but the SAME port it originally resolved for. Recording "port -> resolved IP"
// at resolution time lets BSD::ConnectImpl recover the real address for that exact port.
static std::mutex g_last_ip_for_port_mutex;
static std::unordered_map<u16, Network::IPv4Address> g_last_ip_for_port;

void SetLastIpForPort(u16 port, Network::IPv4Address ip) {
    if (port == 0) {
        return;
    }
    std::lock_guard lock(g_last_ip_for_port_mutex);
    g_last_ip_for_port[port] = ip;
}

std::optional<Network::IPv4Address> GetLastIpForPort(u16 port) {
    if (port == 0) {
        return std::nullopt;
    }
    std::lock_guard lock(g_last_ip_for_port_mutex);
    auto it = g_last_ip_for_port.find(port);
    if (it != g_last_ip_for_port.end()) {
        return it->second;
    }
    return std::nullopt;
}

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

// [Nextendo] La redirection est-elle active ?
//
// Le reglage « enable_openpak » n'existe QUE dans la facade Qt (src/citron/main.cpp) : la facade
// SDL (citron_cmd) ne le cable nulle part et le reecrit a sa valeur par defaut, false, au
// demarrage. Mesure du 2026-08-25 : lance par citron-cmd, Splatoon 3 a resolu
// « t-dce9377b-lp1.lp1.t.npln.srv.nintendo.net » vers 34.49.112.177 — le VRAI serveur de Nintendo —
// alors que le fichier de configuration portait bien enable_openpak=true.
//
// On accepte donc aussi une activation par l'environnement, exactement comme GetConfiguredIp le
// fait deja pour les deux adresses. Une valeur vide, « 0 », « false » ou « no » ne l'active pas.
static bool RedirectionNextendoActive() {
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

static std::optional<std::string> GetNextendoRedirectIp(const std::string& host) {
    if (!RedirectionNextendoActive()) {
        return std::nullopt;
    }

    // [Nextendo][DIAG] Opt-in certificate-validation control for Stardew. Only this exact public
    // tenant hostname bypasses redirection; bsd.cpp suppresses the first client record after the
    // server flight so no authenticated request can reach the normal endpoint.
    if (host == "t-9f607adf-lp1.lp1.t.npln.srv.nintendo.net") {
        const char* probe = std::getenv("NEXTENDO_STARDEW_TLS_PROBE");
        if (probe != nullptr && *probe != '\0' && std::string_view(probe) != "0") {
            LOG_INFO(Service, "[OpenPak][DIAG] Stardew TLS probe using normal DNS for '{}'", host);
            return std::nullopt;
        }
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

    if (host == "nintendo.net" || host.ends_with(".nintendo.net") ||
        host == "nintendo.com" || host.ends_with(".nintendo.com") ||
        host == "nintendowifi.net" || host.ends_with(".nintendowifi.net") ||
        host == "nintendo.co.jp" || host.ends_with(".nintendo.co.jp")) {
        LOG_INFO(Service, "[OpenPak] Redirecting Nintendo host '{}' -> '{}'", host, server_ip);
        return server_ip;
    }

    return std::nullopt;
}

// [Nextendo] Debug-only tap: redirects an "npln" host straight to a local TLS-terminating
// proxy instead of production, for protocol inspection. Independent of enable_openpak (this
// isn't a Nextendo-server redirect, just a temporary debugging aid) -- unset by default, so it
// never affects a normal run. NEXTENDO_S3_DEBUG_PROXY_IP=<ip> to enable.
static std::optional<std::string> GetNplnDebugProxyIp(const std::string& host) {
    if (Common::ToLower(host).find("npln") == std::string::npos) {
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_S3_DEBUG_PROXY_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting npln host '{}' -> debug proxy '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Redirects Outbound's Photon traffic (ns.photonengine.io and friends) to our own
// self-hosted Photon-protocol-compatible server instead of Photon Cloud -- see HANDOFF.md's
// "Self-hosted Photon server" section in outbound-nextendo for why. Independent of
// enable_openpak, same reasoning as GetNplnDebugProxyIp above: a separate redirect target,
// unset by default so it never affects a normal (non-Outbound) run.
// OPENPAK_PHOTON_IP=<ip> to enable.
static std::optional<std::string> GetPhotonRedirectIp(const std::string& host) {
    if (Common::ToLower(host).find("photonengine.io") == std::string::npos) {
        return std::nullopt;
    }
    const char* env = std::getenv("OPENPAK_PHOTON_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting Photon host '{}' -> '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Clean-room PvZ: Battle for Neighborville (Switch) research hook. BFN is an EA
// Frostbite title: static string analysis of its own executable (2026-08-31, see pvz-nextendo
// handoff.md) found an EA GOS/Blaze + Nucleus backend — service discovery via
// spring18.gosredirector.ea.com, identity via accounts/gateway/signin.ea.com. None of these are
// covered by the generic Nintendo redirect. Unlike the CTR hook (exact-host, one auth host
// known), the full set of *.ea.com hosts the client may contact is not yet known, so this
// matches the whole .ea.com suffix — but is gated by its own env var so it is dead code unless
// someone explicitly runs PvZ research. Fails closed: if the var is unset, EA hosts resolve
// normally (i.e. NOT our problem), and with the var set every .ea.com name goes to the local
// research stub instead of production. NEXTENDO_PVZ_EA_IP=<ip> to enable.
static std::optional<std::string> GetPvzEaRedirectIp(const std::string& host) {
    const std::string lower_host = Common::ToLower(host);
    const std::string suffix = ".ea.com";
    const bool is_ea = lower_host == "ea.com" ||
                       (lower_host.size() > suffix.size() &&
                        lower_host.compare(lower_host.size() - suffix.size(), suffix.size(),
                                           suffix) == 0);
    if (!is_ea) {
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_PVZ_EA_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting PvZ EA host '{}' -> '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Clean-room Crash Team Racing Nitro-Fueled research hook. CTR:NF authenticates
// against Demonware, not Nintendo BAAS/NEX -- confirmed live 2026-08-31 (see ctr-nextendo's
// handoff.md), so it isn't covered by GetNextendoRedirectIp's Nintendo-hostname redirect below.
// Exact-host only, same shape as the Fall Guys EOS hook: unset by default, never applied to
// telemetry hosts. Covers both the auth3 host and the lobby ("LSG") host that the client
// resolves next once auth looks accepted -- static analysis of CTR:NF's own binary found an
// igNetTaskLsgGetTicket/igNetTaskLsgAuthenticateTicket sequence suggesting auth alone was never
// going to be sufficient (see handoff.md, Experiment 2026-08-31-7). Same env vars answer both for
// now, since they're being pointed at the same research stub; split them if that stops making
// sense. NEXTENDO_CTR_AUTH_IP=<ip> to enable.
static std::optional<std::string> GetCtrDemonwareAuthRedirectIp(const std::string& host) {
    const std::string lower_host = Common::ToLower(host);
    if (lower_host != "lavender-switch-auth3.prod.demonware.net" &&
        lower_host != "lavender-switch-lobby.prod.demonware.net") {
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_CTR_AUTH_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting CTR:NF Demonware host '{}' -> '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Clean-room Among Us research hook. The Switch client's own cached
// region list (observed 2026-08-31 in a JKSV save backup, see
// amongus-nextendo handoff.md Experiment 2026-08-31-1) names three HTTPS
// matchmaker hosts: matchmaker.among.us (NA), matchmaker-eu.among.us (EU),
// matchmaker-as.among.us (Asia), all on port 443. Exact-host only, gated by
// its own env var, unset by default so it never affects any other title's
// run. NEXTENDO_AMONGUS_IP=<ip> to enable.
static std::optional<std::string> GetAmongUsMatchmakerRedirectIp(const std::string& host) {
    const std::string lower_host = Common::ToLower(host);
    if (lower_host != "matchmaker.among.us" && lower_host != "matchmaker-eu.among.us" &&
        lower_host != "matchmaker-as.among.us") {
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_AMONGUS_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting Among Us matchmaker host '{}' -> '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Clean-room Among Us safety block. Among Us research may run client
// versions older than the current production architecture (e.g. a base dump
// predating the HTTPS matchmaker era). Such versions can try legacy production
// endpoints -- old matchmaker hosts, Photon Cloud -- that this project has not
// approved for redirection. Opt-in gate NEXTENDO_AMONGUS_BLOCK=1 makes every
// resolution under the Among Us-era domains fail fast (EAI_AGAIN) WITHOUT
// reaching production; the requested hostname is still logged by the standard
// resolution logging above, so nothing is lost for research. Never blocks the
// three matchmaker hosts when NEXTENDO_AMONGUS_IP is set -- the redirect chain
// runs first and wins. Default unset so it never affects any other title.
static bool ShouldBlockAmongUsRelatedHost(const std::string& host) {
    const char* env = std::getenv("NEXTENDO_AMONGUS_BLOCK");
    if (!env || !*env) {
        return false;
    }
    const std::string lower_host = Common::ToLower(host);
    const auto has_suffix = [&lower_host](const std::string& suffix) {
        return lower_host.size() >= suffix.size() &&
               lower_host.compare(lower_host.size() - suffix.size(), suffix.size(),
                                  suffix) == 0;
    };
    return has_suffix(".among.us") || lower_host == "among.us" ||
           has_suffix(".photonengine.io") || lower_host == "photonengine.io" ||
           has_suffix(".photonengine.com") || lower_host == "photonengine.com" ||
           has_suffix(".exitgames.com") || lower_host == "exitgames.com" ||
           // EOS (observed 2026-08-31: the 2026.18.0 client embeds
           // EOSSDK-Switch-Shipping.nrs and retries api.epicgames.dev endlessly
           // during sign-in against production -- blocked here so Among Us runs
           // neither leak to EOS nor hang on a login that can never complete with
           // our local identity). NOTE: gate overlaps fallguys-nextendo's
           // NEXTENDO_FALLGUYS_EOS_IP redirect for the same host; that env var is
           // unset during Among Us runs, so the redirect chain never claims it
           // first and this block is the one that applies.
           lower_host == "api.epicgames.dev" ||
           // Unity Cloud Content / CCD telemetry + CDN hosts (observed at menu
           // load): read-only, but they are production contact -- block under the
           // same gate.
           has_suffix(".unity3dusercontent.com") ||
           lower_host == "unity3dusercontent.com";
}

// [Nextendo] Among Us EOS redirect. The 2026.18.0 client embeds the Epic Online
// Services SDK (EOSSDK-Switch-Shipping.nrs) and its sign-in path loops on
// api.epicgames.dev. When NEXTENDO_AMONGUS_EOS_IP is set, that host is pointed
// at a local EOS stub instead of being blocked by ShouldBlockAmongUsRelatedHost
// (this hook runs earlier in the chain, so the redirect wins when enabled).
// Unset by default. NEXTENDO_AMONGUS_EOS_IP=<ip> to enable.
static std::optional<std::string> GetAmongUsEosRedirectIp(const std::string& host) {
    if (Common::ToLower(host) != "api.epicgames.dev") {
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_AMONGUS_EOS_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting Among Us EOS host '{}' -> '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Clean-room Fall Guys EOS research hook. Redirect only the confirmed EOS bootstrap
// hostname to a user-controlled compatibility server. This is intentionally separate from the
// broad Nintendo redirect and the Outbound Photon redirect: unset by default, exact-host only,
// and never applied to telemetry hosts. NEXTENDO_FALLGUYS_EOS_IP=<ip> to enable.
static std::optional<std::string> GetFallGuysEosRedirectIp(const std::string& host) {
    if (Common::ToLower(host) != "api.epicgames.dev") {
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_FALLGUYS_EOS_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting Fall Guys EOS host '{}' -> '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Clean-room Minecraft Dungeons (Switch) research hook. Dungeons is a
// Mojang-published Switch title, but its client was observed resolving ONLY
// Microsoft/Mojang/Xbox hostnames up through the online sign-in screen (2026-08-31,
// citron-nextendo Nightly eed426e61-dirty; see mc-nextendo handoff.md "Hostname /
// Service Inventory"): launchercontent.mojang.com, vortex.data.microsoft.com
// (telemetry), title.mgt.xboxlive.com, sisu.xboxlive.com (device sign-in) and
// login.live.com (MSA OAuth) -- no *.nintendo.net host at all. So it is not covered
// by GetNextendoRedirectIp. Exact-host only, restricted to the confirmed inventory;
// newly observed hosts are added here only after the DNS log confirms them. Gated by
// its own env var, unset by default, so it never affects any other title's run.
// NEXTENDO_MC_DUNGEONS_IP=<ip> to enable.
// NEXTENDO_MC_DUNGEONS_ALLOW_SIGNIN=1 additionally excepts the three
// identity/config hosts (title.mgt.xboxlive.com, sisu.xboxlive.com, login.live.com),
// so a controlled run can sign in against the real Microsoft/Xbox services while
// every other confirmed Dungeons host still resolves to the local research listener.
// That combination is how the still-unknown multiplayer backend hostname is meant to
// be observed: the game gets past sign-in, its next DNS lookups land in the log, and
// only then are they added to this list (and to the bsd.cpp port-remap branch).
static bool IsMinecraftDungeonsSignInHost(const std::string& lower_host) {
    return lower_host == "title.mgt.xboxlive.com" || lower_host == "sisu.xboxlive.com" ||
           lower_host == "login.live.com";
}

static std::optional<std::string> GetMinecraftDungeonsRedirectIp(const std::string& host) {
    const std::string lower_host = Common::ToLower(host);
    const bool is_dungeons_host = lower_host == "launchercontent.mojang.com" ||
                                  lower_host == "vortex.data.microsoft.com" ||
                                  IsMinecraftDungeonsSignInHost(lower_host);
    if (!is_dungeons_host) {
        return std::nullopt;
    }
    const char* allow = std::getenv("NEXTENDO_MC_DUNGEONS_ALLOW_SIGNIN");
    if (allow && *allow && std::string(allow) != "0" &&
        IsMinecraftDungeonsSignInHost(lower_host)) {
        LOG_INFO(Service,
                 "[OpenPak] Letting Minecraft Dungeons sign-in host '{}' resolve normally "
                 "(NEXTENDO_MC_DUNGEONS_ALLOW_SIGNIN)",
                 host);
        return std::nullopt;
    }
    const char* env = std::getenv("NEXTENDO_MC_DUNGEONS_IP");
    if (!env || !*env) {
        return std::nullopt;
    }
    LOG_INFO(Service, "[OpenPak] Redirecting Minecraft Dungeons host '{}' -> '{}'", host, env);
    return std::string(env);
}

// [Nextendo] Optional delay before the first "npln" host resolution, against a hypothesized
// startup deadlock. Disabled by default (max_wait_ms=0) -- unconfirmed benefit, and a nonzero
// delay now blocks every other socket IPC call too (bsdsocket is single-threaded).
// NEXTENDO_NPLN_DELAY_MS opts back into a fixed wait if ever needed.
static std::once_flag g_npln_delay_once;

static void MaybeDelayNplnInit(const std::string& host) {
    if (Common::ToLower(host).find("npln") == std::string::npos) {
        return;
    }
    std::call_once(g_npln_delay_once, [] {
        int max_wait_ms = 0;
        if (const char* env = std::getenv("NEXTENDO_NPLN_DELAY_MS"); env && *env) {
            try {
                const int parsed = std::stoi(env);
                if (parsed >= 0) {
                    max_wait_ms = parsed;
                }
            } catch (const std::exception&) {
                // Malformed override -- keep the default rather than fail resolution over it.
            }
        }
        if (max_wait_ms <= 0) {
            return;
        }

        LOG_INFO(Service,
                 "[OpenPak] Holding the first npln host resolution until the JIT/shader-compile "
                 "burst settles before gRPC's connection setup starts ({} ms) (see "
                 "MaybeDelayNplnInit)",
                 max_wait_ms);

        std::this_thread::sleep_for(std::chrono::milliseconds(max_wait_ms));

        LOG_INFO(Service, "[OpenPak] npln hold finished after {} ms", max_wait_ms);

        // [Nextendo][DIAG] Arm a short window during which any finite, non-trivial
        // WaitSynchronization timeout gets logged -- see nextendo_deadline_watch.h. This is
        // trying to directly OBSERVE the game's gRPC call deadline (if it's implemented as a
        // timed kernel wait) rather than continuing to guess at it via static binary analysis.
        Kernel::Svc::ArmNextendoDeadlineWatch(90000);
        LOG_INFO(Service, "[OpenPak][DIAG] Deadline watch armed for 90000 ms");
    });
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

std::set<std::string> blocked_domains{
    // stupid hogwarts
    "phoenix-api.wbagora.com",
    // prevents various battle net games from crashing
    "battle.net",
    // minecraft from crashing
    "microsoft.com",
    "mojang.com",
    "xboxlive.com",
    "minecraftservices.com",
};

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);

    LOG_INFO(Service, "[OpenPak] DNS resolve (GetHostByName) requested: host={}", host);

    // [Nextendo] See MaybeDelayNplnInit's declaration comment.
    MaybeDelayNplnInit(host);

    if (parameters.use_nsd_resolve || host.find('%') != std::string::npos) {
        auto pos = host.find('%');
        if (pos != std::string::npos) {
            host.replace(pos, 1, "lp1");
        }
        if (host == "api.accounts.nintendo.com" || host == "accounts.nintendo.com") {
            host = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
        }
        LOG_INFO(Service, "[sfdnsres] NSD resolved host to '{}'", host);
    }

    std::string query_host = host;
    auto redirect = GetNplnDebugProxyIp(host);
    if (!redirect.has_value()) {
        redirect = GetPvzEaRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetCtrDemonwareAuthRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetAmongUsEosRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetAmongUsMatchmakerRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetFallGuysEosRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetMinecraftDungeonsRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetPhotonRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetNextendoRedirectIp(host);
    }
    if (redirect.has_value()) {
        query_host = *redirect;
    } else if (blocked_domains.find(host) != blocked_domains.end()) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    } else if (ShouldBlockAmongUsRelatedHost(host)) {
        LOG_WARNING(Network, "[OpenPak] Blocking Among Us-era host '{}' (NEXTENDO_AMONGUS_BLOCK)",
                    host);
        return {0, GetAddrInfoError::AGAIN};
    }

    auto res = Network::GetAddressInfo(query_host, /*service*/ std::nullopt);
    if (!res.has_value()) {
        return {0, Translate(res.error())};
    }

    // Preserve hostname context for diagnostics even when no Nextendo redirect is active.
    // This lets the BSD layer identify Outbound's Photon Name Server packets without logging
    // any packet payload. Literal-IP queries still bypass this block above.
    for (const auto& addrinfo : res.value()) {
        SetLastHostForIp(Network::IPv4AddressToString(addrinfo.addr.ip), host);
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
        // Ryujinx-Nextendo's AddrInfo4.Length fix (was sizeof(Array4<byte>)=4, needed to be
        // sizeof(AddrInfo4)=16): sin_len must be the full sockaddr size, not folded away.
        // [Nextendo]
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

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);

    LOG_INFO(Service, "[OpenPak] DNS resolve (GetAddrInfo) requested: host={}", host);

    // [Nextendo] See MaybeDelayNplnInit's declaration comment.
    MaybeDelayNplnInit(host);

    // [Nextendo] A literal IP has nothing to resolve -- return it as-is, before any of the
    // redirect/blocklist/NSD-rewrite logic below, all of which exist to turn a HOSTNAME into
    // the right address and have no business touching an address that's already one. See
    // TryParseIPv4Literal's declaration comment in internal_network/network.h for why falling
    // through to a real resolution here (which is what happened before this check existed) was
    // the actual root cause of Splatoon 3's NPLN connections completing TCP+TLS+HTTP/2 and then
    // silently closing without ever sending a HEADERS frame -- confirmed live: citron's own
    // connect cycles for this exact hostname resolved and connected correctly, but every one
    // still sent only a single small request and closed within ~1s of the server's reply, on
    // every cycle regardless of timing -- consistent with the game building an HTTP/2
    // :authority header from a corrupted canonical name, not any citron-side socket/scheduling
    // issue (both were separately investigated at length and ruled out).
    if (Network::IPv4Address literal_ip; Network::TryParseIPv4Literal(host, literal_ip)) {
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

        // Deliberately no SetLastHostForIp here, matching Ryujinx-Nextendo's own fix -- a
        // literal IP carries no hostname to record, and recording one would corrupt the
        // reverse lookup table used elsewhere for this exact purpose (see
        // GetLastHostForIp's declaration comment).
        const std::vector<u8> data = SerializeAddrInfo({entry}, host);
        const u32 data_size = static_cast<u32>(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }

    if (parameters.use_nsd_resolve || host.find('%') != std::string::npos) {
        auto pos = host.find('%');
        if (pos != std::string::npos) {
            host.replace(pos, 1, "lp1");
        }
        if (host == "api.accounts.nintendo.com" || host == "accounts.nintendo.com") {
            host = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
        }
        LOG_INFO(Service, "[sfdnsres] NSD resolved host to '{}'", host);
    }

    std::string query_host = host;
    auto redirect = GetNplnDebugProxyIp(host);
    if (!redirect.has_value()) {
        redirect = GetPvzEaRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetCtrDemonwareAuthRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetAmongUsEosRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetAmongUsMatchmakerRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetFallGuysEosRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetMinecraftDungeonsRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetPhotonRedirectIp(host);
    }
    if (!redirect.has_value()) {
        redirect = GetNextendoRedirectIp(host);
    }
    if (redirect.has_value()) {
        query_host = *redirect;
    } else if (blocked_domains.find(host) != blocked_domains.end()) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    } else if (ShouldBlockAmongUsRelatedHost(host)) {
        LOG_WARNING(Network, "[OpenPak] Blocking Among Us-era host '{}' (NEXTENDO_AMONGUS_BLOCK)",
                    host);
        return {0, GetAddrInfoError::AGAIN};
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
        // [Nextendo] GetAddressInfo never requests AI_CANONNAME, so canon_name comes back
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

        // [Nextendo] Port-keyed recovery for gRPC-based titles (Splatoon 3) that lose this
        // resolved address later. See SetLastIpForPort's declaration comment in sfdnsres.h.
        std::optional<u16> service_port;
        if (service.has_value()) {
            try {
                const int parsed = std::stoi(*service);
                if (parsed > 0 && parsed <= 0xFFFF) {
                    service_port = static_cast<u16>(parsed);
                }
            } catch (const std::exception&) {
                // service wasn't a plain port number (a named service like "http") -- nothing
                // to key the fallback on, and that's fine, most titles never need it anyway.
            }
        }
        for (const auto& addrinfo : res.value()) {
            SetLastHostForIp(Network::IPv4AddressToString(addrinfo.addr.ip), host);
            if (service_port.has_value()) {
                SetLastIpForPort(*service_port, addrinfo.addr.ip);
            }
        }
    } else {
        // Same metadata-only hostname tracking as GetHostByNameRequestImpl above. Normal DNS
        // results were previously discarded, leaving Photon traffic identifiable only by port.
        for (const auto& addrinfo : res.value()) {
            SetLastHostForIp(Network::IPv4AddressToString(addrinfo.addr.ip), host);
        }
    }

    const std::vector<u8> data = SerializeAddrInfo(res.value(), host);
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

    // Default success for stub
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
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
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void SFDNSRES::GetHostByAddrRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetHostByAddrRequest called (deprecated)");
    // Similar return to GetHostByName: NetDbError, Errno, data_size
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushEnum(NetDbError::Internal);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
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
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void SFDNSRES::GetAddrInfoRequestRaw(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetAddrInfoRequestRaw called");
    // Similar to GetAddrInfoRequest: Errno, GetAddrInfoError, data_size
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
    rb.PushEnum(GetAddrInfoError::AGAIN); // Changed from INTERNAL to AGAIN
    rb.Push<u32>(0); // data_size
}

// Stubs for functions from original registration table not in Switchbrew sfdnsres
void SFDNSRES::GetNameInfoRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) sfdnsres::GetNameInfoRequest called");
    IPC::ResponseBuilder rb{ctx, 5}; // Similar to GetAddrInfoRequest
    rb.Push(ResultSuccess);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
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
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

} // namespace Service::Sockets
