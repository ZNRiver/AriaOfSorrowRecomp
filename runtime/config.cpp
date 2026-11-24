#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>

namespace gba {

AppConfig::AppConfig() {
    ResetAllDefaults();
}

void AppConfig::ResetDefaultBinds() {
    keyBinds[static_cast<int>(BindAction::Up)]          = VK_UP;
    keyBinds[static_cast<int>(BindAction::Down)]        = VK_DOWN;
    keyBinds[static_cast<int>(BindAction::Left)]        = VK_LEFT;
    keyBinds[static_cast<int>(BindAction::Right)]       = VK_RIGHT;
    keyBinds[static_cast<int>(BindAction::A)]           = 'X';
    keyBinds[static_cast<int>(BindAction::B)]           = 'Z';
    keyBinds[static_cast<int>(BindAction::L)]           = 'A';
    keyBinds[static_cast<int>(BindAction::R)]           = 'S';
    keyBinds[static_cast<int>(BindAction::Start)]       = VK_RETURN;
    keyBinds[static_cast<int>(BindAction::Select)]      = VK_BACK;
    keyBinds[static_cast<int>(BindAction::MenuToggle)]  = VK_F10;
    keyBinds[static_cast<int>(BindAction::FastForward)] = VK_TAB;
    keyBinds[static_cast<int>(BindAction::QuickSave)]   = VK_F5;
    keyBinds[static_cast<int>(BindAction::QuickLoad)]   = VK_F7;
    keyBinds[static_cast<int>(BindAction::Reset)]       = VK_F8;
    keyBinds[static_cast<int>(BindAction::Fullscreen)]  = VK_F11;
}

void AppConfig::ResetDefaultPadBinds() {
    padBinds[static_cast<int>(BindAction::Up)]          = static_cast<int>(PadButton::DpadUp);
    padBinds[static_cast<int>(BindAction::Down)]        = static_cast<int>(PadButton::DpadDown);
    padBinds[static_cast<int>(BindAction::Left)]        = static_cast<int>(PadButton::DpadLeft);
    padBinds[static_cast<int>(BindAction::Right)]       = static_cast<int>(PadButton::DpadRight);
    padBinds[static_cast<int>(BindAction::A)]           = static_cast<int>(PadButton::A);
    padBinds[static_cast<int>(BindAction::B)]           = static_cast<int>(PadButton::X);
    padBinds[static_cast<int>(BindAction::L)]           = static_cast<int>(PadButton::LB);
    padBinds[static_cast<int>(BindAction::R)]           = static_cast<int>(PadButton::RB);
    padBinds[static_cast<int>(BindAction::Start)]       = static_cast<int>(PadButton::Start);
    padBinds[static_cast<int>(BindAction::Select)]      = static_cast<int>(PadButton::Back);
    padBinds[static_cast<int>(BindAction::MenuToggle)]  = static_cast<int>(PadButton::RightThumb);
    padBinds[static_cast<int>(BindAction::FastForward)] = static_cast<int>(PadButton::RT);
    padBinds[static_cast<int>(BindAction::QuickSave)]   = static_cast<int>(PadButton::None);
    padBinds[static_cast<int>(BindAction::QuickLoad)]   = static_cast<int>(PadButton::None);
    padBinds[static_cast<int>(BindAction::Reset)]       = static_cast<int>(PadButton::None);
    padBinds[static_cast<int>(BindAction::Fullscreen)]  = static_cast<int>(PadButton::None);
    padLeftStickAsDpad = true;
}

void AppConfig::ResetAllDefaults() {
    ResetDefaultBinds();
    ResetDefaultPadBinds();
    aspectMode = AspectMode::Fit;
    linearFilter = false;
    vsync = true;
    fullscreen = false;
    showOverlayStats = false;
    windowScale = 4;
    masterVolume = 0.80f;
    muteAudio = false;
    uiLanguage = UiLanguage::Auto;
    pauseOnMenu = true;
    fastForwardSpeed = 3;
    enableRecomp = true;
}

UiLanguage ResolveEffectiveLanguage(UiLanguage pref) {
    if (pref != UiLanguage::Auto) return pref;
    LANGID langId = GetUserDefaultUILanguage();
    WORD primary = PRIMARYLANGID(langId);
    if (primary == LANG_PORTUGUESE) return UiLanguage::Portuguese;
    if (primary == LANG_SPANISH)    return UiLanguage::Spanish;
    return UiLanguage::English;
}

static const char* kActionKeys[static_cast<int>(BindAction::Count)] = {
    "bind_up", "bind_down", "bind_left", "bind_right",
    "bind_a", "bind_b", "bind_l", "bind_r",
    "bind_start", "bind_select",
    "bind_menu_toggle", "bind_fast_forward",
    "bind_quick_save", "bind_quick_load",
    "bind_reset", "bind_fullscreen"
};

static const char* kPadActionKeys[static_cast<int>(BindAction::Count)] = {
    "pad_up", "pad_down", "pad_left", "pad_right",
    "pad_a", "pad_b", "pad_l", "pad_r",
    "pad_start", "pad_select",
    "pad_menu_toggle", "pad_fast_forward",
    "pad_quick_save", "pad_quick_load",
    "pad_reset", "pad_fullscreen"
};

bool AppConfig::Load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        try {
            for (int i = 0; i < static_cast<int>(BindAction::Count); ++i) {
                if (key == kActionKeys[i]) {
                    keyBinds[i] = std::stoi(val);
                } else if (key == kPadActionKeys[i]) {
                    padBinds[i] = std::clamp(std::stoi(val), 0, static_cast<int>(PadButton::Count) - 1);
                }
            }
            if (key == "pad_lstick_as_dpad") padLeftStickAsDpad = (std::stoi(val) != 0);
            else if (key == "aspect_mode") aspectMode = static_cast<AspectMode>(std::clamp(std::stoi(val), 0, 2));
            else if (key == "linear_filter") linearFilter = (std::stoi(val) != 0);
            else if (key == "vsync") vsync = (std::stoi(val) != 0);
            else if (key == "fullscreen") fullscreen = (std::stoi(val) != 0);
            else if (key == "show_overlay_stats") showOverlayStats = (std::stoi(val) != 0);
            else if (key == "window_scale") windowScale = std::clamp(std::stoi(val), 1, 8);
            else if (key == "master_volume") masterVolume = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "mute_audio") muteAudio = (std::stoi(val) != 0);
            else if (key == "ui_language") uiLanguage = static_cast<UiLanguage>(std::clamp(std::stoi(val), 0, 3));
            else if (key == "pause_on_menu") pauseOnMenu = (std::stoi(val) != 0);
            else if (key == "fast_forward_speed") fastForwardSpeed = std::clamp(std::stoi(val), 2, 8);
            else if (key == "enable_recomp") enableRecomp = (std::stoi(val) != 0);
        } catch (...) {
        }
    }
    return true;
}

bool AppConfig::Save(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) return false;

    out << "# Aria of Sorrow Static Recompiler Configuration\n";
    out << "[Controls]\n";
    for (int i = 0; i < static_cast<int>(BindAction::Count); ++i) {
        out << kActionKeys[i] << "=" << keyBinds[i] << "\n";
    }
    out << "\n[Gamepad]\n";
    for (int i = 0; i < static_cast<int>(BindAction::Count); ++i) {
        out << kPadActionKeys[i] << "=" << padBinds[i] << "\n";
    }
    out << "pad_lstick_as_dpad=" << (padLeftStickAsDpad ? 1 : 0) << "\n";

    out << "\n[Video]\n";
    out << "aspect_mode=" << static_cast<int>(aspectMode) << "\n";
    out << "linear_filter=" << (linearFilter ? 1 : 0) << "\n";
    out << "vsync=" << (vsync ? 1 : 0) << "\n";
    out << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    out << "show_overlay_stats=" << (showOverlayStats ? 1 : 0) << "\n";
    out << "window_scale=" << windowScale << "\n";

    out << "\n[Audio]\n";
    out << "master_volume=" << masterVolume << "\n";
    out << "mute_audio=" << (muteAudio ? 1 : 0) << "\n";

    out << "\n[System]\n";
    out << "ui_language=" << static_cast<int>(uiLanguage) << "\n";
    out << "pause_on_menu=" << (pauseOnMenu ? 1 : 0) << "\n";
    out << "fast_forward_speed=" << fastForwardSpeed << "\n";
    out << "enable_recomp=" << (enableRecomp ? 1 : 0) << "\n";
    return true;
}

const char* GetBindActionName(BindAction action, UiLanguage lang) {
    UiLanguage eff = ResolveEffectiveLanguage(lang);
    if (eff == UiLanguage::Portuguese) {
        switch (action) {
            case BindAction::Up:          return "D-Pad Cima";
            case BindAction::Down:        return "D-Pad Baixo";
            case BindAction::Left:        return "D-Pad Esquerda";
            case BindAction::Right:       return "D-Pad Direita";
            case BindAction::A:           return "Botao A (Pular)";
            case BindAction::B:           return "Botao B (Atacar)";
            case BindAction::L:           return "Botao L (Esquiva)";
            case BindAction::R:           return "Botao R (Alma Guardiao)";
            case BindAction::Start:       return "Start (Menu)";
            case BindAction::Select:      return "Select (Mapa)";
            case BindAction::MenuToggle:  return "Menu de Configuracoes (ImGui)";
            case BindAction::FastForward: return "Avancar Rapido (Segurar)";
            case BindAction::QuickSave:   return "Quick Save";
            case BindAction::QuickLoad:   return "Quick Load";
            case BindAction::Reset:       return "Resetar Jogo";
            case BindAction::Fullscreen:  return "Alternar Tela Cheia";
            default:                      return "Desconhecido";
        }
    }
    if (eff == UiLanguage::Spanish) {
        switch (action) {
            case BindAction::Up:          return "Cruceta Arriba";
            case BindAction::Down:        return "Cruceta Abajo";
            case BindAction::Left:        return "Cruceta Izquierda";
            case BindAction::Right:       return "Cruceta Derecha";
            case BindAction::A:           return "Boton A (Saltar)";
            case BindAction::B:           return "Boton B (Atacar)";
            case BindAction::L:           return "Boton L (Retroceso)";
            case BindAction::R:           return "Boton R (Alma Guardian)";
            case BindAction::Start:       return "Start (Menu)";
            case BindAction::Select:      return "Select (Mapa)";
            case BindAction::MenuToggle:  return "Menu de Configuracion (ImGui)";
            case BindAction::FastForward: return "Avance Rapido (Mantener)";
            case BindAction::QuickSave:   return "Guardado Rapido";
            case BindAction::QuickLoad:   return "Carga Rapida";
            case BindAction::Reset:       return "Reiniciar Juego";
            case BindAction::Fullscreen:  return "Pantalla Completa";
            default:                      return "Desconocido";
        }
    }
    switch (action) {
        case BindAction::Up:          return "D-Pad Up";
        case BindAction::Down:        return "D-Pad Down";
        case BindAction::Left:        return "D-Pad Left";
        case BindAction::Right:       return "D-Pad Right";
        case BindAction::A:           return "Button A (Jump)";
        case BindAction::B:           return "Button B (Attack)";
        case BindAction::L:           return "Button L (Backdash)";
        case BindAction::R:           return "Button R (Guardian Soul)";
        case BindAction::Start:       return "Start (Menu)";
        case BindAction::Select:      return "Select (Map)";
        case BindAction::MenuToggle:  return "Config Menu (ImGui)";
        case BindAction::FastForward: return "Fast Forward (Hold)";
        case BindAction::QuickSave:   return "Quick Save State";
        case BindAction::QuickLoad:   return "Quick Load State";
        case BindAction::Reset:       return "Reset Game";
        case BindAction::Fullscreen:  return "Toggle Fullscreen";
        default:                      return "Unknown";
    }
}

std::string GetVkName(int vk) {
    if (vk <= 0) return "---";
    switch (vk) {
        case VK_UP: return "Up Arrow";
        case VK_DOWN: return "Down Arrow";
        case VK_LEFT: return "Left Arrow";
        case VK_RIGHT: return "Right Arrow";
        case VK_RETURN: return "Enter";
        case VK_BACK: return "Backspace";
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_SHIFT: case VK_LSHIFT: return "LShift";
        case VK_RSHIFT: return "RShift";
        case VK_CONTROL: case VK_LCONTROL: return "LCtrl";
        case VK_RCONTROL: return "RCtrl";
        case VK_MENU: case VK_LMENU: return "LAlt";
        case VK_RMENU: return "RAlt";
        case VK_ESCAPE: return "Esc";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    UINT sc = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    char buf[64] = {};
    if (GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, sizeof(buf)) > 0) {
        return std::string(buf);
    }
    char hex[16];
    snprintf(hex, sizeof(hex), "VK_0x%02X", vk);
    return hex;
}

const char* GetPadButtonName(int padBtn, UiLanguage lang) {
    UiLanguage eff = ResolveEffectiveLanguage(lang);
    bool pt = (eff == UiLanguage::Portuguese);
    bool es = (eff == UiLanguage::Spanish);
    switch (static_cast<PadButton>(padBtn)) {
        case PadButton::None:        return "---";
        case PadButton::DpadUp:      return "D-Pad Up";
        case PadButton::DpadDown:    return "D-Pad Down";
        case PadButton::DpadLeft:    return "D-Pad Left";
        case PadButton::DpadRight:   return "D-Pad Right";
        case PadButton::Start:       return "Start";
        case PadButton::Back:        return "Back / Select";
        case PadButton::LeftThumb:   return pt ? "L3 (Stick Esq.)" : (es ? "L3 (Stick Izq.)" : "L3 (Left Stick)");
        case PadButton::RightThumb:  return pt ? "R3 (Stick Dir.)" : (es ? "R3 (Stick Der.)" : "R3 (Right Stick)");
        case PadButton::LB:          return "LB / L1";
        case PadButton::RB:          return "RB / R1";
        case PadButton::A:           return pt ? "A / Cruz" : (es ? "A / Cruz" : "A / Cross");
        case PadButton::B:           return pt ? "B / Circulo" : (es ? "B / Circulo" : "B / Circle");
        case PadButton::X:           return pt ? "X / Quadrado" : (es ? "X / Cuadrado" : "X / Square");
        case PadButton::Y:           return pt ? "Y / Triangulo" : (es ? "Y / Triangulo" : "Y / Triangle");
        case PadButton::LT:          return "LT / L2";
        case PadButton::RT:          return "RT / R2";
        case PadButton::LStickUp:    return pt ? "Analogico Cima" : (es ? "Stick Arriba" : "Left Stick Up");
        case PadButton::LStickDown:  return pt ? "Analogico Baixo" : (es ? "Stick Abajo" : "Left Stick Down");
        case PadButton::LStickLeft:  return pt ? "Analogico Esq." : (es ? "Stick Izq." : "Left Stick Left");
        case PadButton::LStickRight: return pt ? "Analogico Dir." : (es ? "Stick Der." : "Left Stick Right");
        default:                     return "---";
    }
}

} // namespace gba
