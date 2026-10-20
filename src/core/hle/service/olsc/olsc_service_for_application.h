// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/service.h"

namespace Service::OLSC {

// olsc:u -- the save data backup service of the console.
//
// [Nextendo] Animal Crossing: New Horizons does its island archiving through this service (title
// screen, minus button, "Paramètres de sauvegarde"), exactly like on a Switch: one slot per
// account, the last island sent wins, and the holder of the account may always restore it.
//
// The commands and their types come from the ACNH 3.0.3 client (proxy 0x47f9090, wrappers
// 0x5b7a60..0x5b7ed0, requests 0x5b9828..0x5bb004).
class IOlscServiceForApplication final : public ServiceFramework<IOlscServiceForApplication> {
public:
    explicit IOlscServiceForApplication(Core::System& system_);
    ~IOlscServiceForApplication() override;

private:
    void Initialize(HLERequestContext& ctx);
    void VerifySaveDataBackupLicenseAsync(HLERequestContext& ctx);
    void GetSaveDataBackupSetting(HLERequestContext& ctx);
    void SetSaveDataBackupSettingEnabled(HLERequestContext& ctx);
    void SetCustomData(HLERequestContext& ctx);
    void GetSaveDataBackupInfoCache(HLERequestContext& ctx);
    void UpdateSaveDataBackupInfoCacheAsync(HLERequestContext& ctx);
    void ListDownloadableSaveDataBackupInfoAsync(HLERequestContext& ctx);
    void DownloadSaveDataBackupAsync(HLERequestContext& ctx);
    void UploadSaveDataBackupAsync(HLERequestContext& ctx);

    bool initialized{};
};

} // namespace Service::OLSC
