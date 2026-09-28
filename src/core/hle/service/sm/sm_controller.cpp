// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include "common/assert.h"
#include "common/alignment.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sm/sm_controller.h"

namespace Service::SM {

void Controller::ConvertCurrentObjectToDomain(HLERequestContext& ctx) {
    ASSERT_MSG(!ctx.GetManager()->IsDomain(), "Session is already a domain");
    LOG_DEBUG(Service, "called, server_session={}", ctx.Session()->GetId());
    ctx.GetManager()->ConvertToDomainOnRequestEnd();

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(1); // Converted sessions start with 1 request handler
}

void Controller::CloneCurrentObject(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    auto session_manager = ctx.GetManager();

    // FIXME: this is duplicated from the SVC, it should just call it instead
    // once this is a proper process

    // Reserve a new session from the process resource limit.
    Kernel::KScopedResourceReservation session_reservation(
        Kernel::GetCurrentProcessPointer(kernel), Kernel::LimitableResource::SessionCountMax);
    ASSERT(session_reservation.Succeeded());

    // Create the session.
    Kernel::KSession* session = Kernel::KSession::Create(kernel);
    ASSERT(session != nullptr);

    // Initialize the session.
    session->Initialize(nullptr, 0);

    // Commit the session reservation.
    session_reservation.Commit();

    // Register the session.
    Kernel::KSession::Register(kernel, session);

    // Register with server manager.
    session_manager->GetServerManager().RegisterSession(&session->GetServerSession(),
                                                        session_manager);

    // We succeeded.
    IPC::ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
    rb.Push(ResultSuccess);
    rb.PushMoveObjects(session->GetClientSession());
}

void Controller::CloneCurrentObjectEx(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    CloneCurrentObject(ctx);
}

void Controller::QueryPointerBufferSize(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    // [OpenPak] The previous computed formula here (descriptor-size accounting rounded up to a
    // 4KB boundary) resolves to 0x1000 -- a fraction of what real hardware and every other working
    // implementation actually advertise. A client opening many concurrent HIPC sessions/streams
    // (matching Ryujinx-Reference's own finding: an NPLN gRPC client's presence/friends/catalog/
    // lobby streams all carry pointer buffers at once) overflows a buffer this small; the kernel's
    // KServerSession then fails the copy with OutOfResource, and the guest's own SendSyncRequest
    // on its network thread fails outright -- observed as the game aborting entering online play.
    // 0xF000 matches Ryujinx-Reference's own fix for exactly this (515040a) -- the maximum a 16-bit
    // HIPC size field can express, comfortably covering even NPLN's heaviest concurrent-stream
    // case, verified safe for every other title's usage there too.
    // [OpenPak] 0xF000 is the floor, not the answer: a size the guest set itself wins (as Eden).
    auto* process = Kernel::GetCurrentProcessPointer(kernel);
    ASSERT(process != nullptr);

    u32 buffer_size = process->GetPointerBufferSize();
    if (!process->IsPointerBufferSizeSetByGuest() && buffer_size < 0xF000) {
        buffer_size = 0xF000;
    }
    if (buffer_size > std::numeric_limits<u16>::max()) {
        buffer_size = std::numeric_limits<u16>::max();
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u16>(static_cast<u16>(buffer_size));
}

// [OpenPak] The guest sets its own pointer buffer size, kept per process (as Eden).
void Controller::SetPointerBufferSize(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "called");

    auto* process = Kernel::GetCurrentProcessPointer(kernel);
    ASSERT(process != nullptr);

    IPC::RequestParser rp{ctx};
    u32 requested_size = rp.PopRaw<u32>();
    if (requested_size > std::numeric_limits<u16>::max()) {
        requested_size = std::numeric_limits<u16>::max();
    }

    process->SetPointerBufferSizeByGuest(requested_size);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// https://switchbrew.org/wiki/IPC_Marshalling
Controller::Controller(Core::System& system_) : ServiceFramework{system_, "IpcController"} {
    static const FunctionInfo functions[] = {
        {0, &Controller::ConvertCurrentObjectToDomain, "ConvertCurrentObjectToDomain"},
        {1, nullptr, "CopyFromCurrentDomain"},
        {2, &Controller::CloneCurrentObject, "CloneCurrentObject"},
        {3, &Controller::QueryPointerBufferSize, "QueryPointerBufferSize"},
        {4, &Controller::CloneCurrentObjectEx, "CloneCurrentObjectEx"},
        {5, &Controller::SetPointerBufferSize, "SetPointerBufferSize"},
    };
    RegisterHandlers(functions);
}

Controller::~Controller() = default;

} // namespace Service::SM
