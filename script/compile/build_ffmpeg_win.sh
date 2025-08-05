#!/bin/bash
# FFmpeg 智能编译脚本 (Windows/Android)

# 配置区（必须使用正斜杠）=======================================
NDK_PATH="/c/Dev/Android/Sdk/ndk/26.1.10909125"     # NDK路径（必须正斜杠）
FFMPEG_VERSION="7.0.2"                              # FFmpeg版本
ANDROID_ABI="arm64-v8a"                             # 目标ABI
ANDROID_API_LEVEL=21                                # 最低API级别
OUTPUT_DIR="./ffmpeg-build"                         # 输出目录
FFMPEG_SOURCE_DIR="./ffmpeg"                        # 源码目录
ENABLE_COMPONENTS="avcodec avformat avutil swresample swscale" # 启用的组件
DISABLE_COMPONENTS="avdevice"                       # 禁用的组件
EXTRA_OPTIONS="--enable-gpl --enable-version3"      # 额外配置选项
# ==============================================================

# 初始化函数
init() {
    # 检测系统
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
        echo "[系统] Windows环境(Git Bash/Cygwin)"
        HOST_TAG="windows-x86_64"
        # 设置Shell选项
        set -o igncr  # 忽略CRLF行尾
        # 设置临时目录为当前目录
        export TMPDIR=$(pwd)/tmp
        mkdir -p "$TMPDIR"
    else
        echo "[错误] 此脚本设计在Windows的Git Bash或Cygwin中运行"
        exit 1
    fi

    # 创建输出目录
    mkdir -p "$OUTPUT_DIR" || {
        echo "[错误] 无法创建输出目录: $OUTPUT_DIR"
        exit 1
    }

    # 设置make路径
    if [[ -f "$NDK_PATH/prebuilt/$HOST_TAG/bin/make.exe" ]]; then
        export MAKE="$NDK_PATH/prebuilt/$HOST_TAG/bin/make.exe"
    else
        echo "[警告] 未找到NDK make工具，尝试使用系统make"
        if ! command -v make >/dev/null 2>&1; then
            echo "[错误] 找不到make命令"
            exit 1
        fi
        export MAKE="make"
    fi
}

# 检查工具链
check_tools() {
    echo "[检查] 验证编译工具链..."
    
    # 检查NDK工具链
    if [[ ! -d "$NDK_PATH/toolchains/llvm" ]]; then
        echo "[错误] NDK路径不正确或缺少LLVM工具链: $NDK_PATH"
        exit 1
    fi

    echo "[检查] 工具链验证通过"
}

# 源码处理函数
prepare_source() {
    if [[ -d "$FFMPEG_SOURCE_DIR" && -f "$FFMPEG_SOURCE_DIR/configure" ]]; then
        echo "[源码] 使用现有源码: $FFMPEG_SOURCE_DIR"
        # 确保使用正确的版本
        cd "$FFMPEG_SOURCE_DIR" && cd ..
        return 0
    fi

    echo "[源码] 未找到源码，开始下载FFmpeg ${FFMPEG_VERSION}..."
    
    # 克隆源码
    git clone --depth 1 --branch "n${FFMPEG_VERSION}" \
        https://git.ffmpeg.org/ffmpeg.git "$FFMPEG_SOURCE_DIR" || {
        echo "[错误] 源码下载失败"
        exit 1
    }

    echo "[源码] 下载完成"
}

# 配置编译参数
set_abi_config() {
    case "$ANDROID_ABI" in
        armeabi-v7a)
            ARCH="arm"
            CPU="armv7-a"
            CROSS_PREFIX="arm-linux-androideabi"
            TOOLCHAIN_PREFIX="armv7a-linux-androideabi"
            EXTRA_CFLAGS="-march=armv7-a -mfloat-abi=softfp -mfpu=neon"
            ;;
        arm64-v8a)
            ARCH="aarch64"
            CPU="armv8-a"
            CROSS_PREFIX="aarch64-linux-android"
            TOOLCHAIN_PREFIX="aarch64-linux-android"
            EXTRA_CFLAGS="-march=armv8-a"
            ;;
        x86)
            ARCH="i686"
            CPU="i686"
            CROSS_PREFIX="i686-linux-android"
            TOOLCHAIN_PREFIX="i686-linux-android"
            EXTRA_CFLAGS="-march=i686 -msse3 -mstackrealign -mfpmath=sse"
            ;;
        x86_64)
            ARCH="x86_64"
            CPU="x86-64"
            CROSS_PREFIX="x86_64-linux-android"
            TOOLCHAIN_PREFIX="x86_64-linux-android"
            EXTRA_CFLAGS=""
            ;;
        *)
            echo "[错误] 不支持的ABI: $ANDROID_ABI"
            exit 1
            ;;
    esac

    # 工具链路径
    TOOLCHAIN_DIR="$NDK_PATH/toolchains/llvm/prebuilt/$HOST_TAG"
    SYSROOT="$TOOLCHAIN_DIR/sysroot"
    
    # 添加组件配置
    for component in $ENABLE_COMPONENTS; do
        EXTRA_OPTIONS+=" --enable-$component"
    done
    
    for component in $DISABLE_COMPONENTS; do
        EXTRA_OPTIONS+=" --disable-$component"
    done
    
    # 添加链接器选项
    EXTRA_OPTIONS+=" --extra-ldflags=-Wl,-Bsymbolic"
}

# 配置FFmpeg
configure_ffmpeg() {
    echo "[配置] 生成编译配置..."
    
    cd "$FFMPEG_SOURCE_DIR" || exit 1

    # 清理旧配置
    if [[ -f "config.mak" ]]; then
        echo "[配置] 清理旧配置..."
        $MAKE distclean || true
    fi

    # 基础配置
    CONFIG_CMD=(
        ./configure
        $EXTRA_OPTIONS
        --disable-static
        --enable-shared
        --disable-programs
        --disable-doc
        --prefix="$OUTPUT_DIR"
        --target-os=android
        --arch=$ARCH
        --cpu=$CPU
        --enable-cross-compile
        --cross-prefix="$TOOLCHAIN_DIR/bin/$CROSS_PREFIX-"
        --strip="$TOOLCHAIN_DIR/bin/llvm-strip"
        --sysroot="$SYSROOT"
        --cc="$TOOLCHAIN_DIR/bin/${TOOLCHAIN_PREFIX}${ANDROID_API_LEVEL}-clang"
        --cxx="$TOOLCHAIN_DIR/bin/${TOOLCHAIN_PREFIX}${ANDROID_API_LEVEL}-clang++"
        --ar="$TOOLCHAIN_DIR/bin/llvm-ar"
        --nm="$TOOLCHAIN_DIR/bin/llvm-nm"
        --ranlib="$TOOLCHAIN_DIR/bin/llvm-ranlib"
        --extra-cflags="-fPIC -O3"
        --extra-ldflags="-fPIC -L$SYSROOT/usr/lib/$CROSS_PREFIX/$ANDROID_API_LEVEL"
        --disable-x86asm
        --disable-stripping
        --disable-asm
    )

    # 执行配置
    echo "[配置] 执行: ${CONFIG_CMD[*]}"
    "${CONFIG_CMD[@]}" > configure.log 2>&1 || {
        echo "[错误] 配置失败，请检查ffbuild/config.log"
        tail -n 20 configure.log
        exit 1
    }

    cd ..
    echo "[配置] 配置成功完成"
}

# 编译安装
build_ffmpeg() {
    echo "[编译] 开始编译FFmpeg..."
    
    cd "$FFMPEG_SOURCE_DIR" || exit 1

    # 获取CPU核心数
    CPU_CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

    # 编译
    echo "[编译] 使用 $CPU_CORES 线程编译..."
    "$MAKE" -j$CPU_CORES > build.log 2>&1 || {
        echo "[错误] 编译失败"
        tail -n 20 build.log
        exit 1
    }

    # 安装
    echo "[安装] 安装到 $OUTPUT_DIR"
    "$MAKE" install > install.log 2>&1 || {
        echo "[错误] 安装失败"
        tail -n 20 install.log
        exit 1
    }

    cd ..
    echo "[完成] FFmpeg 已成功构建并安装"
    
    # 输出构建信息
    echo "======================================="
    echo "FFmpeg 构建信息:"
    echo "版本: $FFMPEG_VERSION"
    echo "ABI: $ANDROID_ABI"
    echo "API 级别: $ANDROID_API_LEVEL"
    echo "启用组件: $ENABLE_COMPONENTS"
    echo "禁用组件: $DISABLE_COMPONENTS"
    echo "输出目录: $OUTPUT_DIR"
    echo "包含以下库文件:"
    ls -l "$OUTPUT_DIR/lib" || true
    echo "======================================="
}

# 主流程
main() {
    echo "======================================="
    echo "    FFmpeg 智能编译脚本 (Android)      "
    echo "======================================="
    
    # 初始化
    init
    check_tools
    set_abi_config
    
    # 准备源码
    prepare_source
    
    # 配置和编译
    configure_ffmpeg
    build_ffmpeg
    
    # 清理临时目录
    if [[ -d "$TMPDIR" ]]; then
        rm -rf "$TMPDIR"
    fi
}

# 执行主流程
main