cmake_minimum_required(VERSION 3.10)

# 导入配置
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/option.cmake)

# 设置项目名
project(viewtools)

# 设置默认的编译类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Default build type")
endif()

# 指定编译器
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 禁用编译器特有的扩展(避开GNU，CLANG等不同的支持)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 指定输出目录
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# 设置三方库的目录
set(3RD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

# 引入 qt 相关配置
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/qt.cmake)
# 引入三方库相关配置
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/thirddepend.cmake)

# 打印关键信息
message(STATUS "CMAKE_BUILD_TYPE:${CMAKE_BUILD_TYPE}")
message(STATUS "DEPEND_LIBS:${QT_DEPEND_LIBS}  ${THIRD_DEPEND_LIBS}")

# 源码目录
set(SOURCE_CODE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 指定包含头文件
include_directories(
    ${3RD_DIR}/include

    ${SOURCE_CODE_DIR}
    ${SOURCE_CODE_DIR}/core
)

# 指定源码
file(GLOB PROJECT_SRCS ${SOURCE_CODE_DIR}/main.cpp)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/code.cmake)

# 指定库文件搜索路径
link_directories(${3RD_DIR})

# 所有平台共用的源文件
set(TARGET_SOURCES
    ${PROJECT_SRCS}
    ${QT_SRCS}
)
# 生成可执行文件
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform.cmake)

#依赖库
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${QT_DEPEND_LIBS}
    ${THIRD_DEPEND_LIBS}
)

# 安装
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/install.cmake)