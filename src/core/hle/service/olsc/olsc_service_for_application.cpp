// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/olsc/olsc_service_for_application.h"

#include <filesystem>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo_island_sync.h"
#include "core/core.h"
#include "core/file_sys/savedata_factory.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/olsc/olsc_async_result.h"
#include "web_service/nextendo_island_cloud.h"

namespace Service::OLSC {

namespace {

// The olsc module's own error codes (module 179). ACNH 3.0.3 (0x5b7bf0) reads "no cache" in the
// 2179-1200..1399 range of GetSaveDataBackupInfoCache and gives up on any other code: only that
// one may come out.
constexpr Result ErrNullArgument{ErrorModule::OLSC, 100};
constexpr Result ErrNotInitialized{ErrorModule::OLSC, 101};
constexpr Result ErrSaveDataBackupInfoCacheNotFound{ErrorModule::OLSC, 1200};

// The failures of the asynchronous operations: the game shows them through nn::err (2179-09xx),
// without any special handling (only 2124-0200..0269 and 2179-0010 have one).
constexpr Result ErrNextendoUnavailable{ErrorModule::OLSC, 900};
constexpr Result ErrNextendoNetworkError{ErrorModule::OLSC, 901};
constexpr Result ErrNextendoServerRefused{ErrorModule::OLSC, 902};
constexpr Result ErrNextendoNoLocalSave{ErrorModule::OLSC, 903};
constexpr Result ErrNextendoInvalidIsland{ErrorModule::OLSC, 904};

Result ToResult(WebService::NextendoIslandCloud::Outcome outcome) {
    switch (outcome) {
    case WebService::NextendoIslandCloud::Outcome::Ok:
    case WebService::NextendoIslandCloud::Outcome::Unchanged:
        return ResultSuccess;
    case WebService::NextendoIslandCloud::Outcome::Unavailable:
        return ErrNextendoUnavailable;
    case WebService::NextendoIslandCloud::Outcome::Refused:
        return ErrNextendoServerRefused;
    case WebService::NextendoIslandCloud::Outcome::NoLocalSave:
        return ErrNextendoNoLocalSave;
    case WebService::NextendoIslandCloud::Outcome::InvalidIsland:
        return ErrNextendoInvalidIsland;
    default:
        return ErrNextendoNetworkError;
    }
}

u64 TitleId(Core::System& system) {
    return system.GetApplicationProcessProgramID();
}

// The key of the (game, profile) pair the olsc state is kept under.
std::string Key(Core::System& system, const NS::Uid& uid) {
    return Common::NextendoIslandSync::Key(TitleId(system), uid.uuid.RawString());
}

// The directory the game wrote its save in. Eden keeps the save data flat: the island is the set
// of files the game wrote there.
std::filesystem::path FindSaveDir(Core::System& system, const NS::Uid& uid) {
    const auto save_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
    auto vfs_save_dir = system.GetFilesystem()->OpenDirectory(
        Common::FS::PathToUTF8String(save_dir), FileSys::OpenMode::Read);    if (!vfs_save_dir) {
        return {};
    }

    const auto relative = FileSys::SaveDataFactory::GetFullPath(
        TitleId(system), vfs_save_dir, FileSys::SaveDataSpaceId::User, FileSys::SaveDataType::Device,
        TitleId(system), uid.uuid.AsU128(), 0);

    auto dir = vfs_save_dir->GetDirectoryRelative(relative);
    if (!dir) {
        return {};
    }
    return std::filesystem::path{dir->GetFullPath()};
}

// Reads the "enabled" flag and the profile of SetSaveDataBackupSettingEnabled. The flag is one
// byte in the CMIF layout (the profile then follows, aligned on eight bytes) and one word in the
// legacy one.
void PopBackupSettingEnabled(HLERequestContext& ctx, bool& enabled, NS::Uid& account_id) {
    const u32* cmdbuf = ctx.CommandBuffer();
    const std::size_t base = ctx.IsTipc() ? 2 : ctx.GetDataPayloadOffset() + 2;

    enabled = cmdbuf[base] != 0;

    const std::size_t uid_word = ctx.IsTipc() ? base + 2 : base + 1;
    std::memcpy(&account_id.uuid, &cmdbuf[uid_word], sizeof(account_id.uuid));
}

} // Anonymous namespace

IOlscServiceForApplication::IOlscServiceForApplication(Core::System& system_)
    : ServiceFramework{system_, "olsc:u"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IOlscServiceForApplication::Initialize, "Initialize"},
        {10, &IOlscServiceForApplication::VerifySaveDataBackupLicenseAsync, "VerifySaveDataBackupLicenseAsync"},
        {13, &IOlscServiceForApplication::GetSaveDataBackupSetting, "GetSaveDataBackupSetting"},
        {14, &IOlscServiceForApplication::SetSaveDataBackupSettingEnabled, "SetSaveDataBackupSettingEnabled"},
        {15, &IOlscServiceForApplication::SetCustomData, "SetCustomData"},
        {16, nullptr, "DeleteSaveDataBackupSetting"},
        {18, &IOlscServiceForApplication::GetSaveDataBackupInfoCache, "GetSaveDataBackupInfoCache"},
        {19, &IOlscServiceForApplication::UpdateSaveDataBackupInfoCacheAsync, "UpdateSaveDataBackupInfoCacheAsync"},
        {22, nullptr, "DeleteSaveDataBackupAsync"},
        {25, &IOlscServiceForApplication::ListDownloadableSaveDataBackupInfoAsync, "ListDownloadableSaveDataBackupInfoAsync"},
        {26, &IOlscServiceForApplication::DownloadSaveDataBackupAsync, "DownloadSaveDataBackupAsync"},
        {27, &IOlscServiceForApplication::UploadSaveDataBackupAsync, "UploadSaveDataBackupAsync"},
        {9010, nullptr, "VerifySaveDataBackupLicenseAsyncForDebug"},
        {9013, nullptr, "GetSaveDataBackupSettingForDebug"},
        {9014, nullptr, "SetSaveDataBackupSettingEnabledForDebug"},
        {9015, nullptr, "SetCustomDataForDebug"},
        {9016, nullptr, "DeleteSaveDataBackupSettingForDebug"},
        {9018, nullptr, "GetSaveDataBackupInfoCacheForDebug"},
        {9019, nullptr, "UpdateSaveDataBackupInfoCacheAsyncForDebug"},
        {9022, nullptr, "DeleteSaveDataBackupAsyncForDebug"},
        {9025, nullptr, "ListDownloadableSaveDataBackupInfoAsyncForDebug"},
        {9026, nullptr, "DownloadSaveDataBackupAsyncForDebug"},
    };
    // clang-format on

    // The game may talk to the service with either protocol, and the arguments sit at the same
    // place in both, so the same handlers are registered for both.
    RegisterHandlers(functions);
    RegisterHandlersTipc(functions);
}

IOlscServiceForApplication::~IOlscServiceForApplication() = default;

void IOlscServiceForApplication::Initialize(HLERequestContext& ctx) {
    initialized = true;

    LOG_DEBUG(Service_OLSC, "called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IOlscServiceForApplication::VerifySaveDataBackupLicenseAsync(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    rp.PopRaw<NS::Uid>();

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    const u64 title_id = TitleId(system);
    Result result = ResultSuccess;
    if (!Common::NextendoIslandSync::IsStrict(title_id)) {
        result = ErrNextendoUnavailable;
    } else {
        // On the console: the subscription. Here: a usable Nextendo account AND a server that
        // archives this game (as long as it doesn't, the archiving can't be enabled, instead of
        // failing at every save afterwards).
        const auto [outcome, island] = WebService::NextendoIslandCloud::GetMeta(title_id);
        result = ToResult(outcome);
    }

    LOG_INFO(Service_OLSC, "[Nextendo] olsc: VerifySaveDataBackupLicenseAsync");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(ctx, std::make_shared<IAsyncResult>(system, result));
}

void IOlscServiceForApplication::GetSaveDataBackupSetting(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account_id = rp.PopRaw<NS::Uid>();

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    if (account_id.uuid.IsInvalid()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNullArgument);
        return;
    }

    // ACNH only compares it to 1 (enabled).
    const u8 setting = Common::NextendoIslandSync::Get(Key(system, account_id)).setting;

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
    rb.Push<u8>(setting);
}

void IOlscServiceForApplication::SetSaveDataBackupSettingEnabled(HLERequestContext& ctx) {
    bool enabled = false;
    NS::Uid account_id{};
    PopBackupSettingEnabled(ctx, enabled, account_id);

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    if (account_id.uuid.IsInvalid()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNullArgument);
        return;
    }

    const std::string key = Key(system, account_id);
    Common::NextendoIslandSync::Update(key, [enabled](auto& entry) {
        entry.setting = enabled ? u8{1} : u8{2};
    });

    LOG_INFO(Service_OLSC, "[Nextendo] olsc: SetSaveDataBackupSettingEnabled {}", enabled);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IOlscServiceForApplication::SetCustomData(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account_id = rp.PopRaw<NS::Uid>();

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    // The data the game stores itself, and reads back to display the island.
    std::vector<u8> data;
    if (ctx.CanReadBuffer(0)) {
        const auto buffer = ctx.ReadBuffer(0);
        const size_t size = std::min<size_t>(buffer.size(), Common::NextendoIslandSync::CustomDataSize);
        data.assign(buffer.begin(), buffer.begin() + size);
    }

    const std::string key = Key(system, account_id);
    const std::string encoded = Common::NextendoIslandSync::EncodeBase64(data);
    Common::NextendoIslandSync::Update(key, [encoded](auto& entry) { entry.custom = encoded; });

    LOG_DEBUG(Service_OLSC, "[Nextendo] olsc: SetCustomData ({} bytes)", data.size());

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IOlscServiceForApplication::GetSaveDataBackupInfoCache(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account_id = rp.PopRaw<NS::Uid>();

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    const auto entry = Common::NextendoIslandSync::Get(Key(system, account_id));
    if (!entry.has_cache || !ctx.CanWriteBuffer(0) ||
        ctx.GetWriteBufferSize(0) < static_cast<std::size_t>(Common::NextendoIslandSync::BackupInfoSize)) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrSaveDataBackupInfoCacheNotFound);
        return;
    }

    std::vector<u8> info(Common::NextendoIslandSync::BackupInfoSize, 0);
    Common::NextendoIslandSync::WriteBackupInfo(info, entry.cache_id, entry.cache_time,
                                                 Common::NextendoIslandSync::DecodeBase64(entry.cache_custom));
    ctx.WriteBuffer(info);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IOlscServiceForApplication::UpdateSaveDataBackupInfoCacheAsync(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account_id = rp.PopRaw<NS::Uid>();

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    const u64 title_id = TitleId(system);
    const std::string key = Key(system, account_id);

    Result result = ResultSuccess;
    if (!Common::NextendoIslandSync::IsStrict(title_id)) {
        result = ErrNextendoUnavailable;
    } else {
        const auto [outcome, island] = WebService::NextendoIslandCloud::GetMeta(title_id);
        result = ToResult(outcome);
        if (result.IsSuccess()) {
            WebService::NextendoIslandCloud::StoreCache(key, island);
        }
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(ctx, std::make_shared<IAsyncResult>(system, result));
}

void IOlscServiceForApplication::ListDownloadableSaveDataBackupInfoAsync(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    rp.Pop<u32>(); // size of the work memory (0x2000 in ACNH)
    rp.Pop<u32>();
    const auto account_id = rp.PopRaw<NS::Uid>();

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    const u64 title_id = TitleId(system);
    const std::string key = Key(system, account_id);

    Result result = ResultSuccess;
    std::vector<std::vector<u8>> entries;
    if (!Common::NextendoIslandSync::IsStrict(title_id)) {
        result = ErrNextendoUnavailable;
    } else {
        const auto [outcome, island] = WebService::NextendoIslandCloud::GetMeta(title_id);
        result = ToResult(outcome);
        if (result.IsSuccess()) {
            WebService::NextendoIslandCloud::StoreCache(key, island);
            if (Common::NextendoIslandSync::IsRestorable(island)) {
                std::vector<u8> info(Common::NextendoIslandSync::BackupInfoSize, 0);
                Common::NextendoIslandSync::WriteBackupInfo(info, island.rev, island.posix_time,
                                                             island.custom);
                entries.push_back(std::move(info));
            }
        }
    }

    LOG_INFO(Service_OLSC, "[Nextendo] olsc: {} island(s) to restore", entries.size());

    auto object = std::make_shared<IAsyncResult>(system, result, IAsyncResult::Mode::List);
    object->SetEntries(std::move(entries));

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(ctx, object);
}

void IOlscServiceForApplication::DownloadSaveDataBackupAsync(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account_id = rp.PopRaw<NS::Uid>();
    rp.Pop<u64>(); // the backup id

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    const u64 title_id = TitleId(system);
    const std::string key = Key(system, account_id);
    const auto save_dir = FindSaveDir(system, account_id);

    Result result = ResultSuccess;
    if (!Common::NextendoIslandSync::IsStrict(title_id)) {
        result = ErrNextendoUnavailable;
    } else if (save_dir.empty()) {
        result = ErrNextendoNoLocalSave;
    } else {
        result = ToResult(WebService::NextendoIslandCloud::Download(title_id, save_dir, key));
    }

    LOG_INFO(Service_OLSC, "[Nextendo] olsc: DownloadSaveDataBackupAsync -> {}", result.raw);

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(ctx, std::make_shared<IAsyncResult>(system, result, IAsyncResult::Mode::Transfer));
}

void IOlscServiceForApplication::UploadSaveDataBackupAsync(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account_id = rp.PopRaw<NS::Uid>();

    if (!initialized) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ErrNotInitialized);
        return;
    }

    const u64 title_id = TitleId(system);
    const std::string key = Key(system, account_id);
    const auto save_dir = FindSaveDir(system, account_id);

    Result result = ResultSuccess;
    if (!Common::NextendoIslandSync::IsStrict(title_id)) {
        result = ErrNextendoUnavailable;
    } else if (save_dir.empty()) {
        result = ErrNextendoNoLocalSave;
    } else {
        // The game asks for the archiving itself: it's allowed while the game runs.
        result = ToResult(WebService::NextendoIslandCloud::Upload(title_id, save_dir, key, false));
    }

    LOG_INFO(Service_OLSC, "[Nextendo] olsc: UploadSaveDataBackupAsync -> {}", result.raw);

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(ctx, std::make_shared<IAsyncResult>(system, result, IAsyncResult::Mode::Transfer));
}

} // namespace Service::OLSC
