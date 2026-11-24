#include "gba_bus.h"
#include "gba_cpu.h"

#include <cstring>
#include <fstream>

namespace gba {

GbaBus::GbaBus() {
    Reset();
}

void GbaBus::AttachRom(const uint8_t* data, size_t size) {
    romData = data;
    romSize = size;
}

void GbaBus::Reset() {
    ewram.fill(0);
    iwram.fill(0);
    io.fill(0);
    pram.fill(0);
    vram.fill(0);
    oam.fill(0);
    // Keep SRAM intact across soft resets unless never loaded

    ie = 0;
    ifReg = 0;
    ime = 0;
    keyInput = 0x03FF;
    postflg = 1;
    biosLastRead = 0xE129F000u;

    for (auto& d : dma) d = DmaChannel{};
    for (auto& t : timers) t = TimerChannel{};

    // Default BG affine identity matrices (PA=0x100, PD=0x100)
    WriteIo16Direct(0x020, 0x0100);
    WriteIo16Direct(0x026, 0x0100);
    WriteIo16Direct(0x030, 0x0100);
    WriteIo16Direct(0x036, 0x0100);

    // KEYINPUT default all released
    WriteIo16Direct(0x130, 0x03FF);

    ppu.Reset();
    apu.Reset();
}

void GbaBus::SetKeypadState(uint16_t pressedMask) {
    keyInput = static_cast<uint16_t>((~pressedMask) & 0x03FFu);
    WriteIo16Direct(0x130, keyInput);
}

void GbaBus::RequestIrq(uint16_t irqBit) {
    ifReg |= irqBit;
    if (cpu && (ie & ifReg)) {
        cpu->halted = false;
    }
}

bool GbaBus::HasPendingIrq() const {
    return (ime & 1u) != 0 && (ie & ifReg) != 0;
}

bool GbaBus::LoadSram(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        sram.fill(0xFF);
        return false;
    }
    sram.fill(0xFF);
    in.read(reinterpret_cast<char*>(sram.data()), static_cast<std::streamsize>(sram.size()));
    sramDirty = false;
    return true;
}

bool GbaBus::SaveSram(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(sram.data()), static_cast<std::streamsize>(sram.size()));
    return true;
}

uint8_t GbaBus::ReadIo8(uint32_t offset) {
    uint16_t v = ReadIo16(offset & ~1u);
    return (offset & 1u) ? static_cast<uint8_t>(v >> 8) : static_cast<uint8_t>(v & 0xFF);
}

uint16_t GbaBus::ReadIo16(uint32_t offset) {
    if (offset >= 0x400) return 0;
    switch (offset) {
        case 0x100: return timers[0].counter;
        case 0x102: return timers[0].cnt;
        case 0x104: return timers[1].counter;
        case 0x106: return timers[1].cnt;
        case 0x108: return timers[2].counter;
        case 0x10A: return timers[2].cnt;
        case 0x10C: return timers[3].counter;
        case 0x10E: return timers[3].cnt;
        case 0x0BA: return dma[0].cntH;
        case 0x0C6: return dma[1].cntH;
        case 0x0D2: return dma[2].cntH;
        case 0x0DE: return dma[3].cntH;
        case 0x130: return keyInput;
        case 0x200: return ie;
        case 0x202: return ifReg;
        case 0x208: return ime;
        case 0x300: return postflg;
        default:    return ReadIo16Direct(offset);
    }
}

void GbaBus::WriteIo8(uint32_t offset, uint8_t val) {
    if (offset >= 0x400) return;
    if (offset == 0x301) {
        // HALTCNT
        if (cpu) cpu->halted = true;
        return;
    }
    if (offset == 0x300) {
        postflg = val;
        io[0x300] = val;
        return;
    }
    uint32_t aligned = offset & ~1u;
    uint16_t cur = ReadIo16(aligned);
    if (offset & 1u) cur = static_cast<uint16_t>((cur & 0x00FFu) | (static_cast<uint16_t>(val) << 8));
    else             cur = static_cast<uint16_t>((cur & 0xFF00u) | val);
    WriteIo16(aligned, cur);
}

void GbaBus::WriteIo16(uint32_t offset, uint16_t val) {
    if (offset >= 0x400) return;

    switch (offset) {
        case 0x004: { // DISPSTAT (bits 0-2 are read-only)
            uint16_t cur = ReadIo16Direct(0x004);
            WriteIo16Direct(0x004, static_cast<uint16_t>((cur & 0x0007u) | (val & 0xFFF8u)));
            return;
        }
        case 0x006: // VCOUNT is read-only
            return;

        case 0x028: case 0x02A: { // BG2X
            WriteIo16Direct(offset, val);
            ppu.bg2RefX = SignExtend(ReadIo32Direct(0x028), 28);
            return;
        }
        case 0x02C: case 0x02E: { // BG2Y
            WriteIo16Direct(offset, val);
            ppu.bg2RefY = SignExtend(ReadIo32Direct(0x02C), 28);
            return;
        }
        case 0x038: case 0x03A: { // BG3X
            WriteIo16Direct(offset, val);
            ppu.bg3RefX = SignExtend(ReadIo32Direct(0x038), 28);
            return;
        }
        case 0x03C: case 0x03E: { // BG3Y
            WriteIo16Direct(offset, val);
            ppu.bg3RefY = SignExtend(ReadIo32Direct(0x03C), 28);
            return;
        }

        case 0x082: { // SOUNDCNT_H
            if (val & (1u << 11)) apu.ResetFifoA();
            if (val & (1u << 15)) apu.ResetFifoB();
            WriteIo16Direct(offset, val & 0x770Fu);
            return;
        }
        case 0x084: { // SOUNDCNT_X
            uint16_t cur = ReadIo16Direct(0x084);
            WriteIo16Direct(0x084, static_cast<uint16_t>((cur & 0x000Fu) | (val & 0x0080u)));
            return;
        }

        // DMA registers
        case 0x0B0: dma[0].sad = (dma[0].sad & 0xFFFF0000u) | val; break;
        case 0x0B2: dma[0].sad = (dma[0].sad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0B4: dma[0].dad = (dma[0].dad & 0xFFFF0000u) | val; break;
        case 0x0B6: dma[0].dad = (dma[0].dad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0B8: dma[0].cntL = val; break;
        case 0x0BA: {
            bool wasEnabled = (dma[0].cntH & 0x8000u) != 0;
            dma[0].cntH = val;
            if (!wasEnabled && (val & 0x8000u)) {
                dma[0].internalSrc = dma[0].sad & 0x07FFFFFFu;
                dma[0].internalDst = dma[0].dad & 0x07FFFFFFu;
                dma[0].internalCnt = dma[0].cntL & 0x3FFFu;
                if (dma[0].internalCnt == 0) dma[0].internalCnt = 0x4000u;
                if (((val >> 12) & 3u) == 0) ExecuteDma(0);
            }
            break;
        }

        case 0x0BC: dma[1].sad = (dma[1].sad & 0xFFFF0000u) | val; break;
        case 0x0BE: dma[1].sad = (dma[1].sad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0C0: dma[1].dad = (dma[1].dad & 0xFFFF0000u) | val; break;
        case 0x0C2: dma[1].dad = (dma[1].dad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0C4: dma[1].cntL = val; break;
        case 0x0C6: {
            bool wasEnabled = (dma[1].cntH & 0x8000u) != 0;
            dma[1].cntH = val;
            if (!wasEnabled && (val & 0x8000u)) {
                dma[1].internalSrc = dma[1].sad & 0x0FFFFFFFu;
                dma[1].internalDst = dma[1].dad & 0x07FFFFFFu;
                dma[1].internalCnt = dma[1].cntL & 0x3FFFu;
                if (dma[1].internalCnt == 0) dma[1].internalCnt = 0x4000u;
                if (((val >> 12) & 3u) == 0) ExecuteDma(1);
            }
            break;
        }

        case 0x0C8: dma[2].sad = (dma[2].sad & 0xFFFF0000u) | val; break;
        case 0x0CA: dma[2].sad = (dma[2].sad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0CC: dma[2].dad = (dma[2].dad & 0xFFFF0000u) | val; break;
        case 0x0CE: dma[2].dad = (dma[2].dad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0D0: dma[2].cntL = val; break;
        case 0x0D2: {
            bool wasEnabled = (dma[2].cntH & 0x8000u) != 0;
            dma[2].cntH = val;
            if (!wasEnabled && (val & 0x8000u)) {
                dma[2].internalSrc = dma[2].sad & 0x0FFFFFFFu;
                dma[2].internalDst = dma[2].dad & 0x07FFFFFFu;
                dma[2].internalCnt = dma[2].cntL & 0x3FFFu;
                if (dma[2].internalCnt == 0) dma[2].internalCnt = 0x4000u;
                if (((val >> 12) & 3u) == 0) ExecuteDma(2);
            }
            break;
        }

        case 0x0D4: dma[3].sad = (dma[3].sad & 0xFFFF0000u) | val; break;
        case 0x0D6: dma[3].sad = (dma[3].sad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0D8: dma[3].dad = (dma[3].dad & 0xFFFF0000u) | val; break;
        case 0x0DA: dma[3].dad = (dma[3].dad & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16); break;
        case 0x0DC: dma[3].cntL = val; break;
        case 0x0DE: {
            bool wasEnabled = (dma[3].cntH & 0x8000u) != 0;
            dma[3].cntH = val;
            if (!wasEnabled && (val & 0x8000u)) {
                dma[3].internalSrc = dma[3].sad & 0x0FFFFFFFu;
                dma[3].internalDst = dma[3].dad & 0x0FFFFFFFu;
                dma[3].internalCnt = dma[3].cntL;
                if (dma[3].internalCnt == 0) dma[3].internalCnt = 0x10000u;
                if (((val >> 12) & 3u) == 0) ExecuteDma(3);
            }
            break;
        }

        // Timers
        case 0x100: timers[0].reload = val; break;
        case 0x102: {
            bool wasEn = (timers[0].cnt & 0x80u) != 0;
            timers[0].cnt = val;
            if (!wasEn && (val & 0x80u)) { timers[0].counter = timers[0].reload; timers[0].prescalerAccum = 0; }
            break;
        }
        case 0x104: timers[1].reload = val; break;
        case 0x106: {
            bool wasEn = (timers[1].cnt & 0x80u) != 0;
            timers[1].cnt = val;
            if (!wasEn && (val & 0x80u)) { timers[1].counter = timers[1].reload; timers[1].prescalerAccum = 0; }
            break;
        }
        case 0x108: timers[2].reload = val; break;
        case 0x10A: {
            bool wasEn = (timers[2].cnt & 0x80u) != 0;
            timers[2].cnt = val;
            if (!wasEn && (val & 0x80u)) { timers[2].counter = timers[2].reload; timers[2].prescalerAccum = 0; }
            break;
        }
        case 0x10C: timers[3].reload = val; break;
        case 0x10E: {
            bool wasEn = (timers[3].cnt & 0x80u) != 0;
            timers[3].cnt = val;
            if (!wasEn && (val & 0x80u)) { timers[3].counter = timers[3].reload; timers[3].prescalerAccum = 0; }
            break;
        }

        // Interrupts
        case 0x200: ie = val; break;
        case 0x202: ifReg &= static_cast<uint16_t>(~val); break; // Write 1 to acknowledge
        case 0x208: ime = val & 1u; break;

        case 0x300: {
            postflg = static_cast<uint8_t>(val & 0xFF);
            if (val & 0xFF00u) {
                if (cpu) cpu->halted = true;
            }
            break;
        }

        default:
            break;
    }

    WriteIo16Direct(offset, val);
}

void GbaBus::WriteIo32(uint32_t offset, uint32_t val) {
    if (offset == 0x0A0) {
        apu.PushFifoA(val);
        return;
    }
    if (offset == 0x0A4) {
        apu.PushFifoB(val);
        return;
    }
    WriteIo16(offset, static_cast<uint16_t>(val & 0xFFFFu));
    WriteIo16(offset + 2, static_cast<uint16_t>(val >> 16));
}

void GbaBus::ExecuteDma(int ch) {
    DmaChannel& d = dma[ch];
    bool is32 = (d.cntH & (1u << 10)) != 0;
    uint32_t step = is32 ? 4u : 2u;
    uint32_t dstCtrl = (d.cntH >> 5) & 3u;
    uint32_t srcCtrl = (d.cntH >> 7) & 3u;
    uint32_t timing  = (d.cntH >> 12) & 3u;
    bool repeat = (d.cntH & (1u << 9)) != 0;

    uint32_t count = d.internalCnt;
    bool isFifoDma = (timing == 3 && (ch == 1 || ch == 2));
    if (isFifoDma) {
        is32 = true;
        step = 4;
        count = 4;
        dstCtrl = 2; // Fixed FIFO address
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (is32) {
            uint32_t v = Read32(d.internalSrc & ~3u);
            Write32(d.internalDst & ~3u, v);
        } else {
            uint16_t v = Read16(d.internalSrc & ~1u);
            Write16(d.internalDst & ~1u, v);
        }

        if (srcCtrl == 0)      d.internalSrc += step;
        else if (srcCtrl == 1) d.internalSrc -= step;

        if (dstCtrl == 0 || dstCtrl == 3) d.internalDst += step;
        else if (dstCtrl == 1)            d.internalDst -= step;
    }

    if (d.cntH & (1u << 14)) {
        RequestIrq(static_cast<uint16_t>(IRQ_DMA0 << ch));
    }

    if (repeat && timing != 0) {
        if (ch == 3) {
            d.internalCnt = d.cntL ? d.cntL : 0x10000u;
        } else {
            d.internalCnt = (d.cntL & 0x3FFFu) ? (d.cntL & 0x3FFFu) : 0x4000u;
        }
        if (dstCtrl == 3 && !isFifoDma) {
            d.internalDst = d.dad & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
        }
    } else {
        d.cntH &= 0x7FFFu; // Disable DMA
    }
}

void GbaBus::TriggerDma(int timingMode) {
    for (int ch = 0; ch < 4; ++ch) {
        if ((dma[ch].cntH & 0x8000u) && (((dma[ch].cntH >> 12) & 3u) == static_cast<uint32_t>(timingMode))) {
            ExecuteDma(ch);
        }
    }
}

void GbaBus::TriggerDmaFifo(uint32_t fifoAddr) {
    for (int ch = 1; ch <= 2; ++ch) {
        if ((dma[ch].cntH & 0x8000u) &&
            (((dma[ch].cntH >> 12) & 3u) == 3u) &&
            ((dma[ch].dad & 0x0FFFFFFCu) == fifoAddr)) {
            ExecuteDma(ch);
        }
    }
}

void GbaBus::StepHardware(uint32_t cycles) {
    constexpr uint32_t kStepChunk = 64u;
    while (cycles > 0) {
        uint32_t step = (cycles > kStepChunk) ? kStepChunk : cycles;
        StepTimers(step);
        apu.StepCycles(*this, step);
        cycles -= step;
    }
}

void GbaBus::StepTimers(uint32_t cycles) {
    static constexpr uint32_t kPrescaler[4] = { 1, 64, 256, 1024 };
    bool prevOverflow = false;

    for (int i = 0; i < 4; ++i) {
        TimerChannel& t = timers[i];
        bool overflowThis = false;

        if (t.cnt & 0x80u) {
            bool cascade = (i > 0) && ((t.cnt & 0x04u) != 0);
            uint32_t ticks = 0;
            if (cascade) {
                ticks = prevOverflow ? 1u : 0u;
            } else {
                uint32_t div = kPrescaler[t.cnt & 3u];
                t.prescalerAccum += cycles;
                ticks = t.prescalerAccum / div;
                t.prescalerAccum %= div;
            }

            if (ticks > 0) {
                uint32_t next = static_cast<uint32_t>(t.counter) + ticks;
                while (next >= 0x10000u) {
                    overflowThis = true;
                    next = (next - 0x10000u) + t.reload;
                    if (t.cnt & 0x40u) {
                        RequestIrq(static_cast<uint16_t>(IRQ_TIMER0 << i));
                    }
                    if (i == 0 || i == 1) {
                        apu.OnTimerOverflow(*this, i);
                    }
                    if (t.reload == 0xFFFFu && next >= 0x10000u) {
                        next = t.reload;
                        break;
                    }
                }
                t.counter = static_cast<uint16_t>(next);
            }
        }
        prevOverflow = overflowThis;
    }
}

uint8_t GbaBus::Read8(uint32_t addr) {
    uint32_t region = addr >> 24;
    switch (region) {
        case 0x00:
            if (addr < 0x4000u) {
                return static_cast<uint8_t>((biosLastRead >> ((addr & 3u) * 8u)) & 0xFFu);
            }
            return 0;
        case 0x02:
            return ewram[addr & 0x3FFFFu];
        case 0x03:
            return iwram[addr & 0x7FFFu];
        case 0x04:
            return ReadIo8(addr & 0x3FFu);
        case 0x05:
            return pram[addr & 0x3FFu];
        case 0x06: {
            uint32_t off = addr & 0x1FFFFu;
            if (off >= 0x18000u) off -= 0x8000u;
            return vram[off];
        }
        case 0x07:
            return oam[addr & 0x3FFu];
        case 0x08: case 0x09:
        case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: {
            uint32_t off = addr & 0x01FFFFFFu;
            if (romData && off < romSize) return romData[off];
            return static_cast<uint8_t>((addr >> 1) & 0xFFu);
        }
        case 0x0E: case 0x0F:
            return sram[addr & 0xFFFFu];
        default:
            return 0;
    }
}

uint16_t GbaBus::Read16(uint32_t addr) {
    addr &= ~1u;
    uint32_t region = addr >> 24;
    switch (region) {
        case 0x00:
            if (addr < 0x4000u) {
                return static_cast<uint16_t>((biosLastRead >> ((addr & 2u) * 8u)) & 0xFFFFu);
            }
            return 0;
        case 0x02: {
            uint32_t off = addr & 0x3FFFEu;
            return static_cast<uint16_t>(ewram[off] | (ewram[off + 1] << 8));
        }
        case 0x03: {
            uint32_t off = addr & 0x7FFEu;
            return static_cast<uint16_t>(iwram[off] | (iwram[off + 1] << 8));
        }
        case 0x04:
            return ReadIo16(addr & 0x3FEu);
        case 0x05: {
            uint32_t off = addr & 0x3FEu;
            return static_cast<uint16_t>(pram[off] | (pram[off + 1] << 8));
        }
        case 0x06: {
            uint32_t off = addr & 0x1FFFEu;
            if (off >= 0x18000u) off -= 0x8000u;
            return static_cast<uint16_t>(vram[off] | (vram[off + 1] << 8));
        }
        case 0x07: {
            uint32_t off = addr & 0x3FEu;
            return static_cast<uint16_t>(oam[off] | (oam[off + 1] << 8));
        }
        case 0x08: case 0x09:
        case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: {
            uint32_t off = addr & 0x01FFFFFEu;
            if (romData && off + 1 < romSize) {
                return static_cast<uint16_t>(romData[off] | (romData[off + 1] << 8));
            }
            return static_cast<uint16_t>((addr >> 1) & 0xFFFFu);
        }
        case 0x0E: case 0x0F: {
            uint8_t b = sram[addr & 0xFFFFu];
            return static_cast<uint16_t>(b | (b << 8));
        }
        default:
            return 0;
    }
}

uint32_t GbaBus::Read32(uint32_t addr) {
    addr &= ~3u;
    uint32_t region = addr >> 24;
    switch (region) {
        case 0x00:
            return (addr < 0x4000u) ? biosLastRead : 0u;
        case 0x02: {
            uint32_t off = addr & 0x3FFFCu;
            uint32_t v;
            std::memcpy(&v, ewram.data() + off, 4);
            return v;
        }
        case 0x03: {
            uint32_t off = addr & 0x7FFCu;
            uint32_t v;
            std::memcpy(&v, iwram.data() + off, 4);
            return v;
        }
        case 0x04: {
            uint32_t off = addr & 0x3FCu;
            return static_cast<uint32_t>(ReadIo16(off)) |
                   (static_cast<uint32_t>(ReadIo16(off + 2)) << 16);
        }
        case 0x05: {
            uint32_t off = addr & 0x3FCu;
            uint32_t v;
            std::memcpy(&v, pram.data() + off, 4);
            return v;
        }
        case 0x06: {
            uint32_t off = addr & 0x1FFFCu;
            if (off >= 0x18000u) off -= 0x8000u;
            uint32_t v;
            std::memcpy(&v, vram.data() + off, 4);
            return v;
        }
        case 0x07: {
            uint32_t off = addr & 0x3FCu;
            uint32_t v;
            std::memcpy(&v, oam.data() + off, 4);
            return v;
        }
        case 0x08: case 0x09:
        case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: {
            uint32_t off = addr & 0x01FFFFFCu;
            if (romData && off + 3 < romSize) {
                uint32_t v;
                std::memcpy(&v, romData + off, 4);
                return v;
            }
            uint16_t lo = static_cast<uint16_t>((addr >> 1) & 0xFFFFu);
            uint16_t hi = static_cast<uint16_t>(((addr + 2) >> 1) & 0xFFFFu);
            return static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
        }
        case 0x0E: case 0x0F: {
            uint32_t b = sram[addr & 0xFFFFu];
            return b * 0x01010101u;
        }
        default:
            return 0;
    }
}

void GbaBus::Write8(uint32_t addr, uint8_t val) {
    uint32_t region = addr >> 24;
    switch (region) {
        case 0x02:
            ewram[addr & 0x3FFFFu] = val;
            break;
        case 0x03:
            iwram[addr & 0x7FFFu] = val;
            break;
        case 0x04:
            WriteIo8(addr & 0x3FFu, val);
            break;
        case 0x05: {
            // 8-bit writes to PRAM write duplicated 16-bit halfword
            uint32_t off = addr & 0x3FEu;
            pram[off] = val;
            pram[off + 1] = val;
            break;
        }
        case 0x06: {
            // 8-bit writes to BG VRAM write duplicated 16-bit halfword; OBJ VRAM ignores 8-bit writes
            uint32_t off = addr & 0x1FFFFu;
            if (off >= 0x18000u) off -= 0x8000u;
            uint16_t dispcnt = ReadIo16Direct(0x000);
            uint32_t bgLimit = ((dispcnt & 7u) >= 3) ? 0x14000u : 0x10000u;
            if (off < bgLimit) {
                off &= ~1u;
                vram[off] = val;
                vram[off + 1] = val;
            }
            break;
        }
        case 0x07:
            // OAM ignores 8-bit writes
            break;
        case 0x0E: case 0x0F:
            sram[addr & 0xFFFFu] = val;
            sramDirty = true;
            break;
        default:
            break;
    }
}

void GbaBus::Write16(uint32_t addr, uint16_t val) {
    addr &= ~1u;
    uint32_t region = addr >> 24;
    switch (region) {
        case 0x02: {
            uint32_t off = addr & 0x3FFFEu;
            ewram[off] = static_cast<uint8_t>(val & 0xFF);
            ewram[off + 1] = static_cast<uint8_t>(val >> 8);
            break;
        }
        case 0x03: {
            uint32_t off = addr & 0x7FFEu;
            iwram[off] = static_cast<uint8_t>(val & 0xFF);
            iwram[off + 1] = static_cast<uint8_t>(val >> 8);
            break;
        }
        case 0x04:
            WriteIo16(addr & 0x3FEu, val);
            break;
        case 0x05: {
            uint32_t off = addr & 0x3FEu;
            pram[off] = static_cast<uint8_t>(val & 0xFF);
            pram[off + 1] = static_cast<uint8_t>(val >> 8);
            break;
        }
        case 0x06: {
            uint32_t off = addr & 0x1FFFEu;
            if (off >= 0x18000u) off -= 0x8000u;
            vram[off] = static_cast<uint8_t>(val & 0xFF);
            vram[off + 1] = static_cast<uint8_t>(val >> 8);
            break;
        }
        case 0x07: {
            uint32_t off = addr & 0x3FEu;
            oam[off] = static_cast<uint8_t>(val & 0xFF);
            oam[off + 1] = static_cast<uint8_t>(val >> 8);
            break;
        }
        case 0x0E: case 0x0F:
            sram[addr & 0xFFFFu] = static_cast<uint8_t>(val & 0xFF);
            sramDirty = true;
            break;
        default:
            break;
    }
}

void GbaBus::Write32(uint32_t addr, uint32_t val) {
    addr &= ~3u;
    uint32_t region = addr >> 24;
    switch (region) {
        case 0x02: {
            uint32_t off = addr & 0x3FFFCu;
            std::memcpy(ewram.data() + off, &val, 4);
            break;
        }
        case 0x03: {
            uint32_t off = addr & 0x7FFCu;
            std::memcpy(iwram.data() + off, &val, 4);
            break;
        }
        case 0x04:
            WriteIo32(addr & 0x3FCu, val);
            break;
        case 0x05: {
            uint32_t off = addr & 0x3FCu;
            std::memcpy(pram.data() + off, &val, 4);
            break;
        }
        case 0x06: {
            uint32_t off = addr & 0x1FFFCu;
            if (off >= 0x18000u) off -= 0x8000u;
            std::memcpy(vram.data() + off, &val, 4);
            break;
        }
        case 0x07: {
            uint32_t off = addr & 0x3FCu;
            std::memcpy(oam.data() + off, &val, 4);
            break;
        }
        case 0x0E: case 0x0F:
            sram[addr & 0xFFFFu] = static_cast<uint8_t>(val & 0xFF);
            sramDirty = true;
            break;
        default:
            break;
    }
}

} // namespace gba
