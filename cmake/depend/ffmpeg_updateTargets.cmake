# 动态更新目标属性（构建后执行）
foreach(component IN LISTS FFMPEG_COMPONENTS)
    if(WIN32)
        # Windows平台查找最新DLL
        file(GLOB dll_files "${FFMPEG_INSTALL_DIR}/bin/${component}-*.dll")
        list(SORT dll_files)
        list(REVERSE dll_files)
        list(GET dll_files 0 latest_dll)
        
        # 更新目标属性
        set_target_properties(FFmpeg::${component} PROPERTIES
            IMPORTED_LOCATION "${latest_dll}"
        )
    else()
        # Unix平台处理.so版本
        file(GLOB so_files "${FFMPEG_INSTALL_DIR}/lib/lib${component}.so.*")
        list(SORT so_files)
        list(REVERSE so_files)
        list(GET so_files 0 latest_so)
        
        set_target_properties(FFmpeg::${component} PROPERTIES
            IMPORTED_LOCATION "${latest_so}"
        )
    endif()
endforeach()