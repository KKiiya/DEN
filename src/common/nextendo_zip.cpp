// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/nextendo_zip.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

#include <fmt/format.h>

namespace Common::NextendoZip {

namespace {

constexpr u32 LocalHeaderSignature = 0x04034B50;
constexpr u32 CentralHeaderSignature = 0x02014B50;
constexpr u32 EndOfCentralDirSignature = 0x06054B50;

constexpr u16 MethodStored = 0;
constexpr u16 FlagUtf8Names = 0x0800;

void Put16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFF));
    out.push_back(static_cast<u8>((value >> 8) & 0xFF));
}

void Put32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value & 0xFF));
    out.push_back(static_cast<u8>((value >> 8) & 0xFF));
    out.push_back(static_cast<u8>((value >> 16) & 0xFF));
    out.push_back(static_cast<u8>((value >> 24) & 0xFF));
}

u16 Get16(const u8* data) {
    return static_cast<u16>(data[0] | (data[1] << 8));
}

u32 Get32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

// CRC-32 (IEEE 802.3), the checksum ZIP entries carry.
u32 Crc32(const u8* data, size_t size, u32 crc = 0) {
    static u32 table[256];
    static bool ready = false;
    if (!ready) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }

    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// The MS-DOS timestamp ZIP stores: two seconds per second, and a year offset from 1980.
void DosTime(std::time_t time, u16& dos_time, u16& dos_date) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    int year = tm.tm_year + 1900;
    if (year < 1980) {
        year = 1980;
    }
    dos_time = static_cast<u16>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    dos_date = static_cast<u16>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

bool ReadFile(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        file.read(reinterpret_cast<char*>(out.data()), size);
        if (!file) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

std::vector<std::pair<std::string, std::filesystem::path>> ListFiles(const std::filesystem::path& dir) {
    std::vector<std::pair<std::string, std::filesystem::path>> files;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return files;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string relative = std::filesystem::relative(entry.path(), dir, ec).generic_string();
        if (ec || relative.empty()) {
            continue;
        }
        files.emplace_back(std::move(relative), entry.path());
    }

    std::sort(files.begin(), files.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    return files;
}

std::vector<u8> ZipDirectory(const std::filesystem::path& dir, const std::vector<std::string>& skip) {
    std::vector<u8> archive;

    const auto files = ListFiles(dir);
    if (files.empty()) {
        return archive;
    }

    struct CentralEntry {
        std::string name;
        u32 crc = 0;
        u32 size = 0;
        u16 dos_time = 0;
        u16 dos_date = 0;
        u32 offset = 0;
    };
    std::vector<CentralEntry> central;

    for (const auto& [name, path] : files) {
        if (std::find(skip.begin(), skip.end(), name) != skip.end()) {
            continue;
        }

        std::vector<u8> data;
        if (!ReadFile(path, data)) {
            continue;
        }

        CentralEntry entry;
        entry.name = name;
        entry.crc = Crc32(data.data(), data.size());
        entry.size = static_cast<u32>(data.size());
        entry.offset = static_cast<u32>(archive.size());

        // The timestamp of the entry is cosmetic; the clock of the file system isn't portable
        // enough to convert here, so the current time is used.
        DosTime(std::time(nullptr), entry.dos_time, entry.dos_date);

        Put32(archive, LocalHeaderSignature);
        Put16(archive, 20); // version needed
        Put16(archive, FlagUtf8Names);
        Put16(archive, MethodStored);
        Put16(archive, entry.dos_time);
        Put16(archive, entry.dos_date);
        Put32(archive, entry.crc);
        Put32(archive, entry.size);
        Put32(archive, entry.size);
        Put16(archive, static_cast<u16>(entry.name.size()));
        Put16(archive, 0); // extra
        archive.insert(archive.end(), entry.name.begin(), entry.name.end());
        archive.insert(archive.end(), data.begin(), data.end());

        central.push_back(std::move(entry));
    }

    if (central.empty()) {
        return {};
    }

    const u32 central_offset = static_cast<u32>(archive.size());
    for (const auto& entry : central) {
        Put32(archive, CentralHeaderSignature);
        Put16(archive, 20); // version made by
        Put16(archive, 20); // version needed
        Put16(archive, FlagUtf8Names);
        Put16(archive, MethodStored);
        Put16(archive, entry.dos_time);
        Put16(archive, entry.dos_date);
        Put32(archive, entry.crc);
        Put32(archive, entry.size);
        Put32(archive, entry.size);
        Put16(archive, static_cast<u16>(entry.name.size()));
        Put16(archive, 0); // extra
        Put16(archive, 0); // comment
        Put16(archive, 0); // disk number
        Put16(archive, 0); // internal attributes
        Put32(archive, 0); // external attributes
        Put32(archive, entry.offset);
        archive.insert(archive.end(), entry.name.begin(), entry.name.end());
    }
    const u32 central_size = static_cast<u32>(archive.size()) - central_offset;

    Put32(archive, EndOfCentralDirSignature);
    Put16(archive, 0); // disk number
    Put16(archive, 0); // disk with the central directory
    Put16(archive, static_cast<u16>(central.size()));
    Put16(archive, static_cast<u16>(central.size()));
    Put32(archive, central_size);
    Put32(archive, central_offset);
    Put16(archive, 0); // comment

    return archive;
}

bool Extract(const std::vector<u8>& archive, const std::filesystem::path& dest, std::string& error) {
    error.clear();

    if (archive.size() < 22) {
        error = "archive too small";
        return false;
    }

    // The end-of-central-directory record sits at the end, after an optional comment.
    size_t eocd = archive.size() - 22;
    bool found = false;
    const size_t limit = archive.size() > 22 + 0xFFFF ? archive.size() - 22 - 0xFFFF : 0;
    for (; eocd >= limit && eocd + 4 <= archive.size(); eocd--) {
        if (Get32(&archive[eocd]) == EndOfCentralDirSignature) {
            found = true;
            break;
        }
        if (eocd == 0) {
            break;
        }
    }
    if (!found) {
        error = "no end of central directory";
        return false;
    }

    const u16 entries = Get16(&archive[eocd + 10]);
    const u32 central_size = Get32(&archive[eocd + 12]);
    const u32 central_offset = Get32(&archive[eocd + 16]);
    if (static_cast<u64>(central_offset) + central_size > archive.size()) {
        error = "central directory out of bounds";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        error = "cannot create the destination directory";
        return false;
    }

    u32 offset = central_offset;
    for (u16 i = 0; i < entries; i++) {
        if (offset + 46 > archive.size() || Get32(&archive[offset]) != CentralHeaderSignature) {
            error = "corrupt central directory";
            return false;
        }

        const u16 method = Get16(&archive[offset + 10]);
        const u32 size = Get32(&archive[offset + 24]);
        const u16 name_len = Get16(&archive[offset + 28]);
        const u16 extra_len = Get16(&archive[offset + 30]);
        const u16 comment_len = Get16(&archive[offset + 32]);
        const u32 local_offset = Get32(&archive[offset + 42]);

        if (offset + 46 + name_len > archive.size()) {
            error = "corrupt entry name";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(&archive[offset + 46]), name_len);

        offset += 46 + name_len + extra_len + comment_len;

        if (method != MethodStored) {
            error = fmt::format("unsupported compression method {} for {}", method, name);
            return false;
        }

        if (name.empty() || name.back() == '/') {
            continue; // directory entry
        }

        // Refuse absolute paths and anything that would land outside the destination.
        if (name.front() == '/' || name.find(':') != std::string::npos) {
            error = fmt::format("invalid entry name {}", name);
            return false;
        }
        bool escapes = false;
        {
            size_t start = 0;
            while (start <= name.size()) {
                const size_t end = name.find('/', start);
                const std::string part = name.substr(start, end == std::string::npos ? std::string::npos
                                                                                     : end - start);
                if (part == "..") {
                    escapes = true;
                    break;
                }
                start = end == std::string::npos ? name.size() + 1 : end + 1;
            }
        }
        if (escapes) {
            error = fmt::format("entry {} escapes the destination", name);
            return false;
        }

        if (local_offset + 30 > archive.size() || Get32(&archive[local_offset]) != LocalHeaderSignature) {
            error = fmt::format("corrupt local header for {}", name);
            return false;
        }
        const u16 local_name_len = Get16(&archive[local_offset + 26]);
        const u16 local_extra_len = Get16(&archive[local_offset + 28]);
        const u64 data_offset =
            static_cast<u64>(local_offset) + 30 + local_name_len + local_extra_len;
        if (data_offset + size > archive.size()) {
            error = fmt::format("truncated entry {}", name);
            return false;
        }

        const std::filesystem::path target = dest / name;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            error = fmt::format("cannot create the directory of {}", name);
            return false;
        }

        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = fmt::format("cannot write {}", name);
            return false;
        }
        file.write(reinterpret_cast<const char*>(&archive[data_offset]), static_cast<std::streamsize>(size));
        if (!file) {
            error = fmt::format("cannot write {}", name);
            return false;
        }
    }

    return true;
}

} // namespace Common::NextendoZip
