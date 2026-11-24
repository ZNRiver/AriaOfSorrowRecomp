#pragma once

#include "config.h"
#include "gba_bus.h"
#include "gba_cpu.h"
#include "imgui_menu.h"

#include <string>

namespace gba {

class Win32Dx11App {
public:
    Win32Dx11App();
    ~Win32Dx11App();

    int Run(int argc, char** argv);

private:
    bool InitWindow(const std::string& title, int width, int height);
    bool InitD3D11();
    void CleanupD3D11();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void UpdateFramebufferTexture(const uint32_t* rgbaPixels);
    void ToggleFullscreen();
    uint16_t PollGbaInput() const;
    void PollGamepadState();
    void ShowToast(const std::string& msg);

    AppConfig config_;
    GbaBus bus_;
    GbaCpu cpu_;
    ImGuiMenu menu_;
    SaveState quickSave_;
    RuntimeStats stats_;

    bool padConnected_ = false;
    uint32_t padButtonsMask_ = 0;      // Bitmask of PadButton currently pressed
    uint32_t prevPadButtonsMask_ = 0;  // Previous frame bitmask for edge-triggered hotkeys

    std::string exeDir_;
    std::string configPath_;
    std::string sramPath_;

    void* hwnd_ = nullptr;
    void* d3dDevice_ = nullptr;
    void* d3dContext_ = nullptr;
    void* swapChain_ = nullptr;
    void* rtv_ = nullptr;
    void* fbTexture_ = nullptr;
    void* fbSrv_ = nullptr;
    void* pointSampler_ = nullptr;
    void* linearSampler_ = nullptr;

    bool running_ = true;
    bool isFullscreen_ = false;
    long savedStyle_ = 0;
    long savedExStyle_ = 0;
    long savedRect_[4] = {};

    static long long __stdcall WndProc(void* hWnd, unsigned int msg, unsigned long long wParam, long long lParam);
    long long HandleMessage(void* hWnd, unsigned int msg, unsigned long long wParam, long long lParam);
};

} // namespace gba
