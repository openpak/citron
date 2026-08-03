// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <QDialog>
#include <QTimer>

#include "common/common_types.h"

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;
class QStandardItemModel;
class QModelIndex;
class QTabWidget;
class OpenPakController;
class OpenPakFriendDelegate;
class ExternalReferenceProbe;

// Reachable from the OpenPak toolbar menu's "Open Account Page" entry.
class OpenPakAccountDialog : public QDialog {
    Q_OBJECT

public:
    explicit OpenPakAccountDialog(OpenPakController* controller, QWidget* parent = nullptr);
    ~OpenPakAccountDialog() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void RefreshFriends();
    void RefreshHistory();
    void SetBusy(bool busy);
    void OnAdd();

    void RunAsync(std::function<std::string()> task, std::function<void()> on_success = nullptr);

    void OnFriendsViewClicked(const QModelIndex& index);
    u64 SelectedPid(const QModelIndex& index) const;
    void ApplyFriendFilter(const QString& text);
    void UpdateRequestsBadge(int count);
    void OnChangeAvatar();

    OpenPakController* controller;

    QLabel* header_avatar;
    QLabel* header_name;
    QLabel* header_code;
    QLabel* status;

    QListView* friends_view;
    QStandardItemModel* friends_model;
    QStackedWidget* friends_stack;
    QListView* requests_view;
    QStandardItemModel* requests_model;
    QStackedWidget* requests_stack;
    QListView* history_view;
    QStandardItemModel* history_model;
    QStackedWidget* history_stack;
    OpenPakFriendDelegate* friend_delegate;
    OpenPakFriendDelegate* request_delegate;

    QLineEdit* friend_code_input;
    QPushButton* add_button;

    QLineEdit* friend_search;
    QTimer refresh_timer;

    ExternalReferenceProbe* network_probe;
    QLabel* nat_label;
    QLabel* ping_label;

    QTabWidget* tabs;
    QLabel* requests_badge;
};
