# 设置三方依赖库表
set(THIRD_DEPEND_LIBS )

# 安卓使用系统解码器，不引入ffmpeg
if (ENABLE_MEDIA_PLAYER AND NOT ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/depend/ffmpeg.cmake)
endif()