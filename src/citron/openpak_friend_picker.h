// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class QWidget;

// [OpenPak] The friend picker MyPage's "invite friends" opens (openpak::my_page::SetFriendPicker),
// as Ryujinx shows it: the account's friends, up to the number the game allows.
void InstallOpenPakFriendPicker(QWidget* parent);
