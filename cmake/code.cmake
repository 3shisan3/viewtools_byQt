# 基础源码
file(GLOB_RECURSE BASE_SRCS
    ${SOURCE_CODE_DIR}/core/*.cpp
    ${SOURCE_CODE_DIR}/data/*.cpp
    ${SOURCE_CODE_DIR}/event/*.cpp
    ${SOURCE_CODE_DIR}/utils/*.cpp
    
    ${SOURCE_CODE_DIR}/view/component/*.cpp
)
list(APPEND PROJECT_SRCS ${BASE_SRCS})

# 配置项额外引入源码
if (EXAMPLE_PROJ)
    file(GLOB_RECURSE EXAMPLE_SRCS
        ${SOURCE_CODE_DIR}/factory/example/*.cpp
        ${SOURCE_CODE_DIR}/view/widget/example/*.cpp
        ${SOURCE_CODE_DIR}/view/window/example/*.cpp
    )
    list(APPEND PROJECT_SRCS ${EXAMPLE_SRCS})
endif(EXAMPLE_PROJ)

if (ENABLE_OPENGL)
    file(GLOB_RECURSE OPENGL_SRCS
        ${SOURCE_CODE_DIR}/factory/opengl/*.cpp
        ${SOURCE_CODE_DIR}/view/widget/opengl/*.cpp
        ${SOURCE_CODE_DIR}/view/window/opengl/*.cpp
    )
    list(APPEND PROJECT_SRCS ${OPENGL_SRCS})
endif(ENABLE_OPENGL)

if (ENABLE_MEDIA_PLAYER)
    file(GLOB_RECURSE PLAYER_SRCS
        ${SOURCE_CODE_DIR}/view/widget/player/*.cpp
        ${SOURCE_CODE_DIR}/view/window/player/*.cpp
    )
    list(APPEND PROJECT_SRCS ${PLAYER_SRCS})
endif(ENABLE_MEDIA_PLAYER)