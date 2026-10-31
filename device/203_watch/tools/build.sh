#!/bin/sh
# 编译 WS63 手表固件（SPI 提速版）
set -e

export PATH="/d/ws/src/tools/bin/compiler/riscv/cc_riscv32_musl_105/cc_riscv32_musl_fp_win/bin:/d/ws/src/tools/bin/compiler/riscv/cc_riscv32_musl_105/cc_riscv32_musl_fp_win/riscv32-linux-musl/bin:/d/ws63/tools/cmake/bin:/d/ws63/tools/bin:/d/ws63/tools/python311:$PATH"
export PYTHONPATH=
export PYTHONHOME=

cd /d/ws/src || exit 1
/d/ws63/tools/python311/python.exe build.py -nhso -j8 ws63-liteos-app > /d/ws63/build_v15.log 2>&1
echo "BUILD_EXIT=$?"

echo "--- errors ---"
grep -E "error:|error C|Error 1|fatal" /d/ws63/build_v15.log | head -12
echo "--- tail ---"
tail -3 /d/ws63/build_v15.log
