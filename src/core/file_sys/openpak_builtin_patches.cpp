// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>

#include <openssl/evp.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/file_sys/ips_layer.h"
#include "core/file_sys/openpak_builtin_patches.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys::OpenPakBuiltinPatches {
namespace {

// 1C689518406930512C13DDF4217E767600000000000000000000000000000000.ips, 271 bytes,
// sha256 5f44ed4e75889b035f4f16d49715211dfcffcdc7876b1a91e28bfda947f72c54
// (servers/demonware/tools/ctr-key-patch). One IPS32 record: 256 bytes at 0x40B4E61.
constexpr std::array<u8, 271> CtrKeyIps{
    0x49, 0x50, 0x53, 0x33, 0x32, 0x04, 0x0B, 0x4E, 0x61, 0x01, 0x00, 0xA1, 0x62, 0x25, 0x17, 0xE7,
    0xB8, 0x24, 0x46, 0xA0, 0xCE, 0xFE, 0x88, 0x8C, 0x91, 0x0C, 0x77, 0x7D, 0x60, 0x99, 0xD2, 0x4D,
    0xED, 0x6C, 0x29, 0x4C, 0x04, 0x64, 0x9B, 0x31, 0x20, 0xF7, 0x83, 0x02, 0x36, 0xA4, 0x19, 0x68,
    0x4F, 0x47, 0x06, 0x9C, 0xD7, 0xB3, 0xDF, 0x14, 0x1C, 0x7D, 0xFB, 0x26, 0xD2, 0x05, 0x19, 0x74,
    0x91, 0xF1, 0xA0, 0x54, 0x4A, 0x80, 0x5F, 0x72, 0x66, 0x3F, 0x43, 0x42, 0x6A, 0xB9, 0xDC, 0xAF,
    0x94, 0x20, 0x8E, 0x4E, 0x34, 0x9E, 0x29, 0xDB, 0x1F, 0xD6, 0x44, 0x21, 0x7F, 0xE7, 0x80, 0xAD,
    0xA6, 0x57, 0x26, 0x4F, 0xE0, 0x4F, 0x8F, 0xC3, 0x1F, 0xFC, 0xE9, 0x9D, 0xAE, 0xC4, 0xC2, 0x44,
    0xAA, 0xC3, 0x51, 0x96, 0x66, 0x52, 0xAB, 0x72, 0x51, 0xFB, 0x57, 0x22, 0x43, 0xD0, 0x70, 0x75,
    0x75, 0x9B, 0x64, 0xC7, 0x76, 0x8D, 0x10, 0xC8, 0xDB, 0x57, 0x62, 0x55, 0xBA, 0x8F, 0xE3, 0x30,
    0x70, 0x2B, 0x8C, 0xB1, 0xFA, 0x6D, 0xBB, 0x6E, 0x36, 0x2F, 0x28, 0x9F, 0x1E, 0x74, 0x8E, 0x11,
    0x71, 0xF1, 0xC4, 0x35, 0x72, 0x76, 0xD3, 0xB6, 0x80, 0x43, 0x79, 0x27, 0xC6, 0xC8, 0x55, 0x41,
    0x17, 0xD2, 0xAD, 0x09, 0x3E, 0xFF, 0xE0, 0x60, 0xE1, 0x84, 0x29, 0xBC, 0xF8, 0xA1, 0xFE, 0xEC,
    0x64, 0x89, 0x56, 0xDF, 0xAB, 0x17, 0xF5, 0x84, 0x21, 0x80, 0xAB, 0xEE, 0x9C, 0x6E, 0x93, 0xD1,
    0x13, 0x1B, 0x26, 0xA1, 0x71, 0x67, 0xC0, 0x59, 0x90, 0xCE, 0x27, 0x3C, 0xEE, 0xCA, 0xEE, 0x59,
    0x20, 0xA9, 0x8A, 0xC8, 0x15, 0x15, 0xFD, 0x60, 0xC3, 0x6E, 0x37, 0xA0, 0x36, 0xE4, 0x26, 0x9A,
    0xFF, 0xF1, 0xE0, 0xC1, 0xC5, 0x30, 0x59, 0xAC, 0xF7, 0xB0, 0x67, 0xC5, 0x0B, 0x62, 0x2C, 0xB1,
    0xD4, 0x1A, 0x04, 0x4D, 0xBB, 0x26, 0x14, 0x20, 0xAC, 0xFF, 0x6F, 0x45, 0x45, 0x4F, 0x46,
};

// The one sanctioned game patch: a data-only key swap, the same file the console setup
// (openpak.nro) installs to /atmosphere/exefs_patches/openpak_ctr_key/.
//
// Crash Team Racing Nitro-Fueled (0100F9F00C696000), final update v983040, main build
// 1C689518406930512C13DDF4217E7676: the game verifies Demonware's auth replies against an RSA-2048
// key embedded in main. The patch swaps that key's modulus for OpenPak's so the OpenPak Demonware
// server's signatures verify; the exponent, the verifier and every instruction stay as shipped.
// The check is the whole embedded SubjectPublicKeyInfo (294 bytes at 0x40B4E40, the modulus 0x21
// bytes in), which must be Demonware's key.
constexpr std::array<Patch, 1> BuiltinPatches{{
    {"Crash Team Racing Nitro-Fueled v983040 Demonware key", "1C689518406930512C13DDF4217E7676",
     0x40B4E40, 294, "f474dda2e170d9700e329935e35ef7b3e9df9c1c", CtrKeyIps},
}};

std::string_view TrimPadding(std::string_view hex) {
    const auto end = hex.find_last_not_of('0');
    return end == std::string_view::npos ? std::string_view{} : hex.substr(0, end + 1);
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

std::span<const Patch> Patches() {
    return BuiltinPatches;
}

const Patch* Find(std::string_view build_id) {
    const auto trimmed = TrimPadding(build_id);
    if (trimmed.empty()) {
        return nullptr;
    }
    for (const auto& patch : BuiltinPatches) {
        if (EqualsIgnoreCase(TrimPadding(patch.build_id), trimmed)) {
            return &patch;
        }
    }
    return nullptr;
}

bool Apply(const Patch& patch, std::vector<u8>& nso) {
    std::string found = "out of range";
    if (patch.checked_offset <= nso.size() && nso.size() - patch.checked_offset >= patch.checked_size) {
        std::array<u8, 20> digest{};
        unsigned int digest_size = 0;
        if (EVP_Digest(nso.data() + patch.checked_offset, patch.checked_size, digest.data(),
                       &digest_size, EVP_sha1(), nullptr) == 1 &&
            digest_size == digest.size()) {
            found = Common::HexToString(digest, false);
        }
    }
    if (found != patch.checked_sha1) {
        LOG_WARNING(Loader, "[OpenPak] {}: bytes at {:#x} have SHA-1 {}, expected {}; not applying",
                    patch.title, patch.checked_offset, found, patch.checked_sha1);
        return false;
    }

    LOG_INFO(Loader, "[OpenPak] Applying built-in IPS patch: {}", patch.title);
    return WriteIps(patch.ips, nso);
}

bool WriteIps(std::span<const u8> ips, std::vector<u8>& nso) {
    const auto ips_file = std::make_shared<VectorVfsFile>(std::vector<u8>(ips.begin(), ips.end()),
                                                          "openpak_builtin.ips");
    const auto patched = PatchIPS(std::make_shared<VectorVfsFile>(nso), ips_file);
    if (patched == nullptr) {
        LOG_ERROR(Loader, "[OpenPak] Built-in IPS patch did not apply");
        return false;
    }
    nso = patched->ReadAllBytes();
    return true;
}

} // namespace FileSys::OpenPakBuiltinPatches
