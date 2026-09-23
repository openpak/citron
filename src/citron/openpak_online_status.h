// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include <QColor>
#include <QString>

#include "common/common_types.h"

// [OpenPak] How far OpenPak serves a title's online play, as the game list, the carousel and the
// details panel show it: the site's catalogue status (live, beta, alpha) from openpak::compatibility
// -- the list openpak-client ships, laid under what the site said at startup -- with the backend
// in the tooltip. Ported from the Online column Eden and Ryujinx show; Citron draws it in its own
// pills and badges instead of a column of its own, because its Online column already carries the
// LDN room counts.
struct OpenPakOnlineStatus {
    QString label;   // "Live", "Beta" or "Alpha"
    QString tooltip; // what the status means, and what serves the title
    QColor color;
};

// Nothing while OpenPak is off, or for a title OpenPak does not serve.
std::optional<OpenPakOnlineStatus> OpenPakOnlineStatusFor(u64 program_id);
