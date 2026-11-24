cmake_minimum_required(VERSION 3.20)
project(recomp-ariaofsorrow LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# 1. WebAssembly (Emscripten) Target — compiles the RECOMPILED GAME to .wasm/.js/.html
# ==============================================================================
if(EMSCRIPTEN)
    set(RECOMP_WASM_GEN_DIR "" CACHE PATH "Directory containing generated recomp_*.cpp and rom_data.cpp")
    if(NOT RECOMP_WASM_GEN_DIR)
        message(FATAL_ERROR "Please set -DRECOMP_WASM_GEN_DIR=<path_to_generated_cpp_dir>")
    endif()

    file(GLOB RECOMP_WASM_GEN_SRCS "${RECOMP_WASM_GEN_DIR}/*.cpp")

    add_executable(AriaOfSorrow_Recomp
        ${RECOMP_WASM_GEN_SRCS}
        runtime/config.cpp
        runtime/gba_ppu.cpp
        runtime/gba_apu.cpp
        runtime/gba_bus.cpp
        runtime/gba_bios.cpp
        runtime/gba_cpu.cpp
        runtime/imgui_menu.cpp
        runtime/web_sdl_app.cpp
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl2.cpp
        third_party/imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(AriaOfSorrow_Recomp PRIVATE
        "${CMAKE_SOURCE_DIR}/runtime"
        "${CMAKE_SOURCE_DIR}/third_party/imgui"
        "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
        "${RECOMP_WASM_GEN_DIR}"
    )
    target_compile_definitions(AriaOfSorrow_Recomp PRIVATE IMGUI_DISABLE_SSE)
    target_compile_options(AriaOfSorrow_Recomp PRIVATE
        -sUSE_SDL=2
        -Wno-unused-label
        -Wno-c23-extensions
    )
    set_target_properties(AriaOfSorrow_Recomp PROPERTIES SUFFIX ".html")
    target_link_options(AriaOfSorrow_Recomp PRIVATE
        -sUSE_SDL=2
        -sFULL_ES3=1
        -sMAX_WEBGL_VERSION=2
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        -sSTACK_SIZE=2097152
        "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
    )
    return()
endif()

# ==============================================================================
# 2. Windows x86_64 Targets — recomp_stub.exe + self-contained recomp-ariaofsorrow.exe
# ==============================================================================
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)

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
    target_link_libraries(recomp_stub PRIVATE d3d11 dxgi winmm user32 gdi32 shell32 ole32)
    target_link_options(recomp_stub PRIVATE "-Wl,/STACK:8388608")

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
    target_compile_definitions(recomp-ariaofsorrow PRIVATE
        RECOMP_DEFAULT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_compile_options(recomp-ariaofsorrow PRIVATE -Wno-c23-extensions)
    target_link_options(recomp-ariaofsorrow PRIVATE "-Wl,/entry:wmainCRTStartup")
    return()
endif()

# ==============================================================================
# 3. Linux x86_64 Targets — recomp_stub + self-contained recomp-ariaofsorrow ELF
#    (Also supports -DRECOMP_LINUX_GEN_DIR=<dir> to build a C++ cross-generated Linux game)
# ==============================================================================
if(UNIX AND NOT APPLE)
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)

    set(RECOMP_LINUX_GEN_DIR "" CACHE PATH "Optional directory containing generated recomp_*.cpp for C++ Linux game build")
    if(RECOMP_LINUX_GEN_DIR)
        file(GLOB RECOMP_LINUX_GEN_SRCS "${RECOMP_LINUX_GEN_DIR}/*.cpp")
        add_executable(AriaOfSorrow_Recomp
            ${RECOMP_LINUX_GEN_SRCS}
            runtime/config.cpp
            runtime/gba_ppu.cpp
            runtime/gba_apu.cpp
            runtime/gba_bus.cpp
            runtime/gba_bios.cpp
            runtime/gba_cpu.cpp
            runtime/imgui_menu.cpp
            runtime/web_sdl_app.cpp
            third_party/imgui/imgui.cpp
            third_party/imgui/imgui_draw.cpp
            third_party/imgui/imgui_tables.cpp
            third_party/imgui/imgui_widgets.cpp
            third_party/imgui/backends/imgui_impl_sdl2.cpp
            third_party/imgui/backends/imgui_impl_opengl3.cpp
        )
        target_include_directories(AriaOfSorrow_Recomp PRIVATE
            "${CMAKE_SOURCE_DIR}/runtime"
            "${CMAKE_SOURCE_DIR}/third_party/imgui"
            "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
            "${RECOMP_LINUX_GEN_DIR}"
        )
        target_compile_definitions(AriaOfSorrow_Recomp PRIVATE IMGUI_DISABLE_SSE RECOMP_STATIC_CPP)
        target_compile_options(AriaOfSorrow_Recomp PRIVATE -Wno-unused-label -Wno-c23-extensions)
        target_link_libraries(AriaOfSorrow_Recomp PRIVATE SDL2::SDL2 OpenGL::GL Threads::Threads dl)
        return()
    endif()

    # Self-contained Linux Runtime Stub (SDL2 + OpenGL 3.3 + Dear ImGui + Native SysV64 AOT Loader)
    add_executable(recomp_stub
        runtime/aot_loader.cpp
        runtime/config.cpp
        runtime/gba_ppu.cpp
        runtime/gba_apu.cpp
        runtime/gba_bus.cpp
        runtime/gba_bios.cpp
        runtime/gba_cpu.cpp
        runtime/imgui_menu.cpp
        runtime/web_sdl_app.cpp
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl2.cpp
        third_party/imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(recomp_stub PRIVATE
        "${CMAKE_SOURCE_DIR}/runtime"
        "${CMAKE_SOURCE_DIR}/third_party/imgui"
        "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
    )
    target_compile_definitions(recomp_stub PRIVATE IMGUI_DISABLE_SSE)
    target_link_libraries(recomp_stub PRIVATE SDL2::SDL2 OpenGL::GL Threads::Threads dl)

    # Self-contained Linux Static Recompiler CLI: recomp-ariaofsorrow
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
    target_compile_definitions(recomp-ariaofsorrow PRIVATE
        RECOMP_DEFAULT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_compile_options(recomp-ariaofsorrow PRIVATE -Wno-c23-extensions)
    target_link_libraries(recomp-ariaofsorrow PRIVATE Threads::Threads)
endif()
