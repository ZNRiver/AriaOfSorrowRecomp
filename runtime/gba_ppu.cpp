#include "gba_ppu.h"
#include "gba_bus.h"

#include <algorithm>
#include <cstring>

namespace gba {

void GbaPpu::Reset() {
    framebuffer_.fill(0xFF000000u);
    bg2RefX = bg2RefY = 0;
    bg3RefX = bg3RefY = 0;
}

void GbaPpu::ReloadAffineRefs(GbaBus& bus) {
    bg2RefX = SignExtend(bus.ReadIo32Direct(0x028), 28);
    bg2RefY = SignExtend(bus.ReadIo32Direct(0x02C), 28);
    bg3RefX = SignExtend(bus.ReadIo32Direct(0x038), 28);
    bg3RefY = SignExtend(bus.ReadIo32Direct(0x03C), 28);
}

void GbaPpu::AdvanceAffineRefs(GbaBus& bus) {
    bg2RefX += static_cast<int16_t>(bus.ReadIo16Direct(0x022)); // BG2PB
    bg2RefY += static_cast<int16_t>(bus.ReadIo16Direct(0x026)); // BG2PD
    bg3RefX += static_cast<int16_t>(bus.ReadIo16Direct(0x032)); // BG3PB
    bg3RefY += static_cast<int16_t>(bus.ReadIo16Direct(0x036)); // BG3PD
}

static inline uint16_t ReadU16LE(const uint8_t* mem, uint32_t addr) {
    return static_cast<uint16_t>(mem[addr] | (mem[addr + 1] << 8));
}

void GbaPpu::RenderTextBg(GbaBus& bus, int bg, int line, uint16_t* lineColors, bool* lineOpaque) {
    uint16_t bgcnt = bus.ReadIo16Direct(static_cast<uint32_t>(0x008 + bg * 2));
    uint32_t charBase = ((bgcnt >> 2) & 0x3u) * 0x4000u;
    bool is8bpp = (bgcnt & (1u << 7)) != 0;
    uint32_t screenBase = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
    uint32_t sizeMode = (bgcnt >> 14) & 0x3u;

    uint32_t hofs = bus.ReadIo16Direct(static_cast<uint32_t>(0x010 + bg * 4)) & 0x1FFu;
    uint32_t vofs = bus.ReadIo16Direct(static_cast<uint32_t>(0x012 + bg * 4)) & 0x1FFu;

    uint32_t widthMask  = (sizeMode & 1u) ? 511u : 255u;
    uint32_t heightMask = (sizeMode & 2u) ? 511u : 255u;

    uint32_t y = (static_cast<uint32_t>(line) + vofs) & heightMask;
    uint32_t tileY = y >> 3;
    uint32_t fineY = y & 7u;

    const uint8_t* vram = bus.vram.data();
    const uint8_t* pram = bus.pram.data();

    for (int sx = 0; sx < SCREEN_WIDTH; ++sx) {
        uint32_t x = (static_cast<uint32_t>(sx) + hofs) & widthMask;
        uint32_t tileX = x >> 3;
        uint32_t fineX = x & 7u;

        uint32_t screenBlock = 0;
        if (tileX >= 32) { screenBlock += 1; tileX -= 32; }
        if (tileY >= 32) { screenBlock += (sizeMode == 3) ? 2 : 1; }
        uint32_t localTileY = tileY & 31u;

        uint32_t mapAddr = (screenBase + screenBlock * 0x800u + (localTileY * 32u + tileX) * 2u) & 0xFFFFu;
        uint16_t entry = ReadU16LE(vram, mapAddr);

        uint32_t tileNum = entry & 0x3FFu;
        bool hflip = (entry & (1u << 10)) != 0;
        bool vflip = (entry & (1u << 11)) != 0;
        uint32_t px = hflip ? (7u - fineX) : fineX;
        uint32_t py = vflip ? (7u - fineY) : fineY;

        uint8_t colorIdx = 0;
        if (is8bpp) {
            uint32_t addr = charBase + tileNum * 64u + py * 8u + px;
            if (addr < 0x10000u) colorIdx = vram[addr];
            if (colorIdx != 0) {
                lineColors[sx] = ReadU16LE(pram, colorIdx * 2u);
                lineOpaque[sx] = true;
            }
        } else {
            uint32_t addr = charBase + tileNum * 32u + py * 4u + (px >> 1);
            if (addr < 0x10000u) {
                uint8_t byteVal = vram[addr];
                colorIdx = (px & 1u) ? (byteVal >> 4) : (byteVal & 0xFu);
            }
            if (colorIdx != 0) {
                uint32_t palBank = (entry >> 12) & 0xFu;
                lineColors[sx] = ReadU16LE(pram, (palBank * 16u + colorIdx) * 2u);
                lineOpaque[sx] = true;
            }
        }
    }
}

void GbaPpu::RenderAffineBg(GbaBus& bus, int bg, int /*line*/, uint16_t* lineColors, bool* lineOpaque) {
    uint16_t bgcnt = bus.ReadIo16Direct(static_cast<uint32_t>(0x008 + bg * 2));
    uint32_t charBase = ((bgcnt >> 2) & 0x3u) * 0x4000u;
    uint32_t screenBase = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
    bool wrap = (bgcnt & (1u << 13)) != 0;
    uint32_t sizeMode = (bgcnt >> 14) & 0x3u;

    int32_t sizePx = 128 << sizeMode; // 128, 256, 512, 1024
    int32_t tilesPerRow = sizePx >> 3;

    int32_t pa = static_cast<int16_t>(bus.ReadIo16Direct(bg == 2 ? 0x020 : 0x030));
    int32_t pc = static_cast<int16_t>(bus.ReadIo16Direct(bg == 2 ? 0x024 : 0x034));
    int32_t rx = (bg == 2) ? bg2RefX : bg3RefX;
    int32_t ry = (bg == 2) ? bg2RefY : bg3RefY;

    const uint8_t* vram = bus.vram.data();
    const uint8_t* pram = bus.pram.data();

    for (int sx = 0; sx < SCREEN_WIDTH; ++sx) {
        int32_t tx = rx >> 8;
        int32_t ty = ry >> 8;
        rx += pa;
        ry += pc;

        if (wrap) {
            tx &= (sizePx - 1);
            ty &= (sizePx - 1);
        } else {
            if (tx < 0 || tx >= sizePx || ty < 0 || ty >= sizePx) continue;
        }

        uint32_t mapAddr = (screenBase + static_cast<uint32_t>((ty >> 3) * tilesPerRow + (tx >> 3))) & 0xFFFFu;
        uint32_t tileNum = vram[mapAddr];
        uint32_t addr = charBase + tileNum * 64u + static_cast<uint32_t>((ty & 7) * 8 + (tx & 7));
        if (addr < 0x10000u) {
            uint8_t colorIdx = vram[addr];
            if (colorIdx != 0) {
                lineColors[sx] = ReadU16LE(pram, colorIdx * 2u);
                lineOpaque[sx] = true;
            }
        }
    }
}

void GbaPpu::RenderBitmapMode3(GbaBus& bus, int line, uint16_t* lineColors, bool* lineOpaque) {
    const uint8_t* vram = bus.vram.data();
    uint32_t rowBase = static_cast<uint32_t>(line) * SCREEN_WIDTH * 2u;
    for (int sx = 0; sx < SCREEN_WIDTH; ++sx) {
        lineColors[sx] = ReadU16LE(vram, rowBase + static_cast<uint32_t>(sx) * 2u);
        lineOpaque[sx] = true;
    }
}

void GbaPpu::RenderBitmapMode4(GbaBus& bus, int line, uint16_t* lineColors, bool* lineOpaque) {
    uint16_t dispcnt = bus.ReadIo16Direct(0x000);
    uint32_t frameBase = (dispcnt & (1u << 4)) ? 0xA000u : 0x0000u;
    const uint8_t* vram = bus.vram.data();
    const uint8_t* pram = bus.pram.data();
    uint32_t rowBase = frameBase + static_cast<uint32_t>(line) * SCREEN_WIDTH;
    for (int sx = 0; sx < SCREEN_WIDTH; ++sx) {
        uint8_t idx = vram[rowBase + static_cast<uint32_t>(sx)];
        if (idx != 0) {
            lineColors[sx] = ReadU16LE(pram, idx * 2u);
            lineOpaque[sx] = true;
        }
    }
}

void GbaPpu::RenderBitmapMode5(GbaBus& bus, int line, uint16_t* lineColors, bool* lineOpaque) {
    if (line >= 128) return;
    uint16_t dispcnt = bus.ReadIo16Direct(0x000);
    uint32_t frameBase = (dispcnt & (1u << 4)) ? 0xA000u : 0x0000u;
    const uint8_t* vram = bus.vram.data();
    uint32_t rowBase = frameBase + static_cast<uint32_t>(line) * 160u * 2u;
    for (int sx = 0; sx < 160; ++sx) {
        lineColors[sx] = ReadU16LE(vram, rowBase + static_cast<uint32_t>(sx) * 2u);
        lineOpaque[sx] = true;
    }
}

void GbaPpu::RenderSprites(GbaBus& bus, int line, uint16_t* objColors, uint8_t* objPrio,
                           bool* objOpaque, bool* objSemi, bool* objWin) {
    static constexpr int kObjW[3][4] = {
        { 8, 16, 32, 64 }, // Square
        { 16, 32, 32, 64 }, // Horizontal
        { 8,  8, 16, 32 }, // Vertical
    };
    static constexpr int kObjH[3][4] = {
        { 8, 16, 32, 64 },
        { 8,  8, 16, 32 },
        { 16, 32, 32, 64 },
    };

    uint16_t dispcnt = bus.ReadIo16Direct(0x000);
    bool mapping1D = (dispcnt & (1u << 6)) != 0;
    uint32_t mode = dispcnt & 7u;

    const uint8_t* oam = bus.oam.data();
    const uint8_t* vram = bus.vram.data();
    const uint8_t* pram = bus.pram.data();

    for (int i = 0; i < 128; ++i) {
        uint16_t attr0 = ReadU16LE(oam, static_cast<uint32_t>(i * 8 + 0));
        uint16_t attr1 = ReadU16LE(oam, static_cast<uint32_t>(i * 8 + 2));
        uint16_t attr2 = ReadU16LE(oam, static_cast<uint32_t>(i * 8 + 4));

        uint32_t objMode = (attr0 >> 8) & 3u;
        if (objMode == 2) continue; // Disabled

        uint32_t shape = (attr0 >> 14) & 3u;
        if (shape == 3) continue;
        uint32_t size = (attr1 >> 14) & 3u;

        int sprW = kObjW[shape][size];
        int sprH = kObjH[shape][size];
        bool isAffine = (objMode == 1 || objMode == 3);
        bool doubleSize = (objMode == 3);

        int drawW = doubleSize ? sprW * 2 : sprW;
        int drawH = doubleSize ? sprH * 2 : sprH;

        int y = attr0 & 0xFF;
        if (y >= 160) y -= 256;
        if (line < y || line >= y + drawH) continue;

        int x = attr1 & 0x1FF;
        if (x >= 240) x -= 512;
        if (x + drawW <= 0 || x >= SCREEN_WIDTH) continue;

        uint32_t gfxMode = (attr0 >> 10) & 3u;
        bool isSemi = (gfxMode == 1);
        bool isObjWin = (gfxMode == 2);
        bool is8bpp = (attr0 & (1u << 13)) != 0;

        uint32_t baseTile = attr2 & 0x3FFu;
        if (mode >= 3 && baseTile < 512) continue; // Bitmap modes only allow tiles >= 512

        uint8_t prio = static_cast<uint8_t>((attr2 >> 10) & 3u);
        uint32_t palBank = (attr2 >> 12) & 0xFu;

        int16_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (isAffine) {
            uint32_t paramIdx = (attr1 >> 9) & 0x1Fu;
            pa = static_cast<int16_t>(ReadU16LE(oam, paramIdx * 32u + 6u));
            pb = static_cast<int16_t>(ReadU16LE(oam, paramIdx * 32u + 14u));
            pc = static_cast<int16_t>(ReadU16LE(oam, paramIdx * 32u + 22u));
            pd = static_cast<int16_t>(ReadU16LE(oam, paramIdx * 32u + 30u));
        }

        bool hflip = !isAffine && ((attr1 & (1u << 12)) != 0);
        bool vflip = !isAffine && ((attr1 & (1u << 13)) != 0);

        int localY = line - y;
        int halfDrawW = drawW >> 1;
        int halfDrawH = drawH >> 1;
        int halfSprW = sprW >> 1;
        int halfSprH = sprH >> 1;

        for (int lx = 0; lx < drawW; ++lx) {
            int sx = x + lx;
            if (sx < 0 || sx >= SCREEN_WIDTH) continue;

            int tx, ty;
            if (isAffine) {
                int dx = lx - halfDrawW;
                int dy = localY - halfDrawH;
                tx = ((pa * dx + pb * dy) >> 8) + halfSprW;
                ty = ((pc * dx + pd * dy) >> 8) + halfSprH;
                if (tx < 0 || tx >= sprW || ty < 0 || ty >= sprH) continue;
            } else {
                tx = hflip ? (sprW - 1 - lx) : lx;
                ty = vflip ? (sprH - 1 - localY) : localY;
            }

            uint32_t tileX = static_cast<uint32_t>(tx >> 3);
            uint32_t tileY = static_cast<uint32_t>(ty >> 3);
            uint32_t fineX = static_cast<uint32_t>(tx & 7);
            uint32_t fineY = static_cast<uint32_t>(ty & 7);

            uint32_t tileNum;
            if (mapping1D) {
                if (is8bpp) {
                    tileNum = (baseTile & ~1u) + (tileY * static_cast<uint32_t>(sprW >> 3) + tileX) * 2u;
                } else {
                    tileNum = baseTile + tileY * static_cast<uint32_t>(sprW >> 3) + tileX;
                }
            } else {
                if (is8bpp) {
                    tileNum = (baseTile & ~1u) + tileY * 32u + tileX * 2u;
                } else {
                    tileNum = baseTile + tileY * 32u + tileX;
                }
            }
            tileNum &= 0x3FFu;

            uint8_t colorIdx = 0;
            if (is8bpp) {
                uint32_t addr = 0x10000u + tileNum * 32u + fineY * 8u + fineX;
                if (addr < 0x18000u) colorIdx = vram[addr];
            } else {
                uint32_t addr = 0x10000u + tileNum * 32u + fineY * 4u + (fineX >> 1);
                if (addr < 0x18000u) {
                    uint8_t b = vram[addr];
                    colorIdx = (fineX & 1u) ? (b >> 4) : (b & 0xFu);
                }
            }

            if (colorIdx == 0) continue;

            if (isObjWin) {
                objWin[sx] = true;
            } else {
                if (!objOpaque[sx] || prio < objPrio[sx]) {
                    uint32_t palAddr = is8bpp ? (0x200u + colorIdx * 2u)
                                              : (0x200u + (palBank * 16u + colorIdx) * 2u);
                    objColors[sx] = ReadU16LE(pram, palAddr);
                    objPrio[sx] = prio;
                    objOpaque[sx] = true;
                    objSemi[sx] = isSemi;
                }
            }
        }
    }
}

void GbaPpu::RenderScanline(GbaBus& bus, int line) {
    if (line < 0 || line >= SCREEN_HEIGHT) return;

    uint32_t* dst = framebuffer_.data() + line * SCREEN_WIDTH;
    uint16_t dispcnt = bus.ReadIo16Direct(0x000);

    // Forced blank -> white screen
    if (dispcnt & (1u << 7)) {
        for (int x = 0; x < SCREEN_WIDTH; ++x) dst[x] = 0xFFFFFFFFu;
        return;
    }

    uint32_t mode = dispcnt & 7u;
    uint16_t backdrop = ReadU16LE(bus.pram.data(), 0);

    uint16_t bgColors[4][SCREEN_WIDTH];
    bool bgOpaque[4][SCREEN_WIDTH] = {};
    uint8_t bgPrio[4];
    for (int i = 0; i < 4; ++i) {
        bgPrio[i] = static_cast<uint8_t>(bus.ReadIo16Direct(static_cast<uint32_t>(0x008 + i * 2)) & 3u);
    }

    switch (mode) {
        case 0:
            for (int bg = 0; bg < 4; ++bg) {
                if (dispcnt & (1u << (8 + bg))) RenderTextBg(bus, bg, line, bgColors[bg], bgOpaque[bg]);
            }
            break;
        case 1:
            if (dispcnt & (1u << 8)) RenderTextBg(bus, 0, line, bgColors[0], bgOpaque[0]);
            if (dispcnt & (1u << 9)) RenderTextBg(bus, 1, line, bgColors[1], bgOpaque[1]);
            if (dispcnt & (1u << 10)) RenderAffineBg(bus, 2, line, bgColors[2], bgOpaque[2]);
            break;
        case 2:
            if (dispcnt & (1u << 10)) RenderAffineBg(bus, 2, line, bgColors[2], bgOpaque[2]);
            if (dispcnt & (1u << 11)) RenderAffineBg(bus, 3, line, bgColors[3], bgOpaque[3]);
            break;
        case 3:
            if (dispcnt & (1u << 10)) RenderBitmapMode3(bus, line, bgColors[2], bgOpaque[2]);
            break;
        case 4:
            if (dispcnt & (1u << 10)) RenderBitmapMode4(bus, line, bgColors[2], bgOpaque[2]);
            break;
        case 5:
            if (dispcnt & (1u << 10)) RenderBitmapMode5(bus, line, bgColors[2], bgOpaque[2]);
            break;
        default:
            break;
    }

    uint16_t objColors[SCREEN_WIDTH] = {};
    uint8_t objPrio[SCREEN_WIDTH];
    std::memset(objPrio, 4, sizeof(objPrio));
    bool objOpaque[SCREEN_WIDTH] = {};
    bool objSemi[SCREEN_WIDTH] = {};
    bool objWin[SCREEN_WIDTH] = {};

    if (dispcnt & (1u << 12)) {
        RenderSprites(bus, line, objColors, objPrio, objOpaque, objSemi, objWin);
    }

    // Window setup
    bool win0En = (dispcnt & (1u << 13)) != 0;
    bool win1En = (dispcnt & (1u << 14)) != 0;
    bool objWinEn = (dispcnt & (1u << 15)) != 0;
    bool anyWin = win0En || win1En || objWinEn;

    uint16_t win0h = bus.ReadIo16Direct(0x040);
    uint16_t win1h = bus.ReadIo16Direct(0x042);
    uint16_t win0v = bus.ReadIo16Direct(0x044);
    uint16_t win1v = bus.ReadIo16Direct(0x046);
    uint16_t winin = bus.ReadIo16Direct(0x048);
    uint16_t winout = bus.ReadIo16Direct(0x04A);

    auto inRange = [](int v, int start, int end) -> bool {
        if (start <= end) return v >= start && v < end;
        return v >= start || v < end;
    };

    bool win0Y = win0En && inRange(line, win0v >> 8, win0v & 0xFF);
    bool win1Y = win1En && inRange(line, win1v >> 8, win1v & 0xFF);
    int win0X1 = win0h >> 8, win0X2 = win0h & 0xFF;
    int win1X1 = win1h >> 8, win1X2 = win1h & 0xFF;

    // Color effects (BLDCNT / BLDALPHA / BLDY)
    uint16_t bldcnt = bus.ReadIo16Direct(0x050);
    uint16_t bldalpha = bus.ReadIo16Direct(0x052);
    uint16_t bldy = bus.ReadIo16Direct(0x054);

    uint32_t bldMode = (bldcnt >> 6) & 3u;
    int eva = std::min<int>(16, bldalpha & 0x1F);
    int evb = std::min<int>(16, (bldalpha >> 8) & 0x1F);
    int evy = std::min<int>(16, bldy & 0x1F);

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        uint8_t winMask = 0x3F; // Bits 0-3 BG0-3, 4 OBJ, 5 ColorFX
        if (anyWin) {
            if (win0Y && inRange(x, win0X1, win0X2)) {
                winMask = static_cast<uint8_t>(winin & 0x3F);
            } else if (win1Y && inRange(x, win1X1, win1X2)) {
                winMask = static_cast<uint8_t>((winin >> 8) & 0x3F);
            } else if (objWinEn && objWin[x]) {
                winMask = static_cast<uint8_t>((winout >> 8) & 0x3F);
            } else {
                winMask = static_cast<uint8_t>(winout & 0x3F);
            }
        }

        // Find top 2 visible pixels
        LayerPixel top{ backdrop, 4, 5, false };
        LayerPixel second{ backdrop, 4, 5, false };

        auto insertPixel = [&](uint16_t col, uint8_t prio, uint8_t layer, bool semi) {
            // Lower priority number wins; on tie, OBJ (4) beats BG (0..3), lower BG beats higher BG
            auto beats = [](uint8_t pA, uint8_t lA, uint8_t pB, uint8_t lB) {
                if (pA != pB) return pA < pB;
                if (lA == 4 && lB != 4) return true;
                if (lB == 4 && lA != 4) return false;
                return lA < lB;
            };
            if (beats(prio, layer, top.priority, top.layer)) {
                second = top;
                top = { col, prio, layer, semi };
            } else if (beats(prio, layer, second.priority, second.layer)) {
                second = { col, prio, layer, semi };
            }
        };

        if ((winMask & (1u << 4)) && objOpaque[x]) {
            insertPixel(objColors[x], objPrio[x], 4, objSemi[x]);
        }
        for (int bg = 0; bg < 4; ++bg) {
            if ((winMask & (1u << bg)) && bgOpaque[bg][x]) {
                insertPixel(bgColors[bg][x], bgPrio[bg], static_cast<uint8_t>(bg), false);
            }
        }

        uint16_t finalCol = top.color;
        bool fxAllowed = (winMask & (1u << 5)) != 0;

        bool topIsTarget1 = (bldcnt & (1u << top.layer)) != 0;
        bool secIsTarget2 = (bldcnt & (1u << (8 + second.layer))) != 0;

        if (top.semiTransparent && secIsTarget2) {
            int r1 = top.color & 31, g1 = (top.color >> 5) & 31, b1 = (top.color >> 10) & 31;
            int r2 = second.color & 31, g2 = (second.color >> 5) & 31, b2 = (second.color >> 10) & 31;
            int r = std::min(31, (r1 * eva + r2 * evb) >> 4);
            int g = std::min(31, (g1 * eva + g2 * evb) >> 4);
            int b = std::min(31, (b1 * eva + b2 * evb) >> 4);
            finalCol = static_cast<uint16_t>(r | (g << 5) | (b << 10));
        } else if (fxAllowed && topIsTarget1) {
            if (bldMode == 1 && secIsTarget2) {
                int r1 = top.color & 31, g1 = (top.color >> 5) & 31, b1 = (top.color >> 10) & 31;
                int r2 = second.color & 31, g2 = (second.color >> 5) & 31, b2 = (second.color >> 10) & 31;
                int r = std::min(31, (r1 * eva + r2 * evb) >> 4);
                int g = std::min(31, (g1 * eva + g2 * evb) >> 4);
                int b = std::min(31, (b1 * eva + b2 * evb) >> 4);
                finalCol = static_cast<uint16_t>(r | (g << 5) | (b << 10));
            } else if (bldMode == 2) {
                int r = top.color & 31, g = (top.color >> 5) & 31, b = (top.color >> 10) & 31;
                r += ((31 - r) * evy) >> 4;
                g += ((31 - g) * evy) >> 4;
                b += ((31 - b) * evy) >> 4;
                finalCol = static_cast<uint16_t>(r | (g << 5) | (b << 10));
            } else if (bldMode == 3) {
                int r = top.color & 31, g = (top.color >> 5) & 31, b = (top.color >> 10) & 31;
                r -= (r * evy) >> 4;
                g -= (g * evy) >> 4;
                b -= (b * evy) >> 4;
                finalCol = static_cast<uint16_t>(r | (g << 5) | (b << 10));
            }
        }

        dst[x] = Rgb555ToRgba8(finalCol);
    }
}

} // namespace gba
