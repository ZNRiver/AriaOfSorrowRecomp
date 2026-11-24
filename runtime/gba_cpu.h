#pragma once

#include "gba_types.h"
#include "gba_bus.h"

#include <array>
#include <vector>

namespace gba {

struct SaveState {
    bool valid = false;
    std::array<uint32_t, 16> r{};
    uint32_t cpsr = 0;
    uint32_t spsr[6]{};
    uint32_t bankedUsr[7]{};
    uint32_t bankedFiq[7]{};
    uint32_t bankedIrq[2]{};
    uint32_t bankedSvc[2]{};
    uint32_t bankedAbt[2]{};
    uint32_t bankedUnd[2]{};
    bool halted = false;
    bool intrWaitActive = false;
    uint16_t intrWaitMask = 0;

    std::array<uint8_t, 0x40000> ewram{};
    std::array<uint8_t, 0x8000>  iwram{};
    std::array<uint8_t, 0x400>   io{};
    std::array<uint8_t, 0x400>   pram{};
    std::array<uint8_t, 0x18000> vram{};
    std::array<uint8_t, 0x400>   oam{};
    uint16_t ie = 0, ifReg = 0, ime = 0;
    DmaChannel dma[4]{};
    TimerChannel timers[4]{};
    int32_t bg2RefX = 0, bg2RefY = 0, bg3RefX = 0, bg3RefY = 0;
};

class GbaCpu {
public:
    static constexpr uint32_t BIOS_IRQ_RETURN_ADDR = 0x00000138u;

    explicit GbaCpu(GbaBus& busRef);

    void Reset();
    void RunFrame(bool enableRecomp);

    // Quick save/load state
    void CaptureState(SaveState& out) const;
    bool RestoreState(const SaveState& in);

    // CPU Core State — ABI-pinned layout at offset 0 for direct native x86_64 machine code access!
    uint32_t r[16]{};            // offset 0
    uint32_t cpsr = MODE_SYS;    // offset 64
    uint32_t _abiPad0 = 0;       // offset 68
    uint64_t cycles = 0;         // offset 72
    uint64_t nextEventCycles = 0;// offset 80
    uint64_t statRecompInstrs = 0;// offset 88
    uint8_t  halted = 0;         // offset 96
    bool intrWaitActive = false;
    uint16_t intrWaitMask = 0;

    // Banked registers
    // Index: 0=USR/SYS, 1=FIQ, 2=IRQ, 3=SVC, 4=ABT, 5=UND
    uint32_t spsr[6]{};
    uint32_t bankedUsr[7]{}; // r8..r14
    uint32_t bankedFiq[7]{}; // r8..r14
    uint32_t bankedIrq[2]{}; // r13..r14
    uint32_t bankedSvc[2]{};
    uint32_t bankedAbt[2]{};
    uint32_t bankedUnd[2]{};

    GbaBus& bus;

    // Runtime telemetry for ImGui overlay
    uint64_t statRecompCalls = 0;
    uint64_t statFallbackInstrs = 0;
    uint64_t statFrames = 0;

    // --- Helpers used by BOTH statically recompiled C++ code and fallback executor ---
    inline bool IsThumb() const { return (cpsr & FLAG_T) != 0; }
    inline uint32_t GetMode() const { return cpsr & 0x1Fu; }
    inline bool FlagN() const { return (cpsr & FLAG_N) != 0; }
    inline bool FlagZ() const { return (cpsr & FLAG_Z) != 0; }
    inline bool FlagC() const { return (cpsr & FLAG_C) != 0; }
    inline bool FlagV() const { return (cpsr & FLAG_V) != 0; }

    inline bool CheckCond(uint32_t cond) const {
        switch (cond & 0xFu) {
            case 0x0: return FlagZ();
            case 0x1: return !FlagZ();
            case 0x2: return FlagC();
            case 0x3: return !FlagC();
            case 0x4: return FlagN();
            case 0x5: return !FlagN();
            case 0x6: return FlagV();
            case 0x7: return !FlagV();
            case 0x8: return FlagC() && !FlagZ();
            case 0x9: return !FlagC() || FlagZ();
            case 0xA: return FlagN() == FlagV();
            case 0xB: return FlagN() != FlagV();
            case 0xC: return !FlagZ() && (FlagN() == FlagV());
            case 0xD: return FlagZ() || (FlagN() != FlagV());
            case 0xE: return true;
            default:  return false;
        }
    }

    inline void SetNZ(uint32_t v) {
        cpsr = (cpsr & ~(FLAG_N | FLAG_Z)) |
               (v & 0x80000000u) |
               (v == 0 ? FLAG_Z : 0u);
    }

    inline void SetNZC(uint32_t v, bool carry) {
        cpsr = (cpsr & ~(FLAG_N | FLAG_Z | FLAG_C)) |
               (v & 0x80000000u) |
               (v == 0 ? FLAG_Z : 0u) |
               (carry ? FLAG_C : 0u);
    }

    inline uint32_t Add(uint32_t a, uint32_t b, bool setFlags) {
        uint64_t res64 = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
        uint32_t res = static_cast<uint32_t>(res64);
        if (setFlags) {
            bool carry = (res64 >> 32) != 0;
            bool overflow = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
            cpsr = (cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
                   (res & 0x80000000u) | (res == 0 ? FLAG_Z : 0u) |
                   (carry ? FLAG_C : 0u) | (overflow ? FLAG_V : 0u);
        }
        return res;
    }

    inline uint32_t Adc(uint32_t a, uint32_t b, bool setFlags) {
        uint64_t c = FlagC() ? 1ull : 0ull;
        uint64_t res64 = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + c;
        uint32_t res = static_cast<uint32_t>(res64);
        if (setFlags) {
            bool carry = (res64 >> 32) != 0;
            bool overflow = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
            cpsr = (cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
                   (res & 0x80000000u) | (res == 0 ? FLAG_Z : 0u) |
                   (carry ? FLAG_C : 0u) | (overflow ? FLAG_V : 0u);
        }
        return res;
    }

    inline uint32_t Sub(uint32_t a, uint32_t b, bool setFlags) {
        uint32_t res = a - b;
        if (setFlags) {
            bool carry = (a >= b);
            bool overflow = (((a ^ b) & (a ^ res)) >> 31) != 0;
            cpsr = (cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
                   (res & 0x80000000u) | (res == 0 ? FLAG_Z : 0u) |
                   (carry ? FLAG_C : 0u) | (overflow ? FLAG_V : 0u);
        }
        return res;
    }

    inline uint32_t Sbc(uint32_t a, uint32_t b, bool setFlags) {
        uint64_t borrow = FlagC() ? 0ull : 1ull;
        uint64_t res64 = static_cast<uint64_t>(a) - static_cast<uint64_t>(b) - borrow;
        uint32_t res = static_cast<uint32_t>(res64);
        if (setFlags) {
            bool carry = (static_cast<uint64_t>(a) >= static_cast<uint64_t>(b) + borrow);
            bool overflow = (((a ^ b) & (a ^ res)) >> 31) != 0;
            cpsr = (cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
                   (res & 0x80000000u) | (res == 0 ? FLAG_Z : 0u) |
                   (carry ? FLAG_C : 0u) | (overflow ? FLAG_V : 0u);
        }
        return res;
    }

    uint32_t ShiftImm(uint32_t val, uint32_t type, uint32_t amount, bool& carryOut) const;
    uint32_t ShiftReg(uint32_t val, uint32_t type, uint32_t amount, bool& carryOut) const;

    void SwitchMode(uint32_t newMode);
    uint32_t GetCurrentSpsr() const;
    void SetCurrentSpsr(uint32_t val);

    inline void BranchExchange(uint32_t target) {
        if (target & 1u) {
            cpsr |= FLAG_T;
            r[15] = target & ~1u;
        } else {
            cpsr &= ~FLAG_T;
            r[15] = target & ~3u;
        }
    }

    // Memory helpers with ARM7TDMI unaligned rotation semantics
    inline uint32_t Read32Rotated(uint32_t addr) {
        uint32_t v = bus.Read32(addr);
        uint32_t rot = (addr & 3u) * 8u;
        return rot ? Ror32(v, rot) : v;
    }
    inline uint32_t Read16Rotated(uint32_t addr) {
        uint32_t v = bus.Read16(addr);
        return (addr & 1u) ? Ror32(v, 8u) : v;
    }
    inline uint32_t Read16Signed(uint32_t addr) {
        if (addr & 1u) return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(bus.Read8(addr))));
        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(bus.Read16(addr))));
    }
    inline uint32_t Read8Signed(uint32_t addr) {
        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(bus.Read8(addr))));
    }

    void ExecSwi(uint8_t num);
    void ExecArm(uint32_t instr);
    void ExecThumb(uint16_t instr);

private:
    static int ModeToBankIndex(uint32_t mode);
    void EnterBiosIrq();
    void ExitBiosIrq();
    void RunUntil(uint64_t targetCycles, bool enableRecomp);
};

} // namespace gba
