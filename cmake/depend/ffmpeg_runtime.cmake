# 添加 FFmpeg 部署逻辑
if(WIN32)
    # 查找 FFmpeg DLL 文件
    file(GLOB FFMPEG_DLLS
        "${FFMPEG_LIB_DIR}/avcodec-*.dll"
        "${FFMPEG_LIB_DIR}/avformat-*.dll"
        "${FFMPEG_LIB_DIR}/avutil-*.dll"
        "${FFMPEG_LIB_DIR}/swresample-*.dll"
        "${FFMPEG_LIB_DIR}/swscale-*.dll"
    )
    
    # 检查是否找到文件
    if(NOT FFMPEG_DLLS)
        message(WARNING "No FFmpeg DLLs found in ${FFMPEG_LIB_DIR}")
    else()
        message(STATUS "Found FFmpeg DLLs: ${FFMPEG_DLLS}")
        
        # 添加部署命令
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Deploying FFmpeg libraries..."
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_DLLS}
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMENT "Copying FFmpeg DLLs to output directory"
        )
    endif()
elseif(UNIX AND NOT APPLE)
    # Linux 平台的部署逻辑
    file(GLOB FFMPEG_SO_FILES
        "${FFMPEG_LIB_DIR}/libavcodec.so*"
        "${FFMPEG_LIB_DIR}/libavformat.so*"
        "${FFMPEG_LIB_DIR}/libavutil.so*"
        "${FFMPEG_LIB_DIR}/libswresample.so*"
        "${FFMPEG_LIB_DIR}/libswscale.so*"
    )
    
    if(FFMPEG_SO_FILES)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_SO_FILES}
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/lib"
            COMMENT "Copying FFmpeg shared libraries"
        )
    endif()
endif()

# 补充ffmpeg库和主项目的依赖关系
message(STATUS "Building ${PROJECT_NAME} depend FFmpeg ${FFMPEG_VERSION}")
if(TARGET ffmpeg_build_complete)
    add_dependencies(${PROJECT_NAME} ffmpeg_build_complete)
elseif(TARGET ffmpeg)
    add_dependencies(${PROJECT_NAME} ffmpeg)
endif()