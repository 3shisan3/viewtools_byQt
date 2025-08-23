#!/bin/bash
set -eo pipefail

# 参数解析
MODE="${1:-auto}"  # auto/prebuilt/source
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${PROJECT_ROOT}/build/tools/linuxdeployqt"
BINARY_PATH="${INSTALL_DIR}/linuxdeployqt"

# 架构映射（兼容多种架构命名）
declare -A ARCH_MAP=(
    ["x86_64"]="x86_64"
    ["amd64"]="x86_64"     # 兼容不同发行版
    ["aarch64"]="aarch64"
    ["armv7l"]="armhf"
)

# 检查工具是否可用（静默模式）
check_tool() {
    if [ -x "$1" ]; then
        if "$1" --appimage-extract-and-run --version &>/dev/null; then
            echo "$1"  # 仅返回路径
            return 0
        fi
    fi
    return 1
}

# 下载预编译版（使用新的官方仓库）
install_prebuilt() {
    local arch="${ARCH_MAP[$(uname -m)]}"
    [ -z "$arch" ] && return 1

    >&2 echo "Downloading linuxdeploy prebuilt for $arch..."
    mkdir -p "$INSTALL_DIR" || { >&2 echo "Failed to create $INSTALL_DIR"; return 1; }

    curl -sL "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${arch}.AppImage" \
        -o "$BINARY_PATH" || return 1

    chmod +x "$BINARY_PATH" || { >&2 echo "Failed to make executable"; return 1; }
    check_tool "$BINARY_PATH"
}

# 从源码编译（简化版，实际使用时可能需要更多依赖）
install_from_source() {
    >&2 echo "⚠️ Source compilation is time-consuming. Ensure you have dependencies installed."
    mkdir -p "$INSTALL_DIR/build" && cd "$INSTALL_DIR/build"

    # 克隆最新代码（避免缓存问题）
    git clone --depth 1 https://github.com/linuxdeploy/linuxdeploy.git src || return 1
    cmake -B build -S src -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" || return 1
    cmake --build build --parallel $(nproc) || return 1
    cmake --install build || return 1

    # 处理安装后的二进制路径
    if [ -x "$INSTALL_DIR/bin/linuxdeploy" ]; then
        mv "$INSTALL_DIR/bin/linuxdeploy" "$BINARY_PATH"
        check_tool "$BINARY_PATH"
    else
        >&2 echo "Binary not found after compilation"
        return 1
    fi
}

# 主逻辑（确保只输出路径）
case "$MODE" in
    prebuilt) 
        if ! output=$(install_prebuilt); then
            exit 1
        fi
        echo "$output"
        ;;
    source)
        if ! output=$(install_from_source); then
            exit 1
        fi
        echo "$output"
        ;;
    auto)
        if output=$(install_prebuilt); then
            echo "$output"
        else
            >&2 echo "Prebuilt failed, trying source build..."
            if output=$(install_from_source); then
                echo "$output"
            else
                >&2 echo "❌ All installation methods failed"
                exit 1
            fi
        fi
        ;;
    *) 
        >&2 echo "Invalid mode: $MODE. Use auto/prebuilt/source"
        exit 1
        ;;
esac