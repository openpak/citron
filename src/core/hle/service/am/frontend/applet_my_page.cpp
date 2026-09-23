// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <cstring>
#include <thread>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/frontend/applet_my_page.h"
#include "core/hle/service/am/service/storage.h"
#include "openpak/my_page.h"

namespace Service::AM::Frontend {

namespace {

// The running title's presence group, which an invitation is addressed to; my_page falls back
// to the title id when the NACP names none.
u64 PresenceGroupOf(Core::System& system, u64 program_id) {
    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto nacp = pm.GetControlMetadata().first;
    if (nacp == nullptr) {
        return 0;
    }
    const auto raw = nacp->GetRawBytes();
    u64 group = 0;
    constexpr auto offset = offsetof(FileSys::RawNACP, presence_group_id);
    if (raw.size() >= offset + sizeof(group)) {
        std::memcpy(&group, raw.data() + offset, sizeof(group));
    }
    return group;
}

} // namespace

MyPage::MyPage(Core::System& system_, std::shared_ptr<Applet> applet_,
               LibraryAppletMode applet_mode_)
    : FrontendApplet{system_, applet_, applet_mode_} {}

MyPage::~MyPage() = default;

void MyPage::Initialize() {
    FrontendApplet::Initialize();
    started = false;
    if (const auto storage = PopInData()) {
        argument = storage->GetData();
    }
}

Result MyPage::GetStatus() const {
    R_SUCCEED();
}

void MyPage::ExecuteInteractive() {
    LOG_WARNING(Service_AM, "MyPage: unexpected interactive data");
}

void MyPage::Execute() {
    if (started) {
        return;
    }
    started = true;

    // A picker and a POST: off the service thread, which other applets and the game still need.
    // The strong reference keeps this applet, and so this object, alive until the Result is back.
    std::thread{[this, owner = applet.lock()] {
        if (!owner) {
            return;
        }
        const u64 program_id = system.GetApplicationProcessProgramID();
        const auto result =
            openpak::my_page::Run(argument, program_id, PresenceGroupOf(system, program_id));
        if (result) {
            std::vector<u8> out(sizeof(u32));
            std::memcpy(out.data(), &*result, sizeof(u32));
            PushOutData(std::make_shared<IStorage>(system, std::move(out)));
        }
        Exit();
    }}.detach();
}

Result MyPage::RequestExit() {
    R_SUCCEED();
}

} // namespace Service::AM::Frontend
