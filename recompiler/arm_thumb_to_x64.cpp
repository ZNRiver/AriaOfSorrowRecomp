#include "arm_thumb_to_x64.h"
#include "x64_emitter.h"
#include "gba_types.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>

namespace recomp {

namespace {

using Reg = X64Emitter::Reg64;

inline int32_t SignExt(uint32_t v, int bits) {
    uint32_t s = 32u - static_cast<uint32_t>(bits);
    return static_cast<int32_t>(v << s) >> s;
}

inline bool HasLocalAddr(const std::vector<uint32_t>& sortedAddrs, uint32_t target) {
    return std::binary_search(sortedAddrs.begin(), sortedAddrs.end(), target);
}

inline uint32_t RegOff(uint32_t gbaReg) {
    return gba::CPU_OFF_R + gbaReg * 4u;
}

// Helper offsets in gba::RuntimeHelpers
#define HOFF(field) static_cast<uint32_t>(offsetof(gba::RuntimeHelpers, field))

struct PatchRef {
    size_t rel32Offset;
    uint32_t targetGbaAddr;
};

void CompileFunction(
    X64Emitter& e,
    const RomAnalyzer& analyzer,
    const DiscoveredFunction& fn
) {
    e.Align16();

    // Windows x64 ABI prologue:
    // On entry rsp % 16 == 8. Push 5 regs (40 bytes) -> rsp % 16 == 0. Sub 0x20 shadow -> rsp % 16 == 0.
    e.Push(Reg::RBX);
    e.Push(Reg::RBP);
    e.Push(Reg::RSI);
    e.Push(Reg::RDI);
    e.Push(Reg::R12);
    e.SubRspImm8(0x20);
    e.Mov64(Reg::RBX, Reg::RCX); // rbx = GbaCpu*
    e.Mov64(Reg::R12, Reg::RDX); // r12 = const RuntimeHelpers*

    std::unordered_map<uint32_t, size_t> labelPos;
    std::vector<PatchRef> labelPatches;
    std::vector<size_t> returnTruePatches;

    // Entry dispatch: load cpu->r[15] into eax and jump to matching instruction label
    e.Mov32FromRbx(Reg::RAX, RegOff(15));
    // Fast-path check for entryAddr first (most frequent case)
    e.CmpEaxImm32(fn.entryAddr);
    labelPatches.push_back({ e.JeRel32(), fn.entryAddr });
    for (uint32_t addr : fn.insnAddrs) {
        if (addr == fn.entryAddr) continue;
        e.CmpEaxImm32(addr);
        labelPatches.push_back({ e.JeRel32(), addr });
    }

    // Default: address not in this function -> return false (al = 0)
    e.Mov32Imm(Reg::RAX, 0);
    e.AddRspImm8(0x20);
    e.Pop(Reg::R12);
    e.Pop(Reg::RDI);
    e.Pop(Reg::RSI);
    e.Pop(Reg::RBP);
    e.Pop(Reg::RBX);
    e.Ret();

    auto emitReturnTrue = [&]() {
        returnTruePatches.push_back(e.JmpRel32());
    };

    auto emitJumpTarget = [&](uint32_t target) {
        if (HasLocalAddr(fn.insnAddrs, target)) {
            // Check if cycles >= nextEventCycles; if so, save target to r[15] and return true
            e.Mov64FromRbx(Reg::RAX, gba::CPU_OFF_CYCLES);
            e.CmpRaxRbxMem64(gba::CPU_OFF_NEXT_EVENT);
            size_t jaeExit = e.JaeRel32();
            labelPatches.push_back({ e.JmpRel32(), target });
            e.PatchRel32(jaeExit, e.Pos());
            e.Mov32ImmToRbx(RegOff(15), target);
            emitReturnTrue();
        } else {
            e.Mov32ImmToRbx(RegOff(15), target);
            emitReturnTrue();
        }
    };

    // Emit native x86_64 code for each instruction in the function
    for (uint32_t pc : fn.insnAddrs) {
        labelPos[pc] = e.Pos();
        e.Add64RbxImm8(gba::CPU_OFF_CYCLES, 2);
        e.Add64RbxImm8(gba::CPU_OFF_STAT_RECOMP_INSTRS, 1);

        if (!fn.isThumb) {
            // ======================== ARM INSTRUCTION ========================
            uint32_t ins = analyzer.Read32(pc);
            uint32_t cond = ins >> 28;
            uint32_t nextPc = pc + 4u;
            uint32_t pcVal = pc + 8u;

            if (cond == 0xF) {
                if (!HasLocalAddr(fn.insnAddrs, nextPc)) {
                    e.Mov32ImmToRbx(RegOff(15), nextPc);
                    emitReturnTrue();
                }
                continue;
            }

            size_t skipCondPatch = 0;
            bool hasCond = (cond != 0xE);
            if (hasCond) {
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, cond);
                e.CallHelper(HOFF(checkCond));
                e.TestEaxEax();
                skipCondPatch = e.JeRel32();
            }

            // 1. BX Rn
            if ((ins & 0x0FFFFFF0u) == 0x012FFF10u) {
                uint32_t rn = ins & 0xFu;
                e.Mov64(Reg::RCX, Reg::RBX);
                if (rn == 15) e.Mov32Imm(Reg::RDX, pcVal);
                else          e.Mov32FromRbx(Reg::RDX, RegOff(rn));
                e.CallHelper(HOFF(branchExchange));
                emitReturnTrue();
            }
            // 2. B / BL
            else if ((ins & 0x0E000000u) == 0x0A000000u) {
                int32_t off = SignExt(ins & 0x00FFFFFFu, 24) << 2;
                uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~3u;
                if (ins & (1u << 24)) {
                    e.Mov32ImmToRbx(RegOff(14), nextPc);
                    e.Mov32ImmToRbx(RegOff(15), target);
                    emitReturnTrue();
                } else {
                    emitJumpTarget(target);
                }
            }
            // 3. SWI
            else if ((ins & 0x0F000000u) == 0x0F000000u) {
                uint8_t num = static_cast<uint8_t>((ins >> 16) & 0xFFu);
                e.Mov32ImmToRbx(RegOff(15), nextPc);
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, num);
                e.CallHelper(HOFF(execSwi));
                e.CmpByteRbxZero(gba::CPU_OFF_HALTED);
                returnTruePatches.push_back(e.JneRel32());
                e.Mov32FromRbx(Reg::RAX, RegOff(15));
                e.CmpEaxImm32(nextPc);
                returnTruePatches.push_back(e.JneRel32());
            }
            // 4. All other ARM instructions via exact helper (keeping native branch/loop control flow)
            else {
                e.Mov32ImmToRbx(RegOff(15), nextPc);
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, ins);
                e.CallHelper(HOFF(execArm));
                e.Mov32FromRbx(Reg::RAX, RegOff(15));
                e.CmpEaxImm32(nextPc);
                returnTruePatches.push_back(e.JneRel32());
                e.TestDwordRbxImm(gba::CPU_OFF_CPSR, gba::FLAG_T);
                returnTruePatches.push_back(e.JneRel32());
            }

            if (hasCond) {
                e.PatchRel32(skipCondPatch, e.Pos());
            }
            if (!HasLocalAddr(fn.insnAddrs, nextPc)) {
                e.Mov32ImmToRbx(RegOff(15), nextPc);
                emitReturnTrue();
            }
        } else {
            // ======================== THUMB INSTRUCTION ========================
            uint16_t ins = analyzer.Read16(pc);
            uint32_t nextPc = pc + 2u;
            uint32_t pcVal = pc + 4u;

            // Format 1: LSL / LSR / ASR #imm
            if ((ins & 0xE000u) == 0x0000u && (ins & 0x1800u) != 0x1800u) {
                uint32_t op = (ins >> 11) & 3u;
                uint32_t imm = (ins >> 6) & 0x1Fu;
                uint32_t rs = (ins >> 3) & 7u;
                uint32_t rd = ins & 7u;
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32FromRbx(Reg::RDX, RegOff(rs));
                e.Mov32Imm(Reg::R8, op);
                e.Mov32Imm(Reg::R9, imm);
                e.CallHelper(HOFF(shiftImmSetFlags));
                e.Mov32ToRbx(RegOff(rd), Reg::RAX);
            }
            // Format 2: ADD / SUB (3-reg or 3-bit imm)
            else if ((ins & 0xF800u) == 0x1800u) {
                bool imm = (ins & (1u << 10)) != 0;
                bool sub = (ins & (1u << 9)) != 0;
                uint32_t rnOrImm = (ins >> 6) & 7u;
                uint32_t rs = (ins >> 3) & 7u;
                uint32_t rd = ins & 7u;
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32FromRbx(Reg::RDX, RegOff(rs));
                if (imm) e.Mov32Imm(Reg::R8, rnOrImm);
                else     e.Mov32FromRbx(Reg::R8, RegOff(rnOrImm));
                e.Mov32Imm(Reg::R9, 1);
                e.CallHelper(sub ? HOFF(sub) : HOFF(add));
                e.Mov32ToRbx(RegOff(rd), Reg::RAX);
            }
            // Format 3: MOV / CMP / ADD / SUB #imm8
            else if ((ins & 0xE000u) == 0x2000u) {
                uint32_t op = (ins >> 11) & 3u;
                uint32_t rd = (ins >> 8) & 7u;
                uint32_t imm = ins & 0xFFu;
                if (op == 0) {
                    e.Mov32ImmToRbx(RegOff(rd), imm);
                    e.Mov64(Reg::RCX, Reg::RBX);
                    e.Mov32Imm(Reg::RDX, imm);
                    e.CallHelper(HOFF(setNZ));
                } else if (op == 1) {
                    e.Mov64(Reg::RCX, Reg::RBX);
                    e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                    e.Mov32Imm(Reg::R8, imm);
                    e.Mov32Imm(Reg::R9, 1);
                    e.CallHelper(HOFF(sub));
                } else {
                    e.Mov64(Reg::RCX, Reg::RBX);
                    e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                    e.Mov32Imm(Reg::R8, imm);
                    e.Mov32Imm(Reg::R9, 1);
                    e.CallHelper(op == 2 ? HOFF(add) : HOFF(sub));
                    e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                }
            }
            // Format 4: ALU operations
            else if ((ins & 0xFC00u) == 0x4000u) {
                uint32_t op = (ins >> 6) & 0xFu;
                uint32_t rs = (ins >> 3) & 7u;
                uint32_t rd = ins & 7u;
                switch (op) {
                    case 0x0: // AND
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::RAX, RegOff(rs));
                        e.And32(Reg::RDX, Reg::RAX);
                        e.Mov32ToRbx(RegOff(rd), Reg::RDX);
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.CallHelper(HOFF(setNZ));
                        break;
                    case 0x1: // EOR
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::RAX, RegOff(rs));
                        e.Xor32(Reg::RDX, Reg::RAX);
                        e.Mov32ToRbx(RegOff(rd), Reg::RDX);
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.CallHelper(HOFF(setNZ));
                        break;
                    case 0x2: case 0x3: case 0x4: case 0x7: { // LSL / LSR / ASR / ROR
                        uint32_t st = (op == 0x2) ? 0u : (op == 0x3) ? 1u : (op == 0x4) ? 2u : 3u;
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32Imm(Reg::R8, st);
                        e.Mov32FromRbx(Reg::R9, RegOff(rs));
                        e.CallHelper(HOFF(shiftRegSetFlags));
                        e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                        break;
                    }
                    case 0x5: // ADC
                    case 0x6: // SBC
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::R8, RegOff(rs));
                        e.Mov32Imm(Reg::R9, 1);
                        e.CallHelper(op == 0x5 ? HOFF(adc) : HOFF(sbc));
                        e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                        break;
                    case 0x8: // TST
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::RAX, RegOff(rs));
                        e.And32(Reg::RDX, Reg::RAX);
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.CallHelper(HOFF(setNZ));
                        break;
                    case 0x9: // NEG
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.Mov32Imm(Reg::RDX, 0);
                        e.Mov32FromRbx(Reg::R8, RegOff(rs));
                        e.Mov32Imm(Reg::R9, 1);
                        e.CallHelper(HOFF(sub));
                        e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                        break;
                    case 0xA: // CMP
                    case 0xB: // CMN
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::R8, RegOff(rs));
                        e.Mov32Imm(Reg::R9, 1);
                        e.CallHelper(op == 0xA ? HOFF(sub) : HOFF(add));
                        break;
                    case 0xC: // ORR
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::RAX, RegOff(rs));
                        e.Or32(Reg::RDX, Reg::RAX);
                        e.Mov32ToRbx(RegOff(rd), Reg::RDX);
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.CallHelper(HOFF(setNZ));
                        break;
                    case 0xD: // MUL
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::RAX, RegOff(rs));
                        e.Imul32(Reg::RDX, Reg::RAX);
                        e.Mov32ToRbx(RegOff(rd), Reg::RDX);
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.CallHelper(HOFF(setNZ));
                        break;
                    case 0xE: // BIC
                        e.Mov32FromRbx(Reg::RDX, RegOff(rd));
                        e.Mov32FromRbx(Reg::RAX, RegOff(rs));
                        e.Not32(Reg::RAX);
                        e.And32(Reg::RDX, Reg::RAX);
                        e.Mov32ToRbx(RegOff(rd), Reg::RDX);
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.CallHelper(HOFF(setNZ));
                        break;
                    case 0xF: // MVN
                        e.Mov32FromRbx(Reg::RDX, RegOff(rs));
                        e.Not32(Reg::RDX);
                        e.Mov32ToRbx(RegOff(rd), Reg::RDX);
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.CallHelper(HOFF(setNZ));
                        break;
                }
            }
            // Format 5: Hi register ops / BX
            else if ((ins & 0xFC00u) == 0x4400u) {
                uint32_t op = (ins >> 8) & 3u;
                uint32_t rs = ((ins >> 3) & 7u) | ((ins & 0x40u) ? 8u : 0u);
                uint32_t rd = (ins & 7u) | ((ins & 0x80u) ? 8u : 0u);

                auto loadHiReg = [&](Reg dst, uint32_t rIdx) {
                    if (rIdx == 15) e.Mov32Imm(dst, pcVal);
                    else            e.Mov32FromRbx(dst, RegOff(rIdx));
                };

                if (op == 0) { // ADD
                    loadHiReg(Reg::RAX, rd);
                    loadHiReg(Reg::RDX, rs);
                    e.Add32(Reg::RAX, Reg::RDX);
                    if (rd == 15) {
                        e.And32Imm(Reg::RAX, ~1u);
                        e.Mov32ToRbx(RegOff(15), Reg::RAX);
                        emitReturnTrue();
                        continue;
                    } else {
                        e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                    }
                } else if (op == 1) { // CMP
                    e.Mov64(Reg::RCX, Reg::RBX);
                    loadHiReg(Reg::RDX, rd);
                    loadHiReg(Reg::R8, rs);
                    e.Mov32Imm(Reg::R9, 1);
                    e.CallHelper(HOFF(sub));
                } else if (op == 2) { // MOV
                    loadHiReg(Reg::RAX, rs);
                    if (rd == 15) {
                        e.And32Imm(Reg::RAX, ~1u);
                        e.Mov32ToRbx(RegOff(15), Reg::RAX);
                        emitReturnTrue();
                        continue;
                    } else {
                        e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                    }
                } else { // BX
                    e.Mov64(Reg::RCX, Reg::RBX);
                    loadHiReg(Reg::RDX, rs);
                    e.CallHelper(HOFF(branchExchange));
                    emitReturnTrue();
                    continue;
                }
            }
            // Format 6: PC-relative load
            else if ((ins & 0xF800u) == 0x4800u) {
                uint32_t rd = (ins >> 8) & 7u;
                uint32_t addr = (pcVal & ~3u) + ((ins & 0xFFu) << 2);
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, addr);
                e.CallHelper(HOFF(read32));
                e.Mov32ToRbx(RegOff(rd), Reg::RAX);
            }
            // Format 7 & 8: Load/store register offset
            else if ((ins & 0xF000u) == 0x5000u) {
                uint32_t ro = (ins >> 6) & 7u;
                uint32_t rb = (ins >> 3) & 7u;
                uint32_t rd = ins & 7u;
                uint32_t op = (ins >> 9) & 7u;
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32FromRbx(Reg::RDX, RegOff(rb));
                e.Mov32FromRbx(Reg::RAX, RegOff(ro));
                e.Add32(Reg::RDX, Reg::RAX);
                if (op <= 2) {
                    e.Mov32FromRbx(Reg::R8, RegOff(rd));
                    e.CallHelper(op == 0 ? HOFF(write32) : (op == 1 ? HOFF(write16) : HOFF(write8)));
                } else {
                    uint32_t fnOff = (op == 3) ? HOFF(read8Signed) :
                                     (op == 4) ? HOFF(read32Rotated) :
                                     (op == 5) ? HOFF(read16Rotated) :
                                     (op == 6) ? HOFF(read8) : HOFF(read16Signed);
                    e.CallHelper(fnOff);
                    e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                }
            }
            // Format 9: Load/store immediate offset
            else if ((ins & 0xE000u) == 0x6000u) {
                bool b = (ins & (1u << 12)) != 0;
                bool l = (ins & (1u << 11)) != 0;
                uint32_t imm = (ins >> 6) & 0x1Fu;
                uint32_t rb = (ins >> 3) & 7u;
                uint32_t rd = ins & 7u;
                uint32_t off = b ? imm : (imm << 2);
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32FromRbx(Reg::RDX, RegOff(rb));
                if (off) e.Add32Imm(Reg::RDX, off);
                if (l) {
                    e.CallHelper(b ? HOFF(read8) : HOFF(read32Rotated));
                    e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                } else {
                    e.Mov32FromRbx(Reg::R8, RegOff(rd));
                    e.CallHelper(b ? HOFF(write8) : HOFF(write32));
                }
            }
            // Format 10: Load/store halfword
            else if ((ins & 0xF000u) == 0x8000u) {
                bool l = (ins & (1u << 11)) != 0;
                uint32_t off = ((ins >> 6) & 0x1Fu) << 1;
                uint32_t rb = (ins >> 3) & 7u;
                uint32_t rd = ins & 7u;
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32FromRbx(Reg::RDX, RegOff(rb));
                if (off) e.Add32Imm(Reg::RDX, off);
                if (l) {
                    e.CallHelper(HOFF(read16Rotated));
                    e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                } else {
                    e.Mov32FromRbx(Reg::R8, RegOff(rd));
                    e.CallHelper(HOFF(write16));
                }
            }
            // Format 11: SP-relative load/store
            else if ((ins & 0xF000u) == 0x9000u) {
                bool l = (ins & (1u << 11)) != 0;
                uint32_t rd = (ins >> 8) & 7u;
                uint32_t off = (ins & 0xFFu) << 2;
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32FromRbx(Reg::RDX, RegOff(13));
                if (off) e.Add32Imm(Reg::RDX, off);
                if (l) {
                    e.CallHelper(HOFF(read32Rotated));
                    e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                } else {
                    e.Mov32FromRbx(Reg::R8, RegOff(rd));
                    e.CallHelper(HOFF(write32));
                }
            }
            // Format 12: Load address (ADD Rd, PC/SP, #imm)
            else if ((ins & 0xF000u) == 0xA000u) {
                bool sp = (ins & (1u << 11)) != 0;
                uint32_t rd = (ins >> 8) & 7u;
                uint32_t off = (ins & 0xFFu) << 2;
                if (sp) {
                    e.Mov32FromRbx(Reg::RAX, RegOff(13));
                    if (off) e.Add32Imm(Reg::RAX, off);
                    e.Mov32ToRbx(RegOff(rd), Reg::RAX);
                } else {
                    e.Mov32ImmToRbx(RegOff(rd), (pcVal & ~3u) + off);
                }
            }
            // Format 13: Add offset to SP
            else if ((ins & 0xFF00u) == 0xB000u) {
                uint32_t off = (ins & 0x7Fu) << 2;
                if (ins & 0x80u) e.Sub32RbxImm(RegOff(13), off);
                else             e.Add32RbxImm(RegOff(13), off);
            }
            // Format 14: PUSH / POP
            else if ((ins & 0xF600u) == 0xB400u) {
                bool l = (ins & (1u << 11)) != 0;
                bool pclr = (ins & (1u << 8)) != 0;
                uint32_t rlist = ins & 0xFFu;
                if (l) {
                    for (int i = 0; i < 8; ++i) {
                        if (rlist & (1u << i)) {
                            e.Mov64(Reg::RCX, Reg::RBX);
                            e.Mov32FromRbx(Reg::RDX, RegOff(13));
                            e.CallHelper(HOFF(read32));
                            e.Mov32ToRbx(RegOff(static_cast<uint32_t>(i)), Reg::RAX);
                            e.Add32RbxImm(RegOff(13), 4);
                        }
                    }
                    if (pclr) {
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.Mov32FromRbx(Reg::RDX, RegOff(13));
                        e.CallHelper(HOFF(read32));
                        e.And32Imm(Reg::RAX, ~1u);
                        e.Mov32ToRbx(RegOff(15), Reg::RAX);
                        e.Add32RbxImm(RegOff(13), 4);
                        emitReturnTrue();
                        continue;
                    }
                } else {
                    uint32_t count = pclr ? 1u : 0u;
                    for (int i = 0; i < 8; ++i) if (rlist & (1u << i)) count++;
                    e.Sub32RbxImm(RegOff(13), count * 4u);
                    uint32_t off = 0;
                    for (int i = 0; i < 8; ++i) {
                        if (rlist & (1u << i)) {
                            e.Mov64(Reg::RCX, Reg::RBX);
                            e.Mov32FromRbx(Reg::RDX, RegOff(13));
                            if (off) e.Add32Imm(Reg::RDX, off);
                            e.Mov32FromRbx(Reg::R8, RegOff(static_cast<uint32_t>(i)));
                            e.CallHelper(HOFF(write32));
                            off += 4u;
                        }
                    }
                    if (pclr) {
                        e.Mov64(Reg::RCX, Reg::RBX);
                        e.Mov32FromRbx(Reg::RDX, RegOff(13));
                        if (off) e.Add32Imm(Reg::RDX, off);
                        e.Mov32FromRbx(Reg::R8, RegOff(14));
                        e.CallHelper(HOFF(write32));
                    }
                }
            }
            // Format 15: LDMIA / STMIA
            else if ((ins & 0xF000u) == 0xC000u) {
                e.Mov32ImmToRbx(RegOff(15), nextPc);
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, ins);
                e.CallHelper(HOFF(execThumb));
                if ((ins & 0xFFu) == 0) {
                    e.Mov32FromRbx(Reg::RAX, RegOff(15));
                    e.CmpEaxImm32(nextPc);
                    returnTruePatches.push_back(e.JneRel32());
                }
            }
            // Format 17: SWI
            else if ((ins & 0xFF00u) == 0xDF00u) {
                uint8_t num = static_cast<uint8_t>(ins & 0xFFu);
                e.Mov32ImmToRbx(RegOff(15), nextPc);
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, num);
                e.CallHelper(HOFF(execSwi));
                e.CmpByteRbxZero(gba::CPU_OFF_HALTED);
                returnTruePatches.push_back(e.JneRel32());
                e.Mov32FromRbx(Reg::RAX, RegOff(15));
                e.CmpEaxImm32(nextPc);
                returnTruePatches.push_back(e.JneRel32());
            }
            // Format 16: Conditional branch
            else if ((ins & 0xF000u) == 0xD000u) {
                uint32_t cond = (ins >> 8) & 0xFu;
                int32_t off = SignExt(ins & 0xFFu, 8) << 1;
                uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~1u;
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, cond);
                e.CallHelper(HOFF(checkCond));
                e.TestEaxEax();
                size_t skipBranch = e.JeRel32();
                emitJumpTarget(target);
                e.PatchRel32(skipBranch, e.Pos());
            }
            // Format 18: Unconditional branch
            else if ((ins & 0xF800u) == 0xE000u) {
                int32_t off = SignExt(ins & 0x7FFu, 11) << 1;
                uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~1u;
                emitJumpTarget(target);
                continue;
            }
            // Format 19: BL prefix / suffix
            else if ((ins & 0xF000u) == 0xF000u) {
                bool h = (ins & (1u << 11)) != 0;
                if (!h) {
                    int32_t off = SignExt(ins & 0x7FFu, 11) << 12;
                    e.Mov32ImmToRbx(RegOff(14), pcVal + static_cast<uint32_t>(off));
                } else {
                    uint32_t off = (ins & 0x7FFu) << 1;
                    e.Mov32FromRbx(Reg::RAX, RegOff(14));
                    if (off) e.Add32Imm(Reg::RAX, off);
                    e.And32Imm(Reg::RAX, ~1u);
                    e.Mov32ImmToRbx(RegOff(14), nextPc | 1u);
                    e.Mov32ToRbx(RegOff(15), Reg::RAX);
                    emitReturnTrue();
                    continue;
                }
            }
            // Fallback for any other Thumb opcode
            else {
                e.Mov32ImmToRbx(RegOff(15), nextPc);
                e.Mov64(Reg::RCX, Reg::RBX);
                e.Mov32Imm(Reg::RDX, ins);
                e.CallHelper(HOFF(execThumb));
                e.Mov32FromRbx(Reg::RAX, RegOff(15));
                e.CmpEaxImm32(nextPc);
                returnTruePatches.push_back(e.JneRel32());
            }

            if (!HasLocalAddr(fn.insnAddrs, nextPc)) {
                e.Mov32ImmToRbx(RegOff(15), nextPc);
                emitReturnTrue();
            }
        }
    }

    // Shared return-true epilogue
    size_t retTruePos = e.Pos();
    e.Mov32Imm(Reg::RAX, 1);
    e.AddRspImm8(0x20);
    e.Pop(Reg::R12);
    e.Pop(Reg::RDI);
    e.Pop(Reg::RSI);
    e.Pop(Reg::RBP);
    e.Pop(Reg::RBX);
    e.Ret();

    // Patch all internal labels and return-true jumps
    for (const auto& p : labelPatches) {
        auto it = labelPos.find(p.targetGbaAddr);
        if (it != labelPos.end()) {
            e.PatchRel32(p.rel32Offset, it->second);
        }
    }
    for (size_t off : returnTruePatches) {
        e.PatchRel32(off, retTruePos);
    }
}

} // namespace

NativeCodegenResult ArmThumbToX64::CompileAll(
    const RomAnalyzer& analyzer,
    const std::vector<DiscoveredFunction>& funcs
) {
    NativeCodegenResult res;
    res.totalFunctions = static_cast<uint32_t>(funcs.size());
    res.funcRecords.reserve(funcs.size());

    X64Emitter emitter;
    emitter.code.reserve(funcs.size() * 1024);

    for (const auto& fn : funcs) {
        if (fn.insnAddrs.empty()) continue;
        emitter.Align16();

        gba::AotFuncRecord rec{};
        rec.startAddr = fn.insnAddrs.front();
        rec.endAddr = fn.insnAddrs.back();
        rec.codeOffset = static_cast<uint32_t>(emitter.Pos());
        rec.isThumb = fn.isThumb ? 1 : 0;

        CompileFunction(emitter, analyzer, fn);

        res.funcRecords.push_back(rec);
        res.totalInstructions += static_cast<uint32_t>(fn.insnAddrs.size());
    }

    res.x64CodeBlob = std::move(emitter.code);
    return res;
}

} // namespace recomp
