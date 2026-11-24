#pragma once

#include "gba_types.h"
#include "gba_ppu.h"
#include "gba_apu.h"

#include <array>
#include <string>
#include <vector>

namespace gba {

class GbaCpu;

struct DmaChannel {
    uint32_t sad = 0;
    uint32_t dad = 0;
    uint16_t cntL = 0;
    uint16_t cntH = 0;
    uint32_t internalSrc = 0;
    uint32_t internalDst = 0;
    uint32_t internalCnt = 0;
};

struct TimerChannel {
    uint16_t reload = 0;
    uint16_t cnt = 0;
    uint16_t counter = 0;
    uint32_t prescalerAccum = 0;
};

class GbaBus {
public:
    GbaBus();

    void AttachRom(const uint8_t* data, size_t size);
    void Reset();

    // Memory access
    uint8_t  Read8(uint32_t addr);
    uint16_t Read16(uint32_t addr);
    uint32_t Read32(uint32_t addr);

    void Write8(uint32_t addr, uint8_t val);
    void Write16(uint32_t addr, uint16_t val);
    void Write32(uint32_t addr, uint32_t val);

    // Direct I/O helpers for PPU/APU/BIOS
    inline uint16_t ReadIo16Direct(uint32_t offset) const {
        if (offset + 1 >= io.size()) return 0;
        return static_cast<uint16_t>(io[offset] | (io[offset + 1] << 8));
    }
    inline uint32_t ReadIo32Direct(uint32_t offset) const {
        if (offset + 3 >= io.size()) return 0;
        return static_cast<uint32_t>(io[offset]) |
               (static_cast<uint32_t>(io[offset + 1]) << 8) |
               (static_cast<uint32_t>(io[offset + 2]) << 16) |
               (static_cast<uint32_t>(io[offset + 3]) << 24);
    }
    inline void WriteIo16Direct(uint32_t offset, uint16_t val) {
        if (offset + 1 >= io.size()) return;
        io[offset] = static_cast<uint8_t>(val & 0xFF);
        io[offset + 1] = static_cast<uint8_t>(val >> 8);
    }

    // Hardware stepping (interleaves Timers + APU in fine increments for accurate audio)
    void StepTimers(uint32_t cycles);
    void StepHardware(uint32_t cycles);
    void RequestIrq(uint16_t irqBit);
    bool HasPendingIrq() const;
    void SetKeypadState(uint16_t pressedMask); // bit=1 means pressed

    // DMA triggers
    void TriggerDma(int timingMode);        // 0=Immediate, 1=VBlank, 2=HBlank
    void TriggerDmaFifo(uint32_t fifoAddr); // 0x040000A0 (FIFO A) or 0x040000A4 (FIFO B)

    // SRAM persistence
    bool LoadSram(const std::string& path);
    bool SaveSram(const std::string& path) const;
    bool sramDirty = false;

    // Memory regions
    std::array<uint8_t, 0x40000> ewram{}; // 256 KB
    std::array<uint8_t, 0x8000>  iwram{}; // 32 KB
    std::array<uint8_t, 0x400>   io{};    // 1 KB
    std::array<uint8_t, 0x400>   pram{};  // 1 KB
    std::array<uint8_t, 0x18000> vram{};  // 96 KB
    std::array<uint8_t, 0x400>   oam{};   // 1 KB
    std::array<uint8_t, 0x10000> sram{};  // 64 KB (32 KB used by Aria of Sorrow)

    const uint8_t* romData = nullptr;
    size_t romSize = 0;

    // Interrupt & keypad state
    uint16_t ie = 0;
    uint16_t ifReg = 0;
    uint16_t ime = 0;
    uint16_t keyInput = 0x03FF; // Active low: 1 = released, 0 = pressed
    uint8_t  postflg = 0;

    // Subsystems
    DmaChannel dma[4]{};
    TimerChannel timers[4]{};
    GbaPpu ppu;
    GbaApu apu;

    // CPU pointer for halt wakeup
    GbaCpu* cpu = nullptr;

    // BIOS last read value (for protected reads)
    uint32_t biosLastRead = 0xE129F000u;

private:
    void ExecuteDma(int ch);
    void WriteIo8(uint32_t offset, uint8_t val);
    void WriteIo16(uint32_t offset, uint16_t val);
    void WriteIo32(uint32_t offset, uint32_t val);
    uint8_t ReadIo8(uint32_t offset);
    uint16_t ReadIo16(uint32_t offset);
};

} // namespace gba
