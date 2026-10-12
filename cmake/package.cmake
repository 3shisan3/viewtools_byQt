# 单文件分发（PACKAGE_SINGLE_FILE=ON 时启用，当前仅 Windows）
#
# 产物：${EXECUTABLE_OUTPUT_PATH}/${PACKAGE_OUTPUT_NAME}.exe
#   文件结构：launcher.exe + 压缩后的部署目录(archive) + 48 字节尾注(footer)
#   运行方式：首次启动解压到 %LOCALAPPDATA%\ViewTools\<产品名>\<CRC32>\
#             之后命中缓存直接启动（秒开），缓存不完整时自动重新解压
#             命令行参数、工作目录、退出码原样透传给真正的程序
#   依赖：zlib（仅构建期压缩），不需要 7-Zip / Enigma 等任何外部工具
#
# 构建：cmake --build <build> --target package

if(NOT PACKAGE_SINGLE_FILE)
    return()
endif()

if(NOT WIN32)
    message(WARNING "单文件打包目前仅支持 Windows，Linux 请改用 AppImage（deploy 目标）")
    return()
endif()

# 启动器必须静态链接 zlib：否则单文件 exe 移到别的目录会因缺少 zlib1.dll 无法启动
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)

# FindZLIB 经常选到动态导入库（MinGW 的 libz.dll.a / MSVC 的 zlib.lib(导入库)），
# 这里按编译器换成同目录下的静态库，保证启动器不依赖 zlib1.dll / zlib1.dll
set(ZLIB_STATIC_LIBRARY "${ZLIB_LIBRARIES}")
get_filename_component(_zlib_name "${ZLIB_LIBRARIES}" NAME)
get_filename_component(_zlib_dir "${ZLIB_LIBRARIES}" DIRECTORY)
if(MSVC)
    if(NOT _zlib_name STREQUAL "zlibstatic.lib" AND EXISTS "${_zlib_dir}/zlibstatic.lib")
        set(ZLIB_STATIC_LIBRARY "${_zlib_dir}/zlibstatic.lib")
    endif()
elseif(_zlib_name MATCHES "\\.dll\\.a$")
    if(EXISTS "${_zlib_dir}/libz.a")
        set(ZLIB_STATIC_LIBRARY "${_zlib_dir}/libz.a")
    else()
        message(WARNING "未找到静态 zlib(libz.a)，启动器会依赖 zlib1.dll：${ZLIB_LIBRARIES}")
    endif()
endif()
message(STATUS "zlib (launcher/packer): ${ZLIB_STATIC_LIBRARY}")

if(NOT PACKAGE_OUTPUT_NAME)
    set(PACKAGE_OUTPUT_NAME "${PROJECT_NAME}_single")
endif()

set(SFX_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/singlefile")
set(SFX_BIN_DIR "${CMAKE_BINARY_DIR}/singlefile")
set(PACKAGE_OUTPUT_FILE "${EXECUTABLE_OUTPUT_PATH}/${PACKAGE_OUTPUT_NAME}.exe")

# 启动器：GUI 子系统，不依赖 Qt，也不依赖任何第三方 DLL
add_executable(sfx_launcher WIN32
    "${SFX_SRC_DIR}/launcher.c"
    "${SFX_SRC_DIR}/sfx_format.h"
)
target_include_directories(sfx_launcher PRIVATE "${SFX_SRC_DIR}" ${ZLIB_INCLUDE_DIRS})
target_link_libraries(sfx_launcher PRIVATE ${ZLIB_STATIC_LIBRARY} comctl32)

# 打包器：构建期工具，把部署目录压成 archive 并拼到启动器后面
add_executable(sfx_packer
    "${SFX_SRC_DIR}/packer.c"
    "${SFX_SRC_DIR}/sfx_format.h"
)
target_include_directories(sfx_packer PRIVATE "${SFX_SRC_DIR}" ${ZLIB_INCLUDE_DIRS})
target_link_libraries(sfx_packer PRIVATE ${ZLIB_STATIC_LIBRARY} shell32)

# 两个工具的公共属性：纯 C、不参与 Qt 的 AUTOMOC/UIC/RCC
foreach(_sfx_target sfx_launcher sfx_packer)
    set_target_properties(${_sfx_target} PROPERTIES
        C_STANDARD 99
        RUNTIME_OUTPUT_DIRECTORY "${SFX_BIN_DIR}"
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    if(MSVC)
        # 静态 CRT，避免依赖 vcruntime140.dll / msvcp140.dll
        set_target_properties(${_sfx_target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        # 屏蔽 _wfopen 等 C4996 告警；/utf-8 保证源码里的中文注释不会触发 C4819
        target_compile_definitions(${_sfx_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${_sfx_target} PRIVATE /utf-8)
    else()
        target_link_options(${_sfx_target} PRIVATE -static-libgcc)
    endif()
endforeach()

# 单文件打包目标：依赖主程序（含 windeployqt 等 POST_BUILD 部署步骤）
add_custom_target(package
    COMMAND $<TARGET_FILE:sfx_packer>
        --dir "${EXECUTABLE_OUTPUT_PATH}"
        --entry "$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        --launcher "$<TARGET_FILE:sfx_launcher>"
        --out "${PACKAGE_OUTPUT_FILE}"
    DEPENDS ${PROJECT_NAME} sfx_launcher sfx_packer
    COMMENT "打包单文件分发包: ${PACKAGE_OUTPUT_FILE}"
    VERBATIM
)

message(STATUS "Single-file package: ON -> ${PACKAGE_OUTPUT_FILE}")
message(STATUS "  Build it with: cmake --build . --target package")
