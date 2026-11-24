#pragma once

#include <cstdint>
#include <cstddef>

namespace gba {

class GbaCpu;
struct RuntimeHelpers;

// Native x86_64 statically recompiled function signature (Windows x64 ABI: rcx=cpu, rdx=helpers, al=matched)
using NativeRecompFunc = bool (*)(GbaCpu* cpu, const RuntimeHelpers* helpers);

// Runtime helper function table passed in RDX (cached in R12 inside every recompiled x86_64 function)
struct RuntimeHelpers {
    uint32_t (*read8)(GbaCpu* cpu, uint32_t addr);
    uint32_t (*read16)(GbaCpu* cpu, uint32_t addr);
    uint32_t (*read32)(GbaCpu* cpu, uint32_t addr);
    uint32_t (*read32Rotated)(GbaCpu* cpu, uint32_t addr);
    uint32_t (*read16Rotated)(GbaCpu* cpu, uint32_t addr);
    uint32_t (*read16Signed)(GbaCpu* cpu, uint32_t addr);
    uint32_t (*read8Signed)(GbaCpu* cpu, uint32_t addr);

    void (*write8)(GbaCpu* cpu, uint32_t addr, uint32_t val);
    void (*write16)(GbaCpu* cpu, uint32_t addr, uint32_t val);
    void (*write32)(GbaCpu* cpu, uint32_t addr, uint32_t val);

    uint32_t (*checkCond)(GbaCpu* cpu, uint32_t cond);
    void     (*setNZ)(GbaCpu* cpu, uint32_t val);
    void     (*setNZC)(GbaCpu* cpu, uint32_t val, uint32_t carry);
    uint32_t (*add)(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t setFlags);
    uint32_t (*adc)(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t setFlags);
    uint32_t (*sub)(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t setFlags);
    uint32_t (*sbc)(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t setFlags);
    uint32_t (*shiftImmSetFlags)(GbaCpu* cpu, uint32_t val, uint32_t type, uint32_t amount);
    uint32_t (*shiftRegSetFlags)(GbaCpu* cpu, uint32_t val, uint32_t type, uint32_t amount);

    void (*branchExchange)(GbaCpu* cpu, uint32_t target);
    void (*execSwi)(GbaCpu* cpu, uint32_t swiNum);
    void (*execArm)(GbaCpu* cpu, uint32_t ins);
    void (*execThumb)(GbaCpu* cpu, uint32_t ins);
};

// Fixed binary layout for the self-contained AOT bundle appended to the standalone Windows x64 .exe
constexpr uint64_t AOT_BUNDLE_MAGIC = 0x31544F4152534F41ull; // "AOSRAOT1"

#pragma pack(push, 1)
struct AotFuncRecord {
    uint32_t startAddr;   // GBA start address (without Thumb bit)
    uint32_t endAddr;     // Inclusive max GBA instruction address in function
    uint32_t codeOffset;  // Byte offset inside the native x86_64 code blob
    uint8_t  isThumb;     // 1 = Thumb, 0 = ARM
    uint8_t  reserved[3];
};

struct AotBundleHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t funcCount;
    uint32_t totalInstructions;
    uint32_t codeBlobSize;
    uint32_t romSize;
    char     title[16];
    char     gameCode[8];
    char     makerCode[8];
    char     romFilename[128];
};

struct AotBundleFooter {
    uint64_t bundleOffset; // File offset where AotBundleHeader begins
    uint64_t bundleSize;   // Total size of bundle (header + records + code + rom + footer)
    uint64_t magic;        // AOT_BUNDLE_MAGIC
};
#pragma pack(pop)

// Layout offsets inside GbaCpu (verified by static_assert in gba_cpu.cpp!)
constexpr uint32_t CPU_OFF_R                  = 0;   // uint32_t r[16] (64 bytes)
constexpr uint32_t CPU_OFF_CPSR               = 64;  // uint32_t cpsr
constexpr uint32_t CPU_OFF_CYCLES             = 72;  // uint64_t cycles
constexpr uint32_t CPU_OFF_NEXT_EVENT         = 80;  // uint64_t nextEventCycles
constexpr uint32_t CPU_OFF_STAT_RECOMP_INSTRS = 88;  // uint64_t statRecompInstrs
constexpr uint32_t CPU_OFF_HALTED             = 96;  // uint8_t  halted

} // namespace gba
