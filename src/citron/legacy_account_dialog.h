// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <QDialog>
#include <QTimer>

#include "common/common_types.h"

class QAbstractButton;
class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;
class QStandardItemModel;
class QModelIndex;
class QTabWidget;
class QToolButton;
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
    void OnEditUsername();

    void RefreshCloudSaveTab();
    void RebuildCloudSaveTitlePicker();
    void ProbeCloudSaveAvailability(u64 title_id);

    OpenPakController* controller;

    QLabel* header_avatar;
    QLabel* header_name;
    QToolButton* edit_name_button;
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

    QLabel* cloud_save_icon;
    QLabel* cloud_save_title;
    QLabel* cloud_save_status;
    QPushButton* cloud_save_download_button;
    QWidget* cloud_save_picker_container;
    QHBoxLayout* cloud_save_picker_row;
    QButtonGroup* cloud_save_picker_group;
    u64 cloud_save_selected_title_id = 0;
    std::unordered_map<u64, bool> cloud_save_has_data;
    std::unordered_set<u64> cloud_save_probing;
};
