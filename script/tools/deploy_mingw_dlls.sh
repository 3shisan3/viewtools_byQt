#!/bin/bash

# 参数检查
if [ $# -ne 3 ]; then
    echo "Usage: $0 <mingw_bin_dir> <target_dir> <exe_path>"
    exit 1
fi

# 打印输入参数(调试使用)
# echo "============ input arguments ============"
# echo "MinGW Dir: $1"
# echo "Target Dir:   $2"
# echo "The EXE File: $3"
# echo "=================================="

MINGW_BIN_DIR=$1
TARGET_DIR=$2
EXE_PATH=$3

# 创建目标目录
mkdir -p "${TARGET_DIR}"

# 使用 objdump 分析依赖
echo "Analyzing dependencies for ${EXE_PATH}..."
# 命令解释：
# 1. objdump -p：显示可执行文件的程序头信息（包含依赖的DLL）
# 2. grep "DLL Name: lib*.dll"：筛选出以lib开头的DLL（MinGW核心运行时库）
# 3. sed "s/.*DLL Name: //"：提取纯DLL文件名（去掉"DLL Name: "前缀）
DEPENDENCIES=$(ldd "${EXE_PATH}" | awk '$3 ~ /mingw64/ {print $3}' | sort -u)

# 打印识别到的依赖库
echo "Identified dependency libraries:"
printf '%s\n' "${DEPENDENCIES[@]}"
echo "============================"

# 复制依赖的 DLL
for DLL in ${DEPENDENCIES}; do
    DLL_NAME=$(basename "${DLL}")
    # echo "Copying ${DLL_NAME}"

    if [ -f "${DLL}" ]; then
        cp -v "${DLL}" "${TARGET_DIR}"
    else
        # 如果在原路径找不到，尝试在MinGW目录下查找
        MINGW_DLL="${MINGW_BIN_DIR}/${DLL_NAME}"
        if [ -f "${MINGW_DLL}" ]; then
            cp -v "${MINGW_DLL}" "${TARGET_DIR}"
        else
            echo "Warning: Missing DLL - ${DLL} And ${MINGW_DLL}"
        fi
    fi
done

echo "MinGW DLL deployment completed"