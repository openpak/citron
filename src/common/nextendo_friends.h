// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

// Snapshot of the account's friends, refreshed by the frontend and read by the friend service.
// The service must never make a network call from an IPC handler, so it reads this instead.
namespace Common::NextendoFriends {

struct Entry {
    u64 pid = 0;
    std::string name;
    s32 status = 0;        // 0 offline, 1 online, 2 in a game
    std::string app_field; // opaque per-title presence blob, raw bytes (already base64-decoded)
    std::vector<u8> image; // profile picture, raw JPEG bytes (already base64-decoded); empty if none
};

void Set(std::vector<Entry> entries);
std::vector<Entry> Get();

// [Nextendo] Get() never blocks -- essential for NEX titles, which poll this in a loop from
// their own online thread; blocking there would stall PRUDP acks and the server would declare a
// communication error. But Splatoon 3 requests its friend list exactly ONCE, early in boot
// (matches Ryujinx-Nextendo's own measurement: still empty at 39s in), and never asks again --
// if the frontend's background refresh hasn't populated the cache by then, that title is stuck
// with zero friends for its whole session. GetWarm() gives that one call a short, bounded wait
// for the cache to warm up instead. Never used by NEX titles' own repeated polling.
std::vector<Entry> GetWarm(int timeout_ms);

// This player's own presence, as last set by the running game. Pushed to the account server so
// friends see them online.
// nn::friends::PresenceStatus
enum : s32 {
    PresenceOffline = 0,
    PresenceOnline = 1,
    PresenceOnlinePlay = 2,
};

void SetLocalPresence(s32 status, std::string app_field);

// Sets only the status, keeping any app_field the running game published; a title's joinable-session
// blob must survive an emulator-driven status change.
void SetLocalStatus(s32 status);
s32 GetLocalStatus();
std::string GetLocalAppField();

// Hands out the local presence when it changed, or periodically regardless of change to keep the
// account server's presence TTL from expiring while the state is unchanged (e.g. idle in a hosted
// room). False means neither condition applies yet -- don't publish.
bool TakeLocalPresenceForPublish(s32& status, std::string& app_field);

// A real-time game-session invitation waiting for this account, relayed via the account
// server's own mailbox (see nextendo-account's invitations.go) -- not a friend, just an
// invite. app_param is the opaque, game-defined bytes the sender's own game constructed;
// never interpreted here, only carried through to TryPopFromFriendInvitationStorageChannel.
struct PendingInvitation {
    u64 from_pid = 0;
    std::string from_name;
    std::vector<u8> app_param;
};

// Same "frontend polls the network, service reads a cheap cache" split as Set()/Get() above,
// and for the same reason: TryPopFromFriendInvitationStorageChannel is called from the guest's
// own polling loop, dozens of times a second while a friend-invite screen is open -- an IPC
// handler must never itself block on a network round trip. Appends rather than replacing: the
// account server's own GET already pops (consumes) its queue on every poll, so an item this
// call adds must survive until PopPendingInvitation() actually delivers it to the guest, even
// across a poll that lands before that happens.
void SetPendingInvitations(std::vector<PendingInvitation> invitations);

// Pops (removes and returns) the oldest queued invitation, or nullopt if none are waiting --
// matches TryPopFromFriendInvitationStorageChannel's own single-item pop semantics.
std::optional<PendingInvitation> PopPendingInvitation();

// Real HOS semantics for this channel are event-driven: the guest calls
// GetFriendInvitationStorageChannelEvent once, then waits on that event rather than polling
// TryPopFromFriendInvitationStorageChannel in a tight loop. IApplicationFunctions registers a
// callback here (weak-bound to its own Applet) for the lifetime of the running application;
// SetPendingInvitations invokes it after adding new items so a guest genuinely blocked on the
// event wakes immediately, instead of depending on the game happening to poll again on its own
// schedule. No-op if nothing is registered (no application running).
void SetInvitationSignalCallback(std::function<void()> callback);

// Opaque join parameter supplied to the native MyPage invite applet by the running title.
// MyPage performs Nintendo's network send internally on hardware; Citron uses this cached copy
// to reproduce that side effect after the applet reports a successful selection.
void SetOutgoingInvitationParameter(std::vector<u8> app_param);
std::vector<u8> TakeOutgoingInvitationParameter();

} // namespace Common::NextendoFriends
