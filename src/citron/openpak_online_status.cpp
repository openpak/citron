// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCoreApplication>

#include "citron/openpak_online_status.h"
#include "common/settings.h"
#include "openpak/compatibility.h"

std::optional<OpenPakOnlineStatus> OpenPakOnlineStatusFor(u64 program_id) {
    if (!Settings::values.enable_openpak.GetValue()) {
        return std::nullopt;
    }
    const auto entry = openpak::compatibility::Find(program_id);
    if (!entry) {
        return std::nullopt;
    }

    struct Look {
        QColor color;
        const char* text;
        const char* tooltip;
    };
    // The colours and words of the reference's column.
    // clang-format off
    static const Look looks[] = {
        {QColor(0x47, 0xd3, 0x5c), QT_TRANSLATE_NOOP("OpenPakOnlineStatus", "Live"),
         QT_TRANSLATE_NOOP("OpenPakOnlineStatus", "Online play works on OpenPak.")},
        {QColor(0xf2, 0xd6, 0x24), QT_TRANSLATE_NOOP("OpenPakOnlineStatus", "Beta"),
         QT_TRANSLATE_NOOP("OpenPakOnlineStatus", "Online play works on OpenPak, with known gaps.")},
        {QColor(0xe8, 0x84, 0x3c), QT_TRANSLATE_NOOP("OpenPakOnlineStatus", "Alpha"),
         QT_TRANSLATE_NOOP("OpenPakOnlineStatus", "Online play is being brought up on OpenPak; expect it not to work yet.")},
    };
    // clang-format on
    const Look& look = looks[static_cast<std::size_t>(entry->status)];

    const QString tooltip = QCoreApplication::translate("OpenPakOnlineStatus", look.tooltip);
    return OpenPakOnlineStatus{
        .label = QCoreApplication::translate("OpenPakOnlineStatus", look.text),
        .tooltip = entry->backend.empty()
                       ? tooltip
                       : QStringLiteral("%1 (%2)").arg(tooltip,
                                                       QString::fromStdString(entry->backend)),
        .color = look.color,
    };
}
