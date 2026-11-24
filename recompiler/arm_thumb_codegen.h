#pragma once

#include "rom_analyzer.h"

#include <string>
#include <vector>

namespace recomp {

struct CodegenResult {
    std::vector<std::string> generatedCppFiles;
    uint32_t totalFunctions = 0;
    uint32_t totalInstructions = 0;
};

class ArmThumbCodegen {
public:
    CodegenResult GenerateAll(
        const RomAnalyzer& analyzer,
        const std::vector<DiscoveredFunction>& funcs,
        const std::string& emitDir,
        const std::string& romSourcePath,
        uint32_t funcsPerChunk = 250
    );

private:
    void EmitFunction(
        std::ostream& out,
        const RomAnalyzer& analyzer,
        const DiscoveredFunction& fn
    );
    void EmitArmInstruction(
        std::ostream& out,
        uint32_t pc,
        uint32_t ins,
        const std::vector<uint32_t>& localAddrs
    );
    void EmitThumbInstruction(
        std::ostream& out,
        uint32_t pc,
        uint16_t ins,
        const std::vector<uint32_t>& localAddrs
    );
};

} // namespace recomp
