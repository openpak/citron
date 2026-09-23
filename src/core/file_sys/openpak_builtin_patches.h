// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// The IPS patches OpenPak applies by itself while it is enabled, whatever mods the user has (or
// has disabled). They are ordinary IPS32 files, run through the same PatchIPS as the user's exefs
// patches and matched the same way, by the module's build id. Each one first checks that the bytes
// it replaces are the ones it was made for; anything else is logged and left as shipped.
//
// Offset basis: IPS offsets for an NSO count the 0x100-byte NSO header, as Atmosphere's patcher
// does. PatchManager::PatchNSO works on exactly that layout -- the NSO loader hands it the header
// followed by the decompressed image (from module_start, i.e. past any NCE pre-text area) -- and
// PatchIPS writes at the raw offsets it reads. So IPS offset X is image offset X - 0x100, the same
// as for the user's IPS files, and the check offsets below use that header-included basis too.
namespace FileSys::OpenPakBuiltinPatches {

struct Patch {
    std::string_view title;
    std::string_view build_id;     // hex as in the IPS file name, without the zero padding
    std::size_t checked_offset;    // header-included NSO offset of the bytes checked first
    std::size_t checked_size;
    std::string_view checked_sha1; // lowercase hex SHA-1 those bytes must have
    std::span<const u8> ips;       // the IPS32 file, verbatim
};

// Every built-in patch. There is one; see the .cpp.
std::span<const Patch> Patches();

// The built-in patch for this build id (hex; trailing zero padding ignored), or nullptr.
const Patch* Find(std::string_view build_id);

// Applies patch to nso (NSO header + decompressed image, the buffer PatchNSO works on) if the
// checked bytes are as expected. Returns whether it was applied; on false nso is unchanged.
bool Apply(const Patch& patch, std::vector<u8>& nso);

// Runs ips through PatchIPS onto nso. Returns whether it was applied.
bool WriteIps(std::span<const u8> ips, std::vector<u8>& nso);

} // namespace FileSys::OpenPakBuiltinPatches
