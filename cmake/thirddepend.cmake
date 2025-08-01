# 设置三方依赖库表
set(THIRD_DEPEND_LIBS )

# 安卓使用系统解码器，不引入ffmpeg
if (ENABLE_MEDIA_PLAYER AND NOT ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/depend/ffmpeg.cmake)
    # 添加代码使用宏区分
    add_compile_definitions(CAN_USE_FFMPEG)
endif()

if (ANDROID)
    include(FetchContent)
    
    FetchContent_Declare(
      android_openssl
      DOWNLOAD_EXTRACT_TIMESTAMP true
      URL https://github.com/KDAB/android_openssl/archive/refs/heads/master.zip
#      URL_HASH MD5=c97d6ad774fab16be63b0ab40f78d945 #optional
    )
    FetchContent_MakeAvailable(android_openssl)
    include(${android_openssl_SOURCE_DIR}/android_openssl.cmake)
endif()