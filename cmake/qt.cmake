# 设置默认的 Qt 目录
set(QT_PREFIX_PATH "C:\\workspace\\dev_tools\\Qt\\6.8.3\\mingw_64" CACHE PATH "Default Qt install path")

# 检查路径是否存在
if(NOT EXISTS ${QT_PREFIX_PATH})
    message(FATAL_ERROR "QT_PREFIX_PATH: ${QT_PREFIX_PATH} does not exist.")
endif()

# 将 Qt 路径添加到搜索目录中
list(APPEND CMAKE_PREFIX_PATH ${QT_PREFIX_PATH})

# 尝试查找 Qt6
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
if(Qt6_FOUND)
    message(STATUS "Using Qt6")
    set(QT_VERSION 6)
else()
    # 如果 Qt6 未找到，尝试查找 Qt5
    find_package(Qt5 QUIET COMPONENTS Core Gui Widgets)
    if(Qt5_FOUND)
        message(STATUS "Using Qt5")
        set(QT_VERSION 5)
    else()
        message(FATAL_ERROR "Neither Qt5 nor Qt6 found. Please install Qt5 or Qt6.")
    endif()
endif()

# 添加代码使用宏区分
add_compile_definitions(QT_VERSION_MAJOR=${QT_VERSION})

# 若要使用 Qt 的 UI 相关功能，需开启，否则无法找到对应 ui_xxx.h
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 设置MOC，如果要使用信号槽，则必须开启该功能
set(CMAKE_AUTOMOC ON)
# 设置UIC，将.ui 文件（Qt Designer 生成的界面文件）转换为对应的 C++ 头文件
set(CMAKE_AUTOUIC ON)
# 设置RCC，将.qrc 文件（Qt 资源文件）编译为 C++ 代码
set(CMAKE_AUTORCC ON)

# 设置资源文件
file(GLOB PROJECT_QRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/res/res.qrc
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ui.qrc
)

# 设置 UI 文件
file(GLOB_RECURSE PROJECT_UIS
    
)

# 指定使用的 Qt 版本
if(QT_VERSION EQUAL 6)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/custom/qt_version/qt6.cmake)
else()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/custom/qt_version/qt5.cmake)
endif()
