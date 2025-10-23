# 设置默认Qt安装路径
set(QT_PREFIX_PATH "" CACHE PATH "Default Qt install path")

# 如果未设置 QT_PREFIX_PATH，尝试自动查找
if(NOT QT_PREFIX_PATH)
    # 方法1：从环境变量查找
    if(DEFINED ENV{QTDIR})
        set(QT_PREFIX_PATH "$ENV{QTDIR}")
        message(STATUS "Using QTDIR environment variable: ${QT_PREFIX_PATH}")
    # 方法2：从编译器路径推断（适用于 MinGW）
    elseif(CMAKE_CXX_COMPILER MATCHES ".*msys.*")
        get_filename_component(COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(POSSIBLE_QT_DIR "${COMPILER_DIR}/../" ABSOLUTE)

        set(QT_PREFIX_PATH "${POSSIBLE_QT_DIR}")
        message(STATUS "Found Qt in MinGW directory: ${QT_PREFIX_PATH}")
    # 方法3：在常见安装位置查找
    else()
        set(COMMON_QT_PATHS
            "C:/Qt"
            "$ENV{USERPROFILE}/Qt"
            "C:/Program Files/Qt"
        )
        foreach(qt_path IN LISTS COMMON_QT_PATHS)
            if(EXISTS "${qt_path}")
                set(QT_PREFIX_PATH "${qt_path}")
                message(STATUS "Found Qt in common location: ${QT_PREFIX_PATH}")
                break()
            endif()
        endforeach()
    endif()
    
    # 如果仍然未找到，设置默认值但不报错，让后续的 find_package 处理
    if(NOT QT_PREFIX_PATH)
        message(STATUS "QT_PREFIX_PATH not set, relying on system PATH for Qt discovery")
    endif()
endif()

# 检查路径是否存在（仅在路径不为空时检查）
if(QT_PREFIX_PATH AND NOT EXISTS ${QT_PREFIX_PATH})
    message(WARNING "QT_PREFIX_PATH: '${QT_PREFIX_PATH}' does not exist. Relying on system PATH.")
    unset(QT_PREFIX_PATH CACHE)
endif()

# 将 Qt 路径添加到搜索目录中
if(QT_PREFIX_PATH)
    list(APPEND CMAKE_PREFIX_PATH ${QT_PREFIX_PATH})
endif()
message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH}")

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
        message(FATAL_ERROR "Neither Qt5 nor Qt6 found. Please install Qt5 or Qt6 or set QT_PREFIX_PATH.")
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
if (USE_WEB_LEAFLET)
    list(APPEND PROJECT_QRCS ${CMAKE_CURRENT_SOURCE_DIR}/extand/map_by_leaflet/resources.qrc)
elseif(USE_QML_LOCATION)
    list(APPEND PROJECT_QRCS ${CMAKE_CURRENT_SOURCE_DIR}/extand/map_by_qml/resources.qrc)
endif()

# 设置 UI 文件
file(GLOB_RECURSE PROJECT_UIS
    
)

# 指定使用的 Qt 版本
if(QT_VERSION EQUAL 6)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/custom/qt_version/qt6.cmake)
else()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/custom/qt_version/qt5.cmake)
endif()