# 设置默认的qt目录
set(QT_PREFIX_PATH "/home/shisan/Qt/5.15.2/gcc_64" CACHE PATH "Default qt install path")

# 检查路径是否存在
if(NOT EXISTS ${QT_PREFIX_PATH})
    message(FATAL_ERROR "QT_PREFIX_PATH: ${QT_PREFIX_PATH} is not exist.")
endif()

# 将qt路径添加到搜索目录中
list(APPEND CMAKE_PREFIX_PATH ${QT_PREFIX_PATH})

# 查找Qt依赖
find_package(Qt5Core          REQUIRED)
find_package(Qt5Widgets       REQUIRED)
find_package(Qt5Gui           REQUIRED)
find_package(Qt5OpenGL        REQUIRED)
find_package(Qt5Quick         REQUIRED)
find_package(Qt5QuickWidgets  REQUIRED)
# find_package(Qt5 COMPONENTS Core Gui OpenGL Quick QuickWidgets Widgets REQUIRED)

# 若要使用qt的ui相关功能，需开启，否则无找到对应ui_xxx.h
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 设置MOC，如果要使用信号槽，则必须开启该功能
set(CMAKE_AUTOMOC ON)
# 设置UIC，将.ui 文件（Qt Designer 生成的界面文件）转换为对应的 C++ 头文件
# set(CMAKE_AUTOUIC ON)
# 设置RCC，将.qrc 文件（Qt 资源文件）编译为 C++ 代码
set(CMAKE_AUTORCC ON)


# 设置资源文件
file(GLOB PROJECT_QRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/res/res.qrc
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ui.qrc
)

# 添加资源文件
qt5_add_resources(PROJECT_MOC_QRCS ${PROJECT_QRCS})

# 设置ui文件
# file(GLOB_RECURSE PROJECT_UIS
# )

# 添加ui文件
# qt5_wrap_ui(PROJECT_MOC_UIS ${PROJECT_UIS})

# qt的资源
set(QT_SRCS
    ${PROJECT_MOC_QRCS}
    # ${PROJECT_MOC_UIS}
)

# 设置Qt的依赖库
set(QT_DEPEND_LIBS
    Qt5::Core
    Qt5::Widgets
    Qt5::Gui
    Qt5::OpenGL
    Qt5::Quick
    Qt5::QuickWidgets
)