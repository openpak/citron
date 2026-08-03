// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "common/common_types.h"

namespace Common::OpenPakAccount {

// The linked OpenPak Network account. Written by the login dialog, read by the acc service so the
// NEX login presents the account's persistent principal id. Stored as key=value, not JSON, so it
// stays readable if a field is added later.

bool IsLinked();
u64 GetPid();
std::string GetUsername();
std::string GetFriendCode();
std::string GetToken();

void Save(u64 pid, std::string_view username, std::string_view friend_code,
          std::string_view token);
void Clear();

} // namespace Common::OpenPakAccount
