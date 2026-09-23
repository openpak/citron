// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Service::Sockets::InterfaceList {

// [OpenPak] The answer to sysctl {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLISTL, 0}: FreeBSD's
// getifaddrs(). NPLN's WebRTC asks for it to gather connection candidates; refused, a Dinkum host
// offers a joiner no address at all and the join times out with 2321-5248 (2026-09-18). Ported
// from Ryujinx (Services/Sockets/Bsd/InterfaceList.cs).
//
// One interface, the address games are already told through nifm. Every message carries its own
// length and the offset of what follows (ifm_len, ifm_data_off), and the parser walks by those, so
// a guest whose if_data differs in size still finds the addresses. Every sockaddr is a multiple
// of 8 bytes, which reads the same under 4- or 8-byte SA_SIZE rounding.

constexpr s32 CtlNet = 4;
constexpr s32 PfRoute = 17;
constexpr s32 NetRtIfListL = 5;

inline bool Matches(std::span<const s32> mib) {
    return mib.size() >= 5 && mib[0] == CtlNet && mib[1] == PfRoute && mib[4] == NetRtIfListL;
}

namespace detail {

constexpr u8 RtmVersion = 5;
constexpr u8 RtmIfInfo = 0x0e;
constexpr u8 RtmNewAddr = 0x0c;
constexpr s32 RtaNetmask = 0x04;
constexpr s32 RtaIfp = 0x10;
constexpr s32 RtaIfa = 0x20;
constexpr s32 RtaBrd = 0x80;
constexpr s32 IffUpBroadcastRunningMulticast = 0x1 | 0x2 | 0x40 | 0x8000;

constexpr std::size_t HeaderSize = 24; // up to and including the padding before if_data
constexpr std::size_t IfDataSize = 152; // FreeBSD 11+ struct if_data
constexpr std::size_t SockaddrDlSize = 24;
constexpr std::size_t SockaddrInSize = 16;
constexpr u16 InterfaceIndex = 1;

inline void Put16(u8* p, u16 v) {
    std::memcpy(p, &v, sizeof(v)); // the guest is little-endian, as is every host Citron runs on
}

inline void Put32(u8* p, u32 v) {
    std::memcpy(p, &v, sizeof(v));
}

// if_msghdrl and ifa_msghdrl share this prefix; ifam_metric (offset 20) stays 0.
inline void WriteHeader(u8* m, std::size_t length, u8 type, s32 addrs, s32 flags) {
    Put16(m, static_cast<u16>(length)); // msglen
    m[2] = RtmVersion;
    m[3] = type;
    Put32(m + 4, static_cast<u32>(addrs));
    Put32(m + 8, static_cast<u32>(flags));
    Put16(m + 12, InterfaceIndex);
    Put16(m + 16, static_cast<u16>(HeaderSize + IfDataSize)); // len: sockaddrs follow
    Put16(m + 18, static_cast<u16>(HeaderSize));              // data_off
}

inline void WriteIfData(u8* d) {
    d[0] = 6;  // IFT_ETHER
    d[2] = 6;  // addrlen
    d[3] = 14; // hdrlen
    d[4] = 2;  // LINK_STATE_UP
    Put16(d + 6, static_cast<u16>(IfDataSize)); // datalen
    Put32(d + 8, 1500);                         // mtu
}

inline void WriteSockaddrIn(u8* s, const std::array<u8, 4>& address) {
    s[0] = static_cast<u8>(SockaddrInSize);
    s[1] = 2; // AF_INET
    std::memcpy(s + 4, address.data(), address.size()); // network order already; port stays 0
}

} // namespace detail

// address and netmask in network byte order, as nifm hands them to the guest.
inline std::vector<u8> Build(const std::array<u8, 4>& ip, const std::array<u8, 4>& mask) {
    using namespace detail;

    std::array<u8, 4> broadcast{};
    for (std::size_t i = 0; i < broadcast.size(); ++i) {
        broadcast[i] = static_cast<u8>(ip[i] | ~mask[i]);
    }

    const std::size_t info_length = HeaderSize + IfDataSize + SockaddrDlSize;
    const std::size_t addr_length = HeaderSize + IfDataSize + 3 * SockaddrInSize;
    std::vector<u8> buffer(info_length + addr_length);

    // RTM_IFINFO (struct if_msghdrl) + the link-level sockaddr_dl naming the interface
    u8* info = buffer.data();
    WriteHeader(info, info_length, RtmIfInfo, RtaIfp, IffUpBroadcastRunningMulticast);
    WriteIfData(info + HeaderSize);
    u8* dl = info + HeaderSize + IfDataSize;
    dl[0] = static_cast<u8>(SockaddrDlSize); // sdl_len, padded: see the comment at the top
    dl[1] = 18;                              // AF_LINK
    Put16(dl + 2, InterfaceIndex);
    dl[4] = 6; // IFT_ETHER
    dl[5] = 4; // sdl_nlen: "eth0"
    dl[6] = 6; // sdl_alen
    std::memcpy(dl + 8, "eth0", 4);
    const std::array<u8, 6> mac{0x02, 0x00, ip[0], ip[1], ip[2], ip[3]}; // locally administered
    std::memcpy(dl + 12, mac.data(), mac.size());

    // RTM_NEWADDR (struct ifa_msghdrl) + netmask, address, broadcast in RTA_* bit order
    u8* addr = buffer.data() + info_length;
    WriteHeader(addr, addr_length, RtmNewAddr, RtaNetmask | RtaIfa | RtaBrd, 0);
    WriteIfData(addr + HeaderSize);
    u8* sa = addr + HeaderSize + IfDataSize;
    WriteSockaddrIn(sa, mask);
    WriteSockaddrIn(sa + SockaddrInSize, ip);
    WriteSockaddrIn(sa + 2 * SockaddrInSize, broadcast);

    return buffer;
}

} // namespace Service::Sockets::InterfaceList
