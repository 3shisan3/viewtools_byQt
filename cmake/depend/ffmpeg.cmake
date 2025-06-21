# FFmpeg 配置选项
set(FFMPEG_VERSION "6.0" CACHE STRING "FFmpeg version to use")
set(FFMPEG_SOURCE_BUILD OFF CACHE BOOL "Force build FFmpeg from source")
set(FFMPEG_COMPONENTS avcodec avformat avutil swresample swscale)

# 尝试查找系统安装的 FFmpeg
if(NOT FFMPEG_SOURCE_BUILD)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG
            libavcodec
            libavformat
            libavutil
            libswresample
            libswscale
        )
    endif()

    if(FFMPEG_PKG_FOUND)
        message(STATUS "Found system FFmpeg via pkg-config")
        foreach(component IN LISTS FFMPEG_COMPONENTS)
            add_library(FFmpeg::${component} INTERFACE IMPORTED)
            target_link_libraries(FFmpeg::${component} INTERFACE ${FFMPEG_LIBRARIES})
            target_include_directories(FFmpeg::${component} INTERFACE ${FFMPEG_INCLUDE_DIRS})
        endforeach()
        set(FFMPEG_FOUND TRUE)
    else()
        # 尝试传统查找方式
        find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
        foreach(component IN LISTS FFMPEG_COMPONENTS)
            string(TOUPPER ${component} COMPONENT_UPPER)
            find_library(FFMPEG_${COMPONENT_UPPER}_LIBRARY ${component})
            if(FFMPEG_INCLUDE_DIR AND FFMPEG_${COMPONENT_UPPER}_LIBRARY)
                add_library(FFmpeg::${component} UNKNOWN IMPORTED)
                set_target_properties(FFmpeg::${component} PROPERTIES
                    IMPORTED_LOCATION "${FFMPEG_${COMPONENT_UPPER}_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
                )
                set(FFMPEG_${component}_FOUND TRUE)
            endif()
        endforeach()

        # 检查所有需要的组件是否找到
        set(FFMPEG_FOUND TRUE)
        foreach(component IN LISTS FFMPEG_COMPONENTS)
            if(NOT FFMPEG_${component}_FOUND)
                set(FFMPEG_FOUND FALSE)
                message(STATUS "FFmpeg component ${component} not found")
            endif()
        endforeach()
    endif()
endif()

# 如果未找到系统 FFmpeg，则从源码构建
if(NOT FFMPEG_FOUND OR FFMPEG_SOURCE_BUILD)
    message(STATUS "Building FFmpeg ${FFMPEG_VERSION} from source")

    # FFmpeg自定义安装路径（使用正斜杠）
    set(FFMPEG_INSTALL_DIR "${3RD_DIR}/ffmpeg")
    file(TO_CMAKE_PATH "${FFMPEG_INSTALL_DIR}" FFMPEG_INSTALL_DIR) # 确保路径格式统一
    message(STATUS "FFmpeg安装路径: ${FFMPEG_INSTALL_DIR}")
    file(MAKE_DIRECTORY "${FFMPEG_INSTALL_DIR}")

    include(ExternalProject)
    include(ProcessorCount)
    ProcessorCount(NPROC)

    # 查找必要的工具
    find_program(BASH_EXECUTABLE bash)
    find_program(MAKE_EXECUTABLE NAMES make mingw32-make)
    
    if(NOT BASH_EXECUTABLE)
        message(FATAL_ERROR "bash not found! Required for FFmpeg configuration")
    endif()
    
    if(NOT MAKE_EXECUTABLE)
        message(FATAL_ERROR "make not found! Required for building FFmpeg")
    endif()

    # 设置基本FFmpeg构建选项
    set(FFMPEG_CONFIGURE_OPTIONS
        --enable-gpl
        --enable-version3
        --disable-static
        --enable-shared
        --disable-programs
        --disable-doc
    )
    
    # 添加请求的组件
    foreach(component IN LISTS FFMPEG_COMPONENTS)
        list(APPEND FFMPEG_CONFIGURE_OPTIONS --enable-${component})
    endforeach()

    # 平台特定配置
    if(WIN32)
        # 汇编器检测
        find_program(NASM_EXE nasm)
        if(NASM_EXE)
            list(APPEND FFMPEG_CONFIGURE_OPTIONS --enable-nasm)
        else()
            list(APPEND FFMPEG_CONFIGURE_OPTIONS --disable-asm)
        endif()

        if(MSVC)
            list(APPEND FFMPEG_CONFIGURE_OPTIONS
                --toolchain=msvc
                --extra-cflags=-MDd
                --extra-ldflags=-DEBUG
            )
        else()
            # MinGW配置
            find_program(GCC_EXECUTABLE NAMES gcc)
            if(NOT GCC_EXECUTABLE)
                message(FATAL_ERROR "MinGW gcc not found! Please install with: pacman -S mingw-w64-x86_64-toolchain")
            endif()

            get_filename_component(MINGW_BIN_DIR "${GCC_EXECUTABLE}" DIRECTORY)
            get_filename_component(MINGW_ROOT_DIR "${MINGW_BIN_DIR}" DIRECTORY)

            # 添加MinGW特定选项
            list(APPEND FFMPEG_CONFIGURE_OPTIONS
                --target-os=mingw64
                --arch=x86_64
                --cross-prefix=${MINGW_BIN_DIR}/
                --pkg-config=pkg-config
                --extra-cflags=-I${MINGW_ROOT_DIR}/include
                --extra-cflags=-D_WIN32_WINNT=0x0601
                --extra-ldflags=-L${MINGW_ROOT_DIR}/lib
                --extra-ldflags=-static-libgcc
                --extra-ldflags=-lws2_32
                --extra-ldflags=-lsecur32
                --extra-ldflags=-lbcrypt
            )
        endif()
    elseif(ANDROID)
        # Android 交叉编译配置
        if(NOT ANDROID_NDK)
            message(FATAL_ERROR "ANDROID_NDK not set for Android build")
        endif()
        
        # 根据Android ABI设置arch
        if(ANDROID_ABI STREQUAL "armeabi-v7a")
            set(FFMPEG_ARCH "arm")
            set(FFMPEG_CPU "armv7-a")
        elseif(ANDROID_ABI STREQUAL "arm64-v8a")
            set(FFMPEG_ARCH "aarch64")
            set(FFMPEG_CPU "armv8-a")
        elseif(ANDROID_ABI STREQUAL "x86")
            set(FFMPEG_ARCH "i686")
            set(FFMPEG_CPU "i686")
        elseif(ANDROID_ABI STREQUAL "x86_64")
            set(FFMPEG_ARCH "x86_64")
            set(FFMPEG_CPU "x86-64")
        endif()
        
        list(APPEND FFMPEG_CONFIGURE_OPTIONS
            --target-os=android
            --arch=${FFMPEG_ARCH}
            --cpu=${FFMPEG_CPU}
            --enable-cross-compile
            --cross-prefix=${ANDROID_TOOLCHAIN}/bin/${ANDROID_TOOLCHAIN_PREFIX}-
            --sysroot=${ANDROID_SYSROOT}
            --extra-cflags=-fPIC
            --extra-ldflags=-fPIC
            --extra-cflags=-D__ANDROID_API__=${ANDROID_PLATFORM}
        )
    else() # Linux/Unix
        list(APPEND FFMPEG_CONFIGURE_OPTIONS
            --extra-cflags=-fPIC
            --extra-ldflags=-fPIC
        )
        # 检查汇编器
        find_program(NASM_EXE nasm)
        if(NASM_EXE)
            list(APPEND FFMPEG_CONFIGURE_OPTIONS --enable-nasm)
        else()
            message(WARNING "nasm not found, assembly optimizations will be disabled")
        endif()
    endif()

    # 将配置选项列表转换为字符串（处理Windows路径）
    if(WIN32)
        # 在Windows上使用正斜杠
        string(REPLACE ";" " " FFMPEG_CONFIGURE_OPTIONS_STR "${FFMPEG_CONFIGURE_OPTIONS}")
        string(REPLACE "\\" "/" FFMPEG_INSTALL_DIR_FOR_CONFIGURE "${FFMPEG_INSTALL_DIR}")
        set(FFMPEG_CONFIGURE_OPTIONS_STR "--prefix=${FFMPEG_INSTALL_DIR_FOR_CONFIGURE} ${FFMPEG_CONFIGURE_OPTIONS_STR}")
    else()
        string(REPLACE ";" " " FFMPEG_CONFIGURE_OPTIONS_STR "${FFMPEG_CONFIGURE_OPTIONS}")
        set(FFMPEG_CONFIGURE_OPTIONS_STR "--prefix=${FFMPEG_INSTALL_DIR} ${FFMPEG_CONFIGURE_OPTIONS_STR}")
    endif()

    # 下载并构建 FFmpeg
    ExternalProject_Add(ffmpeg
        GIT_REPOSITORY "https://git.ffmpeg.org/ffmpeg.git"
        GIT_TAG "n${FFMPEG_VERSION}"
        PREFIX "${CMAKE_BINARY_DIR}/ffmpeg-build"
        
        # 配置命令（简化版，避免转义问题）
        CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env
            ${BASH_EXECUTABLE} -c
            "cd <SOURCE_DIR> && ./configure ${FFMPEG_CONFIGURE_OPTIONS_STR}"
        
        # 构建命令
        BUILD_COMMAND
            ${CMAKE_COMMAND} -E env
            ${BASH_EXECUTABLE} -c
            "cd <SOURCE_DIR> && ${MAKE_EXECUTABLE} -j${NPROC}"
        
        # 安装命令
        INSTALL_COMMAND
            ${CMAKE_COMMAND} -E env
            ${BASH_EXECUTABLE} -c
            "cd <SOURCE_DIR> && ${MAKE_EXECUTABLE} install"
        
        BUILD_IN_SOURCE 1
        LOG_DOWNLOAD 1
        LOG_CONFIGURE 1
        LOG_BUILD 1
        LOG_INSTALL 1
    )

    # 设置 FFmpeg 库和头文件路径
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_INSTALL_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_INSTALL_DIR}/lib")

    # 为每个组件创建导入目标
    foreach(component IN LISTS FFMPEG_COMPONENTS)
        add_library(FFmpeg::${component} SHARED IMPORTED)
        set_target_properties(FFmpeg::${component} PROPERTIES
            IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}${component}${CMAKE_SHARED_LIBRARY_SUFFIX}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
        )
        add_dependencies(FFmpeg::${component} ffmpeg)
    endforeach()
endif()