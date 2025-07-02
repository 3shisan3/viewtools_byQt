# 查找Qt依赖
find_package(Qt6 COMPONENTS Core Widgets Gui REQUIRED)

# 添加资源文件
qt6_add_resources(PROJECT_MOC_QRCS ${PROJECT_QRCS})

# 添加ui文件
# qt6_wrap_ui(PROJECT_MOC_UIS ${PROJECT_UIS})

# qt的资源
set(QT_SRCS
    ${PROJECT_MOC_QRCS}
    ${PROJECT_MOC_UIS}
)

# 设置Qt的依赖库
set(QT_DEPEND_LIBS
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
)

# 配置项额外的依赖
if (EXAMPLE_PROJ)
    add_definitions(-DEXAMPLE_ON)

    find_package(Qt6 COMPONENTS Quick QuickWidgets QuickControls2 REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt6::Quick
        Qt6::QuickWidgets
        Qt6::QuickControls2
    )
endif (EXAMPLE_PROJ)

if (ENABLE_SVG)
    add_definitions(-DSVG_ENABLE)

    find_package(Qt6 COMPONENTS Svg REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt6::Svg
    )
endif (ENABLE_SVG)

if (ENABLE_MEDIA_PLAYER)
    add_definitions(-DMEDIA_PLAYER_ENABLE)

    find_package(Qt6 COMPONENTS Multimedia MultimediaWidgets REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt6::Multimedia
        Qt6::MultimediaWidgets 
    ) 
endif()

if (ENABLE_OPENGL)
    add_definitions(-DOPENGL_ENABLE)
    
    find_package(Qt6 COMPONENTS OpenGL OpenGLWidgets REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt6::OpenGL
        Qt6::OpenGLWidgets
    )

    if(ANDROID)
        # Android 使用 OpenGL ES
        find_library(OPENGLES2_LIBRARY GLESv2)
        find_library(OPENGLES3_LIBRARY GLESv3)
        find_library(EGL_LIBRARY EGL)
        add_definitions(-DQT_OPENGL_ES_2)
        
        list(APPEND QT_DEPEND_LIBS
            ${OPENGLES2_LIBRARY}
            ${EGL_LIBRARY}
        )
    else()
        # 非 Android 平台（Windows/Linux/macOS）使用标准 OpenGL
        find_package(OpenGL REQUIRED)
        list(APPEND QT_DEPEND_LIBS
            OpenGL::GL   # 系统的 OpenGL 库
            OpenGL::GLU
        )
    endif()
endif (ENABLE_OPENGL)

if (ENABLE_AUTO_Linguist)
    find_package(Qt6 COMPONENTS LinguistTools REQUIRED)

    # 提取可翻译字符串到 .ts 文件
    qt6_add_lupdate(
        TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n/app_zh_CN.ts
        # SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    )
    # 编译 .ts 到 .qm
    qt6_add_lrelease(
        TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n/app_zh_CN.ts
        QM_FILES_OUTPUT_VARIABLE QM_FILES
    )

    # 将 .qm 添加到资源
    qt6_add_resources(translate_resources
        PREFIX "/ui/i18n/"      # 在应用程序虚拟文件系统中的根路径前缀(:/ui/i18n/)
        FILES ${QM_FILES}
    )
    # 加入编译文件资源
    list(APPEND QT_SRCS
        ${translate_resources}
    )
endif (ENABLE_AUTO_Linguist)