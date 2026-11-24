#include "web_sdl_app.h"

#if defined(__EMSCRIPTEN__) || defined(__linux__)

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#else
#include "aot_loader.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <limits.h>
#endif

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace gba {

namespace {
void ApplyCastlevaniaSdlTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.TabRounding = 4.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.08f, 0.08f, 0.11f, 0.94f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.18f, 0.06f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.42f, 0.10f, 0.14f, 1.00f);
    c[ImGuiCol_Tab]           = ImVec4(0.18f, 0.10f, 0.13f, 0.90f);
    c[ImGuiCol_TabHovered]    = ImVec4(0.55f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TabSelected]   = ImVec4(0.45f, 0.12f, 0.16f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.32f, 0.12f, 0.15f, 0.85f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.52f, 0.17f, 0.22f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.65f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.95f, 0.35f, 0.40f, 1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.75f, 0.22f, 0.28f, 1.00f);
}

#ifdef __EMSCRIPTEN__
void EmscriptenMainLoopBridge(void* arg) {
    static_cast<WebSdlApp*>(arg)->MainLoopTick();
}
#endif

double GetNowSec() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now() * 0.001;
#else
    using Clock = std::chrono::steady_clock;
    static const auto kStart = Clock::now();
    return std::chrono::duration<double>(Clock::now() - kStart).count();
#endif
}
} // namespace

#if defined(__EMSCRIPTEN__) || defined(RECOMP_STATIC_CPP)
const RuntimeHelpers* GetRuntimeHelpers() {
    return nullptr;
}
#endif

WebSdlApp::WebSdlApp() : cpu_(bus_) {}

WebSdlApp::~WebSdlApp() {
    bus_.apu.ShutdownAudioOutput();
#ifdef __linux__
    if (bus_.sramDirty && !sramPath_.empty()) bus_.SaveSram(sramPath_);
    if (!configPath_.empty()) config_.Save(configPath_);
#endif
    if (controller_) SDL_GameControllerClose(controller_);
    if (fbTexture_) glDeleteTextures(1, &fbTexture_);
    if (glContext_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(glContext_);
    }
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void WebSdlApp::ShowToast(const std::string& msg) {
    stats_.statusToast = msg;
    stats_.toastTimer = 2.5f;
}

void WebSdlApp::ToggleFullscreen() {
    if (!window_) return;
    isFullscreen_ = !isFullscreen_;
    SDL_SetWindowFullscreen(window_, isFullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    config_.fullscreen = isFullscreen_;
}

void WebSdlApp::PollGamepadState() {
    prevPadButtonsMask_ = padButtonsMask_;
    padButtonsMask_ = 0;
    stats_.activePadButton = static_cast<int>(PadButton::None);

    if (!controller_ && SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) {
        controller_ = SDL_GameControllerOpen(0);
    }
    if (controller_ && !SDL_GameControllerGetAttached(controller_)) {
        SDL_GameControllerClose(controller_);
        controller_ = nullptr;
    }

    stats_.gamepadConnected = (controller_ != nullptr);
    if (!controller_) return;

    auto setBit = [&](PadButton b) {
        padButtonsMask_ |= (1u << static_cast<uint32_t>(b));
    };

    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_UP))        setBit(PadButton::DpadUp);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_DOWN))      setBit(PadButton::DpadDown);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_LEFT))      setBit(PadButton::DpadLeft);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))     setBit(PadButton::DpadRight);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_START))          setBit(PadButton::Start);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_BACK))           setBit(PadButton::Back);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_LEFTSTICK))      setBit(PadButton::LeftThumb);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_RIGHTSTICK))     setBit(PadButton::RightThumb);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))   setBit(PadButton::LB);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))  setBit(PadButton::RB);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_A))              setBit(PadButton::A);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_B))              setBit(PadButton::B);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_X))              setBit(PadButton::X);
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_Y))              setBit(PadButton::Y);
    if (SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) setBit(PadButton::LT);
    if (SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) setBit(PadButton::RT);
    Sint16 lx = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 ly = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);
    if (ly < -14000) setBit(PadButton::LStickUp);
    if (ly >  14000) setBit(PadButton::LStickDown);
    if (lx < -14000) setBit(PadButton::LStickLeft);
    if (lx >  14000) setBit(PadButton::LStickRight);

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

uint16_t WebSdlApp::PollGbaInput() const {
    if (menu_.isOpen) return 0;

    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    uint16_t mask = 0;

    auto isDown = [&](BindAction a) -> bool {
        int sc = config_.keyBinds[static_cast<int>(a)];
        if (sc > 0 && ks[sc]) return true;
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

void WebSdlApp::UpdateFramebufferTexture(const uint32_t* rgbaPixels) {
    glBindTexture(GL_TEXTURE_2D, fbTexture_);
    GLint filter = config_.linearFilter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
}

void WebSdlApp::MainLoopTick() {
    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) {
            running_ = false;
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            int sc = static_cast<int>(ev.key.keysym.scancode);
            if (menu_.isOpen && menu_.IsCapturingBind()) {
                menu_.HandleKeyForRebind(sc);
            } else {
                if (sc == config_.keyBinds[static_cast<int>(BindAction::MenuToggle)]) {
                    menu_.Toggle();
                } else if (!menu_.isOpen) {
                    UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
                    if (sc == config_.keyBinds[static_cast<int>(BindAction::Fullscreen)]) {
                        ToggleFullscreen();
                    } else if (sc == config_.keyBinds[static_cast<int>(BindAction::QuickSave)]) {
                        cpu_.CaptureState(quickSave_);
                        ShowToast(l == UiLanguage::Portuguese ? "Quick Save salvo!" :
                                  (l == UiLanguage::Spanish ? "Guardado rapido completado!" : "Quick Save saved!"));
                    } else if (sc == config_.keyBinds[static_cast<int>(BindAction::QuickLoad)]) {
                        if (cpu_.RestoreState(quickSave_))
                            ShowToast(l == UiLanguage::Portuguese ? "Quick Save carregado!" :
                                      (l == UiLanguage::Spanish ? "Carga rapida completada!" : "Quick Save loaded!"));
                        else
                            ShowToast(l == UiLanguage::Portuguese ? "Nenhum Quick Save disponivel." :
                                      (l == UiLanguage::Spanish ? "No hay guardado rapido." : "No Quick Save available."));
                    } else if (sc == config_.keyBinds[static_cast<int>(BindAction::Reset)]) {
                        bus_.Reset();
                        cpu_.Reset();
                        ShowToast(l == UiLanguage::Portuguese ? "Jogo resetado." :
                                  (l == UiLanguage::Spanish ? "Juego reiniciado." : "Game reset."));
                    }
                }
            }
        }
    }

    PollGamepadState();

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
            if (padJustPressed(BindAction::QuickSave)) { cpu_.CaptureState(quickSave_); ShowToast("Quick Save!"); }
            if (padJustPressed(BindAction::QuickLoad) && cpu_.RestoreState(quickSave_)) { ShowToast("Quick Load!"); }
            if (padJustPressed(BindAction::Reset)) { bus_.Reset(); cpu_.Reset(); ShowToast("Reset!"); }
        }
    }

    double nowSec = GetNowSec();
    float dt = static_cast<float>(nowSec - lastTimeSec_);
    lastTimeSec_ = nowSec;
    if (stats_.toastTimer > 0.0f) stats_.toastTimer = std::max(0.0f, stats_.toastTimer - dt);

    bool paused = menu_.isOpen && config_.pauseOnMenu;
    if (!paused) {
        bus_.SetKeypadState(PollGbaInput());
        bus_.apu.SetVolume(config_.masterVolume, config_.muteAudio);

        const Uint8* ks = SDL_GetKeyboardState(nullptr);
        int ffSc = config_.keyBinds[static_cast<int>(BindAction::FastForward)];
        int ffPb = config_.padBinds[static_cast<int>(BindAction::FastForward)];
        bool ffHeld = !menu_.isOpen && ((ffSc > 0 && ks[ffSc]) ||
                      (ffPb > 0 && ffPb < static_cast<int>(PadButton::Count) && (padButtonsMask_ & (1u << ffPb)) != 0));
        int frames = ffHeld ? config_.fastForwardSpeed : 1;
        for (int f = 0; f < frames; ++f) {
            cpu_.RunFrame(config_.enableRecomp);
        }
        UpdateFramebufferTexture(bus_.ppu.GetFramebuffer());
    } else {
        bus_.apu.OnPaused();
    }

#ifdef __linux__
    if (bus_.sramDirty && !sramPath_.empty()) {
        sramSaveCooldown_++;
        if (sramSaveCooldown_ >= 60) {
            bus_.SaveSram(sramPath_);
            bus_.sramDirty = false;
            sramSaveCooldown_ = 0;
        }
    }
#endif

    fpsFrames_++;
    if (nowSec - fpsTimerSec_ >= 0.5) {
        stats_.fps = static_cast<float>(fpsFrames_ / (nowSec - fpsTimerSec_));
        fpsFrames_ = 0;
        fpsTimerSec_ = nowSec;
    }
    stats_.recompCalls = cpu_.statRecompCalls;
    stats_.recompInstrs = cpu_.statRecompInstrs;
    stats_.fallbackInstrs = cpu_.statFallbackInstrs;
    stats_.hasQuickSave = quickSave_.valid;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
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
        ImGui::GetBackgroundDrawList()->AddImage(
            static_cast<ImTextureID>(fbTexture_),
            imgMin, imgMax);
    }

    const RecompRomInfo& romInfo = GetRecompRomInfo();
    MenuActions act = menu_.Render(config_, romInfo, stats_);
    menu_.RenderOverlayStats(config_, romInfo, stats_);

    if (act.requestSaveConfig && !configPath_.empty()) config_.Save(configPath_);
    if (act.requestToggleFullscreen) ToggleFullscreen();
    if (act.requestQuickSave) { cpu_.CaptureState(quickSave_); ShowToast("Quick Save!"); }
    if (act.requestQuickLoad && cpu_.RestoreState(quickSave_)) { ShowToast("Quick Load!"); }
    if (act.requestResetGame) { bus_.Reset(); cpu_.Reset(); ShowToast("Reset!"); }
    if (act.requestExitApp) running_ = false;

    ImGui::Render();
    glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);
}

int WebSdlApp::Run(int argc, char** argv) {
#ifdef __linux__
    char exeBuf[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", exeBuf, sizeof(exeBuf) - 1);
    if (len > 0) {
        exeBuf[len] = '\0';
        std::filesystem::path exePath(exeBuf);
        configPath_ = (exePath.parent_path() / "recomp_config.ini").string();
        sramPath_ = (exePath.parent_path() / (exePath.stem().string() + ".sav")).string();
        config_.Load(configPath_);
    }
#endif

    size_t romSize = 0;
    const uint8_t* romPtr = GetEmbeddedRomData(romSize);
    const RecompRomInfo& romInfo = GetRecompRomInfo();
    bus_.AttachRom(romPtr, romSize);
    if (!sramPath_.empty()) bus_.LoadSram(sramPath_);
    cpu_.Reset();

#ifdef __linux__
    int headlessFrames = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless-frames") == 0 && i + 1 < argc) {
            headlessFrames = std::atoi(argv[++i]);
        }
    }
    if (headlessFrames > 0) {
        double t0 = GetNowSec();
        for (int f = 0; f < headlessFrames; ++f) cpu_.RunFrame(true);
        double ms = (GetNowSec() - t0) * 1000.0;
        const uint32_t* fb = bus_.ppu.GetFramebuffer();
        int nonBlack = 0;
        for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
            if ((fb[i] & 0x00FFFFFFu) != 0) nonBlack++;
        }
        std::printf("[Headless] ROM: %s (%s) | %d frames in %.1f ms (%.1f FPS)\n",
                    romInfo.title, romInfo.gameCode, headlessFrames, ms,
                    ms > 0.0 ? (headlessFrames * 1000.0 / ms) : 0.0);
        std::printf("[Headless] Recompiled functions: %u | Native calls: %llu | Fallback instrs: %llu | Non-black pixels: %d/%d\n",
                    romInfo.recompiledFunctionCount,
                    static_cast<unsigned long long>(cpu_.statRecompCalls),
                    static_cast<unsigned long long>(cpu_.statFallbackInstrs),
                    nonBlack, SCREEN_WIDTH * SCREEN_HEIGHT);
        return 0;
    }
#else
    (void)argc; (void)argv;
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

#ifdef __EMSCRIPTEN__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    const char* glslVersion = "#version 300 es";
    std::string winTitle = "Castlevania: Aria of Sorrow [WebAssembly Recomp]";
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    const char* glslVersion = "#version 330";
    std::string winTitle = std::string("Castlevania: Aria of Sorrow [Linux Static Recomp x86_64] - ") +
                           romInfo.title + " | F10: Config";
#endif

    window_ = SDL_CreateWindow(winTitle.c_str(),
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               SCREEN_WIDTH * config_.windowScale, SCREEN_HEIGHT * config_.windowScale,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window_) return 1;

    glContext_ = SDL_GL_CreateContext(window_);
    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(config_.vsync ? 1 : 0);

    glGenTextures(1, &fbTexture_);
    glBindTexture(GL_TEXTURE_2D, fbTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREEN_WIDTH, SCREEN_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    bus_.apu.InitAudioOutput();
    bus_.apu.SetVolume(config_.masterVolume, config_.muteAudio);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ApplyCastlevaniaSdlTheme();

    ImGui_ImplSDL2_InitForOpenGL(window_, glContext_);
    ImGui_ImplOpenGL3_Init(glslVersion);

    if (config_.fullscreen) ToggleFullscreen();

    UiLanguage l = ResolveEffectiveLanguage(config_.uiLanguage);
    ShowToast(l == UiLanguage::Portuguese ? "Pressione F10 para abrir as configuracoes e binds (ImGui)" :
              (l == UiLanguage::Spanish ? "Pulsa F10 para abrir la configuracion y controles (ImGui)" :
                                          "Press F10 to open settings and keybinds (ImGui)"));

    lastTimeSec_ = GetNowSec();
    fpsTimerSec_ = lastTimeSec_;

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(EmscriptenMainLoopBridge, this, 60, 1);
#else
    using Clock = std::chrono::steady_clock;
    constexpr auto kFrameDuration = std::chrono::microseconds(16667);
    auto nextFrameTime = Clock::now() + kFrameDuration;
    while (running_) {
        MainLoopTick();
        auto now = Clock::now();
        if (now < nextFrameTime) {
            std::this_thread::sleep_until(nextFrameTime);
        }
        nextFrameTime += kFrameDuration;
        if (Clock::now() > nextFrameTime + std::chrono::milliseconds(50)) {
            nextFrameTime = Clock::now() + kFrameDuration;
        }
    }
#endif
    return 0;
}

} // namespace gba

int main(int argc, char** argv) {
#if defined(__linux__) && !defined(RECOMP_STATIC_CPP)
    if (!gba::LoadEmbeddedAotBundle()) {
        std::fprintf(stderr, "[ERROR] No embedded Aria of Sorrow AOT bundle found in this executable.\n"
                             "Generate a playable binary using: ./recomp-ariaofsorrow <rom.gba>\n");
        return 1;
    }
#endif

    int rc = 0;
    {
        auto app = std::make_unique<gba::WebSdlApp>();
        rc = app->Run(argc, argv);
    }

#if defined(__linux__) && !defined(RECOMP_STATIC_CPP)
    gba::UnloadEmbeddedAotBundle();
#endif
    return rc;
}

#endif // __EMSCRIPTEN__ || __linux__
