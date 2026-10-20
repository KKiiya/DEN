// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/nextendo_island_sync.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo_zip.h"
#include "common/string_util.h"

namespace Common::NextendoIslandSync {

namespace {

std::mutex g_mutex;

// timegm() isn't available on Windows.
std::time_t Timegm(std::tm tm) {
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

// The state file, next to the account file.
std::filesystem::path StorePath() {
    return FS::GetEdenPath(FS::EdenPath::ConfigDir) / "nextendo_olsc.json";
}

std::vector<u8> DecodeBase64Impl(const std::string& text) {
    std::vector<u8> out;
    if (text.empty()) {
        return out;
    }

    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    u32 buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
        const char* found = std::strchr(table, c);
        if (found == nullptr) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<u32>(found - table);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string EncodeBase64Impl(std::span<const u8> data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < data.size()) {
        const u32 value = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8) |
                          static_cast<u32>(data[i + 2]);
        out.push_back(table[(value >> 18) & 0x3F]);
        out.push_back(table[(value >> 12) & 0x3F]);
        out.push_back(table[(value >> 6) & 0x3F]);
        out.push_back(table[value & 0x3F]);
        i += 3;
    }
    if (i + 1 == data.size()) {
        const u32 value = static_cast<u32>(data[i]) << 16;
        out.push_back(table[(value >> 18) & 0x3F]);
        out.push_back(table[(value >> 12) & 0x3F]);
        out.append("==");
    } else if (i + 2 == data.size()) {
        const u32 value = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8);
        out.push_back(table[(value >> 18) & 0x3F]);
        out.push_back(table[(value >> 12) & 0x3F]);
        out.push_back(table[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string ToHex(std::span<const u8> data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const u8 byte : data) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

nlohmann::json EntryToJson(const OlscEntry& entry) {
    nlohmann::json json;
    json["setting"] = entry.setting;
    json["custom"] = entry.custom;
    json["has_cache"] = entry.has_cache;
    json["cache_id"] = entry.cache_id;
    json["cache_time"] = entry.cache_time;
    json["cache_custom"] = entry.cache_custom;
    json["last_uploaded_hash"] = entry.last_uploaded_hash;
    json["pending_upload"] = entry.pending_upload;
    json["pending_save_root"] = entry.pending_save_root;
    return json;
}

OlscEntry EntryFromJson(const nlohmann::json& json) {
    OlscEntry entry;
    entry.setting = json.value("setting", u8{0});
    entry.custom = json.value("custom", std::string{});
    entry.has_cache = json.value("has_cache", false);
    entry.cache_id = json.value("cache_id", static_cast<u64>(0));
    entry.cache_time = json.value("cache_time", static_cast<s64>(0));
    entry.cache_custom = json.value("cache_custom", std::string{});
    entry.last_uploaded_hash = json.value("last_uploaded_hash", std::string{});
    entry.pending_upload = json.value("pending_upload", false);
    entry.pending_save_root = json.value("pending_save_root", std::string{});
    return entry;
}

nlohmann::json LoadAll() {
    const std::string contents = FS::ReadStringFromFile(StorePath(), FS::FileType::TextFile);
    if (contents.empty()) {
        return nlohmann::json::object();
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(contents);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return nlohmann::json::object();
}

void SaveAll(const nlohmann::json& all) {
    void(FS::CreateParentDirs(StorePath()));
    const std::string contents = all.dump();
    void(FS::WriteStringToFile(StorePath(), FS::FileType::TextFile, contents));
}

void TryDelete(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// The island's own files, the emulator's bookkeeping file left out.
std::vector<std::pair<std::string, std::filesystem::path>> IslandFiles(
    const std::filesystem::path& dir) {
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (auto& [name, path] : NextendoZip::ListFiles(dir)) {
        if (name == SaveSizeFileName) {
            continue;
        }
        files.emplace_back(std::move(name), std::move(path));
    }
    return files;
}

bool CopyDirectory(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        return false;
    }
    for (const auto& [name, path] : NextendoZip::ListFiles(source)) {
        const std::filesystem::path target = destination / name;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::copy_file(path, target, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return false;
        }
    }
    return true;
}

bool HasFilesImpl(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    return !IslandFiles(dir).empty();
}

} // Anonymous namespace

bool IsStrict(u64 title_id) {
    return title_id == AcnhTitleId;
}

bool HasFiles(const std::filesystem::path& dir) {
    return HasFilesImpl(dir);
}

std::vector<u8> DecodeBase64(const std::string& text) {
    return DecodeBase64Impl(text);
}

std::string EncodeBase64(std::span<const u8> data) {
    return EncodeBase64Impl(data);
}

void WriteBackupInfo(std::span<u8> dst, u64 id, s64 posix_time, std::span<const u8> custom) {
    if (static_cast<int>(dst.size()) < BackupInfoSize) {
        return;
    }

    std::fill(dst.begin(), dst.begin() + BackupInfoSize, u8{0});
    std::memcpy(dst.data(), &id, sizeof(id));
    std::memcpy(dst.data() + 8, &posix_time, sizeof(posix_time));
    const size_t count = std::min<size_t>(custom.size(), CustomDataSize);
    if (count > 0) {
        std::memcpy(dst.data() + 0x10, custom.data(), count);
    }
}

CloudIsland ParseMeta(const std::string& json) {
    CloudIsland island;

    try {
        const nlohmann::json parsed = nlohmann::json::parse(json);
        island.exists = parsed.value("exists", false);
        island.rev = parsed.value("rev", static_cast<u64>(0));
        island.custom = DecodeBase64Impl(parsed.value("custom", std::string{}));
        if (island.custom.size() > static_cast<size_t>(CustomDataSize)) {
            island.custom.clear();
        }

        const std::string updated_at = parsed.value("updatedAt", std::string{});
        if (!updated_at.empty()) {
            // "2026-10-20T12:34:56Z" (RFC 3339). Only the date and time parts are needed.
            std::tm tm{};
            if (std::sscanf(updated_at.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon,
                            &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                island.posix_time = static_cast<s64>(Timegm(tm));
            }
        }
    } catch (const nlohmann::json::exception&) {
    }

    return island;
}

bool IsRestorable(const CloudIsland& island) {
    return island.exists && !island.custom.empty();
}

std::string Fingerprint(const std::filesystem::path& dir) {
    const auto files = IslandFiles(dir);
    if (files.empty()) {
        return {};
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return {};
    }

    std::string digest;
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1) {
        std::vector<char> buffer(1 << 16);
        for (const auto& [name, path] : files) {
            EVP_DigestUpdate(context, name.data(), name.size());
            EVP_DigestUpdate(context, "\n", 1);

            std::ifstream file(path, std::ios::binary);
            if (!file) {
                continue;
            }
            while (file) {
                file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize read = file.gcount();
                if (read > 0) {
                    EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(read));
                }
            }
        }

        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(context, hash, &length) == 1) {
            digest = ToHex(std::span<const u8>(hash, length));
        }
    }
    EVP_MD_CTX_free(context);

    return digest;
}

std::vector<u8> ZipDirectory(const std::filesystem::path& dir) {
    return NextendoZip::ZipDirectory(dir, {SaveSizeFileName});
}

bool ExtractIsland(const std::vector<u8>& archive, const std::filesystem::path& dest,
                   std::string& error) {
    error.clear();

    if (!NextendoZip::Extract(archive, dest, error)) {
        return false;
    }

    long long extracted = 0;
    bool main = false;
    for (const auto& [name, path] : NextendoZip::ListFiles(dest)) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            continue;
        }
        extracted += static_cast<long long>(size);
        if (std::filesystem::path(name).filename() == "main.dat") {
            main = true;
        }
    }

    if (extracted > MaxExtractedBytes) {
        error = "island too large";
        return false;
    }

    if (!main) {
        error = "not an island (no main.dat)";
        return false;
    }

    return true;
}

ReplaceOutcome ReplaceIsland(const std::filesystem::path& save_dir, const std::vector<u8>& archive,
                             const std::filesystem::path& backups_dir, const std::string& prefix) {
    // The island is extracted next to the save, so the extraction never pollutes the save itself.
    const std::filesystem::path fresh = save_dir.parent_path() / "nx_restore_new";
    TryDelete(fresh);

    std::string error;
    if (!ExtractIsland(archive, fresh, error)) {
        TryDelete(fresh);
        LOG_WARNING(WebService, "[Nextendo] island: restore refused ({})", error);
        return ReplaceOutcome::InvalidIsland;
    }

    // The island of this machine is kept before being replaced.
    std::vector<u8> backup;
    if (HasFiles(save_dir)) {
        std::error_code ec;
        std::filesystem::create_directories(backups_dir, ec);
        const std::time_t now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm);

        backup = ZipDirectory(save_dir);
        if (!backup.empty()) {
            const auto path = backups_dir / fmt::format("{}-{}.zip", prefix, stamp);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (file) {
                file.write(reinterpret_cast<const char*>(backup.data()),
                           static_cast<std::streamsize>(backup.size()));
            }
        }

        // Keep the last five.
        std::vector<std::filesystem::path> backups;
        for (const auto& entry : std::filesystem::directory_iterator(backups_dir, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix + "-", 0) == 0 && name.size() > 4 &&
                name.compare(name.size() - 4, 4, ".zip") == 0) {
                backups.push_back(entry.path());
            }
        }
        std::sort(backups.begin(), backups.end());
        while (backups.size() > 5) {
            TryDelete(backups.front());
            backups.erase(backups.begin());
        }
    }

    // The island of the save is replaced by the one of the archive. The save directory itself is
    // never renamed: the game may still have it open.
    std::error_code ec;
    for (const auto& [name, path] : IslandFiles(save_dir)) {
        std::filesystem::remove(path, ec);
    }

    if (!CopyDirectory(fresh, save_dir)) {
        // Put the old island back.
        TryDelete(fresh);
        if (!backup.empty()) {
            std::string ignored;
            NextendoZip::Extract(backup, save_dir, ignored);
        }
        return ReplaceOutcome::NoLocalSave;
    }

    TryDelete(fresh);
    return ReplaceOutcome::Ok;
}

std::string Key(u64 title_id, const std::string& uid_hex) {
    return fmt::format("{:016x}:{}", title_id, uid_hex);
}

OlscEntry Get(const std::string& key) {
    std::scoped_lock lock{g_mutex};
    const nlohmann::json all = LoadAll();
    const auto it = all.find(key);
    if (it == all.end()) {
        return {};
    }
    try {
        return EntryFromJson(*it);
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

void Update(const std::string& key, const std::function<void(OlscEntry&)>& change) {
    std::scoped_lock lock{g_mutex};
    nlohmann::json all = LoadAll();
    OlscEntry entry;
    const auto it = all.find(key);
    if (it != all.end()) {
        try {
            entry = EntryFromJson(*it);
        } catch (const nlohmann::json::exception&) {
        }
    }
    change(entry);
    all[key] = EntryToJson(entry);
    SaveAll(all);
}

std::vector<std::pair<std::string, OlscEntry>> AllPending() {
    std::scoped_lock lock{g_mutex};
    const nlohmann::json all = LoadAll();

    std::vector<std::pair<std::string, OlscEntry>> pending;
    for (const auto& [key, value] : all.items()) {
        try {
            const OlscEntry entry = EntryFromJson(value);
            if (entry.pending_upload) {
                pending.emplace_back(key, entry);
            }
        } catch (const nlohmann::json::exception&) {
        }
    }
    return pending;
}

std::string DeviceId() {
    static std::string device_id;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto path = FS::GetEdenPath(FS::EdenPath::ConfigDir) / "nextendo_device_id.txt";
        std::string id = FS::ReadStringFromFile(path, FS::FileType::TextFile);
        id = Common::StripSpaces(id);
        if (id.size() != 8) {
            std::string random;
            const std::string source =
                fmt::format("{:x}", std::chrono::steady_clock::now().time_since_epoch().count());
            for (const char c : source) {
                if (random.size() >= 8) {
                    break;
                }
                if (std::isxdigit(static_cast<unsigned char>(c))) {
                    random.push_back(c);
                }
            }
            while (random.size() < 8) {
                random.push_back('0');
            }
            id = random;
            void(FS::CreateParentDirs(path));
            void(FS::WriteStringToFile(path, FS::FileType::TextFile, id));
        }
        device_id = "den:" + id;
    });
    return device_id;
}

} // namespace Common::NextendoIslandSync
