#include "rom_analyzer.h"
#include "arm_thumb_to_x64.h"
#include "arm_thumb_codegen.h"
#include "recomp_abi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

// Embedded native Windows x86_64 runtime stub (DX11 + Dear ImGui + GBA HW + AOT loader)
alignas(16) static const uint8_t kEmbeddedRuntimeStub[] = {
#embed "recomp_stub.exe"
};

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(sz), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), sz, nullptr, nullptr);
    return out;
}

void CopyFixedStr(char* dst, size_t dstSize, const std::string& src) {
    std::memset(dst, 0, dstSize);
    if (dstSize == 0) return;
    size_t n = (src.size() < dstSize - 1) ? src.size() : (dstSize - 1);
    std::memcpy(dst, src.data(), n);
}

void PrintUsage() {
    std::cout << "====================================================================\n";
    std::cout << "  recomp-ariaofsorrow - Self-Contained GBA -> x86_64 Recompiler\n";
    std::cout << "====================================================================\n";
    std::cout << "Usage:\n";
    std::cout << "  recomp-ariaofsorrow <path_to_rom.gba> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o, --output <exe>     Output standalone Windows x86_64 .exe path\n";
    std::cout << "  --max-funcs <N>        Maximum statically recompiled functions (default: 4000)\n";
    std::cout << "  --emit-cpp <dir>       (Optional) Also export readable C++ translation units\n";
    std::cout << "  -h, --help             Show this help message\n\n";
    std::cout << "No external compiler, SDK, or headers required!\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    fs::path romPath;
    fs::path outputExe;
    fs::path emitCppDir;
    uint32_t maxFuncs = 4000;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help") {
            PrintUsage();
            return 0;
        } else if ((arg == L"-o" || arg == L"--output") && i + 1 < argc) {
            outputExe = fs::path(argv[++i]);
        } else if (arg == L"--emit-cpp" && i + 1 < argc) {
            emitCppDir = fs::path(argv[++i]);
        } else if (arg == L"--max-funcs" && i + 1 < argc) {
            maxFuncs = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (!arg.empty() && arg[0] != L'-' && romPath.empty()) {
            romPath = fs::path(arg);
        }
    }

    if (romPath.empty()) {
        std::cerr << "[ERROR] No .gba ROM file specified.\n";
        PrintUsage();
        return 1;
    }

    if (!fs::exists(romPath)) {
        std::cerr << "[ERROR] ROM file not found: " << WideToUtf8(romPath.wstring()) << "\n";
        return 1;
    }

    auto tStart = std::chrono::steady_clock::now();

    if (outputExe.empty()) {
        outputExe = fs::current_path() / (romPath.stem().wstring() + L".exe");
    }

    std::cout << "====================================================================\n";
    std::cout << "  recomp-ariaofsorrow - Self-Contained GBA -> x86_64 Recompiler\n";
    std::cout << "====================================================================\n";
    std::cout << "[1/3] Reading GBA ROM: " << WideToUtf8(romPath.filename().wstring()) << "\n";

    // Read ROM directly using std::ifstream(fs::path) so Unicode filenames (e.g. Ária da Tristeza) work cleanly
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

    // Load into RomAnalyzer via temp path or direct memory — copy to temp only if needed
    fs::path tempRom = fs::temp_directory_path() / L"aos_recomp_input.gba";
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
    std::string origFilename = WideToUtf8(romPath.filename().wstring());
    std::cout << "      Title    : " << hdr.title << "\n";
    std::cout << "      GameCode : " << hdr.gameCode << " (Maker: " << hdr.makerCode << ")\n";
    std::cout << "      Size     : " << (hdr.romSize / (1024 * 1024)) << " MB (" << hdr.romSize << " bytes)\n";
    std::cout << "      Header OK: " << (hdr.validLogo ? "Yes (0x96)" : "Warning: byte 0xB2 != 0x96") << "\n";

    std::cout << "[2/3] Analyzing & compiling ARM/Thumb -> native x86_64 machine code (max " << maxFuncs << " funcs)...\n";
    auto funcs = analyzer.DiscoverFunctions(maxFuncs);
    uint32_t armCount = 0, thumbCount = 0;
    for (const auto& f : funcs) {
        if (f.isThumb) thumbCount++; else armCount++;
    }

    recomp::ArmThumbToX64 nativeCompiler;
    auto compiled = nativeCompiler.CompileAll(analyzer, funcs);

    std::cout << "      Compiled " << compiled.funcRecords.size() << " functions ("
              << armCount << " ARM, " << thumbCount << " Thumb)\n";
    std::cout << "      Translated " << compiled.totalInstructions << " ARM/Thumb instructions -> "
              << (compiled.x64CodeBlob.size() / 1024.0) << " KB native x86_64 code\n";

    // Optional readable C++ export
    if (!emitCppDir.empty()) {
        std::cout << "[OPT] Exporting C++ representation to: " << WideToUtf8(emitCppDir.wstring()) << "\n";
        recomp::ArmThumbCodegen cppGen;
        cppGen.GenerateAll(analyzer, funcs, emitCppDir.string(), tempRom.string(), 250);
    }

    {
        std::error_code ec;
        fs::remove(tempRom, ec);
    }

    std::cout << "[3/3] Building standalone Windows x86_64 executable: "
              << WideToUtf8(outputExe.wstring()) << "\n";

    // Write [recomp_stub.exe] + [AotBundleHeader + AotFuncRecords + x64CodeBlob + RomBytes + AotBundleFooter]
    std::ofstream out(outputExe, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[ERROR] Failed to create output executable (is it currently running?): "
                  << WideToUtf8(outputExe.wstring()) << "\n";
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

    auto tEnd = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(tEnd - tStart).count();

    std::cout << "====================================================================\n";
    std::cout << "  RECOMPILATION SUCCEEDED! (" << secs << "s)\n";
    std::cout << "  Executable: " << WideToUtf8(outputExe.wstring()) << "\n";
    std::cout << "  Tip: Press F10 in-game to open the Dear ImGui config menu!\n";
    std::cout << "====================================================================\n";
    return 0;
}
