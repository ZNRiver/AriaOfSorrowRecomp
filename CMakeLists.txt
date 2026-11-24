cmake_minimum_required(VERSION 3.20)
project(recomp-ariaofsorrow LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
endif()

# 1. Precompiled standalone Windows x86_64 Runtime Stub (GBA HW + DX11 + Dear ImGui + Native AOT Loader)
add_executable(recomp_stub
    runtime/aot_loader.cpp
    runtime/config.cpp
    runtime/gba_ppu.cpp
    runtime/gba_apu.cpp
    runtime/gba_bus.cpp
    runtime/gba_bios.cpp
    runtime/gba_cpu.cpp
    runtime/imgui_menu.cpp
    runtime/win32_dx11_app.cpp
    runtime/runtime_main.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(recomp_stub PRIVATE
    "${CMAKE_SOURCE_DIR}/runtime"
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
)
target_compile_definitions(recomp_stub PRIVATE IMGUI_DISABLE_SSE)
if(WIN32)
    target_link_libraries(recomp_stub PRIVATE d3d11 dxgi winmm user32 gdi32 shell32 ole32)
    target_link_options(recomp_stub PRIVATE "-Wl,/STACK:8388608")
endif()

# 2. Self-Contained Static Recompiler CLI: recomp-ariaofsorrow.exe
#    Built-in ARM/Thumb -> x86_64 machine-code compiler + embedded recomp_stub.exe (ZERO external tools required!)
add_executable(recomp-ariaofsorrow
    recompiler/main.cpp
    recompiler/rom_analyzer.cpp
    recompiler/arm_thumb_to_x64.cpp
    recompiler/arm_thumb_codegen.cpp
)
add_dependencies(recomp-ariaofsorrow recomp_stub)
target_include_directories(recomp-ariaofsorrow PRIVATE
    "${CMAKE_SOURCE_DIR}/recompiler"
    "${CMAKE_SOURCE_DIR}/runtime"
    "${CMAKE_BINARY_DIR}"
)
target_compile_options(recomp-ariaofsorrow PRIVATE -Wno-c23-extensions)
if(WIN32)
    target_link_options(recomp-ariaofsorrow PRIVATE "-Wl,/entry:wmainCRTStartup")
endif()
