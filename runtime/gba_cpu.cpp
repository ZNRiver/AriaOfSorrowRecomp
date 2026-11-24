#include "gba_cpu.h"
#include "gba_bios.h"
#include "aot_loader.h"

#include <cstddef>
#include <cstring>

namespace gba {

static_assert(offsetof(GbaCpu, r) == CPU_OFF_R, "GbaCpu::r ABI offset mismatch");
static_assert(offsetof(GbaCpu, cpsr) == CPU_OFF_CPSR, "GbaCpu::cpsr ABI offset mismatch");
static_assert(offsetof(GbaCpu, cycles) == CPU_OFF_CYCLES, "GbaCpu::cycles ABI offset mismatch");
static_assert(offsetof(GbaCpu, nextEventCycles) == CPU_OFF_NEXT_EVENT, "GbaCpu::nextEventCycles ABI offset mismatch");
static_assert(offsetof(GbaCpu, statRecompInstrs) == CPU_OFF_STAT_RECOMP_INSTRS, "GbaCpu::statRecompInstrs ABI offset mismatch");
static_assert(offsetof(GbaCpu, halted) == CPU_OFF_HALTED, "GbaCpu::halted ABI offset mismatch");

GbaCpu::GbaCpu(GbaBus& busRef) : bus(busRef) {
    bus.cpu = this;
    Reset();
}

void GbaCpu::Reset() {
    std::memset(r, 0, sizeof(r));
    std::memset(spsr, 0, sizeof(spsr));
    std::memset(bankedUsr, 0, sizeof(bankedUsr));
    std::memset(bankedFiq, 0, sizeof(bankedFiq));
    std::memset(bankedIrq, 0, sizeof(bankedIrq));
    std::memset(bankedSvc, 0, sizeof(bankedSvc));
    std::memset(bankedAbt, 0, sizeof(bankedAbt));
    std::memset(bankedUnd, 0, sizeof(bankedUnd));

    // Standard post-BIOS GBA boot state
    cpsr = MODE_SYS;
    r[13] = 0x03007F00u; // SP_usr/sys
    bankedUsr[5] = 0x03007F00u;
    bankedIrq[0] = 0x03007FA0u; // SP_irq
    bankedSvc[0] = 0x03007FE0u; // SP_svc
    r[15] = 0x08000000u;        // ROM entry point

    cycles = 0;
    nextEventCycles = 0;
    halted = false;
    intrWaitActive = false;
    intrWaitMask = 0;

    statRecompCalls = 0;
    statRecompInstrs = 0;
    statFallbackInstrs = 0;
    statFrames = 0;
}

void GbaCpu::CaptureState(SaveState& out) const {
    out.valid = true;
    for (int i = 0; i < 16; ++i) out.r[i] = r[i];
    out.cpsr = cpsr;
    std::memcpy(out.spsr, spsr, sizeof(spsr));
    std::memcpy(out.bankedUsr, bankedUsr, sizeof(bankedUsr));
    std::memcpy(out.bankedFiq, bankedFiq, sizeof(bankedFiq));
    std::memcpy(out.bankedIrq, bankedIrq, sizeof(bankedIrq));
    std::memcpy(out.bankedSvc, bankedSvc, sizeof(bankedSvc));
    std::memcpy(out.bankedAbt, bankedAbt, sizeof(bankedAbt));
    std::memcpy(out.bankedUnd, bankedUnd, sizeof(bankedUnd));
    out.halted = halted;
    out.intrWaitActive = intrWaitActive;
    out.intrWaitMask = intrWaitMask;

    out.ewram = bus.ewram;
    out.iwram = bus.iwram;
    out.io = bus.io;
    out.pram = bus.pram;
    out.vram = bus.vram;
    out.oam = bus.oam;
    out.ie = bus.ie;
    out.ifReg = bus.ifReg;
    out.ime = bus.ime;
    std::memcpy(out.dma, bus.dma, sizeof(out.dma));
    std::memcpy(out.timers, bus.timers, sizeof(out.timers));
    out.bg2RefX = bus.ppu.bg2RefX;
    out.bg2RefY = bus.ppu.bg2RefY;
    out.bg3RefX = bus.ppu.bg3RefX;
    out.bg3RefY = bus.ppu.bg3RefY;
}

bool GbaCpu::RestoreState(const SaveState& in) {
    if (!in.valid) return false;
    for (int i = 0; i < 16; ++i) r[i] = in.r[i];
    cpsr = in.cpsr;
    std::memcpy(spsr, in.spsr, sizeof(spsr));
    std::memcpy(bankedUsr, in.bankedUsr, sizeof(bankedUsr));
    std::memcpy(bankedFiq, in.bankedFiq, sizeof(bankedFiq));
    std::memcpy(bankedIrq, in.bankedIrq, sizeof(bankedIrq));
    std::memcpy(bankedSvc, in.bankedSvc, sizeof(bankedSvc));
    std::memcpy(bankedAbt, in.bankedAbt, sizeof(bankedAbt));
    std::memcpy(bankedUnd, in.bankedUnd, sizeof(bankedUnd));
    halted = in.halted;
    intrWaitActive = in.intrWaitActive;
    intrWaitMask = in.intrWaitMask;

    bus.ewram = in.ewram;
    bus.iwram = in.iwram;
    bus.io = in.io;
    bus.pram = in.pram;
    bus.vram = in.vram;
    bus.oam = in.oam;
    bus.ie = in.ie;
    bus.ifReg = in.ifReg;
    bus.ime = in.ime;
    std::memcpy(bus.dma, in.dma, sizeof(bus.dma));
    std::memcpy(bus.timers, in.timers, sizeof(bus.timers));
    bus.ppu.bg2RefX = in.bg2RefX;
    bus.ppu.bg2RefY = in.bg2RefY;
    bus.ppu.bg3RefX = in.bg3RefX;
    bus.ppu.bg3RefY = in.bg3RefY;
    return true;
}

int GbaCpu::ModeToBankIndex(uint32_t mode) {
    switch (mode & 0x1Fu) {
        case MODE_USR: case MODE_SYS: return 0;
        case MODE_FIQ: return 1;
        case MODE_IRQ: return 2;
        case MODE_SVC: return 3;
        case MODE_ABT: return 4;
        case MODE_UND: return 5;
        default:       return 0;
    }
}

void GbaCpu::SwitchMode(uint32_t newMode) {
    uint32_t oldMode = cpsr & 0x1Fu;
    newMode &= 0x1Fu;
    if (oldMode == newMode) return;

    int oldIdx = ModeToBankIndex(oldMode);
    int newIdx = ModeToBankIndex(newMode);

    // Save current registers into old bank
    if (oldIdx == 1) {
        for (int i = 0; i < 7; ++i) bankedFiq[i] = r[8 + i];
    } else {
        for (int i = 0; i < 5; ++i) bankedUsr[i] = r[8 + i];
        if (oldIdx == 0)      { bankedUsr[5] = r[13]; bankedUsr[6] = r[14]; }
        else if (oldIdx == 2) { bankedIrq[0] = r[13]; bankedIrq[1] = r[14]; }
        else if (oldIdx == 3) { bankedSvc[0] = r[13]; bankedSvc[1] = r[14]; }
        else if (oldIdx == 4) { bankedAbt[0] = r[13]; bankedAbt[1] = r[14]; }
        else if (oldIdx == 5) { bankedUnd[0] = r[13]; bankedUnd[1] = r[14]; }
    }

    // Load new bank into active registers
    if (newIdx == 1) {
        for (int i = 0; i < 7; ++i) r[8 + i] = bankedFiq[i];
    } else {
        for (int i = 0; i < 5; ++i) r[8 + i] = bankedUsr[i];
        if (newIdx == 0)      { r[13] = bankedUsr[5]; r[14] = bankedUsr[6]; }
        else if (newIdx == 2) { r[13] = bankedIrq[0]; r[14] = bankedIrq[1]; }
        else if (newIdx == 3) { r[13] = bankedSvc[0]; r[14] = bankedSvc[1]; }
        else if (newIdx == 4) { r[13] = bankedAbt[0]; r[14] = bankedAbt[1]; }
        else if (newIdx == 5) { r[13] = bankedUnd[0]; r[14] = bankedUnd[1]; }
    }

    cpsr = (cpsr & ~0x1Fu) | newMode;
}

uint32_t GbaCpu::GetCurrentSpsr() const {
    int idx = ModeToBankIndex(cpsr);
    return (idx == 0) ? cpsr : spsr[idx];
}

void GbaCpu::SetCurrentSpsr(uint32_t val) {
    int idx = ModeToBankIndex(cpsr);
    if (idx != 0) spsr[idx] = val;
}

uint32_t GbaCpu::ShiftImm(uint32_t val, uint32_t type, uint32_t amount, bool& carryOut) const {
    carryOut = FlagC();
    switch (type & 3u) {
        case 0: // LSL
            if (amount == 0) return val;
            carryOut = ((val >> (32u - amount)) & 1u) != 0;
            return val << amount;
        case 1: // LSR
            if (amount == 0) {
                carryOut = (val >> 31) != 0;
                return 0;
            }
            carryOut = ((val >> (amount - 1u)) & 1u) != 0;
            return val >> amount;
        case 2: // ASR
            if (amount == 0) {
                carryOut = (val >> 31) != 0;
                return carryOut ? 0xFFFFFFFFu : 0u;
            }
            carryOut = ((val >> (amount - 1u)) & 1u) != 0;
            return static_cast<uint32_t>(static_cast<int32_t>(val) >> amount);
        case 3: // ROR / RRX
            if (amount == 0) { // RRX
                bool oldC = FlagC();
                carryOut = (val & 1u) != 0;
                return (val >> 1) | (oldC ? 0x80000000u : 0u);
            }
            carryOut = ((val >> (amount - 1u)) & 1u) != 0;
            return Ror32(val, amount);
        default:
            return val;
    }
}

uint32_t GbaCpu::ShiftReg(uint32_t val, uint32_t type, uint32_t amount, bool& carryOut) const {
    carryOut = FlagC();
    amount &= 0xFFu;
    if (amount == 0) return val;
    switch (type & 3u) {
        case 0: // LSL
            if (amount < 32) {
                carryOut = ((val >> (32u - amount)) & 1u) != 0;
                return val << amount;
            } else if (amount == 32) {
                carryOut = (val & 1u) != 0;
                return 0;
            } else {
                carryOut = false;
                return 0;
            }
        case 1: // LSR
            if (amount < 32) {
                carryOut = ((val >> (amount - 1u)) & 1u) != 0;
                return val >> amount;
            } else if (amount == 32) {
                carryOut = (val >> 31) != 0;
                return 0;
            } else {
                carryOut = false;
                return 0;
            }
        case 2: // ASR
            if (amount < 32) {
                carryOut = ((val >> (amount - 1u)) & 1u) != 0;
                return static_cast<uint32_t>(static_cast<int32_t>(val) >> amount);
            } else {
                carryOut = (val >> 31) != 0;
                return carryOut ? 0xFFFFFFFFu : 0u;
            }
        case 3: { // ROR
            uint32_t m = amount & 31u;
            if (m == 0) {
                carryOut = (val >> 31) != 0;
                return val;
            }
            carryOut = ((val >> (m - 1u)) & 1u) != 0;
            return Ror32(val, m);
        }
        default:
            return val;
    }
}

void GbaCpu::ExecSwi(uint8_t num) {
    HandleBiosSwi(*this, bus, num);
}

void GbaCpu::EnterBiosIrq() {
    uint32_t oldCpsr = cpsr;
    uint32_t retAddr = r[15] + 4u; // ARM7TDMI IRQ stores next_pc + 4

    SwitchMode(MODE_IRQ);
    spsr[ModeToBankIndex(MODE_IRQ)] = oldCpsr;
    cpsr |= FLAG_I;
    cpsr &= ~FLAG_T;
    r[14] = retAddr;

    // Emulate GBA BIOS IRQ trampoline at 0x00000018:
    // STMFD sp!, {r0-r3, r12, lr}
    r[13] -= 24u;
    bus.Write32(r[13] + 0,  r[0]);
    bus.Write32(r[13] + 4,  r[1]);
    bus.Write32(r[13] + 8,  r[2]);
    bus.Write32(r[13] + 12, r[3]);
    bus.Write32(r[13] + 16, r[12]);
    bus.Write32(r[13] + 20, r[14]);

    r[0] = 0x04000000u;
    r[14] = BIOS_IRQ_RETURN_ADDR;

    uint32_t userHandler = bus.Read32(0x03007FFCu);
    if (userHandler == 0) {
        ExitBiosIrq();
    } else {
        r[15] = userHandler & ~3u;
    }
    halted = false;
}

void GbaCpu::ExitBiosIrq() {
    // Emulate GBA BIOS IRQ return at 0x00000138:
    // LDMFD sp!, {r0-r3, r12, lr}
    // SUBS pc, lr, #4
    r[0]  = bus.Read32(r[13] + 0);
    r[1]  = bus.Read32(r[13] + 4);
    r[2]  = bus.Read32(r[13] + 8);
    r[3]  = bus.Read32(r[13] + 12);
    r[12] = bus.Read32(r[13] + 16);
    r[14] = bus.Read32(r[13] + 20);
    r[13] += 24u;

    uint32_t targetPc = r[14] - 4u;
    uint32_t restoredCpsr = GetCurrentSpsr();
    SwitchMode(restoredCpsr & 0x1Fu);
    cpsr = restoredCpsr;
    r[15] = (cpsr & FLAG_T) ? (targetPc & ~1u) : (targetPc & ~3u);

    // If we were inside BIOS IntrWait/VBlankIntrWait, check if the requested IRQ was acknowledged in 0x03007FF8
    if (intrWaitActive) {
        uint16_t biosIf = bus.Read16(0x03007FF8);
        if (biosIf & intrWaitMask) {
            biosIf &= static_cast<uint16_t>(~intrWaitMask);
            bus.Write16(0x03007FF8, biosIf);
            intrWaitActive = false;
            halted = false;
        } else {
            halted = true;
        }
    }
}

void GbaCpu::RunUntil(uint64_t targetCycles, bool enableRecomp) {
    nextEventCycles = targetCycles;

    while (cycles < targetCycles) {
        // Check pending IRQ
        if (bus.HasPendingIrq() && !(cpsr & FLAG_I)) {
            EnterBiosIrq();
        }

        if (halted) {
            uint64_t skip = targetCycles - cycles;
            cycles = targetCycles;
            bus.StepHardware(static_cast<uint32_t>(skip));
            return;
        }

        // Check BIOS IRQ return trampoline
        if (r[15] == BIOS_IRQ_RETURN_ADDR && !(cpsr & FLAG_T)) {
            ExitBiosIrq();
            continue;
        }

        uint64_t startCycles = cycles;

        // Try dispatching to a statically recompiled native x86_64 function!
        if (enableRecomp && (r[15] >= 0x08000000u && r[15] < 0x0E000000u)) {
            uint32_t key = (r[15] & 0x01FFFFFFu) | 0x08000000u | (IsThumb() ? 1u : 0u);
            RecompFunc fn = LookupRecompiledFunction(key);
            if (fn && fn(this, GetRuntimeHelpers())) {
                statRecompCalls++;
                uint32_t elapsed = static_cast<uint32_t>(cycles - startCycles);
                if (elapsed == 0) { elapsed = 1; cycles++; }
                bus.StepHardware(elapsed);
                continue;
            }
        }

        // Fallback ARM/Thumb execution (for IWRAM IRQ/audio mixer or unanalyzed ROM addresses)
        // Execute up to 32 instructions or until branch/event to keep timer/IRQ overhead low
        uint32_t batchCycles = 0;
        for (int step = 0; step < 32 && cycles < targetCycles && !halted; ++step) {
            if (r[15] == BIOS_IRQ_RETURN_ADDR && !(cpsr & FLAG_T)) {
                ExitBiosIrq();
                break;
            }
            if (bus.HasPendingIrq() && !(cpsr & FLAG_I)) {
                EnterBiosIrq();
                break;
            }
            if (IsThumb()) {
                uint16_t ins = bus.Read16(r[15]);
                r[15] += 2u;
                ExecThumb(ins);
                cycles += 2;
                batchCycles += 2;
            } else {
                uint32_t ins = bus.Read32(r[15]);
                r[15] += 4u;
                ExecArm(ins);
                cycles += 2;
                batchCycles += 2;
            }
            statFallbackInstrs++;

            // If we jumped back into a recompiled ROM function, exit fallback immediately!
            if (enableRecomp && (r[15] >= 0x08000000u && r[15] < 0x0E000000u)) {
                uint32_t key = (r[15] & 0x01FFFFFFu) | 0x08000000u | (IsThumb() ? 1u : 0u);
                if (LookupRecompiledFunction(key) != nullptr) {
                    break;
                }
            }
        }
        if (batchCycles > 0) {
            bus.StepHardware(batchCycles);
        }
    }
}

void GbaCpu::RunFrame(bool enableRecomp) {
    // Keep scanline deadlines locked to an exact 280,896-cycle grid so overshoot never accumulates
    // (critical for GBA MP2K Direct Sound FIFO / Timer 0 synchronization!)
    uint64_t frameBase = (cycles / CYCLES_PER_FRAME) * CYCLES_PER_FRAME;
    if (cycles > frameBase + CYCLES_PER_SCANLINE) {
        frameBase = cycles;
    }

    for (int line = 0; line < SCANLINES_TOTAL; ++line) {
        uint64_t lineStart = frameBase + static_cast<uint64_t>(line) * CYCLES_PER_SCANLINE;

        // Update VCOUNT
        bus.WriteIo16Direct(0x006, static_cast<uint16_t>(line));

        // Update DISPSTAT
        uint16_t dispstat = bus.ReadIo16Direct(0x004);
        dispstat &= ~0x0007u; // Clear VBlank, HBlank, VCount match
        if (line >= SCANLINES_VISIBLE && line < 227) {
            dispstat |= 0x0001u; // VBlank flag
        }
        uint16_t lyc = (dispstat >> 8) & 0xFFu;
        if (line == static_cast<int>(lyc)) {
            dispstat |= 0x0004u; // VCount match flag
            if (dispstat & 0x0020u) {
                bus.RequestIrq(IRQ_VCOUNT);
            }
        }
        bus.WriteIo16Direct(0x004, dispstat);

        if (line == 0) {
            bus.ppu.ReloadAffineRefs(bus);
        }

        // VBlank start at scanline 160
        if (line == SCANLINES_VISIBLE) {
            if (dispstat & 0x0008u) {
                bus.RequestIrq(IRQ_VBLANK);
            }
            bus.TriggerDma(1); // VBlank DMA
        }

        // HDraw phase (960 cycles)
        RunUntil(lineStart + HDRAW_CYCLES, enableRecomp);

        // Render visible scanline at end of HDraw
        if (line < SCANLINES_VISIBLE) {
            bus.ppu.RenderScanline(bus, line);
            bus.ppu.AdvanceAffineRefs(bus);
        }

        // HBlank phase (272 cycles)
        dispstat = bus.ReadIo16Direct(0x004);
        dispstat |= 0x0002u; // HBlank flag
        bus.WriteIo16Direct(0x004, dispstat);

        if (dispstat & 0x0010u) {
            bus.RequestIrq(IRQ_HBLANK);
        }
        if (line < SCANLINES_VISIBLE) {
            bus.TriggerDma(2); // HBlank DMA
        }

        RunUntil(lineStart + CYCLES_PER_SCANLINE, enableRecomp);
    }
    statFrames++;
}

// ============================================================================
// ARM7TDMI Fallback Executor (ARM + Thumb)
// ============================================================================

void GbaCpu::ExecArm(uint32_t ins) {
    if (!CheckCond(ins >> 28)) return;

    uint32_t pcVal = r[15] + 4u; // r[15] was already incremented by 4; ARM PC reads as instr_addr + 8
    auto getReg = [&](uint32_t reg) -> uint32_t {
        return (reg == 15) ? pcVal : r[reg];
    };

    // 1. Branch and Exchange (BX)
    if ((ins & 0x0FFFFFF0u) == 0x012FFF10u) {
        uint32_t rn = ins & 0xFu;
        BranchExchange(getReg(rn));
        return;
    }

    // 2. Branch / Branch with Link (B / BL)
    if ((ins & 0x0E000000u) == 0x0A000000u) {
        if (ins & (1u << 24)) {
            r[14] = r[15]; // Address of next instruction
        }
        int32_t off = SignExtend(ins & 0x00FFFFFFu, 24) << 2;
        r[15] = (pcVal + static_cast<uint32_t>(off)) & ~3u;
        return;
    }

    // 3. Software Interrupt (SWI)
    if ((ins & 0x0F000000u) == 0x0F000000u) {
        ExecSwi(static_cast<uint8_t>((ins >> 16) & 0xFFu));
        return;
    }

    // 4. Multiply / Multiply Long
    if ((ins & 0x0FC000F0u) == 0x00000090u) {
        bool acc = (ins & (1u << 21)) != 0;
        bool s   = (ins & (1u << 20)) != 0;
        uint32_t rd = (ins >> 16) & 0xFu;
        uint32_t rn = (ins >> 12) & 0xFu;
        uint32_t rs = (ins >> 8) & 0xFu;
        uint32_t rm = ins & 0xFu;
        uint32_t res = r[rm] * r[rs] + (acc ? r[rn] : 0u);
        r[rd] = res;
        if (s) SetNZ(res);
        return;
    }
    if ((ins & 0x0F8000F0u) == 0x00800090u) {
        bool isSigned = (ins & (1u << 22)) != 0;
        bool acc      = (ins & (1u << 21)) != 0;
        bool s        = (ins & (1u << 20)) != 0;
        uint32_t rdHi = (ins >> 16) & 0xFu;
        uint32_t rdLo = (ins >> 12) & 0xFu;
        uint32_t rs   = (ins >> 8) & 0xFu;
        uint32_t rm   = ins & 0xFu;
        uint64_t res;
        if (isSigned) {
            int64_t prod = static_cast<int64_t>(static_cast<int32_t>(r[rm])) *
                           static_cast<int64_t>(static_cast<int32_t>(r[rs]));
            res = static_cast<uint64_t>(prod);
        } else {
            res = static_cast<uint64_t>(r[rm]) * static_cast<uint64_t>(r[rs]);
        }
        if (acc) {
            res += (static_cast<uint64_t>(r[rdHi]) << 32) | r[rdLo];
        }
        r[rdLo] = static_cast<uint32_t>(res & 0xFFFFFFFFu);
        r[rdHi] = static_cast<uint32_t>(res >> 32);
        if (s) {
            cpsr = (cpsr & ~(FLAG_N | FLAG_Z)) |
                   (r[rdHi] & 0x80000000u) |
                   (res == 0 ? FLAG_Z : 0u);
        }
        return;
    }

    // 5. Single Data Swap (SWP / SWPB)
    if ((ins & 0x0FB00FF0u) == 0x01000090u) {
        bool byteOp = (ins & (1u << 22)) != 0;
        uint32_t rn = (ins >> 16) & 0xFu;
        uint32_t rd = (ins >> 12) & 0xFu;
        uint32_t rm = ins & 0xFu;
        uint32_t addr = r[rn];
        if (byteOp) {
            uint8_t old = bus.Read8(addr);
            bus.Write8(addr, static_cast<uint8_t>(r[rm]));
            r[rd] = old;
        } else {
            uint32_t old = Read32Rotated(addr);
            bus.Write32(addr, r[rm]);
            r[rd] = old;
        }
        return;
    }

    // 6. Halfword and Signed Data Transfer (LDRH / STRH / LDRSB / LDRSH)
    if ((ins & 0x0E000090u) == 0x00000090u && (ins & 0x00000060u) != 0) {
        bool p = (ins & (1u << 24)) != 0;
        bool u = (ins & (1u << 23)) != 0;
        bool i = (ins & (1u << 22)) != 0;
        bool w = (ins & (1u << 21)) != 0;
        bool l = (ins & (1u << 20)) != 0;
        uint32_t rn = (ins >> 16) & 0xFu;
        uint32_t rd = (ins >> 12) & 0xFu;
        uint32_t sh = (ins >> 5) & 3u;
        uint32_t off = i ? (((ins >> 4) & 0xF0u) | (ins & 0xFu)) : r[ins & 0xFu];

        uint32_t base = getReg(rn);
        uint32_t addr = p ? (u ? base + off : base - off) : base;

        if (l) {
            uint32_t val = 0;
            if (sh == 1)      val = Read16Rotated(addr);
            else if (sh == 2) val = Read8Signed(addr);
            else if (sh == 3) val = Read16Signed(addr);
            if (!p || w) {
                r[rn] = u ? base + off : base - off;
            }
            r[rd] = (rd == 15) ? (val & ~3u) : val;
        } else {
            uint32_t val = (rd == 15) ? (pcVal + 4u) : r[rd];
            bus.Write16(addr, static_cast<uint16_t>(val));
            if (!p || w) {
                r[rn] = u ? base + off : base - off;
            }
        }
        return;
    }

    // 7. PSR Transfer (MRS / MSR)
    if ((ins & 0x0FBF0FFFu) == 0x010F0000u) { // MRS
        bool useSpsr = (ins & (1u << 22)) != 0;
        uint32_t rd = (ins >> 12) & 0xFu;
        r[rd] = useSpsr ? GetCurrentSpsr() : cpsr;
        return;
    }
    if ((ins & 0x0DB0F000u) == 0x0120F000u && ((ins & (1u << 25)) || (ins & 0xFF0u) == 0)) { // MSR
        bool useSpsr = (ins & (1u << 22)) != 0;
        uint32_t mask = 0;
        if (ins & (1u << 19)) mask |= 0xFF000000u;
        if (ins & (1u << 18)) mask |= 0x00FF0000u;
        if (ins & (1u << 17)) mask |= 0x0000FF00u;
        if (ins & (1u << 16)) mask |= 0x000000FFu;
        uint32_t val = (ins & (1u << 25))
            ? Ror32(ins & 0xFFu, ((ins >> 8) & 0xFu) * 2u)
            : r[ins & 0xFu];
        if (useSpsr) {
            SetCurrentSpsr((GetCurrentSpsr() & ~mask) | (val & mask));
        } else {
            if (GetMode() == MODE_USR) mask &= 0xFF000000u;
            uint32_t nextCpsr = (cpsr & ~mask) | (val & mask);
            SwitchMode(nextCpsr & 0x1Fu);
            cpsr = nextCpsr;
        }
        return;
    }

    // 8. Data Processing (ALU)
    if ((ins & 0x0C000000u) == 0x00000000u) {
        bool imm = (ins & (1u << 25)) != 0;
        uint32_t op = (ins >> 21) & 0xFu;
        bool s = (ins & (1u << 20)) != 0;
        uint32_t rn = (ins >> 16) & 0xFu;
        uint32_t rd = (ins >> 12) & 0xFu;

        bool shiftCarry = FlagC();
        uint32_t op2 = 0;
        uint32_t rnVal = getReg(rn);

        if (imm) {
            uint32_t rot = ((ins >> 8) & 0xFu) * 2u;
            op2 = Ror32(ins & 0xFFu, rot);
            if (rot != 0) shiftCarry = (op2 >> 31) != 0;
        } else {
            uint32_t rm = ins & 0xFu;
            uint32_t shiftType = (ins >> 5) & 3u;
            if (ins & (1u << 4)) {
                uint32_t rs = (ins >> 8) & 0xFu;
                uint32_t rmVal = (rm == 15) ? (pcVal + 4u) : r[rm];
                if (rn == 15) rnVal = pcVal + 4u;
                op2 = ShiftReg(rmVal, shiftType, r[rs], shiftCarry);
            } else {
                uint32_t amt = (ins >> 7) & 0x1Fu;
                op2 = ShiftImm(getReg(rm), shiftType, amt, shiftCarry);
            }
        }

        bool setFlags = s && (rd != 15);
        uint32_t res = 0;
        bool writeRd = true;

        switch (op) {
            case 0x0: res = rnVal & op2; if (setFlags) SetNZC(res, shiftCarry); break;
            case 0x1: res = rnVal ^ op2; if (setFlags) SetNZC(res, shiftCarry); break;
            case 0x2: res = Sub(rnVal, op2, setFlags); break;
            case 0x3: res = Sub(op2, rnVal, setFlags); break;
            case 0x4: res = Add(rnVal, op2, setFlags); break;
            case 0x5: res = Adc(rnVal, op2, setFlags); break;
            case 0x6: res = Sbc(rnVal, op2, setFlags); break;
            case 0x7: res = Sbc(op2, rnVal, setFlags); break;
            case 0x8: res = rnVal & op2; SetNZC(res, shiftCarry); writeRd = false; break;
            case 0x9: res = rnVal ^ op2; SetNZC(res, shiftCarry); writeRd = false; break;
            case 0xA: Sub(rnVal, op2, true); writeRd = false; break;
            case 0xB: Add(rnVal, op2, true); writeRd = false; break;
            case 0xC: res = rnVal | op2; if (setFlags) SetNZC(res, shiftCarry); break;
            case 0xD: res = op2; if (setFlags) SetNZC(res, shiftCarry); break;
            case 0xE: res = rnVal & ~op2; if (setFlags) SetNZC(res, shiftCarry); break;
            case 0xF: res = ~op2; if (setFlags) SetNZC(res, shiftCarry); break;
        }

        if (writeRd) {
            if (rd == 15) {
                if (s) {
                    uint32_t restored = GetCurrentSpsr();
                    SwitchMode(restored & 0x1Fu);
                    cpsr = restored;
                }
                r[15] = (cpsr & FLAG_T) ? (res & ~1u) : (res & ~3u);
            } else {
                r[rd] = res;
            }
        }
        return;
    }

    // 9. Single Data Transfer (LDR / STR / LDRB / STRB)
    if ((ins & 0x0C000000u) == 0x04000000u) {
        bool regOff = (ins & (1u << 25)) != 0;
        bool p = (ins & (1u << 24)) != 0;
        bool u = (ins & (1u << 23)) != 0;
        bool b = (ins & (1u << 22)) != 0;
        bool w = (ins & (1u << 21)) != 0;
        bool l = (ins & (1u << 20)) != 0;
        uint32_t rn = (ins >> 16) & 0xFu;
        uint32_t rd = (ins >> 12) & 0xFu;

        uint32_t off;
        if (!regOff) {
            off = ins & 0xFFFu;
        } else {
            bool dummyC = false;
            off = ShiftImm(r[ins & 0xFu], (ins >> 5) & 3u, (ins >> 7) & 0x1Fu, dummyC);
        }

        uint32_t base = getReg(rn);
        uint32_t addr = p ? (u ? base + off : base - off) : base;

        if (l) {
            uint32_t val = b ? bus.Read8(addr) : Read32Rotated(addr);
            if (!p || w) {
                r[rn] = u ? base + off : base - off;
            }
            r[rd] = (rd == 15) ? (val & ~3u) : val;
        } else {
            uint32_t val = (rd == 15) ? (pcVal + 4u) : r[rd];
            if (b) bus.Write8(addr, static_cast<uint8_t>(val));
            else   bus.Write32(addr, val);
            if (!p || w) {
                r[rn] = u ? base + off : base - off;
            }
        }
        return;
    }

    // 10. Block Data Transfer (LDM / STM)
    if ((ins & 0x0E000000u) == 0x08000000u) {
        bool p = (ins & (1u << 24)) != 0;
        bool u = (ins & (1u << 23)) != 0;
        bool s = (ins & (1u << 22)) != 0;
        bool w = (ins & (1u << 21)) != 0;
        bool l = (ins & (1u << 20)) != 0;
        uint32_t rn = (ins >> 16) & 0xFu;
        uint32_t rlist = ins & 0xFFFFu;

        uint32_t base = r[rn];
        uint32_t count = 0;
        for (int i = 0; i < 16; ++i) if (rlist & (1u << i)) count++;

        if (count == 0) {
            // ARM7TDMI empty rlist edge case
            if (l) r[15] = bus.Read32(base) & ~3u;
            else   bus.Write32(u ? base : base - 0x40u, pcVal + 4u);
            r[rn] = u ? (base + 0x40u) : (base - 0x40u);
            return;
        }

        uint32_t startAddr = u ? (p ? base + 4u : base)
                               : (p ? base - count * 4u : base - count * 4u + 4u);
        uint32_t finalBase = u ? (base + count * 4u) : (base - count * 4u);

        bool userBank = s && (!l || !(rlist & 0x8000u));
        uint32_t savedMode = GetMode();
        if (userBank) SwitchMode(MODE_USR);

        uint32_t addr = startAddr;
        bool first = true;
        for (int i = 0; i < 16; ++i) {
            if (!(rlist & (1u << i))) continue;
            if (l) {
                uint32_t v = bus.Read32(addr);
                if (i == 15) {
                    if (s) {
                        uint32_t restored = GetCurrentSpsr();
                        SwitchMode(restored & 0x1Fu);
                        cpsr = restored;
                    }
                    r[15] = (cpsr & FLAG_T) ? (v & ~1u) : (v & ~3u);
                } else {
                    r[i] = v;
                }
            } else {
                uint32_t v;
                if (i == 15) v = pcVal + 4u;
                else if (static_cast<uint32_t>(i) == rn && !first && w) v = finalBase;
                else v = r[i];
                bus.Write32(addr, v);
            }
            addr += 4u;
            first = false;
        }

        if (userBank) SwitchMode(savedMode);
        if (w && (!l || !(rlist & (1u << rn)))) {
            r[rn] = finalBase;
        }
        return;
    }
}

void GbaCpu::ExecThumb(uint16_t ins) {
    uint32_t pcVal = r[15] + 2u; // r[15] was already incremented by 2; Thumb PC reads as instr_addr + 4

    // Format 1: Move shifted register
    if ((ins & 0xE000u) == 0x0000u && (ins & 0x1800u) != 0x1800u) {
        uint32_t op = (ins >> 11) & 3u;
        uint32_t imm = (ins >> 6) & 0x1Fu;
        uint32_t rs = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        bool c = FlagC();
        r[rd] = ShiftImm(r[rs], op, imm, c);
        SetNZC(r[rd], c);
        return;
    }

    // Format 2: Add/subtract
    if ((ins & 0xF800u) == 0x1800u) {
        bool imm = (ins & (1u << 10)) != 0;
        bool sub = (ins & (1u << 9)) != 0;
        uint32_t rnOrImm = (ins >> 6) & 7u;
        uint32_t rs = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        uint32_t op2 = imm ? rnOrImm : r[rnOrImm];
        r[rd] = sub ? Sub(r[rs], op2, true) : Add(r[rs], op2, true);
        return;
    }

    // Format 3: Move/compare/add/subtract immediate
    if ((ins & 0xE000u) == 0x2000u) {
        uint32_t op = (ins >> 11) & 3u;
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t imm = ins & 0xFFu;
        switch (op) {
            case 0: r[rd] = imm; SetNZ(imm); break;
            case 1: Sub(r[rd], imm, true); break;
            case 2: r[rd] = Add(r[rd], imm, true); break;
            case 3: r[rd] = Sub(r[rd], imm, true); break;
        }
        return;
    }

    // Format 4: ALU operations
    if ((ins & 0xFC00u) == 0x4000u) {
        uint32_t op = (ins >> 6) & 0xFu;
        uint32_t rs = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        bool c = FlagC();
        switch (op) {
            case 0x0: r[rd] &= r[rs]; SetNZ(r[rd]); break;
            case 0x1: r[rd] ^= r[rs]; SetNZ(r[rd]); break;
            case 0x2: r[rd] = ShiftReg(r[rd], 0, r[rs], c); SetNZC(r[rd], c); break;
            case 0x3: r[rd] = ShiftReg(r[rd], 1, r[rs], c); SetNZC(r[rd], c); break;
            case 0x4: r[rd] = ShiftReg(r[rd], 2, r[rs], c); SetNZC(r[rd], c); break;
            case 0x5: r[rd] = Adc(r[rd], r[rs], true); break;
            case 0x6: r[rd] = Sbc(r[rd], r[rs], true); break;
            case 0x7: r[rd] = ShiftReg(r[rd], 3, r[rs], c); SetNZC(r[rd], c); break;
            case 0x8: SetNZ(r[rd] & r[rs]); break;
            case 0x9: r[rd] = Sub(0, r[rs], true); break;
            case 0xA: Sub(r[rd], r[rs], true); break;
            case 0xB: Add(r[rd], r[rs], true); break;
            case 0xC: r[rd] |= r[rs]; SetNZ(r[rd]); break;
            case 0xD: r[rd] *= r[rs]; SetNZ(r[rd]); break;
            case 0xE: r[rd] &= ~r[rs]; SetNZ(r[rd]); break;
            case 0xF: r[rd] = ~r[rs]; SetNZ(r[rd]); break;
        }
        return;
    }

    // Format 5: Hi register operations / BX
    if ((ins & 0xFC00u) == 0x4400u) {
        uint32_t op = (ins >> 8) & 3u;
        uint32_t rs = ((ins >> 3) & 7u) | ((ins & 0x40u) ? 8u : 0u);
        uint32_t rd = (ins & 7u) | ((ins & 0x80u) ? 8u : 0u);
        uint32_t rsVal = (rs == 15) ? pcVal : r[rs];
        uint32_t rdVal = (rd == 15) ? pcVal : r[rd];

        switch (op) {
            case 0: // ADD
                if (rd == 15) r[15] = (rdVal + rsVal) & ~1u;
                else r[rd] = rdVal + rsVal;
                break;
            case 1: // CMP
                Sub(rdVal, rsVal, true);
                break;
            case 2: // MOV
                if (rd == 15) r[15] = rsVal & ~1u;
                else r[rd] = rsVal;
                break;
            case 3: // BX
                BranchExchange(rsVal);
                break;
        }
        return;
    }

    // Format 6: PC-relative load
    if ((ins & 0xF800u) == 0x4800u) {
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t off = (ins & 0xFFu) << 2;
        uint32_t addr = (pcVal & ~3u) + off;
        r[rd] = bus.Read32(addr);
        return;
    }

    // Format 7 & 8: Load/store with register offset / sign-extended byte/halfword
    if ((ins & 0xF000u) == 0x5000u) {
        uint32_t ro = (ins >> 6) & 7u;
        uint32_t rb = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        uint32_t addr = r[rb] + r[ro];
        uint32_t op = (ins >> 9) & 7u;
        switch (op) {
            case 0: bus.Write32(addr, r[rd]); break;                   // STR
            case 1: bus.Write16(addr, static_cast<uint16_t>(r[rd])); break; // STRH
            case 2: bus.Write8(addr, static_cast<uint8_t>(r[rd])); break;   // STRB
            case 3: r[rd] = Read8Signed(addr); break;                  // LDSB
            case 4: r[rd] = Read32Rotated(addr); break;                // LDR
            case 5: r[rd] = Read16Rotated(addr); break;                // LDRH
            case 6: r[rd] = bus.Read8(addr); break;                    // LDRB
            case 7: r[rd] = Read16Signed(addr); break;                 // LDSH
        }
        return;
    }

    // Format 9: Load/store with immediate offset
    if ((ins & 0xE000u) == 0x6000u) {
        bool b = (ins & (1u << 12)) != 0;
        bool l = (ins & (1u << 11)) != 0;
        uint32_t imm = (ins >> 6) & 0x1Fu;
        uint32_t rb = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        uint32_t addr = r[rb] + (b ? imm : (imm << 2));
        if (l) r[rd] = b ? bus.Read8(addr) : Read32Rotated(addr);
        else {
            if (b) bus.Write8(addr, static_cast<uint8_t>(r[rd]));
            else   bus.Write32(addr, r[rd]);
        }
        return;
    }

    // Format 10: Load/store halfword
    if ((ins & 0xF000u) == 0x8000u) {
        bool l = (ins & (1u << 11)) != 0;
        uint32_t off = ((ins >> 6) & 0x1Fu) << 1;
        uint32_t rb = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        uint32_t addr = r[rb] + off;
        if (l) r[rd] = Read16Rotated(addr);
        else   bus.Write16(addr, static_cast<uint16_t>(r[rd]));
        return;
    }

    // Format 11: SP-relative load/store
    if ((ins & 0xF000u) == 0x9000u) {
        bool l = (ins & (1u << 11)) != 0;
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t addr = r[13] + ((ins & 0xFFu) << 2);
        if (l) r[rd] = Read32Rotated(addr);
        else   bus.Write32(addr, r[rd]);
        return;
    }

    // Format 12: Load address (ADD Rd, PC/SP, #imm)
    if ((ins & 0xF000u) == 0xA000u) {
        bool sp = (ins & (1u << 11)) != 0;
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t off = (ins & 0xFFu) << 2;
        r[rd] = (sp ? r[13] : (pcVal & ~3u)) + off;
        return;
    }

    // Format 13: Add offset to SP
    if ((ins & 0xFF00u) == 0xB000u) {
        uint32_t off = (ins & 0x7Fu) << 2;
        if (ins & 0x80u) r[13] -= off;
        else             r[13] += off;
        return;
    }

    // Format 14: Push/Pop registers
    if ((ins & 0xF600u) == 0xB400u) {
        bool l = (ins & (1u << 11)) != 0;
        bool pclr = (ins & (1u << 8)) != 0;
        uint32_t rlist = ins & 0xFFu;
        if (l) {
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1u << i)) { r[i] = bus.Read32(r[13]); r[13] += 4u; }
            }
            if (pclr) {
                r[15] = bus.Read32(r[13]) & ~1u;
                r[13] += 4u;
            }
        } else {
            uint32_t count = pclr ? 1u : 0u;
            for (int i = 0; i < 8; ++i) if (rlist & (1u << i)) count++;
            r[13] -= count * 4u;
            uint32_t addr = r[13];
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1u << i)) { bus.Write32(addr, r[i]); addr += 4u; }
            }
            if (pclr) bus.Write32(addr, r[14]);
        }
        return;
    }

    // Format 15: Multiple load/store (LDMIA / STMIA)
    if ((ins & 0xF000u) == 0xC000u) {
        bool l = (ins & (1u << 11)) != 0;
        uint32_t rb = (ins >> 8) & 7u;
        uint32_t rlist = ins & 0xFFu;
        uint32_t addr = r[rb];
        if (rlist == 0) {
            if (l) r[15] = bus.Read32(addr) & ~1u;
            else   bus.Write32(addr, pcVal + 2u);
            r[rb] = addr + 0x40u;
            return;
        }
        if (l) {
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1u << i)) { r[i] = bus.Read32(addr); addr += 4u; }
            }
            if (!(rlist & (1u << rb))) r[rb] = addr;
        } else {
            uint32_t count = 0;
            for (int i = 0; i < 8; ++i) if (rlist & (1u << i)) count++;
            uint32_t finalAddr = addr + count * 4u;
            bool first = true;
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1u << i)) {
                    uint32_t v = (static_cast<uint32_t>(i) == rb && !first) ? finalAddr : r[i];
                    bus.Write32(addr, v);
                    addr += 4u;
                    first = false;
                }
            }
            r[rb] = finalAddr;
        }
        return;
    }

    // Format 17: SWI
    if ((ins & 0xFF00u) == 0xDF00u) {
        ExecSwi(static_cast<uint8_t>(ins & 0xFFu));
        return;
    }

    // Format 16: Conditional branch
    if ((ins & 0xF000u) == 0xD000u) {
        uint32_t cond = (ins >> 8) & 0xFu;
        if (CheckCond(cond)) {
            int32_t off = SignExtend(ins & 0xFFu, 8) << 1;
            r[15] = (pcVal + static_cast<uint32_t>(off)) & ~1u;
        }
        return;
    }

    // Format 18: Unconditional branch
    if ((ins & 0xF800u) == 0xE000u) {
        int32_t off = SignExtend(ins & 0x7FFu, 11) << 1;
        r[15] = (pcVal + static_cast<uint32_t>(off)) & ~1u;
        return;
    }

    // Format 19: Long branch with link (BL prefix / suffix)
    if ((ins & 0xF000u) == 0xF000u) {
        bool h = (ins & (1u << 11)) != 0;
        if (!h) {
            int32_t off = SignExtend(ins & 0x7FFu, 11) << 12;
            r[14] = pcVal + static_cast<uint32_t>(off);
        } else {
            uint32_t nextPc = r[15];
            uint32_t target = r[14] + ((ins & 0x7FFu) << 1);
            r[14] = nextPc | 1u;
            r[15] = target & ~1u;
        }
        return;
    }
}

} // namespace gba
