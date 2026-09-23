// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <utility>

#include <QByteArray>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QUrl>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "openpak/account.h"
#include "openpak/friends_cache.h"
#include "core/core.h"
#include "core/hle/service/friend/friend.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "openpak/compatible_titles.h"
#include "citron/nextendo_chat_client.h"
#include "citron/nextendo_controller.h"
#include "citron/nextendo_save_sync.h"

#ifdef ENABLE_WEB_SERVICE
#include "openpak/api.h"
#endif

NextendoController::NextendoController(Core::System& system_, QWidget* main_window_,
                                       QObject* parent)
    : QObject(parent), system(system_), main_window(main_window_) {
    friend_poll_timer.setInterval(20000);
    connect(&friend_poll_timer, &QTimer::timeout, this, &NextendoController::PollFriends);
    friend_poll_timer.start();

    // [Nextendo] Faster than the friend-list poll: an invitation is time-sensitive/interactive
    // (the sender is actively waiting on the Invite Friends screen), not a passive presence
    // refresh -- 5s keeps it responsive without hammering the account server.
    invitation_poll_timer.setInterval(5000);
    connect(&invitation_poll_timer, &QTimer::timeout, this, &NextendoController::PollInvitations);
    invitation_poll_timer.start();

    PollFriends();
    EnsureChatConnected(); // no-op if not already signed in

    // Covers the "already linked, emulator just relaunched" case -- ProfileManager's own
    // constructor tries this too, but only if it happens to run after this account was
    // linked, which isn't guaranteed to be true this session, and it never syncs the avatar.
    if (Common::OpenPakAccount::IsLinked()) {
        ApplyProfileName(Common::OpenPakAccount::GetUsername());
        SyncProfileAvatar();
    }
}

NextendoController::~NextendoController() = default;

bool NextendoController::IsLinked() const {
    return Common::OpenPakAccount::IsLinked();
}

// Prototype chat server, not the official Nextendo fleet -- this only exists on the
// developer's own test VPS while the feature is being proven out and pitched. No
// OPENPAK_API-style restriction is needed here (unlike the account API, this carries
// no account token, only a bare PID + display name), but an override is still honoured
// so this can point elsewhere without a rebuild.
void NextendoController::EnsureChatConnected() {
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
                        emit ChatInviteReceived(obj.value(QStringLiteral("room_id")).toString(),
                                                obj.value(QStringLiteral("room_name")).toString(),
                                                static_cast<u64>(
                                                    obj.value(QStringLiteral("from_pid")).toDouble()),
                                                obj.value(QStringLiteral("from_name")).toString());
                    } else if (type == QStringLiteral("invite_sent")) {
                        emit ChatInviteSent(static_cast<u64>(
                            obj.value(QStringLiteral("target_pid")).toDouble()));
                    } else if (type == QStringLiteral("member_joined")) {
                        emit ChatMemberJoined(pending_chat_room_id,
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
    QString host = QString::fromUtf8(host_env);
    quint16 port = 8600;
    if (const char* env = std::getenv("OPENPAK_CHAT_PORT"); env && *env) {
        port = static_cast<quint16>(std::atoi(env));
    }
    chat_client->Connect(host, port);
}

QString NextendoController::ResolveGameName(const std::string& app_id_hex,
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
    return tr("a game");
}

QString NextendoController::ResolveGameIcon(const std::string& app_id_hex) const {
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
    return QString::fromLatin1(QByteArray::fromRawData(
        reinterpret_cast<const char*>(icon_bytes.data()), static_cast<int>(icon_bytes.size()))
                                    .toBase64());
}

std::string NextendoController::GetLocalAppId() const {
    if (!system.IsPoweredOn()) {
        return {};
    }
    return fmt::format("{:016X}", system.GetApplicationProcessProgramID());
}

void NextendoController::SignIn() {
#ifdef ENABLE_WEB_SERVICE
    // Email and password, asked here on the UI thread; the sign-in itself runs off it. The
    // password goes to openpak.org over public TLS and nowhere else: what comes back is a
    // website token and the Switch identity the games see.
    bool ok = false;
    const QString email = QInputDialog::getText(main_window, tr("Sign in to OpenPak"),
                                                tr("Email"), QLineEdit::Normal, QString(), &ok);
    if (!ok || email.trimmed().isEmpty()) {
        return;
    }
    const QString password = QInputDialog::getText(main_window, tr("Sign in to OpenPak"),
                                                   tr("Password"), QLineEdit::Password, QString(), &ok);
    if (!ok || password.isEmpty()) {
        return;
    }
    emit StatusChanged(tr("Signing in to OpenPak..."));

    QPointer<NextendoController> self(this);
    std::thread{[this, self, email = email.trimmed().toStdString(), password = password.toStdString()] {
        auto login_result = WebService::OpenPakApi::SignIn(email, password);

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, self, result = std::move(login_result)] {
                if (!self) {
                    return;
                }
                if (!result.ok) {
                    emit StatusChanged(QString::fromStdString(result.error));
                    emit SignInFinished();
                    return;
                }

                Common::OpenPakAccount::Save(result.pid, result.username, result.friend_code,
                                             result.token, result.bearer);
                ApplyProfileName(result.username);
                SyncProfileAvatar();
                Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnline);
                first_poll = true;
                emit AccountLinked();
                emit SignInFinished();
                RefreshFriendCache();
                EnsureChatConnected();
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    emit StatusChanged(tr("This build has no web services support."));
#endif
}

void NextendoController::SignOut() {
    Common::OpenPakAccount::Clear();
    Common::NextendoFriends::Set({});
    last_known_status.clear();
    offline_streak.clear();
    if (chat_client) {
        chat_client->Disconnect();
    }
    emit AccountUnlinked();
}

void NextendoController::ManualSaveDownload(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    // Belt-and-suspenders: the dialog already hides this action while any game is running,
    // but writing into the save directory while the emulated filesystem layer is mounted by
    // a live session is what actually crashes it, so refuse here regardless of caller.
    if (system.IsPoweredOn()) {
        emit StatusChanged(tr("Stop the running game before downloading a cloud save."));
        return;
    }
    if (!Nextendo::CompatibleTitles::Table().count(title_id)) {
        emit StatusChanged(tr("This game doesn't support cloud saves."));
        return;
    }

    emit StatusChanged(tr("Downloading save from the cloud..."));
    QPointer<NextendoController> self(this);
    std::thread{[this, self, title_id] {
        Nextendo::SaveSync::Pull(system, title_id, /*force=*/true);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            this, [this, self] {
                if (!self) {
                    return;
                }
                emit StatusChanged(tr("Cloud save applied."));
            }, Qt::QueuedConnection);
    }}.detach();
#else
    emit StatusChanged(tr("This build has no web services support."));
#endif
}

void NextendoController::QuickStart(u64 title_id) {
    emit QuickStartRequested(title_id);
}

void NextendoController::ApplyProfileName(const std::string& name) {
    if (name.empty()) {
        return;
    }

    // This used to construct its own throwaway Service::Account::ProfileManager here, which
    // re-parses the save file into a brand new object -- separate from system.GetProfileManager(),
    // the one live instance every running game and the Profile Manager config page actually read
    // from. Writes landed on disk but never reached the in-memory copy anything else sees, so the
    // rename appeared to silently do nothing (this was the unresolved half of the earlier Balloon
    // World self-profile investigation).
    auto& profile_manager = system.GetProfileManager();
    const auto uuid = profile_manager.GetLastOpenedUser();
    if (uuid.IsInvalid()) {
        return;
    }

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

void NextendoController::SyncProfileAvatar() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }
    auto& profile_manager = system.GetProfileManager();
    const auto uuid = profile_manager.GetLastOpenedUser();
    if (uuid.IsInvalid()) {
        return;
    }
    const u64 pid = Common::OpenPakAccount::GetPid();
    std::thread{[this, uuid, pid, guard = QPointer<NextendoController>(this)] {
        const std::string b64 = WebService::OpenPakApi::GetAvatarByPid(pid);
        if (b64.empty()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, guard, uuid, b64] {
                if (!guard) {
                    return;
                }
                WriteProfileAvatar(uuid, b64);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoController::WriteProfileAvatar(const Common::UUID& uuid, const std::string& avatar_b64) {
    QImage image;
    if (!image.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(avatar_b64)))) {
        return;
    }
    if (image.width() != 256 || image.height() != 256) {
        image = image.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }

    const auto image_path = QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir) /
        fmt::format("system/save/8000000000000010/su/avators/{}.jpg", uuid.FormattedString())));

    QDir{}.mkpath(QFileInfo(image_path).absolutePath());
    if (image.save(image_path, "JPEG")) {
        LOG_INFO(Frontend, "[OpenPak] Synced the active profile's avatar from the account");
    }
}

void NextendoController::RefreshFriendCache() {
    PollFriends();
}

void NextendoController::NotifyFriendRequestSent(const QString& friend_code) {
    emit FriendRequestSent(friend_code);
}

QString NextendoController::JoinFriendSession(u64 pid) {
    const auto entries = Common::NextendoFriends::Get();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                  [pid](const auto& e) { return e.pid == pid; });
    if (it == entries.end() || it->status != Common::NextendoFriends::PresenceOnlinePlay ||
        it->app_field.empty()) {
        const QString message = tr("That friend isn't in a joinable game right now.");
        emit StatusChanged(message);
        return message;
    }

    // Bypasses the native Invite Friends applet entirely: we already have this friend's
    // published session blob locally (from the same presence feed the friends list itself
    // renders), so there's no need to round-trip through SendFriendInvitation/the account
    // server's mailbox at all -- just hand it straight to the same local queue
    // TryPopFromFriendInvitationStorageChannel already reads from.
    Common::NextendoFriends::SetPendingInvitations({Common::NextendoFriends::PendingInvitation{
        it->pid, it->name, std::vector<u8>(it->app_field.begin(), it->app_field.end())}});

    const QString message = tr("Ready to join %1's game -- start or resume the title now.")
                                .arg(QString::fromStdString(it->name));
    emit StatusChanged(message);
    return message;
}

void NextendoController::PollFriends() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }

    QPointer<NextendoController> self(this);
    std::thread{[this, self] {
        auto fetched = WebService::OpenPakApi::GetFriends();
        if (!fetched.ok) {
            return;
        }
        if (!self) {
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
                    cache.push_back(
                        {entry.pid, entry.name, entry.presence_status, entry.app_field,
                         std::vector<u8>(decoded_image.begin(), decoded_image.end())});
                }
                Common::NextendoFriends::Set(std::move(cache));
                // [Nextendo] The guest's own INotificationService only ever signals once, at
                // construction -- before this first real poll has a chance to land. Without this,
                // a Friends viewer already on-screen never learns that real data showed up.
                Service::Friend::NotifyFriendsListUpdated();

                const bool suppress_toasts = first_poll;
                first_poll = false;

                std::map<u64, s32> current_status;
                for (const auto& entry : list.friends) {
                    const auto it = last_known_status.find(entry.pid);
                    const bool was_online = it != last_known_status.end() && it->second != 0;

                    // A status of 0 on a single poll can be a mid-transition blip on the
                    // server (e.g. switching presence when joining a match together) rather
                    // than a real disconnect. Require it to repeat on the next poll (~20s)
                    // before treating the friend as actually offline, so a one-poll blip can't
                    // fire a false "went offline" or the false "came online" right after it.
                    if (entry.presence_status == 0 && was_online) {
                        if (++offline_streak[entry.pid] < 2) {
                            current_status[entry.pid] = it->second;
                            continue;
                        }
                        if (!suppress_toasts) {
                            emit FriendWentOffline(entry.pid, QString::fromStdString(entry.name),
                                                   QString::fromStdString(entry.image_base64));
                        }
                    } else {
                        offline_streak.erase(entry.pid);
                        const bool was_offline = it == last_known_status.end() || it->second == 0;
                        if (!suppress_toasts && was_offline && entry.presence_status != 0) {
                            emit FriendCameOnline(entry.pid, QString::fromStdString(entry.name),
                                                  ResolveGameName(entry.app_id, entry.app_name),
                                                  QString::fromStdString(entry.image_base64));
                        }
                    }
                    current_status[entry.pid] = entry.presence_status;
                }
                last_known_status = std::move(current_status);

                std::set<u64> current_requests;
                for (const auto& entry : list.requests) {
                    current_requests.insert(entry.pid);
                    if (!suppress_toasts && !last_known_requests.contains(entry.pid)) {
                        emit FriendRequestReceived(entry.pid, QString::fromStdString(entry.name),
                                                   QString::fromStdString(entry.image_base64));
                    }
                }
                last_known_requests = std::move(current_requests);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoController::PollInvitations() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::OpenPakAccount::IsLinked()) {
        LOG_INFO(Frontend, "[OpenPak] PollInvitations: skipped, not linked");
        return;
    }
    LOG_INFO(Frontend, "[OpenPak] PollInvitations: tick");

    QPointer<NextendoController> self(this);
    std::thread{[this, self] {
        auto fetched = WebService::OpenPakApi::PollInvitations();
        if (fetched.empty() || !self) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [self, list = std::move(fetched)] {
                if (!self) {
                    return;
                }
                std::vector<Common::NextendoFriends::PendingInvitation> cache;
                cache.reserve(list.size());
                for (const auto& inv : list) {
                    cache.push_back({inv.from_pid, inv.from_name, inv.app_param});
                }
                Common::NextendoFriends::SetPendingInvitations(std::move(cache));
                for (const auto& inv : list) {
                    emit self->FriendInvitationReceived(inv.from_pid,
                                                        QString::fromStdString(inv.from_name));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}
