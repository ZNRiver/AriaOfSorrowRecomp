# AriaOfSorrowRecomp

Self-contained static AOT recompiler for **Castlevania: Aria of Sorrow (GBA)** with a **built-in ARM7TDMI/Thumb $\rightarrow$ x86_64 machine-code backend**. Pass any `.gba` ROM and it generates a standalone native **Windows x86_64** executable (`.exe`) in under 1 second — with **zero external compiler, SDK, or headers required** for end users!

> **Legal Notice:** This repository does **not** include any ROMs, BIOS dumps, or copyrighted game assets. You must provide your own legally obtained copy of *Castlevania: Aria of Sorrow* (`.gba`).

---

## Features

- **100% Self-Contained Static Recompiler (`~1.15 MB` single `.exe`)**
  - Built-in **ARM7TDMI & Thumb $\rightarrow$ Native Windows x86_64 machine-code compiler** (`recompiler/x64_emitter.h`, `recompiler/arm_thumb_to_x64.cpp`) — no Clang, GCC, MSVC, or Windows SDK needed to recompile a ROM!
  - Recompiles ~3,500+ functions (~300,000 ARM/Thumb instructions) into native x86_64 code and builds a standalone `.exe` in **~0.3 seconds**.
  - Automatic control-flow & function discovery (including ROM hack / translation hooks).
  - Range-based native x86_64 function dispatch with seamless fallback for dynamic code in IWRAM/EWRAM.
  - Optional `--emit-cpp <dir>` flag to also export human-readable C++ translation units.
- **In-Game Dear ImGui Configuration Menu (`F10`)**
  - **Automatic OS Language Detection**: automatically adapts UI language to Windows locale (**English**, **Português**, **Español**), with manual override in settings.
  - **Keyboard & Gamepad (XInput) Rebinding**: interactive rebinding for all 10 GBA buttons plus hotkeys (`F10` Config Menu, Fast-Forward, Quick Save `F5`, Quick Load `F7`, Reset `F8`, Fullscreen `F11`).
  - **Video (Direct3D 11)**: Aspect ratio modes (`3:2 Fit`, `Integer Pixel-Perfect`, `Stretch`), Nearest / Linear filtering, VSync, Fullscreen toggle, and FPS / Recompiler telemetry overlay.
  - **Audio**: GBA Direct Sound FIFO A/B with exact `280,896` cycles/frame lock, interleaved Timer/APU stepping, Dynamic Rate Control (DRC), low-pass DAC filter, and volume/mute controls.
  - **Automatic Persistence**: saves SRAM to `<executable_name>.sav` and settings to `recomp_config.ini`.

---

## Usage (Zero Dependencies!)

Download `recomp-ariaofsorrow.exe` from Releases (or build from source) and run:

```powershell
.\recomp-ariaofsorrow.exe "Castlevania - Aria of Sorrow.gba" -o "AriaOfSorrow_Recomp.exe"
```

### CLI Options
| Option | Description |
| --- | --- |
| `--target <win64\|linux\|wasm>` | Target platform for the compiled game (`win64` default on Windows, `linux` default on Linux, or `wasm` for browser WebAssembly) |
| `-o, --output <path>` | Output executable (native host target) or output directory (cross-target / `wasm`) |
| `--max-funcs <N>` | Maximum statically recompiled functions (default: `4000`) |
| `--emit-cpp <dir>` | (Optional) Also export readable C++ translation units |
| `-h, --help` | Display help message |

### Compiling the Game for Linux x86_64

- **On Linux**: run the self-contained Linux `recomp-ariaofsorrow` binary (uses built-in System V AMD64 x86_64 codegen + SDL2/OpenGL 3.3 + Dear ImGui `F10` — zero external compiler needed!):
  ```bash
  ./recomp-ariaofsorrow "Castlevania - Aria of Sorrow.gba" -o AriaOfSorrow_Recomp
  ./AriaOfSorrow_Recomp
  ```
- **From Windows (`--target linux`)**: generates a portable Linux C++20 + CMake game project (`linux_game/generated/`) ready to build on Linux/WSL with SDL2 + OpenGL:
  ```powershell
  .\recomp-ariaofsorrow.exe "Castlevania - Aria of Sorrow.gba" --target linux -o "linux_game"
  ```

### Compiling the Game to WebAssembly (Browser)

You can also compile *Aria of Sorrow* to **WebAssembly** (WebGL2 + SDL2 + WebAudio + Dear ImGui `F10` menu + HTML5 Gamepad API):

```powershell
.\recomp-ariaofsorrow.exe "Castlevania - Aria of Sorrow.gba" --target wasm -o "wasm_game"
```

If **Emscripten (`emcmake`)** is in your `PATH`, it automatically builds `wasm_game/build/AriaOfSorrow_Recomp.html` (`.js` + `.wasm`). Otherwise, it generates the ready-to-build C++20 project and prints the exact `emcmake cmake` command.

Launch the generated `.exe` (or open the WebAssembly `.html` via a local HTTP server) and press **`F10`** (or **`R3`** on a controller) at any time to open the ImGui configuration menu.

---

## Building from Source

### Automatic Dependency Setup (Windows, Linux & WebAssembly)

Run the included Python setup script to automatically check and install all required build tools (including **Emscripten SDK** for `--target wasm`):

```bash
# Native host dependencies (Windows or Linux)
python scripts/setup_deps.py

# WebAssembly (auto-downloads & activates Emscripten SDK if not installed)
python scripts/setup_deps.py --target wasm

# Both native host + WebAssembly
python scripts/setup_deps.py --target all
```

### Requirements
- **Windows x64** or **Linux x64** (`libsdl2-dev`, `libgl1-mesa-dev` on Linux)
- **CMake 3.20+**
- **Ninja**
- **LLVM / Clang 20+** (with `#embed` support)
- *(Optional, for `--target wasm`)* **Emscripten SDK** (auto-installed by `scripts/setup_deps.py --target wasm`)

### Build Commands

```powershell
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds `build/recomp_stub.exe` and embeds it automatically into the single standalone `build/recomp-ariaofsorrow.exe`.

---

## License

Licensed under the **MIT License**. See [LICENSE](LICENSE) for details.  
Third-party dependency [Dear ImGui](https://github.com/ocornut/imgui) (`third_party/imgui/`) is also licensed under MIT.
