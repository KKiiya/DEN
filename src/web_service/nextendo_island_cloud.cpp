// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "web_service/nextendo_island_cloud.h"

#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "common/nextendo_island_sync.h"
#include "web_service/nextendo_api.h"

namespace WebService::NextendoIslandCloud {

namespace {

// One send or one restore at a time: an older send must not finish after a newer one.
std::mutex g_gate;

std::string TitleHex(u64 title_id) {
    return fmt::format("{:016x}", title_id);
}

} // Anonymous namespace

std::function<bool(u64)> IsGameRunning = [](u64) { return false; };

bool Usable() {
    return Common::NextendoAccount::IsLinked() && !Common::NextendoAccount::GetToken().empty();
}

std::pair<Outcome, Common::NextendoIslandSync::CloudIsland> GetMeta(u64 title_id) {
    if (!Usable()) {
        return {Outcome::Unavailable, {}};
    }

    const auto meta = NextendoApi::GetSaveMeta(TitleHex(title_id));
    if (!meta) {
        return {Outcome::NetworkError, {}};
    }

    Common::NextendoIslandSync::CloudIsland island;
    island.exists = meta->exists;
    island.rev = meta->rev;
    island.custom = Common::NextendoIslandSync::DecodeBase64(meta->custom_base64);
    island.posix_time = meta->updated_at;

    return {Outcome::Ok, island};
}

void StoreCache(const std::string& key, const Common::NextendoIslandSync::CloudIsland& island) {
    Common::NextendoIslandSync::Update(key, [island](auto& entry) {
        entry.has_cache = Common::NextendoIslandSync::IsRestorable(island);
        entry.cache_id = entry.has_cache ? island.rev : 0;
        entry.cache_time = entry.has_cache ? island.posix_time : 0;
        entry.cache_custom =
            entry.has_cache ? Common::NextendoIslandSync::EncodeBase64(island.custom) : std::string{};
    });
}

std::string ContentDir(const std::filesystem::path& save_root) {
    if (save_root.empty()) {
        return {};
    }

    // Eden keeps the save data flat: the island is the set of files the game wrote in its save
    // directory.
    return Common::NextendoIslandSync::HasFiles(save_root) ? save_root.string() : std::string{};
}

Outcome Upload(u64 title_id, const std::filesystem::path& save_root, const std::string& key,
               bool only_if_changed) {
    std::scoped_lock lock{g_gate};

    if (!Usable()) {
        return Outcome::Unavailable;
    }

    if (only_if_changed && IsGameRunning && IsGameRunning(title_id)) {
        return Outcome::Deferred;
    }

    const std::string dir = ContentDir(save_root);
    if (dir.empty()) {
        return Outcome::NoLocalSave;
    }

    const auto entry = Common::NextendoIslandSync::Get(key);
    const std::string hash = Common::NextendoIslandSync::Fingerprint(dir);
    if (only_if_changed && !hash.empty() && hash == entry.last_uploaded_hash) {
        return Outcome::Unchanged;
    }

    // Snapshot read in memory and checked: a save that changes while it is read isn't sent.
    const std::vector<u8> zip = Common::NextendoIslandSync::ZipDirectory(dir);
    if (zip.empty()) {
        return Outcome::NoLocalSave;
    }
    if (Common::NextendoIslandSync::Fingerprint(dir) != hash) {
        return Outcome::Deferred;
    }

    const std::vector<u8> custom = Common::NextendoIslandSync::DecodeBase64(entry.custom);

    const auto result =
        NextendoApi::PushSaveBlob(TitleHex(title_id), zip, Common::NextendoIslandSync::DeviceId(),
                                  Common::NextendoIslandSync::EncodeBase64(custom));
    if (!result.ok) {
        LOG_WARNING(WebService, "[Nextendo] island: upload refused ({})", result.error);
        return result.too_large ? Outcome::Refused : Outcome::NetworkError;
    }

    Common::NextendoIslandSync::CloudIsland sent;
    sent.exists = true;
    sent.rev = 0;
    sent.custom = custom;
    StoreCache(key, sent);

    Common::NextendoIslandSync::Update(key, [hash](auto& state) { state.last_uploaded_hash = hash; });
    LOG_INFO(WebService, "[Nextendo] island: uploaded ({} bytes)", zip.size());

    return Outcome::Ok;
}

Outcome Download(u64 title_id, const std::filesystem::path& save_root, const std::string& key) {
    std::scoped_lock lock{g_gate};

    if (!Usable()) {
        return Outcome::Unavailable;
    }
    if (save_root.empty() || !std::filesystem::is_directory(save_root)) {
        return Outcome::NoLocalSave;
    }

    const auto blob = NextendoApi::PullSaveBlob(TitleHex(title_id));
    if (!blob) {
        return Outcome::Refused;
    }

    const auto backups_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "nextendo_island_backups";
    const auto replaced = Common::NextendoIslandSync::ReplaceIsland(
        save_root, blob->data, backups_dir, TitleHex(title_id));
    if (replaced != Common::NextendoIslandSync::ReplaceOutcome::Ok) {
        LOG_WARNING(WebService, "[Nextendo] island: restore not applied, local island kept");
        return replaced == Common::NextendoIslandSync::ReplaceOutcome::InvalidIsland
                   ? Outcome::InvalidIsland
                   : Outcome::NoLocalSave;
    }

    // The restored island is the cloud one: no need to send it back at the end of the game.
    const std::string restored_hash = Common::NextendoIslandSync::Fingerprint(save_root);
    Common::NextendoIslandSync::Update(key, [restored_hash](auto& state) {
        state.last_uploaded_hash = restored_hash;
        state.pending_upload = false;
    });

    const auto [meta_outcome, island] = GetMeta(title_id);
    if (meta_outcome == Outcome::Ok) {
        StoreCache(key, island);
    }

    LOG_INFO(WebService, "[Nextendo] island: restored ({} bytes, rev {})", blob->data.size(),
             blob->rev);
    return Outcome::Ok;
}

void MarkPending(const std::string& key, const std::filesystem::path& save_root) {
    Common::NextendoIslandSync::Update(key, [save_root](auto& entry) {
        entry.pending_upload = true;
        entry.pending_save_root = save_root.string();
    });
}

Outcome UploadPending(u64 title_id, const std::filesystem::path& save_root, const std::string& key) {
    if (IsGameRunning && IsGameRunning(title_id)) {
        return Outcome::Deferred;
    }

    const Outcome outcome = Upload(title_id, save_root, key, true);
    if (outcome == Outcome::Ok || outcome == Outcome::Unchanged || outcome == Outcome::NoLocalSave) {
        Common::NextendoIslandSync::Update(key, [](auto& entry) { entry.pending_upload = false; });
    }
    return outcome;
}

void RunPending() {
    for (const auto& [key, entry] : Common::NextendoIslandSync::AllPending()) {
        const size_t sep = key.find(':');
        if (sep == std::string::npos) {
            continue;
        }
        u64 title_id = 0;
        try {
            title_id = std::stoull(key.substr(0, sep), nullptr, 16);
        } catch (...) {
            continue;
        }
        if (!Common::NextendoIslandSync::IsStrict(title_id)) {
            continue;
        }

        const Outcome outcome = UploadPending(title_id, entry.pending_save_root, key);
        LOG_INFO(WebService, "[Nextendo] island: pending backup {}", static_cast<int>(outcome));
    }
}

} // namespace WebService::NextendoIslandCloud
