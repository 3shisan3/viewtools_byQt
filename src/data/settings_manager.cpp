#include "settings_manager.h"

#include <QApplication>

SsSettingsManager::SsSettingsManager()
    : QSettings(QApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat)
{
}