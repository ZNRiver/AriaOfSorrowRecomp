#!/usr/bin/env python3
"""
AriaOfSorrowRecomp - Cross-Platform Build Dependency Installer
Automatically checks and installs build dependencies for:
  - win64 : CMake, Ninja, LLVM/Clang (Windows)
  - linux : CMake, Ninja, Clang, SDL2, OpenGL (Linux: apt/dnf/pacman/zypper)
  - wasm  : CMake, Ninja, Emscripten SDK (emsdk latest) on Windows or Linux
  - all   : Native host target + WebAssembly (emsdk)

Usage:
  python scripts/setup_deps.py --target win64
  python scripts/setup_deps.py --target linux
  python scripts/setup_deps.py --target wasm
  python scripts/setup_deps.py --target all
  python scripts/setup_deps.py --target wasm --check
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
IMGUI_DIR = REPO_ROOT / "third_party" / "imgui"
IMGUI_TAG = "v1.91.9"


def info(msg: str) -> None:
    print(f"[INFO] {msg}")


def ok(msg: str) -> None:
    print(f"[ OK ] {msg}")


def warn(msg: str) -> None:
    print(f"[WARN] {msg}")


def err(msg: str) -> None:
    print(f"[ERROR] {msg}", file=sys.stderr)


def run(cmd: list[str], cwd: Path | None = None, check: bool = True) -> int:
    print(f"  > {' '.join(str(c) for c in cmd)}")
    res = subprocess.run(cmd, cwd=str(cwd) if cwd else None)
    if check and res.returncode != 0:
        raise RuntimeError(f"Command failed with exit code {res.returncode}: {' '.join(cmd)}")
    return res.returncode


def find_exe(names: list[str], extra_dirs: list[Path] | None = None) -> Path | None:
    for name in names:
        found = shutil.which(name)
        if found:
            return Path(found)
    if extra_dirs:
        for d in extra_dirs:
            for name in names:
                cand = d / name
                if cand.is_file():
                    return cand
    return None


def ensure_imgui(check_only: bool) -> bool:
    if (IMGUI_DIR / "imgui.h").is_file():
        ok(f"Dear ImGui already present at {IMGUI_DIR}")
        return True
    if check_only:
        warn(f"Dear ImGui missing at {IMGUI_DIR}")
        return False
    if not shutil.which("git"):
        err("git is required to clone Dear ImGui.")
        return False
    info(f"Cloning Dear ImGui {IMGUI_TAG} into {IMGUI_DIR}...")
    IMGUI_DIR.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--depth", "1", "--branch", IMGUI_TAG,
         "https://github.com/ocornut/imgui.git", str(IMGUI_DIR)])
    ok("Dear ImGui installed.")
    return True


def setup_windows(check_only: bool) -> bool:
    info("Checking Windows x64 build dependencies (CMake, Ninja, LLVM/Clang)...")
    winget = shutil.which("winget")

    packages = [
        ("cmake", ["cmake.exe"], "Kitware.CMake"),
        ("ninja", ["ninja.exe"], "Ninja-build.Ninja"),
        ("clang++", ["clang++.exe"], "LLVM.LLVM"),
    ]

    all_ok = True
    for label, exes, winget_id in packages:
        found = find_exe(exes)
        if found:
            ok(f"{label} found: {found}")
            continue
        if check_only:
            warn(f"{label} NOT found in PATH (winget package: {winget_id})")
            all_ok = False
            continue
        if not winget:
            err(f"{label} not found and winget is unavailable. Please install {winget_id} manually.")
            all_ok = False
            continue
        info(f"Installing {label} via winget ({winget_id})...")
        rc = run([
            winget, "install", "--id", winget_id, "-e",
            "--accept-package-agreements", "--accept-source-agreements"
        ], check=False)
        if rc != 0:
            warn(f"winget returned code {rc} for {winget_id} (may already be installed or require shell restart).")
            all_ok = False
        else:
            ok(f"{label} installed.")

    if not ensure_imgui(check_only):
        all_ok = False
    return all_ok


def detect_linux_pkg_manager() -> str | None:
    for pm in ("apt-get", "dnf", "pacman", "zypper"):
        if shutil.which(pm):
            return pm
    return None


def pkg_config_exists(pkg: str) -> bool:
    if not shutil.which("pkg-config"):
        return False
    return subprocess.run(["pkg-config", "--exists", pkg],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def setup_linux(check_only: bool) -> bool:
    info("Checking Linux x64 build dependencies (CMake, Ninja, Clang, SDL2, OpenGL)...")
    missing = []
    for tool in ("cmake", "ninja", "clang++"):
        found = shutil.which(tool)
        if found:
            ok(f"{tool} found: {found}")
        else:
            warn(f"{tool} NOT found")
            missing.append(tool)

    for lib in ("sdl2", "gl"):
        if pkg_config_exists(lib):
            ok(f"pkg-config '{lib}' found")
        else:
            warn(f"Development library '{lib}' NOT found via pkg-config")
            missing.append(lib)

    all_ok = len(missing) == 0
    if missing and not check_only:
        pm = detect_linux_pkg_manager()
        sudo = ["sudo"] if (os.geteuid() != 0 and shutil.which("sudo")) else []
        if pm == "apt-get":
            info("Installing missing Linux dependencies via apt-get...")
            run(sudo + ["apt-get", "update"])
            run(sudo + ["apt-get", "install", "-y",
                        "cmake", "ninja-build", "clang", "pkg-config",
                        "libsdl2-dev", "libgl1-mesa-dev"])
            all_ok = True
        elif pm == "dnf":
            info("Installing missing Linux dependencies via dnf...")
            run(sudo + ["dnf", "install", "-y",
                        "cmake", "ninja-build", "clang", "pkgconf-pkg-config",
                        "SDL2-devel", "mesa-libGL-devel"])
            all_ok = True
        elif pm == "pacman":
            info("Installing missing Linux dependencies via pacman...")
            run(sudo + ["pacman", "-S", "--needed", "--noconfirm",
                        "cmake", "ninja", "clang", "pkgconf", "sdl2", "mesa"])
            all_ok = True
        elif pm == "zypper":
            info("Installing missing Linux dependencies via zypper...")
            run(sudo + ["zypper", "install", "-y",
                        "cmake", "ninja", "clang", "pkg-config", "libSDL2-devel", "Mesa-libGL-devel"])
            all_ok = True
        else:
            err("Unsupported Linux package manager. Install manually: cmake, ninja, clang, SDL2-dev, OpenGL-dev.")
            all_ok = False

    if not ensure_imgui(check_only):
        all_ok = False
    return all_ok


def setup_wasm(tools_dir: Path, check_only: bool) -> bool:
    info("Checking WebAssembly (Emscripten SDK) dependencies...")
    is_win = platform.system() == "Windows"

    # Also check common existing locations (EMSDK env, tools_dir/emsdk, PATH)
    emsdk_candidates = [tools_dir / "emsdk"]
    if os.environ.get("EMSDK"):
        emsdk_candidates.insert(0, Path(os.environ["EMSDK"]))
    if is_win:
        openidea_emsdk = Path.home() / "Downloads" / "openidea" / "data" / "tools" / "emsdk"
        if openidea_emsdk.is_dir():
            emsdk_candidates.append(openidea_emsdk)

    extra_bins = [c / "upstream" / "emscripten" for c in emsdk_candidates]
    emcmake_names = ["emcmake.bat", "emcmake.cmd", "emcmake.exe"] if is_win else ["emcmake"]
    found_emcmake = find_exe(emcmake_names, extra_bins)

    # Ensure CMake + Ninja + ImGui are also present
    if is_win:
        if not find_exe(["cmake.exe"]) or not find_exe(["ninja.exe"]):
            setup_windows(check_only)
    else:
        if not shutil.which("cmake") or not shutil.which("ninja"):
            setup_linux(check_only)
    ensure_imgui(check_only)

    if found_emcmake:
        ok(f"Emscripten (emcmake) already installed: {found_emcmake}")
        return True

    if check_only:
        warn("Emscripten SDK (emcmake) NOT found.")
        return False

    if not shutil.which("git"):
        err("git is required to download Emscripten SDK (emsdk).")
        return False

    emsdk_dir = tools_dir / "emsdk"
    emsdk_dir.parent.mkdir(parents=True, exist_ok=True)
    if not (emsdk_dir / ".git").exists():
        info(f"Cloning Emscripten SDK into {emsdk_dir}...")
        run(["git", "clone", "--depth", "1", "https://github.com/emscripten-core/emsdk.git", str(emsdk_dir)])

    emsdk_cmd = str(emsdk_dir / ("emsdk.bat" if is_win else "emsdk"))
    info("Installing Emscripten SDK 'latest' (this downloads LLVM/Binaryen/Node/Python)...")
    run([emsdk_cmd, "install", "latest"], cwd=emsdk_dir)
    info("Activating Emscripten SDK 'latest'...")
    run([emsdk_cmd, "activate", "latest"], cwd=emsdk_dir)

    ok(f"Emscripten SDK installed in {emsdk_dir}!")
    if is_win:
        info(f"To activate in your terminal, run:  {emsdk_dir / 'emsdk_env.bat'}")
    else:
        info(f"To activate in your terminal, run:  source {emsdk_dir / 'emsdk_env.sh'}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Install build dependencies for AriaOfSorrowRecomp (Windows, Linux, WebAssembly)."
    )
    parser.add_argument(
        "--target",
        choices=["auto", "win64", "linux", "wasm", "all"],
        default="auto",
        help="Target platform to install dependencies for (default: auto-detect host OS)"
    )
    parser.add_argument(
        "--tools-dir",
        type=Path,
        default=REPO_ROOT / "tools",
        help="Directory where portable tools like emsdk will be installed (default: ./tools)"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Only check installed dependencies without downloading/installing anything"
    )
    args = parser.parse_args()

    host_os = platform.system()
    target = args.target
    if target == "auto":
        target = "win64" if host_os == "Windows" else "linux"

    print("====================================================================")
    print(f"  AriaOfSorrowRecomp Dependency Setup (host={host_os}, target={target})")
    print("====================================================================")

    success = True
    try:
        if target == "win64":
            if host_os != "Windows":
                warn("Requested --target win64 on a non-Windows host.")
            success = setup_windows(args.check)
        elif target == "linux":
            if host_os != "Linux":
                warn("Requested --target linux on a non-Linux host (install Linux deps inside Linux/WSL).")
            else:
                success = setup_linux(args.check)
        elif target == "wasm":
            success = setup_wasm(args.tools_dir, args.check)
        elif target == "all":
            if host_os == "Windows":
                success &= setup_windows(args.check)
            elif host_os == "Linux":
                success &= setup_linux(args.check)
            success &= setup_wasm(args.tools_dir, args.check)
    except Exception as exc:
        err(str(exc))
        return 1

    print("====================================================================")
    if success:
        ok("All requested dependencies are ready!")
        return 0
    else:
        warn("Some dependencies are missing or could not be installed automatically.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
