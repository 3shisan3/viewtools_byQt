完整构建流程（Qt Creator 中）​​
1. ​​环境准备​​
    在 Qt Creator 中配置：
    ​​Android SDK/NDK 路径​​：Tools > Options > Devices > Android
    ​​Qt for Android 套件​​：确保选择 Android ARMv8 或 ARMv7 套件
2. ​​构建步骤​​
    ​​生成构建目录​​：
    在 Qt Creator 中选择 Release 构建配置和 Android 套件
    首次构建会自动生成 android-deployment-settings.json
    ​​CMake 配置​​：
    Qt Creator 会调用 CMake 生成 Makefile/Ninja 文件
    关键 CMake 参数（自动传递）
3. ​​编译原生代码​​：
    编译 libMyApp.so 和依赖的 Qt 库
4. 打包 APK​​：
    自动调用 androiddeployqt 工具
5. ​​签名 APK​​：
    根据 build.gradle 中的 signingConfigs 自动签名


项目根目录/
├── CMakeLists.txt                  # 主构建配置
├── android/
│   ├── AndroidManifest.xml         # Android 应用清单
│   ├── build.gradle                # Gradle 构建配置
│   ├── gradle.properties           # 签名密钥配置（可选）
│   ├── res/                        # Android 资源文件（图标、字符串等）
│   └── android-deployment-settings.json # Qt 部署配置（自动生成）
├── src/                            # 项目源代码
└── cmake/                          # 自定义 CMake 脚本
...