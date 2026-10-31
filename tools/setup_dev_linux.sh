#!/usr/bin/env bash
# Installs what building and testing this project needs on Linux, then fetches
# the pinned sources (tools/setup_deps.py).
#
#   tools/setup_dev_linux.sh            install the packages and the sources
#   tools/setup_dev_linux.sh --build    ... then configure, build and run the tests
#   tools/setup_dev_linux.sh --print    only print the package command for this system
#
# Supports pacman (Arch), apt (Debian, Ubuntu), dnf (Fedora) and zypper (openSUSE).
# SDL 3.2+ and Vulkan headers with VK_KHR_present_wait2 (mid-2025 or newer) are
# needed: Ubuntu 24.04 and Debian 12 ship older ones, and the check at the end
# says so. Your own Wii U game is not needed for this; see the README for it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build=0
print_only=0
for argument in "$@"; do
    case "$argument" in
        --build) build=1 ;;
        --print) print_only=1 ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $argument (try --help)" >&2; exit 2 ;;
    esac
done

if [ "$(uname -s)" != Linux ]; then
    echo "This script is for Linux. On Windows, use Start Ultimate Launcher.cmd (Set Up Game)" >&2
    echo "or tools/setup_windows_runtime.ps1." >&2
    exit 1
fi

sudo=()
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null; then sudo=(sudo); else echo "Run as root or install sudo." >&2; exit 1; fi
fi

# Packages the build needs, then optional ones (the GTK launcher and the MinGW
# compiler for the Tekken 7/8 helpers: without them those parts are left out).
if command -v pacman >/dev/null; then
    manager=pacman
    install=("${sudo[@]}" pacman -S --needed --noconfirm)
    required=(base-devel git cmake ninja clang python python-numpy python-pillow zstd zlib openssl
              vulkan-headers vulkan-icd-loader sdl3 shaderc)
    optional=(gtk4 mingw-w64-gcc)
elif command -v apt-get >/dev/null; then
    manager=apt
    install=("${sudo[@]}" apt-get install -y)
    required=(build-essential git cmake ninja-build clang python3 python3-numpy python3-pil libzstd-dev
              zlib1g-dev libssl-dev libvulkan-dev libsdl3-dev libshaderc-dev pkg-config)
    optional=(libgtk-4-dev gcc-mingw-w64-x86-64)
elif command -v dnf >/dev/null; then
    manager=dnf
    install=("${sudo[@]}" dnf install -y)
    required=(gcc gcc-c++ make git cmake ninja-build clang python3 python3-numpy python3-pillow
              libzstd-devel zlib-devel openssl-devel vulkan-headers vulkan-loader-devel SDL3-devel
              libshaderc-devel pkgconf-pkg-config)
    optional=(gtk4-devel mingw64-gcc)
elif command -v zypper >/dev/null; then
    manager=zypper
    install=("${sudo[@]}" zypper --non-interactive install)
    required=(gcc gcc-c++ make git cmake ninja clang python3 python3-numpy python3-Pillow libzstd-devel
              zlib-devel libopenssl-3-devel vulkan-headers vulkan-devel SDL3-devel shaderc-devel pkg-config)
    optional=(gtk4-devel cross-x86_64-w64-mingw32-gcc)
else
    echo "No supported package manager (pacman, apt, dnf, zypper) found." >&2
    echo "Install: Git, CMake 3.20+, Ninja, Clang or GCC 15+, Python 3 with numpy and Pillow, and the" >&2
    echo "development files of SDL3, shaderc, Vulkan (loader and headers), OpenSSL 3, zstd and zlib." >&2
    exit 1
fi

echo "Package manager: $manager"
echo "Required: ${required[*]}"
echo "Optional: ${optional[*]}"
if [ "$print_only" -eq 1 ]; then
    echo "${install[*]} ${required[*]}"
    exit 0
fi

if [ "$manager" = apt ]; then "${sudo[@]}" apt-get update; fi
"${install[@]}" "${required[@]}"
for package in "${optional[@]}"; do
    "${install[@]}" "$package" || echo "Optional package $package is not available here: skipped."
done

cd "$root"
python3 tools/setup_deps.py

# What the packages above cannot guarantee.
problems=0
if ! grep -q -m1 avx2 /proc/cpuinfo || ! grep -q -m1 fma /proc/cpuinfo; then
    echo "WARNING: this CPU lacks AVX2/FMA (x86-64-v3); the game will not run on it." >&2
fi
if command -v pkg-config >/dev/null && pkg-config --exists sdl3; then
    if ! pkg-config --atleast-version=3.2 sdl3; then
        echo "WARNING: SDL $(pkg-config --modversion sdl3) found; 3.2 or newer is needed." >&2
        problems=1
    fi
else
    echo "WARNING: SDL3 development files not found by pkg-config." >&2
    problems=1
fi
if ! grep -rqs VK_KHR_present_wait2 /usr/include/vulkan /usr/local/include/vulkan 2>/dev/null; then
    echo "WARNING: the Vulkan headers do not define VK_KHR_present_wait2 (need mid-2025 or newer)." >&2
    problems=1
fi
if [ "$problems" -ne 0 ]; then
    echo "This distribution's packages are too old for the build: use a newer release (Arch, Fedora," >&2
    echo "Ubuntu 25.10+, Debian 13+) or build SDL3 and the Vulkan headers yourself." >&2
fi

if [ "$build" -eq 1 ]; then
    cc=gcc cxx=g++
    if command -v clang >/dev/null; then cc=clang cxx=clang++; fi
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx"
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure --timeout 180
fi

echo "Done. Next: see \"Building\" in the README (your game archive is needed for the game itself)."
