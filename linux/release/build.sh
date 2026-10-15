#!/usr/bin/env bash
# Builds the Linux release archive, dist/X4_VR-<version>-linux-x86_64.tar.gz, and its checksum file.
# The version is `git describe --tags`, as scripts/package.ps1 names the Windows zip: the release
# tag on a release commit (v0.6.0), else tag-count-commit, -dirty with uncommitted changes.
# Runs inside Valve's Steam Runtime "sniper" SDK image (Debian 11, glibc 2.31), so the binaries
# run on every system that runs Steam. The same script runs locally and in CI. From the
# repository's top folder:
#
#   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/src -w /src \
#     registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest linux/release/build.sh
#
# OPENVR_SOURCE_DIR (optional): a copy of Valve's OpenVR source; without it the build downloads it.
set -euo pipefail

build=build/release            # build folder (build/ is ignored by git)
stage=$build/x4vr              # what goes into the archive, as installed
dist=dist                      # the archive and its checksum (ignored by git)
max_glibc=2.31                 # Steam's own minimum: what every player has

die() { echo "release build: $*" >&2; exit 1; }

[ -f linux/CMakeLists.txt ] || die "run this from the repository's top folder"
command -v g++-14 >/dev/null || die "g++-14 not found: run this inside the sniper SDK image"
version=$(git describe --tags --always --dirty) || die "can't read the version from git"
name=X4_VR-$version-linux-x86_64

# 1. Configure, from scratch: GCC 14 (the image's plain gcc is GCC 10), lib/ for the library.
rm -rf "$build" "$dist"
cmake_args=(-DX4VR_LINUX=ON -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX="$PWD/$stage" -DCMAKE_INSTALL_LIBDIR=lib)
if [ -n "${OPENVR_SOURCE_DIR:-}" ]; then cmake_args+=(-DOPENVR_SOURCE_DIR="$OPENVR_SOURCE_DIR"); fi
CC=gcc-14 CXX=g++-14 cmake -S . -B "$build" "${cmake_args[@]}"

# 2. Build, test, install into the stage folder.
cmake --build "$build" -j "$(nproc)"
ctest --test-dir "$build" --output-on-failure
cmake --install "$build"
echo "$version" > "$stage/version.txt"   # as in the Windows zip: the version without running anything

# 3. Checks: nothing newer than glibc 2.31, and the C++ runtime built in (no libstdc++/libgcc_s).
binaries=("$stage/bin/x4vr" "$stage/lib/libx4vr.so")
newest=$(objdump -T "${binaries[@]}" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
highest=$(printf '%s\n%s\n' "$newest" "$max_glibc" | sort -V | tail -1)
[ "$highest" = "$max_glibc" ] || die "the binaries need glibc $newest, newer than $max_glibc"
if readelf -d "${binaries[@]}" | grep -E 'NEEDED.*(libstdc\+\+|libgcc_s)'; then
    die "the binaries need the system's C++ runtime (above); it should be built in"
fi

# 4. Pack: one top folder, x4vr/; owner and order fixed, so the archive doesn't depend on who built it.
mkdir -p "$dist"
tar -czf "$dist/$name.tar.gz" --owner=0 --group=0 --numeric-owner --sort=name -C "$build" x4vr
(cd "$dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")

echo "release build: done, newest glibc needed: $newest"
tar -tzvf "$dist/$name.tar.gz"
cat "$dist/$name.tar.gz.sha256"
