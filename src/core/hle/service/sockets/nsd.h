// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Sockets {

/// nsd's name resolution, the '%' -> environment substitution included. sfdnsres calls this when
/// a request asks for nsd resolution (or names a '%'), the way hardware routes it through nsd.
std::string NsdResolve(const std::string& fqdn_in);

class NSD final : public ServiceFramework<NSD> {
public:
    explicit NSD(Core::System& system_, const char* name);
    ~NSD() override;

private:
    void Resolve(HLERequestContext& ctx);
    void ResolveEx(HLERequestContext& ctx);
    void GetEnvironmentIdentifier(HLERequestContext& ctx);
    void GetApplicationServerEnvironmentType(HLERequestContext& ctx);

    bool IsNsdA() const;

    void GetSettingUrl(HLERequestContext& ctx);
    void GetSettingName(HLERequestContext& ctx);
    void GetDeviceId(HLERequestContext& ctx);
    void DeleteSettings(HLERequestContext& ctx);
    void ImportSettings(HLERequestContext& ctx);
    void SetChangeEnvironmentIdentifierDisabled(HLERequestContext& ctx);
    void GetNasServiceSetting(HLERequestContext& ctx);
    void GetNasServiceSettingEx(HLERequestContext& ctx);
    void GetNasRequestFqdn(HLERequestContext& ctx);
    void GetNasRequestFqdnEx(HLERequestContext& ctx);
    void GetNasApiFqdn(HLERequestContext& ctx);
    void GetNasApiFqdnEx(HLERequestContext& ctx);
    void GetCurrentSetting(HLERequestContext& ctx);
    void WriteTestParameter(HLERequestContext& ctx);
    void ReadTestParameter(HLERequestContext& ctx);
    void ReadSaveDataFromFsForTest(HLERequestContext& ctx);
    void WriteSaveDataToFsForTest(HLERequestContext& ctx);
    void DeleteSaveDataOfFsForTest(HLERequestContext& ctx);
    void IsChangeEnvironmentIdentifierDisabled(HLERequestContext& ctx);
    void SetWithoutDomainExchangeFqdns(HLERequestContext& ctx);
    void SetApplicationServerEnvironmentType(HLERequestContext& ctx);
    void DeleteApplicationServerEnvironmentType(HLERequestContext& ctx);
};

} // namespace Service::Sockets
