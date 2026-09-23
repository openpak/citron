// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/friend/openpak_friends.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

#include "openpak/baas.h"
#include "openpak/friend_structs.h"
#include "openpak/platform.h"
#include "openpak/presence_state.h"
#include "openpak/session.h"

// ServiceContext::CreateEvent, not the Win32 macro windows.h may have defined by now.
#undef CreateEvent

namespace Service::Friend::OpenPak {

namespace {

namespace baas = openpak::baas;
namespace friends = openpak::friends;

using friends::PresenceState;
using friends::Uid;

/// A NetworkServiceAccountId: the friend's BAAS user id, which is what every id the guest's
/// friend list serves is, and what requests, invitations and relationships are addressed by.
using NetworkServiceAccountId = u64;

constexpr Result ResultNotificationQueueEmpty{ErrorModule::Friends, 15};

/// 2121-0090: this port may not call that command (friends contract §B.1).
constexpr Result ResultPermissionDenied{ErrorModule::Friends, 90};

/// A friends-module description (baas.h) as a Result; 0 is success.
Result FromDescription(int description) {
    return description == 0 ? ResultSuccess
                            : Result{ErrorModule::Friends, static_cast<u32>(description)};
}

/// The bits a port carries (FriendsServicePermissionLevel in the reference).
enum PermissionMask : u32 {
    UserMask = 1,
    ViewerMask = 2,
    ManagerMask = 4,
    SystemMask = 8,
};

u32 PermissionOf(std::string_view port_name) {
    if (port_name == "friend:a") {
        return ~0U;
    }
    if (port_name == "friend:m") {
        return UserMask | ViewerMask | ManagerMask;
    }
    if (port_name == "friend:v") {
        return UserMask | ViewerMask;
    }
    if (port_name == "friend:s") {
        return UserMask | SystemMask;
    }
    return UserMask;
}

bool IsNull(const Uid& user) {
    return user.high == 0 && user.low == 0;
}

Common::UUID ToUuid(const Uid& user) {
    Common::UUID uuid{};
    static_assert(sizeof(uuid) == sizeof(user));
    std::memcpy(&uuid, &user, sizeof(user));
    return uuid;
}

/// The key the client names profiles by: the Uid's RawString, as ProfileManager::CurrentUserKey
/// gives it.
std::string KeyOf(const Uid& user) {
    return ToUuid(user).RawString();
}

/// Whether this user has a signed-in BAAS account whose friends can be served. The friend graph
/// the guest sees is the server's (contract §A.1) and nothing else, and only the active profile
/// has one: any other profile a title names is offline, never handed the active one's friends.
bool AvailableFor(const Uid& user) {
    return Settings::values.enable_openpak.GetValue() && baas::Ready() && !IsNull(user) &&
           KeyOf(user) == openpak::Platform::ProfileId();
}

/// The caller's own presence group: the running title's NACP group, which is what the module
/// compares a friend's against. 0 when nothing runs.
u64 OwnPresenceGroupId() {
    return PresenceState::Instance().OwnPresenceGroupId();
}

/// The friends that pass `filter`, in the server's own order so that paging by `offset` is
/// stable between calls.
std::vector<baas::Friend> Filtered(const friends::SizedFriendFilter& filter, s32 offset) {
    return friends::Filtered(*baas::Friends(), filter, offset, OwnPresenceGroupId());
}

/// One user for the profile commands: a friend out of the list cache, else whatever the user
/// cache has. Both carry the same three fields the flat user shape requires.
std::optional<baas::User> User(u64 id) {
    if (const auto found = baas::FindFriend(id)) {
        return baas::User{found->id, found->nickname, found->thumbnail_url, found->play_log};
    }
    return baas::CachedUser(id);
}

/// Ask for the ids nothing has cached yet, off this thread (§A.7). The call that asked is
/// answered with what is already there; the next one gets the rest.
void Warm(std::span<const NetworkServiceAccountId> ids) {
    std::vector<u64> wanted;
    for (const u64 id : ids) {
        if (!User(id)) {
            wanted.push_back(id);
        }
    }

    if (!wanted.empty()) {
        baas::RunInBackground([wanted = std::move(wanted)] { baas::WarmUsers(wanted); });
    }
}

/// The IPC channel number as the channel string the POST carries. The table is the module's
/// 1-based one (contract §A.1); anything else goes out as FRIEND_CODE rather than failing a
/// request the person asked to send.
std::string RequestChannel(s32 channel) {
    if (channel >= 1 && channel <= static_cast<s32>(baas::Channels.size())) {
        return baas::Channels[static_cast<std::size_t>(channel - 1)];
    }

    LOG_WARNING(Service_Friend, "[OpenPak] Unknown friend-request channel {}; sending as FRIEND_CODE",
                channel);
    return "FRIEND_CODE";
}

/// Fire and forget: a request's POST is a network call and the caller is the game's thread.
void SendFriendRequestInBackground(baas::FriendRequestSend send) {
    baas::RunInBackground([send = std::move(send)] { baas::SendFriendRequest(send); });
}

/// One friend is no longer "newly" (§A.2): the flag is cleared locally at once -- the module does
/// that before it sends anything -- and the PATCH replaces the cached entry with the relationship
/// the server answers with.
void DropNewly(u64 friend_id) {
    baas::Update(friend_id, [](baas::Friend& one) { one.is_newly = false; });
    baas::RunInBackground([friend_id] {
        baas::PatchFriend(friend_id, "add", "/extras/self/isConfirmed", true);
    });
}

/// What friend.cpp's LoadUserSetting always answered, for a user OpenPak does not speak for.
friends::UserSettingImpl OfflineUserSetting(const Uid& user) {
    friends::UserSettingImpl setting{};
    setting.user_id = user;
    setting.presence_permission = 2;
    setting.play_log_permission = 5;
    setting.friend_request_reception = 1;
    setting.friend_code_regenerable_at = 99999999999;
    std::strcpy(setting.friend_code.code, "0000-0000-0000");
    return setting;
}

// ------------------------------------------------------------------------------------------------
// Notifications
// ------------------------------------------------------------------------------------------------

enum class NotificationEventType : u32 {
    Invalid = 0x0,
    FriendListUpdate = 0x1,
    NewFriendRequest = 0x65,
};

struct SizedNotificationInfo {
    NotificationEventType type;
    u32 padding;
    u64 network_user_id_placeholder;
};
static_assert(sizeof(SizedNotificationInfo) == 0x10);

class OpenPakNotificationService;

/// Every notification queue of one emulation session, and the thread that signals them.
///
/// The client raises its hooks on its own background threads, which outlive the emulation
/// session. Signalling a KEvent takes the kernel's scheduler lock, which needs a KThread for the
/// calling thread: Citron makes a dummy one on first use and keeps it in that thread's
/// thread-local storage (KernelCore::Impl::GetHostDummyThread), bound to the kernel of the moment. A client thread that lives for the
/// whole process would keep the dummy thread of the first session and use it against the next
/// one, and could signal while the kernel is being torn down (the Ryujinx reference crashed doing
/// this from a pool thread). So the hooks only queue the change, and a thread owned by this
/// session -- the way nim and nifm signal from their own workers -- does the signalling. It is
/// joined before the kernel goes away, because the queues holding this handler are services and
/// the services are closed first.
class NotificationEventHandler {
public:
    enum class Change { FriendListUpdate, NewFriendRequest };

    NotificationEventHandler() {
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~NotificationEventHandler() {
        thread.request_stop();
        wake.notify_all();
    }

    void Register(OpenPakNotificationService* service) {
        std::scoped_lock lock{registry_mutex};

        // When there's no space in the registry, Nintendo doesn't return any errors.
        const auto free = std::find(registry.begin(), registry.end(), nullptr);
        if (free != registry.end()) {
            *free = service;
        }
    }

    void Unregister(OpenPakNotificationService* service) {
        std::scoped_lock lock{registry_mutex};

        const auto found = std::find(registry.begin(), registry.end(), service);
        if (found != registry.end()) {
            *found = nullptr;
        }
    }

    /// From any thread, and never blocking on anything the guest holds.
    void Post(Change change) {
        {
            std::scoped_lock lock{pending_mutex};
            pending.push_back(change);
        }
        wake.notify_all();
    }

private:
    void Run(std::stop_token stop);

    std::mutex pending_mutex;
    std::condition_variable_any wake;
    std::deque<Change> pending;

    std::mutex registry_mutex;
    std::array<OpenPakNotificationService*, 0x20> registry{};

    std::jthread thread;
};

/// The session's handler while any notification queue is open. The client's hooks are
/// process-wide and set once; they reach whichever session is running through this.
std::mutex current_handler_mutex;
std::weak_ptr<NotificationEventHandler> current_handler;

void Post(NotificationEventHandler::Change change) {
    std::shared_ptr<NotificationEventHandler> handler;
    {
        std::scoped_lock lock{current_handler_mutex};
        handler = current_handler.lock();
    }

    if (handler) {
        handler->Post(change);
    }
}

std::shared_ptr<NotificationEventHandler> HandlerForThisSession() {
    static std::once_flag hooked;
    std::call_once(hooked, [] {
        // The module fills the notification queue from the push handlers and from every list
        // sync that finds a change. Nothing here holds a push connection, so the client's poll
        // stands in for it: a changed friend list -- a new friend, an accepted request, a
        // presence that moved, a flag that was written -- is a list update, and an inbox that
        // grew is a new friend request.
        baas::SetFriendListChangedHook(
            [] { Post(NotificationEventHandler::Change::FriendListUpdate); });
        baas::SetFriendRequestArrivedHook(
            [] { Post(NotificationEventHandler::Change::NewFriendRequest); });

        // An invitation has no event type of its own in this queue. The list update is what
        // sends a system screen back to the service, where 22010 has the new count.
        openpak::client::session::SetInvitationsChangedHook(
            [] { Post(NotificationEventHandler::Change::FriendListUpdate); });
    });

    std::scoped_lock lock{current_handler_mutex};
    auto handler = current_handler.lock();
    if (!handler) {
        handler = std::make_shared<NotificationEventHandler>();
        current_handler = handler;
    }
    return handler;
}

class OpenPakNotificationService final : public ServiceFramework<OpenPakNotificationService> {
public:
    explicit OpenPakNotificationService(Core::System& system_,
                                        std::shared_ptr<NotificationEventHandler> handler_,
                                        const Common::UUID& user, u32 permission_)
        : ServiceFramework{system_, "INotificationService"}, handler{std::move(handler_)},
          user_key{user.RawString()}, permission{permission_},
          service_context{system_, "INotificationService"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&OpenPakNotificationService::GetEvent>, "GetEvent"},
            {1, D<&OpenPakNotificationService::Clear>, "Clear"},
            {2, D<&OpenPakNotificationService::Pop>, "Pop"},
        };
        // clang-format on

        RegisterHandlers(functions);

        notification_event = service_context.CreateEvent("INotificationService:NotifyEvent");

        handler->Register(this);
    }

    ~OpenPakNotificationService() override {
        // Unregistered first: once this returns, the signalling thread cannot reach the event.
        handler->Unregister(this);
        service_context.CloseEvent(notification_event);
    }

    void SignalFriendListUpdate(const std::string& target) {
        std::scoped_lock lock{mutex};
        SignalFriendListUpdateLocked(target);
    }

    void SignalNewFriendRequest(const std::string& target) {
        std::scoped_lock lock{mutex};

        if ((permission & ViewerMask) == 0 || user_key != target) {
            return;
        }

        if (!has_new_friend_request) {
            if (notifications.size() == 100) {
                SignalFriendListUpdateLocked(target);
            }

            notifications.push_back({NotificationEventType::NewFriendRequest, 0, 0});
            has_new_friend_request = true;
        }

        notification_event->Signal();
    }

private:
    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_Friend, "called");
        *out_event = &notification_event->GetReadableEvent();
        R_SUCCEED();
    }

    Result Clear() {
        LOG_DEBUG(Service_Friend, "called");
        std::scoped_lock lock{mutex};

        has_new_friend_request = false;
        has_friend_list_update = false;
        notifications.clear();

        R_SUCCEED();
    }

    Result Pop(Out<SizedNotificationInfo> out_info) {
        LOG_DEBUG(Service_Friend, "called");
        std::scoped_lock lock{mutex};

        if (notifications.empty()) {
            *out_info = {};
            R_THROW(ResultNotificationQueueEmpty);
        }

        *out_info = notifications.front();
        notifications.pop_front();

        if (out_info->type == NotificationEventType::FriendListUpdate) {
            has_friend_list_update = false;
        } else if (out_info->type == NotificationEventType::NewFriendRequest) {
            has_new_friend_request = false;
        }

        R_SUCCEED();
    }

    void SignalFriendListUpdateLocked(const std::string& target) {
        if (user_key != target) {
            return;
        }

        if (!has_friend_list_update) {
            SizedNotificationInfo friend_list_notification{};

            if (!notifications.empty()) {
                friend_list_notification = notifications.front();
                notifications.pop_front();
            }

            friend_list_notification.type = NotificationEventType::FriendListUpdate;
            has_friend_list_update = true;

            if (has_new_friend_request) {
                SizedNotificationInfo new_friend_request_notification{};

                if (!notifications.empty()) {
                    new_friend_request_notification = notifications.front();
                    notifications.pop_front();
                }

                new_friend_request_notification.type = NotificationEventType::NewFriendRequest;
                notifications.push_front(new_friend_request_notification);
            }

            // We defer this to make sure we are on top of the queue.
            notifications.push_front(friend_list_notification);
        }

        notification_event->Signal();
    }

    std::shared_ptr<NotificationEventHandler> handler;
    const std::string user_key;
    const u32 permission;

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* notification_event;

    std::mutex mutex;
    std::deque<SizedNotificationInfo> notifications;
    bool has_new_friend_request = false;
    bool has_friend_list_update = false;
};

void NotificationEventHandler::Run(std::stop_token stop) {
    Common::SetCurrentThreadName("friend:Notify");

    for (;;) {
        Change change;
        {
            std::unique_lock lock{pending_mutex};
            if (!wake.wait(lock, stop, [this] { return !pending.empty(); })) {
                return;
            }
            change = pending.front();
            pending.pop_front();
        }

        // A queue is per user, and an event for nobody is one no service will take.
        const std::string target = openpak::Platform::ProfileId();
        if (target.empty()) {
            continue;
        }

        std::scoped_lock lock{registry_mutex};
        for (OpenPakNotificationService* service : registry) {
            if (service == nullptr) {
                continue;
            }
            if (change == Change::FriendListUpdate) {
                service->SignalFriendListUpdate(target);
            } else {
                service->SignalNewFriendRequest(target);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// IFriendService
// ------------------------------------------------------------------------------------------------

class OpenPakFriendService final : public ServiceFramework<OpenPakFriendService> {
public:
    explicit OpenPakFriendService(Core::System& system_, u32 permission_)
        : ServiceFramework{system_, "IFriendService"}, permission{permission_},
          service_context{system_, "IFriendService"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&OpenPakFriendService::GetCompletionEvent>, "GetCompletionEvent"},
            {1, D<&OpenPakFriendService::Cancel>, "Cancel"},
            {10100, D<&OpenPakFriendService::GetFriendListIds>, "GetFriendListIds"},
            {10101, D<&OpenPakFriendService::GetFriendList>, "GetFriendList"},
            {10102, D<&OpenPakFriendService::UpdateFriendInfo>, "UpdateFriendInfo"},
            {10110, D<&OpenPakFriendService::GetFriendProfileImage>, "GetFriendProfileImage"},
            {10111, nullptr, "GetFriendProfileImageWithImageSize"}, // 18.0.0+
            {10120, D<&OpenPakFriendService::CheckFriendListAvailability>, "CheckFriendListAvailability"},
            {10121, D<&OpenPakFriendService::EnsureFriendListAvailable>, "EnsureFriendListAvailable"},
            {10200, D<&OpenPakFriendService::SendFriendRequestForApplication>, "SendFriendRequestForApplication"},
            {10211, D<&OpenPakFriendService::AddFacedFriendRequestForApplication>, "AddFacedFriendRequestForApplication"},
            {10400, D<&OpenPakFriendService::GetBlockedUserListIds>, "GetBlockedUserListIds"},
            {10420, D<&OpenPakFriendService::CheckBlockedUserListAvailability>, "CheckBlockedUserListAvailability"},
            {10421, D<&OpenPakFriendService::EnsureBlockedUserListAvailable>, "EnsureBlockedUserListAvailable"},
            {10500, D<&OpenPakFriendService::GetProfileList>, "GetProfileList"},
            {10501, nullptr, "GetProfileListV2"}, // 18.0.0+
            {10600, D<&OpenPakFriendService::DeclareOpenOnlinePlaySession>, "DeclareOpenOnlinePlaySession"},
            {10601, D<&OpenPakFriendService::DeclareCloseOnlinePlaySession>, "DeclareCloseOnlinePlaySession"},
            {10610, D<&OpenPakFriendService::UpdateUserPresence>, "UpdateUserPresence"},
            {10700, D<&OpenPakFriendService::GetPlayHistoryRegistrationKey>, "GetPlayHistoryRegistrationKey"},
            {10701, D<&OpenPakFriendService::GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId>, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId"},
            {10702, D<&OpenPakFriendService::AddPlayHistory>, "AddPlayHistory"},
            {11000, D<&OpenPakFriendService::GetProfileImageUrl>, "GetProfileImageUrl"},
            {11001, nullptr, "GetProfileImageUrlV2"}, // 18.0.0+
            {20100, D<&OpenPakFriendService::GetFriendCount>, "GetFriendCount"},
            {20101, D<&OpenPakFriendService::GetNewlyFriendCount>, "GetNewlyFriendCount"},
            {20102, D<&OpenPakFriendService::GetFriendDetailedInfo>, "GetFriendDetailedInfo"},
            {20103, D<&OpenPakFriendService::SyncFriendList>, "SyncFriendList"},
            {20104, D<&OpenPakFriendService::SyncFriendList>, "RequestSyncFriendList"},
            {20105, D<&OpenPakFriendService::GetFriendListForViewer>, "GetFriendListForViewerV1"},
            {20106, D<&OpenPakFriendService::UpdateFriendInfoForViewer>, "UpdateFriendInfoForViewerV1"},
            {20107, D<&OpenPakFriendService::GetFriendDetailedInfo>, "GetFriendDetailedInfoV2"}, // 20.0.0+
            {20108, D<&OpenPakFriendService::GetFriendListForViewerV2>, "GetFriendListForViewerV2"}, // 22.0.0+
            {20109, D<&OpenPakFriendService::UpdateFriendInfoForViewerV2>, "UpdateFriendInfoForViewerV2"}, // 22.0.0+
            {20110, D<&OpenPakFriendService::LoadFriendSetting>, "LoadFriendSettingV1"},
            {20111, D<&OpenPakFriendService::LoadFriendSettingV2>, "LoadFriendSettingV2"}, // 22.0.0+
            {20200, D<&OpenPakFriendService::GetReceivedFriendRequestCount>, "GetReceivedFriendRequestCount"},
            {20201, D<&OpenPakFriendService::GetFriendRequestList>, "GetFriendRequestListV1"},
            {20202, D<&OpenPakFriendService::GetFriendRequestListV2>, "GetFriendRequestListV2"}, // 20.0.0+
            {20203, D<&OpenPakFriendService::GetReceivedFriendRequestPushCount>, "GetFriendRequestReceivedNotificationCount"}, // 22.0.0+
            {20300, D<&OpenPakFriendService::GetFriendCandidateList>, "GetFriendCandidateList"},
            {20301, D<&OpenPakFriendService::GetNintendoNetworkIdInfo>, "GetNintendoNetworkIdInfo"},
            {20302, D<&OpenPakFriendService::GetSnsAccountLinkage>, "GetSnsAccountLinkage"}, // 5.0.0-19.0.1
            {20303, D<&OpenPakFriendService::GetSnsAccountProfile>, "GetSnsAccountProfile"}, // 5.0.0-19.0.1
            {20304, D<&OpenPakFriendService::GetSnsAccountFriendList>, "GetSnsAccountFriendList"}, // 5.0.0-19.0.1
            {20400, D<&OpenPakFriendService::GetBlockedUserList>, "GetBlockedUserListV1"},
            {20401, D<&OpenPakFriendService::SyncBlockedUserList>, "SyncBlockedUserList"},
            {20402, D<&OpenPakFriendService::GetBlockedUserListV2>, "GetBlockedUserListV2"}, // 20.0.0+
            {20500, D<&OpenPakFriendService::GetProfileExtraList>, "GetProfileExtraListV1"},
            {20501, D<&OpenPakFriendService::GetRelationship>, "GetRelationship"},
            {20502, D<&OpenPakFriendService::GetProfileExtraListV2>, "GetProfileExtraListV2"}, // 19.0.0+
            {20600, D<&OpenPakFriendService::GetUserPresenceView>, "GetUserPresenceViewV1"},
            {20601, D<&OpenPakFriendService::GetUserPresenceView>, "GetUserPresenceViewV2"}, // 19.0.0+
            {20700, D<&OpenPakFriendService::GetPlayHistoryList>, "GetPlayHistoryListV1"},
            {20701, D<&OpenPakFriendService::GetPlayHistoryStatistics>, "GetPlayHistoryStatistics"},
            {20702, nullptr, "GetPlayHistoryListV2"}, // 19.0.0+
            {20800, D<&OpenPakFriendService::LoadUserSetting>, "LoadUserSettingV1"},
            {20801, D<&OpenPakFriendService::SyncUserSetting>, "SyncUserSetting"},
            {20802, D<&OpenPakFriendService::LoadUserSettingV2>, "LoadUserSettingV2"}, // 19.0.0+
            {20900, D<&OpenPakFriendService::RequestListSummaryOverlayNotification>, "RequestListSummaryOverlayNotification"},
            {21000, D<&OpenPakFriendService::GetExternalApplicationCatalog>, "GetExternalApplicationCatalog"},
            {22000, D<&OpenPakFriendService::GetReceivedFriendInvitationList>, "GetReceivedFriendInvitationListV1"},
            {22001, D<&OpenPakFriendService::GetReceivedFriendInvitationDetailedInfo>, "GetReceivedFriendInvitationDetailedInfoV1"},
            {22002, D<&OpenPakFriendService::GetReceivedFriendInvitationListV2>, "GetReceivedFriendInvitationListV2"}, // 19.0.0+
            {22003, D<&OpenPakFriendService::GetReceivedFriendInvitationDetailedInfoV2>, "GetReceivedFriendInvitationDetailedInfoV2"}, // 19.0.0+
            {22010, D<&OpenPakFriendService::GetReceivedFriendInvitationCountCache>, "GetReceivedFriendInvitationCountCache"},
            {30100, D<&OpenPakFriendService::DropFriendNewlyFlags>, "DropFriendNewlyFlags"},
            {30101, D<&OpenPakFriendService::DeleteFriend>, "DeleteFriend"},
            {30110, D<&OpenPakFriendService::DropFriendNewlyFlag>, "DropFriendNewlyFlag"},
            {30120, D<&OpenPakFriendService::ChangeFriendFavoriteFlag>, "ChangeFriendFavoriteFlag"},
            {30121, D<&OpenPakFriendService::ChangeFriendOnlineNotificationFlag>, "ChangeFriendOnlineNotificationFlag"},
            {30130, D<&OpenPakFriendService::ChangeFriendNote>, "SetFriendNote"}, // 22.0.0+
            {30131, D<&OpenPakFriendService::SendPendingFriendChangeUnchecked>, "RequestUploadPendingNote"}, // 22.0.0+
            {30190, D<&OpenPakFriendService::SendPendingFriendChange>, "RequestSyncLocalUpdates"}, // 22.0.0+
            {30200, D<&OpenPakFriendService::SendFriendRequest>, "SendFriendRequest"},
            {30201, D<&OpenPakFriendService::SendFriendRequestWithApplicationInfo>, "SendFriendRequestWithApplicationInfoV1"},
            {30202, D<&OpenPakFriendService::CancelFriendRequest>, "CancelFriendRequest"},
            {30203, D<&OpenPakFriendService::AcceptFriendRequest>, "AcceptFriendRequest"},
            {30204, D<&OpenPakFriendService::RejectFriendRequest>, "RejectFriendRequest"},
            {30205, D<&OpenPakFriendService::ReadFriendRequest>, "ReadFriendRequest"},
            {30210, D<&OpenPakFriendService::GetFacedFriendRequestRegistrationKey>, "GetFacedFriendRequestRegistrationKey"},
            {30211, D<&OpenPakFriendService::AddFacedFriendRequest>, "AddFacedFriendRequest"},
            {30212, D<&OpenPakFriendService::CancelFacedFriendRequest>, "CancelFacedFriendRequest"},
            {30213, D<&OpenPakFriendService::GetFacedFriendRequestProfileImage>, "GetFacedFriendRequestProfileImage"},
            {30214, D<&OpenPakFriendService::GetFacedFriendRequestProfileImageFromPath>, "GetFacedFriendRequestProfileImageFromPath"},
            {30215, D<&OpenPakFriendService::SendFriendRequestWithExternalApplicationCatalogId>, "SendFriendRequestWithExternalApplicationCatalogId"},
            {30216, D<&OpenPakFriendService::ResendFacedFriendRequest>, "ResendFacedFriendRequest"},
            {30217, D<&OpenPakFriendService::SendFriendRequestWithNintendoNetworkIdInfo>, "SendFriendRequestWithNintendoNetworkIdInfo"},
            {30218, D<&OpenPakFriendService::SendFriendRequestWithApplicationInfoV2>, "SendFriendRequestWithApplicationInfoV2"}, // 20.0.0+
            {30300, D<&OpenPakFriendService::GetSnsAccountLinkPageUrl>, "GetSnsAccountLinkPageUrl"}, // 5.0.0-19.0.1
            {30301, D<&OpenPakFriendService::UnlinkSnsAccount>, "UnlinkSnsAccount"}, // 5.0.0-19.0.1
            {30400, D<&OpenPakFriendService::BlockUser>, "BlockUser"},
            {30401, D<&OpenPakFriendService::BlockUserWithApplicationInfo>, "BlockUserWithApplicationInfoV1"},
            {30402, D<&OpenPakFriendService::UnblockUser>, "UnblockUser"},
            {30403, nullptr, "BlockUserWithApplicationInfoV2"}, // 20.0.0+
            {30500, D<&OpenPakFriendService::GetProfileExtraFromFriendCode>, "GetProfileExtraFromFriendCodeV1"},
            {30501, nullptr, "GetProfileExtraFromFriendCodeV2"}, // 19.0.0+
            {30700, D<&OpenPakFriendService::DeletePlayHistory>, "DeletePlayHistory"},
            {30701, nullptr, "AddPlayHistoryWithApplication"}, // 19.0.0+
            {30810, D<&OpenPakFriendService::ChangePresencePermission>, "ChangePresencePermission"},
            {30811, D<&OpenPakFriendService::ChangeFriendRequestReception>, "ChangeFriendRequestReception"},
            {30812, D<&OpenPakFriendService::ChangePlayLogPermission>, "ChangePlayLogPermission"},
            {30820, D<&OpenPakFriendService::IssueFriendCode>, "IssueFriendCode"},
            {30830, D<&OpenPakFriendService::ClearPlayLog>, "ClearPlayLog"},
            {30900, &OpenPakFriendService::SendFriendInvitation, "SendFriendInvitationV1"},
            {30901, &OpenPakFriendService::SendFriendInvitationV2, "SendFriendInvitationV2"}, // 19.0.0+
            {30910, D<&OpenPakFriendService::ReadFriendInvitation>, "ReadFriendInvitation"},
            {30911, D<&OpenPakFriendService::ReadAllFriendInvitations>, "ReadAllFriendInvitations"},
            {31000, nullptr, "OpenUser"}, // 19.0.0+
            {40100, D<&OpenPakFriendService::DeleteFriendListCache>, "DeleteFriendListCache"},
            {40400, D<&OpenPakFriendService::DeleteBlockedUserListCache>, "DeleteBlockedUserListCache"},
            {49900, D<&OpenPakFriendService::DeleteNetworkServiceAccountCache>, "DeleteNetworkServiceAccountCache"},
        };
        // clang-format on

        RegisterHandlers(functions);

        completion_event = service_context.CreateEvent("IFriendService:CompletionEvent");

        // Nothing here completes asynchronously as far as the guest can tell: every command
        // answers at once, so the event is simply signalled, as the reference leaves it.
        completion_event->Signal();
    }

    ~OpenPakFriendService() override {
        service_context.CloseEvent(completion_event);
    }

private:
    /// Whether this port carries the viewer bit (friend:v, friend:m, friend:a). A viewer reads a
    /// friend's presence blob whatever group it belongs to; friend:u and friend:s get the privacy
    /// filter (contract §B.3).
    bool Viewer() const {
        return (permission & ViewerMask) != 0;
    }

    /// The viewer bit, or 2121-0090 (contract §B.1). 20xxx and 22xxx need it.
    Result RequireViewer() const {
        return Viewer() ? ResultSuccess : ResultPermissionDenied;
    }

    /// The manager bit (friend:m, friend:a), or 2121-0090. Every 30xxx needs it.
    Result RequireManager() const {
        return (permission & ManagerMask) != 0 ? ResultSuccess : ResultPermissionDenied;
    }

    /// The system bit (friend:s, friend:a), or 2121-0090. 40100/40400/49900 need it.
    Result RequireSystem() const {
        return (permission & SystemMask) != 0 ? ResultSuccess : ResultPermissionDenied;
    }

    Result GetCompletionEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        LOG_DEBUG(Service_Friend, "called");
        *out_event = &completion_event->GetReadableEvent();
        R_SUCCEED();
    }

    Result Cancel() {
        LOG_DEBUG(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    // ---- 10xxx: every port ----

    Result GetFriendListIds(Out<s32> out_count,
                            OutArray<NetworkServiceAccountId, BufferAttr_HipcPointer> out_ids,
                            s32 offset, Uid user, friends::SizedFriendFilter filter,
                            ClientProcessId pid) {
        *out_count = 0;
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, offset={}, uuid=0x{}, pid={}", offset,
                        KeyOf(user), *pid);
            R_SUCCEED();
        }

        s32 count = 0;
        for (const baas::Friend& one : Filtered(filter, offset)) {
            if (static_cast<std::size_t>(count) == out_ids.size()) {
                break;
            }
            out_ids[count++] = one.id;
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetFriendList(Out<s32> out_count,
                         OutArray<friends::FriendImpl, BufferAttr_HipcMapAlias> out_list,
                         s32 offset, Uid user, friends::SizedFriendFilter filter,
                         ClientProcessId pid) {
        *out_count = 0;
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, offset={}, uuid=0x{}, pid={}", offset,
                        KeyOf(user), *pid);
            R_SUCCEED();
        }

        const u64 own_group = OwnPresenceGroupId();
        s32 count = 0;
        for (const baas::Friend& one : Filtered(filter, offset)) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = friends::ToFriendImpl(one, user, own_group, Viewer());
        }

        LOG_DEBUG(Service_Friend, "[OpenPak] Served {} friends to the guest", count);

        *out_count = count;
        R_SUCCEED();
    }

    Result UpdateFriendInfo(OutArray<friends::FriendImpl, BufferAttr_HipcMapAlias> out_info,
                            Uid user,
                            InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids,
                            ClientProcessId pid) {
        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, pid={}", KeyOf(user), *pid);
            R_SUCCEED();
        }

        // The guest asks about specific ids and expects them back in the order it asked, so this
        // answers per slot rather than filling the buffer with whoever matched.
        const u64 own_group = OwnPresenceGroupId();
        const auto list = baas::Friends();

        for (std::size_t index = 0; index < friend_ids.size() && index < out_info.size();
             index++) {
            out_info[index] = {};

            for (const baas::Friend& one : *list) {
                if (one.id == friend_ids[index]) {
                    out_info[index] = friends::ToFriendImpl(one, user, own_group, Viewer());
                    break;
                }
            }
        }

        R_SUCCEED();
    }

    Result GetFriendProfileImage(Out<s32> out_size, Uid user, NetworkServiceAccountId friend_id) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}", KeyOf(user),
                    friend_id);
        *out_size = 0;
        R_SUCCEED();
    }

    Result CheckFriendListAvailability(Out<bool> out_available, Uid user) {
        // Offline this is what friend.cpp always said: a list of nobody, available at once.
        *out_available = AvailableFor(user) ? baas::FriendListAvailable() : true;
        R_SUCCEED();
    }

    Result EnsureFriendListAvailable(Uid user) {
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // The console syncs inline when it has no cache. Here the sync is started and the guest
        // answered at once: blocking a game's thread on an HTTP round trip is the one thing this
        // service may never do.
        if (AvailableFor(user) && !baas::FriendListAvailable()) {
            baas::RunInBackground([] { baas::SyncFriendList(true); });
        }

        R_SUCCEED();
    }

    Result SendFriendRequestForApplication(
        Uid user, NetworkServiceAccountId friend_id,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> own_name,
        ClientProcessId pid) {
        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}, pid={}",
                        KeyOf(user), friend_id, *pid);
            R_SUCCEED();
        }

        // The application info comes from the caller's process, and the channel is always
        // IN_APP: this is the in-game "send friend request" button.
        const u64 title_id = system.GetApplicationProcessProgramID();

        baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = "IN_APP";
        send.application_id = title_id;
        send.presence_group_id = title_id;
        std::tie(send.target_name, send.target_language) = friends::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = friends::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result AddFacedFriendRequestForApplication(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
        R_SUCCEED();
    }

    Result GetBlockedUserListIds(Out<s32> out_count,
                                 OutArray<NetworkServiceAccountId, BufferAttr_HipcPointer> out_ids,
                                 s32 offset, Uid user) {
        *out_count = 0;

        // Offline this is safe to stub, as there should be no adverse consequences from
        // reporting no blocked users.
        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const auto blocks = baas::Blocks();
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < blocks->size() && static_cast<std::size_t>(count) < out_ids.size();
             index++) {
            out_ids[count++] = (*blocks)[index].id;
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result CheckBlockedUserListAvailability(Out<bool> out_available, Uid user) {
        *out_available = AvailableFor(user) ? baas::BlockListAvailable() : true;
        R_SUCCEED();
    }

    Result EnsureBlockedUserListAvailable(Uid user) {
        if (AvailableFor(user) && !baas::BlockListAvailable()) {
            baas::RunInBackground([] { baas::SyncBlockList(); });
        }
        R_SUCCEED();
    }

    Result GetProfileList(OutArray<friends::ProfileImpl, BufferAttr_HipcMapAlias> out_list,
                          Uid user,
                          InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
            R_SUCCEED();
        }

        // Answered per slot, in the order asked, as the module matches its results by id. A user
        // nobody has looked up yet is left invalid and fetched for the next call.
        for (std::size_t index = 0; index < friend_ids.size() && index < out_list.size();
             index++) {
            const auto found = User(friend_ids[index]);
            out_list[index] = found ? friends::ToProfileImpl(*found) : friends::ProfileImpl{};
        }

        Warm(friend_ids);
        R_SUCCEED();
    }

    Result DeclareOpenOnlinePlaySession(Uid user) {
        LOG_DEBUG(Service_Friend, "called, uuid=0x{}", KeyOf(user));
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // Other profiles have no account to publish for, and are not kept.
        if (AvailableFor(user)) {
            PresenceState::Instance().DeclareOpenOnlinePlaySession(KeyOf(user));
        }
        R_SUCCEED();
    }

    Result DeclareCloseOnlinePlaySession(Uid user) {
        LOG_DEBUG(Service_Friend, "called, uuid=0x{}", KeyOf(user));
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (AvailableFor(user)) {
            PresenceState::Instance().DeclareCloseOnlinePlaySession(KeyOf(user));
        }
        R_SUCCEED();
    }

    Result UpdateUserPresence(Uid user,
                              InLargeData<friends::UserPresenceImpl, BufferAttr_HipcPointer> presence,
                              ClientProcessId pid) {
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // Commit: the declaration byte and the whole blob, published as PLAYING/ONLINE and an
        // appField object by the session, and only when one of them changed.
        LOG_DEBUG(Service_Friend, "called, declaration {}, appField {}",
                  presence->online_play_declaration,
                  friends::BlobToAppField(presence->app_key_value_storage));

        if (AvailableFor(user)) {
            PresenceState::Instance().UpdateUserPresence(KeyOf(user), *presence);
        }
        R_SUCCEED();
    }

    Result GetPlayHistoryRegistrationKey(
        OutLargeData<friends::PlayHistoryRegistrationKey, BufferAttr_HipcPointer> out_key,
        bool arg2, Uid user) {
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        // The console keeps a per-user Uuid in its save data and signs the key with one of eight
        // HMAC keys. Play history is not served, so a random Uuid and a blank hash do.
        const Common::UUID random = Common::UUID::MakeRandom();

        friends::PlayHistoryRegistrationKey key{};
        key.type = 0x101;
        key.key_index = static_cast<u8>(random.uuid[0] & 7);
        key.user_id_bool = 0;
        key.unknown_bool = arg2 ? 1 : 0;
        std::memcpy(&key.uuid, random.uuid.data(), sizeof(key.uuid));

        *out_key = key;
        R_SUCCEED();
    }

    Result GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId(
        OutLargeData<friends::PlayHistoryRegistrationKey, BufferAttr_HipcPointer> out_key) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_key = {};
        R_SUCCEED();
    }

    Result AddPlayHistory(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
        R_SUCCEED();
    }

    Result GetProfileImageUrl(Out<friends::Url> out_url) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_url = {};
        R_SUCCEED();
    }

    // ---- 20xxx: viewer ports ----

    Result GetFriendCount(Out<s32> out_count, Uid user, friends::SizedFriendFilter filter,
                          ClientProcessId pid) {
        *out_count = 0;
        R_TRY(RequireViewer());

        *out_count = AvailableFor(user) ? static_cast<s32>(Filtered(filter, 0).size()) : 0;
        R_SUCCEED();
    }

    Result GetNewlyFriendCount(Out<s32> out_count, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        // "Newly" is extras.self.isConfirmed absent or false (§A.1): a friendship the person has
        // not yet looked at, which is what the badge on the friend list counts.
        const auto list = baas::Friends();
        *out_count = static_cast<s32>(std::count_if(
            list->begin(), list->end(), [](const baas::Friend& one) { return one.is_newly; }));
        R_SUCCEED();
    }

    Result GetFriendDetailedInfo(
        OutLargeData<friends::FriendDetailedInfoImpl, BufferAttr_HipcPointer> out_info, Uid user,
        NetworkServiceAccountId friend_id) {
        *out_info = {};
        R_TRY(RequireViewer());

        const auto found = AvailableFor(user) ? baas::FindFriend(friend_id) : std::nullopt;
        R_UNLESS(found.has_value(), FromDescription(baas::FriendNotFound));

        *out_info = friends::ToDetailedInfoImpl(*found, user);
        R_SUCCEED();
    }

    Result SyncFriendList(Uid user) {
        R_TRY(RequireViewer());

        // The console clears the cooldown and syncs inline; here the sync is started and the
        // guest answered at once. The list on screen refreshes through the notification event.
        if (AvailableFor(user)) {
            baas::RunInBackground([] { baas::SyncFriendList(true); });
        }
        R_SUCCEED();
    }

    Result GetFriendListForViewer(
        Out<s32> out_count, OutArray<friends::FriendForViewerImpl, BufferAttr_HipcMapAlias> out_list,
        s32 offset, Uid user, friends::SizedFriendFilter filter) {
        *out_count = 0;
        R_TRY(RequireViewer());
        R_UNLESS(!IsNull(user), ResultInvalidArgument);

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const u64 own_group = OwnPresenceGroupId();
        s32 count = 0;
        for (const baas::Friend& one : Filtered(filter, offset)) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = friends::ToFriendForViewerImpl(one, user, own_group);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result UpdateFriendInfoForViewer(
        OutArray<friends::FriendForViewerImpl, BufferAttr_HipcMapAlias> out_info, Uid user,
        InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        R_TRY(RequireViewer());

        const bool available = AvailableFor(user);
        const u64 own_group = OwnPresenceGroupId();

        for (std::size_t index = 0; index < friend_ids.size() && index < out_info.size();
             index++) {
            const auto found = available ? baas::FindFriend(friend_ids[index]) : std::nullopt;
            out_info[index] = found ? friends::ToFriendForViewerImpl(*found, user, own_group)
                                    : friends::FriendForViewerImpl{};
        }
        R_SUCCEED();
    }

    // 20108 / 20109 are the 0x220 viewer shape. Its first part is the same, but the audit does
    // not pin where the valid flag sits in it, and a friend written without one is a friend the
    // caller drops -- so the commands exist (a missing id is a CMIF error the caller cannot tell
    // from a real failure) and answer an empty list.
    Result GetFriendListForViewerV2(Out<s32> out_count) {
        *out_count = 0;
        R_RETURN(RequireViewer());
    }

    Result UpdateFriendInfoForViewerV2() {
        R_RETURN(RequireViewer());
    }

    Result LoadFriendSetting(
        OutLargeData<friends::FriendSettingImpl, BufferAttr_HipcPointer> out_setting, Uid user,
        NetworkServiceAccountId friend_id) {
        *out_setting = {};
        R_TRY(RequireViewer());

        const auto found = AvailableFor(user) ? baas::FindFriend(friend_id) : std::nullopt;
        R_UNLESS(found.has_value(), FromDescription(baas::FriendNotFound));

        *out_setting = friends::ToFriendSettingImpl(*found, user);
        R_SUCCEED();
    }

    Result LoadFriendSettingV2(
        OutLargeData<friends::FriendSettingImplV2, BufferAttr_HipcPointer> out_setting, Uid user,
        NetworkServiceAccountId friend_id) {
        *out_setting = {};
        R_TRY(RequireViewer());

        const auto found = AvailableFor(user) ? baas::FindFriend(friend_id) : std::nullopt;
        R_UNLESS(found.has_value(), FromDescription(baas::FriendNotFound));

        *out_setting = friends::ToFriendSettingImplV2(*found, user);
        R_SUCCEED();
    }

    Result GetReceivedFriendRequestCount(Out<s32> out_unread, Out<s32> out_read, Uid user) {
        *out_unread = 0;
        *out_read = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
            R_SUCCEED();
        }

        // The badge (contract §A.4.3): unread is the inbox items without extras.receiver.read
        // true, read the ones with it.
        for (const baas::Request& request : *baas::InboxRequests()) {
            if (request.read) {
                ++*out_read;
            } else {
                ++*out_unread;
            }
        }
        R_SUCCEED();
    }

    /// Type 1 is the outbox (sent), 2 the inbox (received). Type 0 is in-person requests, which
    /// live in faced.v2.bin and never touch REST.
    static std::shared_ptr<const std::vector<baas::Request>> Box(s32 list_type) {
        switch (list_type) {
        case 1:
            return baas::OutboxRequests();
        case 2:
            return baas::InboxRequests();
        default:
            return std::make_shared<const std::vector<baas::Request>>();
        }
    }

    Result GetFriendRequestList(Out<s32> out_count,
                                OutArray<friends::FriendRequestImpl, BufferAttr_HipcMapAlias> out_list,
                                s32 offset, s32 list_type, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(STUBBED) called, uuid=0x{}, offset={}, list_type={}",
                      KeyOf(user), offset, list_type);
            R_SUCCEED();
        }

        const auto box = Box(list_type);
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < box->size() && static_cast<std::size_t>(count) < out_list.size(); index++) {
            out_list[count++] = friends::ToRequestImpl((*box)[index], user, list_type);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetFriendRequestListV2(
        Out<s32> out_count, OutArray<friends::FriendRequestImplV2, BufferAttr_HipcMapAlias> out_list,
        s32 offset, s32 list_type, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            LOG_DEBUG(Service_Friend, "(STUBBED) called, uuid=0x{}, offset={}, list_type={}",
                      KeyOf(user), offset, list_type);
            R_SUCCEED();
        }

        const auto box = Box(list_type);
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < box->size() && static_cast<std::size_t>(count) < out_list.size(); index++) {
            out_list[count++] = friends::ToRequestImplV2((*box)[index], user, list_type);
        }

        *out_count = count;
        R_SUCCEED();
    }

    // 20203 counts the `friend_request_received` pushes this console has seen. Nothing here holds
    // a push connection -- the boxes are polled -- so the counter is always 0, which is what a
    // console that has been pushed nothing reports.
    Result GetReceivedFriendRequestPushCount(Out<s32> out_count) {
        *out_count = 0;
        R_RETURN(RequireViewer());
    }

    Result GetFriendCandidateList(Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetNintendoNetworkIdInfo(
        OutLargeData<friends::NintendoNetworkIdUserInfo, BufferAttr_HipcPointer> out_info,
        Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_info = {};
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetSnsAccountLinkage(Out<friends::SnsAccountLinkage> out_linkage) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_linkage = {};
        R_SUCCEED();
    }

    Result GetSnsAccountProfile(
        OutLargeData<friends::SnsAccountProfile, BufferAttr_HipcPointer> out_profile) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_profile = {};
        R_SUCCEED();
    }

    Result GetSnsAccountFriendList(Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetBlockedUserList(Out<s32> out_count,
                              OutArray<friends::BlockedUserImpl, BufferAttr_HipcMapAlias> out_list,
                              s32 offset, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const auto blocks = baas::Blocks();
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < blocks->size() && static_cast<std::size_t>(count) < out_list.size();
             index++) {
            out_list[count++] = friends::ToBlockedUserImpl((*blocks)[index], user);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetBlockedUserListV2(Out<s32> out_count,
                                OutArray<friends::BlockedUserImplV2, BufferAttr_HipcMapAlias> out_list,
                                s32 offset, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        const auto blocks = baas::Blocks();
        s32 count = 0;
        for (std::size_t index = static_cast<std::size_t>((std::max)(offset, 0));
             index < blocks->size() && static_cast<std::size_t>(count) < out_list.size();
             index++) {
            out_list[count++] = friends::ToBlockedUserImplV2((*blocks)[index], user);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result SyncBlockedUserList(Uid user) {
        R_TRY(RequireViewer());

        if (AvailableFor(user)) {
            baas::RunInBackground([] { baas::SyncBlockList(); });
        }
        R_SUCCEED();
    }

    Result GetProfileExtraList(OutArray<friends::ProfileExtraImpl, BufferAttr_HipcMapAlias> out_list,
                               Uid user,
                               InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        R_TRY(RequireViewer());

        for (std::size_t index = 0; index < friend_ids.size() && index < out_list.size();
             index++) {
            const auto found = User(friend_ids[index]);
            out_list[index] = found ? friends::ToProfileExtraImpl(*found)
                                    : friends::ProfileExtraImpl{};
        }

        Warm(friend_ids);
        R_SUCCEED();
    }

    Result GetProfileExtraListV2(
        OutArray<friends::ProfileExtraImplV2, BufferAttr_HipcMapAlias> out_list, Uid user,
        InArray<NetworkServiceAccountId, BufferAttr_HipcPointer> friend_ids) {
        R_TRY(RequireViewer());

        for (std::size_t index = 0; index < friend_ids.size() && index < out_list.size();
             index++) {
            const auto found = User(friend_ids[index]);
            out_list[index] = found ? friends::ToProfileExtraImplV2(*found)
                                    : friends::ProfileExtraImplV2{};
        }

        Warm(friend_ids);
        R_SUCCEED();
    }

    Result GetRelationship(Out<friends::Relationship> out_relationship, Uid user,
                           NetworkServiceAccountId friend_id) {
        *out_relationship = {};
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        // Answered from the caches the server already filled rather than with a round trip: the
        // friend list, the block list, and the sent-request box (§A.7).
        const auto blocks = baas::Blocks();
        const auto outbox = baas::OutboxRequests();

        out_relationship->is_friend = baas::FindFriend(friend_id).has_value() ? 1 : 0;
        out_relationship->is_blocking =
            std::any_of(blocks->begin(), blocks->end(),
                        [friend_id](const baas::Block& block) { return block.id == friend_id; })
                ? 1
                : 0;
        out_relationship->is_request_sent =
            std::any_of(outbox->begin(), outbox->end(),
                        [friend_id](const baas::Request& request) {
                            return request.other_id == friend_id;
                        })
                ? 1
                : 0;
        R_SUCCEED();
    }

    Result GetUserPresenceView(
        OutLargeData<friends::UserPresenceViewImpl, BufferAttr_HipcPointer> out_view, Uid user) {
        *out_view = {};
        R_TRY(RequireViewer());

        if (AvailableFor(user)) {
            *out_view = PresenceState::Instance().OwnPresenceView(KeyOf(user));
        }
        R_SUCCEED();
    }

    Result GetPlayHistoryList(Out<s32> out_count) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_count = 0;
        R_SUCCEED();
    }

    Result GetPlayHistoryStatistics(Out<friends::PlayHistoryStatistics> out_statistics) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_statistics = {};
        R_SUCCEED();
    }

    Result LoadUserSetting(
        OutLargeData<friends::UserSettingImpl, BufferAttr_HipcPointer> out_setting, Uid user) {
        *out_setting = {};
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            *out_setting = OfflineUserSetting(user);
            R_SUCCEED();
        }

        if (const auto setting = baas::OwnUserSetting()) {
            *out_setting = friends::ToUserSettingImpl(*setting, user);
        }
        R_SUCCEED();
    }

    Result LoadUserSettingV2(
        OutLargeData<friends::UserSettingImplV2, BufferAttr_HipcPointer> out_setting, Uid user) {
        *out_setting = {};
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            // The V2 shape is the same up to the play log, so the offline answer is too.
            const friends::UserSettingImpl offline = OfflineUserSetting(user);
            static_assert(sizeof(offline) == sizeof(friends::UserSettingImplV2));
            std::memcpy(out_setting.Get(), &offline, sizeof(offline));
            R_SUCCEED();
        }

        if (const auto setting = baas::OwnUserSetting()) {
            *out_setting = friends::ToUserSettingImplV2(*setting, user);
        }
        R_SUCCEED();
    }

    Result SyncUserSetting(Uid user) {
        R_TRY(RequireViewer());

        if (AvailableFor(user)) {
            baas::RunInBackground([] { baas::SyncUserSetting(); });
        }
        R_SUCCEED();
    }

    Result RequestListSummaryOverlayNotification() {
        LOG_INFO(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result GetExternalApplicationCatalog(
        OutLargeData<friends::ExternalApplicationCatalog, BufferAttr_HipcPointer> out_catalog) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_catalog = {};
        R_SUCCEED();
    }

    // ---- 22xxx: invitations ----

    Result GetReceivedFriendInvitationList(
        Out<s32> out_count,
        OutArray<friends::FriendInvitationForViewerImpl, BufferAttr_HipcMapAlias> out_list,
        Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        // Read and unread both: the inbox request the module makes carries no read filter, and
        // each item says which it is.
        s32 count = 0;
        for (const baas::Invitation& invitation : *baas::Invitations()) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = friends::ToInvitationImpl(invitation);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationListV2(
        Out<s32> out_count,
        OutArray<friends::FriendInvitationForViewerImplV2, BufferAttr_HipcMapAlias> out_list,
        Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        s32 count = 0;
        for (const baas::Invitation& invitation : *baas::Invitations()) {
            if (static_cast<std::size_t>(count) == out_list.size()) {
                break;
            }
            out_list[count++] = friends::ToInvitationImplV2(invitation);
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationDetailedInfo(
        OutLargeData<friends::FriendInvitationGroupImpl, BufferAttr_HipcMapAlias> out_group,
        Uid user, u64 group_id) {
        *out_group = {};
        R_TRY(RequireViewer());

        const auto group = AvailableFor(user) ? baas::CachedInvitationGroup(group_id)
                                              : std::nullopt;
        R_UNLESS(group.has_value(), FromDescription(baas::InvalidArgument));

        *out_group = friends::ToInvitationGroupImpl(*group);
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationDetailedInfoV2(
        OutLargeData<friends::FriendInvitationGroupImplV2, BufferAttr_HipcMapAlias> out_group,
        Uid user, u64 group_id) {
        *out_group = {};
        R_TRY(RequireViewer());

        const auto group = AvailableFor(user) ? baas::CachedInvitationGroup(group_id)
                                              : std::nullopt;
        R_UNLESS(group.has_value(), FromDescription(baas::InvalidArgument));

        *out_group = friends::ToInvitationGroupImplV2(*group);
        R_SUCCEED();
    }

    Result GetReceivedFriendInvitationCountCache(Out<s32> out_count, Uid user) {
        *out_count = 0;
        R_TRY(RequireViewer());

        // What the native inbox held at the last poll, dismissals removed: the count a title's
        // "you have been invited" badge is answered with.
        *out_count = AvailableFor(user)
                         ? static_cast<s32>(openpak::client::session::Invitations().size())
                         : 0;
        R_SUCCEED();
    }

    // ---- 30xxx: manager ports ----

    Result DropFriendNewlyFlags(Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            for (const baas::Friend& one : *baas::Friends()) {
                if (one.is_newly) {
                    DropNewly(one.id);
                }
            }
        }
        R_SUCCEED();
    }

    Result DeleteFriend(Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        // Fire and forget: the DELETE is a network call and this is the game's thread. The
        // friendship disappears from the cache because the sync that follows it no longer
        // carries it, which is how the console loses it too (§A.3).
        if (AvailableFor(user)) {
            baas::RunInBackground([friend_id] { baas::DeleteFriend(friend_id); });
        }
        R_SUCCEED();
    }

    Result DropFriendNewlyFlag(Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            DropNewly(friend_id);
        }
        R_SUCCEED();
    }

    Result ChangeFriendFavoriteFlag(bool favorite, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            baas::Update(friend_id, [favorite](baas::Friend& one) { one.is_favorite = favorite; });
            baas::RunInBackground([friend_id, favorite] {
                baas::PatchFriend(friend_id, "replace", "/isFavorite", favorite);
            });
        }
        R_SUCCEED();
    }

    Result ChangeFriendOnlineNotificationFlag(bool notify, Uid user,
                                              NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            baas::Update(friend_id,
                         [notify](baas::Friend& one) { one.is_online_notification = notify; });
            baas::RunInBackground([friend_id, notify] {
                baas::PatchFriend(friend_id, "add", "/extras/self/isOnlineNotification", notify);
            });
        }
        R_SUCCEED();
    }

    Result ChangeFriendNote(friends::FriendNote note, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            std::string text = friends::Text(std::span<const char>{note.note});
            baas::Update(friend_id, [text](baas::Friend& one) { one.note = text; });
            baas::RunInBackground([friend_id, text = std::move(text)] {
                baas::PatchFriend(friend_id, "replace", "/friendNote", text);
            });
        }
        R_SUCCEED();
    }

    // Nothing here keeps a queue of pending friend changes: every 301xx write goes out the moment
    // it is made. So a flush has nothing to flush, and says so by succeeding. 30131 is the same
    // command without the port check.
    Result SendPendingFriendChange() {
        R_RETURN(RequireManager());
    }

    Result SendPendingFriendChangeUnchecked() {
        R_SUCCEED();
    }

    Result SendFriendRequest(s32 channel, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}, channel={}",
                        KeyOf(user), friend_id, channel);
            R_SUCCEED();
        }

        baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result SendFriendRequestWithApplicationInfo(
        s32 channel, Uid user, NetworkServiceAccountId friend_id,
        friends::ApplicationInfo application_info,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> own_name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}, channel={}",
                        KeyOf(user), friend_id, channel);
            R_SUCCEED();
        }

        // Screen name #1 is the target's name as the sender saw it, #2 the sender's own in-app
        // name -- the buffers arrive in that order.
        baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.application_id = application_info.application_id;
        send.presence_group_id = application_info.presence_group_id;
        std::tie(send.target_name, send.target_language) = friends::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = friends::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    /// Answer or withdraw a request: fire and forget, the PATCH is a network call and this is the
    /// game's thread.
    Result AnswerFriendRequest(Uid user, u64 request_id, const char* state) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, request_id={:016x}",
                        KeyOf(user), request_id);
            R_SUCCEED();
        }

        baas::RunInBackground([request_id, answer = std::string{state}] {
            baas::AnswerFriendRequest(request_id, answer);
        });
        R_SUCCEED();
    }

    Result CancelFriendRequest(Uid user, u64 request_id) {
        R_RETURN(AnswerFriendRequest(user, request_id, "CANCELED"));
    }

    Result AcceptFriendRequest(Uid user, u64 request_id) {
        R_RETURN(AnswerFriendRequest(user, request_id, "AUTHORIZED"));
    }

    Result RejectFriendRequest(Uid user, u64 request_id) {
        R_RETURN(AnswerFriendRequest(user, request_id, "REJECTED"));
    }

    Result ReadFriendRequest(Uid user, u64 request_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, request_id={:016x}",
                        KeyOf(user), request_id);
            R_SUCCEED();
        }

        baas::RunInBackground([request_id] { baas::ReadFriendRequest(request_id); });
        R_SUCCEED();
    }

    Result GetFacedFriendRequestRegistrationKey(
        Out<friends::FacedFriendRequestRegistrationKey> out_key) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_key = {};
        R_SUCCEED();
    }

    Result AddFacedFriendRequest() {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result CancelFacedFriendRequest(Uid user, NetworkServiceAccountId friend_id) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}", KeyOf(user),
                    friend_id);
        R_SUCCEED();
    }

    Result GetFacedFriendRequestProfileImage(Out<s32> out_size) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_size = 0;
        R_SUCCEED();
    }

    Result GetFacedFriendRequestProfileImageFromPath(Out<s32> out_size) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_size = 0;
        R_SUCCEED();
    }

    Result SendFriendRequestWithExternalApplicationCatalogId(
        s32 channel, Uid user, NetworkServiceAccountId friend_id,
        friends::ExternalApplicationCatalogId catalog_id,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> own_name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}, channel={}",
                        KeyOf(user), friend_id, channel);
            R_SUCCEED();
        }

        baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.catalog_id = fmt::format("{:016x}{:016x}", catalog_id.high, catalog_id.low);
        std::tie(send.target_name, send.target_language) = friends::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = friends::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result SendFriendRequestWithApplicationInfoV2(
        s32 channel, Uid user, NetworkServiceAccountId friend_id,
        friends::ApplicationInfoV2 application_info,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> target_name,
        InLargeData<friends::InAppScreenName, BufferAttr_HipcPointer> own_name) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}, channel={}",
                        KeyOf(user), friend_id, channel);
            R_SUCCEED();
        }

        // The V2 send is the app route with the acd index the caller named (§A.4.1).
        baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.application_id = application_info.application_id;
        send.acd_index = application_info.acd_index;
        send.presence_group_id = application_info.presence_group_id;
        std::tie(send.target_name, send.target_language) = friends::ScreenName(*target_name);
        std::tie(send.own_name, send.own_language) = friends::ScreenName(*own_name);

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result ResendFacedFriendRequest(Uid user, NetworkServiceAccountId friend_id) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}", KeyOf(user),
                    friend_id);
        R_SUCCEED();
    }

    Result SendFriendRequestWithNintendoNetworkIdInfo(
        friends::MiiName own_mii_name, friends::MiiImageUrlParam own_mii_image_url_param,
        friends::MiiName other_mii_name, friends::MiiImageUrlParam other_mii_image_url_param,
        s32 channel, Uid user, NetworkServiceAccountId friend_id) {
        R_TRY(RequireManager());

        if (!AvailableFor(user)) {
            LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}, channel={}",
                        KeyOf(user), friend_id, channel);
            R_SUCCEED();
        }

        // The NNID route carries the sender's own Mii name and image parameter -- block A of the
        // pair -- in both extras halves (§A.4.1).
        baas::FriendRequestSend send;
        send.target_id = friend_id;
        send.channel = RequestChannel(channel);
        send.mii_name = friends::Text(std::span<const char>{own_mii_name.name});
        send.mii_image_url_param = friends::Text(std::span<const char>{own_mii_image_url_param.param});

        SendFriendRequestInBackground(std::move(send));
        R_SUCCEED();
    }

    Result GetSnsAccountLinkPageUrl(
        OutLargeData<friends::WebPageUrl, BufferAttr_HipcMapAlias> out_url) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_url = {};
        R_SUCCEED();
    }

    Result UnlinkSnsAccount() {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result BlockUser() {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result BlockUserWithApplicationInfo() {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        R_SUCCEED();
    }

    Result UnblockUser(Uid user, NetworkServiceAccountId friend_id) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}, friend_id={:016x}", KeyOf(user),
                    friend_id);
        R_SUCCEED();
    }

    Result GetProfileExtraFromFriendCode(
        OutLargeData<friends::ProfileExtraImpl, BufferAttr_HipcPointer> out_profile) {
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        *out_profile = {};
        R_SUCCEED();
    }

    Result DeletePlayHistory(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
        R_SUCCEED();
    }

    Result ChangePresencePermission(s32 permission_value, Uid user) {
        R_TRY(RequireManager());

        const char* value = nullptr;
        switch (permission_value) {
        case 0:
            value = "SELF";
            break;
        case 1:
            value = "FAVORITE_FRIENDS";
            break;
        case 2:
            value = "FRIENDS";
            break;
        default:
            R_THROW(ResultInvalidArgument);
        }

        if (AvailableFor(user)) {
            baas::RunInBackground([body = baas::UserPatchBody("/permissions/presence", value)] {
                baas::PatchUser(body);
            });
        }
        R_SUCCEED();
    }

    Result ChangeFriendRequestReception(bool reception, Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            baas::RunInBackground(
                [body = baas::UserPatchBody("/permissions/friendRequestReception", reception)] {
                    baas::PatchUser(body);
                });
        }
        R_SUCCEED();
    }

    Result ChangePlayLogPermission(s32 permission_value, Uid user) {
        R_TRY(RequireManager());
        R_UNLESS(permission_value == 1 || permission_value == 2 || permission_value == 3 ||
                     permission_value == 5,
                 ResultInvalidArgument);

        if (AvailableFor(user)) {
            // The chosen group keeps the log the server already holds; the other three are
            // emptied, which is what makes the group the permission (§A.6).
            const auto setting = baas::OwnUserSetting();
            const std::string play_log =
                setting && !setting->play_log_text.empty() ? setting->play_log_text : "[]";
            baas::RunInBackground(
                [body = baas::PlayLogPermissionBody(permission_value, play_log)] {
                    baas::PatchUser(body);
                });
        }
        R_SUCCEED();
    }

    Result IssueFriendCode(Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            baas::RunInBackground([] { baas::IssueFriendCode(); });
        }
        R_SUCCEED();
    }

    Result ClearPlayLog(Uid user) {
        R_TRY(RequireManager());

        if (AvailableFor(user)) {
            // The same four ops with an empty log in the group that carries the permission.
            const auto setting = baas::OwnUserSetting();
            baas::RunInBackground(
                [body = baas::PlayLogPermissionBody(setting ? setting->play_log_permission : 0,
                                                    "[]")] { baas::PatchUser(body); });
        }
        R_SUCCEED();
    }

    // 30900 / 30901 are written by hand: their in-buffers are one pointer (X) and two map-alias
    // (A) descriptors, and the generated serializer counts in-buffers across both kinds, which
    // would read the description from the wrong descriptor.
    //
    // Raw data, sorted by alignment: the bool at +0, the Uid at +8, the ApplicationInfo at +0x18.

    void SendFriendInvitation(HLERequestContext& ctx) {
        const u8* raw = RawData(ctx);

        Uid user{};
        friends::ApplicationInfo info{};
        std::memcpy(&user, raw + 0x08, sizeof(user));
        std::memcpy(&info, raw + 0x18, sizeof(info));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(SendInvitation(ctx, user, info.application_id, 0, info.presence_group_id,
                               raw[0] != 0));
    }

    void SendFriendInvitationV2(HLERequestContext& ctx) {
        const u8* raw = RawData(ctx);

        Uid user{};
        friends::ApplicationInfoV2 info{};
        std::memcpy(&user, raw + 0x08, sizeof(user));
        std::memcpy(&info, raw + 0x18, sizeof(info));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(SendInvitation(ctx, user, info.application_id, info.acd_index,
                               info.presence_group_id, raw[0] != 0));
    }

    static const u8* RawData(HLERequestContext& ctx) {
        return reinterpret_cast<const u8*>(ctx.CommandBuffer() + ctx.GetDataPayloadOffset() + 2);
    }

    /// POST /v2/invitation_groups (invitations doc §3c). The ids are the friends' own BAAS user
    /// ids, which is what the guest's friend list serves, so they go out as they arrive.
    Result SendInvitation(HLERequestContext& ctx, const Uid& user, u64 application_id,
                          u8 acd_index, u64 presence_group_id, bool application_id_match) {
        R_TRY(RequireManager());

        std::vector<u64> friend_ids;
        if (ctx.CanReadBuffer(0)) {
            const auto ids = ctx.ReadBufferX(0);
            friend_ids.resize(ids.size() / sizeof(u64));
            std::memcpy(friend_ids.data(), ids.data(), friend_ids.size() * sizeof(u64));
        }

        friends::FriendInvitationGameModeDescription description{};
        const auto description_bytes = ctx.ReadBufferA(0);
        std::memcpy(&description, description_bytes.data(),
                    (std::min)(description_bytes.size(), sizeof(description)));

        const auto application_data = ctx.ReadBufferA(1);

        // The module refuses anything outside 1..16 receivers or 0x400 bytes of data.
        R_UNLESS(!friend_ids.empty() && friend_ids.size() <= friends::kMaxReceivers &&
                     application_data.size() <= friends::kApplicationDataSize,
                 ResultInvalidArgument);

        if (!AvailableFor(user)) {
            R_SUCCEED();
        }

        std::vector<std::string> receivers;
        receivers.reserve(friend_ids.size());
        for (const u64 id : friend_ids) {
            receivers.push_back(fmt::format("{:016x}", id));
        }

        // Fire and forget: the POST is a network call and this is the game's thread.
        baas::RunInBackground([receivers = std::move(receivers), application_id, acd_index,
                               presence_group_id,
                               data = std::vector<u8>(application_data.begin(),
                                                      application_data.end()),
                               messages = friends::InvitationMessages(description),
                               application_id_match] {
            baas::SendInvitation(receivers, application_id, acd_index, presence_group_id, data,
                                 messages, application_id_match);
        });
        R_SUCCEED();
    }

    Result ReadFriendInvitation(Uid user,
                                InArray<u64, BufferAttr_HipcPointer> invitation_ids) {
        R_TRY(RequireManager());

        // Fire and forget: the mark-read is a network call and this is the game's thread. It is
        // the same dismissal the host's own invitation list makes, so both agree on what waits.
        std::vector<u64> ids(invitation_ids.begin(), invitation_ids.end());
        baas::RunInBackground([ids = std::move(ids)] { ReadNativeInvitations(ids); });
        R_SUCCEED();
    }

    Result ReadAllFriendInvitations(Uid user) {
        R_TRY(RequireManager());

        baas::RunInBackground([] { ReadNativeInvitations({}); });
        R_SUCCEED();
    }

    /// The guest read some invitations (or all of them, for an empty list).
    static void ReadNativeInvitations(const std::vector<u64>& ids) {
        for (const auto& invitation : openpak::client::session::Invitations()) {
            const u64 id = std::strtoull(invitation.id.c_str(), nullptr, 10);
            if (ids.empty() || std::find(ids.begin(), ids.end(), id) != ids.end()) {
                openpak::client::session::DismissInvitation(invitation.id);
            }
        }
    }

    // ---- 4xxxx: system ports ----

    Result DeleteFriendListCache(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
        R_RETURN(RequireSystem());
    }

    Result DeleteBlockedUserListCache(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
        R_RETURN(RequireSystem());
    }

    Result DeleteNetworkServiceAccountCache(Uid user) {
        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", KeyOf(user));
        R_RETURN(RequireSystem());
    }

    const u32 permission;
    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;
};

} // namespace

std::shared_ptr<ServiceFrameworkBase> CreateFriendService(Core::System& system,
                                                           std::string_view port_name) {
    return std::make_shared<OpenPakFriendService>(system, PermissionOf(port_name));
}

std::shared_ptr<ServiceFrameworkBase> CreateNotificationService(Core::System& system,
                                                               std::string_view port_name,
                                                               const Common::UUID& user) {
    return std::make_shared<OpenPakNotificationService>(system, HandlerForThisSession(), user,
                                                        PermissionOf(port_name));
}

void ApplicationStarted(u64 program_id, const FileSys::NACP* nacp) {
    // The group a friend's game compares against is the NACP's; a title that declares none is its
    // own group, which the client falls back to when this is 0.
    u64 presence_group_id = 0;
    if (nacp != nullptr) {
        const std::vector<u8> raw = nacp->GetRawBytes();
        constexpr std::size_t offset = offsetof(FileSys::RawNACP, presence_group_id);
        if (raw.size() >= offset + sizeof(presence_group_id)) {
            std::memcpy(&presence_group_id, raw.data() + offset, sizeof(presence_group_id));
        }
    }

    openpak::client::session::SetRunningApplication(program_id, presence_group_id);
}

void ApplicationStopped() {
    openpak::client::session::SetRunningApplication(0, 0);
}

} // namespace Service::Friend::OpenPak
