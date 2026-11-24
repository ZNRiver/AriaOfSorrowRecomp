#pragma once

#include <cstdint>
#include <string>

namespace gba {

enum class BindAction : int {
    Up = 0,
    Down,
    Left,
    Right,
    A,
    B,
    L,
    R,
    Start,
    Select,
    MenuToggle,
    FastForward,
    QuickSave,
    QuickLoad,
    Reset,
    Fullscreen,
    Count
};

enum class PadButton : int {
    None = 0,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Start,
    Back,
    LeftThumb,
    RightThumb,
    LB,
    RB,
    A,
    B,
    X,
    Y,
    LT,
    RT,
    LStickUp,
    LStickDown,
    LStickLeft,
    LStickRight,
    Count
};

enum class AspectMode : int {
    Fit = 0,
    IntegerScale = 1,
    Stretch = 2,
};

enum class UiLanguage : int {
    Auto = 0,
    English = 1,
    Portuguese = 2,
    Spanish = 3,
};

struct AppConfig {
    // Win32 Virtual-Key codes (VK_*)
    int keyBinds[static_cast<int>(BindAction::Count)];
    // Gamepad (XInput) button bindings (PadButton)
    int padBinds[static_cast<int>(BindAction::Count)];
    bool padLeftStickAsDpad = true;

    // Video
    AspectMode aspectMode = AspectMode::Fit;
    bool linearFilter = false;   // false = crisp pixel-art nearest neighbor
    bool vsync = true;
    bool fullscreen = false;
    bool showOverlayStats = false;
    int windowScale = 4;         // Default 960x640

    // Audio
    float masterVolume = 0.80f;
    bool muteAudio = false;

    // Emulation / System
    UiLanguage uiLanguage = UiLanguage::Auto;
    bool pauseOnMenu = true;
    int fastForwardSpeed = 3;    // 3x speed when holding fast-forward
    bool enableRecomp = true;    // Toggle static recompiled code vs pure fallback

    AppConfig();
    void ResetDefaultBinds();
    void ResetDefaultPadBinds();
    void ResetAllDefaults();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

UiLanguage ResolveEffectiveLanguage(UiLanguage pref);
const char* GetBindActionName(BindAction action, UiLanguage lang = UiLanguage::English);
std::string GetVkName(int vk);
const char* GetPadButtonName(int padBtn, UiLanguage lang = UiLanguage::English);

} // namespace gba
