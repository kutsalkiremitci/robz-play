#!/usr/bin/env bash
# Robz Play - build and stage a runnable copy under ./dist/RobzPlay.
# Run from an MSYS2 MINGW64 shell after scripts/robzplay-setup-msys2.sh.
#
#   scripts/robzplay-build.sh            incremental build, refresh the exe in dist/
#   scripts/robzplay-build.sh --deploy   also re-collect DLLs / Qt plugins into dist/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f build/build.ninja ]]; then
	cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIAKI_ENABLE_CLI=OFF
fi
cmake --build build --target chiaki

if [[ ! -d dist/RobzPlay || "${1:-}" == "--deploy" ]]; then
	mkdir -p dist
	scripts/deploy-windows-msys2.sh dist/RobzPlay build/gui/RobzPlay.exe \
		build/third-party/cpp-steam-tools /mingw64 gui/src/qml
elif ! cp build/gui/RobzPlay.exe dist/RobzPlay/ 2>/dev/null; then
	# Robz Play is running and holds its executable: leave the new one where start.bat
	# picks it up the next time the application is started
	mkdir -p dist/RobzPlay-next
	cp build/gui/RobzPlay.exe dist/RobzPlay-next/
	echo "== Robz Play is running: new build waits in dist/RobzPlay-next (start.bat applies it)"
fi
# frame generation models live next to the executable
mkdir -p dist/RobzPlay/models
cp -r robzfg/models/. dist/RobzPlay/models/
echo "== dist/RobzPlay/RobzPlay.exe ready"
