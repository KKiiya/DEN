// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

#include "common/common_types.h"
#include "common/nextendo_island_sync.h"

// [Nextendo] What olsc does on Nintendo's side, against the Nextendo account server: the archived
// island of the account lives in /api/save/<title> (one slot, the last island sent wins, the
// restoration is allowed to the holder of the account). Used by olsc:u while the game runs and,
// like the Switch's system service ("the backup happens when you aren't playing"), after the game
// ends.
namespace WebService::NextendoIslandCloud {

enum class Outcome {
    Ok,
    Unavailable,  // no usable Nextendo account
    NetworkError,
    Refused,      // the server refused
    NoLocalSave,  // nothing to send
    InvalidIsland,
    Unchanged,    // the island didn't change since the last send
    Deferred,     // the game is running, or the save changed while it was read
};

// A linked Nextendo account, with a token, able to archive islands.
bool Usable();

// The state of the account's island.
std::pair<Outcome, Common::NextendoIslandSync::CloudIsland> GetMeta(u64 title_id);

// Mirrors the server's state into the olsc cache (GetSaveDataBackupInfoCache).
void StoreCache(const std::string& key, const Common::NextendoIslandSync::CloudIsland& island);

// The content of a save: the committed "0" directory, else the working "1" one. Empty when there
// is nothing to send.
std::string ContentDir(const std::filesystem::path& save_root);

// Sends the island of the save. only_if_changed skips an island identical to the one already sent,
// and refuses to send anything while the game of that title is running.
Outcome Upload(u64 title_id, const std::filesystem::path& save_root, const std::string& key,
               bool only_if_changed);

// Restores the island of the account over the island of the save. The island of this machine is
// kept as a ZIP first, and a failure puts the old island back.
Outcome Download(u64 title_id, const std::filesystem::path& save_root, const std::string& key);

// Like the Switch's system service, the backup happens when you aren't playing: the send is
// marked pending on disk first, so an emulator closed before the end of it leaves the mark, and it
// starts again at the next launch (RunPending).
void MarkPending(const std::string& key, const std::filesystem::path& save_root);

// Sends an island marked pending, then lifts the mark when it worked.
Outcome UploadPending(u64 title_id, const std::filesystem::path& save_root, const std::string& key);

// At startup: sends the islands whose send was left pending.
void RunPending();

// True while the game of that title runs: the post-game send must not start then.
extern std::function<bool(u64)> IsGameRunning;

} // namespace WebService::NextendoIslandCloud
