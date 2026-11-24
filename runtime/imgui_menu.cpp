#include "imgui_menu.h"
#include "imgui.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
#include <SDL2/SDL.h>
#endif

#include <cstdio>

namespace gba {

namespace {

struct UiStrings {
    const char* windowTitlePrefix;
    const char* tabControls;
    const char* tabVideo;
    const char* tabAudio;
    const char* tabSystem;
    const char* padConnected;
    const char* padDisconnected;
    const char* bindHint;
    const char* useLStickAsDpad;
    const char* colAction;
    const char* colKeyboard;
    const char* colGamepad;
    const char* pressKey;
    const char* pressPad;
    const char* resetKeyboard;
    const char* resetGamepad;
    const char* aspectLabel;
    const char* aspectFit;
    const char* aspectInt;
    const char* aspectStretch;
    const char* linearFilter;
    const char* vsync;
    const char* showOverlay;
    const char* enterFullscreen;
    const char* exitFullscreen;
    const char* muteAudio;
    const char* masterVolume;
    const char* languageLabel;
    const char* pauseOnMenu;
    const char* ffSpeed;
    const char* enableRecomp;
    const char* saveStatesHeader;
    const char* quickSave;
    const char* quickLoad;
    const char* resetGame;
    const char* romInfoHeader;
    const char* romTitleFmt;
    const char* romFileFmt;
    const char* romSizeFmt;
    const char* recompFuncsFmt;
    const char* recompInsnsFmt;
    const char* nativeCallsFmt;
    const char* fallbackInsnsFmt;
    const char* saveAndClose;
    const char* exitGame;
};

const UiStrings& GetStrings(UiLanguage lang) {
    static const UiStrings kEn = {
        "Aria of Sorrow Recomp - Settings",
        "Controls / Binds", "Video", "Audio", "System & Recomp",
        "Controller (XInput): Connected",
        "Controller (XInput): Not detected",
        "  |  ESC: cancel  |  DEL: clear bind",
        "Also use Left Analog Stick as D-Pad",
        "Action", "Keyboard", "Controller (Gamepad)",
        "Press a key...", "Press button...",
        "Reset Default Keyboard", "Reset Default Controller",
        "Aspect Ratio", "Fit (3:2)", "Integer Scale (Pixel Perfect)", "Stretch",
        "Linear Filtering (Smooth)",
        "VSync",
        "Show FPS & Recompiler Stats Overlay",
        "Fullscreen (F11)", "Exit Fullscreen (F11)",
        "Mute Audio", "Master Volume (%)",
        "UI Language",
        "Pause game while menu is open",
        "Fast-Forward Speed",
        "Use Static Recompiled Code (Native x86_64)",
        "Save States & Game:",
        "Quick Save (F5)", "Quick Load (F7)", "Reset Game (F8)",
        "Recompiled ROM Information:",
        "ROM Title: %s (%s / Maker %s)",
        "Source File: %s",
        "ROM Size: %.2f MB",
        "Recompiled Functions (x86_64): %u",
        "Translated ARM/Thumb Instructions: %u",
        "Native Recomp Calls: %llu",
        "Fallback Instructions (IWRAM/Dynamic): %llu",
        "Save & Close", "Exit Game"
    };

    static const UiStrings kPt = {
        "Aria of Sorrow Recomp - Configuracoes",
        "Controles / Binds", "Video", "Audio", "Sistema & Recomp",
        "Controle (XInput): Conectado",
        "Controle (XInput): Nenhum controle detectado",
        "  |  ESC: cancelar  |  DEL: limpar bind",
        "Usar Analogico Esquerdo tambem como D-Pad",
        "Acao", "Teclado", "Controle (Gamepad)",
        "Pressione tecla...", "Aperte no controle...",
        "Restaurar Teclado Padrao", "Restaurar Controle Padrao",
        "Proporcao da Tela", "Ajustar (3:2 Fit)", "Escala Inteira (Pixel Perfect)", "Esticar (Stretch)",
        "Filtro Linear (Suavizacao)",
        "VSync",
        "Mostrar FPS e Estatisticas do Recomp na Tela",
        "Tela Cheia (F11)", "Sair de Tela Cheia (F11)",
        "Mudo (Mute)", "Volume Geral (%)",
        "Idioma da Interface",
        "Pausar jogo com o menu aberto",
        "Velocidade Fast-Forward",
        "Usar Codigo Estatico Recompilado (x86_64 Nativo)",
        "Save States & Jogo:",
        "Quick Save (F5)", "Quick Load (F7)", "Resetar Jogo (F8)",
        "Informacoes da ROM Recompilada:",
        "Titulo ROM: %s (%s / Maker %s)",
        "Arquivo Origem: %s",
        "Tamanho ROM: %.2f MB",
        "Funcoes Recompiladas (x86_64): %u",
        "Instrucoes ARM/Thumb Traduzidas: %u",
        "Chamadas Nativas Recomp: %llu",
        "Instrucoes Fallback (IWRAM/Dinamico): %llu",
        "Salvar & Fechar", "Sair do Jogo"
    };

    static const UiStrings kEs = {
        "Aria of Sorrow Recomp - Configuracion",
        "Controles / Binds", "Video", "Audio", "Sistema y Recomp",
        "Mando (XInput): Conectado",
        "Mando (XInput): No detectado",
        "  |  ESC: cancelar  |  DEL: borrar",
        "Usar Stick Izquierdo tambien como Cruceta",
        "Accion", "Teclado", "Mando (Gamepad)",
        "Pulsa una tecla...", "Pulsa en el mando...",
        "Restaurar Teclado", "Restaurar Mando",
        "Relacion de Aspecto", "Ajustar (3:2)", "Escala Entera (Pixel Perfect)", "Estirar",
        "Filtro Lineal (Suavizado)",
        "VSync",
        "Mostrar FPS y Estadisticas del Recomp",
        "Pantalla Completa (F11)", "Salir de Pantalla Completa (F11)",
        "Silenciar Audio", "Volumen General (%)",
        "Idioma de Interfaz",
        "Pausar juego con el menu abierto",
        "Velocidad de Avance Rapido",
        "Usar Codigo Estatico Recompilado (x86_64 Nativo)",
        "Estados y Juego:",
        "Guardado Rapido (F5)", "Carga Rapida (F7)", "Reiniciar Juego (F8)",
        "Informacion de la ROM Recompilada:",
        "Titulo ROM: %s (%s / Maker %s)",
        "Archivo Origen: %s",
        "Tamano ROM: %.2f MB",
        "Funciones Recompiladas (x86_64): %u",
        "Instrucciones ARM/Thumb Traducidas: %u",
        "Llamadas Nativas Recomp: %llu",
        "Instrucciones Fallback (IWRAM/Dinamico): %llu",
        "Guardar y Cerrar", "Salir del Juego"
    };

    UiLanguage eff = ResolveEffectiveLanguage(lang);
    if (eff == UiLanguage::Portuguese) return kPt;
    if (eff == UiLanguage::Spanish)    return kEs;
    return kEn;
}

} // namespace

void ImGuiMenu::Toggle() {
    isOpen = !isOpen;
    waitingForKeyIdx = -1;
    waitingForPadIdx = -1;
}

bool ImGuiMenu::HandleKeyForRebind(int vk) {
    if (!IsCapturingBind()) return false;
#ifdef _WIN32
    if (vk == VK_ESCAPE) {
        waitingForKeyIdx = -1;
        waitingForPadIdx = -1;
        return true;
    }
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
    if (vk == SDL_SCANCODE_ESCAPE) {
        waitingForKeyIdx = -1;
        waitingForPadIdx = -1;
        return true;
    }
#endif
    return false;
}

MenuActions ImGuiMenu::Render(AppConfig& cfg, const RecompRomInfo& romInfo, const RuntimeStats& stats) {
    MenuActions actions{};
    if (!isOpen) return actions;

    UiLanguage effLang = ResolveEffectiveLanguage(cfg.uiLanguage);
    const UiStrings& tr = GetStrings(effLang);

    // Handle active keyboard key capture
    if (waitingForKeyIdx >= 0) {
#ifdef _WIN32
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
            if (GetAsyncKeyState(vk) & 0x8000) {
                if (vk == VK_DELETE) {
                    cfg.keyBinds[waitingForKeyIdx] = 0;
                    actions.requestSaveConfig = true;
                } else if (vk != VK_ESCAPE) {
                    cfg.keyBinds[waitingForKeyIdx] = vk;
                    actions.requestSaveConfig = true;
                }
                waitingForKeyIdx = -1;
                break;
            }
        }
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
        int numKeys = 0;
        const Uint8* ks = SDL_GetKeyboardState(&numKeys);
        for (int sc = 4; sc < numKeys; ++sc) {
            if (ks[sc]) {
                if (sc == SDL_SCANCODE_DELETE) {
                    cfg.keyBinds[waitingForKeyIdx] = 0;
                    actions.requestSaveConfig = true;
                } else if (sc != SDL_SCANCODE_ESCAPE) {
                    cfg.keyBinds[waitingForKeyIdx] = sc;
                    actions.requestSaveConfig = true;
                }
                waitingForKeyIdx = -1;
                break;
            }
        }
#endif
    }

    // Handle active gamepad button capture
    if (waitingForPadIdx >= 0) {
#ifdef _WIN32
        bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        bool delDown = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
        const Uint8* ks = SDL_GetKeyboardState(nullptr);
        bool escDown = ks[SDL_SCANCODE_ESCAPE] != 0;
        bool delDown = ks[SDL_SCANCODE_DELETE] != 0;
#else
        bool escDown = false, delDown = false;
#endif
        if (escDown) {
            waitingForPadIdx = -1;
        } else if (delDown) {
            cfg.padBinds[waitingForPadIdx] = static_cast<int>(PadButton::None);
            actions.requestSaveConfig = true;
            waitingForPadIdx = -1;
        } else if (stats.activePadButton != static_cast<int>(PadButton::None)) {
            cfg.padBinds[waitingForPadIdx] = stats.activePadButton;
            actions.requestSaveConfig = true;
            waitingForPadIdx = -1;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 540), ImGuiCond_FirstUseEver);

    std::string title = std::string(tr.windowTitlePrefix) + " (" +
                        GetVkName(cfg.keyBinds[static_cast<int>(BindAction::MenuToggle)]) +
                        ")###ConfigWindow";

    if (ImGui::Begin(title.c_str(), &isOpen, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("ConfigTabs")) {
            // ======================== CONTROLS / BINDS ========================
            if (ImGui::BeginTabItem(tr.tabControls)) {
                if (stats.gamepadConnected) {
                    ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "%s", tr.padConnected);
                } else {
                    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.0f), "%s", tr.padDisconnected);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr.bindHint);

                if (ImGui::Checkbox(tr.useLStickAsDpad, &cfg.padLeftStickAsDpad)) {
                    actions.requestSaveConfig = true;
                }
                ImGui::Spacing();

                if (ImGui::BeginTable("BindsTable", 3,
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                      ImVec2(0, 330))) {
                    ImGui::TableSetupColumn(tr.colAction, ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn(tr.colKeyboard, ImGuiTableColumnFlags_WidthFixed, 170.0f);
                    ImGui::TableSetupColumn(tr.colGamepad, ImGuiTableColumnFlags_WidthFixed, 190.0f);
                    ImGui::TableHeadersRow();

                    for (int i = 0; i < static_cast<int>(BindAction::Count); ++i) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%s", GetBindActionName(static_cast<BindAction>(i), effLang));

                        ImGui::TableSetColumnIndex(1);
                        char keyLabel[128];
                        if (waitingForKeyIdx == i) {
                            snprintf(keyLabel, sizeof(keyLabel), "%s##k%d", tr.pressKey, i);
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.20f, 1.0f));
                        } else {
                            snprintf(keyLabel, sizeof(keyLabel), "%s##k%d", GetVkName(cfg.keyBinds[i]).c_str(), i);
                        }
                        if (ImGui::Button(keyLabel, ImVec2(-1.0f, 0.0f))) {
                            waitingForKeyIdx = i;
                            waitingForPadIdx = -1;
                        }
                        if (waitingForKeyIdx == i) ImGui::PopStyleColor();

                        ImGui::TableSetColumnIndex(2);
                        char padLabel[128];
                        if (waitingForPadIdx == i) {
                            snprintf(padLabel, sizeof(padLabel), "%s##p%d", tr.pressPad, i);
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.78f, 1.0f));
                        } else {
                            snprintf(padLabel, sizeof(padLabel), "%s##p%d", GetPadButtonName(cfg.padBinds[i], effLang), i);
                        }
                        if (ImGui::Button(padLabel, ImVec2(-1.0f, 0.0f))) {
                            waitingForPadIdx = i;
                            waitingForKeyIdx = -1;
                        }
                        if (waitingForPadIdx == i) ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }

                ImGui::Spacing();
                if (ImGui::Button(tr.resetKeyboard)) {
                    cfg.ResetDefaultBinds();
                    actions.requestSaveConfig = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr.resetGamepad)) {
                    cfg.ResetDefaultPadBinds();
                    actions.requestSaveConfig = true;
                }
                ImGui::EndTabItem();
            }

            // ======================== VIDEO ========================
            if (ImGui::BeginTabItem(tr.tabVideo)) {
                const char* aspectNames[] = { tr.aspectFit, tr.aspectInt, tr.aspectStretch };
                int aspectIdx = static_cast<int>(cfg.aspectMode);
                if (ImGui::Combo(tr.aspectLabel, &aspectIdx, aspectNames, 3)) {
                    cfg.aspectMode = static_cast<AspectMode>(aspectIdx);
                    actions.requestSaveConfig = true;
                }

                if (ImGui::Checkbox(tr.linearFilter, &cfg.linearFilter)) {
                    actions.requestSaveConfig = true;
                }
                if (ImGui::Checkbox(tr.vsync, &cfg.vsync)) {
                    actions.requestSaveConfig = true;
                }
                if (ImGui::Checkbox(tr.showOverlay, &cfg.showOverlayStats)) {
                    actions.requestSaveConfig = true;
                }
                ImGui::Spacing();
                if (ImGui::Button(cfg.fullscreen ? tr.exitFullscreen : tr.enterFullscreen)) {
                    actions.requestToggleFullscreen = true;
                }
                ImGui::EndTabItem();
            }

            // ======================== AUDIO ========================
            if (ImGui::BeginTabItem(tr.tabAudio)) {
                if (ImGui::Checkbox(tr.muteAudio, &cfg.muteAudio)) {
                    actions.requestSaveConfig = true;
                }
                int volPct = static_cast<int>(cfg.masterVolume * 100.0f + 0.5f);
                if (ImGui::SliderInt(tr.masterVolume, &volPct, 0, 100)) {
                    cfg.masterVolume = static_cast<float>(volPct) / 100.0f;
                    actions.requestSaveConfig = true;
                }
                ImGui::EndTabItem();
            }

            // ======================== SYSTEM & RECOMP ========================
            if (ImGui::BeginTabItem(tr.tabSystem)) {
                const char* langNames[] = { "Auto (Windows OS)", "English", "Portugues (BR)", "Espanol" };
                int langIdx = static_cast<int>(cfg.uiLanguage);
                if (ImGui::Combo(tr.languageLabel, &langIdx, langNames, 4)) {
                    cfg.uiLanguage = static_cast<UiLanguage>(langIdx);
                    actions.requestSaveConfig = true;
                }

                if (ImGui::Checkbox(tr.pauseOnMenu, &cfg.pauseOnMenu)) {
                    actions.requestSaveConfig = true;
                }
                if (ImGui::SliderInt(tr.ffSpeed, &cfg.fastForwardSpeed, 2, 8, "%dx")) {
                    actions.requestSaveConfig = true;
                }
                if (ImGui::Checkbox(tr.enableRecomp, &cfg.enableRecomp)) {
                    actions.requestSaveConfig = true;
                }

                ImGui::Separator();
                ImGui::Text("%s", tr.saveStatesHeader);
                if (ImGui::Button(tr.quickSave)) actions.requestQuickSave = true;
                ImGui::SameLine();
                if (!stats.hasQuickSave) ImGui::BeginDisabled();
                if (ImGui::Button(tr.quickLoad)) actions.requestQuickLoad = true;
                if (!stats.hasQuickSave) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(tr.resetGame)) actions.requestResetGame = true;

                ImGui::Separator();
                ImGui::Text("%s", tr.romInfoHeader);
                ImGui::BulletText(tr.romTitleFmt, romInfo.title, romInfo.gameCode, romInfo.makerCode);
                ImGui::BulletText(tr.romFileFmt, romInfo.romFilename);
                ImGui::BulletText(tr.romSizeFmt, romInfo.romSize / (1024.0 * 1024.0));
                ImGui::BulletText(tr.recompFuncsFmt, romInfo.recompiledFunctionCount);
                ImGui::BulletText(tr.recompInsnsFmt, romInfo.recompiledInstructionCount);
                ImGui::BulletText(tr.nativeCallsFmt, static_cast<unsigned long long>(stats.recompCalls));
                ImGui::BulletText(tr.fallbackInsnsFmt, static_cast<unsigned long long>(stats.fallbackInstrs));

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button(tr.saveAndClose, ImVec2(145, 0))) {
            actions.requestSaveConfig = true;
            actions.requestCloseMenu = true;
            isOpen = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr.exitGame, ImVec2(130, 0))) {
            actions.requestSaveConfig = true;
            actions.requestExitApp = true;
        }
    }
    ImGui::End();

    return actions;
}

void ImGuiMenu::RenderOverlayStats(const AppConfig& cfg, const RecompRomInfo& romInfo, const RuntimeStats& stats) {
    if (cfg.showOverlayStats) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##StatsOverlay", nullptr, flags)) {
            ImGui::Text("FPS: %.1f | Recomp: %s (%u funcs)",
                        stats.fps,
                        cfg.enableRecomp ? "ON" : "OFF",
                        romInfo.recompiledFunctionCount);
            ImGui::Text("Native calls: %llu | Menu: %s",
                        static_cast<unsigned long long>(stats.recompCalls),
                        GetVkName(cfg.keyBinds[static_cast<int>(BindAction::MenuToggle)]).c_str());
        }
        ImGui::End();
    }

    if (stats.toastTimer > 0.0f && !stats.statusToast.empty()) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##ToastOverlay", nullptr, flags)) {
            ImGui::Text("%s", stats.statusToast.c_str());
        }
        ImGui::End();
    }
}

} // namespace gba
