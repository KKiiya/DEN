// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/olsc/olsc_async_result.h"

#include <array>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::OLSC {

IAsyncResult::IAsyncResult(Core::System& system_, Result result_, Mode mode_)
    : ServiceFramework{system_, "IAsyncResult"}, service_context{system_, "IAsyncResult"},
      result{result_}, mode{mode_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IAsyncResult::Get, "Get"},
        {1, &IAsyncResult::Cancel, "Cancel"},
        {2, &IAsyncResult::HasDone, "HasDone"},
        {3, &IAsyncResult::GetSystemEvent, "GetSystemEvent"},
        {100, &IAsyncResult::Command100, "GetCountOrProgress"},
        {101, &IAsyncResult::GetList, "GetList"},
    };
    // clang-format on

    RegisterHandlers(functions);
    RegisterHandlersTipc(functions);

    completion_event = service_context.CreateEvent("IAsyncResult:CompletionEvent");
    // The operation is already over when the object is handed to the game.
    completion_event->Signal(system.Kernel());
}

IAsyncResult::~IAsyncResult() {
    service_context.CloseEvent(completion_event);
}

void IAsyncResult::Get(HLERequestContext& ctx) {
    LOG_DEBUG(Service_OLSC, "called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void IAsyncResult::Cancel(HLERequestContext& ctx) {
    LOG_DEBUG(Service_OLSC, "called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IAsyncResult::HasDone(HLERequestContext& ctx) {
    LOG_DEBUG(Service_OLSC, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(true);
}

void IAsyncResult::GetSystemEvent(HLERequestContext& ctx) {
    LOG_DEBUG(Service_OLSC, "called");

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, completion_event->GetReadableEvent());
}

void IAsyncResult::Command100(HLERequestContext& ctx) {
    if (mode == Mode::Transfer) {
        // The progress of the transfer: ACNH doesn't read it.
        LOG_DEBUG(Service_OLSC, "called (progress)");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        rb.PushRaw(std::array<u8, 0x30>{});
        return;
    }

    LOG_DEBUG(Service_OLSC, "called (count)");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(static_cast<u32>(entries.size()));
}

void IAsyncResult::GetList(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 offset = rp.Pop<u32>();

    // The items are written from the start of the output buffer, starting at the requested index.
    std::vector<u8> buffer;
    u32 written = 0;
    const std::size_t capacity = ctx.GetWriteBufferSize();
    for (u64 i = offset; i < entries.size(); i++) {
        if (buffer.size() + entries[i].size() > capacity) {
            break;
        }
        buffer.insert(buffer.end(), entries[i].begin(), entries[i].end());
        written++;
    }
    if (!buffer.empty()) {
        ctx.WriteBuffer(buffer);
    }

    LOG_DEBUG(Service_OLSC, "called, offset={} written={}", offset, written);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(written);
}

void IAsyncResult::SetEntries(std::vector<std::vector<u8>> entries_) {
    entries = std::move(entries_);
}

} // namespace Service::OLSC
