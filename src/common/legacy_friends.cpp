// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <mutex>

#include "common/legacy_friends.h"

namespace Common::OpenPakFriends {

namespace {
std::mutex g_mutex;
std::vector<Entry> g_entries;
s32 g_local_status = 0;
std::string g_local_app_field;
bool g_local_dirty = false;
std::chrono::steady_clock::time_point g_local_last_push{};
// Ryujinx-Reference re-publishes every 45s regardless of change, to stay under the account
// server's 90s presence TTL. An edge-triggered-only push lets an unchanging presence (e.g.
// sitting in a hosted room) silently expire server-side while still active.
constexpr auto kPresenceRefreshInterval = std::chrono::seconds{45};
} // Anonymous namespace

void Set(std::vector<Entry> entries) {
    std::lock_guard lock{g_mutex};
    g_entries = std::move(entries);
}

std::vector<Entry> Get() {
    std::lock_guard lock{g_mutex};
    return g_entries;
}

void SetLocalPresence(s32 status, std::string app_field) {
    std::lock_guard lock{g_mutex};
    if (g_local_status != status || g_local_app_field != app_field) {
        g_local_dirty = true;
    }
    g_local_status = status;
    g_local_app_field = std::move(app_field);
}

void SetLocalStatus(s32 status) {
    std::lock_guard lock{g_mutex};
    if (g_local_status != status) {
        g_local_status = status;
        g_local_dirty = true;
    }
}

s32 GetLocalStatus() {
    std::lock_guard lock{g_mutex};
    return g_local_status;
}

std::string GetLocalAppField() {
    std::lock_guard lock{g_mutex};
    return g_local_app_field;
}

bool TakeLocalPresenceForPublish(s32& status, std::string& app_field) {
    std::lock_guard lock{g_mutex};
    const auto now = std::chrono::steady_clock::now();
    if (!g_local_dirty && now - g_local_last_push < kPresenceRefreshInterval) {
        return false;
    }
    g_local_dirty = false;
    g_local_last_push = now;
    status = g_local_status;
    app_field = g_local_app_field;
    return true;
}

} // namespace Common::OpenPakFriends
