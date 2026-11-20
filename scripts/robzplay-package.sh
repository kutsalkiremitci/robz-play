#!/bin/bash
# Robz Play - assembles the release package.
#
#   scripts/robzplay-build.sh --deploy     # once, so that dist/RobzPlay holds every DLL
#   scripts/robzplay-package.sh
#
# Result: dist/release/RobzPlay-<version>-windows-x64.zip - a folder that runs from anywhere:
# the executable just built, the runtime files of dist/RobzPlay, the frame generation
# model, the licences and a short read-me.
set -euo pipefail
cd "$(dirname "$0")/.."

part() { sed -n "s/^set(ROBZPLAY_VERSION_$1 \([0-9]*\))/\1/p" CMakeLists.txt; }
VERSION="$(part MAJOR).$(part MINOR).$(part PATCH)"
NAME="RobzPlay-$VERSION-windows-x64"
STAGE="dist/release/$NAME"

[ -f build/gui/RobzPlay.exe ] || { echo "build/gui/RobzPlay.exe is missing: run scripts/robzplay-build.sh first"; exit 1; }
[ -f dist/RobzPlay/Qt6Core.dll ] || { echo "dist/RobzPlay has no runtime files: run scripts/robzplay-build.sh --deploy first"; exit 1; }

echo "== Staging $NAME"
rm -rf "$STAGE" "dist/release/$NAME.zip"
mkdir -p "$STAGE"
cp -r dist/RobzPlay/. "$STAGE/"
cp build/gui/RobzPlay.exe "$STAGE/RobzPlay.exe"
mkdir -p "$STAGE/models"
cp -r robzfg/models/. "$STAGE/models/"

mkdir -p "$STAGE/licenses"
cp COPYING "$STAGE/licenses/COPYING.txt"
cp LICENSES/* "$STAGE/licenses/"
cp robzfg/models/LICENSE.rife-ncnn-vulkan "$STAGE/licenses/"
cat > "$STAGE/licenses/THIRD-PARTY.txt" <<'EOF'
Robz Play is free software under the GNU Affero General Public License version 3 only,
with the OpenSSL linking exception (see COPYING.txt and AGPL-3.0-only-OpenSSL.txt).
Its source code, and the scripts that built this package, are at
https://github.com/kutsalkiremitci/robz-play

This package also contains these works, each under its own licence:

  chiaki-ng / Chiaki      AGPL-3.0-only with OpenSSL exception   https://github.com/chiaki-ng/chiaki-ng
  RIFE v4.6 model         MIT                                    https://github.com/hzwer/Practical-RIFE
  rife-ncnn-vulkan        MIT (LICENSE.rife-ncnn-vulkan)         https://github.com/nihui/rife-ncnn-vulkan
  ncnn                    BSD 3-Clause                           https://github.com/Tencent/ncnn
  Qt 6                    LGPL-3.0                               https://www.qt.io/
  FFmpeg                  GPL-3.0 (this build)                   https://ffmpeg.org/
  libplacebo              LGPL-2.1-or-later                      https://code.videolan.org/videolan/libplacebo
  SDL 3, sdl2-compat      zlib                                   https://www.libsdl.org/
  hidapi                  BSD 3-Clause / HIDAPI                  https://github.com/libusb/hidapi
  Opus, SpeexDSP          BSD 3-Clause                           https://opus-codec.org/
  curl                    curl licence                           https://curl.se/
  OpenSSL                 Apache-2.0                             https://www.openssl.org/
  shaderc, SPIRV-Cross    Apache-2.0                             https://github.com/google/shaderc
  ICU, HarfBuzz, FreeType, zlib, zstd, brotli, libpng, PCRE2, GLib and the GCC runtime
  libraries, under their respective licences, as distributed by MSYS2 (https://www.msys2.org/).
EOF

cat > "$STAGE/OKUBENI.txt" <<EOF
Robz Play $VERSION
==================

GTA VI icin tasarlandi: PS5'te 30 FPS calisan oyunu bilgisayarinda 60 FPS akiciliginda
oynatir. PS5'e ev agi uzerinden Remote Play ile baglanir; aradaki kareleri bilgisayarinin
ekran karti uretir. Capture card gerekmez.

Calistirmak icin
  1. Bu klasoru istedigin bir yere cikar.
  2. RobzPlay.exe dosyasina cift tikla.
  3. PS5'te Ayarlar > Sistem > Uzaktan Oynama > "Uzaktan Oynamayi Etkinlestir"i ac.
  4. Konsol ana ekranda gorunur; "Kur"a basip PS5'in "Cihazi Bagla" ekranindaki kodu yaz.

Bilgisayarinda chiaki-ng kuruluysa Robz Play ilk acilista onun ayarlarini ve kayitli
konsollarini alir; yeniden kurulum gerekmez. chiaki-ng'nin kendi ayarlarina dokunulmaz.

Gerekenler
  - Windows 10 veya 11 (64 bit)
  - Vulkan destekli ayri bir ekran karti (tumlesik grafik yetmez)
  - PS5 (veya PS4) ile ayni ag; kablolu baglanti onerilir

Windows "bilinmeyen yayinci" uyarisi gosterebilir; uygulama imzali degildir.

Ayrintilar, kaynak kodu ve sorun bildirimi:
  https://github.com/kutsalkiremitci/robz-play

Lisans: AGPL-3.0 (licenses klasorune bak). Sony Interactive Entertainment, Rockstar Games
ya da Take-Two Interactive ile bir bagi yoktur.
EOF

echo "== Compressing"
( cd dist/release && /c/Windows/System32/tar.exe -a -c -f "$NAME.zip" "$NAME" )
ls -la "dist/release/$NAME.zip" | awk '{printf "== %s (%.1f MB)\n", $NF, $5 / 1048576}'
