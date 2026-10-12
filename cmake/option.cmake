# 设置选项用于选择构建目标平台
option(BUILD_FOR_ANDROID "Build for Android platform" OFF)
option(BUILD_FOR_DESKTOP "Build for Desktop platform" ON)

# 设置是否启用范例选项
option(EXAMPLE_PROJ "build example content" ON)

# 设置是否需要读取svg文件
option(ENABLE_SVG "ability to read SVG related files" ON)

# 设置是否启用opengl绘制相关功能
option(ENABLE_OPENGL "ability to use OpenGL related rendering functions" ON)

# 是否自动管理翻译更新
option(ENABLE_AUTO_Linguist "automatically manage translation updates" OFF)

# 启用播放器模块
option(ENABLE_MEDIA_PLAYER "using video streaming playback module" ON)

# FFmpeg 可选 H.264 编码器支持（用于转码模块；FFmpeg 自身不带 H.264 编码器）
# 说明：
#   1. 两项关闭时，转码仍可用，会自动回退到 MPEG-4 编码（MP4/MKV 等容器支持），界面会给出提示
#   2. 开启后若目标环境缺少依赖（如未安装 x264），FFmpeg 配置会自动回退到无该选项的基础配置，不会中断构建
option(FFMPEG_ENABLE_LIBX264 "try to enable H.264 encoder via libx264 when building FFmpeg" OFF)
option(FFMPEG_ENABLE_MEDIAFOUNDATION "try to enable H.264 encoder via Windows MediaFoundation when building FFmpeg" ON)

# 启用地图组件模块
option(ENABLE_MAP_COMPONENT "using map component" ON)
if (ENABLE_MAP_COMPONENT)
    option(USE_WEB_LEAFLET "using leaflet to show mapview" OFF)
    # option(USE_QML_LOCATION "using qtlocation to show mapview" ON)
endif (ENABLE_MAP_COMPONENT)

# 是否生成单文件分发包：把部署目录压缩后内嵌到启动器，产出一个可独立运行的 exe
option(PACKAGE_SINGLE_FILE "package the deploy dir into one standalone exe (Windows only)" OFF)

# 单文件包名（留空则用 <项目名>_single），产物放在 bin 目录
set(PACKAGE_OUTPUT_NAME "" CACHE STRING "Output name of the single-file package")