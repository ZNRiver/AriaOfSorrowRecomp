#pragma once

#include "gba_types.h"
#include <array>

namespace gba {

class GbaBus;

class GbaPpu {
public:
    void Reset();
    void RenderScanline(GbaBus& bus, int line);
    void ReloadAffineRefs(GbaBus& bus);
    void AdvanceAffineRefs(GbaBus& bus);

    // Output framebuffer in 0xAABBGGRR (R8G8B8A8_UNORM for DX11)
    const uint32_t* GetFramebuffer() const { return framebuffer_.data(); }

    // Internal affine reference point registers (BG2 & BG3)
    int32_t bg2RefX = 0, bg2RefY = 0;
    int32_t bg3RefX = 0, bg3RefY = 0;

private:
    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> framebuffer_{};

    static inline uint32_t Rgb555ToRgba8(uint16_t c) {
        uint32_t r = (c & 0x1Fu);
        uint32_t g = ((c >> 5) & 0x1Fu);
        uint32_t b = ((c >> 10) & 0x1Fu);
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        return 0xFF000000u | (b << 16) | (g << 8) | r;
    }

    struct LayerPixel {
        uint16_t color = 0;
        uint8_t priority = 4;
        uint8_t layer = 5; // 0-3 = BG0-3, 4 = OBJ, 5 = Backdrop
        bool semiTransparent = false;
    };

    void RenderTextBg(GbaBus& bus, int bg, int line, uint16_t* lineColors, bool* lineOpaque);
    void RenderAffineBg(GbaBus& bus, int bg, int line, uint16_t* lineColors, bool* lineOpaque);
    void RenderBitmapMode3(GbaBus& bus, int line, uint16_t* lineColors, bool* lineOpaque);
    void RenderBitmapMode4(GbaBus& bus, int line, uint16_t* lineColors, bool* lineOpaque);
    void RenderBitmapMode5(GbaBus& bus, int line, uint16_t* lineColors, bool* lineOpaque);
    void RenderSprites(GbaBus& bus, int line, uint16_t* objColors, uint8_t* objPrio,
                       bool* objOpaque, bool* objSemi, bool* objWin);
};

} // namespace gba
