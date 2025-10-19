#!/bin/bash

# 参数检查
if [ $# -ne 3 ]; then
    echo "Usage: $0 <ucrt_bin_dir> <target_dir> <exe_path>"
    exit 1
fi

UCRT_BIN_DIR=$1
TARGET_DIR=$2
EXE_PATH=$3

# 创建目标目录
mkdir -p "${TARGET_DIR}"

# 使用 ldd 分析依赖（适用于ucrt64）
echo "Analyzing dependencies for ${EXE_PATH}..."
DEPENDENCIES=$(ldd "${EXE_PATH}" | awk '$3 ~ /ucrt64|clang64|msys64/ {print $3}' | sort -u)

# 如果没有找到依赖，尝试使用objdump作为备用方案
if [ -z "${DEPENDENCIES}" ]; then
    echo "Using objdump as fallback for dependency analysis..."
    DEPENDENCIES=$(objdump -p "${EXE_PATH}" | grep "DLL Name:" | awk '{print $3}' | grep -E "lib.*\.dll|.*ucrt.*\.dll|.*clang.*\.dll" | sort -u)
fi

# 复制依赖的 DLL
COPIED_COUNT=0
for DLL in ${DEPENDENCIES}; do
    DLL_NAME=$(basename "${DLL}")
    
    # 首先尝试从原始路径复制
    if [ -f "${DLL}" ]; then
        cp -v "${DLL}" "${TARGET_DIR}"
        COPIED_COUNT=$((COPIED_COUNT + 1))
    else
        # 如果在原路径找不到，尝试在ucrt64目录下查找
        UCRT_DLL="${UCRT_BIN_DIR}/${DLL_NAME}"
        if [ -f "${UCRT_DLL}" ]; then
            cp -v "${UCRT_DLL}" "${TARGET_DIR}"
            COPIED_COUNT=$((COPIED_COUNT + 1))
        else
            echo "Warning: Missing DLL - ${DLL_NAME}"
        fi
    fi
done

# 确保复制ucrt核心运行时库
UCRT_CORE_DLLS=(
    "libstdc++-6.dll"
    "libgcc_s_seh-1.dll"
    "libwinpthread-1.dll"
    "libgcc_s_dw2-1.dll"  # 备用
)

for CORE_DLL in "${UCRT_CORE_DLLS[@]}"; do
    if [ -f "${UCRT_BIN_DIR}/${CORE_DLL}" ] && [ ! -f "${TARGET_DIR}/${CORE_DLL}" ]; then
        cp -v "${UCRT_BIN_DIR}/${CORE_DLL}" "${TARGET_DIR}"
        COPIED_COUNT=$((COPIED_COUNT + 1))
    fi
done

echo "UCRT64 DLL deployment completed. Copied ${COPIED_COUNT} DLLs."