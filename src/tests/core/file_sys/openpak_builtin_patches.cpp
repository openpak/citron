// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/openpak_builtin_patches.h"

namespace {

namespace Builtin = FileSys::OpenPakBuiltinPatches;

constexpr std::string_view CtrBuild = "1C689518406930512C13DDF4217E7676";

bool AllZero(const std::vector<u8>& data) {
    return std::all_of(data.begin(), data.end(), [](u8 b) { return b == 0; });
}

} // Anonymous namespace

TEST_CASE("OpenPakBuiltinPatches::one entry, the CTR key IPS as shipped", "[core]") {
    REQUIRE(Builtin::Patches().size() == 1);
    const auto ips = Builtin::Patches()[0].ips;
    REQUIRE(ips.size() == 271);
    REQUIRE(std::string(ips.begin(), ips.begin() + 5) == "IPS32");
    REQUIRE(std::string(ips.end() - 4, ips.end()) == "EEOF");
}

TEST_CASE("OpenPakBuiltinPatches::found by build id, zero padding ignored", "[core]") {
    REQUIRE(Builtin::Find(CtrBuild) == &Builtin::Patches()[0]);
    REQUIRE(Builtin::Find(std::string(CtrBuild) + std::string(32, '0')) == &Builtin::Patches()[0]);
    REQUIRE(Builtin::Find("1c689518406930512c13ddf4217e7676") == &Builtin::Patches()[0]);
    REQUIRE(Builtin::Find("1C689518406930512C13DDF4217E7677") == nullptr);
    REQUIRE(Builtin::Find("") == nullptr);
    REQUIRE(Builtin::Find(std::string(64, '0')) == nullptr);
}

TEST_CASE("OpenPakBuiltinPatches::IPS offsets are header-included NSO offsets", "[core]") {
    // PatchNSO's buffer is the 0x100-byte header plus the image, so the record at 0x40B4E61
    // lands at 0x40B4E61 in it (image offset 0x40B4D61).
    const auto ips = Builtin::Patches()[0].ips;
    std::vector<u8> nso(0x40B4E61 + 0x200);
    REQUIRE(Builtin::WriteIps(ips, nso));
    REQUIRE(std::equal(ips.begin() + 11, ips.begin() + 11 + 256, nso.begin() + 0x40B4E61));
    REQUIRE(nso[0x40B4E60] == 0);
    REQUIRE(nso[0x40B4E61 + 256] == 0);
}

TEST_CASE("OpenPakBuiltinPatches::an unexpected key is left alone", "[core]") {
    // Right build, but the embedded key is not Demonware's (all zeroes here).
    std::vector<u8> nso(0x40B4E61 + 0x200);
    REQUIRE_FALSE(Builtin::Apply(Builtin::Patches()[0], nso));
    REQUIRE(AllZero(nso));
}

TEST_CASE("OpenPakBuiltinPatches::a short module is left alone", "[core]") {
    std::vector<u8> nso(0x1000);
    REQUIRE_FALSE(Builtin::Apply(Builtin::Patches()[0], nso));
    REQUIRE(AllZero(nso));
}
