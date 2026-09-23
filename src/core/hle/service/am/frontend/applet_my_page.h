// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "core/hle/service/am/frontend/applets.h"

namespace Service::AM::Frontend {

// [OpenPak] The friends applet (MyPage) as far as a game drives it: sending an invitation, with
// the person picking friends (type 8) or the game naming them (type 9). The work is
// openpak::my_page, shared with every OpenPak emulator; this is only the shell that pops the
// argument and pushes the Result back. Ported from Ryujinx.
class MyPage final : public FrontendApplet {
public:
    explicit MyPage(Core::System& system_, std::shared_ptr<Applet> applet_,
                    LibraryAppletMode applet_mode_);
    ~MyPage() override;

    void Initialize() override;
    Result GetStatus() const override;
    void ExecuteInteractive() override;
    void Execute() override;
    Result RequestExit() override;

private:
    std::vector<u8> argument;
    bool started = false;
};

} // namespace Service::AM::Frontend
