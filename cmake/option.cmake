# 设置是否启用范例选项
option(EXAMPLE_PROJ "build example content" ON)

if (EXAMPLE_PROJ)
    add_definitions(-DEXAMPLE_ON)

    find_package(Qt5Quick         REQUIRED)
    find_package(Qt5QuickWidgets  REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt5::Quick
        Qt5::QuickWidgets
    )
    list(APPEND QT_LIBS
        libQt5QuickWidgets.so.5
        libQt5Quick.so.5
        libQt5QmlModels.so.5
        libQt5Qml.so.5
        libQt5Network.so.5
    )
endif (EXAMPLE_PROJ)

# 设置是否需要读取svg文件
option(ENABLE_SVG "ability to read SVG related files" ON)

if (ENABLE_SVG)
    add_definitions(-DSVG_ENABLE)

    find_package(Qt5Svg           REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt5::Svg
    )
    list(APPEND QT_LIBS
        libQt5Svg.so.5
    )
endif (ENABLE_SVG)

# 设置是否启用opengl绘制相关功能
option(ENABLE_OPENGL "ability to use OpenGL related rendering functions" ON)

if (ENABLE_OPENGL)
    add_definitions(-DOPENGL_ENABLE)

    # find_package(OpenGL           REQUIRED)
    find_package(Qt5OpenGL        REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt5::OpenGL
        # OpenGL::GL   # 系统的 OpenGL 库
    )
    list(APPEND QT_LIBS
        libQt5OpenGL.so.5
    )
endif (ENABLE_OPENGL)