#include "aot_loader.h"
#include "gba_cpu.h"
#include "gba_bus.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#include <limits.h>
#endif

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace gba {

namespace {

// C-ABI trampolines for native x86_64 recompiled code
uint32_t H_Read8(GbaCpu* cpu, uint32_t addr)         { return cpu->bus.Read8(addr); }
uint32_t H_Read16(GbaCpu* cpu, uint32_t addr)        { return cpu->bus.Read16(addr); }
uint32_t H_Read32(GbaCpu* cpu, uint32_t addr)        { return cpu->bus.Read32(addr); }
uint32_t H_Read32Rot(GbaCpu* cpu, uint32_t addr)     { return cpu->Read32Rotated(addr); }
uint32_t H_Read16Rot(GbaCpu* cpu, uint32_t addr)     { return cpu->Read16Rotated(addr); }
uint32_t H_Read16Sgn(GbaCpu* cpu, uint32_t addr)     { return cpu->Read16Signed(addr); }
uint32_t H_Read8Sgn(GbaCpu* cpu, uint32_t addr)      { return cpu->Read8Signed(addr); }

void H_Write8(GbaCpu* cpu, uint32_t addr, uint32_t v)  { cpu->bus.Write8(addr, static_cast<uint8_t>(v)); }
void H_Write16(GbaCpu* cpu, uint32_t addr, uint32_t v) { cpu->bus.Write16(addr, static_cast<uint16_t>(v)); }
void H_Write32(GbaCpu* cpu, uint32_t addr, uint32_t v) { cpu->bus.Write32(addr, v); }

uint32_t H_CheckCond(GbaCpu* cpu, uint32_t cond)     { return cpu->CheckCond(cond) ? 1u : 0u; }
void     H_SetNZ(GbaCpu* cpu, uint32_t v)            { cpu->SetNZ(v); }
void     H_SetNZC(GbaCpu* cpu, uint32_t v, uint32_t c) { cpu->SetNZC(v, c != 0); }
uint32_t H_Add(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t sf) { return cpu->Add(a, b, sf != 0); }
uint32_t H_Adc(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t sf) { return cpu->Adc(a, b, sf != 0); }
uint32_t H_Sub(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t sf) { return cpu->Sub(a, b, sf != 0); }
uint32_t H_Sbc(GbaCpu* cpu, uint32_t a, uint32_t b, uint32_t sf) { return cpu->Sbc(a, b, sf != 0); }

uint32_t H_ShiftImmSF(GbaCpu* cpu, uint32_t val, uint32_t type, uint32_t amt) {
    bool c = cpu->FlagC();
    uint32_t res = cpu->ShiftImm(val, type, amt, c);
    cpu->SetNZC(res, c);
    return res;
}
uint32_t H_ShiftRegSF(GbaCpu* cpu, uint32_t val, uint32_t type, uint32_t amt) {
    bool c = cpu->FlagC();
    uint32_t res = cpu->ShiftReg(val, type, amt, c);
    cpu->SetNZC(res, c);
    return res;
}

void H_BranchExchange(GbaCpu* cpu, uint32_t target)  { cpu->BranchExchange(target); }
void H_ExecSwi(GbaCpu* cpu, uint32_t num)            { cpu->ExecSwi(static_cast<uint8_t>(num)); }
void H_ExecArm(GbaCpu* cpu, uint32_t ins)            { cpu->ExecArm(ins); }
void H_ExecThumb(GbaCpu* cpu, uint32_t ins)          { cpu->ExecThumb(static_cast<uint16_t>(ins)); }

const RuntimeHelpers kHelpers = {
    &H_Read8, &H_Read16, &H_Read32,
    &H_Read32Rot, &H_Read16Rot, &H_Read16Sgn, &H_Read8Sgn,
    &H_Write8, &H_Write16, &H_Write32,
    &H_CheckCond, &H_SetNZ, &H_SetNZC,
    &H_Add, &H_Adc, &H_Sub, &H_Sbc,
    &H_ShiftImmSF, &H_ShiftRegSF,
    &H_BranchExchange, &H_ExecSwi, &H_ExecArm, &H_ExecThumb
};

// Loaded bundle state
void* g_execCodeMem = nullptr;
size_t g_execCodeSize = 0;
std::vector<RecompEntry> g_dispatchTable;
std::vector<uint8_t> g_romData;
std::string g_title, g_gameCode, g_makerCode, g_romFilename;
RecompRomInfo g_romInfo = { "UNKNOWN", "????", "??", "unknown.gba", 0, 0, 0 };

} // namespace

const RuntimeHelpers* GetRuntimeHelpers() {
    return &kHelpers;
}

bool LoadEmbeddedAotBundle() {
#ifdef _WIN32
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;
    std::ifstream in(exePath, std::ios::binary | std::ios::ate);
#elif defined(__linux__)
    char exePath[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) return false;
    exePath[len] = '\0';
    std::ifstream in(exePath, std::ios::binary | std::ios::ate);
#else
    return false;
#endif
    if (!in.is_open()) return false;

    std::streamsize fileSize = in.tellg();
    if (fileSize < static_cast<std::streamsize>(sizeof(AotBundleFooter))) return false;

    in.seekg(fileSize - static_cast<std::streamsize>(sizeof(AotBundleFooter)), std::ios::beg);
    AotBundleFooter footer{};
    if (!in.read(reinterpret_cast<char*>(&footer), sizeof(footer))) return false;
    if (footer.magic != AOT_BUNDLE_MAGIC) return false;
    if (footer.bundleOffset + footer.bundleSize > static_cast<uint64_t>(fileSize)) return false;

    in.seekg(static_cast<std::streamoff>(footer.bundleOffset), std::ios::beg);
    AotBundleHeader hdr{};
    if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) return false;
    if (hdr.magic != AOT_BUNDLE_MAGIC) return false;

    std::vector<AotFuncRecord> records(hdr.funcCount);
    if (hdr.funcCount > 0) {
        if (!in.read(reinterpret_cast<char*>(records.data()),
                     static_cast<std::streamsize>(records.size() * sizeof(AotFuncRecord)))) {
            return false;
        }
    }

    // Allocate executable memory for the statically recompiled native x86_64 code blob!
    if (hdr.codeBlobSize > 0) {
#ifdef _WIN32
        g_execCodeMem = VirtualAlloc(nullptr, hdr.codeBlobSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_execCodeMem) return false;
        g_execCodeSize = hdr.codeBlobSize;
        if (!in.read(reinterpret_cast<char*>(g_execCodeMem), hdr.codeBlobSize)) {
            UnloadEmbeddedAotBundle();
            return false;
        }
        DWORD oldProt = 0;
        if (!VirtualProtect(g_execCodeMem, hdr.codeBlobSize, PAGE_EXECUTE_READ, &oldProt)) {
            UnloadEmbeddedAotBundle();
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), g_execCodeMem, hdr.codeBlobSize);
#elif defined(__linux__)
        g_execCodeMem = mmap(nullptr, hdr.codeBlobSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_execCodeMem == MAP_FAILED) {
            g_execCodeMem = nullptr;
            return false;
        }
        g_execCodeSize = hdr.codeBlobSize;
        if (!in.read(reinterpret_cast<char*>(g_execCodeMem), hdr.codeBlobSize)) {
            UnloadEmbeddedAotBundle();
            return false;
        }
        if (mprotect(g_execCodeMem, hdr.codeBlobSize, PROT_READ | PROT_EXEC) != 0) {
            UnloadEmbeddedAotBundle();
            return false;
        }
        __builtin___clear_cache(static_cast<char*>(g_execCodeMem),
                                static_cast<char*>(g_execCodeMem) + hdr.codeBlobSize);
#endif
    }

    // Read embedded GBA ROM bytes
    g_romData.resize(hdr.romSize);
    if (hdr.romSize > 0) {
        if (!in.read(reinterpret_cast<char*>(g_romData.data()), hdr.romSize)) {
            UnloadEmbeddedAotBundle();
            return false;
        }
    }

    // Build native dispatch table
    g_dispatchTable.clear();
    g_dispatchTable.reserve(hdr.funcCount);
    auto* codeBase = static_cast<uint8_t*>(g_execCodeMem);
    for (const auto& rec : records) {
        RecompEntry e{};
        e.startAddr = rec.startAddr;
        e.endAddr = rec.endAddr;
        e.isThumb = rec.isThumb;
        e.fn = reinterpret_cast<NativeRecompFunc>(codeBase + rec.codeOffset);
        g_dispatchTable.push_back(e);
    }

    g_title = std::string(hdr.title, strnlen(hdr.title, sizeof(hdr.title)));
    g_gameCode = std::string(hdr.gameCode, strnlen(hdr.gameCode, sizeof(hdr.gameCode)));
    g_makerCode = std::string(hdr.makerCode, strnlen(hdr.makerCode, sizeof(hdr.makerCode)));
    g_romFilename = std::string(hdr.romFilename, strnlen(hdr.romFilename, sizeof(hdr.romFilename)));

    g_romInfo.title = g_title.c_str();
    g_romInfo.gameCode = g_gameCode.c_str();
    g_romInfo.makerCode = g_makerCode.c_str();
    g_romInfo.romFilename = g_romFilename.c_str();
    g_romInfo.romSize = hdr.romSize;
    g_romInfo.recompiledFunctionCount = hdr.funcCount;
    g_romInfo.recompiledInstructionCount = hdr.totalInstructions;
    return true;
}

void UnloadEmbeddedAotBundle() {
    if (g_execCodeMem) {
#ifdef _WIN32
        VirtualFree(g_execCodeMem, 0, MEM_RELEASE);
#elif defined(__linux__)
        munmap(g_execCodeMem, g_execCodeSize);
#endif
        g_execCodeMem = nullptr;
        g_execCodeSize = 0;
    }
    g_dispatchTable.clear();
    g_romData.clear();
}

const uint8_t* GetEmbeddedRomData(size_t& outSize) {
    outSize = g_romData.size();
    return g_romData.empty() ? nullptr : g_romData.data();
}

const RecompRomInfo& GetRecompRomInfo() {
    return g_romInfo;
}

NativeRecompFunc LookupRecompiledFunction(uint32_t addrWithModeBit) {
    const size_t N = g_dispatchTable.size();
    if (N == 0) return nullptr;
    uint8_t wantThumb = static_cast<uint8_t>(addrWithModeBit & 1u);
    uint32_t addr = addrWithModeBit & ~1u;
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (g_dispatchTable[mid].startAddr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return nullptr;
    size_t idx = lo - 1;
    for (int back = 0; back < 4 && idx >= static_cast<size_t>(back); ++back) {
        const auto& e = g_dispatchTable[idx - back];
        if (e.isThumb == wantThumb && addr >= e.startAddr && addr <= e.endAddr) return e.fn;
    }
    return nullptr;
}

} // namespace gba
