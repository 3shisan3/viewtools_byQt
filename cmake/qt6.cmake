# 设置默认的qt目录
set(QT_PREFIX_PATH "/home/shisan/Qt/6.6.0/gcc_64" CACHE PATH "Default qt install path")

# 检查路径是否存在
if(NOT EXISTS ${QT_PREFIX_PATH})
    message(FATAL_ERROR "QT_PREFIX_PATH: ${QT_PREFIX_PATH} is not exist.")
endif()

# 将qt路径添加到搜索目录中
list(APPEND CMAKE_PREFIX_PATH ${QT_PREFIX_PATH})

# 查找Qt依赖
find_package(Qt6 COMPONENTS Core Widgets Gui OpenGL REQUIRED)

# 若要使用qt的ui相关功能，需开启，否则无找到对应ui_xxx.h
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 设置MOC，如果要使用信号槽，则必须开启该功能
set(CMAKE_AUTOMOC ON)

# 设置资源文件
file(GLOB_RECURSE PROJECT_QRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/res/res.qrc
)

# 添加资源文件
qt6_add_resources(PROJECT_MOC_QRCS ${PROJECT_QRCS})

# 设置ui文件
file(GLOB_RECURSE PROJECT_UIS
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/about/*.ui
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/shortcut/*.ui
)

# 添加ui文件
qt6_wrap_ui(PROJECT_MOC_UIS ${PROJECT_UIS})

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
    Qt6::OpenGL
)