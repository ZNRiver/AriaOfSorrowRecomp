#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp {

// Minimal, self-contained x86_64 machine-code emitter (Windows x64 ABI)
class X64Emitter {
public:
    enum Reg64 : uint8_t {
        RAX = 0, RCX = 1, RDX = 2, RBX = 3,
        RSP = 4, RBP = 5, RSI = 6, RDI = 7,
        R8  = 8, R9  = 9, R10 = 10, R11 = 11,
        R12 = 12, R13 = 13, R14 = 14, R15 = 15
    };

    std::vector<uint8_t> code;

    size_t Pos() const { return code.size(); }

    void Emit8(uint8_t b) { code.push_back(b); }
    void Emit32(uint32_t v) {
        code.push_back(static_cast<uint8_t>(v & 0xFF));
        code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        code.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        code.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }

    void PatchRel32(size_t rel32Offset, size_t targetPos) {
        int32_t disp = static_cast<int32_t>(static_cast<int64_t>(targetPos) - static_cast<int64_t>(rel32Offset + 4));
        std::memcpy(code.data() + rel32Offset, &disp, 4);
    }

    // Align next function to 16 bytes with INT3 (0xCC) padding
    void Align16() {
        while ((code.size() & 15u) != 0) Emit8(0xCC);
    }

    // push reg64 / pop reg64
    void Push(Reg64 r) {
        if (r >= R8) Emit8(0x41);
        Emit8(static_cast<uint8_t>(0x50 + (r & 7)));
    }
    void Pop(Reg64 r) {
        if (r >= R8) Emit8(0x41);
        Emit8(static_cast<uint8_t>(0x58 + (r & 7)));
    }

    // sub rsp, imm8 / add rsp, imm8
    void SubRspImm8(uint8_t imm) { Emit8(0x48); Emit8(0x83); Emit8(0xEC); Emit8(imm); }
    void AddRspImm8(uint8_t imm) { Emit8(0x48); Emit8(0x83); Emit8(0xC4); Emit8(imm); }

    // mov dst64, src64
    void Mov64(Reg64 dst, Reg64 src) {
        uint8_t rex = 0x48 | ((src >= R8) ? 4 : 0) | ((dst >= R8) ? 1 : 0);
        Emit8(rex);
        Emit8(0x89);
        Emit8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }

    // mov r32, imm32
    void Mov32Imm(Reg64 dst, uint32_t imm) {
        if (dst >= R8) Emit8(0x41);
        Emit8(static_cast<uint8_t>(0xB8 + (dst & 7)));
        Emit32(imm);
    }

    // mov r32, [rbx + disp32]
    void Mov32FromRbx(Reg64 dst, uint32_t disp) {
        if (dst >= R8) Emit8(0x44);
        Emit8(0x8B);
        if (disp < 128) {
            Emit8(static_cast<uint8_t>(0x43 | ((dst & 7) << 3)));
            Emit8(static_cast<uint8_t>(disp));
        } else {
            Emit8(static_cast<uint8_t>(0x83 | ((dst & 7) << 3)));
            Emit32(disp);
        }
    }

    // mov [rbx + disp], r32
    void Mov32ToRbx(uint32_t disp, Reg64 src) {
        if (src >= R8) Emit8(0x44);
        Emit8(0x89);
        if (disp < 128) {
            Emit8(static_cast<uint8_t>(0x43 | ((src & 7) << 3)));
            Emit8(static_cast<uint8_t>(disp));
        } else {
            Emit8(static_cast<uint8_t>(0x83 | ((src & 7) << 3)));
            Emit32(disp);
        }
    }

    // mov dword [rbx + disp], imm32
    void Mov32ImmToRbx(uint32_t disp, uint32_t imm) {
        Emit8(0xC7);
        if (disp < 128) {
            Emit8(0x43);
            Emit8(static_cast<uint8_t>(disp));
        } else {
            Emit8(0x83);
            Emit32(disp);
        }
        Emit32(imm);
    }

    // mov r64, [rbx + disp]
    void Mov64FromRbx(Reg64 dst, uint32_t disp) {
        Emit8(static_cast<uint8_t>(0x48 | ((dst >= R8) ? 4 : 0)));
        Emit8(0x8B);
        if (disp < 128) {
            Emit8(static_cast<uint8_t>(0x43 | ((dst & 7) << 3)));
            Emit8(static_cast<uint8_t>(disp));
        } else {
            Emit8(static_cast<uint8_t>(0x83 | ((dst & 7) << 3)));
            Emit32(disp);
        }
    }

    // add qword [rbx + disp], imm8
    void Add64RbxImm8(uint32_t disp, uint8_t imm) {
        Emit8(0x48);
        Emit8(0x83);
        if (disp < 128) {
            Emit8(0x43);
            Emit8(static_cast<uint8_t>(disp));
        } else {
            Emit8(0x83);
            Emit32(disp);
        }
        Emit8(imm);
    }

    // add/sub dword [rbx + disp], imm32
    void Add32RbxImm(uint32_t disp, uint32_t imm) {
        Emit8(0x81);
        if (disp < 128) { Emit8(0x43); Emit8(static_cast<uint8_t>(disp)); }
        else            { Emit8(0x83); Emit32(disp); }
        Emit32(imm);
    }
    void Sub32RbxImm(uint32_t disp, uint32_t imm) {
        Emit8(0x81);
        if (disp < 128) { Emit8(0x6B); Emit8(static_cast<uint8_t>(disp)); }
        else            { Emit8(0xAB); Emit32(disp); }
        Emit32(imm);
    }

    // ALU r32, r32
    void Add32(Reg64 dst, Reg64 src) {
        uint8_t rex = 0x40 | ((src >= R8) ? 4 : 0) | ((dst >= R8) ? 1 : 0);
        if (rex != 0x40) Emit8(rex);
        Emit8(0x01);
        Emit8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }
    void Add32Imm(Reg64 dst, uint32_t imm) {
        if (dst >= R8) Emit8(0x41);
        Emit8(0x81);
        Emit8(static_cast<uint8_t>(0xC0 | (dst & 7)));
        Emit32(imm);
    }
    void And32(Reg64 dst, Reg64 src) {
        uint8_t rex = 0x40 | ((src >= R8) ? 4 : 0) | ((dst >= R8) ? 1 : 0);
        if (rex != 0x40) Emit8(rex);
        Emit8(0x21);
        Emit8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }
    void And32Imm(Reg64 dst, uint32_t imm) {
        if (dst >= R8) Emit8(0x41);
        Emit8(0x81);
        Emit8(static_cast<uint8_t>(0xE0 | (dst & 7)));
        Emit32(imm);
    }
    void Or32(Reg64 dst, Reg64 src) {
        uint8_t rex = 0x40 | ((src >= R8) ? 4 : 0) | ((dst >= R8) ? 1 : 0);
        if (rex != 0x40) Emit8(rex);
        Emit8(0x09);
        Emit8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }
    void Xor32(Reg64 dst, Reg64 src) {
        uint8_t rex = 0x40 | ((src >= R8) ? 4 : 0) | ((dst >= R8) ? 1 : 0);
        if (rex != 0x40) Emit8(rex);
        Emit8(0x31);
        Emit8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }
    void Not32(Reg64 dst) {
        if (dst >= R8) Emit8(0x41);
        Emit8(0xF7);
        Emit8(static_cast<uint8_t>(0xD0 | (dst & 7)));
    }
    void Imul32(Reg64 dst, Reg64 src) {
        uint8_t rex = 0x40 | ((dst >= R8) ? 4 : 0) | ((src >= R8) ? 1 : 0);
        if (rex != 0x40) Emit8(rex);
        Emit8(0x0F); Emit8(0xAF);
        Emit8(static_cast<uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }

    // cmp eax, imm32
    void CmpEaxImm32(uint32_t imm) { Emit8(0x3D); Emit32(imm); }

    // cmp rax, [rbx + disp]
    void CmpRaxRbxMem64(uint32_t disp) {
        Emit8(0x48); Emit8(0x3B);
        if (disp < 128) { Emit8(0x43); Emit8(static_cast<uint8_t>(disp)); }
        else            { Emit8(0x83); Emit32(disp); }
    }

    // test eax, eax
    void TestEaxEax() { Emit8(0x85); Emit8(0xC0); }

    // cmp byte [rbx + disp], 0
    void CmpByteRbxZero(uint32_t disp) {
        Emit8(0x80);
        if (disp < 128) { Emit8(0x7B); Emit8(static_cast<uint8_t>(disp)); }
        else            { Emit8(0xBB); Emit32(disp); }
        Emit8(0x00);
    }

    // test dword [rbx + disp], imm32
    void TestDwordRbxImm(uint32_t disp, uint32_t imm) {
        Emit8(0xF7);
        if (disp < 128) { Emit8(0x43); Emit8(static_cast<uint8_t>(disp)); }
        else            { Emit8(0x83); Emit32(disp); }
        Emit32(imm);
    }

    // Jumps returning the offset of rel32 to patch later
    size_t JmpRel32()  { Emit8(0xE9); size_t p = code.size(); Emit32(0); return p; }
    size_t JeRel32()   { Emit8(0x0F); Emit8(0x84); size_t p = code.size(); Emit32(0); return p; }
    size_t JneRel32()  { Emit8(0x0F); Emit8(0x85); size_t p = code.size(); Emit32(0); return p; }
    size_t JaeRel32()  { Emit8(0x0F); Emit8(0x83); size_t p = code.size(); Emit32(0); return p; }

    // call qword [r12 + disp] (R12 holds RuntimeHelpers*; SIB byte 0x24 required for r12!)
    void CallHelper(uint32_t offsetInHelpers) {
        Emit8(0x41); // REX.B = 1 (r12)
        Emit8(0xFF);
        if (offsetInHelpers < 128) {
            Emit8(0x54); // Mod=01, Reg=/2 (CALL), RM=100 (SIB)
            Emit8(0x24); // SIB: scale=0, index=none(4), base=r12(4)
            Emit8(static_cast<uint8_t>(offsetInHelpers));
        } else {
            Emit8(0x94); // Mod=10, Reg=/2, RM=100 (SIB)
            Emit8(0x24);
            Emit32(offsetInHelpers);
        }
    }

    // ret
    void Ret() { Emit8(0xC3); }
};

} // namespace recomp
