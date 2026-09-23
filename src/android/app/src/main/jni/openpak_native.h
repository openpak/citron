// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

/// What is being played, pushed in from the emulation thread.
///
/// Presence has to name the running title, and the only safe place to read that is where the
/// session is known to be alive. A timer thread asking Core::System on its own schedule races
/// the session's own creation and teardown, which on Android is a process that disappears.
/// Zero means the game list: online, playing nothing.
void OpenPakSetRunningTitle(u64 program_id);

/// Cloud saves as the desktop build does them: the newest cloud copy before the title reads the
/// one on disk (blocking, a few seconds at most), and the local copy up once it has stopped (the
/// capture here, the upload on its own thread). Both do nothing without a signed-in account.
void OpenPakPullSaveBeforeLaunch(u64 title_id);
void OpenPakPushSaveAfterExit(u64 title_id);
