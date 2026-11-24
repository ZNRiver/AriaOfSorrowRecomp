#pragma once

#include "rom_analyzer.h"
#include "recomp_abi.h"

#include <cstdint>
#include <vector>

namespace recomp {

enum class X64Abi {
    Win64,  // Windows x64: args in RCX, RDX, R8, R9 + 32-byte shadow space
    SysV64, // Linux x86_64 System V: args in RDI, RSI, RDX, RCX + 0 shadow space
};

struct NativeCodegenResult {
    std::vector<gba::AotFuncRecord> funcRecords;
    std::vector<uint8_t> x64CodeBlob;
    uint32_t totalFunctions = 0;
    uint32_t totalInstructions = 0;
};

// Direct AOT static compiler: translates GBA ARM7TDMI & Thumb functions straight into
// native x86_64 machine code for Windows x64 or Linux x86_64 (zero external compiler required!).
class ArmThumbToX64 {
public:
    NativeCodegenResult CompileAll(
        const RomAnalyzer& analyzer,
        const std::vector<DiscoveredFunction>& funcs,
        X64Abi abi = X64Abi::Win64
    );
};

} // namespace recomp
