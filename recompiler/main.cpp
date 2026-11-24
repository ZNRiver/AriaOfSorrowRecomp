#include "rom_analyzer.h"
#include "arm_thumb_to_x64.h"
#include "arm_thumb_codegen.h"
#include "recomp_abi.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Embedded native host runtime stub (DX11 on Windows, SDL2+OpenGL on Linux)
alignas(16) static const uint8_t kEmbeddedRuntimeStub[] = {
#ifdef _WIN32
#embed "recomp_stub.exe"
#else
#embed "recomp_stub"
#endif
};

#ifdef _WIN32
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(sz), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), sz, nullptr, nullptr);
    return out;
}

fs::path FindInPathW(const std::wstring& exeName) {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = SearchPathW(nullptr, exeName.c_str(), nullptr, MAX_PATH, buf, nullptr);
    if (len > 0 && len < MAX_PATH) return fs::path(buf);
    return {};
}

int RunProcessWide(const std::wstring& cmdLine) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}
#endif

void CopyFixedStr(char* dst, size_t dstSize, const std::string& src) {
    std::memset(dst, 0, dstSize);
    if (dstSize == 0) return;
    size_t n = (src.size() < dstSize - 1) ? src.size() : (dstSize - 1);
    std::memcpy(dst, src.data(), n);
}

void PrintUsage() {
    std::cout << "====================================================================\n";
    std::cout << "  recomp-ariaofsorrow - Self-Contained GBA Static Recompiler\n";
    std::cout << "====================================================================\n";
    std::cout << "Usage:\n";
    std::cout << "  recomp-ariaofsorrow <path_to_rom.gba> [options]\n\n";
    std::cout << "Options:\n";
#ifdef _WIN32
    std::cout << "  --target <win64|linux|wasm>  Target platform (default: win64)\n";
#else
    std::cout << "  --target <linux|win64|wasm>  Target platform (default: linux)\n";
#endif
    std::cout << "  -o, --output <path>          Output executable or directory\n";
    std::cout << "  --max-funcs <N>              Max statically recompiled functions (default: 4000)\n";
    std::cout << "  --emit-cpp <dir>             (Optional) Also export readable C++ files\n";
    std::cout << "  -h, --help                   Show this help message\n\n";
    std::cout << "Native host target uses built-in x86_64 codegen — zero external compiler needed!\n";
}

int RunRecompiler(
    const fs::path& romPath,
    const std::string& origFilename,
    fs::path outputPath,
    const fs::path& emitCppDir,
    const std::string& target,
    uint32_t maxFuncs
) {
    bool isWin64 = (target == "win64" || target == "windows");
    bool isLinux = (target == "linux");
    bool isWasm  = (target == "wasm" || target == "web" || target == "webassembly");

    if (!isWin64 && !isLinux && !isWasm) {
        std::cerr << "[ERROR] Unknown target: " << target << " (expected 'win64', 'linux', or 'wasm').\n";
        return 1;
    }

    if (!fs::exists(romPath)) {
        std::cerr << "[ERROR] ROM file not found: " << origFilename << "\n";
        return 1;
    }

    auto tStart = std::chrono::steady_clock::now();

    if (outputPath.empty()) {
        if (isWasm) {
            outputPath = fs::current_path() / "wasm_game";
        } else if (isWin64) {
#ifdef _WIN32
            outputPath = fs::current_path() / (romPath.stem().wstring() + L".exe");
#else
            outputPath = fs::current_path() / "win64_game";
#endif
        } else { // linux
#ifdef _WIN32
            outputPath = fs::current_path() / "linux_game";
#else
            outputPath = fs::current_path() / (romPath.stem().string() + "_recomp");
#endif
        }
    }

    std::cout << "====================================================================\n";
    std::cout << "  recomp-ariaofsorrow - Self-Contained GBA Static Recompiler\n";
    std::cout << "====================================================================\n";
    std::cout << "[1/3] Reading GBA ROM: " << origFilename << "\n";

    std::vector<uint8_t> romBytes;
    {
        std::ifstream in(romPath, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            std::cerr << "[ERROR] Could not open ROM file.\n";
            return 1;
        }
        std::streamsize sz = in.tellg();
        if (sz < 0xC0) {
            std::cerr << "[ERROR] File is too small to be a valid GBA ROM.\n";
            return 1;
        }
        in.seekg(0, std::ios::beg);
        romBytes.resize(static_cast<size_t>(sz));
        if (!in.read(reinterpret_cast<char*>(romBytes.data()), sz)) {
            std::cerr << "[ERROR] Failed to read ROM bytes.\n";
            return 1;
        }
    }

    fs::path tempRom = fs::temp_directory_path() / "aos_recomp_input.gba";
    {
        std::ofstream tmpOut(tempRom, std::ios::binary);
        tmpOut.write(reinterpret_cast<const char*>(romBytes.data()), static_cast<std::streamsize>(romBytes.size()));
    }

    recomp::RomAnalyzer analyzer;
    if (!analyzer.LoadRom(tempRom.string())) {
        std::error_code ec;
        fs::remove(tempRom, ec);
        std::cerr << "[ERROR] Failed to parse GBA ROM header.\n";
        return 1;
    }

    const auto& hdr = analyzer.Header();
    std::cout << "      Title    : " << hdr.title << "\n";
    std::cout << "      GameCode : " << hdr.gameCode << " (Maker: " << hdr.makerCode << ")\n";
    std::cout << "      Size     : " << (hdr.romSize / (1024 * 1024)) << " MB (" << hdr.romSize << " bytes)\n";
    std::cout << "      Header OK: " << (hdr.validLogo ? "Yes (0x96)" : "Warning: byte 0xB2 != 0x96") << "\n";

    std::cout << "[2/3] Analyzing ARM/Thumb control flow (max " << maxFuncs << " funcs)...\n";
    auto funcs = analyzer.DiscoverFunctions(maxFuncs);
    uint32_t armCount = 0, thumbCount = 0;
    for (const auto& f : funcs) {
        if (f.isThumb) thumbCount++; else armCount++;
    }

    fs::path srcRoot = fs::current_path();
#ifdef RECOMP_DEFAULT_SOURCE_DIR
    if (fs::exists(fs::path(RECOMP_DEFAULT_SOURCE_DIR) / "CMakeLists.txt")) {
        srcRoot = fs::path(RECOMP_DEFAULT_SOURCE_DIR);
    }
#endif

    // ======================== WEBASSEMBLY TARGET ========================
    if (isWasm) {
        fs::path wasmOutDir = outputPath;
        fs::path genDir = wasmOutDir / "generated";
        fs::create_directories(genDir);

        std::cout << "      Discovered " << funcs.size() << " functions ("
                  << armCount << " ARM, " << thumbCount << " Thumb)\n";
        std::cout << "[3/3] Generating WebAssembly C++20 game project in: " << wasmOutDir.string() << "\n";

        recomp::ArmThumbCodegen cppGen;
        auto cgRes = cppGen.GenerateAll(analyzer, funcs, genDir.string(), tempRom.string(), 250);
        { std::error_code ec; fs::remove(tempRom, ec); }

        std::cout << "      Emitted " << cgRes.generatedCppFiles.size() << " C++ files ("
                  << cgRes.totalInstructions << " ARM/Thumb instructions translated)\n";

        fs::path wasmBuildDir = wasmOutDir / "build";
#ifdef _WIN32
        fs::path emcmake = FindInPathW(L"emcmake.bat");
        if (emcmake.empty()) emcmake = FindInPathW(L"emcmake.cmd");
        if (emcmake.empty()) emcmake = FindInPathW(L"emcmake.exe");
        if (!emcmake.empty()) {
            std::cout << "      Emscripten detected! Compiling game to WebAssembly (.html/.js/.wasm)...\n";
            std::wstring cfgCmd = L"cmd /c \"\"" + emcmake.wstring() +
                                  L"\" cmake -G Ninja -S \"" + srcRoot.wstring() +
                                  L"\" -B \"" + wasmBuildDir.wstring() +
                                  L"\" -DCMAKE_BUILD_TYPE=Release -DRECOMP_WASM_GEN_DIR=\"" + genDir.wstring() + L"\"\"";
            if (RunProcessWide(cfgCmd) == 0) {
                std::wstring bldCmd = L"cmd /c \"cmake --build \"" + wasmBuildDir.wstring() + L"\" --parallel\"";
                if (RunProcessWide(bldCmd) == 0) {
                    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
                    std::cout << "====================================================================\n";
                    std::cout << "  WEBASSEMBLY GAME BUILD SUCCEEDED! (" << secs << "s)\n";
                    std::cout << "  Output: " << (wasmBuildDir / "AriaOfSorrow_Recomp.html").string() << "\n";
                    std::cout << "====================================================================\n";
                    return 0;
                }
            }
            std::cerr << "[ERROR] Emscripten build failed.\n";
            return 1;
        }
#else
        if (std::system("command -v emcmake >/dev/null 2>&1") == 0) {
            std::cout << "      Emscripten detected! Compiling game to WebAssembly (.html/.js/.wasm)...\n";
            std::string cfgCmd = "emcmake cmake -G Ninja -S \"" + srcRoot.string() +
                                 "\" -B \"" + wasmBuildDir.string() +
                                 "\" -DCMAKE_BUILD_TYPE=Release -DRECOMP_WASM_GEN_DIR=\"" + genDir.string() + "\"";
            if (std::system(cfgCmd.c_str()) == 0) {
                std::string bldCmd = "cmake --build \"" + wasmBuildDir.string() + "\" --parallel";
                if (std::system(bldCmd.c_str()) == 0) {
                    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
                    std::cout << "====================================================================\n";
                    std::cout << "  WEBASSEMBLY GAME BUILD SUCCEEDED! (" << secs << "s)\n";
                    std::cout << "  Output: " << (wasmBuildDir / "AriaOfSorrow_Recomp.html").string() << "\n";
                    std::cout << "====================================================================\n";
                    return 0;
                }
            }
            std::cerr << "[ERROR] Emscripten build failed.\n";
            return 1;
        }
#endif
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
        std::cout << "====================================================================\n";
        std::cout << "  WEBASSEMBLY GAME PROJECT GENERATED! (" << secs << "s)\n";
        std::cout << "  To compile to .wasm/.js/.html with Emscripten SDK:\n";
        std::cout << "    emcmake cmake -G Ninja -S \"" << srcRoot.string()
                  << "\" -B \"" << wasmBuildDir.string()
                  << "\" -DCMAKE_BUILD_TYPE=Release -DRECOMP_WASM_GEN_DIR=\"" << genDir.string() << "\"\n";
        std::cout << "    cmake --build \"" << wasmBuildDir.string() << "\" --parallel\n";
        std::cout << "====================================================================\n";
        return 0;
    }

    // ======================== CROSS-TARGET C++ PROJECT EXPORT ========================
#ifdef _WIN32
    bool isNativeHostTarget = isWin64;
    recomp::X64Abi nativeAbi = recomp::X64Abi::Win64;
#else
    bool isNativeHostTarget = isLinux;
    recomp::X64Abi nativeAbi = recomp::X64Abi::SysV64;
#endif

    if (!isNativeHostTarget) {
        fs::path crossOutDir = outputPath;
        fs::path genDir = crossOutDir / "generated";
        fs::create_directories(genDir);

        std::cout << "      Discovered " << funcs.size() << " functions ("
                  << armCount << " ARM, " << thumbCount << " Thumb)\n";
        std::cout << "[3/3] Generating portable " << target << " C++20 game project in: "
                  << crossOutDir.string() << "\n";

        recomp::ArmThumbCodegen cppGen;
        auto cgRes = cppGen.GenerateAll(analyzer, funcs, genDir.string(), tempRom.string(), 250);
        { std::error_code ec; fs::remove(tempRom, ec); }

        std::cout << "      Emitted " << cgRes.generatedCppFiles.size() << " C++ files ("
                  << cgRes.totalInstructions << " ARM/Thumb instructions translated)\n";

        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
        std::cout << "====================================================================\n";
        std::cout << "  " << (isLinux ? "LINUX" : "WIN64") << " C++20 GAME PROJECT GENERATED! (" << secs << "s)\n";
        std::cout << "  Generated C++ dir: " << genDir.string() << "\n";
        if (isLinux) {
            std::cout << "  Build on Linux (or WSL) with:\n";
            std::cout << "    cmake -G Ninja -S <repo_dir> -B build-linux-game -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
                      << " -DCMAKE_BUILD_TYPE=Release -DRECOMP_LINUX_GEN_DIR=\"" << genDir.string() << "\"\n";
            std::cout << "    cmake --build build-linux-game --parallel\n";
            std::cout << "  Or run the native Linux 'recomp-ariaofsorrow' binary directly for instant 0.3s output!\n";
        }
        std::cout << "====================================================================\n";
        return 0;
    }

    // ======================== NATIVE SELF-CONTAINED x86_64 TARGET ========================
    recomp::ArmThumbToX64 nativeCompiler;
    auto compiled = nativeCompiler.CompileAll(analyzer, funcs, nativeAbi);

    std::cout << "      Compiled " << compiled.funcRecords.size() << " functions ("
              << armCount << " ARM, " << thumbCount << " Thumb)\n";
    std::cout << "      Translated " << compiled.totalInstructions << " ARM/Thumb instructions -> "
              << (compiled.x64CodeBlob.size() / 1024.0) << " KB native x86_64 code ("
              << (nativeAbi == recomp::X64Abi::Win64 ? "Win64 ABI" : "Linux SysV64 ABI") << ")\n";

    if (!emitCppDir.empty()) {
        std::cout << "[OPT] Exporting C++ representation to: " << emitCppDir.string() << "\n";
        recomp::ArmThumbCodegen cppGen;
        cppGen.GenerateAll(analyzer, funcs, emitCppDir.string(), tempRom.string(), 250);
    }

    { std::error_code ec; fs::remove(tempRom, ec); }

    std::cout << "[3/3] Building standalone native x86_64 executable: " << outputPath.string() << "\n";

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[ERROR] Failed to create output executable (is it currently running?): "
                  << outputPath.string() << "\n";
        return 1;
    }

    out.write(reinterpret_cast<const char*>(kEmbeddedRuntimeStub), sizeof(kEmbeddedRuntimeStub));
    uint64_t bundleOffset = static_cast<uint64_t>(sizeof(kEmbeddedRuntimeStub));

    gba::AotBundleHeader aotHdr{};
    aotHdr.magic = gba::AOT_BUNDLE_MAGIC;
    aotHdr.version = 1;
    aotHdr.funcCount = static_cast<uint32_t>(compiled.funcRecords.size());
    aotHdr.totalInstructions = compiled.totalInstructions;
    aotHdr.codeBlobSize = static_cast<uint32_t>(compiled.x64CodeBlob.size());
    aotHdr.romSize = static_cast<uint32_t>(romBytes.size());
    CopyFixedStr(aotHdr.title, sizeof(aotHdr.title), hdr.title);
    CopyFixedStr(aotHdr.gameCode, sizeof(aotHdr.gameCode), hdr.gameCode);
    CopyFixedStr(aotHdr.makerCode, sizeof(aotHdr.makerCode), hdr.makerCode);
    CopyFixedStr(aotHdr.romFilename, sizeof(aotHdr.romFilename), origFilename);

    out.write(reinterpret_cast<const char*>(&aotHdr), sizeof(aotHdr));
    if (!compiled.funcRecords.empty()) {
        out.write(reinterpret_cast<const char*>(compiled.funcRecords.data()),
                  static_cast<std::streamsize>(compiled.funcRecords.size() * sizeof(gba::AotFuncRecord)));
    }
    if (!compiled.x64CodeBlob.empty()) {
        out.write(reinterpret_cast<const char*>(compiled.x64CodeBlob.data()),
                  static_cast<std::streamsize>(compiled.x64CodeBlob.size()));
    }
    out.write(reinterpret_cast<const char*>(romBytes.data()),
              static_cast<std::streamsize>(romBytes.size()));

    uint64_t endBeforeFooter = static_cast<uint64_t>(out.tellp());
    gba::AotBundleFooter footer{};
    footer.bundleOffset = bundleOffset;
    footer.bundleSize = (endBeforeFooter - bundleOffset) + sizeof(footer);
    footer.magic = gba::AOT_BUNDLE_MAGIC;
    out.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
    out.close();

    if (!out) {
        std::cerr << "[ERROR] Failed while writing output executable.\n";
        return 1;
    }

#ifndef _WIN32
    chmod(outputPath.c_str(), 0755);
#endif

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
    std::cout << "====================================================================\n";
    std::cout << "  RECOMPILATION SUCCEEDED! (" << secs << "s)\n";
    std::cout << "  Executable: " << outputPath.string() << "\n";
    std::cout << "  Tip: Press F10 in-game to open the Dear ImGui config menu!\n";
    std::cout << "====================================================================\n";
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { PrintUsage(); return 1; }

    fs::path romPath, outputPath, emitCppDir;
    std::string target = "win64";
    uint32_t maxFuncs = 4000;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help") { PrintUsage(); return 0; }
        else if (arg == L"--target" && i + 1 < argc) target = WideToUtf8(argv[++i]);
        else if ((arg == L"-o" || arg == L"--output") && i + 1 < argc) outputPath = fs::path(argv[++i]);
        else if (arg == L"--emit-cpp" && i + 1 < argc) emitCppDir = fs::path(argv[++i]);
        else if (arg == L"--max-funcs" && i + 1 < argc) maxFuncs = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        else if (!arg.empty() && arg[0] != L'-' && romPath.empty()) romPath = fs::path(arg);
    }

    if (romPath.empty()) { std::cerr << "[ERROR] No .gba ROM file specified.\n"; PrintUsage(); return 1; }
    return RunRecompiler(romPath, WideToUtf8(romPath.filename().wstring()), outputPath, emitCppDir, target, maxFuncs);
}
#else
int main(int argc, char** argv) {
    if (argc < 2) { PrintUsage(); return 1; }

    fs::path romPath, outputPath, emitCppDir;
    std::string target = "linux";
    uint32_t maxFuncs = 4000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { PrintUsage(); return 0; }
        else if (arg == "--target" && i + 1 < argc) target = argv[++i];
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) outputPath = fs::path(argv[++i]);
        else if (arg == "--emit-cpp" && i + 1 < argc) emitCppDir = fs::path(argv[++i]);
        else if (arg == "--max-funcs" && i + 1 < argc) maxFuncs = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (!arg.empty() && arg[0] != '-' && romPath.empty()) romPath = fs::path(arg);
    }

    if (romPath.empty()) { std::cerr << "[ERROR] No .gba ROM file specified.\n"; PrintUsage(); return 1; }
    return RunRecompiler(romPath, romPath.filename().string(), outputPath, emitCppDir, target, maxFuncs);
}
#endif
