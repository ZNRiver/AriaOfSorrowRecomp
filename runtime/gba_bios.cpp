#include "gba_bios.h"
#include "gba_cpu.h"
#include "gba_bus.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace gba {

static constexpr double kPi = 3.14159265358979323846;

void HandleBiosSwi(GbaCpu& cpu, GbaBus& bus, uint8_t swiNum) {
    switch (swiNum) {
        case 0x00: { // SoftReset
            bus.iwram.fill(0); // Clear top of IWRAM
            cpu.Reset();
            uint8_t flag = bus.Read8(0x03007FFA);
            cpu.r[15] = flag ? 0x02000000u : 0x08000000u;
            break;
        }
        case 0x01: { // RegisterRamReset
            uint32_t flags = cpu.r[0];
            if (flags & 0x01) bus.ewram.fill(0);
            if (flags & 0x02) std::memset(bus.iwram.data(), 0, 0x7E00);
            if (flags & 0x04) bus.pram.fill(0);
            if (flags & 0x08) bus.vram.fill(0);
            if (flags & 0x10) bus.oam.fill(0);
            if (flags & 0x80) {
                bus.Write16(0x04000000, 0x0080);
            }
            break;
        }
        case 0x02: { // Halt
            cpu.halted = true;
            break;
        }
        case 0x04: { // IntrWait(discardOld, irqFlags)
            bool discard = (cpu.r[0] != 0);
            uint16_t mask = static_cast<uint16_t>(cpu.r[1]);
            uint16_t biosIf = bus.Read16(0x03007FF8);
            if (discard) {
                biosIf &= static_cast<uint16_t>(~mask);
                bus.Write16(0x03007FF8, biosIf);
            }
            bus.ime = 1;
            if ((biosIf & mask) == 0) {
                cpu.intrWaitActive = true;
                cpu.intrWaitMask = mask;
                cpu.halted = true;
            } else {
                biosIf &= static_cast<uint16_t>(~mask);
                bus.Write16(0x03007FF8, biosIf);
            }
            break;
        }
        case 0x05: { // VBlankIntrWait
            uint16_t biosIf = bus.Read16(0x03007FF8);
            biosIf &= static_cast<uint16_t>(~IRQ_VBLANK);
            bus.Write16(0x03007FF8, biosIf);
            bus.ime = 1;
            cpu.intrWaitActive = true;
            cpu.intrWaitMask = IRQ_VBLANK;
            cpu.halted = true;
            break;
        }
        case 0x06: { // Div(num, denom)
            int32_t num = static_cast<int32_t>(cpu.r[0]);
            int32_t den = static_cast<int32_t>(cpu.r[1]);
            if (den == 0) {
                cpu.r[0] = (num < 0) ? 0xFFFFFFFFu : 1u;
                cpu.r[1] = static_cast<uint32_t>(num);
                cpu.r[3] = (num < 0) ? 1u : 1u;
            } else if (num == INT32_MIN && den == -1) {
                cpu.r[0] = static_cast<uint32_t>(INT32_MIN);
                cpu.r[1] = 0;
                cpu.r[3] = static_cast<uint32_t>(INT32_MIN);
            } else {
                int32_t q = num / den;
                int32_t rem = num % den;
                cpu.r[0] = static_cast<uint32_t>(q);
                cpu.r[1] = static_cast<uint32_t>(rem);
                cpu.r[3] = static_cast<uint32_t>(q < 0 ? -q : q);
            }
            break;
        }
        case 0x07: { // DivArm(denom, num)
            int32_t den = static_cast<int32_t>(cpu.r[0]);
            int32_t num = static_cast<int32_t>(cpu.r[1]);
            if (den == 0) {
                cpu.r[0] = (num < 0) ? 0xFFFFFFFFu : 1u;
                cpu.r[1] = static_cast<uint32_t>(num);
                cpu.r[3] = 1u;
            } else if (num == INT32_MIN && den == -1) {
                cpu.r[0] = static_cast<uint32_t>(INT32_MIN);
                cpu.r[1] = 0;
                cpu.r[3] = static_cast<uint32_t>(INT32_MIN);
            } else {
                int32_t q = num / den;
                int32_t rem = num % den;
                cpu.r[0] = static_cast<uint32_t>(q);
                cpu.r[1] = static_cast<uint32_t>(rem);
                cpu.r[3] = static_cast<uint32_t>(q < 0 ? -q : q);
            }
            break;
        }
        case 0x08: { // Sqrt
            cpu.r[0] = static_cast<uint32_t>(std::sqrt(static_cast<double>(cpu.r[0])));
            break;
        }
        case 0x09: { // ArcTan
            double x = static_cast<int16_t>(cpu.r[0]) / 16384.0;
            double ang = std::atan(x) * (32768.0 / kPi);
            cpu.r[0] = static_cast<uint32_t>(static_cast<int16_t>(std::round(ang)));
            break;
        }
        case 0x0A: { // ArcTan2(x, y)
            int16_t x = static_cast<int16_t>(cpu.r[0]);
            int16_t y = static_cast<int16_t>(cpu.r[1]);
            if (x == 0 && y == 0) {
                cpu.r[0] = 0;
            } else {
                double ang = std::atan2(static_cast<double>(y), static_cast<double>(x));
                if (ang < 0.0) ang += 2.0 * kPi;
                cpu.r[0] = static_cast<uint32_t>(std::round(ang * (65536.0 / (2.0 * kPi)))) & 0xFFFFu;
            }
            break;
        }
        case 0x0B: { // CpuSet
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t cnt = cpu.r[2];
            uint32_t count = cnt & 0x1FFFFFu;
            bool fixed = (cnt & (1u << 24)) != 0;
            bool is32  = (cnt & (1u << 26)) != 0;

            if (is32) {
                src &= ~3u; dst &= ~3u;
                uint32_t val = bus.Read32(src);
                for (uint32_t i = 0; i < count; ++i) {
                    if (!fixed) { val = bus.Read32(src); src += 4; }
                    bus.Write32(dst, val);
                    dst += 4;
                }
            } else {
                src &= ~1u; dst &= ~1u;
                uint16_t val = bus.Read16(src);
                for (uint32_t i = 0; i < count; ++i) {
                    if (!fixed) { val = bus.Read16(src); src += 2; }
                    bus.Write16(dst, val);
                    dst += 2;
                }
            }
            break;
        }
        case 0x0C: { // CpuFastSet
            uint32_t src = cpu.r[0] & ~3u;
            uint32_t dst = cpu.r[1] & ~3u;
            uint32_t cnt = cpu.r[2];
            uint32_t count = (cnt & 0x1FFFFFu);
            count = (count + 7u) & ~7u; // Rounded up to multiple of 8 words
            bool fixed = (cnt & (1u << 24)) != 0;

            uint32_t val = bus.Read32(src);
            for (uint32_t i = 0; i < count; ++i) {
                if (!fixed) { val = bus.Read32(src); src += 4; }
                bus.Write32(dst, val);
                dst += 4;
            }
            break;
        }
        case 0x0E: { // BgAffineSet
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t count = cpu.r[2];
            for (uint32_t i = 0; i < count; ++i) {
                int32_t origX = static_cast<int32_t>(bus.Read32(src + 0));
                int32_t origY = static_cast<int32_t>(bus.Read32(src + 4));
                int16_t dispX = static_cast<int16_t>(bus.Read16(src + 8));
                int16_t dispY = static_cast<int16_t>(bus.Read16(src + 10));
                int16_t scaleX = static_cast<int16_t>(bus.Read16(src + 12));
                int16_t scaleY = static_cast<int16_t>(bus.Read16(src + 14));
                uint16_t angle = bus.Read16(src + 16);
                src += 20;

                double rad = (angle >> 8) * (2.0 * kPi / 256.0);
                double c = std::cos(rad);
                double s = std::sin(rad);
                int16_t pa = static_cast<int16_t>(std::round(c * scaleX));
                int16_t pb = static_cast<int16_t>(std::round(-s * scaleX));
                int16_t pc = static_cast<int16_t>(std::round(s * scaleY));
                int16_t pd = static_cast<int16_t>(std::round(c * scaleY));
                int32_t startX = origX - (pa * dispX + pb * dispY);
                int32_t startY = origY - (pc * dispX + pd * dispY);

                bus.Write16(dst + 0, static_cast<uint16_t>(pa));
                bus.Write16(dst + 2, static_cast<uint16_t>(pb));
                bus.Write16(dst + 4, static_cast<uint16_t>(pc));
                bus.Write16(dst + 6, static_cast<uint16_t>(pd));
                bus.Write32(dst + 8, static_cast<uint32_t>(startX));
                bus.Write32(dst + 12, static_cast<uint32_t>(startY));
                dst += 16;
            }
            break;
        }
        case 0x0F: { // ObjAffineSet
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t count = cpu.r[2];
            uint32_t offset = cpu.r[3];
            for (uint32_t i = 0; i < count; ++i) {
                int16_t scaleX = static_cast<int16_t>(bus.Read16(src + 0));
                int16_t scaleY = static_cast<int16_t>(bus.Read16(src + 2));
                uint16_t angle = bus.Read16(src + 4);
                src += 8;

                double rad = (angle >> 8) * (2.0 * kPi / 256.0);
                double c = std::cos(rad);
                double s = std::sin(rad);
                int16_t pa = static_cast<int16_t>(std::round(c * scaleX));
                int16_t pb = static_cast<int16_t>(std::round(-s * scaleX));
                int16_t pc = static_cast<int16_t>(std::round(s * scaleY));
                int16_t pd = static_cast<int16_t>(std::round(c * scaleY));

                bus.Write16(dst, static_cast<uint16_t>(pa)); dst += offset;
                bus.Write16(dst, static_cast<uint16_t>(pb)); dst += offset;
                bus.Write16(dst, static_cast<uint16_t>(pc)); dst += offset;
                bus.Write16(dst, static_cast<uint16_t>(pd)); dst += offset;
            }
            break;
        }
        case 0x10: { // BitUnPack
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t info = cpu.r[2];
            uint16_t srcLen = bus.Read16(info + 0);
            uint8_t srcWidth = bus.Read8(info + 2);
            uint8_t dstWidth = bus.Read8(info + 3);
            uint32_t dataOffset = bus.Read32(info + 4);
            bool zeroDataFlag = (dataOffset & 0x80000000u) != 0;
            uint32_t offsetVal = dataOffset & 0x7FFFFFFFu;

            if (srcWidth == 0 || dstWidth == 0) break;
            uint32_t srcMask = (1u << srcWidth) - 1u;
            uint32_t outWord = 0;
            int outBits = 0;

            for (uint16_t i = 0; i < srcLen; ++i) {
                uint8_t b = bus.Read8(src++);
                for (int bit = 0; bit < 8; bit += srcWidth) {
                    uint32_t v = (b >> bit) & srcMask;
                    if (v != 0 || zeroDataFlag) v += offsetVal;
                    outWord |= (v << outBits);
                    outBits += dstWidth;
                    if (outBits >= 32) {
                        bus.Write32(dst, outWord);
                        dst += 4;
                        outWord = 0;
                        outBits = 0;
                    }
                }
            }
            break;
        }
        case 0x11: // LZ77UnCompWram (8-bit writes)
        case 0x12: { // LZ77UnCompVram (16-bit writes)
            bool isVram = (swiNum == 0x12);
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t header = bus.Read32(src);
            src += 4;
            uint32_t remaining = header >> 8;

            uint16_t halfBuf = 0;
            bool hasLow = false;
            auto emitByte = [&](uint8_t b) {
                if (!isVram) {
                    bus.Write8(dst++, b);
                } else {
                    if (!hasLow) {
                        halfBuf = b;
                        hasLow = true;
                    } else {
                        halfBuf |= static_cast<uint16_t>(b) << 8;
                        bus.Write16(dst, halfBuf);
                        dst += 2;
                        hasLow = false;
                    }
                }
            };

            std::vector<uint8_t> window;
            window.reserve(remaining);

            while (remaining > 0) {
                uint8_t flags = bus.Read8(src++);
                for (int bit = 7; bit >= 0 && remaining > 0; --bit) {
                    if ((flags & (1u << bit)) == 0) {
                        uint8_t b = bus.Read8(src++);
                        window.push_back(b);
                        emitByte(b);
                        remaining--;
                    } else {
                        uint8_t b1 = bus.Read8(src++);
                        uint8_t b2 = bus.Read8(src++);
                        uint32_t len = ((b1 >> 4) & 0xFu) + 3u;
                        uint32_t disp = (((b1 & 0xFu) << 8) | b2) + 1u;
                        for (uint32_t k = 0; k < len && remaining > 0; ++k) {
                            uint8_t b = (disp <= window.size()) ? window[window.size() - disp] : 0;
                            window.push_back(b);
                            emitByte(b);
                            remaining--;
                        }
                    }
                }
            }
            break;
        }
        case 0x13: { // HuffUnComp
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t header = bus.Read32(src);
            src += 4;
            uint32_t bitDepth = header & 0xFu;
            uint32_t remaining = header >> 8;
            uint32_t treeSize = (static_cast<uint32_t>(bus.Read8(src)) + 1u) * 2u;
            uint32_t treeBase = src + 1;
            uint32_t dataPtr = src + treeSize;

            uint32_t outWord = 0;
            int outBits = 0;
            uint32_t nodeOff = 0;
            uint32_t curWord = 0;
            int bitsLeft = 0;

            while (remaining > 0) {
                if (bitsLeft == 0) {
                    curWord = bus.Read32(dataPtr);
                    dataPtr += 4;
                    bitsLeft = 32;
                }
                uint32_t bit = (curWord >> 31) & 1u;
                curWord <<= 1;
                bitsLeft--;

                uint8_t node = bus.Read8(treeBase + nodeOff);
                uint32_t childOff = (nodeOff & ~1u) + ((node & 0x3Fu) + 1u) * 2u + bit;
                bool isLeaf = (bit == 0) ? ((node & 0x80u) != 0) : ((node & 0x40u) != 0);
                if (isLeaf) {
                    uint8_t val = bus.Read8(treeBase + childOff);
                    outWord |= static_cast<uint32_t>(val) << outBits;
                    outBits += static_cast<int>(bitDepth);
                    if (outBits >= 32) {
                        bus.Write32(dst, outWord);
                        dst += 4;
                        outWord = 0;
                        outBits = 0;
                        remaining = (remaining >= 4) ? (remaining - 4) : 0;
                    }
                    nodeOff = 0;
                } else {
                    nodeOff = childOff;
                }
            }
            break;
        }
        case 0x14: // RLUnCompWram
        case 0x15: { // RLUnCompVram
            bool isVram = (swiNum == 0x15);
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t header = bus.Read32(src);
            src += 4;
            uint32_t remaining = header >> 8;

            uint16_t halfBuf = 0;
            bool hasLow = false;
            auto emitByte = [&](uint8_t b) {
                if (!isVram) {
                    bus.Write8(dst++, b);
                } else {
                    if (!hasLow) { halfBuf = b; hasLow = true; }
                    else { halfBuf |= static_cast<uint16_t>(b) << 8; bus.Write16(dst, halfBuf); dst += 2; hasLow = false; }
                }
            };

            while (remaining > 0) {
                uint8_t flag = bus.Read8(src++);
                if (flag & 0x80u) {
                    uint32_t len = (flag & 0x7Fu) + 3u;
                    uint8_t b = bus.Read8(src++);
                    for (uint32_t i = 0; i < len && remaining > 0; ++i) {
                        emitByte(b);
                        remaining--;
                    }
                } else {
                    uint32_t len = (flag & 0x7Fu) + 1u;
                    for (uint32_t i = 0; i < len && remaining > 0; ++i) {
                        emitByte(bus.Read8(src++));
                        remaining--;
                    }
                }
            }
            break;
        }
        case 0x16: // Diff8bitUnFilterWram
        case 0x17: { // Diff8bitUnFilterVram
            bool isVram = (swiNum == 0x17);
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t header = bus.Read32(src);
            src += 4;
            uint32_t remaining = header >> 8;
            uint8_t prev = 0;
            uint16_t halfBuf = 0;
            bool hasLow = false;
            while (remaining > 0) {
                prev = static_cast<uint8_t>(prev + bus.Read8(src++));
                if (!isVram) {
                    bus.Write8(dst++, prev);
                } else {
                    if (!hasLow) { halfBuf = prev; hasLow = true; }
                    else { halfBuf |= static_cast<uint16_t>(prev) << 8; bus.Write16(dst, halfBuf); dst += 2; hasLow = false; }
                }
                remaining--;
            }
            break;
        }
        case 0x18: { // Diff16bitUnFilter
            uint32_t src = cpu.r[0];
            uint32_t dst = cpu.r[1];
            uint32_t header = bus.Read32(src);
            src += 4;
            uint32_t remaining = header >> 8;
            uint16_t prev = 0;
            while (remaining >= 2) {
                prev = static_cast<uint16_t>(prev + bus.Read16(src));
                src += 2;
                bus.Write16(dst, prev);
                dst += 2;
                remaining -= 2;
            }
            break;
        }
        case 0x1F: { // MidiKey2Freq(WaveData*, mk, fp)
            uint32_t wdAddr = cpu.r[0];
            uint8_t mk = static_cast<uint8_t>(cpu.r[1]);
            uint8_t fp = static_cast<uint8_t>(cpu.r[2]);
            uint32_t baseFreq = bus.Read32(wdAddr + 4);
            double semi = (180.0 - static_cast<double>(mk) - static_cast<double>(fp) / 256.0) / 12.0;
            double freq = static_cast<double>(baseFreq) / std::pow(2.0, semi);
            cpu.r[0] = static_cast<uint32_t>(freq);
            break;
        }
        default:
            break;
    }
}

} // namespace gba
