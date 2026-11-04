// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/audio/hardware_opus_decoder.h"
#include "core/hle/service/audio/hardware_opus_decoder_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nx2_stub_interface.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <fmt/format.h>

namespace Service::Audio {

using namespace AudioCore::OpusDecoder;

namespace {
// Logs a Switch 2 hwacod request in full: its raw words, every X and A buffer and its handles.
// The protocol is undocumented; these logs are what it is rebuilt from.
void LogHwacodRequest(HLERequestContext& ctx, std::string_view session) {
    const u32* words = ctx.CommandBuffer();
    std::string raw;
    for (u32 i = 0; i < 24; ++i) {
        raw += fmt::format("{:08x} ", words[i]);
    }
    LOG_INFO(Service_Audio, "hwacod {} cmd {}: words {}", session, ctx.GetCommand(), raw);
    const auto dump = [&](std::string_view kind, size_t index, std::span<const u8> data) {
        std::string hex;
        for (size_t i = 0; i < (std::min<size_t>)(data.size(), 512); ++i) {
            hex += fmt::format("{:02x}", data[i]);
            if (i % 4 == 3) {
                hex += ' ';
            }
        }
        LOG_INFO(Service_Audio, "hwacod {} cmd {}: {} buffer {} ({} bytes): {}", session,
                 ctx.GetCommand(), kind, index, data.size(), hex);
    };
    for (size_t i = 0; i < ctx.BufferDescriptorX().size(); ++i) {
        dump("X", i, ctx.ReadBufferX(i));
    }
    for (size_t i = 0; i < ctx.BufferDescriptorA().size(); ++i) {
        dump("A", i, ctx.ReadBufferA(i));
    }
    if (ctx.CanWriteBuffer(0)) {
        LOG_INFO(Service_Audio, "hwacod {} cmd {}: output buffer of {} bytes", session,
                 ctx.GetCommand(), ctx.GetWriteBufferSize(0));
    }
}

// [S2] nn::codec::hwacod::IHardwareAudioCodecManagerSession (IClientSession 10). Commands 0, 1
// and 10 are logged and answered with an empty success.
class IHardwareAudioCodecManagerSession final
    : public ServiceFramework<IHardwareAudioCodecManagerSession> {
public:
    explicit IHardwareAudioCodecManagerSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:hwacod:IHardwareAudioCodecManagerSession"} {
        static const FunctionInfo functions[] = {
            {0, &IHardwareAudioCodecManagerSession::Log, "Cmd0"},
            {1, &IHardwareAudioCodecManagerSession::Log, "Cmd1"},
            {10, &IHardwareAudioCodecManagerSession::Log, "Cmd10"},
        };
        RegisterHandlers(functions);
    }

private:
    void Log(HLERequestContext& ctx) {
        LogHwacodRequest(ctx, "IHardwareAudioCodecManagerSession");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

// [S2] nn::codec::hwacod::IClientSession, opened with hwopus 100 (Cyberpunk 2077 opens it while
// starting its audio; answered without a session, it called a session that did not exist and
// aborted with 2010-0212). Commands 0 and 1 are logged and answered with an empty success; 10
// returns an IHardwareAudioCodecManagerSession.
class IHardwareAudioCodecClientSession final
    : public ServiceFramework<IHardwareAudioCodecClientSession> {
public:
    explicit IHardwareAudioCodecClientSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:hwacod:IClientSession"} {
        static const FunctionInfo functions[] = {
            {0, &IHardwareAudioCodecClientSession::Log, "Cmd0"},
            {1, &IHardwareAudioCodecClientSession::Log, "Cmd1"},
            {10, &IHardwareAudioCodecClientSession::OpenManagerSession, "Cmd10"},
        };
        RegisterHandlers(functions);
    }

private:
    void Log(HLERequestContext& ctx) {
        LogHwacodRequest(ctx, "IClientSession");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void OpenManagerSession(HLERequestContext& ctx) {
        LogHwacodRequest(ctx, "IClientSession");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IHardwareAudioCodecManagerSession>(ctx, system);
    }
};
} // Anonymous namespace

IHardwareOpusDecoderManager::IHardwareOpusDecoderManager(Core::System& system_)
    : ServiceFramework{system_, "hwopus"}, system{system_}, impl{system} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IHardwareOpusDecoderManager::OpenHardwareOpusDecoder>, "OpenHardwareOpusDecoder"},
        {1, D<&IHardwareOpusDecoderManager::GetWorkBufferSize>, "GetWorkBufferSize"},
        {2, D<&IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStream>, "OpenOpusDecoderForMultiStream"},
        {3, D<&IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStream>, "GetWorkBufferSizeForMultiStream"},
        {4, D<&IHardwareOpusDecoderManager::OpenHardwareOpusDecoderEx>, "OpenHardwareOpusDecoderEx"},
        {5, D<&IHardwareOpusDecoderManager::GetWorkBufferSizeEx>, "GetWorkBufferSizeEx"},
        {6, D<&IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStreamEx>, "OpenHardwareOpusDecoderForMultiStreamEx"},
        {7, D<&IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamEx>, "GetWorkBufferSizeForMultiStreamEx"},
        {8, D<&IHardwareOpusDecoderManager::GetWorkBufferSizeExEx>, "GetWorkBufferSizeExEx"},
        {9, D<&IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamExEx>, "GetWorkBufferSizeForMultiStreamExEx"},
        {100, &IHardwareOpusDecoderManager::OpenHardwareAudioCodecSession, "Cmd100"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

IHardwareOpusDecoderManager::~IHardwareOpusDecoderManager() = default;

void IHardwareOpusDecoderManager::OpenHardwareAudioCodecSession(HLERequestContext& ctx) {
    LOG_WARNING(Service_Audio, "(STUBBED) Switch 2 hwopus 100 -> hwacod IClientSession");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IHardwareAudioCodecClientSession>(ctx, system);
}

Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoder(
    Out<SharedPointer<IHardwareOpusDecoder>> out_decoder, OpusParameters params, u32 tmem_size,
    InCopyHandle<Kernel::KTransferMemory> tmem_handle) {
    // Info, not debug: Cyberpunk 2077 opens its decoders with parameters that fail (see
    // OpusParameters' WithDefaultSampleRate); these show what it sends.
    LOG_INFO(Service_Audio, "sample_rate {:#x} channel_count {:#x} transfer_memory_size {:#x}",
             params.sample_rate, params.channel_count, tmem_size);

    auto decoder{std::make_shared<IHardwareOpusDecoder>(system, impl.GetHardwareOpus())};
    OpusParametersEx ex{
        .sample_rate = params.sample_rate,
        .channel_count = params.channel_count,
        .use_large_frame_size = false,
    };
    R_TRY(decoder->Initialize(ex, tmem_handle.Get(), tmem_size));

    *out_decoder = decoder;
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::GetWorkBufferSize(Out<u32> out_size, OpusParameters params) {
    R_TRY(impl.GetWorkBufferSize(params, *out_size));
    LOG_DEBUG(Service_Audio, "sample_rate {} channel_count {} -- returned size {:#x}",
              params.sample_rate, params.channel_count, *out_size);
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStream(
    Out<SharedPointer<IHardwareOpusDecoder>> out_decoder,
    InLargeData<OpusMultiStreamParameters, BufferAttr_HipcPointer> params, u32 tmem_size,
    InCopyHandle<Kernel::KTransferMemory> tmem_handle) {
    LOG_DEBUG(Service_Audio,
              "sample_rate {} channel_count {} total_stream_count {} stereo_stream_count {} "
              "transfer_memory_size {:#x}",
              params->sample_rate, params->channel_count, params->total_stream_count,
              params->stereo_stream_count, tmem_size);

    auto decoder{std::make_shared<IHardwareOpusDecoder>(system, impl.GetHardwareOpus())};

    OpusMultiStreamParametersEx ex{
        .sample_rate = params->sample_rate,
        .channel_count = params->channel_count,
        .total_stream_count = params->total_stream_count,
        .stereo_stream_count = params->stereo_stream_count,
        .use_large_frame_size = false,
        .mappings{},
    };
    std::memcpy(ex.mappings.data(), params->mappings.data(), sizeof(params->mappings));
    R_TRY(decoder->Initialize(ex, tmem_handle.Get(), tmem_size));

    *out_decoder = decoder;
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStream(
    Out<u32> out_size, InLargeData<OpusMultiStreamParameters, BufferAttr_HipcPointer> params) {
    R_TRY(impl.GetWorkBufferSizeForMultiStream(*params, *out_size));
    LOG_DEBUG(Service_Audio, "size {:#x}", *out_size);
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderEx(
    Out<SharedPointer<IHardwareOpusDecoder>> out_decoder, OpusParametersEx params, u32 tmem_size,
    InCopyHandle<Kernel::KTransferMemory> tmem_handle) {
    LOG_DEBUG(Service_Audio, "sample_rate {} channel_count {} transfer_memory_size {:#x}",
              params.sample_rate, params.channel_count, tmem_size);

    auto decoder{std::make_shared<IHardwareOpusDecoder>(system, impl.GetHardwareOpus())};
    R_TRY(decoder->Initialize(params, tmem_handle.Get(), tmem_size));

    *out_decoder = decoder;
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::GetWorkBufferSizeEx(Out<u32> out_size,
                                                        OpusParametersEx params) {
    R_TRY(impl.GetWorkBufferSizeEx(params, *out_size));
    LOG_DEBUG(Service_Audio, "size {:#x}", *out_size);
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStreamEx(
    Out<SharedPointer<IHardwareOpusDecoder>> out_decoder,
    InLargeData<OpusMultiStreamParametersEx, BufferAttr_HipcPointer> params, u32 tmem_size,
    InCopyHandle<Kernel::KTransferMemory> tmem_handle) {
    LOG_DEBUG(Service_Audio,
              "sample_rate {} channel_count {} total_stream_count {} stereo_stream_count {} "
              "use_large_frame_size {}"
              "transfer_memory_size {:#x}",
              params->sample_rate, params->channel_count, params->total_stream_count,
              params->stereo_stream_count, params->use_large_frame_size, tmem_size);

    auto decoder{std::make_shared<IHardwareOpusDecoder>(system, impl.GetHardwareOpus())};

    R_TRY(decoder->Initialize(*params, tmem_handle.Get(), tmem_size));

    *out_decoder = decoder;
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamEx(
    Out<u32> out_size, InLargeData<OpusMultiStreamParametersEx, BufferAttr_HipcPointer> params) {
    R_TRY(impl.GetWorkBufferSizeForMultiStreamEx(*params, *out_size));
    LOG_DEBUG(Service_Audio,
              "sample_rate {} channel_count {} total_stream_count {} stereo_stream_count {} "
              "use_large_frame_size {} -- returned size {:#x}",
              params->sample_rate, params->channel_count, params->total_stream_count,
              params->stereo_stream_count, params->use_large_frame_size, *out_size);
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::GetWorkBufferSizeExEx(Out<u32> out_size,
                                                          OpusParametersEx params) {
    R_TRY(impl.GetWorkBufferSizeExEx(params, *out_size));
    LOG_DEBUG(Service_Audio, "size {:#x}", *out_size);
    R_SUCCEED();
}

Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamExEx(
    Out<u32> out_size, InLargeData<OpusMultiStreamParametersEx, BufferAttr_HipcPointer> params) {
    R_TRY(impl.GetWorkBufferSizeForMultiStreamExEx(*params, *out_size));
    LOG_DEBUG(Service_Audio, "size {:#x}", *out_size);
    R_SUCCEED();
}

} // namespace Service::Audio
