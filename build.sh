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
vita-mksfoex \
  -s TITLE_ID=ZMBW00001 \
  -s APP_VER=01.00 \
  "Zombiewood" \
  "zombiewood.vpk_param.sfo" 2>&1 | tee -a build.log

# v5.13: loader only - no Gameloft files in the VPK. The player copies libZombieWood.so,
# libgenerator.so and both OBBs from their own Zombiewood 1.5.3 to ux0:data/zombiewood/.
vita-pack-vpk \
  -s "zombiewood.vpk_param.sfo" \
  -b "zombiewood.self" \
  "zombiewood.vpk" 2>&1 | tee -a build.log

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
