// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

// The OpenPak surface the Android app calls into.
//
// Everything the desktop build gets from its Qt host -- profiles with their own accounts, being
// online, cloud saves around a game, toasts, invitations into the game, the MyPage friend picker
// and the account window's seven pages -- lives in openpak-client and core, which have no UI of
// their own. This is the whole bridge: a few lifecycle calls, and one nativeCall(method, JSON)
// that answers JSON, so the Kotlin side needs no JNI object marshalling to grow a screen.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <common/android/android_common.h>
#include <common/android/id_cache.h>
#include <common/fs/path_util.h>
#include <common/logging.h>
#include <common/scm_rev.h>
#include <common/settings.h>
#include <common/string_util.h>
#include <common/uuid.h>
#include <jni.h>
#include <nlohmann/json.hpp>

#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "jni/native.h"
#include "jni/openpak_native.h"
#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/baas.h"
#include "openpak/compatibility.h"
#include "openpak/friends_cache.h"
#include "openpak/log.h"
#include "openpak/my_page.h"
#include "openpak/platform.h"
#include "openpak/save_sync.h"
#include "openpak/session.h"
#include "openpak/zip_store.h"

// The one line that differs between the forks: which Kotlin class the natives belong to.
#define OPENPAK_JNI(name) Java_org_citron_citron_1emu_utils_OpenPak_##name
#define OPENPAK_CLIENT_NAME "citron"
#define OPENPAK_EMULATOR_NAME "Citron"
#define OPENPAK_PATH(which) Common::FS::GetCitronPath(Common::FS::CitronPath::which)

namespace {

namespace Api = WebService::OpenPakApi;
namespace Session = openpak::client::session;
using json = nlohmann::json;

/// Pushed in by the emulation thread; read by the presence heartbeat and the invitation poll. An
/// atomic rather than a call into Core::System, because the heartbeat runs from app start to app
/// exit and the session it would be asking does not.
std::atomic<u64> g_running_title{0};
std::atomic<bool> g_cloud_sync{true};
std::atomic<bool> g_online{false};

/// The Kotlin object the natives hang off, for the one call that goes the other way: the MyPage
/// picker, asked from the applet's worker thread.
jobject g_bridge = nullptr;
jmethodID g_pick_friends = nullptr;

/// What the poll found for Kotlin to show, and what it has already said.
struct PollState {
    std::mutex mutex;
    std::vector<json> events;
    std::set<std::string> offered;   // asked Join/Ignore, or answered
    std::set<std::string> announced; // said once, waiting for their game to run
    std::map<u64, s32> last_status;
    std::map<u64, int> offline_streak;
    std::set<u64> last_requests;
    bool first_friends_poll = true; // no toast burst for everyone already online at boot
    std::string profile;            // the profile everything above belongs to
    int polls = 0;
};

PollState& Poll() {
    static PollState state;
    return state;
}

void Queue(json event) {
    std::lock_guard lock{Poll().mutex};
    Poll().events.push_back(std::move(event));
}

void Message(const std::string& text) {
    Queue({{"type", "message"}, {"text", text}});
}

jstring Text(JNIEnv* env, const std::string& value) {
    return Common::Android::ToJString(env, value);
}

std::string Hex(u64 id) {
    return fmt::format("{:016X}", id);
}

u64 ParseHex(const std::string& hex) {
    try {
        return hex.empty() ? 0 : std::stoull(hex, nullptr, 16);
    } catch (...) {
        return 0;
    }
}

Core::System& System() {
    return EmulationSession::GetInstance().System();
}

/// The Switch user the setting names, as the desktop host reads it.
std::optional<Common::UUID> CurrentUser() {
    return System().GetProfileManager().GetUser(
        static_cast<std::size_t>(Settings::values.current_user.GetValue()));
}

/// Kotlin names profiles as the formatted UUID; the library keys them by the raw one.
std::string ProfileKey(const std::string& uuid) {
    const Common::UUID parsed{uuid};
    return parsed.IsValid() ? parsed.RawString() : std::string{};
}

std::string ProfileName(const std::string& key) {
    const auto& manager = System().GetProfileManager();
    for (const auto& uuid : manager.GetAllUsers()) {
        Service::Account::ProfileBase profile{};
        if (uuid.IsValid() && uuid.RawString() == key && manager.GetProfileBase(uuid, profile)) {
            return Common::StringFromFixedZeroTerminatedBuffer(
                reinterpret_cast<const char*>(profile.username.data()), profile.username.size());
        }
    }
    return key;
}

/// Presence says what is being played. An empty answer is the game list, and reads as online.
std::string RunningTitleId() {
    const u64 program_id = g_running_title.load(std::memory_order_relaxed);
    return program_id == 0 ? std::string{} : fmt::format("{:016x}", program_id);
}

/// A title's name: the installed game's NACP, else the catalogue's, else what the caller heard.
std::string GameName(u64 title_id, const std::string& hint = {}) {
    if (title_id != 0) {
        try {
            const FileSys::PatchManager pm{title_id, System().GetFileSystemController(),
                                           System().GetContentProvider()};
            const auto [nacp, icon] = pm.GetControlMetadata();
            if (nacp && !nacp->GetApplicationName().empty()) {
                return nacp->GetApplicationName();
            }
        } catch (...) {
        }
        if (std::string name = openpak::compatibility::CatalogueName(title_id); !name.empty()) {
            return name;
        }
    }
    return hint.empty() ? std::string{"a game"} : hint;
}

/// nand/user/save/0000000000000000/<user>/<TITLEID>, the active profile's own, as the desktop
/// host builds it; a title with only a device save keeps it under the all-zero user.
std::filesystem::path SaveDirectory(u64 title_id) {
    const auto current = CurrentUser();
    if (!current || current->IsInvalid()) {
        return {};
    }
    const auto root = OPENPAK_PATH(NANDDir) / "user" / "save" / "0000000000000000";
    const std::string title = Hex(title_id);
    const auto id = current->AsU128();
    const auto own = root / fmt::format("{:016X}{:016X}", id[1], id[0]) / title;
    const auto device = root / std::string(32, '0') / title;
    std::error_code ec;
    if (!std::filesystem::is_directory(own, ec) && std::filesystem::is_directory(device, ec)) {
        return device;
    }
    return own;
}

/// load/<TITLEID>/<mod>: where the mods loader reads, so an installed mod is an add-on like any.
std::filesystem::path ModDirectory(u64 title_id) {
    return OPENPAK_PATH(LoadDir) / Hex(title_id);
}

std::string ModDirName(const Api::Mod& mod) {
    std::string name = mod.slug.empty() ? mod.name : mod.slug;
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') {
            c = '-';
        }
    }
    return name.find_first_not_of(" -.") == std::string::npos ? "openpak-mod" : name;
}

/// Where the account files live; the app's directories are only known once Kotlin set them up.
void SetDirectories() {
    openpak::Platform::SetDirectories(OPENPAK_PATH(ConfigDir), OPENPAK_PATH(CacheDir));
}

/// Everything shown for the last profile's account goes when the profile changes.
void ResetPollState() {
    PollState& state = Poll();
    std::lock_guard lock{state.mutex};
    state.last_status.clear();
    state.offline_streak.clear();
    state.last_requests.clear();
    state.first_friends_poll = true;
    state.profile = openpak::Platform::ProfileId();
}

/// The website's friend list into the guest's cache, and what changed since the last look into
/// toasts: a friend coming online or starting a game, a new friend request.
void PollFriends() {
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }
    const Api::FriendList list = Api::GetFriends();
    if (!list.ok) {
        return;
    }

    std::vector<Common::NextendoFriends::Entry> cache;
    for (const auto& entry : list.friends) {
        cache.push_back({entry.pid, entry.name, entry.presence_status, entry.app_field, {}});
    }
    Common::NextendoFriends::Set(std::move(cache));

    PollState& state = Poll();
    std::lock_guard lock{state.mutex};
    if (state.profile != openpak::Platform::ProfileId()) {
        return; // the profile changed while this was asking; the next poll starts over
    }
    const bool quiet = std::exchange(state.first_friends_poll, false);

    std::map<u64, s32> current;
    for (const auto& entry : list.friends) {
        const auto it = state.last_status.find(entry.pid);
        const bool was_online = it != state.last_status.end() && it->second != 0;
        // One poll at 0 can be a blip while a friend's presence changes; two are a goodbye.
        if (entry.presence_status == 0 && was_online) {
            if (++state.offline_streak[entry.pid] < 2) {
                current[entry.pid] = it->second;
                continue;
            }
        } else {
            state.offline_streak.erase(entry.pid);
            if (!quiet && !was_online && entry.presence_status != 0) {
                const std::string game =
                    entry.app_id.empty() ? std::string{} : GameName(ParseHex(entry.app_id), entry.app_name);
                state.events.push_back({{"type", "friend_online"},
                                        {"name", entry.name},
                                        {"game", game},
                                        {"avatar", entry.image_base64}});
            }
        }
        current[entry.pid] = entry.presence_status;
    }
    state.last_status = std::move(current);

    std::set<u64> requests;
    for (const auto& entry : list.requests) {
        requests.insert(entry.pid);
        if (!quiet && !state.last_requests.contains(entry.pid)) {
            state.events.push_back({{"type", "friend_request"},
                                    {"name", entry.name},
                                    {"avatar", entry.image_base64}});
        }
    }
    state.last_requests = std::move(requests);
}

/// What the heartbeat last read from the native inbox, offered as Ryujinx offers it: an invitation
/// to the running game asks Join or Ignore; one to another game is announced once and offered when
/// that game runs.
void PollInvitations() {
    const std::string running = RunningTitleId();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    PollState& state = Poll();
    std::lock_guard lock{state.mutex};
    for (const auto& invitation : Session::Invitations()) {
        if (state.offered.contains(invitation.id) ||
            (invitation.expires_at != 0 && invitation.expires_at < now)) {
            continue;
        }
        const std::string game = GameName(ParseHex(invitation.title_id));
        if (running.empty() || Common::ToLower(running) != Common::ToLower(invitation.title_id)) {
            if (state.announced.insert(invitation.id).second) {
                state.events.push_back({{"type", "invitation"},
                                        {"name", invitation.sender_name},
                                        {"game", game}});
            }
            continue;
        }
        state.offered.insert(invitation.id);
        state.events.push_back({{"type", "invitation_offer"},
                                {"id", invitation.id},
                                {"name", invitation.sender_name},
                                {"game", game}});
    }
}

/// The running game's invitation channel gets the sender's data, as an accepted invitation would.
std::string JoinInvitation(const std::string& id) {
    for (const auto& invitation : Session::Invitations()) {
        if (invitation.id != id) {
            continue;
        }
        const auto user = CurrentUser();
        std::string failure;
        if (!user || g_running_title.load() == 0 ||
            !System().GetAppletManager().PushFriendInvitation(*user, invitation.app_param)) {
            failure = "The game closed before the invitation could be handed over.";
        }
        std::thread{[id] { Session::DismissInvitation(id); }}.detach();
        return failure;
    }
    return "That invitation is gone.";
}

/// A friend's published session, straight into the running game's invitation channel.
std::string JoinFriend(u64 pid) {
    const auto entries = Common::NextendoFriends::Get();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [pid](const auto& e) { return e.pid == pid; });
    if (it == entries.end() || it->status != Common::NextendoFriends::PresenceOnlinePlay ||
        it->app_field.empty()) {
        return "That friend isn't in a joinable game right now.";
    }
    const auto user = CurrentUser();
    if (!user || g_running_title.load() == 0 ||
        !System().GetAppletManager().PushFriendInvitation(
            *user, std::vector<u8>(it->app_field.begin(), it->app_field.end()))) {
        return "Start the game first, then join from here.";
    }
    return {};
}

// ---- the MyPage picker: the applet's worker asks, Kotlin shows a dialog, the worker waits ----

std::mutex g_pick_mutex;
std::shared_ptr<std::promise<std::vector<u64>>> g_pick;

std::vector<u64> PickFriends(int max) {
    auto promise = std::make_shared<std::promise<std::vector<u64>>>();
    {
        std::lock_guard lock{g_pick_mutex};
        if (g_pick || g_bridge == nullptr || g_pick_friends == nullptr) {
            return {};
        }
        g_pick = promise;
    }
    auto answer = promise->get_future();

    // Online friends first: they are the ones an invitation reaches now.
    std::vector<openpak::baas::Friend> friends;
    if (const auto snapshot = openpak::baas::Friends()) {
        friends = *snapshot;
    }
    std::stable_sort(friends.begin(), friends.end(),
                     [](const auto& a, const auto& b) { return a.state > b.state; });
    json list = json::array();
    for (const auto& f : friends) {
        list.push_back({{"id", Hex(f.id)}, {"name", f.nickname}, {"state", f.state}});
    }

    JNIEnv* env = Common::Android::GetEnvForThread();
    jstring text = Text(env, list.dump());
    env->CallVoidMethod(g_bridge, g_pick_friends, static_cast<jint>(max), text);
    env->DeleteLocalRef(text);

    std::vector<u64> picked;
    if (answer.wait_for(std::chrono::minutes(10)) == std::future_status::ready) {
        picked = answer.get();
    }
    std::lock_guard lock{g_pick_mutex};
    g_pick.reset();
    return picked;
}

void FriendsPicked(const json& ids) {
    std::vector<u64> picked;
    for (const auto& id : ids) {
        if (id.is_string()) {
            if (const u64 value = ParseHex(id.get<std::string>()); value != 0) {
                picked.push_back(value);
            }
        }
    }
    std::lock_guard lock{g_pick_mutex};
    if (g_pick) {
        g_pick->set_value(std::move(picked));
        g_pick.reset();
    }
}

// ---- the account window's pages, as JSON ----

json FriendJson(const Api::AccountFriend& f) {
    return {{"account_id", f.account_id},
            {"pid", std::to_string(f.pid)},
            {"name", f.display_name},
            {"friend_code", f.friend_code},
            {"online", f.online},
            {"title_id", f.title_id},
            {"game", f.title_id.empty() ? std::string{} : GameName(ParseHex(f.title_id))},
            {"incoming", f.incoming}};
}

Api::AccountFriend FriendFrom(const json& args) {
    Api::AccountFriend f;
    f.account_id = args.value("account_id", std::string{});
    f.pid = std::strtoull(args.value("pid", std::string{"0"}).c_str(), nullptr, 10);
    f.display_name = args.value("name", std::string{});
    f.friend_code = args.value("friend_code", std::string{});
    f.incoming = args.value("incoming", false);
    return f;
}

json Status() {
    const std::string profile = openpak::Platform::ProfileId();
    return {{"enabled", Session::Enabled()},
            {"redirect", Settings::values.enable_openpak.GetValue()},
            {"signed_in", !Session::IdToken().empty()},
            {"linked", Session::Linked()},
            {"website_signed_in", Common::OpenPakAccount::IsLinked()},
            {"username", Common::OpenPakAccount::GetUsername()},
            {"nickname", Session::Nickname()},
            {"friend_code", Session::FriendCode().empty() ? Common::OpenPakAccount::GetFriendCode()
                                                          : Session::FriendCode()},
            {"user_id", Session::UserId()},
            {"profile", profile},
            {"profile_name", ProfileName(profile)},
            {"server", Settings::values.openpak_server_ip.GetValue()},
            {"site", Api::BaseUrl()},
            {"running", RunningTitleId()}};
}

json SignIn(const json& args) {
    const std::string email = args.value("email", std::string{});
    const std::string password = args.value("password", std::string{});
    const bool adopt = args.value("adopt", false);

    // Two sign-ins, because they are two different things and the desktop build does both. The
    // website account is what friends, invitations and cloud saves speak with.
    Api::LoginResult website = Api::SignIn(email, password);
    if (!website.ok) {
        return {{"error", website.error.empty() ? std::string{"Sign-in was refused."} : website.error}};
    }

    // One account, one profile: two profiles on one account would share a cloud-save slot and
    // overwrite each other's progress. Checked before the console link, which would otherwise
    // bind this profile's device account to it first.
    const std::string holder =
        Common::OpenPakAccount::HolderOf(website.pid, openpak::Platform::ProfileId());
    if (!holder.empty()) {
        std::thread{[bearer = website.bearer] { Api::RevokeToken(bearer); }}.detach();
        return {{"error", fmt::format("This OpenPak account is already linked to the profile \"{}\". "
                                      "Sign in there, or sign that profile out first.",
                                      ProfileName(holder))}};
    }

    // Kept, as the desktop build keeps it: every later website call reads the account from here.
    Common::OpenPakAccount::Save(website.pid, website.username, website.friend_code, website.token,
                                 website.bearer);
    Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnline);

    // And the console chain, which is what puts an identity in front of a title server.
    std::string link_failure;
    if (Session::Enabled()) {
        link_failure = Session::LinkWithPassword(email, password);
    }
    if (link_failure.empty() && g_online.load()) {
        Session::StartHeartbeat(RunningTitleId);
    }
    ResetPollState();
    std::thread{[] { Session::RefreshGuestFriends(); }}.detach();

    json out{{"error", ""}, {"username", website.username}, {"link_error", link_failure}};
    if (adopt) {
        // Linking copies the account's picture into the profile once; Kotlin scales and stores it.
        out["avatar"] = Api::GetAvatarByPid(website.pid);
    }
    return out;
}

void SignOut() {
    // The website token is revoked on the server, not only forgotten here, as Ryujinx does. The
    // console link stays: signing out of the website does not unlink a Switch either.
    std::thread{[bearer = Common::OpenPakAccount::GetBearer()] { Api::RevokeToken(bearer); }}.detach();
    Common::OpenPakAccount::Clear();
    Common::NextendoFriends::Set({});
    ResetPollState();
}

/// A profile is going: its bearer is revoked and its account and device account forgotten.
void ForgetProfile(const std::string& uuid) {
    const std::string key = ProfileKey(uuid);
    if (key.empty()) {
        return;
    }
    if (const std::string bearer = Common::OpenPakAccount::BearerOf(key); !bearer.empty()) {
        std::thread{[bearer] { Api::RevokeToken(bearer); }}.detach();
    }
    Common::OpenPakAccount::Forget(key);
    std::thread{[key] { Session::ForgetProfile(key); }}.detach();
}

/// Called after anything that may have changed the current user, as the desktop host's
/// ProfileMaybeChanged: the session notices by itself and signs the new profile in.
void ProfileChanged() {
    if (Poll().profile == openpak::Platform::ProfileId()) {
        return;
    }
    if (const auto uuid = CurrentUser()) {
        System().GetProfileManager().OpenUser(*uuid);
    }
    Common::NextendoFriends::Set({});
    ResetPollState();
    if (Settings::values.enable_openpak.GetValue()) {
        std::thread{[] {
            Session::Ensure();
            Session::RefreshGuestFriends();
        }}.detach();
    }
}

json Profiles() {
    json out = json::array();
    auto& manager = System().GetProfileManager();
    if (manager.GetUserCount() == 0) {
        manager.CreateNewUser(Common::UUID::MakeRandom(), OPENPAK_EMULATOR_NAME);
        manager.WriteUserSaveFile();
    }
    const auto current = CurrentUser();
    for (const auto& uuid : manager.GetAllUsers()) {
        if (uuid.IsInvalid()) {
            continue;
        }
        const std::string key = uuid.RawString();
        out.push_back({{"uuid", uuid.FormattedString()},
                       {"name", ProfileName(key)},
                       {"account", Common::OpenPakAccount::UsernameOf(key)},
                       {"current", current && *current == uuid}});
    }
    return out;
}

/// The profile manager, for a frontend that has no screen of its own for it (Citron's Android)
/// and for the setup, which makes and names profiles the way the desktop host does.
json ProfileAction(const json& args) {
    auto& manager = System().GetProfileManager();
    const std::string action = args.value("action", std::string{});
    const Common::UUID uuid{args.value("uuid", std::string{})};
    const std::string name = args.value("name", std::string{});

    if (action == "create") {
        if (!manager.CanSystemRegisterUser()) {
            return {{"error", "There is no room for another profile."}};
        }
        const Common::UUID created = Common::UUID::MakeRandom();
        if (manager.CreateNewUser(created, name.empty() ? std::string{OPENPAK_EMULATOR_NAME} : name)
                .IsError()) {
            return {{"error", "The profile could not be created."}};
        }
        manager.WriteUserSaveFile();
        return {{"error", ""}, {"uuid", created.FormattedString()}};
    }
    if (uuid.IsInvalid()) {
        return {{"error", "No such profile."}};
    }
    if (action == "select") {
        const auto index = manager.GetUserIndex(uuid);
        if (!index) {
            return {{"error", "No such profile."}};
        }
        Settings::values.current_user = static_cast<s32>(*index);
        ProfileChanged();
        return {{"error", ""}};
    }
    if (action == "rename") {
        Service::Account::ProfileBase profile{};
        if (name.empty() || !manager.GetProfileBase(uuid, profile)) {
            return {{"error", "No such profile."}};
        }
        const std::string trimmed = name.substr(0, profile.username.size() - 1);
        std::fill(profile.username.begin(), profile.username.end(), '\0');
        std::copy(trimmed.begin(), trimmed.end(), profile.username.begin());
        manager.SetProfileBase(uuid, profile);
        manager.WriteUserSaveFile();
        return {{"error", ""}};
    }
    if (action == "remove") {
        if (manager.GetUserCount() < 2) {
            return {{"error", "The last profile cannot be deleted."}};
        }
        ForgetProfile(uuid.FormattedString());
        const auto index = manager.GetUserIndex(uuid);
        if (index && Settings::values.current_user.GetValue() == static_cast<s32>(*index)) {
            Settings::values.current_user = 0;
        }
        if (!manager.RemoveUser(uuid)) {
            return {{"error", "The profile could not be deleted."}};
        }
        manager.WriteUserSaveFile();
        ProfileChanged();
        return {{"error", ""}};
    }
    if (action == "image") {
        // A 256x256 JPEG Kotlin made, where the account service reads a profile's picture.
        const auto target = OPENPAK_PATH(NANDDir) /
                            fmt::format("system/save/8000000000000010/su/avators/{}.jpg",
                                        uuid.FormattedString());
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::copy_file(args.value("path", std::string{}), target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return {{"error", ec ? ec.message() : std::string{}}};
    }
    return {{"error", "Unknown action."}};
}

json Friends() {
    const Api::AccountFriends list = Api::GetAccountFriends();
    json friends = json::array();
    json requests = json::array();
    for (const auto& f : list.friends) {
        friends.push_back(FriendJson(f));
    }
    for (const auto& f : list.requests) {
        requests.push_back(FriendJson(f));
    }
    return {{"ok", list.ok}, {"error", list.error}, {"friends", friends}, {"requests", requests},
            {"running", RunningTitleId()}};
}

std::string FriendAction(const json& args) {
    const std::string action = args.value("action", std::string{});
    if (action == "add") {
        return Api::SendFriendRequest(args.value("code", std::string{}));
    }
    const Api::AccountFriend f = FriendFrom(args);
    if (action == "accept") {
        return Api::AcceptFriendRequest(f);
    }
    if (action == "decline") {
        return Api::DeclineFriendRequest(f);
    }
    if (action == "remove") {
        return Api::RemoveAccountFriend(f.account_id);
    }
    if (action == "block") {
        return Api::BlockAccount(f.account_id);
    }
    if (action == "join") {
        return JoinFriend(f.pid);
    }
    return "Unknown action.";
}

json Invitations() {
    json out = json::array();
    const std::string running = Common::ToLower(RunningTitleId());
    for (const auto& invitation : Session::Invitations()) {
        std::string message;
        if (!invitation.messages.empty()) {
            const auto en = invitation.messages.find("en-US");
            message = en != invitation.messages.end() ? en->second : invitation.messages.begin()->second;
        }
        out.push_back({{"source", "console"},
                       {"id", invitation.id},
                       {"from", invitation.sender_name},
                       {"title_id", invitation.title_id},
                       {"game", GameName(ParseHex(invitation.title_id))},
                       {"message", message},
                       {"expires_at", invitation.expires_at},
                       {"joinable", !running.empty() &&
                                        running == Common::ToLower(invitation.title_id)}});
    }
    if (Common::OpenPakAccount::IsLinked()) {
        for (const auto& invitation : Api::GetInvitations()) {
            out.push_back({{"source", "website"},
                           {"id", invitation.id},
                           {"from", invitation.from},
                           {"title_id", invitation.title_id},
                           {"game", GameName(ParseHex(invitation.title_id))},
                           {"message", ""},
                           {"expires_at", invitation.expires_at},
                           {"joinable", false}});
        }
    }
    return out;
}

std::string InvitationAction(const json& args) {
    const std::string id = args.value("id", std::string{});
    const std::string action = args.value("action", std::string{});
    if (args.value("source", std::string{}) == "website") {
        return Api::DeclineInvitation(id);
    }
    {
        std::lock_guard lock{Poll().mutex};
        Poll().offered.insert(id);
    }
    if (action == "join") {
        return JoinInvitation(id);
    }
    Session::DismissInvitation(id);
    return {};
}

const char* LocalStateName(Nextendo::SaveSync::LocalState state) {
    using Nextendo::SaveSync::LocalState;
    switch (state) {
    case LocalState::NoLocal:
        return "no_local";
    case LocalState::InStep:
        return "in_step";
    case LocalState::ChangedHere:
        return "changed_here";
    case LocalState::CloudNewer:
        return "cloud_newer";
    case LocalState::NoHistory:
        return "no_history";
    }
    return "";
}

json CloudSaves() {
    const Api::CloudSaves saves = Api::GetCloudSaves();
    json titles = json::array();
    for (const auto& save : saves.saves) {
        const u64 title_id = ParseHex(save.title_id);
        const int newest = save.versions.empty() ? 0 : save.versions.front().number;
        const auto local = Nextendo::SaveSync::Compare(SaveDirectory(title_id), newest);
        json versions = json::array();
        for (const auto& v : save.versions) {
            versions.push_back({{"id", v.id}, {"number", v.number}, {"conflict", v.conflict},
                                {"size", v.size}, {"device", v.device}, {"saved_at", v.saved_at}});
        }
        titles.push_back({{"title_id", Hex(title_id)},
                          {"name", GameName(title_id, save.name)},
                          {"newest", newest},
                          {"local", LocalStateName(local.state)},
                          {"local_written", local.last_written},
                          {"versions", versions}});
    }
    return {{"ok", saves.ok}, {"error", saves.error}, {"titles", titles},
            {"used", saves.allowance_used}, {"allowance", saves.allowance},
            {"running", RunningTitleId()}};
}

std::string CloudSaveAction(const json& args) {
    const std::string action = args.value("action", std::string{});
    const u64 title_id = ParseHex(args.value("title_id", std::string{}));
    if (action == "delete") {
        return Api::DeleteSaveVersion(args.value("version_id", static_cast<s64>(0)));
    }
    // Writing into a save folder a running game has mounted is what breaks it.
    if (g_running_title.load() != 0) {
        return "Stop the running game first.";
    }
    if (action == "download") {
        return Nextendo::SaveSync::Download(SaveDirectory(title_id), title_id);
    }
    if (action == "upload") {
        return Nextendo::SaveSync::Upload(SaveDirectory(title_id), title_id,
                                          args.value("newest", 0));
    }
    return "Unknown action.";
}

json Mods(const std::string& title) {
    const u64 title_id = ParseHex(title);
    const auto favourites = Api::GetFavouriteModIds();
    json out = json::array();
    for (const auto& mod : Api::GetMods(title)) {
        std::error_code ec;
        out.push_back({{"id", mod.id},
                       {"name", mod.name},
                       {"version", mod.version},
                       {"author", mod.author},
                       {"summary", mod.summary},
                       {"installed",
                        std::filesystem::is_directory(ModDirectory(title_id) / ModDirName(mod), ec)},
                       {"favourite", std::find(favourites.begin(), favourites.end(), mod.id) !=
                                         favourites.end()}});
    }
    return out;
}

std::string ModAction(const json& args) {
    const std::string title = args.value("title_id", std::string{});
    const std::string id = args.value("mod_id", std::string{});
    const std::string action = args.value("action", std::string{});
    if (action == "favourite" || action == "unfavourite") {
        return Api::SetModFavourite(id, action == "favourite") ? std::string{}
                                                               : "The favourite could not be saved.";
    }
    for (const auto& mod : Api::GetMods(title)) {
        if (mod.id != id) {
            continue;
        }
        const auto dir = ModDirectory(ParseHex(title)) / ModDirName(mod);
        std::error_code ec;
        if (action == "uninstall") {
            std::filesystem::remove_all(dir, ec);
            return ec ? ec.message() : std::string{};
        }
        // Verified against the catalogue's sha256, then unpacked in place of any older version:
        // files the new one dropped would otherwise keep being applied.
        const auto package = Api::DownloadModPackage(mod);
        if (!package) {
            return "The mod could not be downloaded, or it did not match what the catalogue published.";
        }
        std::filesystem::remove_all(dir, ec);
        if (!openpak::ZipStore::UnzipToDirectory(*package, dir)) {
            std::filesystem::remove_all(dir, ec);
            return "The mod's package could not be unpacked.";
        }
        return {};
    }
    return "That mod is not in the catalogue any more.";
}

json News(const std::string& title) {
    const Api::NewsManifest manifest = Api::GetNewsManifest(title);
    json files = json::array();
    for (const auto& file : manifest.files) {
        files.push_back({{"path", file.path}, {"size", file.size}});
    }
    return {{"ok", manifest.ok}, {"valid_from", manifest.valid_from}, {"files", files}};
}

json NetworkStatus() {
    const Api::ServiceStatus service = Api::GetServiceStatus();
    const Api::NetworkStatus network = Api::GetStatus();
    json services = json::array();
    for (const auto& check : service.services) {
        services.push_back({{"group", check.group}, {"name", check.name}, {"up", check.up},
                            {"uptime", check.uptime}, {"latency", check.latency}});
    }
    json titles = json::array();
    for (const auto& [title, players] : network.titles) {
        titles.push_back({{"title_id", title}, {"name", GameName(ParseHex(title))},
                          {"players", players}});
    }
    return {{"ok", service.ok},
            {"url", service.url},
            {"headline", service.headline},
            {"sub", service.sub},
            {"state", service.state},
            {"services", services},
            {"network_ok", network.ok},
            {"players_online", network.players_online},
            {"titles", titles},
            {"online", Session::Beating()}};
}

json Catalogue() {
    json out = json::array();
    for (const auto& [id, title] : Api::Catalogue()) {
        out.push_back({{"title_id", id}, {"name", title.name}, {"status", title.status}});
    }
    return out;
}

/// Everything the Kotlin side asks, by name. Blocking: Kotlin calls it off the main thread.
json Call(const std::string& method, const json& args) {
    if (method == "status") {
        return Status();
    }
    if (method == "sign_in") {
        return SignIn(args);
    }
    if (method == "sign_out") {
        SignOut();
        return json::object();
    }
    if (method == "forget_profile") {
        ForgetProfile(args.value("uuid", std::string{}));
        return json::object();
    }
    if (method == "profile_changed") {
        ProfileChanged();
        return json::object();
    }
    if (method == "profiles") {
        return Profiles();
    }
    if (method == "profile_action") {
        return ProfileAction(args);
    }
    if (method == "poll") {
        // Every call: the inbox the heartbeat keeps warm. Every fourth (the poll runs every five
        // seconds, the desktop's friend timer every twenty): the website's friend list.
        if (g_online.load()) {
            PollInvitations();
            if (Poll().polls++ % 4 == 0) {
                PollFriends();
            }
        }
        std::lock_guard lock{Poll().mutex};
        json events = json::array();
        for (auto& event : Poll().events) {
            events.push_back(std::move(event));
        }
        Poll().events.clear();
        return events;
    }
    if (method == "friends") {
        return Friends();
    }
    if (method == "friend_action") {
        return {{"error", FriendAction(args)}};
    }
    if (method == "invitations") {
        Session::RefreshInvitations();
        return Invitations();
    }
    if (method == "invitation_action") {
        return {{"error", InvitationAction(args)}};
    }
    if (method == "cloud_saves") {
        return CloudSaves();
    }
    if (method == "cloud_save_action") {
        return {{"error", CloudSaveAction(args)}};
    }
    if (method == "mods") {
        return Mods(args.value("title_id", std::string{}));
    }
    if (method == "mod_action") {
        return {{"error", ModAction(args)}};
    }
    if (method == "news") {
        return News(args.value("title_id", std::string{}));
    }
    if (method == "network_status") {
        return NetworkStatus();
    }
    if (method == "catalogue") {
        return Catalogue();
    }
    if (method == "compatibility_refresh") {
        return {{"changed", openpak::compatibility::Refresh()}};
    }
    if (method == "friends_picked") {
        FriendsPicked(args.value("ids", json::array()));
        return json::object();
    }
    if (method == "cloud_sync") {
        g_cloud_sync = args.value("enabled", true);
        return json::object();
    }
    return {{"error", "unknown method " + method}};
}

} // namespace

void OpenPakSetRunningTitle(u64 program_id) {
    g_running_title.store(program_id, std::memory_order_relaxed);
}

void OpenPakPullSaveBeforeLaunch(u64 title_id) {
    if (title_id == 0 || !g_cloud_sync.load() || !Common::OpenPakAccount::IsLinked()) {
        return;
    }
    switch (Nextendo::SaveSync::PullBeforeLaunch(SaveDirectory(title_id), title_id)) {
    case Nextendo::SaveSync::PullOutcome::Pulled:
        Message(fmt::format("Cloud save for {} downloaded; the previous local copy was kept beside it.",
                            GameName(title_id)));
        break;
    case Nextendo::SaveSync::PullOutcome::BothExist:
        Message(fmt::format("{} has a save both here and in the cloud. Choose one on the OpenPak "
                            "Cloud saves page.",
                            GameName(title_id)));
        break;
    case Nextendo::SaveSync::PullOutcome::Nothing:
        break;
    }
}

void OpenPakPushSaveAfterExit(u64 title_id) {
    if (title_id == 0 || !g_cloud_sync.load() || !Common::OpenPakAccount::IsLinked()) {
        return;
    }
    const auto directory = SaveDirectory(title_id);
    auto zip = Nextendo::SaveSync::CaptureOnExit(directory, title_id);
    if (zip.empty()) {
        return;
    }
    std::thread{[directory, title_id, zip = std::move(zip)]() mutable {
        const std::string error = Nextendo::SaveSync::PushCaptured(directory, title_id, std::move(zip));
        Message(error.empty() ? fmt::format("Save for {} uploaded to OpenPak.", GameName(title_id))
                              : error);
    }}.detach();
}

extern "C" {

// Before anything else and without the network: where the files go, who is asking, and the
// MyPage picker. Kotlin calls this once, from Application.onCreate.
void OPENPAK_JNI(nativeInit)(JNIEnv* env, jobject self) {
    openpak::SetLogSink([](openpak::LogLevel, const std::string& message) {
        LOG_INFO(Frontend, "[openpak] {}", message);
    });

    const std::string_view hash{Common::g_scm_rev};
    const std::string version{Common::g_build_version};
    openpak::Platform::SetClient(
        OPENPAK_CLIENT_NAME,
        version.empty() ? std::string{hash.substr(0, 8)}
                        : fmt::format("{}+{}", version, hash.substr(0, 8)));
    Api::SetSaveDevice(fmt::format("{} on Android", OPENPAK_EMULATOR_NAME));
    SetDirectories();

    if (g_bridge == nullptr) {
        g_bridge = env->NewGlobalRef(self);
        g_pick_friends = env->GetMethodID(env->GetObjectClass(self), "onPickFriends",
                                          "(ILjava/lang/String;)V");
        openpak::my_page::SetFriendPicker(PickFriends);
    }
    ResetPollState();
}

// Online, so the account is visible to friends and hearing about invitations before anybody
// opens anything. The chain runs on the caller's thread: Kotlin calls this off the main thread.
jboolean OPENPAK_JNI(nativeStart)(JNIEnv*, jobject) {
    if (!Settings::values.enable_openpak.GetValue()) {
        return JNI_FALSE;
    }
    SetDirectories();
    Session::Configure(Settings::values.openpak_server_ip.GetValue(), 443, {});
    g_online = true;
    ResetPollState();

    if (!Session::Ensure()) {
        LOG_WARNING(Frontend, "[OpenPak] Could not sign in at launch");
        return JNI_FALSE;
    }
    Session::StartHeartbeat(RunningTitleId);
    Session::RefreshGuestFriends();
    LOG_INFO(Frontend, "[OpenPak] Signed in at launch");
    return JNI_TRUE;
}

// Say we are going, so friends see us leave now rather than when the presence lease lapses:
// three seconds at most, as the desktop build waits.
void OPENPAK_JNI(nativeGoOffline)(JNIEnv*, jobject) {
    if (!g_online.exchange(false)) {
        return;
    }
    auto said = std::make_shared<std::promise<void>>();
    auto done = said->get_future();
    std::thread{[said] {
        Session::GoOffline();
        said->set_value();
    }}.detach();
    done.wait_for(std::chrono::seconds(3));
}

// The game list's OpenPak pill: "live", "beta", "alpha", or empty for a title OpenPak does not
// serve. A map lookup; safe on the main thread.
jstring OPENPAK_JNI(nativeCompatibility)(JNIEnv* env, jobject, jstring jprogram_id) {
    const u64 title_id = ParseHex(Common::Android::GetJString(env, jprogram_id));
    const auto entry = openpak::compatibility::Find(title_id);
    return Text(env, entry ? openpak::compatibility::Name(entry->status) : "");
}

jstring OPENPAK_JNI(nativeCall)(JNIEnv* env, jobject, jstring jmethod, jstring jargs) {
    const std::string method = Common::Android::GetJString(env, jmethod);
    json args = json::parse(Common::Android::GetJString(env, jargs), nullptr, false);
    if (args.is_discarded() || !args.is_object()) {
        args = json::object();
    }
    json result;
    try {
        result = Call(method, args);
    } catch (const std::exception& e) {
        LOG_ERROR(Frontend, "[OpenPak] {} failed: {}", method, e.what());
        result = {{"error", e.what()}};
    }
    return Text(env, result.dump(-1, ' ', false, json::error_handler_t::replace));
}

} // extern "C"
