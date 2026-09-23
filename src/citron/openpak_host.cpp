// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project, OpenPak contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QMenu>
#include <QPointer>
#include <QStandardItemModel>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "hid_core/hid_core.h"
#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/friends_cache.h"
#include "openpak/platform.h"
#include "openpak/session.h"
#include "openpak/qt/account_dialog.h"
#include "openpak/qt/chat_client.h"
#include "openpak/qt/crash_report_prompt.h"
#include "openpak/qt/host_kit.h"
#include "openpak/qt/prompts.h"
#include "openpak/qt/save_sync.h"
#include "openpak/qt/sign_in_dialog.h"
#include "openpak/qt/strings.h"
#include "openpak/qt/toast.h"
#include "citron/game_list.h"
#include "citron/game_list_p.h"
#include "citron/openpak_host.h"
#include "citron/uisettings.h"
#include "citron/util/controller_navigation.h"

using openpak::qt::Tr;
using Kind = NextendoToast::Kind;
namespace Api = WebService::OpenPakApi;

namespace {

// The emulator's name where OpenPak shows it: the device name, the cloud saves' "from".
constexpr auto kEmulator = "Citron";

// Citron's gamepad navigation, the HID core's player-one controller, handed to the shared dialogs
// through the library's Navigation interface. The dialogs are the library's; the controller that
// drives them is Citron's, and this is the only place the two meet.
class OpenPakNavigation final : public openpak::qt::Navigation {
public:
    OpenPakNavigation(Core::HID::HIDCore& hid_core, QWidget* owner) : Navigation(owner) {
        auto* source = new ControllerNavigation(hid_core, owner);
        connect(source, &ControllerNavigation::navigated, this, &Navigation::navigated);
        connect(source, &ControllerNavigation::activated, this, &Navigation::activated);
        connect(source, &ControllerNavigation::cancelled, this, &Navigation::cancelled);
        connect(source, &ControllerNavigation::backPressed, this, &Navigation::backPressed);
        connect(source, &ControllerNavigation::leftShoulderPressed, this,
                &Navigation::leftShoulderPressed);
        connect(source, &ControllerNavigation::rightShoulderPressed, this,
                &Navigation::rightShoulderPressed);
        connect(source, &ControllerNavigation::auxiliaryAction, this,
                &Navigation::auxiliaryAction);
        connect(source, &ControllerNavigation::activityDetected, this,
                &Navigation::activityDetected);
    }
};

QPixmap FromBase64(const std::string& base64) {
    QPixmap picture;
    if (!base64.empty()) {
        picture.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(base64)));
    }
    return picture;
}

QPixmap FromBytes(const std::vector<u8>& bytes) {
    QPixmap picture;
    if (!bytes.empty()) {
        picture.loadFromData(bytes.data(), static_cast<uint>(bytes.size()));
    }
    return picture;
}

} // namespace

OpenPakHost::OpenPakHost(Core::System& system_, QWidget* main_window_, QObject* parent)
    : openpak::qt::Host(parent), system(system_), main_window(main_window_),
      toast(new NextendoToast(main_window_)) {
    friend_poll_timer.setInterval(20000);
    connect(&friend_poll_timer, &QTimer::timeout, this, &OpenPakHost::PollFriends);
    friend_poll_timer.start();

    // Faster than the friend-list poll: an invitation is time-sensitive (the sender is waiting
    // on the other side), not a passive presence refresh. This only reads what the heartbeat
    // last fetched from the native inbox, so it costs nothing on the network.
    invitation_poll_timer.setInterval(5000);
    connect(&invitation_poll_timer, &QTimer::timeout, this, &OpenPakHost::PollInvitations);
    invitation_poll_timer.start();

    active_profile = openpak::Platform::ProfileId();

    // Which machine a cloud save version came from, as the Cloud saves page lists it; the
    // sign-in dialog's device name replaces it once somebody signs in.
    Api::SetSaveDevice(
        openpak::qt::DefaultDeviceName(QString::fromLatin1(kEmulator)).toStdString());
}

OpenPakHost::~OpenPakHost() = default;

void OpenPakHost::Notify(int kind, const QString& text, const QPixmap& picture, int page,
                         std::function<void()> on_click) {
    if (page >= 0 && !on_click) {
        on_click = [this, page] { OpenWindow(page); };
    }
    if (static_cast<Kind>(kind) == Kind::GameInvite && on_click) {
        on_click = [this, click = std::move(on_click)] {
            if (!invitation_clicked || !invitation_clicked()) {
                click();
            }
        };
    }
    toast->Show(static_cast<Kind>(kind), text, picture, std::move(on_click));
}

void OpenPakHost::GoOnline() {
    // Sign in now rather than when a game first asks. Being online is the point of the
    // integration: until this runs the account is offline, invisible to friends, and hears about
    // no invitation. The chain is walked off the UI thread because a server that is slow to
    // answer must not be a window that is slow to open.
    QPointer<OpenPakHost> self(this);
    const QString profile = ProfileName();
    std::thread{[this, self, profile] {
        // Configure before signing in, or Enabled() is false and Ensure() returns without a
        // word: the account service configures it too, but not until a game first asks, and the
        // whole point here is to be online before that.
        if (Settings::values.enable_openpak.GetValue()) {
            openpak::client::session::Configure(Settings::values.openpak_server_ip.GetValue(), 443,
                                                {});
        }

        // A stored sign-in the website no longer takes starts the profile offline, with a toast
        // rather than a prompt (UX spec §5.1).
        if (Common::OpenPakAccount::HasBearer()) {
            const auto profile_answer = Api::GetProfile();
            if (!profile_answer.ok && profile_answer.error == "Your session expired. Sign in again.") {
                QMetaObject::invokeMethod(
                    this,
                    [this, self, profile] {
                        if (self) {
                            Notify(static_cast<int>(Kind::Account),
                                   Tr("The OpenPak sign-in for %1 has expired. Sign in again from "
                                      "the OpenPak menu.")
                                       .arg(profile),
                                   {}, -1, [this] { SignIn(); });
                        }
                    },
                    Qt::QueuedConnection);
            }
        }

        if (!openpak::client::session::Ensure()) {
            return;
        }

        // Presence says what is being played. The host reads it from the system it owns, on its
        // own thread, and only while a game is actually loaded.
        openpak::client::session::StartHeartbeat([this] {
            if (!system.IsPoweredOn()) {
                return std::string{};
            }
            const u64 program_id = system.GetApplicationProcessProgramID();
            return program_id == 0 ? std::string{} : fmt::format("{:016x}", program_id);
        });
    }}.detach();

    PollFriends();
    EnsureChatConnected(); // no-op if not already signed in
}

std::optional<Common::UUID> OpenPakHost::CurrentUser() const {
    return system.GetProfileManager().GetUser(
        static_cast<std::size_t>(Settings::values.current_user.GetValue()));
}

QString OpenPakHost::ProfileName(const std::string& key) const {
    const auto& profile_manager = system.GetProfileManager();
    for (const auto& uuid : profile_manager.GetAllUsers()) {
        Service::Account::ProfileBase profile{};
        if (uuid.IsValid() && uuid.RawString() == key &&
            profile_manager.GetProfileBase(uuid, profile)) {
            return QString::fromStdString(Common::StringFromFixedZeroTerminatedBuffer(
                std::string_view{reinterpret_cast<const char*>(profile.username.data()),
                                 profile.username.size()},
                profile.username.size()));
        }
    }
    return QString::fromStdString(key);
}

QString OpenPakHost::ProfileName() const {
    const auto current = CurrentUser();
    return current ? ProfileName(current->RawString()) : QString{};
}

std::filesystem::path OpenPakHost::AvatarPath(const Common::UUID& uuid) const {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir) /
           fmt::format("system/save/8000000000000010/su/avators/{}.jpg", uuid.FormattedString());
}

void OpenPakHost::SelectUser(const Common::UUID& uuid) {
    if (const auto index = system.GetProfileManager().GetUserIndex(uuid)) {
        Settings::values.current_user = static_cast<s32>(*index);
    }
    ProfileMaybeChanged();
}

void OpenPakHost::ProfileMaybeChanged() {
    const std::string profile = openpak::Platform::ProfileId();
    if (profile == active_profile) {
        return;
    }
    active_profile = profile;

    // The account service reads the last opened user; it is the current one from here on.
    if (const auto uuid = CurrentUser()) {
        system.GetProfileManager().OpenUser(*uuid);
    }

    // Nothing shown for the last profile's account may be shown as this one's.
    Common::NextendoFriends::Set({});
    last_known_status.clear();
    last_known_game.clear();
    offline_streak.clear();
    last_known_requests.clear();
    first_poll = true;
    if (chat_client) {
        chat_client->Disconnect();
    }

    if (IsLinked()) {
        emit AccountLinked();
    } else {
        emit AccountUnlinked();
    }

    // The session notices the switch by itself -- the old account goes offline, the new one's
    // device account signs in -- on whichever of this and the heartbeat asks first.
    if (Settings::values.enable_openpak.GetValue()) {
        std::thread{[] { openpak::client::session::Ensure(); }}.detach();
    }

    PollFriends();
    EnsureChatConnected();
}

void OpenPakHost::RunStartup(bool interactive) {
    if (interactive && Settings::values.enable_openpak.GetValue()) {
        PickStartupProfile();

        // Set up once per install, and never over a game that is starting: sign in, create an
        // account, or play offline -- after which the profile is the person's own either way.
        if (!IsLinked() && !UISettings::values.openpak_setup_offered.GetValue()) {
            UISettings::values.openpak_setup_offered = true;
            RunSetup(false);
        }

        // A crash last time left a report; it goes nowhere unless somebody says so.
        openpak::qt::OfferCrashReports(main_window);
    }

    GoOnline();
}

void OpenPakHost::PickStartupProfile() {
    auto& profile_manager = system.GetProfileManager();
    if (profile_manager.GetUserCount() < 2) {
        return;
    }

    const QString startup = QString::fromStdString(UISettings::values.openpak_startup_profile.GetValue());
    if (startup.isEmpty()) {
        return; // the last used, which current_user already is
    }
    if (startup != QStringLiteral("ask")) {
        for (const auto& uuid : profile_manager.GetAllUsers()) {
            if (uuid.IsValid() && QString::fromStdString(uuid.RawString()) == startup) {
                SelectUser(uuid);
            }
        }
        return;
    }

    std::vector<openpak::qt::ProfileRow> rows;
    for (const auto& uuid : profile_manager.GetAllUsers()) {
        if (uuid.IsInvalid()) {
            continue;
        }
        const std::string key = uuid.RawString();
        rows.push_back({QString::fromStdString(key), ProfileName(key),
                        QString::fromStdString(Common::OpenPakAccount::UsernameOf(key)),
                        QPixmap(QString::fromStdString(Common::FS::PathToUTF8String(AvatarPath(uuid))))});
    }
    const auto current = CurrentUser();
    const auto choice = openpak::qt::PickProfile(
        main_window, rows, current ? QString::fromStdString(current->RawString()) : QString{});
    switch (choice.action) {
    case openpak::qt::ProfileChoice::Action::Add:
        RunSetup(true);
        return;
    case openpak::qt::ProfileChoice::Action::Cancel:
        return; // closing it keeps the last used
    case openpak::qt::ProfileChoice::Action::Continue:
        break;
    }
    if (choice.remember) {
        UISettings::values.openpak_startup_profile = choice.key.toStdString();
    }
    for (const auto& uuid : profile_manager.GetAllUsers()) {
        if (uuid.IsValid() && QString::fromStdString(uuid.RawString()) == choice.key) {
            SelectUser(uuid);
        }
    }
}

void OpenPakHost::RunSetup(bool add_account) {
    const openpak::qt::SetupChoice choice = openpak::qt::AskSetup(main_window, add_account);
    if (choice == openpak::qt::SetupChoice::Cancel) {
        return;
    }

    auto& profile_manager = system.GetProfileManager();
    if (choice == openpak::qt::SetupChoice::Offline) {
        const auto current = CurrentUser();
        const QString suggested =
            add_account || !current ? QString{} : ProfileName(current->RawString());
        const QString name = openpak::qt::AskProfileName(main_window, suggested);
        if (name.isEmpty()) {
            RunSetup(add_account); // an empty name or Cancel goes back to the setup
            return;
        }
        if (add_account) {
            const auto uuid = Common::UUID::MakeRandom();
            profile_manager.CreateNewUser(uuid, name.toStdString());
            profile_manager.WriteUserSaveFile();
            SelectUser(uuid);
        } else {
            ApplyProfileName(name.toStdString());
        }
        return;
    }

    // The account is kept under the current profile, so a new one is made current before the
    // sign-in and taken away again if nobody signs in.
    const auto previous = CurrentUser();
    std::optional<Common::UUID> created;
    if (add_account) {
        created = Common::UUID::MakeRandom();
        profile_manager.CreateNewUser(*created, "OpenPak");
        profile_manager.WriteUserSaveFile();
        SelectUser(*created);
    }

    // Create an account opened the website; the sign-in follows with what to do first.
    const QString intro =
        choice == openpak::qt::SetupChoice::Create
            ? Tr("Finish creating your account in the browser and verify your email, then sign "
                 "in here.")
            : QString{};

    AskAndSignIn(true, intro, [this, add_account, created, previous](bool signed_in) {
        if (signed_in) {
            return;
        }
        if (created) {
            system.GetProfileManager().RemoveUser(*created);
            system.GetProfileManager().WriteUserSaveFile();
            if (previous) {
                SelectUser(*previous);
            }
        }
        RunSetup(add_account);
    });
}

bool OpenPakHost::IsLinked() const {
    return Common::OpenPakAccount::IsLinked();
}

void OpenPakHost::EnsureChatConnected() {
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }
    if (chat_client && chat_client->IsConnected()) {
        return;
    }
    if (!chat_client) {
        chat_client = new NextendoChatClient(this);
        connect(chat_client, &NextendoChatClient::MessageReceived, this,
                [this](const QJsonObject& obj) {
                    const QString type = obj.value(QStringLiteral("type")).toString();
                    if (type == QStringLiteral("invite_received")) {
                        emit ChatInviteReceived(
                            obj.value(QStringLiteral("room_id")).toString(),
                            obj.value(QStringLiteral("room_name")).toString(),
                            static_cast<u64>(obj.value(QStringLiteral("from_pid")).toDouble()),
                            obj.value(QStringLiteral("from_name")).toString());
                    } else if (type == QStringLiteral("invite_sent")) {
                        emit ChatInviteSent(
                            static_cast<u64>(obj.value(QStringLiteral("target_pid")).toDouble()));
                    } else if (type == QStringLiteral("member_joined")) {
                        emit ChatMemberJoined(
                            pending_chat_room_id,
                            static_cast<u64>(obj.value(QStringLiteral("pid")).toDouble()),
                            obj.value(QStringLiteral("name")).toString());
                    } else if (type == QStringLiteral("room_joined")) {
                        pending_chat_room_id = obj.value(QStringLiteral("room_id")).toString();
                    } else if (type == QStringLiteral("banned")) {
                        emit ChatBanned(obj.value(QStringLiteral("reason")).toString());
                    }
                    emit ChatRawMessage(obj);
                });
        connect(chat_client, &NextendoChatClient::Connected, this, [this] {
            chat_client->SendJson(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("identify")},
                {QStringLiteral("pid"), static_cast<qint64>(Common::OpenPakAccount::GetPid())},
                {QStringLiteral("name"),
                 QString::fromStdString(Common::OpenPakAccount::GetUsername())},
            });
        });
    }

    // OpenPak runs no chat server. The client stays for whoever points it at one.
    const char* host_env = std::getenv("OPENPAK_CHAT_HOST");
    if (!host_env || !*host_env) {
        return;
    }
    const QString host = QString::fromUtf8(host_env);
    quint16 port = 8600;
    if (const char* env = std::getenv("OPENPAK_CHAT_PORT"); env && *env) {
        port = static_cast<quint16>(std::atoi(env));
    }
    chat_client->Connect(host, port);
}

QString OpenPakHost::ResolveGameName(const std::string& app_id_hex,
                                     const std::string& hint_name) const {
    if (app_id_hex.empty()) {
        return {};
    }
    u64 program_id = 0;
    try {
        program_id = std::stoull(app_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return {};
    }
    if (program_id == 0) {
        return {};
    }

    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto [nacp, icon] = pm.GetControlMetadata();
    if (nacp) {
        const auto name = nacp->GetApplicationName();
        if (!name.empty()) {
            return QString::fromStdString(name);
        }
    }
    if (!hint_name.empty()) {
        return QString::fromStdString(hint_name);
    }
    return Tr("Unknown game");
}

QString OpenPakHost::ResolveGameIcon(const std::string& app_id_hex) const {
    if (app_id_hex.empty()) {
        return {};
    }
    u64 program_id = 0;
    try {
        program_id = std::stoull(app_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return {};
    }
    if (program_id == 0) {
        return {};
    }

    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto [nacp, icon_file] = pm.GetControlMetadata();
    if (!icon_file) {
        return {};
    }
    const std::vector<u8> icon_bytes = icon_file->ReadAllBytes();
    if (icon_bytes.empty()) {
        return {};
    }
    return QString::fromLatin1(
        QByteArray::fromRawData(reinterpret_cast<const char*>(icon_bytes.data()),
                                static_cast<int>(icon_bytes.size()))
            .toBase64());
}

bool OpenPakHost::ChatEnabled() const {
    // Citron's chat rooms are an experiment outside the OpenPak spec, offered only where a chat
    // server has been named (OPENPAK_CHAT_HOST).
    const char* host_env = std::getenv("OPENPAK_CHAT_HOST");
    return host_env && *host_env;
}

QString OpenPakHost::ChatInviteText() const {
    return tr("Invite to Chat Room");
}

std::string OpenPakHost::GetLocalAppId() const {
    if (!system.IsPoweredOn()) {
        return {};
    }
    return fmt::format("{:016X}", system.GetApplicationProcessProgramID());
}

void OpenPakHost::SignIn() {
    AskAndSignIn(false, {});
}

void OpenPakHost::AskAndSignIn(bool adopt, const QString& intro, std::function<void(bool)> done) {
    // Email, password and device name, asked here; the sign-in runs off the UI thread with the
    // dialog's busy bar, and a failure stays in the dialog with the email kept (UX spec §3.3).
    // The password goes to OpenPak over TLS and nowhere else: what comes back is a website token
    // and the Switch identity the games see.
    struct Outcome {
        Api::LoginResult result;
        std::string link_failure;
        bool linked = false;
    };
    auto outcome = std::make_shared<Outcome>();
    const std::string profile = openpak::Platform::ProfileId();

    OpenPakSignInDialog dialog(main_window, intro);
    dialog.SetDeviceName(openpak::qt::DefaultDeviceName(QString::fromLatin1(kEmulator), ProfileName()));
    dialog.SetSubmitter([this, outcome, profile](QString email, QString password,
                                                 QString device) -> QString {
        if (!device.isEmpty()) {
            Api::SetDeviceName(device.toStdString());
            Api::SetSaveDevice(device.toStdString());
        }
        auto result = Api::SignIn(email.toStdString(), password.toStdString());
        if (!result.ok) {
            return openpak::qt::ErrorText(result.error);
        }
        // One account, one profile: two profiles on one account would share a cloud-save slot and
        // overwrite each other's progress. Checked before the console link, which would otherwise
        // bind this profile's device account to it first.
        const std::string holder = Common::OpenPakAccount::HolderOf(result.pid, profile);
        if (!holder.empty()) {
            return Tr("This OpenPak account is already linked to the profile \"%1\". Sign in "
                      "there, or sign that profile out first.")
                .arg(ProfileName(holder));
        }
        // Two sign-ins, because they are two different things: the website account is what
        // friends and cloud saves speak with, and the console chain is what puts an identity in
        // front of a title server. A link that fails leaves the website half standing, and the
        // Account page offers Try again.
        if (openpak::client::session::Enabled()) {
            outcome->link_failure =
                openpak::client::session::LinkWithPassword(email.toStdString(), password.toStdString());
            outcome->linked = outcome->link_failure.empty();
        }
        outcome->result = std::move(result);
        return {};
    });

    if (dialog.exec() != QDialog::Accepted || !outcome->result.ok) {
        if (done) {
            done(false);
        }
        return;
    }

    const auto& result = outcome->result;
    if (!outcome->link_failure.empty()) {
        LOG_WARNING(Frontend, "[OpenPak] Signed in, but the console link did not complete: {}",
                    outcome->link_failure);
    }
    Common::OpenPakAccount::Save(result.pid, result.username, result.friend_code, result.token,
                                 result.bearer);
    if (adopt) {
        ApplyProfileName(result.username);
        SyncProfileAvatar();
    }
    Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnline);
    first_poll = true;
    const QString name = QString::fromStdString(result.username);
    Notify(static_cast<int>(Kind::Account),
           outcome->linked
               ? Tr("Signed in as %1, and this console is now linked to your account.").arg(name)
               : Tr("Signed in as %1.").arg(name),
           {}, OpenPakAccountDialog::kAccountPage);
    emit AccountLinked();
    emit SignInFinished();
    RefreshFriendCache();
    EnsureChatConnected();
    if (done) {
        done(true);
    }
}

void OpenPakHost::SignOut() {
    // The website token is revoked on the server, not only forgotten here, as Ryujinx does. The
    // console link stays: signing out of the website does not unlink a Switch either.
    std::thread{[bearer = Common::OpenPakAccount::GetBearer()] { Api::RevokeToken(bearer); }}.detach();
    Common::OpenPakAccount::Clear();
    Common::NextendoFriends::Set({});
    last_known_status.clear();
    last_known_game.clear();
    offline_streak.clear();
    if (chat_client) {
        chat_client->Disconnect();
    }
    Notify(static_cast<int>(Kind::Account), Tr("Signed out of OpenPak."));
    emit AccountUnlinked();
}

void OpenPakHost::PopulateMenu(QMenu* menu, std::function<void()> open_settings_) {
    open_settings = std::move(open_settings_);
    openpak::qt::MenuHooks hooks;
    hooks.game_running = [this] { return system.IsPoweredOn(); };
    hooks.openpak_on = [] { return Settings::values.enable_openpak.GetValue(); };
    hooks.signed_in_as = [this] {
        return IsLinked() ? QString::fromStdString(Common::OpenPakAccount::GetUsername()) : QString{};
    };
    hooks.avatar = [this] {
        const auto current = CurrentUser();
        return current ? QPixmap(QString::fromStdString(Common::FS::PathToUTF8String(AvatarPath(*current))))
                       : QPixmap{};
    };
    hooks.sign_in = [this] { SignIn(); };
    hooks.sign_out = [this] { SignOut(); };
    hooks.open_window = [this](int page) { OpenWindow(page); };
    hooks.open_settings = [this] {
        if (open_settings) {
            open_settings();
        }
    };
    hooks.profile_name = [this] { return ProfileName(); };
    openpak::qt::PopulateOpenPakMenu(menu, std::move(hooks));
}

void OpenPakHost::OpenWindow(int page) {
    if (window) {
        window->GoToPage(page);
        window->raise();
        window->activateWindow();
        return;
    }
    OpenPakAccountDialog dialog(this, main_window, page);
    window = &dialog;
    if (decorate_window) {
        decorate_window(dialog);
    }
    dialog.exec();
    window = nullptr;
}

void OpenPakHost::ToggleWindow() {
    if (window) {
        window->close();
        return;
    }
    OpenWindow(OpenPakAccountDialog::LastPage());
}

openpak::qt::SettingsHooks OpenPakHost::MakeSettingsHooks() {
    openpak::qt::SettingsHooks hooks;
    hooks.enabled = [] { return Settings::values.enable_openpak.GetValue(); };
    hooks.set_enabled = [](bool on) { Settings::values.enable_openpak = on; };
    hooks.game_running = [this] { return system.IsPoweredOn(); };
    hooks.profile = [this] { return ProfileName(); };
    hooks.signed_in_as = [this] {
        return IsLinked() ? QString::fromStdString(Common::OpenPakAccount::GetUsername()) : QString{};
    };
    hooks.sign_in = [this] { SignIn(); };
    hooks.sign_out = [this] { SignOut(); };
    hooks.open_window = [this] { OpenWindow(OpenPakAccountDialog::kAccountPage); };
    hooks.profiles = [this] {
        std::vector<std::pair<QString, QString>> profiles;
        for (const auto& uuid : system.GetProfileManager().GetAllUsers()) {
            if (uuid.IsValid()) {
                profiles.emplace_back(QString::fromStdString(uuid.RawString()),
                                      ProfileName(uuid.RawString()));
            }
        }
        return profiles;
    };
    hooks.startup = [] {
        return QString::fromStdString(UISettings::values.openpak_startup_profile.GetValue());
    };
    hooks.set_startup = [](const QString& value) {
        UISettings::values.openpak_startup_profile = value.toStdString();
    };
    return hooks;
}

void OpenPakHost::ManualSaveDownload(u64 title_id) {
    // The Cloud saves page already disables this while a game runs, but writing into the save
    // directory while the emulated filesystem has it mounted is what breaks it, so it is refused
    // here whoever asks.
    if (system.IsPoweredOn()) {
        emit StatusChanged(Tr("Stop the running game first."));
        return;
    }
    QPointer<OpenPakHost> self(this);
    const QString game = ResolveGameName(fmt::format("{:016X}", title_id));
    std::thread{[this, self, title_id, game, directory = SaveDirectory(title_id)] {
        Nextendo::SaveSync::Pull(directory, title_id, /*force=*/true);
        QMetaObject::invokeMethod(
            this,
            [this, self, game] {
                if (self) {
                    emit StatusChanged(Tr("Downloaded the cloud save for %1.").arg(game));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
}

void OpenPakHost::PullSaveBeforeLaunch(u64 title_id) {
    if (title_id == 0 || !CloudSyncEnabled() || !IsLinked()) {
        return;
    }
    const QString game = ResolveGameName(fmt::format("{:016X}", title_id));
    switch (Nextendo::SaveSync::PullBeforeLaunch(SaveDirectory(title_id), title_id)) {
    case Nextendo::SaveSync::PullOutcome::Pulled:
    {
        const QString text =
            Tr("Cloud save for %1 downloaded; the previous local copy was kept beside it.").arg(game);
        Notify(static_cast<int>(Kind::Saves), text, {}, OpenPakAccountDialog::kCloudSavesPage);
        emit StatusChanged(text);
        break;
    }
    case Nextendo::SaveSync::PullOutcome::BothExist:
        // Never a blocking choice at launch (UX spec §3.9): the game starts on its local save,
        // and the toast leads to Resolve... on the Cloud saves page.
        Notify(static_cast<int>(Kind::Saves),
               Tr("%1 has a save here and a different one in the cloud. Choose one on the Cloud "
                  "saves page.")
                   .arg(game),
               {}, OpenPakAccountDialog::kCloudSavesPage);
        break;
    case Nextendo::SaveSync::PullOutcome::Nothing:
        break;
    }
}

void OpenPakHost::PushSaveAfterExit(u64 title_id) {
    if (title_id == 0 || !CloudSyncEnabled() || !IsLinked()) {
        return;
    }
    const auto directory = SaveDirectory(title_id);
    auto zip = Nextendo::SaveSync::CaptureOnExit(directory, title_id);
    if (zip.empty()) {
        return;
    }
    QPointer<OpenPakHost> self(this);
    const QString game = ResolveGameName(fmt::format("{:016X}", title_id));
    std::thread{[this, self, directory, title_id, game, captured = std::move(zip)]() mutable {
        const std::string error =
            Nextendo::SaveSync::PushCaptured(directory, title_id, std::move(captured));
        QMetaObject::invokeMethod(
            this,
            [this, self, error, game] {
                if (!self) {
                    return;
                }
                const QString text = error.empty()
                                         ? Tr("Save for %1 uploaded to OpenPak.").arg(game)
                                         : Tr("The save for %1 did not upload: %2")
                                               .arg(game, openpak::qt::ErrorText(error));
                Notify(static_cast<int>(Kind::Saves), text, {}, OpenPakAccountDialog::kCloudSavesPage);
                emit StatusChanged(text);
            },
            Qt::QueuedConnection);
    }}.detach();
}

void OpenPakHost::QuickStart(u64 title_id) {
    emit QuickStartRequested(title_id);
}

void OpenPakHost::ApplyProfileName(const std::string& name) {
    if (name.empty()) {
        return;
    }
    // The live profile manager, not a throwaway one parsed from disk: that is the instance every
    // running game and the profile manager page read, so a rename made anywhere else never shows.
    auto& profile_manager = system.GetProfileManager();
    const auto current = CurrentUser();
    if (!current || current->IsInvalid()) {
        return;
    }
    const auto uuid = *current;

    Service::Account::ProfileBase profile{};
    if (!profile_manager.GetProfileBase(uuid, profile)) {
        return;
    }

    const std::string trimmed = name.substr(0, profile.username.size() - 1);
    std::fill(profile.username.begin(), profile.username.end(), '\0');
    std::copy(trimmed.begin(), trimmed.end(), profile.username.begin());

    profile_manager.SetProfileBase(uuid, profile);
    profile_manager.WriteUserSaveFile();
    LOG_INFO(Frontend, "[OpenPak] Renamed the active profile to the account nickname");
}

void OpenPakHost::SyncProfileAvatar() {
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }
    const auto current = CurrentUser();
    if (!current || current->IsInvalid()) {
        return;
    }
    const auto uuid = *current;
    const u64 pid = Common::OpenPakAccount::GetPid();
    std::thread{[this, uuid, pid, guard = QPointer<OpenPakHost>(this)] {
        const std::string b64 = Api::GetAvatarByPid(pid);
        if (b64.empty()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, guard, uuid, b64] {
                if (guard) {
                    WriteProfileAvatar(uuid, b64);
                }
            },
            Qt::QueuedConnection);
    }}.detach();
}

void OpenPakHost::WriteProfileAvatar(const Common::UUID& uuid, const std::string& avatar_b64) {
    QImage image;
    if (!image.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(avatar_b64)))) {
        return;
    }
    if (image.width() != 256 || image.height() != 256) {
        image = image.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    const auto image_path = QString::fromStdString(Common::FS::PathToUTF8String(AvatarPath(uuid)));
    QDir{}.mkpath(QFileInfo(image_path).absolutePath());
    if (image.save(image_path, "JPEG")) {
        LOG_INFO(Frontend, "[OpenPak] Synced the active profile's avatar from the account");
    }
}

void OpenPakHost::RefreshFriendCache() {
    PollFriends();
}

void OpenPakHost::NotifyFriendRequestSent(const QString& friend_code) {
    // The window's status line says it (UX spec §3.10: request sent is not a toast).
    emit FriendRequestSent(friend_code);
}

QString OpenPakHost::JoinFriendSession(u64 pid) {
    const auto entries = Common::NextendoFriends::Get();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [pid](const auto& e) { return e.pid == pid; });
    if (it == entries.end() || it->status != Common::NextendoFriends::PresenceOnlinePlay ||
        it->app_field.empty()) {
        return Tr("That friend is not in a game you can join right now.");
    }

    // Bypasses the native Invite Friends applet entirely: this friend's published session blob
    // is already here (the same presence feed the list renders), so it goes straight into the
    // running game's invitation channel, as an accepted invitation would.
    const auto user = CurrentUser();
    if (!user || !system.GetAppletManager().PushFriendInvitation(
                     *user, std::vector<u8>(it->app_field.begin(), it->app_field.end()))) {
        return Tr("Start the game first, then join from here.");
    }
    return Tr("Joining %1's game.").arg(QString::fromStdString(it->name));
}

void OpenPakHost::PollFriends() {
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }

    QPointer<OpenPakHost> self(this);
    std::thread{[this, self] {
        auto fetched = Api::GetFriends();
        if (!fetched.ok || !self) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, self, list = std::move(fetched)] {
                if (!self) {
                    return;
                }
                std::vector<Common::NextendoFriends::Entry> cache;
                cache.reserve(list.friends.size());
                for (const auto& entry : list.friends) {
                    const auto decoded_image =
                        QByteArray::fromBase64(QByteArray::fromStdString(entry.image_base64));
                    cache.push_back({entry.pid, entry.name, entry.presence_status, entry.app_field,
                                     std::vector<u8>(decoded_image.begin(), decoded_image.end())});
                }
                Common::NextendoFriends::Set(std::move(cache));

                // The first answer after start or sign-in is silent (UX spec §3.10).
                const bool quiet = first_poll;
                first_poll = false;

                std::map<u64, s32> current_status;
                std::map<u64, std::string> current_game;
                for (const auto& entry : list.friends) {
                    const auto it = last_known_status.find(entry.pid);
                    const bool was_online = it != last_known_status.end() && it->second != 0;
                    const QString name = QString::fromStdString(entry.name);

                    // A status of 0 on a single poll can be a blip while presence changes (say,
                    // joining a match together) rather than a real disconnect: it has to repeat
                    // on the next poll before the friend counts as offline.
                    if (entry.presence_status == 0 && was_online) {
                        if (++offline_streak[entry.pid] < 2) {
                            current_status[entry.pid] = it->second;
                            current_game[entry.pid] = last_known_game[entry.pid];
                            continue;
                        }
                        emit FriendWentOffline(entry.pid, name, QString::fromStdString(entry.image_base64));
                    } else {
                        offline_streak.erase(entry.pid);
                        const QString game = entry.app_id.empty()
                                                 ? QString{}
                                                 : ResolveGameName(entry.app_id, entry.app_name);
                        const bool came_online = !was_online && entry.presence_status != 0;
                        const bool new_game = was_online && !entry.app_id.empty() &&
                                              last_known_game[entry.pid] != entry.app_id;
                        if (!quiet && came_online) {
                            Notify(static_cast<int>(Kind::Online),
                                   game.isEmpty() ? Tr("%1 is online").arg(name)
                                                  : Tr("%1 is playing %2").arg(name, game),
                                   FromBase64(entry.image_base64), OpenPakAccountDialog::kFriendsPage);
                            emit FriendCameOnline(entry.pid, name, game,
                                                  QString::fromStdString(entry.image_base64));
                        } else if (!quiet && new_game) {
                            Notify(static_cast<int>(Kind::Online), Tr("%1 is playing %2").arg(name, game),
                                   FromBase64(entry.image_base64), OpenPakAccountDialog::kFriendsPage);
                        }
                    }
                    current_status[entry.pid] = entry.presence_status;
                    current_game[entry.pid] = entry.presence_status != 0 ? entry.app_id : std::string{};
                }
                last_known_status = std::move(current_status);
                last_known_game = std::move(current_game);

                std::set<u64> current_requests;
                for (const auto& entry : list.requests) {
                    current_requests.insert(entry.pid);
                    if (!quiet && !last_known_requests.contains(entry.pid)) {
                        const QString name = QString::fromStdString(entry.name);
                        Notify(static_cast<int>(Kind::Request),
                               Tr("%1 wants to be your friend").arg(name),
                               FromBase64(entry.image_base64), OpenPakAccountDialog::kFriendsPage);
                        emit FriendRequestReceived(entry.pid, name,
                                                   QString::fromStdString(entry.image_base64));
                    }
                }
                last_known_requests = std::move(current_requests);
            },
            Qt::QueuedConnection);
    }}.detach();
}

void OpenPakHost::PollInvitations() {
    // What the heartbeat last read from the native inbox, offered as Ryujinx offers it: an
    // invitation to the running game asks Join or Ignore, and Join leaves the sender's data in the
    // game's invitation channel, where the game looks for it. One to another game is announced
    // once and offered when that game is running. Either answer marks it read.
    const std::string running = GetLocalAppId();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    for (const auto& invitation : openpak::client::session::Invitations()) {
        if (offered_invitations.contains(invitation.id) ||
            (invitation.expires_at != 0 && invitation.expires_at < now)) {
            continue;
        }

        const QString sender = QString::fromStdString(invitation.sender_name);
        const QString game = ResolveGameName(invitation.title_id);
        // The sender's picture, when they are a friend the cache has one for.
        QPixmap sender_picture;
        for (const auto& known : Common::NextendoFriends::Get()) {
            if (known.name == invitation.sender_name) {
                sender_picture = FromBytes(known.image);
                break;
            }
        }
        if (running.empty() || Common::ToLower(running) != Common::ToLower(invitation.title_id)) {
            if (announced_invitations.insert(invitation.id).second) {
                Notify(static_cast<int>(Kind::GameInvite), Tr("%1 invited you to %2").arg(sender, game),
                       sender_picture, OpenPakAccountDialog::kInvitationsPage);
                emit FriendInvitationReceived(0, sender);
            }
            continue;
        }

        offered_invitations.insert(invitation.id);
        openpak::qt::InvitationCard card;
        card.sender = sender;
        card.sender_picture = sender_picture;
        card.game = game;
        card.game_icon = FromBase64(ResolveGameIcon(invitation.title_id).toStdString());
        card.sent = invitation.created_at;
        card.message = openpak::qt::InvitationMessage(invitation.messages);
        if (openpak::qt::OfferInvitation(main_window, card)) {
            const auto user = CurrentUser();
            if (!user ||
                !system.GetAppletManager().PushFriendInvitation(*user, invitation.app_param)) {
                Notify(static_cast<int>(Kind::GameInvite),
                       Tr("The game closed before the invitation could be handed over."));
            }
        }

        std::thread{[id = invitation.id] {
            openpak::client::session::DismissInvitation(id);
        }}.detach();
    }
}

// ---- openpak::qt::Host: Citron-specific answers ----

std::vector<openpak::qt::Host::Title> OpenPakHost::InstalledTitles() const {
    // The game list already resolved every name; asking the content provider again would parse
    // a control NCA per title on the UI thread.
    std::vector<Title> out;
    const auto* game_list = main_window->findChild<GameList*>();
    const QStandardItemModel* model = game_list ? game_list->GetModel() : nullptr;
    for (int dir = 0; model && dir < model->rowCount(); ++dir) {
        const QStandardItem* folder = model->item(dir);
        for (int row = 0; row < folder->rowCount(); ++row) {
            const QStandardItem* game = folder->child(row);
            const u64 id = game->data(GameListItemPath::ProgramIdRole).toULongLong();
            const bool seen = std::any_of(out.begin(), out.end(),
                                          [id](const Title& title) { return title.id == id; });
            if (id != 0 && !seen) { // the favourites folder repeats rows
                out.push_back({id, game->data(GameListItemPath::TitleRole).toString()});
            }
        }
    }
    return out;
}

std::filesystem::path OpenPakHost::ModDirectory(u64 title_id) {
    // load/<TITLEID>/<mod>/romfs: where BISFactory::GetModificationLoadRoot reads, so an
    // installed mod shows up in the title's Properties -> Add-Ons like any other.
    return Common::FS::GetCitronPath(Common::FS::CitronPath::LoadDir) / fmt::format("{:016X}", title_id);
}

std::filesystem::path OpenPakHost::SaveDirectory(u64 title_id) {
    // nand/user/save/0000000000000000/<user>/<TITLEID>, the path SaveDataFactory::GetFullPath
    // builds. The active profile's own, because the cloud save belongs to the account that
    // profile is signed in as: another profile's folder would upload somebody else's progress.
    // A title with only a device save (ACNH) keeps it under the all-zero user, as on a console.
    const auto root = Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir) / "user" / "save" /
                      "0000000000000000";
    const std::string title = fmt::format("{:016X}", title_id);
    const auto current = CurrentUser();
    if (!current || current->IsInvalid()) {
        return {};
    }
    const auto id = current->AsU128();
    const auto own = root / fmt::format("{:016X}{:016X}", id[1], id[0]) / title;
    const auto device = root / std::string(32, '0') / title;

    std::error_code ec;
    if (!std::filesystem::is_directory(own, ec) && std::filesystem::is_directory(device, ec)) {
        return device;
    }
    return own;
}

QString OpenPakHost::AccentColor() const {
    return QStringLiteral("#2f56d0");
}

bool OpenPakHost::IsDarkTheme() const {
    return UISettings::IsDarkTheme();
}

// The notification and cloud-sync preferences are settings, under the same keys as Citron's
// (UX spec §3.13).
bool OpenPakHost::NotificationsEnabled() const {
    return UISettings::values.openpak_notifications_enabled.GetValue();
}

void OpenPakHost::SetNotificationsEnabled(bool enabled) {
    UISettings::values.openpak_notifications_enabled = enabled;
}

int OpenPakHost::NotificationCorner() const {
    return UISettings::values.openpak_notification_corner.GetValue();
}

void OpenPakHost::SetNotificationCorner(int corner) {
    UISettings::values.openpak_notification_corner = corner;
}

bool OpenPakHost::RedirectEnabled() const {
    return Settings::values.enable_openpak.GetValue();
}

bool OpenPakHost::CloudSyncEnabled() const {
    return Settings::values.openpak_cloud_sync_enabled.GetValue();
}

void OpenPakHost::SetCloudSyncEnabled(bool enabled) {
    Settings::values.openpak_cloud_sync_enabled = enabled;
}

std::string OpenPakHost::ServerIp() const {
    return Settings::values.openpak_server_ip.GetValue();
}

std::string OpenPakHost::NatIp() const {
    return Settings::values.openpak_nat_ip.GetValue();
}

void OpenPakHost::SetGuestInputSuspended(bool suspended) {
    // A dialog driven by the controller must not also drive the game behind it.
    system.HIDCore().SetGuestInputSuspended(suspended);
}

openpak::qt::Navigation* OpenPakHost::CreateNavigation(QObject* parent) {
    auto* owner = qobject_cast<QWidget*>(parent);
    if (!owner) {
        return nullptr;
    }
    return new OpenPakNavigation(system.HIDCore(), owner);
}
