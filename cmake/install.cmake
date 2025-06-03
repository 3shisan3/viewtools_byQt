# 设置默认的安装目录
set(PACKAGE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/package/viewtools CACHE PATH "Default install path")
set(CMAKE_INSTALL_PREFIX ${PACKAGE_PATH})

# 安装可执行程序
install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX})

# 安装库文件
file(GLOB LIB_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/lib/*.so*
)
install(FILES ${LIB_FILES} DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)

# 创建文件夹
install(CODE "execute_process(COMMAND mkdir -p ${CMAKE_INSTALL_PREFIX}/depend/lib)")

# 拷贝qt的库


# 需要安装的qt插件
install(DIRECTORY ${QT_PREFIX_PATH}/plugins DESTINATION ${CMAKE_INSTALL_PREFIX}/depend)
