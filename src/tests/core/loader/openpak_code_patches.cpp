// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/openpak_code_patches.h"

namespace {

constexpr u64 WonderTitle = 0x010015100B514000ULL;
constexpr std::string_view WonderBuild = "FF773E90972D544EB79406EAA65396D53C43EFB9";

const Loader::OpenPakCodePatches::Build& Wonder() {
    for (const auto& build : Loader::OpenPakCodePatches::Builds()) {
        if (build.title_id == WonderTitle) {
            return build;
        }
    }
    FAIL("no Wonder entry");
    return Loader::OpenPakCodePatches::Builds().front();
}

// A zeroed image large enough for the entry, with the shipped instructions in place.
std::vector<u8> ShippedImage() {
    std::size_t end = 0;
    for (const auto& change : Wonder().changes) {
        end = std::max(end, change.offset + change.expected.size());
    }
    std::vector<u8> image(end + 0x100);
    for (const auto& change : Wonder().changes) {
        std::copy(change.expected.begin(), change.expected.end(), image.begin() + change.offset);
    }
    return image;
}

bool At(const std::vector<u8>& image, std::size_t offset, std::array<u8, 4> bytes) {
    return std::equal(bytes.begin(), bytes.end(), image.begin() + offset);
}

} // Anonymous namespace

TEST_CASE("OpenPakCodePatches::Wonder offsets are decompressed-image offsets", "[core]") {
    // The IPS offsets for the same change are these plus the 0x100 NSO header.
    const auto changes = Wonder().changes;
    REQUIRE(changes.size() == 3);
    REQUIRE(changes[0].offset == 0xB03528);
    REQUIRE(changes[1].offset == 0xB02BBC);
    REQUIRE(changes[2].offset == 0xB02AA4);
}

TEST_CASE("OpenPakCodePatches::matching build is changed", "[core]") {
    auto image = ShippedImage();
    // Build ids arrive as 32 bytes of hex with zero padding.
    const std::string padded = std::string(WonderBuild) + std::string(24, '0');

    REQUIRE(Loader::OpenPakCodePatches::Apply(true, WonderTitle, "main", padded, image) == 3);
    REQUIRE(At(image, 0xB03528, {0x2A, 0x00, 0x80, 0x52}));
    REQUIRE(At(image, 0xB02BBC, {0x1F, 0x20, 0x03, 0xD5}));
    REQUIRE(At(image, 0xB02AA4, {0x1F, 0x20, 0x03, 0xD5}));
}

TEST_CASE("OpenPakCodePatches::nothing changes when OpenPak is off", "[core]") {
    auto image = ShippedImage();
    const auto before = image;
    REQUIRE(Loader::OpenPakCodePatches::Apply(false, WonderTitle, "main", WonderBuild, image) == 0);
    REQUIRE(image == before);
}

TEST_CASE("OpenPakCodePatches::other modules are left alone", "[core]") {
    auto image = ShippedImage();
    const auto before = image;
    REQUIRE(Loader::OpenPakCodePatches::Apply(
                true, WonderTitle, "main", "FF773E90972D544EB79406EAA65396D53C43EFB8", image) == 0);
    REQUIRE(Loader::OpenPakCodePatches::Apply(true, 0x010015100B514800ULL, "main", WonderBuild,
                                              image) == 0);
    REQUIRE(Loader::OpenPakCodePatches::Apply(true, WonderTitle, "subsdk0", WonderBuild, image) ==
            0);
    REQUIRE(Loader::OpenPakCodePatches::Apply(true, WonderTitle, "main", "", image) == 0);
    REQUIRE(image == before);
}

TEST_CASE("OpenPakCodePatches::one unexpected instruction skips the whole entry", "[core]") {
    auto image = ShippedImage();
    // The last change checked differs; the first two must not have been written either.
    image[0xB02AA4] = static_cast<u8>(image[0xB02AA4] ^ 0xFF);
    const auto before = image;
    REQUIRE(Loader::OpenPakCodePatches::Apply(true, WonderTitle, "main", WonderBuild, image) == 0);
    REQUIRE(image == before);
}

TEST_CASE("OpenPakCodePatches::short image is left alone", "[core]") {
    std::vector<u8> image(0x1000);
    REQUIRE(Loader::OpenPakCodePatches::Apply(true, WonderTitle, "main", WonderBuild, image) == 0);
    REQUIRE(std::all_of(image.begin(), image.end(), [](u8 b) { return b == 0; }));
}
