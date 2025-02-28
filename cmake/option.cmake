# 设置是否启用范例选项
option(EXAMPLE_PROJ "build example content" ON)

if (EXAMPLE_PROJ)
    add_definitions(-DEXAMPLE_ON)

    find_package(Qt5Quick         REQUIRED)
    find_package(Qt5QuickWidgets  REQUIRED)
    list(APPEND QT_DEPEND_LIBS
        Qt5::Quick
        Qt5::QuickWidgets
    )
    list(APPEND QT_LIBS
        libQt5QuickWidgets.so.5
        libQt5Quick.so.5
        libQt5QmlModels.so.5
        libQt5Qml.so.5
        libQt5Network.so.5
    )
endif (EXAMPLE_PROJ)

# 