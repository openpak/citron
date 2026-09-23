// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// [OpenPak] The friends sysmodule's IFriendService and INotificationService on OpenPak's friend
// graph, ported command by command from the Ryujinx reference
// (Ryujinx.Horizon/Sdk/Friends/Detail/Ipc/FriendService.cs, NotificationService.cs and
// NotificationEventHandler.cs). The data, the structs and the REST calls are the shared
// openpak-client library's; this is only the IPC glue.
//
// Only the active profile, signed in, gets OpenPak's data. Any other user a title names, and
// every user while nothing is signed in, gets the stubs friend.cpp always answered with -- a
// console that is simply not online.

#pragma once

#include <memory>
#include <string_view>

#include "common/common_types.h"
#include "common/uuid.h"
#include "core/hle/result.h"

namespace Core {
class System;
}

namespace FileSys {
class NACP;
}

namespace Service {
class ServiceFrameworkBase;
}

namespace Service::Friend::OpenPak {

/// 2121-0002, the module's bad argument.
constexpr Result ResultInvalidArgument{ErrorModule::Friends, 2};

/// The IFriendService a port hands out; the port's name picks its permission bits.
std::shared_ptr<ServiceFrameworkBase> CreateFriendService(Core::System& system,
                                                         std::string_view port_name);

/// The INotificationService a port hands out for one user.
std::shared_ptr<ServiceFrameworkBase> CreateNotificationService(Core::System& system,
                                                               std::string_view port_name,
                                                               const Common::UUID& user);

/// A title started: presence publishes it, with the presence group its NACP declares (the title
/// id stands in when there is none, or no NACP at all).
void ApplicationStarted(u64 program_id, const FileSys::NACP* nacp);

/// Nothing runs any more: presence goes back to INACTIVE.
void ApplicationStopped();

} // namespace Service::Friend::OpenPak
