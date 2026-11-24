#pragma once

#include "gba_types.h"

namespace gba {

class GbaCpu;
class GbaBus;

// Execute a GBA BIOS Software Interrupt (SWI 0x00..0x2A) via High-Level Emulation
void HandleBiosSwi(GbaCpu& cpu, GbaBus& bus, uint8_t swiNum);

} // namespace gba
