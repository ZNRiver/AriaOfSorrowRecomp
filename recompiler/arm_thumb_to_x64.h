#pragma once

#include "rom_analyzer.h"
#include "recomp_abi.h"

#include <cstdint>
#include <vector>

namespace recomp {

struct NativeCodegenResult {
    std::vector<gba::AotFuncRecord> funcRecords;
    std::vector<uint8_t> x64CodeBlob;
    uint32_t totalFunctions = 0;
    uint32_t totalInstructions = 0;
};

// Direct AOT static compiler: translates GBA ARM7TDMI & Thumb functions straight into
// native Windows x86_64 machine code (zero external compiler required!).
class ArmThumbToX64 {
public:
    NativeCodegenResult CompileAll(
        const RomAnalyzer& analyzer,
        const std::vector<DiscoveredFunction>& funcs
    );
};

} // namespace recomp
