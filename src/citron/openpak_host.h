// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project, OpenPak contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

#include <QJsonObject>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QTimer>

#include "openpak/qt/host.h"
#include "openpak/qt/settings_section.h"
#include "openpak/types.h"

namespace Core {
class System;
}

namespace Common {
struct UUID;
}

class NextendoChatClient;
class NextendoToast;
class OpenPakAccountDialog;
class QMenu;
class QWidget;

// What the shared OpenPak dialogs (externals/openpak-client) need from the emulator, and the
// OpenPak flows around them (emulators/prds/openpak-ux-spec.md): the menu, sign-in and sign-out,
// the startup profile picker and the one-time setup, the OpenPak window, the settings section,
// the toasts, invitations offered to the running game, and cloud saves around a launch. One
// instance, owned by the main window. Eden's and Citron's are the same file but for the few
// lines where the two emulators differ.
class OpenPakHost : public openpak::qt::Host {
    Q_OBJECT

public:
    explicit OpenPakHost(Core::System& system, QWidget* main_window, QObject* parent = nullptr);
    ~OpenPakHost() override;

    bool IsLinked() const override;

    // Title id -> display name via the locally installed game's NACP. Falls back to hint_name
    // (a name the PLAYING client already resolved, e.g. from a friend's presence) when this
    // game isn't in the local library; falls back further to a generic "a game" if both fail.
    QString ResolveGameName(const std::string& app_id_hex,
                            const std::string& hint_name = {}) const override;

    // Title id -> base64 icon via the locally installed game's NACP; empty if not installed.
    QString ResolveGameIcon(const std::string& app_id_hex) const override;

    // 16 hex digits of the locally running title, matching Friend::app_id; empty if not running.
    std::string GetLocalAppId() const override;

    void SignIn() override;
    void SignOut() override;
    // At launch: which profile (the startup setting, or the picker), then -- once ever -- set that
    // profile up, then online. Not interactive (a game path, -u) asks nothing and only goes online.
    void RunStartup(bool interactive);
    // Each profile is its own OpenPak account. Call after anything that may have changed the
    // current user; the host drops what it showed for the last one and brings up the new one.
    void ProfileMaybeChanged();

    // The OpenPak menu (UX spec §3.1) in the menu the main window placed; open_settings opens
    // the settings dialog at its OpenPak section.
    void PopulateMenu(QMenu* menu, std::function<void()> open_settings);
    // The OpenPak window at a page, or the one already open moved there.
    void OpenWindow(int page);
    // The Open OpenPak hotkey: the window at its last page, or closed when it is open.
    void ToggleWindow();
    // Called with every window the host opens, for what a main window adds to it.
    std::function<void(OpenPakAccountDialog&)> decorate_window;
    // Asked first when a game-invitation toast is clicked; true when the main window handled it
    // (Citron's Outbound join), else the Invitations page opens.
    std::function<bool()> invitation_clicked;
    // The toasts, for a main window's own notices outside the spec (Citron's chat experiment).
    NextendoToast* Toasts() const {
        return toast;
    }
    // What the settings section (UX spec §3.13) needs from this emulator.
    openpak::qt::SettingsHooks MakeSettingsHooks();

    // Cloud saves as Ryujinx does them: the newest cloud copy before the title boots (blocking,
    // a few seconds at most), the local copy up once it has stopped.
    void PullSaveBeforeLaunch(u64 title_id);
    void PushSaveAfterExit(u64 title_id);
    void RefreshFriendCache() override;
    void NotifyFriendRequestSent(const QString& friend_code) override;

    // Skips the native Invite Friends applet entirely: hands the friend's already-fetched
    // presence app_field to the running game's invitation channel, as an accepted invitation
    // would be, where the title's own TryPopFromFriendInvitationStorageChannel poll picks it up.
    // Returns a human-readable result so the caller can show it directly.
    QString JoinFriendSession(u64 pid) override;

    void ManualSaveDownload(u64 title_id) override;
    void QuickStart(u64 title_id) override;

    // Chat: one persistent connection for the whole session (not tied to any dialog's
    // lifetime), so invite/join notifications work the same way FriendRequestReceived
    // does -- in the background, regardless of whether the chat window is open. Idempotent.
    void EnsureChatConnected() override;
    NextendoChatClient* GetChatClient() override {
        return chat_client;
    }
    bool ChatEnabled() const override;
    QString ChatInviteText() const override;

    // openpak::qt::Host: what the shared dialogs need from this emulator.
    QString ProfileName() const override;
    std::vector<Title> InstalledTitles() const override;
    std::filesystem::path ModDirectory(u64 title_id) override;
    std::filesystem::path SaveDirectory(u64 title_id) override;
    QString AccentColor() const override;
    bool IsDarkTheme() const override;
    bool NotificationsEnabled() const override;
    void SetNotificationsEnabled(bool enabled) override;
    int NotificationCorner() const override;
    void SetNotificationCorner(int corner) override;
    bool RedirectEnabled() const override;
    bool CloudSyncEnabled() const override;
    void SetCloudSyncEnabled(bool enabled) override;
    std::string ServerIp() const override;
    std::string NatIp() const override;
    void SetGuestInputSuspended(bool suspended) override;
    openpak::qt::Navigation* CreateNavigation(QObject* parent) override;
    void NetworkRedirectsChanged() override;

private:
    // The sign-in dialog. adopt copies the account's name and avatar into the profile, which only
    // the setup does: after that the profile is the person's own to rename. done hears whether
    // somebody signed in.
    void AskAndSignIn(bool adopt, const QString& intro, std::function<void(bool)> done = {});
    void GoOnline();
    void RunSetup(bool add_account);
    void PickStartupProfile();
    std::optional<Common::UUID> CurrentUser() const;
    QString ProfileName(const std::string& key) const;
    std::filesystem::path AvatarPath(const Common::UUID& uuid) const;
    void SelectUser(const Common::UUID& uuid);
    void ApplyProfileName(const std::string& name);
    // Forces the active profile's picture to match the linked OpenPak account's avatar, as
    // ApplyProfileName does the name. Fetched off the UI thread; WriteProfileAvatar writes it.
    void SyncProfileAvatar();
    void WriteProfileAvatar(const Common::UUID& uuid, const std::string& avatar_b64);
    void PollFriends();
    void PollInvitations();
    // A toast (UX spec §3.10); clicking it opens the window at page, or runs on_click, or nothing
    // for page < 0.
    void Notify(int kind, const QString& text, const QPixmap& picture = {}, int page = -1,
                std::function<void()> on_click = {});

    Core::System& system;
    QWidget* main_window;
    NextendoToast* toast = nullptr;
    QPointer<OpenPakAccountDialog> window;
    std::function<void()> open_settings;
    QTimer friend_poll_timer;
    QTimer invitation_poll_timer;
    std::set<std::string> offered_invitations;   // asked Join/Ignore, or answered
    std::set<std::string> announced_invitations; // said once, waiting for their game to run
    std::map<u64, s32> last_known_status;
    std::map<u64, std::string> last_known_game; // what each friend was playing at the last poll
    std::map<u64, int> offline_streak; // consecutive polls seen offline, not yet confirmed
    std::set<u64> last_known_requests;
    bool first_poll = true; // suppresses a toast burst for every friend already online at boot
    std::string active_profile; // the profile everything shown here belongs to

    NextendoChatClient* chat_client = nullptr;
    QString pending_chat_room_id; // set by whichever create/join is in flight, used to tag ChatMemberJoined
};
