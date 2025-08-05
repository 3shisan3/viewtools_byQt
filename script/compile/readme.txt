FFmpeg 交叉编译配置参数完整说明
基础配置参数
​​NDK_VERSION="r28c"​​
    作用：指定使用的Android NDK版本
    可选值：r21e, r22b, r23c, r25b等
    建议：使用较新版本以获得更好的兼容性
​​NDK_REVISION="28.2.13676358"​​
    作用：NDK的修订号
    说明：确保NDK版本与修订号匹配
​​FFMPEG_VERSION="7.0.2"​​
    作用：要编译的FFmpeg版本
    可选值：4.4, 5.1, 6.0, 7.0等
    建议：使用稳定版本
​​ANDROID_ABI="arm64-v8a"​​
    作用：目标架构ABI
    可选值：
    armeabi-v7a (32位ARM)
    arm64-v8a (64位ARM)
    x86 (32位Intel)
    x86_64 (64位Intel)
    建议：根据目标设备选择
​​ANDROID_API_LEVEL=24​​
    作用：最低支持的Android API级别
    范围：16-34+ (Android 4.1-Android 14+)
    建议：根据应用最低支持版本设置
​​OUTPUT_DIR="./ffmpeg-build"​​
    作用：编译输出目录
    说明：存放生成的库文件和头文件
​​FFMPEG_SOURCE_DIR="./ffmpeg"​​
    作用：FFmpeg源码目录
    说明：存放FFmpeg源代码

组件控制参数
​​ENABLE_COMPONENTS="avcodec avformat avutil swresample swscale"​​
    作用：启用的FFmpeg组件
    可用组件：
    avcodec: 编解码器核心
    avformat: 文件格式处理
    avutil: 工具函数
    swresample: 音频重采样
    swscale: 视频缩放和色彩空间转换
    avfilter: 滤镜处理
    avdevice: 设备输入输出
    postproc: 后期处理
    建议：按需启用，减少不必要的组件可减小库体积
​​DISABLE_COMPONENTS="avdevice avfilter postproc"​​
    作用：禁用的FFmpeg组件
    说明：与ENABLE_COMPONENTS互补，明确禁用不需要的组件
    编译选项参数
EXTRA_OPTIONS="--disable-symver"​​
作用：额外的配置选项
常用选项：
--disable-symver: 禁用符号版本控制
--enable-shared: 生成动态库
--disable-static: 不生成静态库
--disable-programs: 不编译可执行程序
--disable-doc: 不生成文档
--enable-x86asm: 启用x86汇编优化(x86/x86_64)
--enable-neon: 启用NEON优化(ARM)
--enable-jni: 启用JNI支持
建议：根据需求添加
工具链参数（自动设置）
​​ARCH​​
    作用：目标架构
    自动根据ANDROID_ABI设置
​​CPU​​
    作用：目标CPU类型
    自动根据ANDROID_ABI设置
​​CROSS_PREFIX​​
    作用：交叉编译工具前缀
    自动根据ANDROID_ABI设置
​​TOOLCHAIN_PREFIX​​
    作用：工具链前缀
    自动根据ANDROID_ABI设置
​​EXTRA_CFLAGS​​
作用：额外的C编译标志
常见设置：
-fPIC: 位置无关代码
-O3: 最高优化级别
-march=armv8-a: ARM64架构指定
-march=i686: x86架构指定
自动根据ANDROID_ABI设置基础标志
​​TOOLCHAIN_DIR​​
    作用：工具链目录
    自动从NDK路径派生
​​SYSROOT​​
    作用：系统根目录
    自动从NDK路径派生
编译控制参数
​​MAKE="make"​​
    作用：指定make程序
    可选值：make, gmake等
​​CPU_CORES​​
    作用：编译使用的CPU核心数
    自动检测系统核心数
环境变量
​​ANDROID_NDK_HOME​​
    作用：NDK安装路径
    默认值：HOME/dev/AndroidSdk/ndk/NDK_VERSION
    说明：可手动设置覆盖默认值