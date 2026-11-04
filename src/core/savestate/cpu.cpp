// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/savestate/cpu.h"

namespace Core::SaveState {
namespace {
void CheckArchitecture(Architecture architecture) {
    if (architecture != Architecture::AArch64 && architecture != Architecture::AArch32) {
        throw Error{"Unsupported state CPU architecture"};
    }
}
} // namespace

std::vector<u8> EncodeThreadRegisters(const ThreadRegisters& thread) {
    CheckArchitecture(thread.architecture);
    Encoder output;
    output.U32(1);
    output.U32(thread.architecture == Architecture::AArch64 ? 64 : 32);
    output.U64(thread.process_id);
    output.U64(thread.thread_id);
    output.U64(thread.tpidrro);
    const auto& context = thread.context;
    for (const auto value : context.r) {
        output.U64(value);
    }
    output.U64(context.fp);
    output.U64(context.lr);
    output.U64(context.sp);
    output.U64(context.pc);
    output.U32(context.pstate);
    for (const auto& value : context.v) {
        output.U64(value[0]);
        output.U64(value[1]);
    }
    output.U32(context.fpcr);
    output.U32(context.fpsr);
    output.U64(context.tpidr);
    return std::move(output.data);
}

ThreadRegisters DecodeThreadRegisters(std::span<const u8> bytes) {
    Decoder input{bytes};
    if (input.U32() != 1) {
        throw Error{"Unsupported CPU state version"};
    }
    ThreadRegisters thread;
    const u32 architecture = input.U32();
    if (architecture != 64 && architecture != 32) {
        throw Error{"Unsupported state CPU architecture"};
    }
    thread.architecture = architecture == 64 ? Architecture::AArch64 : Architecture::AArch32;
    thread.process_id = input.U64();
    thread.thread_id = input.U64();
    thread.tpidrro = input.U64();
    auto& context = thread.context;
    for (auto& value : context.r) {
        value = input.U64();
    }
    context.fp = input.U64();
    context.lr = input.U64();
    context.sp = input.U64();
    context.pc = input.U64();
    context.pstate = input.U32();
    for (auto& value : context.v) {
        value[0] = input.U64();
        value[1] = input.U64();
    }
    context.fpcr = input.U32();
    context.fpsr = input.U32();
    context.tpidr = input.U64();
    input.Finish();
    return thread;
}

} // namespace Core::SaveState
