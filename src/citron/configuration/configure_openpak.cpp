// SPDX-FileCopyrightText: Copyright 2026 OpenPak contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/settings.h"
#include "core/core.h"
#include "openpak/qt/settings_section.h"
#include "citron/configuration/configure_openpak.h"
#include "citron/openpak_host.h"

ConfigureOpenPak::ConfigureOpenPak(const Core::System& system_, QWidget* parent)
    : QWidget(parent), system{system_} {
    setAccessibleName(tr("OpenPak"));
    auto* host = static_cast<OpenPakHost*>(openpak::qt::Host::Current());
    openpak::qt::SettingsHooks hooks = host ? host->MakeSettingsHooks() : openpak::qt::SettingsHooks{};
    hooks.server_fields = MakeServerFields();
    section = new openpak::qt::SettingsSection(host, std::move(hooks), this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(section);
}

ConfigureOpenPak::~ConfigureOpenPak() = default;

QWidget* ConfigureOpenPak::MakeServerFields() {
    // Where the redirected consoles connect: the OpenPak server and the second NAT responder.
    // Hidden like a password until asked for, and fixed while a game runs.
    auto* fields = new QWidget;
    auto* grid = new QGridLayout(fields);
    grid->setContentsMargins(0, 0, 0, 0);
    const bool editable = !system.IsPoweredOn();
    const auto add = [&](int row, const QString& label, QLineEdit*& edit,
                         const Settings::Setting<std::string>& setting) {
        edit = new QLineEdit(QString::fromStdString(setting.GetValue()));
        edit->setEchoMode(QLineEdit::Password);
        edit->setEnabled(editable);
        auto* show = new QPushButton(tr("Unhide"));
        show->setCheckable(true);
        auto* reset = new QPushButton(tr("Restore Default"));
        reset->setEnabled(editable);
        QLineEdit* field = edit;
        connect(show, &QPushButton::toggled, this, [field, show](bool on) {
            field->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
            show->setText(on ? tr("Hide") : tr("Unhide"));
        });
        const QString fallback = QString::fromStdString(setting.GetDefault());
        connect(reset, &QPushButton::clicked, this, [field, fallback] { field->setText(fallback); });
        grid->addWidget(new QLabel(label), row, 0);
        grid->addWidget(edit, row, 1);
        grid->addWidget(show, row, 2);
        grid->addWidget(reset, row, 3);
    };
    add(0, tr("Server IP"), server_ip, Settings::values.openpak_server_ip);
    add(1, tr("NAT IP"), nat_ip, Settings::values.openpak_nat_ip);
    return fields;
}

void ConfigureOpenPak::ApplyConfiguration() {
    section->Apply();
    if (!system.IsPoweredOn()) {
        Settings::values.openpak_server_ip = server_ip->text().trimmed().toStdString();
        Settings::values.openpak_nat_ip = nat_ip->text().trimmed().toStdString();
    }
}
