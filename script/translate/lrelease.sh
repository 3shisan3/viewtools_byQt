#!/bin/bash
# 用法：./lrelease.sh [QT安装目录] [语言包目录] [输出目录]
# 示例：
# 1. 使用默认路径：./lrelease.sh
# 2. 全自定义路径：./lrelease.sh /home/shisan/Qt/5.15.2/gcc_64/bin ./ui/i18n ./ui/i18n

basepath=$(cd `dirname $0`; pwd)

# 默认配置
DEFAULT_QT_PATH="/opt/Qt5.15.2/5.15.2/gcc_64/bin"
DEFAULT_LANG_DIR="${basepath}/../../language"
DEFAULT_INSTALL_DIR="${basepath}/../../bin/resource/language"

# 参数配置（使用传入参数或默认值）
QT_PATH="${1:-$DEFAULT_QT_PATH}"
LANGUAGE_PATH="${2:-$DEFAULT_LANG_DIR}"
INSTALL_DIR="${3:-$DEFAULT_INSTALL_DIR}"

# 创建必要目录
mkdir -p "${LANGUAGE_PATH}"
mkdir -p "${INSTALL_DIR}"

# 检查lrelease是否存在
if [ ! -f "${QT_PATH}/lrelease" ]; then
    echo "错误: 在 ${QT_PATH} 中找不到 lrelease"
    exit 1
fi

# 转换ts到qm文件
echo "正在转换语言文件..."
find "${LANGUAGE_PATH}" -maxdepth 1 -name '*.ts' -print0 | while IFS= read -r -d $'\0' ts_file; do
    ts_name=$(basename "${ts_file%.*}")
    
    # 执行转换
    if "${QT_PATH}/lrelease" "${ts_file}" -qm "${LANGUAGE_PATH}/${ts_name}.qm"; then
        echo "转换成功: ${ts_name}.ts -> ${ts_name}.qm"
    else
        echo "转换失败: ${ts_name}.ts"
        exit 2
    fi
done

# 拷贝语言文件
echo "正在拷贝语言文件到: ${INSTALL_DIR}"
if cp -v "${LANGUAGE_PATH}"/*.qm "${INSTALL_DIR}"; then
    echo "拷贝完成，共拷贝 $(ls "${INSTALL_DIR}"/*.qm 2>/dev/null | wc -l) 个语言文件"
else
    echo "拷贝过程中发生错误"
    exit 3
fi

# 最终状态检查
if [ $(ls "${INSTALL_DIR}"/*.qm 2>/dev/null | wc -l) -gt 0 ]; then
    echo "[最终状态] 语言包准备就绪"
    exit 0
else
    echo "[最终状态] 警告: 未找到任何语言文件"
    exit 4
fi