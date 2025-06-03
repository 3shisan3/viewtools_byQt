# 生成可执行程序
if (ANDROID)
    # 设置 Android 包名（必须与 AndroidManifest.xml 的 package 一致）
    set(ANDROID_PACKAGE_NAME "com.example.${PROJECT_NAME}")
    # 指定 AndroidManifest.xml 路径
    set(ANDROID_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml")
    # 可选：指定 Android 资源目录
    set(ANDROID_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/res")
    # 可选：指定 Android 额外文件（如 gradle 配置）
    set(ANDROID_EXTRA_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/android/gradle.properties"
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
            INSTALL_RPATH "$ORIGIN"
        )
    endif()
endif()

# 构建后运行依赖打包命令
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Starting Qt deployment..."
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/bin"
        COMMAND ${QT_PREFIX_PATH}/bin/windeployqt.exe 
            --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/ui/qml 
            "${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}.exe" > NUL 2>&1
        COMMAND ${CMAKE_COMMAND} -E echo "Qt deployment completed"
        COMMENT "Deploying Qt runtime dependencies..."
    )
endif()