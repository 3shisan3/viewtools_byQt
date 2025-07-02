#!/bin/bash

# 参数检查
if [ $# -ne 3 ]; then
    echo "Usage: $0 <mingw_bin_dir> <target_dir> <exe_path>"
    exit 1
fi

MINGW_BIN_DIR=$1
TARGET_DIR=$2
EXE_PATH=$3

# 创建目标目录
mkdir -p "${TARGET_DIR}"

# 使用 objdump 分析依赖
echo "Analyzing dependencies for ${EXE_PATH}..."
DEPENDENCIES=$("${MINGW_BIN_DIR}/objdump.exe" -p "${EXE_PATH}" | grep "DLL Name: lib.*.dll" | sed "s/.*DLL Name: //")

# 复制依赖的 DLL
for DLL in ${DEPENDENCIES}; do
    SRC="${MINGW_BIN_DIR}/${DLL}"
    if [ -f "${SRC}" ]; then
        cp -v "${SRC}" "${TARGET_DIR}"
    else
        echo "Warning: Missing DLL - ${SRC}"
    fi
done

echo "MinGW DLL deployment completed"