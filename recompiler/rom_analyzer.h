#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recomp {

struct RomHeader {
    std::string title;
    std::string gameCode;
    std::string makerCode;
    std::string filename;
    uint32_t romSize = 0;
    uint32_t entryPoint = 0x08000000u;
    bool validLogo = false;
};

struct DiscoveredFunction {
    uint32_t entryAddr = 0; // GBA address (without Thumb bit)
    bool isThumb = false;
    std::vector<uint32_t> insnAddrs; // Sorted unique instruction addresses belonging to this function
};

class RomAnalyzer {
public:
    bool LoadRom(const std::string& path);
    const RomHeader& Header() const { return header_; }
    const std::vector<uint8_t>& RomBytes() const { return rom_; }

    std::vector<DiscoveredFunction> DiscoverFunctions(uint32_t maxFunctions = 2500) const;

    uint16_t Read16(uint32_t gbaAddr) const;
    uint32_t Read32(uint32_t gbaAddr) const;
    bool IsValidRomAddr(uint32_t gbaAddr) const;

private:
    RomHeader header_;
    std::vector<uint8_t> rom_;
};

} // namespace recomp
