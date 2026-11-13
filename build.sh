#!/bin/bash
set -euo pipefail

export VITASDK=${VITASDK:-/usr/local/vitasdk}
export PATH=$VITASDK/bin:$PATH

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJ"

echo "=== Building Zombiewood Vita ==="
rm -rf build
mkdir -p build
cd build
# ZW_CMAKE_ARGS: extra cmake options, e.g. ZW_CMAKE_ARGS=-DZW_CALLPROF=1 for the measurement build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake ${ZW_CMAKE_ARGS:-}
make -j$(nproc) zombiewood.self-self 2>&1 | tee build.log

echo "=== Packaging Zombiewood VPK ===" | tee -a build.log
vita-mksfoex   -s TITLE_ID=ZMBW00001   -s APP_VER=01.00   "Zombiewood"   "zombiewood.vpk_param.sfo" 2>&1 | tee -a build.log

# Loader only: no Gameloft code or data in the VPK. The bubble/LiveArea art (sce_sys/, not in
# git) is added when present. Packed here rather than by the CMake vpk target, which does not
# quote these paths and fails when the checkout path contains spaces.
ART=()
if [ -f "$PROJ/sce_sys/icon0.png" ]; then
  for f in icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml; do
    ART+=(-a "$PROJ/sce_sys/$f=sce_sys/$f")
  done
else
  echo "note: no sce_sys/ art found, the VPK gets the default bubble" | tee -a build.log
fi
vita-pack-vpk   -s "zombiewood.vpk_param.sfo"   -b "zombiewood.self"   "${ART[@]}"   "zombiewood.vpk" 2>&1 | tee -a build.log

if [ -f zombiewood.vpk ]; then
  echo ""
  echo "=============================="
  echo " SUCCESS: zombiewood.vpk ready"
  echo " Path: $PROJ/build/zombiewood.vpk"
  echo "=============================="
else
  echo "BUILD FAILED — check build.log"
  exit 1
fi
