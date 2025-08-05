#!/bin/bash
# FFmpeg 智能编译脚本 (Linux/Android)

# 配置区 ========================================
NDK_VERSION="r28c"                          # NDK版本号
NDK_REVISION="28.2.13676358"                # NDK修订号
FFMPEG_VERSION="7.0.2"                      # FFmpeg版本
ANDROID_ABI="arm64-v8a"                     # 目标ABI
ANDROID_API_LEVEL=24                        # 最低API级别
OUTPUT_DIR="./ffmpeg-build"                 # 输出目录
FFMPEG_SOURCE_DIR="./ffmpeg"                # 源码目录
ENABLE_COMPONENTS="avcodec avformat avutil swresample swscale"  # 启用的组件
DISABLE_COMPONENTS="avdevice avfilter postproc"     # 禁用的组件
EXTRA_OPTIONS="--disable-symver"            # 额外配置选项
# ==============================================

# 初始化函数
init() {
    echo "[系统] Linux环境"
    HOST_TAG="linux-x86_64"

    # 脚本运行的当前目录
    SH_RUN_PATH=$(pwd)
    
    # 设置NDK路径
    if [ -z "$ANDROID_NDK_HOME" ]; then
        ANDROID_NDK_HOME="$HOME/dev/AndroidSdk/ndk/$NDK_VERSION"
    fi
    
    # # 创建输出目录
    # mkdir -p "$OUTPUT_DIR" || {
    #     echo "[错误] 无法创建输出目录: $OUTPUT_DIR"
    #     exit 1
    # }
    
    # 设置make路径
    export MAKE="make"
    if ! command -v make >/dev/null 2>&1; then
        echo "[错误] 找不到make命令"
        exit 1
    fi
}

# 检查并下载NDK
check_ndk() {
    echo "[检查] 验证NDK工具链..."
    
    if [[ ! -d "$ANDROID_NDK_HOME" ]]; then
        echo "[NDK] 未找到NDK，开始下载NDK $NDK_VERSION..."
        
        mkdir -p "$HOME/dev/AndroidSdk/ndk" || {
            echo "[错误] 无法创建NDK目录"
            exit 1
        }
        
        cd "$HOME/dev/AndroidSdk/ndk" || exit 1
        
        # 构建下载URL
        NDK_DOWNLOAD_URL="https://googledownloads.cn/android/repository/android-ndk-${NDK_VERSION}-linux.zip"
        
        echo "[NDK] 从 $NDK_DOWNLOAD_URL 下载..."
        
        # 检查wget是否可用
        if ! command -v wget >/dev/null 2>&1; then
            echo "[错误] 需要安装wget工具"
            echo "在Ubuntu/Debian上运行: sudo apt-get install wget"
            echo "在CentOS/RHEL上运行: sudo yum install wget"
            exit 1
        fi
        
        # 检查unzip是否可用
        if ! command -v unzip >/dev/null 2>&1; then
            echo "[错误] 需要安装unzip工具"
            echo "在Ubuntu/Debian上运行: sudo apt-get install unzip"
            echo "在CentOS/RHEL上运行: sudo yum install unzip"
            exit 1
        fi
        
        # 下载并解压
        wget --tries=3 --timeout=30 --no-check-certificate "$NDK_DOWNLOAD_URL" && \
        unzip -q "android-ndk-${NDK_VERSION}-linux.zip" && \
        mv "android-ndk-${NDK_VERSION}" "$NDK_VERSION" && \
        rm "android-ndk-${NDK_VERSION}-linux.zip" || {
            echo "[错误] NDK下载或解压失败"
            exit 1
        }
        
        echo "[NDK] 下载完成并解压到: $ANDROID_NDK_HOME"
        
        # 恢复原始目录
        cd "$SH_RUN_PATH" || exit 1
    fi

    # 验证NDK结构
    if [[ ! -d "$ANDROID_NDK_HOME/toolchains/llvm" ]]; then
        echo "[错误] NDK路径不正确或缺少LLVM工具链: $ANDROID_NDK_HOME"
        exit 1
    fi

    echo "[检查] NDK验证通过"
}

# 源码处理函数
prepare_source() {
    if [[ -d "$FFMPEG_SOURCE_DIR" && -f "$FFMPEG_SOURCE_DIR/configure" ]]; then
        echo "[源码] 使用现有源码: $FFMPEG_SOURCE_DIR"
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
    TOOLCHAIN_DIR="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG"
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
    
    cd "$SH_RUN_PATH/$FFMPEG_SOURCE_DIR" || exit 1

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
        --extra-cflags="-fPIC -O3 $EXTRA_CFLAGS"
        --extra-ldflags="-fPIC -L$SYSROOT/usr/lib/$CROSS_PREFIX/$ANDROID_API_LEVEL"
        --disable-x86asm
    )

    # 执行配置
    echo "[配置] 执行: ${CONFIG_CMD[*]}"
    "${CONFIG_CMD[@]}" > configure.log 2>&1 || {
        echo "[错误] 配置失败，请检查ffbuild/config.log"
        tail -n 20 configure.log
        exit 1
    }

    echo "[配置] 配置成功完成"
}

# 编译安装
build_ffmpeg() {
    echo "[编译] 开始编译FFmpeg..."
    
    cd "$SH_RUN_PATH/$FFMPEG_SOURCE_DIR" || exit 1

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
    check_ndk
    set_abi_config
    
    # 准备源码
    prepare_source
    
    # 配置和编译
    configure_ffmpeg
    build_ffmpeg
}

# 执行主流程
main