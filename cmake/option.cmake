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

    find_package(OpenGL           REQUIRED)
    find_package(Qt5OpenGL        REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt5::OpenGL
        OpenGL::GL   # 系统的 OpenGL 库
        OpenGL::GLU
    )
    list(APPEND QT_LIBS
        libQt5OpenGL.so.5
    )
endif (ENABLE_OPENGL)

# 是否自动管理翻译更新
option(ENABLE_AUTO_Linguist "automatically manage translation updates" OFF)

if (ENABLE_AUTO_Linguist)
    find_package(Qt5LinguistTools REQUIRED)

    # 提取可翻译字符串到 .ts 文件
    qt_add_lupdate(
        TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n/app_zh_CN.ts
        # SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    )
    # 编译 .ts 到 .qm
    qt_add_lrelease(
        TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n/app_zh_CN.ts
        QM_FILES_OUTPUT_VARIABLE QM_FILES
    )

    # 将 .qm 添加到资源
    qt_add_resources(translate_resources
        PREFIX "/ui/i18n/"      # 在应用程序虚拟文件系统中的​根路径前缀(:/ui/i18n/)
        FILES ${QM_FILES}
    )
    # 加入编译文件资源
    list(APPEND QT_SRCS
        ${translate_resources}
    )
endif (ENABLE_AUTO_Linguist)