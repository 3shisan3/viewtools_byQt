# 创建部署脚本
file(WRITE "${CMAKE_BINARY_DIR}/deploy_ffmpeg.cmake" "
if(WIN32)
    file(GLOB dll_files \"${FFMPEG_INSTALL_DIR}/bin/*.dll\")
    foreach(dll IN LISTS dll_files)
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different
            \"\${dll}\" \"${EXECUTABLE_OUTPUT_PATH}\")
    endforeach()
else()
    file(GLOB so_files \"${FFMPEG_INSTALL_DIR}/lib/lib*.so*\")
    foreach(so IN LISTS so_files)
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different
            \"\${so}\" \"${EXECUTABLE_OUTPUT_PATH}/lib\")
    endforeach()
endif()
")

# 添加部署命令
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/deploy_ffmpeg.cmake"
    COMMENT "Deploying FFmpeg libraries using script"
    VERBATIM
)

# 确保主项目依赖关系
if(TARGET ffmpeg_build_complete)
    foreach(component IN LISTS FFMPEG_COMPONENTS)
        if(TARGET FFmpeg::${component})
            add_dependencies(${PROJECT_NAME} FFmpeg::${component})
            message(STATUS "Added dependency: ${PROJECT_NAME} -> FFmpeg::${component}")
        else()
            message(WARNING "FFmpeg component target not found: FFmpeg::${component}")
        endif()
    endforeach()
endif()