// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <string>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/loader/openpak_code_patches.h"

namespace Loader::OpenPakCodePatches {
namespace {

constexpr std::array<u8, 4> Nop{0x1F, 0x20, 0x03, 0xD5};

constexpr std::array<Change, 3> WonderChanges{{
    // LDRB W10, [X21, #0x38] -> MOV W10, #1: take the path that trusts the configured local
    // certificate instead of the pinned one.
    {0xB03528, {0xAA, 0xE2, 0x40, 0x39}, {0x2A, 0x00, 0x80, 0x52}},
    // CBNZ W0, +0x80 -> NOP: do not branch to the peer-name rejection.
    {0xB02BBC, {0x00, 0x04, 0x00, 0x35}, Nop},
    // B.NE +0x1B8 -> NOP: do not branch to the peer-name rejection.
    {0xB02AA4, {0xC1, 0x0D, 0x00, 0x54}, Nop},
}};

constexpr std::array<Build, 1> AllBuilds{{
    {"Super Mario Bros. Wonder 1.2.1", 0x010015100B514000ULL,
     "FF773E90972D544EB79406EAA65396D53C43EFB9", WonderChanges},
}};

std::string_view TrimPadding(std::string_view hex) {
    const auto last = hex.find_last_not_of('0');
    return last == std::string_view::npos ? std::string_view{} : hex.substr(0, last + 1);
}

bool SameHex(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

const Build* Find(u64 title_id, std::string_view build_id) {
    const auto trimmed = TrimPadding(build_id);
    for (const auto& build : AllBuilds) {
        if (build.title_id == title_id && SameHex(TrimPadding(build.main_build_id), trimmed)) {
            return &build;
        }
    }
    return nullptr;
}

} // Anonymous namespace

std::span<const Build> Builds() {
    return AllBuilds;
}

std::size_t Apply(bool enabled, u64 title_id, std::string_view module_name,
                  std::string_view build_id, std::span<u8> image) {
    if (!enabled || module_name != "main") {
        return 0;
    }

    const Build* build = Find(title_id, build_id);
    if (build == nullptr) {
        return 0;
    }

    for (const auto& change : build->changes) {
        const bool in_range = change.offset <= image.size() &&
                              image.size() - change.offset >= change.expected.size();
        if (!in_range || !std::equal(change.expected.begin(), change.expected.end(),
                                     image.begin() + change.offset)) {
            const std::string found =
                in_range ? Common::HexToString(image.subspan(change.offset, change.expected.size()))
                         : std::string{"out of range"};
            LOG_WARNING(Loader,
                        "[OpenPak] {}: expected {} at {:#X}, found {}; leaving the module "
                        "unchanged",
                        build->title, Common::HexToString(change.expected), change.offset, found);
            return 0;
        }
    }

    for (const auto& change : build->changes) {
        std::copy(change.written.begin(), change.written.end(), image.begin() + change.offset);
    }

    LOG_INFO(Loader, "[OpenPak] {}: changed {} instruction(s) so it accepts the OpenPak server",
             build->title, build->changes.size());
    return build->changes.size();
}

} // namespace Loader::OpenPakCodePatches
