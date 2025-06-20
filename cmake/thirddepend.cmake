# 设置三方依赖库表
set(THIRD_DEPEND_LIBS )

if (ENABLE_MEDIA_PLAYER)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/depend/ffmpeg.cmake)
endif()