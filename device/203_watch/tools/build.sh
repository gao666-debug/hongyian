#!/bin/sh
# 编译 WS63 手表固件（app 增量编译）
#
# 关键：每次都要先删掉 libsamples.a。demo 目录的白名单只决定"编译哪些 .c"，
# 旧的 .obj 会留在静态库里，换入口文件后会出现两个 app_run、镜像跑错应用。
set -e

SDK_ROOT=${SDK_ROOT:-/d/ws/src}
OUT=${SDK_ROOT}/output/ws63/acore/ws63-liteos-app
LOG=${BUILD_LOG:-/d/ws63/build.log}
PY=${PYTHON:-/d/ws63/tools/python311/python.exe}

export PATH="${SDK_ROOT}/tools/bin/compiler/riscv/cc_riscv32_musl_105/cc_riscv32_musl_fp_win/bin:${SDK_ROOT}/tools/bin/compiler/riscv/cc_riscv32_musl_105/cc_riscv32_musl_fp_win/riscv32-linux-musl/bin:/d/ws63/tools/cmake/bin:/d/ws63/tools/bin:/d/ws63/tools/python311:$PATH"
export PYTHONPATH=
export PYTHONHOME=

rm -f "${OUT}/application/samples/libsamples.a"

cd "${SDK_ROOT}" || exit 1
${PY} build.py -nhso -j8 ws63-liteos-app > "${LOG}" 2>&1
echo "BUILD_EXIT=$?"

echo "--- errors ---"
grep -E "error:|error C|Error 1|fatal" "${LOG}" | head -12
echo "--- tail ---"
tail -3 "${LOG}"
