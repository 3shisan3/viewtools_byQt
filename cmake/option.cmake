# 设置是否启用范例选项
option(EXAMPLE_PROJ "build example content" ON)

if (EXAMPLE_PROJ)
    add_definitions(-DEXAMPLE_ON)
endif (EXAMPLE_PROJ)

# 