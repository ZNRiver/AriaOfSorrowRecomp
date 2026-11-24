#pragma once

#include "config.h"
#include "gba_types.h"

#include <string>

namespace gba {

struct MenuActions {
    bool requestSaveConfig = false;
    bool requestResetGame = false;
    bool requestQuickSave = false;
    bool requestQuickLoad = false;
    bool requestToggleFullscreen = false;
    bool requestCloseMenu = false;
    bool requestExitApp = false;
};

struct RuntimeStats {
    float fps = 60.0f;
    uint64_t recompCalls = 0;
    uint64_t recompInstrs = 0;
    uint64_t fallbackInstrs = 0;
    bool hasQuickSave = false;
    bool gamepadConnected = false;
    int activePadButton = 0; // First currently pressed PadButton (for live rebinding)
    std::string statusToast;
    float toastTimer = 0.0f;
};

class ImGuiMenu {
public:
    bool isOpen = false;
    int waitingForKeyIdx = -1; // >= 0 when capturing keyboard bind
    int waitingForPadIdx = -1; // >= 0 when capturing gamepad bind

    bool IsCapturingBind() const { return waitingForKeyIdx >= 0 || waitingForPadIdx >= 0; }
    void Toggle();
    bool HandleKeyForRebind(int vk);
    MenuActions Render(AppConfig& cfg, const RecompRomInfo& romInfo, const RuntimeStats& stats);
    void RenderOverlayStats(const AppConfig& cfg, const RecompRomInfo& romInfo, const RuntimeStats& stats);
};

} // namespace gba
