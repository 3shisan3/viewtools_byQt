#!/bin/bash
# 用法：./lupdate.sh <QT安装目录> <扫描代码目录> <TS输出目录> [语言列表]
# 示例：
# 1. 仅更新现有ts文件： 
#    ./lupdate.sh /home/shisan/Qt/5.15.2/gcc_64/bin ./src ./ui/i18n
# 2. 指定多语言生成：
#    ./lupdate.sh /home/shisan/Qt/5.15.2/gcc_64/bin ./src ./translations "zh_CN en_US ja_JP"

basepath=$(cd `dirname $0`; pwd)

# 参数检查
if [ $# -lt 3 ]; then
    echo "Usage: $0 <QT_INSTALL_DIR> <SOURCE_DIR> <TS_OUTPUT_DIR> [LANGUAGES]"
    echo "Example: $0 /opt/Qt5.14.2/5.14.2/gcc_64/bin ./src ./translations \"zh_CN en_US\""
    exit 1
fi

QT_PATH="$1"
SOURCE_DIR="$2"
TS_OUTPUT_DIR="$3"
LANGUAGES=($4)  # 可选参数转换为数组

# 创建输出目录
mkdir -p "${TS_OUTPUT_DIR}"

# 检查lupdate是否存在
if [ ! -f "${QT_PATH}/lupdate" ]; then
    echo "错误: 在 ${QT_PATH} 中找不到 lupdate"
    exit 2
fi

# 构建lupdate命令
if [ ${#LANGUAGES[@]} -gt 0 ]; then
    # 多语言模式：生成指定语言的ts文件
    TS_FILES=()
    for lang in "${LANGUAGES[@]}"; do
        TS_FILES+=("${TS_OUTPUT_DIR}/${lang}.ts")
    done
    CMD="${QT_PATH}/lupdate -no-obsolete \"${SOURCE_DIR}\" -ts ${TS_FILES[*]}"
else
    # 兼容模式：更新现有所有ts文件
    CMD="${QT_PATH}/lupdate -no-obsolete \"${SOURCE_DIR}\" -ts ${TS_OUTPUT_DIR}/*.ts"
fi

# 执行命令
echo "执行命令: ${CMD}"
eval "${CMD}"

# 结果处理
if [ $? -eq 0 ]; then
    if [ ${#LANGUAGES[@]} -gt 0 ]; then
        echo "[成功] 已更新指定语言文件: ${LANGUAGES[*]}"
    else
        echo "[成功] 已更新所有现有ts文件"
    fi
else
    echo "[错误] 翻译更新失败"
    exit 3
fi