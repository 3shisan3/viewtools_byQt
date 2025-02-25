# 设置默认的 Qt 目录
set(QT_PREFIX_PATH "/home/shisan/Qt" CACHE PATH "Default Qt install path")

# 检查路径是否存在
if(NOT EXISTS ${QT_PREFIX_PATH})
    message(FATAL_ERROR "QT_PREFIX_PATH: ${QT_PREFIX_PATH} does not exist.")
endif()

# 尝试查找 Qt6
find_package(Qt6 QUIET COMPONENTS Core Widgets Gui OpenGL)

if(Qt6_FOUND)
    message(STATUS "Using Qt6")
    set(QT_VERSION 6)
else()
    # 如果 Qt6 未找到，尝试查找 Qt5
    find_package(Qt5 QUIET COMPONENTS Core Widgets Gui OpenGL)
    if(Qt5_FOUND)
        message(STATUS "Using Qt5")
        set(QT_VERSION 5)
    else()
        message(FATAL_ERROR "Neither Qt5 nor Qt6 found. Please install Qt5 or Qt6.")
    endif()
endif()

# 将 Qt 路径添加到搜索目录中
list(APPEND CMAKE_PREFIX_PATH ${QT_PREFIX_PATH})

# 若要使用 Qt 的 UI 相关功能，需开启，否则无法找到对应 ui_xxx.h
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 设置 MOC，如果要使用信号槽，则必须开启该功能
set(CMAKE_AUTOMOC ON)

# 设置资源文件
file(GLOB_RECURSE PROJECT_QRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/res/res.qrc
)

# 添加资源文件
if(QT_VERSION EQUAL 6)
    qt6_add_resources(PROJECT_MOC_QRCS ${PROJECT_QRCS})
else()
    qt5_add_resources(PROJECT_MOC_QRCS ${PROJECT_QRCS})
endif()

# 设置 UI 文件
file(GLOB_RECURSE PROJECT_UIS
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/about/*.ui
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/shortcut/*.ui
)

# 添加 UI 文件
if(QT_VERSION EQUAL 6)
    qt6_wrap_ui(PROJECT_MOC_UIS ${PROJECT_UIS})
else()
    qt5_wrap_ui(PROJECT_MOC_UIS ${PROJECT_UIS})
endif()

# Qt 的资源
set(QT_SRCS
    ${PROJECT_MOC_QRCS}
    ${PROJECT_MOC_UIS}
)

# 设置 Qt 的依赖库
set(QT_DEPEND_LIBS "")

# 根据 Qt 版本添加对应的库
if(QT_VERSION EQUAL 6)
    list(APPEND QT_DEPEND_LIBS
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
        Qt6::OpenGL
    )
else()
    list(APPEND QT_DEPEND_LIBS
        Qt5::Core
        Qt5::Widgets
        Qt5::Gui
        Qt5::OpenGL
    )
endif()