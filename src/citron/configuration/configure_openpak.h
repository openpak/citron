// SPDX-FileCopyrightText: Copyright 2026 OpenPak contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

class QLineEdit;

namespace Core {
class System;
}

namespace openpak::qt {
class SettingsSection;
}

// Configure -> OpenPak (emulators/prds/openpak-ux-spec.md §3.13): the library's settings section,
// the same in every emulator, with this emulator's console server fields under its Advanced part.
class ConfigureOpenPak : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureOpenPak(const Core::System& system, QWidget* parent = nullptr);
    ~ConfigureOpenPak() override;

    void ApplyConfiguration();

private:
    QWidget* MakeServerFields();

    const Core::System& system;
    openpak::qt::SettingsSection* section = nullptr;
    QLineEdit* server_ip = nullptr;
    QLineEdit* nat_ip = nullptr;
};
