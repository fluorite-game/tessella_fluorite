#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
#
# The two libc++ libraries a cross build cannot get from anywhere else.
#
# Both link Filament's static archives, which are built with clang and libc++;
# emb's cross toolchain is gcc, and so is the toolchain it hands a Dart build
# hook, so neither the hook nor an emb `modules:` entry can produce them -- the
# module build runs cmake without CC/CXX in its environment, and fluorite's
# clang wrapper has nothing to swap in. They are built here instead, against the
# sysroot the rpi5-trixie manifest resolved, and picked up by:
#
#   - the quad's `prebuilt:` user-define (libtessella_fluorite.so), which the
#     tessella_fluorite hook publishes as the bundled code asset, and
#   - the deploy, which puts libfluorite_core_ffi.so beside it on the device.
#
#   .emb-cross-libs.sh <profile-dir> <emb-toolchain-file> [cpu-flag]
#
# The cpu flag defaults to the Pi 5's -mcpu=cortex-a76; a Pi 4 wants
# -mcpu=cortex-a72, and it has to match the profile or the Rust half is built
# for a core the board does not have.
#
# Both arguments come from `emb cross ... --update-lock`: the "target sysroot"
# and "cmake tc file" lines. The profile hash moves whenever the manifest does.
set -euo pipefail

P="${1:?profile dir, e.g. .../cross-aarch64-none-linux-gnu-<hash>}"
TC="${2:?emb cmake toolchain file}"
CPU="${3:--mcpu=cortex-a76}"
PKG="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$PKG/build-arm64"
# One directory per profile, because two boards are two sets of libraries, and
# a `current` symlink beside them because the app's `prebuilt:` user-define is a
# static string and has to name one. Whichever board was built last is the one
# the bundle carries -- which is explicit rather than implied by a file date.
OUT="$ROOT/$(basename "$P")"
F=/mnt/dev/ihs_filament_view/packages/fluorite
TESSELLA=/mnt/dev/tessella

export CC="${CC:-$(command -v clang)}"
export CXX="${CXX:-$(command -v clang++)}"
export EMB_TOOLCHAIN_FILE="$TC"
export EMB_GCC_TOOLCHAIN="$P/toolchain/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu"
# The cross signal fluorite's wrapper keys its "was this prepared?" check on.
export PKG_CONFIG_SYSROOT_DIR="$P/sysroot"

mkdir -p "$OUT"

# The producer. A Rust staticlib, so no C++ runtime of its own and no linker
# needed for the archive -- but tessella-ffi also builds a cdylib, which does.
FLAGS="--sysroot=$P/sysroot $CPU -B$P/sysroot/usr/lib/aarch64-linux-gnu -L$P/sysroot/usr/lib/aarch64-linux-gnu -L$P/sysroot/lib/aarch64-linux-gnu -isystem$P/sysroot/usr/include/aarch64-linux-gnu"
GCCBIN="$EMB_GCC_TOOLCHAIN/bin"
( cd "$TESSELLA" && env \
    CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="$GCCBIN/aarch64-none-linux-gnu-gcc" \
    CC_aarch64_unknown_linux_gnu="$GCCBIN/aarch64-none-linux-gnu-gcc" \
    AR_aarch64_unknown_linux_gnu="$GCCBIN/aarch64-none-linux-gnu-ar" \
    CFLAGS_aarch64_unknown_linux_gnu="$FLAGS" \
    CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_RUSTFLAGS="$(for f in $FLAGS; do printf -- '-C link-arg=%s ' "$f"; done)" \
    cargo build -p tessella-ffi --release --features tls --target aarch64-unknown-linux-gnu )

# fluorite's renderer.
cmake -S "$F" -B "$OUT/fluorite" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$F/clang-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DFLUORITE_ENABLE_IHS_PV=ON -DFLUORITE_IHS_PV_GL=ON
cmake --build "$OUT/fluorite" --parallel
cp "$OUT/fluorite/libfluorite_core_ffi.so" "$OUT/"

# The tessella extension, linked against that copy.
cmake -S "$PKG" -B "$OUT/tessella" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$F/clang-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DTESSELLA_DIR="$TESSELLA" \
  -DTESSELLA_LIB="$TESSELLA/target/aarch64-unknown-linux-gnu/release/libtessella_ffi.a" \
  -DFILAMENT_INCLUDE_DIR="$P/sysroot/usr/include" \
  -DFLUORITE_INCLUDE_DIR="$F/include" \
  -DFLUORITE_CORE_FFI_LIB="$OUT/libfluorite_core_ffi.so"
cmake --build "$OUT/tessella" --parallel
cp "$OUT/tessella/libtessella_fluorite.so" "$OUT/"

ln -sfn "$(basename "$OUT")" "$ROOT/current"
echo "cross libs → $OUT (build-arm64/current)"
