// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "common/common_types.h"

// Instruction changes a few game builds need before they will accept an OpenPak server.
//
// Some NPLN titles pin the certificate of the server they expect and check the peer's name
// themselves, so a server chaining to the OpenPak CA is refused whatever the console trusts. For
// those builds a handful of instructions are swapped while the module is loaded; the user's files
// are never touched.
//
// Same semantics as Ryujinx's OpenPakCodePatches (the reference implementation):
//   - only while OpenPak is enabled;
//   - only for the exact title id and main-module build id listed;
//   - every original instruction is checked first; if any one differs the whole entry is skipped
//     and logged, so a half-patched module never runs;
//   - applied before the user's own IPS/IPSwitch patches, so the check sees the module as shipped.
//
// Offset basis: offsets are positions in the decompressed module image -- the segments laid out
// at their load locations, text at 0, with no NSO header in front. An IPS/IPS32 file for the same
// change counts the 0x100-byte NSO header, so each of its offsets is exactly 0x100 higher.
namespace Loader::OpenPakCodePatches {

// One instruction replaced at an offset into the decompressed image.
struct Change {
    std::size_t offset;
    std::array<u8, 4> expected;
    std::array<u8, 4> written;
};

// A single build of a title and the changes it needs.
struct Build {
    std::string_view title;
    u64 title_id;
    std::string_view main_build_id; // hex, without the zero padding
    std::span<const Change> changes;
};

// Every build OpenPak changes. A new title is one more entry.
std::span<const Build> Builds();

// Applies the entry matching this module to image. build_id is the module's build id in hex;
// trailing zero padding is ignored. Returns the number of instructions changed: zero when nothing
// matches, when OpenPak is off, or when the original bytes are not what the entry expects.
std::size_t Apply(bool enabled, u64 title_id, std::string_view module_name,
                  std::string_view build_id, std::span<u8> image);

} // namespace Loader::OpenPakCodePatches
