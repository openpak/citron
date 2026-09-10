// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The OpenPak account API, as the emulator uses it. Everything here talks to openpak.org over
// public TLS with the website's API token; the redirected Nintendo names never see this token.
//
//   POST /api/v1/token                       email + password -> bearer
//   GET  /api/v1/me                          display name, avatar
//   GET  /api/v1/me/switch                   pid, friend code, nnex token, friends with presence
//   POST /api/v1/me/switch/friend            request by friend code; accept / decline by pid
//   POST /api/v1/me/friends/remove           by account id
//   GET  /api/v1/status                      players per title
//   GET/PUT /api/v1/me/saves/citron/<title>  cloud saves, versioned: a PUT names the version it
//                                            loaded from and a stale one becomes a conflict
//   GET  /ca.pem                             the OpenPak CA, pinned by the SSL service
//
// What the Nextendo server offered and OpenPak does not (lobby, recent players, reports, history,
// BCAT seeds, profile edits from the emulator) answers "not available" here so the UI degrades
// instead of failing.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/x509_vfy.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/openpak_account.h"
#include "common/string_util.h"
#include "web_service/openpak_api.h"

namespace WebService::OpenPakApi {

namespace {

constexpr const char* CanonicalUrl = "https://openpak.org";
constexpr const char* SavesPlatform = "citron";
constexpr int TimeoutSeconds = 15;

constexpr std::string_view NotAvailable = "Not available on OpenPak yet.";
constexpr std::string_view Unreachable = "Could not reach openpak.org.";
constexpr std::string_view SessionExpired = "Your session expired. Sign in again.";

std::string Base64StdEncode(std::span<const u8> data) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const u32 n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(table[(n >> 6) & 63]);
        out.push_back(table[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        u32 n = data[i] << 16;
        if (i + 1 < data.size()) {
            n |= data[i + 1] << 8;
        }
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<u8> Base64StdDecode(std::string_view text) {
    static constexpr auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+' || c == '-') {
            return 62;
        }
        if (c == '/' || c == '_') {
            return 63;
        }
        return -1;
    };
    std::vector<u8> out;
    out.reserve(text.size() / 4 * 3);
    u32 buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

bool IsLoopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
}

// CMakeModules/openssl_build.cmake links web_service against a static OpenSSL built from source,
// whose default cert paths point inside the build tree. On Linux, point at the distro bundle;
// elsewhere httplib's own native store loader takes over.
void ApplyCaCertPath(httplib::Client& client) {
#ifdef __linux__
    static constexpr std::array<const char*, 4> candidates{
        "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Arch
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL/CentOS
        "/etc/ssl/cert.pem",                  // Alpine
        "/etc/ssl/ca-bundle.pem",             // openSUSE
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            client.set_ca_cert_path(path);
            return;
        }
    }
    LOG_ERROR(WebService, "ApplyCaCertPath: no known system CA bundle found");
#else
    (void)client;
#endif
}

// "https://host[:port]" with no path. The token rides on these requests, so only https, or
// loopback for a local stack, may receive it.
std::optional<std::string> SanitizeBaseUrl(std::string raw) {
    raw = Common::StripSpaces(raw);
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();
    }
    const auto scheme_end = raw.find("://");
    if (raw.empty() || scheme_end == std::string::npos) {
        return std::nullopt;
    }
    const std::string scheme = raw.substr(0, scheme_end);
    const std::string authority = raw.substr(scheme_end + 3);
    if (authority.empty() || authority.find('/') != std::string::npos) {
        return std::nullopt;
    }
    std::string host = authority;
    if (const auto colon = host.rfind(':'); colon != std::string::npos && host.back() != ']') {
        host = host.substr(0, colon);
    }
    if (IsLoopback(host) || scheme == "https") {
        return raw;
    }
    return std::nullopt;
}

httplib::Client& SharedClient() {
    static httplib::Client client = [] {
        httplib::Client c{BaseUrl()};
        c.set_connection_timeout(TimeoutSeconds);
        c.set_read_timeout(TimeoutSeconds);
        c.set_follow_location(true);
        c.set_keep_alive(true);
        ApplyCaCertPath(c);
        return c;
    }();
    return client;
}

httplib::Result Send(const std::string& method, const std::string& path, const std::string& body,
                     const std::string& bearer, const httplib::Headers& extra_headers = {},
                     const std::string& content_type = "application/json") {
    static std::mutex client_mutex;
    std::lock_guard lock{client_mutex};
    httplib::Client& client = SharedClient();

    httplib::Headers headers{{"User-Agent", "citron-openpak"}};
    if (!bearer.empty()) {
        headers.emplace("Authorization", "Bearer " + bearer);
    }
    for (const auto& [key, value] : extra_headers) {
        headers.emplace(key, value);
    }

    auto result = method == "GET"      ? client.Get(path, headers)
                  : method == "PUT"    ? client.Put(path, headers, body, content_type)
                  : method == "DELETE" ? client.Delete(path, headers)
                                       : client.Post(path, headers, body, content_type);
    if (!result) {
        const long verify_result = client.get_openssl_verify_result();
        LOG_ERROR(WebService, "Send {} {}: httplib error={}, openssl verify_result={} ({})",
                  method, path, httplib::to_string(result.error()), verify_result,
                  X509_verify_cert_error_string(verify_result));
    }
    return result;
}

// A 401 on an authenticated call means the stored token is expired or revoked. Only a real 401
// counts: network errors and 5xx must never clear a good session.
bool ClearSessionIfRejected(const httplib::Result& result) {
    if (!result || result->status != 401) {
        return false;
    }
    LOG_WARNING(WebService, "openpak.org rejected the stored account token; signing out");
    Common::OpenPakAccount::Clear();
    return true;
}

// The website answers failures with {"error": "..."}.
std::string ErrorFrom(const std::string& payload, const std::string& fallback) {
    try {
        const auto json = nlohmann::json::parse(payload);
        if (json.contains("error") && json["error"].is_string()) {
            return json["error"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
    }
    return fallback;
}

std::string Bearer() {
    return Common::OpenPakAccount::GetBearer();
}

Friend ParseFriend(const nlohmann::json& json) {
    Friend out;
    out.pid = json.value("pid", u64{0});
    out.account_id = json.value("account_id", std::string{});
    out.name = json.value("name", std::string{});
    out.friend_code = json.value("friend_code", std::string{});
    if (const auto presence = json.find("presence"); presence != json.end() && presence->is_object()) {
        out.presence_status = presence->value("status", s32{0});
        // The adapter hands back what the game server published: base64 text wrapping the raw
        // AppKeyValueStorage blob the guest's UserPresenceImpl.app_key_value expects.
        const auto decoded = Base64StdDecode(presence->value("app_field", std::string{}));
        out.app_field = std::string{reinterpret_cast<const char*>(decoded.data()), decoded.size()};
        out.app_id = presence->value("app_id", std::string{});
    }
    return out;
}

// Fetches /api/v1/me/switch: the whole Switch-side view of the signed-in account.
std::optional<nlohmann::json> FetchSwitch(std::string& error) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        error = "Not signed in.";
        return std::nullopt;
    }
    const auto result = Send("GET", "/api/v1/me/switch", {}, bearer);
    if (ClearSessionIfRejected(result)) {
        error = std::string{SessionExpired};
        return std::nullopt;
    }
    if (!result) {
        error = std::string{Unreachable};
        return std::nullopt;
    }
    if (result->status != 200) {
        error = ErrorFrom(result->body, fmt::format("Could not load your Switch identity (HTTP {}).",
                                                    result->status));
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(result->body);
    } catch (const nlohmann::json::exception& e) {
        error = fmt::format("Unexpected response: {}", e.what());
        return std::nullopt;
    }
}

std::string FriendAction(const nlohmann::json& body) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return "Not signed in.";
    }
    const auto result = Send("POST", "/api/v1/me/switch/friend", body.dump(), bearer);
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

// The version each title's save was last pulled at, so a push can say what it was based on.
std::mutex g_save_versions_mutex;
std::map<std::string, std::string> g_save_versions;

} // Anonymous namespace

std::string BaseUrl() {
    static const std::string url = [] {
        const char* env = std::getenv("OPENPAK_API");
        if (env && *env) {
            if (const auto sanitized = SanitizeBaseUrl(env)) {
                return *sanitized;
            }
            LOG_WARNING(WebService,
                        "Ignoring OPENPAK_API=\"{}\": only https, or loopback, may receive the "
                        "account token",
                        env);
        }
        return std::string{CanonicalUrl};
    }();
    return url;
}

bool FetchCA() {
    const auto result = Send("GET", "/ca.pem", {}, {});
    if (!result || result->status != 200 || result->body.find("BEGIN CERTIFICATE") == std::string::npos) {
        LOG_WARNING(WebService, "FetchCA: no CA from {} (HTTP {})", BaseUrl(),
                    result ? result->status : 0);
        return false;
    }
    const auto path = Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir) / "openpak" / "ca.pem";
    void(Common::FS::CreateParentDirs(path));
    if (!Common::FS::WriteStringToFile(path, Common::FS::FileType::TextFile, result->body)) {
        LOG_WARNING(WebService, "FetchCA: could not write the CA");
        return false;
    }
    LOG_INFO(WebService, "FetchCA: stored the OpenPak CA; it is pinned from the next launch");
    return true;
}

LoginResult SignIn(const std::string& email, const std::string& password) {
    LoginResult out;
    const nlohmann::json credentials{{"email", email}, {"password", password}};
    const auto minted = Send("POST", "/api/v1/token", credentials.dump(), {});
    if (!minted) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (minted->status == 401) {
        out.error = "Wrong email or password.";
        return out;
    }
    if (minted->status == 429) {
        out.error = "Too many attempts. Wait a minute and try again.";
        return out;
    }
    if (minted->status != 201 && minted->status != 200) {
        out.error = ErrorFrom(minted->body, fmt::format("Sign-in failed (HTTP {}).", minted->status));
        return out;
    }
    try {
        out.bearer = nlohmann::json::parse(minted->body).value("token", std::string{});
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected sign-in response: {}", e.what());
        return out;
    }
    if (out.bearer.empty()) {
        out.error = "The website issued no token.";
        return out;
    }

    const auto identity = Send("GET", "/api/v1/me/switch", {}, out.bearer);
    if (!identity) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (identity->status != 200) {
        out.error = ErrorFrom(identity->body,
                              fmt::format("Could not load your Switch identity (HTTP {}).", identity->status));
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(identity->body);
        out.pid = json.value("pid", u64{0});
        out.username = json.value("username", std::string{});
        out.friend_code = json.value("friend_code", std::string{});
        out.token = json.value("token", std::string{});
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected identity response: {}", e.what());
        return out;
    }
    if (out.pid == 0 || out.token.empty()) {
        out.error = "The website answered without a Switch identity.";
        return out;
    }
    FetchCA();
    out.ok = true;
    LOG_INFO(WebService, "SignIn: pid={} username={}", out.pid, out.username);
    return out;
}

OnlineStatus GetOnlineStatus() {
    // OpenPak has no per-account online gate: a signed-in account may play.
    OnlineStatus out;
    if (Bearer().empty()) {
        return out;
    }
    out.queried = true;
    out.allow = true;
    return out;
}

std::vector<u8> DownloadBcatSeed(const std::string& title_id_hex) {
    (void)title_id_hex;
    return {};
}

std::string HashBcatSeedHex(const std::vector<u8>& data) {
    (void)data;
    return {};
}

std::optional<std::vector<u8>> PullSave(const std::string& title_id_hex) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return std::nullopt;
    }
    const std::string path = fmt::format("/api/v1/me/saves/{}/{}", SavesPlatform, title_id_hex);
    const auto result = Send("GET", path, {}, bearer);
    if (ClearSessionIfRejected(result)) {
        return std::nullopt;
    }
    if (!result || result->status == 204) {
        return std::nullopt; // nothing stored yet
    }
    if (result->status != 200) {
        LOG_WARNING(WebService, "OpenPak save pull failed (HTTP {})", result->status);
        return std::nullopt;
    }
    {
        std::lock_guard lock{g_save_versions_mutex};
        g_save_versions[title_id_hex] = result->get_header_value("X-Save-Version");
    }
    return std::vector<u8>(result->body.begin(), result->body.end());
}

std::string PushSave(const std::string& title_id_hex, std::span<const u8> data) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return "Not signed in.";
    }
    httplib::Headers headers{{"X-Save-Device", "citron"}};
    {
        std::lock_guard lock{g_save_versions_mutex};
        if (const auto it = g_save_versions.find(title_id_hex); it != g_save_versions.end() && !it->second.empty()) {
            headers.emplace("X-Save-Base", it->second);
        }
    }
    const std::string body(reinterpret_cast<const char*>(data.data()), data.size());
    const std::string path = fmt::format("/api/v1/me/saves/{}/{}", SavesPlatform, title_id_hex);
    const auto result = Send("PUT", path, body, bearer, headers, "application/octet-stream");
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status == 507) {
        return "Your OpenPak storage allowance is full. Connect your own storage at openpak.org.";
    }
    if (result->status != 201 && result->status != 200) {
        return ErrorFrom(result->body, "Could not upload the save.");
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        std::lock_guard lock{g_save_versions_mutex};
        g_save_versions[title_id_hex] = std::to_string(json.value("number", 0));
    } catch (const nlohmann::json::exception&) {
    }
    return {};
}

std::map<std::string, int> GetOnlineCounts() {
    std::map<std::string, int> out;
    const auto result = Send("GET", "/api/v1/status", {}, {});
    if (!result || result->status != 200) {
        LOG_WARNING(WebService, "OpenPak status fetch failed (HTTP {})", result ? result->status : 0);
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        for (const auto& row : json.value("titles", nlohmann::json::array())) {
            const std::string title_id = Common::ToLower(row.value("title_id", std::string{}));
            if (!title_id.empty()) {
                out[title_id] += row.value("players", 0);
            }
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_WARNING(WebService, "Unexpected status response: {}", e.what());
    }
    return out;
}

int GetNzpOnlineCount() {
    const char* env = std::getenv("NZP_API");
    if (!env || !*env) {
        return 0;
    }
    httplib::Client client{env};
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    const auto result = client.Get("/online-count");
    if (!result || result->status != 200) {
        return 0;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        if (const auto it = json.find("count"); it != json.end() && it->is_number_integer()) {
            return it->get<int>();
        }
    } catch (const nlohmann::json::exception&) {
    }
    return 0;
}

Profile GetProfile() {
    Profile out;
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        out.error = "Not signed in.";
        return out;
    }
    const auto result = Send("GET", "/api/v1/me", {}, bearer);
    if (ClearSessionIfRejected(result)) {
        out.error = std::string{SessionExpired};
        return out;
    }
    if (!result) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (result->status != 200) {
        out.error = ErrorFrom(result->body, fmt::format("Could not load profile (HTTP {}).", result->status));
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        out.name = json.value("display_name", std::string{});
        out.console_nickname = out.name;
        if (const auto avatar = json.find("avatar_url"); avatar != json.end() && avatar->is_string()) {
            const auto image = Send("GET", avatar->get<std::string>(), {}, bearer);
            if (image && image->status == 200) {
                out.image_base64 = Base64StdEncode(std::span<const u8>{
                    reinterpret_cast<const u8*>(image->body.data()), image->body.size()});
            }
        }
        out.ok = true;
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected profile response: {}", e.what());
    }
    return out;
}

std::string PushProfilePicture(const std::string& image_base64) {
    (void)image_base64;
    return "Change your picture on openpak.org.";
}

std::string SetUsername(const std::string& username) {
    (void)username;
    return "Change your name on openpak.org.";
}

HistoryList GetHistory() {
    HistoryList out;
    out.ok = true;
    return out;
}

void SyncHistory(const std::vector<HistoryEntry>& entries) {
    (void)entries;
}

FriendList GetFriends() {
    FriendList out;
    const auto json = FetchSwitch(out.error);
    if (!json) {
        LOG_WARNING(WebService, "GetFriends: {}", out.error);
        return out;
    }
    try {
        for (const auto& entry : json->value("friends", nlohmann::json::array())) {
            out.friends.push_back(ParseFriend(entry));
        }
        for (const auto& entry : json->value("requests", nlohmann::json::array())) {
            out.requests.push_back(ParseFriend(entry));
        }
        out.ok = true;
        LOG_INFO(WebService, "GetFriends: {} friend(s), {} request(s)", out.friends.size(),
                 out.requests.size());
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected friends response: {}", e.what());
    }
    return out;
}

std::string AddFriendByCode(const std::string& friend_code) {
    return FriendAction({{"action", "request"}, {"friend_code", friend_code}});
}

std::string AcceptFriend(u64 pid) {
    return FriendAction({{"action", "accept"}, {"pid", pid}});
}

std::string DeclineFriend(u64 pid) {
    return FriendAction({{"action", "decline"}, {"pid", pid}});
}

std::string RemoveFriend(u64 pid) {
    // The website removes by account id; the pid maps to one through the friend list.
    std::string error;
    const auto json = FetchSwitch(error);
    if (!json) {
        return error;
    }
    std::string account_id;
    for (const auto& entry : json->value("friends", nlohmann::json::array())) {
        if (entry.value("pid", u64{0}) == pid) {
            account_id = entry.value("account_id", std::string{});
        }
    }
    if (account_id.empty()) {
        return "That player is not on your friend list.";
    }
    const auto result = Send("POST", "/api/v1/me/friends/remove",
                             nlohmann::json{{"account_id", account_id}}.dump(), Bearer());
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

void PushProfileName(const std::string& name) {
    // The display name is the account's own, edited on the website; the console nickname is
    // not a separate thing on OpenPak.
    (void)name;
}

void PushPresence(s32 status, const std::string& app_field, const std::string& app_id,
                  const std::string& app_name) {
    // Presence on OpenPak comes from the game servers a title is playing on, which register
    // the session with the account core; the emulator has nothing to add.
    (void)status;
    (void)app_field;
    (void)app_id;
    (void)app_name;
}

std::string SendInvitation(const std::vector<u64>& target_pids, std::span<const u8> app_param) {
    (void)target_pids;
    (void)app_param;
    return std::string{NotAvailable};
}

std::vector<ReceivedInvitation> PollInvitations() {
    return {};
}

Lobby GetMyLobby() {
    return {};
}

std::vector<LobbyPlayer> GetRecentPlayers() {
    return {};
}

std::string GetAvatarByPid(u64 pid) {
    (void)pid;
    return {};
}

std::string ReportPlayer(u64 pid, const std::string& reason, const std::string& comment) {
    (void)pid;
    (void)reason;
    (void)comment;
    return std::string{NotAvailable};
}

std::optional<int> PingBackend() {
    const auto start = std::chrono::steady_clock::now();
    const auto result = Send("GET", "/healthz", {}, {});
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    return static_cast<int>(elapsed_ms.count());
}

} // namespace WebService::OpenPakApi
