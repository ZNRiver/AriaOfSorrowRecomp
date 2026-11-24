#pragma once

#include "gba_types.h"
#include "recomp_abi.h"

namespace gba {

// Loads the appended AOT native x86_64 code + embedded ROM bundle from the current executable.
// Returns true if loaded successfully.
bool LoadEmbeddedAotBundle();
void UnloadEmbeddedAotBundle();

const RuntimeHelpers* GetRuntimeHelpers();

} // namespace gba
