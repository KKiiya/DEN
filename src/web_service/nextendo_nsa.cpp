// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "web_service/nextendo_nsa.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "common/string_util.h"
#include "web_service/nextendo_api.h"

namespace WebService::NextendoNsa {

namespace {

std::mutex g_mutex;
u64 g_pid = 0;
u64 g_nsa = 0;
s64 g_asked_at = 0;

// The account server is asked at most once a minute; the value barely ever changes.
constexpr s64 RetryMs = 60 * 1000;

std::filesystem::path FilePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "nextendo_nsa.txt";
}

u64 ParseHex(std::string_view text) {
    if (text.size() != 16) {
        return 0;
    }
    u64 value = 0;
    for (const char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<u64>(c - 'A' + 10);
        } else {
            return 0;
        }
    }
    return value;
}

u64 ReadFile(u64 pid) {
    const std::string contents = Common::FS::ReadStringFromFile(FilePath(), Common::FS::FileType::TextFile);
    if (contents.empty()) {
        return 0;
    }

    std::vector<std::string> lines;
    Common::SplitString(contents, '\n', lines);
    for (const std::string& line : lines) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        u64 stored_pid = 0;
        try {
            stored_pid = std::stoull(line.substr(0, eq));
        } catch (...) {
            continue;
        }
        if (stored_pid == pid) {
            return ParseHex(line.substr(eq + 1));
        }
    }
    return 0;
}

void WriteFile(u64 pid, u64 nsa) {
    const auto path = FilePath();
    const std::string contents = Common::FS::ReadStringFromFile(path, Common::FS::FileType::TextFile);

    std::string out;
    std::vector<std::string> lines;
    Common::SplitString(contents, '\n', lines);
    for (const std::string& line : lines) {
        if (line.empty()) {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            u64 stored_pid = 0;
            try {
                stored_pid = std::stoull(line.substr(0, eq));
            } catch (...) {
            }
            if (stored_pid == pid) {
                continue;
            }
        }
        out += line;
        out += '\n';
    }
    out += fmt::format("{}={:016x}\n", pid, nsa);

    void(Common::FS::CreateParentDirs(path));
    void(Common::FS::WriteStringToFile(path, Common::FS::FileType::TextFile, out));
}

} // Anonymous namespace

bool TitleUsesBaasNsa(u64 program_id) {
    return program_id == AcnhProgramId;
}

u64 Get() {
    const char* disabled = std::getenv("NEXTENDO_NSA_BAAS");
    if (disabled != nullptr && disabled[0] == '0' && disabled[1] == '\0') {
        return 0;
    }
    if (!Common::NextendoAccount::IsLinked()) {
        return 0;
    }

    const u64 pid = Common::NextendoAccount::GetPid();
    if (pid == 0) {
        return 0;
    }

    {
        std::scoped_lock lock{g_mutex};
        if (g_pid == pid && g_nsa != 0) {
            return g_nsa;
        }

        if (g_pid != pid) {
            g_pid = pid;
            g_nsa = ReadFile(pid);
            g_asked_at = 0;
            if (g_nsa != 0) {
                return g_nsa;
            }
        }

        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (g_asked_at != 0 && now - g_asked_at < RetryMs) {
            return 0;
        }
        g_asked_at = now;
    }

    const std::string asked = NextendoApi::GetMyNsa();
    const u64 nsa = ParseHex(asked);

    std::scoped_lock lock{g_mutex};
    if (g_pid == pid && g_nsa == 0 && nsa != 0) {
        g_nsa = nsa;
        WriteFile(pid, nsa);
        LOG_INFO(WebService, "[Nextendo] baasUserID known for pid {}", pid);
    }
    return g_pid == pid ? g_nsa : 0;
}

u64 Choose(u64 program_id, u64 pid, u64 baas_nsa) {
    return TitleUsesBaasNsa(program_id) && baas_nsa != 0 ? baas_nsa : pid;
}

} // namespace WebService::NextendoNsa
