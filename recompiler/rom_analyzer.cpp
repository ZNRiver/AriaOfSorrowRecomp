#include "rom_analyzer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <queue>
#include <unordered_set>

namespace recomp {

namespace {
inline int32_t SignExt(uint32_t v, int bits) {
    uint32_t s = 32u - static_cast<uint32_t>(bits);
    return static_cast<int32_t>(v << s) >> s;
}

std::string CleanAscii(const uint8_t* ptr, size_t len) {
    std::string s;
    for (size_t i = 0; i < len; ++i) {
        char c = static_cast<char>(ptr[i]);
        if (c == '\0') break;
        if (c >= 32 && c < 127) s.push_back(c);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
} // namespace

bool RomAnalyzer::LoadRom(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize size = in.tellg();
    if (size < 0xC0) return false;
    in.seekg(0, std::ios::beg);

    rom_.resize(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(rom_.data()), size)) return false;

    header_.filename = std::filesystem::path(path).filename().string();
    header_.romSize = static_cast<uint32_t>(rom_.size());
    header_.title = CleanAscii(rom_.data() + 0xA0, 12);
    header_.gameCode = CleanAscii(rom_.data() + 0xAC, 4);
    header_.makerCode = CleanAscii(rom_.data() + 0xB0, 2);
    header_.validLogo = (rom_[0xB2] == 0x96);
    header_.entryPoint = 0x08000000u;
    return true;
}

bool RomAnalyzer::IsValidRomAddr(uint32_t gbaAddr) const {
    if (gbaAddr < 0x08000000u || gbaAddr >= 0x0A000000u) return false;
    uint32_t off = gbaAddr - 0x08000000u;
    return off + 4 <= rom_.size();
}

uint16_t RomAnalyzer::Read16(uint32_t gbaAddr) const {
    uint32_t off = (gbaAddr & 0x01FFFFFFu) & ~1u;
    if (off + 2 > rom_.size()) return 0;
    return static_cast<uint16_t>(rom_[off] | (rom_[off + 1] << 8));
}

uint32_t RomAnalyzer::Read32(uint32_t gbaAddr) const {
    uint32_t off = (gbaAddr & 0x01FFFFFFu) & ~3u;
    if (off + 4 > rom_.size()) return 0;
    uint32_t v;
    std::memcpy(&v, rom_.data() + off, 4);
    return v;
}

std::vector<DiscoveredFunction> RomAnalyzer::DiscoverFunctions(uint32_t maxFunctions) const {
    std::vector<DiscoveredFunction> result;
    if (rom_.empty()) return result;

    // Queue of function entry points: (addrWithoutModeBit, isThumb)
    std::queue<std::pair<uint32_t, bool>> funcQueue;
    std::unordered_set<uint32_t> visitedEntries; // key = addr | (isThumb ? 1 : 0)

    auto enqueueFunc = [&](uint32_t addr, bool isThumb) {
        addr = isThumb ? (addr & ~1u) : (addr & ~3u);
        if (!IsValidRomAddr(addr)) return;
        uint32_t key = addr | (isThumb ? 1u : 0u);
        if (visitedEntries.insert(key).second) {
            funcQueue.push({ addr, isThumb });
        }
    };

    // 1. Always start from ROM entry point 0x08000000 (ARM)
    enqueueFunc(0x08000000u, false);

    // 2. Scan first 1.5 MB of ROM for Thumb PUSH {..., lr} prologues as secondary seeds
    //    (we process reachable call graph first, then fill up to maxFunctions)
    std::vector<uint32_t> prologueSeeds;
    uint32_t scanLimit = std::min<uint32_t>(static_cast<uint32_t>(rom_.size()), 0x180000u);
    for (uint32_t off = 0xC0; off + 4 <= scanLimit; off += 2) {
        uint16_t ins = static_cast<uint16_t>(rom_[off] | (rom_[off + 1] << 8));
        if ((ins & 0xFF00u) == 0xB500u) { // PUSH {..., lr}
            prologueSeeds.push_back(0x08000000u + off);
        }
    }

    auto analyzeOneFunc = [&](uint32_t entryAddr, bool isThumb) -> DiscoveredFunction {
        DiscoveredFunction fn;
        fn.entryAddr = entryAddr;
        fn.isThumb = isThumb;

        std::queue<uint32_t> bbQueue;
        std::unordered_set<uint32_t> visitedInsns;
        bbQueue.push(entryAddr);

        constexpr size_t kMaxInsnsPerFunc = 320;
        constexpr uint32_t kMaxSpanBytes = 0x1000u;

        while (!bbQueue.empty() && visitedInsns.size() < kMaxInsnsPerFunc) {
            uint32_t pc = bbQueue.front();
            bbQueue.pop();

            while (visitedInsns.size() < kMaxInsnsPerFunc) {
                if (!IsValidRomAddr(pc)) break;
                if (pc < entryAddr || pc > entryAddr + kMaxSpanBytes) break;
                if (!visitedInsns.insert(pc).second) break;

                if (!isThumb) {
                    uint32_t ins = Read32(pc);
                    uint32_t cond = ins >> 28;
                    uint32_t pcVal = pc + 8u;

                    // ARM B / BL
                    if ((ins & 0x0E000000u) == 0x0A000000u) {
                        int32_t off = SignExt(ins & 0x00FFFFFFu, 24) << 2;
                        uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~3u;
                        bool isLink = (ins & (1u << 24)) != 0;
                        if (isLink) {
                            enqueueFunc(target, false);
                            pc += 4u;
                            continue;
                        } else {
                            if (target >= entryAddr && target <= entryAddr + kMaxSpanBytes) {
                                bbQueue.push(target);
                            } else {
                                enqueueFunc(target, false);
                            }
                            if (cond == 0xE) break; // Unconditional branch ends block
                            pc += 4u;
                            continue;
                        }
                    }

                    // ARM LDR Rd, [PC, #imm] followed by BX Rd (with optional MOV lr, pc in between)
                    if ((ins & 0x0F7F0000u) == 0x051F0000u) {
                        bool add = (ins & (1u << 23)) != 0;
                        uint32_t imm = ins & 0xFFFu;
                        uint32_t poolAddr = add ? (pcVal + imm) : (pcVal - imm);
                        if (IsValidRomAddr(poolAddr)) {
                            uint32_t loadedVal = Read32(poolAddr);
                            uint32_t rd = (ins >> 12) & 0xFu;
                            for (uint32_t look = 4u; look <= 12u; look += 4u) {
                                uint32_t nextIns = Read32(pc + look);
                                if ((nextIns & 0x0FFFFFF0u) == 0x012FFF10u) {
                                    uint32_t rm = nextIns & 0xFu;
                                    if (rd == rm && IsValidRomAddr(loadedVal & ~1u)) {
                                        enqueueFunc(loadedVal & ~1u, (loadedVal & 1u) != 0);
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    // ARM BX
                    if ((ins & 0x0FFFFFF0u) == 0x012FFF10u) {
                        if (cond == 0xE) break;
                        pc += 4u;
                        continue;
                    }

                    // ARM LDM ..., {..., pc}
                    if ((ins & 0x0E108000u) == 0x08108000u) {
                        if (cond == 0xE) break;
                        pc += 4u;
                        continue;
                    }

                    // ARM MOV/LDR PC, ... (exclude MSR/CMP/CMN/TST/TEQ which have Rd=0xF or don't write PC)
                    bool isMsr = (ins & 0x0DB0F000u) == 0x0120F000u;
                    if (!isMsr && ((ins & 0x0C00F000u) == 0x0000F000u || (ins & 0x0C10F000u) == 0x0410F000u) && cond == 0xE) {
                        break;
                    }

                    pc += 4u;
                } else {
                    uint16_t ins = Read16(pc);
                    uint32_t pcVal = pc + 4u;

                    // Thumb BL (32-bit pair: prefix F000..F7FF + suffix F800..FFFF)
                    if ((ins & 0xF800u) == 0xF000u) {
                        uint16_t ins2 = Read16(pc + 2u);
                        if ((ins2 & 0xF800u) == 0xF800u) {
                            visitedInsns.insert(pc + 2u);
                            int32_t offHi = SignExt(ins & 0x7FFu, 11) << 12;
                            uint32_t target = (pcVal + static_cast<uint32_t>(offHi) + ((ins2 & 0x7FFu) << 1)) & ~1u;
                            enqueueFunc(target, true);
                            pc += 4u;
                            continue;
                        }
                    }

                    // Thumb unconditional branch B
                    if ((ins & 0xF800u) == 0xE000u) {
                        int32_t off = SignExt(ins & 0x7FFu, 11) << 1;
                        uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~1u;
                        if (target >= entryAddr && target <= entryAddr + kMaxSpanBytes) {
                            bbQueue.push(target);
                        } else {
                            enqueueFunc(target, true);
                        }
                        break;
                    }

                    // Thumb conditional branch Bcond (exclude SWI 0xDFxx and undefined 0xDExx)
                    if ((ins & 0xF000u) == 0xD000u && (ins & 0x0F00u) < 0x0E00u) {
                        int32_t off = SignExt(ins & 0xFFu, 8) << 1;
                        uint32_t target = (pcVal + static_cast<uint32_t>(off)) & ~1u;
                        if (target >= entryAddr && target <= entryAddr + kMaxSpanBytes) {
                            bbQueue.push(target);
                        } else {
                            enqueueFunc(target, true);
                        }
                        pc += 2u;
                        continue;
                    }

                    // Thumb POP {..., pc}
                    if ((ins & 0xFF00u) == 0xBD00u) {
                        break;
                    }

                    // Thumb BX Rm
                    if ((ins & 0xFF87u) == 0x4700u) {
                        break;
                    }

                    // Thumb MOV pc, Rm
                    if ((ins & 0xFF87u) == 0x4687u) {
                        break;
                    }

                    // Thumb LDR Rd, [PC, #imm] — check if literal pool holds a ROM code pointer (followed by BX or MOV pc, Rd)
                    if ((ins & 0xF800u) == 0x4800u) {
                        uint32_t poolAddr = (pcVal & ~3u) + ((ins & 0xFFu) << 2);
                        if (IsValidRomAddr(poolAddr)) {
                            uint32_t val = Read32(poolAddr);
                            uint16_t nextIns = Read16(pc + 2u);
                            bool isBx = (nextIns & 0xFF87u) == 0x4700u;
                            bool isMovPc = (nextIns & 0xFF87u) == 0x4687u;
                            if ((isBx || isMovPc) && IsValidRomAddr(val & ~1u)) {
                                enqueueFunc(val & ~1u, isMovPc ? true : ((val & 1u) != 0));
                            }
                        }
                    }

                    pc += 2u;
                }
            }
        }

        fn.insnAddrs.assign(visitedInsns.begin(), visitedInsns.end());
        std::sort(fn.insnAddrs.begin(), fn.insnAddrs.end());
        return fn;
    };

    // Process reachable call graph first
    while (!funcQueue.empty() && result.size() < maxFunctions) {
        auto [addr, isThumb] = funcQueue.front();
        funcQueue.pop();
        DiscoveredFunction fn = analyzeOneFunc(addr, isThumb);
        if (!fn.insnAddrs.empty()) {
            result.push_back(std::move(fn));
        }
    }

    // Fill remaining budget with Thumb PUSH {..., lr} functions (for jump-table / callback functions)
    for (uint32_t seed : prologueSeeds) {
        if (result.size() >= maxFunctions) break;
        enqueueFunc(seed, true);
        while (!funcQueue.empty() && result.size() < maxFunctions) {
            auto [addr, isThumb] = funcQueue.front();
            funcQueue.pop();
            DiscoveredFunction fn = analyzeOneFunc(addr, isThumb);
            if (!fn.insnAddrs.empty()) {
                result.push_back(std::move(fn));
            }
        }
    }

    // Sort functions by entry address for deterministic output & binary search
    std::sort(result.begin(), result.end(), [](const DiscoveredFunction& a, const DiscoveredFunction& b) {
        uint32_t ka = a.entryAddr | (a.isThumb ? 1u : 0u);
        uint32_t kb = b.entryAddr | (b.isThumb ? 1u : 0u);
        return ka < kb;
    });

    return result;
}

} // namespace recomp
