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

# 启用地图组件模块
option(ENABLE_MAP_COMPONENT "using map component" ON)
if (ENABLE_MAP_COMPONENT)
    option(USE_WEB_LEAFLET "using leaflet to show mapview" OFF)
    # option(USE_QML_LOCATION "using qtlocation to show mapview" ON)
endif (ENABLE_MAP_COMPONENT)