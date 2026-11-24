#include "win32_dx11_app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <xinput.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gba {

namespace {
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinputGetState = nullptr;

void InitXInputDynamic() {
    if (g_xinputGetState) return;
    HMODULE mod = LoadLibraryA("xinput1_4.dll");
    if (!mod) mod = LoadLibraryA("xinput9_1_0.dll");
    if (mod) {
        g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"));
    }
}

struct SamplerCallbackData {
    ID3D11DeviceContext* ctx;
    ID3D11SamplerState* sampler;
};

void SetImGuiSamplerCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* data = static_cast<SamplerCallbackData*>(cmd->UserCallbackData);
    if (data && data->ctx && data->sampler) {
        data->ctx->PSSetSamplers(0, 1, &data->sampler);
    }
}

void ApplyCastlevaniaImGuiTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.08f, 0.08f, 0.11f, 0.94f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.18f, 0.06f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.42f, 0.10f, 0.14f, 1.00f);
    c[ImGuiCol_Tab]              = ImVec4(0.18f, 0.10f, 0.13f, 0.90f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.55f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.45f, 0.12f, 0.16f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.32f, 0.12f, 0.15f, 0.85f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.52f, 0.17f, 0.22f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.65f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.35f, 0.12f, 0.16f, 0.80f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.50f, 0.17f, 0.21f, 0.90f);
    c[ImGuiCol_CheckMark]        = ImVec4(0.95f, 0.35f, 0.40f, 1.00f);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.75f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.30f, 0.36f, 1.00f);
}
} // namespace

Win32Dx11App::Win32Dx11App() : cpu_(bus_) {}

Win32Dx11App::~Win32Dx11App() {
    bus_.apu.ShutdownAudioOutput();
    CleanupD3D11();
}

void Win32Dx11App::ShowToast(const std::string& msg) {
    stats_.statusToast = msg;
    stats_.toastTimer = 2.5f;
}

bool Win32Dx11App::InitWindow(const std::string& title, int width, int height) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Win32Dx11App::WndProc);
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "AriaOfSorrowRecompWndClass";
    RegisterClassExA(&wc);

    RECT rc{ 0, 0, width, height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowA(
        wc.lpszClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, this
    );
    if (!hwnd) return false;
    hwnd_ = hwnd;
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    return true;
}

bool Win32Dx11App::InitD3D11() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = reinterpret_cast<HWND>(hwnd_);
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION,
        &sd, &sc, &dev, &featureLevel, &ctx
    );
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &sc, &dev, &featureLevel, &ctx
        );
        if (FAILED(hr)) return false;
    }

    d3dDevice_ = dev;
    d3dContext_ = ctx;
    swapChain_ = sc;

    CreateRenderTarget();

    // Create 240x160 dynamic framebuffer texture
    D3D11_TEXTURE2D_DESC td{};
    td.Width = SCREEN_WIDTH;
    td.Height = SCREEN_HEIGHT;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) return false;
    fbTexture_ = tex;

    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) return false;
    fbSrv_ = srv;

    // Create Point & Linear samplers
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11SamplerState* pt = nullptr;
    dev->CreateSamplerState(&sampDesc, &pt);
    pointSampler_ = pt;

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11SamplerState* lin = nullptr;
    dev->CreateSamplerState(&sampDesc, &lin);
    linearSampler_ = lin;

    return true;
}

void Win32Dx11App::CreateRenderTarget() {
    auto* sc = reinterpret_cast<IDXGISwapChain*>(swapChain_);
    auto* dev = reinterpret_cast<ID3D11Device*>(d3dDevice_);
    ID3D11Texture2D* backBuffer = nullptr;
    sc->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        ID3D11RenderTargetView* rtv = nullptr;
        dev->CreateRenderTargetView(backBuffer, nullptr, &rtv);
        rtv_ = rtv;
        backBuffer->Release();
    }
}

void Win32Dx11App::CleanupRenderTarget() {
    if (rtv_) {
        reinterpret_cast<ID3D11RenderTargetView*>(rtv_)->Release();
        rtv_ = nullptr;
    }
}

void Win32Dx11App::CleanupD3D11() {
    CleanupRenderTarget();
    if (pointSampler_)  { reinterpret_cast<ID3D11SamplerState*>(pointSampler_)->Release(); pointSampler_ = nullptr; }
    if (linearSampler_) { reinterpret_cast<ID3D11SamplerState*>(linearSampler_)->Release(); linearSampler_ = nullptr; }
    if (fbSrv_)         { reinterpret_cast<ID3D11ShaderResourceView*>(fbSrv_)->Release(); fbSrv_ = nullptr; }
    if (fbTexture_)     { reinterpret_cast<ID3D11Texture2D*>(fbTexture_)->Release(); fbTexture_ = nullptr; }
    if (swapChain_)     { reinterpret_cast<IDXGISwapChain*>(swapChain_)->Release(); swapChain_ = nullptr; }
    if (d3dContext_)    { reinterpret_cast<ID3D11DeviceContext*>(d3dContext_)->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_)     { reinterpret_cast<ID3D11Device*>(d3dDevice_)->Release(); d3dDevice_ = nullptr; }
}

void Win32Dx11App::UpdateFramebufferTexture(const uint32_t* rgbaPixels) {
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(d3dContext_);
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(fbTexture_);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        const auto* src = reinterpret_cast<const uint8_t*>(rgbaPixels);
        for (int y = 0; y < SCREEN_HEIGHT; ++y) {
            std::memcpy(dst + y * mapped.RowPitch, src + y * SCREEN_WIDTH * 4, SCREEN_WIDTH * 4);
        }
        ctx->Unmap(tex, 0);
    }
}

void Win32Dx11App::ToggleFullscreen() {
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    if (!isFullscreen_) {
        savedStyle_ = GetWindowLongA(hwnd, GWL_STYLE);
        savedExStyle_ = GetWindowLongA(hwnd, GWL_EXSTYLE);
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        savedRect_[0] = rc.left; savedRect_[1] = rc.top;
        savedRect_[2] = rc.right; savedRect_[3] = rc.bottom;

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(hwnd, GWL_STYLE, savedStyle_ & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowLongA(hwnd, GWL_EXSTYLE, savedExStyle_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
        isFullscreen_ = true;
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, savedStyle_);
        SetWindowLongA(hwnd, GWL_EXSTYLE, savedExStyle_);
        SetWindowPos(hwnd, nullptr,
                     savedRect_[0], savedRect_[1],
                     savedRect_[2] - savedRect_[0], savedRect_[3] - savedRect_[1],
                     SWP_NOZORDER | SWP_FRAMECHANGED);
        isFullscreen_ = false;
    }
    config_.fullscreen = isFullscreen_;
}

void Win32Dx11App::PollGamepadState() {
    prevPadButtonsMask_ = padButtonsMask_;
    padButtonsMask_ = 0;
    padConnected_ = false;
    stats_.gamepadConnected = false;
    stats_.activePadButton = static_cast<int>(PadButton::None);

    if (!g_xinputGetState) return;

    XINPUT_STATE st{};
    bool found = false;
    for (DWORD i = 0; i < 4; ++i) {
        if (g_xinputGetState(i, &st) == ERROR_SUCCESS) {
            found = true;
            break;
        }
    }
    if (!found) return;

    padConnected_ = true;
    stats_.gamepadConnected = true;

    auto setBit = [&](PadButton b) {
        padButtonsMask_ |= (1u << static_cast<uint32_t>(b));
    };

    WORD w = st.Gamepad.wButtons;
    if (w & XINPUT_GAMEPAD_DPAD_UP)        setBit(PadButton::DpadUp);
    if (w & XINPUT_GAMEPAD_DPAD_DOWN)      setBit(PadButton::DpadDown);
    if (w & XINPUT_GAMEPAD_DPAD_LEFT)      setBit(PadButton::DpadLeft);
    if (w & XINPUT_GAMEPAD_DPAD_RIGHT)     setBit(PadButton::DpadRight);
    if (w & XINPUT_GAMEPAD_START)          setBit(PadButton::Start);
    if (w & XINPUT_GAMEPAD_BACK)           setBit(PadButton::Back);
    if (w & XINPUT_GAMEPAD_LEFT_THUMB)     setBit(PadButton::LeftThumb);
    if (w & XINPUT_GAMEPAD_RIGHT_THUMB)    setBit(PadButton::RightThumb);
    if (w & XINPUT_GAMEPAD_LEFT_SHOULDER)  setBit(PadButton::LB);
    if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) setBit(PadButton::RB);
    if (w & XINPUT_GAMEPAD_A)              setBit(PadButton::A);
    if (w & XINPUT_GAMEPAD_B)              setBit(PadButton::B);
    if (w & XINPUT_GAMEPAD_X)              setBit(PadButton::X);
    if (w & XINPUT_GAMEPAD_Y)              setBit(PadButton::Y);
    if (st.Gamepad.bLeftTrigger > 80)      setBit(PadButton::LT);
    if (st.Gamepad.bRightTrigger > 80)     setBit(PadButton::RT);
    if (st.Gamepad.sThumbLY > 14000)       setBit(PadButton::LStickUp);
    if (st.Gamepad.sThumbLY < -14000)      setBit(PadButton::LStickDown);
    if (st.Gamepad.sThumbLX < -14000)      setBit(PadButton::LStickLeft);
    if (st.Gamepad.sThumbLX > 14000)       setBit(PadButton::LStickRight);

    // Report newly pressed button first (so holding a button doesn't instantly rebind)
    uint32_t newlyPressed = padButtonsMask_ & ~prevPadButtonsMask_;
    if (newlyPressed) {
        for (int b = 1; b < static_cast<int>(PadButton::Count); ++b) {
            if (newlyPressed & (1u << b)) {
                stats_.activePadButton = b;
                break;
            }
        }
    }
}

uint16_t Win32Dx11App::PollGbaInput() const {
    if (menu_.isOpen) return 0;

    uint16_t mask = 0;
    auto isDown = [&](BindAction a) -> bool {
        int vk = config_.keyBinds[static_cast<int>(a)];
        if (vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0) return true;
        int pb = config_.padBinds[static_cast<int>(a)];
        if (pb > 0 && pb < static_cast<int>(PadButton::Count) && (padButtonsMask_ & (1u << pb)) != 0) {
            return true;
        }
        return false;
    };

    if (isDown(BindAction::A))      mask |= BTN_A;
    if (isDown(BindAction::B))      mask |= BTN_B;
    if (isDown(BindAction::Select)) mask |= BTN_SELECT;
    if (isDown(BindAction::Start))  mask |= BTN_START;
    if (isDown(BindAction::Right))  mask |= BTN_RIGHT;
    if (isDown(BindAction::Left))   mask |= BTN_LEFT;
    if (isDown(BindAction::Up))     mask |= BTN_UP;
    if (isDown(BindAction::Down))   mask |= BTN_DOWN;
    if (isDown(BindAction::R))      mask |= BTN_R;
    if (isDown(BindAction::L))      mask |= BTN_L;

    if (config_.padLeftStickAsDpad) {
        if (padButtonsMask_ & (1u << static_cast<int>(PadButton::LStickUp)))    mask |= BTN_UP;
        if (padButtonsMask_ & (1u << static_cast<int>(PadButton::LStickDown)))  mask |= BTN_DOWN;
        if (padButtonsMask_ & (1u << static_cast<int>(PadButton::LStickLeft)))  mask |= BTN_LEFT;
        if (padButtonsMask_ & (1u << static_cast<int>(PadButton::LStickRight))) mask |= BTN_RIGHT;
    }

    return mask;
}

long long __stdcall Win32Dx11App::WndProc(void* hWnd, unsigned int msg, unsigned long long wParam, long long lParam) {
    HWND hwnd = reinterpret_cast<HWND>(hWnd);
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<Win32Dx11App*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hWnd, msg, wParam, lParam);
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

long long Win32Dx11App::HandleMessage(void* hWnd, unsigned int msg, unsigned long long wParam, long long lParam) {
    HWND hwnd = reinterpret_cast<HWND>(hWnd);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        // Still allow MenuToggle (F10) even when ImGui has focus!
        if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 1;
    }

    switch (msg) {
        case WM_SIZE:
            if (d3dDevice_ && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                reinterpret_cast<IDXGISwapChain*>(swapChain_)->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU) return 0; // Disable ALT/F10 system menu beep!
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = static_cast<int>(wParam);
            bool repeat = (lParam & (1LL << 30)) != 0;
            if (!repeat) {
                if (menu_.isOpen && menu_.IsCapturingBind()) {
                    if (menu_.HandleKeyForRebind(vk)) return 0;
                } else {
                    if (vk == config_.keyBinds[static_cast<int>(BindAction::MenuToggle)]) {
                        menu_.Toggle();
                        return 0;
                    }
                    if (!menu_.isOpen) {
                        if (vk == config_.keyBinds[static_cast<int>(BindAction::Fullscreen)]) {
                            ToggleFullscreen();
                            return 0;
                        }
                        if (vk == config_.keyBinds[static_cast<int>(BindAction::QuickSave)]) {
                            cpu_.CaptureState(quickSave_);
                            UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
                            ShowToast(l == UiLanguage::Portuguese ? "Quick Save salvo!" :
                                      (l == UiLanguage::Spanish ? "Guardado rapido completado!" : "Quick Save saved!"));
                            return 0;
                        }
                        if (vk == config_.keyBinds[static_cast<int>(BindAction::QuickLoad)]) {
                            UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
                            if (cpu_.RestoreState(quickSave_))
                                ShowToast(l == UiLanguage::Portuguese ? "Quick Save carregado!" :
                                          (l == UiLanguage::Spanish ? "Carga rapida completada!" : "Quick Save loaded!"));
                            else
                                ShowToast(l == UiLanguage::Portuguese ? "Nenhum Quick Save disponivel." :
                                          (l == UiLanguage::Spanish ? "No hay guardado rapido." : "No Quick Save available."));
                            return 0;
                        }
                        if (vk == config_.keyBinds[static_cast<int>(BindAction::Reset)]) {
                            bus_.Reset();
                            cpu_.Reset();
                            UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
                            ShowToast(l == UiLanguage::Portuguese ? "Jogo resetado." :
                                      (l == UiLanguage::Spanish ? "Juego reiniciado." : "Game reset."));
                            return 0;
                        }
                    }
                }
            }
            if (vk == VK_F10) return 0; // Prevent Windows menu bar activation
            break;
        }

        case WM_DESTROY:
            running_ = false;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int Win32Dx11App::Run(int argc, char** argv) {
    // Locate executable directory for config & save file
    char exePathBuf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH);
    std::filesystem::path exePath(exePathBuf);
    exeDir_ = exePath.parent_path().string();
    std::string stem = exePath.stem().string();
    configPath_ = (exePath.parent_path() / "recomp_config.ini").string();
    sramPath_ = (exePath.parent_path() / (stem + ".sav")).string();

    // Attach embedded ROM
    size_t romSize = 0;
    const uint8_t* romPtr = GetEmbeddedRomData(romSize);
    const RecompRomInfo& romInfo = GetRecompRomInfo();
    bus_.AttachRom(romPtr, romSize);
    bus_.LoadSram(sramPath_);
    cpu_.Reset();

    // Optional headless verification mode: --headless-frames <N> [--dump-ppm <path>]
    int headlessFrames = 0;
    std::string dumpPpmPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless-frames") == 0 && i + 1 < argc) {
            headlessFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--dump-ppm") == 0 && i + 1 < argc) {
            dumpPpmPath = argv[++i];
        }
    }

    if (headlessFrames > 0) {
        auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < headlessFrames; ++f) {
            cpu_.RunFrame(true);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        const uint32_t* fb = bus_.ppu.GetFramebuffer();
        int nonBlack = 0;
        for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
            if ((fb[i] & 0x00FFFFFFu) != 0) nonBlack++;
        }
        std::printf("[Headless] ROM: %s (%s) | %d frames in %.1f ms (%.1f FPS)\n",
                    romInfo.title, romInfo.gameCode, headlessFrames, ms,
                    (ms > 0.0) ? (headlessFrames * 1000.0 / ms) : 0.0);
        std::printf("[Headless] Recompiled functions: %u | Native calls: %llu | Fallback instrs: %llu | Non-black pixels: %d/%d\n",
                    romInfo.recompiledFunctionCount,
                    static_cast<unsigned long long>(cpu_.statRecompCalls),
                    static_cast<unsigned long long>(cpu_.statFallbackInstrs),
                    nonBlack, SCREEN_WIDTH * SCREEN_HEIGHT);
        std::printf("[Headless] Avg cycles/frame: %.1f (expected %d) | SOUNDCNT_H: 0x%04X | SOUNDCNT_X: 0x%04X | TM0 reload: 0x%04X cnt: 0x%04X | DMA1: 0x%04X->0x%08X | DMA2: 0x%04X->0x%08X\n",
                    static_cast<double>(cpu_.cycles) / headlessFrames, CYCLES_PER_FRAME,
                    bus_.ReadIo16Direct(0x082), bus_.ReadIo16Direct(0x084),
                    bus_.timers[0].reload, bus_.timers[0].cnt,
                    bus_.dma[1].cntH, bus_.dma[1].dad,
                    bus_.dma[2].cntH, bus_.dma[2].dad);

        if (!dumpPpmPath.empty()) {
            std::ofstream out(dumpPpmPath, std::ios::binary);
            if (out.is_open()) {
                out << "P6\n" << SCREEN_WIDTH << " " << SCREEN_HEIGHT << "\n255\n";
                for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
                    uint32_t c = fb[i];
                    char rgb[3] = {
                        static_cast<char>(c & 0xFF),
                        static_cast<char>((c >> 8) & 0xFF),
                        static_cast<char>((c >> 16) & 0xFF)
                    };
                    out.write(rgb, 3);
                }
            }
        }
        return 0;
    }

    config_.Load(configPath_);
    InitXInputDynamic();

    std::string winTitle = std::string("Castlevania: Aria of Sorrow [Static Recomp x86_64] - ") +
                           romInfo.title + " | F10: Config";
    if (!InitWindow(winTitle, SCREEN_WIDTH * config_.windowScale, SCREEN_HEIGHT * config_.windowScale)) {
        return 1;
    }
    if (!InitD3D11()) {
        return 1;
    }

    bus_.apu.InitAudioOutput();
    bus_.apu.SetVolume(config_.masterVolume, config_.muteAudio);

    // Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Don't clutter with imgui.ini
    ApplyCastlevaniaImGuiTheme();

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(reinterpret_cast<ID3D11Device*>(d3dDevice_),
                        reinterpret_cast<ID3D11DeviceContext*>(d3dContext_));

    if (config_.fullscreen) {
        ToggleFullscreen();
    }

    {
        UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
        ShowToast(l == UiLanguage::Portuguese ? "Pressione F10 para abrir as configuracoes e binds (ImGui)" :
                  (l == UiLanguage::Spanish ? "Pulsa F10 para abrir la configuracion y controles (ImGui)" :
                                              "Press F10 to open settings and keybinds (ImGui)"));
    }

    // Request 1ms Windows timer resolution for smooth frame & audio pacing
    timeBeginPeriod(1);
    using Clock = std::chrono::steady_clock;
    // 16.667 ms aligns cleanly with 60/120/180Hz VSync without 33ms VSync-miss stalls;
    // APU Dynamic Rate Control (DRC) smoothly absorbs the 0.45% diff to native 59.73Hz with zero clicks!
    constexpr auto kFrameDuration = std::chrono::microseconds(16667);

    auto lastTime = Clock::now();
    auto nextFrameTime = lastTime + kFrameDuration;
    auto fpsTimer = lastTime;
    int fpsFrames = 0;
    int sramSaveCooldown = 0;

    while (running_) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) running_ = false;
        }
        if (!running_) break;

        PollGamepadState();

        // Handle edge-triggered gamepad hotkeys (when not capturing a bind)
        if (!menu_.IsCapturingBind()) {
            auto padJustPressed = [&](BindAction a) -> bool {
                int pb = config_.padBinds[static_cast<int>(a)];
                if (pb <= 0 || pb >= static_cast<int>(PadButton::Count)) return false;
                uint32_t bit = 1u << pb;
                return (padButtonsMask_ & bit) != 0 && (prevPadButtonsMask_ & bit) == 0;
            };

            if (padJustPressed(BindAction::MenuToggle)) {
                menu_.Toggle();
            } else if (!menu_.isOpen) {
                if (padJustPressed(BindAction::Fullscreen)) ToggleFullscreen();
                if (padJustPressed(BindAction::QuickSave)) {
                    cpu_.CaptureState(quickSave_);
                    UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
                    ShowToast(l == UiLanguage::Portuguese ? "Quick Save salvo!" :
                              (l == UiLanguage::Spanish ? "Guardado rapido completado!" : "Quick Save saved!"));
                }
                if (padJustPressed(BindAction::QuickLoad)) {
                    UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
                    if (cpu_.RestoreState(quickSave_))
                        ShowToast(l == UiLanguage::Portuguese ? "Quick Save carregado!" :
                                  (l == UiLanguage::Spanish ? "Carga rapida completada!" : "Quick Save loaded!"));
                    else
                        ShowToast(l == UiLanguage::Portuguese ? "Nenhum Quick Save disponivel." :
                                  (l == UiLanguage::Spanish ? "No hay guardado rapido." : "No Quick Save available."));
                }
                if (padJustPressed(BindAction::Reset)) {
                    bus_.Reset();
                    cpu_.Reset();
                    UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
                    ShowToast(l == UiLanguage::Portuguese ? "Jogo resetado." :
                              (l == UiLanguage::Spanish ? "Juego reiniciado." : "Game reset."));
                }
            }
        }

        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (stats_.toastTimer > 0.0f) stats_.toastTimer = std::max(0.0f, stats_.toastTimer - dt);

        // Emulate 1 (or fast-forward N) frames
        bool paused = menu_.isOpen && config_.pauseOnMenu;
        if (!paused) {
            bus_.SetKeypadState(PollGbaInput());
            bus_.apu.SetVolume(config_.masterVolume, config_.muteAudio);

            int ffVk = config_.keyBinds[static_cast<int>(BindAction::FastForward)];
            int ffPb = config_.padBinds[static_cast<int>(BindAction::FastForward)];
            bool ffKey = (ffVk > 0 && (GetAsyncKeyState(ffVk) & 0x8000) != 0);
            bool ffPad = (ffPb > 0 && ffPb < static_cast<int>(PadButton::Count) && (padButtonsMask_ & (1u << ffPb)) != 0);
            bool ffHeld = (!menu_.isOpen && (ffKey || ffPad));
            int framesToRun = ffHeld ? config_.fastForwardSpeed : 1;
            for (int f = 0; f < framesToRun; ++f) {
                cpu_.RunFrame(config_.enableRecomp);
            }
            UpdateFramebufferTexture(bus_.ppu.GetFramebuffer());
        } else {
            bus_.apu.OnPaused();
        }

        // Periodic SRAM auto-save
        if (bus_.sramDirty) {
            sramSaveCooldown++;
            if (sramSaveCooldown >= 60) {
                bus_.SaveSram(sramPath_);
                bus_.sramDirty = false;
                sramSaveCooldown = 0;
            }
        }

        // Update stats
        fpsFrames++;
        double fpsElapsed = std::chrono::duration<double>(now - fpsTimer).count();
        if (fpsElapsed >= 0.5) {
            stats_.fps = static_cast<float>(fpsFrames / fpsElapsed);
            fpsFrames = 0;
            fpsTimer = now;
        }
        stats_.recompCalls = cpu_.statRecompCalls;
        stats_.recompInstrs = cpu_.statRecompInstrs;
        stats_.fallbackInstrs = cpu_.statFallbackInstrs;
        stats_.hasQuickSave = quickSave_.valid;

        // Render ImGui + GBA Framebuffer
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Draw GBA screen on background draw list
        ImVec2 disp = io.DisplaySize;
        if (disp.x > 0 && disp.y > 0) {
            ImVec2 imgMin(0, 0), imgMax = disp;
            if (config_.aspectMode == AspectMode::Fit) {
                float targetAspect = static_cast<float>(SCREEN_WIDTH) / static_cast<float>(SCREEN_HEIGHT);
                float winAspect = disp.x / disp.y;
                if (winAspect > targetAspect) {
                    float w = disp.y * targetAspect;
                    imgMin.x = (disp.x - w) * 0.5f;
                    imgMax.x = imgMin.x + w;
                } else {
                    float h = disp.x / targetAspect;
                    imgMin.y = (disp.y - h) * 0.5f;
                    imgMax.y = imgMin.y + h;
                }
            } else if (config_.aspectMode == AspectMode::IntegerScale) {
                int scale = std::max(1, std::min(static_cast<int>(disp.x) / SCREEN_WIDTH,
                                                 static_cast<int>(disp.y) / SCREEN_HEIGHT));
                float w = static_cast<float>(SCREEN_WIDTH * scale);
                float h = static_cast<float>(SCREEN_HEIGHT * scale);
                imgMin.x = (disp.x - w) * 0.5f;
                imgMin.y = (disp.y - h) * 0.5f;
                imgMax.x = imgMin.x + w;
                imgMax.y = imgMin.y + h;
            }

            static SamplerCallbackData s_samplerData;
            s_samplerData.ctx = reinterpret_cast<ID3D11DeviceContext*>(d3dContext_);
            s_samplerData.sampler = reinterpret_cast<ID3D11SamplerState*>(
                config_.linearFilter ? linearSampler_ : pointSampler_);

            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            bg->AddCallback(SetImGuiSamplerCallback, &s_samplerData);
            bg->AddImage(reinterpret_cast<ImTextureID>(fbSrv_), imgMin, imgMax);
            bg->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }

        // Render F10 Menu & HUD overlays
        MenuActions act = menu_.Render(config_, romInfo, stats_);
        menu_.RenderOverlayStats(config_, romInfo, stats_);

        if (act.requestSaveConfig) config_.Save(configPath_);
        if (act.requestToggleFullscreen) ToggleFullscreen();
        if (act.requestQuickSave) {
            cpu_.CaptureState(quickSave_);
            UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
            ShowToast(l == UiLanguage::Portuguese ? "Quick Save salvo!" :
                      (l == UiLanguage::Spanish ? "Guardado rapido completado!" : "Quick Save saved!"));
        }
        if (act.requestQuickLoad) {
            UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
            if (cpu_.RestoreState(quickSave_))
                ShowToast(l == UiLanguage::Portuguese ? "Quick Save carregado!" :
                          (l == UiLanguage::Spanish ? "Carga rapida completada!" : "Quick Save loaded!"));
        }
        if (act.requestResetGame) {
            bus_.Reset();
            cpu_.Reset();
            UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
            ShowToast(l == UiLanguage::Portuguese ? "Jogo resetado." :
                      (l == UiLanguage::Spanish ? "Juego reiniciado." : "Game reset."));
        }
        if (act.requestExitApp) running_ = false;

        ImGui::Render();

        auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(d3dContext_);
        auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(rtv_);
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        reinterpret_cast<IDXGISwapChain*>(swapChain_)->Present(config_.vsync ? 1 : 0, 0);

        // Pace cleanly to ~60Hz (prevents speedup on 120/144/165Hz monitors while avoiding 60Hz VSync-miss stalls)
        auto frameEnd = Clock::now();
        if (frameEnd < nextFrameTime) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextFrameTime - frameEnd);
            if (remaining.count() > 2) {
                Sleep(static_cast<DWORD>(remaining.count() - 1));
            }
            while (Clock::now() < nextFrameTime) {
            }
        }
        nextFrameTime += kFrameDuration;
        if (Clock::now() > nextFrameTime + std::chrono::milliseconds(50)) {
            nextFrameTime = Clock::now() + kFrameDuration;
        }
    }

    timeEndPeriod(1);

    // Save SRAM & config on exit
    if (bus_.sramDirty) bus_.SaveSram(sramPath_);
    config_.Save(configPath_);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

} // namespace gba
