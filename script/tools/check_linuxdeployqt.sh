#!/bin/bash
set -eo pipefail

# 参数解析
MODE="${1:-auto}"  # auto/prebuilt/source
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${PROJECT_ROOT}/build/tools/linuxdeployqt"
BINARY_PATH="${INSTALL_DIR}/linuxdeployqt"

# 架构映射
declare -A ARCH_MAP=(
    ["x86_64"]="x86_64"
    ["aarch64"]="arm64"
    ["armv7l"]="armhf"
)

# 检查工具是否可用
check_tool() {
    [ -x "$1" ] && "$1" --version 2>/dev/null | grep -q "linuxdeployqt"
}

# 下载预编译版
install_prebuilt() {
    local arch="${ARCH_MAP[$(uname -m)]}"
    [ -z "$arch" ] && return 1

    echo "Downloading prebuilt for $arch..."
    mkdir -p "$INSTALL_DIR"
    curl -sL "https://github.com/linuxdeploy/linuxdeployqt/releases/download/continuous/linuxdeployqt-${arch}.AppImage" \
        -o "$BINARY_PATH" || return 1
    chmod +x "$BINARY_PATH"
    check_tool "$BINARY_PATH"
}

# 从源码编译
install_from_source() {
    echo "Building from source..."
    mkdir -p "$INSTALL_DIR/build"
    cd "$INSTALL_DIR/build"

    # 克隆和构建
    git clone --depth 1 https://github.com/linuxdeploy/linuxdeployqt.git src || return 1
    cmake -B build -S src -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" || return 1
    cmake --build build --parallel $(nproc) || return 1
    cmake --install build || return 1

    # 验证
    [ -x "$INSTALL_DIR/bin/linuxdeployqt" ] && \
        mv "$INSTALL_DIR/bin/linuxdeployqt" "$BINARY_PATH"
    check_tool "$BINARY_PATH"
}

# 主逻辑
case "$MODE" in
    prebuilt) install_prebuilt ;;
    source) install_from_source ;;
    auto)
        if ! install_prebuilt; then
            echo "Prebuilt failed, trying source build..."
            install_from_source
        fi
        ;;
    *) echo "Invalid mode: $MODE" >&2; exit 1 ;;
esac

# 结果处理
if check_tool "$BINARY_PATH"; then
    echo "$BINARY_PATH"
else
    echo "❌ All installation methods failed" >&2
    exit 1
fi