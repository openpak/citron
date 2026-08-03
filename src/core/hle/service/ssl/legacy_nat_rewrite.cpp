// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/legacy_nat.h"
#include "core/hle/service/ssl/legacy_nat_rewrite.h"

namespace Service::SSL {

namespace {

constexpr std::string_view kAddressMarker = "prudp:/address=";

u16 ReadU16LE(std::span<const u8> b, size_t pos) {
    return static_cast<u16>(b[pos] | (b[pos + 1] << 8));
}

void WriteU16LE(std::vector<u8>& b, size_t pos, u16 value) {
    b[pos] = static_cast<u8>(value);
    b[pos + 1] = static_cast<u8>(value >> 8);
}

u32 ReadU32LE(std::span<const u8> b, size_t pos) {
    return static_cast<u32>(b[pos]) | (static_cast<u32>(b[pos + 1]) << 8) |
           (static_cast<u32>(b[pos + 2]) << 16) | (static_cast<u32>(b[pos + 3]) << 24);
}

void AppendU32LE(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value));
    out.push_back(static_cast<u8>(value >> 8));
    out.push_back(static_cast<u8>(value >> 16));
    out.push_back(static_cast<u8>(value >> 24));
}

bool IsPrivateIPv4(std::string_view addr) {
    int a, b, c, d;
    if (std::sscanf(std::string(addr).c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a == 10) return true;
    if (a == 192 && b == 168) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 100 && b >= 64 && b <= 127) return true; // CGNAT
    if (a == 127) return true;
    return false;
}

// Only touch a station whose natf/natm are already resolved (nonzero) but whose address is
// still private -- that's the one meant to be public. natf=0;natm=0 is genuinely local.
bool StationNeedsExternalIp(std::string_view station) {
    const auto addr_pos = station.find("address=");
    if (addr_pos == std::string_view::npos) {
        return false;
    }
    const auto addr_start = addr_pos + 8;
    const auto addr_end = station.find(';', addr_start);
    if (addr_end == std::string_view::npos) {
        return false;
    }
    if (!IsPrivateIPv4(station.substr(addr_start, addr_end - addr_start))) {
        return false; // already not a private address; nothing to fix
    }

    const auto natf_pos = station.find("natf=");
    if (natf_pos == std::string_view::npos) {
        return false;
    }
    return station[natf_pos + 5] != '0';
}

std::string SubstituteAddress(std::string_view station, std::array<u8, 4> ext_ip) {
    const auto addr_pos = station.find("address=");
    const auto addr_start = addr_pos + 8;
    const auto addr_end = station.find(';', addr_start);

    std::string out{station.substr(0, addr_start)};
    out += fmt::format("{}.{}.{}.{}", ext_ip[0], ext_ip[1], ext_ip[2], ext_ip[3]);
    out += station.substr(addr_end);
    return out;
}

struct Match {
    size_t length_field_pos; // offset of the u16 length prefix, within `body`
    size_t old_total;        // 2 + old string length
    std::string new_string;
};

// Locates by content marker rather than parsing Register's u32-counted list vs. ReplaceURL's
// two bare strings -- works for both without caring which.
std::vector<Match> FindMatches(std::span<const u8> body, std::array<u8, 4> ext_ip) {
    std::vector<Match> matches;
    std::string_view body_view{reinterpret_cast<const char*>(body.data()), body.size()};

    size_t search_from = 0;
    while (true) {
        const auto marker_pos = body_view.find(kAddressMarker, search_from);
        if (marker_pos == std::string_view::npos) {
            break;
        }
        if (marker_pos < 2) {
            search_from = marker_pos + kAddressMarker.size();
            continue;
        }
        const size_t length_field_pos = marker_pos - 2;
        const u16 str_len = ReadU16LE(body, length_field_pos);
        if (marker_pos + str_len > body.size()) {
            search_from = marker_pos + kAddressMarker.size();
            continue;
        }
        const std::string_view station = body_view.substr(marker_pos, str_len);
        if (StationNeedsExternalIp(station)) {
            matches.push_back({
                .length_field_pos = length_field_pos,
                .old_total = 2 + static_cast<size_t>(str_len),
                .new_string = SubstituteAddress(station, ext_ip),
            });
        }
        search_from = marker_pos + str_len;
    }
    return matches;
}

std::vector<u8> RebuildBody(std::span<const u8> body, const std::vector<Match>& matches) {
    std::vector<u8> out;
    out.reserve(body.size() + matches.size() * 8);
    size_t cursor = 0;
    for (const Match& m : matches) {
        out.insert(out.end(), body.begin() + cursor, body.begin() + m.length_field_pos);
        const u16 new_len = static_cast<u16>(m.new_string.size());
        out.push_back(static_cast<u8>(new_len));
        out.push_back(static_cast<u8>(new_len >> 8));
        out.insert(out.end(), m.new_string.begin(), m.new_string.end());
        cursor = m.length_field_pos + m.old_total;
    }
    out.insert(out.end(), body.begin() + cursor, body.end());
    return out;
}

} // Anonymous namespace

bool TryFixupStationAddress(std::span<const u8> input, std::vector<u8>& output) {
    const auto ext_ip_opt = Common::OpenPakNat::GetObservedExternalIp();
    if (!ext_ip_opt) {
        return false; // haven't seen a NAT-check reply yet, nothing to substitute with
    }

    // --- WebSocket frame ---
    if (input.size() < 2) {
        return false;
    }
    if ((input[0] & 0x80) == 0 || (input[0] & 0x0F) != 0x2) {
        return false; // only a single, complete, unfragmented binary frame
    }
    const bool masked = (input[1] & 0x80) != 0;
    u64 ws_len = input[1] & 0x7F;
    size_t pos = 2;
    if (ws_len == 126) {
        if (input.size() < pos + 2) return false;
        ws_len = (static_cast<u64>(input[pos]) << 8) | input[pos + 1];
        pos += 2;
    } else if (ws_len == 127) {
        return false; // not expected for these small RMC packets; don't guess
    }
    std::array<u8, 4> mask_key{};
    if (masked) {
        if (input.size() < pos + 4) return false;
        std::memcpy(mask_key.data(), input.data() + pos, 4);
        pos += 4;
    }
    if (input.size() != pos + ws_len) {
        return false; // buffer must be exactly one frame, no leftovers
    }

    std::vector<u8> ws_payload(input.begin() + pos, input.end());
    if (masked) {
        for (size_t i = 0; i < ws_payload.size(); ++i) {
            ws_payload[i] ^= mask_key[i % 4];
        }
    }

    // --- PRUDP-Lite ---
    if (ws_payload.size() < 12 || ws_payload[0] != 0x80) {
        return false;
    }
    const u8 opt_size = ws_payload[1];
    const u16 payload_size = ReadU16LE(ws_payload, 2);
    const size_t prudp_total = 12u + opt_size + payload_size;
    if (ws_payload.size() != prudp_total) {
        return false;
    }
    const u16 type_flags = ReadU16LE(ws_payload, 8);
    if ((type_flags & 0xF) != 2) {
        return false; // only DATA packets carry RMC
    }
    const size_t prudp_payload_start = 12u + opt_size;
    std::span<const u8> rmc = std::span(ws_payload).subspan(prudp_payload_start, payload_size);

    // --- RMC ---
    if (rmc.size() < 5) {
        return false;
    }
    const u32 rmc_len = ReadU32LE(rmc, 0);
    if (rmc_len != rmc.size() - 4) {
        return false;
    }
    const u8 proto_byte = rmc[4];
    if ((proto_byte & 0x80) == 0) {
        return false; // only requests carry an outgoing station -- not a response
    }
    const u16 protocol = proto_byte & 0x7F;
    if (protocol != 0x0B) {
        return false; // SecureConnection only
    }
    if (rmc.size() < 5 + 8) {
        return false;
    }
    const u32 method = ReadU32LE(rmc, 9);
    if (method != 0x1 && method != 0x7) {
        return false; // Register(1) / ReplaceURL(7) only
    }
    const size_t body_start = 5 + 8;
    std::span<const u8> body = rmc.subspan(body_start);

    const auto matches = FindMatches(body, *ext_ip_opt);
    if (matches.empty()) {
        return false;
    }

    // --- Rebuild bottom-up: body -> RMC -> PRUDP-Lite -> WebSocket ---
    const std::vector<u8> new_body = RebuildBody(body, matches);

    std::vector<u8> new_rmc;
    new_rmc.reserve(rmc.size() + new_body.size() - body.size());
    AppendU32LE(new_rmc, static_cast<u32>((rmc.size() - 4) + (new_body.size() - body.size())));
    new_rmc.insert(new_rmc.end(), rmc.begin() + 4, rmc.begin() + body_start);
    new_rmc.insert(new_rmc.end(), new_body.begin(), new_body.end());

    std::vector<u8> new_ws_payload;
    new_ws_payload.reserve(prudp_payload_start + new_rmc.size());
    new_ws_payload.insert(new_ws_payload.end(), ws_payload.begin(),
                          ws_payload.begin() + prudp_payload_start);
    WriteU16LE(new_ws_payload, 2, static_cast<u16>(new_rmc.size()));
    new_ws_payload.insert(new_ws_payload.end(), new_rmc.begin(), new_rmc.end());

    if (masked) {
        for (size_t i = 0; i < new_ws_payload.size(); ++i) {
            new_ws_payload[i] ^= mask_key[i % 4];
        }
    }

    output.clear();
    output.push_back(input[0]);
    const u64 new_len = new_ws_payload.size();
    if (new_len <= 125) {
        output.push_back(static_cast<u8>((masked ? 0x80 : 0) | new_len));
    } else if (new_len <= 0xFFFF) {
        output.push_back(static_cast<u8>((masked ? 0x80 : 0) | 126));
        output.push_back(static_cast<u8>(new_len >> 8));
        output.push_back(static_cast<u8>(new_len));
    } else {
        LOG_ERROR(Service_SSL, "[OpenPak] station-address rewrite produced an implausibly "
                               "large frame ({} bytes); leaving traffic untouched",
                  new_len);
        return false;
    }
    if (masked) {
        output.insert(output.end(), mask_key.begin(), mask_key.end());
    }
    output.insert(output.end(), new_ws_payload.begin(), new_ws_payload.end());

    LOG_INFO(Service_SSL,
             "[OpenPak] Rewrote {} station address(es) in outgoing SecureConnection.{} "
             "to this console's real external IP ({}.{}.{}.{})",
             matches.size(), method == 0x1 ? "Register" : "ReplaceURL", (*ext_ip_opt)[0],
             (*ext_ip_opt)[1], (*ext_ip_opt)[2], (*ext_ip_opt)[3]);
    return true;
}

} // namespace Service::SSL
