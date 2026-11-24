#include "arm_thumb_codegen.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace recomp {

namespace {

inline int32_t SignExt(uint32_t v, int bits) {
    uint32_t s = 32u - static_cast<uint32_t>(bits);
    return static_cast<int32_t>(v << s) >> s;
}

inline bool HasLocalAddr(const std::vector<uint32_t>& sortedAddrs, uint32_t target) {
    return std::binary_search(sortedAddrs.begin(), sortedAddrs.end(), target);
}

std::string Hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return buf;
}

std::string LabelName(uint32_t addr) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "L_%08X", addr);
    return buf;
}

std::string FuncName(uint32_t entryAddr, bool isThumb) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "recomp_%c_%08X", isThumb ? 't' : 'a', entryAddr);
    return buf;
}

std::string EscapeCppString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (static_cast<unsigned char>(c) >= 32) out.push_back(c);
    }
    return out;
}

} // namespace

void ArmThumbCodegen::EmitArmInstruction(
    std::ostream& out,
    uint32_t pc,
    uint32_t ins,
    const std::vector<uint32_t>& localAddrs
) {
    uint32_t cond = ins >> 28;
    uint32_t nextPc = pc + 4u;
    uint32_t pcVal = pc + 8u;

    out << "    " << LabelName(pc) << ":\n";
    out << "    cpu.cycles += 2; cpu.statRecompInstrs++;\n";

    // Unconditional / conditional check
    bool hasCond = (cond != 0xE);
    if (cond == 0xF) {
        // Never / undefined on ARMv4T
        out << "    cpu.r[15] = " << Hex32(nextPc) << ";\n";
        return;
    }

    if (hasCond) {
        out << "    if (cpu.CheckCond(" << cond << "u)) {\n";
    }

    auto emitJump = [&](uint32_t target) {
        if (HasLocalAddr(localAddrs, target)) {
            out << "        if (cpu.cycles >= cpu.nextEventCycles) { cpu.r[15] = " << Hex32(target) << "; return true; }\n";
            out << "        goto " << LabelName(target) << ";\n";
        } else {
            out << "        cpu.r[15] = " << Hex32(target) << "; return true;\n";
        }
    };

    // 1. BX Rn
    if ((ins & 0x0FFFFFF0u) == 0x012FFF10u) {
        uint32_t rn = ins & 0xFu;
        if (rn == 15) out << "        cpu.BranchExchange(" << Hex32(pcVal) << "); return true;\n";
        else          out << "        cpu.BranchExchange(cpu.r[" << rn << "]); return true;\n";
    }
    // 2. B / BL
    else if ((ins & 0x0E000000u) == 0x0A000000u) {
        int32_t off = SignExt(ins & 0x00FFFFFFu, 24) << 2;
        uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~3u;
        if (ins & (1u << 24)) {
            out << "        cpu.r[14] = " << Hex32(nextPc) << ";\n";
            out << "        cpu.r[15] = " << Hex32(target) << "; return true;\n";
        } else {
            emitJump(target);
        }
    }
    // 3. SWI
    else if ((ins & 0x0F000000u) == 0x0F000000u) {
        uint8_t num = static_cast<uint8_t>((ins >> 16) & 0xFFu);
        out << "        cpu.r[15] = " << Hex32(nextPc) << ";\n";
        out << "        cpu.ExecSwi(" << static_cast<unsigned>(num) << "u);\n";
        out << "        if (cpu.halted || cpu.r[15] != " << Hex32(nextPc) << ") return true;\n";
    }
    // 4. All other ARM instructions: delegate to exact inlineable helper or emit direct C++ for hot ops
    //    For ARM Data Processing (immediate MOV/ADD/SUB/CMP/ORR/AND) and LDR/STR, emit direct C++!
    else if ((ins & 0x0C000000u) == 0x00000000u &&
             (ins & 0x0FBF0FFFu) != 0x010F0000u &&
             (ins & 0x0DB0F000u) != 0x0120F000u &&
             (ins & 0x0FC000F0u) != 0x00000090u &&
             (ins & 0x0F8000F0u) != 0x00800090u &&
             (ins & 0x0FB00FF0u) != 0x01000090u &&
             !((ins & 0x0E000090u) == 0x00000090u && (ins & 0x00000060u) != 0) &&
             (ins & (1u << 25)) != 0 && // Immediate operand2
             ((ins >> 12) & 0xFu) != 15 && // Rd != PC
             ((ins >> 16) & 0xFu) != 15)   // Rn != PC
    {
        uint32_t op = (ins >> 21) & 0xFu;
        bool s = (ins & (1u << 20)) != 0;
        uint32_t rn = (ins >> 16) & 0xFu;
        uint32_t rd = (ins >> 12) & 0xFu;
        uint32_t rot = ((ins >> 8) & 0xFu) * 2u;
        uint32_t imm = (rot == 0) ? (ins & 0xFFu) : (((ins & 0xFFu) >> rot) | ((ins & 0xFFu) << (32u - rot)));
        bool shiftCarry = (rot != 0) ? ((imm >> 31) != 0) : false;

        switch (op) {
            case 0x0: // AND
                out << "        cpu.r[" << rd << "] = cpu.r[" << rn << "] & " << Hex32(imm) << ";\n";
                if (s) out << "        cpu.SetNZC(cpu.r[" << rd << "], " << (rot ? (shiftCarry ? "true" : "false") : "cpu.FlagC()") << ");\n";
                break;
            case 0x1: // EOR
                out << "        cpu.r[" << rd << "] = cpu.r[" << rn << "] ^ " << Hex32(imm) << ";\n";
                if (s) out << "        cpu.SetNZC(cpu.r[" << rd << "], " << (rot ? (shiftCarry ? "true" : "false") : "cpu.FlagC()") << ");\n";
                break;
            case 0x2: // SUB
                out << "        cpu.r[" << rd << "] = cpu.Sub(cpu.r[" << rn << "], " << Hex32(imm) << ", " << (s ? "true" : "false") << ");\n";
                break;
            case 0x4: // ADD
                out << "        cpu.r[" << rd << "] = cpu.Add(cpu.r[" << rn << "], " << Hex32(imm) << ", " << (s ? "true" : "false") << ");\n";
                break;
            case 0xA: // CMP
                out << "        cpu.Sub(cpu.r[" << rn << "], " << Hex32(imm) << ", true);\n";
                break;
            case 0xC: // ORR
                out << "        cpu.r[" << rd << "] = cpu.r[" << rn << "] | " << Hex32(imm) << ";\n";
                if (s) out << "        cpu.SetNZC(cpu.r[" << rd << "], " << (rot ? (shiftCarry ? "true" : "false") : "cpu.FlagC()") << ");\n";
                break;
            case 0xD: // MOV
                out << "        cpu.r[" << rd << "] = " << Hex32(imm) << ";\n";
                if (s) out << "        cpu.SetNZC(cpu.r[" << rd << "], " << (rot ? (shiftCarry ? "true" : "false") : "cpu.FlagC()") << ");\n";
                break;
            case 0xE: // BIC
                out << "        cpu.r[" << rd << "] = cpu.r[" << rn << "] & " << Hex32(~imm) << ";\n";
                if (s) out << "        cpu.SetNZC(cpu.r[" << rd << "], " << (rot ? (shiftCarry ? "true" : "false") : "cpu.FlagC()") << ");\n";
                break;
            case 0xF: // MVN
                out << "        cpu.r[" << rd << "] = " << Hex32(~imm) << ";\n";
                if (s) out << "        cpu.SetNZC(cpu.r[" << rd << "], " << (rot ? (shiftCarry ? "true" : "false") : "cpu.FlagC()") << ");\n";
                break;
            default:
                out << "        cpu.r[15] = " << Hex32(nextPc) << "; cpu.ExecArm(" << Hex32(ins) << ");\n";
                out << "        if (cpu.r[15] != " << Hex32(nextPc) << " || cpu.IsThumb()) return true;\n";
                break;
        }
    } else {
        // Exact ARM7TDMI execution for complex instructions (LDM/STM, MSR/MRS, PC-writing ALU, etc.)
        out << "        cpu.r[15] = " << Hex32(nextPc) << "; cpu.ExecArm(" << Hex32(ins) << ");\n";
        out << "        if (cpu.r[15] != " << Hex32(nextPc) << " || cpu.IsThumb()) return true;\n";
    }

    if (hasCond) {
        out << "    }\n";
    }

    // If next instruction is not in this function, exit back to dispatcher
    if (!HasLocalAddr(localAddrs, nextPc)) {
        out << "    cpu.r[15] = " << Hex32(nextPc) << "; return true;\n";
    }
}

void ArmThumbCodegen::EmitThumbInstruction(
    std::ostream& out,
    uint32_t pc,
    uint16_t ins,
    const std::vector<uint32_t>& localAddrs
) {
    uint32_t nextPc = pc + 2u;
    uint32_t pcVal = pc + 4u;

    out << "    " << LabelName(pc) << ":\n";
    out << "    cpu.cycles += 2; cpu.statRecompInstrs++;\n";

    auto emitJump = [&](uint32_t target) {
        if (HasLocalAddr(localAddrs, target)) {
            out << "        if (cpu.cycles >= cpu.nextEventCycles) { cpu.r[15] = " << Hex32(target) << "; return true; }\n";
            out << "        goto " << LabelName(target) << ";\n";
        } else {
            out << "        cpu.r[15] = " << Hex32(target) << "; return true;\n";
        }
    };

    // Format 1: Move shifted register (LSL / LSR / ASR #imm)
    if ((ins & 0xE000u) == 0x0000u && (ins & 0x1800u) != 0x1800u) {
        uint32_t op = (ins >> 11) & 3u;
        uint32_t imm = (ins >> 6) & 0x1Fu;
        uint32_t rs = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        out << "    { bool c = cpu.FlagC(); cpu.r[" << rd << "] = cpu.ShiftImm(cpu.r[" << rs << "], "
            << op << "u, " << imm << "u, c); cpu.SetNZC(cpu.r[" << rd << "], c); }\n";
    }
    // Format 2: ADD / SUB (3-register or 3-bit imm)
    else if ((ins & 0xF800u) == 0x1800u) {
        bool imm = (ins & (1u << 10)) != 0;
        bool sub = (ins & (1u << 9)) != 0;
        uint32_t rnOrImm = (ins >> 6) & 7u;
        uint32_t rs = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        out << "    cpu.r[" << rd << "] = cpu." << (sub ? "Sub" : "Add") << "(cpu.r[" << rs << "], ";
        if (imm) out << rnOrImm << "u";
        else     out << "cpu.r[" << rnOrImm << "]";
        out << ", true);\n";
    }
    // Format 3: MOV / CMP / ADD / SUB #imm8
    else if ((ins & 0xE000u) == 0x2000u) {
        uint32_t op = (ins >> 11) & 3u;
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t imm = ins & 0xFFu;
        switch (op) {
            case 0: out << "    cpu.r[" << rd << "] = " << imm << "u; cpu.SetNZ(" << imm << "u);\n"; break;
            case 1: out << "    cpu.Sub(cpu.r[" << rd << "], " << imm << "u, true);\n"; break;
            case 2: out << "    cpu.r[" << rd << "] = cpu.Add(cpu.r[" << rd << "], " << imm << "u, true);\n"; break;
            case 3: out << "    cpu.r[" << rd << "] = cpu.Sub(cpu.r[" << rd << "], " << imm << "u, true);\n"; break;
        }
    }
    // Format 4: ALU operations
    else if ((ins & 0xFC00u) == 0x4000u) {
        uint32_t op = (ins >> 6) & 0xFu;
        uint32_t rs = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        switch (op) {
            case 0x0: out << "    cpu.r[" << rd << "] &= cpu.r[" << rs << "]; cpu.SetNZ(cpu.r[" << rd << "]);\n"; break;
            case 0x1: out << "    cpu.r[" << rd << "] ^= cpu.r[" << rs << "]; cpu.SetNZ(cpu.r[" << rd << "]);\n"; break;
            case 0x2: out << "    { bool c; cpu.r[" << rd << "] = cpu.ShiftReg(cpu.r[" << rd << "], 0u, cpu.r[" << rs << "], c); cpu.SetNZC(cpu.r[" << rd << "], c); }\n"; break;
            case 0x3: out << "    { bool c; cpu.r[" << rd << "] = cpu.ShiftReg(cpu.r[" << rd << "], 1u, cpu.r[" << rs << "], c); cpu.SetNZC(cpu.r[" << rd << "], c); }\n"; break;
            case 0x4: out << "    { bool c; cpu.r[" << rd << "] = cpu.ShiftReg(cpu.r[" << rd << "], 2u, cpu.r[" << rs << "], c); cpu.SetNZC(cpu.r[" << rd << "], c); }\n"; break;
            case 0x5: out << "    cpu.r[" << rd << "] = cpu.Adc(cpu.r[" << rd << "], cpu.r[" << rs << "], true);\n"; break;
            case 0x6: out << "    cpu.r[" << rd << "] = cpu.Sbc(cpu.r[" << rd << "], cpu.r[" << rs << "], true);\n"; break;
            case 0x7: out << "    { bool c; cpu.r[" << rd << "] = cpu.ShiftReg(cpu.r[" << rd << "], 3u, cpu.r[" << rs << "], c); cpu.SetNZC(cpu.r[" << rd << "], c); }\n"; break;
            case 0x8: out << "    cpu.SetNZ(cpu.r[" << rd << "] & cpu.r[" << rs << "]);\n"; break;
            case 0x9: out << "    cpu.r[" << rd << "] = cpu.Sub(0u, cpu.r[" << rs << "], true);\n"; break;
            case 0xA: out << "    cpu.Sub(cpu.r[" << rd << "], cpu.r[" << rs << "], true);\n"; break;
            case 0xB: out << "    cpu.Add(cpu.r[" << rd << "], cpu.r[" << rs << "], true);\n"; break;
            case 0xC: out << "    cpu.r[" << rd << "] |= cpu.r[" << rs << "]; cpu.SetNZ(cpu.r[" << rd << "]);\n"; break;
            case 0xD: out << "    cpu.r[" << rd << "] *= cpu.r[" << rs << "]; cpu.SetNZ(cpu.r[" << rd << "]);\n"; break;
            case 0xE: out << "    cpu.r[" << rd << "] &= ~cpu.r[" << rs << "]; cpu.SetNZ(cpu.r[" << rd << "]);\n"; break;
            case 0xF: out << "    cpu.r[" << rd << "] = ~cpu.r[" << rs << "]; cpu.SetNZ(cpu.r[" << rd << "]);\n"; break;
        }
    }
    // Format 5: Hi register ops / BX
    else if ((ins & 0xFC00u) == 0x4400u) {
        uint32_t op = (ins >> 8) & 3u;
        uint32_t rs = ((ins >> 3) & 7u) | ((ins & 0x40u) ? 8u : 0u);
        uint32_t rd = (ins & 7u) | ((ins & 0x80u) ? 8u : 0u);
        std::string rsExpr = (rs == 15) ? Hex32(pcVal) : ("cpu.r[" + std::to_string(rs) + "]");
        std::string rdExpr = (rd == 15) ? Hex32(pcVal) : ("cpu.r[" + std::to_string(rd) + "]");

        if (op == 0) {
            if (rd == 15) out << "    cpu.r[15] = (" << rdExpr << " + " << rsExpr << ") & ~1u; return true;\n";
            else          out << "    cpu.r[" << rd << "] = " << rdExpr << " + " << rsExpr << ";\n";
        } else if (op == 1) {
            out << "    cpu.Sub(" << rdExpr << ", " << rsExpr << ", true);\n";
        } else if (op == 2) {
            if (rd == 15) out << "    cpu.r[15] = " << rsExpr << " & ~1u; return true;\n";
            else          out << "    cpu.r[" << rd << "] = " << rsExpr << ";\n";
        } else {
            out << "    cpu.BranchExchange(" << rsExpr << "); return true;\n";
        }
    }
    // Format 6: PC-relative load (statically resolved address!)
    else if ((ins & 0xF800u) == 0x4800u) {
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t addr = (pcVal & ~3u) + ((ins & 0xFFu) << 2);
        out << "    cpu.r[" << rd << "] = cpu.bus.Read32(" << Hex32(addr) << ");\n";
    }
    // Format 7 & 8: Load/store with register offset
    else if ((ins & 0xF000u) == 0x5000u) {
        uint32_t ro = (ins >> 6) & 7u;
        uint32_t rb = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        uint32_t op = (ins >> 9) & 7u;
        std::string addr = "cpu.r[" + std::to_string(rb) + "] + cpu.r[" + std::to_string(ro) + "]";
        switch (op) {
            case 0: out << "    cpu.bus.Write32(" << addr << ", cpu.r[" << rd << "]);\n"; break;
            case 1: out << "    cpu.bus.Write16(" << addr << ", static_cast<uint16_t>(cpu.r[" << rd << "]));\n"; break;
            case 2: out << "    cpu.bus.Write8(" << addr << ", static_cast<uint8_t>(cpu.r[" << rd << "]));\n"; break;
            case 3: out << "    cpu.r[" << rd << "] = cpu.Read8Signed(" << addr << ");\n"; break;
            case 4: out << "    cpu.r[" << rd << "] = cpu.Read32Rotated(" << addr << ");\n"; break;
            case 5: out << "    cpu.r[" << rd << "] = cpu.Read16Rotated(" << addr << ");\n"; break;
            case 6: out << "    cpu.r[" << rd << "] = cpu.bus.Read8(" << addr << ");\n"; break;
            case 7: out << "    cpu.r[" << rd << "] = cpu.Read16Signed(" << addr << ");\n"; break;
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
        std::string addr = "cpu.r[" + std::to_string(rb) + "] + " + std::to_string(off) + "u";
        if (l) {
            if (b) out << "    cpu.r[" << rd << "] = cpu.bus.Read8(" << addr << ");\n";
            else   out << "    cpu.r[" << rd << "] = cpu.Read32Rotated(" << addr << ");\n";
        } else {
            if (b) out << "    cpu.bus.Write8(" << addr << ", static_cast<uint8_t>(cpu.r[" << rd << "]));\n";
            else   out << "    cpu.bus.Write32(" << addr << ", cpu.r[" << rd << "]);\n";
        }
    }
    // Format 10: Load/store halfword
    else if ((ins & 0xF000u) == 0x8000u) {
        bool l = (ins & (1u << 11)) != 0;
        uint32_t off = ((ins >> 6) & 0x1Fu) << 1;
        uint32_t rb = (ins >> 3) & 7u;
        uint32_t rd = ins & 7u;
        std::string addr = "cpu.r[" + std::to_string(rb) + "] + " + std::to_string(off) + "u";
        if (l) out << "    cpu.r[" << rd << "] = cpu.Read16Rotated(" << addr << ");\n";
        else   out << "    cpu.bus.Write16(" << addr << ", static_cast<uint16_t>(cpu.r[" << rd << "]));\n";
    }
    // Format 11: SP-relative load/store
    else if ((ins & 0xF000u) == 0x9000u) {
        bool l = (ins & (1u << 11)) != 0;
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t off = (ins & 0xFFu) << 2;
        if (l) out << "    cpu.r[" << rd << "] = cpu.Read32Rotated(cpu.r[13] + " << off << "u);\n";
        else   out << "    cpu.bus.Write32(cpu.r[13] + " << off << "u, cpu.r[" << rd << "]);\n";
    }
    // Format 12: Load address (ADD Rd, PC/SP, #imm)
    else if ((ins & 0xF000u) == 0xA000u) {
        bool sp = (ins & (1u << 11)) != 0;
        uint32_t rd = (ins >> 8) & 7u;
        uint32_t off = (ins & 0xFFu) << 2;
        if (sp) out << "    cpu.r[" << rd << "] = cpu.r[13] + " << off << "u;\n";
        else    out << "    cpu.r[" << rd << "] = " << Hex32((pcVal & ~3u) + off) << ";\n";
    }
    // Format 13: Add offset to SP
    else if ((ins & 0xFF00u) == 0xB000u) {
        uint32_t off = (ins & 0x7Fu) << 2;
        if (ins & 0x80u) out << "    cpu.r[13] -= " << off << "u;\n";
        else             out << "    cpu.r[13] += " << off << "u;\n";
    }
    // Format 14: PUSH / POP
    else if ((ins & 0xF600u) == 0xB400u) {
        bool l = (ins & (1u << 11)) != 0;
        bool pclr = (ins & (1u << 8)) != 0;
        uint32_t rlist = ins & 0xFFu;
        if (l) {
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1u << i)) {
                    out << "    cpu.r[" << i << "] = cpu.bus.Read32(cpu.r[13]); cpu.r[13] += 4u;\n";
                }
            }
            if (pclr) {
                out << "    cpu.r[15] = cpu.bus.Read32(cpu.r[13]) & ~1u; cpu.r[13] += 4u; return true;\n";
                return;
            }
        } else {
            uint32_t count = pclr ? 1u : 0u;
            for (int i = 0; i < 8; ++i) if (rlist & (1u << i)) count++;
            out << "    cpu.r[13] -= " << (count * 4u) << "u;\n";
            uint32_t off = 0;
            for (int i = 0; i < 8; ++i) {
                if (rlist & (1u << i)) {
                    out << "    cpu.bus.Write32(cpu.r[13] + " << off << "u, cpu.r[" << i << "]);\n";
                    off += 4u;
                }
            }
            if (pclr) {
                out << "    cpu.bus.Write32(cpu.r[13] + " << off << "u, cpu.r[14]);\n";
            }
        }
    }
    // Format 15: LDMIA / STMIA
    else if ((ins & 0xF000u) == 0xC000u) {
        out << "    cpu.r[15] = " << Hex32(nextPc) << "; cpu.ExecThumb(" << static_cast<unsigned>(ins) << "u);\n";
        if ((ins & 0xFFu) == 0) {
            out << "    if (cpu.r[15] != " << Hex32(nextPc) << ") return true;\n";
        }
    }
    // Format 17: SWI
    else if ((ins & 0xFF00u) == 0xDF00u) {
        uint8_t num = static_cast<uint8_t>(ins & 0xFFu);
        out << "    cpu.r[15] = " << Hex32(nextPc) << ";\n";
        out << "    cpu.ExecSwi(" << static_cast<unsigned>(num) << "u);\n";
        out << "    if (cpu.halted || cpu.r[15] != " << Hex32(nextPc) << ") return true;\n";
    }
    // Format 16: Conditional branch
    else if ((ins & 0xF000u) == 0xD000u) {
        uint32_t cond = (ins >> 8) & 0xFu;
        int32_t off = SignExt(ins & 0xFFu, 8) << 1;
        uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~1u;
        out << "    if (cpu.CheckCond(" << cond << "u)) {\n";
        emitJump(target);
        out << "    }\n";
    }
    // Format 18: Unconditional branch
    else if ((ins & 0xF800u) == 0xE000u) {
        int32_t off = SignExt(ins & 0x7FFu, 11) << 1;
        uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~1u;
        out << "    {\n";
        emitJump(target);
        out << "    }\n";
        return;
    }
    // Format 19: Long branch with link (BL prefix / suffix)
    else if ((ins & 0xF000u) == 0xF000u) {
        bool h = (ins & (1u << 11)) != 0;
        if (!h) {
            int32_t off = SignExt(ins & 0x7FFu, 11) << 12;
            out << "    cpu.r[14] = " << Hex32(pcVal + static_cast<uint32_t>(off)) << ";\n";
        } else {
            uint32_t off = (ins & 0x7FFu) << 1;
            out << "    { uint32_t target = (cpu.r[14] + " << off << "u) & ~1u; "
                << "cpu.r[14] = " << Hex32(nextPc | 1u) << "; "
                << "cpu.r[15] = target; return true; }\n";
            return;
        }
    } else {
        out << "    cpu.r[15] = " << Hex32(nextPc) << "; cpu.ExecThumb(" << static_cast<unsigned>(ins) << "u);\n";
        out << "    if (cpu.r[15] != " << Hex32(nextPc) << ") return true;\n";
    }

    if (!HasLocalAddr(localAddrs, nextPc)) {
        out << "    cpu.r[15] = " << Hex32(nextPc) << "; return true;\n";
    }
}

void ArmThumbCodegen::EmitFunction(
    std::ostream& out,
    const RomAnalyzer& analyzer,
    const DiscoveredFunction& fn
) {
    out << "bool " << FuncName(fn.entryAddr, fn.isThumb) << "(gba::GbaCpu* cpuPtr, const gba::RuntimeHelpers*) {\n";
    out << "    gba::GbaCpu& cpu = *cpuPtr;\n";
    out << "    switch (cpu.r[15]) {\n";
    for (uint32_t addr : fn.insnAddrs) {
        out << "        case " << Hex32(addr) << ": goto " << LabelName(addr) << ";\n";
    }
    out << "        default: return false;\n";
    out << "    }\n";

    for (uint32_t addr : fn.insnAddrs) {
        if (fn.isThumb) {
            EmitThumbInstruction(out, addr, analyzer.Read16(addr), fn.insnAddrs);
        } else {
            EmitArmInstruction(out, addr, analyzer.Read32(addr), fn.insnAddrs);
        }
    }
    out << "    return true;\n";
    out << "}\n\n";
}

CodegenResult ArmThumbCodegen::GenerateAll(
    const RomAnalyzer& analyzer,
    const std::vector<DiscoveredFunction>& funcs,
    const std::string& emitDir,
    const std::string& romSourcePath,
    uint32_t funcsPerChunk
) {
    namespace fs = std::filesystem;
    fs::create_directories(emitDir);

    CodegenResult res;
    res.totalFunctions = static_cast<uint32_t>(funcs.size());

    // 1. Emit chunked C++ translation units
    uint32_t chunkIdx = 0;
    for (size_t i = 0; i < funcs.size(); i += funcsPerChunk) {
        size_t end = std::min(funcs.size(), i + static_cast<size_t>(funcsPerChunk));
        char fname[64];
        std::snprintf(fname, sizeof(fname), "recomp_chunk_%03u.cpp", chunkIdx++);
        fs::path chunkPath = fs::path(emitDir) / fname;

        std::ofstream out(chunkPath);
        out << "// Auto-generated by recomp-ariaofsorrow static recompiler\n";
        out << "#include \"gba_cpu.h\"\n\n";
        out << "namespace gba_recomp {\n\n";

        for (size_t j = i; j < end; ++j) {
            EmitFunction(out, analyzer, funcs[j]);
            res.totalInstructions += static_cast<uint32_t>(funcs[j].insnAddrs.size());
        }

        out << "} // namespace gba_recomp\n";
        res.generatedCppFiles.push_back(chunkPath.string());
    }

    // 2. Emit recomp_table.cpp (dispatch table + metadata)
    {
        fs::path tablePath = fs::path(emitDir) / "recomp_table.cpp";
        std::ofstream out(tablePath);
        out << "// Auto-generated dispatch table & ROM metadata\n";
        out << "#include \"gba_types.h\"\n";
        out << "#include <algorithm>\n\n";
        out << "namespace gba_recomp {\n";
        for (const auto& fn : funcs) {
            out << "bool " << FuncName(fn.entryAddr, fn.isThumb) << "(gba::GbaCpu* cpu, const gba::RuntimeHelpers*);\n";
        }
        out << "} // namespace gba_recomp\n\n";

        out << "namespace gba {\n\n";
        out << "static const RecompEntry kRecompTable[] = {\n";
        for (const auto& fn : funcs) {
            uint32_t startAddr = fn.insnAddrs.front();
            uint32_t endAddr = fn.insnAddrs.back();
            out << "    { " << Hex32(startAddr) << ", " << Hex32(endAddr) << ", "
                << (fn.isThumb ? "1" : "0") << ", &gba_recomp::" << FuncName(fn.entryAddr, fn.isThumb) << " },\n";
        }
        out << "};\n\n";

        out << "RecompFunc LookupRecompiledFunction(uint32_t addrWithModeBit) {\n";
        out << "    constexpr size_t N = sizeof(kRecompTable) / sizeof(kRecompTable[0]);\n";
        out << "    uint8_t wantThumb = static_cast<uint8_t>(addrWithModeBit & 1u);\n";
        out << "    uint32_t addr = addrWithModeBit & ~1u;\n";
        out << "    size_t lo = 0, hi = N;\n";
        out << "    while (lo < hi) {\n";
        out << "        size_t mid = lo + ((hi - lo) >> 1);\n";
        out << "        if (kRecompTable[mid].startAddr <= addr) lo = mid + 1;\n";
        out << "        else hi = mid;\n";
        out << "    }\n";
        out << "    if (lo == 0) return nullptr;\n";
        out << "    size_t idx = lo - 1;\n";
        out << "    for (int back = 0; back < 4 && idx >= static_cast<size_t>(back); ++back) {\n";
        out << "        const auto& e = kRecompTable[idx - back];\n";
        out << "        if (e.isThumb == wantThumb && addr >= e.startAddr && addr <= e.endAddr) return e.fn;\n";
        out << "    }\n";
        out << "    return nullptr;\n";
        out << "}\n\n";

        const auto& h = analyzer.Header();
        out << "const RecompRomInfo& GetRecompRomInfo() {\n";
        out << "    static const RecompRomInfo kInfo = {\n";
        out << "        \"" << EscapeCppString(h.title) << "\",\n";
        out << "        \"" << EscapeCppString(h.gameCode) << "\",\n";
        out << "        \"" << EscapeCppString(h.makerCode) << "\",\n";
        out << "        \"" << EscapeCppString(h.filename) << "\",\n";
        out << "        " << h.romSize << "u,\n";
        out << "        " << res.totalFunctions << "u,\n";
        out << "        " << res.totalInstructions << "u\n";
        out << "    };\n";
        out << "    return kInfo;\n";
        out << "}\n\n";
        out << "} // namespace gba\n";

        res.generatedCppFiles.push_back(tablePath.string());
    }

    // 3. Copy ROM binary into emitDir as embedded_rom.bin and emit rom_data.cpp using #embed!
    {
        fs::path binPath = fs::path(emitDir) / "embedded_rom.bin";
        fs::copy_file(romSourcePath, binPath, fs::copy_options::overwrite_existing);

        fs::path romCppPath = fs::path(emitDir) / "rom_data.cpp";
        std::ofstream out(romCppPath);
        out << "// Auto-generated embedded GBA ROM binary\n";
        out << "#include \"gba_types.h\"\n\n";
        out << "namespace gba {\n\n";
        out << "alignas(16) static const uint8_t kEmbeddedRom[] = {\n";
        out << "#embed \"embedded_rom.bin\"\n";
        out << "};\n\n";
        out << "const uint8_t* GetEmbeddedRomData(size_t& outSize) {\n";
        out << "    outSize = sizeof(kEmbeddedRom);\n";
        out << "    return kEmbeddedRom;\n";
        out << "}\n\n";
        out << "} // namespace gba\n";

        res.generatedCppFiles.push_back(romCppPath.string());
    }

    return res;
}

} // namespace recomp
