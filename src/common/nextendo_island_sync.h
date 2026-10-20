// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

// [Nextendo] Animal Crossing: New Horizons island archiving, the part that needs no network and
// no IPC: the layout of the olsc backup info, the fingerprint of an island, the ZIP of an island
// and the careful replacement of the local island by the cloud one.
//
// Like on a Switch, the game itself does the archiving through the olsc:u service (title screen,
// minus button, "Paramètres de sauvegarde"); one slot per account, the last island sent wins, and
// the holder of the account may always restore it.
//
// An island is the set of files the game wrote in its save directory. Eden keeps the save data in
// a flat directory (no "0"/"1" double buffer), and the emulator's own bookkeeping file sits next
// to the game's files: it is not part of the island and never leaves the machine.
namespace Common::NextendoIslandSync {

// Animal Crossing: New Horizons.
constexpr u64 AcnhTitleId = 0x01006F8002326000ULL;

// The emulator's save size bookkeeping file, which lives in the save directory but isn't part of
// the island.
constexpr const char* SaveSizeFileName = ".yuzu_save_size";

// The titles whose backup goes through the Nextendo server (the Device save, one slot per
// account).
bool IsStrict(u64 title_id);

// The SaveDataBackupInfo structure olsc hands over: +0 the backup id (the server revision), +8 the
// POSIX time, +0x10 the custom data the game itself stored.
constexpr int BackupInfoSize = 0x410;
constexpr int CustomDataSize = 0x400;

void WriteBackupInfo(std::span<u8> dst, u64 id, s64 posix_time, std::span<const u8> custom);

// The state of the account's island, as the server's /api/save/<title>/meta answers it.
struct CloudIsland {
    bool exists = false;
    u64 rev = 0;
    s64 posix_time = 0;
    std::vector<u8> custom;
};

CloudIsland ParseMeta(const std::string& json);

// The custom data the game stores is carried as base64 (the server and the state file both keep
// it that way).
std::vector<u8> DecodeBase64(const std::string& text);
std::string EncodeBase64(std::span<const u8> data);

// An island is only offered for restoration when it carries the custom data the game reads back
// to display the island: without it, the game fails the whole list.
bool IsRestorable(const CloudIsland& island);

// SHA-256 of the directory (sorted relative paths, sizes and contents). Empty when the directory
// holds no file.
std::string Fingerprint(const std::filesystem::path& dir);

// True when the directory holds at least one file of the island (the emulator's own bookkeeping
// file doesn't count).
bool HasFiles(const std::filesystem::path& dir);

// The ZIP of the island, as the server stores it.
std::vector<u8> ZipDirectory(const std::filesystem::path& dir);

// Extracts an island into a brand new directory. Refuses an archive that isn't an island (no
// main.dat), and an archive bigger than MaxExtractedBytes once extracted.
constexpr long long MaxExtractedBytes = 64LL << 20;

bool ExtractIsland(const std::vector<u8>& archive, const std::filesystem::path& dest,
                   std::string& error);

enum class ReplaceOutcome {
    Ok,
    NoLocalSave,
    InvalidIsland,
};

// Replaces the island of the save directory by the one of the archive. The island of this machine
// is kept as a ZIP in `backups_dir` (the last five), and a failure puts the old island back.
ReplaceOutcome ReplaceIsland(const std::filesystem::path& save_dir, const std::vector<u8>& archive,
                             const std::filesystem::path& backups_dir, const std::string& prefix);

// What olsc keeps per (profile, game): the backup setting (1 = enabled), the game's custom data
// and the cache of the cloud island.
struct OlscEntry {
    u8 setting = 0;
    std::string custom;
    bool has_cache = false;
    u64 cache_id = 0;
    s64 cache_time = 0;
    std::string cache_custom;
    // Fingerprint of the last island sent or restored, so an unchanged island isn't sent again.
    std::string last_uploaded_hash;
    // The post-game upload that hasn't been done yet (retried at the next launch).
    bool pending_upload = false;
    std::string pending_save_root;
};

// The key of a (game, profile) pair: "<title id in hex>:<profile id in hex>".
std::string Key(u64 title_id, const std::string& uid_hex);

OlscEntry Get(const std::string& key);
void Update(const std::string& key, const std::function<void(OlscEntry&)>& change);

// The entries whose post-game upload is still pending.
std::vector<std::pair<std::string, OlscEntry>> AllPending();

// This installation's identifier, kept in the island's metadata.
std::string DeviceId();

} // namespace Common::NextendoIslandSync
