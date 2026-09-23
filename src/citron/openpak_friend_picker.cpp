// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdint>
#include <vector>

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "openpak/baas.h"
#include "openpak/my_page.h"
#include "citron/openpak_friend_picker.h"

namespace {

QString Tr(const char* text) {
    return QCoreApplication::translate("OpenPakFriendPicker", text);
}

std::vector<std::uint64_t> Pick(QWidget* parent, int max) {
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Invite friends"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(max == 1 ? Tr("Invite a friend to play:")
                                          : Tr("Invite up to %1 friends to play:").arg(max)));

    auto* list = new QListWidget;
    list->setSelectionMode(max == 1 ? QAbstractItemView::SingleSelection
                                    : QAbstractItemView::MultiSelection);
    layout->addWidget(list);

    // Online friends first: they are the ones an invitation reaches now.
    auto friends = *openpak::baas::Friends();
    std::stable_sort(friends.begin(), friends.end(),
                     [](const auto& a, const auto& b) { return a.state > b.state; });
    for (const auto& f : friends) {
        const QString status = f.state == 2   ? Tr("playing")
                               : f.state == 1 ? Tr("online")
                                              : Tr("offline");
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 (%2)").arg(QString::fromStdString(f.nickname), status));
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(f.id));
        list->addItem(item);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton* invite = buttons->addButton(Tr("Invite"), QDialogButtonBox::AcceptRole);
    invite->setEnabled(false);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemSelectionChanged, &dialog, [list, invite, max] {
        // More than the game allows is refused here, not silently trimmed later.
        auto selected = list->selectedItems();
        while (selected.size() > max) {
            selected.back()->setSelected(false);
            selected.pop_back();
        }
        invite->setEnabled(!selected.isEmpty());
    });

    if (friends.empty()) {
        layout->insertWidget(1, new QLabel(Tr("You have no friends on OpenPak yet.")));
    }
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    std::vector<std::uint64_t> picked;
    for (const auto* item : list->selectedItems()) {
        picked.push_back(item->data(Qt::UserRole).toULongLong());
    }
    return picked;
}

} // namespace

void InstallOpenPakFriendPicker(QWidget* parent) {
    // Asked from the applet's worker thread; the dialog belongs on the UI thread, and the worker
    // waits for the answer.
    openpak::my_page::SetFriendPicker([window = QPointer<QWidget>(parent)](int max) {
        std::vector<std::uint64_t> picked;
        if (window) {
            QMetaObject::invokeMethod(
                window, [&] { picked = Pick(window, max); }, Qt::BlockingQueuedConnection);
        }
        return picked;
    });
}
