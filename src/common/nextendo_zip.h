// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

// Minimal ZIP reader/writer, used for the Animal Crossing island backups exchanged with the
// Nextendo account server. Entries are stored uncompressed: the .dat files ACNH writes are
// encrypted, so deflating them buys nothing and costs time. Entry names are the paths relative to
// the archived directory, separated by '/', which is what the account server and Ryujinx-Nextendo
// both expect.
namespace Common::NextendoZip {

// The files under `dir`, as (relative path, absolute path), sorted by relative path.
std::vector<std::pair<std::string, std::filesystem::path>> ListFiles(const std::filesystem::path& dir);

// Builds the archive of `dir`. Empty when the directory doesn't exist or holds no file. The files
// whose relative name is in `skip` are left out.
std::vector<u8> ZipDirectory(const std::filesystem::path& dir,
                             const std::vector<std::string>& skip = {});

// Extracts the archive into `dest`, which is created if needed. Only stored (uncompressed)
// entries are supported; a compressed entry is refused rather than guessed at. Entry names that
// are absolute or that escape `dest` are refused too. Returns false and fills `error` on
// failure.
bool Extract(const std::vector<u8>& archive, const std::filesystem::path& dest, std::string& error);

} // namespace Common::NextendoZip
