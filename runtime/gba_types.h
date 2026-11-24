#pragma once

#include <cstdint>
#include <cstddef>
#include "recomp_abi.h"

namespace gba {

// GBA CPU Modes (CPSR bits 0..4)
enum CpuMode : uint32_t {
    MODE_USR = 0x10,
    MODE_FIQ = 0x11,
    MODE_IRQ = 0x12,
    MODE_SVC = 0x13,
    MODE_ABT = 0x17,
    MODE_UND = 0x1B,
    MODE_SYS = 0x1F,
};

// CPSR Flag bits
constexpr uint32_t FLAG_N = 1u << 31;
constexpr uint32_t FLAG_Z = 1u << 30;
constexpr uint32_t FLAG_C = 1u << 29;
constexpr uint32_t FLAG_V = 1u << 28;
constexpr uint32_t FLAG_I = 1u << 7;
constexpr uint32_t FLAG_F = 1u << 6;
constexpr uint32_t FLAG_T = 1u << 5;

// GBA Keypad bitmask (active LOW in KEYINPUT 0x04000130)
enum GbaButton : uint16_t {
    BTN_A      = 1u << 0,
    BTN_B      = 1u << 1,
    BTN_SELECT = 1u << 2,
    BTN_START  = 1u << 3,
    BTN_RIGHT  = 1u << 4,
    BTN_LEFT   = 1u << 5,
    BTN_UP     = 1u << 6,
    BTN_DOWN   = 1u << 7,
    BTN_R      = 1u << 8,
    BTN_L      = 1u << 9,
};

// Interrupt bit flags (IE / IF)
enum IrqBit : uint16_t {
    IRQ_VBLANK  = 1u << 0,
    IRQ_HBLANK  = 1u << 1,
    IRQ_VCOUNT  = 1u << 2,
    IRQ_TIMER0  = 1u << 3,
    IRQ_TIMER1  = 1u << 4,
    IRQ_TIMER2  = 1u << 5,
    IRQ_TIMER3  = 1u << 6,
    IRQ_SERIAL  = 1u << 7,
    IRQ_DMA0    = 1u << 8,
    IRQ_DMA1    = 1u << 9,
    IRQ_DMA2    = 1u << 10,
    IRQ_DMA3    = 1u << 11,
    IRQ_KEYPAD  = 1u << 12,
    IRQ_GAMEPAK = 1u << 13,
};

constexpr int SCREEN_WIDTH  = 240;
constexpr int SCREEN_HEIGHT = 160;
constexpr int CYCLES_PER_SCANLINE = 1232;
constexpr int HDRAW_CYCLES = 960;
constexpr int HBLANK_CYCLES = 272;
constexpr int SCANLINES_VISIBLE = 160;
constexpr int SCANLINES_TOTAL = 228;
constexpr int CYCLES_PER_FRAME = CYCLES_PER_SCANLINE * SCANLINES_TOTAL; // 280,896
constexpr uint32_t CPU_FREQ_HZ = 16777216;

class GbaBus;

// Signature of every statically recompiled native x86_64 ARM/Thumb function
using RecompFunc = NativeRecompFunc;

struct RecompEntry {
    uint32_t startAddr; // GBA address (without Thumb bit)
    uint32_t endAddr;   // Inclusive max instruction address in function
    uint8_t  isThumb;   // 1 = Thumb, 0 = ARM
    RecompFunc fn;
};

struct RecompRomInfo {
    const char* title;
    const char* gameCode;
    const char* makerCode;
    const char* romFilename;
    uint32_t romSize;
    uint32_t recompiledFunctionCount;
    uint32_t recompiledInstructionCount;
};

// Provided by generated code (recomp_table.cpp & rom_data.cpp)
extern const uint8_t* GetEmbeddedRomData(size_t& outSize);
extern const RecompRomInfo& GetRecompRomInfo();
extern RecompFunc LookupRecompiledFunction(uint32_t addrWithModeBit);

inline uint32_t Ror32(uint32_t val, uint32_t shift) {
    shift &= 31u;
    if (shift == 0) return val;
    return (val >> shift) | (val << (32u - shift));
}

inline int32_t SignExtend(uint32_t val, int bits) {
    uint32_t shift = 32u - static_cast<uint32_t>(bits);
    return static_cast<int32_t>(val << shift) >> shift;
}

} // namespace gba
