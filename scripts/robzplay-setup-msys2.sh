#!/usr/bin/env bash
# Robz Play - one-shot dependency setup for Windows (MSYS2 MINGW64 shell).
# Mirrors the official chiaki-ng MSYS2 release workflow, but keeps every
# download and source build under ./_deps so the repository stays clean.
#
# Usage (from an MSYS2 MINGW64 shell, in the repository root):
#   scripts/robzplay-setup-msys2.sh
#
# Clone the repository with LF line endings (git config core.autocrlf false), including
# its submodules: the bundled curl is patched at configure time and the patch does not
# apply to CRLF files.
set -euo pipefail

if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
	echo "error: run this from an MSYS2 MINGW64 shell (MSYSTEM=MINGW64)" >&2
	exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/_deps"
PREFIX=/mingw64
FFMPEG_NAME="ffmpeg-n9.0-latest-win64-gpl-shared-9.0"
FFMPEG_URL="https://github.com/streetpea/FFmpeg-Builds/releases/download/latest/${FFMPEG_NAME}.zip"

mkdir -p "$DEPS"
cd "$ROOT"

echo "== [1/6] MSYS2 packages"
pacman -S --noconfirm --needed pactoys git diffutils make unzip zip
pacboy -S --noconfirm --needed \
	ca-certificates:p cc:p cmake:p curl:p fast_float:p fftw:p json-c:p libevent:p \
	lcms2:p libdovi:p meson:p miniupnpc:p gcc:p nasm:p ninja:p openssl:p opus:p \
	pkgconf:p protobuf:p python:p python-psutil:p python-glad:p python-jinja:p \
	python-pip:p python-protobuf:p qt6-base:p qt6-declarative:p qt6-svg:p shaderc:p \
	speexdsp:p spirv-cross:p vulkan:p vulkan-headers:p compiler-rt:p librsvg:p \
	nsis:p

echo "== [2/6] FFmpeg (prebuilt, shared)"
if [[ ! -d "$DEPS/$FFMPEG_NAME" ]]; then
	curl -L -o "$DEPS/$FFMPEG_NAME.zip" "$FFMPEG_URL"
	unzip -q -o "$DEPS/$FFMPEG_NAME.zip" -d "$DEPS"
fi
cp -a "$DEPS/$FFMPEG_NAME/bin/." "$PREFIX/bin"
cp -a "$DEPS/$FFMPEG_NAME/include/." "$PREFIX/include"
cp -a "$DEPS/$FFMPEG_NAME/lib/." "$PREFIX/lib"

echo "== [3/6] libplacebo"
if ! pkg-config --exists libplacebo; then
	scripts/build-libplacebo-windows.sh _deps
fi

echo "== [4/6] SDL3 + sdl2-compat"
if ! pkg-config --exists sdl2-compat; then
	INSTALL_PREFIX="$PREFIX" scripts/build-sdl2-compat.sh _deps
fi
cp "$PREFIX/lib/pkgconfig/sdl2-compat.pc" "$PREFIX/lib/pkgconfig/sdl2.pc"

echo "== [5/6] hidapi"
if ! pkg-config --exists hidapi; then
	INSTALL_PREFIX="$PREFIX" scripts/build-hidapi.sh _deps
fi

echo "== [6/6] ncnn (Vulkan inference runtime for frame generation)"
NCNN_TAG="${NCNN_TAG:-20261020}"
if [[ ! -f "$DEPS/ncnn-install/lib/cmake/ncnn/ncnnConfig.cmake" ]]; then
	if [[ ! -d "$DEPS/ncnn" ]]; then
		git -c core.autocrlf=false clone --depth 1 --branch "$NCNN_TAG" https://github.com/Tencent/ncnn.git "$DEPS/ncnn"
		git -C "$DEPS/ncnn" submodule update --init --depth 1 glslang
	fi
	cmake -S "$DEPS/ncnn" -B "$DEPS/ncnn/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$DEPS/ncnn-install" -DNCNN_VULKAN=ON -DNCNN_SHARED_LIB=OFF -DNCNN_OPENMP=OFF \
		-DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF \
		-DNCNN_PIXEL_ROTATE=OFF -DNCNN_PIXEL_AFFINE=OFF -DNCNN_PIXEL_DRAWING=OFF
	cmake --build "$DEPS/ncnn/build"
	cmake --install "$DEPS/ncnn/build"
fi

echo "== Dependencies ready"
pkg-config --modversion libplacebo sdl2 hidapi libavcodec
