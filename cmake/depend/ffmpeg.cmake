# FFmpeg 配置选项
set(FFMPEG_VERSION "7.0.2" CACHE STRING "FFmpeg version to use")
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
    message(STATUS "FFmpeg install path: ${FFMPEG_INSTALL_DIR}")
    file(MAKE_DIRECTORY "${FFMPEG_INSTALL_DIR}")

    include(ExternalProject)
    include(ProcessorCount)
    ProcessorCount(NPROC)

    # 查找必要的工具
    find_program(BASH_EXECUTABLE bash)
    if(NOT BASH_EXECUTABLE)
        message(FATAL_ERROR "bash not found! Required for FFmpeg configuration")
    endif()

    # 在Android NDK中查找make工具
    if(ANDROID)
        # 确定NDK主机标签
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
            set(NDK_HOST_TAG "windows-x86_64")
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            set(NDK_HOST_TAG "linux-x86_64")
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
            set(NDK_HOST_TAG "darwin-x86_64")
        endif()

        # 查找make工具（在NDK的prebuilt目录中）
        find_program(MAKE_EXECUTABLE
            NAMES make mingw32-make make.exe
            PATHS "${ANDROID_NDK}/prebuilt/${NDK_HOST_TAG}/bin"
            NO_DEFAULT_PATH)

        if(NOT MAKE_EXECUTABLE)
            message(WARNING "make not found in NDK prebuilt directory, trying system path")
            find_program(MAKE_EXECUTABLE NAMES make mingw32-make make.exe)
        endif()

        if(NOT MAKE_EXECUTABLE)
            message(FATAL_ERROR "make not found! Required for building FFmpeg. "
                "For Android builds, ensure ANDROID_NDK is set correctly and "
                "make is available in ${ANDROID_NDK}/prebuilt/${NDK_HOST_TAG}/bin")
        endif()

        message(STATUS "Found make for Android: ${MAKE_EXECUTABLE}")
    else()
        # 非Android平台的make查找
        find_program(MAKE_EXECUTABLE NAMES make mingw32-make)
        if(NOT MAKE_EXECUTABLE)
            message(FATAL_ERROR "make not found! Required for building FFmpeg")
        endif()
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

        # 设置NDK工具链路径
        set(NDK_TOOLCHAIN_DIR "${ANDROID_NDK}/toolchains/llvm/prebuilt/${NDK_HOST_TAG}")
        # 验证工具链路径是否存在
        if(NOT EXISTS "${NDK_TOOLCHAIN_DIR}")
            message(FATAL_ERROR "Could not find NDK toolchain directory at ${NDK_TOOLCHAIN_DIR}")
        endif()
        set(ANDROID_SYSROOT "${NDK_TOOLCHAIN_DIR}/sysroot")

        # 根据Android ABI设置arch
        if(ANDROID_ABI STREQUAL "armeabi-v7a")
            set(FFMPEG_ARCH "arm")
            set(FFMPEG_CPU "armv7-a")
            set(FFMPEG_CROSS_PREFIX "arm-linux-androideabi")
            set(FFMPEG_TOOLCHAIN_PREFIX "armv7a-linux-androideabi")
            
            list(APPEND FFMPEG_CONFIGURE_OPTIONS --disable-x86asm)
        elseif(ANDROID_ABI STREQUAL "arm64-v8a")
            set(FFMPEG_ARCH "aarch64")
            set(FFMPEG_CPU "armv8-a")
            set(FFMPEG_CROSS_PREFIX "aarch64-linux-android")
            set(FFMPEG_TOOLCHAIN_PREFIX "aarch64-linux-android")

            list(APPEND FFMPEG_CONFIGURE_OPTIONS --disable-x86asm)
        elseif(ANDROID_ABI STREQUAL "x86")
            set(FFMPEG_ARCH "i686")
            set(FFMPEG_CPU "i686")
            set(FFMPEG_CROSS_PREFIX "i686-linux-android")
            set(FFMPEG_TOOLCHAIN_PREFIX "i686-linux-android")
        elseif(ANDROID_ABI STREQUAL "x86_64")
            set(FFMPEG_ARCH "x86_64")
            set(FFMPEG_CPU "x86-64")
            set(FFMPEG_CROSS_PREFIX "x86_64-linux-android")
            set(FFMPEG_TOOLCHAIN_PREFIX "x86_64-linux-android")
        endif()

        # 正确解析Android平台版本号
        string(REGEX REPLACE "android-" "" ANDROID_API_LEVEL "${ANDROID_PLATFORM}")

        # 设置Android特定选项
        list(APPEND FFMPEG_CONFIGURE_OPTIONS
            --disable-avdevice
            --enable_jni
            --target-os=android
            --arch=${FFMPEG_ARCH}
            --cpu=${FFMPEG_CPU}
            --enable-cross-compile
            --cross-prefix=${NDK_TOOLCHAIN_DIR}/bin/${FFMPEG_CROSS_PREFIX}-
            --sysroot=${ANDROID_SYSROOT}
            --cc=${NDK_TOOLCHAIN_DIR}/bin/${FFMPEG_TOOLCHAIN_PREFIX}${ANDROID_API_LEVEL}-clang
            --cxx=${NDK_TOOLCHAIN_DIR}/bin/${FFMPEG_TOOLCHAIN_PREFIX}${ANDROID_API_LEVEL}-clang++
            --ar=${NDK_TOOLCHAIN_DIR}/bin/llvm-ar
            --nm=${NDK_TOOLCHAIN_DIR}/bin/llvm-nm
            --ranlib=${NDK_TOOLCHAIN_DIR}/bin/llvm-ranlib
            --extra-cflags=-fPIC
            --extra-ldflags=-fPIC
            --extra-cflags=-D__ANDROID_API__=${ANDROID_API_LEVEL}
            --extra-cflags=-I${ANDROID_SYSROOT}/usr/include
            --extra-ldflags=-L${ANDROID_SYSROOT}/usr/lib/${FFMPEG_CROSS_PREFIX}/${ANDROID_API_LEVEL}
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

    # 将配置选项列表转换为字符串
    string(REPLACE ";" " " FFMPEG_CONFIGURE_OPTIONS_STR "${FFMPEG_CONFIGURE_OPTIONS}")
    file(TO_CMAKE_PATH "${FFMPEG_INSTALL_DIR}" FFMPEG_INSTALL_DIR_FOR_CONFIGURE)
    # 确保所有路径使用正斜杠
    string(REPLACE "\\" "/" FFMPEG_INSTALL_DIR_FOR_CONFIGURE "${FFMPEG_INSTALL_DIR_FOR_CONFIGURE}")
    string(REPLACE "\\" "/" FFMPEG_CONFIGURE_OPTIONS_STR "${FFMPEG_CONFIGURE_OPTIONS_STR}")
    set(FFMPEG_CONFIGURE_OPTIONS_STR "--prefix=${FFMPEG_INSTALL_DIR_FOR_CONFIGURE} ${FFMPEG_CONFIGURE_OPTIONS_STR}")

    if(WIN32)
        set(FFMPEG_PRE_CONFIGURE_COMMAND 
            COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                    ${BASH_EXECUTABLE} -c "chmod a+x configure"
        )
    else()
        # 换行符转换
        find_program(DOS2UNIX_EXECUTABLE dos2unix)
        if(DOS2UNIX_EXECUTABLE)
            message(STATUS "found dos2unix executable")
            set(FFMPEG_PRE_CONFIGURE_COMMAND 
                COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                        ${DOS2UNIX_EXECUTABLE} configure
            )
        else()
            message(STATUS "don't found dos2unix, use git scheme")
            set(FFMPEG_PRE_CONFIGURE_COMMAND 
                COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                        git config --local core.autocrlf false
                COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                        git rm --cached -r .
                COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                        git reset --hard
            )
        endif()
    endif()
    
    # 下载并构建 FFmpeg
    ExternalProject_Add(ffmpeg
        GIT_REPOSITORY "https://git.ffmpeg.org/ffmpeg.git"
        GIT_TAG "n${FFMPEG_VERSION}"
        PREFIX "${CMAKE_BINARY_DIR}/ffmpeg-build"

        # 配置前准备：修复换行符问题
        CONFIGURE_COMMAND
            ${FFMPEG_PRE_CONFIGURE_COMMAND}
            COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
                    ${BASH_EXECUTABLE} -c "chmod a+x configure && bash configure ${FFMPEG_CONFIGURE_OPTIONS_STR}"

        # 构建命令
        BUILD_COMMAND
            ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
            ${MAKE_EXECUTABLE} -j${NPROC}

        # 安装命令
        INSTALL_COMMAND
            ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
            ${MAKE_EXECUTABLE} install

        BUILD_IN_SOURCE 1
        LOG_DOWNLOAD 1
        LOG_CONFIGURE 1
        LOG_BUILD 1
        LOG_INSTALL 1
        LOG_OUTPUT_ON_FAILURE TRUE
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

# 添加 FFmpeg 组件到第三方依赖库列表
foreach(component IN LISTS FFMPEG_COMPONENTS)
    list(APPEND THIRD_DEPEND_LIBS FFmpeg::${component})
endforeach()