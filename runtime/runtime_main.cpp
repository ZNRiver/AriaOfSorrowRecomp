#include "win32_dx11_app.h"
#include "aot_loader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
    if (!gba::LoadEmbeddedAotBundle()) {
        std::fprintf(stderr, "[ERROR] No embedded AOT ROM bundle found in this executable.\n");
        MessageBoxA(nullptr,
                    "No embedded Aria of Sorrow AOT bundle found.\n"
                    "Generate a playable executable using:\n"
                    "recomp-ariaofsorrow.exe <rom.gba>",
                    "Aria of Sorrow Recomp", MB_OK | MB_ICONERROR);
        return 1;
    }

    int rc = 0;
    {
        auto app = std::make_unique<gba::Win32Dx11App>();
        rc = app->Run(argc, argv);
    }

    gba::UnloadEmbeddedAotBundle();
    return rc;
}
