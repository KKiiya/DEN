// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::OLSC {

// [Nextendo] The asynchronous objects of olsc:u.
//
// The commands and their types come from the ACNH 3.0.3 client (the async object's vtables at
// 0x47f9208, 0x47f9328, 0x47f9470): 0 Get (the result of the operation), 1 Cancel, 2 HasDone -> b8,
// 3 GetSystemEvent -> handle<copy>. The game waits on the event, then calls HasDone and Get
// (0x5bb398).
//
// The list object (command 25) adds 100 GetCount -> u32 and 101 GetList (u32 start index, output
// buffer of 0x410-byte items) -> u32 written; the transfer objects (commands 26 and 27) add
// 100 GetProgress -> 0x30 bytes (0x5bb180), which ACNH doesn't call: the operation is reported as
// done in one go, so the progress is empty.
//
// The object is created with its result already known: the operation ran before the object was
// handed to the game, and the completion event is signalled right away, exactly like the console
// does for an operation that completed quickly.
class IAsyncResult : public ServiceFramework<IAsyncResult> {
public:
    enum class Mode {
        Plain,    // commands 0 to 3 only
        List,     // the list of the downloadable islands
        Transfer, // the upload or the download of an island
    };

    explicit IAsyncResult(Core::System& system_, Result result, Mode mode = Mode::Plain);
    ~IAsyncResult() override;

    // The items to hand to the game, each one 0x410 bytes (SaveDataBackupInfo).
    void SetEntries(std::vector<std::vector<u8>> entries);

private:
    void Get(HLERequestContext& ctx);
    void Cancel(HLERequestContext& ctx);
    void HasDone(HLERequestContext& ctx);
    void GetSystemEvent(HLERequestContext& ctx);
    void Command100(HLERequestContext& ctx);
    void GetList(HLERequestContext& ctx);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* completion_event;
    Result result;
    Mode mode;
    std::vector<std::vector<u8>> entries;
};

} // namespace Service::OLSC
