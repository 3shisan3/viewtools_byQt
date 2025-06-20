#include "settings_manager.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

SsSettingsManager::SsSettingsManager()
    : QSettings(QApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat)
{
}

QString SsSettingsManager::getConfigPath()
{
    QString configPath;
    
#ifdef RUNNING_ANDROID
    // Android 平台使用应用数据目录
    configPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config.ini";
    
    // 如果配置文件不存在，尝试从 assets 复制默认配置
    if (!QFile::exists(configPath)) {
        QFile defaultConfig("assets:/config.ini");
        if (defaultConfig.exists()) {
            // 确保目标目录存在
            QDir().mkpath(QFileInfo(configPath).path());
            
            if (defaultConfig.copy(configPath)) {
                // 设置文件权限
                QFile::setPermissions(configPath, 
                    QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
            }
        }
    }
#else
    // 其他平台使用应用程序目录
    configPath = QApplication::applicationDirPath() + "/config.ini";
    
    // 确保目录存在（适用于非Android平台）
    QDir().mkpath(QFileInfo(configPath).path());
#endif

    return configPath;
}