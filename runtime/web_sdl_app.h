#pragma once

#include "config.h"
#include "gba_bus.h"
#include "gba_cpu.h"
#include "imgui_menu.h"

#include <string>

struct SDL_Window;
struct _SDL_GameController;

namespace gba {

// Cross-platform SDL2 + OpenGL + Dear ImGui runtime shell (used on Linux x86_64 and WebAssembly)
class WebSdlApp {
public:
    WebSdlApp();
    ~WebSdlApp();

    int Run(int argc, char** argv);
    void MainLoopTick();

private:
    void PollGamepadState();
    uint16_t PollGbaInput() const;
    void UpdateFramebufferTexture(const uint32_t* rgbaPixels);
    void ToggleFullscreen();
    void ShowToast(const std::string& msg);

    AppConfig config_;
    GbaBus bus_;
    GbaCpu cpu_;
    ImGuiMenu menu_;
    SaveState quickSave_;
    RuntimeStats stats_;

    std::string configPath_;
    std::string sramPath_;

    SDL_Window* window_ = nullptr;
    void* glContext_ = nullptr;
    _SDL_GameController* controller_ = nullptr;
    unsigned int fbTexture_ = 0;

    bool running_ = true;
    bool isFullscreen_ = false;
    int sramSaveCooldown_ = 0;

    uint32_t padButtonsMask_ = 0;
    uint32_t prevPadButtonsMask_ = 0;
    double lastTimeSec_ = 0.0;
    double fpsTimerSec_ = 0.0;
    int fpsFrames_ = 0;
};

} // namespace gba
