# 针对不同平台生成可执行程序
if (ANDROID)
    # 设置 Android 包名（必须与 AndroidManifest.xml 的 package 一致）
    set(ANDROID_PACKAGE_NAME "org.qtproject.example.${PROJECT_NAME}")
    # 指定 AndroidManifest.xml 路径
    set(ANDROID_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml")
    # 可选：指定 Android 资源目录
    set(ANDROID_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/res")
    # 可选：指定 Android 额外文件（如 gradle 配置）
    set(ANDROID_EXTRA_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/android/build.gradle"
    )

    qt_add_executable(${PROJECT_NAME}
        MANUAL_FINALIZATION
        ${PROJECT_SRCS} ${QT_SRCS}
    )

    # 设置 Android 相关属性
    set_target_properties(${PROJECT_NAME} PROPERTIES
        ANDROID_MANIFEST "${ANDROID_MANIFEST}"
        ANDROID_RES_DIR "${ANDROID_RES_DIR}"
        ANDROID_EXTRA_FILES "${ANDROID_EXTRA_FILES}"
    )

    # 调用函数为目标添加 OpenSSL 库
    add_android_openssl_libraries(${PROJECT_NAME})

    qt_finalize_executable(${PROJECT_NAME})
elseif (WIN32)
    # Windows 平台需要 WIN32 标志来避免控制台窗口
    add_executable(${PROJECT_NAME} WIN32 ${TARGET_SOURCES})
    
    # Windows 特定设置（例如设置图标、子系统等）
    if (MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
    endif()

else()
    # 其他平台（Linux/macOS）的通用配置
    add_executable(${PROJECT_NAME} ${TARGET_SOURCES})
    
    # Unix-like 平台的特定设置（例如 RPATH）
    if (UNIX AND NOT APPLE)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            INSTALL_RPATH "$ORIGIN;$ORIGIN/lib;$ORIGIN/thirdlib;$ORIGIN/../lib"
        )
    endif()
endif()

# 增加构建过程中的宏
if (ANDROID)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RUNNING_ANDROID)
endif()

# 构建后运行依赖打包命令
if(WIN32)
    if (MINGW)
        # 获取 MinGW 的 bin 目录（存放运行时 DLL）
        get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(MINGW_BIN_DIR "${MINGW_BIN_DIR}/../bin" ABSOLUTE)
        # 设置部署脚本路径
        set(DEPLOY_SCRIPT "${CMAKE_SOURCE_DIR}/script/tools/deploy_mingw_dlls.sh")

        # 构建后分析依赖并复制
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Deploying MinGW runtime DLLs..."
            COMMAND ${CMAKE_COMMAND} -E make_directory "${EXECUTABLE_OUTPUT_PATH}"
            COMMAND bash "${DEPLOY_SCRIPT}"
                "${MINGW_BIN_DIR}"
                "${EXECUTABLE_OUTPUT_PATH}"
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Copying required MinGW runtime DLLs..."
        )
    endif()

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Starting Qt deployment..."
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/bin"
        COMMAND ${QT_PREFIX_PATH}/bin/windeployqt.exe 
            --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/ui/qml 
            "${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}.exe" > NUL 2>&1
        COMMAND ${CMAKE_COMMAND} -E echo "Qt deployment completed"
        COMMENT "Deploying Qt runtime dependencies..."
    )
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    # Linux 平台使用 linuxdeployqt 或手动处理
    # 自动获取工具路径
    execute_process(
        COMMAND bash "${CMAKE_SOURCE_DIR}/script/tools/check_linuxdeployqt.sh" "auto"
        OUTPUT_VARIABLE DEPLOYQT_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE INSTALL_RESULT
    )
    if(INSTALL_RESULT EQUAL 0 AND EXISTS "${DEPLOYQT_PATH}")
        add_custom_target(deploy
            COMMAND "${DEPLOYQT_PATH}"
                "$<TARGET_FILE:${PROJECT_NAME}>"
                -qmldir="${CMAKE_SOURCE_DIR}/ui/qml"
                -bundle-non-qt-libs
                -no-translations
                -appimage
            DEPENDS ${PROJECT_NAME}
            COMMENT "Creating AppImage bundle..."
        )
    else()
        message(WARNING "linuxdeployqt not available, deployment disabled")
        # 备用方案：使用 patchelf 和 ldd 手动处理依赖
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Manual dependency handling for Linux..."
            COMMAND ${CMAKE_COMMAND} -E make_directory "${EXECUTABLE_OUTPUT_PATH}/lib"
            COMMAND sh -c "ldd ${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME} | grep \"=> /\" | awk '{print \$3}' | xargs -I '{}' cp -v '{}' ${EXECUTABLE_OUTPUT_PATH}/lib"
            COMMAND sh -c "patchelf --set-rpath '\\$ORIGIN/lib' ${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}"
            COMMENT "Manually copying shared libraries..."
        )
    endif()
elseif(ANDROID)
    # Android 平台自动处理依赖，但可以添加额外步骤
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Android build complete - APK should contain all dependencies"
        COMMENT "Android deployment handled by Qt build system..."
    )
endif()

# 添加运行以来的三方库部署逻辑
if(ENABLE_MEDIA_PLAYER AND NOT ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/depend/ffmpeg_runtime.cmake)
endif()