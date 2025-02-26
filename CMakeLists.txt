cmake_minimum_required(VERSION 3.5)

project(viewtools)

# 设置默认的编译类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Default build type")
endif()

# 指定编译器
set(CMAKE_CXX_FLAGS "-std=c++11 -fpermissive -pthread")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-rpath=./:./lib:./depend/:./depend/plugins/:./depend/lib")

# 指定输出目录
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# 设置三方库的目录
set(3RD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

# 导入配置
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/qt5.cmake)

message(STATUS "CMAKE_BUILD_TYPE:${CMAKE_BUILD_TYPE}")

#指定包含头文件
file(GLOB PROJECT_HEADERS
    ${3RD_DIR}/include

)

# 包含头文件
include_directories(${PROJECT_HEADERS})

# 源码目录
set(SOURCE_CODE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 指定源码
file(GLOB_RECURSE PROJECT_SRCS
    ${SOURCE_CODE_DIR}/main.cpp
)

# 指定库文件搜索路径
link_directories(${3RD_DIR}/lib)

# 生成可执行文件
if (WIN32)
    # 需标记WIN32，否则运行时会出现控制台界面
    add_executable(${PROJECT_NAME} WIN32 ${PROJECT_SRCS} ${QT_SRCS})
else()
    add_executable(${PROJECT_NAME}  ${PROJECT_SRCS} ${QT_SRCS})
endif()

#依赖库
target_link_libraries(${PROJECT_NAME}
    ${QT_DEPEND_LIBS}
)

# 安装
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/install.cmake)